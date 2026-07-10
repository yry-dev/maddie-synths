#pragma once

// SynthCore shared math / control helpers.
//
// This header is intentionally dependency-free: it includes only <math.h> and
// <stdint.h> so it compiles unchanged on AVR (MOD1), RP2350 (MOD2) and the
// desktop (VCV Rack). No Arduino.h, no rack.hpp, no Pico SDK. Everything is
// `float`, header-only, allocation-free and AVR-friendly.

#include <math.h>
#include <stdint.h>

namespace sc {

// Pi constants as explicit literals (M_PI is not guaranteed to be defined).
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 6.28318530717958647692f;

// Clamp x into [lo, hi].
inline float clampf(float x, float lo, float hi) {
  return x < lo ? lo : (x > hi ? hi : x);
}

// Linear interpolation: returns a at t=0, b at t=1 (t is not clamped).
inline float lerpf(float a, float b, float t) {
  return a + (b - a) * t;
}

// Linear map of x from [inMin, inMax] onto [outMin, outMax] (not clamped).
inline float mapf(float x, float inMin, float inMax, float outMin, float outMax) {
  if (inMax == inMin) return outMin;
  return outMin + (x - inMin) * (outMax - outMin) / (inMax - inMin);
}

// Same as mapf but clamps the input to [inMin, inMax] first.
inline float mapClampf(float x, float inMin, float inMax, float outMin, float outMax) {
  return mapf(clampf(x, inMin, inMax), inMin, inMax, outMin, outMax);
}

// Portable finiteness test (math.h's isfinite is a macro on every target).
inline bool isFiniteF(float x) {
  return isfinite(x) != 0;
}

// Three-position selector from a normalised 0..1 control. Boundaries mirror the
// firmware's mod1::select3FromAdc (ADC thresholds 340 and 681 over 0..1023), so
// firmware (pass adc/1023) and VCV Rack (pass the 0..1 knob) agree exactly.
inline uint8_t select3(float v01) {
  if (v01 <= 340.0f / 1023.0f) return 0;
  if (v01 <= 681.0f / 1023.0f) return 1;
  return 2;
}

// Per-sample multiplier for an exponential decay reaching exp(-decayRate) over
// `samples` steps. Matches mod2::expDecayCoef.
inline float expDecayCoef(float decayRate, float samples) {
  return expf(-decayRate / samples);
}

// Raised-cosine ramp: 0 at mu=0, 1 at mu=1. Used for smooth anti-click fades.
// Matches mod2::raisedCosine.
inline float raisedCosine(float mu) {
  return (1.0f - cosf(mu * kPi)) * 0.5f;
}

// ── MOD1 envelope time compander (shared by EG and Dual-AD) ────────────────
//
// The mod1 envelope firmwares map a 0..1 time pot through a 1024-entry PROGMEM
// table (Mod1Common's kEnvelopePotAdjust) to a "divisor" D = 1024 - PotAdjust,
// then derive the segment duration as 40960/D ms (40 ms at pot=0 .. ~41 s at
// pot=1). The curve is a steep compander that no simple power-law / exponential
// fits — a pure fit is 2.5-3x off mid-dial. We reproduce it with a 33-point
// sample of D interpolated in log space: exact at the sample points (40 /
// 141.7 / 518.5 / 2275.6 ms at pot 0/.25/.5/.75), <=4.5% across the musical
// 40 ms..2.3 s range, with the only larger residual confined to the >10 s
// extreme (pot > 0.97) where the firmware's own integer table is already
// coarse. Keeps both env cores RAM-light (66 bytes) vs the original 4 KB table.
inline float envPotDivisor(float pot01) {
  static const uint16_t kEnvDiv[33] = {
    1024, 875, 748, 639, 545, 466, 397, 339, 289, 246, 210,
    179,  152, 129, 110, 93,  79,  67,  56,  47,  40,  33,
    27,   23,  18,  15,  12,  9,   7,   5,   4,   2,   1};
  float x = clampf(pot01, 0.0f, 1.0f) * 32.0f;
  int j = (int)x;
  if (j >= 32) j = 31;
  const float fr = x - (float)j;
  // Interpolate in log space (geometric) to track the steep compander.
  const float lo = logf((float)kEnvDiv[j]);
  const float hi = logf((float)kEnvDiv[j + 1]);
  return expf(lo + (hi - lo) * fr);
}

// Envelope segment duration in seconds for a 0..1 time pot (= 40960/D ms).
inline float envPotTimeSec(float pot01) {
  return 40.96f / envPotDivisor(pot01);
}

}  // namespace sc
