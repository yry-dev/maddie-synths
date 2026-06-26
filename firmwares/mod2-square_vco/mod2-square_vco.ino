/* Square VCO

Description:
Square-wave oscillator with V/oct support, plus a sine LFO for vibrato and
chiptune-style octave toggling. Closed-form V/oct (2^(adc/1023 * 5)),
1.0-2.0x fine tune, 6-step octave selection, sine vibrato (~10 Hz, up to
±5%), and a 20 Hz octave-toggle chiptune mode. A2 carries the V/oct CV
(primary pitch).

Key Variables:
  A0 -> Fine tune (1.0-2.0x)
  A1 -> Octave selection (1-6)
  A2 -> V/oct CV (0..3.3V → 5 octave range)

      ╔═══════════╗
      ║  SQ VCO   ║
      ║ chiptune  ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - fine tune 1.0-2.0x
      ║   TUNE    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - octave 1-6
      ║  OCTAVE   ║
      ║           ║
      ║   (A2)    ║   CV  (A2)  - V/oct (0..3.3V = 5 octaves)
      ║  V/OCT    ║
      ║           ║
      ║    [·]    ║   LED (GPIO5) - chiptune mode
      ║   (BTN)   ║   BTN (GPIO6) - chiptune mode on/off
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - octave CV in (reserved)
      ║ (o)   (o) ║   IN2 (GPIO0) - reserved
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - V/oct
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio (~36.6 kHz)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Square Wave VCO
  - 1.1 Forked and refactored for maddie synths
  - 1.2 Refactored to use shared SquareVcoCore

License:
CC0 1.0 Universal (CC0 1.0) Public Domain Dedication

Hardware:
HAGIWO MOD2 (Seeed Xiao RP2350)
*/

#include <Arduino.h>
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include <math.h>
#include <Mod2Common.h>      // Shared MOD2 pin map, PWM-audio setup and helpers
#include <SquareVcoCore.h>   // Shared square-wave VCO core (also used by VCV Rack port)

/* ============================== Constants ============================== */
constexpr float FULL_SCALE   = 1023.0f;     // 10-bit PWM top
constexpr float SYS_CLK      = 150000000.0f;
constexpr int   PWM_WRAP_IRQ = 4095;
// Audio sample rate: 150 MHz / 4096 = ~36621.09375 Hz
constexpr float AUDIO_FS     = SYS_CLK / (PWM_WRAP_IRQ + 1);

/* ========================== Hardware Globals =========================== */
uint sliceAudio;
uint sliceIRQ;

/* ========================== Shared Core ================================ */
sc::SquareVcoCore core;

// Volatile bridge: loop() writes, ISR reads each sample
volatile float gFreqFactor  = 1.0f;
volatile int   gOctaveIndex = 0;
volatile float gVibDepth    = 0.02f;
volatile float gCvMult      = 1.0f;
volatile bool  gChiptuneOn  = false;

/* ========================== Button / LED ================================ */
constexpr int  BUTTON_PIN    = mod2::BUTTON_PIN;
constexpr int  LED_PIN       = mod2::LED_PIN;
int            lastButtonState        = HIGH;
unsigned long  buttonPreviousMillis   = 0;
constexpr unsigned long DEBOUNCE_DELAY = 50;
bool           chiptuneModeActive     = false;

/* ========================== Timing ===================================== */
unsigned long lastPotUpdate = 0;

/* =======================================================================
 * PWM Interrupt Service Routine
 * Refreshes core params from the volatile bridge, calls core.process(),
 * and writes the result to the PWM duty register.
 * ==================================================================== */
void __isr onPwmWrap() {
    // Sync parameters from loop() context
    core.freqFactor  = gFreqFactor;
    core.octaveIndex = gOctaveIndex;
    core.vibDepth    = gVibDepth;
    core.cvMult      = gCvMult;
    core.chiptuneOn  = gChiptuneOn;

    // Render one sample: -1..+1 → 0..1023 PWM duty
    const float audio = core.process(1.0f / AUDIO_FS);
    const auto  level = static_cast<uint16_t>((audio + 1.0f) * (FULL_SCALE * 0.5f));
    pwm_set_chan_level(sliceAudio, PWM_CHAN_B, level);

    pwm_clear_irq(sliceIRQ);
}

/* =======================================================================
 * Hardware / PWM Initialization
 * ==================================================================== */
void setup() {
    pinMode(A0, INPUT);
    pinMode(A1, INPUT);
    pinMode(A2, INPUT);
    pinMode(mod2::IN1_PIN, INPUT);
    pinMode(mod2::IN2_PIN, INPUT);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // Audio PWM + ~36.6 kHz wrap-IRQ (AUDIO_FS is constexpr so ISR is safe immediately)
    mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);

    lastPotUpdate = millis();
}

/* =======================================================================
 * Main Loop — Handle UI and Update Core Parameters
 * ==================================================================== */
void loop() {
    unsigned long currentMillis = millis();

    // --- Button debounce → chiptune toggle ---
    const int reading = digitalRead(BUTTON_PIN);
    if (currentMillis - buttonPreviousMillis > DEBOUNCE_DELAY) {
        if (reading == LOW && lastButtonState == HIGH) {
            chiptuneModeActive = !chiptuneModeActive;
            digitalWrite(LED_PIN, chiptuneModeActive ? HIGH : LOW);
            buttonPreviousMillis = currentMillis;
        }
    }
    lastButtonState = reading;
    gChiptuneOn = chiptuneModeActive;

    // --- Read pots every 10 ms ---
    if (currentMillis - lastPotUpdate >= 10) {
        lastPotUpdate = currentMillis;

        // POT1 (A0): fine tune 1.0-2.0x
        gFreqFactor = sc::squareVcoTune(analogRead(A0) / 1023.0f);

        // POT2 (A1): octave selection (6 steps)
        gOctaveIndex = sc::squareVcoOctaveIdx(analogRead(A1) / 1023.0f);

        // A2 (CV jack): V/oct — 0..1023 ADC maps to 5 octaves (replaces voctMap[])
        const float v01 = analogRead(A2) / 1023.0f;
        gCvMult = powf(2.0f, v01 * 5.0f);  // 1x at 0V, 32x at full scale

        // Vibrato depth: matches original firmware fixed value
        gVibDepth = 0.02f;
    }
}
