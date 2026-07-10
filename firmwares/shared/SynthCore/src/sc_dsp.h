#pragma once

// SynthCore shared DSP building blocks.
//
// Pure ports of the platform-agnostic DSP that the MOD2 firmwares previously
// reached only through Mod2Common (which drags in Arduino.h + the Pico SDK).
// Moving the maths here lets the VCV Rack ports reuse the exact same filters,
// soft-clippers and noise as the firmware, instead of each module re-deriving a
// biquad. Depends only on sc_math.h -> <math.h>/<stdint.h>; float, no heap, no
// STL. Compiles on AVR, RP2350 and the desktop.
//
// The firmware keeps using mod2:: for hardware (PWM/IRQ setup, pins); these
// sc:: versions are the shareable signal-processing half.

#include "sc_math.h"

namespace sc {

// --------------------------------------------------
// xorshift32 PRNG (deterministic, platform-identical). Seed with any non-zero.
// --------------------------------------------------
inline uint32_t xorshift32(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

// One white-noise sample in -1..+1 from an xorshift state (replaces rand()-based
// fills so firmware and Rack get identical noise from the same seed).
inline float noise1f(uint32_t& state) {
  return (float)(xorshift32(state) >> 8) * (2.0f / 16777216.0f) - 1.0f;
}

// --------------------------------------------------
// 2-pole biquad band-pass (RBJ cookbook, constant skirt, peak gain = Q).
// Identical maths to mod2::Biquad.
// --------------------------------------------------
struct Biquad {
  float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

  void reset() { x1 = x2 = y1 = y2 = 0; }

  // Band-pass at centre frequency `fc` (Hz), quality `q`, sample rate `fs` (Hz).
  void setBandpass(float fc, float q, float fs) {
    float w0 = kTwoPi * fc / fs;
    float sw = sinf(w0), cw = cosf(w0);
    float alpha = sw / (2.0f * q);
    float nb0 = q * alpha, nb2 = -q * alpha;
    float na0 = 1.0f + alpha, na1 = -2.0f * cw, na2 = 1.0f - alpha;
    float ia0 = 1.0f / na0;
    b0 = nb0 * ia0;
    b1 = 0.0f;
    b2 = nb2 * ia0;
    a1 = na1 * ia0;
    a2 = na2 * ia0;
  }

  inline float process(float x0) {
    float y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x0;
    y2 = y1;
    y1 = y0;
    return y0;
  }
};

// --------------------------------------------------
// tanh soft-clip, normalised to reach +/-1 at the input extremes.
// `rate` controls hardness (larger = harder). Matches mod2::softClipTanh.
// --------------------------------------------------
inline float softClipTanh(float x, float rate) {
  return tanhf(rate * x) / tanhf(rate);
}

// Cubic soft-saturator / smooth limiter, saturates to +/-1. Matches mod2::softSat.
inline float softSat(float x) {
  if (x > 3.0f) return 1.0f;
  if (x < -3.0f) return -1.0f;
  float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// --------------------------------------------------
// One-pole DC-blocking high-pass. Matches mod2::DcBlocker.
// --------------------------------------------------
struct DcBlocker {
  float x1 = 0, y1 = 0;
  float alpha = 0.998f;
  inline float process(float in) {
    float out = in - x1 + alpha * y1;
    x1 = in;
    y1 = out;
    return out;
  }
};

// --------------------------------------------------
// Fixed 2-pole low-pass used to smooth the PWM output before the DAC.
// Matches mod2::OutputLpBiquad (same hard-coded coefficients).
// --------------------------------------------------
struct OutputLpBiquad {
  float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  inline float process(float in) {
    constexpr float B0 = 0.1518f, B1 = 0.3036f, B2 = 0.1518f;
    constexpr float A1 = -0.5765f, A2 = 0.1838f;
    float out = B0 * in + B1 * x1 + B2 * x2 - A1 * y1 - A2 * y2;
    x2 = x1;
    x1 = in;
    y2 = y1;
    y1 = out;
    return out;
  }
};

// --------------------------------------------------
// Mono delay line over a caller-provided int16 buffer (no heap, per the
// SynthCore rules): the firmware hands it a static SRAM arena, the Rack ports
// hand it a vector sized for the engine sample rate. int16 storage keeps the
// memory budget identical on both platforms (and matches the PWM path's
// resolution). Fractional read with linear interpolation. Shared by
// DelayFxCore and TapeEchoCore.
// --------------------------------------------------
struct DelayLine {
  int16_t* buf = nullptr;
  uint32_t len = 0;       // capacity in samples
  uint32_t writeIdx = 0;  // next slot to be written

  void init(int16_t* b, uint32_t n) {
    buf = b;
    len = n;
    writeIdx = 0;
    clear();
  }

  void clear() {
    for (uint32_t i = 0; i < len; i++) buf[i] = 0;
  }

  // Push one sample (clamped to -1..+1).
  inline void write(float x) {
    x = clampf(x, -1.0f, 1.0f);
    buf[writeIdx] = (int16_t)(x * 32767.0f);
    if (++writeIdx >= len) writeIdx = 0;
  }

  // Read `delaySamples` behind the most recent write() (1 = last sample
  // written), fractional, linear interp. Valid range 1 .. len-2.
  inline float read(float delaySamples) const {
    delaySamples = clampf(delaySamples, 1.0f, (float)(len - 2));
    const uint32_t d0 = (uint32_t)delaySamples;
    const float fr = delaySamples - (float)d0;
    // writeIdx points at the NEXT slot, so "delay d0" lives at writeIdx - d0.
    uint32_t i0 = writeIdx + len - d0;
    if (i0 >= len) i0 -= len;
    uint32_t i1 = (i0 == 0) ? len - 1 : i0 - 1;  // one sample older
    const float s0 = (float)buf[i0] * (1.0f / 32768.0f);
    const float s1 = (float)buf[i1] * (1.0f / 32768.0f);
    return s0 + (s1 - s0) * fr;
  }
};

// One-pole low-pass coefficient for cutoff `fc` (Hz) at timestep `dt`:
// y += (x - y) * onePoleCoef(fc, dt). Cheap and stable for fc*dt << 1.
inline float onePoleCoef(float fc, float dt) {
  const float a = kTwoPi * fc * dt;
  return a > 0.99f ? 0.99f : a;
}

// --------------------------------------------------
// Q12 fixed-point sample-playback helpers (for the sample-based voices).
// Match mod2:: equivalents.
// --------------------------------------------------
constexpr uint32_t FRAC_BITS = 12;
constexpr uint32_t FRAC_ONE = 1UL << FRAC_BITS;
constexpr uint32_t FRAC_MASK = FRAC_ONE - 1;

inline int16_t readPCM16LE(const uint8_t* base, uint32_t idx) {
  uint32_t byteIndex = idx << 1;
  return (int16_t)(base[byteIndex] | (base[byteIndex + 1] << 8));
}

inline int16_t lerpFixed(int16_t s1, int16_t s2, uint32_t frac) {
  return (int16_t)(((int32_t)s1 * (int32_t)(FRAC_ONE - frac) +
                    (int32_t)s2 * (int32_t)frac) >> FRAC_BITS);
}

}  // namespace sc
