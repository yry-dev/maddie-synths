/* REVERB

Description:
Hall & plate algorithmic reverb. Audio comes in on the CV jack (sampled by the
RP2350 ADC inside the ~36.6 kHz PWM ISR, per firmwares/mod2-fx/README.md). A
Dattorro-style figure-8 tank (input-diffusion allpasses -> two cross-coupled
delay branches with damping one-poles, two branches gently LFO-modulated to
kill metallic ring) lives in the shared sc::ReverbCore (also used by the VCV
Rack port). POT1 sets Size (delay-line scaling + Hall pre-delay), POT2 sets
Decay (feedback gain; the top of the range is musically infinite). Short button
presses switch mode: Hall (long, dark) / Plate (shorter, brighter, denser).
Hold BUTTON + turn POT1 for wet/dry mix and BUTTON + turn POT2 for damping /
tone (both persisted to flash with the mode). A long press latches FREEZE
(infinite tail); IN2 is a momentary FREEZE gate. The LED pulses slowly in Hall
and quickly in Plate, its brightness following the tail level.

Key Variables:
  A0 -> Size (delay scaling + pre-delay) (BTN held: wet/dry mix)
  A1 -> Decay (feedback gain; top ~ infinite) (BTN held: damping/tone)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║  REVERB   ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - size / pre-delay (BTN held: wet/dry)
      ║   SIZE    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - decay (BTN held: damping/tone)
      ║   DECAY   ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - pulses (slow Hall / fast Plate) x tail level
      ║   (BTN)   ║   BTN (GPIO6) - short: mode; long: freeze; hold+POT: mix/damp
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - (spare)
      ║ (o)   (o) ║   IN2 (GPIO0) - freeze gate (HIGH = infinite tail)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial reverb firmware (maddie synths original, shared ReverbCore)

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
#include <EEPROM.h>      // persisted wet/dry + damping + mode
#include <Mod2Common.h>  // Shared MOD2 pin map, PWM-audio setup and helpers
#include <ReverbCore.h>  // Shared reverb DSP (also used by rack-plugins/src/mod2-reverb.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // below = mode, above = freeze latch
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  float damping;
  uint8_t mode;
};
constexpr uint32_t SETTINGS_MAGIC = 0x52564231;  // "RVB1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
// ~0.84 s int16 tank arena in SRAM (~61 KB at 36.6 kHz — well under the
// 200 KB budget; see ReverbCore.h for the memory rationale).
static int16_t reverbArena[sc::reverbArenaSamples(mod2::AUDIO_FS)];
sc::ReverbCore reverb;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes (already mapped to engine units), ISR reads
   (same pattern as mod2-delay / mod2-chorus). */
volatile float   g_sizeScale = sc::reverbSizeScale(0.6f);
volatile float   g_decayGain = sc::reverbDecayGain(0.6f);
volatile float   g_damping   = 0.5f;
volatile float   g_wet       = 0.4f;
volatile uint8_t g_mode      = sc::REVERB_HALL;
volatile bool    g_freeze    = false;   // button-latched freeze
volatile int16_t g_ledForce  = -1;      // >=0: loop-driven blink, -1: tail pulse

/* =======================================================================
 *  PWM interrupt service routine (~36.6 kHz)
 * ==================================================================== */
void __isr onPwmWrap()
{
  /* --- audio input: sample the CV jack at the audio rate ------------- */
  const int raw = analogRead(mod2::CV_PIN);
  const float in = dcBlock.process(raw * (2.0f / 1023.0f) - 1.0f);

  /* --- reverb --------------------------------------------------------- */
  reverb.sizeScale = g_sizeScale;
  reverb.decayGain = g_decayGain;
  reverb.damping   = g_damping;
  reverb.wet       = g_wet;
  reverb.mode      = g_mode;
  // FREEZE = button latch OR IN2 gate held HIGH.
  reverb.freeze    = g_freeze || (digitalRead(mod2::IN2_PIN) == HIGH);
  float out = reverb.process(in, 1.0f / mod2::AUDIO_FS);

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: tail level x a mode-rate pulse (loop can override for the
         mode-ID blink) ------------------------------------------------- */
  const int16_t force = g_ledForce;
  if (force >= 0) {
    pwm_set_chan_level(sliceLed, PWM_CHAN_B, (uint16_t)force);
  } else {
    // Slow pulse in Hall (~0.5 Hz), fast in Plate (~2 Hz), gated by tail level.
    static float ledPhase = 0.0f;
    const float rate = (g_mode == sc::REVERB_PLATE) ? 2.0f : 0.5f;
    ledPhase += rate / mod2::AUDIO_FS;
    if (ledPhase >= 1.0f) ledPhase -= 1.0f;
    const float pulse = 0.5f + 0.5f * sc::reverbLfo(ledPhase);
    const float lvl = reverb.ledLevel() * pulse;
    pwm_set_chan_level(sliceLed, PWM_CHAN_B, (uint16_t)(lvl * mod2::PWM_FS));
  }

  pwm_clear_irq(sliceIRQ);
}

