/*
Random Walk with Gravity Mode + Lagged Output for Hagiwos MOD1 module by Rob Heel.
Button toggles between classic Random Walk mode and Gravity Mode.
F2 outputs a lagged/delayed version of F4 for ambient crossfading effects.

Hardware Configuration:
- Potentiometer 1 (Rate)    → A0
- Potentiometer 2 (Bias/Offset)  → A1
- Potentiometer 3 (ChaosDepth) → A2

- LED Indicator          → Pin 3 (OCR2B)
- Push Button  → Pin 4

Inputs/Outputs:
- F1    A3  CV input (controls Lag Amount - how closely F2 follows F4)
- F2    D9  Lagged Output (slower following version of F4)
- F3    A5  CV input (adds to ChaosDepth)
- F4    D11 Random Walk Output (main)

Ultra Slow Random walk CV module code
- Locked to ultra-slow frequency range for gentle random walks
- F2 creates a lagged version that slowly follows F4

Behavior:
- Pot1 controls frequency (how often new random steps occur).
- Pot2 (Bias/Offset): Controls an offset applied to the walk output, shifting it up or down.
- Pot3 (ChaosDepth): Controls how large each random step can be. F3 CV input affects ChaosDepth.
  Low values make the walk drift subtly, high values allow bigger, wilder jumps between steps.

- F1 CV input controls Lag Amount - how closely F2 follows F4. Perfect for LFO modulation!
  No CV (0V) = F2 is almost independent (wide, evolving textures)
  High CV (5V) = F2 follows F4 very closely (tight relationship)
  
- Button toggles between classic Random Walk mode and Gravity Mode.
  Gravity Mode pulls the output slowly back to 0 over time.
- F2 outputs a lagged version - creates beautiful dancing relationships with F4.
*/

#include <Arduino.h>
#include <Mod1Common.h>

// Phase and value tracking
float walkPhase = 0.0;
float laggedPhase = 0.0; // Lagged version that slowly follows walkPhase

// Chaos parameters
float chaosDepth = 0.0;
float rate = 0.001f; // Default ultra slow rate
float bias = 0.0f; // Bias offset

// Lag parameters
float baseLagAmount = 0.9995f; // Base lag amount - almost independent when no CV
float lagAmount = 0.9995f; // Current lag amount (calculated each loop)

// Mode toggle
bool gravityMode = false; // Default to classic random walk

mod1::DebouncedInput buttonDebounce(50, HIGH);

void setup() {
  // Set up pins
  pinMode(mod1::PIN_F4, OUTPUT);      // F4 - Random Walk Output (main)
  pinMode(mod1::PIN_F2, OUTPUT);      // F2 - Lagged Output
  pinMode(mod1::PIN_LED, OUTPUT);     // LED Indicator
  pinMode(mod1::PIN_BUTTON, INPUT_PULLUP); // Button input

  mod1::setupFastPwmEgStyle();
  
  // Initialize lagged phase to match walk phase
  laggedPhase = walkPhase;
}

void loop() {
  unsigned long currentMillis = millis();

  // Read potentiometer values and CV inputs
  rate = readFrequency(mod1::PIN_POT1); // Rate controlled only by pot now
  chaosDepth = (analogRead(mod1::PIN_POT3) / 1023.0f) + (analogRead(mod1::PIN_CV3) / 1023.0f); // ChaosDepth modulated by F3 CV input
  chaosDepth = constrain(chaosDepth, 0.0f, 1.0f); // Ensure chaos stays within 0-1

  bias = (analogRead(mod1::PIN_POT2) / 1023.0f) * 0.8f - 0.4f; // Pot2 as bias control (-0.4 to +0.4 offset)

  // F1 CV input controls lag amount - INVERTED for intuitive behavior
  float lagCV = analogRead(mod1::PIN_CV1) / 1023.0f; // Read F1 CV input (0.0 to 1.0)
  lagAmount = baseLagAmount - (lagCV * 0.015f); // Range from 0.9995 (independent) to 0.9845 (tight following)
  lagAmount = constrain(lagAmount, 0.98f, 0.9995f); // Safety limits

  // Check for button press to toggle mode
  buttonDebounce.update((uint8_t)digitalRead(mod1::PIN_BUTTON), currentMillis);
  if (buttonDebounce.fell()) {
    gravityMode = !gravityMode;
  }

  // Random walk update
  if (gravityMode) {
    updateGravityWalk(walkPhase, rate, chaosDepth);
  } else {
    updateRandomWalk(walkPhase, rate, chaosDepth);
  }

  // Update lagged output - now dynamically controlled by F1 CV!
  updateLaggedOutput(walkPhase, laggedPhase, lagAmount);

  // Apply bias to both outputs
  int walkStepVal = (int)((walkPhase + bias) * 255.0f);
  walkStepVal = constrain(walkStepVal, 0, 255);
  
  int laggedStepVal = (int)((laggedPhase + bias) * 255.0f);
  laggedStepVal = constrain(laggedStepVal, 0, 255);

  analogWrite(mod1::PIN_F4, walkStepVal);   // F4 - Main Random Walk output
  analogWrite(mod1::PIN_F2, laggedStepVal);  // F2 - Lagged output
  OCR2B = walkStepVal; // LED brightness reflects main output
}

// Classic random walk behavior for F4 output
void updateRandomWalk(float &phase, float rate, float depth) {
  float randomStep = (random(-100, 100) / 100.0f) * depth;
  phase += randomStep * rate;

  if (phase < 0.0f) phase = 0.0f;
  if (phase > 1.0f) phase = 1.0f;
}

// Gravity mode — random walk that slowly falls back to 0
void updateGravityWalk(float &phase, float rate, float depth) {
  float randomStep = (random(-100, 100) / 100.0f) * depth;
  phase += randomStep * rate;

  // Introduce gravity pull towards 0
  phase *= 0.99f; // 0.995f → Gentle pull (slow decay) 0.98f → Medium pull (faster drift to zero) 0.9f → Strong pull (snaps back quickly)

  if (phase < 0.0f) phase = 0.0f;
  if (phase > 1.0f) phase = 1.0f;
}

// Update lagged output - now with dynamic lag control!
void updateLaggedOutput(float mainPhase, float &laggedPhase, float currentLagAmount) {
  // Exponential smoothing with CV-controlled lag amount
  // Higher lag = slower following (F2 drifts independently) - DEFAULT with no CV
  // Lower lag = faster following (F2 stays close to F4) - when CV is applied
  
  laggedPhase = (laggedPhase * currentLagAmount) + (mainPhase * (1.0f - currentLagAmount));
  
  // Perfect: Patch nothing = wide independent textures
  // Patch LFO = dynamic relationship from independent to tight coupling!
}

// Read frequency from Pot1 (A0)
float readFrequency(int analogPin) {
  int rawVal = analogRead(analogPin);
  float fMin = 0.001f;  // Locked to ultra slow mode
  float fMax = 0.1f;
  return fMin * powf(fMax / fMin, rawVal / 1023.0f);  // Exponential scaling
}