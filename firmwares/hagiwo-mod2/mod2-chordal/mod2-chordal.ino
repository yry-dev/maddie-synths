/* Chordal

Description:
Four-note chording oscillator. Every note of the chord is rendered twice — once
as a sine, once as a square, saw or triangle of the same pitch — and one knob
cross-fades the whole chord between the two. 24 chord voicings (intervals,
triads, sevenths, octave triads) and 8 inversions that walk the chord upward an
octave at a time.

Original Chordal firmware by Sean Luke, ported from the AE Modular GRAINS
module. Synthesis lives in the shared core
firmwares/shared/SynthCore/src/ChordalCore.h, which the VCV Rack port
(rack-plugins/src/mod2-chordal.cpp) also uses; this sketch keeps only hardware
I/O: pot reading, the button gesture, flash persistence and the PWM audio ISR.

Deliberate changes from the original:
  - The second waveform is a runtime choice, not a compile-time one. Upstream
    picked square, saw or triangle with a #define and you reflashed to change
    it; here the button cycles the three and the choice is saved to flash.
  - The chord tones are band-limited with PolyBLEP instead of Mozzi's
    MetaOscil bank of 17 pre-filtered wavetables. PolyBLEP is sample-rate
    independent; the wavetable set was baked for 16384 Hz and this module runs
    at ~36.6 kHz. Triangle stays naive (1/n^2 harmonics, negligible aliasing).
  - Pitch is 1 V/oct, not GRAINS' 1.3 V/oct. Upstream spent POT1 on a "Pitch CV
    Scaling" trim you had to keep re-adjusting as the GRAINS resistors warmed
    up; MOD2 needs no such trim, so POT1 is free and carries the chord select
    (upstream's POT3). The bottom of the pitch range is still C0 (32.7 Hz),
    upstream's 0 V note, and POT3 sweeps five octaves up from there.
  - Upstream's Pitch Tune (AUDIO IN) is gone: POT3 shares the CV jack's ADC on
    this hardware, so the pot IS the tune control when nothing is patched.
  - Inversion moves from a dedicated CV jack (upstream IN3) to two places MOD2
    does have: hold the button and turn POT1, or feed IN1 a trigger to step
    through 0..7. Upstream suggests driving inversion from an LFO to get
    arpeggios; the IN1 stepper is that idea in the gate-only world MOD2 offers.
  - Output runs at full scale. Upstream scaled its mix into Mozzi's +/-244 PWM
    headroom (~0.69 of full scale); the mean of the active notes is already
    bounded by 1, so no headroom is needed here.
  - Mix is a plain float cross-fade rather than upstream's 0..255 integer
    weights with the >>1 / div3 / >>2 note-count divisors. Same curve, no
    quantisation.

Key Variables:
  A0 -> Chord select (24) | Inversion (while the button is held)
  A1 -> Mix (sine <-> square/saw/tri)
  A2 -> Root pitch / V-oct (shared with CV)

      ╔═══════════╗
      ║  CHORDAL  ║
      ║ chord osc ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - chord | inversion (hold BTN)
      ║   CHORD   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - mix: sine <-> squ/saw/tri
      ║    MIX    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - root pitch / V-oct
      ║   PITCH   ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - wave mode / inversion
      ║   (BTN)   ║   BTN (GPIO6) - wave: squ, saw, tri
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - step inversion (0..7, wraps)
      ║ (o)   (o) ║   IN2 (GPIO0) - N/A
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - 1 V/oct root (shared POT3)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Chordal firmware by Sean Luke, for the AE Modular GRAINS (Mozzi)
  - 1.1 Ported to HAGIWO MOD2 for maddie synths; float DSP in the shared
        ChordalCore, runtime wave select, 1 V/oct pitch, flash persistence

License:
Apache License 2.0. This is a port of third-party code: the original Chordal
firmware is Copyright 2023 Sean Luke (sean@cs.gmu.edu), from the GRAINS
project (github.com/eclab/grains), Apache 2.0. Apache requires the license
notice to travel with the code and modified files to carry prominent notice
of changes — the notice lives beside this sketch as LICENSE.md, and the
"Deliberate changes from the original" list above is the notice of changes.
Keep both.

Hardware:
HAGIWO MOD2 (Seeed Xiao RP2350)
*/

#include <Arduino.h>
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include <math.h>
#include <EEPROM.h>      // RP2350 Arduino core maps on-board flash as EEPROM
#include <Mod2Common.h>  // Shared MOD2 pin map, PWM-audio setup and helpers
#include <ChordalCore.h>  // Shared Chordal engine (also used by the Rack port)

/* ============================== constants ============================== */
constexpr float AUDIO_FS = mod2::AUDIO_FS;  // ~36621 Hz wrap-IRQ sample rate
constexpr float DT = 1.0f / AUDIO_FS;

/* Root-pitch range on A2. Upstream plays C0 at 0 V; the MOD2 CV input spans
   about five octaves across the ADC with a negative slope (same constants
   mod2-vco calibrated), so the pot alone sweeps C0..C5 and a patched CV shifts
   it 1 V/oct on top. */
