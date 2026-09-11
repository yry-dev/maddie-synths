#pragma once

// Droplets voice — a tinkling chord of sine "droplets", like a wind chime.
//
// Used by:
//   - firmwares/mod2-droplets/mod2-droplets.ino  (one sample per PWM-wrap ISR)
//   - rack-plugins/src/mod2-droplets.cpp         (one sample per process())
//
// Ported from Sean Luke's GRAINS `droplets` firmware. Every trigger drops one
// note: a sine oscillator is allocated round-robin from a pool of four, tuned to
// a semitone drawn at random from the selected chord table, faded in over ~2 ms
// and then rung down linearly. Nothing is sequenced — the whole musical result
// comes out of the chord tables and the draw, so those are reproduced verbatim.
//
// What survived the move from Mozzi's 16384 Hz integer engine to floats:
//
//   CHORDS    The 24 x 15 table below IS upstream's, note for note, including
//             its duplicated fundamentals (most chords list the root three
//             times, which is what weights the draw towards it) and its two
//             apparent typos in the 7 and dim7 rows, where an `_Eb` sits where
//             the pattern wants `_Eb1`. Those are audible details of a shipped
//             module, not bugs to tidy: they are what it sounds like.
//   RANGE     The first twelve rows span 3-4 octaves, the last twelve are the
//             same chords folded into roughly half that. One pot picks a row
//             from both halves and a release time — see dropletsRangeSelect().
//   ENVELOPE  Upstream fades a droplet in by adding 8 to an 8-bit counter every
//             audio sample (31 samples, ~1.9 ms), and only once that finishes
//             does it start decrementing a 0..127 gain by one every `releases[]`
//             samples. That is a linear attack followed by a *linear* decay, not
//             the exponential most drum voices use, and the flat ring-down is
//             half of why the module sounds like struck metal. Both are kept,
//             converted to seconds at upstream's sample rate.
//   PITCH     0 V is C0 (MIDI 24, 32.7 Hz), as upstream documents.
//
// Deliberate changes, all of them replacing an artefact of the AVR rather than
// a musical decision:
//
//   - Upstream's two PROGMEM tables are gone. `frequencies[1536]` was exactly
//     32.7 * 2^(i * 5/1023) (its last entry, 5929.368 Hz, is 32.7 * 2^7.5024),
//     and `semitoneFrequencyRatios[73]` was exactly 2^(n/12); both are computed
//     directly here. That returns ~6.4 KB of flash and removes the tables' own
//     quantisation.
//   - The droplet frequency ceiling is the caller's real Nyquist rather than
//     upstream's `NYQUIST 16384`, which was the Mozzi *sample rate* and so let
//     droplets alias. It only bites at a high root; the retry-and-reject
//     behaviour that biases the draw downwards there is unchanged.
//   - When all 16 draws are above the ceiling, upstream falls back to
//     `frequency = baseFrequency` and then multiplies by the base again, i.e. it
//     plays baseFrequency squared. That is plainly not the intent; this core
//     falls back to the root note (ratio 1).
//   - `random()` is sc::xorshift32, so the firmware and the Rack port draw the
//     same sequence from the same seed (the repo-wide rule in PORTING.md).
//
// Pure C++: depends only on sc_math.h / sc_dsp.h. No Arduino / Rack / Pico SDK;
// float-only, no heap, no STL.
//
// License:
// Derived from the GRAINS `droplets` firmware (github.com/eclab/grains) under
// the Apache License 2.0, Copyright 2023 Sean Luke. The upstream notice
// lives at firmwares/mod2-droplets/LICENSE.md and Apache 2.0 requires it to ship
// with any copy of this header — the CC0 cores next to it have no such
// condition, so don't fold this one into them.

#include "sc_math.h"
#include "sc_dsp.h"

