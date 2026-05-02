/*
Dual AD envelope for Mod1 module designed by HAGIWO, adapted by Rob Heel.

Two independent Attack–Decay envelopes with shared attack and release knobs.
Per-envelope random timing variation via Pot3 (A2):
 • Fully CCW → no variation
 • Fully CW → max random deviation per trigger
Variations are stronger at shorter attack/release settings, lighter at longer ones.

Envelope 1 is also manually triggerable via push button.
LED indicates envelope 1 level.

Outputs are fast PWM (16-bit for ENV1/ENV2, 8-bit LED).
Designed for Eurorack/modular trigger input and CV envelope output.

--Pin assign---
POT1  A0  Attack time
POT2  A1  Release Time
POT3  A2  Variation amount

F1    D17 Trigger1 IN
F2    D9  envelope1 out

F3    D10 Trigger2 IN
F4    D11 envelope2 out
BUTTON    Trigger envelope1
LED       output envelope1
EEPROM    N/A

*/
#include <Arduino.h>
#include <Mod1Common.h>
#include <Mod1EnvelopeData.h>

#define Brightness 160 // 0 - 255
constexpr int kEnvelopeTableSize = mod1::kEnvelopeTableSize;

constexpr int kStateIdle = 0;
constexpr int kStateAttack = 1;
constexpr int kStateRelease = 2;
constexpr float kEnvelopeStepScale = 0.05f / 2.0f;
constexpr int kVariationRandomScale = 1000;
constexpr int kEndOfCycleHoldTicks = 10;

// Envelope lookup tables are provided by Mod1EnvelopeData.

struct EnvelopeChannel
{
    float waveIndex;
    int outputValue;
    int lastOutput;
    int state;
    bool endOfCycle;
    int endOfCycleCount;
    int attackTime;
    int releaseTime;
    uint8_t outputPin;
};

unsigned long previousMillis = 0;
unsigned long currentMillis = 0;
int atkTime = 0;
int relTime = 0;

EnvelopeChannel env1 = {0.0f, 0, 0, kStateIdle, false, 0, 0, 0, mod1::PIN_F2};
EnvelopeChannel env2 = {0.0f, 0, 0, kStateIdle, false, 0, 0, 0, mod1::PIN_F4};

mod1::DebouncedInput buttonDebounce(50, HIGH);
mod1::EdgeInput trig1Edge(HIGH);
mod1::EdgeInput trig2Edge(HIGH);

int varyTime(int baseTime, float variationAmount)
{
    return baseTime + random(-kVariationRandomScale, kVariationRandomScale) / 1000.0f * baseTime * variationAmount;
}

void triggerEnvelope(EnvelopeChannel &env, int baseAttackTime, int baseReleaseTime, float attackVariation, float releaseVariation)
{
    env.lastOutput = (env.state == kStateRelease) ? env.outputValue : 0;
    env.state = kStateAttack;
    env.waveIndex = 0;
    env.attackTime = varyTime(baseAttackTime, attackVariation);
    env.releaseTime = varyTime(baseReleaseTime, releaseVariation);
}

void updateEnvelope(EnvelopeChannel &env)
{
    if (env.state == kStateAttack)
    {
        env.outputValue = map(pgm_read_byte(&mod1::kEnvelopeCurve[(int)env.waveIndex]), 0, 255, 255, env.lastOutput);
        env.waveIndex += kEnvelopeStepScale * env.attackTime;
    }
    else if (env.state == kStateRelease)
    {
        env.outputValue = map(pgm_read_byte(&mod1::kEnvelopeCurve[(int)env.waveIndex - kEnvelopeTableSize]), 0, 255, 0, 255);
        env.waveIndex += kEnvelopeStepScale * env.releaseTime;
    }

    if (env.waveIndex > kEnvelopeTableSize && env.waveIndex < 2 * kEnvelopeTableSize)
        env.state = kStateRelease;

    if (env.waveIndex >= 2 * kEnvelopeTableSize)
    {
        env.waveIndex = 0;
        env.state = kStateIdle;
        env.endOfCycle = true;
    }

    if (env.endOfCycle)
    {
        if (++env.endOfCycleCount > kEndOfCycleHoldTicks)
        {
            env.endOfCycleCount = 0;
            env.endOfCycle = false;
        }
    }
}

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

    // Read pots
    int pot1 = analogRead(mod1::PIN_POT1);
    int pot2 = analogRead(mod1::PIN_POT2);
    int variationPot = analogRead(mod1::PIN_POT3);

    atkTime = mod1::kEnvelopeTableSize - pgm_read_word(&mod1::kEnvelopePotAdjust[pot1]);
    relTime = mod1::kEnvelopeTableSize - pgm_read_word(&mod1::kEnvelopePotAdjust[pot2]);

    // Normalized pot values
    float atkNorm = pot1 / 1023.0;
    float relNorm = pot2 / 1023.0;
    float varNorm = variationPot / 1023.0;

    // Scaled variation range (0.2 to 0.8)
    // To shift the range to 0.1–0.8, replace 0.6 + 0.2 with 0.7 + 0.1
    float atkVarScale = (1.0 - atkNorm) * 0.6 + 0.2;
    float relVarScale = (1.0 - relNorm) * 0.6 + 0.2;
    float atkVarAmount = varNorm * atkVarScale;
    float relVarAmount = varNorm * relVarScale;

    // Trigger reads with edge detection
    buttonDebounce.update((uint8_t)digitalRead(mod1::PIN_BUTTON), currentMillis);
    trig1Edge.update((uint8_t)digitalRead(mod1::PIN_F1));
    trig2Edge.update((uint8_t)digitalRead(mod1::PIN_F3));

    bool triggerEnvelope1 = buttonDebounce.fell() || trig1Edge.fell();
    bool triggerEnvelope2 = trig2Edge.fell();

    if (triggerEnvelope1)
        triggerEnvelope(env1, atkTime, relTime, atkVarAmount, relVarAmount);

    if (triggerEnvelope2)
        triggerEnvelope(env2, atkTime, relTime, atkVarAmount, relVarAmount);

    if (currentMillis - previousMillis >= 1)
    {
        previousMillis = currentMillis;

        updateEnvelope(env1);
        updateEnvelope(env2);

        analogWrite(env1.outputPin, env1.outputValue);
        analogWrite(mod1::PIN_LED, env1.outputValue * Brightness / 255);
        analogWrite(env2.outputPin, env2.outputValue);
    }
}
