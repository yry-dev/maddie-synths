/* FX — multi-algorithm effects platform

Description:
One firmware, many effects: the MOD2 answer to an FX Aid / Pico DSP. A short
button press cycles the algorithm and the LED blinks its ID (index+1 flashes);
every algorithm uses the identical control mapping so each effect is immediately
familiar. Audio comes in on the CV jack (sampled by the RP2350 ADC inside the
~36.6 kHz PWM ISR, per firmwares/mod2-fx/README.md). POT1 is the main parameter,
POT2 the character; hold BUTTON + turn POT1 for wet/dry mix (persisted to flash
with the algorithm). IN1 is a clock / tap / trigger (per algorithm), IN2 a gate
action (freeze / hold / bypass / damp, per algorithm), and a long button press
fires the per-algorithm action (cycles that effect's sub-mode). Only ONE
algorithm runs at a time; all of them share a single ~2.5 s int16 arena, and
switching mutes for ~10 ms while the incoming effect is re-initialised so it
never clicks. All 16 planned algorithms are hosted (delay, distortion,
bitcrusher, chorus, resonator, tape echo, flanger, phaser, ring mod, wavefolder,
reverb, freeze, glitch delay, karplus, comb, pitch shift).
DSP lives in the shared sc::FxPlatformCore (also used by the VCV Rack port).

Key Variables:
  A0 -> Main parameter (BTN held: wet/dry mix)
  A1 -> Character
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║    FX     ║
      ║  platform ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - main parameter (BTN held: wet/dry mix)
      ║   MAIN    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - character
      ║   CHAR    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - blinks algorithm ID; activity while running
      ║   (BTN)   ║   BTN (GPIO6) - short: next algorithm; long: algo action;
      ║           ║                 hold+POT1: wet/dry
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - clock / tap / trigger (per algorithm)
      ║ (o)   (o) ║   IN2 (GPIO0) - gate action (per algorithm)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial FX platform firmware (maddie synths original, shared
        FxPlatformCore hosting all 16 sc effect cores)

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
#include <EEPROM.h>          // persisted algorithm + wet/dry + per-algo sub-modes
#include <Mod2Common.h>      // Shared MOD2 pin map, PWM-audio setup and helpers
#include <FxPlatformCore.h>  // Shared FX platform (also used by rack-plugins/src/mod2-fx.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 350;     // <= this = "next algorithm"
constexpr uint32_t LONG_PRESS_MS   = 800;     // >= this = per-algorithm action
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save

/* ------------------------- persisted settings -------------------------
 * The README asks for 8-16 preset slots. We persist the current algorithm,
 * the wet/dry mix, and each algorithm's sub-mode (so every effect remembers
 * its own flavour) — a compact whole-panel preset rather than N discrete
 * slots. Everything else (POT1/POT2) is a live knob and reads from the panel.
 */
struct Settings {
  uint32_t magic;
  uint8_t  algorithm;
  float    wet;
  uint8_t  modeOf[sc::FX_ALGO_COUNT];
};
constexpr uint32_t SETTINGS_MAGIC = 0x46583031;  // "FX01"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
// One ~2.5 s int16 arena, reused by whichever algorithm runs (see
// FxPlatformCore's memory note). Sized from the audio rate at compile time.
static int16_t fxArena[(uint32_t)(sc::kFxArenaSec * mod2::AUDIO_FS) + 64];
sc::FxPlatformCore fx;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-chorus). */
volatile float   g_main     = 0.5f;   // POT1 main parameter
volatile float   g_char     = 0.5f;   // POT2 character
volatile float   g_wet      = 0.5f;   // wet/dry mix
volatile int16_t g_ledForce = -1;     // >=0: loop-driven blink level, -1: activity

/* Cross-thread event flags (loop -> ISR). A single-byte write is atomic on the
 * RP2350; the ISR consumes these each sample. */
volatile bool    g_algoReq   = false;
volatile uint8_t g_algoReqTo = 0;
volatile bool    g_actionReq = false;

/* =======================================================================
 *  PWM interrupt service routine (~36.6 kHz)
 *  All DSP + core-state mutation happens here so loop() only ever writes the
 *  volatile shadows above; the algorithm swap (arena clear) is performed by
 *  fx.process() while it is muted, so the long clear never clicks.
 * ==================================================================== */
