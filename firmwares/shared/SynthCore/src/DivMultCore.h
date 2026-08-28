#pragma once

// Div Mult — two-track clock divider AND multiplier on one ratio dial.
//
// This core fuses TWO GRAINS firmwares that Sean Luke shipped separately:
// `divvy` (two-track clock divider with pulsewidth) and `multiple` (two-track
// clock multiplier with pulsewidth). Both are the same instrument seen from
// opposite sides — two independent tracks, one shared pulse-width pot, one
// clock in, one reset — so the port concatenates their option tables into a
// single 21-position sweep with unity in the middle:
//
//   index  0 .. 7   multiplications, `multiple`'s table verbatim and in its
//                   own order:  x16, x7, x6, x5, x4, x3, "swing 2", x2
//   index  8        x1 — the join between the two tables, the one ratio
//                   neither upstream had (it is `multiple` with a factor of 1)
//   index  9 .. 20  divisions, `divvy`'s table verbatim and in its own order:
//                   /2, /2 offset 1, /3, /4, /4 offset 2, /6, /8, /16, /24,
//                   /32, /64, /96
//
// Each track picks anywhere in that range, so one module covers all of divvy
// (both tracks dividing), all of multiple (both tracks multiplying), and the
// combination neither upstream could reach (one of each off the same clock).
//
// The two halves keep their own state machines because they genuinely differ:
// division is COUNTED (a down-counter of clock edges, no notion of time, exact
// forever) while multiplication is PREDICTED (the clock period is measured,
// then subdivided), which is why multiplication needs a beat before it can
// lock and division does not.
//
// Used by:
//   - firmwares/mod1-divmult/mod1-divmult.ino   (loop-rate step(), digital outs)
//   - rack-plugins/src/mod1-divmult.cpp         (audio-rate step(), 0/10 V outs)
//
// Pure C++: depends only on sc_math.h. No Arduino / Rack / Pico SDK.
//
// Derived from the GRAINS `divvy` and `multiple` firmwares
// (github.com/eclab/grains) under the Apache License 2.0, Copyright 2023 Sean
// Luke. The upstream notice lives at firmwares/mod1-divmult/LICENSE.md and
// Apache 2.0 requires it to ship with any copy of this header — the CC0 cores
// next to it have no such condition, so don't fold this one into them.

#include "sc_math.h"

