#pragma once

// Comb — tuned feedforward / feedback comb filter core.
//
// Used by:
//   - firmwares/mod2-comb/mod2-comb.ino
//   - rack-plugins/src/mod2-comb.cpp
//
// A single, precise, performable comb filter — the simplest way to make any
// source "ring" (metallic tines, plucked-string colour, hollow square tones).
// (The multi-comb *cluster* lives in ResonatorCore's COMB mode; this is the
// one, tuned, playable comb.) Three modes:
//   FEEDBACK    — y = in + g·y[n-N]: resonant peaks at every harmonic of the
//                 tuned frequency; positive g = full harmonic series, negative
//                 g = odd-only (hollow, square-ish). At |g|→1 it self-oscillates
//                 into a sine-ish drone (soft-limited, never clips).
//   FEEDFORWARD — y = in + g·x[n-N]: non-resonant comb (flanger-like notches),
//                 always stable regardless of g.
//   BOTH        — a Schroeder all-pass (feedforward + feedback nested on one
//                 delay): flat magnitude, dispersive "shimmer" ring.
// A one-pole low-pass in the feedback path (damping) rolls off the ring's high
// end so high feedback settles into a rounded, sine-ish tone instead of a buzz.
// Pitch is expected pre-quantized when the caller wants it — see combFreqHz().
//
// The delay memory is a caller-provided int16 arena over sc::DelayLine — static
// SRAM in firmware, a std::vector sized per engine rate in Rack. Size it for the
// lowest tuned frequency (combArenaSamples()). The fractional (interpolated)
// read keeps the tuning in-tune between integer sample steps, and the delay
// length is one-pole smoothed so sweeping the tune knob glides without clicks.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop.

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Comb topology (BUTTON short-press cycles these on hardware).
enum CombMode : uint8_t {
  COMB_FEEDBACK = 0,     // resonant comb (rings / self-oscillates)
  COMB_FEEDFORWARD = 1,  // non-resonant comb (notches, always stable)
  COMB_BOTH = 2,         // nested all-pass (dispersive shimmer)
  COMB_MODE_COUNT = 3
};

// Tuning range: 20 Hz .. 2 kHz, exponential taper (~6.6 octaves).
constexpr float kCombMinFreq = 20.0f;
constexpr float kCombMaxFreq = 2000.0f;

// Near-unity so the feedback extremes ring/self-oscillate; the in-loop
// soft-limiter (softSat) keeps that bounded rather than clipping.
constexpr float kCombMaxFeedback = 0.998f;

// POT1 -> tuned frequency (Hz). `quantize` snaps to the nearest equal-tempered
// semitone (the default on hardware); free-tune otherwise. Shared so firmware
// and Rack tune identically.
inline float combFreqHz(float pot01, bool quantize) {
  float f = kCombMinFreq * powf(kCombMaxFreq / kCombMinFreq, clampf(pot01, 0.0f, 1.0f));
  if (quantize) {
    const float semis = floorf(log2f(f / kCombMinFreq) * 12.0f + 0.5f);
    f = kCombMinFreq * powf(2.0f, semis * (1.0f / 12.0f));
  }
  return f;
}

// POT2 -> bipolar feedback: CCW (pot=0) = negative comb (odd harmonics,
// hollow), centre = none, CW (pot=1) = positive comb (full harmonic series).
inline float combFeedback(float pot01) {
  return (clampf(pot01, 0.0f, 1.0f) - 0.5f) * 2.0f * kCombMaxFeedback;
}

// Arena length for `fsHz`: one full period at the lowest tuned frequency, plus
// interpolation margin. int16 storage -> ~3.7 KB at 36.6 kHz (within the 4 KB
// budget), a little more at 48 kHz in Rack.
inline uint32_t combArenaSamples(float fsHz) {
  return (uint32_t)(fsHz / kCombMinFreq) + 16;
}

struct CombCore {
  // Parameters (write directly; see the mappers above).
  float freqHz = 220.0f;             // tuned frequency
  float feedback = 0.0f;             // -kMaxFb..+kMaxFb (bipolar; see mapper)
  float damping = 0.3f;              // 0 bright .. 1 dark (LP in the loop)
  float wet = 0.5f;                  // 0 dry .. 1 fully wet
  uint8_t mode = COMB_FEEDBACK;      // CombMode
  bool fbKill = false;               // IN2 gate: mute feedback while true

