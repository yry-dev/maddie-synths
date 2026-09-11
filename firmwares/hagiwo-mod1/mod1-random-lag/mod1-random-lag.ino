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
  - 1.2 Algorithm moved to RandomLagCore.h (shared with VCV Rack port)

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO MOD1
*/

#include <Arduino.h>
#include <Mod1Common.h>
#include <RandomLagCore.h>

sc::RandomLagVoice voice;

mod1::DebouncedInput buttonDebounce(50, HIGH);

void setup() {
  pinMode(mod1::PIN_F4, OUTPUT);           // F4 - Random Walk Output (main)
  pinMode(mod1::PIN_F2, OUTPUT);           // F2 - Lagged Output
  pinMode(mod1::PIN_LED, OUTPUT);          // LED Indicator
  pinMode(mod1::PIN_BUTTON, INPUT_PULLUP); // Button input

  mod1::setupFastPwmEgStyle();

  voice.reset();
}

void loop() {
  unsigned long currentMillis = millis();

  // Read potentiometers and CV inputs, normalised to [0..1].
  const float pot0 = analogRead(mod1::PIN_POT1) / 1023.0f; // Rate
  const float pot1 = analogRead(mod1::PIN_POT2) / 1023.0f; // Bias
  const float pot2 = analogRead(mod1::PIN_POT3) / 1023.0f; // ChaosDepth
  const float cv1  = analogRead(mod1::PIN_CV1)  / 1023.0f; // F1: Lag amount CV
  const float cv3  = analogRead(mod1::PIN_CV3)  / 1023.0f; // F3: Chaos depth CV

  const sc::RandomLagParams p = sc::randomLagMapParams(pot0, pot1, pot2, cv1, cv3);

  // Button toggles gravity mode.
  buttonDebounce.update((uint8_t)digitalRead(mod1::PIN_BUTTON), currentMillis);
  if (buttonDebounce.fell()) {
    voice.gravityMode = !voice.gravityMode;
  }

  // Advance the walk by one nominal loop period (no external trigger on MOD1).
  voice.process(1.0f / sc::kRandomLagLoopHz, false, p);

  // Scale normalised outputs to 8-bit PWM and write to hardware.
  const int walkVal   = constrain((int)(voice.walkOut(p.bias)   * 255.0f), 0, 255);
  const int laggedVal = constrain((int)(voice.laggedOut(p.bias) * 255.0f), 0, 255);

  analogWrite(mod1::PIN_F4, walkVal);    // F4 - Main Random Walk output
  analogWrite(mod1::PIN_F2, laggedVal);  // F2 - Lagged output
  OCR2B = walkVal;                       // LED brightness reflects main output
}
