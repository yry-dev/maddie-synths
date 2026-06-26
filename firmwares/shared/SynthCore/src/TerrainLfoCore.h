#pragma once

// Terrain LFO voice — the shared core of the TerrainLFO module.
//
// Used by:
//   - firmwares/mod1-terrain-lfo/mod1-terrain-lfo.ino  (one loop iteration / step)
//   - rack-plugins/src/TerrainLFO.cpp                       (LOOP_HZ-emulated steps)
//
// Procedurally generated triple-wavetable LFO. On regenerate(), three independent
// "terrain" wavetables are built from a handful of random knots (seamless loop,
// at least one zero and one peak, nonlinear knot spacing, one Bezier-curved
// segment). The three terrains are read back at fixed detune (x0.9 / x1.0 / x1.1)
// and each can independently drop into a tempo-scaled "SloMo breath".
//
// Pure C++: depends only on sc_math.h. No Arduino / Rack / Pico SDK. All DSP math
// is float; the terrains are stored as uint16_t to stay AVR-RAM-light (1.5 KB,
// identical footprint to the original firmware's three global tables).

#include "sc_math.h"

namespace sc {

// Wavetable resolution (one terrain). Matches the firmware's TABLE_SIZE.
static const int kTerrainTableSize = 256;
// Upper bound on knots-per-waveform (firmware maps POT3 to 3..12).
static const int kTerrainMaxKnots = 16;

// Parameters mapped from the three panel pots. baseHz/intensity drive every
// step(); knots is only consumed by regenerate().
struct TerrainParams {
  float baseHz;    // POT1 -> base speed, 0.01 .. ~3 Hz (exponential)
  float intensity; // POT2 -> SloMo probability / intensity, 0..1
  int knots;       // POT3 -> knots per waveform, 3..12
};

// Map normalised panel controls (0..1) to engine units, exactly like
// mod1-terrain-lfo: POT1 -> base speed, POT2 -> SloMo intensity, POT3 -> knots.
// Pass adc/1023 from firmware or the raw 0..1 knob value from VCV Rack — both
// produce identical results.
inline TerrainParams terrainMapParams(float pot1, float pot2, float pot3) {
  TerrainParams p;
  p.baseHz = 0.01f * powf(300.0f, clampf(pot1, 0.0f, 1.0f)); // 0.01 .. ~3 Hz
  p.intensity = clampf(pot2, 0.0f, 1.0f);
  p.knots = (int) mapClampf(pot3, 0.0f, 1.0f, 3.0f, 12.0f);
  return p;
}

// Small, portable xorshift32 RNG so terrain generation is identical on every
// target (Arduino's random() is not available off-AVR). Seeded from hardware
// entropy on the firmware, from the host RNG in VCV Rack.
struct TerrainRng {
  uint32_t s = 0xC0FFEEu;

  inline void seed(uint32_t v) { s = v ? v : 0xC0FFEEu; }

  inline uint32_t nextU32() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }

  // Uniform float in [0, 1).
  inline float nextFloat() { return (nextU32() >> 8) * (1.0f / 16777216.0f); }

  // Uniform int in [lo, hi).
  inline int range(int lo, int hi) {
    if (hi <= lo) return lo;
    return lo + (int) (nextFloat() * (float) (hi - lo));
  }
};

// One independent SloMo "breath" event. Durations are tracked in seconds so the
// engine is sample-rate independent (the firmware tracked millis()).
struct TerrainSlowMo {
  bool active = false;
  float slowFactor = 1.0f; // 0.1 .. 0.5 (fraction of normal speed) while active
  float duration = 0.0f;   // seconds
  float elapsed = 0.0f;    // seconds since the event started
};

// The terrain engine. out[0..2] hold the smoothed, normalised (0..1) LFO values
// for the three detuned channels; platform code scales them to PWM / voltage.
struct TerrainLfoCore {
  uint16_t table[3][kTerrainTableSize]; // generated terrains (0..65535)
  float phase[3];                        // read position, table-index units
  float out[3];                          // smoothed output, 0..1
  TerrainSlowMo slow[3];
  TerrainRng rng;

  TerrainLfoCore() { reset(); }

  // Restore the firmware's power-on state: phases at 0, outputs at mid-scale,
  // no active SloMo. Leaves the generated terrains intact (call regenerate() to
  // build new ones).
  void reset() {
    for (int c = 0; c < 3; ++c) {
      phase[c] = 0.0f;
      out[c] = 0.5f; // firmware seeded lastPwm at mid (128/255)
      slow[c] = TerrainSlowMo();
    }
  }

