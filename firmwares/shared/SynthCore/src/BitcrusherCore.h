#pragma once

// Bitcrusher — bit-depth & sample-rate reduction core.
//
// Used by:
//   - firmwares/mod2-bitcrusher/mod2-bitcrusher.ino
//   - rack-plugins/src/mod2-bitcrusher.cpp
//
// Classic digital degradation: a phase-accumulator sample-and-hold reduces the
// effective sample rate (deliberately with NO anti-alias filtering — the
// aliasing is the point), and the held sample is quantized to a reduced bit
// depth. Three quantizer styles: truncate (floor), TPDF dither, and bitwise
// AND-mask ("broken ROM"). Bit depth is continuous 16..1: truncate/mask
// crossfade between adjacent integer depths; dither is naturally continuous.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop.

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Quantizer styles (BUTTON short-press cycles these on hardware).
enum BitcrusherMode : uint8_t {
  BITCRUSH_TRUNCATE = 0,  // floor to the coarser grid — gritty, adds DC-ish bias
  BITCRUSH_DITHER = 1,    // round with +/-1 LSB TPDF dither — smooth depth sweep
  BITCRUSH_MASK = 2,      // AND the 16-bit word with a top-bits mask — harshest
  BITCRUSH_MODE_COUNT = 3
};

// POT1 -> sample-and-hold rate in Hz: exponential taper from the platform's
// full rate (pot=0, i.e. no reduction) down to ~200 Hz (pot=1). `fsHz` is the
// native rate (~36.6 kHz firmware / args.sampleRate in Rack).
inline float bitcrusherRateHz(float pot01, float fsHz) {
  return fsHz * powf(200.0f / fsHz, clampf(pot01, 0.0f, 1.0f));
}

// POT2 -> continuous bit depth: 16 bits (pot=0) down to 1 bit (pot=1).
inline float bitcrusherBits(float pot01) {
  return 16.0f - 15.0f * clampf(pot01, 0.0f, 1.0f);
}

// Floor-quantize x in -1..+1 to n integer bits (n in 1..16).
inline float bitcrushQuantTruncN(float x, int n) {
  const float scale = (float)(1L << (n - 1));  // half-range levels
  return floorf(x * scale) / scale;
}

// AND-mask quantize: reinterpret as a signed 16-bit word and keep the top n
// bits. Two's-complement AND floors negative values extra hard — the "broken
// ROM" character the truncate mode doesn't quite reach.
inline float bitcrushQuantMaskN(float x, int n) {
  const int32_t i = (int32_t)(clampf(x, -1.0f, 1.0f) * 32767.0f);
  const uint16_t masked = (uint16_t)(int16_t)i & (uint16_t)(0xFFFFu << (16 - n));
  return (float)(int16_t)masked * (1.0f / 32768.0f);
}

struct BitcrusherCore {
  // Parameters (write directly; see the mappers above).
  float rateHz = 36600.0f;            // sample-and-hold rate
  float bits = 16.0f;                 // continuous bit depth, 1..16
  uint8_t mode = BITCRUSH_TRUNCATE;   // BitcrusherMode
  float wet = 1.0f;                   // 0 dry .. 1 fully crushed

  // State.
  float phase = 1.0f;  // starts >=1 so the first sample is captured immediately
  float held = 0.0f;
  uint32_t rng = 0x9e3779b9u;

  void reset() {
    phase = 1.0f;
    held = 0.0f;
    rng = 0x9e3779b9u;
  }

  // Quantize one sample at the current continuous bit depth.
  float quantize(float x) {
    x = clampf(x, -1.0f, 1.0f);
    const float b = clampf(bits, 1.0f, 16.0f);
    if (mode == BITCRUSH_DITHER) {
      // Continuous depth falls out of the dither: round on a 2^(b-1) grid with
      // triangular (TPDF) dither of +/-1 LSB.
      const float scale = powf(2.0f, b - 1.0f);
      const float tpdf = 0.5f * (noise1f(rng) + noise1f(rng));
      return clampf(floorf(x * scale + 0.5f + tpdf) / scale, -1.0f, 1.0f);
    }
    // Truncate / mask: crossfade between adjacent integer depths so POT2
    // sweeps smoothly instead of stepping.
    const int n0 = (int)b;                    // 1..16
    const float fr = b - (float)n0;
    if (mode == BITCRUSH_MASK) {
      const float lo = bitcrushQuantMaskN(x, n0);
      return n0 >= 16 ? lo : lerpf(lo, bitcrushQuantMaskN(x, n0 + 1), fr);
    }
    const float lo = bitcrushQuantTruncN(x, n0);
    return n0 >= 16 ? lo : lerpf(lo, bitcrushQuantTruncN(x, n0 + 1), fr);
  }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  // With `useExtClock` the sample-and-hold captures only on `extTick` (IN1
  // rising edge — audio-rate FM of the crush); otherwise the internal phase
  // accumulator runs at rateHz.
  float process(float in, float dt, bool useExtClock = false, bool extTick = false) {
    bool capture = false;
    if (useExtClock) {
      capture = extTick;
    } else {
      phase += rateHz * dt;
      if (phase >= 1.0f) {
        phase -= floorf(phase);
        capture = true;
      }
    }
    if (capture) held = quantize(in);
    return in + (held - in) * wet;
  }
};

}  // namespace sc
