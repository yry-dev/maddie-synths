/* DISTORTION

Description:
Multi-algorithm drive — five flavours behind one button. Audio comes in on the
CV jack (sampled by the RP2350 ADC inside the ~36.6 kHz PWM ISR, per
firmwares/mod2-fx/README.md). POT1 sets the drive (pre-shaper gain with
auto-compensated output level), POT2 a post-shaper tilt tone (dark <-> bright).
Short button presses cycle the algorithm: Soft (tanh) / Hard clip / Tube
(asymmetric, even harmonics) / Foldback / Fuzz (gated, dying-battery sputter).
The shaper runs 2x oversampled inside the core so hard clip and foldback don't
alias badly at 36.6 kHz. Hold BUTTON + turn POT1 for wet/dry mix — parallel
drive! — persisted to flash with the algorithm. IN2 is a bypass gate.
DSP lives in the shared sc::DistortionCore (also used by the VCV Rack port).

Key Variables:
  A0 -> Drive amount (1x - ~50x pre-gain, exponential taper)
  A1 -> Tone (post-shaper tilt: dark <-> bright)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║  DISTORT  ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - drive amount (BTN held: wet/dry mix)
      ║   DRIVE   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - tone (dark <-> bright tilt)
      ║   TONE    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - output level (blinks algorithm ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: algorithm; hold+POT1: wet/dry
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
  - 1.0 Initial distortion firmware (maddie synths original, shared DistortionCore)

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
#include <EEPROM.h>          // persisted wet/dry + algorithm
#include <Mod2Common.h>      // Shared MOD2 pin map, PWM-audio setup and helpers
#include <DistortionCore.h>  // Shared drive DSP (also used by rack-plugins/src/mod2-distortion.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // max length of an algorithm-cycle press
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  uint8_t mode;
};
constexpr uint32_t SETTINGS_MAGIC = 0x44535431;  // "DST1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
sc::DistortionCore drive;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-bitcrusher). */
volatile float   g_drive    = 1.0f;
volatile float   g_tone     = 0.5f;
volatile uint8_t g_mode     = sc::DISTORTION_SOFT;
volatile float   g_wet      = 1.0f;
volatile int16_t g_ledForce = -1;  // >=0: loop-driven blink level, -1: follow audio

/* ISR-only state. */
static float ledFollow = 0.0f;

/* =======================================================================
 *  PWM interrupt service routine (~36.6 kHz)
 * ==================================================================== */
void __isr onPwmWrap()
{
  /* --- audio input: sample the CV jack at the audio rate ------------- */
  const int raw = analogRead(mod2::CV_PIN);
  const float in = dcBlock.process(raw * (2.0f / 1023.0f) - 1.0f);

  /* --- drive ---------------------------------------------------------- */
  drive.drive = g_drive;
  drive.tone  = g_tone;
  drive.mode  = g_mode;
  drive.wet   = g_wet;
  float out = drive.process(in, 1.0f / mod2::AUDIO_FS);

  /* --- IN2: bypass gate (HIGH = pass the dry input) ------------------- */
  if (digitalRead(mod2::IN2_PIN) == HIGH)
    out = in;

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: smoothed |output|, unless loop() is blinking an algo ID --- */
  ledFollow += (fabsf(out) - ledFollow) * 0.002f;
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(ledFollow * mod2::PWM_FS));

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
  pinMode(A0, INPUT);                       // drive / wet-dry (shifted)
  pinMode(A1, INPUT);                       // tone
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // (spare)
  pinMode(mod2::IN2_PIN, INPUT);            // bypass gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // algorithm / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_mode = s.mode < sc::DISTORTION_MODE_COUNT ? s.mode : sc::DISTORTION_SOFT;
  }

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (algorithm / wet-dry shift layer), flash saves
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

  g_tone = pot2;

  /* --- button: short press = algorithm, hold + POT1 = wet/dry --------- */
  static bool lastPressed = false;
  static bool adjustingWet = false;
  static uint32_t pressStartMs = 0;
  static float potAtPress = 0.0f;
  static mod2::PickupParam drivePickup;
  static bool dirty = false;
  static uint32_t lastChangeMs = 0;

  const bool pressed = digitalRead(mod2::BUTTON_PIN) == LOW;
  const uint32_t now = millis();

  /* blink state (algorithm-ID feedback: mode+1 flashes) */
  static uint8_t blinkPhasesLeft = 0;
  static uint32_t blinkNextMs = 0;

  if (pressed && !lastPressed) {  // press edge
    pressStartMs = now;
    potAtPress = pot1;
    adjustingWet = false;
  }
  if (pressed && !adjustingWet && fabsf(pot1 - potAtPress) > POT_MOVE_THRESHOLD)
    adjustingWet = true;  // shift layer engaged; short-press action suppressed
  if (pressed && adjustingWet) {
    g_wet = pot1;
    dirty = true;
    lastChangeMs = now;
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustingWet) {
      /* POT1 was borrowed for wet/dry: freeze the drive until the pot is
         turned back across where it sat before the press.               */
      drivePickup.targetValue = potAtPress;
      drivePickup.lastPotValue = pot1;
      drivePickup.pickupActive = true;
    } else if (now - pressStartMs < SHORT_PRESS_MS) {
      g_mode = (g_mode + 1) % sc::DISTORTION_MODE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_mode + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT1 -> drive (unless shifted / waiting for pickup) ------------ */
  if (!pressed) {
    if (mod2::checkPickup(drivePickup, pot1))
      g_drive = sc::distortionDriveGain(pot1);
    drivePickup.lastPotValue = pot1;
  }

  /* --- LED blink code for the newly selected algorithm ---------------- */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to following the audio
    }
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_wet, g_mode};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
