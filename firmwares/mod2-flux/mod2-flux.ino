/* Flux

Description:
Physical modelling, resonance & noise - from ordered resonance to pure
chaos. Seven modes in three groups:
  RESONANCE: 0 Modal (tuned resonator bank), 1 Karplus (plucked string)
  NOISE:     2 White, 3 Pink (1/f), 4 S&H (stepped), 5 Quantum (Lorenz)
  TEXTURE:   6 Drone (evolving harmonic texture)
Short button press cycles modes; long press (>500 ms) manually
triggers/plucks. LED blinks on trigger.

The synthesis lives in the shared core (firmwares/shared/SynthCore/src/
FluxCore.h), which is also used by the VCV Rack port (rack-plugins/src/Flux.cpp).
This sketch only owns the MOD2 hardware I/O: pots, button, LED and the
~36.6 kHz dual-slice PWM audio path.

Key Variables:
  A0 -> Frequency / pitch
  A1 -> Rate / trigger speed / chaos rate
  A2 -> Character / brightness / slew (per mode, shared with CV)

      ╔═══════════╗
      ║   FLUX    ║
      ║ resonance ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - frequency / pitch
      ║   FREQ    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - rate / chaos rate
      ║   RATE    ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - character / brightness / slew
      ║   CHAR    ║
      ║           ║
      ║   (BTN)   ║   BTN (GPIO6) - short=cycle mode, long=trigger
      ║    [·]    ║   LED (GPIO5) - blinks on trigger
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - N/A
      ║ (o)   (o) ║   IN2 (GPIO0) - N/A
      ║           ║
      ║ CV    Out ║   CV  (A2)    - character (shared POT3)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Flux firmware by Hagiwo
  - 1.1 Forked and refactored for maddie synths (shared SynthCore voice)

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
#include <Mod2Common.h>  // Shared MOD2 pin map, PWM-audio setup and helpers
#include <FluxCore.h>    // Shared Flux voice (also used by the VCV Rack port)


/* ═══════════════════════════════════════════════════════════════════════════
                              CONSTANTS
   ═══════════════════════════════════════════════════════════════════════════ */

constexpr float FULL_SCALE    = mod2::PWM_FS;    // 1023 (10-bit PWM)
constexpr float MID_LEVEL     = mod2::PWM_MID;   // mid-scale (silence)
constexpr float AUDIO_DT      = 1.0f / mod2::AUDIO_FS;  // ~36.6 kHz sample period

constexpr float CENTER_MIN_HZ = 32.0f;
constexpr float CENTER_MAX_HZ = 2000.0f;

constexpr uint32_t LONG_PRESS_MS = 500;
constexpr uint32_t DEBOUNCE_MS   = 50;
constexpr uint32_t LED_BLINK_MS  = 30;


/* ═══════════════════════════════════════════════════════════════════════════
                              GLOBAL STATE
   ═══════════════════════════════════════════════════════════════════════════ */

uint sliceAudio;
uint sliceIRQ;

// Shared synthesis core (all DSP for the seven modes lives here).
sc::FluxVoice flux;


/* ═══════════════════════════════════════════════════════════════════════════
                              PWM ISR (~36.6 kHz)
   ═══════════════════════════════════════════════════════════════════════════ */

void __isr onPwmWrap()
{
  const float sample = flux.process(AUDIO_DT);  // -1..+1, output stage applied

  float output = MID_LEVEL + MID_LEVEL * sample;
  if (output < 0.0f) output = 0.0f;
  if (output > FULL_SCALE) output = FULL_SCALE;

  pwm_set_chan_level(sliceAudio, PWM_CHAN_B, static_cast<uint16_t>(output + 0.5f));
  pwm_clear_irq(sliceIRQ);
}


/* ═══════════════════════════════════════════════════════════════════════════
                              SETUP
   ═══════════════════════════════════════════════════════════════════════════ */

void setup()
{
  pinMode(A0, INPUT);
  pinMode(A1, INPUT);
  pinMode(A2, INPUT);
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);
  pinMode(mod2::LED_PIN, OUTPUT);

  flux.reset();

  // Audio PWM + ~36.6 kHz wrap-IRQ setup (shared).
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);

  digitalWrite(mod2::LED_PIN, HIGH);
}


/* ═══════════════════════════════════════════════════════════════════════════
                              MAIN LOOP
   ═══════════════════════════════════════════════════════════════════════════ */

void loop()
{
  static int      lastBtn     = HIGH;
  static uint32_t btnDownTime = 0;
  static bool     btnHandled  = false;
  static uint32_t ledOffTime  = 0;

  const int btn = digitalRead(mod2::BUTTON_PIN);
  const uint32_t now = millis();

  if (lastBtn == HIGH && btn == LOW)
  {
    btnDownTime = now;
    btnHandled = false;
  }

  // Long press: manual trigger / pluck.
  if (btn == LOW && !btnHandled && (now - btnDownTime >= LONG_PRESS_MS))
  {
    flux.trigger();
    btnHandled = true;
  }

  // Short press (release): cycle mode.
  if (lastBtn == LOW && btn == HIGH && !btnHandled && (now - btnDownTime >= DEBOUNCE_MS))
  {
    flux.setMode((flux.mode + 1) % sc::kFluxNumModes);
  }

  lastBtn = btn;

  // Read controls.
  const float cv = analogRead(A0) / 1023.0f;
  const float oct = cv * 5.0f;
  float centerFreq = CENTER_MIN_HZ * powf(2.0f, oct);
  centerFreq = fminf(fmaxf(centerFreq, CENTER_MIN_HZ), CENTER_MAX_HZ);

  const float speed = analogRead(A1) / 1023.0f;
  const float aux   = analogRead(A2) / 1023.0f;
  flux.setParams(centerFreq, speed, aux);

  // LED: brief blink whenever the core fires a trigger (auto or manual).
  if (flux.consumeFired())
  {
    digitalWrite(mod2::LED_PIN, HIGH);
    ledOffTime = now + LED_BLINK_MS;
  }
  if (ledOffTime != 0 && now >= ledOffTime)
  {
    digitalWrite(mod2::LED_PIN, LOW);
    ledOffTime = 0;
  }

  delay(1);
}
