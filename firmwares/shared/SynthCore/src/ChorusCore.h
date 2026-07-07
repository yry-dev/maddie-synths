#pragma once

// Chorus — Juno-style chorus + string-machine ensemble core.
//
// Used by:
//   - firmwares/mod2-chorus/mod2-chorus.ino
//   - rack-plugins/src/mod2-chorus.cpp
//
// Classic modulated short-delay thickening, mono-in mono-out (the Juno's
// stereo spread is faked by summing two anti-phase-modulated voices). Three
// modes: Chorus I / Chorus II (2 fractional taps on one short delay line,
// modulated by a single sine-shaped triangle LFO in anti-phase; II is faster
// and deeper) and Ensemble (3 taps, 3 phase-offset LFOs, each the classic
// slow + fast two-component recipe). A gentle high-pass in the wet path keeps
// the low end mono-solid. The LED breathes with the main LFO (ledLevel()).
//
// The delay memory is a caller-provided int16 arena over sc::DelayLine —
// static SRAM in firmware, a std::vector sized per engine rate in Rack. Size
// it for kChorusBufferSec of audio (chorusArenaSamples()).
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop.

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Voicing (BUTTON short-press cycles these on hardware).
enum ChorusMode : uint8_t {
  CHORUS_I = 0,         // Juno chorus, slow and subtle
  CHORUS_II = 1,        // faster / deeper
  CHORUS_ENSEMBLE = 2,  // string-machine triple tap
  CHORUS_MODE_COUNT = 3
};

// POT1 -> LFO rate in Hz: exponential taper 0.1 Hz (pot=0) .. 8 Hz (pot=1).
inline float chorusRateHz(float pot01) {
  return 0.1f * powf(80.0f, clampf(pot01, 0.0f, 1.0f));
}

// Longest delay excursion the taps can reach; size the arena from this.
constexpr float kChorusBufferSec = 0.020f;

inline uint32_t chorusArenaSamples(float fsHz) {
  return (uint32_t)(kChorusBufferSec * fsHz) + 16;
}

// Sine-shaped triangle in -1..+1 from a 0..1 phase (soft cubic rounding of
// the triangle corners — the "slightly sine-shaped" Juno LFO).
inline float chorusLfo(float phase01) {
  phase01 -= floorf(phase01);
  const float tri = 4.0f * fabsf(phase01 - 0.5f) - 1.0f;
  return tri * (1.5f - 0.5f * tri * tri);
}

struct ChorusCore {
  // Parameters (write directly; see the mappers above).
  float rateHz = 0.5f;           // main LFO rate
  float depth = 0.5f;            // 0..1 modulation excursion
  float wet = 0.5f;              // 0 dry .. 1 fully wet
  uint8_t mode = CHORUS_I;       // ChorusMode

  // State.
  DelayLine line;
  float phase = 0.0f;      // main (slow) LFO, 0..1
  float phaseFast = 0.0f;  // ensemble fast component, 0..1
  float wetHpLp = 0.0f;    // wet-path high-pass (one-pole LP subtracted)

  // `buf`/`n`: caller-owned arena (chorusArenaSamples() long).
  void init(int16_t* buf, uint32_t n) {
    line.init(buf, n);
    reset();
  }

  void reset() {
    line.clear();
    phase = 0.0f;
    phaseFast = 0.0f;
    wetHpLp = 0.0f;
  }

  // 0..1 brightness for the panel LED — breathes with the main LFO.
  float ledLevel() const {
    return 0.5f + 0.5f * chorusLfo(phase);
  }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    line.write(in);

    // Chorus II runs its one LFO faster and deeper than I; Ensemble keeps
    // the slow rate and adds the classic ~10x fast shimmer component.
    const float rateMul = (mode == CHORUS_II) ? 1.6f : 1.0f;
    phase += rateHz * rateMul * dt;
    phase -= floorf(phase);
    phaseFast += rateHz * 10.0f * dt;
    phaseFast -= floorf(phaseFast);

    float wetSig = 0.0f;
    if (mode == CHORUS_ENSEMBLE) {
      // 3 taps, 3 phase-offset (slow + fast) LFOs.
      for (int i = 0; i < 3; i++) {
        const float off = (float)i * (1.0f / 3.0f);
        const float lfo = 0.7f * chorusLfo(phase + off) +
                          0.3f * chorusLfo(phaseFast + off);
        const float delayMs = 5.0f + 3.5f * depth * lfo;
        wetSig += line.read(clampf(delayMs * 0.001f / dt, 1.0f, 1e9f));
      }
      wetSig *= (1.0f / 3.0f);
    } else {
      // Juno pair: 2 taps modulated in anti-phase by the one LFO.
      const float lfo = chorusLfo(phase);
      const float baseMs = (mode == CHORUS_II) ? 3.0f : 4.0f;
      const float depthMs = ((mode == CHORUS_II) ? 2.5f : 1.6f) * depth;
      const float dA = clampf((baseMs + depthMs * lfo) * 0.001f / dt, 1.0f, 1e9f);
      const float dB = clampf((baseMs - depthMs * lfo) * 0.001f / dt, 1.0f, 1e9f);
      wetSig = 0.5f * (line.read(dA) + line.read(dB));
    }

    // Gentle ~150 Hz high-pass on the wet path only.
    wetHpLp += (wetSig - wetHpLp) * onePoleCoef(150.0f, dt);
    wetSig -= wetHpLp;

    return in + (wetSig - in) * wet;
  }
};

}  // namespace sc
