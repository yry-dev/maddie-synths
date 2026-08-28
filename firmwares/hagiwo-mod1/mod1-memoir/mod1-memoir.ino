/* Memoir

Description:
A real-time CV and gate recorder. One take is 512 frames of 9-bit CV plus a
one-bit gate track, stretched over 4 to 32 seconds depending on the LENGTH pot,
and it is kept in EEPROM so it survives a power cycle. Hold the button to erase
and start recording; tap it to replay. Because LENGTH is read live, a take
recorded at one setting can be replayed at another — record four seconds of
knob wiggling and smear it over thirty-two, or the other way round. Original
Memoir firmware by Sean Luke, ported from the AE Modular GRAINS module.

Key Variables:
  A0 -> CV in offset (sums with the F1 jack)
  A1 -> Gate in offset (sums with the F2 jack)
  A2 -> Length: 4, 8, 12, 16, 20, 24, 28 or 32 seconds

      ╔═══════════╗
      ║  MEMOIR   ║
      ║ recorder  ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   CV      — CV in offset (sums with F1)
      ║    CV     ║
      ║           ║
      ║   (A1)    ║   GATE    — gate in offset (sums with F2)
      ║   GATE    ║
      ║           ║
      ║   (A2)    ║   LENGTH  — take length, 4…32 seconds
      ║  LENGTH   ║
      ║           ║
      ║    [·]    ║   LED (D3) — blinks while recording, CV level on playback
      ║   (BTN)   ║   BTN (D4) — tap: play / restart, hold: erase and record
      ║           ║
      ╠═══════════╣
      ║ F1     F2 ║   F1 (A3)  IN  — CV
      ║ (o)   (o) ║   F2 (A4)  IN  — Gate
      ║           ║
      ║ F3     F4 ║   F3 (D10) OUT — CV (9-bit PWM)
      ║ (o)   (o) ║   F4 (D11) OUT — Gate
      ║           ║
      ╚═══════════╝

Deliberate changes from the original:
  - GRAINS spends two jacks on RECORD and PLAY triggers. MOD1 has four jacks and
    the recorder itself needs all four (CV in, gate in, CV out, gate out), so the
    transport moved to the button MOD1 has and GRAINS does not: a tap plays or
    restarts playback, a hold of 600 ms or more erases and starts recording. The
    destructive gesture is the deliberate one.
  - MOD1 has an LED and GRAINS has nothing, which is why upstream's manual says
    you cannot tell when a take has finished. Here it blinks while recording and
    follows the CV during playback.
  - Length: upstream reads the pot as `adc >> 7 + 1`, which C parses as
    `adc >> 8` and so only ever yields 0…3 — three of its eight documented
    settings, and a zero divider. This port implements the documented 1…8, so
    the pot really does span 4 to 32 seconds.
  - CV output is unity: the recorded 0…511 spans the full PWM range, so a CV
    recorded at some voltage plays back at that voltage. Upstream scaled its
    9-bit value by 103/128 into part of the Mozzi output range.
  - The CV goes out on F3 as 9-bit fast PWM (Timer1, ICR1 = 511, 31.25 kHz)
    rather than Mozzi's 16.4 kHz output, so the PWM resolution matches the
    stored resolution exactly and nothing is lost on the way out.
  - A virgin ATmega328P has its EEPROM erased to 0xFF, which decodes as a
    take of full-scale CV with the gate stuck high. On a first boot that is
    detected and the buffer starts empty instead.

Version History:
  - 1.0 Memoir firmware by Sean Luke for AE Modular GRAINS
  - 1.1 Ported to HAGIWO MOD1: transport on the button, 1…8 length divider,
        9-bit Timer1 CV output; recorder extracted to MemoirCore.h (shared with
        the VCV Rack port)

License:
Apache License 2.0. This is a port of third-party code: the original Memoir
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
#include <EEPROM.h>
#include <Mod1Common.h>
#include <MemoirCore.h>

#define Brightness 160  // LED brightness scale (0-255); adjust for LED luminance

// Upstream's gate thresholds on a 10-bit ADC, hysteretic so a slow CV edge does
// not chatter the gate track.
const int kGateHigh = 600;
const int kGateLow = 400;

// Hold this long to erase and start recording. Long enough that you cannot do
// it by accident, short enough to catch a downbeat.
const unsigned long kHoldMs = 600;

// The recorder is 1046 bytes of the ATmega328P's 2048, which is why this sketch
// keeps everything else to a handful of scalars.
sc::MemoirEngine engine;

mod1::DebouncedInput buttonDebounce(20, HIGH);

unsigned long pressMs = 0;
bool holdFired = false;   // the hold already started a take; ignore the release
bool gateIn = false;      // hysteretic, so it has to persist between reads
unsigned long lastMicros = 0;

// Copy the whole take out to EEPROM. EEPROM.update skips bytes that already
// match, but a take that changed everywhere costs ~3.3 ms per byte and so
// blocks for a couple of seconds — upstream warns about this too, and the
// engine resynchronises rather than fast-forwarding when we come back.
void store() {
  const uint8_t* bytes = engine.bytes();
  for (uint16_t i = 0; i < sc::MemoirEngine::kBytes; i++) {
    EEPROM.update(i, bytes[i]);
  }
}

void load() {
  uint8_t* bytes = engine.bytes();
  bool virgin = true;
  for (uint16_t i = 0; i < sc::MemoirEngine::kBytes; i++) {
    bytes[i] = EEPROM.read(i);
    if (bytes[i] != 0xFF) virgin = false;
  }
  // An erased chip decodes as full-scale CV with the gate high; start empty.
  if (virgin) engine.erase();
}

void setup() {
  pinMode(mod1::PIN_BUTTON, INPUT_PULLUP);
  pinMode(mod1::PIN_LED, OUTPUT);
  pinMode(mod1::PIN_CV1, INPUT);  // F1 CV in — no pullup, it is an analog sense
  pinMode(mod1::PIN_CV2, INPUT);  // F2 gate in, likewise
  pinMode(mod1::PIN_F3, OUTPUT);  // CV out
  pinMode(mod1::PIN_F4, OUTPUT);  // gate out
  digitalWrite(mod1::PIN_F4, LOW);

  // 9-bit fast PWM on OC1B (D10 = F3): mode 14, TOP = ICR1 = 511, no prescaler,
  // so 16 MHz / 512 = 31.25 kHz. Not mod1::setupFastPwmEgStyle(), which pins
  // TOP at 255 on OC1A — we want a PWM step per stored CV step, and the stored
  // CV is 9 bits.
  TCCR1A = (1 << WGM11) | (1 << COM1B1);
  TCCR1B = (1 << WGM13) | (1 << WGM12) | (1 << CS10);
  ICR1 = 511;
  OCR1B = 0;

  load();
  lastMicros = micros();
}

void loop() {
  const unsigned long nowMicros = micros();
  const unsigned long elapsed = nowMicros - lastMicros;
  if (elapsed < 1000) return;  // service the engine at ~1 kHz
  lastMicros = nowMicros;

  const unsigned long nowMs = millis();

  // Transport. The hold fires the moment it passes the threshold rather than on
  // release, so the take starts when you feel it, not when you let go.
  buttonDebounce.update((uint8_t)digitalRead(mod1::PIN_BUTTON), nowMs);
  if (buttonDebounce.fell()) {
    pressMs = nowMs;
    holdFired = false;
  }
  if (buttonDebounce.state() == LOW && !holdFired && (nowMs - pressMs) >= kHoldMs) {
    holdFired = true;
    engine.triggerRecord();
  }
  if (buttonDebounce.rose() && !holdFired) {
    engine.triggerPlay();
  }

  // Both tracks sum their pot with their jack, as they do on GRAINS: the pot is
  // an offset you can record on its own, or a bias under the incoming CV.
  const int cvAdc = mod1::addClamp1023(analogRead(mod1::PIN_POT1), analogRead(mod1::PIN_CV1));
  const int gateAdc = mod1::addClamp1023(analogRead(mod1::PIN_POT2), analogRead(mod1::PIN_CV2));
  if (gateAdc > kGateHigh) {
    gateIn = true;
  } else if (gateAdc < kGateLow) {
    gateIn = false;
  }
  engine.setRate(sc::MemoirEngine::rateFromPot(analogRead(mod1::PIN_POT3) / 1023.0f));

  const float dt = elapsed * 1.0e-6f;
  const sc::MemoirFrame f = engine.process(dt, cvAdc / 1023.0f, gateIn);

  OCR1B = engine.cvRaw();  // 0..511 straight into the 9-bit PWM register
  digitalWrite(mod1::PIN_F4, f.gate ? HIGH : LOW);
  analogWrite(mod1::PIN_LED, (int)(f.led * Brightness));

  // A finished take gets written out here rather than inside the engine, which
  // knows nothing about EEPROM.
  if (engine.takeStoreRequest()) store();
}