namespace sc {

// ── The combined ratio table ───────────────────────────────────────────────

constexpr uint8_t kDivMultOptions = 21;  // 8 multiplications + unity + 12 divisions
constexpr uint8_t kDivMultUnity   = 8;   // index of x1; anything above it divides
constexpr uint8_t kDivMultSwing2  = 6;   // `multiple`'s SWING_2 special case

// Three tracks: two independent ratios (upstream's two) plus the unity thru
// this port adds on F4.
constexpr uint8_t kDivMultTracks = 3;

// Fixed trigger length used wherever the pulse-width pot bottoms out, and for
// the very first multiplied pulse (before the tempo is known). Upstream divvy
// counted PULSEWIDTH_COUNTDOWN = 100 loop iterations, which is loop-rate
// dependent and meaningless off an Arduino; 10 ms is the repo's trigger length
// (mod1-euclidean's triggerTime) and reads the same on every target.
constexpr float kDivMultTrigSec = 0.010f;

// A measured clock period outside this window is ignored rather than believed.
// Guards the multiplier against the first edge after power-on, a patch load, or
// a clock that stopped and restarted minutes later.
constexpr float kDivMultMinPeriodSec = 0.002f;  // 500 Hz
constexpr float kDivMultMaxPeriodSec = 20.0f;

// Consecutive step() calls a new ratio index must hold before it is accepted.
// This is upstream divvy's OPTION_WAIT: ADC noise sitting on a table boundary
// would otherwise reset the track continuously.
constexpr uint8_t kDivMultOptionWait = 16;

// Pot -> ratio index. Mirrors divvy's `(analogRead(pot) * NUM_OPTIONS) >> 10`,
// so the firmware (pass adc/1023) and Rack (pass the 0..1 knob) land on the
// same boundaries.
inline uint8_t divMultOptionFromPot(float pot01) {
  int i = (int)(clampf(pot01, 0.0f, 1.0f) * (float)kDivMultOptions);
  if (i >= (int)kDivMultOptions) i = (int)kDivMultOptions - 1;
  if (i < 0) i = 0;
  return (uint8_t)i;
}

// Hysteresis width, as a fraction of one ratio slot.
//
// This dial packs 21 options onto one pot, ~49 ADC counts each, so a pot parked
// on a boundary dithers between two ratios on ADC noise alone — and both
// upstreams handle that badly, in opposite directions. `multiple` re-armed the
// track on every flip, which suppressed its sub-pulses and left the output
// running at x1: the "pot between two options may not pulse at all" weakness
// its own header warns about. `divvy`'s OPTION_WAIT counter restarts whenever
// the pending index changes, so under the same dither it never finishes
// counting and the ratio locks out — the pot goes dead rather than the output.
// Real hysteresis fixes both: the pot must penetrate a fifth of a slot (~10
// counts) into its neighbour before the selection moves, which clears typical
// ADC noise by a wide margin and is still well short of feeling sticky.
constexpr float kDivMultHysteresis = 0.2f;

// Ratio index with hysteresis against the currently selected one. Coming off a
// boundary the thresholds are still divvy's `(adc * 21) >> 10`; only the exit
// from the current slot is widened.
inline uint8_t divMultOptionHyst(float pot01, uint8_t current) {
  if (current >= kDivMultOptions) return divMultOptionFromPot(pot01);
  const float x = clampf(pot01, 0.0f, 1.0f) * (float)kDivMultOptions;
  if (x >= (float)(current + 1) + kDivMultHysteresis ||
      x <  (float)current - kDivMultHysteresis)
    return divMultOptionFromPot(pot01);  // far enough in; take the plain mapping
  return current;
}

// Above unity the track divides (counts edges); at or below it multiplies.
inline bool divMultIsDivision(uint8_t opt) { return opt > kDivMultUnity; }

// Pulses emitted per incoming clock, for the multiplying half. Index 6
// ("swing 2") shares x3's factor and jump — it differs only in that its middle
// pulse is suppressed, which is what turns a triplet into a shuffle.
inline uint8_t divMultMultiplier(uint8_t opt) {
  static const uint8_t kMul[kDivMultUnity + 1] = {16, 7, 6, 5, 4, 3, 3, 2, 1};
  return kMul[opt <= kDivMultUnity ? opt : kDivMultUnity];
}

// Incoming clocks per output pulse, for the dividing half (divvy's clockPulses).
inline uint8_t divMultDivisor(uint8_t opt) {
  static const uint8_t kDiv[12] = {2, 2, 3, 4, 4, 6, 8, 16, 24, 32, 64, 96};
  return kDiv[opt > kDivMultUnity ? (uint8_t)(opt - kDivMultUnity - 1) : 0];
}

// Clocks to swallow after a reset before the first pulse (divvy's clockOffsets).
// This is what makes "/2 offset 1" and "/4 offset 2" binary counter digits
// rather than duplicates of "/2" and "/4".
inline uint8_t divMultOffset(uint8_t opt) {
  static const uint8_t kOff[12] = {0, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0};
  return kOff[opt > kDivMultUnity ? (uint8_t)(opt - kDivMultUnity - 1) : 0];
}

// Human-readable ratio name, for the Rack knob's tooltip. Only the desktop
// build references this, so the AVR linker drops the strings entirely.
inline const char* divMultRatioName(uint8_t opt) {
  static const char* const kNames[kDivMultOptions] = {
    "x16", "x7", "x6", "x5", "x4", "x3", "swing 2", "x2",
    "x1 (thru)",
    "/2", "/2 offset 1", "/3", "/4", "/4 offset 2", "/6",
    "/8", "/16", "/24", "/32", "/64", "/96"};
  return kNames[opt < kDivMultOptions ? opt : (uint8_t)(kDivMultOptions - 1)];
}

// ── One track ──────────────────────────────────────────────────────────────

struct DivMultTrack {
  uint8_t option = kDivMultUnity;

  // Dividing half — divvy's counter, in clock edges. Zero means "pulse on the
  // next edge"; after a pulse it reloads to P-1 so the period is P edges.
  uint16_t divCount = 0;

  // Multiplying half — multiple's counters, converted from loop iterations to
  // seconds so the behaviour survives any host rate.
  float   elapsed   = 0.0f;  // since this sequence's first (on-clock) pulse
  float   jump      = 0.0f;  // seconds between sub-pulses (0 = tempo unknown)
  uint8_t total     = 1;     // sub-pulses emitted so far, 1-based
  uint8_t maxPulses = 1;     // sub-pulses this sequence will emit

