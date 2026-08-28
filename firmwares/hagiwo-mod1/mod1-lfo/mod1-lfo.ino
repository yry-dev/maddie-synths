/* LFO

Description:
Simple LFO. Various CV inputs allow you to create chaotic CVs. The frequency
range is saved to EEPROM when the button is pressed. Original firmware by Hagiwo
for Mod1.

Key Variables:
  A0 -> Frequency
  A1 -> Waveform select
  A2 -> Output level

      ╔═══════════╗
      ║    LFO    ║
      ║ modulator ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   FREQ    — frequency
      ║   FREQ    ║
      ║           ║
      ║   (A1)    ║   WAVE    — waveform select
      ║   WAVE    ║
      ║           ║
      ║   (A2)    ║   LEVEL   — output level
      ║   LEVEL   ║
      ║           ║
      ║    [·]    ║   LED (D3) — output
      ║   (BTN)   ║   BTN (D4) — change frequency range (saved to EEPROM)
      ║           ║
      ╠═══════════╣
      ║ F1     F2 ║   F1 (A3)  IN  — Frequency CV
      ║ (o)   (o) ║   F2 (A4)  IN  — Waveform CV
      ║           ║
      ║ F3     F4 ║   F3 (A5)  IN  — Output level CV
      ║ (o)   (o) ║   F4 (D11) OUT — LFO
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 LFO firmware by Hagiwo
  - 1.1 Forked and refactored from https://github.com/modulove/MOD1/tree/main/Firmware
  - 1.2 Shared LfoCore: six PROGMEM lookup tables replaced by closed-form
        waveforms in sc::LfoVoice; saves ~5 KB flash. Waveform shapes are
        mathematically equivalent to the original tables. See LfoCore.h for
        the table-to-closed-form behavioral note.

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO MOD1
*/
#include <Arduino.h>
#include <EEPROM.h>
#include <Mod1Common.h>
#include <LfoCore.h>

#define Brightness 64  // LED brightness scale (0-255); adjust for LED luminance

int freqRange = 1;  // 1 = low range (~0.01–1.5 Hz), 10 = high range (~0.01–15 Hz)

unsigned long previousMillis = 0;
unsigned long currentMillis = 0;

mod1::DebouncedInput buttonDebounce(50, HIGH);
sc::LfoVoice lfo;

void setup() {
  pinMode(mod1::PIN_BUTTON, INPUT_PULLUP);
  pinMode(mod1::PIN_LED, OUTPUT);
  pinMode(mod1::PIN_F4, OUTPUT);

  freqRange = EEPROM.read(0);
  if (freqRange != 1 && freqRange != 10) freqRange = 1;  // sanitise uninitialised EEPROM

  mod1::setupFastPwmEgStyle();
}

void loop() {
  currentMillis = millis();

  // Button: toggle frequency range 1x ↔ 10x (persisted to EEPROM)
  buttonDebounce.update((uint8_t)digitalRead(mod1::PIN_BUTTON), currentMillis);
  if (buttonDebounce.fell()) {
    freqRange = (freqRange == 1) ? 10 : 1;
    EEPROM.write(0, freqRange);
  }

  // Read pots and CV; normalise to 0..1 matching the shared core's input domain
  int potValueA0 = analogRead(mod1::PIN_POT1);  // frequency pot
  int CVValueA3  = analogRead(mod1::PIN_CV1);   // frequency CV
  float rate01 = mod1::addClamp1023(potValueA0, CVValueA3) / 1023.0f;

  int potValueA1 = analogRead(mod1::PIN_POT2);  // waveform pot
  int CVValueA4  = analogRead(mod1::PIN_CV2);   // waveform CV
  float waveNorm = mod1::addClamp1023(potValueA1, CVValueA4) / 1023.0f;

  int potValueA2 = analogRead(mod1::PIN_POT3);  // level pot
  int CVValueA5  = analogRead(mod1::PIN_CV3);   // level CV
  float levelNorm = min(potValueA2 / 4 + CVValueA5 / 4, 255) / 255.0f;

  if (currentMillis - previousMillis >= 1) {
    previousMillis = currentMillis;

    uint8_t waveSelect = sc::lfoSelectWave(waveNorm);
    float freq = sc::lfoMapFreq(rate01, freqRange);

    float ledPhase = 0.0f;
    // dt = 0.001 s (1 ms update rate matches the original 1 ms timer)
    float out = lfo.process(0.001f, freq, waveSelect, levelNorm, ledPhase);

    // Map bipolar −1..+1 to 8-bit PWM (0 = min, 127 = centre/0V, 255 = max)
    int pwm = constrain((int)((out * 0.5f + 0.5f) * 255.0f + 0.5f), 0, 255);

    analogWrite(mod1::PIN_F4, pwm);                       // LFO waveform output
    analogWrite(mod1::PIN_LED, pwm * Brightness / 255);  // LED follows output
  }
}
