#pragma once

// Tremolo — LFO-driven VCA (amplitude modulation), clock-syncable.
//
// Used by:
//   - firmwares/mod2-tremolo/mod2-tremolo.ino
//   - rack-plugins/src/mod2-tremolo.cpp
//
// One LFO, one multiply: the whole audio-in -> process -> out path with
// trivially audible behaviour. A single low-frequency oscillator produces a
// unipolar 0..1 shape that scales the input gain from full (peak) down to
// (1 - depth) (trough); depth == 1 is a full chop to silence. Four LFO shapes
// (BUTTON short-press cycles them on hardware): sine / triangle / square /
// ramp-down. The square is one-pole smoothed (~2 ms) so the hard edges don't
// click, and the same smoother rounds the ramp's wrap discontinuity. Rate,
// depth and wet/dry are one-pole smoothed so knob moves never zipper. IN2 on
// hardware resets the LFO phase (resetPhase()) so the chop can be pinned to a
// downbeat, and the LED breathes with the LFO (ledLevel()).
//
// The rate can come from POT1 (tremoloRateHz), from a tapped tempo, or locked
// to a musical ratio of an external clock (tremoloClockRatio) — the platform
// measures the clock / tap and writes the resulting Hz into rateHz, exactly as
// mod2-delay drives DelayFxCore.timeSec. The core only ever sees a rate in Hz,
// so it stays sample-rate independent and platform-agnostic.
//
// No delay memory is needed (a VCA has no buffer), so there is no init(arena):
// just reset(). Pure C++: depends only on sc_math.h / sc_dsp.h
// (<math.h>/<stdint.h>). No Arduino.h, rack.hpp or Pico SDK; float only, no
// heap, no STL — compiles on AVR, RP2350 and the desktop.

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// LFO waveform (BUTTON short-press cycles these on hardware).
enum TremoloShape : uint8_t {
  TREMOLO_SINE = 0,      // smooth, classic amp tremolo
  TREMOLO_TRIANGLE = 1,  // linear rise/fall
  TREMOLO_SQUARE = 2,    // hard on/off chop (one-pole smoothed to declick)
  TREMOLO_RAMP = 3,      // ramp down — sharp attack, slow release "stutter"
  TREMOLO_SHAPE_COUNT = 4
};

// POT1 -> LFO rate in Hz: exponential taper 0.1 Hz (pot=0) .. 30 Hz (pot=1).
inline float tremoloRateHz(float pot01) {
  return 0.1f * powf(300.0f, clampf(pot01, 0.0f, 1.0f));
}

// POT2 -> modulation depth 0..1 (0 = no tremolo, 1 = full chop to silence).
// Chosen mapping is amplitude-linear: the trough gain is (1 - depth), so 50%
// depth dips the audio to exactly half amplitude (-6 dB). This is the simple,
// defensible reading of the plan's "equal-loudness-ish" note — half the knob
// gives half the amplitude swing — and keeps the control predictable without a
// per-sample curve. The top of the knob reaches a genuine full chop, which
// reads as "square-ish" the way the plan wants when paired with the square LFO.
inline float tremoloDepth(float pot01) {
  return clampf(pot01, 0.0f, 1.0f);
}

// Clock-synced ratios: with a clock on IN1, POT1 picks a musical division /
// multiple of the measured clock rate instead of an absolute rate. The tremolo
// rate is clockHz * ratio, spanning the plan's {1/4 .. 4x} of the clock.
constexpr float kTremoloClockRatios[8] = {
    0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f};

inline float tremoloClockRatio(float pot01) {
  int idx = (int)(clampf(pot01, 0.0f, 1.0f) * 7.999f);
  return kTremoloClockRatios[idx];
}

// Unipolar 0..1 LFO shape from a 0..1 phase (1 = peak / full volume, 0 =
// trough). Ramp-down starts at the peak on the downbeat and falls to the
// trough, so a phase reset lands on the loudest point.
inline float tremoloShape(float phase01, uint8_t shape) {
  phase01 -= floorf(phase01);
  switch (shape) {
    case TREMOLO_TRIANGLE:
      // Peak (1) at phase 0, trough (0) at phase 0.5, back to 1 at phase 1.
      return 2.0f * fabsf(phase01 - 0.5f);
    case TREMOLO_SQUARE:
      return phase01 < 0.5f ? 1.0f : 0.0f;
    case TREMOLO_RAMP:
      return 1.0f - phase01;  // fast attack on the wrap, linear release
    case TREMOLO_SINE:
    default:
      // cos so phase 0 is the peak (matches the other shapes' downbeat).
      return 0.5f + 0.5f * cosf(kTwoPi * phase01);
  }
}

struct TremoloCore {
  // Parameters (write directly; see the mappers above).
  float rateHz = 4.0f;              // LFO rate (Hz) — platform computes it
  float depth = 0.5f;               // 0..1 modulation excursion
  float wet = 1.0f;                 // 0 dry .. 1 fully modulated
  uint8_t shape = TREMOLO_SINE;     // TremoloShape

  // Smoothing cutoffs (Hz). Rate/depth/wet ~ knob smoothing; the shape gets a
  // ~2 ms one-pole (≈80 Hz) so the square edges and ramp wrap don't click.
  static constexpr float kParamSmoothHz = 12.0f;
  static constexpr float kShapeSmoothHz = 80.0f;

  // State.
  float phase = 0.0f;      // LFO phase, 0..1
  float rateSm = 4.0f;     // smoothed rate
  float depthSm = 0.5f;    // smoothed depth
  float wetSm = 1.0f;      // smoothed wet/dry
  float shapeSm = 1.0f;    // smoothed unipolar shape (drives gain + LED)

  void reset() {
    phase = 0.0f;
    rateSm = rateHz;
    depthSm = depth;
    wetSm = wet;
    shapeSm = 1.0f;
  }

  // Pin the LFO to a downbeat (IN2 on hardware / a reset trigger in Rack).
  void resetPhase() { phase = 0.0f; }

  // 0..1 brightness for the panel LED — breathes with the (smoothed) LFO.
  float ledLevel() const { return shapeSm; }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    // Smooth the user params so knob moves never zipper the gain.
    const float pc = onePoleCoef(kParamSmoothHz, dt);
    rateSm += (rateHz - rateSm) * pc;
    depthSm += (depth - depthSm) * pc;
    wetSm += (wet - wetSm) * pc;

    // Advance the LFO.
    phase += rateSm * dt;
    phase -= floorf(phase);

    // Raw shape, then one-pole smoothed to declick the square/ramp edges.
    const float raw = tremoloShape(phase, shape);
    shapeSm += (raw - shapeSm) * onePoleCoef(kShapeSmoothHz, dt);
    shapeSm = flushDenorm(shapeSm);

    // Gain: 1 at the peak, (1 - depth) at the trough. Full chop at depth 1.
    const float gain = 1.0f - depthSm * (1.0f - shapeSm);
    const float trem = in * gain;

    // Wet/dry between the dry input and the tremolo'd signal.
    return in + (trem - in) * wetSm;
  }

 private:
  // Flush sub-normal magnitudes to zero (denormal-safe smoothing state).
  static inline float flushDenorm(float x) {
    return (x < 1.0e-25f && x > -1.0e-25f) ? 0.0f : x;
  }
};

}  // namespace sc
