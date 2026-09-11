/* Terrain LFO

Description:
Procedurally generated triple wavetable LFO with CV speed modulation and
probabilistic SloMo mode. On each button press, three independent wavetables
("terrains") are generated, output on F2/F3/F4, with Pot C setting the number of
"knots" per waveform. Generation follows musical constraints (seamless looping,
at least one zero crossing, nonlinear knot spacing, rest after a spike, one
Bezier segment). Reading is detuned per channel (x0.9 -> F2, x1.0 -> F3,
x1.1 -> F4). SloMo randomly and independently slows each terrain for a short,
tempo-scaled "breath". F1 CV (0-5V) adds 0-1 Hz of speed offset.
Firmware idea by Rob Heel for Mod1 designed by Hagiwo.

The terrain generation, phase reading, detune, SloMo and smoothing all live in
the shared SynthCore (TerrainLfoCore.h), which the VCV Rack port reuses. This
sketch keeps only the MOD1 hardware I/O: pots, button, CV in, PWM out and LED.

Key Variables:
  A0 -> Base speed (0.01-3 Hz)
  A1 -> SloMo probability
  A2 -> Knots per waveform (3..12)

      ╔═══════════╗
      ║  TERRAIN  ║
      ║    LFO    ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   SPEED   — base speed (0.01-3 Hz)
      ║   SPEED   ║
      ║           ║
      ║   (A1)    ║   SLOMO   — SloMo probability
      ║   SLOMO   ║
      ║           ║
      ║   (A2)    ║   KNOTS   — knots per waveform (3..12)
      ║   KNOTS   ║
      ║           ║
      ║    [·]    ║   LED (D3) — blink during generation
      ║   (BTN)   ║   BTN (D4) — generate new waveforms
      ║           ║
      ╠═══════════╣
      ║ F1     F2 ║   F1 (A3)  IN  — Speed offset CV
      ║ (o)   (o) ║   F2 (D9)  OUT — Terrain 1
      ║           ║
      ║ F3     F4 ║   F3 (D10) OUT — Terrain 2
      ║ (o)   (o) ║   F4 (D11) OUT — Terrain 3
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Terrain LFO firmware idea by Rob Heel for HAGIWO MOD1
  - 1.1 Forked and refactored from https://github.com/rob-scape/hgw-mod1-firmwares
  - 1.2 Refactored onto the shared SynthCore (TerrainLfoCore.h)

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO MOD1
*/

#include <Arduino.h>
#include <Mod1Common.h>
#include <TerrainLfoCore.h>  // Shared terrain core (also used by the VCV Rack port)

// Terrain state (generation, phases, SloMo, smoothing) lives in the shared core.
sc::TerrainLfoCore core;

// --- Uncomment to enable serial debug ---
// #define DEBUG_SERIAL

// --- Pins ---
const int potA = mod1::PIN_POT1;     // speed
const int potB = mod1::PIN_POT2;     // SloMo probability
const int potC = mod1::PIN_POT3;     // knots
const int cvIn = mod1::PIN_CV1;      // F1 speed modulation CV in
const int buttonPin = mod1::PIN_BUTTON;
const int ledPin = mod1::PIN_LED;
const int output1Pin = mod1::PIN_F2;
const int output2Pin = mod1::PIN_F3;
const int output3Pin = mod1::PIN_F4;

mod1::DebouncedInput buttonDebounce(50, HIGH);

bool blinking = false;
unsigned long blinkStart = 0;
const unsigned long blinkDuration = 200;

