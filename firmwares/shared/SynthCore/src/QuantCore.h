#pragma once

// Quant — a note quantizer: a pitch CV in, snapped down to the nearest note of
// one of 30 scales and chords, back out as 1 V/oct.
//
// Used by:
//   - firmwares/mod1-quant/mod1-quant.ino   (HAGIWO MOD1, ATmega328P)
//   - rack-plugins/src/mod1-quant.cpp       (VCV Rack)
//
// Pure C++: includes only sc_math.h. NO Arduino.h, rack.hpp, Pico SDK.
// float only, no heap, no STL — must compile on AVR, RP2350 and desktop.
//
// This engine works in ADC counts (0..1023 = 0..5 V), not volts, which is the
// one place it breaks the repo's usual "normalise to 0..1" habit. Upstream's
// input tracker is an integer median-and-IIR filter whose settling behaviour is
// a product of its own truncation, so keeping the integer domain means firmware
// and Rack settle on the same note from the same input rather than merely close
// to it. Rack converts volts to counts on the way in.
//
// The clock is the exception: upstream ran its control loop at Mozzi's
// CONTROL_RATE of 256 Hz, so the engine accumulates the caller's dt and ticks
// the tracker at exactly 256 Hz on both platforms. Slew and settling therefore
// match at any sample rate, per the repo's dt-driven core convention.
//
// License:
// Derived from the GRAINS `quant` firmware (github.com/eclab/grains) under
// the Apache License 2.0, Copyright 2023 Sean Luke. The upstream notice
// lives at firmwares/mod1-quant/LICENSE.md and Apache 2.0 requires it to ship
// with any copy of this header — the CC0 cores next to it have no such
// condition, so don't fold this one into them.

#include "sc_math.h"

namespace sc {

// Upstream's Mozzi CONTROL_RATE. The tracker's slew is defined in ticks, so
// this rate is part of the sound and is held fixed across platforms.
constexpr float kQuantControlRate = 256.0f;

// Tracker tuning, verbatim from upstream (LARGE_JUMP / FREQ_COUNTER_MAX).
constexpr uint16_t kQuantLargeJump = 32;
constexpr uint8_t kQuantJumpTicks = 4;

// 30 scales in 3 banks of 10, and the note range the tracker can reach:
// (1023 * 60) >> 10 == 59, i.e. just under five octaves.
constexpr uint8_t kQuantScaleCount = 30;
constexpr uint8_t kQuantScalesPerBank = 10;
constexpr uint8_t kQuantBankCount = 3;
constexpr uint8_t kQuantMaxNote = 59;

// Bit n set = semitone n belongs to the scale (bit 0 = the root).
//
// Transcribed from upstream's scales[30][12] table of bytes: same 30 rows in
// the same order, 60 bytes of masks instead of 360 bytes of one-per-note, which
// matters on a chip with 2 KB of RAM. Note that upstream's header comment lists
// "Augmented Triad" and "Minor-Major 7" in the third bank, but both rows are
// commented out in the table itself and Major 6 / Minor 6 stand in their place.
// The table is the ground truth, so these are the table's contents.
inline uint16_t quantScaleMask(uint8_t scale) {
  static const uint16_t kMasks[kQuantScaleCount] = {
    // Bank 1 — chromatic and the 7-tone modes
    0xAB5,  //  0 Major
    0x9AD,  //  1 Harmonic Minor
    0xAAD,  //  2 Melodic Minor
    0x6AD,  //  3 Dorian
    0x5AB,  //  4 Phrygian
    0xAD5,  //  5 Lydian
    0x6B5,  //  6 Mixolydian
    0x5AD,  //  7 Aeolian (Relative Minor)
    0x56B,  //  8 Locrian
    0xFFF,  //  9 Chromatic
    // Bank 2 — everything else
    0x4E9,  // 10 Blues Minor
    0x295,  // 11 Pentatonic
    0x4A9,  // 12 Minor Pentatonic
    0x1A3,  // 13 Japanese Pentatonic
    0x555,  // 14 Whole Tone
    0x5CD,  // 15 Hungarian Gypsy
    0x5B9,  // 16 Phrygian Dominant
    0x973,  // 17 Persian
    0xB6D,  // 18 Diminished (Octatonic)
    0x999,  // 19 Augmented (Hexatonic)
    // Bank 3 — chords
    0x001,  // 20 Octave
    0x081,  // 21 5th + Octave
    0x091,  // 22 Major Triad
    0x089,  // 23 Minor Triad
    0x291,  // 24 Major 6
    0x289,  // 25 Minor 6
    0x491,  // 26 7
    0x891,  // 27 Major 7
    0x489,  // 28 Minor 7
    0x249,  // 29 Diminished 7
  };
  return kMasks[scale < kQuantScaleCount ? scale : (uint8_t)(kQuantScaleCount - 1)];
}

// Bank pot + scale pot -> scale index 0..29.
//
// The bank split uses sc::select3, whose boundaries (340/1023, 681/1023) are
// the repo's shared three-way convention; upstream's (adc * 3) >> 10 splits one
// or two ADC counts later. Both platforms use this function, so the ~0.1% knob
// shift is versus upstream only, never between firmware and Rack.
inline uint8_t quantSelectScale(float bank01, float scale01) {
  const uint8_t bank = select3(bank01);
  uint8_t index = (uint8_t)(clampf(scale01, 0.0f, 1.0f) * (float)kQuantScalesPerBank);
  if (index >= kQuantScalesPerBank) index = kQuantScalesPerBank - 1;
  return (uint8_t)(bank * kQuantScalesPerBank + index);
}

// Median of three, upstream's MEDIAN_OF_THREE macro as a function.
inline uint16_t quantMedian3(uint16_t a, uint16_t b, uint16_t c) {
  if (a <= b) return (b <= c) ? b : ((a < c) ? c : a);
  return (a <= c) ? a : ((b < c) ? c : b);
}

// Snap a note down to the nearest member of `scale`, keeping it in its octave.
inline uint8_t quantizeNote(uint8_t note, uint8_t scale) {
  if (note > kQuantMaxNote) note = kQuantMaxNote;
  // Upstream unrolled this into a comparison ladder to dodge AVR's software
  // divide; at 256 Hz the divide costs nothing and this reads as what it is.
  const uint8_t octave = (uint8_t)((note / 12) * 12);
  uint8_t degree = (uint8_t)(note - octave);
  const uint16_t mask = quantScaleMask(scale);
  // Every one of the 30 masks has bit 0 set, so the walk always terminates.
  while (!(mask & (uint16_t)(1u << degree))) --degree;
  return (uint8_t)(octave + degree);
}

// A quantized note as a 1 V/oct control voltage, note 0 = 0 V.
inline float quantNoteVolts(uint8_t note) {
  return (float)note * (1.0f / 12.0f);
}

struct QuantEngine {
  // Upstream getPitch()'s state: the running filtered value, the two previous
  // raw reads that feed the median, and the countdown that holds the filter
  // open for a few ticks after a jump.
  uint16_t pitchCv;
  uint16_t pA;
  uint16_t pB;
  uint8_t freqCounter;

