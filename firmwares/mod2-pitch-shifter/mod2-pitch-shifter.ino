/* PITCH SHIFTER

Description:
Real-time pitch shift of the input — instant sub/sparkle or free harmony.
Audio comes in on the CV jack (sampled by the RP2350 ADC inside the ~36.6 kHz
PWM ISR, per firmwares/mod2-fx/README.md). The DSP is the classic two-tap
granular / delay-line "harmonizer": input runs through a short ring buffer and
two read taps sweep it 180 deg apart, equal-power crossfaded so the grain wraps
never click. POT1 sets the pitch (+/-12 st, semitone-detented), POT2 the grain
size (10 - 100 ms: short = tight for drums, long = smooth for pads). Short
button presses cycle the mode: Octave-up / Octave-down / Free (POT1 continuous)
/ Detune (dual-voice +/- cents). Hold BUTTON + turn POT1 for wet/dry mix, hold
BUTTON + turn POT2 for shifted-signal feedback (shimmer cascades) — both
persisted to flash with the mode. IN2 latches an octave-down while held. The
LED pulses faster the further from unison.
DSP lives in the shared sc::PitchShifterCore (also used by the VCV Rack port).

Key Variables:
  A0 -> Pitch (+/-12 st, semitone-detented; BTN held: wet/dry mix)
  A1 -> Grain size (10 - 100 ms; BTN held: feedback)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║  PITCH    ║
      ║  SHIFT    ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - pitch +/-12 st (BTN held: wet/dry mix)
      ║   PITCH   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - grain size 10-100 ms (BTN held: feedback)
      ║   GRAIN   ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - pulses w/ shift size (blinks mode ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: mode; hold+POT1/2: mix/feedback
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - (spare)
      ║ (o)   (o) ║   IN2 (GPIO0) - octave-down latch (HIGH = -1 oct)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial pitch-shifter firmware (maddie synths original, shared PitchShifterCore)

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
#include <EEPROM.h>            // persisted wet/dry + feedback + mode
#include <Mod2Common.h>        // Shared MOD2 pin map, PWM-audio setup and helpers
#include <PitchShifterCore.h>  // Shared pitch-shift DSP (also used by rack-plugins/src/mod2-pitch-shifter.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // max length of a mode-cycle press
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  float feedback;
  uint8_t mode;
};
constexpr uint32_t SETTINGS_MAGIC = 0x50534831;  // "PSH1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
// ~110 ms int16 arena in SRAM — holds the 100 ms max grain plus headroom.
static int16_t pitchArena[(uint32_t)(sc::kPitchBufferSec * mod2::AUDIO_FS) + 16];
sc::PitchShifterCore pitch;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-chorus). */
volatile float   g_semitones = 0.0f;
volatile float   g_grainSec  = 0.050f;
volatile uint8_t g_mode      = sc::PITCH_FREE;
volatile float   g_wet       = 1.0f;
volatile float   g_feedback  = 0.0f;
volatile int16_t g_ledForce  = -1;  // >=0: loop-driven blink level, -1: pulse

/* =======================================================================
 *  PWM interrupt service routine (~36.6 kHz)
 * ==================================================================== */
void __isr onPwmWrap()
{
  /* --- audio input: sample the CV jack at the audio rate ------------- */
  const int raw = analogRead(mod2::CV_PIN);
  const float in = dcBlock.process(raw * (2.0f / 1023.0f) - 1.0f);

  /* --- IN2: latch an octave-down while the gate is HIGH -------------- */
  const bool octDownLatch = digitalRead(mod2::IN2_PIN) == HIGH;
  pitch.mode      = octDownLatch ? (uint8_t)sc::PITCH_OCT_DOWN : g_mode;
  pitch.semitones = g_semitones;
  pitch.grainSec  = g_grainSec;
  pitch.wet       = g_wet;
  pitch.feedback  = g_feedback;
  float out = pitch.process(in, 1.0f / mod2::AUDIO_FS);

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: pulses with the shift size, unless loop() blinks a mode ID */
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(pitch.ledLevel() * mod2::PWM_FS));

  pwm_clear_irq(sliceIRQ);
}

/* =======================================================================
 *  Guarded pot read — the ISR reads A2 through the same ADC mux, so block
 *  the IRQ for the ~2 us conversion (delays one audio sample slightly).
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
  pinMode(A1, INPUT);                       // grain / feedback (shifted)
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // (spare)
  pinMode(mod2::IN2_PIN, INPUT);            // octave-down latch gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // mode / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_feedback = sc::clampf(s.feedback, 0.0f, 1.0f);
    g_mode = s.mode < sc::PITCH_MODE_COUNT ? s.mode : sc::PITCH_FREE;
  }

  pitch.init(pitchArena, sizeof(pitchArena) / sizeof(pitchArena[0]));

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (mode / shift layers), flash saves
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

  /* --- button: short = mode; hold + POT1 = wet/dry; hold + POT2 = fb -- */
  static bool lastPressed = false;
  static bool adjustingWet = false;
  static bool adjustingFb = false;
  static uint32_t pressStartMs = 0;
  static float pot1AtPress = 0.0f;
  static float pot2AtPress = 0.0f;
  static mod2::PickupParam pitchPickup;
  static mod2::PickupParam grainPickup;
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
    adjustingFb = false;
  }
  if (pressed && !adjustingWet && fabsf(pot1 - pot1AtPress) > POT_MOVE_THRESHOLD)
    adjustingWet = true;  // shift layer engaged; short-press action suppressed
  if (pressed && !adjustingFb && fabsf(pot2 - pot2AtPress) > POT_MOVE_THRESHOLD)
    adjustingFb = true;
  if (pressed && adjustingWet) {
    g_wet = pot1;
    dirty = true;
    lastChangeMs = now;
  }
  if (pressed && adjustingFb) {
    g_feedback = pot2;
    dirty = true;
    lastChangeMs = now;
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustingWet || adjustingFb) {
      /* POT1/POT2 were borrowed for the shift layer: freeze each until the
         pot is turned back across where it sat before the press.          */
      if (adjustingWet) {
        pitchPickup.targetValue = pot1AtPress;
        pitchPickup.lastPotValue = pot1;
        pitchPickup.pickupActive = true;
      }
      if (adjustingFb) {
        grainPickup.targetValue = pot2AtPress;
        grainPickup.lastPotValue = pot2;
        grainPickup.pickupActive = true;
      }
    } else if (now - pressStartMs < SHORT_PRESS_MS) {
      g_mode = (g_mode + 1) % sc::PITCH_MODE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_mode + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT1 -> pitch, POT2 -> grain (unless shifted / waiting pickup) - */
  if (!pressed) {
    if (mod2::checkPickup(pitchPickup, pot1))
      g_semitones = sc::pitchShifterSemitones(pot1, true);  // detented
    pitchPickup.lastPotValue = pot1;

    if (mod2::checkPickup(grainPickup, pot2))
      g_grainSec = sc::pitchShifterGrainSec(pot2);
    grainPickup.lastPotValue = pot2;
  }

  /* --- LED blink code for the newly selected mode --------------------- */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to pulsing with the shift size
    }
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_wet, g_feedback, g_mode};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
