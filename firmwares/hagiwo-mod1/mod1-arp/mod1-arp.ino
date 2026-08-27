/* Arp — clocked arpeggiator with pitch CV out

Description:
Pick a CHORD and a STYLE and Arp plays the chord one note at a time, advancing
on every clock pulse. The whole figure is transposed by the PITCH control, so a
sequencer driving the pitch CV moves the arpeggio through a progression. 16
chords (triads, sevenths, pentatonics, dim7, and the major/minor scales) and 12
styles (up / down / up-down / up-down-plus / random / random-walk, each over one
octave of the chord or two). The button folds in the original's compile-time
INVERSION and its reset input.

Pitch leaves on F3 as a 10-bit PWM CV at true 1 V/oct over five octaves, and F4
fires a gate a hair after the note changes so an envelope reads the new pitch
rather than the old one.

Original Arp firmware by Sean Luke, ported from the AE Modular GRAINS module.

Key Variables:
  A0 -> Pitch (root of the arpeggio, 0-35 semitones; sums the F2 CV jack)
  A1 -> Chord (16 chords)
  A2 -> Style (12 arpeggio patterns)

      ╔═══════════╗
      ║    ARP    ║
      ║arpeggiator║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   PITCH   — root pitch, 3 octaves
      ║   PITCH   ║             (sums with the F2 CV in)
      ║           ║
      ║   (A1)    ║   CHORD   — 16 chords, major → minor → dim7/scales
      ║   CHORD   ║
      ║           ║
      ║   (A2)    ║   STYLE   — 12 patterns; lower half is one octave
      ║   STYLE   ║             of the chord, upper half is two
      ║           ║
      ║    [·]    ║   LED (D3) — pitch CV level
      ║   (BTN)   ║   BTN (D4) — tap: inversion 0-3 (saved to EEPROM)
      ║           ║             hold: restart the arpeggio
      ╠═══════════╣
      ║ F1     F2 ║   F1 (A3/D17) IN  — CLOCK (advances on the rising edge)
      ║ (o)   (o) ║   F2 (A4)     IN  — Pitch CV (sums with A0)
      ║           ║
      ║ F3     F4 ║   F3 (D10)    OUT — Pitch CV, 1 V/oct (10-bit PWM)
      ║ (o)   (o) ║   F4 (D11)    OUT — Gate (follows the clock, delayed 2 ms)
      ║           ║
      ╚═══════════╝

  Styles in POT3 order:
    0 up          1 down          2 up-down      3 up-down-plus
    4 random      5 random walk           ... the same six again, over two
    6-11                                      octaves of the chord

Version History:
  - 1.0 Arp firmware by Sean Luke, Copyright 2025, from the GRAINS project
        https://github.com/eclab/grains
  - 1.1 Ported to HAGIWO MOD1 for maddie synths; the chord/style/inversion
        engine extracted to the shared SynthCore ArpCore.h, which the VCV Rack
        port shares. This sketch keeps only hardware I/O.

Deliberate changes from the original:
  - the five PWM calibration tables are gone. Upstream had to pick a table to
    match whichever AE oscillator it was plugged into (555 / VCO / uBUFFER /
    4BUFFER / 2OSCd) because GRAINS' output voltage sags with the current the
    oscillator pulls, and it warned you to tune tracking on POT1. MOD1 drives a
    buffered CV output, so pitch is a straight linear semitone ramp: 10-bit PWM
    over 0-5 V, five octaves, true 1 V/oct with no tuning pot.
  - Timer1 runs in mode 14 with ICR1 = 1023 rather than the repo's usual 8-bit
    fast PWM. 8 bits across 5 V is a quarter of a semitone per step, which is
    audibly out of tune; 10 bits is ~3.5 cents at the cost of dropping the
    carrier to 15.6 kHz, which the output filter still smooths.
  - upstream's INVERSION was a #define ("sorry, there just aren't enough pots
    and inputs"). It is on the button here: tap to cycle 0-3, saved to EEPROM.
    The engine clamps it to the chord's note count, which the #define did not —
    a too-large inversion left upstream's chord map short.
  - upstream's RESET input (AUDIO IN) becomes a button hold. MOD1 has four
    jacks and the clock, pitch CV, pitch out and gate use all of them.
  - upstream's CHORD CV (IN2) is dropped for the same reason; the one CV jack
    goes to pitch, which is the input a sequencer actually wants to drive.
  - the gate is delayed 2 ms after the clock edge instead of ~11.7 ms. That
    delay existed because Mozzi's 256 Hz control loop took three ticks to
    notice the new note; here the pitch PWM is written on the clock edge
    itself, so the gate only has to wait for the output filter to slew.
  - the random styles draw from sc::xorshift32 rather than Arduino random(),
    so the firmware and the Rack port produce identical sequences from the same
    seed (the repo-wide convention — see rack-plugins/PORTING.md). Both random
    styles pick their note directly instead of rejection-sampling in a
    while(1); same distribution, but it cannot spin on an 8-bit MCU.
  - the 16x12-byte chord table is 16 12-bit masks, 32 bytes instead of 192 —
    an eighth of the ATmega328P's RAM handed back.

License:
Apache License 2.0. This is a port of third-party code: the original Arp
firmware is Copyright 2025 Sean Luke (sean@cs.gmu.edu), from the GRAINS
project (github.com/eclab/grains), Apache 2.0. Apache requires the license
notice to travel with the code and modified files to carry prominent notice
of changes — the notice lives beside this sketch as LICENSE.md, and the
"Deliberate changes from the original" list above is the notice of changes.
Keep both.

Hardware:
HAGIWO MOD1
*/
#include <Arduino.h>
#include <EEPROM.h>
#include <Mod1Common.h>
#include <ArpCore.h>  // Shared arpeggiator engine (also used by the Rack port)