namespace sc {

// GRAINS runs Mozzi at 16384 Hz. Upstream's fade-in step and release counters
// are both measured in samples at that rate, so every conversion to seconds
// goes through this constant — it is a property of the original firmware, not
// of the host.
constexpr float kDropletRefFs = 16384.0f;

// Four sine oscillators, allocated round-robin. A fifth droplet steals the
// oldest, which is upstream's voice-stealing and part of how dense triggering
// thins itself out.
constexpr int kDropletVoices = 4;

// Twelve chords, each 15 notes deep; the table below holds them twice (full
// range, then shortened).
constexpr int kDropletChordCount = 12;
constexpr int kDropletChordNotes = 15;

// Fade-in: upstream starts an 8-bit counter at 7 and adds 8 per audio sample
// until it sticks at 255, so 31 samples at 16384 Hz. Short enough to be a
// transient, long enough to kill the click of starting a sine at full level.
constexpr float kDropletAttackSec = 31.0f / kDropletRefFs;  // ~1.89 ms

// Release: upstream decrements a 0..127 gain once every `releases[]` samples,
// so a droplet rings for 127 * releases[i] samples. The five settings are the
// five positions of the release pot within a range half.
constexpr float kDropletReleaseSec[5] = {
    127.0f * 16.0f / kDropletRefFs,   // ~0.124 s — a tick
    127.0f * 32.0f / kDropletRefFs,   // ~0.248 s
    127.0f * 64.0f / kDropletRefFs,   // ~0.496 s
    127.0f * 128.0f / kDropletRefFs,  // ~0.992 s
    127.0f * 255.0f / kDropletRefFs,  // ~1.977 s — a long chime
};

// 0 V plays C0, three octaves below middle C (MIDI 24). Upstream's tuning.
constexpr float kDropletRootC0Hz = 32.7f;

// Upstream clamps its pitch index to 1535, the end of the frequency table, which
// is 1535 * 5/1023 octaves above C0. Kept so the root has the same ceiling.
constexpr float kDropletMaxOct = 1535.0f * 5.0f / 1023.0f;  // ~7.50 octaves

// How long the panel button must be held to count as a long press. Shared so the
// firmware and the Rack port agree on the gesture, not because it is DSP.
constexpr float kDropletLongPressSec = 0.5f;

// The chord tables, transcribed from upstream's `chords[NUM_CHORDS][NUM_NOTES]`.
// Values are semitones above the root. Rows 0-11 are the full-range voicings
// (3-4 octaves), rows 12-23 the same chords shortened to about half that; the
// range pot picks which half. Upstream's header lists the chords in a different
// order than the table actually stores them — the table is what plays, and this
// is the table.
constexpr uint8_t kDropletChords[2 * kDropletChordCount][kDropletChordNotes] = {
    // ── Full range ──────────────────────────────────────────────────────────
    {0, 0, 0, 4, 7, 12, 16, 19, 24, 28, 31, 36, 40, 43, 48},  // Maj
    {0, 0, 0, 3, 7, 12, 15, 19, 24, 27, 31, 36, 39, 43, 48},  // min
    {0, 0, 0, 4, 7, 11, 12, 16, 19, 23, 24, 28, 31, 35, 36},  // Maj7
    {0, 0, 0, 3, 7, 10, 12, 15, 19, 22, 24, 27, 31, 34, 36},  // min7
    {0, 0, 0, 4, 7, 10, 12, 3, 19, 22, 24, 3, 31, 34, 36},    // 7   (Eb, not Eb1)
    {0, 0, 0, 3, 6, 9, 12, 3, 18, 21, 24, 3, 30, 9, 36},      // dim7 (same)
    {0, 0, 0, 4, 8, 12, 4, 20, 24, 4, 32, 36, 4, 32, 48},     // Aug7
    {0, 2, 4, 7, 9, 12, 14, 16, 19, 21, 24, 26, 28, 31, 33},  // Pentatonic
    {0, 0, 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24},    // Whole tone
    {0, 0, 0, 3, 5, 6, 7, 10, 12, 15, 17, 18, 19, 22, 24},    // Blues
    {0, 0, 7, 12, 19, 24, 31, 36, 43, 48, 12, 19, 24, 31, 36},  // 5 + Oct
    {0, 0, 0, 12, 24, 36, 48, 12, 24, 36, 48, 12, 24, 36, 48},  // Oct
    // ── Shortened ───────────────────────────────────────────────────────────
    {0, 0, 0, 4, 7, 12, 16, 19, 24, 4, 7, 12, 16, 19, 24},    // Maj
    {0, 0, 0, 3, 7, 12, 15, 19, 24, 3, 7, 12, 15, 19, 24},    // min
    {0, 0, 0, 4, 7, 11, 12, 4, 7, 11, 12, 16, 19, 23, 24},    // Maj7
    {0, 0, 0, 3, 7, 10, 12, 3, 7, 10, 12, 15, 19, 22, 24},    // min7
    {0, 0, 0, 4, 7, 10, 12, 4, 7, 10, 12, 3, 19, 22, 24},     // 7   (Eb, not Eb1)
    {0, 0, 0, 3, 6, 9, 12, 3, 6, 9, 12, 3, 18, 21, 24},       // dim7
    {0, 0, 0, 4, 8, 12, 4, 20, 24, 4, 8, 12, 4, 20, 24},      // Aug7
    {0, 2, 4, 7, 9, 0, 2, 4, 7, 9, 12, 14, 16, 19, 21},       // Pentatonic
    {0, 0, 0, 2, 4, 6, 8, 10, 12, 2, 4, 6, 8, 10, 12},        // Whole tone
    {0, 0, 0, 3, 5, 6, 7, 10, 12, 3, 5, 6, 7, 10, 12},        // Blues
    {0, 0, 0, 7, 12, 19, 24, 31, 36, 7, 12, 19, 24, 31, 36},  // 5 + Oct
    {0, 0, 0, 12, 24, 12, 24, 12, 24, 12, 24, 12, 24, 12, 24},  // Oct
};

// Name of chord `c` (0..kDropletChordCount-1), for panel labels and tooltips.
// Lives here so the chord list has exactly one definition.
inline const char* dropletsChordName(uint8_t c) {
  switch (c) {
    case 0: return "Maj";
    case 1: return "min";
    case 2: return "Maj7";
    case 3: return "min7";
    case 4: return "7";
    case 5: return "dim7";
    case 6: return "Aug7";
    case 7: return "Pentatonic";
    case 8: return "Whole tone";
    case 9: return "Blues";
    case 10: return "5 + Oct";
    default: return "Oct";
  }
}

// Chord pot -> chord 0..11. Upstream: `(adc * (NUM_CHORDS/2)) >> 10`.
inline uint8_t dropletsChordSelect(float pot01) {
  int c = (int)(clampf(pot01, 0.0f, 1.0f) * (float)kDropletChordCount);
  if (c >= kDropletChordCount) c = kDropletChordCount - 1;
  return (uint8_t)c;
}

// Range/release pot -> 0..9. Upstream: `(adc * 10) >> 10`. Positions 0-4 are the
// full-range voicings from fast to slow release, positions 5-9 repeat the five
// releases against the shortened voicings — one knob, two things, exactly as the
// original's single "Release and Range" pot.
inline uint8_t dropletsRangeSelect(float pot01) {
  int r = (int)(clampf(pot01, 0.0f, 1.0f) * 10.0f);
  if (r > 9) r = 9;
  return (uint8_t)r;
}

// Root frequency for a pitch expressed in octaves above C0, clamped to the span
// upstream's frequency table covered.
inline float dropletsRootFreq(float octavesAboveC0) {
  return kDropletRootC0Hz * powf(2.0f, clampf(octavesAboveC0, 0.0f, kDropletMaxOct));
}

// One rendered sample: audio in -1..+1 and the loudest active droplet's envelope
// in 0..1 (both platforms use the latter for LED brightness).
struct DropletsFrame {
  float audio;
  float env;
};

struct DropletsVoice {
  // One sine droplet. `relRate` is latched per droplet so a droplet already
  // ringing keeps the release it was struck with when the pot moves — upstream
  // copies `releases[release]` into a per-oscillator `max[]` for the same reason.
  struct Droplet {
    bool active = false;
    float phase = 0.0f;    // oscillator phase, radians
    float freq = 0.0f;     // Hz
    float attack = 0.0f;   // 0..1 fade-in progress
    float rel = 0.0f;      // 0..1 ring-down progress
    float relRate = 1.0f;  // 1 / release seconds, latched at the strike
    float level = 1.0f;    // accent attenuation, latched at the strike
  };

