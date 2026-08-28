/* RING MOD

Description:
Carrier-oscillator ring modulator. Audio comes in on the CV jack (sampled by
the RP2350 ADC inside the ~36.6 kHz PWM ISR, per firmwares/mod2-fx/README.md)
and is multiplied by an internal phase-continuous carrier. POT1 sets the
carrier frequency (0.5 Hz - 5 kHz, exponential — sub-audio = tremolo-ish AM),
POT2 morphs the carrier shape sine -> triangle -> square (harsher products).
Short button presses cycle the carrier mode: Fixed / Track (a decimated
autocorrelation pitch detector slews the carrier to a POT1-set ratio of the
input pitch — clangorous but harmonically related; a confidence gate freezes
the carrier when tracking is garbage) / S&H (a new random carrier per IN1
trigger). Hold BUTTON + turn POT1 for wet/dry mix, hold BUTTON + turn POT2
for the AM <-> ring-mod blend (how much dry leaks through the multiply) —
both persisted to flash with the mode. IN1 hard-syncs the carrier, IN2 drops
it an octave while high (instant "broken speaker"). The LED blinks at the
carrier rate (visible sub-audio, solid above). DSP lives in the shared
sc::RingModCore (also used by the VCV Rack port).

Key Variables:
  A0 -> Carrier frequency (0.5 Hz - 5 kHz) / Track-mode ratio (0.25 - 4)
  A1 -> Carrier shape morph (sine -> triangle -> square)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║ RING MOD  ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - carrier freq / ratio (BTN held: wet/dry)
      ║   FREQ    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - carrier shape (BTN held: AM/ring blend)
      ║   SHAPE   ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - blinks at carrier rate (blinks mode ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: mode; hold+POT: shift
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - carrier hard-sync / S&H trigger
      ║ (o)   (o) ║   IN2 (GPIO0) - octave-drop gate (HIGH = -1 oct)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial ring mod firmware (maddie synths original, shared RingModCore)

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
#include <EEPROM.h>       // persisted wet/dry + AM blend + mode
#include <Mod2Common.h>   // Shared MOD2 pin map, PWM-audio setup and helpers
#include <RingModCore.h>  // Shared ring mod DSP (also used by rack-plugins/src/mod2-ringmod.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // max length of a mode-cycle press
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  float amBlend;
  uint8_t mode;
};
constexpr uint32_t SETTINGS_MAGIC = 0x524E4731;  // "RNG1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
sc::RingModCore ringmod;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-bitcrusher). */
volatile float   g_carrierHz  = 100.0f;
volatile float   g_trackRatio = 1.0f;
volatile float   g_shape      = 0.0f;
volatile float   g_amBlend    = 0.0f;
volatile uint8_t g_mode       = sc::RINGMOD_FIXED;
volatile float   g_wet        = 1.0f;
volatile int16_t g_ledForce   = -1;  // >=0: loop-driven blink level, -1: carrier

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

  /* --- IN1: hard-sync / S&H trigger (sample-accurate edge detect) ----- */
  const bool in1 = digitalRead(mod2::IN1_PIN) == HIGH;
  if (in1 && !isrLastIn1)
    ringmod.trigger();
  isrLastIn1 = in1;

  /* --- ring modulate --------------------------------------------------- */
  ringmod.carrierHz  = g_carrierHz;
  ringmod.trackRatio = g_trackRatio;
  ringmod.shape      = g_shape;
  ringmod.amBlend    = g_amBlend;
  ringmod.mode       = g_mode;
  ringmod.wet        = g_wet;
  ringmod.octaveDrop = digitalRead(mod2::IN2_PIN) == HIGH;
  float out = ringmod.process(in, 1.0f / mod2::AUDIO_FS);

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: blinks at the carrier, unless loop() blinks a mode ID ----- */
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(ringmod.ledLevel() * mod2::PWM_FS));

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
  pinMode(A0, INPUT);                       // freq/ratio / wet-dry (shifted)
  pinMode(A1, INPUT);                       // shape / AM blend (shifted)
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // hard-sync / S&H trigger
  pinMode(mod2::IN2_PIN, INPUT);            // octave-drop gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // mode / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_amBlend = sc::clampf(s.amBlend, 0.0f, 1.0f);
    g_mode = s.mode < sc::RINGMOD_MODE_COUNT ? s.mode : sc::RINGMOD_FIXED;
  }

  ringmod.reset();

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (mode / dual shift layer), pitch tracking,
 *  flash saves
 * ==================================================================== */
void loop()
{
  /* --- Track-mode pitch search: heavy (O(lags x window)), so it runs
         here at control rate, never inside the audio ISR. -------------- */
  ringmod.analyzePitch(mod2::AUDIO_FS);

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

  /* --- button: short = mode, hold + POT1/POT2 = shift ----------------- */
  static bool lastPressed = false;
  static bool adjustingWet = false;    // shift layer: POT1 -> wet/dry
  static bool adjustingBlend = false;  // shift layer: POT2 -> AM/ring blend
  static uint32_t pressStartMs = 0;
  static float pot1AtPress = 0.0f, pot2AtPress = 0.0f;
  static mod2::PickupParam freqPickup, shapePickup;
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
    adjustingBlend = false;
  }
  if (pressed && !adjustingWet && fabsf(pot1 - pot1AtPress) > POT_MOVE_THRESHOLD)
    adjustingWet = true;  // shift layer engaged; short-press action suppressed
  if (pressed && !adjustingBlend && fabsf(pot2 - pot2AtPress) > POT_MOVE_THRESHOLD)
    adjustingBlend = true;
  if (pressed && adjustingWet) {
    g_wet = pot1;
    dirty = true;
    lastChangeMs = now;
  }
  if (pressed && adjustingBlend) {
    g_amBlend = pot2;
    dirty = true;
    lastChangeMs = now;
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustingWet) {
      /* POT1 was borrowed for wet/dry: freeze the freq until the pot is
         turned back across where it sat before the press.               */
      freqPickup.targetValue = pot1AtPress;
      freqPickup.lastPotValue = pot1;
      freqPickup.pickupActive = true;
    }
    if (adjustingBlend) {
      shapePickup.targetValue = pot2AtPress;
      shapePickup.lastPotValue = pot2;
      shapePickup.pickupActive = true;
    }
    if (!adjustingWet && !adjustingBlend &&
        now - pressStartMs < SHORT_PRESS_MS) {
      g_mode = (g_mode + 1) % sc::RINGMOD_MODE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_mode + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT1 -> carrier freq (Fixed/S&H) + Track ratio, POT2 -> shape -- */
  if (!pressed) {
    if (mod2::checkPickup(freqPickup, pot1)) {
      g_carrierHz = sc::ringModCarrierHz(pot1);
      g_trackRatio = sc::ringModTrackRatio(pot1);
    }
    freqPickup.lastPotValue = pot1;
    if (mod2::checkPickup(shapePickup, pot2))
      g_shape = pot2;
    shapePickup.lastPotValue = pot2;
  }

  /* --- LED blink code for the newly selected mode ---------------------- */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to blinking with the carrier
    }
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_wet, g_amBlend, g_mode};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
