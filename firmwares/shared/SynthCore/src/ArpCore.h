#pragma once

// Arp — arpeggiator engine shared by the mod1-arp firmware and the Rack port.
//
// Give the engine a CHORD (one of 16 semitone masks), a STYLE (one of 12
// traversal patterns) and an INVERSION; every clock tick it hands back the
// next note of the arpeggio as a semitone offset. The caller adds its own root
// pitch to that offset, so the whole figure transposes with the pitch CV.
//
// There is no per-sample audio here: this is a clocked utility like ClkCore.h,
// so the interface is step()-per-tick rather than process(dt).
//
// Used by:
//   - firmwares/mod1-arp/mod1-arp.ino    (clock jack edge -> step())
//   - rack-plugins/src/mod1-arp.cpp      (clock from dsp::SchmittTrigger)
//
// Pure C++: depends only on sc_math.h. No Arduino / Rack / Pico SDK.
//
// sc_dsp.h would be the natural home for the PRNG, but it cannot be included
// from a MOD1 sketch: its Biquad names coefficients B0..B2 / A1..A2, and on AVR
// those are already macros (Arduino's binary.h and the analog-pin defines), so
// the header fails to compile behind Arduino.h. The xorshift32 steps are
// therefore written out inline in ArpEngine::nextRandom() below — the same
// three shifts as sc::xorshift32, so the streams still match. Deliberately a
// member rather than another free function of that name: RandomCvCore.h,
// RandomLagCore.h and sc_dsp.h each define one already, and a fourth would
// collide the moment a translation unit pulled in two of these cores.
//
// License:
// Derived from the GRAINS `arp` firmware (github.com/eclab/grains) under
// the Apache License 2.0, Copyright 2025 Sean Luke. The upstream notice
// lives at firmwares/mod1-arp/LICENSE.md and Apache 2.0 requires it to ship
// with any copy of this header — the CC0 cores next to it have no such
// condition, so don't fold this one into them.

#include "sc_math.h"

