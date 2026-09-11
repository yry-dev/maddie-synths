/* Droplets

Description:
A wind chime. Every trigger drops one sine note, picked at random from a chord
you choose with CHORD, and left to ring down. Nothing is sequenced and nothing
repeats: the music is entirely in the chord tables and the draw. Four droplets
can overlap, so a fast trigger builds into a shimmer and a slow one leaves each
note alone in the air.

RANGE does two things at once, exactly as it did on the original. Its lower half
gives you the wide voicings, spread over three or four octaves, with five
release lengths from a tick to a two-second chime; its upper half repeats those
five releases against the same chords folded into about half the span, so the
tinkling sits in one register instead of raining down through the whole keyboard.

Original Droplets firmware by Sean Luke, ported from the AE Modular GRAINS
module. GRAINS tracks 1.3 V/octave and burns a whole pot on trimming that back
into tune; MOD2's CV input is a normal 1 V/octave jack, so that pot was free and
the chord selection moved onto it.

The synthesis lives in the shared core (firmwares/shared/SynthCore/src/
DropletsCore.h), which is also used by the VCV Rack port (rack-plugins/src/
mod2-droplets.cpp). This sketch only owns the MOD2 hardware I/O: pots, button,
LED, gates and the ~36.6 kHz dual-slice PWM audio path.

Deliberate changes from the original:
  - No pitch-scaling trim pot. Upstream's POT1 exists only to stretch GRAINS's
    1.3 V/octave inputs back to 1 V/octave, and the header warns the scaling
    drifts as the board warms up. MOD2's CV input needs none of that, so POT1
    carries CHORD (upstream's POT3) and the CV jack is plain 1 V/octave.
  - Root pitch is a pot AND the CV jack, because on MOD2 they are the same ADC
    pin. 0 V still plays C0 (32.7 Hz), upstream's tuning; the pot adds up to
    five octaves on top and the whole thing is clamped at the 7.5-octave ceiling
    upstream's frequency table had.
  - The button transposes. Upstream's TRANSPOSE_OCTAVES is a #define you
    recompile to change; here a short press cycles it 0/+1/+2/+3, the same
    gesture mod2-vco uses for octaves, and the setting is saved to flash.
  - A long press drops a droplet by hand, so the module can be auditioned
    without patching a trigger into it. GRAINS has no button at all.
  - IN2 is an accent gate. Upstream put a Release-and-Range CV on its IN2, but
    MOD2's IN2 is a digital pin; holding it high makes the next droplets land at
    half volume, which is the accent convention the other MOD2 drum voices use.
  - Droplets are capped at the real Nyquist rather than upstream's NYQUIST
    constant, which was actually Mozzi's 16384 Hz sample rate and so let high
    droplets alias. See DropletsCore.h for the rest of the numeric divergences
    (both lookup tables replaced by closed forms, the squared-frequency fallback
    fixed, and a portable PRNG).

Key Variables:
  A0 -> Chord (which chord the droplets are drawn from)
  A1 -> Range and release (voicing width + ring-down length)
  A2 -> Root pitch, 0..5 octaves above C0 (shared with CV)

      ╔═══════════╗
      ║ DROPLETS  ║
      ║   chime   ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - chord
      ║   CHORD   ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - range + release
      ║   RANGE   ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - root pitch (shared with CV)
      ║   ROOT    ║
      ║           ║
      ║   (BTN)   ║   BTN (GPIO6) - short=octave 0/+1/+2/+3, long=one droplet
      ║    [·]    ║   LED (GPIO5) - droplet envelope; resting glow = octave
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - trigger one droplet
      ║ (o)   (o) ║   IN2 (GPIO0) - accent gate (half-volume droplets)
      ║           ║
      ║ Out    CV ║   CV  (A2)    - 1 V/oct root pitch (shared POT3)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Droplets firmware by Sean Luke (GRAINS, AE Modular)
  - 1.1 Ported to HAGIWO MOD2 for maddie synths (shared SynthCore voice,
        float engine at ~36.6 kHz, 1 V/oct CV, octave transpose on the button)

License:
Apache License 2.0. This is a port of third-party code: the original Droplets
firmware is Copyright 2023 Sean Luke (sean@cs.gmu.edu), from the GRAINS
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
#include <EEPROM.h>        // RP2350 Arduino core maps on-board flash as EEPROM
#include <Mod2Common.h>    // Shared MOD2 pin map, PWM-audio setup and helpers
#include <DropletsCore.h>  // Shared Droplets voice (also used by the Rack port)


/* ═══════════════════════════════════════════════════════════════════════════
                              CONSTANTS
   ═══════════════════════════════════════════════════════════════════════════ */

