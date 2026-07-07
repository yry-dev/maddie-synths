#pragma once

// Resonator — Rings-like tuned resonator bank core.
//
// Used by:
//   - firmwares/mod2-resonator/mod2-resonator.ino
//   - rack-plugins/src/mod2-resonator.cpp
//
// External audio (or a gate-triggered internal noise-burst exciter) excites a
// bank of tuned resonators. Three modes:
//   MODAL       — 12 two-pole band-pass resonators (sc::Biquad, stable under
//                 sweeps) at partial ratios that morph harmonic -> piano ->
//                 marimba -> bell with the structure control (the same 4-zone
//                 morph as FluxCore's modal voice); damping sets the Q.
//   COMB        — 4 tuned feedback combs; structure spreads their tuning from
//                 a near-unison cluster out to a root/fifth/octave/twelfth
//                 chord; damping sets the feedback.
//   SYMPATHETIC — 1 bright driven Karplus string + 4 quieter strings at
//                 fifths/octaves that ring in sympathy (the input feeds them
//                 at low gain); structure spreads their detune.
// strike() fires the internal noise-burst exciter (IN1 trigger / long press
// on hardware); the damp gate (IN2) chokes the bank while high. Pitch is
// expected semitone-quantized — use resonatorPitchHz().
//
// The comb/string delay memory is a caller-provided int16 arena split into
// kResonatorStrings equal segments — static SRAM in firmware, a std::vector
// sized per engine rate in Rack (resonatorArenaSamples()).
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop.

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Resonator flavours (BUTTON short-press cycles these on hardware).
enum ResonatorMode : uint8_t {
  RESONATOR_MODAL = 0,
  RESONATOR_COMB = 1,
  RESONATOR_SYMPATHETIC = 2,
  RESONATOR_MODE_COUNT = 3
};

constexpr int kResonatorModes = 12;   // modal band-pass bank size
constexpr int kResonatorCombs = 4;    // comb-cluster voices
constexpr int kResonatorStrings = 5;  // sympathetic strings (1 main + 4)

// Pitch floor: A1. POT1 quantizes to semitones over 4 octaves (A1..A5) —
// it's a melodic module.
constexpr float kResonatorBaseHz = 55.0f;

inline float resonatorPitchHz(float pot01) {
  const float semis = floorf(clampf(pot01, 0.0f, 1.0f) * 48.0f + 0.5f);
  return kResonatorBaseHz * powf(2.0f, semis * (1.0f / 12.0f));
}

// Arena size for `fsHz`: kResonatorStrings segments, each long enough for a
// full period at the lowest pitch.
inline uint32_t resonatorArenaSamples(float fsHz) {
  return (uint32_t)kResonatorStrings * ((uint32_t)(fsHz / 50.0f) + 8);
}

struct ResonatorCore {
  // Parameters (write directly; see the mappers above).
  float pitchHz = 220.0f;              // fundamental (semitone-quantized)
  float structure = 0.5f;              // partial / detune spread
  float damping = 0.5f;                // decay time (0 short .. 1 long)
  float wet = 0.5f;                    // 0 dry .. 1 fully wet
  uint8_t mode = RESONATOR_MODAL;      // ResonatorMode
  bool dampGate = false;               // choke the bank while true

  // State.
  Biquad modal[kResonatorModes];
  bool modalActive[kResonatorModes] = {};
  float modalInScale = 1.0f;
  DelayLine seg[kResonatorStrings];    // comb/string delay segments
  float loopLp[kResonatorStrings] = {};  // in-loop damping filters
  float exciteEnv = 0.0f;              // strike noise-burst envelope
  uint32_t rng = 0x52534E31u;          // "RSN1"
  float energy = 0.0f;                 // smoothed |wet| for the LED

  // Cached per-parameter work (recomputed only when the keys change).
  float lastPitch = -1.0f, lastStructure = -1.0f, lastDamping = -1.0f;
  float lastDt = -1.0f;
  uint8_t lastMode = 0xFF;
  float exciteCoef = 0.99f;            // per-sample burst decay
  float chokeCoef = 0.995f;            // per-sample state decay while damped

  // `buf`/`n`: caller-owned arena (resonatorArenaSamples() long), split into
  // kResonatorStrings equal delay segments.
  void init(int16_t* buf, uint32_t n) {
    const uint32_t segLen = n / (uint32_t)kResonatorStrings;
    for (int i = 0; i < kResonatorStrings; i++)
      seg[i].init(buf + (uint32_t)i * segLen, segLen);
    reset();
  }

  void reset() {
    for (int i = 0; i < kResonatorModes; i++) modal[i].reset();
    for (int i = 0; i < kResonatorStrings; i++) {
      seg[i].clear();
      loopLp[i] = 0.0f;
    }
    exciteEnv = 0.0f;
    energy = 0.0f;
    lastPitch = -1.0f;  // force a coefficient rebuild
    lastDt = -1.0f;
  }

  // Fire the internal noise-burst exciter (IN1 trigger / long press).
  void strike() { exciteEnv = 1.0f; }

  // 0..1 brightness for the panel LED — follows the bank's energy.
  float ledLevel() const { return clampf(energy * 3.0f, 0.0f, 1.0f); }