  // State.
  DelayLine line;
  float delaySmooth = 0.0f;          // smoothed delay length (samples)
  float fbSmooth = 0.0f;             // smoothed feedback amount
  float dampSmooth = 0.3f;           // smoothed damping amount
  float wetSmooth = 0.5f;            // smoothed wet/dry
  float loopLp = 0.0f;               // one-pole LP state in the feedback path
  float energy = 0.0f;               // smoothed |wet| for the LED
  bool primed = false;               // first-sample smoother snap

  // `buf`/`n`: caller-owned arena (combArenaSamples() long).
  void init(int16_t* buf, uint32_t n) {
    line.init(buf, n);
    reset();
  }

  void reset() {
    line.clear();
    delaySmooth = 0.0f;
    fbSmooth = 0.0f;
    dampSmooth = damping;
    wetSmooth = wet;
    loopLp = 0.0f;
    energy = 0.0f;
    primed = false;
  }

  // 0..1 brightness for the panel LED — follows the comb's resonant energy.
  float ledLevel() const { return clampf(energy * 3.0f, 0.0f, 1.0f); }

  // Flush denormals in the recirculating state (cheap, keeps the loop fast
  // during long silences at high feedback).
  static inline float fdenorm(float x) {
    return (x < 1e-15f && x > -1e-15f) ? 0.0f : x;
  }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    // Target delay length for the tuned frequency (samples).
    const float dTarget =
        clampf((1.0f / dt) / freqHz, 2.0f, (float)(line.len - 2));

    if (!primed) {  // snap the smoothers so startup doesn't chirp/zip
      delaySmooth = dTarget;
      fbSmooth = feedback;
      dampSmooth = damping;
      wetSmooth = wet;
      primed = true;
    }

    // One-pole smooth every user param (no zipper) and the delay time (glide,
    // click-free tune sweeps).
    delaySmooth += (dTarget - delaySmooth) * onePoleCoef(20.0f, dt);
    fbSmooth += (feedback - fbSmooth) * onePoleCoef(30.0f, dt);
    dampSmooth += (damping - dampSmooth) * onePoleCoef(30.0f, dt);
    wetSmooth += (wet - wetSmooth) * onePoleCoef(30.0f, dt);

    const float fb = fbKill ? 0.0f : fbSmooth;

    // Damping = a one-pole low-pass in the loop; more damping -> lower cutoff.
    const float cutoff = mapClampf(dampSmooth, 0.0f, 1.0f, 14000.0f, 300.0f);
    const float dampCoef = onePoleCoef(cutoff, dt);

    float wetSig;
    switch (mode) {
      case COMB_FEEDFORWARD: {
        // Non-resonant: delay a copy of the dry input and mix it back.
        line.write(in);
        const float delayed = line.read(delaySmooth);
        loopLp += (delayed - loopLp) * dampCoef;
        loopLp = fdenorm(loopLp);
        wetSig = softSat(in + fb * loopLp);
        break;
      }
      case COMB_BOTH: {
        // Schroeder all-pass: nested feedforward + feedback on one delay.
        const float delayed = line.read(delaySmooth);  // w[n-N]
        loopLp += (delayed - loopLp) * dampCoef;
        loopLp = fdenorm(loopLp);
        const float w = softSat(in + fb * loopLp);      // w[n]
        line.write(w);
        wetSig = -fb * w + loopLp;                      // y[n]
        break;
      }
      case COMB_FEEDBACK:
      default: {
        // Resonant: recirculate the (damped) output back into the delay.
        const float delayed = line.read(delaySmooth);   // y[n-N]
        loopLp += (delayed - loopLp) * dampCoef;
        loopLp = fdenorm(loopLp);
        wetSig = softSat(in + fb * loopLp);             // y[n], soft-limited
        line.write(wetSig);
        break;
      }
    }

    energy += (fabsf(wetSig) - energy) * onePoleCoef(6.0f, dt);

    return in + (wetSig - in) * wetSmooth;
  }
};

}  // namespace sc
