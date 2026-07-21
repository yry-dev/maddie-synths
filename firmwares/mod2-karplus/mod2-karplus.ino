/* KARPLUS

Description:
Dedicated Karplus-Strong plucked string. A tuned feedback delay line (the
"string") is excited by an internal shaped noise burst (a pluck) or by external
audio on the CV jack (sampled by the RP2350 ADC inside the ~36.6 kHz PWM ISR,
per firmwares/mod2-fx/README.md) — a bowed / scraped string. POT1 sets the
pitch (semitone-quantized, A1..A5), POT2 the damping (loop-filter cutoff +
decay: bright/long .. dark/short). Short button presses cycle the mode:
Pluck (triggered strings decay naturally) / Bow (external input drives the loop
continuously so it sings) / Drone (near-unity loop that self-sustains, held
stable by an energy limiter). A long press (>=0.6 s) plucks the string, as does
a trigger on IN1. IN2 palm-mutes (chokes) the loop while high. Hold BUTTON +
turn POT1 for wet/dry mix (dry = the excitation signal), hold BUTTON + turn
POT2 for excitation colour (dark .. bright noise burst) — both persisted to
flash with the mode. The LED follows the string's energy (so it flashes on each
pluck). DSP lives in the shared sc::KarplusCore (also used by the VCV Rack port).

Key Variables:
  A0 -> Pitch / fundamental (semitone-quantized, A1 - A5)
  A1 -> Damping / brightness (loop-filter cutoff + decay)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║  KARPLUS  ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - pitch, quantized (BTN held: wet/dry mix)
      ║   PITCH   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - damping / decay (BTN held: excite colour)
      ║   DAMP    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - string energy (blinks mode ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: mode; long: pluck; hold+POT: shift
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - pluck trigger (internal exciter)
      ║ (o)   (o) ║   IN2 (GPIO0) - damp gate (HIGH = palm-mute the string)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial karplus firmware (maddie synths original, shared KarplusCore)

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
#include <EEPROM.h>       // persisted wet/dry + colour + mode
#include <Mod2Common.h>   // Shared MOD2 pin map, PWM-audio setup and helpers
#include <KarplusCore.h>  // Shared karplus DSP (also used by rack-plugins/src/mod2-karplus.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // below = mode cycle, above = pluck
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  float colour;
  uint8_t mode;
};
constexpr uint32_t SETTINGS_MAGIC = 0x4B504C31;  // "KPL1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
// One delay segment: a full period at the lowest pitch (~1.3 KB in SRAM).
static int16_t karplusArena[(uint32_t)(mod2::AUDIO_FS / sc::kKarplusBaseHz) + 8];
sc::KarplusCore karplus;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-resonator). */
volatile float   g_pitchHz  = 220.0f;
volatile float   g_damping  = 0.5f;
volatile float   g_colour   = 0.5f;
volatile uint8_t g_mode     = sc::KARPLUS_PLUCK;
volatile float   g_wet      = 1.0f;
volatile bool    g_pluck    = false;  // loop -> ISR pluck request (button)
volatile int16_t g_ledForce = -1;     // >=0: loop-driven blink level, -1: energy

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

  /* --- IN1: pluck trigger (sample-accurate edge detect) -------------- */
  const bool in1 = digitalRead(mod2::IN1_PIN) == HIGH;
  if ((in1 && !isrLastIn1) || g_pluck) {
    karplus.pluck();
    g_pluck = false;
  }
  isrLastIn1 = in1;

  /* --- pluck / bow the string ---------------------------------------- */
  karplus.pitchHz  = g_pitchHz;
  karplus.damping  = g_damping;
  karplus.colour   = g_colour;
  karplus.mode     = g_mode;
  karplus.wet      = g_wet;
  karplus.dampGate = digitalRead(mod2::IN2_PIN) == HIGH;
  float out = karplus.process(in, 1.0f / mod2::AUDIO_FS);

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: string energy, unless loop() is blinking a mode ID -------- */
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(karplus.ledLevel() * mod2::PWM_FS));

  pwm_clear_irq(sliceIRQ);
}

