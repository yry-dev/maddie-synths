/* Assembly Test

Description:
Assembly / bring-up test - verifies pots, CV, gate inputs, button, LED and
audio output. Generates an audible test tone (POT1 pitch, POT2 waveform,
POT3/CV volume). IN1 adds a pitch jump while HIGH; IN2 forces a short
repeating beep while HIGH. The button cycles three test modes. Serial
monitor prints all readings every 200 ms at 115200 baud.

Key Variables:
  A0 -> Test-tone pitch
  A1 -> Waveform select
  A2 -> Volume (POT3 / CV)

      ╔═══════════╗
      ║   TEST    ║
      ║ assembly  ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - test-tone pitch
      ║   PITCH   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - waveform select
      ║   WAVE    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - volume (CV)
      ║    VOL    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - follows output / state
      ║   (BTN)   ║   BTN (GPIO6) - cycle 3 test modes
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - pitch jump while HIGH
      ║ (o)   (o) ║   IN2 (GPIO0) - repeating beep while HIGH
      ║           ║
      ║ CV    OUT ║   CV  (A2)    - volume (shared POT3)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM test tone
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 MOD2 assembly test program
  - 1.1 Forked and refactored for maddie synths

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO MOD2 (Seeed Xiao RP2350)
*/

#include <Arduino.h>
#include <math.h>

// ---------------- Pins ----------------
const uint8_t POT1_PIN   = A0;
const uint8_t POT2_PIN   = A1;
const uint8_t POT3_PIN   = A2;     // shared with CV input analog path
const uint8_t IN2_PIN    = 0;      // GPIO0
const uint8_t OUT_PIN    = 1;      // GPIO1
const uint8_t LED_PIN    = 5;      // GPIO5
const uint8_t BUTTON_PIN = 6;      // GPIO6
const uint8_t IN1_PIN    = 7;      // GPIO7

// ---------------- Audio ----------------
const uint32_t SAMPLE_RATE = 22050;
const uint32_t SAMPLE_PERIOD_US = 1000000UL / SAMPLE_RATE;
const float TWO_PI_F = 6.28318530718f;

volatile uint8_t currentSample = 128;

// ---------------- Button debounce ----------------
const unsigned long DEBOUNCE_MS = 50;
int lastButtonReading = HIGH;
int stableButtonState = HIGH;
unsigned long lastDebounceTime = 0;

// ---------------- Print timing ----------------
unsigned long lastPrintMs = 0;

// ---------------- Mode ----------------
enum TestMode : uint8_t {
  MODE_NORMAL = 0,
  MODE_INPUT_TEST = 1,
  MODE_DRONE_TEST = 2
};

TestMode mode = MODE_NORMAL;

// ---------------- Synth state ----------------
float phase = 0.0f;
uint32_t lastSampleUs = 0;

// ---------------- Helpers ----------------
static const char* modeName(TestMode m) {
  switch (m) {
    case MODE_NORMAL:     return "NORMAL";
    case MODE_INPUT_TEST: return "INPUT_TEST";
    case MODE_DRONE_TEST: return "DRONE_TEST";
    default:              return "UNKNOWN";
  }
}

static const char* waveName(int wave) {
  switch (wave) {
    case 0: return "SINE";
    case 1: return "TRIANGLE";
    case 2: return "SAW";
    case 3: return "SQUARE";
    default: return "UNKNOWN";
  }
}

float mapf(float x, float in_min, float in_max, float out_min, float out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

float clampf(float x, float lo, float hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}

// 0..1 phase -> -1..1 waveform
float waveformValue(int wave, float p) {
  switch (wave) {
    case 0: // SINE
      return sinf(TWO_PI_F * p);

    case 1: // TRIANGLE
      if (p < 0.25f) return mapf(p, 0.00f, 0.25f,  0.0f,  1.0f);
      if (p < 0.75f) return mapf(p, 0.25f, 0.75f,  1.0f, -1.0f);
      return mapf(p, 0.75f, 1.00f, -1.0f,  0.0f);

    case 2: // SAW
      return mapf(p, 0.0f, 1.0f, -1.0f, 1.0f);

    case 3: // SQUARE
      return (p < 0.5f) ? 1.0f : -1.0f;

    default:
      return 0.0f;
  }
}

void updateButton() {
  int reading = digitalRead(BUTTON_PIN);

  if (reading != lastButtonReading) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > DEBOUNCE_MS) {
    if (reading != stableButtonState) {
      stableButtonState = reading;

      // Button pressed (using INPUT_PULLUP)
      if (stableButtonState == LOW) {
        mode = static_cast<TestMode>((static_cast<uint8_t>(mode) + 1) % 3);

        Serial.println();
        Serial.println(F("=== BUTTON PRESSED ==="));
        Serial.print(F("New Mode: "));
        Serial.println(modeName(mode));
      }
    }
  }

  lastButtonReading = reading;
}

