#pragma once

// FM Drum percussion voice — the shared core of the FMDrum module.
//
// Used by:
//   - firmwares/mod2-fm_drum/mod2-fm_drum.ino  (renders TABLE_SIZE samples/strike)
//   - vcvrack/src/FMDrum.cpp                    (renders live, one sample/process)
//
// A strike is a fixed-length window (kFmDrumNoteLen seconds) of a two-operator
// FM tone (carrier + modulator phase accumulators), with a per-strike ratio
// envelope and index envelope, an exponential amplitude envelope, a tanh
// soft-clip (drive = modulation index), and raised-cosine fades over the first
// 2% / last 10% to kill the click. Both platforms drive it one sample at a
// time, passing their own dt, so it is sample-rate independent.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h. No Arduino / Rack / Pico SDK.

#include "sc_math.h"
#include "sc_dsp.h"

namespace sc {

// Strike/note length in seconds. Mirrors the firmware's fixed NOTE_LEN. The
// firmware fills a 4096-sample table over this window (dt = NOTE_LEN/4096, so
// dt/NOTE_LEN = 1/4096); the VCV Rack port renders an identically-long strike
// at any host sample rate.
constexpr float kFmDrumNoteLen = 0.3f;

// Fixed index-envelope depth. The firmware never exposes this on a pot, so it
// stays a constant on both targets.
constexpr float kFmDrumIndexEnv = 0.3f;

// Raised-cosine head fade (first 2%) and where the tail fade begins (last 10%),
// as fractions of the strike. Mirror the firmware's 0.02 / 0.90 table splits.
constexpr float kFmDrumFadeIn = 0.02f;
constexpr float kFmDrumFadeOutStart = 0.90f;

// One rendered sample: audio in -1..+1 and the loudness envelope in 0..1 (the
// VCV port uses the latter for LED brightness).
struct FmDrumFrame {
  float audio;
  float env;
};

struct FmDrumVoice {
  bool playing = false;
  float strikePhase = 0.0f;  // 0..1 across the strike window
  float phaseC = 0.0f;       // carrier phase (radians)
  float phaseM = 0.0f;       // modulator phase (radians)

  // Synthesis parameters in engine units (set by the platform, which owns the
  // knob/CV mapping). modIndex doubles as the tanh soft-clip drive.
  float f0 = 200.0f;          // carrier fundamental (Hz)
  float opRatio = 2.0f;       // modulator/carrier frequency ratio
  float modIndex = 1.0f;      // modulation index (1..10), also soft-clip drive
  float decayRate = 5.0f;     // amplitude decay rate
  float ratioEnv = 0.0f;      // ratio-envelope depth (0..1)
  float indexEnv = kFmDrumIndexEnv;  // index-envelope depth (0..1)
  float accentLevel = 1.0f;   // 1.0 normal, 0.5 = -6 dB accent

  // Update the synthesis parameters. Both platforms freeze these at strike time
  // (the firmware bakes a table; the VCV port samples knobs on trigger).
  void setParams(float f0_, float opRatio_, float modIndex_, float decayRate_,
                 float ratioEnv_, float indexEnv_ = kFmDrumIndexEnv,
                 float accentLevel_ = 1.0f) {
    f0 = f0_;
    opRatio = opRatio_;
    // Guard the soft-clip normaliser (1/tanh(modIndex)) against a zero drive.
    modIndex = modIndex_ < 1.0f ? 1.0f : modIndex_;
    decayRate = decayRate_;
    ratioEnv = ratioEnv_;
    indexEnv = indexEnv_;
    accentLevel = accentLevel_;
  }

  // Begin a new strike (phases reset). Call setParams() first.
  void strike() {
    strikePhase = 0.0f;
    phaseC = 0.0f;
    phaseM = 0.0f;
    playing = true;
  }

  void reset() {
    playing = false;
    strikePhase = 0.0f;
    phaseC = 0.0f;
    phaseM = 0.0f;
  }

  // Render one sample and advance by `dt` seconds. Returns silence once the
  // strike window has elapsed (and clears `playing`). `strikeDuration` is the
  // window length in seconds (default kFmDrumNoteLen).
  FmDrumFrame process(float dt, float strikeDuration = kFmDrumNoteLen) {
    FmDrumFrame f;
    f.audio = 0.0f;
    f.env = 0.0f;
    if (!playing) return f;

    const float x = strikePhase;             // 0..1 across the strike
    const float envR = 1.0f - ratioEnv * x;  // ratio envelope
    const float envI = 1.0f - indexEnv * x;  // index envelope

    // Advance modulator then carrier (matches the firmware table-fill order).
    phaseM += kTwoPi * f0 * (opRatio * envR) * dt;
    phaseC += kTwoPi * f0 * dt;

    const float sample = sinf(phaseC + (modIndex * envI) * sinf(phaseM));

    // Exponential amplitude envelope; tanh soft-clip with drive = modIndex.
    const float ampEnv = expf(-decayRate * x);
    float y = softClipTanh(sample * ampEnv, modIndex) * accentLevel;
    float env = ampEnv;

    // Raised-cosine head/tail fades (bipolar -> fade toward 0 = silence).
    if (x < kFmDrumFadeIn) {
      const float fade = raisedCosine(x / kFmDrumFadeIn);
      y *= fade;
      env *= fade;
    } else if (x >= kFmDrumFadeOutStart) {
      const float mu = (x - kFmDrumFadeOutStart) / (1.0f - kFmDrumFadeOutStart);
      const float fade = 1.0f - raisedCosine(mu);
      y *= fade;
      env *= fade;
    }

    f.audio = y;
    f.env = env;

    strikePhase += dt / strikeDuration;
    if (strikePhase >= 1.0f) {
      playing = false;
    }
    return f;
  }
};

}  // namespace sc
