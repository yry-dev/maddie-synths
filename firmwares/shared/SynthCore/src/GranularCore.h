#pragma once

// Granular — live-buffer granular delay / cloud-texture core.
//
// Used by:
//   - firmwares/mod2-granular/mod2-granular.ino
//   - rack-plugins/src/mod2-granular.cpp
//
// Tiny grains scattered from a continuously-recorded buffer: Clouds-adjacent
// smears, stutters and shimmering textures from any input. A fixed pool of
// grains (kGranularMaxGrains) reads short windowed slices of the record buffer
// at (optionally pitch-shifted) rates, sprayed randomly around the write head.
// A "texture" macro moves grain density, position spray and pitch jitter
// together; grain size, grain pitch (quantised octaves/fifths) and wet/dry are
// separate. Three grain characters: SMOOTH (raised-cosine window), PERC (the
// same window times an exponential decay -> percussive expodec) and REVERSE
// (window, buffer read backwards). Grains spawn either stochastically from the
// density macro or on demand via triggerGrain() (IN1 external clock on
// hardware). freeze stops recording so the held buffer is granulated.
//
// Windows are computed with an incremental unit-vector rotation (one sinf/cosf
// per grain *spawn*, then 4 mul / 2 add per sample) — never a per-sample cosf.
// Overlap is soft-normalised by 1/sqrt(active grains) so density does not clip.
//
// The record buffer is a caller-provided int16 arena over which this core does
// its own absolute-index circular addressing (grains need to move forward and
// backward through the buffer, unlike a plain delay tap). Size it for
// kGranularBufferSec of audio via granularArenaSamples(fs) — ~4.8 s / ~350 KB
// int16 at the firmware's 36.6 kHz (the README RAM budget), a std::vector on
// Rack.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop.

#include <math.h>
#include <stdint.h>

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Grain character (BUTTON short-press cycles these on hardware).
enum GranularMode : uint8_t {
  GRAN_SMOOTH = 0,   // raised-cosine (Hann) window
  GRAN_PERC = 1,     // window * exponential decay -> percussive expodec
  GRAN_REVERSE = 2,  // window, buffer read backwards
  GRAN_MODE_COUNT = 3
};

// Fixed grain pool. README open question was 6 vs 8 on real hardware; 8 is
// chosen (the lush end of the range) and the scheduler ducks the *effective*
// count at small grain sizes so the overlap never explodes — see the density
// -> targetActive mapping in process().
constexpr int kGranularMaxGrains = 8;

// Record buffer length: ~4.8 s (README: "~350 KB, int16"). int16 -> ~350 KB at
// the firmware's 36.6 kHz; a vector sized for the engine rate on Rack.
constexpr float kGranularBufferSec = 4.8f;

inline uint32_t granularArenaSamples(float fsHz) {
  return (uint32_t)(kGranularBufferSec * fsHz) + 16;
}

// POT1 -> grain size in seconds: exponential taper 10 ms (pot=0) .. 250 ms.
inline float granularSizeSec(float pot01) {
  return 0.010f * powf(25.0f, clampf(pot01, 0.0f, 1.0f));
}

// BUTTON+POT2 -> grain pitch: quantised octaves/fifths around unity, i.e. one
// of {-12, -7, 0, +7, +12} semitones (unity at pot=0.5). Returns semitones.
inline float granularPitchSemi(float pot01) {
  static const float kSet[5] = {-12.0f, -7.0f, 0.0f, 7.0f, 12.0f};
  int idx = (int)(clampf(pot01, 0.0f, 1.0f) * 4.0f + 0.5f);
  if (idx < 0) idx = 0;
  if (idx > 4) idx = 4;
  return kSet[idx];
}

struct GranularCore {
  // Parameters (write directly; see the mappers above).
  float sizeSec = 0.08f;    // grain length (10 - 250 ms)
  float density = 0.5f;     // texture macro: spawn rate + spray + pitch jitter
  float pitchSemi = 0.0f;   // base grain transpose in semitones
  float mix = 0.5f;         // 0 dry .. 1 fully granulated
  uint8_t mode = GRAN_SMOOTH;
  bool freeze = false;      // true = stop recording, granulate held buffer

  // ---- one grain voice ----
  struct Grain {
    bool active = false;
    bool perc = false;      // character captured at spawn (PERC decay applied)
    float readIdx = 0.0f;   // absolute fractional index into buf [0, len)
    float rate = 1.0f;      // buffer samples advanced per output sample
    float dir = 1.0f;       // +1 forward, -1 reverse
    uint32_t age = 0;       // samples elapsed
    uint32_t life = 1;      // grain length in samples
    // Incremental Hann window: unit vector rotated by omega = 2*pi/life each
    // sample; window = 0.5 - 0.5*cx goes 0 -> 1 -> 0 over the grain.
    float cx = 1.0f, cy = 0.0f;   // rotating unit vector (cx = cos(n*omega))
    float cw = 1.0f, sw = 0.0f;   // rotation step (cos/sin of omega)
    float envAmp = 1.0f;    // PERC exponential-decay multiplier state
    float envMul = 1.0f;    // per-sample decay factor (exp(-k/life))
  };

