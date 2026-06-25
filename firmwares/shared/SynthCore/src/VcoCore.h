#pragma once

// VCO voice — six-waveform oscillator with PolyBLEP anti-aliasing.
//
// Used by:
//   - firmwares/mod2-vco/mod2-vco.ino  (ISR-driven, ~36.6 kHz, RP2350 FPU)
//   - vcvrack/src/VCO.cpp               (live synthesis at host sample rate)
//
// Waveforms: 0 Sine | 1 Triangle | 2 Square | 3 Saw | 4 FM-4x | 5 FM-2x.
// PolyBLEP corrects the Square and Saw discontinuities. FM uses sinf for
// both modulator and carrier (closed-form; no table required).
//
// Caller sets `freq`, `waveIndex`, `fmAmount`, then calls process(dt) once
// per sample where dt is 1/sampleRate. Phase is internally normalised to
// 0..1 so the math is sample-rate independent.
//
// Pure C++: depends only on sc_math.h / <math.h>. No Arduino / Rack / Pico SDK.

#include "sc_math.h"

namespace sc {

// Wave selector matching the firmware's A0 ADC thresholds.
// Pass pot as normalised 0..1 (firmware: analogRead(A0) / 1023.f).
inline uint8_t vcoWaveSelect(float pot01) {
    if (pot01 <  32.0f / 1023.0f) return 0;  // Sine
    if (pot01 < 248.0f / 1023.0f) return 1;  // Triangle
    if (pot01 < 514.0f / 1023.0f) return 2;  // Square
    if (pot01 < 720.0f / 1023.0f) return 3;  // Saw
    if (pot01 < 926.0f / 1023.0f) return 4;  // FM 4×
    return 5;                                 // FM 2×
}

// PolyBLEP discontinuity residual. t and dt are both normalised (0..1 per
// period). Matches the firmware's inline polyBLEP helper.
inline float polyBLEP(float t, float dt) {
    if (t < dt)          { float x = t / dt;            return  x + x - x*x - 1.0f; }
    if (t > 1.0f - dt)   { float x = (t - 1.0f) / dt;  return  x*x + x + x + 1.0f; }
    return 0.0f;
}

// Six-waveform VCO core.  All signal synthesis lives here; platform I/O and
// volatile bookkeeping stay in the firmware / Rack wrapper.
struct VcoCore {
    float   freq      = 320.0f;  // oscillator frequency (Hz)
    uint8_t waveIndex = 0;       // 0..5 (use vcoWaveSelect to map a pot)
    float   fmAmount  = 2.0f;    // FM phase-modulation depth (radians)

    void reset() {
        phase   = 0.0f;
        lpState = 0.0f;
    }

    // Render one sample and advance by dt seconds. Returns audio in -1..+1.
    // Firmware: call with dt = 1.0f / sampleRate (computed once in setup).
    // VCV Rack: call with args.sampleTime.
    float process(float dt) {
        const float t  = phase;       // normalised phase 0..1
        const float dp = freq * dt;   // phase increment per sample

        float s;
        switch (waveIndex) {

            case 0:  // Sine — closed-form replaces firmware LUT + interpolation
                s = sinf(kTwoPi * t);
                break;

            case 1:  // Triangle: −1 at t=0, +1 at t=0.5, −1 at t=1
                s = 1.0f - 4.0f * fabsf(t - 0.5f);
                break;

            case 2: {  // Square + PolyBLEP on both edges
                s  = (t < 0.5f) ? 1.0f : -1.0f;
                s += polyBLEP(t, dp);
                s -= polyBLEP(fmodf(t + 0.5f, 1.0f), dp);  // falling edge at t=0.5
                break;
            }

            case 3: {  // Saw + PolyBLEP
                s  = 2.0f * t - 1.0f;
                s -= polyBLEP(t, dp);
                break;
            }

            case 4: {  // FM — modulator at 4× carrier freq
                float mod = sinf(kTwoPi * t * 4.0f);
                s = sinf(kTwoPi * t + fmAmount * mod);
                break;
            }

            default: {  // FM — modulator at 2× carrier freq (waveIndex == 5)
                float mod = sinf(kTwoPi * t * 2.0f);
                s = sinf(kTwoPi * t + fmAmount * mod);
                break;
            }
        }

        // One-pole RC low-pass — matches firmware LP_ALPHA (0.18). Softens
        // residual alias / PWM texture, identical math in both platforms.
        lpState += (s - lpState) * 0.18f;

        // Advance and wrap phase accumulator.
        phase += dp;
        phase -= floorf(phase);

        return lpState;
    }

private:
    float phase   = 0.0f;
    float lpState = 0.0f;
};

}  // namespace sc
