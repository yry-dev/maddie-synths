/* DELAY

Description:
Clean/dirty mono digital delay — the bread-and-butter MOD2 FX firmware. Audio
comes in on the CV jack (sampled by the RP2350 ADC inside the ~36.6 kHz PWM
ISR, per firmwares/mod2-fx/README.md) and a 366 KB SRAM arena gives up to 5 s
of delay. Time changes crossfade between two read heads so the knob never
pitch-zips. Feedback reaches ~110% and soft-limits into self-oscillation.
Short button presses toggle Clean (flat repeats) / Dirty (saturated + darkened
repeats, bucket-brigade flavour). A clock on IN1 locks the delay to musical
divisions of the incoming period ({1/4, 1/3, 1/2, 2/3, 3/4, 1, 1.5, 2}x picked
by POT1); long presses (>=0.6 s) are taps — the interval between the last two
long-press starts sets the time. IN2 is a hold gate: input mutes and repeats
loop at unity for momentary infinite-repeat, spilling over on release.
Hold BUTTON + turn POT1 for wet/dry mix (persisted to flash with the mode).
DSP lives in the shared sc::DelayFxCore (also used by the VCV Rack port).

Key Variables:
  A0 -> Delay time (10 ms - 5 s, exponential taper) / clock division
  A1 -> Feedback (0 - ~110%)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║   DELAY   ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - delay time / clock division (BTN held: wet/dry)
      ║   TIME    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - feedback (soft-limited past 100%)
      ║   FDBK    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - blinks at the delay time (blinks mode ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: Clean/Dirty; long: tap; hold+POT1: wet/dry
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - clock in (delay locks to divisions of it)
      ║ (o)   (o) ║   IN2 (GPIO0) - hold gate (HIGH = infinite repeat, input muted)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial delay firmware (maddie synths original, shared DelayFxCore)

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
#include <EEPROM.h>        // persisted wet/dry + mode
#include <Mod2Common.h>    // Shared MOD2 pin map, PWM-audio setup and helpers
#include <DelayFxCore.h>   // Shared delay DSP (also used by rack-plugins/src/mod2-delay.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // below = mode toggle, above = tap
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save
constexpr float MAX_DELAY_SEC      = 5.0f;    // arena-limited maximum time
constexpr uint32_t TAP_MAX_MS      = 5500;    // longest usable tap interval
// A clock is "present" while the last IN1 tick is under 4 periods old.
constexpr uint32_t CLOCK_MAX_PERIOD = (uint32_t)(MAX_DELAY_SEC * mod2::AUDIO_FS);

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  uint8_t mode;
};
constexpr uint32_t SETTINGS_MAGIC = 0x444C5931;  // "DLY1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
// 366 KB int16 arena in SRAM = 5.0 s at ~36.6 kHz (RP2350 has 520 KB).
static int16_t delayArena[(uint32_t)(MAX_DELAY_SEC * mod2::AUDIO_FS)];
sc::DelayFxCore delayFx;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-bitcrusher). */
volatile float   g_timeSec = 0.5f;
volatile float   g_feedback = 0.4f;
volatile float   g_wet = 0.5f;
volatile uint8_t g_mode = sc::DELAYFX_CLEAN;

