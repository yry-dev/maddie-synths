#pragma once

// Wavefolder — digital West-Coast (Serge / Buchla-style) folder core.
//
// Used by:
//   - firmwares/mod2-wavefolder/mod2-wavefolder.ino
//   - rack-plugins/src/mod2-wavefolder.cpp
//
// A wavefolder drives a signal hard into a bounded, oscillatory transfer
// function so the waveform "reflects" off the rails several times — turning a
// plain sine or triangle into a bright, harmonically dense timbre that sweeps
// as the fold amount rises. Three curves (BUTTON short-press cycles them):
// triangle Reflect (mirror at +/-1, sharp), Sine (Buchla-259-ish sinf folder,
// smooth) and Cascade (4 progressively-gained soft folds, Serge-ish, densest).
// A pre-fold symmetry/offset knob adds DC bias so the fold becomes asymmetric
// (even harmonics, a timbral "tilt"); a post-fold low-pass tames the top.
//
// Anti-aliasing is the real work: folding is brutally nonlinear at the MOD2's
// 36.6 kHz, so the folder runs 4x oversampled (linear-interp upsample -> fold
// -> 2-pole decimation LP), the same idea DistortionCore uses at 2x. A DC
// blocker removes the offset's static bias and an auto level-compensation keeps
// the output roughly constant as fold depth increases. Mode changes crossfade
// the two curves over a few ms so switching never clicks; every user parameter
// is one-pole smoothed so there is no zipper noise.
//
// No RAM: the folder is memoryless, so there is no init(arena,len) — reset()
// alone returns the state to power-on. Sample-rate independent: process(in, dt)
// advances by the caller's dt.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop.

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Fold curve (BUTTON short-press cycles these on hardware).
enum WavefolderMode : uint8_t {
  WAVEFOLDER_REFLECT = 0,  // hard triangle reflect at +/-1
  WAVEFOLDER_SINE = 1,     // sinf(drive*x) — Buchla 259-ish, smooth
  WAVEFOLDER_CASCADE = 2,  // 4 progressively-gained soft folds, Serge-ish
  WAVEFOLDER_MODE_COUNT = 3
};

// POT1 -> fold amount = pre-fold gain into the folder: 1x (pot=0) .. 20x
// (pot=1), exponential taper (each fold "octave" is roughly equal knob travel).
inline float wavefolderFoldGain(float pot01) {
  return powf(20.0f, clampf(pot01, 0.0f, 1.0f));
}

// POT2 -> symmetry / offset: a pre-gain DC bias in -0.5 .. +0.5 (pot=0.5 is
// symmetric). Because it is added before the fold gain it stays audible at any
// fold amount, tilting the harmonic balance toward even harmonics.
inline float wavefolderOffset(float pot01) {
  return clampf(pot01, 0.0f, 1.0f) - 0.5f;
}

// BUTTON+POT2 (shift) -> post-fold low-pass cutoff: 200 Hz (pot=0) .. ~18 kHz
// (pot=1), exponential. Default is wide open (pot=1) so the folder is bright.
inline float wavefolderToneHz(float pot01) {
  return 200.0f * powf(90.0f, clampf(pot01, 0.0f, 1.0f));
}

// Triangular reflect: identity for |x| <= 1, mirrors at the +/-1 rails
// (period-4 triangle of the input value). Same maths as DistortionCore's
// foldback, kept local so this core includes only sc_math.h / sc_dsp.h.
inline float wavefolderReflect(float x) {
  float t = 0.25f * x - 0.25f;
  t -= floorf(t);
  return 4.0f * fabsf(t - 0.5f) - 1.0f;
}

// The bounded fold transfer for a given curve. `d` is the already-driven,
// already-offset input; the result is always within roughly +/-1. Free function
// so the anti-alias oversampler (and the mode crossfade) can call it for any
// mode without touching engine state.
inline float wavefolderCurve(uint8_t mode, float d) {
  switch (mode) {
    case WAVEFOLDER_SINE:
      // Buchla-259-ish sine folder: as `d` grows the sine wraps repeatedly.
      return sinf(d);
    case WAVEFOLDER_CASCADE: {
      // Serge-ish cascade: four triangle folds, each re-gained so successive
      // stages keep folding (a single reflect is idempotent on [-1,1]).
      float y = d;
      for (int i = 0; i < 4; i++) y = wavefolderReflect(1.5f * y);
      return y;
    }
    case WAVEFOLDER_REFLECT:
    default:
      // Single hard triangle reflect at the +/-1 rails.
      return wavefolderReflect(d);
  }
}

struct WavefolderCore {
  // Parameters (write directly; see the mappers above).
  float foldGain = 1.0f;             // pre-fold gain (wavefolderFoldGain)
  float offset = 0.0f;               // pre-gain DC bias (wavefolderOffset)
  float toneHz = 18000.0f;           // post-fold low-pass cutoff
  float wet = 1.0f;                  // 0 dry .. 1 fully folded
  uint8_t mode = WAVEFOLDER_REFLECT; // WavefolderMode

  // Smoothed parameter shadows (one-pole, no zipper on the audio path).
  float foldGainS = 1.0f;
  float offsetS = 0.0f;
  float toneHzS = 18000.0f;
  float wetS = 1.0f;

  // State.
  float prevIn = 0.0f;    // last input, for the 4x linear upsample
  float osLp1 = 0.0f;     // 2-pole decimation low-pass (oversampled rate)
  float osLp2 = 0.0f;
  float toneLp = 0.0f;    // post-fold tone low-pass
  float ledEnv = 0.0f;    // smoothed fold-density for the panel LED
  DcBlocker dcBlock;      // strips the offset's static fold DC

