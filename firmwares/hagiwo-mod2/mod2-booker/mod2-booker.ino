/* Booker

Description:
A Hammond tonewheel organ with a Leslie in the cabinet. Nine sine drawbars are
added over one note; REG picks which of 16 classic drawbar registrations is
pulled out — Full Organ, Blues, two Booker T. Jones settings, Gospel, Greg
Allman, Paul Shaffer, Reggae, Strings and more — and the button spins the
Leslie up and down between chorale and tremolo. Pitch is 1 V/oct on the CV
jack. More drawbars out means louder and dirtier, exactly as on the console:
Full Organ at full volume overdrives, and that is the sound.

Original Booker firmware by Sean Luke, ported from the AE Modular GRAINS
module. GRAINS drones — it has a pitch CV and a volume CV and no gate at all —
so MOD2 gains a key: IN1 gates notes once you switch it out of drone mode.

The synthesis lives in the shared core (firmwares/shared/SynthCore/src/
BookerCore.h), which is also used by the VCV Rack port (rack-plugins/src/
mod2-booker.cpp). This sketch only owns the MOD2 hardware I/O: pots, button,
LED, gates and the ~36.6 kHz dual-slice PWM audio path.

Deliberate changes from the original:
  - Pitch is 1 V/oct, not GRAINS's 1.3 V/oct. Upstream spends POT1 on a Pitch
    CV Scaling trimmer because GRAINS tracks 1.3 V/octave and drifts as its
    resistors warm up; MOD2's input divider is fixed and calibrated, so the pot
    is free for something else and the jack tracks a sequencer directly. This
    uses mod2-vco's calibration verbatim. 0 V is still upstream's C0 (32.7 Hz
    at the 16' drawbar), and the span is ~4.9 octaves against upstream's 5.0.
  - The 1536-entry pitch table is gone. It is equal-tempered at 1/17 semitone
    per step, so the core computes 32.7 * 2^volts instead and gets ~6 KB back.
  - The Leslie has two speeds and coasts between them. Upstream runs one fixed
    5.66 Hz rotor; the button here cycles OFF / chorale / tremolo using
    upstream's own two rates (0.66 and 5.66 Hz), ramping over about a second
    the way a belt-driven horn does. IN2 is a speed footswitch.
  - The Leslie's pitch depth is a ratio, not a fixed number of hertz. See
    BookerCore.h — upstream's constant +/-1 Hz offset is 52 cents at the bottom
    of the keyboard and nothing at the top; this is +/-1 % everywhere.
  - The Leslie's amplitude depth is upstream's deepest setting rather than its
    default. Its shipped LESLIE_VOLUME of 1 shifts by 8 bits and so adds
    exactly nothing — the stock Leslie is pitch-only, which is not what the
    #define reads like it should do.
  - Partials above Nyquist fade out instead of aliasing. Upstream's header
    calls this out as a known fault it could not afford to fix on an ATmega.
  - Volume is a pot only. Upstream's IN2 is a volume CV; MOD2's IN2 is a
    digital pin, so it carries the Leslie speed switch instead.
  - IN1 gates notes, and the button's long press chooses whether it does.
    Drone mode (the default) is upstream's behaviour; gated mode adds a fast
    key envelope, since a Hammond's contacts make and break almost instantly.

Key Variables:
  A0 -> Registration (which of the 16 drawbar settings)
  A1 -> Volume
  A2 -> Pitch, 1 V/oct (shared with CV)

      ╔═══════════╗
      ║  BOOKER   ║
      ║   organ   ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - registration (16 drawbar settings)
      ║    REG    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - volume
      ║    VOL    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - pitch / V-oct (shared with CV)
      ║   PITCH   ║
      ║           ║
      ║   (BTN)   ║   BTN (GPIO6) - short=Leslie off/slow/fast, long=drone/gate
      ║    [·]    ║   LED (GPIO5) - note level, pulsing at the rotor speed
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - key gate (gated mode only)
      ║ (o)   (o) ║   IN2 (GPIO0) - Leslie fast while high
      ║           ║
      ║ OUT    CV ║   OUT (GPIO1) - PWM audio
      ║ (o)   (o) ║   CV  (A2)    - 1 V/oct pitch (shared POT3)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Booker firmware by Sean Luke (GRAINS, AE Modular)
  - 1.1 Ported to HAGIWO MOD2 for maddie synths (shared SynthCore voice, float
        additive engine at ~36.6 kHz, two-speed Leslie, gate input)

License:
Apache License 2.0. This is a port of third-party code: the original Booker
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
#include <EEPROM.h>      // RP2350 Arduino core maps on-board flash as EEPROM
#include <Mod2Common.h>  // Shared MOD2 pin map, PWM-audio setup and helpers
#include <BookerCore.h>  // Shared Booker voice (also used by the Rack port)


/* ═══════════════════════════════════════════════════════════════════════════
                              CONSTANTS
   ═══════════════════════════════════════════════════════════════════════════ */

constexpr float FULL_SCALE = mod2::PWM_FS;           // 1023 (10-bit PWM)
constexpr float MID_LEVEL  = mod2::PWM_MID;          // mid-scale (silence)
constexpr float AUDIO_DT   = 1.0f / mod2::AUDIO_FS;  // ~36.6 kHz period

constexpr uint32_t LONG_PRESS_MS = 500;
constexpr uint32_t DEBOUNCE_MS   = 50;

// Pitch calibration, taken from mod2-vco: the 8.3 and the 33/55 describe the
// input divider ahead of A2, so together they turn the ADC's full scale into
// octaves and a volt at the jack into an octave of pitch. TUNE_CAL trims the
// last fraction of a percent by ear; edit it there and here together.
const float TUNE_CAL = 0.992f;
const float VOCT_SPAN_OCT = 8.3f * (33.0f / 55.0f) * TUNE_CAL;  // ~4.94 octaves

