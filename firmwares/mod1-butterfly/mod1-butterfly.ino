/*
Lorenz Attractor CV Generator firmware by Rob Heel for Mod1 designed by Hagiwo. 

Lorenz Attractor is great for organic, non-repeating CV movement, chaotic but not random.  
This system inspired the popular 'butterfly effect' metaphor, where small changes can lead 
to dramatically different outcomes.” https://en.wikipedia.org/wiki/Lorenz_system 

Pots:
  A0 → Sigma (flow strength / controls how fast x and y try to equalize/ also mapped to stepSize)
  A1 → Rho   (divergence / higher = stronger pull from center -> more chaos, from calm to chaos)
  A2 → Beta  (damping / controls how sharply z grows or decays)

Input:
F1 (A3 / D17) → Trigger input (resets attractor on rising edge)

Outputs:
  D9 (F2)  → x axis (PWM CV)
  D10 (F3) → y axis (PWM CV)
  D11 (F4) → z axis (PWM CV)

Button (D4) → toggle normal and slow mode
LED (D3)    → Blinks in stepsize
*/


#include <Arduino.h>
#include <EEPROM.h>
#include <Mod1Common.h>

#define EEPROM_ADDR 0  // Address to store slowMode

const int potSigmaPin = mod1::PIN_POT1;
const int potRhoPin = mod1::PIN_POT2;
const int potBetaPin = mod1::PIN_POT3;

const int triggerPin = mod1::PIN_F1;
const int pinF2 = mod1::PIN_F2;
const int pinF3 = mod1::PIN_F3;
const int pinF4 = mod1::PIN_F4;

const int ledPin = mod1::PIN_LED;
const int buttonPin = mod1::PIN_BUTTON;

// slow and normal mode
bool slowMode = false;
mod1::DebouncedInput buttonDebounce(50, HIGH);
mod1::EdgeInput triggerEdge(LOW);

// Lorenz variables
float x = 0.1, y = 0.0, z = 0.0;
float dt = 0.01;

void setup() {
   Serial.begin(9600); // for debug open the serial port at 9600 bps:
  // Setup input pins
  pinMode(potSigmaPin, INPUT);
  pinMode(potRhoPin, INPUT);
  pinMode(potBetaPin, INPUT);

  pinMode(ledPin, OUTPUT);
  pinMode(buttonPin, INPUT_PULLUP);
  // trigger-in to reset attractor
  pinMode(triggerPin, INPUT);  // or INPUT_PULLUP if signal is open when idle

  // Setup PWM pins as outputs
  pinMode(pinF2, OUTPUT);
  pinMode(pinF3, OUTPUT);
  pinMode(pinF4, OUTPUT);

  // --- Timer1 (16-bit) PWM Setup for F2 (D9) & F3 (D10) ---
  // Fast PWM, 8-bit
  TCCR1A = _BV(COM1A1) | _BV(COM1B1) | _BV(WGM10);
  TCCR1B = _BV(WGM12) | _BV(CS10); // No prescale, ~31.4kHz PWM

  // --- Timer2 (8-bit) PWM Setup for F4 (D11) ---
  // Fast PWM on OC2A (D11), prescaler 8 (~7.8kHz)
  TCCR2A = _BV(COM2A1) | _BV(WGM21) | _BV(WGM20);
  TCCR2B = _BV(CS21);

  // Initialize OCR registers to mid-value
  OCR1A = 128;
  OCR1B = 128;
  OCR2A = 128;
}

unsigned long lastBlinkTime = 0;
bool ledState = false;

void loop() {
  buttonDebounce.update((uint8_t)digitalRead(buttonPin), millis());
  if (buttonDebounce.fell()) {
    slowMode = !slowMode;
    EEPROM.write(EEPROM_ADDR, slowMode ? 1 : 0);
    Serial.print("Slow mode: ");
    Serial.println(slowMode ? "ON" : "OFF");
  }

  // input trigger rising edge to reset attractor
  triggerEdge.update((uint8_t)digitalRead(triggerPin));
  if (triggerEdge.rose()) {
    // Rising edge detected — reset attractor
    x = 0.1;
    y = 0.0;
    z = 0.0;
  }

  // Read pots and map to Lorenz parameters
  float sigma = map(analogRead(potSigmaPin), 0, 1023, slowMode ? 1 : 5, slowMode ? 10 : 20);
  float rho   = map(analogRead(potRhoPin), 0, 1023, 20, 50);
  float beta  = map(analogRead(potBetaPin), 0, 1023, 1, 4);

  // Map Sigma pin to one of three discrete step sizes.
  const uint8_t stepMode = mod1::select3FromAdc(analogRead(potSigmaPin));
  if (slowMode) {
    dt = (stepMode == 0) ? 0.0001 : (stepMode == 1) ? 0.0005 : 0.001;
  } else {
    dt = (stepMode == 0) ? 0.001 : (stepMode == 1) ? 0.005 : 0.01;
  }

  // Lorenz attractor differential equations
  float dx = sigma * (y - x);
  float dy = x * (rho - z) - y;
  float dz = x * y - beta * z;

  // Euler integration
  x += dx * dt;
  y += dy * dt;
  z += dz * dt;

  // Map Lorenz outputs to PWM range
  int pwmX = constrain(mapFloat(x, -30, 30, 0, 255), 0, 255);
  int pwmY = constrain(mapFloat(y, -30, 30, 0, 255), 0, 255);
  int pwmZ = constrain(mapFloat(z, 0, 50, 0, 255), 0, 255);

  // Output PWM signals
  OCR1A = pwmX; // F2 - D9
  OCR1B = pwmY; // F3 - D10
  OCR2A = pwmZ; // F4 - D11

  // LED blink on D3
  unsigned long blinkInterval = map(dt * 1000000, 50, 10000, 500, 50);  // microseconds to milliseconds
  blinkInterval = constrain(blinkInterval, 50, 500);  // clamp to a sane range

  unsigned long now = millis();
  if (now - lastBlinkTime > blinkInterval) {
  ledState = !ledState;
  digitalWrite(ledPin, ledState);
  lastBlinkTime = now;
}

  // No delay for responsiveness
}


// Helper function to map float like Arduino's map()
float mapFloat(float x, float in_min, float in_max, float out_min, float out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