  Droplet drops[kDropletVoices];
  uint8_t next = 0;         // round-robin allocation cursor
  uint8_t lastPitch = 255;  // last semitone played, to avoid immediate repeats
  uint32_t rng = 0x5eed1e77u;  // xorshift32 state (any non-zero value)

  // Parameters, set via setParams().
  float rootHz = kDropletRootC0Hz;
  float maxHz = kDropletRefFs * 0.5f;  // droplet frequency ceiling (host Nyquist)
  uint8_t chordIdx = 0;                // 0..23, a row of kDropletChords
  uint8_t releaseIdx = 0;              // 0..4, an entry of kDropletReleaseSec

  void reset() {
    for (int i = 0; i < kDropletVoices; i++) drops[i] = Droplet();
    next = 0;
    lastPitch = 255;
  }

  // Latch the two panel pots (normalised 0..1) and the root frequency. The chord
  // and range pots interact the way upstream's do: the upper half of the release
  // pot pushes the chord selection into the shortened table.
  void setParams(float chordPot01, float rangePot01, float rootHz_) {
    rootHz = rootHz_;
    uint8_t sel = dropletsRangeSelect(rangePot01);
    uint8_t chord = dropletsChordSelect(chordPot01);
    if (sel > 4) {
      sel -= 5;
      chord += kDropletChordCount;  // same chord, shortened voicing
    }
    releaseIdx = sel;
    chordIdx = chord;
  }

