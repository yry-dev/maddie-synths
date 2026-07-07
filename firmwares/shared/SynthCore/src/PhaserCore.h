#pragma once

// Phaser — 4/6/8-stage allpass phaser core.
//
// Used by:
//   - firmwares/mod2-phaser/mod2-phaser.ino
//   - rack-plugins/src/mod2-phaser.cpp
//
// A chain of first-order allpass sections (y = -a*x + x1 + a*y1) swept by a
// sine-shaped LFO. The allpass corner sweeps exponentially around ~630 Hz,
// spanning up to 100 Hz - 4 kHz at full depth, with a slight per-stage corner
// detune for a lusher notch spread. Global feedback (resonance) runs from the
// last stage back to the input, DC-blocked and soft-saturated. Short button
// presses cycle the stage count 4 / 6 / 8. Full-CCW rate = manual mode: the
// sweep position is set by hand (manualPos). Classic phaser null at wet=0.5.
// The LED follows the LFO (ledLevel()).
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop.

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Stage count selector (BUTTON short-press cycles these on hardware).
enum PhaserStages : uint8_t {
  PHASER_4 = 0,
  PHASER_6 = 1,
  PHASER_8 = 2,
  PHASER_STAGES_COUNT = 3
};

constexpr int kPhaserMaxStages = 8;

// POT1 -> LFO rate in Hz: exponential taper 0.02 Hz (pot=0) .. 8 Hz (pot=1).
inline float phaserRateHz(float pot01) {
  return 0.02f * powf(400.0f, clampf(pot01, 0.0f, 1.0f));
}

// Full-CCW rate pot = manual mode (sweep by hand instead of the LFO).
inline bool phaserManual(float pot01) {
  return pot01 < 0.03f;
}

// POT2 -> feedback / resonance: 0 .. 0.92 (notch depth -> vowely peaks).
inline float phaserFeedback(float pot01) {
  return 0.92f * clampf(pot01, 0.0f, 1.0f);
}

struct PhaserCore {
  // Parameters (write directly; see the mappers above).
  float rateHz = 0.4f;      // LFO rate (ignored in manual mode)
  bool manual = false;      // true: sweep position comes from manualPos
  float manualPos = 0.5f;   // 0..1 hand sweep (manual mode)
  float feedback = 0.0f;    // 0..~0.92 resonance
  float depth = 0.8f;       // 0..1 sweep range around the centre
  float wet = 0.5f;         // 0 dry .. 1 fully wet (0.5 = classic null)
  uint8_t stageSel = PHASER_4;

  // State.
  float phase = 0.0f;                  // LFO phase 0..1
  float x1[kPhaserMaxStages] = {0};    // allpass input memories
  float y1[kPhaserMaxStages] = {0};    // allpass output memories
  float fbSample = 0.0f;               // last-stage output, fed back
  float lastLfo = 0.5f;                // 0..1 LFO position (for the LED)
  DcBlocker fbDc;

  void reset() {
    phase = 0.0f;
    for (int i = 0; i < kPhaserMaxStages; i++) x1[i] = y1[i] = 0.0f;
    fbSample = 0.0f;
    lastLfo = 0.5f;
    fbDc = DcBlocker();
  }

  // IN1 rising edge: restart the LFO sweep.
  void retrigger() { phase = 0.0f; }

  int stages() const { return 4 + 2 * (int)stageSel; }

  // 0..1 brightness for the panel LED — follows the LFO.
  float ledLevel() const { return lastLfo; }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    // Sweep position -1..+1 (sine-shaped LFO, or the manual knob).
    float lfo;
    if (manual) {
      lfo = manualPos * 2.0f - 1.0f;
      lastLfo = manualPos;
    } else {
      phase += rateHz * dt;
      phase -= floorf(phase);
      lfo = sinf(kTwoPi * phase);
      lastLfo = 0.5f + 0.5f * lfo;
    }

    // Allpass corner: exponential around the ~630 Hz geometric centre of the
    // 100 Hz - 4 kHz span; depth scales the excursion (log2(4000/100)/2 oct).
    const float fc = 632.0f * powf(2.0f, lfo * depth * 2.66f);

    // Bilinear-warped coefficient. The per-stage detune multiplies the warp
    // g = tan(pi*fc*dt) directly — g is near-linear in fc for these corners,
    // so a g-multiplier is an accurate small frequency offset without one
    // tan() per stage.
    const float g = tanf(kPi * clampf(fc * dt, 0.0002f, 0.45f));

    // Feedback (resonance) from the last stage, DC-blocked and limited.
    float x = in + feedback * softSat(fbDc.process(fbSample));

    static const float kDetune[kPhaserMaxStages] = {
        1.00f, 1.07f, 0.94f, 1.13f, 0.89f, 1.19f, 0.84f, 1.25f};
    const int n = stages();
    for (int i = 0; i < n; i++) {
      const float gi = g * kDetune[i];
      const float a = (1.0f - gi) / (1.0f + gi);
      const float y = -a * x + x1[i] + a * y1[i];
      x1[i] = x;
      y1[i] = y;
      x = y;
    }
    fbSample = x;

    // wet=0.5 sums dry and chain equally — the classic notch null.
    return in + (x - in) * wet;
  }
};

}  // namespace sc