/* ISR -> loop: IN1 clock measurement. */
volatile uint32_t g_clockPeriod = 0;          // samples between the last two ticks
volatile uint32_t g_samplesSinceTick = CLOCK_MAX_PERIOD;

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

  /* --- IN1: clock period measurement (sample-accurate edges) --------- */
  const bool in1 = digitalRead(mod2::IN1_PIN) == HIGH;
  if (in1 && !isrLastIn1) {
    if (g_samplesSinceTick < CLOCK_MAX_PERIOD)
      g_clockPeriod = g_samplesSinceTick;
    g_samplesSinceTick = 0;
  } else if (g_samplesSinceTick < 0xFFFFFFF0u) {
    g_samplesSinceTick++;
  }
  isrLastIn1 = in1;

  /* --- delay ---------------------------------------------------------- */
  delayFx.timeSec  = g_timeSec;
  delayFx.feedback = g_feedback;
  delayFx.wet      = g_wet;
  delayFx.mode     = g_mode;
  delayFx.hold     = digitalRead(mod2::IN2_PIN) == HIGH;  // infinite-repeat gate
  float out = delayFx.process(in, 1.0f / mod2::AUDIO_FS);

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

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
  pinMode(A0, INPUT);                       // delay time / wet-dry (shifted)
  pinMode(A1, INPUT);                       // feedback
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // clock in
  pinMode(mod2::IN2_PIN, INPUT);            // hold gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // mode / tap / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_mode = s.mode < sc::DELAYFX_MODE_COUNT ? s.mode : sc::DELAYFX_CLEAN;
  }

  delayFx.init(delayArena, sizeof(delayArena) / sizeof(delayArena[0]));

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (mode / tap / wet-dry shift), clock sync,
 *  LED time blink, flash saves
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

  g_feedback = sc::delayFxFeedback(pot2);

  /* --- button: short = mode, long = tap, hold+POT1 = wet/dry ---------- */
  static bool lastPressed = false;
  static bool adjustingWet = false;
  static bool tapRegistered = false;
  static uint32_t pressStartMs = 0;
  static uint32_t lastTapStartMs = 0;
  static float potAtPress = 0.0f;
  static float tapTimeSec = 0.0f;         // 0 = no tap override active
  static mod2::PickupParam timePickup;
  static bool dirty = false;
  static uint32_t lastChangeMs = 0;

  const bool pressed = digitalRead(mod2::BUTTON_PIN) == LOW;
  const uint32_t now = millis();

  /* blink state (mode-ID feedback: mode+1 flashes) */
  static uint8_t blinkPhasesLeft = 0;
  static uint32_t blinkNextMs = 0;
  static int16_t ledForce = -1;

  if (pressed && !lastPressed) {  // press edge
    pressStartMs = now;
    potAtPress = pot1;
    adjustingWet = false;
    tapRegistered = false;
  }
  if (pressed && !adjustingWet && fabsf(pot1 - potAtPress) > POT_MOVE_THRESHOLD)
    adjustingWet = true;  // shift layer engaged; mode/tap suppressed
  if (pressed && adjustingWet) {
    g_wet = pot1;
    dirty = true;
    lastChangeMs = now;
  }
  /* Long press = tap, timestamped at the press EDGE. Registered as soon as
     the hold crosses the threshold so the rhythm isn't release-dependent.  */
  if (pressed && !adjustingWet && !tapRegistered &&
      now - pressStartMs >= SHORT_PRESS_MS) {
    tapRegistered = true;
    const uint32_t interval = pressStartMs - lastTapStartMs;
    lastTapStartMs = pressStartMs;
    if (interval >= SHORT_PRESS_MS && interval <= TAP_MAX_MS) {
      tapTimeSec = interval * 0.001f;
      /* POT1 gives way to the tap until it moves past where it sits now. */
      timePickup.targetValue = pot1;
      timePickup.lastPotValue = pot1;
      timePickup.pickupActive = true;
    }
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustingWet) {
      /* POT1 was borrowed for wet/dry: freeze the time until the pot is
         turned back across where it sat before the press.               */
      timePickup.targetValue = potAtPress;
      timePickup.lastPotValue = pot1;
      timePickup.pickupActive = true;
    } else if (!tapRegistered && now - pressStartMs < SHORT_PRESS_MS) {
      g_mode = (g_mode + 1) % sc::DELAYFX_MODE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_mode + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- delay time: clock > tap > POT1 --------------------------------- */
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
  }

  if (clocked) {
    /* POT1 picks a musical ratio of the measured period. */
    const float periodSec = clockPeriod / mod2::AUDIO_FS;
    g_timeSec = sc::clampf(periodSec * sc::delayFxClockRatio(pot1),
                           0.010f, MAX_DELAY_SEC);
  } else if (tapTimeSec > 0.0f) {
    g_timeSec = tapTimeSec;
  } else if (!pressed && !timePickup.pickupActive) {
    g_timeSec = sc::delayFxTimeSec(pot1, MAX_DELAY_SEC);
  }

  /* --- LED: blinks at the delay time (mode-ID blink takes priority) --- */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        ledForce = -1;  // back to the time blink
    }
    if (ledForce >= 0)
      pwm_set_chan_level(sliceLed, PWM_CHAN_B, (uint16_t)ledForce);
  } else {
    const uint32_t periodMs = (uint32_t)(g_timeSec * 1000.0f);
    const bool on = periodMs > 0 && (now % periodMs) < 50;
    pwm_set_chan_level(sliceLed, PWM_CHAN_B, on ? mod2::PWM_FS : 0);
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_wet, g_mode};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
