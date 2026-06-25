/* Claves

Description:
Simple claves voice. The waveform mixes sine and triangle at a variable
ratio; pitch follows V/oct. LED brightness follows the envelope when
triggered.

Key Variables:
  A0 -> Decay
  A1 -> Waveform (sine <-> triangle)
  A2 -> Pitch (V/oct, shared with CV)

      ╔═══════════╗
      ║  CLAVES   ║
      ║percussion ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - decay
      ║   DECAY   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - sine <-> triangle
      ║   WAVE    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - pitch (V/oct)
      ║   PITCH   ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - envelope (PWM)
      ║   (BTN)   ║   BTN (GPIO6) - manual trigger
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - trigger
      ║ (o)   (o) ║   IN2 (GPIO0) - N/A
      ║           ║
      ║ CV    OUT ║   CV  (A2)    - pitch V/oct (shared POT3)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Claves firmware by Hagiwo
  - 1.1 LED envelope display
  - 1.2 Forked and refactored for maddie synths

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
#include <ClavesVoice.h>  // Shared Claves voice (also used by the VCV Rack port)

/* ============================================================ */
/* ★ Basic constants                                            */
/* ============================================================ */
const float   SYS_CLOCK   = 150000000.0f;   // 150 MHz system clock (fixed on RP2040)
const uint16_t TABLE_SZ   = 8192;           // Sample table length (generated per trigger)

const float PWM_FS  = 1023.0f;              // 10‑bit PWM resolution (wrap value)
const float PWM_MID = PWM_FS / 2.0f;        // Mid‑scale for silence (50 % duty)

const float AUDIO_FS = SYS_CLOCK / 4096.0f; // ≈36.6 kHz effective sample rate

/* ---- CV input on A2 ---------------------------------------- */
#define  ADC_RES_BITS 10                         // Use 10‑bit resolution (1023 max)
#define  ADC_MAX_VAL  ((1 << ADC_RES_BITS) - 1)  // 1023
#define  ADC_AVG_CNT  16                         // Simple 16‑sample moving average

const float CV_FULL_V    = 5.0f;            // CV range 0‑5 V (1 V/Oct assumed)
const float PITCH_F0     = 50.0f;           // Base frequency at 5 V CV
const float PITCH_MAX_HZ = 1500.0f;         // Absolute frequency limit

/* ============================================================ */
/* ★ Global variables                                           */
/* ============================================================ */
uint16_t finalTbl[TABLE_SZ];   // 16‑bit PWM samples generated per strike
uint16_t envTbl[TABLE_SZ];     // Envelope values for LED brightness

volatile bool  playing  = false;     // Playback state flag
volatile uint16_t tblIdx = 0;        // Current playback index

// Shared synthesis core: renders the sine/triangle morph + decay envelope.
sc::ClavesVoice clavesVoice;

uint sliceAudio, sliceIRQ, sliceLED;           // PWM slice numbers

/* ============================================================ */
/* ★ Read ADC with 16‑sample averaging                          */
/* ============================================================ */
inline uint16_t readAnalogAvg(uint8_t pin) {
  uint32_t sum = 0;
  for (int i = 0; i < ADC_AVG_CNT; ++i) sum += analogRead(pin);
  return uint16_t(sum / ADC_AVG_CNT);       // 0‑1023 result
}

/* ============================================================ */
/* ★ PWM‑wrap ISR  (≈36.6 kHz sample rate)                      */
/*    Called every time sliceIRQ reaches its wrap value.        */
/* ============================================================ */
void on_pwm_wrap() {
  pwm_clear_irq(sliceIRQ);                   // Clear IRQ for next cycle

  if (!playing) {                           // Output mid‑scale when idle
    pwm_set_chan_level(sliceAudio, PWM_CHAN_B, uint16_t(PWM_MID));
    pwm_set_chan_level(sliceLED, PWM_CHAN_B, 0);  // LED off
    return;
  }

  // Set audio output
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, finalTbl[tblIdx]);
  
  // Set LED brightness based on envelope
  pwm_set_chan_level(sliceLED, PWM_CHAN_B, envTbl[tblIdx]);

  if (++tblIdx >= TABLE_SZ) {               // Stop after final sample
    playing = false;
    tblIdx  = 0;
    pwm_set_chan_level(sliceLED, PWM_CHAN_B, 0);  // Ensure LED is off
  }
}

/* ============================================================ */
/* ★ Trigger ISR (GPIO7 rising edge or GPIO6 falling edge)      */
/*    Generates a new sample table and starts playback.         */
/* ============================================================ */
void onTrigger() {
  /* ----- Stop audio IRQ & enter edit state ----------------- */
  playing = false;
  irq_set_enabled(PWM_IRQ_WRAP, false);

  /* ----- Allow CV to settle, then read knobs/CV ------------- */
  delay(5);   // Small wait to avoid ADC glitch (≈5 ms)

  const float decayRate = 1.0f + 9.0f * (analogRead(A0) / 1023.0f);  // 1‑10 range
  const float waveMorph =               analogRead(A1) / 1023.0f;    // 0‑1

  uint16_t adc   = ADC_MAX_VAL - readAnalogAvg(A2);       // Invert: 0 V → high pitch
  float cvV      = (adc / float(ADC_MAX_VAL)) * CV_FULL_V;
  float baseFreq = PITCH_F0 * powf(2.0f, cvV);            // 1 V/Oct mapping
  if (baseFreq > PITCH_MAX_HZ) baseFreq = PITCH_MAX_HZ;   // Clamp

  /* ----- Generate sample table via the shared Claves core --- */
  // The core renders one strike sample per audio-rate tick; over TABLE_SZ ticks
  // its strike window (kClavesStrikeDuration) spans exactly the table length.
  clavesVoice.strike(decayRate, waveMorph, baseFreq);
  const float dt = 1.0f / AUDIO_FS;
  for (uint16_t i = 0; i < TABLE_SZ; ++i) {
    const sc::ClavesFrame f = clavesVoice.process(dt);
    finalTbl[i] = uint16_t((f.audio + 1.0f) * (PWM_FS / 2.0f));  // −1..+1 → 0..1023
    envTbl[i]   = uint16_t(f.env * PWM_FS);                      // 0..1   → 0..1023
  }

  /* ----- Restart playback --------------------------------- */
  tblIdx  = 0;
  playing = true;
  irq_set_enabled(PWM_IRQ_WRAP, true);
}

/* ============================================================ */
/* ★ SETUP                                                     */
/*    Hardware initialisation runs once at boot.               */
/* ============================================================ */
void setup() {
  analogReadResolution(ADC_RES_BITS);        // 10‑bit ADC resolution

  /* --- Audio PWM + 36.6 kHz wrap-IRQ setup (shared) ----------------- */
  mod2::initAudioPwm(sliceAudio, sliceIRQ, on_pwm_wrap);

  /* --- LED PWM output pin (GPIO5) ----------------------------------- */
  sliceLED = mod2::initPwmOutput10bit(mod2::LED_PIN);  // GPIO5 = slice 2, channel B

  /* --- Trigger input (jack) on GPIO7, rising edge ------------------- */
  pinMode(mod2::IN1_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(mod2::IN1_PIN), onTrigger, RISING);

  /* --- Push‑button input on GPIO6, falling edge (active‑low) --------- */
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(mod2::BUTTON_PIN), onTrigger, FALLING);
}

/* ============================================================ */
/* ★ Main loop (empty)                                          */
/*    All real‑time work is done in ISRs.                       */
/* ============================================================ */
void loop() {
  /* Nothing here – trigger and playback handled by interrupts. */
}
