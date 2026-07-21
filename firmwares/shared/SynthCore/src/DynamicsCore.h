#pragma once

// Dynamics — one-knob compressor + limiter (+ trigger ducker) core.
//
// Used by:
//   - firmwares/mod2-dynamics/mod2-dynamics.ino
//   - rack-plugins/src/mod2-dynamics.cpp
//
// A tiny mono dynamics processor built for the 10-bit PWM output, where
// squashing *before* quantization audibly improves perceived quality. Three
// modes (BUTTON short-press cycles them on hardware):
//   - Compressor: one knob (Amount) sweeps threshold, ratio and makeup gain
//     together ("smash"), soft-knee gain computer, program-dependent
//     attack/release.
//   - Limiter: high ratio, very fast attack with ~1 ms lookahead (a tiny
//     float delay buffer) so peaks are caught before the PWM clips; Amount
//     sets the ceiling and auto-makes-up to it (a loudness maximiser).
//   - Ducker: an external trigger (IN1) fires a decaying envelope that
//     attenuates the audio — sidechain pumping in a mono system.
// A hard safety limiter (soft-sat + clamp) guarantees the output never
// leaves -1..+1 regardless of mode or makeup gain.
//
// Envelope follower: a peak/RMS blend feeding an attack/release one-pole
// detector. Gain computer: soft-knee, in dB. To keep the audio path cheap the
// only logf/expf/powf live in updateControl(), which runs at a low control
// rate (kDynControlInterval samples); the per-sample path is just adds,
// multiplies and one sqrtf (hardware VSQRT on the RP2350). The smoothed gain
// and every user parameter are one-pole interpolated per sample, so there is
// no zipper noise and no clicks on gain changes.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop.

#include <math.h>
#include <stdint.h>

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Processing mode (BUTTON short-press cycles these on hardware).
enum DynamicsMode : uint8_t {
  DYN_COMPRESSOR = 0,  // one-knob glue/smash compressor
  DYN_LIMITER = 1,     // brickwall-ish limiter with lookahead
  DYN_DUCKER = 2,      // IN1-triggered sidechain duck
  DYN_MODE_COUNT = 3
};

// dB <-> linear. Only ever called at control rate (see updateControl); kept as
// free functions so the knob/curve maths is shared with the platform code.
inline float dynDb2Lin(float db) { return expf(db * 0.11512925465f); }  // 10^(db/20)
inline float dynLin2Db(float lin) { return 8.6858896381f * logf(lin); }  // 20*log10

// POT2 -> release time: 30 ms .. 1 s, exponential taper.
inline float dynamicsReleaseSec(float pot01) {
  return 0.030f * powf(1000.0f / 30.0f, clampf(pot01, 0.0f, 1.0f));
}

// Attack time auto-scaled from the release setting (program-dependent feel:
// snappier when the release is short). Clamped to a musical 0.8 .. 30 ms.
inline float dynamicsAttackSec(float releaseSec) {
  return clampf(releaseSec * 0.03f, 0.0008f, 0.030f);
}

// One-knob COMPRESSOR curve: Amount sweeps threshold / ratio / makeup together.
// amount=0 is a gentle 1.5:1 glue at -6 dB; amount=1 is a -32 dB, 12:1 smash
// with +16 dB makeup. Linear interpolation (see README open question — this is
// the documented default sweep). Returns dB / ratio via out-params.
inline void dynamicsCompCurve(float amount, float& threshDb, float& ratio,
                              float& makeupDb) {
  amount = clampf(amount, 0.0f, 1.0f);
  threshDb = lerpf(-6.0f, -32.0f, amount);
  ratio = lerpf(1.5f, 12.0f, amount);
  makeupDb = lerpf(0.0f, 16.0f, amount);
}

// LIMITER curve: Amount lowers the ceiling from -1 dB down to -18 dB, ratio is
// fixed high, and the makeup brings the ceiling back to ~0 dBFS (loudness).
inline void dynamicsLimitCurve(float amount, float& threshDb, float& ratio,
                               float& makeupDb) {
  amount = clampf(amount, 0.0f, 1.0f);
  threshDb = lerpf(-1.0f, -18.0f, amount);
  ratio = 20.0f;
  makeupDb = -threshDb;
}

