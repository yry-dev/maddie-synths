#pragma once

// Chordal voice — four-note chording oscillator that cross-fades every note
// between a sine and a square / saw / triangle of the same pitch.
//
// Used by:
//   - firmwares/mod2-chordal/mod2-chordal.ino  (ISR-driven, ~36.6 kHz, RP2350)
//   - rack-plugins/src/mod2-chordal.cpp        (host sample rate)
//
// The engine holds four note slots. Slot 0 is the fundamental; slots 1..3 are
// the chord tones, tuned by the semitone offsets in the chord table and pushed
// up in octaves by the inversion table. Each slot renders one sine and one
// "edge" wave from the SAME phase accumulator, and the output is the mean of
// the active slots, cross-faded by `mix`.
//
// Two things the upstream Mozzi build did with lookup tables are closed-form
// here, because the RP2350 has an FPU and the Rack host has no fixed rate:
//   - the sine is sinf() instead of a 512-point int8 table, and
//   - the band-limited square / saw come from PolyBLEP instead of MetaOscil
//     switching between 17 pre-filtered wavetables. PolyBLEP is sample-rate
//     independent, which the fixed 16384 Hz table set is not.
// The triangle is naive (its harmonics fall off as 1/n^2, so audible aliasing
// is negligible) — the same choice VcoCore makes.
//
// Pure C++: depends only on sc_math.h -> <math.h>/<stdint.h>. No Arduino.h, no
// rack.hpp, no Pico SDK. float only, no heap, no static mutable state.
//
// License:
// Derived from the GRAINS `chordal` firmware (github.com/eclab/grains) under
// the Apache License 2.0, Copyright 2023 Sean Luke. The upstream notice
// lives at firmwares/mod2-chordal/LICENSE.md and Apache 2.0 requires it to ship
// with any copy of this header — the CC0 cores next to it have no such
// condition, so don't fold this one into them.

#include "sc_math.h"

namespace sc {

// Which wave the sine is mixed against. Upstream picked one of these at compile
// time with a #define (USE_SQUARE / USE_SAW / USE_TRI); here it is a runtime
// field so the panel button can cycle it.
enum ChordalWave : uint8_t {
  kChordalSquare = 0,
  kChordalSaw = 1,
  kChordalTri = 2,
  kChordalWaveCount = 3
};

constexpr uint8_t kChordalNumChords = 24;
constexpr uint8_t kChordalNumInversions = 8;
constexpr uint8_t kChordalMaxNotes = 4;

// Upstream's `chords[24][4]`, verbatim. Element [0] is the number of notes
// ABOVE the fundamental (0..3); elements [1..3] are those notes' semitone
// offsets from the fundamental, 0 where the note does not exist.
inline const uint8_t* chordalChord(uint8_t idx) {
  static const uint8_t kChords[kChordalNumChords][kChordalMaxNotes] = {
      {0, 0, 0, 0},    // None
      {1, 3, 0, 0},    // m3
      {1, 4, 0, 0},    // M3
      {1, 5, 0, 0},    // 4
      {1, 7, 0, 0},    // 5
      {1, 8, 0, 0},    // m6
      {1, 9, 0, 0},    // M6
      {1, 10, 0, 0},   // m7
      {1, 12, 0, 0},   // Octave
      {1, 15, 0, 0},   // Octave + m3
      {1, 16, 0, 0},   // Octave + M3
      {1, 19, 0, 0},   // Octave + 5
      {2, 3, 7, 0},    // min
      {2, 4, 9, 0},    // min-1
      {2, 5, 8, 0},    // min-2
      {2, 4, 7, 0},    // Maj
      {2, 3, 8, 0},    // Maj-1
      {2, 5, 9, 0},    // Maj-2
      {3, 4, 7, 10},   // 7
      {3, 3, 7, 10},   // min7
      {3, 4, 7, 11},   // Maj7
      {3, 3, 6, 9},    // dim7
      {3, 3, 7, 12},   // min + Octave
      {3, 4, 7, 12},   // Maj + Octave
  };
  return kChords[idx < kChordalNumChords ? idx : kChordalNumChords - 1];
}

// Upstream's `inversions[4][4][8]`, verbatim: the octave multiplier (1, 2, 4
// or 8) applied to note `note` of a chord that has `extraNotes` tones above the
// fundamental, at inversion `inv`. Raising the multiplier of the lowest note
// walks the chord up one voicing at a time.
inline uint8_t chordalInversionMul(uint8_t extraNotes, uint8_t note, uint8_t inv) {
  static const uint8_t kInversions[kChordalMaxNotes][kChordalMaxNotes][kChordalNumInversions] = {
      {{1, 1, 2, 2, 4, 4, 8, 8}, {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0}},
      {{1, 1, 2, 2, 2, 2, 4, 4}, {1, 1, 1, 1, 2, 2, 2, 2}, {0, 0, 0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0, 0, 0}},
      {{1, 2, 2, 2, 4, 4, 4, 8}, {1, 1, 2, 2, 2, 4, 4, 4}, {1, 1, 1, 2, 2, 2, 4, 4}, {0, 0, 0, 0, 0, 0, 0, 0}},
      {{1, 2, 2, 2, 2, 4, 4, 4}, {1, 1, 2, 2, 2, 2, 4, 4}, {1, 1, 1, 2, 2, 2, 2, 4}, {1, 1, 1, 1, 2, 2, 2, 2}}};
  if (extraNotes >= kChordalMaxNotes) extraNotes = kChordalMaxNotes - 1;
  if (note >= kChordalMaxNotes) note = kChordalMaxNotes - 1;
  if (inv >= kChordalNumInversions) inv = kChordalNumInversions - 1;
  return kInversions[extraNotes][note][inv];
}

// Upstream's `semitoneFrequencyRatios[]` (0..24 semitones), narrowed to float.
// Kept as a table rather than exp2f(s/12) so the chord tuning is bit-for-bit
// the ratios Sean Luke shipped, and so no exp/pow runs in the audio path.
inline float chordalSemitoneRatio(uint8_t semis) {
  static const float kRatios[25] = {
      1.0f,          1.0594631f,  1.1224620f,  1.1892071f,  1.2599211f,
      1.3348399f,    1.4142136f,  1.4983071f,  1.5874011f,  1.6817928f,
      1.7817974f,    1.8877486f,  2.0f,        2.1189262f,  2.2449241f,
      2.3784142f,    2.5198421f,  2.6696797f,  2.8284271f,  2.9966142f,
      3.1748021f,    3.3635857f,  3.5635949f,  3.7754973f,  4.0f};
  return kRatios[semis < 25 ? semis : 24];
}

// PolyBLEP discontinuity residual for a step at phase 0. `t` and `dt` are both
// normalised to one period. Named for this core rather than reusing VcoCore's
// identical `sc::polyBLEP` because a core header may not include another core.
inline float chordalPolyBLEP(float t, float dt) {
  if (t < dt) {
    const float x = t / dt;
    return x + x - x * x - 1.0f;
  }
  if (t > 1.0f - dt) {
    const float x = (t - 1.0f) / dt;
    return x * x + x + x + 1.0f;
  }
  return 0.0f;
}

struct ChordalVoice {
  // Fundamental in Hz. Upstream derived this from a 1536-entry frequency table
  // indexed at 1/17 semitone; both platforms here use V/oct arithmetic instead.
  float rootFreq = 32.703f;  // C0 — the note upstream plays at 0 V
  uint8_t chord = 0;         // 0..23, index into chordalChord()
  float mix = 0.0f;          // 0 = all sine, 1 = all square/saw/tri
  uint8_t inversion = 0;     // 0..7
  uint8_t wave = kChordalSquare;

