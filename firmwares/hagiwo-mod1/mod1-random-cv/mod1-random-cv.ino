/* Random CV

Description:
Periodic random CV sequencer. A cyclic pattern of random CV and trigger values
of selectable length, re-randomised on button press or via F2. Original firmware
by Hagiwo for Mod1.

Key Variables:
  A0 -> Step length (3, 4, 5, 8, 16, 32)
  A1 -> Output level
  A2 -> Trigger probability

      ╔═══════════╗
      ║ RANDOM CV ║
      ║ sequencer ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   STEPS   — step length (3,4,5,8,16,32)
      ║   STEPS   ║
      ║           ║
      ║   (A1)    ║   LEVEL   — output level
      ║   LEVEL   ║
      ║           ║
      ║   (A2)    ║   PROB    — trigger probability
      ║   PROB    ║
      ║           ║
      ║    [·]    ║   LED (D3) — CV output
      ║   (BTN)   ║   BTN (D4) — random value update
      ║           ║
      ╠═══════════╣
      ║ F1     F2 ║   F1 (D17) IN  — Clock
      ║ (o)   (o) ║   F2 (D9)  IN  — Random value update
      ║           ║
      ║ F3     F4 ║   F3 (D10) OUT — CV
      ║ (o)   (o) ║   F4 (D11) OUT — Trigger
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 RandomCV firmware by Hagiwo
  - 1.1 Forked and refactored from https://note.com/solder_state/n/nd2af5f03a9c7
  - 1.2 Algorithm extracted to RandomCvCore.h (shared with VCV Rack port)

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO MOD1
*/
#include <Arduino.h>
#include <Mod1Common.h>
#include <RandomCvCore.h>

unsigned long currentMillis = 0;

mod1::DebouncedInput buttonDebounce(50, HIGH);

const int triggerPin    = mod1::PIN_F1;   // stepping trigger
const int reRandomPin   = mod1::PIN_F2;   // re-randomize trigger input
const int cvOutPin      = mod1::PIN_F3;
const int trigOutPin    = mod1::PIN_F4;
const int ledPin        = mod1::PIN_LED;
const int buttonPin     = mod1::PIN_BUTTON;  // momentary switch (INPUT_PULLUP)
const int potPin        = mod1::PIN_POT2;    // CV scaling (A1)
const int stepSelectPin = mod1::PIN_POT1;    // variable number of steps (A0)
const int trigProbPin   = mod1::PIN_POT3;    // trigger probability (A2)

bool lastTriggerState = HIGH;
bool lastReRandState  = HIGH;

unsigned long trigOutTime  = 0;
unsigned long trigOutStart = 0;
byte trigOutState = 2;

sc::RandomCvVoice core;

void setup() {
  pinMode(buttonPin,   INPUT_PULLUP);
  pinMode(ledPin,      OUTPUT);
  pinMode(trigOutPin,  OUTPUT);
  pinMode(triggerPin,  INPUT_PULLUP);
  pinMode(cvOutPin,    OUTPUT);
  pinMode(reRandomPin, INPUT_PULLUP);

  mod1::setupFastPwmEgStyle();
  digitalWrite(trigOutPin, LOW);

  // Seed the PRNG from analog noise (mirrors original randomSeed(analogRead(...))).
  uint32_t analogSeed = (uint32_t)analogRead(mod1::PIN_POT1);
  core.seed(analogSeed != 0 ? analogSeed : 1u);

  // Initialise step count from pot (mirrors original updateStepCount() in setup()).
  core.currentTotalSteps =
      sc::kRandomCvStepModes[mod1::select6FromAdc(analogRead(stepSelectPin))];
}

void loop() {
  currentMillis = millis();

  // Step forward on rising edge of clock input.
  int trigReading = digitalRead(triggerPin);
  if (trigReading == HIGH && lastTriggerState == LOW) {
    sc::RandomCvParams p = sc::randomCvMapParams(
        analogRead(stepSelectPin) / 1023.0f,
        analogRead(potPin)        / 1023.0f,
        analogRead(trigProbPin)   / 1023.0f);

    sc::RandomCvFrame frame = core.step(true, p);

    int outputCV = (int)(frame.cv * 255.0f);
    analogWrite(cvOutPin, outputCV);
    analogWrite(ledPin,   outputCV);

    if (frame.gate) {
      trigOutTime  = currentMillis + 10;
      trigOutState = 0;
    } else {
      trigOutState = 2;
    }
  }
  lastTriggerState = trigReading;

  // Re-randomize on rising edge of F2.
  int reRandReading = digitalRead(reRandomPin);
  if (reRandReading == HIGH && lastReRandState == LOW) {
    core.randomize();
  }
  lastReRandState = reRandReading;

  // Trigger output pulse: 10 ms delay then 2 ms width (unchanged from original).
  if (trigOutState == 0 && (long)(currentMillis - trigOutTime) >= 0) {
    digitalWrite(trigOutPin, HIGH);
    trigOutStart = currentMillis;
    trigOutState = 1;
  } else if (trigOutState == 1 && (currentMillis - trigOutStart) > 2) {
    digitalWrite(trigOutPin, LOW);
    trigOutState = 2;
  }

  // Re-randomize on button press (debounced).
  buttonDebounce.update((uint8_t)digitalRead(buttonPin), currentMillis);
  if (buttonDebounce.fell()) {
    core.randomize();
  }
}
