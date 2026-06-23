/* Euclidean

Description:
8-step or 16-step Euclidean rhythm sequencer with adjustable output probability
and number of hits. Original firmware by Hagiwo for Mod1.

Key Variables:
  A0 -> Number of hits
  A1 -> Output probability
  A2 -> Step length: 8 (knob left) <-> 16 (knob right)

      ╔═══════════╗
      ║ EUCLIDEAN ║
      ║ sequencer ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   HITS    — number of hits
      ║   HITS    ║
      ║           ║
      ║   (A1)    ║   PROB    — output probability
      ║   PROB    ║
      ║           ║
      ║   (A2)    ║   STEPS   — step length (8 <-> 16)
      ║   STEPS   ║
      ║           ║
      ║    [·]    ║   LED (D3) — trigger output
      ║   (BTN)   ║   BTN (D4) — reset step
      ║           ║
      ╠═══════════╣
      ║ F1     F2 ║   F1 (D17) IN  — Reset step
      ║ (o)   (o) ║   F2 (D9)  IN  — Clock
      ║           ║
      ║ F3     F4 ║   F3 (D10) IN  — Number of hits CV
      ║ (o)   (o) ║   F4 (D11) OUT — Trigger
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Initial release (Hagiwo)
  - 1.1 Noise countermeasures: prevent the ADC being fixed at its maximum value
  - 1.2 Forked and refactored from https://note.com/solder_state/n/n42841e48c0ea

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO MOD1
*/
#include <Arduino.h>
#include <Mod1Common.h>
// Definition of Euclidean rhythms
const int euclidean_rhythm[9][8] = {
    {0, 0, 0, 0, 0, 0, 0, 0}, // Hits: 0
    {1, 0, 0, 0, 0, 0, 0, 0}, // Hits: 1
    {1, 0, 0, 0, 1, 0, 0, 0}, // Hits: 2
    {1, 0, 1, 0, 0, 1, 0, 0}, // Hits: 3
    {1, 0, 1, 0, 1, 0, 1, 0}, // Hits: 4
    {1, 1, 0, 1, 1, 0, 1, 0}, // Hits: 5
    {1, 1, 1, 0, 1, 1, 1, 0}, // Hits: 6
    {1, 1, 1, 1, 1, 1, 1, 0}, // Hits: 7
    {1, 1, 1, 1, 1, 1, 1, 1}, // Hits: 8
};

const int euclidean_rhythm_16[17][16] = {
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, // Hits: 0
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, // Hits: 1
    {1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0}, // Hits: 2
    {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0}, // Hits: 3
    {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0}, // Hits: 4
    {1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 0}, // Hits: 5
    {1, 0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0}, // Hits: 6
    {1, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0, 0}, // Hits: 7
    {1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0}, // Hits: 8
    {1, 1, 0, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0}, // Hits: 9
    {1, 1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 1, 0, 1, 0}, // Hits: 10
    {1, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0, 1, 1, 0}, // Hits: 11
    {1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0}, // Hits: 12
    {1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1, 1, 1, 0}, // Hits: 13
    {1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0}, // Hits: 14
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0}, // Hits: 15
    {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1}, // Hits: 16
};

// Pin definitions and timing settings
const int resetInputPin = mod1::PIN_F1; // Pin for step reset
const int resetButtonPin = mod1::PIN_BUTTON; // Pin for reset button
const int outputPin = mod1::PIN_F4; // Trigger output pin
const int hitCVPin = mod1::PIN_CV3; // Trigger CV input pin
const int extraLedPin = mod1::PIN_LED; // Extra LED connected to D3 pin
const int triggerInputPin = mod1::PIN_F2; // Trigger input pin
const int potPin = mod1::PIN_POT1; // Potentiometer pin
const int stepModePin = mod1::PIN_POT3; // Analog pin for selecting step mode
const unsigned long triggerTime = 10; // Trigger duration in milliseconds

// State management variables
unsigned long triggerStartMillis = 0; // Trigger start time
int currentStep = 0; // Current step index
bool isTriggering = false; // Triggering state flag
bool lastTriggerInputState = false; // Previous trigger input state
static bool use16Step = false; // Initial value for step mode
static bool lastUse16Step = false; // Last step mode state
unsigned long modeChangeLedStartMillis = 0; // LED start time for mode change
bool isModeChangeLedOn = false; // LED state for mode change
bool disableOutputLed = false; // Flag to disable output LED during mode change

