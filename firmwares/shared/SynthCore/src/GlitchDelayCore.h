#pragma once

// Glitch Delay — a delay whose read head misbehaves on purpose.
//
// Used by:
//   - firmwares/mod2-glitch-delay/mod2-glitch-delay.ino
//   - rack-plugins/src/mod2-glitch-delay.cpp
//
// Built on the same sc::DelayLine engine as DelayFxCore, but instead of one
// steady read head it runs an EVENT SCHEDULER. Audio is written to a circular
// buffer every sample; the read side is chopped into chunks (one chunk = the
// POT1 delay time). At each chunk boundary a die is rolled against the "chaos"
// amount and, per the selected palette, the next chunk is played as one of:
//
//   NORMAL  — plain delay repeat (read head sits at the base delay).
//   SKIP    — jump the read head to a random offset in the buffer.
//   REVERSE — play the chunk backwards (read head runs against the write head).
//   STUTTER — re-read one captured chunk in place, repeating it click-free
//             across consecutive stutter chunks (machine-gun repeat).
//   TAPECUT — half / double read speed for the chunk (pitch-cut splice).
//
// Two read heads (current + incoming) with a short raised-cosine crossfade make
// EVERY transition — including the wild jumps — click-free with one mechanism
// (identical idea to DelayFxCore's retune fade, ~3 ms here). The feedback path
// is low-passed and soft-saturated so runaway repeats limit warmly instead of
// wrapping the int16 buffer.
//
// Deterministic-feel: the per-chunk RNG is (optionally) reseeded from the chunk
// index modulo `loopChunks`, so over a repeating musical loop the same chunks
// glitch the same way every time (great for live sets). Turn `deterministic`
// off for a free-running, never-repeating stream. Randomness is sc::xorshift32
// throughout, so firmware and the VCV Rack port produce bit-identical glitches
// from the same seed.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop. Delay memory is a caller-provided int16 arena
// (platform owns it): ~300 KB = ~4 s at 36.6 kHz, within the mod2-fx budget.

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Glitch palette (BUTTON short-press cycles these on hardware). The value is an
// index into kGlitchPaletteWeights below.
enum GlitchPalette : uint8_t {
  GLITCH_SKIPS = 0,    // random skips + the odd tape-cut
  GLITCH_REVERSE = 1,  // reversed chunks
  GLITCH_STUTTER = 2,  // in-place stutter repeats + tape-cuts
  GLITCH_ALL = 3,      // the full grab-bag
  GLITCH_PALETTE_COUNT = 4
};

// Event kinds a chunk can be rendered as (NORMAL is handled separately).
enum GlitchEvent : uint8_t {
  GEV_SKIP = 0,
  GEV_REVERSE = 1,
  GEV_STUTTER = 2,
  GEV_TAPECUT = 3,
  GEV_COUNT = 4
};

// Per-palette event weights (rows sum to 1). Chaos scales the *probability* a
// glitch happens at all; once it does, these pick which kind. Tuned by ear
// against a drum loop (README open question): skips read as "wrong tape",
// reverse is the ambient special, stutter is the rhythmic one, ALL is chaos.
static const float kGlitchPaletteWeights[GLITCH_PALETTE_COUNT][GEV_COUNT] = {
    /* SKIPS   */ {0.70f, 0.00f, 0.00f, 0.30f},
    /* REVERSE */ {0.15f, 0.80f, 0.00f, 0.05f},
    /* STUTTER */ {0.05f, 0.00f, 0.70f, 0.25f},
    /* ALL     */ {0.30f, 0.25f, 0.25f, 0.20f},
};

// POT1 -> base delay time / chunk length in seconds: exp taper 20 ms .. maxSec.
inline float glitchDelayTimeSec(float pot01, float maxSec) {
  return 0.020f * powf(maxSec / 0.020f, clampf(pot01, 0.0f, 1.0f));
}

// POT2 -> chaos amount 0..1 (probability + intensity of glitch events).
inline float glitchDelayChaos(float pot01) {
  return clampf(pot01, 0.0f, 1.0f);
}

// Shift-layer pot -> feedback amount: 0 .. ~0.98 (kept below unity — the skip
// events already recirculate wild material, so runaway is easy to hit).
inline float glitchDelayFeedback(float pot01) {
  return 0.98f * clampf(pot01, 0.0f, 1.0f);
}

// Clock-synced ratios: with a clock on IN1, POT1 picks a musical division /
// multiple of the measured period for the chunk length instead of a raw time.
constexpr float kGlitchClockRatios[8] = {
    0.25f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 0.75f, 1.0f, 1.5f, 2.0f};

inline float glitchDelayClockRatio(float pot01) {
  int idx = (int)(clampf(pot01, 0.0f, 1.0f) * 7.999f);
  return kGlitchClockRatios[idx];
}

