/*
HAGIWO MOD2 Kick Ver1.2 - WITH PICKUP FEATURE
Sin wave base , 6 parameters kick drum.
Pressing the button will change the assigned parameter.
Pickup feature prevents value jumping when switching modes.

--Pin assign---
POT1  A0  Pitch | Start freq
POT2  A1  Soft clip rate | End freq
POT3  A2  Amp envelope | Pitch envelope
IN1   D7  Clock in
IN2   D0  Accent (Volume decreases when HIGH)
CV    A2  Shared with POT3
OUT   D11 Audio output
BUTTON    Change assign parameters
LED       Assign parameters
EEPROM    Record parameters when a button is pressed

CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial purposes, all without asking permission.

[History]
v1.2  - Add: Pickup feature for smooth parameter transitions
v1.1  - Fix: EEPROM-related malfunction
v1.0  - Init: Initial release
*/

#include <Arduino.h>
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include <math.h>
#include <EEPROM.h>  // RP2350 Arduino core allows using on‑board flash as EEPROM
#include <Mod2Common.h>  // Shared MOD2 pin map, PWM-audio setup and helpers

/* --------------------------------------------------
   System configuration
   --------------------------------------------------
   ‑ All timing / scaling constants are collected here
-------------------------------------------------- */
const float sys_clock = 150000000.0;                              // System clock (Hz)
const float T = 0.3;                                              // Kick playback duration (seconds)
const float baseIncrement = (2048.0 * 4096.0) / (T * sys_clock);  // Phase increment per PWM cycle
const float dt = T / 2048.0;                                      // Sample period derived from table size
const float FULL_SCALE = 1023.0;                                  // 10‑bit full‑scale value
const float MID_LEVEL = FULL_SCALE / 2.0;                         // Mid‑level (silence)

// Pickup feature constants
const int POT_SMOOTH_SAMPLES = 4;      // Number of samples for averaging

/* Flag set by GATE input to reduce level by 50 % */
bool reduce_state = 0;

/* --------------------------------------------------
   Wavetable buffers
-------------------------------------------------- */
uint16_t kickTable[2048];   // Pure sine wave (2048 samples)
uint16_t finalTable[2048];  // Post‑processed table (soft‑clipped & faded)

/* --------------------------------------------------
   PWM slice numbers (filled in at runtime)
-------------------------------------------------- */
uint slice_num1;
uint slice_num2;

/* --------------------------------------------------
   Playback control flags & parameters
-------------------------------------------------- */
volatile bool kickPlaying = false;     // TRUE while the kick is being output
volatile float kickPhase = 0.0;        // Fractional index into the wavetable
volatile float pitchMultiplier = 1.0;  // Real‑time pitch scaling (0.5‑2.0)
volatile float softClipRate = 1.0;     // Soft‑clip strength

/* --------------------------------------------------
   Pitch‑envelope curve LUTs
-------------------------------------------------- */
#define NUM_CURVES 32
const int LUT_SIZE = 256;
float pitchEnvLUTs[NUM_CURVES][LUT_SIZE];  // [curve][0‑255]
volatile uint8_t selectedCurve = 0;        // Active curve index

/* --------------------------------------------------
   Frequency parameters (modifiable via CV)
-------------------------------------------------- */
float f0 = 250.0;                                // Start frequency (Hz)
float f1 = 50.0;                                 // End   frequency (Hz)
float decayRate = 1.0 + 9.0 * (300.0 / 1023.0);  // Decay rate (1‑10)

/* ==================================================
   ★ Piecewise‑linear interpolation settings
   --------------------------------------------------
   More SEGMENTS → higher accuracy / lower speed / higher RAM
   We use 8 segments: smooth enough & lightweight
   ================================================== */
#define SEGMENTS 8
float ratioLUT[SEGMENTS + 1];  // Stores ratio at each segment edge (re‑calculated)

/* --------------------------------------------------
   Pickup Feature Data Structure
   (shared implementation lives in Mod2Common)
-------------------------------------------------- */
using ParameterData = mod2::PickupParam;

