/* TAPE ECHO

Description:
Worn-tape delay — a tape-machine model in the feedback loop. Audio comes in on
the CV jack (sampled by the RP2350 ADC inside the ~36.6 kHz PWM ISR, per
firmwares/mod2-fx/README.md). The read head is a servo-controlled tape
transport, so time changes GLIDE with a tape-speed pitch bend instead of
crossfading — the key feel. POT2 is a single "tape age" macro: wow/flutter
depth, feedback saturation, high-frequency loss and dropout rate all worsen
together. IN2 is a splice gate: the transport ramps to a stop (pitch dives,
output fades) and lurches back up to re-catch the write head on release.
Long presses (>=0.6 s) are taps — the interval between the last two long-press
starts sets the time; a clock on IN1 does the same automatically.
Hold BUTTON + turn POT1 for wet/dry mix, BUTTON + POT2 for feedback (both
persisted to flash). A 220 KB SRAM arena holds 3 s of tape (2 s max delay +
stop headroom). DSP lives in the shared sc::TapeEchoCore (also used by the
VCV Rack port).

Key Variables:
  A0 -> Delay time (30 ms - 2 s, tape-speed glide)
  A1 -> Tape age macro (wow/flutter + HF loss + saturation + dropouts)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║ TAPE ECHO ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - delay time (BTN held: wet/dry mix)
      ║   TIME    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - tape age (BTN held: feedback)
      ║   AGE     ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - tape health (flickers with flutter/dropouts)
      ║   (BTN)   ║   BTN (GPIO6) - long: tap tempo; hold+POT1/POT2: mix/feedback
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - tap tempo / clock in
      ║ (o)   (o) ║   IN2 (GPIO0) - splice gate (HIGH = tape-stop lurch)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial tape echo firmware (maddie synths original, shared TapeEchoCore)

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO MOD2 (Seeed Xiao RP2350)
*/

#include <Arduino.h>
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include <math.h>
#include <EEPROM.h>        // persisted wet/dry + feedback
#include <Mod2Common.h>    // Shared MOD2 pin map, PWM-audio setup and helpers
#include <TapeEchoCore.h>  // Shared tape DSP (also used by rack-plugins/src/mod2-tape-echo.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t LONG_PRESS_MS   = 600;     // hold this long to register a tap
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save
constexpr float MAX_DELAY_SEC      = 2.0f;    // README cap; glides feel right here
constexpr float TAPE_SEC           = 3.0f;    // arena: max delay + tape-stop headroom
constexpr uint32_t TAP_MAX_MS      = 2200;    // longest usable tap interval
constexpr uint32_t CLOCK_MAX_PERIOD = (uint32_t)(TAPE_SEC * mod2::AUDIO_FS);

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  float feedback;
};
constexpr uint32_t SETTINGS_MAGIC = 0x54504531;  // "TPE1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
// ~220 KB int16 arena in SRAM = 3.0 s of tape at ~36.6 kHz. The extra second
// past MAX_DELAY_SEC is what the read head consumes during a splice stop.
static int16_t tapeArena[(uint32_t)(TAPE_SEC * mod2::AUDIO_FS)];
sc::TapeEchoCore tape;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-bitcrusher). */
volatile float g_timeSec  = 0.4f;
volatile float g_age      = 0.3f;
volatile float g_feedback = 0.5f;
volatile float g_wet      = 0.5f;

/* ISR -> loop: IN1 tap/clock measurement + LED level from the core. */
volatile uint32_t g_clockPeriod = 0;
volatile uint32_t g_samplesSinceTick = CLOCK_MAX_PERIOD;
volatile float g_ledLevel = 0.0f;

/* ISR-only state. */
static bool isrLastIn1 = false;

/* =======================================================================
 *  PWM interrupt service routine (~36.6 kHz)
 * ==================================================================== */
void __isr onPwmWrap()
{
  /* --- audio input: sample the CV jack at the audio rate ------------- */
  const int raw = analogRead(mod2::CV_PIN);
  const float in = dcBlock.process(raw * (2.0f / 1023.0f) - 1.0f);

  /* --- IN1: tap/clock period measurement ------------------------------ */
  const bool in1 = digitalRead(mod2::IN1_PIN) == HIGH;
  if (in1 && !isrLastIn1) {
    if (g_samplesSinceTick < CLOCK_MAX_PERIOD)
      g_clockPeriod = g_samplesSinceTick;
    g_samplesSinceTick = 0;
  } else if (g_samplesSinceTick < 0xFFFFFFF0u) {
    g_samplesSinceTick++;
  }
  isrLastIn1 = in1;

  /* --- tape ------------------------------------------------------------ */
  tape.timeSec  = g_timeSec;
  tape.age      = g_age;
  tape.feedback = g_feedback;
  tape.wet      = g_wet;
  tape.splice   = digitalRead(mod2::IN2_PIN) == HIGH;  // tape-stop gate
  float out = tape.process(in, 1.0f / mod2::AUDIO_FS);
  g_ledLevel = tape.led;

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: the core's tape-health flicker ----------------------------- */
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     (uint16_t)(g_ledLevel * mod2::PWM_FS));

  pwm_clear_irq(sliceIRQ);
}

/* =======================================================================
 *  Guarded pot read — the ISR reads A2 through the same ADC mux (see the
 *  mod2-bitcrusher note); blocking the IRQ for the ~2 us conversion just
 *  delays one audio sample slightly.
 * ==================================================================== */