  // Build a single terrain into `table[ch]` from `knots` random knots, mirroring
  // the firmware's generateSingleTerrain (heightScale fixed at 1.0).
  void generateSingleTerrain(int ch, int knots) {
    if (knots < 2) knots = 2;
    if (knots > kTerrainMaxKnots) knots = kTerrainMaxKnots;

    float knotVals[kTerrainMaxKnots];
    for (int k = 0; k < knots; ++k)
      knotVals[k] = rng.range(0, 1000) / 1000.0f; // 0..1 (heightScale = 1)
    knotVals[rng.range(0, knots)] = 0.0f;          // guarantee a zero crossing
    knotVals[rng.range(0, knots)] = 1.0f;          // guarantee a full-scale peak

    float knotPos[kTerrainMaxKnots];
    float total = 0.0f;
    for (int k = 0; k < knots; ++k) {
      float step = rng.range(50, 200) / 1000.0f; // nonlinear knot spacing
      total += step;
      knotPos[k] = total;
    }
    if (total <= 0.0f) total = 1.0f;
    for (int k = 0; k < knots; ++k) knotPos[k] /= total;

    // Seamless loop: last knot mirrors the first, pinned to t = 1.
    knotVals[knots - 1] = knotVals[0];
    knotPos[knots - 1] = 1.0f;

    int curvedSegment = rng.range(0, knots - 1);
    float curvature = rng.range(-80, 80) / 100.0f;

    for (int i = 0; i < kTerrainTableSize; ++i) {
      float t = (float) i / (float) (kTerrainTableSize - 1);
      int k0 = 0;
      while (k0 < knots - 1 && t > knotPos[k0 + 1]) k0++;
      int k1 = k0 + 1;
      if (k1 > knots - 1) k1 = knots - 1;

      float span = knotPos[k1] - knotPos[k0];
      float frac = (span != 0.0f) ? (t - knotPos[k0]) / span : 0.0f;

      float v;
      if (k0 == curvedSegment) {
        float v0 = knotVals[k0];
        float v1 = knotVals[k1];
        float vMid = clampf((v0 + v1) * 0.5f + curvature, 0.0f, 1.0f);
        float oneMinusT = 1.0f - frac;
        v = oneMinusT * oneMinusT * v0
          + 2.0f * oneMinusT * frac * vMid
          + frac * frac * v1; // quadratic Bezier
      } else {
        v = lerpf(knotVals[k0], knotVals[k1], frac);
      }

      table[ch][i] = (uint16_t) (clampf(v, 0.0f, 1.0f) * 65535.0f);
    }
  }

  // Regenerate all three terrains from `seed` and restart the read phases. The
  // firmware reseeded each table from a fresh hardware-noise read; here a single
  // seeded RNG stream drives all three (deterministic and portable).
  void regenerate(int knots, uint32_t seed) {
    rng.seed(seed);
    for (int c = 0; c < 3; ++c) generateSingleTerrain(c, knots);
    for (int c = 0; c < 3; ++c) {
      phase[c] = 0.0f;
      slow[c] = TerrainSlowMo();
    }
  }

  // Linear-interpolated read of terrain `ch` at its current phase, returned 0..1.
  inline float readTerrain(int ch) const {
    int i0 = (int) phase[ch];
    if (i0 < 0) i0 = 0;
    if (i0 >= kTerrainTableSize) i0 = kTerrainTableSize - 1;
    int i1 = i0 + 1;
    if (i1 >= kTerrainTableSize) i1 = 0;
    float frac = phase[ch] - (float) i0;
    float v0 = table[ch][i0] * (1.0f / 65535.0f);
    float v1 = table[ch][i1] * (1.0f / 65535.0f);
    return lerpf(v0, v1, frac);
  }

  // Advance one emulated loop iteration by `dt` seconds. `baseHz` + `cvHz` set
  // the combined read speed (safety-clamped 0..10 Hz like the firmware), and
  // `intensity` (0..1) the per-iteration SloMo trigger probability. Updates
  // out[0..2] (smoothed, 0..1). The 0.9/0.1 smoothing and the SloMo trigger odds
  // are per-iteration quantities — the firmware ran them once per loop, so this
  // must be stepped at the loop rate (see TerrainLFO.cpp's LOOP_HZ), not per
  // audio sample.
  void step(float dt, float baseHz, float cvHz, float intensity) {
    float tableHz = baseHz + cvHz;
    if (tableHz < 0.0f) tableHz = 0.0f;
    if (tableHz > 10.0f) tableHz = 10.0f;

    static const float detune[3] = {0.9f, 1.0f, 1.1f};

    for (int c = 0; c < 3; ++c) {
      TerrainSlowMo& s = slow[c];

      // Maybe trigger a SloMo breath: probability intensity*5 in 10000 per step.
      if (!s.active && (float) rng.range(0, 10000) < intensity * 5.0f) {
        s.active = true;
        s.slowFactor = rng.range(10, 50) / 100.0f;        // 0.1 .. 0.5x speed
        float durBaseMs = 800.0f + (float) rng.range(200, 3000);
        s.duration = (durBaseMs / (tableHz + 0.1f)) * 0.001f; // ms -> seconds
        s.elapsed = 0.0f;
      }
      if (s.active) {
        s.elapsed += dt;
        if (s.elapsed > s.duration) s.active = false;
      }

      float speed = tableHz * detune[c];
      if (s.active) speed *= s.slowFactor;

      phase[c] += speed * (float) kTerrainTableSize * dt;
      while (phase[c] >= (float) kTerrainTableSize) phase[c] -= (float) kTerrainTableSize;
      while (phase[c] < 0.0f) phase[c] += (float) kTerrainTableSize;

      float v = readTerrain(c);
      out[c] = out[c] * 0.9f + v * 0.1f; // one-pole anti-zipper smoothing
    }
  }
};

}  // namespace sc
