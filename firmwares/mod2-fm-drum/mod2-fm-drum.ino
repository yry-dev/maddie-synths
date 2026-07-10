/* FM Drum

Description:
Two-operator FM percussion voice. Three pots x two button-selected modes
give six parameters. The accent input cuts level by 6 dB when HIGH.
Wavetable synthesis (4096 samples/note) with click-free cosine fades
(2% in / 10% out). All six parameters are stored in flash (EEPROM
emulation); a pickup feature prevents value jumps when switching modes.

Key Variables:
  A0 -> Mode 0: Pitch        | Mode 1: Decay time
  A1 -> Mode 0: Operator ratio | Mode 1: Ratio envelope
  A2 -> Modulation index (both modes, shared with CV)

      ╔═══════════╗
      ║  FM DRUM  ║
      ║  FM perc  ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - Pitch | Decay time
      ║   PITCH   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - Op ratio | Ratio env
      ║   RATIO   ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - Modulation index
      ║   INDEX   ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - mode (ON = Mode 1)
      ║   (BTN)   ║   BTN (GPIO6) - mode toggle & save
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - trigger
      ║ (o)   (o) ║   IN2 (GPIO0) - accent (HIGH = -6 dB)
      ║           ║
      ║  OUT   CV ║   CV  (A2)    - mod index (shared POT3)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Init: initial release
  - 1.1 Fix: EEPROM-related malfunction
  - 1.2 Add: pickup feature for smooth parameter transitions
  - 1.3 Fix: validate EEPROM parameter reads
  - 1.4 Forked and refactored for maddie synths

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication
You can copy, modify, distribute and perform the work, even for commercial
purposes, all without asking permission.

Hardware:
HAGIWO MOD2 (Seeed Xiao RP2350)
*/

#include <Arduino.h>
#include "hardware/pwm.h"  // RP2040 hardware PWM register access
#include "hardware/irq.h"  // IRQ helpers
#include <math.h>
#include <EEPROM.h>  // on‑flash key/value storage
#include <Mod2Common.h>  // Shared MOD2 pin map, PWM-audio setup and helpers
#include <FmDrumCore.h>  // Shared FM-drum voice (also used by the VCV Rack port)

/**********************  Global compile‑time constants  **********************/
constexpr int TABLE_SIZE = 4096;                      // wavetable length
constexpr int TABLE_MASK = TABLE_SIZE - 1;            // for cheap modulo

const float SYS_CLOCK = 150'000'000.0f;                                  // RP2040 core clock (Hz)
const float NOTE_LEN = sc::kFmDrumNoteLen;                               // fixed note duration (s)
const float BASE_INC = (TABLE_SIZE * 4096.0f) / (NOTE_LEN * SYS_CLOCK);  // phase step per PWM tick
const float DT = NOTE_LEN / TABLE_SIZE;                                  // time per sample
const float FULL_SCALE = 1023.0f;                                        // 10‑bit PWM range
const float MID_LEVEL = FULL_SCALE / 2.0f;                               // mid‑rail => silence

// Pickup feature constants
const float PICKUP_THRESHOLD = 0.05f;  //  threshold for pot noise
const int POT_SMOOTH_SAMPLES = 5;      // Number of samples for averaging

/****************************  Real‑time parameters  *************************/
volatile float f0 = 200.0f;       // fundamental (Hz)
volatile float opRatio = 2.0f;    // operator frequency ratio
volatile float modIndex = 1.0f;   // modulation index   (1 … 10)
volatile float decayRate = 5.0f;  // amplitude decay rate
volatile float ratioEnv = 0.0f;   // ratio envelope depth
volatile float indexEnv = 0.3f;   // index envelope depth (fixed)

// Accent flag – updated on every trigger edge
volatile bool accentState = false;  // true = level × 0.5

/**************************  Playback state variables  **********************/
volatile bool noteOn = false;     // true while table is streamed
volatile float phase = 0.0f;      // fractional table index

/****************************  Wavetable buffers  ****************************/
uint16_t finalTable[TABLE_SIZE];  // clipped & tapered copy streamed by PWM

