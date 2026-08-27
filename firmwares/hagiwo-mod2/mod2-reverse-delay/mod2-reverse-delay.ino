/* REVERSE DELAY

Description:
Classic ambient reverse echo — repeats play backwards for swelling, pre-echo
trails. Audio comes in on the CV jack (sampled by the RP2350 ADC inside the
~36.6 kHz PWM ISR, per firmwares/mod2-fx/README.md). A ~4 s int16 SRAM arena
holds the history; playback replays fixed-length chunks of it in reverse with
overlapping, raised-cosine-windowed grains (normalised overlap-add), so chunk
boundaries and time changes never click. POT1 sets the chunk time (100 ms - 2 s,
or a musical division of a clock on IN1); POT2 sets the re-reversing feedback.
Short button presses cycle the mode: Reverse / Alternating (rev, fwd, rev…) /
Octave-up reverse (2x read = shimmer-lite). Long press is tap tempo. IN2 is a
momentary direction-flip gate (forward "normal delay" pockets). Hold BUTTON +
turn POT1 for wet/dry mix; hold BUTTON + turn POT2 for the per-chunk fade-in
"swell" amount (both persisted to flash with the mode). The LED ramps up across
each reverse sweep, visualising the swell.
DSP lives in the shared sc::ReverseDelayCore (also used by the VCV Rack port).

Key Variables:
  A0 -> Chunk time (100 ms - 2 s, exponential taper) / clock division
  A1 -> Feedback (0 - 95%)  (BTN held: swell / fade-in amount)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║  REVERSE  ║
      ║   delay   ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - chunk time / clock division (BTN held: wet/dry)
      ║   TIME    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - feedback (BTN held: swell / fade-in)
      ║   FDBK    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - ramps up across each reverse sweep (mode ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: mode; long: tap; hold+POT1/2: mix/swell
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - clock in (chunk locks to divisions of it)
      ║ (o)   (o) ║   IN2 (GPIO0) - direction flip (HIGH = forward pockets)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial reverse-delay firmware (maddie synths original, shared
        ReverseDelayCore)

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
#include <EEPROM.h>            // persisted wet/dry + swell + mode
#include <Mod2Common.h>        // Shared MOD2 pin map, PWM-audio setup and helpers
#include <ReverseDelayCore.h>  // Shared reverse-delay DSP (also used by the Rack port)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // below = mode toggle, above = tap
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save
constexpr uint32_t TAP_MAX_MS      = 4000;    // longest usable tap interval (chunk <= 2 s => interval <= ~4 s)
// A clock is "present" while the last IN1 tick is under 4 chunk-lengths old.
constexpr uint32_t CLOCK_MAX_PERIOD =
    (uint32_t)(sc::kReverseMaxChunkSec * mod2::AUDIO_FS);

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  float swell;
  uint8_t mode;
};
constexpr uint32_t SETTINGS_MAGIC = 0x52564431;  // "RVD1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
// ~4 s int16 arena = 2 x max chunk (reversed reads reach ~2x the chunk time
// behind the write head). ~293 KB at ~36.6 kHz (RP2350 has 520 KB).
static int16_t reverseArena[(uint32_t)(2.0f * sc::kReverseMaxChunkSec * mod2::AUDIO_FS) + 16];
sc::ReverseDelayCore reverseDelay;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-delay). */
volatile float   g_chunkSec = 0.5f;
volatile float   g_feedback = 0.4f;
volatile float   g_wet = 0.5f;
volatile float   g_swell = 0.35f;
volatile uint8_t g_mode = sc::REVDLY_REVERSE;

/* ISR -> loop: IN1 clock measurement. */
volatile uint32_t g_clockPeriod = 0;          // samples between the last two ticks
volatile uint32_t g_samplesSinceTick = CLOCK_MAX_PERIOD;

/* loop -> ISR: newest LED sweep level (>=0 forces a mode-ID blink). */
volatile int16_t g_ledForce = -1;

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

  /* --- reverse delay -------------------------------------------------- */
  reverseDelay.chunkSec = g_chunkSec;
  reverseDelay.feedback = g_feedback;
  reverseDelay.wet      = g_wet;
  reverseDelay.swell    = g_swell;
  reverseDelay.mode     = g_mode;
  reverseDelay.flip     = digitalRead(mod2::IN2_PIN) == HIGH;  // forward pocket
  float out = reverseDelay.process(in, 1.0f / mod2::AUDIO_FS);

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: ramps with the reverse sweep (mode-ID blink takes over) --- */
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(reverseDelay.ledLevel() * mod2::PWM_FS));

  pwm_clear_irq(sliceIRQ);
}

