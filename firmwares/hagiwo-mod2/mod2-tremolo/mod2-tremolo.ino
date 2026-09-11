/* TREMOLO

Description:
LFO-driven VCA (amplitude modulation) — the easiest, most useful-live MOD2 FX.
Audio comes in on the CV jack (sampled by the RP2350 ADC inside the ~36.6 kHz
PWM ISR, per firmwares/mod2-fx/README.md). One LFO scales the input gain from
full (peak) down to (1 - depth) (trough); depth to 100% is a full chop to
silence. POT1 sets the rate (0.1 - 30 Hz); POT2 the depth. Short button presses
cycle the LFO shape: Sine / Triangle / Square (smoothed to declick) / Ramp-down.
A clock on IN1 locks the rate to a musical ratio of the incoming period
({1/4, 1/2, 3/4, 1, 1.5, 2, 3, 4}x picked by POT1); long presses (>=0.6 s) are
taps — the interval between the last two long-press starts sets the rate. IN2
resets the LFO phase, so the chop can be pinned to a downbeat. Hold BUTTON +
turn POT1 for wet/dry mix (persisted to flash with the shape). The LED breathes
with the LFO — an instant visual tempo check.
DSP lives in the shared sc::TremoloCore (also used by the VCV Rack port).

Key Variables:
  A0 -> LFO rate (0.1 - 30 Hz, exponential taper) / clock division
  A1 -> Depth (0 - 100%, top end full chop)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║  TREMOLO  ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - LFO rate / clock division (BTN held: wet/dry)
      ║   RATE    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - depth (0..100%, top = full chop)
      ║   DEPTH   ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - breathes with the LFO (blinks shape ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: shape; long: tap; hold+POT1: wet/dry
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - clock in (rate locks to divisions of it)
      ║ (o)   (o) ║   IN2 (GPIO0) - LFO phase reset (chop syncs to downbeats)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial tremolo firmware (maddie synths original, shared TremoloCore)

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
#include <EEPROM.h>        // persisted wet/dry + shape
#include <Mod2Common.h>    // Shared MOD2 pin map, PWM-audio setup and helpers
#include <TremoloCore.h>   // Shared tremolo DSP (also used by rack-plugins/src/mod2-tremolo.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // below = shape cycle, above = tap
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save
constexpr float MIN_RATE_HZ        = 0.1f;    // clamp for clock/tap-derived rate
constexpr float MAX_RATE_HZ        = 30.0f;
constexpr uint32_t TAP_MAX_MS      = 10000;   // longest usable tap interval (0.1 Hz)
// A clock is "present" while the last IN1 tick is under a few slow periods old.
constexpr uint32_t CLOCK_MAX_PERIOD =
    (uint32_t)(mod2::AUDIO_FS / MIN_RATE_HZ);  // slowest measurable clock

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  uint8_t shape;
};
constexpr uint32_t SETTINGS_MAGIC = 0x54524D31;  // "TRM1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
sc::TremoloCore tremolo;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-delay). */
volatile float   g_rateHz = 4.0f;
volatile float   g_depth  = 0.5f;
volatile float   g_wet    = 1.0f;
volatile uint8_t g_shape  = sc::TREMOLO_SINE;
volatile int16_t g_ledForce = -1;  // >=0: loop-driven blink level, -1: breathe

/* ISR -> loop: IN1 clock measurement. */
volatile uint32_t g_clockPeriod = 0;              // samples between the last two ticks
volatile uint32_t g_samplesSinceTick = CLOCK_MAX_PERIOD;

/* ISR-only edge state. */
static bool isrLastIn1 = false;
static bool isrLastIn2 = false;

/* =======================================================================
 *  PWM interrupt service routine (~36.6 kHz)
 * ==================================================================== */
void __isr onPwmWrap()
{
  /* --- audio input: sample the CV jack at the audio rate ------------- */
  const int raw = analogRead(mod2::CV_PIN);
  const float in = dcBlock.process(raw * (2.0f / 1023.0f) - 1.0f);

  /* --- IN1: clock period measurement (sample-accurate edges) --------- */
  const bool in1 = digitalRead(mod2::IN1_PIN) == HIGH;
  if (in1 && !isrLastIn1) {
    if (g_samplesSinceTick < CLOCK_MAX_PERIOD)
      g_clockPeriod = g_samplesSinceTick;
    g_samplesSinceTick = 0;
  } else if (g_samplesSinceTick < 0xFFFFFFF0u) {
    g_samplesSinceTick++;
  }
  isrLastIn1 = in1;

  /* --- IN2: LFO phase reset on the rising edge (sync to downbeat) ----- */
  const bool in2 = digitalRead(mod2::IN2_PIN) == HIGH;
  if (in2 && !isrLastIn2)
    tremolo.resetPhase();
  isrLastIn2 = in2;

  /* --- tremolo -------------------------------------------------------- */
  tremolo.rateHz = g_rateHz;
  tremolo.depth  = g_depth;
  tremolo.wet    = g_wet;
  tremolo.shape  = g_shape;
  float out = tremolo.process(in, 1.0f / mod2::AUDIO_FS);

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: breathes with the LFO, unless loop() is blinking a shape ID */
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(tremolo.ledLevel() * mod2::PWM_FS));

  pwm_clear_irq(sliceIRQ);
}