#define Brightness 200  // LED brightness scale (0-255); adjust for LED luminance

// Control tick. Upstream ran its pot reads and its pitch de-glitcher at Mozzi's
// CONTROL_RATE of 256 Hz; the filter time constants are in ticks, so hold the
// rate rather than letting them run at whatever speed loop() happens to spin.
const unsigned long kControlIntervalMs = 4;  // 250 Hz

// Button hold that means "restart", rather than "next inversion".
const unsigned long kHoldMs = 500;

// Gate delay after the clock edge, from the shared engine (2 ms).
const unsigned long kGateDelayMs =
    (unsigned long)(sc::kArpGateDelaySec * 1000.0f);

const uint8_t kInversionCount = 4;   // button cycles 0..3
const int kEepromInversionAddr = 0;  // one byte

sc::ArpEngine arp;
sc::ArpPitchFilter pitchFilter;

mod1::DebouncedInput buttonDebounce(50, HIGH);
mod1::EdgeInput clockEdge(LOW);

uint8_t inversion = 0;

// Smoothed pot readings, 0..1023. Upstream ran chord and style through the same
// 3/4 exponential average so a pot parked on a selector boundary cannot flicker
// between two chords; the pitch reading gets the heavier de-glitcher in the core.
uint16_t chordSmooth = 0;
uint16_t styleSmooth = 0;
uint8_t rootSemitone = 0;

unsigned long currentMillis = 0;
unsigned long previousControlMillis = 0;
unsigned long buttonDownMillis = 0;
unsigned long gateDueMillis = 0;
bool gatePending = false;
bool buttonHeld = false;

// Pitch CV out on F3 / D10 = OC1B. 0..1023 spans the full output range, which
// the engine defines as five octaves, so this is 1 V/oct by construction.
static void writePitchCv(float cv01) {
  int v = (int)(cv01 * 1023.0f + 0.5f);
  if (v < 0) v = 0;
  if (v > 1023) v = 1023;
  OCR1B = (uint16_t)v;
  analogWrite(mod1::PIN_LED, (int)(cv01 * (float)Brightness));
}