// Soft-knee gain-reduction (dB) for a level `overDb` above threshold, a given
// `ratio` and knee width `kneeDb`. Returns <= 0 (a reduction). Standard
// quadratic knee (Reiss/Zolzer).
inline float dynamicsGrDb(float overDb, float ratio, float kneeDb) {
  const float slope = (1.0f / ratio) - 1.0f;  // <= 0
  if (overDb <= -0.5f * kneeDb) return 0.0f;
  if (overDb >= 0.5f * kneeDb) return slope * overDb;
  const float x = overDb + 0.5f * kneeDb;
  return slope * (x * x) / (2.0f * kneeDb);
}

// Fixed soft-knee width in dB.
constexpr float kDynKneeDb = 6.0f;
// Gain computer / detector-coefficient update interval (samples). ~4.6 kHz at
// the firmware's 36.6 kHz — keeps logf/expf out of the per-sample path.
constexpr int kDynControlInterval = 8;
// Lookahead ring length (samples); ~1 ms at 36.6 kHz needs ~37, this caps it.
constexpr int kDynLookMax = 48;

struct DynamicsCore {
  // Parameters (write directly; see the mappers above).
  float amount = 0.3f;       // 0..1 one-knob amount
  float releaseSec = 0.15f;  // release time (also duck decay)
  float dryMix = 0.0f;       // 0 = fully processed .. 1 = fully dry (parallel)
  float outTrim = 1.0f;      // linear output trim (post-makeup)
  uint8_t mode = DYN_COMPRESSOR;

  // Smoothed parameter shadows (one-pole, no zipper).
  float sAmount = 0.3f, sRelease = 0.15f, sDryMix = 0.0f, sTrim = 1.0f;

  // Detector state.
  float sq_ = 0.0f;   // running mean-square (RMS component)
  float env_ = 0.0f;  // attack/release envelope (linear)

  // Gain-stage state.
  float gain_ = 1.0f;        // smoothed comp/limiter gain (linear, <= 1)
  float targetGain_ = 1.0f;  // control-rate gain target
  float makeupLin_ = 1.0f;   // makeup gain (linear)
  float gAtkC_ = 1.0f, gRelC_ = 0.01f;  // per-sample smoothing coefs
  float attackSec_ = 0.005f, releaseUsed_ = 0.15f;

  // Ducker state.
  float duckEnv_ = 0.0f;    // 1 on trigger, decays to 0
  float duckDepth_ = 0.0f;  // 0..~0.97 attenuation depth

  // Lookahead ring (fixed, no heap).
  float look_[kDynLookMax] = {0.0f};
  uint32_t w_ = 0;
  int lookLen_ = 0;

  int ctl_ = 0;    // control-rate countdown
  float led_ = 0.0f;  // gain-reduction meter, 0..1

  void reset() {
    sAmount = amount;
    sRelease = releaseSec;
    sDryMix = dryMix;
    sTrim = outTrim;
    sq_ = 0.0f;
    env_ = 0.0f;
    gain_ = 1.0f;
    targetGain_ = 1.0f;
    makeupLin_ = 1.0f;
    attackSec_ = 0.005f;
    releaseUsed_ = clampf(releaseSec, 0.03f, 1.0f);
    gAtkC_ = 1.0f;
    gRelC_ = 0.01f;
    duckEnv_ = 0.0f;
    duckDepth_ = 0.0f;
    for (int i = 0; i < kDynLookMax; i++) look_[i] = 0.0f;
    w_ = 0;
    lookLen_ = 0;
    ctl_ = 0;
  }

  // Fire the ducker envelope (call on the IN1 gate rising edge).
  void duckTrigger() { duckEnv_ = 1.0f; }

  // 0..1 brightness for the panel LED = amount of gain reduction (brighter =
  // more GR). Updated at control rate (comp/limiter) or per sample (ducker).
  float ledLevel() const { return led_; }

