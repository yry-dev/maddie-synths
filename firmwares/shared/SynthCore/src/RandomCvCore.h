#pragma once

// Random CV sequencer core — shared by mod1-random-cv firmware and VCV Rack.
//
// Used by:
//   - firmwares/mod1-random-cv/mod1-random-cv.ino  (advances on digital clock edge)
//   - rack-plugins/src/RandomCV.cpp                       (clock from SchmittTrigger)
//
// Pure C++: depends only on sc_math.h. No Arduino / Rack / Pico SDK.

#include "sc_math.h"

namespace sc {

// Minimal xorshift32 PRNG (Marsaglia 2003). State must be non-zero.
// Returns the next pseudo-random uint32 and advances state in-place.
inline uint32_t xorshift32(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

// Six-position selector from a normalised 0..1 control.
// Mirrors Mod1Common::select6FromAdc (ADC thresholds 102, 308, 514, 720, 926 / 1023).
// Pass adc/1023 from firmware or the raw 0..1 knob from VCV Rack — results agree.
inline uint8_t select6(float v01) {
    if (v01 <= 102.0f / 1023.0f) return 0;
    if (v01 <= 308.0f / 1023.0f) return 1;
    if (v01 <= 514.0f / 1023.0f) return 2;
    if (v01 <= 720.0f / 1023.0f) return 3;
    if (v01 <= 926.0f / 1023.0f) return 4;
    return 5;
}

// Step-mode table: mirrors firmware's stepModes[] = { 3, 4, 5, 8, 16, 32 }.
static const uint8_t kRandomCvStepModes[6] = { 3, 4, 5, 8, 16, 32 };

// Parameters mapped from the three panel pots.
struct RandomCvParams {
    uint8_t totalSteps;  // selected from kRandomCvStepModes (3, 4, 5, 8, 16, or 32)
    float   level;       // 0..1 — CV amplitude scale
    float   trigProb;    // 0..1 — gate fire probability
};

// Map normalised panel controls (0..1) to RandomCvParams, exactly like
// mod1-random-cv: POT1 -> step count, POT2 -> level, POT3 -> trigger probability.
// Pass adc/1023 from firmware or the raw 0..1 knob value from VCV Rack.
inline RandomCvParams randomCvMapParams(float pot_steps, float pot_level, float pot_prob) {
    RandomCvParams p;
    p.totalSteps = kRandomCvStepModes[select6(pot_steps)];
    p.level      = clampf(pot_level, 0.0f, 1.0f);
    p.trigProb   = clampf(pot_prob,  0.0f, 1.0f);
    return p;
}

// Per-step output: normalised CV (0..1, level already applied) and gate flag.
struct RandomCvFrame {
    float cv;   // 0..1 — scale to output voltage in the platform layer
    bool  gate; // true if gate should fire this step
};

// The sequencer state. CV and trigger tables are stored as uint8_t (0..255) to
// match the firmware's RAM footprint on the 2 KB ATmega328P (64 bytes here vs
// 128 bytes for int arrays or 256 bytes for float arrays).
struct RandomCvVoice {
    uint8_t  cvValues[32];     // random CV pattern (0..255)
    uint8_t  trigValues[32];   // random gate pattern (0..255)
    uint8_t  currentStep;
    uint8_t  currentTotalSteps;
    uint32_t rngState;

    // Held output between steps (level not yet applied to currentCv).
    float currentCv;
    bool  currentGate;

    RandomCvVoice()
        : currentStep(0), currentTotalSteps(8), rngState(0xDEADBEEFu),
          currentCv(0.0f), currentGate(false) {
        _fillArrays();
    }

    // Reset the step counter only; does not change the stored pattern.
    void reset() {
        currentStep = 0;
        currentCv   = cvValues[0] / 255.0f;
        currentGate = false;
    }

    // Re-randomize the CV/gate tables using the current rngState (advances it).
    // Equivalent to the firmware's reRandomizeCV(). Also resets the step counter.
    void randomize() {
        _fillArrays();
        currentStep = 0;
        currentCv   = 0.0f;
        currentGate = false;
    }

    // Set a new PRNG seed and re-randomize all tables. Use for deterministic reset.
    void seed(uint32_t s) {
        if (s == 0) s = 1;  // xorshift32 requires non-zero state
        rngState = s;
        randomize();
    }

    // Advance the sequencer on a clock rising edge (clockRose == true).
    // When clockRose == false, returns the held output scaled by p.level.
    // p should be freshly computed from randomCvMapParams() each call so that
    // pot changes take effect immediately (mirrors the firmware).
    RandomCvFrame step(bool clockRose, const RandomCvParams& p) {
        if (!clockRose) {
            return { currentCv * p.level, currentGate };
        }

        // Apply step-count change immediately on each clock edge (mirrors
        // firmware's updateStepCount() call at the top of the trigger handler).
        if (p.totalSteps != currentTotalSteps) {
            if (currentStep >= p.totalSteps)
                currentStep = currentStep % p.totalSteps;
            currentTotalSteps = p.totalSteps;
        }

        currentStep = (currentStep + 1) % currentTotalSteps;

        currentCv = cvValues[currentStep] / 255.0f;

        // Gate fires when trigValues[step] / 255 < trigProb, matching the
        // firmware's comparison trigValues[step] < map(pot, 0, 1023, 0, 255).
        currentGate = ((float)trigValues[currentStep] / 255.0f) < p.trigProb;

        return { currentCv * p.level, currentGate };
    }

private:
    void _fillArrays() {
        for (int i = 0; i < 32; i++) {
            cvValues[i]   = (uint8_t)(xorshift32(rngState) & 0xFF);
            trigValues[i] = (uint8_t)(xorshift32(rngState) & 0xFF);
        }
    }
};

}  // namespace sc