// Shared FM synthesis core: fills finalTable on each strike (one sample/tick).
sc::FmDrumVoice fmVoice;

/**********************  PWM slice numbers (set in setup)  *******************/
uint sliceAudio;  // GPIO1 – PWM channel B → audio
uint sliceTimer;  // GPIO2 – dummy PWM used only for IRQ timing

/**********************  Pickup Feature Data Structure  **********************/
// Shared implementation lives in Mod2Common.
using ParameterData = mod2::PickupParam;

// Structure to hold all 6 parameters
struct {
  ParameterData pitch;        // Mode 0, POT1
  ParameterData operatorRatio;// Mode 0, POT2
  ParameterData modIndexM0;   // Mode 0, POT3
  ParameterData decayTime;    // Mode 1, POT1
  ParameterData ratioEnvelope;// Mode 1, POT2
  ParameterData modIndexM1;   // Mode 1, POT3
} paramData;

// Pot smoothing (shared circular-buffer averager)
mod2::PotSmoother<POT_SMOOTH_SAMPLES> pot1Smoother;
mod2::PotSmoother<POT_SMOOTH_SAMPLES> pot2Smoother;
mod2::PotSmoother<POT_SMOOTH_SAMPLES> pot3Smoother;

/**************************  PWM wrap interrupt (audio ISR) ******************/
void on_pwm_wrap() {
  pwm_clear_irq(sliceTimer);  // acknowledge IRQ

  if (!noteOn) {  // idle → output mid‑rail
    pwm_set_chan_level(sliceAudio, PWM_CHAN_B, (uint16_t)MID_LEVEL);
    return;
  }

  // Linear interpolation between table samples
  float idx = phase;
  uint32_t i = (uint32_t)idx;
  float frac = idx - i;
  uint16_t s1 = finalTable[i];
  uint16_t s2 = finalTable[(i + 1) & TABLE_MASK];
  float y = s1 * (1.0f - frac) + s2 * frac;
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, (uint16_t)y);

  // advance phase; stop after one table pass
  phase += BASE_INC;
  if (phase >= (float)TABLE_SIZE) {
    noteOn = false;
    phase = 0.0f;
    pwm_set_chan_level(sliceAudio, PWM_CHAN_B, (uint16_t)MID_LEVEL);
  }
}

/************************  FM wavetable generation ***************************/
// Bake one strike into finalTable using the shared FM-drum core. The core
// renders one sample per audio-rate tick; over TABLE_SIZE ticks its strike
// window (NOTE_LEN) spans exactly the table length (dt/NOTE_LEN = 1/TABLE_SIZE).
// The returned audio already includes the FM tone, ratio/index/amplitude
// envelopes, tanh soft-clip and anti-click fades, with the accent applied.
void fillFinalTable() {
  fmVoice.setParams(f0, opRatio, modIndex, decayRate, ratioEnv, indexEnv,
                    accentState ? 0.5f : 1.0f);  // −6 dB accent when HIGH
  fmVoice.strike();
  for (int i = 0; i < TABLE_SIZE; ++i) {
    const sc::FmDrumFrame f = fmVoice.process(DT, NOTE_LEN);
    finalTable[i] = (uint16_t)((f.audio + 1.0f) * (FULL_SCALE / 2.0f));  // −1..+1 → 0..1023
  }
}

/************************  Initialize parameter data  ************************/
void initParameterData() {
  // Initialize all parameters with default values
  paramData.pitch.value = 200.0f;
  paramData.pitch.pickupActive = false;

  paramData.operatorRatio.value = 2.0f;
  paramData.operatorRatio.pickupActive = false;

  paramData.modIndexM0.value = 1.0f;
  paramData.modIndexM0.pickupActive = false;

  paramData.decayTime.value = 5.0f;
  paramData.decayTime.pickupActive = false;

  paramData.ratioEnvelope.value = 0.0f;
  paramData.ratioEnvelope.pickupActive = false;

  paramData.modIndexM1.value = 1.0f;
  paramData.modIndexM1.pickupActive = false;
}

