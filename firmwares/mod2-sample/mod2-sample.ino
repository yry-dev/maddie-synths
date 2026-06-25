/* Sample Player

Description:
One-shot 16-bit PCM sample player - up to 18 samples (~20 s total) stored
in flash. Two-stage selection: group (POT2) then index within group
(POT3). Variable-speed playback (0.5-1.5x) with anti-aliasing
interpolation. Requires a generated sample.h (see scripts/wav_to_sample.py).

Key Variables:
  A0 -> Playback speed (0.5-1.5x)
  A1 -> Sample group (1-6, 7-12, 13-18)
  A2 -> Sample index within group (shared with CV)

      ╔═══════════╗
      ║  SAMPLE   ║
      ║  player   ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - playback speed 0.5-1.5x
      ║   SPEED   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - sample group
      ║   GROUP   ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - index within group
      ║   INDEX   ║
      ║           ║
      ║   (BTN)   ║   BTN (GPIO6) - manual trigger      
      ║    [·]    ║   LED (GPIO5) - trigger (20 ms pulse)
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - external trigger (rising)
      ║ (o)   (o) ║   IN2 (GPIO0) - select +6 sample number
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - index (shared POT3)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Sample Player firmware by Hagiwo
  - 1.1 Forked and refactored for maddie synths

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO MOD2 (Seeed Xiao RP2350)
*/

#include <Arduino.h>
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include "sample.h"   // Header file containing sample data arrays
#include <Mod2Common.h>  // Shared MOD2 pin map, PWM-audio setup and helpers

#define FRAC_BITS    12
#define FRAC_MASK    ((1UL << FRAC_BITS) - 1)

const uint16_t PWM_FS   = 1023;
const uint16_t PWM_MID  = PWM_FS / 2;
const uint16_t PWM_WRAP = 4095;

volatile bool     playing     = false;
volatile uint32_t tblAcc      = 0;
volatile uint32_t tblStepFP   = 1UL << FRAC_BITS;
volatile const uint8_t* curSample = sample01;
volatile uint32_t curLen16   = sampleLens[0];

uint sliceAudio, sliceIRQ;

volatile bool ledOn     = false;
uint32_t     ledOnTime = 0;

void on_pwm_wrap() {
  pwm_clear_irq(sliceIRQ);

  if (!playing) {
    pwm_set_chan_level(sliceAudio, PWM_CHAN_B, PWM_MID);
    return;
  }

  uint32_t idx  = tblAcc >> FRAC_BITS;
  uint32_t frac = tblAcc & FRAC_MASK;

  if (idx >= curLen16) {
    playing = false;
    tblAcc  = 0;
    pwm_set_chan_level(sliceAudio, PWM_CHAN_B, PWM_MID);
    return;
  }

  int16_t s1 = mod2::readPCM16LE(curSample, idx);
  int16_t s2 = (idx + 1 < curLen16) ? mod2::readPCM16LE(curSample, idx + 1) : 0;
  int16_t interpolated = mod2::lerpFixed(s1, s2, frac);

  uint16_t pwmVal = uint16_t(int32_t(interpolated) + 32768) >> 6;
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, pwmVal);

  tblAcc += tblStepFP;
}

uint8_t selectSampleIndex() {
  uint16_t groupVal = analogRead(A1);

  uint8_t group;
  if (groupVal <= 341)        group = 0;
  else if (groupVal <= 684)   group = 1;
  else                        group = 2;

  uint16_t val = 1023 - analogRead(A2);
  uint8_t subIndex;
  if (val < 32)        subIndex = 0;
  else if (val < 248)  subIndex = 1;
  else if (val < 514)  subIndex = 2;
  else if (val < 720)  subIndex = 3;
  else if (val < 926)  subIndex = 4;
  else                  subIndex = 5;

  uint8_t index = group * 6 + subIndex;

  if (digitalRead(mod2::IN2_PIN) == HIGH) {
    index += 6;
    if (index >= 18) {
      index = index % 6;
    }
  }

  return index;
}

void onTrigger() {
  playing = false;
  tblAcc  = 0;

  delay(5);

  uint8_t idx = selectSampleIndex();
  curSample = samples[idx];
  curLen16  = sampleLens[idx];

  uint16_t raw = analogRead(A0);
  float rate   = 0.5f + (raw / 1023.0f);
  tblStepFP    = uint32_t(rate * float(1 << FRAC_BITS));

  digitalWrite(mod2::LED_PIN, HIGH);
  ledOn     = true;
  ledOnTime = millis();

  playing = true;
}

void setup() {
  analogReadResolution(10);

  pinMode(mod2::IN2_PIN, INPUT);
  pinMode(mod2::LED_PIN, OUTPUT);
  digitalWrite(mod2::LED_PIN, LOW);

  // PWM audio + 36.6 kHz wrap-IRQ setup (shared)
  mod2::initAudioPwm(sliceAudio, sliceIRQ, on_pwm_wrap);

  pinMode(mod2::IN1_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(mod2::IN1_PIN), onTrigger, RISING);
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(mod2::BUTTON_PIN), onTrigger, FALLING);
}

void loop() {
  if (ledOn && (millis() - ledOnTime >= 20)) {
    digitalWrite(mod2::LED_PIN, LOW);
    ledOn = false;
  }
}