/* =======================================================================
 *  Guarded pot read — the ISR reads A2 through the same ADC mux; blocking the
 *  IRQ for the ~2 us conversion just delays one audio sample slightly.
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
  pinMode(A0, INPUT);                       // size / wet-dry (shifted)
  pinMode(A1, INPUT);                       // decay / damping (shifted)
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // (spare)
  pinMode(mod2::IN2_PIN, INPUT);            // freeze gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // mode / freeze / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_damping = sc::clampf(s.damping, 0.0f, 1.0f);
    g_mode = s.mode < sc::REVERB_MODE_COUNT ? s.mode : sc::REVERB_HALL;
  }

  reverb.init(reverbArena, sizeof(reverbArena) / sizeof(reverbArena[0]));

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (mode / freeze / wet+damp shift layers), flash
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

  /* --- button: short = mode, long = freeze latch, hold+POT1 = wet/dry,
         hold+POT2 = damping ---------------------------------------------- */
  static bool lastPressed = false;
  static bool adjustingWet = false;
  static bool adjustingDamp = false;
  static bool freezeLatched = false;
  static uint32_t pressStartMs = 0;
  static float pot1AtPress = 0.0f, pot2AtPress = 0.0f;
  static mod2::PickupParam sizePickup, decayPickup;
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
  }
  if (pressed && !adjustingWet && fabsf(pot1 - pot1AtPress) > POT_MOVE_THRESHOLD)
    adjustingWet = true;  // shift layer engaged; short/long action suppressed
  if (pressed && !adjustingDamp && fabsf(pot2 - pot2AtPress) > POT_MOVE_THRESHOLD)
    adjustingDamp = true;
  if (pressed && adjustingWet) {
    g_wet = pot1;
    dirty = true;
    lastChangeMs = now;
  }
  if (pressed && adjustingDamp) {
    g_damping = sc::reverbDampingAmount(pot2);
    dirty = true;
    lastChangeMs = now;
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustingWet || adjustingDamp) {
      /* A pot was borrowed for the shift layer: freeze the borrowed
         parameter(s) until the pot is turned back across where it sat.     */
      if (adjustingWet) {
        sizePickup.targetValue = pot1AtPress;
        sizePickup.lastPotValue = pot1;
        sizePickup.pickupActive = true;
      }
      if (adjustingDamp) {
        decayPickup.targetValue = pot2AtPress;
        decayPickup.lastPotValue = pot2;
        decayPickup.pickupActive = true;
      }
    } else if (now - pressStartMs < SHORT_PRESS_MS) {
      // Short press = cycle mode.
      g_mode = (g_mode + 1) % sc::REVERB_MODE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_mode + 1) * 2);  // on/off phases
      blinkNextMs = now;
    } else {
      // Long press = toggle the freeze latch.
      freezeLatched = !freezeLatched;
      g_freeze = freezeLatched;
    }
  }
  lastPressed = pressed;

  /* --- POT1 -> Size (unless shifted / waiting for pickup) ------------- */
  if (!pressed) {
    if (mod2::checkPickup(sizePickup, pot1))
      g_sizeScale = sc::reverbSizeScale(pot1);
    sizePickup.lastPotValue = pot1;
  }

  /* --- POT2 -> Decay (unless shifted / waiting for pickup) ------------ */
  if (!pressed) {
    if (mod2::checkPickup(decayPickup, pot2))
      g_decayGain = sc::reverbDecayGain(pot2);
    decayPickup.lastPotValue = pot2;
  }

  /* --- LED blink code for the newly selected mode --------------------- */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to the tail-level pulse
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