  void reset() {
    for (uint8_t i = 0; i < kChordalMaxNotes; i++) phase[i] = 0.0f;
  }

  // Render one sample and advance by dt seconds. Returns audio in -1..+1.
  float process(float dt) {
    const uint8_t* c = chordalChord(chord);
    const uint8_t notes = c[0] + 1;  // fundamental + chord tones
    const uint8_t m = clampWave();

    // Highest note a slot may reach. Deep inversions of a wide chord multiply
    // the root by up to 8 x 4, which can pass Nyquist at the top of the pitch
    // range; clamping there costs the chord its tuning in that corner but keeps
    // the fold-back out of the audio.
    const float maxFreq = 0.45f / dt;

    float sines = 0.0f;
    float edges = 0.0f;
    for (uint8_t i = 0; i < notes; i++) {
      const float ratio = (i == 0) ? 1.0f : chordalSemitoneRatio(c[i]);
      const float oct = (float)chordalInversionMul(c[0], i, inversion);
      float f = rootFreq * oct * ratio;
      if (f < 0.0f) f = 0.0f;
      if (f > maxFreq) f = maxFreq;

      const float t = phase[i];
      const float dp = f * dt;

      sines += sinf(kTwoPi * t);

      float e;
      if (m == kChordalSquare) {
        e = (t < 0.5f) ? 1.0f : -1.0f;
        e += chordalPolyBLEP(t, dp);
        e -= chordalPolyBLEP(fmodf(t + 0.5f, 1.0f), dp);  // falling edge
      } else if (m == kChordalSaw) {
        e = 2.0f * t - 1.0f;
        e -= chordalPolyBLEP(t, dp);
      } else {
        e = 1.0f - 4.0f * fabsf(t - 0.5f);
      }
      edges += e;

      float p = t + dp;
      p -= floorf(p);
      phase[i] = p;
    }

    // Upstream mixed with integer weights (alpha and 255-alpha, each divided by
    // the note count via >>1 / div3 / >>2) and then scaled the sum into Mozzi's
    // +/-244 PWM headroom, landing at ~0.69 of full scale. Here the mean of the
    // active notes is already bounded by 1, so the port keeps full scale.
    const float g = 1.0f / (float)notes;
    const float w = clampf(mix, 0.0f, 1.0f);
    return (edges * w + sines * (1.0f - w)) * g;
  }

private:
  float phase[kChordalMaxNotes] = {0.0f, 0.0f, 0.0f, 0.0f};

  uint8_t clampWave() const {
    return wave < kChordalWaveCount ? wave : (uint8_t)kChordalSquare;
  }
};

}  // namespace sc
