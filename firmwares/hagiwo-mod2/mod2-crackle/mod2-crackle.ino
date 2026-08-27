/* Crackle

Description:
Random snaps, crackles and pops — the surface noise of a worn record, or an
old-time radio hunting between stations. Crackles arrive at random times;
DENSITY sets how often, GAIN sets how loud and how *unequal* they are, and
LENGTH sets how long each one lasts (a tick at the low end, a scratch at the
high end). Nothing is looped or sequenced: the texture is generated live from
the same probability model the original firmware used.

Original Crackle firmware by Sean Luke, ported from the AE Modular GRAINS
module. The GRAINS version has two outputs — noisy analog crackles and clean
full-volume digital pops — and MOD2 has one audio jack, so the two live on the
button instead: a short press swaps between them and the choice is saved.

The synthesis lives in the shared core (firmwares/shared/SynthCore/src/
CrackleCore.h), which is also used by the VCV Rack port (rack-plugins/src/
mod2-crackle.cpp). This sketch only owns the MOD2 hardware I/O: pots, button,
LED, gates and the ~36.6 kHz dual-slice PWM audio path.

Deliberate changes from the original:
  - DENSITY is inverted. Upstream's POT1 is labelled "rate" but holds a period
    divisor, so turning it clockwise made crackling *rarer*. The span it
    covered (about 128 down to 1 crackle per second) is unchanged.
  - Pots respond immediately. Upstream deliberately crawled towards a new pot
    setting (CONTROL_RATE 16 with a one-pole average on top) to mask GRAINS's
    PWM whine; MOD2's 10-bit ~36.6 kHz PWM has no whine to hide.
  - Crackles are centred on zero. Upstream masked its PRNG to 8 bits before
    reinterpreting it as signed, so every burst sample was positive and each
    pop carried a DC step; the core keeps the same amplitude distribution and
    randomises the sign instead. GAIN still behaves as the variance control
    upstream describes.
  - The digital output is a mode, not a second jack (see above). It keeps
    upstream's shape: unipolar, always full volume, unaffected by GAIN.
  - IN1 fires a pop on demand and the button does the same on a long press.
    GRAINS has no trigger input for this (its IN3 is unused).
  - IN2 is a gate, not a CV. Upstream summed a GAIN CV into POT2; MOD2's IN2 is
    a digital pin, so holding it high forces every crackle to full volume.
  - Rate CV lands on LENGTH. Upstream's rate CV arrives on IN1; on MOD2 the
    only analog input shares POT3, so CV modulates LENGTH here. The VCV Rack
    port has a free jack and routes CV to DENSITY the way GRAINS did.

Key Variables:
  A0 -> Density (how often crackles arrive)
  A1 -> Gain (crackle volume and volume variance)
  A2 -> Length (crackle duration, shared with CV)

      ╔═══════════╗
      ║  CRACKLE  ║
      ║  texture  ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - density
      ║   DENS    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - gain / volume variance
      ║   GAIN    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - length (shared with CV)
      ║    LEN    ║
      ║           ║
      ║   (BTN)   ║   BTN (GPIO6) - short=analog/digital, long=manual pop
      ║    [·]    ║   LED (GPIO5) - flashes on every crackle
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - trigger one pop
      ║ (o)   (o) ║   IN2 (GPIO0) - accent gate (full-volume crackles)
      ║           ║
      ║ Out    CV ║   CV  (A2)    - length (shared POT3)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Crackle firmware by Sean Luke (GRAINS, AE Modular)
  - 1.1 Ported to HAGIWO MOD2 for maddie synths (shared SynthCore voice,
        float engine at ~36.6 kHz, analog/digital output mode on the button)

License:
Apache License 2.0. This is a port of third-party code: the original Crackle
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
#include <math.h>
#include <EEPROM.h>       // RP2350 Arduino core maps on-board flash as EEPROM
#include <Mod2Common.h>   // Shared MOD2 pin map, PWM-audio setup and helpers
#include <CrackleCore.h>  // Shared Crackle voice (also used by the Rack port)


/* ═══════════════════════════════════════════════════════════════════════════
                              CONSTANTS
   ═══════════════════════════════════════════════════════════════════════════ */

constexpr float FULL_SCALE = mod2::PWM_FS;               // 1023 (10-bit PWM)
constexpr float MID_LEVEL  = mod2::PWM_MID;              // mid-scale (silence)
constexpr float AUDIO_DT   = 1.0f / mod2::AUDIO_FS;      // ~36.6 kHz period

constexpr uint32_t LONG_PRESS_MS = 500;
constexpr uint32_t DEBOUNCE_MS   = 50;

// Crackles are only a few milliseconds long — far too short to see — so the LED
// is a decaying peak-hold rather than a straight copy of the crackle gate.
// ~40 ms time constant at the audio rate.
constexpr float LED_DECAY = 0.99932f;

// Flash wear: only commit a mode change once the panel has been quiet for a
// while, since EEPROM.commit() stalls the audio ISR for several milliseconds.
constexpr uint32_t SAVE_DEBOUNCE_MS = 1500;
constexpr int      EE_ADDR_DIGITAL  = 0;
constexpr uint8_t  EE_MAGIC         = 0xC5;  // marks a slot we have written
constexpr int      EE_ADDR_MAGIC    = 1;


/* ═══════════════════════════════════════════════════════════════════════════
                              GLOBAL STATE
   ═══════════════════════════════════════════════════════════════════════════ */