/* =======================================================================
 *  Guarded pot read — the ISR reads A2 through the same ADC mux; blocking
 *  the IRQ for the ~2 us conversion just delays one audio sample slightly.
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
  pinMode(A0, INPUT);                       // chunk time / wet-dry (shifted)
  pinMode(A1, INPUT);                       // feedback / swell (shifted)
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // clock in
  pinMode(mod2::IN2_PIN, INPUT);            // direction-flip gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // mode / tap / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_swell = sc::clampf(s.swell, 0.0f, 1.0f);
    g_mode = s.mode < sc::REVDLY_MODE_COUNT ? s.mode : sc::REVDLY_REVERSE;
  }

  reverseDelay.init(reverseArena, sizeof(reverseArena) / sizeof(reverseArena[0]));

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (mode / tap / mix + swell shift), clock sync,
 *  LED mode-ID blink, flash saves
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

  /* --- button: short = mode, long = tap, hold+POT1/2 = mix/swell ------ */
  static bool lastPressed = false;
  static bool adjustingShift = false;   // POT1 or POT2 borrowed for mix/swell
  static bool tapRegistered = false;
  static uint32_t pressStartMs = 0;
  static uint32_t lastTapStartMs = 0;
  static float pot1AtPress = 0.0f;
  static float pot2AtPress = 0.0f;
  static float tapTimeSec = 0.0f;         // 0 = no tap override active
  static mod2::PickupParam timePickup;    // POT1 pickup after a shift / tap
  static mod2::PickupParam fbPickup;      // POT2 pickup after a swell shift
  static bool dirty = false;
  static uint32_t lastChangeMs = 0;

  const bool pressed = digitalRead(mod2::BUTTON_PIN) == LOW;
  const uint32_t now = millis();

  /* blink state (mode-ID feedback: mode+1 flashes) */
  static uint8_t blinkPhasesLeft = 0;
  static uint32_t blinkNextMs = 0;

  if (pressed && !lastPressed) {  // press edge
    pressStartMs = now;
    pot1AtPress = pot1;
    pot2AtPress = pot2;
    adjustingShift = false;
    tapRegistered = false;
  }
  if (pressed && !adjustingShift &&
      (fabsf(pot1 - pot1AtPress) > POT_MOVE_THRESHOLD ||
       fabsf(pot2 - pot2AtPress) > POT_MOVE_THRESHOLD))
    adjustingShift = true;  // shift layer engaged; mode/tap suppressed
  if (pressed && adjustingShift) {
    g_wet = pot1;    // BTN + POT1 -> wet/dry mix
    g_swell = pot2;  // BTN + POT2 -> swell / fade-in amount
    dirty = true;
    lastChangeMs = now;
  }
  /* Long press = tap, timestamped at the press EDGE (rhythm not release-dep). */
  if (pressed && !adjustingShift && !tapRegistered &&
      now - pressStartMs >= SHORT_PRESS_MS) {
    tapRegistered = true;
    const uint32_t interval = pressStartMs - lastTapStartMs;
    lastTapStartMs = pressStartMs;
    if (interval >= SHORT_PRESS_MS && interval <= TAP_MAX_MS) {
      tapTimeSec = sc::clampf(interval * 0.001f, sc::kReverseMinChunkSec,
                              sc::kReverseMaxChunkSec);
      timePickup.targetValue = pot1;  // POT1 gives way until it moves
      timePickup.lastPotValue = pot1;
      timePickup.pickupActive = true;
    }
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustingShift) {
      /* POT1/POT2 were borrowed: freeze time/feedback until each pot is
         turned back across where it sat before the press.               */
      timePickup.targetValue = pot1AtPress;
      timePickup.lastPotValue = pot1;
      timePickup.pickupActive = true;
      fbPickup.targetValue = pot2AtPress;
      fbPickup.lastPotValue = pot2;
      fbPickup.pickupActive = true;
    } else if (!tapRegistered && now - pressStartMs < SHORT_PRESS_MS) {
      g_mode = (g_mode + 1) % sc::REVDLY_MODE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_mode + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT2 -> feedback (unless shifted / waiting for pickup) ---------- */
  if (!pressed) {
    if (mod2::checkPickup(fbPickup, pot2))
      g_feedback = sc::reverseFeedback(pot2);
    fbPickup.lastPotValue = pot2;
  }

  /* --- chunk time: clock > tap > POT1 --------------------------------- */
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
    g_chunkSec = sc::clampf(periodSec * sc::reverseClockRatio(pot1),
                            sc::kReverseMinChunkSec, sc::kReverseMaxChunkSec);
  } else if (tapTimeSec > 0.0f) {
    g_chunkSec = tapTimeSec;
  } else if (!pressed && !timePickup.pickupActive) {
    g_chunkSec = sc::reverseChunkSec(pot1);
  }

  /* --- LED: mode-ID blink (else the ISR ramps it with the sweep) ------ */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to the reverse-sweep ramp
    }
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_wet, g_swell, g_mode};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
