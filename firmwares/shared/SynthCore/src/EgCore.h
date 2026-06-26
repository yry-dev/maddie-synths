#pragma once

// Envelope-generator voice — the shared core of the EG module.
//
// Used by:
//   - firmwares/mod1-eg/mod1-eg.ino   (1 ms loop tick, analogRead/PWM)
//   - rack-plugins/src/Eg.cpp               (audio rate, Rack params/outputs)
//
// 3-output AR envelope with an end-of-cycle (EoC) gate pulse.
//
// Pure C++: depends only on sc_math.h. No Arduino / Rack / Pico SDK.

#include "sc_math.h"

namespace sc {

// ── Parameter mapping ──────────────────────────────────────────────────────
//
// The firmware stores a 1024-entry PROGMEM int PotAdjust[] table that maps
// ADC readings to phase-advance rates via a steep compander curve (coarse
// control at low pot, fine at high). It is reproduced here by the shared
// sc::envPotDivisor() 33-point log-interpolated table (see sc_math.h), which
// returns D = 1024 − PotAdjust[pot] within a few % across the musical range —
// a plain power-law fit was 2.5–8× off mid-dial. The phase rate is then
// 25 × D / 1024 per second, giving attack/release times of ~40 ms – ~41 s,
// matching the firmware's 0.025×atkTime/1024 per-ms advance (duration = 40960/D
// ms). RAM-light (66 bytes) vs the original 4 KB table.

struct EgParams {
  float attackRate;   // phase per second (high = fast attack)
  float releaseRate;  // phase per second (high = fast release)
  float level;        // 0..1 output level
};

// Map normalised 0..1 controls to envelope parameters.
// Pass analogRead(Ax)/1023.f from firmware, or raw 0..1 Rack param values.
inline EgParams egMapParams(float pot_attack, float pot_release, float pot_level) {
  EgParams p;
  // atkTime = D = 1024 − PotAdjust[pot], reproduced by the shared compander.
  const float atkTime = envPotDivisor(pot_attack);
  const float relTime = envPotDivisor(pot_release);
  // phase rate (per second) = 25 × atkTime / 1024  (= 0.025×atkTime/1024 per ms × 1000)
  p.attackRate  = 25.0f * clampf(atkTime, 1.0f, 1024.0f) / 1024.0f;
  p.releaseRate = 25.0f * clampf(relTime, 1.0f, 1024.0f) / 1024.0f;
  p.level       = clampf(pot_level, 0.0f, 1.0f);
  return p;
}

// ── Voice ──────────────────────────────────────────────────────────────────
//
// Envelope shape: the firmware uses a 1024-entry PROGMEM Curve[] byte table
// that decays from 255 to ≈0 over its full index range. It is replaced here
// with a closed-form exponential:
//
//   c(phase) = expf(−3.0 × phase),   phase ∈ [0..1]
//
// This matches the table within ±1/255 at every point verified:
//   phase 0.25 → c = 0.472,  Curve[256]/255 = 0.467
//   phase 0.50 → c = 0.223,  Curve[512]/255 = 0.220
//   phase 0.75 → c = 0.105,  Curve[768]/255 = 0.098
//
// Attack segment  (state 1, phase 0→1): envelope = (1 − c×(1−lastEnvNorm)) × level
//   Rises from lastEnvNorm×level to level along the inverse of the curve.
// Release segment (state 2, phase 0→1): envelope = c × level
//   Falls from level to ~0 along the curve.
//
// EoC gate duration mirrors the firmware's 10-loop-tick (10 ms) pulse.
//
// Retrigger note: the firmware saves the level-scaled PWM value as `lastout`
// and reuses it in the attack formula, causing a double-level-scaling when
// retriggerring during release at level < 1. Here `lastEnvNorm` is stored
// un-scaled (curve fraction at trigger phase) so the retrigger start point
// correctly tracks the actual envelope level regardless of the level pot.

static constexpr float kEgEocDuration        = 0.010f;           // 10 ms EoC pulse
static constexpr float kEgInstantAttackRate  = 25.0f * 1021.0f / 1024.0f; // atkTime > 1020

struct EgVoice {
  int   state       = 0;      // 0 = idle, 1 = attack, 2 = release
  float phase       = 0.0f;   // 0..1 within the current segment
  float lastEnvNorm = 0.0f;   // un-level-scaled envelope at the last retrigger point
  float envelope    = 0.0f;   // current output (level already applied), 0..1
  bool  eoc         = false;  // end-of-cycle gate active
  float eocTimer    = 0.0f;   // seconds remaining in the EoC pulse

  void reset() {
    state = 0; phase = 0.0f; lastEnvNorm = 0.0f;
    envelope = 0.0f; eoc = false; eocTimer = 0.0f;
  }

  // Fire a trigger. Captures the current curve phase for a smooth retrigger
  // during release; resets to 0 otherwise (mirrors firmware lastout logic).
  void trigger() {
    lastEnvNorm = (state == 2) ? expf(-3.0f * phase) : 0.0f;
    state = 1;
    phase = 0.0f;
  }

  // Advance by dt seconds. Updates `envelope` (0..1, level-scaled) and `eoc`.
  // Call once per sample from VCV Rack or once per 1 ms loop tick from firmware.
  float process(float dt, const EgParams& p) {
    if (state == 1) {
      // Instant-attack: skip straight to release (mirrors firmware atkTime > 1020).
      if (p.attackRate >= kEgInstantAttackRate) {
        state = 2;
        phase = 0.0f;
      } else {
        const float c = expf(-3.0f * phase);
        envelope = (1.0f - c * (1.0f - lastEnvNorm)) * p.level;
        phase += p.attackRate * dt;
        if (phase >= 1.0f) {
          state = 2;
          phase = 0.0f;
        }
      }
    }

    if (state == 2) {
      const float c = expf(-3.0f * phase);
      envelope = c * p.level;
      phase += p.releaseRate * dt;
      if (phase >= 1.0f) {
        state = 0;
        phase = 0.0f;
        envelope = 0.0f;
        eoc      = true;
        eocTimer = kEgEocDuration;
      }
    }

    if (state == 0) {
      envelope = 0.0f;
    }

    // EoC pulse timer (10 ms, mirrors firmware EoCcount loop).
    if (eoc) {
      eocTimer -= dt;
      if (eocTimer <= 0.0f) {
        eoc      = false;
        eocTimer = 0.0f;
      }
    }

    return envelope;
  }
};

}  // namespace sc