struct GlitchDelayCore {
  // Parameters (write directly; see the mappers above).
  float timeSec = 0.4f;                 // base delay time = chunk length
  float chaos = 0.4f;                   // 0 clean .. 1 mayhem
  float feedback = 0.4f;                // 0 .. ~0.98
  float wet = 0.5f;                     // 0 dry .. 1 fully wet
  uint8_t palette = GLITCH_ALL;         // GlitchPalette
  bool force = false;                   // IN2 gate: guarantee a glitch/boundary

  // Deterministic-feel controls.
  bool deterministic = true;            // repeat glitches over a loop
  uint16_t loopChunks = 16;             // loop length (chunks) when deterministic
  uint32_t seed = 0x2468ACE1u;          // base RNG seed (reset value)

  static constexpr float kXfadeSec = 0.003f;   // event crossfade length (3 ms)
  static constexpr float kMinChunkSec = 0.02f; // shortest chunk (avoids div blowups)

  // A single read voice: `delay` = samples behind the write head, advancing by
  // (1 - v) each output sample. v = 1 normal, -1 reverse, 0.5/2 tape-cut.
  struct Head {
    float delay = 4410.0f;
    float v = 1.0f;
  };

  // State.
  DelayLine line;
  Head headA;              // active read head
  Head headB;              // incoming read head while crossfading
  float xfade = 1.0f;      // 0 -> 1 crossfade position (1 = settled on A)
  float baseDelay = 4410.0f;  // smoothed delay target (samples)
  float chunkPhase = 0.0f;    // seconds into the current chunk

  // Smoothed user params (one-pole, no zipper).
  float fbS = 0.4f;
  float wetS = 0.5f;
  float chaosS = 0.4f;

  // Feedback / housekeeping.
  float fbLp = 0.0f;       // feedback-path one-pole low-pass state
  float led = 0.0f;        // 0..1 flash, pulsed on each glitch event

  // Event bookkeeping.
  uint32_t rng = 0x2468ACE1u;  // free-running RNG (non-deterministic mode)
  uint32_t chunkIndex = 0;     // chunks elapsed since reset
  bool glitching = false;      // headA currently rendering a glitch chunk
  bool inStutter = false;      // consecutive-stutter run active
  float stutterAnchor = 0.0f;  // captured delay of the stuttered chunk
  bool primed = false;         // first process() snaps heads to the target

  // `buf`/`n`: caller-owned arena; usable delay is n-2 samples.
  void init(int16_t* buf, uint32_t n) {
    line.init(buf, n);
    reset();
  }

  void reset() {
    line.clear();
    headA = Head();
    headB = Head();
    xfade = 1.0f;
    chunkPhase = 0.0f;
    fbLp = 0.0f;
    led = 0.0f;
    rng = seed ? seed : 1u;
    chunkIndex = 0;
    glitching = false;
    inStutter = false;
    stutterAnchor = 0.0f;
    fbS = feedback;
    wetS = wet;
    chaosS = chaos;
    primed = false;  // heads snap on the next process(), once dt is known
  }

  // 0..1 activity level for the panel LED (flashes on each glitch event).
  float ledLevel() const { return led; }

  // One xorshift draw in 0..1.
  static inline float rand01(uint32_t& s) {
    return (float)(xorshift32(s) >> 8) * (1.0f / 16777216.0f);
  }

  // Advance a read head one output sample and keep it inside the buffer.
  static inline void advanceHead(Head& h, float maxDelay) {
    h.delay += (1.0f - h.v);
    if (h.delay < 1.0f) h.delay = 1.0f;
    if (h.delay > maxDelay) h.delay = maxDelay;
  }

  // Weighted pick of an event kind for the active palette.
  uint8_t pickEventType(uint32_t& st) const {
    const float* w = kGlitchPaletteWeights[palette < GLITCH_PALETTE_COUNT
                                               ? palette
                                               : GLITCH_ALL];
    float r = rand01(st);
    float acc = 0.0f;
    for (uint8_t i = 0; i < GEV_COUNT; i++) {
      acc += w[i];
      if (r < acc) return i;
    }
    return GEV_TAPECUT;
  }

