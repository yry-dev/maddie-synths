/* SPECTRAL FREEZE

Description:
FFT-domain infinite sustain — freeze the *spectrum* rather than the waveform.
Audio comes in on the CV jack (sampled by the RP2350 ADC inside the ~36.6 kHz
PWM ISR, per firmwares/mod2-fx/README.md). An STFT (1024-pt FFT, Hann window,
75% overlap / 256-sample hop) analyses the input; on freeze the per-bin
magnitudes are held while each bin's phase keeps advancing, giving an endless,
motionless-yet-alive pad from any instant of sound. POT1 sets the shimmer (per-
bin phase-randomisation depth), POT2 the spectral tilt. Short button presses
cycle the freeze character: Single frame / 4-frame averaged (smoother) /
Drifting (slow random bin walk). IN1 captures a fresh spectrum (crossfaded in
the frequency domain); IN2 is the freeze gate. Hold BUTTON + turn POT1 for
wet/dry (frozen vs live), persisted to flash with the mode. The LED is solid
while frozen and breathes at the shimmer rate.

Because a 1024-pt FFT cannot run inside the audio ISR, the ISR only samples the
ADC into a lock-free input ring and streams the output ring to the PWM DAC; the
heavy phase-vocoder frames run in loop() (on hardware this belongs on core 1,
per the README) with the two rings acting as the ping-pong hop buffers. DSP
lives in the shared sc::SpectralFreezeCore (also used by the VCV Rack port).

Key Variables:
  A0 -> Shimmer (phase-randomisation depth) (BTN held: wet/dry mix)
  A1 -> Spectral tilt (dark <-> bright) (BTN held: freeze attack/blend time)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║  FREEZE   ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - shimmer / animation (BTN held: wet/dry mix)
      ║  SHIMMER  ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - spectral tilt (BTN held: attack/blend time)
      ║   TILT    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - solid when frozen; breathes at shimmer rate
      ║   (BTN)   ║   BTN (GPIO6) - short: mode; hold+POT1: wet/dry;
      ║           ║                 hold+POT2: freeze attack/blend time
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - capture a fresh spectrum (spectral crossfade)
      ║ (o)   (o) ║   IN2 (GPIO0) - freeze gate (HIGH = frozen)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial spectral-freeze firmware (maddie synths original, shared
        SpectralFreezeCore)

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
#include <EEPROM.h>               // persisted wet/dry + mode
#include <Mod2Common.h>           // Shared MOD2 pin map, PWM-audio setup, helpers
#include <SpectralFreezeCore.h>   // Shared DSP (also used by rack-plugins/src/mod2-spectral-freeze.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // max length of a mode-cycle press
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  float attack;  // freeze engage/release crossfade, seconds
  uint8_t mode;
};
constexpr uint32_t SETTINGS_MAGIC = 0x53504632;  // "SPF2" (layout changed)

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
sc::SpectralFreezeCore freezeCore;   // ~34 KB of tables + STFT buffers
sc::DcBlocker dcBlock;               // removes the ADC's unipolar bias

/* Lock-free SPSC rings decouple the audio ISR (core 0) from the heavy FFT
   frame work in loop() (core 1 on hardware): the ISR fills g_inRing and drains
   g_outRing; loop() drains g_inRing through the core and fills g_outRing. Sized
   well past one FFT frame so the periodic 1024-pt burst never underruns. */
constexpr uint32_t RING = 4096;      // power of two, > 2 * FFT frame
constexpr uint32_t RING_MASK = RING - 1;
volatile float   g_inRing[RING];
volatile float   g_outRing[RING];
volatile uint32_t g_inHead = 0, g_inTail = 0;    // ISR writes head, loop reads tail
volatile uint32_t g_outHead = 0, g_outTail = 0;  // loop writes head, ISR reads tail

/* Volatile shadows: loop() writes, ISR/loop read. */
volatile float   g_wet = 1.0f;
volatile uint8_t g_mode = sc::SPFREEZE_SINGLE;
volatile int16_t g_ledLevel = 0;   // 0..PWM_FS, computed in loop()
volatile bool    g_ledForceActive = false;
volatile int16_t g_ledForce = 0;

/* ISR-only edge state for IN1 (capture) / IN2 (freeze gate). */
static bool isrLastIn1 = false;
volatile bool g_captureReq = false;  // set by ISR on IN1 rising edge
volatile bool g_freezeGate = false;  // IN2 level

/* =======================================================================
 *  PWM interrupt service routine (~36.6 kHz) — cheap only: ADC in, ring
 *  bookkeeping, PWM out. No FFT here.
 * ==================================================================== */
void __isr onPwmWrap()
{
  /* --- audio input: sample the CV jack, push to the input ring -------- */
  const int raw = analogRead(mod2::CV_PIN);
  const float in = dcBlock.process(raw * (2.0f / 1023.0f) - 1.0f);
  const uint32_t inNext = (g_inHead + 1) & RING_MASK;
  if (inNext != g_inTail) {          // drop if the (large) ring is full
    g_inRing[g_inHead] = in;
    g_inHead = inNext;
  }

  /* --- IN1 rising edge -> request a fresh spectrum capture ------------ */
  const bool in1 = digitalRead(mod2::IN1_PIN) == HIGH;
  if (in1 && !isrLastIn1) g_captureReq = true;
  isrLastIn1 = in1;

  /* --- IN2 level -> freeze gate -------------------------------------- */
  g_freezeGate = digitalRead(mod2::IN2_PIN) == HIGH;

  /* --- audio output: drain the output ring to the PWM DAC ------------ */
  float out = 0.0f;
  if (g_outTail != g_outHead) {
    out = g_outRing[g_outTail];
    g_outTail = (g_outTail + 1) & RING_MASK;
  }
  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED (level computed in loop()) -------------------------------- */
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     g_ledForceActive ? (uint16_t)g_ledForce
                                      : (uint16_t)g_ledLevel);

  pwm_clear_irq(sliceIRQ);
}