/*******************************  Arduino setup  *****************************/
void setup() {
  // Initialize parameter data
  initParameterData();

  // --- restore parameters from flash --------------------------------------
  EEPROM.begin(64);

  float temp;

  EEPROM.get(0, temp);
  if (!isnan(temp) && temp >= 30.0f && temp <= 1200.0f) paramData.pitch.value = temp;
  f0 = paramData.pitch.value;

  EEPROM.get(4, temp);
  if (!isnan(temp) && temp >= 0.5f && temp <= 8.0f) paramData.operatorRatio.value = temp;
  opRatio = paramData.operatorRatio.value;

  EEPROM.get(8, temp);
  if (!isnan(temp) && temp >= 1.0f && temp <= 10.0f) paramData.modIndexM0.value = temp;
  paramData.modIndexM1.value = paramData.modIndexM0.value;
  modIndex  = paramData.modIndexM0.value;
  if (modIndex < 1.0f) modIndex = 1.0f;

  EEPROM.get(12, temp);
  if (!isnan(temp) && temp >= 0.5f && temp <= 10.0f) paramData.decayTime.value = temp;
  decayRate = paramData.decayTime.value;

  EEPROM.get(16, temp);
  if (!isnan(temp) && temp >= 0.0f && temp <= 1.0f) paramData.ratioEnvelope.value = temp;
  ratioEnv = paramData.ratioEnvelope.value;

  fillFinalTable();  // bake the initial (idle) table via the shared core

  // --- PWM audio + wrap-IRQ setup (shared) --------------------------------
  mod2::initAudioPwm(sliceAudio, sliceTimer, on_pwm_wrap);

  // --- GPIO setup ----------------------------------------------------------
  pinMode(mod2::IN1_PIN, INPUT);         // trigger input
  pinMode(mod2::IN2_PIN, INPUT);         // accent input (active HIGH)
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);  // mode button
  pinMode(mod2::LED_PIN, OUTPUT);        // mode LED

  // attach trigger ISR (inline lambda for brevity) -------------------------
  attachInterrupt(
    digitalPinToInterrupt(mod2::IN1_PIN), []() {
      noteOn = true;
      phase = 0.0f;
      accentState = digitalRead(mod2::IN2_PIN);  // sample accent pin

      // protect against audio ISR while rebuilding the table, then bake the
      // strike (FM tone + envelopes + soft-clip + fades + accent) via the core.
      irq_set_enabled(PWM_IRQ_WRAP, false);
      fillFinalTable();
      irq_set_enabled(PWM_IRQ_WRAP, true);  // resume audio ISR
    },
    RISING);

  // Initialize pot smoothing windows with current readings
  pot1Smoother.prime(A0);
  pot2Smoother.prime(A1);
  pot3Smoother.prime(A2);
}