constexpr float ROOT_MIN_HZ = 32.703f;                  // C0
constexpr float VOCT_SPAN = 8.3f * (33.0f / 55.0f);     // ~4.98 octaves
const float TUNE_CAL = 0.992f;                          // 1.000 = no correction

/* Chord select must hold still before it commits: the raw pot sits right on a
   boundary often enough that ADC noise would flip chords on its own. Upstream
   used 10 consecutive control cycles at 128 Hz (~78 ms); this loop runs at
   ~1 kHz, so it counts to 80 for the same wall-clock feel. */
constexpr uint16_t CHORD_STABLE_LOOPS = 80;

/* Turning POT1 this far while the button is held means "edit inversion", not
   "short press to cycle the wave". */
constexpr float HOLD_POT_THRESHOLD = 0.06f;

/* Flash writes stall audio, so a change waits for the panel to go quiet. */
constexpr uint32_t SAVE_DEBOUNCE_MS = 1500;
constexpr uint8_t EEPROM_MAGIC = 0xC5;
constexpr int ADDR_MAGIC = 0;
constexpr int ADDR_WAVE = 1;
constexpr int ADDR_INVERSION = 2;

constexpr int POT_SMOOTH_SAMPLES = 4;

/* Debug logging over USB serial: raw ADCs, smoothed pots, the chord-select
   pipeline (candidate/pending/stable) and every engine parameter, four times a
   second. Turn off for release builds — the prints run on the panel core and
   cost nothing audible, but they are noise. */
#define DEBUG_POTS 1

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice carrying the audio output
uint sliceIRQ;    // PWM slice whose wrap fires the audio ISR
uint sliceLed;    // PWM slice driving the LED

mod2::PotSmoother<POT_SMOOTH_SAMPLES> pot1Smoother;
mod2::PotSmoother<POT_SMOOTH_SAMPLES> pot2Smoother;
mod2::PotSmoother<POT_SMOOTH_SAMPLES> pot3Smoother;

/* -------------------- shared DSP core --------------------------------- */
sc::ChordalVoice voice;

/* Volatile shadows: loop() writes, the ISR reads (the mod2-vco pattern). */
volatile float g_rootFreq = ROOT_MIN_HZ;
volatile float g_mix = 0.0f;
volatile uint8_t g_chord = 0;
volatile uint8_t g_inversion = 0;
volatile uint8_t g_wave = sc::kChordalSquare;

/* Set by the IN1 edge ISR, consumed by loop(). */
volatile bool g_stepInversion = false;

/* =======================================================================
 *  PWM interrupt service routine — one sample per wrap
 * ==================================================================== */
void __isr onPwmWrap() {
  voice.rootFreq = g_rootFreq;
  voice.chord = g_chord;
  voice.mix = g_mix;
  voice.inversion = g_inversion;
  voice.wave = g_wave;

  const float s = voice.process(DT);

  /* Map -1..+1 -> 0..1023 for the 10-bit PWM DAC. */
  const uint16_t pwmSample =
      (uint16_t)((sc::clampf(s, -1.0f, 1.0f) + 1.0f) * mod2::PWM_MID + 0.5f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, pwmSample);

  pwm_clear_irq(sliceIRQ);
}

/* IN1 rising edge: advance the inversion. Kept to a flag so the audio ISR is
   never held off by the loop-side work. */
void onInversionStep() { g_stepInversion = true; }

/* =======================================================================
 *  Setup
 * ==================================================================== */
void setup() {
#if DEBUG_POTS
  Serial.begin(115200);
#endif
  pinMode(A0, INPUT);  // chord / inversion
  pinMode(A1, INPUT);  // mix
  pinMode(A2, INPUT);  // root pitch, shared with the CV jack
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);
  pinMode(mod2::IN2_PIN, INPUT);

  /* --- restore the two saved settings ------------------------------- */
  EEPROM.begin(64);
  if (EEPROM.read(ADDR_MAGIC) == EEPROM_MAGIC) {
    const uint8_t w = EEPROM.read(ADDR_WAVE);
    const uint8_t inv = EEPROM.read(ADDR_INVERSION);
    if (w < sc::kChordalWaveCount) g_wave = w;
    if (inv < sc::kChordalNumInversions) g_inversion = inv;
  }

  /* --- audio PWM + ~36.6 kHz wrap-IRQ (shared) ---------------------- */
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
  sliceLed = mod2::initPwmOutput10bit(mod2::LED_PIN);

  /* --- IN1: inversion stepper --------------------------------------- */
  pinMode(mod2::IN1_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(mod2::IN1_PIN), onInversionStep, RISING);

  pot1Smoother.prime(A0);
  pot2Smoother.prime(A1);
  pot3Smoother.prime(A2);
}

/* =======================================================================
 *  Main loop — panel I/O only
 * ==================================================================== */
