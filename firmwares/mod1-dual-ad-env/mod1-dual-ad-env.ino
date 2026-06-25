/* Dual AD

Description:
Two independent Attack-Decay envelopes with shared attack and release knobs.
Per-envelope random timing variation via Pot3 (A2): fully CCW = no variation,
fully CW = max random deviation per trigger. Variations are stronger at shorter
attack/release settings, lighter at longer ones. Envelope 1 is also manually
triggerable via the push button. Outputs are fast PWM (16-bit for ENV1/ENV2,
8-bit LED). Designed for Eurorack/modular trigger input and CV envelope output.

Key Variables:
  A0 -> Attack time
  A1 -> Release time
  A2 -> Variation amount (per-trigger random timing deviation)

      ╔═══════════╗
      ║  DUAL AD  ║
      ║ envelope  ║
      ╠═══════════╣
      ║           ║
      ║   (A0)    ║   ATTACK  — attack time
      ║  ATTACK   ║
      ║           ║
      ║   (A1)    ║   RELEASE — release time
      ║  RELEASE  ║
      ║           ║
      ║   (A2)    ║   VARY    — random timing variation
      ║   VARY    ║
      ║           ║
      ║    [·]    ║   LED (D3) — envelope 1 level
      ║   (BTN)   ║   BTN (D4) — trigger envelope 1
      ║           ║
      ╠═══════════╣
      ║ F1     F2 ║   F1 (D17) IN  — Trigger 1
      ║ (o)   (o) ║   F2 (D9)  OUT — Envelope 1 (PWM CV)
      ║           ║
      ║ F3     F4 ║   F3 (D10) IN  — Trigger 2
      ║ (o)   (o) ║   F4 (D11) OUT — Envelope 2 (PWM CV)
      ║           ║
      ╚═══════════╝

Version History:
  - 1.0 Forked from Rob Scape: https://github.com/rob-scape/hgw-mod1-firmwares/
  - 1.1 Refactored for Maddie Synths
  - 1.2 Ported to shared DualADEnvCore (closed-form exp curves, dt-driven)

License:
  MIT License

Hardware:
HAGIWO MOD1
*/
#include <Arduino.h>
#include <Mod1Common.h>
#include <DualADEnvCore.h>

#define Brightness 160 // 0 - 255

sc::ADEnvVoice env1;
sc::ADEnvVoice env2;

mod1::DebouncedInput buttonDebounce(50, HIGH);
mod1::EdgeInput trig1Edge(HIGH);
mod1::EdgeInput trig2Edge(HIGH);

unsigned long previousMillis = 0;
unsigned long currentMillis = 0;

void setup()
{
    pinMode(mod1::PIN_BUTTON, INPUT_PULLUP);
    pinMode(mod1::PIN_F1, INPUT);
    pinMode(mod1::PIN_F3, INPUT);
    pinMode(mod1::PIN_LED, OUTPUT);
    pinMode(mod1::PIN_F2, OUTPUT);
    pinMode(mod1::PIN_F4, OUTPUT);

    mod1::setupFastPwmEgStyle();
}

void loop()
{
    currentMillis = millis();

    // Read pots (normalised 0..1)
    float atkNorm = analogRead(mod1::PIN_POT1) / 1023.0f;
    float relNorm = analogRead(mod1::PIN_POT2) / 1023.0f;
    float varNorm = analogRead(mod1::PIN_POT3) / 1023.0f;

    // Map pots to time (seconds) via shared closed-form mapping.
    float baseAtk = sc::adEnvMapTime(atkNorm);
    float baseRel = sc::adEnvMapTime(relNorm);

    // Variation scale: shorter times → wider variation range.
    float atkVarAmount = varNorm * sc::adEnvVarScale(atkNorm);
    float relVarAmount = varNorm * sc::adEnvVarScale(relNorm);

    // Trigger edge detection
    buttonDebounce.update((uint8_t)digitalRead(mod1::PIN_BUTTON), currentMillis);
    trig1Edge.update((uint8_t)digitalRead(mod1::PIN_F1));
    trig2Edge.update((uint8_t)digitalRead(mod1::PIN_F3));

    bool doTrig1 = buttonDebounce.fell() || trig1Edge.fell();
    bool doTrig2 = trig2Edge.fell();

    if (doTrig1)
    {
        float dev1atk = random(-1000, 1000) / 1000.0f;
        float dev1rel = random(-1000, 1000) / 1000.0f;
        env1.trigger(sc::adEnvApplyVariation(baseAtk, dev1atk, atkVarAmount),
                     sc::adEnvApplyVariation(baseRel, dev1rel, relVarAmount));
    }

    if (doTrig2)
    {
        float dev2atk = random(-1000, 1000) / 1000.0f;
        float dev2rel = random(-1000, 1000) / 1000.0f;
        env2.trigger(sc::adEnvApplyVariation(baseAtk, dev2atk, atkVarAmount),
                     sc::adEnvApplyVariation(baseRel, dev2rel, relVarAmount));
    }

    if (currentMillis - previousMillis >= 1)
    {
        previousMillis = currentMillis;

        // dt = 1 ms = 0.001 s (fixed tick rate)
        constexpr float dt = 0.001f;
        env1.process(dt);
        env2.process(dt);

        analogWrite(mod1::PIN_F2, (int)(env1.output * 255.0f));
        analogWrite(mod1::PIN_LED, (int)(env1.output * Brightness));
        analogWrite(mod1::PIN_F4, (int)(env2.output * 255.0f));
    }
}