  // Control-rate work: the gain computer and coefficient maths. The only place
  // logf/expf/powf run — hence the low rate (kDynControlInterval).
  void updateControl(float dt) {
    if (mode == DYN_LIMITER) {
      float th, ra, mk;
      dynamicsLimitCurve(sAmount, th, ra, mk);
      attackSec_ = 0.0006f;  // fast — lookahead does the peak catching
      releaseUsed_ = clampf(sRelease, 0.030f, 0.400f);
      const float envDb = dynLin2Db(env_ + 1e-9f);
      const float grDb = dynamicsGrDb(envDb - th, ra, kDynKneeDb);
      targetGain_ = dynDb2Lin(grDb);
      makeupLin_ = dynDb2Lin(mk);
      lookLen_ = (int)clampf(0.001f / dt, 1.0f, (float)(kDynLookMax - 1));
      led_ = clampf(-grDb / 24.0f, 0.0f, 1.0f);
    } else if (mode == DYN_DUCKER) {
      duckDepth_ = clampf(sAmount, 0.0f, 1.0f) * 0.97f;
      attackSec_ = 0.003f;
      releaseUsed_ = clampf(sRelease, 0.030f, 1.0f);
      lookLen_ = 0;
      makeupLin_ = 1.0f;
      targetGain_ = 1.0f;
      // led_ tracked per sample from the duck envelope.
    } else {  // DYN_COMPRESSOR
      float th, ra, mk;
      dynamicsCompCurve(sAmount, th, ra, mk);
      releaseUsed_ = clampf(sRelease, 0.030f, 1.0f);
      attackSec_ = dynamicsAttackSec(releaseUsed_);
      const float envDb = dynLin2Db(env_ + 1e-9f);
      const float grDb = dynamicsGrDb(envDb - th, ra, kDynKneeDb);
      targetGain_ = dynDb2Lin(grDb);
      makeupLin_ = dynDb2Lin(mk);
      lookLen_ = 0;
      led_ = clampf(-grDb / 24.0f, 0.0f, 1.0f);
    }
    gAtkC_ = clampf(dt / attackSec_, 0.0f, 1.0f);
    gRelC_ = clampf(dt / releaseUsed_, 0.0f, 1.0f);
  }

  // Advance one sample of `dt` seconds and return the processed output. The
  // sidechain/ducker trigger comes from the platform via duckTrigger().
  float process(float in, float dt) {
    // --- smooth every user parameter (one-pole ~25 Hz, no zipper) ---------
    const float pc = onePoleCoef(25.0f, dt);
    sAmount += (amount - sAmount) * pc;
    sRelease += (releaseSec - sRelease) * pc;
    sDryMix += (dryMix - sDryMix) * pc;
    sTrim += (outTrim - sTrim) * pc;

    // --- lookahead ring: write current, read `lookLen_` samples behind ----
    look_[w_] = in;
    uint32_t rd = w_ + (uint32_t)kDynLookMax - (uint32_t)lookLen_;
    if (rd >= (uint32_t)kDynLookMax) rd -= (uint32_t)kDynLookMax;
    const float delayed = look_[rd];  // == in when lookLen_ == 0
    if (++w_ >= (uint32_t)kDynLookMax) w_ = 0;

    // --- detector: peak/RMS blend feeding an attack/release envelope ------
    const float rect = fabsf(in);
    sq_ += (in * in - sq_) * clampf(dt / 0.010f, 0.0f, 1.0f);  // ~10 ms RMS
    const float det = 0.5f * rect + 0.5f * sqrtf(sq_ + 1e-20f);
    if (det > env_)
      env_ += (det - env_) * gAtkC_;
    else
      env_ += (det - env_) * gRelC_;
    if (env_ < 1e-30f) env_ = 0.0f;  // denormal flush

    // --- control-rate gain computer (logf/expf live only here) ------------
    if (--ctl_ <= 0) {
      ctl_ = kDynControlInterval;
      updateControl(dt);
    }

    float processed;
    if (mode == DYN_DUCKER) {
      // Decaying trigger envelope attenuates the audio.
      duckEnv_ -= duckEnv_ * gRelC_;
      if (duckEnv_ < 1e-6f) duckEnv_ = 0.0f;
      const float g = 1.0f - duckDepth_ * duckEnv_;
      processed = delayed * g;
      led_ = duckDepth_ * duckEnv_;
    } else {
      // Smooth the gain toward the target (fast when reducing, slow when
      // recovering) — this is what keeps gain changes click-free.
      if (targetGain_ < gain_)
        gain_ += (targetGain_ - gain_) * gAtkC_;
      else
        gain_ += (targetGain_ - gain_) * gRelC_;
      processed = delayed * gain_ * makeupLin_;
    }

    // --- parallel/NY dry blend, then output trim --------------------------
    float out = processed + (in - processed) * sDryMix;
    out *= sTrim;

    // --- hard safety limiter: soft-saturate then clamp to +/-1 ------------
    out = softSat(out);
    return clampf(out, -1.0f, 1.0f);
  }
};

}  // namespace sc