  bool  gate     = false;
  float trigLeft = -1.0f;  // >= 0 while a fixed-length trigger is running

  // Ratio-change debounce (divvy's optionCounter).
  uint8_t pendingOption = kDivMultUnity;
  int8_t  optionWait    = -1;
};

// ── The engine ─────────────────────────────────────────────────────────────

struct DivMultEngine {
  DivMultTrack tracks[kDivMultTracks];

  float pulseWidth = 0.5f;   // 0..1, shared by every track (upstream's POT3)
  float sinceClock = 0.0f;   // seconds since the last accepted clock edge
  float period     = 0.0f;   // most recent measured clock period, seconds
  bool  havePeriod = false;  // false until two edges have been seen

  void reset() {
    sinceClock = 0.0f;
    period     = 0.0f;
    havePeriod = false;
    for (uint8_t t = 0; t < kDivMultTracks; t++) resetTrack(tracks[t]);
  }

  // Restart one track without disturbing the others. Upstream's reset() zeroed
  // both tracks whenever either pot moved; with a divider and a multiplier able
  // to share the module that would let one pot interrupt the other's phrase, so
  // a ratio change now restarts only the track that changed. A reset trigger
  // (or the panel button) still restarts everything via reset().
  void resetTrack(DivMultTrack& tr) {
    tr.gate     = false;
    tr.trigLeft = -1.0f;
    tr.elapsed  = 0.0f;
    tr.jump     = 0.0f;
    if (divMultIsDivision(tr.option)) {
      tr.divCount  = divMultOffset(tr.option);
      tr.total     = 1;
      tr.maxPulses = 1;
    } else {
      tr.maxPulses = divMultMultiplier(tr.option);
      tr.total     = tr.maxPulses;  // "sequence finished" -> resync on next clock
    }
  }

  void setPulseWidth(float pw01) { pulseWidth = clampf(pw01, 0.0f, 1.0f); }

  // Feed a track's ratio pot every step(). Hysteresis stops a pot parked on a
  // boundary from dithering, and the resulting index still has to hold across
  // kDivMultOptionWait + 2 consecutive calls (arm, count down, accept — the
  // same debounce idea as upstream's OPTION_WAIT) before the track restarts.
  void setRatioPot(uint8_t t, float pot01) {
    if (t >= kDivMultTracks) return;
    setOption(t, divMultOptionHyst(pot01, tracks[t].option));
  }

  // Set a ratio by index. Accepted only once the same index has held across
  // kDivMultOptionWait + 2 consecutive calls, then the track restarts. Callers
  // driving this from a pot should use setRatioPot() so the hysteresis applies.
  void setOption(uint8_t t, uint8_t opt) {
    if (t >= kDivMultTracks || opt >= kDivMultOptions) return;
    DivMultTrack& tr = tracks[t];
    if (opt == tr.option) { tr.optionWait = -1; return; }
    if (tr.optionWait < 0 || opt != tr.pendingOption) {
      tr.pendingOption = opt;
      tr.optionWait    = (int8_t)kDivMultOptionWait;
    } else if (tr.optionWait == 0) {
      tr.option     = opt;
      tr.optionWait = -1;
      resetTrack(tr);
    } else {
      tr.optionWait--;
    }
  }

