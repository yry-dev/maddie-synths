/* VCO

Description:
Six-waveform VCO with PolyBLEP anti-aliasing and V/oct tracking. Waveforms:
0 Sine, 1 Triangle, 2 Square, 3 Saw, 4 FM-4x, 5 FM-2x. A 1-pole RC
low-pass softens residual PWM/alias noise. Coarse tune on POT2; a
negative-slope 1 V/oct CV on A2. The octave button cycles 0/+1/+2/+3.
Shared VcoCore drives synthesis (sample-rate independent); ISR-driven audio.

Key Variables:
  A0 -> Waveform select (Sin/Tri/Squ/Saw/FM-4x/FM-2x)
  A1 -> Coarse tune
  A2 -> Frequency / V-oct (shared with CV)

      ╔═══════════╗
      ║    VCO    ║
      ║oscillator ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - waveform select (6)
      ║   WAVE    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - coarse tune
      ║   TUNE    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - frequency / V-oct
      ║   FREQ    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - N/A
      ║   (BTN)   ║   BTN (GPIO6) - octave 0,+1,+2,+3
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - N/A
      ║ (o)   (o) ║   IN2 (GPIO0) - N/A
      ║           ║
      ║ CV    OUT ║   CV  (A2)    - V/oct (shared POT3)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 VCO firmware by Hagiwo
  - 1.1 Forked and refactored for maddie synths
  - 1.2 Ported DSP to shared VcoCore (no behaviour change)

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
#include <math.h>
#include <Mod2Common.h>  // Shared MOD2 pin map, PWM-audio setup and helpers
#include <VcoCore.h>     // Shared VCO voice (also used by vcvrack/src/VCO.cpp)

/* ============================== constants ============================== */
constexpr float SYS_CLK         = 150'000'000.0f; // 150 MHz default
constexpr int   PWM_WRAP_IRQ    = 4095;            // ≈36.6 kHz ISR rate
constexpr float FULL_SCALE      = 1023.0f;         // 10-bit PWM top
constexpr float MID_LEVEL       = FULL_SCALE / 2.0f;

/* base tuning range (front-panel pot A1) */
constexpr float TUNE_MIN_HZ     = 320.0f;          // 0 % of the pot
constexpr float TUNE_RANGE_HZ   =  90.0f;          // span 320 → 410 Hz

/* frequency-calibration factor (edit here after measuring) */
const float     TUNE_CAL        = 0.992f;          // 1.000 = no correction

/* ========================== hardware globals =========================== */
uint sliceAudio;  // PWM slice for audio out
uint sliceIRQ;    // PWM slice that triggers the ISR

float sampleRate  = 0.0f;  // ≈36.6 kHz, set in setup()

/* -------------------- shared DSP core --------------------------------- */
sc::VcoCore vco;

/* Volatile shadows: loop() writes, ISR reads.  Mirrors the original
   volatile phaseStep / waveSel pattern used before the shared core. */
volatile float   g_vcoFreq  = 320.0f;
volatile uint8_t g_vcoWave  = 0;

/* =======================================================================
 *  PWM interrupt service routine
 * ==================================================================== */
void __isr onPwmWrap()
{
  /* Pull the latest freq / wave from the main-loop shadows. */
  vco.freq      = g_vcoFreq;
  vco.waveIndex = g_vcoWave;

  /* Render one sample (-1..+1) from the shared core. */
  const float s = vco.process(1.0f / sampleRate);

  /* Map -1..+1 → 0..1023 and write to PWM duty cycle (10-bit). */
  const uint16_t pwmSample =
      static_cast<uint16_t>((s + 1.0f) * MID_LEVEL + 0.5f);
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, pwmSample);

  /* Clear IRQ flag. */
  pwm_clear_irq(sliceIRQ);
}

/* =======================================================================
 *  Hardware / PWM initialisation
 * ==================================================================== */
void setup()
{
  /* --- user inputs --------------------------------------------------- */
  pinMode(A0, INPUT);                             // wave select
  pinMode(A1, INPUT);                             // coarse tune
  pinMode(A2, INPUT);                             // 1 V/Oct CV
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);        // octave shift button

  /* --- audio PWM + ~36.6 kHz wrap-IRQ setup (shared) ---------------- */
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);

  /* --- compute sample rate from hardware constants ------------------- */
  sampleRate = SYS_CLK / (PWM_WRAP_IRQ + 1);
}

/* =======================================================================
 *  Main loop — handle UI and update frequency
 * ==================================================================== */
void loop()
{
  /* --- A0 : wave select (thresholds match VcoCore::vcoWaveSelect) ---- */
  g_vcoWave = sc::vcoWaveSelect(analogRead(A0) / 1023.0f);

  /* --- A1 : base frequency 320–410 Hz -------------------------------- */
  const float baseFreq =
      TUNE_MIN_HZ + TUNE_RANGE_HZ * analogRead(A1) / 1023.0f;

  /* --- A2 : external CV in octaves (negative slope, hardware wiring) - */
  const float cvOct =
      -(analogRead(A2) / 1023.0f) * 8.3f * (33.0f / 55.0f) * TUNE_CAL;

  /* --- push-button : 0 / +1 / +2 / +3 octave steps ------------------ */
  static int    lastBtn  = HIGH;
  static uint8_t octShift = 0;
  const int btn = digitalRead(mod2::BUTTON_PIN);
  if (lastBtn == HIGH && btn == LOW)
    octShift = (octShift + 1) & 3;
  lastBtn = btn;

  /* --- final frequency → volatile shadow for the ISR ----------------- */
  g_vcoFreq = baseFreq * powf(2.0f, octShift + cvOct);
}
