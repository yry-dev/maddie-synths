/* DYNAMICS

Description:
One-knob compressor + limiter (+ trigger ducker) for the MOD2. Audio comes in
on the CV jack (sampled by the RP2350 ADC inside the ~36.6 kHz PWM ISR, per
firmwares/mod2-fx/README.md). Squashing the signal *before* the 10-bit PWM
quantization audibly improves the module's perceived quality. POT1 is the one
"Amount" knob (threshold + ratio + makeup swept together, gentle glue .. smash);
POT2 is the release time (30 ms - 1 s, attack auto-scaled). Short button presses
cycle the mode: Compressor / Limiter (fast attack + ~1 ms lookahead) / Ducker
(IN1 trigger fires a decaying envelope that ducks the audio). Hold BUTTON +
turn POT1 for parallel dry blend, hold BUTTON + turn POT2 for output trim; both
persisted to flash with the mode. IN2 is a bypass gate. The LED is a
gain-reduction meter (brighter = more GR).
DSP lives in the shared sc::DynamicsCore (also used by the VCV Rack port).

Key Variables:
  A0 -> Amount (BTN held: parallel dry blend)
  A1 -> Release (30 ms - 1 s; BTN held: output trim)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║ DYNAMICS  ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - Amount (BTN held: dry blend)
      ║  AMOUNT   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - Release (BTN held: output trim)
      ║ RELEASE   ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - gain-reduction meter (blinks mode ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: mode; hold+POT1/2: dry / trim
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - sidechain trigger (ducks in Ducker mode)
      ║ (o)   (o) ║   IN2 (GPIO0) - bypass gate (HIGH = dry)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial dynamics firmware (maddie synths original, shared DynamicsCore)

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
#include <EEPROM.h>        // persisted mode + dry blend + output trim
#include <Mod2Common.h>    // Shared MOD2 pin map, PWM-audio setup and helpers
#include <DynamicsCore.h>  // Shared dynamics DSP (also used by rack-plugins/src/mod2-dynamics.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // max length of a mode-cycle press
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  uint8_t mode;
  float dryMix;
  float outTrim;
};
constexpr uint32_t SETTINGS_MAGIC = 0x44594e31;  // "DYN1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
sc::DynamicsCore dynamics;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-chorus). */
volatile float   g_amount   = 0.3f;
volatile float   g_release  = 0.15f;  // seconds
volatile float   g_dryMix   = 0.0f;
volatile float   g_outTrim  = 1.0f;
volatile uint8_t g_mode     = sc::DYN_COMPRESSOR;
volatile int16_t g_ledForce = -1;     // >=0: loop-driven blink level, -1: GR meter

/* =======================================================================
 *  PWM interrupt service routine (~36.6 kHz)
 * ==================================================================== */
void __isr onPwmWrap()
{
  /* --- audio input: sample the CV jack at the audio rate ------------- */
  const int raw = analogRead(mod2::CV_PIN);
  const float in = dcBlock.process(raw * (2.0f / 1023.0f) - 1.0f);

  /* --- IN1: sidechain trigger — fire the ducker envelope on a rising edge */
  static bool in1Prev = false;
  const bool in1 = digitalRead(mod2::IN1_PIN) == HIGH;
  if (in1 && !in1Prev)
    dynamics.duckTrigger();
  in1Prev = in1;

  /* --- dynamics ------------------------------------------------------- */
  dynamics.amount     = g_amount;
  dynamics.releaseSec = g_release;
  dynamics.dryMix     = g_dryMix;
  dynamics.outTrim    = g_outTrim;
  dynamics.mode       = g_mode;
  float out = dynamics.process(in, 1.0f / mod2::AUDIO_FS);

  /* --- IN2: bypass gate (HIGH = pass the dry input) ------------------- */
  if (digitalRead(mod2::IN2_PIN) == HIGH)
    out = in;

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: gain-reduction meter, unless loop() is blinking a mode ID -- */
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(dynamics.ledLevel() * mod2::PWM_FS));

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
  pinMode(A0, INPUT);                       // amount / dry blend (shifted)
  pinMode(A1, INPUT);                       // release / output trim (shifted)
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // sidechain trigger (ducker)
  pinMode(mod2::IN2_PIN, INPUT);            // bypass gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // mode / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_mode = s.mode < sc::DYN_MODE_COUNT ? s.mode : sc::DYN_COMPRESSOR;
    g_dryMix = sc::clampf(s.dryMix, 0.0f, 1.0f);
    g_outTrim = sc::clampf(s.outTrim, 0.0f, 2.0f);
  }

  dynamics.dryMix = g_dryMix;
  dynamics.outTrim = g_outTrim;
  dynamics.mode = g_mode;
  dynamics.reset();

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (mode / dry-blend + trim shift layers), saves
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

  /* --- button: short = mode, hold + POT1 = dry blend, hold + POT2 = trim */
  static bool lastPressed = false;
  static bool adjustedDry = false;   // POT1 borrowed this press
  static bool adjustedTrim = false;  // POT2 borrowed this press
  static uint32_t pressStartMs = 0;
  static float pot1AtPress = 0.0f, pot2AtPress = 0.0f;
  static mod2::PickupParam amountPickup, releasePickup;
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
    adjustedDry = false;
    adjustedTrim = false;
  }
  if (pressed) {
    if (fabsf(pot1 - pot1AtPress) > POT_MOVE_THRESHOLD) adjustedDry = true;
    if (fabsf(pot2 - pot2AtPress) > POT_MOVE_THRESHOLD) adjustedTrim = true;
    if (adjustedDry) {
      g_dryMix = pot1;
      dirty = true;
      lastChangeMs = now;
    }
    if (adjustedTrim) {
      g_outTrim = pot2 * 2.0f;  // 0 .. 2x, unity at centre
      dirty = true;
      lastChangeMs = now;
    }
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustedDry) {  // POT1 was borrowed: freeze Amount until it returns
      amountPickup.targetValue = pot1AtPress;
      amountPickup.lastPotValue = pot1;
      amountPickup.pickupActive = true;
    }
    if (adjustedTrim) {  // POT2 was borrowed: freeze Release until it returns
      releasePickup.targetValue = pot2AtPress;
      releasePickup.lastPotValue = pot2;
      releasePickup.pickupActive = true;
    }
    if (!adjustedDry && !adjustedTrim && now - pressStartMs < SHORT_PRESS_MS) {
      g_mode = (g_mode + 1) % sc::DYN_MODE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_mode + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT1 -> Amount, POT2 -> Release (unless shifted / awaiting pickup) */
  if (!pressed) {
    if (mod2::checkPickup(amountPickup, pot1))
      g_amount = pot1;
    amountPickup.lastPotValue = pot1;
    if (mod2::checkPickup(releasePickup, pot2))
      g_release = sc::dynamicsReleaseSec(pot2);
    releasePickup.lastPotValue = pot2;
  }

  /* --- LED blink code for the newly selected mode --------------------- */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to the gain-reduction meter
    }
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_mode, g_dryMix, g_outTrim};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
