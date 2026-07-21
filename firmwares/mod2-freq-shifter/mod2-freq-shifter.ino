/* FREQ SHIFTER

Description:
Bode-style single-sideband frequency shifter. Audio comes in on the CV jack
(sampled by the RP2350 ADC inside the ~36.6 kHz PWM ISR, per
firmwares/mod2-fx/README.md) and every partial is shifted by a fixed Hz amount
(not a ratio), turning harmonic sources inharmonic — a slow barberpole shimmer
at small shifts, clangor at large ones. Implemented as a Hilbert-transform SSB
modulator: a two-path IIR phase-difference network makes a quadrature (I/Q)
copy of the input, which is multiplied by a quadrature carrier and recombined
to keep a single sideband. POT1 sets the signed shift amount (centre-detented
zero, fine near the middle), POT2 the feedback (shifted output spiralled back
in — barberpole magic). Short button presses cycle the range: +/-20 Hz
(barberpole) / +/-200 Hz / +/-1 kHz. Hold BUTTON + turn POT1 for wet/dry mix,
hold BUTTON + turn POT2 for the up/down sideband blend (centre = ring-mod-like,
both sidebands) — both persisted to flash with the range. IN1 flips the shift
direction while high, IN2 is a bypass gate. The LED rotates at the shift rate
(visible barberpole for slow shifts, solid above). DSP lives in the shared
sc::FreqShifterCore (also used by the VCV Rack port).

Key Variables:
  A0 -> Shift amount (+/-range, centre = 0 Hz) / wet-dry (BTN held)
  A1 -> Feedback / up-down sideband blend (BTN held)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║ FREQSHIFT ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - shift amount (BTN held: wet/dry mix)
      ║   SHIFT   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - feedback (BTN held: up/down sideband)
      ║   FDBK    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - rotates at the shift rate (blinks range ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: range; hold+POT: shift layer
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - shift direction flip gate (HIGH = flip)
      ║ (o)   (o) ║   IN2 (GPIO0) - bypass gate (HIGH = dry)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial freq shifter firmware (maddie synths original, shared FreqShifterCore)

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
#include <EEPROM.h>            // persisted wet/dry + sideband + range
#include <Mod2Common.h>        // Shared MOD2 pin map, PWM-audio setup and helpers
#include <FreqShifterCore.h>   // Shared SSB DSP (also used by rack-plugins/src/mod2-freq-shifter.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // max length of a mode-cycle press
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  float sideband;
  uint8_t range;
};
constexpr uint32_t SETTINGS_MAGIC = 0x46534831;  // "FSH1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
sc::FreqShifterCore shifter;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-ringmod). */
volatile float   g_shiftHz  = 0.0f;
volatile float   g_feedback = 0.0f;
volatile float   g_sideband = 0.0f;
volatile float   g_wet      = 1.0f;
volatile uint8_t g_range    = sc::FREQSHIFT_1000;
volatile int16_t g_ledForce = -1;  // >=0: loop-driven blink level, -1: rotation

/* =======================================================================
 *  PWM interrupt service routine (~36.6 kHz)
 * ==================================================================== */
void __isr onPwmWrap()
{
  /* --- audio input: sample the CV jack at the audio rate ------------- */
  const int raw = analogRead(mod2::CV_PIN);
  const float in = dcBlock.process(raw * (2.0f / 1023.0f) - 1.0f);

  /* --- frequency shift ------------------------------------------------ */
  shifter.shiftHz  = g_shiftHz;
  shifter.feedback = g_feedback;
  shifter.sideband = g_sideband;
  shifter.wet      = g_wet;
  shifter.range    = g_range;
  shifter.flip     = digitalRead(mod2::IN1_PIN) == HIGH;  // direction flip gate
  float out = shifter.process(in, 1.0f / mod2::AUDIO_FS);

  /* --- IN2: bypass gate (HIGH = pass the dry input) ------------------- */
  if (digitalRead(mod2::IN2_PIN) == HIGH)
    out = in;

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: rotates with the shift, unless loop() is blinking a range ID */
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(shifter.ledLevel() * mod2::PWM_FS));

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
  pinMode(A0, INPUT);                       // shift / wet-dry (shifted)
  pinMode(A1, INPUT);                       // feedback / sideband (shifted)
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // direction flip gate
  pinMode(mod2::IN2_PIN, INPUT);            // bypass gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // range / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_sideband = sc::clampf(s.sideband, 0.0f, 1.0f);
    g_range = s.range < sc::FREQSHIFT_RANGE_COUNT ? s.range : sc::FREQSHIFT_1000;
  }

  shifter.reset();

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (range / dual shift layer), flash saves
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

  /* --- button: short = range, hold + POT1/POT2 = shift ---------------- */
  static bool lastPressed = false;
  static bool adjustingWet = false;     // shift layer: POT1 -> wet/dry
  static bool adjustingSide = false;    // shift layer: POT2 -> sideband blend
  static uint32_t pressStartMs = 0;
  static float pot1AtPress = 0.0f, pot2AtPress = 0.0f;
  static mod2::PickupParam shiftPickup, fbPickup;
  static bool dirty = false;
  static uint32_t lastChangeMs = 0;

  const bool pressed = digitalRead(mod2::BUTTON_PIN) == LOW;
  const uint32_t now = millis();

  /* blink state (range-ID feedback: range+1 flashes) */
  static uint8_t blinkPhasesLeft = 0;
  static uint32_t blinkNextMs = 0;

  if (pressed && !lastPressed) {  // press edge
    pressStartMs = now;
    pot1AtPress = pot1;
    pot2AtPress = pot2;
    adjustingWet = false;
    adjustingSide = false;
  }
  if (pressed && !adjustingWet && fabsf(pot1 - pot1AtPress) > POT_MOVE_THRESHOLD)
    adjustingWet = true;  // shift layer engaged; short-press action suppressed
  if (pressed && !adjustingSide && fabsf(pot2 - pot2AtPress) > POT_MOVE_THRESHOLD)
    adjustingSide = true;
  if (pressed && adjustingWet) {
    g_wet = pot1;
    dirty = true;
    lastChangeMs = now;
  }
  if (pressed && adjustingSide) {
    g_sideband = pot2;
    dirty = true;
    lastChangeMs = now;
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustingWet) {
      /* POT1 was borrowed for wet/dry: freeze the shift until the pot is
         turned back across where it sat before the press.               */
      shiftPickup.targetValue = pot1AtPress;
      shiftPickup.lastPotValue = pot1;
      shiftPickup.pickupActive = true;
    }
    if (adjustingSide) {
      fbPickup.targetValue = pot2AtPress;
      fbPickup.lastPotValue = pot2;
      fbPickup.pickupActive = true;
    }
    if (!adjustingWet && !adjustingSide &&
        now - pressStartMs < SHORT_PRESS_MS) {
      g_range = (g_range + 1) % sc::FREQSHIFT_RANGE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_range + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT1 -> shift amount, POT2 -> feedback (unless shifted/pickup) -- */
  if (!pressed) {
    if (mod2::checkPickup(shiftPickup, pot1))
      g_shiftHz = sc::freqShifterShiftHz(pot1, sc::freqShifterRangeHz(g_range));
    shiftPickup.lastPotValue = pot1;
    if (mod2::checkPickup(fbPickup, pot2))
      g_feedback = sc::freqShifterFeedback(pot2);
    fbPickup.lastPotValue = pot2;
  }

  /* --- LED blink code for the newly selected range -------------------- */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to rotating with the shift
    }
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_wet, g_sideband, g_range};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