  // Partial ratio for modal mode `i` — FluxCore's 4-zone structure morph
  // (harmonic -> piano -> marimba -> bell).
  float modalRatio(int i) const {
    const float n = (float)(i + 1);
    if (structure < 0.25f) return n;
    if (structure < 0.5f) {
      const float inh = (structure - 0.25f) * 4.0f * 0.001f;
      return n * (1.0f + inh * n * n);
    }
    if (structure < 0.75f) {
      static const float marimba[kResonatorModes] = {
          1.0f, 2.76f, 5.4f, 8.93f, 13.34f, 18.64f,
          24.82f, 31.87f, 39.81f, 48.62f, 58.31f, 68.88f};
      const float blend = (structure - 0.5f) * 4.0f;
      return n * (1.0f - blend) + marimba[i] * blend;
    }
    static const float bell[kResonatorModes] = {
        1.0f, 1.88f, 2.83f, 3.76f, 4.67f, 5.52f,
        6.35f, 7.15f, 7.93f, 8.69f, 9.43f, 10.15f};
    return bell[i];
  }

  // Comb tuning ratio: structure spreads a near-unison cluster (0) out to a
  // root/fifth/octave/twelfth chord and beyond (1).
  float combRatio(int i) const {
    static const float chord[kResonatorCombs] = {1.0f, 1.5f, 2.0f, 3.0f};
    return 1.0f + (chord[i] - 1.0f) * (0.1f + 1.4f * structure);
  }

  // Sympathetic string ratio for strings 1..4 (0 is the driven string at the
  // fundamental): fifths/octaves, detuned up to ~half a semitone by structure.
  float stringRatio(int i) const {
    static const float base[kResonatorStrings] = {1.0f, 1.5f, 2.0f, 3.0f, 4.0f};
    static const float detSemis[kResonatorStrings] = {0.0f, -0.5f, 0.35f, -0.25f, 0.45f};
    return base[i] * powf(2.0f, detSemis[i] * structure * (1.0f / 12.0f));
  }

  void updateCoefficients(float dt) {
    const float fs = 1.0f / dt;
    if (dt != lastDt) {
      exciteCoef = expf(-dt / 0.004f);  // ~4 ms noise burst
      chokeCoef = expf(-60.0f * dt);    // fast ring-down while damp-gated
    }
    if (mode == RESONATOR_MODAL) {
      // The constant-skirt band-pass peaks at gain Q but its impulse response
      // starts at a Q-independent amplitude, so strikes go in unscaled and
      // ring naturally; the steady audio input is attenuated by ~1/Q instead
      // so a sustained tone at a resonance settles near unity, not at Q.
      const float q = 40.0f * powf(50.0f, damping);
      modalInScale = 4.0f / q;
      for (int i = 0; i < kResonatorModes; i++) {
        const float fc = pitchHz * modalRatio(i);
        modalActive[i] = fc < fs * 0.45f;
        if (modalActive[i]) modal[i].setBandpass(fc, q, fs);
      }
    }
    lastPitch = pitchHz;
    lastStructure = structure;
    lastDamping = damping;
    lastMode = mode;
    lastDt = dt;
  }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    if (pitchHz != lastPitch || structure != lastStructure ||
        damping != lastDamping || mode != lastMode || dt != lastDt)
      updateCoefficients(dt);

    const float fs = 1.0f / dt;

    // Internal exciter: a short noise burst rings the bank with nothing
    // patched into the audio input.
    float burst = 0.0f;
    if (exciteEnv > 0.001f) {
      burst = noise1f(rng) * exciteEnv;
      exciteEnv *= exciteCoef;
    }
    const float drive = in + burst;

    float wetSig = 0.0f;
    switch (mode) {
      case RESONATOR_COMB: {
        for (int i = 0; i < kResonatorCombs; i++) {
          const float delay = clampf(fs / (pitchHz * combRatio(i)), 2.0f,
                                     (float)(seg[i].len - 2));
          const float echo = seg[i].read(delay);
          loopLp[i] += (echo - loopLp[i]) * onePoleCoef(3500.0f, dt);
          float fb = 0.86f + 0.135f * damping;
          if (dampGate) fb *= 0.4f;
          seg[i].write(softSat(drive + loopLp[i] * fb));
          wetSig += echo;
        }
        wetSig *= (1.5f / (float)kResonatorCombs);
        break;
      }
      case RESONATOR_SYMPATHETIC: {
        float sympSum = 0.0f, mainOut = 0.0f;
        for (int i = 0; i < kResonatorStrings; i++) {
          const float delay = clampf(fs / (pitchHz * stringRatio(i)), 2.0f,
                                     (float)(seg[i].len - 2));
          const float out = seg[i].read(delay);
          // In-loop damping filter (string brightness) + feedback.
          loopLp[i] += (out - loopLp[i]) * onePoleCoef(3000.0f, dt);
          float fb = 0.95f + 0.0495f * damping;
          if (dampGate) fb *= 0.3f;
          // The driven string takes the input full-strength; the sympathetic
          // strings hear it (and each strike) quietly.
          const float inGain = (i == 0) ? 0.8f : 0.12f;
          seg[i].write(softSat(drive * inGain + loopLp[i] * fb));
          if (i == 0) mainOut = out;
          else sympSum += out;
        }
        wetSig = mainOut * 0.9f + sympSum * 0.3f;
        break;
      }
      case RESONATOR_MODAL:
      default: {
        const float bankIn = in * modalInScale + burst;
        for (int i = 0; i < kResonatorModes; i++) {
          if (!modalActive[i]) continue;
          if (dampGate) {  // bleed the filter state for a fast ring-down
            modal[i].y1 *= chokeCoef;
            modal[i].y2 *= chokeCoef;
          }
          const float g = 1.0f / (1.0f + 0.25f * (float)i);
          wetSig += modal[i].process(bankIn * g) * g;
        }
        wetSig *= 0.7f;
        break;
      }
    }

    wetSig = softSat(wetSig);
    energy += (fabsf(wetSig) - energy) * onePoleCoef(6.0f, dt);

    return in + (wetSig - in) * wet;
  }
};

}  // namespace sc
