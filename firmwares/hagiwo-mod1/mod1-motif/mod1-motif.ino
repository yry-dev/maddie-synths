/* Motif

Description:
A 16-step random melody generator — think of it as Topograf for notes. Rather
than rolling dice each step, Motif hands you a three-dimensional space of short
melodies you can navigate and return to: PATTERN picks one of 32 base shapes
(four 4-step groups rising or falling, the lower 16 in minor and the upper 16 in
major), RANDOM picks one of 32 fixed deviation phrases layered on top, and
VARIANCE sets how far that deviation is allowed to pull the melody off its base
shape. Clock F1 to walk the 16 steps; F2 (or the button) resets to step 0, where
the starting note is re-derived from the pattern. Pitch leaves F3 as 1V/oct CV
and F4 fires a trigger on every note so a fast envelope stays in step with it.

Original Motif firmware by Sean Luke, ported from the AE Modular GRAINS module.

Deliberate changes from the original:
  - GRAINS runs this under Mozzi and spends its four analog inputs on Variance
    CV, Random CV, Transpose CV and Reset. MOD1 has four jacks total, so they go
    to the four things a melody generator cannot work without: clock in, reset
    in, pitch out, trigger out. Variance and Random lose their CV inputs and
    Transpose is dropped entirely — upstream already offered Transpose and its
    CLOCK_OUT option as an either/or (its `#define CLOCK_OUT`), noted that the
    transpose input was not volt/octave, and warned that transposing pushes the
    3.5-octave range out of bounds. We take the clock-out branch permanently.
  - The button doubles the reset input, following mod1-euclidean.
  - Pitch out is true 1V/oct. Upstream shipped five hand-measured calibration
    tables (OUTPUT_555, OUTPUT_VCO, ...) because a GRAINS' output voltage sags
    by however much current the oscillator downstream pulls; those numbers
    describe specific AE Modular modules and mean nothing here. MOD1 drives a
    buffered 0–5 V output, so a semitone is simply 1/12 V.
  - Pitch out uses Timer1 in 10-bit fast PWM (TOP = ICR1 = 1023, 15.625 kHz)
    rather than the 8-bit/62.5 kHz mode the rest of the MOD1 firmwares use for
    CV. Over a 0–5 V span 8 bits is only ~4 counts per semitone (~23 cents of
    quantisation); 10 bits brings that to ~17 counts (~6 cents). The carrier is
    still four decades above the output filter's corner, so the extra ripple
    does not reach the pitch.
  - The 8192-byte PROGMEM deviation table is regenerated at startup from the
    xorshift64 generator upstream documents at the foot of its sketch, saving
    ~8 KB of flash on a 30 KB part. See MotifCore.h for the byte-for-byte
    check against the published table.
  - Upstream's two scale tables hold 25 entries but its bounds check let the
    note index reach 25, so the top of its own range read one element past the
    end of the table. The scales are seven degrees repeated, so we compute them
    rather than tabulate them, which makes index 25 well defined and keeps the
    range exactly as upstream intended. Checked over the whole knob space
    (1,048,576 notes): identical to the original, minus the stray read.
  - The clock input is polled every loop instead of at Mozzi's 256 Hz control
    rate, so a note lands within microseconds of the edge rather than up to
    4 ms after it. Pot smoothing still runs on a 4 ms tick to match the
    original's response. This is also why the trigger on F4 fires immediately:
    upstream delayed its clock-out ~12 ms to let Mozzi catch up on the pitch,
    and there is nothing to wait for here.

Key Variables:
  A0 -> Variance (how far the melody deviates from the pattern)
  A1 -> Random (which deviation phrase)
  A2 -> Pattern (32 base shapes: 16 minor, then 16 major)

      ╔═══════════╗
      ║   MOTIF   ║
      ║  melody   ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   VARY    — deviation amount
      ║   VARY    ║
      ║           ║
      ║   (A1)    ║   RAND    — deviation phrase
      ║   RAND    ║
      ║           ║
      ║   (A2)    ║   PATT    — base pattern (minor <-> major)
      ║   PATT    ║
      ║           ║
      ║    [·]    ║   LED (D3) — pitch height
      ║   (BTN)   ║   BTN (D4) — reset to step 1
      ║           ║
      ╠═══════════╣
      ║ F1     F2 ║   F1 (D17) IN  — Clock
      ║ (o)   (o) ║   F2 (D9)  IN  — Reset
      ║           ║
      ║ F3     F4 ║   F3 (D10) OUT — Pitch (1V/oct CV)
      ║ (o)   (o) ║   F4 (D11) OUT — Trigger (per note)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Motif firmware by Sean Luke (GRAINS)
  - 1.1 Ported to HAGIWO MOD1; algorithm extracted to MotifCore.h (shared with
        the VCV Rack port), deviation table regenerated instead of stored,
        pitch out rescaled to 1V/oct at 10-bit PWM

License:
Apache License 2.0. This is a port of third-party code: the original Motif
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
#include <MotifCore.h>  // shared melody engine (also used by the VCV Rack port)

const int clockInPin  = mod1::PIN_F1;      // clock — advances one step
const int resetInPin  = mod1::PIN_F2;      // reset — back to step 0
const int pitchOutPin = mod1::PIN_F3;      // pitch CV (OC1B, 10-bit PWM)
const int trigOutPin  = mod1::PIN_F4;      // trigger, one per note
const int ledPin      = mod1::PIN_LED;
const int buttonPin   = mod1::PIN_BUTTON;  // second reset (active LOW)
const int variancePin = mod1::PIN_POT1;
const int randomPin   = mod1::PIN_POT2;
const int patternPin  = mod1::PIN_POT3;

// Timer1 fast PWM, TOP = ICR1: 1023 gives 10-bit resolution at 16 MHz / 1024 =
// 15.625 kHz on OC1B. See the header for why pitch gets 10 bits and not 8.
const uint16_t pwmTop = 1023;

// MOD1's CV output spans 0–5 V, so full scale is five octaves of 1V/oct.
const float outputVoltsFullScale = 5.0f;

// Trigger width, matching the other MOD1 sequencers.
const unsigned long triggerTime = 10;

// Pot smoothing runs on this tick to reproduce Mozzi's 256 Hz control rate
// (3.9 ms); the clock input is read every loop regardless.
const unsigned long controlIntervalMs = 4;

sc::MotifEngine engine;

mod1::EdgeInput clockEdge(LOW);
mod1::EdgeInput resetEdge(LOW);
mod1::DebouncedInput buttonDebounce(50, HIGH);

// Upstream's one-pole pot smoother: x = (x * 3 + adc) >> 2, in ADC counts.
uint16_t varSmooth = 0;
uint16_t ranSmooth = 0;
uint16_t patSmooth = 0;

unsigned long lastControlMillis = 0;
unsigned long triggerStartMillis = 0;
bool isTriggering = false;

void writePitch(int8_t semitone) {
  const float volts = sc::motifSemitoneToVolts(semitone);
  const float duty = volts / outputVoltsFullScale * (float)pwmTop;
  OCR1B = (uint16_t)(duty + 0.5f);
}

void setup() {
  pinMode(clockInPin, INPUT);
  pinMode(resetInPin, INPUT);
  pinMode(buttonPin,  INPUT_PULLUP);
  pinMode(pitchOutPin, OUTPUT);
  pinMode(trigOutPin,  OUTPUT);
  pinMode(ledPin,      OUTPUT);
  digitalWrite(trigOutPin, LOW);

  // Shared setup gets Timer2 (the LED on D3) onto 62.5 kHz fast PWM...
  mod1::setupFastPwmEgStyle();
  // ...then Timer1 is re-pointed at OC1B in mode 14 so pitch gets 10 bits.
  // F2/D9 is OC1A and stays a plain digital input: COM1A1 is left clear.
  TCCR1A = _BV(WGM11) | _BV(COM1B1);
  TCCR1B = _BV(WGM13) | _BV(WGM12) | _BV(CS10);
  ICR1 = pwmTop;
  OCR1B = 0;

  // Prime the smoothers so the first clock uses the knobs as they are set
  // rather than walking up from zero.
  varSmooth = analogRead(variancePin);
  ranSmooth = analogRead(randomPin);
  patSmooth = analogRead(patternPin);
}

void loop() {
  const unsigned long now = millis();

  if (now - lastControlMillis >= controlIntervalMs) {
    lastControlMillis = now;
    varSmooth = (uint16_t)((varSmooth * 3 + analogRead(variancePin)) >> 2);
    ranSmooth = (uint16_t)((ranSmooth * 3 + analogRead(randomPin))   >> 2);
    patSmooth = (uint16_t)((patSmooth * 3 + analogRead(patternPin))  >> 2);
  }

  // Reset from either the jack or the button. Like upstream, this only moves
  // the position — the note is re-picked when the next clock arrives.
  resetEdge.update((uint8_t)digitalRead(resetInPin));
  buttonDebounce.update((uint8_t)digitalRead(buttonPin), now);
  if (resetEdge.rose() || buttonDebounce.fell()) {
    engine.resetPosition();
  }

  clockEdge.update((uint8_t)digitalRead(clockInPin));
  if (clockEdge.rose()) {
    const sc::MotifParams p = sc::motifMapParams(
        varSmooth / 1023.0f,
        ranSmooth / 1023.0f,
        patSmooth / 1023.0f);

    const int8_t semitone = engine.step(p);
    writePitch(semitone);

    // LED tracks pitch height, so the melody's shape is visible on the panel.
    analogWrite(ledPin, (int)(semitone * 255L / sc::kMotifSemitoneMax));

    digitalWrite(trigOutPin, HIGH);
    triggerStartMillis = now;
    isTriggering = true;
  }

  if (isTriggering && now - triggerStartMillis >= triggerTime) {
    digitalWrite(trigOutPin, LOW);
    isTriggering = false;
  }
}
