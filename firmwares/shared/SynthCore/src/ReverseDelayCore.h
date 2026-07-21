#pragma once

// Reverse Delay — chunked reverse-echo core (ambient swells / pre-echo trails).
//
// Used by:
//   - firmwares/mod2-reverse-delay/mod2-reverse-delay.ino
//   - rack-plugins/src/mod2-reverse-delay.cpp
//
// The write head runs forward into a caller-owned int16 circular buffer
// (sc::DelayLine); playback replays fixed-length "chunks" of that history
// *backwards*, so each repeat swells and pre-echoes. Reverse playback is done
// with a small pool of overlapping grains (normalised overlap-add): a fresh
// grain is launched every half-chunk, so there are always ~2 grains covering
// the output. Each grain fades in/out with a raised-cosine window (the fade-in
// length is the POT2-shift "swell" control), and the summed grains are divided
// by the summed windows — a weighted average that is click-free across chunk
// boundaries, gap-free during time changes (a grain captures its length at
// birth, so a moving knob only affects *new* grains), and always bounded by the
// loudest grain sample (so the output can never blow up).
//
// A grain reading reversed at speed s reads the delay line at
//   delay = age + phase          (age = samples lived, phase = s*age)
// which sweeps a recorded chunk from newest to oldest; a forward grain reads
//   delay = age + len - phase     (a plain constant-delay tap at s = 1)
// so "forward" grains are ordinary delay repeats — that is exactly the
// Alternating mode (rev, fwd, rev…) and the IN2 direction-flip pocket.
//
// Modes (BUTTON short-press on hardware):
//   REVERSE    — every grain reversed.
//   ALTERNATING— grains alternate reversed / forward each launch.
//   OCTAVE     — reversed at 2x read speed = "shimmer-lite", up an octave.
//
// Feedback re-injects the (low-passed, soft-limited) reversed output into the
// write path; because it is itself reversed, even repeats come back
// forward-ish. Feedback is clamped < 1 and the write is soft-saturated, and
// DelayLine::write clamps to +/-1, so the buffer — and therefore the output —
// stays finite at any feedback setting.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop.

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Playback character (BUTTON short-press cycles these on hardware).
enum ReverseDelayMode : uint8_t {
  REVDLY_REVERSE = 0,      // all repeats reversed
  REVDLY_ALTERNATING = 1,  // reversed / forward / reversed…
  REVDLY_OCTAVE = 2,       // reversed at 2x = octave-up shimmer
  REVDLY_MODE_COUNT = 3
};

// Chunk-time span. POT1 sweeps 100 ms .. kReverseMaxChunkSec exponentially.
constexpr float kReverseMinChunkSec = 0.100f;
constexpr float kReverseMaxChunkSec = 2.000f;

// POT1 -> reverse chunk length in seconds (exponential taper).
inline float reverseChunkSec(float pot01) {
  return kReverseMinChunkSec *
         powf(kReverseMaxChunkSec / kReverseMinChunkSec, clampf(pot01, 0.0f, 1.0f));
}

// POT2 -> feedback amount: 0 .. 0.95 (clamped below unity; the write is
// soft-saturated so it thickens without ever running away).
inline float reverseFeedback(float pot01) {
  return 0.95f * clampf(pot01, 0.0f, 1.0f);
}

// Clock-synced ratios: with a clock on IN1, POT1 picks a musical division /
// multiple of the measured period instead of an absolute chunk time.
constexpr float kReverseClockRatios[8] = {
    0.25f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 0.75f, 1.0f, 1.5f, 2.0f};

inline float reverseClockRatio(float pot01) {
  int idx = (int)(clampf(pot01, 0.0f, 1.0f) * 7.999f);
  return kReverseClockRatios[idx];
}

// Grain amplitude window over its life `u` in 0..1. `swell` (0..1) stretches the
// raised-cosine fade-in from a short de-click ramp (hard, percussive) to almost
// the whole grain (a long ambient swell); the tail always keeps a short
// raised-cosine release so a grain never ends on a step.
inline float reverseGrainWindow(float u, float swell) {
  u = clampf(u, 0.0f, 1.0f);
  const float rel = 0.12f;  // fixed release fraction (de-click tail)
  const float atk = 0.10f + (0.85f - 0.10f) * clampf(swell, 0.0f, 1.0f);
  if (u < atk) return 0.5f * (1.0f - cosf(kPi * (u / atk)));
  if (u > 1.0f - rel) return 0.5f * (1.0f - cosf(kPi * ((1.0f - u) / rel)));
  return 1.0f;
}

struct ReverseDelayCore {
  // Parameters (write directly; see the mappers above).
  float chunkSec = 0.5f;             // target reverse-chunk length
  float feedback = 0.4f;             // 0 .. 0.95
  float wet = 0.5f;                  // 0 dry .. 1 fully wet
  float swell = 0.35f;               // grain fade-in amount (POT2 shift)
  uint8_t mode = REVDLY_REVERSE;     // ReverseDelayMode
  bool flip = false;                 // IN2: momentary forward ("normal") pockets

  static constexpr int kMaxGrains = 4;