// Structure to hold all 6 parameters
struct {
  ParameterData pitchMult;      // Mode 0, POT1
  ParameterData softClip;       // Mode 0, POT2
  ParameterData decay;          // Mode 0, POT3
  ParameterData startFreq;      // Mode 1, POT1
  ParameterData endFreq;        // Mode 1, POT2
  ParameterData curve;          // Mode 1, POT3
} paramData;

// Pot smoothing (shared circular-buffer averager)
mod2::PotSmoother<POT_SMOOTH_SAMPLES> pot1Smoother;
mod2::PotSmoother<POT_SMOOTH_SAMPLES> pot2Smoother;
mod2::PotSmoother<POT_SMOOTH_SAMPLES> pot3Smoother;

/* --------------------------------------------------
   PWM wrap interrupt: performs linear interpolation and
   writes the sample to the PWM channel each PWM cycle.
-------------------------------------------------- */
void on_pwm_wrap() {
  pwm_clear_irq(slice_num2);  // Clear IRQ flag

  /* Idle state: keep output at mid‑level (= silence) */
  if (!kickPlaying) {
    pwm_set_chan_level(slice_num1, PWM_CHAN_B, (uint16_t)MID_LEVEL);
    return;
  }

  /* Effective phase increment after pitch modulation */
  float currInc = baseIncrement * pitchMultiplier;

  /* --- Linear interpolation between two table samples --- */
  float index = kickPhase;
  uint16_t idx = (uint16_t)index;
  float frac = index - idx;
  uint16_t s1 = finalTable[idx];
  uint16_t s2 = finalTable[(idx + 1) % 2048];
  float interp_sample = s1 * (1.0f - frac) + s2 * frac;
  pwm_set_chan_level(slice_num1, PWM_CHAN_B, (uint16_t)interp_sample);

  /* --- Advance phase ------------------------------- */
  kickPhase += currInc;
  if (kickPhase >= 2048.0f) {  // End of table → stop playback
    kickPlaying = false;
    kickPhase = 0.0f;
    pwm_set_chan_level(slice_num1, PWM_CHAN_B, (uint16_t)MID_LEVEL);
  }
}

/* --------------------------------------------------
   Wavetable generation
   Uses the selected curve LUT to shape the frequency
   sweep between f0 and f1, then fills kickTable.
-------------------------------------------------- */
void make_wavetable() {
  float reduce_level = 1 - (reduce_state * 0.5f);  // -6 dB when GATE LOW

  /* --- Recalculate ratioLUT each time f0/f1 changes --- */
  float ratio = f1 / f0;
  for (int i = 0; i <= SEGMENTS; i++) {
    float t = float(i) / SEGMENTS;  // 0, 1/8, 2/8, ... 1
    ratioLUT[i] = powf(ratio, t);   // Exact ratio at segment edge
  }

  float phase = 0.0f;
  for (int i = 0; i < 2048; i++) {
    /* Normalised position 0‑1 across the table */
    float x = float(i) / 2047.0f;

    /* Transform x using the envelope curve LUT */
    int lutIdx = int(x * (LUT_SIZE - 1));
    float x_adj = pitchEnvLUTs[selectedCurve][lutIdx];

    /* --- Piecewise‑linear interpolation of ratio --- */
    float segF = x_adj * SEGMENTS;
    int seg = int(segF);
    if (seg >= SEGMENTS) seg = SEGMENTS - 1;  // Safety clamp
    float segFrac = segF - seg;
    float ratio_pow = ratioLUT[seg] * (1.0f - segFrac) + ratioLUT[seg + 1] * segFrac;

    /* Frequency at this sample */
    float f = f0 * ratio_pow;

    /* Phase advance (keep phase 0 at i == 0) */
    if (i > 0) phase += 2.0f * PI * f * dt;

    /* Map sine value to 0‑FULL_SCALE (10‑bit) */
    float sample = sinf(phase) * reduce_level;  // -1.0…+1.0
    kickTable[i] = uint16_t((sample + 1.0f) * (FULL_SCALE / 2.0f));
  }
}

