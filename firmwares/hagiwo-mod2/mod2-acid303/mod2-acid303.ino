/* Acid303 / Acidwalk303

Description:
A 303-ish bass voice plus a generative sequencer. POT1 morphs from a
locked 8-step pattern (CCW) through fully random / evolving pitch+rhythm
(mid) to a locked 16-step pattern (CW). The clock input advances the
sequence; the accent input forces accents. Transpose is quantized to
semitones and shared with CV. Earle Philhower Arduino-Pico core.

The voice and sequencer live in the shared core <Acid303Voice.h>, which is
also compiled into the VCV Rack port (rack-plugins/src/mod2-acid303.cpp) so
both stay bit-for-bit in step. This sketch is just the MOD2 hardware glue:
ADC reads, the PWM audio ISR, the clock-input interrupt, the button gestures
and the LED. All synthesis/sequencing decisions are the core's.

Key Variables:
  A0 -> Turing: randomness + length (8-step / random / 16-step)
  A1 -> Decay (amp decay + bite; accent extends decay)
  A2 -> Transpose (quantized semitones, shared with CV) + no-clock tempo

      ╔═══════════╗
      ║  ACID303  ║
      ║ acid bass ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - randomness + length
      ║  TURING   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - decay + bite
      ║   DECAY   ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - transpose (quantized)
      ║   TRANS   ║
      ║           ║
      ║   (BTN)   ║   BTN (GPIO6) - short=scale, double=wave, long=regen
      ║    [·]    ║   LED (GPIO5) - step / accent-slide blink
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - clock in (rising advances)
      ║ (o)   (o) ║   IN2 (GPIO0) - accent hold (HIGH = accent)
      ║           ║
      ║ OUT   CV  ║   CV  (A2)    - transpose (shared POT3)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 ACIDWALK303 generative 303 voice + sequencer
  - 1.1 Forked and refactored for maddie synths
  - 1.2 Voice + sequencer moved to the shared <Acid303Voice.h> core
        (shared with the VCV Rack port); sketch is now hardware glue only.

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
#include <Mod2Common.h>   // Shared MOD2 pin map + PWM-audio setup
#include <Acid303Voice.h> // Shared voice + sequencer (also used by the VCV Rack port)

// ── Shared synthesis/sequencer core ─────────────────────────────────────────
static sc::Acid303Voice voice;

// The audio ISR renders one sample per tick; the core is rate-independent, so
// we hand it the MOD2 audio path's fixed dt (~36.6 kHz).
static const float AUDIO_DT = 1.0f / mod2::AUDIO_FS;

static uint sliceAudio, sliceTimer, sliceLED;

// ── Clock input (rising edge advances the sequence) ─────────────────────────
static volatile uint32_t lastClockUs = 0;
static volatile bool clockEdge = false;

static void onClockRise() {
  uint32_t now = micros();
  if (now - lastClockUs < 1500) return;  // glitch filter
  lastClockUs = now;
  clockEdge = true;
}

// Dither PRNG state (ISR-only, so no locking). Seeded off a fixed constant; the
// exact stream is irrelevant, only that it is white and signal-independent.
static uint32_t ditherState = 0x2468ACEu;

// ── Audio PWM ISR (~36.6 kHz) ───────────────────────────────────────────────
// The core does everything: portamento, oscillator, envelopes, gate timing and
// (when free-running) the sequencer advance. We only clock it and write PWM.
void on_pwm_wrap() {
  pwm_clear_irq(sliceTimer);

  float s = voice.process(AUDIO_DT);            // -1..+1
  // Map to the 10-bit PWM range, then add ±1 LSB TPDF dither *before* truncating.
  // The dither decorrelates the quantisation error from the signal, so the
  // acid bass's decaying tail dissolves into faint hiss instead of gritty,
  // pitch-locked quantisation distortion. Clamp so dither can't wrap the count.
  float pwmF = (s * 0.5f + 0.5f) * mod2::PWM_AUDIO_WRAP + sc::tpdfDither(ditherState);
  pwmF = sc::clampf(pwmF, 0.0f, (float)mod2::PWM_AUDIO_WRAP);
  pwm_set_gpio_level(mod2::OUT_PIN, (uint16_t)(pwmF + 0.5f));
}

// ── Button: short = next scale, double = next wave, long-hold = regenerate ───
static void handleButton() {
  static bool wasDown = false;
  static uint32_t btnDownMs = 0;
  static uint32_t lastClickMs = 0;
  static int clickCount = 0;

  bool down = (digitalRead(mod2::BUTTON_PIN) == LOW);  // active-low
  uint32_t now = millis();

  if (down && !wasDown) {
    btnDownMs = now;
    wasDown = true;
  } else if (!down && wasDown) {
    uint32_t held = now - btnDownMs;
    wasDown = false;

    if (held > 520) {          // long hold -> regenerate pattern
      voice.regen();
      clickCount = 0;
      return;
    }

    if (now - lastClickMs < 350) clickCount++;
    else clickCount = 1;
    lastClickMs = now;

    if (clickCount == 2) {     // double click -> next waveform
      voice.cycleWave();
      clickCount = 0;
    }
  }

  // A lone click that never became a double-click selects the next scale.
  if (clickCount == 1 && (millis() - lastClickMs) > 380) {
    voice.cycleScale();
    clickCount = 0;
  }
}

// ── SETUP ────────────────────────────────────────────────────────────────────
void setup() {
  pinMode(mod2::IN1_PIN, INPUT_PULLDOWN);  // clock in
  pinMode(mod2::IN2_PIN, INPUT_PULLDOWN);  // accent hold
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);

  analogReadResolution(10);

  // Seed the core's deterministic PRNG from analog noise so each power-up gets
  // a different opening pattern, then generate it.
  uint32_t seed = (uint32_t)analogRead(A0) ^ ((uint32_t)analogRead(A1) << 10) ^ micros();
  voice.rng = seed ? seed : 0x1234567u;
  voice.regen();
  voice.stepIndex = 0;
  voice.triggerStep(0);
  voice.gateOpen = voice.pattern[0].gate;
  voice.gateVal = 1.0f;  // gate starts open on step 0

  // Audio PWM + ~36.6 kHz wrap IRQ (shared), and the LED as a 10-bit PWM out.
  mod2::initAudioPwm(sliceAudio, sliceTimer, on_pwm_wrap);
  sliceLED = mod2::initPwmOutput10bit(mod2::LED_PIN);

  attachInterrupt(digitalPinToInterrupt(mod2::IN1_PIN), onClockRise, RISING);
}

// ── LOOP: controls, clock handling, LED ──────────────────────────────────────
void loop() {
  handleButton();

  // Controls (POT3/A2 is physically shared between transpose and CV, so a
  // single ADC read already carries the summed transpose).
  voice.setTuring(analogRead(mod2::POT1_PIN) / 1023.0f);
  voice.decay01 = analogRead(mod2::POT2_PIN) / 1023.0f;

  int a2 = analogRead(mod2::POT3_PIN);
  voice.setTranspose(map(a2, 0, 1023, -12, +12));
  voice.internalInterval = map(a2, 0, 1023, 60, 220) / 1000.0f;  // no-clock tempo

  voice.accentHold = (digitalRead(mod2::IN2_PIN) == HIGH);

  // Free-run (internal tempo) when no external clock has arrived for >1.5 s.
  voice.freeRun = (micros() - lastClockUs) > 1500000UL;

  // External clock edge advances the sequence. When an external clock is
  // present freeRun is false, so the core never self-advances in the ISR and
  // this is the only stepping source (no double-advance race).
  if (clockEdge) {
    noInterrupts();
    clockEdge = false;
    interrupts();
    voice.clock();
  }

  // LED follows the amp envelope (pulses on each gated step).
  pwm_set_gpio_level(mod2::LED_PIN,
                     (uint16_t)(voice.lightLevel() * mod2::PWM_AUDIO_WRAP));
}