  // State.
  int16_t* buf = nullptr;
  uint32_t len = 0;         // buffer capacity in samples
  uint32_t writeIdx = 0;    // next slot to record into
  Grain grains[kGranularMaxGrains];
  uint32_t rng = 0x1234abcdu;  // xorshift state (grain spray / jitter)

  bool primed = false;      // first process() snaps the smoothers
  float sSize = 0.08f;      // smoothed sizeSec (no zipper)
  float sDensity = 0.5f;    // smoothed density
  float sPitch = 0.0f;      // smoothed pitchSemi
  float sMix = 0.5f;        // smoothed mix
  float gainSmooth = 1.0f;  // smoothed overlap normalisation
  float ledFlash = 0.0f;    // decays after each spawn (panel LED)
  bool pendingTrigger = false;  // external (IN1) grain request

  // `b`/`n`: caller-owned arena (granularArenaSamples() long).
  void init(int16_t* b, uint32_t n) {
    buf = b;
    len = n;
    reset();
  }

  void reset() {
    writeIdx = 0;
    if (buf)
      for (uint32_t i = 0; i < len; i++) buf[i] = 0;
    for (int i = 0; i < kGranularMaxGrains; i++) grains[i].active = false;
    rng = 0x1234abcdu;
    primed = false;
    gainSmooth = 1.0f;
    ledFlash = 0.0f;
    pendingTrigger = false;
  }

  // Request a grain on the next process() (IN1 external clock on hardware).
  void triggerGrain() { pendingTrigger = true; }

  // 0..1 brightness for the panel LED — flickers on each grain spawn so the
  // density is visible.
  float ledLevel() const { return clampf(ledFlash, 0.0f, 1.0f); }

  // Uniform random in [0,1).
  inline float rand01() {
    return (float)(xorshift32(rng) >> 8) * (1.0f / 16777216.0f);
  }

  // Denormal flush for slowly-decaying state.
  static inline float flush(float x) {
    return (x < 1e-25f && x > -1e-25f) ? 0.0f : x;
  }

  // Spawn one grain using the current smoothed parameters. `fs` = 1/dt.
  void spawnGrain(float fs) {
    if (len < 32) return;
    // Pick a free slot, else steal the grain closest to finishing (max age
    // fraction) so the freshest onsets survive.
    int slot = -1;
    float worst = -1.0f;
    for (int i = 0; i < kGranularMaxGrains; i++) {
      if (!grains[i].active) { slot = i; break; }
      const float frac = (float)grains[i].age / (float)grains[i].life;
      if (frac > worst) { worst = frac; slot = i; }
    }
    if (slot < 0) return;
    Grain& g = grains[slot];

    // Grain length in samples (bounded so it always fits the buffer).
    float lifeF = clampf(sSize * fs, 4.0f, (float)len * 0.5f);
    uint32_t life = (uint32_t)lifeF;
    if (life < 2) life = 2;

    // Pitch: quantised base + density-scaled jitter (a random octave/fifth
    // jump plus a few cents of detune). Keeps it lush without going sour.
    const float d = clampf(sDensity, 0.0f, 1.0f);
    float semi = sPitch;
    if (rand01() < d * 0.5f) {
      static const float kJump[4] = {-12.0f, -7.0f, 7.0f, 12.0f};
      semi += kJump[(int)(rand01() * 3.999f) & 3];
    }
    semi += (rand01() * 2.0f - 1.0f) * d * 0.5f;  // <= +/- 0.5 semitone detune
    const float rate = powf(2.0f, semi * (1.0f / 12.0f));

    const bool reverse = (mode == GRAN_REVERSE);
    const float span = (float)life * rate;  // buffer span the grain traverses

    // Position spray: random offset behind the write head, up to ~0.5 s at
    // full density. Clamp so the read window stays inside recorded memory and
    // never overtakes the write head.
    const float maxSpray = d * 0.5f * fs;
    float spray = rand01() * maxSpray;
    const float guard = 4.0f;
    // Forward grains advance toward the write head, so they must start at least
    // `span` behind it; reverse grains move away, so they only need the guard.
    float startDelay = guard + spray + (reverse ? 0.0f : span);
    const float maxDelay = (float)len - span - 8.0f;
    if (startDelay > maxDelay) startDelay = maxDelay;
    if (startDelay < 1.0f) startDelay = 1.0f;

    float ridx = (float)writeIdx - startDelay;
    while (ridx < 0.0f) ridx += (float)len;
    while (ridx >= (float)len) ridx -= (float)len;

    g.active = true;
    g.perc = (mode == GRAN_PERC);
    g.readIdx = ridx;
    g.rate = rate;
    g.dir = reverse ? -1.0f : 1.0f;
    g.age = 0;
    g.life = life;
    const float omega = kTwoPi / (float)life;
    g.cx = 1.0f;
    g.cy = 0.0f;
    g.cw = cosf(omega);
    g.sw = sinf(omega);
    g.envAmp = 1.0f;
    // PERC: exp decay reaching ~e^-4 across the grain (percussive expodec).
    g.envMul = g.perc ? expf(-4.0f / (float)life) : 1.0f;

    ledFlash = 1.0f;
  }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    if (!buf || len < 32) return in;
    const float fs = 1.0f / dt;