static float readPotGuarded(uint8_t pin)
{
  noInterrupts();
  const int v = analogRead(pin);
  interrupts();
  return v / 1023.0f;
}

/* =======================================================================
 *  Setup
 * ==================================================================== */
void setup()
{
  pinMode(A0, INPUT);                       // time / wet-dry (shifted)
  pinMode(A1, INPUT);                       // age / feedback (shifted)
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // tap / clock in
  pinMode(mod2::IN2_PIN, INPUT);            // splice gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // tap / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_feedback = sc::clampf(s.feedback, 0.0f, 1.1f);
  }

  tape.init(tapeArena, sizeof(tapeArena) / sizeof(tapeArena[0]));

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (tap / two-pot shift layer), tap & clock,
 *  flash saves
 * ==================================================================== */
void loop()
{
  /* --- pots, one-pole smoothed ---------------------------------------- */
  static float pot1 = 0.0f, pot2 = 0.0f;
  static bool primed = false;
  const float r1 = readPotGuarded(mod2::POT1_PIN);
  const float r2 = readPotGuarded(mod2::POT2_PIN);
  if (!primed) {
    pot1 = r1;
    pot2 = r2;
    primed = true;
  }
  pot1 += (r1 - pot1) * 0.2f;
  pot2 += (r2 - pot2) * 0.2f;

  /* --- button: long = tap, hold+POT1 = wet/dry, hold+POT2 = feedback -- */
  static bool lastPressed = false;
  static bool adjustingWet = false;       // POT1 moved during the hold
  static bool adjustingFb = false;        // POT2 moved during the hold
  static bool tapRegistered = false;
  static uint32_t pressStartMs = 0;
  static uint32_t lastTapStartMs = 0;
  static float pot1AtPress = 0.0f, pot2AtPress = 0.0f;
  static float tapTimeSec = 0.0f;         // 0 = no tap override active
  static mod2::PickupParam timePickup;
  static mod2::PickupParam agePickup;
  static bool dirty = false;
  static uint32_t lastChangeMs = 0;

  const bool pressed = digitalRead(mod2::BUTTON_PIN) == LOW;
  const uint32_t now = millis();

  if (pressed && !lastPressed) {  // press edge
    pressStartMs = now;
    pot1AtPress = pot1;
    pot2AtPress = pot2;
    adjustingWet = adjustingFb = false;
    tapRegistered = false;
  }
  if (pressed) {
    if (!adjustingWet && fabsf(pot1 - pot1AtPress) > POT_MOVE_THRESHOLD)
      adjustingWet = true;
    if (!adjustingFb && fabsf(pot2 - pot2AtPress) > POT_MOVE_THRESHOLD)
      adjustingFb = true;
    if (adjustingWet) {
      g_wet = pot1;
      dirty = true;
      lastChangeMs = now;
    }
    if (adjustingFb) {
      g_feedback = sc::tapeEchoFeedback(pot2);
      dirty = true;
      lastChangeMs = now;
    }
  }
  /* Long press = tap, timestamped at the press EDGE (suppressed once the
     shift layer engages).                                               */
  if (pressed && !adjustingWet && !adjustingFb && !tapRegistered &&
      now - pressStartMs >= LONG_PRESS_MS) {
    tapRegistered = true;
    const uint32_t interval = pressStartMs - lastTapStartMs;
    lastTapStartMs = pressStartMs;
    if (interval >= LONG_PRESS_MS && interval <= TAP_MAX_MS) {
      tapTimeSec = interval * 0.001f;
      timePickup.targetValue = pot1;
      timePickup.lastPotValue = pot1;
      timePickup.pickupActive = true;
    }
  }
  if (!pressed && lastPressed) {  // release edge: engage pot pickups
    if (adjustingWet) {
      timePickup.targetValue = pot1AtPress;
      timePickup.lastPotValue = pot1;
      timePickup.pickupActive = true;
    }
    if (adjustingFb) {
      agePickup.targetValue = pot2AtPress;
      agePickup.lastPotValue = pot2;
      agePickup.pickupActive = true;
    }
  }
  lastPressed = pressed;

  /* --- IN1 tap/clock: each measured period sets the time directly ----- */
  noInterrupts();
  const uint32_t clockPeriod = g_clockPeriod;
  const uint32_t sinceTick = g_samplesSinceTick;
  interrupts();
  const bool clocked = clockPeriod > 0 && sinceTick < clockPeriod * 4 &&
                       sinceTick < CLOCK_MAX_PERIOD;

  if (!pressed) {
    if (mod2::checkPickup(timePickup, pot1))
      tapTimeSec = 0.0f;  // knob reclaimed: back to manual time
    timePickup.lastPotValue = pot1;
    if (mod2::checkPickup(agePickup, pot2))
      g_age = pot2;
    agePickup.lastPotValue = pot2;
  }

  if (clocked) {
    g_timeSec = sc::clampf(clockPeriod / mod2::AUDIO_FS, 0.030f, MAX_DELAY_SEC);
  } else if (tapTimeSec > 0.0f) {
    g_timeSec = sc::clampf(tapTimeSec, 0.030f, MAX_DELAY_SEC);
  } else if (!pressed && !timePickup.pickupActive) {
    g_timeSec = sc::tapeEchoTimeSec(pot1, MAX_DELAY_SEC);
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_wet, g_feedback};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
