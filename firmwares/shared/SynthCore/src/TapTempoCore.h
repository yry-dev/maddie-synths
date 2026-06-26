#pragma once

// Tap-tempo master clock — the shared timing engine of the TapTempo module.
//
// Used by:
//   - firmwares/mod1-tap-tempo/mod1-tap-tempo.ino  (millis() taps -> digitalWrite outs)
//   - rack-plugins/src/TapTempo.cpp                      (button/clock taps -> PulseGenerators)
//
// Pure C++: depends only on sc_math.h. No Arduino / Rack / Pico SDK. float only,
// header-only, no heap, AVR-friendly (~tiny RAM footprint).
//
// UNITS: everything is in SECONDS and Hz. The engine is dt-driven: process(dt)
// advances an internal clock and one phase per output (phase is in *cycles*,
// wrapping at 1.0) and reports a rising trigger edge whenever an output should
// fire. Identical tempo behaviour at any sample / loop rate.
//
// The original Mod1 firmware measured tap spacing with millis() and scheduled
// each output by comparing absolute millisecond timestamps. Here that becomes:
//   - taps stamp an internal seconds clock (advanced by dt), so consistent
//     loop/sample latency cancels out of the interval differences,
//   - the tempo is the average of the last three tap intervals (as the firmware
//     averaged its last three press intervals),
//   - the 4x and multiply outputs are locked sub-divisions of the master phase
//     (the firmware re-synced them to the downbeat every period),
//   - the two divide ("period expansion") outputs are free-running clocks of
//     period * factor that only re-sync when the tempo changes.

#include "sc_math.h"

namespace sc {

// Pot (0..1) -> integer rate factor {1,2,3,4,8,16}. Mirrors the firmware's
// getMultiplierFromPot ADC thresholds (170/340/510/680/850 over 0..1023), so
// firmware (pass adc/1023) and VCV Rack (pass the raw 0..1 knob) agree exactly.
inline uint8_t tapTempoFactorFromPot(float pot01) {
  if (pot01 < 170.0f / 1023.0f) return 1;
  if (pot01 < 340.0f / 1023.0f) return 2;
  if (pot01 < 510.0f / 1023.0f) return 3;
  if (pot01 < 680.0f / 1023.0f) return 4;
  if (pot01 < 850.0f / 1023.0f) return 8;
  return 16;
}

// True if advancing the master phase from `prev` to `cur` (where `cur` may have
// just wrapped back into [0,1)) crosses a k/n sub-boundary -- i.e. an
// n-pulses-per-period sub-clock should fire this step. A period roll-over always
// crosses the k=0 boundary, so every sub-clock fires on the downbeat. One edge
// per step (sample rate is assumed >> clock rate, as on both targets).
inline bool tapTempoSubEdge(float prev, float cur, uint8_t n, bool wrapped) {
  if (n == 0) return false;
  if (wrapped) return true;
  return (int)(cur * (float)n) != (int)(prev * (float)n);
}

// Advance a free-running divide clock of length `lenSec`; return true on the
// step it completes a cycle. `phase` is in cycles and wraps at 1.0.
inline bool tapTempoDivEdge(float& phase, float dt, float lenSec) {
  if (lenSec <= 1.0e-6f) return false;
  phase += dt / lenSec;
  if (phase >= 1.0f) {
    while (phase >= 1.0f) phase -= 1.0f;
    return true;
  }
  return false;
}

// Trigger edges produced by one process() step. Each fX is true for the single
// step on which that output should emit a clock pulse; `beat` marks the master
// 1x downbeat (drives the panel LED). Pulse *width* is owned by the platform
// (firmware: 5ms digitalWrite; VCV: dsp::PulseGenerator).
struct TapTempoEdges {
  bool f1;    // 4x fixed sub-clock
  bool f2;    // 1..16x multiply (locked to the master phase)
  bool f3;    // 1..16x divide / period expansion (free-running)
  bool f4;    // 1..16x divide / period expansion (free-running)
  bool beat;  // master 1x downbeat (LED)
};

struct TapTempoCore {
  // --- Constants (seconds), from the firmware ---
  // Default period before any tempo is tapped (firmware clockInterval = 500ms).
  static constexpr float kDefaultPeriodSec = 0.5f;
  // LED on-time, reused as the minimum-period guard (firmware ledOnTime = 10ms).
  static constexpr float kLedOnSec = 0.010f;
  // Number of fixed sub-pulses on F1 per master period (firmware fires 4).
  static constexpr uint8_t kMainPulsesPerBeat = 4;

