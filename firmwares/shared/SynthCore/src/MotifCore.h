#pragma once

// Motif — a 16-step random melody generator: "Topograf for notes".  You pick a
// point in a three-dimensional space and it hands you a short melody.  The axes
// are the base PATTERN (four 4-step groups, each rising or falling, in major or
// minor), a fixed deterministic NOISE deviation selected by RANDOM, and the
// VARIANCE that scales how far each step may jump off the pattern.  Clock it to
// walk the 16 steps; reset it to return to step 0, where the starting note is
// re-derived from the pattern.
//
// Used by:
//   - firmwares/mod1-motif/mod1-motif.ino
//   - rack-plugins/src/mod1-motif.cpp
//
// Pure C++: sc_math.h only. NO Arduino.h, rack.hpp, Pico SDK.
// float only, no heap, no STL — must compile on AVR, RP2350 and desktop.
//
// Do not reach for sc_dsp.h here: it does not compile on AVR, because Biquad's
// B0..B2 / A1..A2 members collide with macros out of Arduino.h, and a MOD1
// sketch includes both. The deviation generator below is therefore written out
// inline rather than borrowed, as GeigerCore.h and RandomCvCore.h do for theirs.
//
// This is a clocked utility rather than a per-sample voice, so it follows the
// ClkCore.h shape: one `step()` per clock edge, no dt.  The pitch it returns is
// a semitone offset; each platform decides what a semitone is worth in volts.
//
// Derived from the GRAINS `motif` firmware (github.com/eclab/grains) under
// the Apache License 2.0, Copyright 2023 Sean Luke. The upstream notice
// lives at firmwares/mod1-motif/LICENSE.md and Apache 2.0 requires it to ship
// with any copy of this header — the CC0 cores next to it have no such
// condition, so don't fold this one into them.

#include "sc_math.h"

namespace sc {

// Steps in one motif. Position wraps here; RESET also returns to 0.
constexpr uint8_t kMotifSteps = 16;

// Highest reachable scale-degree index. Upstream's bounds check allows 25 but
// its `major`/`minor` tables hold only 25 entries (indices 0..24), so the top
// of its own range read one element past the end. Computing the scale instead
// of tabulating it makes index 25 well defined (43 semitones, which the
// semitone clamp below pins back to 41), so the range stays exactly as
// upstream intended and the out-of-bounds read simply cannot happen.
constexpr int8_t kMotifNoteMax = 25;

// Semitone span of the generator: degree 24 of either scale is 41 semitones
// above the root, so a motif covers just under three and a half octaves —
// the same range upstream's calibration tables spanned on GRAINS.
constexpr int8_t kMotifSemitoneMax = 41;

// The engine addresses its noise table at `pattern * 16 + randomSel + position`,
// which for pattern 0..15, randomSel 0..31 and position 1..15 tops out at 286.
// Upstream shipped 8192 bytes; only these first 287 are ever read.
constexpr uint16_t kMotifNoiseSize = 287;

// Scale degrees within one octave. Upstream stored both scales as flat 25-entry
// tables spanning three and a half octaves; they are exactly these seven
// degrees repeated, so we keep 14 bytes instead of 50 and rebuild the rest.
// Verified degree-for-degree against both original tables, and unlike them this
// is defined for index 25 — see kMotifNoteMax.
inline int8_t motifScaleDegree(bool major, uint8_t index) {
  static const int8_t kMajor[7] = {0, 2, 4, 5, 7, 9, 11};
  static const int8_t kMinor[7] = {0, 2, 3, 5, 7, 8, 10};
  const uint8_t octave = index / 7u;
  const uint8_t degree = index % 7u;
  return (int8_t)(12 * octave + (major ? kMajor[degree] : kMinor[degree]));
}

// One point in Motif's three-dimensional melody space, as the engine wants it.
struct MotifParams {
  uint8_t pattern;    // 0..15 — bit b is the direction of 4-step group b (1 = up)
  uint8_t variance;   // 0..15 — how hard the noise pulls the melody off `pattern`
  uint8_t randomSel;  // 0..31 — which noise phrase to overlay
  bool major;         // false = minor, true = major
};

// Turn three normalised 0..1 panel controls into engine units.
//
// The pot ranges are taken through the original's 10-bit ADC arithmetic rather
// than scaled directly, so the knob boundaries land in exactly the same places
// on hardware and in Rack: PATTERN is `adc >> 5` (32 positions, the upper 16
// being major), VARIANCE is `adc >> 6` (16), RANDOM is `adc >> 5` (32).
inline MotifParams motifMapParams(float variance01, float random01, float pattern01) {
  const uint16_t varAdc = (uint16_t)(clampf(variance01, 0.0f, 1.0f) * 1023.0f + 0.5f);
  const uint16_t ranAdc = (uint16_t)(clampf(random01, 0.0f, 1.0f) * 1023.0f + 0.5f);
  const uint16_t patAdc = (uint16_t)(clampf(pattern01, 0.0f, 1.0f) * 1023.0f + 0.5f);

  const uint8_t patScale = (uint8_t)(patAdc >> 5);  // 0..31

  MotifParams p;
  // Upstream inverts the low nibble so the knob sweeps from all-down to all-up.
  p.pattern = (uint8_t)(15 - (patScale & 0x0F));
  p.major = (patScale >= 16);
  p.variance = (uint8_t)(varAdc >> 6);   // 0..15
  p.randomSel = (uint8_t)(ranAdc >> 5);  // 0..31
  return p;
}

// A semitone offset in 1V/oct volts. Both platforms scale from here so the
// pitch axis is defined once.
inline float motifSemitoneToVolts(int8_t semitone) {
  return (float)semitone * (1.0f / 12.0f);
}

struct MotifEngine {
  uint8_t position;  // 0..15, step within the motif
  int8_t note;       // scale-degree index, 0..kMotifNoteMax
  int8_t semitone;   // last emitted pitch, 0..kMotifSemitoneMax