// -----------------------------------------------------------------------------
// setup()
// -----------------------------------------------------------------------------
void setup() {
  pinMode(potA, INPUT);
  pinMode(potB, INPUT);
  pinMode(potC, INPUT);
  pinMode(cvIn, INPUT);
  pinMode(buttonPin, INPUT_PULLUP);
  pinMode(ledPin, OUTPUT);

  // PWM outs
  pinMode(output1Pin, OUTPUT);  // F2
  pinMode(output2Pin, OUTPUT);  // F3
  pinMode(output3Pin, OUTPUT);  // F4

  // --- Timer1 (16-bit) PWM Setup for D9 (F2) & D10 (F3) ---
  TCCR1A = _BV(COM1A1) | _BV(COM1B1) | _BV(WGM10);
  TCCR1B = _BV(WGM12) | _BV(CS10);   // ~31.4 kHz PWM, no prescale

  // --- Timer2 (8-bit) PWM Setup for D11 (F4) ---
  TCCR2A = _BV(COM2A1) | _BV(WGM21) | _BV(WGM20);
  TCCR2B = _BV(CS21); // prescale 8 → ~7.8 kHz PWM

  OCR1A = 128;
  OCR1B = 128;
  OCR2A = 128;

  #ifdef DEBUG_SERIAL
  Serial.begin(115200);
  Serial.println("Dual Terrain LFO Ready");
  #endif

  generateTerrains(); // initial generation
}

// -----------------------------------------------------------------------------
// generateTerrains() — read knots pot, seed from hardware noise, regenerate all.
// -----------------------------------------------------------------------------
void generateTerrains() {
  digitalWrite(ledPin, HIGH);
  blinking = true;
  blinkStart = millis();

  int knots = (int)mod1::mapClamp(analogRead(potC), 0, 1023, 3, 12);

  // Seed from floating-ADC noise + timer entropy, mirroring the firmware's
  // per-table randomSeed(analogRead(A7/A6)) / micros() entropy sources.
  uint32_t seed = ((uint32_t)analogRead(A7) << 20)
                ^ ((uint32_t)analogRead(A6) << 10)
                ^ (uint32_t)micros();
  core.regenerate(knots, seed);

  #ifdef DEBUG_SERIAL
  Serial.println("New terrains generated");
  #endif
}

// -----------------------------------------------------------------------------
// loop()
// -----------------------------------------------------------------------------
void loop() {
  // --- button ---
  buttonDebounce.update((uint8_t)digitalRead(buttonPin), millis());
  if (buttonDebounce.fell()) {
    generateTerrains();
  }

  if (blinking && millis() - blinkStart > blinkDuration) {
    digitalWrite(ledPin, LOW);
    blinking = false;
  }

  // --- map pots (normalised 0..1) to engine units via the shared core ---
  const float pot1 = analogRead(potA) / 1023.0f;
  const float pot2 = analogRead(potB) / 1023.0f;
  const float pot3 = analogRead(potC) / 1023.0f;
  const sc::TerrainParams tp = sc::terrainMapParams(pot1, pot2, pot3);

  // --- CV modulation (0–5V -> 0–1 Hz speed offset) ---
  float cvHz = mod1::mapClamp(analogRead(cvIn), 0, 1023, 0, 1000) / 1000.0f;

  // --- real time step ---
  static unsigned long lastTime = 0;
  unsigned long now = micros();
  float dt = (now - lastTime) / 1e6f;
  if (dt <= 0) dt = 0.001f;
  lastTime = now;

  // Advance the terrain engine one loop iteration.
  core.step(dt, tp.baseHz, cvHz, tp.intensity);

  // --- write smoothed 0..1 outputs to the PWM registers (0..255) ---
  OCR1A = (uint8_t)(core.out[0] * 255.0f + 0.5f);  // F2 - D9
  OCR1B = (uint8_t)(core.out[1] * 255.0f + 0.5f);  // F3 - D10
  OCR2A = (uint8_t)(core.out[2] * 255.0f + 0.5f);  // F4 - D11

  #ifdef DEBUG_SERIAL
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 100) {
    Serial.print("BaseHz="); Serial.print(tp.baseHz, 2);
    Serial.print(" CVHz="); Serial.print(cvHz, 2);
    Serial.print(" Int="); Serial.println(tp.intensity, 2);
    lastPrint = millis();
  }
  #endif
}