/* --------------------------------------------------
   Initialize parameter data
-------------------------------------------------- */
void initParameterData() {
  // Initialize all parameters with default values
  paramData.pitchMult.value = 1.0f;
  paramData.pitchMult.pickupActive = false;
  paramData.pitchMult.lastPotValue = 0.5f;
  
  paramData.softClip.value = 1.0f;
  paramData.softClip.pickupActive = false;
  paramData.softClip.lastPotValue = 0.0f;
  
  paramData.decay.value = 5.0f;
  paramData.decay.pickupActive = false;
  paramData.decay.lastPotValue = 0.444f;
  
  paramData.startFreq.value = 250.0f;
  paramData.startFreq.pickupActive = false;
  paramData.startFreq.lastPotValue = 0.243f;
  
  paramData.endFreq.value = 50.0f;
  paramData.endFreq.pickupActive = false;
  paramData.endFreq.lastPotValue = 0.094f;
  
  paramData.curve.value = 0;
  paramData.curve.pickupActive = false;
  paramData.curve.lastPotValue = 0.0f;
}

/* --------------------------------------------------
   SETUP
   ‑ Initialises EEPROM, LUTs, wavetables and PWM
-------------------------------------------------- */
void setup() {
  // Initialize parameter data
  initParameterData();
  
  EEPROM.begin(128);  // Reserve 128 bytes of flash for settings

  // Load saved values with validation
  float temp;
  
  EEPROM.get(0, temp);
  if (!isnan(temp) && temp >= 0.5f && temp <= 2.0f) {
    paramData.pitchMult.value = temp;
    pitchMultiplier = temp;
  } else {
    pitchMultiplier = paramData.pitchMult.value;
  }
  
  EEPROM.get(4, temp);
  if (!isnan(temp) && temp >= 0.5f && temp <= 10.0f) {
    paramData.softClip.value = temp;
    softClipRate = temp;
  } else {
    softClipRate = paramData.softClip.value;
  }
  
  EEPROM.get(8, temp);
  if (!isnan(temp) && temp >= 1.0f && temp <= 10.0f) {
    paramData.decay.value = temp;
    decayRate = temp;
  } else {
    decayRate = paramData.decay.value;
  }
  
  EEPROM.get(12, temp);
  if (!isnan(temp) && temp >= 3.0f && temp <= 1026.0f) {
    paramData.startFreq.value = temp;
    f0 = temp;
  } else {
    f0 = paramData.startFreq.value;
  }
  
  EEPROM.get(16, temp);
  if (!isnan(temp) && temp >= 2.0f && temp <= 513.0f) {
    paramData.endFreq.value = temp;
    f1 = temp;
  } else {
    f1 = paramData.endFreq.value;
  }
  
  EEPROM.get(20, temp);
  if (!isnan(temp) && temp >= 0 && temp < NUM_CURVES) {
    paramData.curve.value = temp;
    selectedCurve = (uint8_t)temp;
  } else {
    selectedCurve = (uint8_t)paramData.curve.value;
  }

  /* --- Build 32 pitch‑envelope LUTs --------------- */
  const float curveMin = 0.1f;
  const float curveMax = 2.0f;
  const float step = (curveMax - curveMin) / float(NUM_CURVES - 1);
  for (int c = 0; c < NUM_CURVES; c++) {
    float curveVal = curveMin + step * c;
    for (int i = 0; i < LUT_SIZE; i++) {
      float x = float(i) / float(LUT_SIZE - 1);
      pitchEnvLUTs[c][i] = powf(x, curveVal);
    }
  }

  /* --- Initial wavetable & finalTable ------------- */
  make_wavetable();
  for (int i = 0; i < 2048; i++) finalTable[i] = kickTable[i];

  /* --- PWM audio + wrap-IRQ setup (shared) -------- */
  mod2::initAudioPwm(slice_num1, slice_num2, on_pwm_wrap);

  /* --- Trigger input (rising edge) ---------------- */
  pinMode(mod2::IN1_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(mod2::IN1_PIN), onTrigger, RISING);

  /* --- GATE input for level reduction ------------- */
  pinMode(mod2::IN2_PIN, INPUT);

  /* --- Mode switch & status LED ------------------- */
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // Tactile switch
  pinMode(mod2::LED_PIN, OUTPUT);           // LED

  // Initialize pot smoothing windows with current readings
  pot1Smoother.prime(A0);
  pot2Smoother.prime(A1);
  pot3Smoother.prime(A2);

  // Set initial pot values to prevent pickup on startup
  float p1 = analogRead(A0) / 1023.0f;
  float p2 = analogRead(A1) / 1023.0f;
  float p3 = analogRead(A2) / 1023.0f;
  paramData.pitchMult.lastPotValue = p1;
  paramData.softClip.lastPotValue = p2;
  paramData.decay.lastPotValue = p3;
  paramData.startFreq.lastPotValue = p1;
  paramData.endFreq.lastPotValue = p2;
  paramData.curve.lastPotValue = p3;
}

