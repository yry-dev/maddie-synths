#pragma once

#include <Arduino.h>

namespace mod1 {

constexpr int kEnvelopeTableSize = 1024;

// Exponential-ish shape table used for attack/release lookup.
extern const uint8_t kEnvelopeCurve[kEnvelopeTableSize] PROGMEM;

// Pot remap table used to shape attack/release time response.
extern const int kEnvelopePotAdjust[kEnvelopeTableSize] PROGMEM;

}  // namespace mod1
