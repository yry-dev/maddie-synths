/* Radio Sampler

Description:
Long-form audio sampler (up to ~130 s at 36.6 kHz), tuned for
vocal/speech/podcast sampling. Variable-speed playback with pitch
shifting (0.5-1.5x), start-position control and forward/reverse switching.
Linear interpolation smooths the pitch shift. Requires a generated
sample.h (see scripts/wav_to_sample.py).

Key Variables:
  A0 -> Playback speed (0.5-1.5x)
  A1 -> Direction (<512 forward, >=512 reverse)
  A2 -> Start position (0-100%, shared with CV)

      ╔═══════════╗
      ║   RADIO   ║
      ║  sampler  ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - playback speed 0.5-1.5x
      ║   SPEED   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - forward / reverse
      ║    DIR    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - start position 0-100%
      ║   START   ║
      ║           ║
      ║   (BTN)   ║   BTN (GPIO6) - manual trigger      
      ║    [·]    ║   LED (GPIO5) - trigger (20 ms pulse)
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - external trigger (rising)
      ║ (o)   (o) ║   IN2 (GPIO0) - loop toggle (HIGH = loop)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - start position (shared POT3)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Radio Sampler firmware by Hagiwo
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
#include "sample.h"   // Header file containing the single long-form sample
#include <Mod2Common.h>  // Shared MOD2 pin map, PWM-audio setup and helpers

#define FRAC_BITS    12
#define FRAC_MASK    ((1UL << FRAC_BITS) - 1)

const uint16_t PWM_FS   = 1023;
const uint16_t PWM_MID  = PWM_FS / 2;
const uint16_t PWM_WRAP = 4095;

volatile bool     playing      = false;
volatile uint32_t tblAcc       = 0;
volatile uint32_t tblStepFP    = 1UL << FRAC_BITS;
volatile bool     isReverse    = false;
volatile bool     loopMode     = false;
volatile uint32_t startPos16   = 0;
volatile uint32_t endPos16     = 0;

uint sliceAudio, sliceIRQ;

volatile bool ledOn     = false;
uint32_t     ledOnTime = 0;

// PWM interrupt handler - generates audio output
void on_pwm_wrap() {
  pwm_clear_irq(sliceIRQ);

  if (!playing) {
    pwm_set_chan_level(sliceAudio, PWM_CHAN_B, PWM_MID);
    return;
  }

  uint32_t idx  = tblAcc >> FRAC_BITS;
  uint32_t frac = tblAcc & FRAC_MASK;

  // Check boundaries
  if (!isReverse) {
    // Forward playback
    if (idx >= endPos16) {
      if (loopMode) {
        tblAcc = startPos16 << FRAC_BITS;
      } else {
        playing = false;
        tblAcc  = startPos16 << FRAC_BITS;
        pwm_set_chan_level(sliceAudio, PWM_CHAN_B, PWM_MID);
        return;
      }
    }
  } else {
    // Reverse playback
    if (idx <= startPos16 || idx >= SAMPLE_LENGTH) {
      if (loopMode) {
        tblAcc = (endPos16 - 1) << FRAC_BITS;
      } else {
        playing = false;
        tblAcc  = startPos16 << FRAC_BITS;
        pwm_set_chan_level(sliceAudio, PWM_CHAN_B, PWM_MID);
        return;
      }
    }
  }

  idx = tblAcc >> FRAC_BITS;
  frac = tblAcc & FRAC_MASK;

  // Linear interpolation for smooth playback
  int16_t s1 = mod2::readPCM16LE(vocalSample, idx);
  int16_t s2;

  if (!isReverse) {
    s2 = (idx + 1 < endPos16) ? mod2::readPCM16LE(vocalSample, idx + 1) : 0;
  } else {
    s2 = (idx > startPos16) ? mod2::readPCM16LE(vocalSample, idx - 1) : 0;
  }

  int16_t interpolated = mod2::lerpFixed(s1, s2, frac);

  // Convert to PWM value
  uint16_t pwmVal = uint16_t(int32_t(interpolated) + 32768) >> 6;
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, pwmVal);

  // Advance position
  if (!isReverse) {
    tblAcc += tblStepFP;
  } else {
    // In reverse, subtract the step
    if (tblAcc >= tblStepFP) {
      tblAcc -= tblStepFP;
    } else {
      tblAcc = startPos16 << FRAC_BITS;
      if (loopMode) {
        tblAcc = (endPos16 - 1) << FRAC_BITS;
      } else {
        playing = false;
      }
    }
  }
}

void onTrigger() {
  playing = false;
  delay(5);

  // Read POT3 (A2) for start position (0-100% of sample)
  uint16_t posVal = 1023 - analogRead(A2);
  uint32_t startPercent = (uint32_t)posVal * 100UL / 1023UL;

  // Calculate start position (0-100% of sample length)
  startPos16 = (SAMPLE_LENGTH * startPercent) / 100UL;

  // Set end position based on a reasonable snippet length
  // Default: play from start to end of sample
  endPos16 = SAMPLE_LENGTH;

  // Read POT2 (A1) for direction
  uint16_t dirVal = analogRead(A1);
  isReverse = (dirVal >= 512);

  // Set initial position based on direction
  if (!isReverse) {
    tblAcc = startPos16 << FRAC_BITS;
  } else {
    tblAcc = (endPos16 - 1) << FRAC_BITS;
  }

  // Read POT1 (A0) for playback speed (0.5× to 1.5×)
  uint16_t raw = analogRead(A0);
  float rate   = 0.5f + (raw / 1023.0f);
  tblStepFP    = uint32_t(rate * float(1 << FRAC_BITS));

  // Read IN2 (GPIO0) for loop mode
  loopMode = (digitalRead(mod2::IN2_PIN) == HIGH);

  // LED indicator
  digitalWrite(mod2::LED_PIN, HIGH);
  ledOn     = true;
  ledOnTime = millis();

  playing = true;
}

void setup() {
  analogReadResolution(10);

  pinMode(mod2::IN2_PIN, INPUT);    // IN2 - Loop mode
  pinMode(mod2::LED_PIN, OUTPUT);   // LED
  digitalWrite(mod2::LED_PIN, LOW);

  // PWM audio + 36.6 kHz wrap-IRQ setup (shared)
  mod2::initAudioPwm(sliceAudio, sliceIRQ, on_pwm_wrap);

  // Trigger inputs
  pinMode(mod2::IN1_PIN, INPUT);       // IN1 - External trigger
  attachInterrupt(digitalPinToInterrupt(mod2::IN1_PIN), onTrigger, RISING);
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP); // Button
  attachInterrupt(digitalPinToInterrupt(mod2::BUTTON_PIN), onTrigger, FALLING);
}

void loop() {
  // LED pulse control
  if (ledOn && (millis() - ledOnTime >= 20)) {
    digitalWrite(mod2::LED_PIN, LOW);
    ledOn = false;
  }
}
