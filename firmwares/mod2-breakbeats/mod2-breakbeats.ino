/* Breakbeats

Description:
PCM break-beat playback module. An external trigger or short button press
starts playback from a selected slice; six pre-programmed start points are
selectable via POT3/CV. Loop mode retriggers at the end; a medium press
(>1 s) toggles loop, a long press (>=3 s) swaps between the two stored drum
samples. GPIO0 emits a 5 ms end-of-playback pulse. 10-bit PWM audio on GPIO1
(~36.6 kHz), RC-filter externally. Requires a generated sample.h
(see scripts/wav_to_sample.py).

Key Variables:
  A0 -> Playback speed (0.7-1.5x, CV)
  A1 -> Playback length (0.3-2.3 s, CV)
  A2 -> Slice select (6 start points, shared with CV)

      ╔═══════════╗
      ║ BREAKBEAT ║
      ║  sampler  ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - playback speed 0.7-1.5x
      ║   SPEED   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - playback length 0.3-2.3 s
      ║  LENGTH   ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - start point (6) / CV
      ║   SLICE   ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - loop active
      ║   (BTN)   ║   BTN (GPIO6) - short=trig, med=loop, long=swap
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - external trigger (rising)
      ║ (o)   (o) ║   IN2 (GPIO0) - end-of-playback pulse OUT (5 ms)
      ║           ║
      ║ CV    OUT ║   CV  (A2)    - slice select (shared POT3)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Break Beats firmware by Hagiwo
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
#include "hardware/gpio.h"
#include "sample.h"  // two sample sets + tables
#include <Mod2Common.h>  // Shared MOD2 pin map, PWM-audio setup and helpers

// ---------- Fixed‑point & PWM constants ----------
#define FRAC_BITS 12
#define FRAC_MASK ((1UL << FRAC_BITS) - 1)
#define SAMPLE_RATE 44100                                    // 44.1 kHz
const uint16_t PWM_FS = 1023;                                // 10‑bit full scale
const uint16_t PWM_MID = PWM_FS / 2;                         // DC offset
const uint16_t PWM_WRAP = 4095;                              // ~36.6 kHz interrupt rate
const uint32_t END_MARK_SAMPLES = (SAMPLE_RATE * 5) / 1000;  // 5 ms ≈ 220 samples

// Manual‑button timing thresholds
const uint32_t MODE_SWITCH_MS = 1000;    // medium press > 1 s
const uint32_t SAMPLE_SWITCH_MS = 3000;  // long press  ≥ 3 s

// ---------- Playback state ----------
volatile bool playing = false;       // true while audio is playing
volatile bool loopMode = false;      // false = 1‑shot, true = loop
volatile bool loopRetrig = false;    // request flag for loop restart
volatile uint8_t currentSample = 0;  // 0 or 1

volatile uint32_t tblAcc = 0;                    // fixed‑point play pointer (Q12)
volatile uint32_t tblStepFP = 1UL << FRAC_BITS;  // step size in Q12
volatile const uint8_t* curSample = sampleDatas[0];
volatile uint32_t curLen16 = sampleLens16[0];

uint sliceAudio, sliceIRQ;  // PWM slice numbers

// ---------- Manual‑button state ----------
bool btnPrev = true;    // previous state (true = released)
uint32_t btnStart = 0;  // press‑start timestamp

// ------------------------------------------------------------
// PWM wrap interrupt – audio output & pointer update
// ------------------------------------------------------------
void on_pwm_wrap() {
  pwm_clear_irq(sliceIRQ);

  if (!playing) {
    pwm_set_chan_level(sliceAudio, PWM_CHAN_B, PWM_MID);  // silence
    gpio_put(0, 0);                                       // GPIO0 LOW
    return;
  }

  uint32_t idx = tblAcc >> FRAC_BITS;  // integer part
  uint32_t frac = tblAcc & FRAC_MASK;  // fractional part (0–4095)

  // ---- End‑of‑sample check ----
  if (idx >= curLen16) {
    gpio_put(0, 0);  // ensure LOW at the end
    if (loopMode) {
      playing = false;  // let main loop retrigger
      loopRetrig = true;
      tblAcc = 0;
      pwm_set_chan_level(sliceAudio, PWM_CHAN_B, PWM_MID);
      return;
    } else {
      playing = false;
      tblAcc = 0;
      pwm_set_chan_level(sliceAudio, PWM_CHAN_B, PWM_MID);
      return;
    }
  }

  // ---- GPIO0 HIGH during final 5 ms ----
  gpio_put(0, (curLen16 - idx <= END_MARK_SAMPLES));

  // ---- Linear interpolation ----
  int16_t s1 = mod2::readPCM16LE(curSample, idx);
  int16_t s2 = (idx + 1 < curLen16) ? mod2::readPCM16LE(curSample, idx + 1) : 0;
  int16_t mix = mod2::lerpFixed(s1, s2, frac);
  uint16_t pwmVal = uint16_t(int32_t(mix) + 32768) >> 6;  // 16‑bit → 10‑bit
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, pwmVal);

  tblAcc += tblStepFP;
}

// ------------------------------------------------------------
// Quantise A2 CV to one of six start‑point indices
// ------------------------------------------------------------
uint8_t selectStartIndex() {
  uint16_t val = 1023 - analogRead(A2);  // invert so 0 V = high value
  if (val < 32) return 0;
  if (val < 248) return 1;
  if (val < 514) return 2;
  if (val < 720) return 3;
  if (val < 926) return 4;
  return 5;
}

// ------------------------------------------------------------
// (Re)start playback using current CV values & selected sample
// ------------------------------------------------------------
void startPlayback() {
  playing = false;
  tblAcc = 0;
  gpio_put(0, 0);
  delay(5);
  // --- Determine start point ---
  uint8_t startIdx = selectStartIndex();
  const long* stbl = startTables[currentSample];
  uint32_t startSample = stbl[startIdx];

  // --- Playback length (0.3–2.3 s) ---
  uint16_t rawLen = analogRead(A1);
  float durationSec = 0.3f + (rawLen / 1023.0f) * 2.0f;  // 2.0 s span
  uint32_t playSamples = uint32_t(durationSec * SAMPLE_RATE);

  uint32_t totalLen = sampleLens16[currentSample];
  if (startSample + playSamples > totalLen)
    playSamples = totalLen - startSample;

  // --- Set sample pointer ---
  curSample = sampleDatas[currentSample] + (startSample << 1);  // ×2 for bytes
  curLen16 = playSamples;

  // --- Playback speed (0.7–1.5×) ---
  uint16_t rawSpeed = analogRead(A0);
  float rate = 0.7f + (rawSpeed / 1023.0f) * 0.8f;
  tblStepFP = uint32_t(rate * float(1 << FRAC_BITS));

  playing = true;
}

// ------------------------------------------------------------
// External trigger ISR (GPIO7)
// ------------------------------------------------------------
void onExtTrigger() {
  startPlayback();
}

// ------------------------------------------------------------
void setup() {
  analogReadResolution(10);  // 10‑bit ADC

  // GPIO0 output (end‑of‑playback pulse)
  gpio_init(mod2::IN2_PIN);
  gpio_set_dir(mod2::IN2_PIN, GPIO_OUT);
  gpio_put(mod2::IN2_PIN, 0);

  // Loop‑mode LED
  pinMode(mod2::LED_PIN, OUTPUT);
  digitalWrite(mod2::LED_PIN, LOW);
  // Manual button (GPIO6, active‑low)
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);

  // PWM audio + 36.6 kHz wrap-IRQ setup (shared)
  mod2::initAudioPwm(sliceAudio, sliceIRQ, on_pwm_wrap);

  // External trigger (GPIO7)
  pinMode(mod2::IN1_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(mod2::IN1_PIN), onExtTrigger, RISING);
}

// ------------------------------------------------------------
void loop() {
  // ---- Manual button handling ----
  bool btnNow = (digitalRead(mod2::BUTTON_PIN) == LOW);  // active‑low
  uint32_t nowMs = millis();

  if (!btnPrev && btnNow) {  // pressed
    btnStart = nowMs;

  } else if (btnPrev && !btnNow) {  // released
    uint32_t dt = nowMs - btnStart;

    if (dt < MODE_SWITCH_MS) {
      // ① Short press: trigger
      startPlayback();

    } else if (dt < SAMPLE_SWITCH_MS) {
      // ② Medium press: toggle loop / 1‑shot
      loopMode = !loopMode;
      digitalWrite(mod2::LED_PIN, loopMode ? HIGH : LOW);

    } else {
      // ③ Long press: switch sample
      currentSample ^= 1;  // 0 ↔ 1
      if (playing)         // update immediately if currently playing
        startPlayback();
    }
  }
  btnPrev = btnNow;

  // ---- Loop retrigger ----
  if (loopMode && loopRetrig) {
    loopRetrig = false;
    startPlayback();
  }
}
