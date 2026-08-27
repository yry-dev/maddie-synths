/* Switchblade

Description:
A CV conditioner in one module: a summing attenuverter with a lag smoother and a
noise fuzzer sharing its shape dial. F1 arrives through the LEVEL attenuator and
F2 joins it at unity; the sum is de-glitched, then either lag-smoothed (SHAPE
CCW) or roughened with noise (SHAPE CW), then scaled and optionally flipped by
the attenuverter. F3 carries the result and F4 carries its mirror image about
mid-scale. The button swaps LEVEL between attenuating F1 and being a manual CV
level of its own, so the module is useful with nothing patched in.

Smoothing and fuzzing cannot happen at once — they share one dial. That is the
original module's central compromise and it is kept deliberately.

Original Switchblade firmware by Sean Luke, ported from the AE Modular GRAINS
module.

Key Variables:
  A0 -> Level (F1 attenuation, or a manual CV level in MAN mode)
  A1 -> Attenuvert (CCW inverted, centre silent, CW normal)
  A2 -> Shape (CCW lag smoothing <-> CW noise fuzz)

      ╔═══════════╗
      ║SWITCHBLADE║
      ║ cv shaper ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   LEVEL   — F1 attenuation / manual level
      ║   LEVEL   ║
      ║           ║
      ║   (A1)    ║   ±       — attenuvert: inverted..silent..normal
      ║     ±     ║
      ║           ║
      ║   (A2)    ║   SHAPE   — lag <-> fuzz
      ║   SHAPE   ║
      ║           ║
      ║    [·]    ║   LED (D3) — output level (flashes on mode change)
      ║   (BTN)   ║   BTN (D4) — LEVEL source: F1 / manual
      ║           ║
      ╠═══════════╣
      ║ F1     F2 ║   F1 (A3)  IN  — CV A (through LEVEL)
      ║ (o)   (o) ║   F2 (A4)  IN  — CV B (summed at unity)
      ║           ║
      ║ F3     F4 ║   F3 (D10) OUT — Processed CV (10-bit PWM)
      ║ (o)   (o) ║   F4 (D11) OUT — Inverted copy
      ║           ║
      ╚═══════════╝

Deliberate changes from the original:
  - GRAINS reads IN 1 pre-attenuated by POT 1 in hardware and sums IN 3 on top;
    MOD1 has no such switch matrix, so the attenuation is done in software and
    the two jacks become F1 (attenuated) and F2 (unity).
  - GRAINS's IN 2 attenuvert CV is dropped. Only A3/A4/A5 can read analog on
    MOD1, and F3/F4 are needed as outputs, so there is no jack left for it. The
    attenuverter is knob-only, and with it goes upstream's median/smoothing
    filter on that read — that filter existed to tame a CV-summed pot.
  - The button is GRAINS's own Man/In switch for POT 1, which MOD1's panel lacks:
    in MAN mode LEVEL is a manual CV level and F1 is ignored. It is not saved —
    the module powers up attenuating F1, the position upstream's header tells you
    to set the hardware switch to.
  - F4 (inverted output) is new. GRAINS has one output; MOD1 has a spare jack and
    an attenuverter is the obvious thing to want a mirror of.
  - Upstream's attenuverter has a bug in its inverting half: `(512*512) - input *
    (512 - a)` keeps the 512*512 term through the >>9, which pins the whole CCW
    half of POT 2 at maximum output. Fixed to the behaviour its own panel legend
    describes — one signed gain about mid-scale.
  - Upstream's fuzz is `in + random(width) - 256`, a fixed offset against a width
    that runs 0..511, so it is only centred at full CW and the dial's centre
    drops the signal a quarter of full scale. The noise is centred on its width
    instead, which restores the "Normal" the legend promises; the full-CW
    amplitude is unchanged at +/-256/1023.
  - Upstream's one-pole is primed with `if (oldIn == 0) oldIn = in`, which re-arms
    any time the state lands on zero. A flag does the intended job once.
  - Output resolution is raised. GRAINS is stuck with Mozzi's 488 steps and says
    so in its header; F3 here is Timer1 10-bit PWM at 15.6 kHz, matching the
    10-bit ADC that feeds it. F4's mirror is Timer2 8-bit at 62.5 kHz.

Version History:
  - 1.0 Switchblade firmware by Sean Luke for AE Modular GRAINS
  - 1.1 Ported to HAGIWO MOD1, DSP moved to SwitchbladeCore.h (shared with the
        VCV Rack port)

License:
Apache License 2.0. This is a port of third-party code: the original Switchblade
firmware is Copyright 2023 Sean Luke (sean@cs.gmu.edu), from the GRAINS project
(github.com/eclab/grains), Apache 2.0. Apache requires the license notice to
travel with the code and modified files to carry prominent notice of changes —
the notice lives beside this sketch as LICENSE.md, and the "Deliberate changes
from the original" list above is the notice of changes. Keep both.

Hardware:
HAGIWO MOD1
*/