/* =======================================================================
 *  Guarded pot read — the ISR reads A2 through the same ADC mux; block the
 *  IRQ for the ~2 us conversion so it just delays one audio sample slightly.
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
  pinMode(A0, INPUT);                       // shimmer / wet-dry (shifted)
  pinMode(A1, INPUT);                       // tilt
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // capture trigger
  pinMode(mod2::IN2_PIN, INPUT);            // freeze gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // mode / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    freezeCore.attackSec = sc::clampf(s.attack, 0.005f, 2.0f);
    g_mode = s.mode < sc::SPFREEZE_MODE_COUNT ? s.mode : sc::SPFREEZE_SINGLE;
  }

  freezeCore.init();

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Panel scan — pots, button (mode / wet-dry shift), flash saves. Runs in
 *  loop() alongside the DSP pump; only touched every few ms.
 * ==================================================================== */
static void scanPanel()
{
  /* --- pots, one-pole smoothed ---------------------------------------- */
  static float pot1 = 0.0f, pot2 = 0.0f;
  static bool primed = false;
  const float r1 = readPotGuarded(mod2::POT1_PIN);
  const float r2 = readPotGuarded(mod2::POT2_PIN);
  if (!primed) { pot1 = r1; pot2 = r2; primed = true; }
  pot1 += (r1 - pot1) * 0.2f;
  pot2 += (r2 - pot2) * 0.2f;

  /* --- button: short press = mode; hold + POT1 = wet/dry, hold + POT2 =
         freeze attack/blend time (dual shift layer, as on mod2-comb) ----- */
  static bool lastPressed = false;
  static bool adjustingWet = false;
  static bool adjustingAttack = false;
  static uint32_t pressStartMs = 0;
  static float potAtPress = 0.0f;
  static float pot2AtPress = 0.0f;
  static mod2::PickupParam shimmerPickup;
  static mod2::PickupParam tiltPickup;
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
    pot2AtPress = pot2;
    adjustingWet = false;
    adjustingAttack = false;
  }
  if (pressed && !adjustingWet && fabsf(pot1 - potAtPress) > POT_MOVE_THRESHOLD)
    adjustingWet = true;  // shift layer engaged; short-press action suppressed
  if (pressed && !adjustingAttack && fabsf(pot2 - pot2AtPress) > POT_MOVE_THRESHOLD)
    adjustingAttack = true;
  if (pressed && adjustingWet) {
    g_wet = pot1;
    dirty = true;
    lastChangeMs = now;
  }
  if (pressed && adjustingAttack) {
    freezeCore.attackSec = sc::spectralAttackSec(pot2);
    dirty = true;
    lastChangeMs = now;
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustingWet) {
      /* POT1 was borrowed for wet/dry: freeze the shimmer until the pot is
         turned back across where it sat before the press.               */
      shimmerPickup.targetValue = potAtPress;
      shimmerPickup.lastPotValue = pot1;
      shimmerPickup.pickupActive = true;
    }
    if (adjustingAttack) {
      /* POT2 was borrowed for attack: same pickup dance for the tilt.  */
      tiltPickup.targetValue = pot2AtPress;
      tiltPickup.lastPotValue = pot2;
      tiltPickup.pickupActive = true;
    }
    if (!adjustingWet && !adjustingAttack &&
        now - pressStartMs < SHORT_PRESS_MS) {
      g_mode = (g_mode + 1) % sc::SPFREEZE_MODE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_mode + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT1 -> shimmer, POT2 -> tilt (unless shifted / in pickup) ----- */
  if (!pressed) {
    if (mod2::checkPickup(shimmerPickup, pot1))
      freezeCore.shimmer = sc::spectralShimmerRad(pot1);
    shimmerPickup.lastPotValue = pot1;
    if (mod2::checkPickup(tiltPickup, pot2))
      freezeCore.tiltSlope = sc::spectralTiltSlope(pot2);
    tiltPickup.lastPotValue = pot2;
  }

  /* --- LED: solid/breathe when frozen (core), mode-ID blink overrides - */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      g_ledForceActive = true;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0) g_ledForceActive = false;
    }
  } else {
    g_ledLevel = (int16_t)(freezeCore.ledLevel() * mod2::PWM_FS);
  }

  /* --- debounced flash save ------------------------------------------ */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_wet, freezeCore.attackSec, g_mode};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}

/* =======================================================================
 *  Main loop — pump the STFT (heavy FFT frames) and scan the panel. The
 *  input/output rings decouple this from the audio ISR (this belongs on
 *  core 1 on hardware, per the README).
 * ==================================================================== */
void loop()
{
  const float dt = 1.0f / mod2::AUDIO_FS;

  /* --- pump every buffered input sample through the core -------------- */
  while (g_inTail != g_inHead) {
    // Latch shared controls once per sample.
    freezeCore.wet = g_wet;
    freezeCore.mode = g_mode;
    freezeCore.freeze = g_freezeGate;
    if (g_captureReq) { freezeCore.triggerCapture(); g_captureReq = false; }

    const float in = g_inRing[g_inTail];
    g_inTail = (g_inTail + 1) & RING_MASK;

    const float out = freezeCore.process(in, dt);

    const uint32_t outNext = (g_outHead + 1) & RING_MASK;
    if (outNext != g_outTail) {       // push if the output ring has room
      g_outRing[g_outHead] = out;
      g_outHead = outNext;
    }
  }

  /* --- panel / LED / flash (cheap, runs between pumps) ---------------- */
  scanPanel();
}