namespace sc {

// ── Chords ────────────────────────────────────────────────────────────────
// Upstream stored these as uint8_t chords[16][12], one byte per semitone —
// 192 bytes of an ATmega328P's 2 KB of RAM. The same information is one bit
// per semitone, so each chord is a 12-bit mask and the table costs 32 bytes.
// Bit j set means semitone j (0 = root) belongs to the chord. Order and
// contents are upstream's, unchanged, so POT2 selects the same chords.
static const uint16_t kArpChords[16] = {
    // MAJOR
    0x095,  // 0, 2, 4, 7          Maj add2                      4 notes
    0x295,  // 0, 2, 4, 7, 9       Major pentatonic               5 notes
    0x895,  // 0, 2, 4, 7, 11      Maj7 add2                      5 notes
    0x891,  // 0, 4, 7, 11         Maj7                           4 notes
    0x491,  // 0, 4, 7, 10         Dominant 7                     4 notes
    0x091,  // 0, 4, 7             Major                          3 notes
    // MINOR
    0x081,  // 0, 7                Root and 5th                   2 notes
    0x089,  // 0, 3, 7             Minor                          3 notes
    0x489,  // 0, 3, 7, 10         m7                             4 notes
    0x48D,  // 0, 2, 3, 7, 10      m7 add2                        5 notes
    0x68D,  // 0, 2, 3, 7, 9, 10   m7 add2 add6                   6 notes
    0x28D,  // 0, 2, 3, 7, 9       Minor pentatonic               5 notes
    0x08D,  // 0, 2, 3, 7          Minor add2                     4 notes
    // OTHER
    0x249,  // 0, 3, 6, 9          dim7                           4 notes
    0xAB5,  // 0,2,4,5,7,9,11      Major scale   [good for random] 7 notes
    0x5AD,  // 0,2,3,5,7,8,10      Minor scale   [good for random] 7 notes
};

// How many notes each mask contains. Kept as its own table (upstream did the
// same) because the traversal lengths are derived from it on every rebuild and
// popcounting a mask on AVR is not free.
static const uint8_t kArpChordNotes[16] = {4, 5, 5, 4, 4, 3, 2, 3,
                                           4, 5, 6, 5, 4, 4, 7, 7};

constexpr uint8_t kArpChordCount = 16;

// ── Styles ────────────────────────────────────────────────────────────────
// Six patterns over one octave of the chord, then the same six over two.
// Indices are upstream's #define values, in POT3 order.
enum {
  kArpUp = 0,
  kArpDown,
  kArpUpDown,
  kArpUpDownPlus,  // plays the root at the top of the run as well as the bottom
  kArpRandom,
  kArpRandomWalk,
  kArpUp2,
  kArpDown2,
  kArpUpDown2,
  kArpUpDownPlus2,
  kArpRandom2,
  kArpRandomWalk2,
  kArpStyleCount
};

// Longest run any style can ask for: the 7-note chords, two octaves, plus the
// repeated top root, plus upstream's slack. Sized as upstream's
// MAX_CHORD_MAP_SIZE (7 * 2 + 3).
constexpr uint8_t kArpMapSize = 7 * 2 + 3;

// Semitone span of the pitch output. MOD1's PWM CV out covers 0–5 V, so five
// octaves is exactly 1 V/oct with the top note at 4.917 V. Notes above this
// fold down by octaves, which is what upstream did against its 42/47-entry
// calibration table.
constexpr uint8_t kArpMaxSemitone = 59;

// Root-pitch range, 0..35 semitones (three octaves) — upstream's (adc * 36) >> 10.
constexpr uint8_t kArpRootRange = 36;

// Seconds between the clock edge and the gate going high. Upstream waited three
// of Mozzi's 256 Hz control ticks (~11.7 ms) because that is how long its
// control loop took to notice the new note. Here the pitch PWM is written in
// the same breath as the clock edge, so the delay only has to let the output
// filter slew before an envelope reads the note.
constexpr float kArpGateDelaySec = 0.002f;

// ── Panel mapping ─────────────────────────────────────────────────────────
// Each takes a normalised 0..1 control (adc / 1023 on the firmware, the raw
// knob value in Rack) and reproduces upstream's integer selection exactly.

// POT2 -> chord, upstream's (adc >> 6).
inline uint8_t arpSelectChord(float v01) {
  const int c = (int)(clampf(v01, 0.0f, 1.0f) * (float)kArpChordCount);
  return (uint8_t)(c >= kArpChordCount ? kArpChordCount - 1 : c);
}

// POT3 -> style, upstream's ((adc * 12) >> 10).
inline uint8_t arpSelectStyle(float v01) {
  const int s = (int)(clampf(v01, 0.0f, 1.0f) * (float)kArpStyleCount);
  return (uint8_t)(s >= kArpStyleCount ? kArpStyleCount - 1 : s);
}

// POT1 (+ pitch CV) -> root semitone, upstream's ((adc * 36) >> 10).
inline uint8_t arpRootSemitone(float v01) {
  const int p = (int)(clampf(v01, 0.0f, 1.0f) * (float)kArpRootRange);
  return (uint8_t)(p >= kArpRootRange ? kArpRootRange - 1 : p);
}

// ── The engine ────────────────────────────────────────────────────────────
struct ArpEngine {
  int8_t map[kArpMapSize];  // the run of semitone offsets, -1 past the end
  uint8_t mapLen;           // entries actually filled
  int8_t idx;               // cursor into map; -1 means "not started"
  uint8_t direction;        // 0 = ascending, 1 = descending (up-down styles)
  uint8_t chord;            // latched chord index, 0..15
  uint8_t style;            // latched style index, 0..11
  uint8_t inversion;        // latched inversion, already clamped to the chord
  int8_t note;              // semitone offset emitted by the last step()
  uint32_t rng;             // PRNG state for the random styles

  ArpEngine() {
    chord = 0;
    style = 0;
    inversion = 0;
    rng = 0x1BADB002u;
    reset();
  }

  // Return to the start of the run and rebuild it. Upstream's doReset().
  void reset() {
    _buildMap();
    idx = -1;
    direction = 0;
    note = 0;
  }

  // Seed the random styles. Non-zero states only, as xorshift32 requires.
  void seed(uint32_t s) { rng = (s == 0u) ? 1u : s; }

  // Next raw draw. xorshift32 (Marsaglia 2003) written out inline — see the
  // include note at the top. Drawn the same way on every platform, so a given
  // seed produces the same arpeggio in the firmware and in Rack.
  uint32_t nextRandom() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
  }