constexpr float FULL_SCALE = mod2::PWM_FS;           // 1023 (10-bit PWM)
constexpr float MID_LEVEL  = mod2::PWM_MID;          // mid-scale (silence)
constexpr float AUDIO_DT   = 1.0f / mod2::AUDIO_FS;  // ~36.6 kHz period

constexpr uint32_t LONG_PRESS_MS = (uint32_t)(sc::kDropletLongPressSec * 1000.0f);
constexpr uint32_t DEBOUNCE_MS   = 50;

// How far the root pot reaches on its own, before the CV jack adds to it.
// Upstream's pitch pot covered five octaves of its frequency table.
constexpr float ROOT_POT_OCTAVES = 5.0f;

// Accent (IN2) halves the level of droplets struck while it is high, matching
// the other MOD2 voices.
constexpr float ACCENT_LEVEL = 0.5f;

// Resting LED brightness per octave of transpose, so the button's state is
// readable at a glance without swamping the droplet flashes on top of it.
constexpr float OCT_GLOW = 0.10f;

// Flash wear: only commit an octave change once the panel has been quiet for a
// while, since EEPROM.commit() stalls the audio ISR for several milliseconds.
constexpr uint32_t SAVE_DEBOUNCE_MS = 1500;
constexpr int      EE_ADDR_OCT      = 0;
constexpr uint8_t  EE_MAGIC         = 0xD3;  // marks a slot we have written
constexpr int      EE_ADDR_MAGIC    = 1;


/* ═══════════════════════════════════════════════════════════════════════════
                              GLOBAL STATE
   ═══════════════════════════════════════════════════════════════════════════ */

uint sliceAudio;
uint sliceIRQ;
uint sliceLED;

// Shared synthesis core (the whole droplet model lives here).
sc::DropletsVoice droplets;

// Panel state, written by loop() and read by the ISR when it services a strike.
// Each is a single word, so a torn read is not possible on this core.
volatile float g_chordPot = 0.0f;
volatile float g_rangePot = 0.0f;
volatile float g_rootHz   = sc::kDropletRootC0Hz;

// A droplet is requested by loop() (trigger jack or long press) and performed by
// the audio ISR, so the voice pool has exactly one writer.
volatile bool  g_reqStrike = false;
volatile float g_reqLevel  = 1.0f;

// Octave transpose, 0..3, cycled by the button and saved to flash.
uint8_t  octShift    = 0;
bool     pendingSave = false;
uint32_t saveDueAt   = 0;


/* ═══════════════════════════════════════════════════════════════════════════
                              PWM ISR (~36.6 kHz)
   ═══════════════════════════════════════════════════════════════════════════ */