void __isr onPwmWrap()
{
  /* --- audio input: sample the CV jack at the audio rate ------------- */
  const int raw = analogRead(mod2::CV_PIN);
  const float in = dcBlock.process(raw * (2.0f / 1023.0f) - 1.0f);

  /* --- consume loop() requests --------------------------------------- */
  if (g_algoReq)   { g_algoReq = false;   fx.requestAlgorithm(g_algoReqTo); }
  if (g_actionReq) { g_actionReq = false; fx.action(); }

  /* --- IN1 rising edge: clock / tap / trigger (per algorithm) --------- */
  static bool lastIn1 = false;
  const bool in1 = digitalRead(mod2::IN1_PIN) == HIGH;
  if (in1 && !lastIn1) fx.onClock();
  lastIn1 = in1;

  /* --- IN2 level: gate action (per algorithm) ------------------------- */
  fx.setGate(digitalRead(mod2::IN2_PIN) == HIGH);

  /* --- controls + run the active algorithm --------------------------- */
  fx.setControls(g_main, g_char, g_wet);
  float out = fx.process(in, 1.0f / mod2::AUDIO_FS);

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: activity follower, unless loop() is blinking an algo ID --- */
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(fx.ledLevel() * mod2::PWM_FS));

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
  pinMode(A0, INPUT);                       // main / wet-dry (shifted)
  pinMode(A1, INPUT);                       // character
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // clock / tap / trigger
  pinMode(mod2::IN2_PIN, INPUT);            // gate action
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // algorithm / action / shift button
  analogReadResolution(10);

  fx.init(fxArena, sizeof(fxArena) / sizeof(fxArena[0]));

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(256);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    uint8_t a = s.algorithm < sc::FX_ALGO_COUNT ? s.algorithm : 0;
    for (uint8_t i = 0; i < sc::FX_ALGO_COUNT; i++) {
      uint8_t cnt = sc::fxModeCount(i);
      fx.modeOf[i] = (cnt && s.modeOf[i] < cnt) ? s.modeOf[i] : 0;
    }
    // Snap directly to the saved algorithm (no fade at boot).
    fx.algo = fx.pendingAlgo = a;
    fx.initActive();
  }

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (algorithm / action / wet-dry shift), flash saves
 * ==================================================================== */
void loop()
{
  /* --- control-rate hook (ring-mod pitch tracker; no-op otherwise) ---- */
  fx.analyze(mod2::AUDIO_FS);

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

  g_char = pot2;

  /* --- button: short = next algorithm, long = action, hold+POT1 = wet -- */
  static bool lastPressed = false;
  static bool adjustingWet = false;
  static bool actionFired = false;
  static uint32_t pressStartMs = 0;
  static float potAtPress = 0.0f;
  static mod2::PickupParam mainPickup;
  static bool dirty = false;
  static uint32_t lastChangeMs = 0;

  const bool pressed = digitalRead(mod2::BUTTON_PIN) == LOW;
  const uint32_t now = millis();

  /* blink state (algorithm-ID feedback: algo+1 flashes) */
  static uint8_t blinkPhasesLeft = 0;
  static uint32_t blinkNextMs = 0;

  if (pressed && !lastPressed) {  // press edge
    pressStartMs = now;
    potAtPress = pot1;
    adjustingWet = false;
    actionFired = false;
  }
  if (pressed && !adjustingWet && fabsf(pot1 - potAtPress) > POT_MOVE_THRESHOLD)
    adjustingWet = true;  // shift layer engaged; press-actions suppressed
  if (pressed && adjustingWet) {
    g_wet = pot1;
    dirty = true;
    lastChangeMs = now;
  }
  /* Long press (still held, not shifting) fires the per-algorithm action once. */
  if (pressed && !adjustingWet && !actionFired &&
      now - pressStartMs >= LONG_PRESS_MS) {
    g_actionReq = true;   // ISR calls fx.action()
    actionFired = true;
    dirty = true;
    lastChangeMs = now;
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustingWet) {
      /* POT1 was borrowed for wet/dry: freeze the main param until the pot is
         turned back across where it sat before the press.                */
      mainPickup.targetValue = potAtPress;
      mainPickup.lastPotValue = pot1;
      mainPickup.pickupActive = true;
    } else if (!actionFired && now - pressStartMs < SHORT_PRESS_MS) {
      /* Short press = next algorithm. */
      uint8_t next = (uint8_t)((fx.algorithm() + 1) % sc::FX_ALGO_COUNT);
      g_algoReqTo = next;
      g_algoReq = true;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((next + 1) * 2);  // (index+1) on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT1 -> main parameter (unless shifted / waiting for pickup) ---- */
  if (!pressed) {
    if (mod2::checkPickup(mainPickup, pot1))
      g_main = pot1;
    mainPickup.lastPotValue = pot1;
  }

  /* --- LED blink code for the newly selected algorithm ---------------- */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 140;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to the activity follower
    }
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s;
    s.magic = SETTINGS_MAGIC;
    s.algorithm = fx.algorithm();
    s.wet = g_wet;
    for (uint8_t i = 0; i < sc::FX_ALGO_COUNT; i++) s.modeOf[i] = fx.modeOf[i];
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
