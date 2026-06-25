/* Tap Tempo

Description:
4-output tap tempo master clock: a 4x fixed output, a 1~16x variable output, and
two variable 1~/16 division outputs. Tap the button four times to set the tempo.
Original firmware by Hagiwo for Mod1.

Key Variables:
  A0 -> F2 multiple rate
  A1 -> F3 division rate
  A2 -> F4 division rate

      ╔═══════════╗
      ║ TAP TEMPO ║
      ║   clock   ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   MULT    — F2 multiple rate
      ║   MULT    ║
      ║           ║
      ║   (A1)    ║   DIV     — F3 division rate
      ║    DIV    ║
      ║           ║
      ║   (A2)    ║   DIV     — F4 division rate
      ║    DIV    ║
      ║           ║
      ║    [·]    ║   LED (D3) — 1x output
      ║   (BTN)   ║   BTN (D4) — tap tempo (push 4x)
      ║           ║
      ╠═══════════╣
      ║ F1     F2 ║   F1 (D17) OUT — 4x clock
      ║ (o)   (o) ║   F2 (D9)  OUT — 1~16x variable
      ║           ║
      ║ F3     F4 ║   F3 (D10) OUT — 1~/16 variable
      ║ (o)   (o) ║   F4 (D11) OUT — 1~/16 variable
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Tap Tempo Clock firmware by Hagiwo
  - 1.1 Forked and refactored from https://note.com/solder_state/n/nc05d8e8fd311
  - 1.2 Timing algorithm extracted to the shared SynthCore TapTempoCore (seconds,
        dt-driven); this sketch keeps only the hardware I/O.

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO MOD1
*/
#include <Arduino.h>
#include <Mod1Common.h>
#include <TapTempoCore.h>  // Shared tap-tempo timing core (also used by vcvrack/src/TapTempo.cpp)

const byte buttonPin   = mod1::PIN_BUTTON;  // tap-tempo button input
const byte ledPin      = mod1::PIN_LED;     // 1x beat LED output
const byte trigPinMain = mod1::PIN_F1;      // F1 OUT — 4x clock
const byte trigPinA0   = mod1::PIN_F2;      // F2 OUT — 1~16x multiply
const byte trigPinA1   = mod1::PIN_F3;      // F3 OUT — 1~/16 divide
const byte trigPinA2   = mod1::PIN_F4;      // F4 OUT — 1~/16 divide
const byte potPinA0    = mod1::PIN_POT1;    // analog input for F2 multiply rate
const byte potPinA1    = mod1::PIN_POT2;    // analog input for F3 divide rate
const byte potPinA2    = mod1::PIN_POT3;    // analog input for F4 divide rate

mod1::DebouncedInput buttonDebounce(50, HIGH);
sc::TapTempoCore clockCore;

const unsigned long triggerOnTime = 5;   // 5ms HIGH per clock pulse
const unsigned long ledOnTime     = 10;  // 10ms LED on per beat

unsigned long lastLoopMillis = 0;

// Per-output 5ms pulse state (timing edges come from the core; pulse width and
// the digitalWrite stay here as hardware I/O).
struct PulseOut {
  byte pin;
  bool active;
  unsigned long startMs;
};
PulseOut pulses[4] = {
  {mod1::PIN_F1, false, 0},  // F1 4x
  {mod1::PIN_F2, false, 0},  // F2 multiply
  {mod1::PIN_F3, false, 0},  // F3 divide
  {mod1::PIN_F4, false, 0},  // F4 divide
};

bool ledActive = false;
unsigned long ledStartMs = 0;

void firePulse(byte idx, unsigned long now) {
  digitalWrite(pulses[idx].pin, HIGH);
  pulses[idx].active = true;
  pulses[idx].startMs = now;
}

void updatePulse(byte idx, unsigned long now) {
  if (pulses[idx].active && (now - pulses[idx].startMs) >= triggerOnTime) {
    digitalWrite(pulses[idx].pin, LOW);
    pulses[idx].active = false;
  }
}

void setup() {
  pinMode(buttonPin, INPUT_PULLUP);
  pinMode(ledPin, OUTPUT);
  pinMode(trigPinMain, OUTPUT);
  pinMode(trigPinA0, OUTPUT);
  pinMode(trigPinA1, OUTPUT);
  pinMode(trigPinA2, OUTPUT);

  digitalWrite(ledPin, LOW);
  digitalWrite(trigPinMain, LOW);
  digitalWrite(trigPinA0, LOW);
  digitalWrite(trigPinA1, LOW);
  digitalWrite(trigPinA2, LOW);

  lastLoopMillis = millis();
}

void loop() {
  unsigned long now = millis();
  float dt = (float)(now - lastLoopMillis) / 1000.0f;  // millis -> seconds
  lastLoopMillis = now;

  // Button input processing (tap on falling edge, like the original sketch).
  buttonDebounce.update((uint8_t)digitalRead(buttonPin), now);
  bool tapEdge = buttonDebounce.fell();

  // Read the three division pots as normalised 0..1 controls.
  float potF2 = analogRead(potPinA0) / 1023.0f;
  float potF3 = analogRead(potPinA1) / 1023.0f;
  float potF4 = analogRead(potPinA2) / 1023.0f;

  // Advance the shared timing engine and collect this step's trigger edges.
  sc::TapTempoEdges e = clockCore.process(dt, tapEdge, potF2, potF3, potF4);

  if (e.f1) firePulse(0, now);
  if (e.f2) firePulse(1, now);
  if (e.f3) firePulse(2, now);
  if (e.f4) firePulse(3, now);

  if (e.beat) {
    digitalWrite(ledPin, HIGH);
    ledActive = true;
    ledStartMs = now;
  }

  // Close each output's pulse after triggerOnTime / the LED after ledOnTime.
  updatePulse(0, now);
  updatePulse(1, now);
  updatePulse(2, now);
  updatePulse(3, now);

  if (ledActive && (now - ledStartMs) >= ledOnTime) {
    digitalWrite(ledPin, LOW);
    ledActive = false;
  }
}