uint sliceAudio;
uint sliceIRQ;
uint sliceLED;

// Shared synthesis core (the whole crackle model lives here).
sc::CrackleVoice crackle;

// LED peak-hold level, owned by the ISR.
float ledLevel = 0.0f;

// Manual-crackle request from loop(). The voice's RNG and burst state are
// otherwise touched only by the ISR, so loop() must not call trigger()
// directly — the ISR could start its own crackle mid-call and the two would
// interleave (same deferred-strike pattern as mod2-droplets).
volatile bool reqCrackle = false;

// Output mode: false = analog crackles, true = clean digital pops.
bool digitalMode = false;
bool pendingSave = false;
uint32_t saveDueAt = 0;


/* ═══════════════════════════════════════════════════════════════════════════
                              PWM ISR (~36.6 kHz)
   ═══════════════════════════════════════════════════════════════════════════ */

void __isr onPwmWrap()
{
  if (reqCrackle) {
    crackle.trigger();
    reqCrackle = false;
  }

  const sc::CrackleFrame f = crackle.process(AUDIO_DT);

  // Analog mode is ordinary bipolar audio swinging around mid-scale. Digital
  // mode reproduces GRAINS's gate pin instead: it rests at 0, not mid-scale,
  // and jumps to full scale for the crackle. Swapping modes therefore steps the
  // DC level once — the same thing that happens on GRAINS when you move the
  // patch cable from the audio jack to the digital one.
  float output = digitalMode ? (FULL_SCALE * f.audio)
                             : (MID_LEVEL + MID_LEVEL * f.audio);
  if (output < 0.0f) output = 0.0f;
  if (output > FULL_SCALE) output = FULL_SCALE;
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, static_cast<uint16_t>(output + 0.5f));

  ledLevel = (f.env > 0.0f) ? 1.0f : ledLevel * LED_DECAY;
  pwm_set_chan_level(sliceLED, PWM_CHAN_B,
                     static_cast<uint16_t>(ledLevel * FULL_SCALE));

  pwm_clear_irq(sliceIRQ);
}


/* ═══════════════════════════════════════════════════════════════════════════
                              SETUP
   ═══════════════════════════════════════════════════════════════════════════ */

void setup()
{
  analogReadResolution(10);

  pinMode(A0, INPUT);
  pinMode(A1, INPUT);
  pinMode(A2, INPUT);
  pinMode(mod2::IN1_PIN, INPUT);
  pinMode(mod2::IN2_PIN, INPUT);
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);

  // Restore the saved output mode. The magic byte keeps a virgin flash page
  // (which reads back as 0xFF) from being taken for a saved "digital" setting.
  EEPROM.begin(64);
  uint8_t magic = 0;
  EEPROM.get(EE_ADDR_MAGIC, magic);
  if (magic == EE_MAGIC) {
    uint8_t stored = 0;
    EEPROM.get(EE_ADDR_DIGITAL, stored);
    digitalMode = (stored != 0);
  }

  crackle.reset();
  crackle.setDigital(digitalMode);

  sliceLED = mod2::initPwmOutput10bit(mod2::LED_PIN);
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);
}


/* ═══════════════════════════════════════════════════════════════════════════
                              MAIN LOOP
   ═══════════════════════════════════════════════════════════════════════════ */

void loop()
{
  static int      lastBtn     = HIGH;
  static uint32_t btnDownTime = 0;
  static bool     btnHandled  = false;
  static int      lastIn1     = LOW;

  const uint32_t now = millis();

  /* ---- Button: short press swaps output mode, long press pops ------------ */
  const int btn = digitalRead(mod2::BUTTON_PIN);

  if (lastBtn == HIGH && btn == LOW) {
    btnDownTime = now;
    btnHandled = false;
  }

  if (btn == LOW && !btnHandled && (now - btnDownTime >= LONG_PRESS_MS)) {
    reqCrackle = true;
    btnHandled = true;
  }

  if (lastBtn == LOW && btn == HIGH && !btnHandled &&
      (now - btnDownTime >= DEBOUNCE_MS)) {
    digitalMode = !digitalMode;
    crackle.setDigital(digitalMode);
    pendingSave = true;
    saveDueAt = now + SAVE_DEBOUNCE_MS;
  }

  lastBtn = btn;

  /* ---- IN1: one crackle per rising edge --------------------------------- */
  const int in1 = digitalRead(mod2::IN1_PIN);
  if (lastIn1 == LOW && in1 == HIGH)
    reqCrackle = true;
  lastIn1 = in1;

  /* ---- IN2: accent gate — hold high for full-volume crackles ------------- */
  crackle.setAccent(digitalRead(mod2::IN2_PIN) == HIGH);

  /* ---- Pots ------------------------------------------------------------- */
  // A2 carries POT3 summed with the CV jack, which is why CV lands on LENGTH
  // on this hardware (see the header's list of deliberate changes).
  const float density = analogRead(A0) / 1023.0f;
  const float gain    = analogRead(A1) / 1023.0f;
  const float length  = analogRead(A2) / 1023.0f;
  crackle.setParams(density, gain, length);

  /* ---- Deferred flash write --------------------------------------------- */
  if (pendingSave && (int32_t)(now - saveDueAt) >= 0) {
    EEPROM.put(EE_ADDR_DIGITAL, (uint8_t)(digitalMode ? 1 : 0));
    EEPROM.put(EE_ADDR_MAGIC, EE_MAGIC);
    EEPROM.commit();
    pendingSave = false;
  }

  delay(1);
}
