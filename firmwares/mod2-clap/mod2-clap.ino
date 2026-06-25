/* Clap

Description:
TR-808-style digital hand-clap: two short gated noise bursts (4 ms, 15 ms
apart) followed by an exponential noise tail (<=0.60 s). LED brightness
follows the envelope. The accent input drops level by 6 dB when HIGH.

Key Variables:
  A0 -> BPF Q (0.5-4.0; left=wide, right=narrow)
  A1 -> Decay time (20-200 ms)
  A2 -> BPF centre frequency (50 Hz-8 kHz, shared with CV)

      ╔═══════════╗
      ║   CLAP    ║
      ║ hand clap ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - BPF Q 0.5-4.0
      ║     Q     ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - decay 20-200 ms
      ║   DECAY   ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - BPF Fc 50 Hz-8 kHz
      ║   FREQ    ║
      ║           ║
      ║   (BTN)   ║   BTN (GPIO6) - manual trigger      
      ║    [·]    ║   LED (GPIO5) - envelope (PWM)
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - trigger (rising)
      ║ (o)   (o) ║   IN2 (GPIO0) - accent (HIGH = -6 dB)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - BPF Fc (shared POT3)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Clap firmware by Hagiwo
  - 1.1 LED envelope display
  - 1.2 Forked and refactored for maddie synths
  - 1.3 DSP extracted to ClapCore.h (shared with VCV Rack port)

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO MOD2 (Seeed Xiao RP2350)
*/

#include <Arduino.h>            // Standard Arduino core
#include "hardware/pwm.h"       // RP2040 PWM hardware access
#include "hardware/irq.h"       // IRQ helpers
#include <math.h>               // mathf / sin / cos etc.
#include <Mod2Common.h>         // Shared MOD2 pin map, PWM-audio setup and helpers
#include <ClapCore.h>           // Shared clap voice (also used by the VCV Rack port)

/**********************  === Core constants ===  *****************************/
const float    SYS_CLOCK = 150000000.0f;      // RP2040 system clock (150 MHz)
const float    AUDIO_FS  = SYS_CLOCK / 4096.0f; // ≈36.6 kHz playback ISR rate
const uint32_t TABLE_SZ  = 22000;             // 22 k-sample buffer ≈0.60 s

// PWM output (10-bit, centre-aligned)
const float    PWM_FS   = 1023.0f;            // 10-bit full-scale value
const float    PWM_MID  = PWM_FS / 2.0f;      // Mid-scale (silence)

/**********************  === ADC setup ===  **********************************/
const uint8_t  ADC_RES_BITS = 10;             // 10-bit ADC resolution
const uint16_t ADC_MAX_VAL  = (1 << ADC_RES_BITS) - 1;

/**********************  === Buffers / state ===  ****************************/
uint16_t finalTbl[TABLE_SZ];         // Rendered PWM audio samples
uint16_t envTbl[TABLE_SZ];           // Envelope values for LED brightness

volatile bool     playingClap = false; // Playback state flag
volatile uint32_t idxClap     = 0;     // Current playback index

// Shared synthesis core (also used by vcvrack/src/Clap.cpp)
sc::ClapCore clapCore;

volatile float    decayMs     = 110.0f; // 20-200 ms decay time (pot A1)
volatile float    bpfQ        = 2.25f;  // 0.5-4.0 BPF Q factor (pot A0)
volatile float    fc          = 1500.0f;// 50-8000 Hz centre freq (pot A2)
volatile float    volFactor   = 1.0f;   // Accent volume (1.0 normal / 0.5 soft)
volatile bool     reqTrig     = false;  // Trigger request signalled by ISRs

uint sliceAudio, sliceIRQ, sliceLED;   // PWM slice indices

/**********************  === Helper ===  *************************************/
inline uint16_t readADC(uint8_t pin){ return analogRead(pin); }

/**********************  === PWM ISR ===  *************************************/
void on_pwm_wrap(){                      // Called at ≈36.6 kHz from sliceIRQ
  pwm_clear_irq(sliceIRQ);              // Clear IRQ flag

  if(!playingClap){
    pwm_set_chan_level(sliceAudio, PWM_CHAN_B, uint16_t(PWM_MID));
    pwm_set_chan_level(sliceLED, PWM_CHAN_B, 0);  // LED off
    return;
  }

  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, finalTbl[idxClap]);
  pwm_set_chan_level(sliceLED,   PWM_CHAN_B, envTbl[idxClap]);

  idxClap++;
  if(idxClap >= TABLE_SZ){
    playingClap = false;
    idxClap     = 0;
    pwm_set_chan_level(sliceLED, PWM_CHAN_B, 0);  // Ensure LED is off
  }
}

/**********************  === ISRs ===  *****************************************/
void triggerISR(){
  volFactor = digitalRead(mod2::IN2_PIN) ? 0.5f : 1.0f;
  reqTrig   = true;
}

void manualButtonISR(){
  volFactor = digitalRead(mod2::IN2_PIN) ? 0.5f : 1.0f;
  reqTrig   = true;
}

/**********************  === SETUP ===  ****************************************/
void setup(){
  analogReadResolution(ADC_RES_BITS);

  /* --- PWM audio + wrap-IRQ setup (shared) --- */
  mod2::initAudioPwm(sliceAudio, sliceIRQ, on_pwm_wrap);

  /* --- LED PWM output pin (GPIO5) --- */
  sliceLED = mod2::initPwmOutput10bit(mod2::LED_PIN);

  /* --- GPIO configuration --- */
  pinMode(mod2::IN2_PIN, INPUT);
  pinMode(mod2::IN1_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(mod2::IN1_PIN), triggerISR, RISING);
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(mod2::BUTTON_PIN), manualButtonISR, FALLING);
}

/**********************  === LOOP ===  *****************************************/
void loop(){
  /* --- Read potentiometers once per ms (loop runs ≈1 kHz) --- */
  float rawA0 = readADC(A0) / float(ADC_MAX_VAL);         // Q control (0→1)
  float rawA1 = readADC(A1) / float(ADC_MAX_VAL);         // Decay time control
  float norm2 = 1.0f - (readADC(A2) / float(ADC_MAX_VAL));// Fc inverted mapping

  bpfQ    = 0.5f + 3.5f * rawA0;                          // Map to 0.5-4.0
  decayMs = 20.0f + 180.0f * rawA1;                       // 20-200 ms
  fc      = 50.0f + 7950.0f * norm2;                      // 50 Hz-8 kHz

  /* --- If any ISR requested a trigger, render & start playback --- */
  if(reqTrig){
    reqTrig = false;

    // Render the clap into the PWM table via the shared core.
    // The core generates noise internally (xorshift32) and applies BPF +
    // burst/tail envelopes. volFactor (accent) scales the audio output.
    float vol = volFactor;
    clapCore.strike(decayMs, fc, bpfQ, AUDIO_FS);
    const float dt = 1.0f / AUDIO_FS;
    for(uint32_t i = 0; i < TABLE_SZ; ++i){
      sc::ClapFrame f = clapCore.process(dt);
      finalTbl[i] = uint16_t((f.audio * vol + 1.0f) * PWM_MID);
      envTbl[i]   = uint16_t(f.env * PWM_FS);
    }

    idxClap     = 0;
    playingClap = true;
  }

  delayMicroseconds(500);                                  // ≈1 kHz main loop
}