    // ---- smooth every user parameter (no zipper) --------------------------
    if (!primed) {
      sSize = sizeSec;
      sDensity = density;
      sPitch = pitchSemi;
      sMix = mix;
      primed = true;
    } else {
      const float c = onePoleCoef(20.0f, dt);
      sSize += (sizeSec - sSize) * c;
      sDensity += (density - sDensity) * c;
      sPitch += (pitchSemi - sPitch) * c;
      sMix += (mix - sMix) * c;
    }

    // ---- record (unless frozen) -------------------------------------------
    if (!freeze) {
      const float w = clampf(in, -1.0f, 1.0f);
      buf[writeIdx] = (int16_t)(w * 32767.0f);
      if (++writeIdx >= len) writeIdx = 0;
    }

    // ---- scheduler: stochastic density spawn + external trigger -----------
    const float d = clampf(sDensity, 0.0f, 1.0f);
    const float grainLenSec = clampf(sSize, 0.005f, 1.0f);
    // Effective simultaneous grains rise with density (quadratic taper) and
    // are capped by the pool — this is the "auto-duck at small grain sizes"
    // the README asks about: spawnRate is derived from a bounded target count,
    // so tiny grains never demand more overlap than the pool can hold.
    const float targetActive = 0.4f + d * d * ((float)kGranularMaxGrains - 0.4f);
    float spawnRate = targetActive / grainLenSec;   // grains / second
    const float maxRate = (float)kGranularMaxGrains / 0.010f;
    if (spawnRate > maxRate) spawnRate = maxRate;
    if (rand01() < spawnRate * dt) spawnGrain(fs);
    if (pendingTrigger) {
      spawnGrain(fs);
      pendingTrigger = false;
    }

    // ---- render the grain pool (overlap-sum) ------------------------------
    float sum = 0.0f;
    int activeCount = 0;
    const float invLen = 1.0f / 32768.0f;
    for (int i = 0; i < kGranularMaxGrains; i++) {
      Grain& g = grains[i];
      if (!g.active) continue;

      // Fractional read with linear interpolation (circular).
      uint32_t i0 = (uint32_t)g.readIdx;
      if (i0 >= len) i0 = len - 1;
      uint32_t i1 = i0 + 1;
      if (i1 >= len) i1 = 0;
      const float fr = g.readIdx - floorf(g.readIdx);
      const float s0 = (float)buf[i0] * invLen;
      const float s1 = (float)buf[i1] * invLen;
      const float sample = s0 + (s1 - s0) * fr;

      // Window (incremental Hann) * optional percussive decay.
      float env = 0.5f - 0.5f * g.cx;
      if (g.perc) env *= g.envAmp;
      sum += sample * env;
      activeCount++;

      // Advance the window rotation.
      const float nx = g.cx * g.cw - g.cy * g.sw;
      const float ny = g.cx * g.sw + g.cy * g.cw;
      g.cx = nx;
      g.cy = ny;
      g.envAmp = flush(g.envAmp * g.envMul);

      // Advance the read position (circular).
      g.readIdx += g.dir * g.rate;
      while (g.readIdx >= (float)len) g.readIdx -= (float)len;
      while (g.readIdx < 0.0f) g.readIdx += (float)len;

      if (++g.age >= g.life) g.active = false;
    }

    // ---- soft-normalise overlap so density doesn't clip -------------------
    const float targetGain =
        1.0f / sqrtf((float)(activeCount > 0 ? activeCount : 1));
    gainSmooth += (targetGain - gainSmooth) * onePoleCoef(30.0f, dt);
    gainSmooth = flush(gainSmooth);
    float wetSig = softSat(sum * gainSmooth);

    // LED flicker decays between spawns.
    ledFlash += (0.0f - ledFlash) * onePoleCoef(12.0f, dt);
    ledFlash = flush(ledFlash);

    return lerpf(in, wetSig, clampf(sMix, 0.0f, 1.0f));
  }
};

}  // namespace sc
