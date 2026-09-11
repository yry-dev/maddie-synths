/* Byte — bytebeat emitter, sixteen formulas

Description:
A bytebeat is a single C expression over a free-running 32-bit counter `t`
whose low eight bits are the audio sample — no oscillator, no envelope, no
filter, just arithmetic that happens to sound like music. Sixteen of them are
built in (the table lives in the shared core). POT3/CV picks the formula, POT1
scales the pitch, POT2 sets the level, and IN1 restarts the sequence.

Original Byte firmware by Sean Luke, ported from the AE Modular GRAINS module.
Upstream runs on Mozzi at a fixed 16384 Hz; the MOD2's audio ISR is ~36.6 kHz,
so this sketch runs the formula at its own selectable base rate and holds the
result in between — the same staircase the original fed its PWM.

The arithmetic lives in the shared core (firmwares/shared/SynthCore/src/
ByteCore.h), so this sketch and the VCV Rack port (rack-plugins/src/mod2-byte.cpp)
evaluate the identical expressions. This file keeps only the MOD2 hardware I/O:
ADC reads, the reset input, the button/LED logic, EEPROM persistence and the
PWM-audio ISR.

Deliberate changes from the original:
  - POT2 and POT3 swap roles. GRAINS has three CV jacks; the MOD2 has one, and
    it is wired to A2 alongside POT3. Putting the formula selector on A2 gives
    that jack upstream's own IN2 job ("Bytebeat CV") and makes CV-scanning the
    sixteen formulas — the most musical thing this module does — reachable.
    Level moves to POT2.
  - The base rate is selectable (8000 / 11025 / 16384 / 22050 Hz) on the button,
    saved to flash. Upstream wanted this and could not have it: "You can't
    really tune BYTE: to do so would require changing the sampling rate, and
    Mozzi doesn't make it easy ... Maybe later." 16384 Hz is the default and is
    upstream's rate exactly.
  - IN2 drives the auxiliary variable `x` as a gate (LOW = 0, HIGH = 255) rather
    than upstream's continuous IN3 CV — GPIO0 is not an ADC pin on the XIAO
    RP2350. None of the stock sixteen formulas read `x`; it is there for anyone
    who edits the table, exactly as upstream intended.
  - Shift counts of 32 or more resolve to 0 and division by zero yields 0.
    Several formulas shift by a count derived from `t` and one divides by a
    value that reaches zero; in C both are undefined, and the targets disagree.
    0 is what both avr-gcc and this board's Cortex-M33 produce, so this pins the
    behaviour rather than changing it.
  - Peaks clamp instead of wrapping. Upstream's gain reaches ~1.9x full scale at
    the top of the dial and let Mozzi's output stage wrap; on a signal this
    square, clamping reads as drive where a wrap reads as breakup.
  - The button adds a manual reset (long press); GRAINS has no button.

Key Variables:
  A0 -> Pitch scaling (16 steps; step 8 = upstream's default)
  A1 -> Output level
  A2 -> Bytebeat formula 1..16 (shared with CV)

      ╔═══════════╗
      ║   BYTE    ║
      ║ bytebeat  ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - pitch scaling
      ║   PITCH   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - level
      ║   LEVEL   ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - formula select (shared with CV)
      ║   BYTE    ║
      ║           ║
      ║   (BTN)   ║   BTN (GPIO6) - short=base rate, long=reset
      ║    [·]    ║   LED (GPIO5) - output level; blinks the new rate
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - reset (rising edge restarts t)
      ║ (o)   (o) ║   IN2 (GPIO0) - aux variable x (HIGH = 255)
      ║           ║
      ║ OUT   CV  ║   CV  (A2)    - formula select (shared POT3)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 BYTE firmware by Sean Luke (GRAINS, AE Modular)
  - 1.1 Ported to HAGIWO MOD2 for maddie synths; expression table and rate
        divider moved to the shared <ByteCore.h> (shared with the VCV Rack
        port), selectable base rate, reset input, flash persistence.

License:
Apache License 2.0. This is a port of third-party code: the original Byte
firmware is Copyright 2024 Sean Luke (sean@cs.gmu.edu), from the GRAINS
project (github.com/eclab/grains), Apache 2.0. Apache requires the license
notice to travel with the code and modified files to carry prominent notice
of changes — the notice lives beside this sketch as LICENSE.md, and the
"Deliberate changes from the original" list above is the notice of changes.
Keep both.

Hardware:
HAGIWO MOD2 (Seeed Xiao RP2350)
*/

#include <Arduino.h>
#include "hardware/pwm.h"
#include "hardware/irq.h"
#include <EEPROM.h>      // RP2350 Arduino core allows using on-board flash as EEPROM
#include <Mod2Common.h>  // Shared MOD2 pin map + PWM-audio setup
#include <ByteCore.h>    // Shared bytebeat engine (also used by the VCV Rack port)

// ── Shared engine ───────────────────────────────────────────────────────────
static sc::ByteVoice voice;

// The ISR renders one sample per tick; the core is rate-independent, so we hand
// it the MOD2 audio path's fixed dt (~36.6 kHz) and it clocks the bytebeat at
// whichever base rate is selected.
static const float AUDIO_DT = 1.0f / mod2::AUDIO_FS;

static uint sliceAudio, sliceTimer, sliceLED;

// ── Persistence ─────────────────────────────────────────────────────────────
// Only the base-rate choice is saved. Committing stalls audio for a few ms, so
// it waits until the setting has been still for a moment.
static const int EE_ADDR_RATE = 0;
static const uint32_t EE_SETTLE_MS = 1500;
static bool eeDirty = false;
static uint32_t eeDirtyMs = 0;

// ── Reset input (IN1) ───────────────────────────────────────────────────────
static volatile bool resetEdge = false;
static volatile uint32_t lastResetUs = 0;

