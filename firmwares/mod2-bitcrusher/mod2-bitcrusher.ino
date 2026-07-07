/* BITCRUSHER

Description:
Bit-depth & sample-rate reduction effect — the first MOD2 FX firmware, and the
validation vehicle for the audio-input path (the CV jack sampled by the RP2350
ADC at the audio rate, see firmwares/mod2-fx/README.md). A phase-accumulator
sample-and-hold reduces the effective sample rate (36.6 kHz down to ~200 Hz,
deliberately unfiltered — aliasing is the point) and quantizes to a continuous
1..16 bit depth. Three quantizer styles cycled by short button presses:
truncate / TPDF dither / AND-mask. Hold BUTTON + turn POT1 for wet/dry mix
(persisted to flash along with the quantizer style). An IN1 clock overrides the
internal crush rate — patch audio-rate pulses for crush-rate FM.
DSP lives in the shared sc::BitcrusherCore (also used by the VCV Rack port).

Key Variables:
  A0 -> Crush rate (36.6 kHz -> ~200 Hz, exponential taper)
  A1 -> Bit depth (16 -> 1 bits, continuous)
  A2 -> AUDIO INPUT (POT3 is therefore unavailable)

      ╔═══════════╗
      ║ BITCRUSH  ║
      ║    fx     ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - crush rate (BTN held: wet/dry mix)
      ║   RATE    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - bit depth
      ║   BITS    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - unavailable (pin shared with audio in)
      ║    ---    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - crushed output level (blinks style ID)
      ║   (BTN)   ║   BTN (GPIO6) - short: quantizer style; hold+POT1: wet/dry
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - external crush clock (overrides POT1 rate)
      ║ (o)   (o) ║   IN2 (GPIO0) - bypass gate (HIGH = dry)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - AUDIO INPUT (~36.6 kHz ADC sampling)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial bitcrusher firmware (maddie synths original, shared BitcrusherCore)

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
#include <EEPROM.h>          // persisted wet/dry + quantizer style
#include <Mod2Common.h>      // Shared MOD2 pin map, PWM-audio setup and helpers
#include <BitcrusherCore.h>  // Shared crush DSP (also used by rack-plugins/src/mod2-bitcrusher.cpp)

/* ============================== constants ============================== */
constexpr float POT_MOVE_THRESHOLD = 0.04f;   // pot travel that counts as "moved"
constexpr uint32_t SHORT_PRESS_MS  = 600;     // max length of a style-cycle press
constexpr uint32_t SAVE_DELAY_MS   = 1500;    // settle time before a flash save
// IN1 pulses within this window put the crush clock in external mode (~0.3 s).
constexpr uint32_t EXT_CLOCK_WINDOW = (uint32_t)(0.3f * mod2::AUDIO_FS);

/* ------------------------- persisted settings ------------------------- */
struct Settings {
  uint32_t magic;
  float wet;
  uint8_t mode;
};
constexpr uint32_t SETTINGS_MAGIC = 0x42435231;  // "BCR1"

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR
uint sliceLed;    // PWM slice for LED brightness

/* -------------------- shared DSP core --------------------------------- */
sc::BitcrusherCore crusher;
sc::DcBlocker dcBlock;  // removes the ADC's unipolar bias from the audio in

/* Volatile shadows: loop() writes, ISR reads (same pattern as mod2-vco). */
volatile float   g_rateHz   = mod2::AUDIO_FS;
volatile float   g_bits     = 16.0f;
volatile uint8_t g_mode     = sc::BITCRUSH_TRUNCATE;
volatile float   g_wet      = 1.0f;
volatile int16_t g_ledForce = -1;  // >=0: loop-driven blink level, -1: follow audio

/* ISR-only state. */
static bool     isrLastIn1       = false;
static uint32_t samplesSinceTick = EXT_CLOCK_WINDOW;  // start in internal mode
static float    ledFollow        = 0.0f;

/* =======================================================================
 *  PWM interrupt service routine (~36.6 kHz)
 * ==================================================================== */
