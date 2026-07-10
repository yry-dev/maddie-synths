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
  - 1.3 Algorithm moved to EuclideanCore.h (shared with VCV Rack port).
        Euclidean pattern is now computed on-the-fly via the Bjorklund/Bresenham
        formula instead of PROGMEM lookup tables, freeing ~400 bytes of flash.

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO MOD1
*/
#include <Arduino.h>
#include <Mod1Common.h>
#include <EuclideanCore.h>  // shared sequencer core (also used by VCV Rack port)

// Pin definitions and timing settings
const int resetInputPin   = mod1::PIN_F1;      // Pin for step reset
const int resetButtonPin  = mod1::PIN_BUTTON;  // Pin for reset button
const int outputPin       = mod1::PIN_F4;      // Trigger output pin
const int hitCVPin        = mod1::PIN_CV3;     // Hits CV input pin
const int extraLedPin     = mod1::PIN_LED;     // Extra LED connected to D3 pin
const int triggerInputPin = mod1::PIN_F2;      // Clock input pin
const int potPin          = mod1::PIN_POT1;    // Hits potentiometer pin
const int stepModePin     = mod1::PIN_POT3;    // Analog pin for selecting step mode
const unsigned long triggerTime = 10;          // Trigger duration in milliseconds

// Shared sequencer core (algorithm + state)
sc::EuclideanVoice euclid;

// State management variables
unsigned long triggerStartMillis       = 0;
bool isTriggering                      = false;
bool lastTriggerInputState             = false;
static bool use16Step                  = false;
static bool lastUse16Step              = false;
unsigned long modeChangeLedStartMillis = 0;
bool isModeChangeLedOn                 = false;
bool disableOutputLed                  = false;

// Debounce variables
unsigned long lastResetDebounceTime    = 0;
const unsigned long resetDebounceDelay = 50;

void setup() {
  lastUse16Step = use16Step;
  pinMode(resetInputPin,   INPUT);
  pinMode(resetButtonPin,  INPUT_PULLUP);
  pinMode(outputPin,       OUTPUT);
  pinMode(extraLedPin,     OUTPUT);
  pinMode(triggerInputPin, INPUT);
  pinMode(potPin,          INPUT);
  digitalWrite(outputPin,  LOW);
}

void loop() {
  // Read A2 value to determine step mode
  int stepModeValue = analogRead(stepModePin);
  use16Step = stepModeValue > 511;

  // Turn off mode-change LED after the appropriate duration
  if (isModeChangeLedOn) {
    if (use16Step && millis() - modeChangeLedStartMillis >= 1000) {
      digitalWrite(extraLedPin, LOW);
      isModeChangeLedOn = false;
      disableOutputLed  = false;
    } else if (!use16Step && millis() - modeChangeLedStartMillis >= 500) {
      digitalWrite(extraLedPin, LOW);
      isModeChangeLedOn = false;
      disableOutputLed  = false;
    }
  }

  // Map normalised controls to sequencer parameters via the shared core
  int potValue         = min(analogRead(potPin) + 5 + analogRead(hitCVPin), 1023); // v1.1 FIX
  int probabilityValue = min(analogRead(mod1::PIN_POT2) + 5, 1023);                // v1.1 FIX
  sc::EuclideanParams ep = sc::euclideanMapParams(
      potValue         / 1023.0f,
      probabilityValue / 1023.0f,
      stepModeValue    / 1023.0f);

  // Check clock input for rising edge
  bool triggerInput = digitalRead(triggerInputPin) == HIGH;
  if (triggerInput && !lastTriggerInputState) {
    if (!disableOutputLed) {
      // Step the sequencer; supply a random draw for the probability gate
      bool fire = euclid.step(true, (float)random(100) / 100.0f, ep);
      if (fire) {
        digitalWrite(outputPin,   HIGH);
        digitalWrite(extraLedPin, HIGH);
        triggerStartMillis = millis();
        isTriggering = true;
      }
    }
  }

  // Check reset input or reset button with debounce
  bool resetInput  = digitalRead(resetInputPin)  == HIGH;
  bool resetButton = digitalRead(resetButtonPin) == LOW;  // active LOW
  static bool lastResetInputState = false;

  if ((resetInput || resetButton) && !lastResetInputState) {
    if (millis() - lastResetDebounceTime > resetDebounceDelay) {
      lastResetDebounceTime = millis();
      euclid.reset();  // Reset to first step
    }
  }
  lastResetInputState = resetInput || resetButton;

  // Update clock input state
  lastTriggerInputState = triggerInput;

  // Handle trigger timeout
  if (isTriggering) {
    if (millis() - triggerStartMillis >= triggerTime) {
      digitalWrite(outputPin, LOW);
      if (!disableOutputLed) {
        digitalWrite(extraLedPin, LOW);
      }
      isTriggering = false;
    }
  }
}
