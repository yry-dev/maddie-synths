#pragma once

// Crackle voice — random snaps, crackles and pops, like a worn record or an
// old-time radio between stations.
//
// Used by:
//   - firmwares/mod2-crackle/mod2-crackle.ino  (one sample per PWM-wrap ISR)
//   - rack-plugins/src/mod2-crackle.cpp        (one sample per process())
//
// Ported from Sean Luke's GRAINS `crackle` firmware. Upstream is a Mozzi sketch
// running at 16384 Hz whose whole engine is three integer registers; this core
// keeps its *statistics* rather than its arithmetic, so the same sound survives
// a move to floats and a different sample rate:
//
//   ARRIVALS  Upstream rolls `rand(0, rate * 64) == 0` on every audio sample,
//             where `rate` settles at (POT1 >> 2) + 2, i.e. 2..257. That is a
//             Bernoulli trial with mean arrival rate 16384 / (rate * 64) =
//             256 / rate crackles per second — 128 Hz at one end of the pot,
//             just under 1 Hz at the other. We keep the Bernoulli trial but
//             scale it by the caller's dt, so the *rate in hertz* is preserved
//             at any sample rate instead of the per-sample probability.
//   LENGTH    Upstream draws four uniform values in 1..POT3 samples and keeps
//             the SHORTEST. That min-of-four is what makes most crackles a
//             tick and only occasionally a scratch — it is the character of the
//             module, so it is reproduced exactly, converted to seconds.
//   AMPLITUDE Upstream multiplies a uniform 0..255 by a uniform 0..GAIN and
//             clips at full scale, which is why the GAIN pot behaves as a
//             *variance* control: low settings give quiet crackles that differ
//             from one another, high settings drive most of them into the clip
//             so they arrive at maximum volume. Same product-of-uniforms law
//             here, expressed in normalised full-scale units.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h. No Arduino / Rack / Pico SDK;
// float-only, no heap, no STL.
//
// License:
// Derived from the GRAINS `crackle` firmware (github.com/eclab/grains) under
// the Apache License 2.0, Copyright 2024 Sean Luke. The upstream notice
// lives at firmwares/mod2-crackle/LICENSE.md and Apache 2.0 requires it to ship
// with any copy of this header — the CC0 cores next to it have no such
// condition, so don't fold this one into them.

#include "sc_math.h"
#include "sc_dsp.h"

namespace sc {

// GRAINS runs Mozzi at 16384 Hz. Upstream's rate and length registers are both
// counted in samples at that rate, so every conversion to seconds goes through
// this constant — it is a property of the original firmware, not of the host.
constexpr float kCrackleRefFs = 16384.0f;

// Upstream amplitude: r = (u8 * g) >> 8 clipped to +/-128 full scale, with
// u8 uniform in 0..255 and g uniform in 0..GAIN-1 (GAIN = a 10-bit pot).
// Normalised, that is u1 * u2 * (255 * 1023 / 32768) * gain01.
constexpr float kCrackleGainSpan = 255.0f * 1023.0f / 32768.0f;  // ~7.9585

// Upstream clips at +127 out of a 128 full scale; keep the same ceiling so the
// proportion of crackles that reach "maximum volume" matches.
constexpr float kCrackleClip = 127.0f / 128.0f;

// Longest crackle upstream can draw: POT3 >> 2 gives 0..255 samples at 16384 Hz.
constexpr float kCrackleMaxLenSec = 255.0f / kCrackleRefFs;  // ~15.6 ms

struct CrackleFrame {
  float audio;  // -1..+1 in analog mode, 0 or +1 in digital mode
  float env;    // 1 while a crackle is sounding, else 0 (LED / gate)
};

struct CrackleVoice {
  // ── Mapped parameters (set by setParams / setDigital / setAccent) ────────
  float lambdaHz = 16.0f;  // mean crackle arrivals per second
  float gain = 0.5f;       // 0..1, upstream's GAIN pot (a variance control)
  float lenSec = 0.004f;   // longest crackle a draw can produce, seconds
  bool digital = false;    // false = analog crackles, true = clean gate pops
  bool accent = false;     // while true, every crackle is at full volume