  // Upstream's melody deviation is not random at all: it is a fixed byte table
  // so that a given (pattern, random, position) always yields the same melody,
  // which is what makes Motif a *space* you can navigate rather than a dice
  // roll.  The table's own header documents the generator that produced it — a
  // 64-bit xorshift seeded with 1234 — so we regenerate it at reset() instead
  // of carrying 8 KB of PROGMEM on a 30 KB part.  Checked against all 8192
  // published bytes: 8191 match, and the single mismatch is byte 0 (0xF1 vs
  // 0xF2, a one-bit transcription slip upstream) which the index arithmetic can
  // never reach.  Every byte Motif actually plays is identical.
  uint8_t noise[kMotifNoiseSize];

  MotifEngine() { reset(); }

  void reset() {
    position = 0;
    note = 14;
    semitone = 0;
    fillNoise();
  }

  // Panel RESET: return to step 0 without disturbing the noise table. The
  // starting note is re-derived from the pattern on the next clock, so a reset
  // only takes effect when the sequence is clocked again — as upstream.
  void resetPosition() { position = 0; }

  // Advance one clock and return the new pitch as a semitone offset 0..41.
  int8_t step(const MotifParams& p) {
    if (position == 0) {
      // Choose a starting note that leaves room for where the pattern is
      // heading, so a mostly-rising motif starts low and a falling one starts
      // high. The literal pattern values are upstream's.
      if (p.pattern == 7 || p.pattern == 11 || p.pattern == 13 ||
          p.pattern == 14 || p.pattern == 15) {
        note = 0;  // majority up — start at the bottom
      } else if (p.pattern == 8 || p.pattern == 4 || p.pattern == 2 ||
                 p.pattern == 1 || p.pattern == 0) {
        note = 21;  // majority down — start near the top
      } else if ((p.pattern & 3) == 0) {
        note = 14;  // the first two groups fall — start high-ish
      } else {
        note = 7;
      }
    } else {
      // The deviation for this step. Index is stable in (pattern, random,
      // position), which is what makes the melody repeatable.
      const uint8_t rnd = noise[(uint16_t)p.pattern * 16u + p.randomSel + position];

      // Scale the deviation by VARIANCE, then tame it: the two shifts land it
      // in -9..+8, and nudging the negatives up gives a near-symmetric -8..+8
      // with two ways to draw a zero. Integer arithmetic throughout, exactly
      // as the original — the shifts are what give the deviation its coarse,
      // steppy character.
      int16_t jump = (int16_t)(((int16_t)rnd - 128) * (int16_t)p.variance) >> 4;
      jump = (int16_t)((jump * 9) >> 7);
      if (jump < 0) jump++;

      // The pattern contributes one semitone-step of direction per 4-step
      // group; the deviation is added on top.
      int16_t delta = ((p.pattern >> (position >> 2)) & 1) ? 1 : -1;
      delta = (int16_t)(delta + jump);
      // A deviation that exactly cancels the pattern would stall the melody,
      // so apply it twice instead of standing still.
      if (delta == 0) delta = (int16_t)(delta + jump);

      // Reflect off the ends of the scale rather than clamping, which keeps the
      // motion going instead of parking on the top or bottom note.
      if (delta + note < 0 || delta + note > kMotifNoteMax) delta = (int16_t)(-delta);
      if (delta + note >= 0 && delta + note <= kMotifNoteMax)
        note = (int8_t)(note + delta);
    }

    int16_t val = motifScaleDegree(p.major, (uint8_t)note);
    if (val > kMotifSemitoneMax) val = kMotifSemitoneMax;
    if (val < 0) val = 0;
    semitone = (int8_t)val;

    if (++position >= kMotifSteps) position = 0;
    return semitone;
  }

 private:
  void fillNoise() {
    // xorshift64 — the generator upstream's table was printed from.
    uint64_t s = 1234u;
    for (uint16_t i = 0; i < kMotifNoiseSize; ++i) {
      s ^= s << 21;
      s ^= s >> 35;
      s ^= s << 4;
      noise[i] = (uint8_t)(s & 0xFFu);
    }
  }
};

}  // namespace sc