/*****************************  Main loop (UI)  ******************************/
void loop() {
  static bool editMode = false;  // false: Mode0, true: Mode1
  static bool prevBtn = HIGH;
  bool currBtn = digitalRead(mod2::BUTTON_PIN);

  // --- handle button press -------------------------------------------------
  if (prevBtn == HIGH && currBtn == LOW) {
    // Save current values before switching modes
    if (!editMode) {
      // Leaving Mode 0, save Mode 0 values
      paramData.pitch.value = f0;
      paramData.operatorRatio.value = opRatio;
      paramData.modIndexM0.value = modIndex;
    } else {
      // Leaving Mode 1, save Mode 1 values
      paramData.decayTime.value = decayRate;
      paramData.ratioEnvelope.value = ratioEnv;
      paramData.modIndexM1.value = modIndex;
    }

    editMode = !editMode;       // toggle mode
    digitalWrite(mod2::LED_PIN, editMode);  // LED reflects current mode

    // Set up pickup targets for the new mode
    if (!editMode) {
      // Entering Mode 0
      // Normalize target values to 0-1 range for pot comparison
      paramData.pitch.targetValue = (paramData.pitch.value - 30.0f) / 1170.0f;
      paramData.pitch.pickupActive = true;

      paramData.operatorRatio.targetValue = (paramData.operatorRatio.value - 0.5f) / 7.5f;
      paramData.operatorRatio.pickupActive = true;

      paramData.modIndexM0.targetValue = 1.0f - ((paramData.modIndexM0.value - 1.0f) / 9.0f);
      paramData.modIndexM0.pickupActive = true;
    } else {
      // Entering Mode 1
      paramData.decayTime.targetValue = 1.0f - ((paramData.decayTime.value - 0.5f) / 9.5f);
      paramData.decayTime.pickupActive = true;

      paramData.ratioEnvelope.targetValue = 1.0f - paramData.ratioEnvelope.value;
      paramData.ratioEnvelope.pickupActive = true;

      paramData.modIndexM1.targetValue = 1.0f - ((paramData.modIndexM1.value - 1.0f) / 9.0f);
      paramData.modIndexM1.pickupActive = true;
    }

    // Save all parameters to EEPROM
    EEPROM.put(0, paramData.pitch.value);
    EEPROM.put(4, paramData.operatorRatio.value);
    EEPROM.put(8, paramData.modIndexM0.value);
    EEPROM.put(12, paramData.decayTime.value);
    EEPROM.put(16, paramData.ratioEnvelope.value);
    EEPROM.commit();
  }
  prevBtn = currBtn;

  // --- read potentiometers with pickup feature ----------------------------
  if (!editMode) {  //  Mode 0 : Pitch / Ratio / Index
    float pot1Val = pot1Smoother.read(A0);
    if (mod2::checkPickup(paramData.pitch, pot1Val, PICKUP_THRESHOLD)) {
      f0 = 30.0f + 1170.0f * pot1Val;
      paramData.pitch.value = f0;
    } else {
      f0 = paramData.pitch.value;  // Use stored value
    }

    float pot2Val = pot2Smoother.read(A1);
    if (mod2::checkPickup(paramData.operatorRatio, pot2Val, PICKUP_THRESHOLD)) {
      opRatio = 0.5f + 7.5f * pot2Val;
      paramData.operatorRatio.value = opRatio;
    } else {
      opRatio = paramData.operatorRatio.value;  // Use stored value
    }

    float pot3Val = pot3Smoother.read(A2);
    if (mod2::checkPickup(paramData.modIndexM0, pot3Val, PICKUP_THRESHOLD)) {
      modIndex = 1.0f + 9.0f * (1.0f - pot3Val);
      paramData.modIndexM0.value = modIndex;
    } else {
      modIndex = paramData.modIndexM0.value;  // Use stored value
    }

  } else {  //  Mode 1 : Decay / RatioEnv / Index
    float pot1Val = pot1Smoother.read(A0);
    if (mod2::checkPickup(paramData.decayTime, pot1Val, PICKUP_THRESHOLD)) {
      decayRate = 0.5f + 9.5f * (1.0f - pot1Val);
      paramData.decayTime.value = decayRate;
    } else {
      decayRate = paramData.decayTime.value;  // Use stored value
    }

    float pot2Val = pot2Smoother.read(A1);
    if (mod2::checkPickup(paramData.ratioEnvelope, pot2Val, PICKUP_THRESHOLD)) {
      ratioEnv = 1.0f - pot2Val;
      paramData.ratioEnvelope.value = ratioEnv;
    } else {
      ratioEnv = paramData.ratioEnvelope.value;  // Use stored value
    }

    float pot3Val = pot3Smoother.read(A2);
    if (mod2::checkPickup(paramData.modIndexM1, pot3Val, PICKUP_THRESHOLD)) {
      modIndex = 1.0f + 9.0f * (1.0f - pot3Val);
      paramData.modIndexM1.value = modIndex;
    } else {
      modIndex = paramData.modIndexM1.value;  // Use stored value
    }
  }

  delay(10);  // simple UI debounce / CPU breather
}
