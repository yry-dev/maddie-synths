/* Logic

Description:
Dual logic processor: AND/NAND, OR/NOR, XOR/XNOR, COMPARE, MAX/MIN, FLIP-FLOP.
Each output corresponds to one side of the selected logic operation. Original
firmware by Hagiwo for Mod1.

Key Variables:
  A0 -> Mode select (AND/NAND, OR/NOR, XOR/XNOR, COMPARE, MAX/MIN, FLIP-FLOP)
  A1 -> Input A value
  A2 -> Input B value

      ╔═══════════╗
      ║   LOGIC   ║
      ║   gates   ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   MODE    — logic mode select
      ║   MODE    ║
      ║           ║
      ║   (A1)    ║   IN A    — input A value
      ║   IN A    ║
      ║           ║
      ║   (A2)    ║   IN B    — input B value
      ║   IN B    ║
      ║           ║
      ║    [·]    ║   LED (D3) — PWM output A
      ║   (BTN)   ║   BTN (D4) — N/A
      ║           ║
      ╠═══════════╣
      ║ F1     F2 ║   F1 (A3)  IN  — CV input A
      ║ (o)   (o) ║   F2 (A4)  IN  — CV input B
      ║           ║
      ║ F3     F4 ║   F3 (D10) OUT — PWM output A
      ║ (o)   (o) ║   F4 (D11) OUT — PWM output B
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Logic firmware by Hagiwo
  - Forked and refactored from https://github.com/modulove/MOD1/tree/main/Firmware
  - Refactored to use LogicPairCore shared core (also used by the VCV Rack port)

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO MOD1
*/
#include <Arduino.h>
#include <Mod1Common.h>
#include <LogicPairCore.h>  // Shared logic core (also used by the VCV Rack port)

// Pin definitions
#define LOGIC_SELECT_PIN mod1::PIN_POT1 // Pin for selecting logic type
#define POT_A_PIN mod1::PIN_POT2        // Additional pot for input A
#define POT_B_PIN mod1::PIN_POT3        // Additional pot for input B
#define IN_A_PIN mod1::PIN_CV1          // Input A
#define IN_B_PIN mod1::PIN_CV2          // Input B
#define LED_PIN mod1::PIN_LED           // LED output (PWM) using Timer2 OCR2B
#define OUT_A_PIN mod1::PIN_F3          // PWM output for result A side (Timer1 OCR1B)
#define OUT_B_PIN mod1::PIN_F4          // PWM output for result B side (Timer2 OCR2A)

// Logic core holds the T flip-flop state for FLIP-FLOP mode.
sc::LogicPairVoice logic;

void setup() {
  // Set pin modes
  pinMode(OUT_A_PIN, OUTPUT);
  pinMode(OUT_B_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(IN_A_PIN, INPUT);
  pinMode(IN_B_PIN, INPUT);
  pinMode(POT_A_PIN, INPUT);
  pinMode(POT_B_PIN, INPUT);

  mod1::setupFastPwmLogicStyle();
}

void loop() {
  // Read logic selection from A0 and map to one of 6 modes.
  int selectValue = analogRead(LOGIC_SELECT_PIN); // 0 to 1023
  uint8_t logicMode = mod1::select6FromAdc(selectValue);

  // Mix pot + CV and clamp to ADC range.
  int sumA = mod1::addClamp1023(analogRead(POT_A_PIN), analogRead(IN_A_PIN));
  int sumB = mod1::addClamp1023(analogRead(POT_B_PIN), analogRead(IN_B_PIN));

  // Digital threshold (matches firmware's original 512 boundary).
  bool digitalA = (sumA > 512);
  bool digitalB = (sumB > 512);

  // Normalised analogue levels for COMPARE and MAX/MIN modes.
  float valA01 = sumA / 1023.0f;
  float valB01 = sumB / 1023.0f;

  // Run the shared logic core.
  sc::LogicPairResult out = logic.step(digitalA, digitalB, valA01, valB01, logicMode);

  // Scale 0..1 → 0..255 for PWM registers.
  int outA = (int)(out.outA * 255.0f + 0.5f);
  int outB = (int)(out.outB * 255.0f + 0.5f);

  // Write results to PWM registers.
  OCR1B = outA; // D10 OUT A
  OCR2A = outB; // D11 OUT B
  OCR2B = outA; // D3  LED (tracks outA, same as original firmware)
}