  // Drop one note. `level` is the accent attenuation (1 normal, 0.5 accented),
  // held for the whole droplet.
  void strike(float level = 1.0f) {
    const uint8_t* table = kDropletChords[chordIdx];

    // Upstream draws up to 16 times, rejecting any note that would put the
    // droplet above the frequency ceiling, and inside each draw keeps rerolling
    // until it gets something other than the note it just played. The reroll is
    // capped here: a chord row whose every entry equalled lastPitch would spin
    // forever, and this runs in an audio ISR.
    float ratio = 1.0f;
    uint8_t pitch = lastPitch;
    for (int attempt = 0; attempt < 16; attempt++) {
      for (int guard = 0; guard < 32; guard++) {
        pitch = table[xorshift32(rng) % (uint32_t)kDropletChordNotes];
        if (pitch != lastPitch) break;
      }
      const float f = powf(2.0f, (float)pitch * (1.0f / 12.0f));
      if (rootHz * f < maxHz) {
        ratio = f;
        break;
      }
    }
    lastPitch = pitch;

    Droplet& d = drops[next];
    d.active = true;
    d.phase = 0.0f;
    d.freq = rootHz * ratio;
    d.attack = 0.0f;
    d.rel = 0.0f;
    d.relRate = 1.0f / kDropletReleaseSec[releaseIdx];
    d.level = level;

    next++;
    if (next >= kDropletVoices) next = 0;
  }

  // Render one sample and advance by `dt` seconds.
  DropletsFrame process(float dt) {
    float sum = 0.0f;
    float peak = 0.0f;

    for (int i = 0; i < kDropletVoices; i++) {
      Droplet& d = drops[i];
      if (!d.active) continue;

      // Fade in first, then ring down — upstream's release counter does not
      // start until the fade-in counter has saturated.
      float env = (d.attack < 1.0f) ? d.attack : (1.0f - d.rel);
      if (env < 0.0f) env = 0.0f;
      env *= d.level;

      sum += sinf(d.phase) * env;
      if (env > peak) peak = env;

      d.phase += kTwoPi * d.freq * dt;
      if (d.phase >= kTwoPi) d.phase -= kTwoPi * floorf(d.phase * (1.0f / kTwoPi));

      if (d.attack < 1.0f) {
        d.attack += dt * (1.0f / kDropletAttackSec);
        if (d.attack > 1.0f) d.attack = 1.0f;
      } else {
        d.rel += dt * d.relRate;
        if (d.rel >= 1.0f) d.active = false;
      }
    }

    DropletsFrame fr;
    // Upstream sums four oscillators and scales the sum so that four
    // simultaneous full-gain droplets in phase reach full scale; dividing by the
    // voice count is that same normalisation.
    fr.audio = sum * (1.0f / (float)kDropletVoices);
    fr.env = peak;
    return fr;
  }
};

}  // namespace sc
