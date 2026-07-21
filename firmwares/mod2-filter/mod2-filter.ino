/* FILTER

Description:
Tilt EQ + resonant cleanup filters. Audio comes in on the CV jack (sampled by
the RP2350 ADC inside the ~36.6 kHz PWM ISR, per firmwares/mod2-fx/README.md).
The unglamorous patch-saver: tame a harsh source, clean rumble, tilt a mix
element dark/bright — plus a resonant 2-pole state-variable filter for bonus
performance use. POT1 sets the cutoff / tilt pivot (20 Hz - 16 kHz), POT2 the
resonance (filter modes) or tilt amount (EQ, bipolar +/-6 dB). Short button
presses cycle the mode: Tilt EQ / Low-pass / High-pass / Band-pass. Hold BUTTON
+ turn POT1 for wet/dry mix, hold BUTTON + turn POT2 for output trim; both are
persisted to flash with the mode. IN2 is a bypass gate. The LED tracks the
output level. DSP lives in the shared sc::FilterCore (also used by the VCV Rack
port).

Key Variables:
  A0 -> cutoff / tilt pivot (20 Hz - 16 kHz, exponential)  (BTN held: wet/dry)
  A1 -> resonance (filter) or tilt amount (EQ)             (BTN held: trim)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║  FILTER   ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - cutoff / pivot (BTN held: wet/dry mix)
      ║   FREQ    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - resonance / tilt (BTN held: output trim)
      ║   RES     ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - follows the output level (blinks mode ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: mode; hold+POT1/POT2: wet/trim
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - (spare)
      ║ (o)   (o) ║   IN2 (GPIO0) - bypass gate (HIGH = dry)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial filter firmware (maddie synths original, shared FilterCore)

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
#include <EEPROM.h>      // persisted wet/dry + trim + mode
#include <Mod2Common.h>  // Shared MOD2 pin map, PWM-audio setup and helpers
#include <FilterCore.h>  // Shared filter DSP (also used by rack-plugins/src/mod2-filter.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // max length of a mode-cycle press
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  float trim;
  uint8_t mode;
};
constexpr uint32_t SETTINGS_MAGIC = 0x464C5431;  // "FLT1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
sc::FilterCore filt;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-chorus). */
volatile float   g_cutoffPot = 0.5f;
volatile float   g_shapePot  = 0.5f;
volatile uint8_t g_mode      = sc::FILTER_TILT;
volatile float   g_wet       = 1.0f;
volatile float   g_trim      = 1.0f;
volatile int16_t g_ledForce  = -1;  // >=0: loop-driven blink level, -1: follow level

/* =======================================================================
 *  PWM interrupt service routine (~36.6 kHz)
 * ==================================================================== */
void __isr onPwmWrap()
{
  /* --- audio input: sample the CV jack at the audio rate ------------- */
  const int raw = analogRead(mod2::CV_PIN);
  const float in = dcBlock.process(raw * (2.0f / 1023.0f) - 1.0f);

  /* --- filter --------------------------------------------------------- */
  filt.cutoffPot = g_cutoffPot;
  filt.shapePot  = g_shapePot;
  filt.mode      = g_mode;
  filt.wet       = g_wet;
  filt.trim      = g_trim;
  float out = filt.process(in, 1.0f / mod2::AUDIO_FS);

  /* --- IN2: bypass gate (HIGH = pass the dry input) ------------------- */
  if (digitalRead(mod2::IN2_PIN) == HIGH)
    out = in;

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: follows the output level, unless loop() is blinking a mode ID */
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(filt.ledLevel() * mod2::PWM_FS));

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
  pinMode(A0, INPUT);                       // cutoff / wet-dry (shifted)
  pinMode(A1, INPUT);                       // resonance-tilt / trim (shifted)
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // (spare)
  pinMode(mod2::IN2_PIN, INPUT);            // bypass gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // mode / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet  = sc::clampf(s.wet, 0.0f, 1.0f);
    g_trim = sc::clampf(s.trim, 0.0f, 1.0f);
    g_mode = s.mode < sc::FILTER_MODE_COUNT ? s.mode : sc::FILTER_TILT;
  }

  filt.mode = g_mode;
  filt.wet = g_wet;
  filt.trim = g_trim;
  filt.reset();

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (mode / wet + trim shift layers), flash saves
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

  /* --- button: short press = mode; hold + POT1 = wet, + POT2 = trim --- */
  static bool lastPressed = false;
  static bool adjustingWet = false;
  static bool adjustingTrim = false;
  static uint32_t pressStartMs = 0;
  static float pot1AtPress = 0.0f, pot2AtPress = 0.0f;
  static mod2::PickupParam freqPickup;   // POT1 (cutoff) after a wet shift
  static mod2::PickupParam shapePickup;  // POT2 (res/tilt) after a trim shift
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
    adjustingWet = false;
    adjustingTrim = false;
  }
  if (pressed) {
    if (!adjustingWet && fabsf(pot1 - pot1AtPress) > POT_MOVE_THRESHOLD)
      adjustingWet = true;   // POT1 shift layer engaged; short-press suppressed
    if (!adjustingTrim && fabsf(pot2 - pot2AtPress) > POT_MOVE_THRESHOLD)
      adjustingTrim = true;  // POT2 shift layer engaged; short-press suppressed
    if (adjustingWet) {
      g_wet = pot1;
      dirty = true;
      lastChangeMs = now;
    }
    if (adjustingTrim) {
      g_trim = pot2;
      dirty = true;
      lastChangeMs = now;
    }
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustingWet || adjustingTrim) {
      /* A pot was borrowed for a shift layer: freeze that param until the pot
         is turned back across where it sat before the press. */
      if (adjustingWet) {
        freqPickup.targetValue = pot1AtPress;
        freqPickup.lastPotValue = pot1;
        freqPickup.pickupActive = true;
      }
      if (adjustingTrim) {
        shapePickup.targetValue = pot2AtPress;
        shapePickup.lastPotValue = pot2;
        shapePickup.pickupActive = true;
      }
    } else if (now - pressStartMs < SHORT_PRESS_MS) {
      g_mode = (g_mode + 1) % sc::FILTER_MODE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_mode + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT1 -> cutoff, POT2 -> res/tilt (unless shifted / awaiting pickup) */
  if (!pressed) {
    if (mod2::checkPickup(freqPickup, pot1))
      g_cutoffPot = pot1;
    freqPickup.lastPotValue = pot1;
    if (mod2::checkPickup(shapePickup, pot2))
      g_shapePot = pot2;
    shapePickup.lastPotValue = pot2;
  }

  /* --- LED blink code for the newly selected mode --------------------- */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to following the output level
    }
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_wet, g_trim, g_mode};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
