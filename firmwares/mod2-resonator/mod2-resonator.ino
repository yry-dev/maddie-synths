/* RESONATOR

Description:
Rings-like tuned resonator bank. External audio on the CV jack (sampled by the
RP2350 ADC inside the ~36.6 kHz PWM ISR, per firmwares/mod2-fx/README.md) — or
gate-triggered internal noise bursts — excites a bank of tuned resonators.
POT1 sets the fundamental (semitone-quantized, A1..A5), POT2 the structure
(partial spread / string detune spread). Short button presses cycle the mode:
Modal (12 band-pass partials morphing harmonic -> bell) / Comb cluster
(4 chord-tuned feedback combs) / Sympathetic (1 bright driven string + 4
quieter strings at fifths/octaves that ring along). Long presses (>=0.6 s)
strike the bank with the internal noise-burst exciter, as does a trigger on
IN1 — it sings with nothing patched. IN2 chokes the bank while high. Hold
BUTTON + turn POT1 for wet/dry mix, hold BUTTON + turn POT2 for damping /
decay time — both persisted to flash with the mode. The LED follows the bank's
energy. DSP lives in the shared sc::ResonatorCore (also used by the VCV Rack
port).

Key Variables:
  A0 -> Pitch / fundamental (semitone-quantized, A1 - A5)
  A1 -> Structure (partial spread / string detune spread)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║ RESONATOR ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - pitch, quantized (BTN held: wet/dry mix)
      ║   PITCH   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - structure (BTN held: damping / decay)
      ║   STRUCT  ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - bank energy (blinks mode ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: mode; long: strike; hold+POT: shift
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - strike trigger (internal noise-burst exciter)
      ║ (o)   (o) ║   IN2 (GPIO0) - damp gate (HIGH = choke the bank)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial resonator firmware (maddie synths original, shared ResonatorCore)

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
#include <EEPROM.h>         // persisted wet/dry + damping + mode
#include <Mod2Common.h>     // Shared MOD2 pin map, PWM-audio setup and helpers
#include <ResonatorCore.h>  // Shared resonator DSP (also used by rack-plugins/src/mod2-resonator.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // below = mode cycle, above = strike
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  float damping;
  uint8_t mode;
};
constexpr uint32_t SETTINGS_MAGIC = 0x52534E31;  // "RSN1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
// 5 delay segments, each a full period at the lowest pitch (~7.3 KB total).
static int16_t resonatorArena[(uint32_t)sc::kResonatorStrings *
                              ((uint32_t)(mod2::AUDIO_FS / 50.0f) + 8)];
sc::ResonatorCore resonator;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-bitcrusher). */
volatile float   g_pitchHz  = 220.0f;
volatile float   g_struct   = 0.5f;
volatile float   g_damping  = 0.5f;
volatile uint8_t g_mode     = sc::RESONATOR_MODAL;
volatile float   g_wet      = 0.5f;
volatile bool    g_strike   = false;  // loop -> ISR strike request (button)
volatile int16_t g_ledForce = -1;     // >=0: loop-driven blink level, -1: energy

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

  /* --- IN1: strike trigger (sample-accurate edge detect) -------------- */
  const bool in1 = digitalRead(mod2::IN1_PIN) == HIGH;
  if ((in1 && !isrLastIn1) || g_strike) {
    resonator.strike();
    g_strike = false;
  }
  isrLastIn1 = in1;

  /* --- resonate ------------------------------------------------------- */
  resonator.pitchHz   = g_pitchHz;
  resonator.structure = g_struct;
  resonator.damping   = g_damping;
  resonator.mode      = g_mode;
  resonator.wet       = g_wet;
  resonator.dampGate  = digitalRead(mod2::IN2_PIN) == HIGH;
  float out = resonator.process(in, 1.0f / mod2::AUDIO_FS);

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: bank energy, unless loop() is blinking a mode ID ---------- */
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(resonator.ledLevel() * mod2::PWM_FS));

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
  pinMode(A0, INPUT);                       // pitch / wet-dry (shifted)
  pinMode(A1, INPUT);                       // structure / damping (shifted)
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // strike trigger
  pinMode(mod2::IN2_PIN, INPUT);            // damp gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // mode / strike / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_damping = sc::clampf(s.damping, 0.0f, 1.0f);
    g_mode = s.mode < sc::RESONATOR_MODE_COUNT ? s.mode : sc::RESONATOR_MODAL;
  }

  resonator.init(resonatorArena,
                 sizeof(resonatorArena) / sizeof(resonatorArena[0]));

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (mode / strike / dual shift layer), flash saves
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

  /* --- button: short = mode, long = strike, hold + POT1/POT2 = shift -- */
  static bool lastPressed = false;
  static bool adjustingWet = false;     // shift layer: POT1 -> wet/dry
  static bool adjustingDamp = false;    // shift layer: POT2 -> damping
  static bool struck = false;
  static uint32_t pressStartMs = 0;
  static float pot1AtPress = 0.0f, pot2AtPress = 0.0f;
  static mod2::PickupParam pitchPickup, structPickup;
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
    struck = false;
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
  /* Long press = strike, fired once the hold crosses the threshold (the
     same long-press convention as the delay's tap).                     */
  if (pressed && !adjustingWet && !adjustingDamp && !struck &&
      now - pressStartMs >= SHORT_PRESS_MS) {
    struck = true;
    g_strike = true;
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustingWet) {
      /* POT1 was borrowed for wet/dry: freeze the pitch until the pot is
         turned back across where it sat before the press.               */
      pitchPickup.targetValue = pot1AtPress;
      pitchPickup.lastPotValue = pot1;
      pitchPickup.pickupActive = true;
    }
    if (adjustingDamp) {
      structPickup.targetValue = pot2AtPress;
      structPickup.lastPotValue = pot2;
      structPickup.pickupActive = true;
    }
    if (!adjustingWet && !adjustingDamp && !struck &&
        now - pressStartMs < SHORT_PRESS_MS) {
      g_mode = (g_mode + 1) % sc::RESONATOR_MODE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_mode + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT1 -> pitch, POT2 -> structure (unless shifted / pickup) ----- */
  if (!pressed) {
    if (mod2::checkPickup(pitchPickup, pot1))
      g_pitchHz = sc::resonatorPitchHz(pot1);
    pitchPickup.lastPotValue = pot1;
    if (mod2::checkPickup(structPickup, pot2))
      g_struct = pot2;
    structPickup.lastPotValue = pot2;
  }

  /* --- LED blink code for the newly selected mode --------------------- */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to following the bank energy
    }
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_wet, g_damping, g_mode};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