/* --------------------------------------------------
   LOOP
   ‑ Reads CVs / button and updates run‑time parameters
-------------------------------------------------- */
void loop() {
  /* --- Toggle edit mode with button --------------- */
  static bool selectMode = 1;  // 0: pitch/clip/decay, 1: f0/f1/curve
  static bool prevBtn = HIGH;
  static bool firstRun = true;  // Flag to ensure params update on first loop
  bool currBtn = digitalRead(mod2::BUTTON_PIN);
  
  if (prevBtn == HIGH && currBtn == LOW) {
    // Save current values before switching modes
    if (selectMode == 0) {
      // Leaving Mode 0, save Mode 0 values
      paramData.pitchMult.value = pitchMultiplier;
      paramData.softClip.value = softClipRate;
      paramData.decay.value = decayRate;
    } else {
      // Leaving Mode 1, save Mode 1 values
      paramData.startFreq.value = f0;
      paramData.endFreq.value = f1;
      paramData.curve.value = selectedCurve;
    }
    
    selectMode = !selectMode;
    digitalWrite(mod2::LED_PIN, selectMode ? HIGH : LOW);  // Show current mode on LED

    // Set up pickup targets for the new mode
    if (selectMode == 0) {
      // Entering Mode 0
      // Normalize target values to 0-1 range for pot comparison
      paramData.pitchMult.targetValue = (paramData.pitchMult.value - 0.5f) / 1.5f;
      paramData.pitchMult.pickupActive = true;
      
      paramData.softClip.targetValue = (paramData.softClip.value - 0.5f) / 9.5f;
      paramData.softClip.pickupActive = true;
      
      paramData.decay.targetValue = (paramData.decay.value - 1.0f) / 9.0f;
      paramData.decay.pickupActive = true;
    } else {
      // Entering Mode 1
      paramData.startFreq.targetValue = (paramData.startFreq.value - 3.0f) / 1023.0f;
      paramData.startFreq.pickupActive = true;
      
      paramData.endFreq.targetValue = (paramData.endFreq.value - 2.0f) / 510.5f;
      paramData.endFreq.pickupActive = true;
      
      paramData.curve.targetValue = paramData.curve.value / float(NUM_CURVES - 1);
      paramData.curve.pickupActive = true;
    }

    // Save all parameters to EEPROM
    EEPROM.put(0, paramData.pitchMult.value);
    EEPROM.put(4, paramData.softClip.value);
    EEPROM.put(8, paramData.decay.value);
    EEPROM.put(12, paramData.startFreq.value);
    EEPROM.put(16, paramData.endFreq.value);
    EEPROM.put(20, paramData.curve.value);
    EEPROM.commit();
  }

  prevBtn = currBtn;

  /* --- Read analog controls with pickup feature --- */
  if (selectMode == 0) {
    /* Mode 0: edit pitchMultiplier / softClipRate / decayRate */
    float pot1Val = pot1Smoother.read(A0);
    if (firstRun || mod2::checkPickup(paramData.pitchMult, pot1Val)) {
      pitchMultiplier = 0.5f + 1.5f * pot1Val;
      paramData.pitchMult.value = pitchMultiplier;
    } else {
      pitchMultiplier = paramData.pitchMult.value;  // Use stored value
    }

    float pot2Val = pot2Smoother.read(A1);
    if (firstRun || mod2::checkPickup(paramData.softClip, pot2Val)) {
      softClipRate = 0.5f + 9.5f * pot2Val;
      paramData.softClip.value = softClipRate;
    } else {
      softClipRate = paramData.softClip.value;  // Use stored value
    }

    float pot3Val = pot3Smoother.read(A2);
    if (firstRun || mod2::checkPickup(paramData.decay, pot3Val)) {
      decayRate = 1.0f + 9.0f * pot3Val;
      paramData.decay.value = decayRate;
    } else {
      decayRate = paramData.decay.value;  // Use stored value
    }

  } else {
    /* Mode 1: edit f0 / f1 / envelope curve index */
    float pot1Val = pot1Smoother.read(A0);
    if (firstRun || mod2::checkPickup(paramData.startFreq, pot1Val)) {
      f0 = pot1Val * 1023.0f + 3.0f;
      paramData.startFreq.value = f0;
    } else {
      f0 = paramData.startFreq.value;  // Use stored value
    }

    float pot2Val = pot2Smoother.read(A1);
    if (firstRun || mod2::checkPickup(paramData.endFreq, pot2Val)) {
      f1 = pot2Val * 510.5f + 2.0f;
      paramData.endFreq.value = f1;
    } else {
      f1 = paramData.endFreq.value;  // Use stored value
    }

    float pot3Val = pot3Smoother.read(A2);
    if (firstRun || mod2::checkPickup(paramData.curve, pot3Val)) {
      selectedCurve = min(NUM_CURVES - 1, int(pot3Val * NUM_CURVES));
      paramData.curve.value = selectedCurve;
    } else {
      selectedCurve = (uint8_t)paramData.curve.value;  // Use stored value
    }
  }
  
  firstRun = false;  // Clear first run flag
  delay(10);  // simple UI debounce / CPU breather
}