  // Advance one clock tick and return the new note as a semitone offset.
  // The three selections are passed on every tick, as upstream re-read its
  // pots inside go(): changing the chord, style or inversion restarts the run.
  int8_t step(uint8_t chordSel, uint8_t styleSel, uint8_t invSel) {
    _latch(chordSel, styleSel, invSel);

    const int8_t top1 = (int8_t)(kArpChordNotes[chord] - 1);
    const int8_t top2 = (int8_t)(kArpChordNotes[chord] * 2 - 1);

    switch (style) {
      case kArpUp:
      case kArpUp2:
        if (idx < 0 || _at(idx) < 0) idx = 0;
        note = _at(idx);
        idx++;
        break;

      case kArpDown:
        if (idx < 0 || _at(idx) < 0) idx = top1;
        note = _at(idx);
        idx--;
        break;

      case kArpDown2:
        if (idx < 0 || _at(idx) < 0) idx = top2;
        note = _at(idx);
        idx--;
        break;

      // The "+" variants share the traversal and differ only in how long
      // _buildMap() made the run — the extra top entry is the repeated root.
      // Turning around at idx -= 2 re-reads the note below the top, so the
      // apex is never played twice in the plain up-down styles.
      case kArpUpDown:
      case kArpUpDownPlus:
        _upDown(top1);
        break;

      case kArpUpDown2:
      case kArpUpDownPlus2:
        _upDown(top2);
        break;

      case kArpRandom:
      case kArpRandom2:
        if (mapLen <= 1) {
          idx = 0;
        } else if (idx < 0 || (uint8_t)idx >= mapLen) {
          idx = (int8_t)(nextRandom() % mapLen);
        } else {
          // Upstream rejection-sampled random(mapLen) until it differed from
          // the current index. Drawing from mapLen - 1 and stepping over the
          // current index is the same uniform distribution over the same
          // choices, without an unbounded retry loop on an 8-bit MCU.
          uint8_t v = (uint8_t)(nextRandom() % (uint32_t)(mapLen - 1));
          if (v >= (uint8_t)idx) v++;
          idx = (int8_t)v;
        }
        note = _at(idx);
        break;

      case kArpRandomWalk:
      case kArpRandomWalk2:
        if (mapLen <= 1) {
          idx = 0;
        } else {
          // Upstream flipped a coin and retried whenever the step would leave
          // the run, which reflects off both ends; deciding at the ends
          // directly is the same walk with no retry loop. The walk never
          // stands still, so a note always changes.
          bool up;
          if (idx <= 0)
            up = true;
          else if ((uint8_t)(idx + 1) >= mapLen)
            up = false;
          else
            up = (nextRandom() & 1u) != 0u;
          idx = (int8_t)(up ? idx + 1 : idx - 1);
        }
        note = _at(idx);
        break;

      default:
        break;
    }

    // _at() answers -1 for anything off the end of the run. Nothing reachable
    // should land there, but upstream would have transposed that -1 into a
    // negative table index, so pin it to the root instead.
    if (note < 0) note = 0;
    return note;
  }

  // The semitone actually sent to the pitch output: the current note plus the
  // caller's root, folded down by octaves until it fits the output span.
  uint8_t pitchSemitone(uint8_t root) const {
    int16_t p = (int16_t)note + (int16_t)root;
    while (p > (int16_t)kArpMaxSemitone) p -= 12;
    if (p < 0) p = 0;
    return (uint8_t)p;
  }

  // Normalised 0..1 pitch CV — multiply by the output's full-scale volts.
  // Linear in semitones over exactly kArpMaxSemitone + 1 of them, so scaling
  // by 5 V gives true 1 V/oct.
  float pitchCv(uint8_t root) const {
    return (float)pitchSemitone(root) / (float)(kArpMaxSemitone + 1);
  }

 private:
  // Bounds-checked read. The traversals walk the cursor past both ends of the
  // run on purpose and use the -1 they get back as the turn-around signal;
  // upstream indexed the bare array to do it, which on AVR reads whatever sits
  // next to the map.
  int8_t _at(int8_t i) const {
    if (i < 0 || (uint8_t)i >= kArpMapSize) return -1;
    return map[i];
  }