  // ── State ────────────────────────────────────────────────────────────────
  uint32_t rng = 0x1a2b3c4du;  // xorshift32 seed (any non-zero value)
  float remaining = 0.0f;      // seconds left in the crackle being sounded
  bool busy = false;           // a crackle is sounding right now
  float noiseHold = 0.0f;      // burst sample held between 16384 Hz redraws
  float noiseAccum = 0.0f;     // seconds until the next redraw

  void reset() {
    rng = 0x1a2b3c4du;
    remaining = 0.0f;
    busy = false;
    accent = false;
    noiseHold = 0.0f;
    noiseAccum = 0.0f;
  }

  // Density 0..1 -> arrivals per second. Upstream's POT1 is labelled "rate" but
  // holds a *period* divisor, so turning it up makes crackling rarer; we invert
  // it here (clockwise = denser) and keep the 128 Hz..1 Hz span it produced.
  static float densityToLambda(float density01) {
    const float rate = 2.0f + (1.0f - clampf(density01, 0.0f, 1.0f)) * 255.0f;
    return 256.0f / rate;  // = kCrackleRefFs / (rate * 64)
  }

  // All three panel controls, normalised 0..1.
  void setParams(float density01, float gain01, float length01) {
    lambdaHz = densityToLambda(density01);
    gain = clampf(gain01, 0.0f, 1.0f);
    lenSec = clampf(length01, 0.0f, 1.0f) * kCrackleMaxLenSec;
  }

  // Upstream's second output: clean full-volume pops instead of noisy ones.
  void setDigital(bool on) { digital = on; }

  // Upstream summed a GAIN CV into POT2; both targets only have a gate there,
  // so a high gate is treated as "full GAIN" for as long as it is held.
  void setAccent(bool on) { accent = on; }

  // Force a crackle now (manual button / trigger jack). Restarts one already in
  // flight so a tap always makes a sound.
  void trigger() { startCrackle(); }

  // Advance one sample of `dt` seconds (sample-rate independent).
  CrackleFrame process(float dt) {
    if (!busy && rand01() < lambdaHz * dt)
      startCrackle();

    CrackleFrame f = {0.0f, 0.0f};
    if (!busy)
      return f;

    if (digital) {
      // Upstream's digital out sits at 0 and jumps to full scale for the whole
      // crackle: unipolar, always maximum volume, GAIN has no effect on it.
      f.audio = 1.0f;
    }
    else {
      // Redraw the burst noise at upstream's 16384 Hz and hold in between, so
      // the crackle's bandwidth — its timbre — does not scale with the host
      // sample rate (at 96 kHz a per-sample redraw would be far brighter than
      // the original, with energy past 20 kHz).
      noiseAccum -= dt;
      if (noiseAccum <= 0.0f) {
        const float g = accent ? 1.0f : gain;
        float a = rand01() * rand01() * kCrackleGainSpan * g;
        if (a > kCrackleClip)
          a = kCrackleClip;
        // Upstream masks its PRNG to 8 bits before reinterpreting it as
        // signed, so every burst sample comes out positive and the "pop"
        // carries a DC step. We keep the magnitude law and randomise the sign
        // instead, which centres the burst without changing how loud any
        // crackle is.
        noiseHold = randSign() * a;
        noiseAccum += 1.0f / kCrackleRefFs;
        if (noiseAccum < 0.0f)
          noiseAccum = 0.0f;  // fs below 16384 Hz: redraw every sample
      }
      f.audio = noiseHold;
    }
    f.env = 1.0f;

    remaining -= dt;
    if (remaining <= 0.0f) {
      remaining = 0.0f;
      busy = false;
    }
    return f;
  }

 private:
  // Uniform in [0, 1).
  float rand01() {
    return (float)(xorshift32(rng) >> 8) * (1.0f / 16777216.0f);
  }

  float randSign() { return (xorshift32(rng) & 0x80000000u) ? -1.0f : 1.0f; }

  // Upstream draws four candidate lengths in 1..POT3 samples and keeps the
  // shortest, which skews the distribution hard towards brief ticks.
  void startCrackle() {
    float dur = kCrackleMaxLenSec + 1.0f;  // longer than any possible draw
    for (int i = 0; i < 4; ++i) {
      const float c = rand01() * lenSec + 1.0f / kCrackleRefFs;
      if (c < dur)
        dur = c;
    }
    remaining = dur;
    busy = true;
    noiseAccum = 0.0f;  // fresh draw on the crackle's first sample
  }
};

}  // namespace sc