  // Auto level compensation (cached; recomputed when drive/offset/mode move).
  float comp = 1.0f;
  float lastGainComp = -1.0f;
  float lastOffsetComp = 999.0f;
  uint8_t lastModeComp = 0xFF;

  // Mode-change crossfade: fold both the old and new curve for a few ms and
  // blend, so the transfer-function discontinuity never clicks.
  uint8_t prevMode = WAVEFOLDER_REFLECT;
  float modeXfade = 1.0f;  // 1 = settled on `mode`; 0 = just switched

  void reset() {
    foldGainS = foldGain;
    offsetS = offset;
    toneHzS = toneHz;
    wetS = wet;
    prevIn = 0.0f;
    osLp1 = 0.0f;
    osLp2 = 0.0f;
    toneLp = 0.0f;
    ledEnv = 0.0f;
    dcBlock = DcBlocker();
    comp = 1.0f;
    lastGainComp = -1.0f;
    lastOffsetComp = 999.0f;
    lastModeComp = 0xFF;
    prevMode = mode;
    modeXfade = 1.0f;
  }

  // Call when the mode changes (from the button handler) to start the crossfade.
  void setMode(uint8_t newMode) {
    if (newMode != mode) {
      prevMode = mode;
      mode = newMode;
      modeXfade = 0.0f;
    }
  }

  // 0..1 brightness for the panel LED — follows fold density (input energy
  // driven into the folder), smoothed so it glows rather than flickers.
  float ledLevel() const { return clampf(ledEnv, 0.0f, 1.0f); }

  // Auto output-level compensation: probe the current curve at a few reference
  // amplitudes and normalise its RMS response toward a nominal level so raising
  // the fold amount (or switching curves) doesn't jump the output volume.
  // Averaging several probes smooths the folder's oscillatory response so the
  // gain doesn't lurch when one probe happens to land on a fold null.
  void updateComp() {
    const float refs[3] = {0.2f, 0.4f, 0.6f};
    float acc = 0.0f;
    for (int i = 0; i < 3; i++) {
      const float y = wavefolderCurve(mode, foldGainS * (refs[i] + offsetS));
      acc += y * y;
    }
    const float outRms = sqrtf(acc * (1.0f / 3.0f));
    const float refRms = 0.4f;  // matches the mid probe
    comp = clampf(refRms / (outRms > 1e-4f ? outRms : 1e-4f), 0.25f, 4.0f);
  }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    // --- one-pole parameter smoothing (no zipper) ----------------------------
    const float pc = onePoleCoef(30.0f, dt);
    foldGainS += (foldGain - foldGainS) * pc;
    offsetS += (offset - offsetS) * pc;
    toneHzS += (toneHz - toneHzS) * pc;
    wetS += (wet - wetS) * pc;

    // --- refresh level compensation only when the shape actually moves -------
    if (fabsf(foldGainS - lastGainComp) > 0.01f ||
        fabsf(offsetS - lastOffsetComp) > 0.01f || mode != lastModeComp) {
      lastGainComp = foldGainS;
      lastOffsetComp = offsetS;
      lastModeComp = mode;
      updateComp();
    }

    // --- advance the mode crossfade -----------------------------------------
    const bool crossfading = modeXfade < 1.0f;
    if (crossfading) {
      modeXfade += dt / 0.005f;  // ~5 ms raised-cosine crossfade
      if (modeXfade > 1.0f) modeXfade = 1.0f;
    }
    const float blend = raisedCosine(modeXfade);  // 0 (old) .. 1 (new)

    // --- 4x oversampled folding ---------------------------------------------
    // Decimation LP cutoff ~0.45*fs at the oversampled rate; two cascaded
    // one-poles give ~12 dB/oct above Nyquist. onePoleCoef(0.45/dt, dt/4).
    const float osCoef = onePoleCoef(0.45f / dt, dt * 0.25f);
    float folded = 0.0f;
    for (int k = 0; k < 4; k++) {
      const float t = (float)(k + 1) * 0.25f;         // 0.25 .. 1.0
      const float x = prevIn + (in - prevIn) * t;     // linear upsample
      const float d = foldGainS * (x + offsetS);      // gain + pre-fold bias
      float y = wavefolderCurve(mode, d);
      if (crossfading) {
        const float yOld = wavefolderCurve(prevMode, d);
        y = yOld + (y - yOld) * blend;
      }
      // 2-pole decimation low-pass at the oversampled rate.
      osLp1 += (y - osLp1) * osCoef;
      osLp2 += (osLp1 - osLp2) * osCoef;
      folded = osLp2;  // decimate by keeping the last filtered subsample
    }
    prevIn = in;

    folded *= comp;                    // auto level compensation
    folded = dcBlock.process(folded);  // remove the offset's static DC

    // --- post-fold tone low-pass (BUTTON+POT2 on hardware) -------------------
    toneLp += (folded - toneLp) * onePoleCoef(toneHzS, dt);
    float wetSig = toneLp;

    // --- LED: smoothed fold density -----------------------------------------
    ledEnv += (fabsf(wetSig) - ledEnv) * onePoleCoef(20.0f, dt);

    // --- wet/dry mix + safety clamp -----------------------------------------
    float out = in + (wetSig - in) * wetS;
    return clampf(out, -1.2f, 1.2f);
  }
};

}  // namespace sc