// Debounce variables
unsigned long lastResetDebounceTime = 0; // Last debounce time for reset button
const unsigned long resetDebounceDelay = 50; // Debounce delay in milliseconds

void setup() {
  lastUse16Step = use16Step; // Initialize last mode state
  pinMode(resetInputPin, INPUT); // Set step reset pin as input
  pinMode(resetButtonPin, INPUT_PULLUP); // Set reset button pin as input with pull-up
  pinMode(outputPin, OUTPUT);
  pinMode(extraLedPin, OUTPUT);
  pinMode(triggerInputPin, INPUT);
  pinMode(potPin, INPUT);
  digitalWrite(outputPin, LOW);
}

void loop() {
  // Read A2 value to determine step mode
  int stepModeValue = analogRead(stepModePin);
  use16Step = stepModeValue > 511;

  // Turn off LED after the appropriate time
  if (isModeChangeLedOn) {
    if (use16Step && millis() - modeChangeLedStartMillis >= 1000) {
      digitalWrite(extraLedPin, LOW);
      isModeChangeLedOn = false;
      disableOutputLed = false; // Re-enable output LED
    } else if (!use16Step && millis() - modeChangeLedStartMillis >= 500) {
      digitalWrite(extraLedPin, LOW);
      isModeChangeLedOn = false;
      disableOutputLed = false; // Re-enable output LED
    }
  }

  // Read potentiometer value to select Hits
  int potValue = min(analogRead(potPin)+5 + analogRead(hitCVPin), 1023);//v1.1 FIX

  // Explicitly set range for Hits selection based on mode
  int selectedHits;
  if (use16Step) {
    selectedHits = map(potValue, 0, 1023, 0, 16); // For 16 steps
  } else {
    selectedHits = map(potValue, 0, 1023, 0, 8); // For 8 steps
  }

  // Set probability for triggering output
  int probabilityValue = min(analogRead(mod1::PIN_POT2)+5,1023); // Read probability value from A1 pin , v1.1FIX
  int triggerProbability = map(probabilityValue, 0, 1023, 0, 100); // Map to 0-100%

  // Check trigger input
  bool triggerInput = digitalRead(triggerInputPin) == HIGH;
  if (triggerInput && !lastTriggerInputState) {
    // Advance step on LOW to HIGH transition of trigger input

    // Output based on current step
    if (!disableOutputLed) { // Skip output LED if mode change LED is active
      if (use16Step) {
        if (euclidean_rhythm_16[selectedHits][currentStep] == 1 && random(100) < triggerProbability) {
          digitalWrite(outputPin, HIGH); // Start trigger
          digitalWrite(extraLedPin, HIGH); // Turn on extra LED
          triggerStartMillis = millis();
          isTriggering = true;
        }
        currentStep = (currentStep + 1) % 16;
      } else {
        if (euclidean_rhythm[selectedHits][currentStep] == 1 && random(100) < triggerProbability) {
          digitalWrite(outputPin, HIGH); // Start trigger
          digitalWrite(extraLedPin, HIGH); // Turn on extra LED
          triggerStartMillis = millis();
          isTriggering = true;
        }
        currentStep = (currentStep + 1) % 8;
      }
    }
  }

  // Check reset input or reset button with debounce
  bool resetInput = digitalRead(resetInputPin) == HIGH;
  bool resetButton = digitalRead(resetButtonPin) == LOW; // Button is active LOW
  static bool lastResetInputState = false; // Previous reset input state

  if ((resetInput || resetButton) && !lastResetInputState) {
    if (millis() - lastResetDebounceTime > resetDebounceDelay) {
      lastResetDebounceTime = millis();
      currentStep = 0; // Reset to first step
    }
  }
  lastResetInputState = resetInput || resetButton;

  // Update trigger input state
  lastTriggerInputState = triggerInput;

  // Handle triggering process
  if (isTriggering) {
    if (millis() - triggerStartMillis >= triggerTime) {
      digitalWrite(outputPin, LOW); // End trigger
      if (!disableOutputLed) {
        digitalWrite(extraLedPin, LOW); // Turn off extra LED
      }
      isTriggering = false;
    }
  }
}
