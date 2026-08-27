/* Quant — note quantizer

Description:
Snaps an incoming pitch CV to the nearest note at or below it in one of 30
scales and chords, and puts it back out as 1 V/oct. Ten scales in each of three
banks: the chromatic scale and the seven-tone modes, a bank of pentatonics and
symmetrical scales, and a bank of chords. TUNE sums into the pitch input, so it
transposes, and the result is always in scale whatever you dial in. The output
follows the input continuously — there is no mode to pick and nothing to clock.
Original Quant firmware by Sean Luke, ported from the AE Modular GRAINS module.

Key Variables:
  A0 -> Transpose, summed with the F1 pitch CV
  A1 -> Scale bank (modes / other scales / chords)
  A2 -> Scale within the bank (10 per bank)

      ╔═══════════╗
      ║   QUANT   ║
      ║ quantizer ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   TUNE    — transpose, sums with F1
      ║   TUNE    ║
      ║           ║
      ║   (A1)    ║   BANK    — modes / scales / chords
      ║   BANK    ║
      ║           ║
      ║   (A2)    ║   SCALE   — scale within the bank
      ║   SCALE   ║
      ║           ║
      ║    [·]    ║   LED (D3) — flash on note change
      ║   (BTN)   ║   BTN (D4) — unused
      ║           ║
      ╠═══════════╣
      ║ F1     F2 ║   F1 (A3)  IN  — Pitch CV (sums with TUNE)
      ║ (o)   (o) ║   F2 (D9)      — unused
      ║           ║
      ║ F3     F4 ║   F3 (D10) OUT — Quantized pitch, 1 V/oct
      ║ (o)   (o) ║   F4 (D11) OUT — Note-change trigger
      ║           ║
      ╚═══════════╝

Deliberate changes from the original:
  - Upstream's positions[] pitch tables are gone and the output is true 1 V/oct.
    Those five tables (OUTPUT_555 / _VCO / _UBUFFER / _4BUFFER / _2OSCD, chosen
    by a #define at compile time) exist because GRAINS' unbuffered output sags
    by however much current the oscillator downstream pulls, so every target
    needed its own hand-measured note-to-PWM curve. MOD1 buffers its output, so
    there is nothing to correct for and no reason to make the user recompile.
    Same class of change as SquareVCO's voctMap in PORTING.md.
  - For the same reason POT1 changes job. Upstream spent it on an input tracking
    trim, because GRAINS' inputs read 1.3 V/oct and drift as the board warms up.
    Here it sums into the pitch CV the way every other MOD1 sketch sums a pot
    with its jack, which makes it a transpose — and one that lands in scale,
    since it goes in before the quantizer.
  - Upstream capped the output at note 47 because Mozzi's PWM ran out of range
    there. The full 0..59 the tracker can produce is emitted instead: a hardware
    ceiling, not a musical one.
  - The output is 12-bit PWM, not 8-bit. See setupPitchPwm() for why.
  - The note-change trigger on F4 and the LED flash that goes with it are
    additions; upstream quantizes silently and drives nothing but the pitch out.
    They are the only added behaviour here. Everything the quantizer does to the
    signal is upstream's.
  - F2 and the button are left unused, the way GRAINS leaves IN 2, IN 3, AUDIO
    IN and DIGITAL OUT unused for this firmware. There is no mode to put on the
    button: upstream's only compile-time option was the output calibration table
    that MOD1 does not need, so a button would have had to invent a feature
    rather than expose one.

Version History:
  - 1.0 Quant firmware by Sean Luke for AE Modular GRAINS
  - 1.1 Ported to HAGIWO MOD1; engine extracted to sc::QuantEngine, output
        converted from the calibrated Mozzi tables to 12-bit 1 V/oct PWM,
        note-change trigger added

License:
Apache License 2.0. This is a port of third-party code: the original Quant
firmware is Copyright 2023 Sean Luke (sean@cs.gmu.edu), from the GRAINS
project (github.com/eclab/grains), Apache 2.0. Apache requires the license
notice to travel with the code and modified files to carry prominent notice
of changes — the notice lives beside this sketch as LICENSE.md, and the
"Deliberate changes from the original" list above is the notice of changes.
Keep both.

Hardware:
HAGIWO MOD1
*/
#include <Arduino.h>
#include <Mod1Common.h>
#include <QuantCore.h>

// Timer1 TOP. 4095 gives 12 bits at 16 MHz / 4096 = 3.906 kHz.
constexpr uint16_t kPitchPwmTop = 4095;

