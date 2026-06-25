#pragma once

// LFO voice — shared core for the mod1-lfo module.
//
// Used by:
//   - firmwares/mod1-lfo/mod1-lfo.ino  (stepped every 1 ms)
//   - vcvrack/src/LFO.cpp               (stepped at audio rate via args.sampleTime)
//
// Pure C++: depends only on sc_math.h. No Arduino / Rack / Pico SDK.
//
// Behavior note (table → closed-form): the original firmware drives its
// waveforms from six 1024-byte PROGMEM lookup tables (SinTable, TriTable,
// SquTable, SawTable, SawRevWaveTable, MaxTable). Moving those tables into
// a shared header would exhaust AVR SRAM (~6 KB tables into a 2 KB budget).
// Instead, this core computes each waveform closed-form from a 0..1 phase:
//
//   sine     = sinf(2π · phase)
//   triangle = 1 − 4 · |phase − 0.5|
//   square   = phase < 0.5 ? +1 : −1
//   saw      = 2 · phase − 1
//   rev-saw  = 1 − 2 · phase
//   max/DC   = +1  (firmware's MaxTable: all 255)
//
// All six waveform shapes are mathematically equivalent to their LUT originals;
// the closed-form versions are slightly cleaner (no 8-bit quantization steps).
// This removes ~5 KB of PROGMEM from AVR flash and all associated pgm_read_byte
// overhead, freeing resources for future firmware additions.
//
// Minor behavioral difference from the original firmware: the original "level"
// control multiplicatively scales the unipolar table value (0..255 * level),
// so at level=0 the output is 0V (floor). This core works in bipolar −1..+1,
// so at level=0 the output is 0 (center / 0V DC). On the hardware the output
// is AC-coupled so the DC offset difference is imperceptible; on VCV Rack the
// bipolar center is the correct behavior for a ±5V LFO.

#include "sc_math.h"

namespace sc {

// Select a waveform index (0..5) from a normalised 0..1 control.
// Boundaries mirror the firmware's select6FromAdc() ADC thresholds
// (102 / 308 / 514 / 720 / 926 out of 1023):
//   0 = sine, 1 = triangle, 2 = square, 3 = saw, 4 = rev-saw, 5 = max/DC
inline uint8_t lfoSelectWave(float v01) {
    if (v01 <= 102.0f / 1023.0f) return 0;
    if (v01 <= 308.0f / 1023.0f) return 1;
    if (v01 <= 514.0f / 1023.0f) return 2;
    if (v01 <= 720.0f / 1023.0f) return 3;
    if (v01 <= 926.0f / 1023.0f) return 4;
    return 5;
}

// Evaluate a waveform at the given phase (0..1). Returns bipolar −1..+1.
// Replaces the firmware's six 1024-byte PROGMEM wavetables with analytic
// closed-form expressions.
inline float lfoEvalWave(uint8_t wave, float phase) {
    switch (wave) {
        case 0:  return sinf(kTwoPi * phase);
        case 1:  return 1.0f - 4.0f * fabsf(phase - 0.5f);
        case 2:  return phase < 0.5f ? 1.0f : -1.0f;
        case 3:  return 2.0f * phase - 1.0f;
        case 4:  return 1.0f - 2.0f * phase;
        default: return 1.0f;  // max / DC (MaxTable = all 255)
    }
}

// Map a normalised rate (0..1, pot+CV clamped) and a frequency-range
// multiplier (1 or 10, stored in EEPROM on the firmware) to a frequency
// in Hz. Mirrors the firmware's calculation exactly:
//   lfoFrequeny      = combined_adc(0..1023) * 0.0015 * freqRange
//   advance_per_ms   = lfoFrequeny + 0.01          (always > 0)
//   freq_hz          = advance_per_ms * 1000 / 1024
//
// Range at freqRange=1:  ~0.010 Hz (rate=0) to ~1.508 Hz (rate=1)
// Range at freqRange=10: ~0.010 Hz (rate=0) to ~14.99 Hz (rate=1)
inline float lfoMapFreq(float rate01, int freqRange) {
    const float advance = rate01 * 1023.0f * 0.0015f * (float)freqRange + 0.01f;
    return advance * (1000.0f / 1024.0f);
}

// LFO voice state. `phase` runs 0..1 (one complete oscillator cycle).
// Power-on default matches the firmware's waveIndex = 0 at startup.
struct LfoVoice {
    float phase = 0.0f;

    void reset() { phase = 0.0f; }

    // Advance the LFO by dt seconds at `freq` Hz for waveform `wave` (0..5),
    // scaled by `level` (0..1). Returns bipolar output in −1..+1.
    // Sets ledPhase to the current phase (0..1) for use as an LED source.
    inline float process(float dt, float freq, uint8_t wave, float level,
                         float& ledPhase) {
        phase += freq * dt;
        if (phase >= 1.0f) phase -= 1.0f;
        ledPhase = phase;
        return lfoEvalWave(wave, phase) * level;
    }
};

}  // namespace sc
