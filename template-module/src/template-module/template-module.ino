/**
 * template-module
 *
 * Replace this file with your module firmware.
 *
 * Board: Arduino Uno (or compatible)
 * FQBN:  arduino:avr:uno
 */

// ---- Pin definitions --------------------------------------------------------

// Example: analogue input from a CV jack
const int CV_IN_PIN = A0;

// Example: digital output to a gate jack
const int GATE_OUT_PIN = 2;

// ---- Setup ------------------------------------------------------------------

void setup() {
  pinMode(GATE_OUT_PIN, OUTPUT);
  Serial.begin(115200);
}

// ---- Loop -------------------------------------------------------------------

void loop() {
  int cvValue = analogRead(CV_IN_PIN);

  // Simple threshold gate: output HIGH when CV > half-scale
  if (cvValue > 512) {
    digitalWrite(GATE_OUT_PIN, HIGH);
  } else {
    digitalWrite(GATE_OUT_PIN, LOW);
  }

  // Useful for debugging over the serial monitor
  Serial.println(cvValue);

  delay(1);
}