  void _upDown(int8_t topIdx) {
    if (direction == 0) {
      if (idx < 0) idx = 0;
      note = _at(idx);
      idx++;
      if (_at(idx) < 0) {
        direction = 1;
        idx -= 2;
      }
    } else {
      if (_at(idx) < 0) idx = topIdx;
      note = _at(idx);
      idx--;
      if (idx < 0) {
        direction = 0;
        idx = 1;
      }
    }
  }

  void _latch(uint8_t c, uint8_t s, uint8_t inv) {
    if (c >= kArpChordCount) c = kArpChordCount - 1;
    if (s >= kArpStyleCount) s = kArpStyleCount - 1;
    // Upstream's INVERSION was a compile-time constant carrying a "don't go
    // past the note count" warning in the header. Ours is on the button, so
    // clamp it: inverting further than the chord has notes would leave the run
    // short and upstream's builder silently kept the previous length.
    const uint8_t maxInv = (uint8_t)(kArpChordNotes[c] - 1);
    if (inv > maxInv) inv = maxInv;

    if (c != chord || s != style || inv != inversion) {
      chord = c;
      style = s;
      inversion = inv;
      reset();
    }
  }

  void _buildMap() {
    for (uint8_t i = 0; i < kArpMapSize; i++) map[i] = -1;

    const uint16_t mask = kArpChords[chord];
    const uint8_t notes = kArpChordNotes[chord];

    // How far the run goes before the traversal turns around or wraps.
    uint8_t want;
    if (style == kArpUpDownPlus)
      want = (uint8_t)(notes + 1);
    else if (style == kArpUpDownPlus2)
      want = (uint8_t)(notes * 2 + 1);
    else if (style >= kArpUp2)
      want = (uint8_t)(notes * 2);
    else
      want = notes;
    if (want > kArpMapSize) want = kArpMapSize;

    // Inversion skips the first `inversion` SEMITONE SLOTS, not the first
    // `inversion` chord notes — that is what upstream's counter did, and it is
    // why inversions 1..4 of a major triad all land on the same first
    // inversion. Preserved deliberately; the pot/button feel follows from it.
    uint8_t skip = inversion;
    uint8_t pos = 0;
    for (uint8_t oct = 0; oct < 3 && pos < want; oct++) {
      for (uint8_t j = 0; j < 12; j++) {
        if (skip > 0) {
          skip--;
        } else if (mask & (uint16_t)(1u << j)) {
          map[pos++] = (int8_t)(oct * 12 + j);
          if (pos >= want) break;
        }
      }
    }
    mapLen = pos;
  }
};

// ── Pitch-CV conditioning (firmware only) ─────────────────────────────────
// Upstream's getPitchCV() de-glitcher, kept intact: a large move jumps
// straight through, small ones settle through a median-of-three feeding a 7/8
// exponential average. Integer maths on the raw 0..1023 reading, as the
// original. This conditions a noisy ATmega ADC — Rack's CV is already clean,
// so the Rack module reads the root pitch directly and skips this.
constexpr uint16_t kArpPitchJump = 32;   // upstream LARGE_JUMP
constexpr uint8_t kArpPitchSettle = 4;   // upstream FREQ_COUNTER_MAX

struct ArpPitchFilter {
  uint16_t value;  // filtered reading, 0..1023
  uint16_t pA;     // previous raw reading
  uint16_t pB;     // the one before that
  uint8_t settle;  // fast-settle ticks still owed after a large jump

  ArpPitchFilter() { reset(0); }

  void reset(uint16_t adc) {
    value = adc;
    pA = adc;
    pB = adc;
    settle = 0;
  }

  uint16_t update(uint16_t adc) {
    const uint16_t diff = (adc > value) ? (uint16_t)(adc - value)
                                        : (uint16_t)(value - adc);
    if (diff >= kArpPitchJump) {
      value = adc;  // deliberate move: track it immediately
      settle = kArpPitchSettle;
    } else if (settle > 0) {
      settle--;
      value = (uint16_t)((value + adc) >> 1);
      pB = pA;
      pA = value;
    } else {
      const uint16_t med = _median3(adc, pA, pB);
      pB = pA;
      pA = adc;
      value = (uint16_t)((value * 7u + med) >> 3);
    }
    return value;
  }

 private:
  static uint16_t _median3(uint16_t a, uint16_t b, uint16_t c) {
    if (a <= b) return (b <= c) ? b : ((a < c) ? c : a);
    return (a <= c) ? a : ((b < c) ? c : b);
  }
};

}  // namespace sc
