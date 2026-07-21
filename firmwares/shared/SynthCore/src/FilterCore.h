#pragma once

// Filter — tilt EQ + resonant cleanup filters core.
//
// Used by:
//   - firmwares/mod2-filter/mod2-filter.ino
//   - rack-plugins/src/mod2-filter.cpp
//
// The unglamorous patch-saver: tame a harsh source, clean rumble, tilt a mix
// element dark/bright — plus a resonant state-variable filter for bonus
// performance use. Four modes (BUTTON short-press cycles on hardware):
//   Tilt EQ  — low-shelf + high-shelf pair with opposite gains around a pivot
//              (POT1), POT2 bipolar: CCW darker, CW brighter (+/-6 dB).
//   LP / HP  — one 2-pole state-variable filter (POT2 = resonance).
//   BP       — same SVF, band output.
//
// The filter is the Cytomic / Andrew Simper trapezoidal (TPT) SVF: stable to
// Nyquist for any g > 0, k > 0, and cheap per sample (integrator adds/mults
// only). The expensive coefficient maths (tanf / powf / sqrtf) is *not* run per
// sample — it is refreshed at control rate every kCoefRefresh samples from the
// one-pole-smoothed parameters, so there is no zipper and no per-sample
// transcendental in the audio path. The resonant output is soft-saturated
// (sc::softSat) so max-resonance self-oscillation is usable, not a crash.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop. Sample-rate independent: process(in, dt)
// advances by the caller-supplied dt (seconds).

#include <math.h>
#include <stdint.h>

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Filter mode (BUTTON short-press cycles these on hardware).
enum FilterMode : uint8_t {
  FILTER_TILT = 0,  // tilt EQ (shelf pair)
  FILTER_LP = 1,    // 2-pole low-pass, POT2 = resonance
  FILTER_HP = 2,    // 2-pole high-pass, POT2 = resonance
  FILTER_BP = 3,    // 2-pole band-pass, POT2 = resonance
  FILTER_MODE_COUNT = 4
};

// ── POT -> engine-unit mappings (shared so firmware and Rack agree) ──────────

// POT1 -> cutoff / pivot frequency: exponential 20 Hz (pot=0) .. 16 kHz (pot=1).
inline float filterCutoffHz(float pot01) {
  return 20.0f * powf(800.0f, clampf(pot01, 0.0f, 1.0f));  // 20 * 800 = 16000
}

// POT2 (filter modes) -> resonance Q: exponential 0.5 (pot=0) .. 20 (pot=1).
inline float filterResonanceQ(float pot01) {
  return 0.5f * powf(40.0f, clampf(pot01, 0.0f, 1.0f));
}

// POT2 (tilt mode) -> tilt amount in dB: -6 dB (pot=0, darker) .. +6 dB
// (pot=1, brighter); pot=0.5 is flat. Positive = boost highs / cut lows.
inline float filterTiltDb(float pot01) {
  return mapf(clampf(pot01, 0.0f, 1.0f), 0.0f, 1.0f, -6.0f, 6.0f);
}

// ── Trapezoidal (TPT) state-variable filter ──────────────────────────────────
// One SVF stage; setCoef() (control rate) computes the trapezoidal integrator
// gains, tick() (per sample) advances the two integrators and yields the band
// (v1) and low (v2) taps. Unconditionally stable for g > 0, k > 0.
struct Svf {
  float ic1 = 0.0f, ic2 = 0.0f;  // integrator (memory) states
  float g = 0.0f, k = 1.4142f;   // tan(pi*fc*dt), damping = 1/Q
  float a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;

  void reset() {
    ic1 = 0.0f;
    ic2 = 0.0f;
  }

  // Refresh coefficients from cutoff-gain g and damping k (call at control rate,
  // not per sample). a1 = 1/(1 + g*(g+k)) is always finite & positive, so a
  // coefficient change can never make the recursion blow up.
  void setCoef(float gIn, float kIn) {
    g = gIn;
    k = kIn;
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
  }

  // One sample. Returns band (v1) and low (v2) via refs. Integrator states are
  // denormal-flushed so a decaying tail costs no FPU slow-path.
  inline void tick(float v0, float& v1, float& v2) {
    const float v3 = v0 - ic2;
    v1 = a1 * ic1 + a2 * v3;
    v2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2.0f * v1 - ic1;
    ic2 = 2.0f * v2 - ic2;
    if (ic1 < 1e-18f && ic1 > -1e-18f) ic1 = 0.0f;
    if (ic2 < 1e-18f && ic2 > -1e-18f) ic2 = 0.0f;
  }
};

struct FilterCore {
  // Parameters — the platform writes these each block as normalised 0..1 pots
  // (plus mode); the core smooths them and does the unit mapping so the knob
  // maths is shared, not just the DSP.
  float cutoffPot = 0.5f;  // POT1 (A0): cutoff / tilt pivot
  float shapePot = 0.5f;   // POT2 (A1): resonance (filter) or tilt amount (EQ)
  float wet = 1.0f;        // wet/dry mix (shift layer on hardware)
  float trim = 1.0f;       // 0..1 output trim (shift layer on hardware)
  uint8_t mode = FILTER_TILT;

