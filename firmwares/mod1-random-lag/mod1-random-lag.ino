/* Random Walk + Lag

Description:
Ultra-slow random walk CV generator, locked to a gentle frequency range. F2
outputs a lagged/delayed version of F4 for ambient crossfading textures, creating
dancing relationships between the two outputs. The button toggles between classic
Random Walk mode and Gravity Mode (which pulls the output slowly back to 0 over
time). F1 CV sets how closely F2 follows F4 (0V = almost independent / wide
evolving textures, 5V = tight following).

Key Variables:
  A0 -> Rate (how often new random steps occur)
  A1 -> Bias/Offset (shifts the walk output up or down)
  A2 -> ChaosDepth (step size: low = subtle drift, high = wilder jumps)

      ╔═══════════╗
      ║ RND WALK  ║
      ║   + lag   ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   RATE    — step rate
      ║   RATE    ║
      ║           ║
      ║   (A1)    ║   BIAS    — output offset
      ║   BIAS    ║
      ║           ║
      ║   (A2)    ║   CHAOS   — step size / chaos depth
      ║   CHAOS   ║
      ║           ║
      ║    [·]    ║   LED (D3) — output indicator
      ║   (BTN)   ║   BTN (D4) — Random Walk / Gravity mode
      ║           ║
      ╠═══════════╣
      ║ F1     F2 ║   F1 (A3)  IN  — Lag amount CV
      ║ (o)   (o) ║   F2 (D9)  OUT — Lagged walk
      ║           ║
      ║ F3     F4 ║   F3 (A5)  IN  — Chaos depth CV
      ║ (o)   (o) ║   F4 (D11) OUT — Random walk (main)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Random Walk + Lag firmware by Rob Heel for HAGIWO MOD1
  - 1.1 Forked and refactored from https://github.com/rob-scape/hgw-mod1-firmwares

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO MOD1
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