// Flash wear: only commit a setting once the panel has been quiet for a while,
// since EEPROM.commit() stalls the audio ISR for several milliseconds.
constexpr uint32_t SAVE_DEBOUNCE_MS = 1500;
constexpr int      EE_ADDR_LESLIE   = 0;
constexpr int      EE_ADDR_DRONE    = 1;
constexpr int      EE_ADDR_MAGIC    = 2;
constexpr uint8_t  EE_MAGIC         = 0xB0;  // marks a slot we have written


/* ═══════════════════════════════════════════════════════════════════════════
                              GLOBAL STATE
   ═══════════════════════════════════════════════════════════════════════════ */

uint sliceAudio;
uint sliceIRQ;
uint sliceLED;

// Shared synthesis core (the whole organ and its Leslie live here).
sc::BookerVoice booker;

// Panel settings, restored from flash at boot.
uint8_t leslieSetting = sc::BOOKER_LESLIE_FAST;  // upstream ships the Leslie on
bool droneMode = true;                           // upstream drones; IN1 is ignored
bool pendingSave = false;
uint32_t saveDueAt = 0;


/* ═══════════════════════════════════════════════════════════════════════════
                              PWM ISR (~36.6 kHz)
   ═══════════════════════════════════════════════════════════════════════════ */

void __isr onPwmWrap()
{
  const sc::BookerFrame f = booker.process(AUDIO_DT);

  float output = MID_LEVEL + MID_LEVEL * f.audio;
  if (output < 0.0f) output = 0.0f;
  if (output > FULL_SCALE) output = FULL_SCALE;
  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, static_cast<uint16_t>(output + 0.5f));

  pwm_set_chan_level(sliceLED, PWM_CHAN_B,
                     static_cast<uint16_t>(f.env * FULL_SCALE));

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

  // Restore the saved settings. The magic byte keeps a virgin flash page (which
  // reads back as 0xFF) from being taken for saved values.
  EEPROM.begin(64);
  uint8_t magic = 0;
  EEPROM.get(EE_ADDR_MAGIC, magic);
  if (magic == EE_MAGIC) {
    uint8_t stored = 0;
    EEPROM.get(EE_ADDR_LESLIE, stored);
    if (stored <= sc::BOOKER_LESLIE_FAST) leslieSetting = stored;
    EEPROM.get(EE_ADDR_DRONE, stored);
    droneMode = (stored != 0);
  }

  booker.reset();
  booker.leslieMode = leslieSetting;
  booker.gate = true;

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

  const uint32_t now = millis();

  /* ---- Button: short press cycles the Leslie, long press toggles gating --- */
  const int btn = digitalRead(mod2::BUTTON_PIN);

  if (lastBtn == HIGH && btn == LOW) {
    btnDownTime = now;
    btnHandled = false;
  }

  if (btn == LOW && !btnHandled && (now - btnDownTime >= LONG_PRESS_MS)) {
    droneMode = !droneMode;
    btnHandled = true;
    pendingSave = true;
    saveDueAt = now + SAVE_DEBOUNCE_MS;
  }

  if (lastBtn == LOW && btn == HIGH && !btnHandled &&
      (now - btnDownTime >= DEBOUNCE_MS)) {
    leslieSetting = (leslieSetting + 1) % (sc::BOOKER_LESLIE_FAST + 1);
    pendingSave = true;
    saveDueAt = now + SAVE_DEBOUNCE_MS;
  }

  lastBtn = btn;

  /* ---- IN2: Leslie speed footswitch -------------------------------------- */
  // High forces tremolo while the Leslie is running; the core ramps between the
  // two rates either way, so this behaves like the pedal on the real cabinet.
  if (leslieSetting == sc::BOOKER_LESLIE_OFF) {
    booker.leslieMode = sc::BOOKER_LESLIE_OFF;
  } else if (digitalRead(mod2::IN2_PIN) == HIGH) {
    booker.leslieMode = sc::BOOKER_LESLIE_FAST;
  } else {
    booker.leslieMode = leslieSetting;
  }

  /* ---- IN1: key gate ----------------------------------------------------- */
  // Drone mode holds the key down, which is GRAINS's own behaviour and also the
  // only sensible default: an unpatched IN1 reads low and would otherwise leave
  // the module silent with no way to tell why.
  booker.gate = droneMode || (digitalRead(mod2::IN1_PIN) == HIGH);

  /* ---- Pots -------------------------------------------------------------- */
  // Writing amp[] here races the ISR's read of it by design: the worst a torn
  // update can do is blend two registrations for a single sample.
  booker.setRegistration(sc::bookerRegistrationSelect(analogRead(A0) / 1023.0f));
  booker.volume = analogRead(A1) / 1023.0f;

  // A2 carries POT3 summed with the CV jack, and the panel pots are wired in
  // reverse, so a rising ADC reading means falling pitch. 0 V (pot fully up)
  // is upstream's C0; full scale is ~4.9 octaves above it.
  const float pitchOct = VOCT_SPAN_OCT * (1.0f - analogRead(A2) / 1023.0f);
  booker.freq = sc::bookerFreqFromVolts(pitchOct);

  /* ---- Deferred flash write ---------------------------------------------- */
  if (pendingSave && (int32_t)(now - saveDueAt) >= 0) {
    EEPROM.put(EE_ADDR_LESLIE, leslieSetting);
    EEPROM.put(EE_ADDR_DRONE, (uint8_t)(droneMode ? 1 : 0));
    EEPROM.put(EE_ADDR_MAGIC, EE_MAGIC);
    EEPROM.commit();
    pendingSave = false;
  }

  delay(1);
}