void setup() {
  pinMode(mod1::PIN_BUTTON, INPUT_PULLUP);
  pinMode(mod1::PIN_F1, INPUT);   // clock in
  pinMode(mod1::PIN_F3, OUTPUT);  // pitch CV out (OC1B)
  pinMode(mod1::PIN_F4, OUTPUT);  // gate out
  pinMode(mod1::PIN_LED, OUTPUT);
  digitalWrite(mod1::PIN_F4, LOW);

  inversion = EEPROM.read(kEepromInversionAddr);
  if (inversion >= kInversionCount) inversion = 0;  // sanitise uninitialised EEPROM

  // The shared helper sets up Timer2's 62.5 kHz fast PWM for the LED. Timer1 is
  // then taken over for the pitch output: the helper's EG-style setup tops out
  // at 255, and 8 bits across 5 V is a quarter of a semitone per step. Mode 14
  // with ICR1 = 1023 buys 10-bit resolution for a 15.6 kHz carrier. Only COM1B1
  // is enabled — F2's other pin is D9 / OC1A, and driving it would fight the
  // pitch CV being read on A4.
  mod1::setupFastPwmEgStyle();
  TCCR1A = _BV(COM1B1) | _BV(WGM11);
  TCCR1B = _BV(WGM13) | _BV(WGM12) | _BV(CS10);
  ICR1 = 1023;
  OCR1B = 0;

  // Seed the random styles from a floating ADC, as upstream seeded from A5.
  uint32_t s = ((uint32_t)analogRead(mod1::PIN_CV3) << 16) ^
               (uint32_t)analogRead(mod1::PIN_CV2);
  arp.seed(s);

  pitchFilter.reset((uint16_t)analogRead(mod1::PIN_POT1));
  chordSmooth = (uint16_t)analogRead(mod1::PIN_POT2);
  styleSmooth = (uint16_t)analogRead(mod1::PIN_POT3);
}

void loop() {
  currentMillis = millis();

  // Button: a tap steps the inversion, a hold restarts the arpeggio. The hold
  // fires as soon as the threshold passes so it is felt, not waited for, and
  // the release that follows is then not read as a tap.
  buttonDebounce.update((uint8_t)digitalRead(mod1::PIN_BUTTON), currentMillis);
  if (buttonDebounce.fell()) {  // active LOW
    buttonDownMillis = currentMillis;
    buttonHeld = false;
  }
  if (!buttonHeld && buttonDebounce.state() == LOW &&
      (currentMillis - buttonDownMillis) >= kHoldMs) {
    buttonHeld = true;
    arp.reset();
  }
  if (buttonDebounce.rose() && !buttonHeld) {
    inversion = (uint8_t)((inversion + 1) % kInversionCount);
    EEPROM.write(kEepromInversionAddr, inversion);
  }

  // Clock in on F1. The rising edge advances the arpeggio and arms the gate;
  // the falling edge drops it, so the gate is the clock shifted by kGateDelayMs
  // — which is what makes it safe to drive an envelope from.
  clockEdge.update((uint8_t)digitalRead(mod1::PIN_F1));
  if (clockEdge.rose()) {
    arp.step(sc::arpSelectChord(chordSmooth / 1023.0f),
             sc::arpSelectStyle(styleSmooth / 1023.0f), inversion);
    writePitchCv(arp.pitchCv(rootSemitone));
    gateDueMillis = currentMillis + kGateDelayMs;
    gatePending = true;
  } else if (clockEdge.fell()) {
    gatePending = false;
    digitalWrite(mod1::PIN_F4, LOW);
  }
  if (gatePending && (long)(currentMillis - gateDueMillis) >= 0) {
    gatePending = false;
    digitalWrite(mod1::PIN_F4, HIGH);
  }

  if (currentMillis - previousControlMillis >= kControlIntervalMs) {
    previousControlMillis = currentMillis;

    chordSmooth = (uint16_t)((chordSmooth * 3u +
                              (uint16_t)analogRead(mod1::PIN_POT2)) >> 2);
    styleSmooth = (uint16_t)((styleSmooth * 3u +
                              (uint16_t)analogRead(mod1::PIN_POT3)) >> 2);

    // Root pitch: POT1 summed with the F2 CV jack, then de-glitched.
    const uint16_t pitchAdc = pitchFilter.update((uint16_t)mod1::addClamp1023(
        analogRead(mod1::PIN_POT1), analogRead(mod1::PIN_CV2)));
    rootSemitone = sc::arpRootSemitone(pitchAdc / 1023.0f);

    // Re-emit the held note against the current root, so turning PITCH (or
    // moving the CV) transposes what is already sounding rather than waiting
    // for the next clock. Upstream got this for free by rebuilding its output
    // at the audio rate.
    writePitchCv(arp.pitchCv(rootSemitone));
  }
}
