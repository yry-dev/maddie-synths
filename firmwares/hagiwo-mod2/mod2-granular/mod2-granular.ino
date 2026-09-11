/* GRANULAR

Description:
Live-buffer granular delay / cloud textures. Audio comes in on the CV jack
(sampled by the RP2350 ADC inside the ~36.6 kHz PWM ISR, per
firmwares/mod2-fx/README.md). A continuously-recorded buffer (~4.8 s, int16) is
sprayed into tiny windowed grains: Clouds-adjacent smears, stutters and
shimmering textures from any input. POT1 sets grain size (10 - 250 ms), POT2 is
a single "texture" macro that opens up grain density, position spray and pitch
jitter together. Short button presses cycle the grain character: Smooth (Hann
window) / Perc (percussive expodec) / Reverse (grains read backwards). Hold
BUTTON + turn POT1 for wet/dry mix; hold BUTTON + turn POT2 for grain pitch
(quantised octaves/fifths around unity). IN1 is an external grain trigger (patch
a clock for rhythmic granular); IN2 freezes the buffer (stops recording, keeps
granulating the held audio). The LED flickers on every grain spawn, so the
density is visible. Wet/dry, pitch and mode persist to flash.
DSP lives in the shared sc::GranularCore (also used by the VCV Rack port).

Key Variables:
  A0 -> Grain size (10 - 250 ms)   (BTN held: wet/dry mix)
  A1 -> Texture macro (density + spray + pitch jitter)  (BTN held: grain pitch)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║ GRANULAR  ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - grain size (BTN held: wet/dry mix)
      ║   SIZE    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - texture macro (BTN held: grain pitch)
      ║  TEXTURE  ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - flickers per grain spawn (blinks mode ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: character; hold+POT1: mix; +POT2: pitch
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - grain trigger (ext clock -> rhythmic grains)
      ║ (o)   (o) ║   IN2 (GPIO0) - freeze gate (HIGH = stop recording)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial granular firmware (maddie synths original, shared GranularCore)

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
#include <EEPROM.h>       // persisted wet/dry + pitch + mode
#include <Mod2Common.h>   // Shared MOD2 pin map, PWM-audio setup and helpers
#include <GranularCore.h> // Shared granular DSP (also used by rack-plugins/src/mod2-granular.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // max length of a mode-cycle press
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float mix;
  float pitchSemi;
  uint8_t mode;
};
constexpr uint32_t SETTINGS_MAGIC = 0x47524e31;  // "GRN1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
// ~4.8 s int16 record buffer in SRAM (~350 KB — RAM near max, per the plan).
static int16_t granArena[(uint32_t)(sc::kGranularBufferSec * mod2::AUDIO_FS) + 16];
sc::GranularCore granular;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-chorus). */
volatile float   g_sizeSec   = 0.08f;
volatile float   g_density   = 0.5f;
volatile float   g_pitchSemi = 0.0f;
volatile float   g_mix       = 0.5f;
volatile uint8_t g_mode      = sc::GRAN_SMOOTH;
volatile int16_t g_ledForce  = -1;  // >=0: loop-driven blink level, -1: grain flicker

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

  /* --- IN1: rising edge spawns a grain (external-clock granular) ------ */
  const bool in1 = digitalRead(mod2::IN1_PIN) == HIGH;
  if (in1 && !isrLastIn1)
    granular.triggerGrain();
  isrLastIn1 = in1;

  /* --- granular ------------------------------------------------------- */
  granular.sizeSec   = g_sizeSec;
  granular.density   = g_density;
  granular.pitchSemi = g_pitchSemi;
  granular.mix       = g_mix;
  granular.mode      = g_mode;
  granular.freeze    = digitalRead(mod2::IN2_PIN) == HIGH;  // freeze gate
  float out = granular.process(in, 1.0f / mod2::AUDIO_FS);

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: flickers per grain spawn, unless loop() is blinking a mode */
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(granular.ledLevel() * mod2::PWM_FS));

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
  pinMode(A0, INPUT);                       // grain size / wet-dry (shifted)
  pinMode(A1, INPUT);                       // texture / grain pitch (shifted)
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // grain trigger
  pinMode(mod2::IN2_PIN, INPUT);            // freeze gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // character / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_mix = sc::clampf(s.mix, 0.0f, 1.0f);
    g_pitchSemi = sc::clampf(s.pitchSemi, -12.0f, 12.0f);
    g_mode = s.mode < sc::GRAN_MODE_COUNT ? s.mode : sc::GRAN_SMOOTH;
  }

  granular.init(granArena, sizeof(granArena) / sizeof(granArena[0]));

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (character / two-knob shift layer), flash saves
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

  /* --- button: short press = character, hold + POT1/POT2 = shift ------ */
  static bool lastPressed = false;
  static bool adjustMix = false;    // POT1 borrowed for wet/dry
  static bool adjustPitch = false;  // POT2 borrowed for grain pitch
  static uint32_t pressStartMs = 0;
  static float pot1AtPress = 0.0f, pot2AtPress = 0.0f;
  static mod2::PickupParam sizePickup;     // POT1 -> grain size
  static mod2::PickupParam densityPickup;  // POT2 -> texture
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
    adjustMix = false;
    adjustPitch = false;
  }
  if (pressed) {
    if (!adjustMix && fabsf(pot1 - pot1AtPress) > POT_MOVE_THRESHOLD)
      adjustMix = true;    // shift layer engaged on POT1
    if (!adjustPitch && fabsf(pot2 - pot2AtPress) > POT_MOVE_THRESHOLD)
      adjustPitch = true;  // shift layer engaged on POT2
    if (adjustMix) {
      g_mix = pot1;
      dirty = true;
      lastChangeMs = now;
    }
    if (adjustPitch) {
      g_pitchSemi = sc::granularPitchSemi(pot2);
      dirty = true;
      lastChangeMs = now;
    }
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustMix) {  // POT1 borrowed: hold grain size until the pot returns
      sizePickup.targetValue = pot1AtPress;
      sizePickup.lastPotValue = pot1;
      sizePickup.pickupActive = true;
    }
    if (adjustPitch) {  // POT2 borrowed: hold texture until the pot returns
      densityPickup.targetValue = pot2AtPress;
      densityPickup.lastPotValue = pot2;
      densityPickup.pickupActive = true;
    }
    if (!adjustMix && !adjustPitch && now - pressStartMs < SHORT_PRESS_MS) {
      g_mode = (g_mode + 1) % sc::GRAN_MODE_COUNT;  // cycle grain character
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_mode + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT1 -> grain size, POT2 -> texture (unless shifted / pickup) -- */
  if (!pressed) {
    if (mod2::checkPickup(sizePickup, pot1))
      g_sizeSec = sc::granularSizeSec(pot1);
    sizePickup.lastPotValue = pot1;
    if (mod2::checkPickup(densityPickup, pot2))
      g_density = pot2;
    densityPickup.lastPotValue = pot2;
  }

  /* --- LED blink code for the newly selected character ---------------- */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to per-grain flicker
    }
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_mix, g_pitchSemi, g_mode};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
