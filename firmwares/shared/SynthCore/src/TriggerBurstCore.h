#pragma once

// Trigger-burst voice — the shared core of the TriggerBurst module.
//
// Used by:
//   - firmwares/mod1-trigger-burst/mod1-trigger-burst.ino  (one step per loop, dt from millis)
//   - rack-plugins/src/TriggerBurst.cpp                          (driven at audio rate, dt = args.sampleTime)
//
// Pure C++: depends only on sc_math.h. No Arduino / Rack / Pico SDK.
// All timing is dt-driven (seconds) so it is sample-rate independent.

#include "sc_math.h"

namespace sc {

// Six-position selector mirroring mod1::select6FromAdc thresholds exactly
// (102, 308, 514, 720, 926 over 0..1023), so firmware (pass adc/1023.f) and
// VCV Rack (pass the 0..1 knob value) agree exactly.
inline uint8_t select6(float v01) {
    if (v01 <= 102.0f / 1023.0f) return 0;
    if (v01 <= 308.0f / 1023.0f) return 1;
    if (v01 <= 514.0f / 1023.0f) return 2;
    if (v01 <= 720.0f / 1023.0f) return 3;
    if (v01 <= 926.0f / 1023.0f) return 4;
    return 5;
}

// Burst count options (matches firmware's triggerOptions[])
static const int kBurstCounts[6] = {1, 3, 4, 6, 8, 16};

// Division ratios (matches firmware's divisionOptions[])
static const float kBurstDivRatios[6] = {0.5f, 0.3333f, 0.25f, 0.1667f, 0.125f, 0.0625f};

// Parameters mapped from normalised controls.
struct TriggerBurstParams {
    int   numTriggers;  // number of pulses in the burst
    float divRatio;     // fraction of clock beat per trigger interval
    float bpm;          // internal clock rate (80..280 BPM)
};

// Map normalised (0..1) pot values to burst parameters.
//   numNorm : A0 (+optional CV3 sum, clamped) / 1023.f
//   divNorm : A1 / 1023.f
//   bpmNorm : A2 / 1023.f  (caller decides external vs. internal based on threshold)
// Pass adc/1023.f from firmware or the raw 0..1 knob value from VCV Rack;
// both produce identical results via the shared select6 thresholds.
inline TriggerBurstParams triggerBurstMapParams(float numNorm, float divNorm, float bpmNorm) {
    TriggerBurstParams p;
    p.numTriggers = kBurstCounts[select6(numNorm)];
    p.divRatio    = kBurstDivRatios[select6(divNorm)];
    p.bpm         = mapClampf(bpmNorm, 0.0f, 1.0f, 80.0f, 280.0f);
    return p;
}

// Gate on-time fixed at 5 ms, matching the firmware's triggerOnTime = 5 ms.
static const float kBurstGateSec = 0.005f;

// Output for one process() call.
struct TriggerBurstResult {
    bool gateOn;  // true when trigger/gate output should be HIGH
};

// Burst state machine.
// On an input trigger the voice emits numTriggers pulses, each kBurstGateSec wide,
// separated by (clockPeriodSec * divRatio - kBurstGateSec) of silence.
// A new trigger while active is ignored (matches firmware's startBurst() guard).
struct TriggerBurstVoice {
    bool  active     = false;
    int   remaining  = 0;
    float phaseTimer = 0.0f;   // seconds until next state transition
    bool  gateOn     = false;

    void reset() {
        active     = false;
        remaining  = 0;
        phaseTimer = 0.0f;
        gateOn     = false;
    }

    // Advance by dt seconds.
    //   trigRose        : true for exactly one sample when a new trigger arrives
    //   numTriggers     : from triggerBurstMapParams
    //   divRatio        : from triggerBurstMapParams
    //   clockPeriodSec  : 60.f / bpm (internal) or measured external period
    TriggerBurstResult process(float dt, bool trigRose,
                               int numTriggers, float divRatio,
                               float clockPeriodSec) {
        // Start a new burst on rising trigger edge (ignored if already active).
        if (trigRose && !active) {
            active     = true;
            remaining  = numTriggers;
            gateOn     = true;
            phaseTimer = kBurstGateSec;
        }

        if (active) {
            phaseTimer -= dt;
            if (phaseTimer <= 0.0f) {
                if (gateOn) {
                    // Gate turns off; count the pulse just finished.
                    gateOn = false;
                    remaining--;
                    if (remaining > 0) {
                        // Off gap until next pulse.
                        float intervalSec = clockPeriodSec * divRatio;
                        float offSec = intervalSec - kBurstGateSec;
                        if (offSec < 0.0f) offSec = 0.0f;
                        phaseTimer = offSec;
                    } else {
                        // Burst complete.
                        active = false;
                    }
                } else {
                    // Gate turns on for the next pulse in the burst.
                    gateOn     = true;
                    phaseTimer = kBurstGateSec;
                }
            }
        }

        TriggerBurstResult r;
        r.gateOn = gateOn;
        return r;
    }
};

}  // namespace sc
