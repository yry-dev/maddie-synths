/* CHORUS

Description:
Juno-style chorus + string-machine ensemble. Audio comes in on the CV jack
(sampled by the RP2350 ADC inside the ~36.6 kHz PWM ISR, per
firmwares/mod2-fx/README.md). Classic modulated short-delay thickening,
mono-in mono-out (the Juno's stereo spread is faked by summing two
anti-phase-modulated voices — still lush in mono). POT1 sets the LFO rate
(0.1 - 8 Hz), POT2 the modulation depth. Short button presses cycle the mode:
Chorus I (slow, subtle) / Chorus II (faster, deeper) / Ensemble (3 taps on 3
phase-offset slow+fast LFOs, the classic string-machine recipe). Hold BUTTON +
turn POT1 for wet/dry mix, persisted to flash with the mode. IN2 is a bypass
gate. The LED breathes at the LFO rate.
DSP lives in the shared sc::ChorusCore (also used by the VCV Rack port).

Key Variables:
  A0 -> LFO rate (0.1 - 8 Hz, exponential taper)
  A1 -> Depth (modulation excursion)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║  CHORUS   ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - LFO rate (BTN held: wet/dry mix)
      ║   RATE    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - depth (modulation excursion)
      ║   DEPTH   ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - breathes at the LFO rate (blinks mode ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: mode; hold+POT1: wet/dry
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
  - 1.0 Initial chorus firmware (maddie synths original, shared ChorusCore)

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
#include <EEPROM.h>      // persisted wet/dry + mode
#include <Mod2Common.h>  // Shared MOD2 pin map, PWM-audio setup and helpers
#include <ChorusCore.h>  // Shared chorus DSP (also used by rack-plugins/src/mod2-chorus.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // max length of a mode-cycle press
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  uint8_t mode;
};
constexpr uint32_t SETTINGS_MAGIC = 0x43485231;  // "CHR1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
// ~20 ms int16 arena in SRAM — the taps never reach past kChorusBufferSec.
static int16_t chorusArena[(uint32_t)(sc::kChorusBufferSec * mod2::AUDIO_FS) + 16];
sc::ChorusCore chorus;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-bitcrusher). */
volatile float   g_rateHz   = 0.5f;
volatile float   g_depth    = 0.5f;
volatile uint8_t g_mode     = sc::CHORUS_I;
volatile float   g_wet      = 0.5f;
volatile int16_t g_ledForce = -1;  // >=0: loop-driven blink level, -1: breathe

/* =======================================================================
 *  PWM interrupt service routine (~36.6 kHz)
 * ==================================================================== */
void __isr onPwmWrap()
{
  /* --- audio input: sample the CV jack at the audio rate ------------- */
  const int raw = analogRead(mod2::CV_PIN);
  const float in = dcBlock.process(raw * (2.0f / 1023.0f) - 1.0f);

  /* --- chorus --------------------------------------------------------- */
  chorus.rateHz = g_rateHz;
  chorus.depth  = g_depth;
  chorus.mode   = g_mode;
  chorus.wet    = g_wet;
  float out = chorus.process(in, 1.0f / mod2::AUDIO_FS);

  /* --- IN2: bypass gate (HIGH = pass the dry input) ------------------- */
  if (digitalRead(mod2::IN2_PIN) == HIGH)
    out = in;

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: breathes with the LFO, unless loop() is blinking a mode ID */
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(chorus.ledLevel() * mod2::PWM_FS));

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
  pinMode(A0, INPUT);                       // rate / wet-dry (shifted)
  pinMode(A1, INPUT);                       // depth
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // (spare)
  pinMode(mod2::IN2_PIN, INPUT);            // bypass gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // mode / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_mode = s.mode < sc::CHORUS_MODE_COUNT ? s.mode : sc::CHORUS_I;
  }

  chorus.init(chorusArena, sizeof(chorusArena) / sizeof(chorusArena[0]));

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (mode / wet-dry shift layer), flash saves
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

  g_depth = pot2;

  /* --- button: short press = mode, hold + POT1 = wet/dry -------------- */
  static bool lastPressed = false;
  static bool adjustingWet = false;
  static uint32_t pressStartMs = 0;
  static float potAtPress = 0.0f;
  static mod2::PickupParam ratePickup;
  static bool dirty = false;
  static uint32_t lastChangeMs = 0;

  const bool pressed = digitalRead(mod2::BUTTON_PIN) == LOW;
  const uint32_t now = millis();

  /* blink state (mode-ID feedback: mode+1 flashes) */
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
      /* POT1 was borrowed for wet/dry: freeze the rate until the pot is
         turned back across where it sat before the press.               */
      ratePickup.targetValue = potAtPress;
      ratePickup.lastPotValue = pot1;
      ratePickup.pickupActive = true;
    } else if (now - pressStartMs < SHORT_PRESS_MS) {
      g_mode = (g_mode + 1) % sc::CHORUS_MODE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_mode + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT1 -> LFO rate (unless shifted / waiting for pickup) --------- */
  if (!pressed) {
    if (mod2::checkPickup(ratePickup, pot1))
      g_rateHz = sc::chorusRateHz(pot1);
    ratePickup.lastPotValue = pot1;
  }

  /* --- LED blink code for the newly selected mode --------------------- */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to breathing with the LFO
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