/* --------------------------------------------------
   External trigger (GATE 7) ISR
-------------------------------------------------- */
void onTrigger() {
  /* --- Start kick playback ------------------------ */
  kickPlaying = true;
  kickPhase = 0.0f;

  /* Read GATE 0 for optional level reduction */
  reduce_state = digitalRead(mod2::IN2_PIN);

  /* --- Critical section: rebuild tables ----------- */
  irq_set_enabled(PWM_IRQ_WRAP, false);  // Disable audio IRQ

  make_wavetable();  // Rebuild kickTable with new params

  /* --- Build finalTable (soft‑clip & fade‑out) ---- */
  const float invSamples = 1.0f / 2047.0f;
  const float expStep = expf(-decayRate * invSamples);
  float env = 1.0f;  // Exponential decay envelope

  const float halfScale = FULL_SCALE / 2.0f;
  const float invHalfScale = 2.0f / FULL_SCALE;
  const float clipNorm = 1.0f / tanhf(softClipRate);  // Normalise tanh() output

  const int fadeStart = int(2048 * 0.95f);  // Start 95 % into table
  const int fadeDenom = 2047 - fadeStart;
  const float invFadeDenom = 1.0f / fadeDenom;

  for (int i = 0; i < 2048; i++) {
    float bipolar = (kickTable[i] - MID_LEVEL) * invHalfScale;  // -1…+1
    float attenuated = bipolar * env;                           // Apply envelope
    float clipped = tanhf(softClipRate * attenuated) * clipNorm;
    float sampleOut = clipped * halfScale + MID_LEVEL;  // Back to 0‑FULL_SCALE

    /* --- Cosine fade‑out for the tail ------------- */
    if (i >= fadeStart) {
      float mu = (i - fadeStart) * invFadeDenom;  // 0‑1 inside fade
      float mu2 = (1.0f - cosf(mu * PI)) * 0.5f;  // Smooth cosine curve
      sampleOut = (1.0f - mu2) * sampleOut + mu2 * MID_LEVEL;
    }
    finalTable[i] = uint16_t(sampleOut);
    env *= expStep;  // Next envelope value
  }

  irq_set_enabled(PWM_IRQ_WRAP, true);  // Re‑enable audio IRQ
}