  struct Grain {
    bool active = false;
    float age = 0.0f;      // samples lived (advances by 1)
    float phase = 0.0f;    // read position within the chunk (advances by speed)
    float len = 4410.0f;   // chunk length in samples (captured at launch)
    float speed = 1.0f;    // 1 = normal, 2 = octave-up
    bool reversed = true;  // false = forward (plain delay) grain
  };

  // State.
  DelayLine line;
  Grain grains[kMaxGrains];
  float chunkSecS = 0.5f;   // smoothed chunk length (no zipper on time)
  float feedbackS = 0.4f;   // smoothed feedback
  float wetS = 0.5f;        // smoothed mix
  float swellS = 0.35f;     // smoothed swell
  float hopTimer = 0.0f;    // samples until the next grain launch
  float fbLp = 0.0f;        // low-passed feedback signal
  float ledSweep = 0.0f;    // 0..1 progress of the newest grain (LED ramp)
  bool altToggle = false;   // alternating mode: next grain reversed?
  bool primed = false;      // first process() snaps smoothing + launches a grain

  // `buf`/`n`: caller-owned arena; reversed playback reaches ~2x the chunk
  // length behind the write head, so usable chunk time is ~n/2 samples.
  void init(int16_t* buf, uint32_t n) {
    line.init(buf, n);
    reset();
  }

  void reset() {
    line.clear();
    for (int i = 0; i < kMaxGrains; i++) grains[i] = Grain();
    hopTimer = 0.0f;
    fbLp = 0.0f;
    ledSweep = 0.0f;
    altToggle = false;
    primed = false;  // smoothing snaps + a grain launches on the next process()
  }

  // 0..1 brightness for the panel LED — ramps up across each reverse sweep.
  float ledLevel() const { return ledSweep; }

  // Longest chunk (samples) that keeps reversed reads inside the buffer.
  float maxChunkSamples() const {
    return line.len > 8 ? (float)line.len * 0.5f - 2.0f : 1.0f;
  }

  void launchGrain(float dt) {
    float len = clampf(chunkSecS / dt, 4.0f, maxChunkSamples());
    bool reversed;
    float speed;
    if (flip) {
      reversed = false;  // IN2 forward pocket = plain delay repeat
      speed = 1.0f;
    } else if (mode == REVDLY_OCTAVE) {
      reversed = true;
      speed = 2.0f;
    } else if (mode == REVDLY_ALTERNATING) {
      reversed = altToggle;
      altToggle = !altToggle;
      speed = 1.0f;
    } else {
      reversed = true;
      speed = 1.0f;
    }

    // Reuse a free slot, else steal the oldest grain (it is nearly faded out).
    int slot = 0;
    float oldest = -1.0f;
    for (int i = 0; i < kMaxGrains; i++) {
      if (!grains[i].active) { slot = i; oldest = -1.0f; break; }
      if (grains[i].age > oldest) { oldest = grains[i].age; slot = i; }
    }
    Grain& g = grains[slot];
    g.active = true;
    g.age = 0.0f;
    g.phase = 0.0f;
    g.len = len;
    g.speed = speed;
    g.reversed = reversed;
  }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    if (!primed) {
      chunkSecS = chunkSec;
      feedbackS = feedback;
      wetS = wet;
      swellS = swell;
      primed = true;
      launchGrain(dt);
    }

    // One-pole parameter smoothing (~20 Hz) — no zipper on any control.
    const float pc = onePoleCoef(20.0f, dt);
    chunkSecS += (chunkSec - chunkSecS) * pc;
    feedbackS += (feedback - feedbackS) * pc;
    wetS += (wet - wetS) * pc;
    swellS += (swell - swellS) * pc;

    // Write the input plus the (soft-limited) reversed feedback. DelayLine
    // clamps to +/-1, so the buffer is always finite.
    line.write(softSat(in + feedbackS * fbLp));

    // Launch a fresh grain every half-grain-life so ~2 grains always overlap.
    hopTimer -= 1.0f;
    if (hopTimer <= 0.0f) {
      launchGrain(dt);
      // grain real lifetime = len / speed; hop = half of it (50% overlap).
      float lenNow = clampf(chunkSecS / dt, 4.0f, maxChunkSamples());
      float speedNow = (mode == REVDLY_OCTAVE && !flip) ? 2.0f : 1.0f;
      hopTimer = 0.5f * (lenNow / speedNow);
      if (hopTimer < 1.0f) hopTimer = 1.0f;
    }

    // Normalised overlap-add of the active grains.
    float num = 0.0f;
    float den = 1e-4f;
    float newestAge = 1e30f;
    for (int i = 0; i < kMaxGrains; i++) {
      Grain& g = grains[i];
      if (!g.active) continue;
      const float u = g.phase / g.len;
      if (u >= 1.0f) { g.active = false; continue; }
      const float w = reverseGrainWindow(u, swellS);
      const float delay = g.reversed ? (g.age + g.phase)
                                     : (g.age + g.len - g.phase);
      const float s = line.read(delay);
      num += s * w;
      den += w;
      if (g.age < newestAge) { newestAge = g.age; ledSweep = u; }
      g.age += 1.0f;
      g.phase += g.speed;
    }
    const float rev = num / den;

    // Low-pass the feedback path (darker, controlled recirculation).
    fbLp += (rev - fbLp) * onePoleCoef(4000.0f, dt);

    return in + (rev - in) * wetS;
  }
};

}  // namespace sc
