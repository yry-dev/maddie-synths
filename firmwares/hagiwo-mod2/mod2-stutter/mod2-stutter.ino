/* STUTTER

Description:
Clock-aware beat repeat. Audio comes in on the CV jack (sampled by the RP2350
ADC inside the ~36.6 kHz PWM ISR, per firmwares/mod2-fx/README.md) and an
always-recording ~95 KB SRAM buffer keeps the last ~1.3 s ready. Patch a clock
into IN1 to set the musical grid and raise the stutter gate on IN2 (or latch it
with a long button press): the effect locks the last slice of audio and
machine-guns it in divisions picked by POT1 ({1/32 ... 1 bar} clocked, 20 ms -
1 s free). POT2 sets the per-repeat behaviour (decay / pitch-ramp amount).
Short button presses cycle the flavour: Straight / Decaying / Pitch-ramp up /
Pitch-ramp down. Hold BUTTON + turn POT1 for wet/dry mix, BUTTON + POT2 for
auto-stutter probability (roll per division for instant IDM); both persist to
flash with the flavour. The LED flashes on every repeat.
DSP lives in the shared sc::StutterCore (also used by the VCV Rack port).

Key Variables:
  A0 -> Repeat length (clock division / 20 ms - 1 s free)
  A1 -> Behaviour amount (decay / pitch-ramp per repeat)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║  STUTTER  ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - repeat length / division (BTN held: wet/dry)
      ║   LEN     ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - behaviour amount (BTN held: probability)
      ║   BEHAV   ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - flashes per repeat (blinks flavour ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: flavour; long: latch; hold+POT: shift
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - clock in (defines the musical grid)
      ║ (o)   (o) ║   IN2 (GPIO0) - stutter gate (HIGH = repeats)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial stutter firmware (maddie synths original, shared StutterCore)

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
#include <EEPROM.h>        // persisted wet/dry + probability + flavour
#include <Mod2Common.h>    // Shared MOD2 pin map, PWM-audio setup and helpers
#include <StutterCore.h>   // Shared stutter DSP (also used by rack-plugins/src/mod2-stutter.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // below = flavour, above = latch
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save
// A clock is "present" while the last IN1 tick is under a few periods old.
constexpr uint32_t CLOCK_MAX_PERIOD = (uint32_t)(sc::kStutterBufferSec * mod2::AUDIO_FS);

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  float probability;
  uint8_t mode;
};
constexpr uint32_t SETTINGS_MAGIC = 0x53545231;  // "STR1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
// ~95 KB int16 arena in SRAM = ~1.3 s of always-recording audio (RP2350 has
// 520 KB) — covers a bar at 60 BPM in 4/4 with headroom.
static int16_t stutterArena[(uint32_t)(sc::kStutterBufferSec * mod2::AUDIO_FS) + 16];
sc::StutterCore stutter;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-delay). */
volatile float   g_sliceSec    = 0.125f;
volatile float   g_amount      = 0.4f;
volatile float   g_wet         = 1.0f;
volatile float   g_probability = 1.0f;
volatile uint8_t g_mode        = sc::STUTTER_STRAIGHT;
volatile bool    g_latch       = false;   // BUTTON-long manual stutter latch
volatile int16_t g_ledForce    = -1;      // >=0: loop-driven blink, -1: per-repeat flash

