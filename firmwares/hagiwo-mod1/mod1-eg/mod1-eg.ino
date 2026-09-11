/* EG

Description:
3-output Envelope Generator. The end-of-cycle output can be self-patched for use
as an LFO or clock source. Original firmware by Hagiwo for Mod1.

Key Variables:
  A0 -> Attack time
  A1 -> Release time
  A2 -> Output level

      ╔═══════════╗
      ║    EG     ║
      ║ envelope  ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   ATTACK  — attack time
      ║  ATTACK   ║
      ║           ║
      ║   (A1)    ║   RELEASE — release time
      ║  RELEASE  ║
      ║           ║
      ║   (A2)    ║   LEVEL   — output level
      ║   LEVEL   ║
      ║           ║
      ║    [·]    ║   LED (D3) — output level
      ║   (BTN)   ║   BTN (D4) — trigger
      ║           ║
      ╠═══════════╣
      ║ F1     F2 ║   F1 (D17) IN  — Trigger
      ║ (o)   (o) ║   F2 (D9)  OUT — End-of-cycle pulse
      ║           ║
      ║ F3     F4 ║   F3 (D10) OUT — Inverted EG
      ║ (o)   (o) ║   F4 (D11) OUT — EG
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 EG firmware by Hagiwo
  - 1.1 Fix trig timing
  - 1.2 Forked and refactored from https://note.com/solder_state/n/n7499f01be846
  - 1.3 Shared-core refactor: PROGMEM Curve[] and PotAdjust[] tables replaced
        by closed-form expf/powf in EgCore.h (saves ~2 KB AVR RAM, same behavior)

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO MOD1
*/
#include <Mod1Common.h>
#include <EgCore.h>

#define Brightness 160  // 0-255: LED brightness cap

sc::EgVoice eg;

unsigned long previousMillis = 0;
unsigned long currentMillis  = 0;

mod1::EdgeInput signalEdge(HIGH);  // button (INPUT_PULLUP, fires on fell())
mod1::EdgeInput buttonEdge(LOW);   // F1 trigger input (fires on rose())

void setup() {
  pinMode(mod1::PIN_BUTTON, INPUT_PULLUP);  // button setting
  pinMode(mod1::PIN_F1, INPUT);             // trigger input
  pinMode(mod1::PIN_LED, OUTPUT);           // LED setting
  pinMode(mod1::PIN_F2, OUTPUT);            // EoC out
  pinMode(mod1::PIN_F3, OUTPUT);            // inv EG out
  pinMode(mod1::PIN_F4, OUTPUT);            // EG out

  mod1::setupFastPwmEgStyle();
}

void loop() {
  currentMillis = millis();

  // Read pots (normalised 0..1 for the shared core)
  float pot0 = analogRead(mod1::PIN_POT1) / 1023.0f;  // attack time
  float pot1 = analogRead(mod1::PIN_POT2) / 1023.0f;  // release time
  float pot2 = analogRead(mod1::PIN_POT3) / 1023.0f;  // output level
  sc::EgParams params = sc::egMapParams(pot0, pot1, pot2);

  // Edge detection — trigger on button press (fell = active-low) or F1 rising edge
  signalEdge.update((uint8_t)digitalRead(mod1::PIN_BUTTON));
  if (signalEdge.fell()) {
    eg.trigger();
  }

  buttonEdge.update((uint8_t)digitalRead(mod1::PIN_F1));
  if (buttonEdge.rose()) {
    eg.trigger();
  }

  // 1 ms tick: advance the envelope and update outputs
  if (currentMillis - previousMillis >= 1) {
    previousMillis = currentMillis;

    eg.process(0.001f, params);

    // Map envelope 0..1 → PWM 0..255
    int outputValue = (int)(eg.envelope * 255.0f + 0.5f);

    analogWrite(mod1::PIN_F4, outputValue);                      // EG out
    analogWrite(mod1::PIN_LED, outputValue * Brightness / 255);  // LED (brightness-capped)
    analogWrite(mod1::PIN_F3, 255 - outputValue);                // inverted EG
    digitalWrite(mod1::PIN_F2, eg.eoc ? HIGH : LOW);             // EoC pulse
  }
}
