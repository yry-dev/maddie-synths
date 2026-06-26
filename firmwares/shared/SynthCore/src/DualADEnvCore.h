#pragma once

// Dual attack-decay envelope voice — shared core of the DualADEnv module.
//
// Used by:
//   - firmwares/mod1-dual-ad-env/mod1-dual-ad-env.ino
//   - rack-plugins/src/DualADEnv.cpp
//
// The original firmware used two 1024-entry PROGMEM tables (kEnvelopeCurve and
// kEnvelopePotAdjust from Mod1EnvelopeData). These are replaced here with
// closed-form expressions, which avoids any RAM pressure on the AVR:
//
//   kEnvelopeCurve (uint8_t[1024] PROGMEM):
//     The table decays from 255 at i=0 to 0 near i=1023 in a shape that
//     halves roughly every 170 steps (~6 halvings over 1024 steps).
//     Closed-form equivalent: expf(-4.16 * phase) where phase = i/1023.
//
//   kEnvelopePotAdjust (int[1024] PROGMEM):
//     Maps ADC value 0..1023 to 0..1023, used as:
//       attackTime = kEnvelopeTableSize - kEnvelopePotAdjust[pot]
//     producing 1..1024 (attack steps), driven at 1 ms/step with
//     kEnvelopeStepScale=0.025: duration = 1024/(0.025*attackTime) ms = 40960/D,
//     i.e. ~40 ms (pot=0) to ~41 s (pot=1023). Reproduced by the shared
//     sc::envPotTimeSec() compander (33-point log-interp table, see sc_math.h) —
//     the same map EG uses. A plain exponential fit was ~2.5x off mid-dial.
//
// Both attack and release are driven by caller-supplied dt (seconds) so they
// are sample-rate independent. VCV Rack passes args.sampleTime; the firmware
// passes the elapsed milliseconds converted to seconds.
//
// Pure C++: depends only on sc_math.h. No Arduino / Rack / Pico SDK.

#include "sc_math.h"

namespace sc {

// Map normalised pot value (0..1) to attack/release time in seconds, via the
// shared compander (= 40960/D ms). Matches the firmware's kEnvelopePotAdjust
// table to within a few % across the musical range — the same map EG uses.
inline float adEnvMapTime(float pot01) {
  return envPotTimeSec(pot01);
}

// Compute the variation scale for a given time-pot position.
// Mirrors the firmware's `(1 - timePot) * 0.6 + 0.2` expression.
// Shorter times (lower pot) give a wider variation range (0.2..0.8).
inline float adEnvVarScale(float timePot01) {
  return (1.0f - clampf(timePot01, 0.f, 1.f)) * 0.6f + 0.2f;
}

// Apply a random deviation to a base time in seconds.
// `deviation` is caller-supplied in -1..+1 (platform provides randomness).
// Mirrors: baseTime + random(-1,1) * baseTime * varAmount
inline float adEnvApplyVariation(float baseTime, float deviation, float varAmount) {
  return baseTime * (1.0f + deviation * varAmount);
}

// Single attack-decay envelope voice. Phase-based, dt-driven (seconds).
//
// Envelope shape (matches kEnvelopeCurve, see header comment):
//   Attack:  output rises from startLevel to 1.0 via 1 - exp(-4.16*phase)
//            (concave: fast initial rise, slow approach to peak)
//   Release: output falls from 1.0 to 0 via exp(-4.16*phase)
//            (convex: fast initial fall, slow tail)
//
// Re-trigger: if fired during release the attack starts from the current
// output level, matching the firmware's lastOutput logic.
struct ADEnvVoice {
  enum State : uint8_t { kIdle = 0, kAttack = 1, kRelease = 2 };

  State state      = kIdle;
  float phase      = 0.f;   // 0..1 within the current segment
  float startLevel = 0.f;   // output level at attack onset (re-trigger support)
  float output     = 0.f;   // current output 0..1

  void reset() {
    state      = kIdle;
    phase      = 0.f;
    startLevel = 0.f;
    output     = 0.f;
    _atkTime   = 0.1f;
    _relTime   = 0.5f;
  }

  // Begin a new cycle with the given attack and release durations (seconds).
  void trigger(float atkSeconds, float relSeconds) {
    _atkTime   = atkSeconds > 0.f ? atkSeconds : 0.001f;
    _relTime   = relSeconds > 0.f ? relSeconds : 0.001f;
    startLevel = (state == kRelease) ? output : 0.f;
    state      = kAttack;
    phase      = 0.f;
  }

  // Advance by dt seconds, update output (0..1), and return it.
  float process(float dt) {
    if (state == kAttack) {
      float t = 1.0f - expf(-4.16f * phase);
      output   = startLevel + (1.0f - startLevel) * t;
      phase   += dt / _atkTime;
      if (phase >= 1.0f) {
        output = 1.0f;
        phase  = 0.f;
        state  = kRelease;
      }
    } else if (state == kRelease) {
      output  = expf(-4.16f * phase);
      phase  += dt / _relTime;
      if (phase >= 1.0f) {
        output = 0.f;
        phase  = 0.f;
        state  = kIdle;
      }
    }
    return output;
  }

 private:
  float _atkTime = 0.1f;
  float _relTime = 0.5f;
};

}  // namespace sc