#include <Arduino.h>
#include <Mod1Common.h>
#include <SwitchbladeCore.h>

sc::SwitchbladeVoice voice;

mod1::DebouncedInput buttonDebounce(50, HIGH);

// Mode-change acknowledgement: the LED normally shows the output, so a change of
// mode has to announce itself some other way. Full brightness for a moment does.
constexpr unsigned long kFlashMs = 150;
unsigned long flashUntil = 0;

// LED ceiling — the panel LED is bright enough that full scale is unpleasant.
constexpr int kLedBrightness = 160;

unsigned long lastMicros = 0;

void setup() {
  pinMode(mod1::PIN_BUTTON, INPUT_PULLUP);
  pinMode(mod1::PIN_CV1, INPUT);   // F1 - CV A
  pinMode(mod1::PIN_CV2, INPUT);   // F2 - CV B
  pinMode(mod1::PIN_F3, OUTPUT);   // Processed CV
  pinMode(mod1::PIN_F4, OUTPUT);   // Inverted copy
  pinMode(mod1::PIN_LED, OUTPUT);

  // Timer2 half of the shared logic-style setup gives us 62.5 kHz 8-bit PWM on
  // OC2A (D11/F4) and OC2B (D3/LED), which is all we want from it.
  mod1::setupFastPwmLogicStyle();

  // Timer1 is then re-pointed at 10-bit fast PWM (mode 14, TOP = ICR1) on OC1B
  // alone, which is D10/F3. Ten bits is what the signal path actually carries —
  // the pots and CV inputs are 10-bit reads — and the 15.6 kHz carrier that buys
  // is still far above anything a CV output needs to pass, and above the 7.8 kHz
  // that mod1-terrain-lfo already runs its F4 at. OC1A (D9/F2) is left
  // disconnected because F2 is an input here.
  TCCR1A = _BV(COM1B1) | _BV(WGM11);
  TCCR1B = _BV(WGM13) | _BV(WGM12) | _BV(CS10);
  ICR1 = 1023;
  OCR1B = 512;  // rest at mid-scale, the attenuverter's pivot

  voice.reset();
  // Seed the fuzz off the F1 input's power-on noise, so two modules on the same
  // rail do not fuzz in lockstep. F3 would read back our own PWM, so not that.
  voice.seed((uint32_t)analogRead(mod1::PIN_CV1) * 2654435761UL + 1UL);

  lastMicros = micros();
}

void loop() {
  const unsigned long nowMicros = micros();
  const unsigned long nowMillis = millis();

  // Real elapsed time, so the core's 256 Hz control tick lands where it should
  // regardless of how long the loop actually took.
  const float dt = (float)(nowMicros - lastMicros) * 1.0e-6f;
  lastMicros = nowMicros;

  const sc::SwitchbladeParams p = sc::switchbladeMapParams(
      analogRead(mod1::PIN_POT1) / 1023.0f,   // LEVEL
      analogRead(mod1::PIN_POT2) / 1023.0f,   // attenuvert
      analogRead(mod1::PIN_POT3) / 1023.0f);  // SHAPE

  const float inA = analogRead(mod1::PIN_CV1) / 1023.0f;  // F1
  const float inB = analogRead(mod1::PIN_CV2) / 1023.0f;  // F2

  // Button toggles LEVEL between attenuating F1 and being a manual level.
  buttonDebounce.update((uint8_t)digitalRead(mod1::PIN_BUTTON), nowMillis);
  if (buttonDebounce.fell()) {
    voice.manualMode = !voice.manualMode;
    flashUntil = nowMillis + kFlashMs;
  }

  voice.process(dt, inA, inB, p);

  // F3: 10-bit, straight into the Timer1 compare register.
  OCR1B = (uint16_t)(voice.out() * 1023.0f + 0.5f);

  // F4: the mirror, at Timer2's 8 bits.
  OCR2A = (uint8_t)(voice.inverted() * 255.0f + 0.5f);

  // LED follows the output, except while acknowledging a mode change.
  const bool flashing = (long)(nowMillis - flashUntil) < 0;
  OCR2B = flashing ? 255 : (uint8_t)(voice.out() * (float)kLedBrightness);
}
