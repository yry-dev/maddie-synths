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

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO MOD1
*/
#include <Arduino.h>
#include <Mod1Common.h>

// Pin definitions
#define LOGIC_SELECT_PIN mod1::PIN_POT1 // Pin for selecting logic type
#define POT_A_PIN mod1::PIN_POT2        // Additional pot for input A
#define POT_B_PIN mod1::PIN_POT3        // Additional pot for input B
#define IN_A_PIN mod1::PIN_CV1          // Input A
#define IN_B_PIN mod1::PIN_CV2          // Input B
#define LED_PIN mod1::PIN_LED           // LED output (PWM) using Timer2 OCR2B
#define OUT_A_PIN mod1::PIN_F3          // PWM output for result A side (Timer1 OCR1B)
#define OUT_B_PIN mod1::PIN_F4          // PWM output for result B side (Timer2 OCR2A)

// Global variables for Flip-Flop mode
bool flipA = false;        // Flip-Flop state for input A
bool flipB = false;        // Flip-Flop state for input B
bool lastA = false;        // Previous digital state for input A
bool lastB = false;        // Previous digital state for input B

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
  // Read logic selection from A0
  int selectValue = analogRead(LOGIC_SELECT_PIN); // 0 to 1023

  // Determine logic type based on A0 range
  byte logicMode = mod1::select6FromAdc(selectValue);

  // Read inputs for A and B with additional pots
  // Ensure the sum does not exceed 1023
  int rawA1 = analogRead(POT_A_PIN); // 0..1023
  int rawA3 = analogRead(IN_A_PIN);  // 0..1023
  int sumA  = mod1::addClamp1023(rawA1, rawA3);

  int rawB2 = analogRead(POT_B_PIN); // 0..1023
  int rawB4 = analogRead(IN_B_PIN);  // 0..1023
  int sumB  = mod1::addClamp1023(rawB2, rawB4);

  int valA = sumA;
  int valB = sumB;

  // Prepare output variables
  int outA = 0; // 0..255 for OCR1B
  int outB = 0; // 0..255 for OCR2A

  // Threshold for digital logic
  bool digitalA = (valA > 512);
  bool digitalB = (valB > 512);

  // Logic processing
  switch (logicMode) {
    case 0: // AND
      // outA => AND
      // outB => NAND
      outA = (digitalA && digitalB) ? 255 : 0;
      outB = (!(digitalA && digitalB)) ? 255 : 0;
      break;

    case 1: // OR
      // outA => OR
      // outB => NOR
      outA = (digitalA || digitalB) ? 255 : 0;
      outB = (!(digitalA || digitalB)) ? 255 : 0;
      break;

    case 2: // XOR
      // outA => XOR
      // outB => XNOR
      outA = ((digitalA ^ digitalB)) ? 255 : 0;
      outB = (! (digitalA ^ digitalB)) ? 255 : 0;
      break;

    case 3: // COMPARE
      // If A>B => D10=HIGH, else if B>A => D11=HIGH
      if (valA > valB) {
        outA = 255;
        outB = 0;
      } else if (valB > valA) {
        outA = 0;
        outB = 255;
      } else {
        // If equal, both 0
        outA = 0;
        outB = 0;
      }
      break;

    case 4: // MAX/MIN
      // D10 => MAX, D11 => MIN, 0~1023 -> 0~255
      if (valA > valB) {
        outA = valA >> 2; // MAX => A
        outB = valB >> 2; // MIN => B
      } else {
        outA = valB >> 2; // MAX => B
        outB = valA >> 2; // MIN => A
      }
      break;

    case 5: // FLIP-FLOP
    {
      // T-type flip-flop style
      bool currentA = digitalA;
      bool currentB = digitalB;

      // Rising edge detection for A
      if (currentA && !lastA) {
        flipA = !flipA;
      }
      // Rising edge detection for B
      if (currentB && !lastB) {
        flipB = !flipB;
      }

      lastA = currentA;
      lastB = currentB;

      // Output states
      outA = flipA ? 255 : 0;
      outB = flipB ? 255 : 0;
    }
    break;
  }

  // Write results to PWM registers
  OCR1B = outA; // D10 output
  OCR2A = outB; // D11 output

  // For LED on D3, same duty cycle as outA
  OCR2B = outA; // D3 output
}
