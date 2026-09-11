/* FREEZE

Description:
Buffer capture & seamless loop — the simplest "deep" MOD2 FX. Audio comes in on
the CV jack (sampled by the RP2350 ADC inside the ~36.6 kHz PWM ISR, per
firmwares/mod2-fx/README.md) into an always-recording ~400 KB SRAM ring (~5.5 s).
FREEZE stops the write head and loops a window out of the frozen buffer, so a
held chord or drone survives even after the input goes silent. POT1 sets the
loop length (5 ms - full buffer; short = pitched buzz-drone), POT2 scans the
loop window through the captured buffer (0 = freshest slice, 1 = oldest). Short
button presses cycle the playback mode: Forward / Ping-pong / Half-speed (a free
octave-down pad via a 0.5x read rate). A long press latches FREEZE by hand; IN1
is the freeze gate (freeze while high), IN2 re-captures fresh audio without
leaving freeze. An equal-power crossfade at the loop seam keeps the wrap
click-free, and freeze enter/exit crossfades live<->frozen over ~10 ms.
Hold BUTTON + turn POT1 for the frozen/live mix, BUTTON + POT2 for the seam
crossfade length (both persisted to flash with the mode).
DSP lives in the shared sc::FreezeCore (also used by the VCV Rack port).

Key Variables:
  A0 -> Loop length (5 ms - 5.5 s, exponential taper)
  A1 -> Loop window position (0 = newest, 1 = oldest)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║  FREEZE   ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - loop length (BTN held: frozen/live mix)
      ║   LENGTH  ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - window position (BTN held: seam crossfade)
      ║    POS    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - solid frozen; follows input while live
      ║   (BTN)   ║   BTN (GPIO6) - short: mode; long: freeze latch; hold+POT: shift
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - freeze gate (HIGH = freeze)
      ║ (o)   (o) ║   IN2 (GPIO0) - re-capture trigger (grab fresh audio)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial freeze firmware (maddie synths original, shared FreezeCore)

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
#include <EEPROM.h>       // persisted mix / crossfade / mode
#include <Mod2Common.h>   // Shared MOD2 pin map, PWM-audio setup and helpers
#include <FreezeCore.h>   // Shared freeze DSP (also used by rack-plugins/src/mod2-freeze.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // below = mode, above = latch toggle
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  float xfade;
  uint8_t mode;
};
constexpr uint32_t SETTINGS_MAGIC = 0x46525A31;  // "FRZ1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
// ~400 KB int16 ring = ~5.5 s at ~36.6 kHz (RP2350 has 520 KB SRAM).
static int16_t freezeArena[(uint32_t)(sc::kFreezeBufferSec * mod2::AUDIO_FS) + 16];
sc::FreezeCore freeze;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-chorus). */
volatile float   g_loopLen  = 0.5f;
volatile float   g_position = 0.0f;
volatile float   g_xfade    = 0.030f;
volatile float   g_wet      = 1.0f;
volatile uint8_t g_mode     = sc::FREEZE_FORWARD;
volatile bool    g_latch    = false;  // manual freeze latch (BUTTON long press)
volatile int16_t g_ledForce = -1;     // >=0: loop-driven blink level, -1: core LED

/* ISR-only edge state. */
static bool isrLastIn2 = false;

/* =======================================================================
 *  PWM interrupt service routine (~36.6 kHz)
 * ==================================================================== */
