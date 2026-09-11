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
// AND-mask ("broken ROM"). Bit depth is continuous 12..1: truncate/mask
// crossfade between adjacent integer depths; dither is naturally continuous.
// A post-crush drive stage gains the held step into a hard clip.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop.
//
// License:
// MIT License, Copyright (c) 2026 Madelyn Yeary — see LICENSE.md in this
// library. Derived from firmwares/mod2-bitcrusher/mod2-bitcrusher.ino,
// HAGIWO's CC0 1.0 firmware; CC0 places no conditions on derivative works.

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

// NOTE: MASK and TRUNCATE currently produce bit-identical output. Clearing the
// low bits of a two's-complement word IS a floor onto the same 2^(1-n) grid, so
// the two paths only differ in how they get there. GRAINS `bit` (its only
// quantizer) is the same AND-mask and lands in the same place. Keeping the two
// modes separate is a placeholder for giving MASK a genuinely different
// transfer (overflow-wrap rather than clip is the usual choice); see
// .omc/research/grains-compare-bit.md before picking one.

// POT1 -> sample-and-hold rate in Hz: exponential taper from the platform's
// full rate (pot=0, i.e. no reduction) down to ~200 Hz (pot=1). `fsHz` is the
// native rate (~36.6 kHz firmware / args.sampleRate in Rack).
inline float bitcrusherRateHz(float pot01, float fsHz) {
  return fsHz * powf(200.0f / fsHz, clampf(pot01, 0.0f, 1.0f));
}

// POT2 -> continuous bit depth: 12 bits (pot=0) down to 1 bit (pot=1).
//
// The ceiling is 12 rather than 16 on purpose. MOD2 feeds this a 10-bit ADC
// reading and plays it back through a 10-bit PWM, so every depth above ~10 is
// inaudible and the top third of the knob used to do nothing. The idea is
// GRAINS `bit`'s, which notes that "Grains inputs at a resolution of 1024, but
// outputs at most at a resolution of 488 — thus we're already bitcrushing in
// the output to begin with", and which spends its whole knob on 8 useful
// depths. GRAINS `bit` is Apache 2.0, Copyright 2024 Sean Luke
// (github.com/eclab/grains); only the observation is borrowed, not code.
inline float bitcrusherBits(float pot01) {
  return 12.0f - 11.0f * clampf(pot01, 0.0f, 1.0f);
}

// BUTTON+POT2 (shift) -> output drive: 0x (silence) .. 4x, applied *after* the
// crush and hard-clipped, so pushing it past 1x flattens the quantizer's coarse
// steps against the rails. The placement is GRAINS `bit`'s idea (the code is
// ours): its POT 3 is a gain stage
// in exactly that position ("Then it changes the gain, likely clipping") —
// clipping crushed steps is a different, dirtier sound than clipping first and
// crushing the result. Apache 2.0, Copyright 2024 Sean Luke.
inline float bitcrusherDrive(float pot01) {
  return 4.0f * clampf(pot01, 0.0f, 1.0f);
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
  float bits = 12.0f;                 // continuous bit depth, 1..12
  uint8_t mode = BITCRUSH_TRUNCATE;   // BitcrusherMode
  float drive = 1.0f;                 // post-crush gain into a hard clip
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
    // Crush, then gain, then clip — GRAINS `bit`'s ordering. The drive lives
    // inside the hold so the held step keeps its clipped shape until the next
    // capture, instead of the gain re-shaping a frozen sample.
    if (capture) held = clampf(quantize(in) * drive, -1.0f, 1.0f);
    return in + (held - in) * wet;
  }
};

}  // namespace sc
