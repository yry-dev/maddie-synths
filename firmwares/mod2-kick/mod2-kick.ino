/* Kick

Description:
Sine-wave kick drum with six parameters across two button-selected modes.
Pressing the button changes the assigned parameter set; a pickup feature
prevents value jumps when switching modes. Parameters are saved to flash
on button press.

The synthesis (sine + exponential pitch sweep + decay + soft-clip + tail fade)
lives in the shared core firmwares/shared/SynthCore/src/KickCore.h, which the
VCV Rack port (vcvrack/src/Kick.cpp) also uses. This sketch keeps all hardware
I/O: the dual-mode pot multiplexing, EEPROM persistence, the PickupParam smooth
transitions, the PWM audio path and the trigger ISR. On each trigger it renders
the kick into a table at the audio rate (the Claves pattern) and the PWM-wrap
ISR plays the table back at a fixed rate. (The old code instead built a 2048-pt
table and resampled it by pitchMultiplier in the ISR; folding pitchMult into the
core's synthesis is numerically equivalent — see KickCore.h.)

Key Variables:
  A0 -> Pitch        | Start frequency
  A1 -> Soft-clip rate | End frequency
  A2 -> Amp envelope  | Pitch envelope (shared with CV)

      ╔═══════════╗
      ║   KICK    ║
      ║   drum    ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - Pitch | Start freq
      ║   PITCH   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - Soft clip | End freq
      ║   CLIP    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - Amp env | Pitch env
      ║    ENV    ║
      ║           ║
      ║   (BTN)   ║   BTN (GPIO6) - change assigned params      
      ║    [·]    ║   LED (GPIO5) - assigned parameter
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - clock in
      ║ (o)   (o) ║   IN2 (GPIO0) - accent (HIGH lowers volume)
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - shared with POT3
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Init: initial release
  - 1.1 Fix: EEPROM-related malfunction
  - 1.2 Add: pickup feature for smooth parameter transitions
  - 1.3 Forked and refactored for maddie synths
  - 1.4 Synthesis moved to shared KickCore (VCV Rack port shares the voice)

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
#include <EEPROM.h>  // RP2350 Arduino core allows using on‑board flash as EEPROM
#include <Mod2Common.h>  // Shared MOD2 pin map, PWM-audio setup and helpers
#include <KickCore.h>    // Shared Kick voice (also used by the VCV Rack port)

/* --------------------------------------------------
   System configuration
-------------------------------------------------- */
const float sys_clock = 150000000.0;        // System clock (Hz)
const float AUDIO_FS = sys_clock / 4096.0f;  // ≈36.6 kHz wrap-IRQ sample rate
const float FULL_SCALE = 1023.0;            // 10‑bit full‑scale value
const float MID_LEVEL = FULL_SCALE / 2.0;   // Mid‑level (silence)

// Longest possible kick: pitchMult = 0.5 -> 0.3 s / 0.5 = 0.6 s of audio.
const int MAX_TABLE = 22050;  // > 0.6 s * AUDIO_FS, with margin

// Pickup feature constants
const int POT_SMOOTH_SAMPLES = 4;      // Number of samples for averaging

/* Flag set by accent (IN2) input to reduce level by 50 % */
bool reduce_state = 0;

/* --------------------------------------------------
   Pitch‑envelope curve range (selectedCurve index -> exponent)
-------------------------------------------------- */
#define NUM_CURVES 32
const float CURVE_MIN = 0.1f;
const float CURVE_MAX = 2.0f;

/* --------------------------------------------------
   Wavetable buffer (rendered per trigger by the core)
-------------------------------------------------- */
uint16_t finalTable[MAX_TABLE];        // Post‑processed samples (0..1023)
volatile uint16_t tableLen = 0;        // Valid samples in finalTable
volatile uint16_t playIdx = 0;         // Current playback index

/* Shared synthesis core (sine sweep + decay + soft-clip + tail fade) */
sc::KickVoice kickVoice;

/* --------------------------------------------------
   PWM slice numbers (filled in at runtime)
-------------------------------------------------- */
uint slice_num1;
uint slice_num2;

/* --------------------------------------------------
   Playback control flag
-------------------------------------------------- */
volatile bool kickPlaying = false;     // TRUE while the kick is being output
volatile bool reqKick = false;         // set by the trigger ISR; the bake runs in loop()
volatile float reqReduceLevel = 1.0f;  // accent level captured at the trigger edge

