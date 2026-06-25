#pragma once

// Euclidean rhythm sequencer core — shared between mod1-euclidean firmware
// and the VCV Rack Euclidean module.
//
// Used by:
//   - firmwares/mod1-euclidean/mod1-euclidean.ino  (one step per clock edge)
//   - vcvrack/src/Euclidean.cpp                     (audio-rate process loop)
//
// Algorithm: on-the-fly Bjorklund/Bresenham. No lookup tables, no heap,
// O(1) per step query. Verified to match the PROGMEM tables
// (euclidean_rhythm[9][8] and euclidean_rhythm_16[17][16]) in the original
// firmware for every (k, n, s) entry. Frees ~400 bytes of AVR flash that
// the two const arrays cost.
//
// Pure C++: depends only on sc_math.h. No Arduino / Rack / Pico SDK.

#include "sc_math.h"

namespace sc {

// Returns true if step s (0-indexed) is a hit in a Euclidean rhythm with
// k hits distributed over n steps.
//
// Derivation: Toussaint places hit j at position floor(j*n/k). Step s is a
// hit iff there exists an integer j in [s*k/n, (s+1)*k/n). Letting
// r = (s*k) % n, this simplifies to:
//   r == 0  — n divides s*k, so j = s*k/n is an exact-integer hit position
//   r+k > n — the Bresenham accumulator overflows into slot s
// Verified against every entry in the original firmware PROGMEM tables.
inline bool euclidean(int s, int k, int n) {
    if (k <= 0) return false;
    if (k >= n) return true;
    const int r = (s * k) % n;
    return (r == 0) || ((r + k) > n);
}

// Sequencer parameters mapped from normalised (0..1) panel controls.
struct EuclideanParams {
    int   hits;    // number of hits, 0..length
    int   length;  // step count: 8 or 16
    float prob;    // gate probability, 0..1 (0 = never fire, 1 = always)
};

// Map normalised panel controls to sequencer parameters.
//   potHits:   0..1 → 0..length hits. Uses integer truncation (floor) to
//              mirror Arduino map()'s integer arithmetic exactly.
//   potProb:   0..1 → trigger probability (0 = never fire, 1 = always).
//   potLength: ≤0.5 → 8 steps, >0.5 → 16 steps; mirrors the firmware's
//              (analogRead(stepModePin) > 511) threshold.
inline EuclideanParams euclideanMapParams(float potHits, float potProb, float potLength) {
    EuclideanParams p;
    p.length = (potLength > 0.5f) ? 16 : 8;
    // Arduino map(x, 0, 1023, 0, length) truncates (floor) — replicated here.
    const int h = (int)(clampf(potHits, 0.0f, 1.0f) * (float)p.length);
    p.hits = (h > p.length) ? p.length : h;
    p.prob = clampf(potProb, 0.0f, 1.0f);
    return p;
}

// Euclidean sequencer state. Advances one step per rising clock edge.
// currentStep: current position in the pattern, 0-indexed, wraps at p.length.
struct EuclideanVoice {
    int currentStep = 0;

    // Return to step 0, mirroring the firmware's currentStep = 0 on reset.
    void reset() { currentStep = 0; }

    // Advance the sequencer by one clock event and decide whether to fire.
    //   clockRose: true on the rising clock edge; returns false immediately
    //              if not a rising edge (no state change).
    //   rnd01:     uniform random in [0, 1) for the probability gate.
    //              Supply random::uniform() from VCV Rack, or the firmware's
    //              (float)random(100) / 100.0f — the core never calls rand().
    //   p:         current parameters, re-evaluated each call by the platform.
    // Returns true if a gate should fire this clock tick.
    inline bool step(bool clockRose, float rnd01, const EuclideanParams& p) {
        if (!clockRose) return false;
        // Clamp step index in case length changed mid-run.
        if (currentStep >= p.length) currentStep = 0;
        const bool isHit = euclidean(currentStep, p.hits, p.length);
        const bool fire  = isHit && (rnd01 < p.prob);
        currentStep = (currentStep + 1) % p.length;
        return fire;
    }
};

}  // namespace sc
