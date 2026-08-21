#pragma once

// Declarations for the two 1024-entry PROGMEM tables the MOD1 envelope
// firmwares share: the exponential-ish curve and the pot-response remap.
//
// License:
// MIT License, Copyright (c) 2026 Madelyn Yeary — see LICENSE.md in this
// library. Board support for HAGIWO MOD1 hardware; the pin map and table
// shapes follow HAGIWO's CC0 1.0 MOD1 firmware, which places no conditions on
// derivative works.

#include <Arduino.h>

namespace mod1 {

constexpr int kEnvelopeTableSize = 1024;

// Exponential-ish shape table used for attack/release lookup.
extern const uint8_t kEnvelopeCurve[kEnvelopeTableSize] PROGMEM;

// Pot remap table used to shape attack/release time response.
extern const int kEnvelopePotAdjust[kEnvelopeTableSize] PROGMEM;

}  // namespace mod1