  // Advance `dt` seconds. `clockEdge` is true exactly once per rising edge of
  // the incoming clock. Call order mirrors upstream's loop: gates fall first,
  // then the clock is serviced, then any multiplied sub-pulse is emitted.
  void step(float dt, bool clockEdge) {
    if (dt > 0.0f) sinceClock += dt;

    for (uint8_t t = 0; t < kDivMultTracks; t++) {
      DivMultTrack& tr = tracks[t];
      const bool dividing = divMultIsDivision(tr.option);
      if (!dividing) tr.elapsed += dt;

      if (tr.trigLeft >= 0.0f) {
        tr.trigLeft -= dt;
        if (tr.trigLeft <= 0.0f) { tr.trigLeft = -1.0f; tr.gate = false; }
        continue;  // a fixed trigger owns the gate until it expires
      }

      if (dividing) {
        // divvy holds the gate high for `pw` whole clocks after the pulse,
        // i.e. until the down-counter reaches P-pw-1. Upstream tested that
        // boundary with `==`; `<=` is used here so that winding the pulse-width
        // pot down past the current count drops the gate immediately instead of
        // sticking high until the next pulse.
        const uint8_t P  = divMultDivisor(tr.option);
        const uint8_t pw = divGateClocks(P);
        if (pw != 0 && tr.gate && (uint16_t)(tr.divCount + 1) <= (uint16_t)(P - pw))
          tr.gate = false;
      } else {
        // multiple holds the gate for `pulseWidth * jump` after the most recent
        // sub-pulse, which sits at jump*(total-1).
        if (tr.gate && tr.jump > 0.0f && tr.total <= tr.maxPulses &&
            tr.elapsed >= tr.jump * (float)(tr.total - 1) + multGateSec(tr.jump))
          tr.gate = false;
      }
    }

    if (clockEdge) {
      const float p = sinceClock;
      sinceClock = 0.0f;
      if (p >= kDivMultMinPeriodSec && p <= kDivMultMaxPeriodSec) {
        period     = p;
        havePeriod = true;
      }
      onClock();
    }

    // Multiplied sub-pulses, after the clock so a sequence started this call
    // does not immediately fire its second pulse.
    for (uint8_t t = 0; t < kDivMultTracks; t++) {
      DivMultTrack& tr = tracks[t];
      if (divMultIsDivision(tr.option) || tr.jump <= 0.0f) continue;
      if (tr.total < tr.maxPulses && tr.elapsed >= tr.jump * (float)tr.total) {
        // "swing 2" is x3 with the middle pulse dropped: pulses land on the
        // beat and at 2/3 of it, which is the shuffle upstream was after.
        if (!(tr.option == kDivMultSwing2 && tr.total == 1)) firePulse(tr);
        tr.total++;
      }
    }
  }

  bool out(uint8_t t) const { return t < kDivMultTracks ? tracks[t].gate : false; }

 private:
  // Whole clocks the divided gate stays high. Upstream computed
  // `(pot * P) >> 10` from a 0..1023 pot, which can never reach P — the gate
  // always falls at least one clock before the next pulse. Preserved here.
  uint8_t divGateClocks(uint8_t P) const {
    int pw = (int)(pulseWidth * (float)P);
    if (pw > (int)P - 1) pw = (int)P - 1;
    return (uint8_t)(pw < 0 ? 0 : pw);
  }

  // Seconds the multiplied gate stays high. Floored at the trigger length so
  // the bottom of the pot still produces something a module downstream can see
  // (upstream's floor was jump/1024, tens of microseconds), and capped just
  // below `jump` so consecutive pulses always have a falling edge between them
  // (upstream let a full-width gate merge into the next one).
  float multGateSec(float jump) const {
    float g = pulseWidth * jump;
    if (g < kDivMultTrigSec) g = kDivMultTrigSec;
    const float cap = jump * 0.95f;
    if (g > cap) g = cap;
    return g;
  }

  void firePulse(DivMultTrack& tr) {
    tr.gate     = true;
    tr.trigLeft = -1.0f;
  }

  void onClock() {
    for (uint8_t t = 0; t < kDivMultTracks; t++) {
      DivMultTrack& tr = tracks[t];

      if (divMultIsDivision(tr.option)) {
        const uint8_t P = divMultDivisor(tr.option);
        if (tr.divCount == 0) {
          tr.divCount = (uint16_t)(P - 1);
          firePulse(tr);
          // Pulse width rounds down to whole clocks; at zero divvy sends a
          // trigger instead of a gate.
          if (divGateClocks(P) == 0) tr.trigLeft = kDivMultTrigSec;
        } else {
          tr.divCount--;
        }
      } else if (tr.total >= tr.maxPulses) {
        // The previous sequence is done — start the next one on the beat.
        tr.maxPulses = divMultMultiplier(tr.option);
        tr.jump      = havePeriod ? (period / (float)tr.maxPulses) : 0.0f;
        tr.elapsed   = 0.0f;
        tr.total     = 1;
        firePulse(tr);
        if (tr.jump <= 0.0f) {
          // No trustworthy period yet, so there is nothing to subdivide: emit a
          // plain trigger on the beat and wait for the next edge to measure the
          // tempo. Upstream instead subdivided whatever its counter happened to
          // hold, which is the "won't be right until two beats after a reset"
          // weakness its own header calls out.
          tr.trigLeft = kDivMultTrigSec;
          tr.total    = tr.maxPulses;
        }
      }
    }
  }
};

}  // namespace sc