  uint8_t note;  // the note currently on the output, 0..59
  float accum;   // seconds owed to the 256 Hz control clock

  void reset() {
    pitchCv = 0;
    pA = 0;
    pB = 0;
    freqCounter = 0;
    note = 0;
    accum = 0.0f;
  }

  // Seed the tracker from the first reading so it starts settled. Upstream
  // seeded only pitchCV and left the median's history at zero, which drags the
  // first two ticks down towards note 0; priming all three costs nothing and
  // removes a startup glide that was never a musical choice.
  void initPitch(uint16_t adc) {
    pitchCv = adc;
    pA = adc;
    pB = adc;
    freqCounter = 0;
  }

  // One control tick of upstream's getPitch(): a median-of-three feeding a 7/8
  // IIR, both bypassed for four ticks after any jump of 32 counts or more so
  // that real interval leaps land immediately instead of gliding into place.
  // Returns the unquantized note, 0..59.
  uint8_t trackPitch(uint16_t p) {
    const uint16_t diff = (p > pitchCv) ? (uint16_t)(p - pitchCv) : (uint16_t)(pitchCv - p);
    if (diff >= kQuantLargeJump) {
      pitchCv = p;  // jump right there
      freqCounter = kQuantJumpTicks;
    } else if (freqCounter > 0) {
      --freqCounter;
      pitchCv = (uint16_t)((pitchCv + p) >> 1);
      pB = pA;
      pA = pitchCv;
    } else {
      const uint16_t p1 = quantMedian3(p, pA, pB);
      pB = pA;
      pA = p;
      pitchCv = (uint16_t)(((uint32_t)pitchCv * 7u + p1) >> 3);
    }
    uint8_t n = (uint8_t)(((uint32_t)pitchCv * 60u) >> 10);
    if (n > kQuantMaxNote) n = kQuantMaxNote;
    return n;
  }

  // Advance by dt seconds. `adc` is the summed pitch input in ADC counts
  // (0..1023 = 0..5 V), `scale` is 0..29. The output follows the input
  // continuously, as upstream's does. Returns true on the tick where the output
  // note changed.
  bool step(float dt, uint16_t adc, uint8_t scale) {
    const float period = 1.0f / kQuantControlRate;
    accum += dt;

    bool changed = false;
    // Cap the catch-up: a caller that stalled must not spin here for thousands
    // of ticks, and a quantizer has nothing to gain from replaying stale input.
    for (uint8_t i = 0; i < kQuantJumpTicks && accum >= period; ++i) {
      accum -= period;
      const uint8_t q = quantizeNote(trackPitch(adc), scale);
      if (q != note) {
        note = q;
        changed = true;
      }
    }
    if (accum > period * (float)kQuantJumpTicks) accum = 0.0f;  // resync after a stall
    return changed;
  }

  // The output note as 1 V/oct.
  float volts() const { return quantNoteVolts(note); }
};

}  // namespace sc