/* =======================================================================
 *  Guarded pot read — the ISR reads A2 through the same ADC mux (see the
 *  mod2-resonator note); blocking the IRQ for the ~2 us conversion just
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
  pinMode(A0, INPUT);                       // pitch / wet-dry (shifted)
  pinMode(A1, INPUT);                       // damping / colour (shifted)
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // pluck trigger
  pinMode(mod2::IN2_PIN, INPUT);            // damp gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // mode / pluck / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_colour = sc::clampf(s.colour, 0.0f, 1.0f);
    g_mode = s.mode < sc::KARPLUS_MODE_COUNT ? s.mode : sc::KARPLUS_PLUCK;
  }

  karplus.init(karplusArena, sizeof(karplusArena) / sizeof(karplusArena[0]));

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (mode / pluck / dual shift layer), flash saves
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

  /* --- button: short = mode, long = pluck, hold + POT1/POT2 = shift --- */
  static bool lastPressed = false;
  static bool adjustingWet = false;     // shift layer: POT1 -> wet/dry
  static bool adjustingColour = false;  // shift layer: POT2 -> excite colour
  static bool plucked = false;
  static uint32_t pressStartMs = 0;
  static float pot1AtPress = 0.0f, pot2AtPress = 0.0f;
  static mod2::PickupParam pitchPickup, dampPickup;
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
    adjustingColour = false;
    plucked = false;
  }
  if (pressed && !adjustingWet && fabsf(pot1 - pot1AtPress) > POT_MOVE_THRESHOLD)
    adjustingWet = true;   // shift layer engaged; short/long actions suppressed
  if (pressed && !adjustingColour && fabsf(pot2 - pot2AtPress) > POT_MOVE_THRESHOLD)
    adjustingColour = true;
  if (pressed && adjustingWet) {
    g_wet = pot1;
    dirty = true;
    lastChangeMs = now;
  }
  if (pressed && adjustingColour) {
    g_colour = pot2;
    dirty = true;
    lastChangeMs = now;
  }
  /* Long press = pluck, fired once the hold crosses the threshold (the
     same long-press convention as the resonator's strike).              */
  if (pressed && !adjustingWet && !adjustingColour && !plucked &&
      now - pressStartMs >= SHORT_PRESS_MS) {
    plucked = true;
    g_pluck = true;
  }
  if (!pressed && lastPressed) {  // release edge
    if (adjustingWet) {
      /* POT1 was borrowed for wet/dry: freeze the pitch until the pot is
         turned back across where it sat before the press.               */
      pitchPickup.targetValue = pot1AtPress;
      pitchPickup.lastPotValue = pot1;
      pitchPickup.pickupActive = true;
    }
    if (adjustingColour) {
      dampPickup.targetValue = pot2AtPress;
      dampPickup.lastPotValue = pot2;
      dampPickup.pickupActive = true;
    }
    if (!adjustingWet && !adjustingColour && !plucked &&
        now - pressStartMs < SHORT_PRESS_MS) {
      g_mode = (g_mode + 1) % sc::KARPLUS_MODE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_mode + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT1 -> pitch, POT2 -> damping (unless shifted / pickup) ------- */
  if (!pressed) {
    if (mod2::checkPickup(pitchPickup, pot1))
      g_pitchHz = sc::karplusPitchHz(pot1);
    pitchPickup.lastPotValue = pot1;
    if (mod2::checkPickup(dampPickup, pot2))
      g_damping = pot2;
    dampPickup.lastPotValue = pot2;
  }

  /* --- LED blink code for the newly selected mode --------------------- */
  if (blinkPhasesLeft > 0) {
    if (now >= blinkNextMs) {
      blinkPhasesLeft--;
      g_ledForce = (blinkPhasesLeft & 1) ? (int16_t)mod2::PWM_FS : 0;
      blinkNextMs = now + 120;
      if (blinkPhasesLeft == 0)
        g_ledForce = -1;  // back to following the string energy
    }
  }

  /* --- debounced flash save (commit stalls the audio ISR for a few ms,
         so only save once the panel has settled) ----------------------- */
  if (dirty && !pressed && now - lastChangeMs > SAVE_DELAY_MS) {
    Settings s = {SETTINGS_MAGIC, g_wet, g_colour, g_mode};
    EEPROM.put(0, s);
    EEPROM.commit();
    dirty = false;
  }
}
