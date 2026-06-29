#pragma once

// SquareVcoCore — square-wave VCO with sine LFO vibrato and chiptune mode.
//
// Used by:
//   - firmwares/mod2-square-vco/mod2-square-vco.ino  (RP2350 @ ~36621 Hz)
//   - rack-plugins/src/SquareVCO.cpp                       (host sample rate)
//
// A phase-accumulator square oscillator (±1) whose pitch is modulated by a
// 10 Hz sine LFO (vibrato). An optional chiptune mode imposes a 20 Hz square
// wave on top, shifting pitch up one octave on every second half-cycle.
// Oscillator frequency = BASE_FREQ × freqFactor × octave × cvMult × vibratoMod.
//
// Pure C++: depends only on sc_math.h. No Arduino / Rack / Pico SDK.

#include "sc_math.h"

namespace sc {

// ------ tuning constants (match firmware) -------
constexpr float kSquareVcoBaseFreq = 40.0f;     // Hz — firmware BASE_FREQ
constexpr float kSquareVcoMaxFreq  = 20000.0f;  // Hz — firmware MAX_FREQ
constexpr float kSquareVcoLfoRate  = 10.0f;     // Hz — firmware LFO_FREQUENCY
constexpr float kChiptuneRate      = 20.0f;     // Hz — firmware CHIPTUNE_FREQ

// Octave multipliers — firmware octMap[6] = {1,2,4,8,16,32}
constexpr float kSquareVcoOctaves[6] = {1.f, 2.f, 4.f, 8.f, 16.f, 32.f};

// ------ parameter-mapping free functions -------

// Fine-tune pot (0..1) → frequency factor 1.0..2.0
// Mirrors: freqFactor = 1.0f + (rawA0 / 1023.0f)
inline float squareVcoTune(float knob01) {
    return 1.0f + knob01;
}

// Octave pot / CV (0..1) → octave index 0..5
// Mirrors firmware thresholds: <102, <308, <514, <720, <926, else (over 0..1023)
inline int squareVcoOctaveIdx(float v01) {
    if (v01 < (102.f / 1023.f)) return 0;
    if (v01 < (308.f / 1023.f)) return 1;
    if (v01 < (514.f / 1023.f)) return 2;
    if (v01 < (720.f / 1023.f)) return 3;
    if (v01 < (926.f / 1023.f)) return 4;
    return 5;
}

// Vibrato depth pot (0..1) → amplitude fraction 0..0.05 (0..5%)
// Firmware had this fixed at 0.02; this lets VCV expose the full range.
inline float squareVcoVibDepth(float knob01) {
    return knob01 * 0.05f;
}

// ------ voice core -------

struct SquareVcoCore {
    // Internal phase accumulators (advanced by process(); ISR-exclusive in firmware)
    float oscPhase  = 0.0f;  // 0..1 oscillator phase
    float lfoPhase  = 0.0f;  // 0..1 vibrato LFO phase
    float chipPhase = 0.0f;  // 0..1 chiptune toggle phase

    // Parameters — platform I/O layer sets these (loop() in firmware; process() in VCV)
    float freqFactor  = 1.0f;   // 1.0..2.0 fine-tune
    int   octaveIndex = 0;      // 0..5 → kSquareVcoOctaves
    float vibDepth    = 0.02f;  // 0..0.05 vibrato amplitude fraction (firmware default 0.02)
    float cvMult      = 1.0f;   // pitch multiplier from V/Oct input (2^volts, or 2^(v01*5) on hw)
    bool  chiptuneOn  = false;

    void reset() {
        oscPhase  = 0.0f;
        lfoPhase  = 0.0f;
        chipPhase = 0.0f;
    }

    // Synthesise one audio sample and advance all phases by dt (seconds).
    // Returns audio in -1..+1.
    //   Firmware:  (audio + 1.0f) * 511.5f → 0..1023 PWM duty
    //   VCV Rack:  audio * 5.0f → ±5 V
    float process(float dt) {
        // Sine LFO → vibrato
        lfoPhase += kSquareVcoLfoRate * dt;
        if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
        const float vibratoMod = 1.0f + sinf(lfoPhase * kTwoPi) * vibDepth;

        // Clamp octave index defensively
        const int idx = octaveIndex < 0 ? 0 : (octaveIndex > 5 ? 5 : octaveIndex);

        // Composite frequency
        float freq = kSquareVcoBaseFreq
                   * freqFactor
                   * kSquareVcoOctaves[idx]
                   * cvMult
                   * vibratoMod;

        // Chiptune: 20 Hz square that shifts pitch up one octave on the second
        // half-cycle — matches firmware chipPhase + sinf() > 0 gate.
        if (chiptuneOn) {
            chipPhase += kChiptuneRate * dt;
            if (chipPhase >= 1.0f) chipPhase -= 1.0f;
            if (chipPhase >= 0.5f) freq *= 2.0f;
        }

        freq = clampf(freq, 1.0f, kSquareVcoMaxFreq);

        // Phase accumulator + square wave (±1)
        oscPhase += freq * dt;
        if (oscPhase >= 1.0f) oscPhase -= 1.0f;
        return oscPhase < 0.5f ? 1.0f : -1.0f;
    }
};

}  // namespace sc
