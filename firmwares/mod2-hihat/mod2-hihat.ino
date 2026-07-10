/* HiHat

Description:
White/blue-noise hi-hat. LED brightness follows the envelope. The button
has a dual function: short press = manual trigger, long press (>500 ms) =
toggle noise type.

Key Variables:
  A0 -> Decay time
  A1 -> Decay curve
  A2 -> BPF frequency (shared with CV)

      ╔═══════════╗
      ║   HIHAT   ║
      ║   noise   ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - decay time
      ║   DECAY   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - decay curve
      ║   CURVE   ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - BPF frequency
      ║   FREQ    ║
      ║           ║
      ║   (BTN)   ║   BTN (GPIO6) - short=trig, long=noise type
      ║    [·]    ║   LED (GPIO5) - envelope (PWM)
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - trigger
      ║ (o)   (o) ║   IN2 (GPIO0) - accent (HIGH lowers volume)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - BPF freq (shared POT3)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 HiHat firmware by Hagiwo
  - 1.1 LED envelope display
  - 1.2 Forked and refactored for maddie synths
  - 1.3 Synthesis moved to the shared sc::HihatVoice core (also used by VCV Rack)

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
#include <HihatCore.h>   // Shared Hi-hat voice (also used by the VCV Rack port)

/********************  === Core constants ===  *******************************************/
const float    SYS_CLOCK = 150000000.0f;      // 150 MHz
const float    AUDIO_FS  = SYS_CLOCK / 4096;  // ≒36.6 kHz
const uint32_t TABLE_SZ  = 30000;             // Length of noise table

const float    PWM_FS    = 1023.0f;           // 10‑bit full‑scale
const float    PWM_MID   = PWM_FS / 2.0f;     // 511

/********************  === ADC ===  ******************************************************/
const uint8_t  ADC_RES_BITS = 10;
const uint16_t ADC_MAX_VAL  = (1 << ADC_RES_BITS) - 1;

/********************  === Buffers ===  **************************************************/
// The synthesis (noise, band-pass, envelope, fades, amplitude) now lives in the
// shared core. The firmware still bakes a strike into a table once per trigger
// (the core renders one sample per audio-rate tick, Claves pattern) and the ISR
// plays it back at the fixed ~36.6 kHz rate.
int16_t  outHH[TABLE_SZ];        // Output waveform buffer (already amplitude-scaled, ±PWM_MID)
uint16_t envTbl[TABLE_SZ];       // Envelope values for LED brightness

sc::HihatVoice hh;               // Shared Hi-hat synthesis core

/********************  === Playback state / control ===  *********************************/
volatile bool     playingHH   = false;
volatile uint32_t idxHH       = 0;

volatile float decayBase  = 5.0f;
volatile float decayCurve = 1.0f;
volatile float fc         = 500.0f;

volatile bool  reqTrig         = false;
volatile float volFactor       = 1.0f;

// --- Noise mode switching ---
volatile uint8_t noiseMode      = 0;   // 0:Blue / 1:White

// --- Button timing for dual function ---
volatile uint32_t buttonPressTime = 0;
volatile bool     buttonPressed = false;
volatile uint32_t lastButtonChange = 0;
const uint32_t    LONG_PRESS_MS = 500;  // 500ms for long press
const uint32_t    DEBOUNCE_MS = 20;     // 20ms debounce time

uint sliceAudio, sliceIRQ, sliceLED;

/********************  === Helpers ===  **************************************************/
inline uint16_t readADC(uint8_t pin){ return analogRead(pin); }