void __isr onPwmWrap()
{
  /* --- audio input: sample the CV jack at the audio rate ------------- */
  const int raw = analogRead(mod2::CV_PIN);
  const float in = dcBlock.process(raw * (2.0f / 1023.0f) - 1.0f);

  /* --- freeze --------------------------------------------------------- */
  freeze.loopLen  = g_loopLen;
  freeze.position = g_position;
  freeze.xfadeLen = g_xfade;
  freeze.wet      = g_wet;
  freeze.mode     = g_mode;
  // Freeze while IN1 is high OR the manual latch is engaged.
  freeze.freeze   = (digitalRead(mod2::IN1_PIN) == HIGH) || g_latch;

  /* --- IN2: re-capture trigger (rising edge) ------------------------- */
  const bool in2 = digitalRead(mod2::IN2_PIN) == HIGH;
  if (in2 && !isrLastIn2)
    freeze.recapture();
  isrLastIn2 = in2;

  float out = freeze.process(in, 1.0f / mod2::AUDIO_FS);

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: solid when frozen / follows input while live, unless loop()
         is blinking a mode / latch ID ------------------------------------ */
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(freeze.ledLevel() * mod2::PWM_FS));

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
  pinMode(A0, INPUT);                       // loop length / mix (shifted)
  pinMode(A1, INPUT);                       // position / crossfade (shifted)
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // freeze gate
  pinMode(mod2::IN2_PIN, INPUT);            // re-capture trigger
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // mode / latch / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_xfade = sc::clampf(s.xfade, sc::kFreezeMinLoopSec, sc::kFreezeMaxXfadeSec);
    g_mode = s.mode < sc::FREEZE_MODE_COUNT ? s.mode : sc::FREEZE_FORWARD;
  }

  freeze.init(freezeArena, sizeof(freezeArena) / sizeof(freezeArena[0]));

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (mode / latch / shift layer), flash saves
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

  /* --- button: short = mode, long = latch, hold + POT = shift layer ---- */
  static bool lastPressed = false;
  static bool shiftEngaged = false;   // any pot moved while held
  static bool latchFired = false;     // long-press latch already toggled
  static uint32_t pressStartMs = 0;
  static float pot1AtPress = 0.0f, pot2AtPress = 0.0f;
  static mod2::PickupParam lenPickup, posPickup;
  static bool dirty = false;
  static uint32_t lastChangeMs = 0;

  const bool pressed = digitalRead(mod2::BUTTON_PIN) == LOW;
  const uint32_t now = millis();

  /* blink state (mode / latch feedback) */
  static uint8_t blinkPhasesLeft = 0;
  static uint32_t blinkNextMs = 0;

  if (pressed && !lastPressed) {  // press edge
    pressStartMs = now;
    pot1AtPress = pot1;
    pot2AtPress = pot2;
    shiftEngaged = false;
    latchFired = false;
  }
  if (pressed) {
    /* Shift layer: POT1 -> frozen/live mix, POT2 -> seam crossfade. */
    if (fabsf(pot1 - pot1AtPress) > POT_MOVE_THRESHOLD) {
      shiftEngaged = true;
      g_wet = pot1;
      dirty = true;
      lastChangeMs = now;
    }
    if (fabsf(pot2 - pot2AtPress) > POT_MOVE_THRESHOLD) {
      shiftEngaged = true;
      g_xfade = sc::freezeXfadeSec(pot2);
      dirty = true;
      lastChangeMs = now;
    }
    /* Long press with no pot movement = toggle the manual freeze latch. */
    if (!shiftEngaged && !latchFired && now - pressStartMs >= SHORT_PRESS_MS) {
      latchFired = true;
      g_latch = !g_latch;
      blinkPhasesLeft = g_latch ? 6 : 2;  // 3 blinks on, 1 blink off
      blinkNextMs = now;
    }
  }
  if (!pressed && lastPressed) {  // release edge
    if (shiftEngaged) {
      /* POT1/POT2 were borrowed: freeze length/position until each pot is
         turned back across where it sat before the press. */
      lenPickup.targetValue = pot1AtPress;
      lenPickup.lastPotValue = pot1;
      lenPickup.pickupActive = true;
      posPickup.targetValue = pot2AtPress;
      posPickup.lastPotValue = pot2;
      posPickup.pickupActive = true;
    } else if (!latchFired && now - pressStartMs < SHORT_PRESS_MS) {
      g_mode = (g_mode + 1) % sc::FREEZE_MODE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_mode + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT1 -> loop length, POT2 -> position (unless shifted / pickup) - */
  if (!pressed) {
    if (mod2::checkPickup(lenPickup, pot1))
      g_loopLen = sc::freezeLoopLenSec(pot1, sc::kFreezeBufferSec);
    lenPickup.lastPotValue = pot1;
    if (mod2::checkPickup(posPickup, pot2))
      g_position = pot2;
    posPickup.lastPotValue = pot2;
  }

  /* --- LED blink code for a mode change / latch toggle ---------------- */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to the core LED (frozen / input level)
    }
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_wet, g_xfade, g_mode};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