  // Roll the dice at a chunk boundary and configure the next read head.
  void scheduleEvent(float dt, float maxDelay, float chunkSec) {
    chunkIndex++;

    // Pick the RNG source: a per-chunk reseed (repeats over a loop) or the
    // free-running state. Either way the draws below are xorshift32.
    uint32_t local = 0;
    uint32_t* rp;
    if (deterministic) {
      const uint32_t period = loopChunks ? loopChunks : 1u;
      local = (seed ? seed : 1u) ^
              ((uint32_t)(chunkIndex % period) * 0x9E3779B9u + 0x85EBCA6Bu);
      if (local == 0) local = 0xDEADBEEFu;
      xorshift32(local);
      xorshift32(local);
      rp = &local;
    } else {
      rp = &rng;
    }

    const float roll = rand01(*rp);
    const bool doGlitch = force || (roll < chaosS * 0.9f);

    Head nh;
    nh.delay = baseDelay;
    nh.v = 1.0f;
    bool startFade;

    if (!doGlitch) {
      inStutter = false;
      // A plain repeat only needs a fade if we were glitching or the base
      // delay has drifted; otherwise the head continues seamlessly.
      const bool settledNormal =
          !glitching && fabsf(headA.delay - baseDelay) <= (headA.delay * 0.001f + 0.5f);
      startFade = !settledNormal;
      glitching = false;
    } else {
      led = 1.0f;
      const uint8_t type = pickEventType(*rp);
      const float intensity = clampf(chaosS, 0.0f, 1.0f);
      const float chunkLenSamp = chunkSec / dt;

      if (type == GEV_SKIP) {
        inStutter = false;
        nh.delay = 1.0f + rand01(*rp) * intensity * (maxDelay - 2.0f);
        nh.v = 1.0f;
      } else if (type == GEV_REVERSE) {
        inStutter = false;
        nh.delay = headA.delay;  // start from here and run backwards
        nh.v = -1.0f;
      } else if (type == GEV_STUTTER) {
        // Re-read one captured chunk in place. As the buffer keeps filling the
        // chunk recedes by one chunk-length each boundary, so advancing the
        // anchor by chunkLenSamp keeps pointing at the *same* audio.
        if (!inStutter) {
          stutterAnchor = headA.delay;
          inStutter = true;
        } else {
          stutterAnchor += chunkLenSamp;
        }
        nh.delay = clampf(stutterAnchor, 1.0f, maxDelay);
        stutterAnchor = nh.delay;
        nh.v = 1.0f;
      } else {  // GEV_TAPECUT
        inStutter = false;
        nh.v = (rand01(*rp) < 0.5f) ? 0.5f : 2.0f;
        // Double-speed reads twice as fast toward the write head: start it
        // further back so it doesn't collide during the chunk.
        nh.delay = (nh.v > 1.0f)
                       ? clampf(baseDelay * 0.5f + chunkLenSamp, 1.0f, maxDelay)
                       : baseDelay;
      }
      glitching = true;
      startFade = true;
    }

    if (startFade) {
      // If a fade is still in flight, collapse it onto A first so we never
      // three-way blend (keeps the crossfade a clean two-head A->B).
      if (xfade < 1.0f) headA = headB;
      headB = nh;
      xfade = 0.0f;
    }
  }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    const float maxDelay = (float)(line.len - 2);
    const float baseTarget = clampf(timeSec / dt, 1.0f, maxDelay);

    if (!primed) {
      baseDelay = baseTarget;
      headA.delay = headB.delay = baseTarget;
      headA.v = headB.v = 1.0f;
      fbS = feedback;
      wetS = wet;
      chaosS = chaos;
      chunkPhase = 0.0f;
      primed = true;
    }

    // One-pole smoothing of every user param (no zipper).
    const float pcoef = onePoleCoef(20.0f, dt);
    fbS += (clampf(feedback, 0.0f, 0.999f) - fbS) * pcoef;
    wetS += (clampf(wet, 0.0f, 1.0f) - wetS) * pcoef;
    chaosS += (clampf(chaos, 0.0f, 1.0f) - chaosS) * pcoef;
    baseDelay += (baseTarget - baseDelay) * pcoef;

    // Chunk boundary: roll for the next event.
    chunkPhase += dt;
    float chunkSec = timeSec;
    if (chunkSec < kMinChunkSec) chunkSec = kMinChunkSec;
    if (chunkPhase >= chunkSec) {
      chunkPhase -= chunkSec;
      if (chunkPhase >= chunkSec) chunkPhase = 0.0f;  // guard huge dt / tiny chunk
      scheduleEvent(dt, maxDelay, chunkSec);
    }

    // Read (two heads with a raised-cosine crossfade at event boundaries).
    const float outA = line.read(headA.delay);
    float echo;
    if (xfade < 1.0f) {
      const float outB = line.read(headB.delay);
      xfade += dt / kXfadeSec;
      if (xfade >= 1.0f) {
        headA = headB;
        xfade = 1.0f;
        echo = outB;
      } else {
        echo = lerpf(outA, outB, raisedCosine(xfade));
      }
    } else {
      echo = outA;
    }

    // Feedback path: low-pass then soft-saturate so >0 feedback of the wild
    // (skipped/reversed) material limits warmly instead of blowing up.
    fbLp += (echo - fbLp) * onePoleCoef(6000.0f, dt);
    if (fabsf(fbLp) < 1e-15f) fbLp = 0.0f;  // denormal flush
    line.write(softSat(in + fbLp * fbS));

    // Advance the read head(s) for next sample.
    advanceHead(headA, maxDelay);
    if (xfade < 1.0f) advanceHead(headB, maxDelay);

    // LED flash envelope (~50 ms per event).
    if (led > 0.0f) {
      led -= dt / 0.05f;
      if (led < 0.0f) led = 0.0f;
    }

    float out = in + (echo - in) * wetS;
    if (!isFiniteF(out)) out = 0.0f;
    return out;
  }
};

}  // namespace sc