/********************  === PWM IRQ ===  **************************************************/
void on_pwm_wrap(){
  pwm_clear_irq(sliceIRQ);

  if(!playingHH){
    pwm_set_chan_level(sliceAudio,PWM_CHAN_B,uint16_t(PWM_MID));
    pwm_set_chan_level(sliceLED, PWM_CHAN_B, 0);  // LED off
    return;
  }

  // Audio output. outHH already carries the full amplitude chain from the core
  // (band-pass * env * body-fade * master/amp scale, clamped); only the accent
  // attenuation (volFactor) stays at playback time.
  int32_t mix = outHH[idxHH];
  int32_t val = PWM_MID + int32_t(mix * volFactor);
  if(val<0) val=0; else if(val>1023) val=1023;
  pwm_set_chan_level(sliceAudio,PWM_CHAN_B,uint16_t(val));

  // LED brightness based on envelope
  pwm_set_chan_level(sliceLED, PWM_CHAN_B, envTbl[idxHH]);

  idxHH++;
  if(idxHH>=TABLE_SZ){
    playingHH=false;
    idxHH=0;
    pwm_set_chan_level(sliceLED, PWM_CHAN_B, 0);  // Ensure LED is off
  }
}

/********************  === ISRs ===  *****************************************************/
// Trigger input ISR
void triggerISR(){
  volFactor = digitalRead(mod2::IN2_PIN) ? 0.5f : 1.0f; // Volume switch
  reqTrig = true;
}

// Button state change ISR (GPIO6)
void buttonISR(){
  uint32_t now = millis();
  if(now - lastButtonChange < DEBOUNCE_MS) return;  // Debounce
  lastButtonChange = now;

  if(digitalRead(mod2::BUTTON_PIN) == LOW){
    // Button pressed (falling edge)
    buttonPressed = true;
    buttonPressTime = now;
  } else {
    // Button released (rising edge)
    if(buttonPressed){
      buttonPressed = false;
      uint32_t pressDuration = now - buttonPressTime;

      if(pressDuration < LONG_PRESS_MS){
        // Short press - trigger sound
        volFactor = digitalRead(mod2::IN2_PIN) ? 0.5f : 1.0f;
        reqTrig = true;
      } else {
        // Long press - toggle noise mode
        noiseMode ^= 1;
      }
    }
  }
}

/********************  === SETUP ===  ****************************************************/
void setup(){
  analogReadResolution(ADC_RES_BITS);

  // Audio PWM + wrap-IRQ setup (shared)
  mod2::initAudioPwm(sliceAudio, sliceIRQ, on_pwm_wrap);

  // LED PWM output (GPIO5)
  sliceLED = mod2::initPwmOutput10bit(mod2::LED_PIN);  // GPIO5 = slice 2, channel B

  pinMode(mod2::IN2_PIN,INPUT);
  pinMode(mod2::IN1_PIN,INPUT);
  attachInterrupt(digitalPinToInterrupt(mod2::IN1_PIN),triggerISR,RISING);
  pinMode(mod2::BUTTON_PIN,INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(mod2::BUTTON_PIN),buttonISR,CHANGE);  // Detect both press and release
}

/********************  === LOOP ===  *****************************************************/
void loop(){
  float norm0 = 1.0f - (readADC(A0)/float(ADC_MAX_VAL));
  float norm1 = 1.0f - (readADC(A1)/float(ADC_MAX_VAL));
  float norm2 = 1.0f - (readADC(A2)/float(ADC_MAX_VAL));

  decayBase  = 0.1f +  9.0f * norm0;
  decayCurve = 0.2f +  5.0f * norm1;
  fc         = 100.0f + 15900.0f * norm2;

  if(reqTrig){
    reqTrig=false;
    // Bake one strike into the table via the shared core (one sample per
    // audio-rate tick); the ISR then plays it back at the fixed rate.
    hh.strike(decayBase, decayCurve, fc, (sc::HihatNoiseMode)noiseMode, AUDIO_FS);
    for(uint32_t i=0;i<TABLE_SZ;++i){
      sc::HihatFrame f = hh.process(1.0f/AUDIO_FS);
      outHH[i]  = int16_t(f.audio * PWM_MID);
      envTbl[i] = uint16_t(f.env * PWM_FS);
    }
    idxHH=0; playingHH=true;
  }
  delayMicroseconds(500);   // ≒1 kHz main loop rate
}
