#pragma once

// Random Walk + Lag voice — shared core for the RandomLag module.
//
// Used by:
//   - firmwares/mod1-random-lag/mod1-random-lag.ino  (one update per loop)
//   - vcvrack/src/RandomLag.cpp                       (at audio sample rate)
//
// Pure C++: depends only on sc_math.h. No Arduino / Rack / Pico SDK.

#include "sc_math.h"

namespace sc {

// xorshift32 PRNG — deterministic, allocation-free, no stdlib dependency.
// Updates `state` in-place and returns the new value.
inline uint32_t xorshift32(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// Uniform float in [-1, +1) drawn from the xorshift state.
inline float randF11(uint32_t& state) {
    // Cast top 16 bits to signed so the range is symmetric around 0.
    return static_cast<float>(static_cast<int16_t>(xorshift32(state) >> 16))
           / 32768.0f;
}

// Reference loop rate used to normalise per-step advances so both the firmware
// (running at its native loop speed) and VCV Rack (running at audio rate) produce
// the same walk velocity. 2 kHz matches the Butterfly reference loop rate and is
// a reasonable estimate for a Nano loop with five analogRead calls.
constexpr float kRandomLagLoopHz = 2000.0f;

// Parameters mapped from normalised 0..1 controls.
struct RandomLagParams {
    float rate;       // loop-normalised step multiplier (0.001 .. 0.1, exponential)
    float bias;       // output addend (-0.4 .. +0.4); applied at output, not in walk
    float chaos;      // step depth 0 .. 1
    float lagAmount;  // per-loop exp-smoothing coeff for F2 (0.98 .. 0.9995)
};

// Map normalised 0..1 controls to RandomLagParams, mirroring the firmware exactly.
//   pot0  = rate pot (A0)               — readFrequency() exponential curve
//   pot1  = bias pot (A1)               — maps [0..1] → [-0.4..+0.4]
//   pot2  = chaos pot (A2)              — direct depth
//   cv1   = lag CV (F1) or lag knob     — higher = tighter F2 following
//   cv3   = chaos depth CV (F3)         — additive to pot2, clamped
// Pass analogRead(pin)/1023.f from the firmware or a 0..1 VCV param directly.
inline RandomLagParams randomLagMapParams(float pot0, float pot1, float pot2,
                                           float cv1,  float cv3) {
    RandomLagParams p;

    // Rate: exponential, mirroring firmware's readFrequency().
    const float fMin = 0.001f;
    const float fMax = 0.1f;
    p.rate = fMin * powf(fMax / fMin, clampf(pot0, 0.0f, 1.0f));

    // Bias: linear map [0..1] → [-0.4..+0.4]
    p.bias = clampf(pot1, 0.0f, 1.0f) * 0.8f - 0.4f;

    // Chaos depth: pot + CV, clamped to [0..1]
    p.chaos = clampf(clampf(pot2, 0.0f, 1.0f) + clampf(cv3, 0.0f, 1.0f),
                     0.0f, 1.0f);

    // Lag amount: more cv1 = tighter following (matches firmware F1 CV behaviour).
    p.lagAmount = clampf(0.9995f - clampf(cv1, 0.0f, 1.0f) * 0.015f,
                         0.98f, 0.9995f);

    return p;
}

// Walk + lag voice state.  reset() restores power-on defaults.
struct RandomLagVoice {
    float    walkPhase   = 0.5f;
    float    laggedPhase = 0.5f;
    bool     gravityMode = false;   // toggled by button; readable from outside
    uint32_t rngState    = 0xdeadbeefUL;

    void reset() {
        walkPhase   = 0.5f;
        laggedPhase = 0.5f;
        gravityMode = false;
        rngState    = 0xdeadbeefUL;
    }

    void seed(uint32_t s) {
        rngState = (s != 0u) ? s : 0xdeadbeefUL;
    }

    // Advance one sample of `dt` seconds (sample-rate independent).
    //
    // dt usage:
    //   Firmware : pass 1.0f / kRandomLagLoopHz  (≈ nominal loop period)
    //   VCV Rack : pass args.sampleTime
    //
    // trigRose=true resets both phases to 0.5 (mid-scale). The firmware has no
    // external trigger; VCV Rack uses F1 (SchmittTrigger rising edge) or onReset().
    //
    // gravityMode is set externally (firmware button / VCV latch param) before calling.
    inline void process(float dt, bool trigRose, const RandomLagParams& p) {
        if (trigRose) {
            walkPhase   = 0.5f;
            laggedPhase = 0.5f;
        }

        // Per-sample loopFactor normalises rates so kRandomLagLoopHz
        // firmware-loop-equivalents happen per second regardless of actual dt.
        // When dt = 1/kRandomLagLoopHz the factor equals 1.0, giving exactly the
        // firmware's per-loop behaviour.
        const float loopFactor = kRandomLagLoopHz * dt;

        // The walk is a diffusion process, so its per-step size scales with the
        // SQUARE ROOT of the time step (variance accumulates ∝ dt, not dt²).
        // Using loopFactor linearly here made the VCV walk drift ~0.2x as fast
        // as the firmware and vary with host sample rate. sqrt(loopFactor) keeps
        // the per-second walk velocity sample-rate independent, and is exactly
        // 1.0 at the firmware's native loop period (so firmware is unchanged).
        const float step = randF11(rngState) * p.chaos * p.rate * sqrtf(loopFactor);
        walkPhase += step;

        if (gravityMode) {
            // Firmware: phase *= 0.99f per loop.  Normalised by loopFactor so the
            // per-second decay rate is sample-rate independent.
            walkPhase *= powf(0.99f, loopFactor);
        }

        walkPhase = clampf(walkPhase, 0.0f, 1.0f);

        // Exponential lag follower: F2 tracks F4 with per-loop coeff p.lagAmount.
        // Normalise to per-sample equivalent to stay sample-rate independent.
        const float effLag = powf(p.lagAmount, loopFactor);
        laggedPhase = laggedPhase * effLag + walkPhase * (1.0f - effLag);
    }

    // Main walk output in [0..1] with bias applied.
    inline float walkOut(float bias) const {
        return clampf(walkPhase + bias, 0.0f, 1.0f);
    }

    // Lagged output in [0..1] with bias applied.
    inline float laggedOut(float bias) const {
        return clampf(laggedPhase + bias, 0.0f, 1.0f);
    }
};

}  // namespace sc
