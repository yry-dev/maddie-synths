/*
HAGIWO MOD1 RandomCV Ver1.0
Periodic random CV sequencer.

--Pin assign---
POT1  A0  Step length 3,4,5,8,16,32
POT2  A1  output level
POT3  A2  Trigger probability
F1    D17  Clock in
F2    D9  Random value update
F3    D10  CV output
F4    D11 Trigger output
BUTTON    Random value update
LED       CV output
EEPROM    N/A
*/
#include <Arduino.h>
#include <Mod1Common.h>
unsigned long previousMillis = 0;
unsigned long currentMillis = 0;

mod1::DebouncedInput buttonDebounce(50, HIGH);

const int triggerPin = mod1::PIN_F1;  // stepping trigger
const int reRandomPin = mod1::PIN_F2; // re-randomize trigger input
const int cvOutPin = mod1::PIN_F3;
const int trigOutPin = mod1::PIN_F4;
const int ledPin = mod1::PIN_LED;
const int buttonPin = mod1::PIN_BUTTON;  // momentary switch (INPUT_PULLUP)
const int potPin = mod1::PIN_POT2;       // For CV scaling (A1)
const int stepSelectPin = mod1::PIN_POT1;  // Variable number of steps (A0)
const int trigProbPin = mod1::PIN_POT3;    // For trigger probability (A2)

int stepModes[] = { 3, 4, 5, 8, 16, 32 };
int currentStep = 0;
int currentTotalSteps = 8;
int cvValues[32];    // 0 to 255
int trigValues[32];  // 0 to 255, random values for trigger detection (cyclic pattern)
int selectedMode = 0;

bool lastTriggerState = HIGH;
bool lastReRandState = HIGH;

unsigned long trigOutTime = 0;
unsigned long trigOutStart = 0;
byte trigOutState = 2;

void setup() {
  pinMode(buttonPin, INPUT_PULLUP);
  pinMode(ledPin, OUTPUT);
  pinMode(trigOutPin, OUTPUT);

  pinMode(triggerPin, INPUT_PULLUP);
  pinMode(cvOutPin, OUTPUT);
  pinMode(reRandomPin, INPUT_PULLUP);

  mod1::setupFastPwmEgStyle();

  digitalWrite(trigOutPin, LOW);

  randomSeed(analogRead(mod1::PIN_POT1));
  reRandomizeCV();

  updateStepCount();
}

void loop() {
  currentMillis = millis();

  // Step progress on trigger input
  int trigReading = digitalRead(triggerPin);
  if (trigReading == HIGH && lastTriggerState == LOW) {
    updateStepCount();

    currentStep = (currentStep + 1) % currentTotalSteps;

    // CV scaling
    int potValue = analogRead(potPin);
    int cvMax = map(potValue, 0, 1023, 0, 255);
    uint16_t temp = (uint16_t)cvValues[currentStep] * (uint16_t)cvMax;
    int outputCV = temp / 255;  // 0～255
    analogWrite(cvOutPin, outputCV);
    analogWrite(ledPin, outputCV);

    // Apply trigger probability in real time
    int trigVal = analogRead(trigProbPin);
    int trigThreshold = map(trigVal, 0, 1023, 0, 255);
    // Fire if trigValues[currentStep] < trigThreshold
    if (trigValues[currentStep] < trigThreshold) {
      // Trigger out after 10ms
      trigOutTime = currentMillis + 10;
      trigOutState = 0;
    } else {
      trigOutState = 2;  // No trigger
    }
  }
  lastTriggerState = trigReading;

  // Re-randomize upon rising edge of D9 trigger
  int reRandReading = digitalRead(reRandomPin);
  if (reRandReading == HIGH && lastReRandState == LOW) {
    reRandomizeCV();
  }
  lastReRandState = reRandReading;

  // Trigger output process
  if (trigOutState == 0 && (long)(currentMillis - trigOutTime) >= 0) {
    digitalWrite(trigOutPin, HIGH);
    trigOutStart = currentMillis;
    trigOutState = 1;
  } else if (trigOutState == 1 && (currentMillis - trigOutStart) > 2) {
    digitalWrite(trigOutPin, LOW);
    trigOutState = 2;
  }

  // Re-randomize on D4 button press (debounce)
  buttonDebounce.update((uint8_t)digitalRead(buttonPin), currentMillis);
  if (buttonDebounce.fell()) {
    reRandomizeCV();
  }
}

void reRandomizeCV() {
  // Randomly regenerate values for CV and trigger (0 to 255)
  for (int i = 0; i < 32; i++) {
    cvValues[i] = random(256);
    trigValues[i] = random(256);
  }
}

void updateStepCount() {
  int val = analogRead(stepSelectPin);
  selectedMode = mod1::select6FromAdc(val);

  int newTotalSteps = stepModes[selectedMode];

  if (newTotalSteps != currentTotalSteps) {
    currentStep = currentStep % newTotalSteps;
    currentTotalSteps = newTotalSteps;
  }
}