void __isr onPwmWrap()
{
  /* --- audio input: sample the CV jack at the audio rate ------------- */
  /* 10-bit unipolar ADC -> -1..+1, then a one-pole DC block strips the
     input network's bias (per the mod2-fx audio-in plan).               */
  const int raw = analogRead(mod2::CV_PIN);
  const float in = dcBlock.process(raw * (2.0f / 1023.0f) - 1.0f);

  /* --- IN1: external crush clock (sample-accurate edge detect) ------- */
  const bool in1 = digitalRead(mod2::IN1_PIN) == HIGH;
  const bool tick = in1 && !isrLastIn1;
  isrLastIn1 = in1;
  if (tick)
    samplesSinceTick = 0;
  else if (samplesSinceTick < EXT_CLOCK_WINDOW)
    samplesSinceTick++;
  const bool useExt = samplesSinceTick < EXT_CLOCK_WINDOW;

  /* --- crush ---------------------------------------------------------- */
  crusher.rateHz = g_rateHz;
  crusher.bits   = g_bits;
  crusher.mode   = g_mode;
  crusher.wet    = g_wet;
  float out = crusher.process(in, 1.0f / mod2::AUDIO_FS, useExt, tick);

  /* --- IN2: bypass gate (HIGH = pass the dry input) ------------------- */
  if (digitalRead(mod2::IN2_PIN) == HIGH)
    out = in;

  out = sc::clampf(out, -1.0f, 1.0f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B,
                     (uint16_t)((out + 1.0f) * mod2::PWM_MID + 0.5f));

  /* --- LED: smoothed |output|, unless loop() is blinking a style ID --- */
  ledFollow += (fabsf(out) - ledFollow) * 0.002f;
  const int16_t force = g_ledForce;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B,
                     force >= 0 ? (uint16_t)force
                                : (uint16_t)(ledFollow * mod2::PWM_FS));

  pwm_clear_irq(sliceIRQ);
}

/* =======================================================================
 *  Guarded pot read — the ISR reads A2 through the same ADC mux, so a
 *  loop-side analogRead could otherwise be interrupted between its channel
 *  select and conversion and return the wrong pin's value. Blocking the
 *  IRQ for the ~2 us conversion just delays one audio sample slightly.
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
  pinMode(A0, INPUT);                       // crush rate / wet-dry (shifted)
  pinMode(A1, INPUT);                       // bit depth
  pinMode(A2, INPUT);                       // audio input
  pinMode(mod2::IN1_PIN, INPUT);            // external crush clock
  pinMode(mod2::IN2_PIN, INPUT);            // bypass gate
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // style / shift button
  analogReadResolution(10);

  /* --- persisted settings -------------------------------------------- */
  EEPROM.begin(64);
  Settings s;
  EEPROM.get(0, s);
  if (s.magic == SETTINGS_MAGIC) {
    g_wet = sc::clampf(s.wet, 0.0f, 1.0f);
    g_mode = s.mode < sc::BITCRUSH_MODE_COUNT ? s.mode : sc::BITCRUSH_TRUNCATE;
  }

  /* --- LED PWM + audio PWM + ~36.6 kHz wrap-IRQ (shared setup) ------- */
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}

/* =======================================================================
 *  Main loop — pots, button (style / wet-dry shift layer), flash saves
 * ==================================================================== */
void loop()
{
  /* --- pots, one-pole smoothed (the crush itself must stay stepped) -- */
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

  g_bits = sc::bitcrusherBits(pot2);

  /* --- button: short press = quantizer style, hold + POT1 = wet/dry -- */
  static bool lastPressed = false;
  static bool adjustingWet = false;
  static uint32_t pressStartMs = 0;
  static float potAtPress = 0.0f;
  static mod2::PickupParam ratePickup;
  static bool dirty = false;
  static uint32_t lastChangeMs = 0;

  const bool pressed = digitalRead(mod2::BUTTON_PIN) == LOW;
  const uint32_t now = millis();

  /* blink state (style-ID feedback: mode+1 flashes) */
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
      g_mode = (g_mode + 1) % sc::BITCRUSH_MODE_COUNT;
      dirty = true;
      lastChangeMs = now;
      blinkPhasesLeft = (uint8_t)((g_mode + 1) * 2);  // on/off phases
      blinkNextMs = now;
    }
  }
  lastPressed = pressed;

  /* --- POT1 -> crush rate (unless shifted / waiting for pickup) ------ */
  if (!pressed) {
    if (mod2::checkPickup(ratePickup, pot1))
      g_rateHz = sc::bitcrusherRateHz(pot1, mod2::AUDIO_FS);
    ratePickup.lastPotValue = pot1;
  }

  /* --- LED blink code for the newly selected quantizer style --------- */
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