/* =======================================================================
 *  Guarded pot read — the ISR reads A2 through the same ADC mux (see the
 *  mod2-delay note); blocking the IRQ for the ~2 us conversion just delays
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
  pinMode(A0, INPUT);                       // rate / wet-dry (shifted)
  pinMode(A1, INPUT);                       // depth
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // clock in
  pinMode(mod2::IN2_PIN, INPUT);            // LFO phase reset
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // shape / tap / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_shape = s.shape < sc::TREMOLO_SHAPE_COUNT ? s.shape : sc::TREMOLO_SINE;
  }

  tremolo.reset();

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (shape / tap / wet-dry shift), clock sync,
 *  LED shape blink, flash saves
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

  g_depth = sc::tremoloDepth(pot2);

  /* --- button: short = shape, long = tap, hold+POT1 = wet/dry --------- */
  static bool lastPressed = false;
  static bool adjustingWet = false;
  static bool tapRegistered = false;
  static uint32_t pressStartMs = 0;
  static uint32_t lastTapStartMs = 0;
  static float potAtPress = 0.0f;
  static float tapRateHz = 0.0f;          // 0 = no tap override active
  static mod2::PickupParam ratePickup;
  static bool dirty = false;
  static uint32_t lastChangeMs = 0;

  const bool pressed = digitalRead(mod2::BUTTON_PIN) == LOW;
  const uint32_t now = millis();

  /* blink state (shape-ID feedback: shape+1 flashes) */
  static uint8_t blinkPhasesLeft = 0;
  static uint32_t blinkNextMs = 0;

  if (pressed && !lastPressed) {  // press edge
    pressStartMs = now;
    potAtPress = pot1;
    adjustingWet = false;
    tapRegistered = false;
  }
  if (pressed && !adjustingWet && fabsf(pot1 - potAtPress) > POT_MOVE_THRESHOLD)
    adjustingWet = true;  // shift layer engaged; shape/tap suppressed
  if (pressed && adjustingWet) {
    g_wet = pot1;
    dirty = true;
    lastChangeMs = now;
  }
  /* Long press = tap, timestamped at the press EDGE. Registered as soon as
     the hold crosses the threshold so the rhythm isn't release-dependent.  */
  if (pressed && !adjustingWet && !tapRegistered &&
      now - pressStartMs >= SHORT_PRESS_MS) {
    tapRegistered = true;
    const uint32_t interval = pressStartMs - lastTapStartMs;
    lastTapStartMs = pressStartMs;
    if (interval >= SHORT_PRESS_MS && interval <= TAP_MAX_MS) {
      tapRateHz = sc::clampf(1000.0f / (float)interval, MIN_RATE_HZ, MAX_RATE_HZ);
      /* POT1 gives way to the tap until it moves past where it sits now. */
      ratePickup.targetValue = pot1;
      ratePickup.lastPotValue = pot1;
      ratePickup.pickupActive = true;
    }
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustingWet) {
      /* POT1 was borrowed for wet/dry: freeze the rate until the pot is
         turned back across where it sat before the press.               */
      ratePickup.targetValue = potAtPress;
      ratePickup.lastPotValue = pot1;
      ratePickup.pickupActive = true;
    } else if (!tapRegistered && now - pressStartMs < SHORT_PRESS_MS) {
      g_shape = (g_shape + 1) % sc::TREMOLO_SHAPE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_shape + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- LFO rate: clock > tap > POT1 ----------------------------------- */
  noInterrupts();
  const uint32_t clockPeriod = g_clockPeriod;
  const uint32_t sinceTick = g_samplesSinceTick;
  interrupts();
  const bool clocked = clockPeriod > 0 && sinceTick < clockPeriod * 4 &&
                       sinceTick < CLOCK_MAX_PERIOD;

  if (!pressed) {
    if (mod2::checkPickup(ratePickup, pot1))
      tapRateHz = 0.0f;  // knob reclaimed: back to manual rate
    ratePickup.lastPotValue = pot1;
  }

  if (clocked) {
    /* POT1 picks a musical ratio of the measured clock rate. */
    const float clockHz = mod2::AUDIO_FS / (float)clockPeriod;
    g_rateHz = sc::clampf(clockHz * sc::tremoloClockRatio(pot1),
                          MIN_RATE_HZ, MAX_RATE_HZ);
  } else if (tapRateHz > 0.0f) {
    g_rateHz = tapRateHz;
  } else if (!pressed && !ratePickup.pickupActive) {
    g_rateHz = sc::tremoloRateHz(pot1);
  }

  /* --- LED shape-ID blink code (breathe resumes when it finishes) ----- */
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
    Settings s = {SETTINGS_MAGIC, g_wet, g_shape};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
