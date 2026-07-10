#pragma once

// Distortion — multi-algorithm drive core.
//
// Used by:
//   - firmwares/mod2-distortion/mod2-distortion.ino
//   - rack-plugins/src/mod2-distortion.cpp
//
// Five drive flavours behind one mode switch: soft saturation (tanh), hard
// clip, tube (asymmetric transfer, even harmonics, mild bias shift with
// drive), foldback (reflect-at-threshold) and fuzz (heavy asymmetric gain
// with a gate-y low-level cutoff driven by an input envelope follower).
// The shaper runs 2x oversampled (linear-interp upsample -> shape -> average
// decimate) because hard clip / foldback alias badly at the MOD2's 36.6 kHz.
// Output level is auto-compensated per algorithm/drive so switching modes
// doesn't jump volume, then a one-pole tilt EQ (dark <-> bright) and a DC
// blocker (the asymmetric shapers generate DC) finish the wet path.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop.

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Drive algorithms (BUTTON short-press cycles these on hardware).
enum DistortionMode : uint8_t {
  DISTORTION_SOFT = 0,  // tanh, gentle knee
  DISTORTION_HARD = 1,  // straight clip
  DISTORTION_TUBE = 2,  // asymmetric transfer, even harmonics
  DISTORTION_FOLD = 3,  // reflect-at-threshold foldback
  DISTORTION_FUZZ = 4,  // heavy asymmetric gain + gated low end
  DISTORTION_MODE_COUNT = 5
};

// POT1 -> pre-shaper gain: exponential taper 1x (pot=0) .. ~50x (pot=1).
inline float distortionDriveGain(float pot01) {
  return powf(10.0f, 1.7f * clampf(pot01, 0.0f, 1.0f));
}

// Triangular foldback: identity for |x| <= 1, reflects at the +/-1 rails
// (period-4 triangle of the input value).
inline float distortionFold(float x) {
  float t = 0.25f * x + 0.25f;
  t -= floorf(t);
  return 4.0f * fabsf(t - 0.5f) - 1.0f;
}

struct DistortionCore {
  // Parameters (write directly; see the mappers above).
  float drive = 1.0f;                  // pre-shaper gain (distortionDriveGain)
  float tone = 0.5f;                   // 0 dark .. 1 bright (0.5 = flat)
  float wet = 1.0f;                    // 0 dry .. 1 fully driven
  uint8_t mode = DISTORTION_SOFT;      // DistortionMode

  // State.
  float prevIn = 0.0f;      // last input, for the 2x linear upsample
  float tiltLp = 0.0f;      // tilt EQ low band state
  float gateEnv = 0.0f;     // fuzz gate input-envelope follower
  float comp = 1.0f;        // cached auto level compensation
  float lastDrive = -1.0f;  // cache keys for comp
  uint8_t lastMode = 0xFF;
  DcBlocker dcBlock;        // strips shaper DC (tube/fuzz are asymmetric)

  void reset() {
    prevIn = 0.0f;
    tiltLp = 0.0f;
    gateEnv = 0.0f;
    lastDrive = -1.0f;
    lastMode = 0xFF;
    dcBlock = DcBlocker();
  }

  // The raw transfer function for the current mode. Input has the drive gain
  // already applied.
  float shape(float x) const {
    switch (mode) {
      case DISTORTION_HARD:
        return clampf(1.2f * x, -1.0f, 1.0f);
      case DISTORTION_TUBE: {
        // Asymmetric knee: a bias that grows with drive shifts the operating
        // point (even harmonics); the static offset is removed afterwards.
        const float bias = 0.25f + 0.10f * clampf(drive * 0.05f, 0.0f, 1.0f);
        return tanhf(x + bias) - tanhf(bias);
      }
      case DISTORTION_FOLD:
        return distortionFold(x);
      case DISTORTION_FUZZ:
        // Heavy asymmetric exponential clipper (transistor-fuzz flavour).
        return x >= 0.0f ? 1.0f - expf(-2.0f * x)
                         : -0.8f * (1.0f - expf(1.5f * x));
      case DISTORTION_SOFT:
      default:
        return tanhf(x);
    }
  }

  // Auto output-level compensation: normalise the shaper's response to a
  // nominal 0.4 input so mode/drive changes don't jump volume. Clamped —
  // foldback can null the probe amplitude and would otherwise explode.
  void updateComp() {
    const float ref = 0.4f;
    const float y = fabsf(shape(drive * ref));
    comp = clampf(ref / (y > 1e-4f ? y : 1e-4f), 0.25f, 6.0f);
  }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    if (drive != lastDrive || mode != lastMode) {
      lastDrive = drive;
      lastMode = mode;
      updateComp();
    }

    // 2x oversampled shaping: shape the linear-interp midpoint and the new
    // sample, then average (box decimator) to knock down the fold-back images.
    const float mid = 0.5f * (prevIn + in);
    prevIn = in;
    float y = 0.5f * (shape(drive * mid) + shape(drive * in));
    y *= comp;

    // Fuzz gate: a slow input envelope chokes the output below the threshold
    // ("dying battery" sputter).
    if (mode == DISTORTION_FUZZ) {
      gateEnv += (fabsf(in) - gateEnv) * onePoleCoef(20.0f, dt);
      const float g = clampf(gateEnv * (1.0f / 0.03f), 0.0f, 1.0f);
      y *= g * g;
    }

    // Tilt EQ around ~1.2 kHz: tone 0 keeps only the low band, 1 only the
    // high band, 0.5 is exactly flat.
    tiltLp += (y - tiltLp) * onePoleCoef(1200.0f, dt);
    y = 2.0f * ((1.0f - tone) * tiltLp + tone * (y - tiltLp));

    y = dcBlock.process(y);
    y = clampf(y, -1.2f, 1.2f);

    return in + (y - in) * wet;
  }
};

}  // namespace sc