void generateAudio() {
  uint32_t nowUs = micros();
  if (nowUs - lastSampleUs < SAMPLE_PERIOD_US) {
    return;
  }
  lastSampleUs += SAMPLE_PERIOD_US;

  int pot1 = analogRead(POT1_PIN);
  int pot2 = analogRead(POT2_PIN);
  int pot3 = analogRead(POT3_PIN);

  bool in1 = digitalRead(IN1_PIN);
  bool in2 = digitalRead(IN2_PIN);

  // Wave select from POT2
  int wave = map(pot2, 0, 1023, 0, 3);

  // Frequency from POT1
  float freq = mapf((float)pot1, 0.0f, 1023.0f, 40.0f, 1200.0f);

  // POT3/CV path is inverted in hardware on this module,
  // so higher voltage often means lower ADC reading.
  // We invert in software so "more clockwise / more CV" feels like more level.
  float amp = 1.0f - ((float)pot3 / 1023.0f);
  amp = clampf(amp, 0.0f, 1.0f);

  // Mode behaviors
  if (mode == MODE_NORMAL) {
    if (in1) {
      freq *= 2.0f;       // gate input 1 bumps pitch
    }
    if (in2) {
      freq *= 1.5f;       // gate input 2 shifts pitch differently
      amp = max(amp, 0.5f);
    }
  } else if (mode == MODE_INPUT_TEST) {
    // In input test mode, no gate = silence.
    // IN1 or IN2 creates obvious beeps.
    if (!(in1 || in2)) {
      amp = 0.0f;
    } else {
      freq = in1 && in2 ? 880.0f : (in1 ? 440.0f : 660.0f);
      wave = in1 && in2 ? 3 : 0;
      amp = 0.9f;
    }
  } else if (mode == MODE_DRONE_TEST) {
    // Good for checking output path continuously
    freq = mapf((float)pot1, 0.0f, 1023.0f, 55.0f, 220.0f);
    amp = max(amp, 0.25f);
    if (in1) freq *= 2.0f;
    if (in2) wave = 3;
  }

  // Keep frequency sane
  freq = clampf(freq, 1.0f, 5000.0f);

  // Advance oscillator
  phase += freq / (float)SAMPLE_RATE;
  while (phase >= 1.0f) {
    phase -= 1.0f;
  }

  float v = waveformValue(wave, phase);        // -1..1
  float out = (v * amp * 0.5f) + 0.5f;         // 0..1
  out = clampf(out, 0.0f, 1.0f);

  uint8_t pwm = (uint8_t)(out * 255.0f);

  analogWrite(OUT_PIN, pwm);

  // LED follows output and input activity
  uint8_t ledValue = pwm;
  if (in1 || in2) {
    ledValue = max<uint8_t>(ledValue, 180);
  }
  analogWrite(LED_PIN, ledValue);

  currentSample = pwm;
}

void printStatus() {
  unsigned long now = millis();
  if (now - lastPrintMs < 200) {
    return;
  }
  lastPrintMs = now;

  int pot1 = analogRead(POT1_PIN);
  int pot2 = analogRead(POT2_PIN);
  int pot3 = analogRead(POT3_PIN);

  bool in1 = digitalRead(IN1_PIN);
  bool in2 = digitalRead(IN2_PIN);
  bool sw  = (digitalRead(BUTTON_PIN) == LOW);

  int wave = map(pot2, 0, 1023, 0, 3);
  float freq = mapf((float)pot1, 0.0f, 1023.0f, 40.0f, 1200.0f);
  float amp = 1.0f - ((float)pot3 / 1023.0f);
  amp = clampf(amp, 0.0f, 1.0f);

  Serial.println(F("----------------------------------------"));
  Serial.print(F("Mode: "));
  Serial.println(modeName(mode));

  Serial.print(F("POT1 / A0 (pitch): "));
  Serial.println(pot1);

  Serial.print(F("POT2 / A1 (wave select): "));
  Serial.print(pot2);
  Serial.print(F(" -> "));
  Serial.println(waveName(wave));

  Serial.print(F("POT3/CV / A2 (raw ADC): "));
  Serial.println(pot3);

  Serial.print(F("Computed level (inverted): "));
  Serial.println(amp, 3);

  Serial.print(F("IN1 / GPIO7: "));
  Serial.println(in1 ? F("HIGH") : F("LOW"));

  Serial.print(F("IN2 / GPIO0: "));
  Serial.println(in2 ? F("HIGH") : F("LOW"));

  Serial.print(F("Button / GPIO6: "));
  Serial.println(sw ? F("PRESSED") : F("RELEASED"));

  Serial.print(F("Base Freq (Hz): "));
  Serial.println(freq, 2);

  Serial.print(F("PWM Sample: "));
  Serial.println(currentSample);
}

void setup() {
  Serial.begin(115200);
  delay(800);

  analogReadResolution(10);
  analogWriteResolution(8);

  pinMode(POT1_PIN, INPUT);
  pinMode(POT2_PIN, INPUT);
  pinMode(POT3_PIN, INPUT);

  pinMode(IN1_PIN, INPUT);
  pinMode(IN2_PIN, INPUT);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  pinMode(OUT_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  analogWrite(OUT_PIN, 128);
  analogWrite(LED_PIN, 0);

  Serial.println();
  Serial.println(F("HAGIWO MOD2 Assembly Test"));
  Serial.println(F("Open Serial Monitor at 115200"));
  Serial.println(F("Button cycles modes: NORMAL -> INPUT_TEST -> DRONE_TEST"));
  Serial.println(F("Expected behavior:"));
  Serial.println(F("  POT1 = pitch"));
  Serial.println(F("  POT2 = waveform"));
  Serial.println(F("  POT3/CV = level (inverted in hardware path)"));
  Serial.println(F("  IN1/IN2 = gate inputs"));
  Serial.println(F("  LED = output/activity"));
  Serial.println(F("  OUT = audible test signal"));
  Serial.println();
}

void loop() {
  updateButton();
  generateAudio();
  printStatus();
}