void loop() {
  const float p1 = pot1Smoother.read(A0);
  const float p2 = pot2Smoother.read(A1);
  const float p3 = pot3Smoother.read(A2);

  static bool prevBtn = HIGH;
  static float potAtPress = 0.0f;
  static bool editingInversion = false;
  static bool dirty = false;
  static uint32_t dirtyAt = 0;

  const bool btn = digitalRead(mod2::BUTTON_PIN);  // LOW = pressed

  /* --- button: short press cycles the wave, hold + POT1 sets inversion --- */
  if (prevBtn == HIGH && btn == LOW) {
    potAtPress = p1;
    editingInversion = false;
  } else if (btn == LOW) {
    if (fabsf(p1 - potAtPress) > HOLD_POT_THRESHOLD) editingInversion = true;
    if (editingInversion) {
      uint8_t inv = (uint8_t)(p1 * sc::kChordalNumInversions);
      if (inv >= sc::kChordalNumInversions) inv = sc::kChordalNumInversions - 1;
      if (inv != g_inversion) {
        g_inversion = inv;
        dirty = true;
        dirtyAt = millis();
      }
    }
  } else if (prevBtn == LOW) {  // released
    if (!editingInversion) {
      g_wave = (g_wave + 1) % sc::kChordalWaveCount;
      dirty = true;
      dirtyAt = millis();
    }
    editingInversion = false;
  }
  prevBtn = btn;

  /* --- IN1 edge: step the inversion, wrapping at 8 -------------------- */
  if (g_stepInversion) {
    g_stepInversion = false;
    g_inversion = (g_inversion + 1) % sc::kChordalNumInversions;
    dirty = true;
    dirtyAt = millis();
  }

  /* --- POT1: chord select, once the reading has settled ---------------- */
  static uint8_t pending = 0;
  static uint16_t stable = 0;
  uint8_t candidate = pending;  // remembered for the debug line below
  if (!(btn == LOW && editingInversion)) {
    candidate = (uint8_t)(p1 * sc::kChordalNumChords);
    if (candidate >= sc::kChordalNumChords) candidate = sc::kChordalNumChords - 1;

    if (candidate != pending) {
      pending = candidate;
      stable = 0;
    } else if (candidate != g_chord) {
      if (++stable >= CHORD_STABLE_LOOPS) g_chord = candidate;
    } else {
      stable = 0;
    }
  }

#if DEBUG_POTS
  /* Four lines a second: enough to watch a pot sweep, slow enough to read. */
  static uint32_t lastDebugMs = 0;
  if (millis() - lastDebugMs >= 250) {
    lastDebugMs = millis();
    Serial.print("adc A0=");
    Serial.print(analogRead(A0));
    Serial.print(" A1=");
    Serial.print(analogRead(A1));
    Serial.print(" A2=");
    Serial.print(analogRead(A2));
    Serial.print(" | p1=");
    Serial.print(p1, 3);
    Serial.print(" p2=");
    Serial.print(p2, 3);
    Serial.print(" p3=");
    Serial.print(p3, 3);
    Serial.print(" | cand=");
    Serial.print(candidate);
    Serial.print(" pend=");
    Serial.print(pending);
    Serial.print(" stable=");
    Serial.print(stable);
    Serial.print(" chord=");
    Serial.print(g_chord);
    Serial.print(" | mix=");
    Serial.print(g_mix, 3);
    Serial.print(" inv=");
    Serial.print(g_inversion);
    Serial.print(" wave=");
    Serial.print(g_wave);
    Serial.print(" root=");
    Serial.print(g_rootFreq, 1);
    Serial.print("Hz btn=");
    Serial.println(btn == LOW ? "DOWN" : "up");
  }
#endif

  /* --- POT2: sine <-> square/saw/tri mix ------------------------------- */
  g_mix = p2;

  /* --- POT3 / CV: root pitch. A2 is negative-slope (hardware wiring), so
         turning the pot clockwise — or raising the CV — raises the pitch. --- */
  g_rootFreq = ROOT_MIN_HZ * powf(2.0f, (1.0f - p3) * VOCT_SPAN * TUNE_CAL);

  /* --- LED: inversion while editing, otherwise the wave mode ----------- */
  const float ledLevel =
      (btn == LOW && editingInversion)
          ? (float)g_inversion / (float)(sc::kChordalNumInversions - 1)
          : (float)(g_wave + 1) / (float)sc::kChordalWaveCount;
  pwm_set_chan_level(sliceLed, PWM_CHAN_B, (uint16_t)(ledLevel * mod2::PWM_FS));

  /* --- flash: write once the panel has been quiet for a moment --------- */
  if (dirty && (millis() - dirtyAt) > SAVE_DEBOUNCE_MS) {
    EEPROM.write(ADDR_MAGIC, EEPROM_MAGIC);
    EEPROM.write(ADDR_WAVE, g_wave);
    EEPROM.write(ADDR_INVERSION, g_inversion);
    EEPROM.commit();
    dirty = false;
  }

  delay(1);  // ~1 kHz panel scan; CHORD_STABLE_LOOPS is counted in these ticks
}