static void onResetRise() {
  const uint32_t now = micros();
  if (now - lastResetUs < 1000) return;  // glitch filter
  lastResetUs = now;
  resetEdge = true;
}

// ── LED level follower ──────────────────────────────────────────────────────
// A bytebeat has no envelope, so the LED tracks a decaying peak of the output —
// bright on dense formulas, dim on sparse ones. Written by the ISR, read by
// loop(); a torn float here would cost at most one wrong LED frame.
static volatile float ledLevel = 0.0f;

// Restart request from loop(). voice.restart() zeroes `t` while the ISR does
// `t += increment` every sample — calling it from loop() risks the ISR's
// in-flight increment overwriting the reset (a lost update, so an occasional
// reset pulse would silently do nothing). Deferring the restart into the ISR
// makes `t` single-writer.
static volatile bool reqRestart = false;

// ── Audio PWM ISR (~36.6 kHz) ───────────────────────────────────────────────
void on_pwm_wrap() {
  pwm_clear_irq(sliceTimer);

  if (reqRestart) {
    voice.restart();
    reqRestart = false;
  }

  const sc::ByteFrame f = voice.process(AUDIO_DT);

  float pwmF = (f.audio * 0.5f + 0.5f) * mod2::PWM_AUDIO_WRAP;
  pwmF = sc::clampf(pwmF, 0.0f, (float)mod2::PWM_AUDIO_WRAP);
  pwm_set_gpio_level(mod2::OUT_PIN, (uint16_t)(pwmF + 0.5f));

  // Peak-hold with a slow release, so the LED reads as brightness rather than
  // as a 16 kHz flicker. No dither on the audio: quantisation noise is not
  // something a bytebeat needs protecting from.
  float lvl = ledLevel * 0.9995f;
  if (f.env > lvl) lvl = f.env;
  ledLevel = lvl;
}

// ── Button: short = next base rate, long = manual reset ─────────────────────
static void blinkRate(int count) {
  for (int i = 0; i <= count; i++) {
    pwm_set_gpio_level(mod2::LED_PIN, mod2::PWM_AUDIO_WRAP);
    delay(70);
    pwm_set_gpio_level(mod2::LED_PIN, 0);
    delay(90);
  }
}

static void handleButton() {
  static bool wasDown = false;
  static uint32_t btnDownMs = 0;
  static bool longFired = false;

  const bool down = (digitalRead(mod2::BUTTON_PIN) == LOW);  // active-low
  const uint32_t now = millis();

  if (down && !wasDown) {
    btnDownMs = now;
    longFired = false;
    wasDown = true;
  }
  else if (down && wasDown && !longFired && (now - btnDownMs) > 500) {
    reqRestart = true;  // long hold -> restart the bytebeat from t = 0
    longFired = true;
    blinkRate(0);
  }
  else if (!down && wasDown) {
    wasDown = false;
    if (!longFired && (now - btnDownMs) > 30) {
      voice.setRateIndex((voice.rateIndex + 1) % sc::kByteNumRates);
      eeDirty = true;
      eeDirtyMs = now;
      blinkRate(voice.rateIndex);  // 1..4 flashes = 8000 / 11025 / 16384 / 22050
    }
  }
}

// ── SETUP ────────────────────────────────────────────────────────────────────
void setup() {
  pinMode(mod2::IN1_PIN, INPUT_PULLDOWN);  // reset in
  pinMode(mod2::IN2_PIN, INPUT_PULLDOWN);  // aux variable x
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);

  analogReadResolution(10);

  voice.reset();

  // Restore the saved base rate; anything out of range means unwritten flash.
  EEPROM.begin(64);
  int savedRate = 0;
  EEPROM.get(EE_ADDR_RATE, savedRate);
  voice.setRateIndex((savedRate >= 0 && savedRate < sc::kByteNumRates)
                         ? savedRate
                         : sc::kByteDefaultRate);

  // Audio PWM + ~36.6 kHz wrap IRQ (shared), and the LED as a 10-bit PWM out.
  mod2::initAudioPwm(sliceAudio, sliceTimer, on_pwm_wrap);
  sliceLED = mod2::initPwmOutput10bit(mod2::LED_PIN);

  attachInterrupt(digitalPinToInterrupt(mod2::IN1_PIN), onResetRise, RISING);
}

// ── LOOP: controls, reset, LED, persistence ─────────────────────────────────
void loop() {
  handleButton();

  voice.setPitch(analogRead(mod2::POT1_PIN) / 1023.0f);
  voice.setLevel(analogRead(mod2::POT2_PIN) / 1023.0f);
  // POT3/A2 is physically shared with the CV jack, so one ADC read already
  // carries the summed formula selection.
  voice.setExpression(analogRead(mod2::POT3_PIN) / 1023.0f);

  // Upstream's IN3 is a continuous CV for `x`; GPIO0 has no ADC, so it is a
  // gate here — the two values `x` can take are its endpoints.
  voice.setAux(digitalRead(mod2::IN2_PIN) == HIGH ? 1.0f : 0.0f);

  if (resetEdge) {
    noInterrupts();
    resetEdge = false;
    interrupts();
    reqRestart = true;
  }

  pwm_set_gpio_level(mod2::LED_PIN,
                     (uint16_t)(sc::clampf(ledLevel, 0.0f, 1.0f) *
                                mod2::PWM_AUDIO_WRAP));

  // Commit the rate once the button has been still for a moment.
  if (eeDirty && (millis() - eeDirtyMs) > EE_SETTLE_MS) {
    const int rate = voice.rateIndex;
    EEPROM.put(EE_ADDR_RATE, rate);
    EEPROM.commit();
    eeDirty = false;
  }
}