// The output's full-scale span in volts, i.e. what OCR1B == kPitchPwmTop is
// worth at the jack. This is the single number that sets the module's tracking:
// if a tuner says the octaves are wide or narrow, trim it here and nowhere
// else. 5 V matches the board's own conventions: the ADC reference is 5 V, the
// CV inputs read 0..5 V, and MOD1 modules are designed to self-patch (see
// mod1-eg's EoC loop), which only works if outs span what ins can read. (The
// Rack ports scale some outputs by 10 — that is the VCV voltage standard, not
// a statement about the hardware jack.)
constexpr float kOutputFullScaleVolts = 5.0f;
constexpr float kCountsPerVolt = (float)(kPitchPwmTop + 1) / kOutputFullScaleVolts;

// Note-change trigger and the LED flash that rides along with it.
constexpr unsigned long kTrigMs = 10;
constexpr uint8_t kLedFlash = 200;  // brightness of the note-change flash

sc::QuantEngine quant;

unsigned long lastMicros = 0;
unsigned long trigOffMs = 0;
bool trigActive = false;

// Timer1 in fast-PWM mode 14 (TOP = ICR1) on OC1B / D10 / F3, prescaler 1.
//
// The 8-bit 62.5 kHz setup the other MOD1 sketches share puts 256 steps across
// the output's 10 V span — 2.1 steps per semitone, so the output could land up
// to 23 cents from the note it means, on a signal whose whole job is to be in
// tune. 12 bits give 34.1 steps per semitone, worst-case 1.5 cents, and the
// price is a carrier 16x lower for the output filter to deal with. For a CV
// that changes at note rate that is the right side of the trade.
//
// Only COM1B1 is enabled, so OC1A / D9 / F2 is left alone as an ordinary pin
// rather than being driven by a timer this sketch is using for something else.
// Timer2 is left to setupFastPwmEgStyle() for the LED, and Timer0
// (millis/micros) is untouched.
static void setupPitchPwm() {
  TCCR1A = (1 << WGM11) | (1 << COM1B1);
  TCCR1B = (1 << WGM13) | (1 << WGM12) | (1 << CS10);
  ICR1 = kPitchPwmTop;
  OCR1B = 0;
}

static void writePitch(uint8_t note) {
  uint16_t counts = (uint16_t)(sc::quantNoteVolts(note) * kCountsPerVolt + 0.5f);
  if (counts > kPitchPwmTop) counts = kPitchPwmTop;
  OCR1B = counts;
}

// Pitch CV and transpose share one ADC domain: 0..1023 counts is 0..5 V is the
// engine's five-octave note range, exactly as upstream mapped IN 1.
static uint16_t readPitchAdc() {
  return (uint16_t)mod1::addClamp1023(analogRead(mod1::PIN_POT1),
                                      analogRead(mod1::PIN_CV1));
}

static uint8_t readScale() {
  return sc::quantSelectScale(analogRead(mod1::PIN_POT2) / 1023.0f,
                              analogRead(mod1::PIN_POT3) / 1023.0f);
}

void setup() {
  pinMode(mod1::PIN_F3, OUTPUT);  // quantized pitch out
  pinMode(mod1::PIN_F4, OUTPUT);  // note-change trigger out
  pinMode(mod1::PIN_LED, OUTPUT);

  mod1::setupFastPwmEgStyle();  // Timer2 -> 62.5 kHz LED PWM on D3
  setupPitchPwm();              // Timer1 -> 12-bit pitch PWM on D10 (overrides)

  digitalWrite(mod1::PIN_F4, LOW);

  // Start settled on whatever is already at the input rather than sweeping up
  // from note 0 on power-up.
  quant.reset();
  quant.initPitch(readPitchAdc());
  quant.note = sc::quantizeNote(quant.trackPitch(readPitchAdc()), readScale());
  writePitch(quant.note);

  lastMicros = micros();
}

void loop() {
  const unsigned long nowMicros = micros();
  const unsigned long nowMillis = millis();
  const float dt = (float)(nowMicros - lastMicros) * 1e-6f;  // unsigned: wrap-safe
  lastMicros = nowMicros;

  if (quant.step(dt, readPitchAdc(), readScale())) {
    writePitch(quant.note);
    digitalWrite(mod1::PIN_F4, HIGH);
    trigOffMs = nowMillis + kTrigMs;
    trigActive = true;
  }

  if (trigActive && (long)(nowMillis - trigOffMs) >= 0) {
    digitalWrite(mod1::PIN_F4, LOW);
    trigActive = false;
  }

  // LED flashes with the trigger, so the panel shows note changes even when
  // nothing is patched into F4.
  analogWrite(mod1::PIN_LED, trigActive ? kLedFlash : 0);
}
