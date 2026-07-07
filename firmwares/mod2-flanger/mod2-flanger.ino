/* FLANGER

Description:
Swept-comb flanger. Audio comes in on the CV jack (sampled by the RP2350 ADC
inside the ~36.6 kHz PWM ISR, per firmwares/mod2-fx/README.md). A single
fractional tap sweeps 0.2 - 12 ms over a short delay line with bipolar
feedback around it — jet-swoosh comb sweeps. POT1 sets the LFO rate (0.02 -
5 Hz; full CCW = manual mode, where POT2 sweeps the comb by hand), POT2 the
feedback (CCW negative / CW positive, centre = none; the last few percent
deliberately scream). Short button presses cycle the sweep source: triangle /
sine / envelope-follow (input level drives the sweep — great on drums). Hold
BUTTON + turn POT1 for wet/dry mix, hold BUTTON + turn POT2 for sweep depth —
both persisted to flash with the shape. IN1 retriggers the LFO, IN2 is a
bypass gate. The LED follows the sweep. DSP lives in the shared
sc::FlangerCore (also used by the VCV Rack port).

Key Variables:
  A0 -> LFO rate (0.02 - 5 Hz; full CCW = manual sweep via POT2)
  A1 -> Feedback (bipolar) / manual sweep position (manual mode)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║  FLANGER  ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - LFO rate (BTN held: wet/dry mix)
      ║   RATE    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - feedback (BTN held: sweep depth)
      ║   FDBK    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - follows the sweep (blinks shape ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: sweep shape; hold+POT: shift
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - LFO retrigger
      ║ (o)   (o) ║   IN2 (GPIO0) - bypass gate (HIGH = dry)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial flanger firmware (maddie synths original, shared FlangerCore)

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
#include <EEPROM.h>       // persisted wet/dry + depth + shape
#include <Mod2Common.h>   // Shared MOD2 pin map, PWM-audio setup and helpers
#include <FlangerCore.h>  // Shared flanger DSP (also used by rack-plugins/src/mod2-flanger.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // max length of a shape-cycle press
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  float depth;
  uint8_t shape;
};
constexpr uint32_t SETTINGS_MAGIC = 0x464C4731;  // "FLG1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
// ~20 ms int16 arena in SRAM — the tap never reaches past kFlangerBufferSec.
static int16_t flangerArena[(uint32_t)(sc::kFlangerBufferSec * mod2::AUDIO_FS) + 16];
sc::FlangerCore flanger;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-bitcrusher). */
volatile float   g_rateHz    = 0.3f;
volatile bool    g_manual    = false;
volatile float   g_manualPos = 0.5f;
volatile float   g_feedback  = 0.0f;
volatile float   g_depth     = 0.7f;
volatile uint8_t g_shape     = sc::FLANGER_TRI;
volatile float   g_wet       = 0.5f;
volatile int16_t g_ledForce  = -1;  // >=0: loop-driven blink level, -1: sweep

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

  /* --- IN1: LFO retrigger (sample-accurate edge detect) --------------- */
  const bool in1 = digitalRead(mod2::IN1_PIN) == HIGH;
  if (in1 && !isrLastIn1)
    flanger.retrigger();
  isrLastIn1 = in1;

  /* --- flange ---------------------------------------------------------- */
  flanger.rateHz    = g_rateHz;
  flanger.manual    = g_manual;
  flanger.manualPos = g_manualPos;
  flanger.feedback  = g_feedback;
  flanger.depth     = g_depth;
  flanger.shape     = g_shape;
  flanger.wet       = g_wet;
  float out = flanger.process(in, 1.0f / mod2::AUDIO_FS);

  /* --- IN2: bypass gate (HIGH = pass the dry input) ------------------- */
  if (digitalRead(mod2::IN2_PIN) == HIGH)
    out = in;

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: follows the sweep, unless loop() is blinking a shape ID --- */
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(flanger.ledLevel() * mod2::PWM_FS));

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
  pinMode(A1, INPUT);                       // feedback / depth (shifted)
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // LFO retrigger
  pinMode(mod2::IN2_PIN, INPUT);            // bypass gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // shape / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_depth = sc::clampf(s.depth, 0.0f, 1.0f);
    g_shape = s.shape < sc::FLANGER_SHAPE_COUNT ? s.shape : sc::FLANGER_TRI;
  }

  flanger.init(flangerArena, sizeof(flangerArena) / sizeof(flangerArena[0]));

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (shape / dual shift layer), flash saves
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

  /* --- button: short = shape, hold + POT1/POT2 = shift ---------------- */
  static bool lastPressed = false;
  static bool adjustingWet = false;    // shift layer: POT1 -> wet/dry
  static bool adjustingDepth = false;  // shift layer: POT2 -> sweep depth
  static uint32_t pressStartMs = 0;
  static float pot1AtPress = 0.0f, pot2AtPress = 0.0f;
  static mod2::PickupParam ratePickup, fbPickup;
  static bool dirty = false;
  static uint32_t lastChangeMs = 0;

  const bool pressed = digitalRead(mod2::BUTTON_PIN) == LOW;
  const uint32_t now = millis();

  /* blink state (shape-ID feedback: shape+1 flashes) */
  static uint8_t blinkPhasesLeft = 0;
  static uint32_t blinkNextMs = 0;

  if (pressed && !lastPressed) {  // press edge
    pressStartMs = now;
    pot1AtPress = pot1;
    pot2AtPress = pot2;
    adjustingWet = false;
    adjustingDepth = false;
  }
  if (pressed && !adjustingWet && fabsf(pot1 - pot1AtPress) > POT_MOVE_THRESHOLD)
    adjustingWet = true;  // shift layer engaged; short-press action suppressed
  if (pressed && !adjustingDepth && fabsf(pot2 - pot2AtPress) > POT_MOVE_THRESHOLD)
    adjustingDepth = true;
  if (pressed && adjustingWet) {
    g_wet = pot1;
    dirty = true;
    lastChangeMs = now;
  }
  if (pressed && adjustingDepth) {
    g_depth = pot2;
    dirty = true;
    lastChangeMs = now;
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustingWet) {
      /* POT1 was borrowed for wet/dry: freeze the rate until the pot is
         turned back across where it sat before the press.               */
      ratePickup.targetValue = pot1AtPress;
      ratePickup.lastPotValue = pot1;
      ratePickup.pickupActive = true;
    }
    if (adjustingDepth) {
      fbPickup.targetValue = pot2AtPress;
      fbPickup.lastPotValue = pot2;
      fbPickup.pickupActive = true;
    }
    if (!adjustingWet && !adjustingDepth &&
        now - pressStartMs < SHORT_PRESS_MS) {
      g_shape = (g_shape + 1) % sc::FLANGER_SHAPE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_shape + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT1 -> rate / manual, POT2 -> feedback or hand sweep ---------- */
  if (!pressed) {
    if (mod2::checkPickup(ratePickup, pot1)) {
      g_manual = sc::flangerManual(pot1);
      g_rateHz = sc::flangerRateHz(pot1);
    }
    ratePickup.lastPotValue = pot1;
    if (mod2::checkPickup(fbPickup, pot2)) {
      if (g_manual)
        g_manualPos = pot2;  // manual mode: POT2 sweeps the comb by hand
      else
        g_feedback = sc::flangerFeedback(pot2);
    }
    fbPickup.lastPotValue = pot2;
  }

  /* --- LED blink code for the newly selected shape --------------------- */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to following the sweep
    }
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_wet, g_depth, g_shape};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