void __isr onPwmWrap()
{
  // Service a pending droplet here rather than in the GPIO interrupt: strike()
  // is a handful of random draws and a powf, cheap enough to sit in the audio
  // path, and doing it here means the voice pool is only ever touched by this
  // ISR. (Contrast mod2-kick, whose strike bakes a 22k-sample table and so has
  // to be deferred to loop().)
  if (g_reqStrike) {
    g_reqStrike = false;
    droplets.setParams(g_chordPot, g_rangePot, g_rootHz);
    droplets.strike(g_reqLevel);
  }

  const sc::DropletsFrame f = droplets.process(AUDIO_DT);

  float output = MID_LEVEL + MID_LEVEL * f.audio;
  if (output < 0.0f) output = 0.0f;
  if (output > FULL_SCALE) output = FULL_SCALE;
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, static_cast<uint16_t>(output + 0.5f));

  // Droplet envelopes last hundreds of milliseconds, so the LED can follow the
  // envelope directly with no peak-hold. The transpose sits underneath it as a
  // floor, which is the only way the button's state is visible.
  float led = f.env;
  const float glow = OCT_GLOW * octShift;
  if (led < glow) led = glow;
  pwm_set_chan_level(sliceLED, PWM_CHAN_B, static_cast<uint16_t>(led * FULL_SCALE));

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

  // Restore the saved transpose. The magic byte keeps a virgin flash page
  // (which reads back as 0xFF) from being taken for a saved octave.
  EEPROM.begin(64);
  uint8_t magic = 0;
  EEPROM.get(EE_ADDR_MAGIC, magic);
  if (magic == EE_MAGIC) {
    uint8_t stored = 0;
    EEPROM.get(EE_ADDR_OCT, stored);
    if (stored <= 3) octShift = stored;
  }

  droplets.reset();
  // Upstream capped droplets at its 16384 Hz "NYQUIST"; use the real one.
  droplets.maxHz = mod2::AUDIO_FS * 0.5f;
  droplets.setParams(0.0f, 0.0f, sc::kDropletRootC0Hz);

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

  /* ---- Pots ------------------------------------------------------------- */
  // A2 carries POT3 summed with the CV jack, so the root pitch is whatever the
  // two of them add up to — there is no way to separate them on this hardware.
  g_chordPot = analogRead(A0) / 1023.0f;
  g_rangePot = analogRead(A1) / 1023.0f;
  g_rootHz   = sc::dropletsRootFreq(
      (analogRead(A2) / 1023.0f) * ROOT_POT_OCTAVES + octShift);

  /* ---- Button: short press transposes, long press drops one ------------- */
  const int btn = digitalRead(mod2::BUTTON_PIN);

  if (lastBtn == HIGH && btn == LOW) {
    btnDownTime = now;
    btnHandled = false;
  }

  if (btn == LOW && !btnHandled && (now - btnDownTime >= LONG_PRESS_MS)) {
    g_reqLevel = (digitalRead(mod2::IN2_PIN) == HIGH) ? ACCENT_LEVEL : 1.0f;
    g_reqStrike = true;
    btnHandled = true;
  }

  if (lastBtn == LOW && btn == HIGH && !btnHandled &&
      (now - btnDownTime >= DEBOUNCE_MS)) {
    octShift = (octShift + 1) & 3;
    pendingSave = true;
    saveDueAt = now + SAVE_DEBOUNCE_MS;
  }

  lastBtn = btn;

  /* ---- IN1: one droplet per rising edge --------------------------------- */
  const int in1 = digitalRead(mod2::IN1_PIN);
  if (lastIn1 == LOW && in1 == HIGH) {
    // IN2 is sampled at the edge, so the accent applies to this droplet for its
    // whole ring-down rather than following the gate afterwards.
    g_reqLevel = (digitalRead(mod2::IN2_PIN) == HIGH) ? ACCENT_LEVEL : 1.0f;
    g_reqStrike = true;
  }
  lastIn1 = in1;

  /* ---- Deferred flash write --------------------------------------------- */
  if (pendingSave && (int32_t)(now - saveDueAt) >= 0) {
    EEPROM.put(EE_ADDR_OCT, octShift);
    EEPROM.put(EE_ADDR_MAGIC, EE_MAGIC);
    EEPROM.commit();
    pendingSave = false;
  }

  delay(1);
}
