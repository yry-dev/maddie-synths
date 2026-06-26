/* Spiral 4Ever

Description:
Auditory illusions & impossible sounds, named after the endless spiral
staircase - the visual equivalent of Shepard tones. Nine modes:
  0 Shepard rising   1 Shepard falling   2 Barber pole
  3 Risset rhythm    4 Tritone paradox   5 Tritone explorer
  6 Shepard cluster maj   7 Shepard cluster min   8 Euler spiral
Short button press cycles modes; long press (>500 ms) toggles direction.

The synthesis lives in the shared core (firmwares/shared/SynthCore/src/
SpiralCore.h), so this sketch and the VCV Rack port (rack-plugins/src/Spiral.cpp)
run the exact same DSP. This file keeps only the MOD2 hardware I/O: ADC reads,
the button/LED logic, and the PWM-audio ISR that drives the core one sample at
a time.

Key Variables:
  A0 -> Center frequency (V/oct)
  A1 -> Sweep speed
  A2 -> Envelope width / pitch class / spiral spread (shared with CV)

      ╔═══════════╗
      ║  SPIRAL   ║
      ║ illusions ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   POT1 (A0) - center freq (V/oct)
      ║   FREQ    ║
      ║           ║
      ║   (A1)    ║   POT2 (A1) - sweep speed
      ║   SPEED   ║
      ║           ║
      ║   (A2)    ║   POT3 (A2) - env width / spread
      ║   WIDTH   ║
      ║           ║
      ║   (BTN)   ║   BTN (GPIO6) - short=cycle mode, long=direction
      ║    [·]    ║   LED (GPIO5) - solid = direction up
      ║           ║
      ╠═══════════╣
      ║ I1     I2 ║   IN1 (GPIO7) - N/A
      ║ (o)   (o) ║   IN2 (GPIO0) - N/A
      ║           ║
      ║ OUT    CV ║   CV  (A2)    - V/oct (shared POT3)
      ║ (o)   (o) ║   OUT (GPIO1) - PWM audio
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Spiral 4Ever firmware by Hagiwo
  - 1.1 Forked and refactored for maddie synths (shared SpiralCore)

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
#include <SpiralCore.h>  // Shared Spiral synthesis (also used by the VCV Rack port)


/* ═══════════════════════════════════════════════════════════════════════════
                              CONSTANTS
   ═══════════════════════════════════════════════════════════════════════════ */

constexpr float FULL_SCALE      = 1023.0f;
constexpr float MID_LEVEL       = FULL_SCALE / 2.0f;

constexpr float SYS_CLK         = 150000000.0f;
constexpr int   PWM_WRAP_IRQ    = 4095;

constexpr uint32_t LONG_PRESS_MS = 500;
constexpr uint32_t DEBOUNCE_MS   = 50;


/* ═══════════════════════════════════════════════════════════════════════════
                              GLOBAL STATE
   ═══════════════════════════════════════════════════════════════════════════ */

uint sliceAudio;
uint sliceIRQ;
float sampleRateInv = 0.0f;

// Shared synthesis core (renders every mode; same DSP as the VCV Rack port).
sc::SpiralCore spiral;

volatile uint8_t currentMode = 0;
volatile bool    directionUp = true;


/* ═══════════════════════════════════════════════════════════════════════════
                              PWM ISR
   ═══════════════════════════════════════════════════════════════════════════ */

void __isr onPwmWrap()
{
  float sample = spiral.process(sampleRateInv);

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
  spiral.reset();

  pinMode(A0, INPUT);
  pinMode(A1, INPUT);
  pinMode(A2, INPUT);
  pinMode(mod2::BUTTON_PIN, INPUT_PULLUP);
  pinMode(mod2::LED_PIN, OUTPUT);

  // Audio PWM + ~36.6 kHz wrap-IRQ setup (shared)
  mod2::initAudioPwm(sliceAudio, sliceIRQ, onPwmWrap);

  sampleRateInv = (PWM_WRAP_IRQ + 1) / SYS_CLK;  // 1 / ~36621 Hz

  digitalWrite(mod2::LED_PIN, HIGH);
}


/* ═══════════════════════════════════════════════════════════════════════════
                              MAIN LOOP
   ═══════════════════════════════════════════════════════════════════════════ */

void loop()
{
  static int      lastBtn      = HIGH;
  static uint32_t btnDownTime  = 0;
  static bool     btnHandled   = false;

  int btn = digitalRead(mod2::BUTTON_PIN);
  uint32_t now = millis();

  if (lastBtn == HIGH && btn == LOW)
  {
    btnDownTime = now;
    btnHandled = false;
  }

  if (btn == LOW && !btnHandled && (now - btnDownTime >= LONG_PRESS_MS))
  {
    directionUp = !directionUp;
    btnHandled = true;

    digitalWrite(mod2::LED_PIN, HIGH); delay(40);
    digitalWrite(mod2::LED_PIN, LOW);  delay(40);
    digitalWrite(mod2::LED_PIN, HIGH); delay(40);
    digitalWrite(mod2::LED_PIN, LOW);
  }

  if (lastBtn == LOW && btn == HIGH && !btnHandled && (now - btnDownTime >= DEBOUNCE_MS))
  {
    currentMode = (currentMode + 1) % sc::spiral::NUM_MODES;

    digitalWrite(mod2::LED_PIN, HIGH); delay(60);
    digitalWrite(mod2::LED_PIN, LOW);
  }

  lastBtn = btn;

  static uint32_t lastLed = 0;
  if (now - lastLed > 150)
  {
    digitalWrite(mod2::LED_PIN, directionUp ? HIGH : LOW);
    lastLed = now;
  }

  // Read controls and push them into the shared core.
  float centerOct = (analogRead(A0) / 1023.0f) * 5.0f;
  float speed     = analogRead(A1) / 1023.0f;
  float aux       = analogRead(A2) / 1023.0f;

  spiral.setMode(currentMode);
  spiral.setDirection(directionUp);
  spiral.setParams(centerOct, speed, aux);

  delay(1);
}