/* ISR -> loop: IN1 clock measurement. */
volatile uint32_t g_clockPeriod = 0;                  // samples between the last two ticks
volatile uint32_t g_samplesSinceTick = CLOCK_MAX_PERIOD;

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

  /* --- stutter -------------------------------------------------------- */
  stutter.sliceSec    = g_sliceSec;
  stutter.amount      = g_amount;
  stutter.wet         = g_wet;
  stutter.probability = g_probability;
  stutter.mode        = g_mode;
  // Engage on the IN2 gate OR the manual button latch.
  stutter.engaged = (digitalRead(mod2::IN2_PIN) == HIGH) || g_latch;
  float out = stutter.process(in, 1.0f / mod2::AUDIO_FS);

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: flashes per repeat, unless loop() is blinking a flavour ID */
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(stutter.ledLevel() * mod2::PWM_FS));

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
  pinMode(A0, INPUT);                       // repeat length / wet-dry (shifted)
  pinMode(A1, INPUT);                       // behaviour / probability (shifted)
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // clock in
  pinMode(mod2::IN2_PIN, INPUT);            // stutter gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // flavour / latch / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_probability = sc::clampf(s.probability, 0.0f, 1.0f);
    g_mode = s.mode < sc::STUTTER_MODE_COUNT ? s.mode : sc::STUTTER_STRAIGHT;
  }

  stutter.init(stutterArena, sizeof(stutterArena) / sizeof(stutterArena[0]));

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (flavour / latch / shift layers), clock sync,
 *  flash saves
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

  /* --- button: short = flavour, long = latch, hold+POT1 = wet/dry,
         hold+POT2 = probability ----------------------------------------- */
  static bool lastPressed = false;
  static bool shifted = false;            // a shift layer engaged this press
  static bool latchToggled = false;       // long-press latch handled this press
  static uint32_t pressStartMs = 0;
  static float pot1AtPress = 0.0f, pot2AtPress = 0.0f;
  static mod2::PickupParam lenPickup, behavPickup;
  static bool dirty = false;
  static uint32_t lastChangeMs = 0;

  const bool pressed = digitalRead(mod2::BUTTON_PIN) == LOW;
  const uint32_t now = millis();

  /* blink state (flavour-ID feedback: mode+1 flashes) */
  static uint8_t blinkPhasesLeft = 0;
  static uint32_t blinkNextMs = 0;

  if (pressed && !lastPressed) {  // press edge
    pressStartMs = now;
    pot1AtPress = pot1;
    pot2AtPress = pot2;
    shifted = false;
    latchToggled = false;
  }
  /* POT1 shift -> wet/dry, POT2 shift -> probability. */
  if (pressed && fabsf(pot1 - pot1AtPress) > POT_MOVE_THRESHOLD) {
    g_wet = pot1;
    shifted = true;
    dirty = true;
    lastChangeMs = now;
  }
  if (pressed && fabsf(pot2 - pot2AtPress) > POT_MOVE_THRESHOLD) {
    g_probability = pot2;
    shifted = true;
    dirty = true;
    lastChangeMs = now;
  }
  /* Long press with no shift = toggle the manual stutter latch, at the
     moment the hold crosses the threshold (not release-dependent).       */
  if (pressed && !shifted && !latchToggled &&
      now - pressStartMs >= SHORT_PRESS_MS) {
    latchToggled = true;
    g_latch = !g_latch;
  }
  if (!pressed && lastPressed) {  // release edge
    if (shifted) {
      /* A pot was borrowed: freeze its parameter until it returns across
         where it sat before the press. */
      lenPickup.targetValue = pot1AtPress;
      lenPickup.lastPotValue = pot1;
      lenPickup.pickupActive = true;
      behavPickup.targetValue = pot2AtPress;
      behavPickup.lastPotValue = pot2;
      behavPickup.pickupActive = true;
    } else if (!latchToggled && now - pressStartMs < SHORT_PRESS_MS) {
      g_mode = (g_mode + 1) % sc::STUTTER_MODE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_mode + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT2 -> behaviour amount (unless shifted / waiting for pickup) -- */
  if (!pressed) {
    if (mod2::checkPickup(behavPickup, pot2))
      g_amount = pot2;
    behavPickup.lastPotValue = pot2;
  }

  /* --- POT1 -> repeat length: clock division when clocked, else free --- */
  noInterrupts();
  const uint32_t clockPeriod = g_clockPeriod;
  const uint32_t sinceTick = g_samplesSinceTick;
  interrupts();
  const bool clocked = clockPeriod > 0 && sinceTick < clockPeriod * 4 &&
                       sinceTick < CLOCK_MAX_PERIOD;

  if (!pressed && mod2::checkPickup(lenPickup, pot1)) {
    if (clocked) {
      const float periodSec = clockPeriod / mod2::AUDIO_FS;
      g_sliceSec = sc::clampf(periodSec * sc::stutterClockRatio(pot1),
                              0.020f, sc::kStutterBufferSec);
    } else {
      g_sliceSec = sc::stutterFreeSec(pot1);
    }
  }
  if (!pressed)
    lenPickup.lastPotValue = pot1;

  /* --- LED flavour-ID blink (per-repeat flash resumes when done) ------ */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to per-repeat flashing
    }
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_wet, g_probability, g_mode};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
