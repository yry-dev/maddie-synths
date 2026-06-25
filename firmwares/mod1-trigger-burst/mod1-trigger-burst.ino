/* Trigger Burst

Description:
Clock-syncable trigger burst. When POT3 is fully left it accepts an external
clock on F1; to the right of 9:00 it switches to the internal clock, whose rate
is then set by POT3. Original firmware by Hagiwo for Mod1.

Key Variables:
  A0 -> Burst number (1, 3, 4, 6, 8, 16)
  A1 -> Burst frequency (master clock bpm /2, /3, /4, /6, /8, /16)
  A2 -> Internal clock rate (~bpm 280)

      ╔═══════════╗
      ║   BURST   ║
      ║  trigger  ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   NUM     — burst number (1,3,4,6,8,16)
      ║    NUM    ║
      ║           ║
      ║   (A1)    ║   DIV     — burst frequency division
      ║    DIV    ║
      ║           ║
      ║   (A2)    ║   CLOCK   — internal clock rate
      ║   CLOCK   ║
      ║           ║
      ║    [·]    ║   LED (D3) — trigger output
      ║   (BTN)   ║   BTN (D4) — trigger in
      ║           ║
      ╠═══════════╣
      ║ F1     F2 ║   F1 (D17) IN  — External clock
      ║ (o)   (o) ║   F2 (D9)  IN  — Trigger
      ║           ║
      ║ F3     F4 ║   F3 (D10) IN  — Burst number CV
      ║ (o)   (o) ║   F4 (D11) OUT — Trigger
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Trigger Burst firmware by Hagiwo
  - 1.1 Refactored to use shared TriggerBurstCore

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO MOD1
*/
#include <Arduino.h>
#include <Mod1Common.h>
#include <TriggerBurstCore.h>

const int pinTriggerIn       = mod1::PIN_F2;
const int pinTriggerOut      = mod1::PIN_F4;
const int pinLed             = mod1::PIN_LED;
const int pinNumPot          = mod1::PIN_POT1;
const int pinNumPot2         = mod1::PIN_CV3;
const int pinDivPot          = mod1::PIN_POT2;
const int pinBpmPot          = mod1::PIN_POT3;
const int pinButton          = mod1::PIN_BUTTON;
const int pinExternalClock   = mod1::PIN_F1;

// Burst voice (shared core)
sc::TriggerBurstVoice burst;

// Trigger input edge detection
int lastTriggerInState = LOW;

// Button debounce
mod1::DebouncedInput buttonDebounce(20, HIGH);

// External clock tracking (hardware I/O — stays in firmware)
bool          useExternalClock       = false;
unsigned long externalClockPeriods[3] = {0, 0, 0};
byte          externalIndex          = 0;
unsigned long lastExternalMillis     = 0;
unsigned long externalPeriodMs       = 0;
int           lastExternalState      = LOW;

// Loop timing for dt
unsigned long lastLoopMillis = 0;

//----------------------------------------------------------------------------------
// checkExternalClock
// Detect rising edge on D17, average last 3 intervals (unchanged from original)
void checkExternalClock() {
  int currentExternalState = digitalRead(pinExternalClock);
  if (currentExternalState == HIGH && lastExternalState == LOW) {
    unsigned long now = millis();
    unsigned long cycle = now - lastExternalMillis;
    lastExternalMillis = now;

    externalClockPeriods[externalIndex] = cycle;
    externalIndex++;
    if (externalIndex >= 3) {
      externalIndex = 0;
    }

    unsigned long sum = 0;
    for (int i = 0; i < 3; i++) {
      sum += externalClockPeriods[i];
    }
    externalPeriodMs = sum / 3;
  }
  lastExternalState = currentExternalState;
}

//----------------------------------------------------------------------------------
// setup
void setup() {
  pinMode(pinTriggerIn, INPUT);
  pinMode(pinTriggerOut, OUTPUT);
  pinMode(pinLed, OUTPUT);
  pinMode(pinButton, INPUT_PULLUP);
  pinMode(pinExternalClock, INPUT);

  digitalWrite(pinTriggerOut, LOW);
  digitalWrite(pinLed, LOW);

  lastLoopMillis = millis();
}

//----------------------------------------------------------------------------------
// loop
void loop() {
  unsigned long currentMillis = millis();
  float dt = (float)(currentMillis - lastLoopMillis) / 1000.0f;
  if (dt <= 0.0f) dt = 0.001f;
  lastLoopMillis = currentMillis;

  // 1) Read burst number: POT1 + CV3 summed and clamped
  int numRaw = mod1::addClamp1023(analogRead(pinNumPot), analogRead(pinNumPot2));
  float numNorm = numRaw / 1023.0f;

  // 2) Read division from POT2
  float divNorm = analogRead(pinDivPot) / 1023.0f;

  // 3) Read clock pot; threshold <50 selects external clock
  int bpmRaw = analogRead(pinBpmPot);
  useExternalClock = (bpmRaw < 50);
  float bpmNorm = bpmRaw / 1023.0f;

  // Map normalised controls to burst parameters
  sc::TriggerBurstParams p = sc::triggerBurstMapParams(numNorm, divNorm, bpmNorm);

  // 4) Measure external clock period
  checkExternalClock();

  // Determine clock period in seconds
  float clockPeriodSec;
  if (useExternalClock && externalPeriodMs > 0) {
    clockPeriodSec = (float)externalPeriodMs / 1000.0f;
  } else {
    clockPeriodSec = 60.0f / p.bpm;
  }

  // 5) Detect trigger rising edge on F2
  int currentTriggerState = digitalRead(pinTriggerIn);
  bool trigRose = (currentTriggerState == HIGH && lastTriggerInState == LOW);
  lastTriggerInState = currentTriggerState;

  // 6) Debounce button; treat press as a manual trigger
  buttonDebounce.update((uint8_t)digitalRead(pinButton), currentMillis);
  bool btnFell = buttonDebounce.fell();

  // 7) Advance burst core
  sc::TriggerBurstResult r = burst.process(
    dt, trigRose || btnFell,
    p.numTriggers, p.divRatio, clockPeriodSec
  );

  // 8) Drive outputs
  digitalWrite(pinTriggerOut, r.gateOn ? HIGH : LOW);
  digitalWrite(pinLed,        r.gateOn ? HIGH : LOW);
}