  // Smoothed parameter shadows (one-pole toward the targets above).
  float cutoffSm = 0.5f, shapeSm = 0.5f, wetSm = 1.0f, trimSm = 1.0f;

  // Two SVF stages. Filter modes use svfLow only; tilt cascades svfLow (low
  // shelf) -> svfHigh (high shelf).
  Svf svfLow, svfHigh;

  // Tilt shelf mixing coefficients (Simper shelf form: out = m0*v0+m1*v1+m2*v2).
  float lsM0 = 1.0f, lsM1 = 0.0f, lsM2 = 0.0f;  // low shelf
  float hsM0 = 1.0f, hsM1 = 0.0f, hsM2 = 0.0f;  // high shelf

  int coefCtr = 0;   // control-rate coefficient refresh countdown
  float ledEnv = 0.0f;  // output-level envelope for the panel LED

  static constexpr float kParamSmoothHz = 30.0f;  // param smoothing cutoff
  static constexpr int kCoefRefresh = 16;          // samples between tan/pow refresh
  static constexpr float kShelfK = 1.41421356f;    // 1/Q for the tilt shelves (Q=0.707)

  void reset() {
    svfLow.reset();
    svfHigh.reset();
    cutoffSm = cutoffPot;
    shapeSm = shapePot;
    wetSm = wet;
    trimSm = trim;
    coefCtr = 0;  // force a refresh on the next process()
    ledEnv = 0.0f;
  }

  // 0..1 brightness for the panel LED — follows the output level (envelope).
  float ledLevel() const {
    return clampf(ledEnv, 0.0f, 1.0f);
  }

  // Recompute the (expensive) filter coefficients from the smoothed params.
  // Called every kCoefRefresh samples; never per sample.
  void refreshCoef(float dt) {
    const float nyq = 0.49f / dt;  // keep the pre-warp tan() well inside Nyquist
    const float fc = clampf(filterCutoffHz(cutoffSm), 5.0f, nyq);
    const float g0 = tanf(kPi * fc * dt);

    if (mode == FILTER_TILT) {
      const float tdb = filterTiltDb(shapeSm);
      // Simper shelf: A = 10^(dB/40) (A^2 = linear gain). Low shelf cuts by tdb,
      // high shelf boosts by tdb (opposite gains => a tilt).
      const float aLow = powf(10.0f, -tdb / 40.0f);
      const float aHigh = powf(10.0f, tdb / 40.0f);
      svfLow.setCoef(g0 / sqrtf(aLow), kShelfK);
      lsM0 = 1.0f;
      lsM1 = kShelfK * (aLow - 1.0f);
      lsM2 = aLow * aLow - 1.0f;
      svfHigh.setCoef(g0 * sqrtf(aHigh), kShelfK);
      hsM0 = aHigh * aHigh;
      hsM1 = kShelfK * (1.0f - aHigh) * aHigh;
      hsM2 = 1.0f - aHigh * aHigh;
    } else {
      const float q = filterResonanceQ(shapeSm);
      svfLow.setCoef(g0, 1.0f / q);
    }
  }

  // Advance one sample of `dt` seconds; returns the wet/dry + trimmed output.
  float process(float in, float dt) {
    // One-pole smooth every user parameter (no zipper on knob moves).
    const float ps = onePoleCoef(kParamSmoothHz, dt);
    cutoffSm += (cutoffPot - cutoffSm) * ps;
    shapeSm += (shapePot - shapeSm) * ps;
    wetSm += (wet - wetSm) * ps;
    trimSm += (trim - trimSm) * ps;

    // Control-rate coefficient refresh — keeps tanf/powf out of the audio path.
    if (--coefCtr <= 0) {
      refreshCoef(dt);
      coefCtr = kCoefRefresh;
    }

    float wetSig;
    if (mode == FILTER_TILT) {
      float v1, v2;
      svfLow.tick(in, v1, v2);
      const float ls = lsM0 * in + lsM1 * v1 + lsM2 * v2;  // low shelf
      svfHigh.tick(ls, v1, v2);
      wetSig = hsM0 * ls + hsM1 * v1 + hsM2 * v2;           // high shelf
    } else {
      float v1, v2;
      svfLow.tick(in, v1, v2);
      const float low = v2;
      const float band = v1;
      const float high = in - svfLow.k * v1 - v2;
      wetSig = (mode == FILTER_LP) ? low : (mode == FILTER_HP) ? high : band;
    }

    // Soft-saturate the wet path only (dry stays a clean bypass at wet=0):
    // bounds a +6 dB tilt boost and keeps max-resonance self-oscillation
    // usable rather than a blow-up.
    wetSig = softSat(wetSig);

    // Wet/dry mix, then output trim.
    float out = (in + (wetSig - in) * wetSm) * trimSm;

    // LED envelope follows the output level (fast attack, slow release).
    const float mag = fabsf(out);
    ledEnv += (mag - ledEnv) * onePoleCoef(mag > ledEnv ? 200.0f : 8.0f, dt);

    return out;
  }
};

}  // namespace sc
