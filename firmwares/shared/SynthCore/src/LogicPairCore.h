#pragma once

// Boolean-logic pair voice — the shared core of the LogicPair module.
//
// Used by:
//   - firmwares/mod1-logic-pair/mod1-logic-pair.ino  (one step per loop)
//   - rack-plugins/src/LogicPair.cpp                       (per-sample at audio rate)
//
// Pure C++: depends only on sc_math.h. No Arduino / Rack / Pico SDK.

#include "sc_math.h"

namespace sc {

// Six logic modes, matching the firmware's select6FromAdc mapping.
enum LogicMode : uint8_t {
    LOGIC_AND_NAND = 0,   // outA = AND,  outB = NAND
    LOGIC_OR_NOR   = 1,   // outA = OR,   outB = NOR
    LOGIC_XOR_XNOR = 2,   // outA = XOR,  outB = XNOR
    LOGIC_COMPARE  = 3,   // outA = (A>B) gate, outB = (B>A) gate
    LOGIC_MAX_MIN  = 4,   // outA = max(A,B), outB = min(A,B) (analogue, 0..1)
    LOGIC_FLIPFLOP = 5    // T-type flip-flop per channel, triggered on rising edge
};

// Map a normalised 0..1 control to one of 6 mode slots. Boundaries mirror
// mod1::select6FromAdc (thresholds 102, 308, 514, 720, 926 over 0..1023), so
// firmware (pass adc/1023) and VCV Rack (pass the 0..1 knob) agree exactly.
inline uint8_t select6(float v01) {
    if (v01 <= 102.0f / 1023.0f) return 0;
    if (v01 <= 308.0f / 1023.0f) return 1;
    if (v01 <= 514.0f / 1023.0f) return 2;
    if (v01 <= 720.0f / 1023.0f) return 3;
    if (v01 <= 926.0f / 1023.0f) return 4;
    return 5;
}

// Output of one logic step: outA and outB both in 0..1.
// For gate modes (AND/NAND, OR/NOR, XOR/XNOR, COMPARE, FLIP-FLOP) the values
// are exactly 0 or 1. For MAX/MIN they are the normalised analogue magnitudes
// of the larger and smaller input respectively.
struct LogicPairResult {
    float outA;
    float outB;
};

// Logic pair state. Only the FLIP-FLOP mode has persistent state; the other
// five modes are purely combinational. reset() restores power-on values
// (matches the firmware's global-variable initialisation: all false).
struct LogicPairVoice {
    bool flipA = false;  // T flip-flop latch for channel A
    bool flipB = false;  // T flip-flop latch for channel B
    bool lastA = false;  // previous gate state for A (rising-edge detection)
    bool lastB = false;  // previous gate state for B

    void reset() {
        flipA = false;
        flipB = false;
        lastA = false;
        lastB = false;
    }

    // Compute one logic step. gateA/gateB are the digital (thresholded) gate
    // states; valA01/valB01 are the normalised 0..1 analogue levels (used only
    // in COMPARE and MAX/MIN modes). mode selects the operation (0..5).
    // Returns outA and outB in 0..1; caller scales to PWM (×255) or Rack
    // voltage (×10V) as appropriate.
    inline LogicPairResult step(bool gateA, bool gateB,
                                float valA01, float valB01,
                                uint8_t mode) {
        LogicPairResult r = {0.f, 0.f};
        switch (mode) {
            case LOGIC_AND_NAND:
                r.outA = (gateA && gateB) ? 1.f : 0.f;
                r.outB = !(gateA && gateB) ? 1.f : 0.f;
                break;

            case LOGIC_OR_NOR:
                r.outA = (gateA || gateB) ? 1.f : 0.f;
                r.outB = !(gateA || gateB) ? 1.f : 0.f;
                break;

            case LOGIC_XOR_XNOR:
                r.outA = (gateA ^ gateB) ? 1.f : 0.f;
                r.outB = !(gateA ^ gateB) ? 1.f : 0.f;
                break;

            case LOGIC_COMPARE:
                // If equal, both outputs remain 0 (matches firmware).
                if (valA01 > valB01) {
                    r.outA = 1.f;
                    r.outB = 0.f;
                } else if (valB01 > valA01) {
                    r.outA = 0.f;
                    r.outB = 1.f;
                }
                break;

            case LOGIC_MAX_MIN:
                if (valA01 >= valB01) {
                    r.outA = valA01;  // MAX => A channel
                    r.outB = valB01;  // MIN => B channel
                } else {
                    r.outA = valB01;  // MAX => B channel
                    r.outB = valA01;  // MIN => A channel
                }
                break;

            case LOGIC_FLIPFLOP:
                if (gateA && !lastA) flipA = !flipA;
                if (gateB && !lastB) flipB = !flipB;
                lastA = gateA;
                lastB = gateB;
                r.outA = flipA ? 1.f : 0.f;
                r.outB = flipB ? 1.f : 0.f;
                break;
        }
        return r;
    }
};

}  // namespace sc
