/* COMB

Description:
Tuned comb filter — the simplest way to make any source "ring". Audio comes in
on the CV jack (sampled by the RP2350 ADC inside the ~36.6 kHz PWM ISR, per
firmwares/mod2-fx/README.md). POT1 tunes the comb frequency (20 Hz - 2 kHz,
semitone-quantized by default), POT2 sets a bipolar feedback: CCW is a negative
comb (odd harmonics, hollow / square-ish), centre is none, CW is a positive
comb (full harmonic series). Push the feedback to the extremes and the comb
rings, then self-oscillates into a soft-limited sine-ish drone. Short button
presses cycle the mode: Feedback (resonant) / Feedforward (non-resonant notches)
/ Both (nested all-pass shimmer). A long press toggles semitone-quantize vs
free-tune. Hold BUTTON + turn POT1 for wet/dry mix, hold BUTTON + turn POT2 for
damping (a low-pass in the feedback path that rounds the ring) — both persisted
to flash with the mode and the quantize flag. IN2 kills the feedback while high
(chokes the ring). The LED follows the comb's resonant energy. DSP lives in the
shared sc::CombCore (also used by the VCV Rack port).

Key Variables:
  A0 -> Tune (20 Hz - 2 kHz, semitone-quantized; BTN held: wet/dry mix)
  A1 -> Feedback (bipolar; BTN held: damping)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║   COMB    ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - tune (BTN held: wet/dry mix)
      ║   TUNE    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - feedback (BTN held: damping)
      ║   FDBK    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - resonant energy (blinks mode ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: mode; long: quantize; hold+POT: shift
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - (spare)
      ║ (o)   (o) ║   IN2 (GPIO0) - feedback kill gate (HIGH = choke the ring)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial comb firmware (maddie synths original, shared CombCore)

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
#include <EEPROM.h>      // persisted wet/dry + damping + mode + quantize
#include <Mod2Common.h>  // Shared MOD2 pin map, PWM-audio setup and helpers
#include <CombCore.h>    // Shared comb DSP (also used by rack-plugins/src/mod2-comb.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // below = mode cycle, above = quantize toggle
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  float damping;
  uint8_t mode;
  uint8_t quantize;
};
constexpr uint32_t SETTINGS_MAGIC = 0x434F4D31;  // "COM1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
// One period at the lowest tuned frequency (~3.7 KB int16 arena in SRAM).
static int16_t combArena[(uint32_t)(mod2::AUDIO_FS / sc::kCombMinFreq) + 16];
sc::CombCore comb;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-chorus). */
volatile float   g_freqHz   = 220.0f;
volatile float   g_feedback = 0.0f;
volatile float   g_damping  = 0.3f;
volatile uint8_t g_mode     = sc::COMB_FEEDBACK;
volatile float   g_wet      = 0.5f;
volatile bool    g_quantize = true;  // semitone-quantize tune (loop-owned, flash-seeded)
volatile int16_t g_ledForce = -1;  // >=0: loop-driven blink level, -1: energy

/* =======================================================================
 *  PWM interrupt service routine (~36.6 kHz)
 * ==================================================================== */
void __isr onPwmWrap()
{
  /* --- audio input: sample the CV jack at the audio rate ------------- */
  const int raw = analogRead(mod2::CV_PIN);
  const float in = dcBlock.process(raw * (2.0f / 1023.0f) - 1.0f);

  /* --- comb ----------------------------------------------------------- */
  comb.freqHz   = g_freqHz;
  comb.feedback = g_feedback;
  comb.damping  = g_damping;
  comb.mode     = g_mode;
  comb.wet      = g_wet;
  comb.fbKill   = digitalRead(mod2::IN2_PIN) == HIGH;  // choke the ring while high
  float out = comb.process(in, 1.0f / mod2::AUDIO_FS);

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: resonant energy, unless loop() is blinking a mode ID ------ */
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(comb.ledLevel() * mod2::PWM_FS));

  pwm_clear_irq(sliceIRQ);
}

