/* WAVEFOLDER

Description:
Digital West-Coast (Serge / Buchla-style) wavefolder. Audio comes in on the CV
jack (sampled by the RP2350 ADC inside the ~36.6 kHz PWM ISR, per
firmwares/mod2-fx/README.md). The signal is driven hard into a bounded,
oscillatory transfer function so it "folds" off the rails several times, turning
a plain sine or triangle into a bright, harmonically dense timbre that sweeps as
the fold amount rises. POT1 sets the fold amount (input gain, 1x - 20x), POT2 a
symmetry/offset (pre-fold DC bias -> even harmonics, a timbral tilt). Short
button presses cycle the fold curve: Reflect (hard triangle) / Sine (Buchla
259-ish) / Cascade (4-stage Serge-ish). Hold BUTTON + turn POT1 for wet/dry mix,
hold BUTTON + turn POT2 for a post-fold low-pass; both persist to flash with the
curve. IN2 is a bypass gate. The LED brightness follows fold density. The folder
runs 4x oversampled for anti-aliasing.
DSP lives in the shared sc::WavefolderCore (also used by the VCV Rack port).

Key Variables:
  A0 -> Fold amount (input gain 1x - 20x; BTN held: wet/dry mix)
  A1 -> Symmetry / offset (pre-fold DC bias; BTN held: post-fold low-pass)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║ WAVEFOLD  ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - fold amount (BTN held: wet/dry mix)
      ║   FOLD    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - symmetry (BTN held: post-fold low-pass)
      ║   SYMM    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - follows fold density (blinks curve ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: curve; hold+POT1/POT2: mix/tone
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
  - 1.0 Initial wavefolder firmware (maddie synths original, shared WavefolderCore)

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
#include <EEPROM.h>          // persisted wet/dry + tone + curve
#include <Mod2Common.h>      // Shared MOD2 pin map, PWM-audio setup and helpers
#include <WavefolderCore.h>  // Shared folder DSP (also used by rack-plugins/src/mod2-wavefolder.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // max length of a curve-cycle press
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  float tone;   // 0..1 post-fold low-pass position
  uint8_t mode;
};
constexpr uint32_t SETTINGS_MAGIC = 0x57464431;  // "WFD1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
sc::WavefolderCore folder;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-chorus). */
volatile float   g_foldGain = 1.0f;
volatile float   g_offset   = 0.0f;
volatile float   g_toneHz   = 18000.0f;
volatile float   g_wet      = 1.0f;
volatile uint8_t g_mode     = sc::WAVEFOLDER_REFLECT;
volatile int16_t g_ledForce = -1;  // >=0: loop-driven blink level, -1: follow density

/* =======================================================================
 *  PWM interrupt service routine (~36.6 kHz)
 * ==================================================================== */
void __isr onPwmWrap()
{
  /* --- audio input: sample the CV jack at the audio rate ------------- */
  const int raw = analogRead(mod2::CV_PIN);
  const float in = dcBlock.process(raw * (2.0f / 1023.0f) - 1.0f);

  /* --- wavefolder ----------------------------------------------------- */
  folder.foldGain = g_foldGain;
  folder.offset   = g_offset;
  folder.toneHz   = g_toneHz;
  folder.wet      = g_wet;
  folder.setMode(g_mode);  // starts a click-free crossfade on a real change
  float out = folder.process(in, 1.0f / mod2::AUDIO_FS);

  /* --- IN2: bypass gate (HIGH = pass the dry input) ------------------- */
  if (digitalRead(mod2::IN2_PIN) == HIGH)
    out = in;

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: follows fold density, unless loop() is blinking a curve ID */
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(folder.ledLevel() * mod2::PWM_FS));

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
  pinMode(A0, INPUT);                       // fold amount / wet-dry (shifted)
  pinMode(A1, INPUT);                       // symmetry / tone (shifted)
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // (spare)
  pinMode(mod2::IN2_PIN, INPUT);            // bypass gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // curve / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_toneHz = sc::wavefolderToneHz(sc::clampf(s.tone, 0.0f, 1.0f));
    g_mode = s.mode < sc::WAVEFOLDER_MODE_COUNT ? s.mode : sc::WAVEFOLDER_REFLECT;
  }

  folder.mode = g_mode;
  folder.reset();

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (curve / wet-dry + tone shift layer), flash saves
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

  /* --- button: short press = curve, hold + POT1/POT2 = wet/dry + tone - */
  static bool lastPressed = false;
  static bool adjustingShift = false;
  static uint32_t pressStartMs = 0;
  static float pot1AtPress = 0.0f, pot2AtPress = 0.0f;
  static float toneNorm = 1.0f;  // POT2 shift value (0..1), wide-open default
  static mod2::PickupParam foldPickup;  // POT1 -> fold amount pickup
  static mod2::PickupParam symPickup;   // POT2 -> symmetry pickup
  static bool dirty = false;
  static uint32_t lastChangeMs = 0;

  const bool pressed = digitalRead(mod2::BUTTON_PIN) == LOW;
  const uint32_t now = millis();

  /* blink state (curve-ID feedback: mode+1 flashes) */
  static uint8_t blinkPhasesLeft = 0;
  static uint32_t blinkNextMs = 0;

  if (pressed && !lastPressed) {  // press edge
    pressStartMs = now;
    pot1AtPress = pot1;
    pot2AtPress = pot2;
    adjustingShift = false;
  }
  if (pressed && !adjustingShift &&
      (fabsf(pot1 - pot1AtPress) > POT_MOVE_THRESHOLD ||
       fabsf(pot2 - pot2AtPress) > POT_MOVE_THRESHOLD))
    adjustingShift = true;  // shift layer engaged; short-press action suppressed
  if (pressed && adjustingShift) {
    g_wet = pot1;                             // POT1 -> wet/dry
    toneNorm = pot2;                          // POT2 -> post-fold low-pass
    g_toneHz = sc::wavefolderToneHz(toneNorm);
    dirty = true;
    lastChangeMs = now;
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustingShift) {
      /* POT1/POT2 were borrowed for the shift layer: freeze fold amount and
         symmetry until each pot is turned back across where it sat before. */
      foldPickup.targetValue = pot1AtPress;
      foldPickup.lastPotValue = pot1;
      foldPickup.pickupActive = true;
      symPickup.targetValue = pot2AtPress;
      symPickup.lastPotValue = pot2;
      symPickup.pickupActive = true;
    } else if (now - pressStartMs < SHORT_PRESS_MS) {
      g_mode = (g_mode + 1) % sc::WAVEFOLDER_MODE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_mode + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT1 -> fold amount, POT2 -> symmetry (unless shifted / picking up) */
  if (!pressed) {
    if (mod2::checkPickup(foldPickup, pot1))
      g_foldGain = sc::wavefolderFoldGain(pot1);
    foldPickup.lastPotValue = pot1;
    if (mod2::checkPickup(symPickup, pot2))
      g_offset = sc::wavefolderOffset(pot2);
    symPickup.lastPotValue = pot2;
  }

  /* --- LED blink code for the newly selected curve -------------------- */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to following fold density
    }
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_wet, toneNorm, g_mode};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