/* --------------------------------------------------
   Run‑time parameters (set by the dual-mode pot logic in loop())
-------------------------------------------------- */
volatile float pitchMultiplier = 1.0;  // Real‑time pitch scaling (0.5‑2.0)
volatile float softClipRate = 1.0;     // Soft‑clip strength
volatile uint8_t selectedCurve = 0;    // Active curve index (0..NUM_CURVES-1)
float f0 = 250.0;                       // Start frequency (Hz)
float f1 = 50.0;                        // End   frequency (Hz)
float decayRate = 1.0 + 9.0 * (300.0 / 1023.0);  // Decay rate (1‑10)

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

/* Convert the discrete curve index to the core's continuous exponent. */
static inline float curveExponent(uint8_t idx) {
  const float step = (CURVE_MAX - CURVE_MIN) / float(NUM_CURVES - 1);
  return CURVE_MIN + step * idx;
}

/* --------------------------------------------------
   PWM wrap interrupt: plays the rendered table back at a fixed rate
   (one sample per PWM cycle), writing to the PWM channel.
-------------------------------------------------- */
void on_pwm_wrap() {
  pwm_clear_irq(slice_num2);  // Clear IRQ flag

  /* Idle state: keep output at mid‑level (= silence) */
  if (!kickPlaying) {
    pwm_set_chan_level(slice_num1, PWM_CHAN_B, (uint16_t)MID_LEVEL);
    return;
  }

  pwm_set_chan_level(slice_num1, PWM_CHAN_B, finalTable[playIdx]);

  if (++playIdx >= tableLen) {  // End of table → stop playback
    kickPlaying = false;
    playIdx = 0;
    pwm_set_chan_level(slice_num1, PWM_CHAN_B, (uint16_t)MID_LEVEL);
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
   ‑ Initialises EEPROM, parameters and PWM
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

  /* --- PWM audio + wrap-IRQ setup (shared) -------- */
  mod2::initAudioPwm(slice_num1, slice_num2, on_pwm_wrap);

  /* --- Trigger input (rising edge) ---------------- */
  pinMode(mod2::IN1_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(mod2::IN1_PIN), onTrigger, RISING);

  /* --- Accent input for level reduction ----------- */
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

  /* --- Service a pending trigger: rebuild the kick table here, not in the
         ISR, so audio playback never stalls during the render. --- */
  if (reqKick) {
    reqKick = false;
    bakeKick();
  }

  delay(1);  // ~1 kHz loop: keeps trigger->bake latency ~1 ms (was 10 ms)
}

/* --------------------------------------------------
   External trigger (IN1) ISR
   ‑ Renders the kick into finalTable via the shared core, then
     restarts fixed-rate playback.
-------------------------------------------------- */
void onTrigger() {
  /* Keep the ISR minimal: capture the accent level and ask loop() to rebuild
     the table. The (potentially ~22k-sample) bake must NOT run here — doing it
     in the GPIO ISR with the audio IRQ disabled freezes output for the whole
     render. Deferring to loop() keeps the audio ISR live (mirrors clap/hihat). */
  reduce_state = digitalRead(mod2::IN2_PIN);
  reqReduceLevel = 1.0f - (reduce_state * 0.5f);  // 1.0 or 0.5
  reqKick = true;
}

/* Render a fresh kick into finalTable via the shared core. Runs in loop() so the
   audio ISR keeps running; this module simply goes silent (kickPlaying=false ->
   ISR emits mid-scale) for the few ms of the bake instead of stalling all audio. */
void bakeKick() {
  kickPlaying = false;  // silence THIS voice during the rebuild (no table tearing)

  kickVoice.setParams(pitchMultiplier, softClipRate, decayRate,
                      f0, f1, curveExponent(selectedCurve));
  kickVoice.strike(reqReduceLevel);

  const float dt = 1.0f / AUDIO_FS;
  uint16_t n = 0;
  while (n < MAX_TABLE) {
    sc::KickFrame fr = kickVoice.process(dt);
    finalTable[n] = (uint16_t)((fr.audio + 1.0f) * (FULL_SCALE / 2.0f));  // −1..+1 → 0..1023
    n++;
    if (!kickVoice.playing) break;
  }
  tableLen = n;

  /* --- Start playback ----------------------------- */
  playIdx = 0;
  kickPlaying = true;
}