/* =======================================================================
 *  Guarded pot read — the ISR reads A2 through the same ADC mux (see the
 *  mod2-chorus note); blocking the IRQ for the ~2 us conversion just delays
 *  one audio sample slightly.
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
  pinMode(A0, INPUT);                       // tune / wet-dry (shifted)
  pinMode(A1, INPUT);                       // feedback / damping (shifted)
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // (spare)
  pinMode(mod2::IN2_PIN, INPUT);            // feedback kill gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // mode / quantize / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  bool quantize = true;  // semitone-quantized tune by default
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_damping = sc::clampf(s.damping, 0.0f, 1.0f);
    g_mode = s.mode < sc::COMB_MODE_COUNT ? s.mode : sc::COMB_FEEDBACK;
    quantize = s.quantize != 0;
  }

  comb.init(combArena, sizeof(combArena) / sizeof(combArena[0]));

  g_quantize = quantize;

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (mode / quantize / dual shift layer), saves
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

  /* --- button: short = mode, long = quantize, hold + POT1/POT2 = shift  */
  static bool lastPressed = false;
  static bool adjustingWet = false;     // shift layer: POT1 -> wet/dry
  static bool adjustingDamp = false;    // shift layer: POT2 -> damping
  static bool toggled = false;          // long-press quantize toggle fired
  static uint32_t pressStartMs = 0;
  static float pot1AtPress = 0.0f, pot2AtPress = 0.0f;
  static mod2::PickupParam tunePickup, fbPickup;
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
    adjustingDamp = false;
    toggled = false;
  }
  if (pressed && !adjustingWet && fabsf(pot1 - pot1AtPress) > POT_MOVE_THRESHOLD)
    adjustingWet = true;   // shift layer engaged; short/long actions suppressed
  if (pressed && !adjustingDamp && fabsf(pot2 - pot2AtPress) > POT_MOVE_THRESHOLD)
    adjustingDamp = true;
  if (pressed && adjustingWet) {
    g_wet = pot1;
    dirty = true;
    lastChangeMs = now;
  }
  if (pressed && adjustingDamp) {
    g_damping = pot2;
    dirty = true;
    lastChangeMs = now;
  }
  /* Long press (no shift) = toggle semitone-quantize, fired once the hold
     crosses the threshold. Blink twice to confirm.                       */
  if (pressed && !adjustingWet && !adjustingDamp && !toggled &&
      now - pressStartMs >= SHORT_PRESS_MS) {
    toggled = true;
    g_quantize = !g_quantize;
    g_freqHz = sc::combFreqHz(pot1, g_quantize);  // re-tune immediately
    dirty = true;
    lastChangeMs = now;
    blinkPhasesLeft = 4;  // two on/off flashes
    blinkNextMs = now;
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustingWet) {
      /* POT1 was borrowed for wet/dry: freeze the tune until the pot is
         turned back across where it sat before the press.               */
      tunePickup.targetValue = pot1AtPress;
      tunePickup.lastPotValue = pot1;
      tunePickup.pickupActive = true;
    }
    if (adjustingDamp) {
      fbPickup.targetValue = pot2AtPress;
      fbPickup.lastPotValue = pot2;
      fbPickup.pickupActive = true;
    }
    if (!adjustingWet && !adjustingDamp && !toggled &&
        now - pressStartMs < SHORT_PRESS_MS) {
      g_mode = (g_mode + 1) % sc::COMB_MODE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_mode + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT1 -> tune, POT2 -> feedback (unless shifted / pickup) -------- */
  if (!pressed) {
    if (mod2::checkPickup(tunePickup, pot1))
      g_freqHz = sc::combFreqHz(pot1, g_quantize);
    tunePickup.lastPotValue = pot1;
    if (mod2::checkPickup(fbPickup, pot2))
      g_feedback = sc::combFeedback(pot2);
    fbPickup.lastPotValue = pot2;
  }

  /* --- LED blink code for the newly selected mode / quantize toggle --- */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to following the resonant energy
    }
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_wet, g_damping, g_mode,
                  (uint8_t)(g_quantize ? 1 : 0)};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