  // --- Tap capture ---
  float tapTimes[4] = {0.0f, 0.0f, 0.0f, 0.0f};  // last 4 tap stamps (sec)
  uint8_t tapCount = 0;                            // saturating 0..4
  float nowSec = 0.0f;                             // internal clock (sec)

  // --- Tempo ---
  float periodSec = kDefaultPeriodSec;  // master 1x period (sec)

  // --- Phase accumulators (cycles, wrap at 1.0) ---
  float masterPhase = 0.0f;  // drives beat, F1 (4x) and F2 (multiply)
  float divPhaseF3 = 0.0f;   // free-running expansion clock
  float divPhaseF4 = 0.0f;

  // Restore the firmware's power-on state.
  void reset() {
    for (int i = 0; i < 4; i++) tapTimes[i] = 0.0f;
    tapCount = 0;
    nowSec = 0.0f;
    periodSec = kDefaultPeriodSec;
    masterPhase = 0.0f;
    divPhaseF3 = 0.0f;
    divPhaseF4 = 0.0f;
  }

  // Register a tap (button press / external clock rising edge) at the current
  // internal time. Shifts a 4-deep tap-time history and, once four taps are in,
  // sets the period to the average of the last three intervals (guarded to a
  // sane minimum), then re-syncs the master + expansion phases so the next
  // downbeat starts fresh -- exactly the firmware's recordPressTime +
  // calculateClockInterval.
  void tap() {
    tapTimes[0] = tapTimes[1];
    tapTimes[1] = tapTimes[2];
    tapTimes[2] = tapTimes[3];
    tapTimes[3] = nowSec;
    if (tapCount < 4) tapCount++;
    if (tapCount < 4) return;

    const float i1 = tapTimes[1] - tapTimes[0];
    const float i2 = tapTimes[2] - tapTimes[1];
    const float i3 = tapTimes[3] - tapTimes[2];
    float avg = (i1 + i2 + i3) / 3.0f;
    if (avg < (kLedOnSec + 0.001f)) avg = kLedOnSec + 0.010f;  // firmware guard

    periodSec = avg;
    // Tempo changed: restart the master period and the free-running expansion
    // clocks (firmware resets lastBlinkTime and reschedules A1/A2).
    masterPhase = 0.0f;
    divPhaseF3 = 0.0f;
    divPhaseF4 = 0.0f;
  }

  // Advance the engine by `dt` seconds. `tapEdge` registers a tap this step.
  // potF2/potF3/potF4 are the three division pots (0..1): F2 multiplies the
  // tempo (faster), F3 and F4 divide it (slower / period expansion).
  TapTempoEdges process(float dt, bool tapEdge, float potF2, float potF3, float potF4) {
    nowSec += dt;
    if (tapEdge) tap();

    TapTempoEdges e = {false, false, false, false, false};
    const float invPeriod = (periodSec > 1.0e-6f) ? (1.0f / periodSec) : 0.0f;

    // Master phase: one cycle per period. Detect the downbeat at the wrap.
    const float oldMaster = masterPhase;
    masterPhase += dt * invPeriod;
    bool wrapped = false;
    while (masterPhase >= 1.0f) {
      masterPhase -= 1.0f;
      wrapped = true;
    }
    e.beat = wrapped;

    // F1: fixed 4x sub-clock locked to the master phase (firmware's "4 triggers
    // within the main period").
    e.f1 = tapTempoSubEdge(oldMaster, masterPhase, kMainPulsesPerBeat, wrapped);

    // F2: 1..16x multiply, also locked to the master phase (the firmware
    // re-synced A0 to the downbeat every period).
    const uint8_t mF2 = tapTempoFactorFromPot(potF2);
    e.f2 = tapTempoSubEdge(oldMaster, masterPhase, mF2, wrapped);

    // F3/F4: 1..16x divide -- free-running expansion clocks of period * factor.
    const uint8_t mF3 = tapTempoFactorFromPot(potF3);
    const uint8_t mF4 = tapTempoFactorFromPot(potF4);
    e.f3 = tapTempoDivEdge(divPhaseF3, dt, periodSec * (float)mF3);
    e.f4 = tapTempoDivEdge(divPhaseF4, dt, periodSec * (float)mF4);

    return e;
  }

  // Current tempo in Hz (master 1x rate), for display / debugging.
  float tempoHz() const { return (periodSec > 1.0e-6f) ? (1.0f / periodSec) : 0.0f; }
};

}  // namespace sc
