#pragma once

// "The Count" clock engine — the shared brain of the rabid.audio CLK module.
//
// Derived from rabid.audio's `clock` firmware (github.com/rabidaudio/synthesizer)
// under the MIT License, Copyright 2015-2020 Julian Knight. The upstream
// notice lives at firmwares/rabid-audio-clk/SOFTWARE_LICENSE and MIT requires it
// to ship with any copy of this header — the CC0 cores next to it have no such
// condition, so don't fold this one into them.
//
// Used by:
//   - firmwares/rabid-audio-clk/rabid-audio-clk.ino  (AVR Timer1 ISRs -> digitalWrite)
//   - rack-plugins/src/rabid-audio-clk.cpp           (dt-driven -> setVoltage)
//
// Pure C++: depends only on sc_math.h. No Arduino / Rack / Pico SDK, no PROGMEM,
// float only, header-only, no heap, AVR-friendly.
//
// UNITS: everything is in SECONDS and BPM. The engine owns the tempo/subdivision/
// swing state machine and hands the platform one number — how long until the next
// tick — so the two targets can drive it from completely different time sources:
//
//   firmware : Timer1 COMPA fires -> tick() -> write the returned interval to OCR1A
//   VCV Rack : step(dt) subtracts dt from an internal countdown and calls tick()
//
// ── Why there is no OCR1A lookup table any more ────────────────────────────
// The original firmware carried a 1586-entry PROGMEM table (`CLOCK_LOOKUP`,
// ~3.2 KB of flash) mapping clock-BPM to a Timer1 compare value, "to avoid
// floating point math". Every entry of that table is exactly
// `trunc(937500 / clock)`, i.e. `trunc(period_seconds * 15625)` for the 16 MHz /
// 1024-prescaler tick of 64 µs. So the engine works in seconds and the firmware
// converts once per tempo change:
//
//     OCR1A = (uint16_t)(intervalSeconds * kClkTimerHz)
//
// Verified against the table's endpoints and midpoint: 15 BPM -> 62500,
// 120 BPM -> 7812, 1600 BPM -> 585. Bit-identical output, 3.2 KB of flash back,
// and the tempo math becomes platform-neutral.

#include "sc_math.h"

namespace sc {

// ── Range constants (from the original Timer.h) ────────────────────────────
// The clock line runs at beat-rate x subdivisions, so it is allowed four times
// the BPM ceiling.
constexpr uint16_t kClkMinClock = 15;
constexpr uint16_t kClkMaxClock = 1600;
constexpr uint16_t kClkMinBPM = kClkMinClock;
constexpr uint16_t kClkMaxBPM = kClkMaxClock / 4;  // 400

// Swing is a percentage of the beat, applied as an alternating early/late drift.
// The divisor is 198 (= 66 * 3), not 200, so that "66%" lands on true triplets.
constexpr float kClkSwingDivisor = 198.0f;
constexpr int8_t kClkMinSwing = -75;
constexpr int8_t kClkMaxSwing = 75;

// Subdivision N: N >= 1 -> N clock ticks per beat; N <= -2 -> one clock tick
// every |N| beats. -1 and 0 are skipped (they would both mean "1x").
constexpr int8_t kClkMinSubdiv = -8;
constexpr int8_t kClkMaxSubdiv = 4;

// Timer1 at 16 MHz with the /1024 prescaler ticks at 15625 Hz (64 µs). Kept here
// (rather than in the sketch) because it is the constant that ties the engine's
// seconds to the firmware's compare values — see the header note above.
constexpr float kClkTimerHz = 15625.0f;

// Output pulse width. The firmware holds OCR1B at 250 timer ticks = 16 ms; Rack
// reuses the same figure so a gate looks the same on both targets.
constexpr float kClkPulseSec = 250.0f / kClkTimerHz;  // 0.016 s

// ── Persisted settings (EEPROM on the firmware, patch JSON in Rack) ────────
struct ClkSettings {
  uint16_t baseBPM;
  int8_t subdivisions;
  int8_t swing;
};

constexpr ClkSettings kClkDefaults = {120, 4, 0};

// Integer clamps kept local: sc_math's helpers are float-only and rounding a
// tempo through a float would be a silent off-by-one at the range ends.
inline uint16_t clkClampU16(int32_t v, uint16_t lo, uint16_t hi) {
  return (uint16_t)(v < (int32_t)lo ? lo : (v > (int32_t)hi ? hi : v));
}
inline int8_t clkClampI8(int32_t v, int8_t lo, int8_t hi) {
  return (int8_t)(v < (int32_t)lo ? lo : (v > (int32_t)hi ? hi : v));
}
inline uint8_t clkAbs8(int8_t v) { return (uint8_t)(v < 0 ? -v : v); }

// ── The clock engine ───────────────────────────────────────────────────────
//
// Two outputs, BEAT and DIV, are derived from a single tick stream running at
// whichever of the two is *faster*; the slower one is an integer division of it,
// counted down by `subdivIdx`. That is the original Timer1 design and it is what
// keeps the two outputs phase-locked with no drift.
//
//   subdivisions >= 1  ("subdivision mode")   : DIV is the fast line, BEAT = DIV / N
//   subdivisions <= -2 ("superdivision mode") : BEAT is the fast line, DIV = BEAT / |N|
//
// Swing offsets alternate ticks early then late by `swingSec`, so a pair of ticks
// still spans exactly two nominal periods (the tempo does not drift).
struct ClkEngine {
  // --- Settings ---
  uint16_t baseBPM = kClkDefaults.baseBPM;
  uint16_t bpmOffset = 0;  // added by the CV input
  int8_t subdivisions = kClkDefaults.subdivisions;
  int8_t swing = kClkDefaults.swing;
  bool enabled = true;

  // --- Derived timing (recomputed only when a setting changes) ---
  float periodSec = 0.5f;  // one tick of the FAST line, nominal
  float swingSec = 0.0f;   // early/late drift applied to alternating ticks

  // --- Tick state machine ---
  uint8_t subdivIdx = 1;  // ticks remaining until the slow line fires
  bool isEven = true;     // parity selects the early or the late interval
  bool fastHigh = false;  // level of the fast line
  bool slowHigh = false;  // level of the slow line

  // --- Countdown used by step(dt) hosts (Rack); unused on the firmware, whose
  // hardware timer owns the schedule. ---
  float nextTickSec = 0.0f;
  float pulseLeftSec = 0.0f;

  ClkEngine() { update(); }

  // True when DIV is slower than BEAT, i.e. BEAT is the fast line.
  bool superdivision() const { return subdivisions < 0; }

  // ── Tempo / clock rate ───────────────────────────────────────────────────

  // Beat rate actually in force, base tempo plus the CV offset.
  uint16_t bpm() const {
    return clkClampU16((int32_t)baseBPM + (int32_t)bpmOffset, kClkMinBPM, kClkMaxBPM);
  }

  // Rate of the FAST line in BPM. In subdivision mode that is beat x N.
  uint16_t clockBPM() const {
    int32_t c = bpm();
    if (subdivisions > 1) c *= subdivisions;
    return clkClampU16(c, kClkMinClock, kClkMaxClock);
  }

  // Recompute the derived timing. Cheap, but only called on a real change so the
  // AVR never does float math inside the audio-rate path.
  void update() {
    periodSec = 60.0f / (float)clockBPM();
    swingSec = periodSec * ((float)swing / kClkSwingDivisor);
  }

  void setBaseBPM(uint16_t v) {
    const uint16_t c = clkClampU16((int32_t)v, kClkMinBPM, kClkMaxBPM);
    if (c != baseBPM) {
      baseBPM = c;
      update();
    }
  }

  // Nudge the tempo by a (possibly negative) amount; returns the new base BPM.
  uint16_t incrementBaseBPM(int16_t amount) {
    if (amount != 0) setBaseBPM((uint16_t)clkClampU16((int32_t)baseBPM + amount,
                                                      kClkMinBPM, kClkMaxBPM));
    return baseBPM;
  }

  // CV offset in BPM (the firmware feeds adc/4, i.e. 0..255).
  void setBPMOffset(uint16_t v) {
    if (v != bpmOffset) {
      bpmOffset = v;
      update();
    }
  }

  void setSwing(int8_t v) {
    const int8_t c = clkClampI8((int32_t)v, kClkMinSwing, kClkMaxSwing);
    if (c != swing) {
      swing = c;
      update();
    }
  }

  int8_t incrementSwing(int16_t amount) {
    setSwing((int8_t)clkClampI8((int32_t)swing + amount, kClkMinSwing, kClkMaxSwing));
    return swing;
  }

  // Allowed values: {-8..-2, 1..4}. -1 and 0 are skipped on the way past.
  void setSubdivisions(int8_t v) {
    int8_t s = clkClampI8((int32_t)v, kClkMinSubdiv, kClkMaxSubdiv);
    if (s == -1 || s == 0) s = 1;
    if (s != subdivisions) {
      subdivisions = s;
      update();
    }
  }

  // Step the subdivision one detent at a time, hopping the -1/0 gap in whichever
  // direction the encoder is turning (the original's while-loop, kept so a fast
  // spin lands on the same value).
  int8_t incrementSubdivisions(int16_t amount) {
    if (amount == 0) return subdivisions;
    while (amount != 0) {
      if (amount > 0) {
        subdivisions++;
        if (subdivisions > kClkMaxSubdiv) subdivisions = kClkMaxSubdiv;
        if (subdivisions == -1 || subdivisions == 0) subdivisions = 1;
        amount--;
      } else {
        subdivisions--;
        if (subdivisions < kClkMinSubdiv) subdivisions = kClkMinSubdiv;
        if (subdivisions == -1 || subdivisions == 0) subdivisions = -2;
        amount++;
      }
    }
    update();
    return subdivisions;
  }

  void setEnabled(bool v) { enabled = v; }

  void loadSettings(const ClkSettings& s) {
    setSubdivisions(s.subdivisions);
    setBaseBPM(s.baseBPM);
    setSwing(s.swing);
  }

  ClkSettings currentSettings() const {
    ClkSettings s;
    s.baseBPM = baseBPM;
    s.subdivisions = subdivisions;
    s.swing = swing;
    return s;
  }

  // ── Tick state machine ───────────────────────────────────────────────────

  // Rewind to the start of a bar: the next tick fires both lines together.
  // The platform is expected to restart its timer / countdown afterwards.
  void reset() {
    subdivIdx = 1;  // 1 so the very next tick also fires the slow line
    isEven = true;
    fastHigh = false;
    slowHigh = false;
    nextTickSec = firstIntervalSec();
    pulseLeftSec = 0.0f;
  }

  // Interval the platform should schedule immediately after reset(). The
  // original wrote `OCR1A = clock - swingOffset` there, which means the first two
  // intervals after a reset are both the "early" one; reproduced deliberately.
  float firstIntervalSec() const { return periodSec - swingSec; }

  // Raise the outputs for one tick and return how long until the next one.
  // Mirrors the firmware's TIMER1_COMPA ISR exactly, minus the pin writes:
  // the interval is chosen from the *current* parity, then the parity flips.
  float tick() {
    const float interval = isEven ? (periodSec - swingSec) : (periodSec + swingSec);
    isEven = !isEven;

    fastHigh = true;
    if (subdivIdx > 0) subdivIdx--;
    if (subdivIdx == 0) {
      slowHigh = true;
      subdivIdx = clkAbs8(subdivisions);
      if (subdivIdx == 0) subdivIdx = 1;  // guard: subdivisions is never 0
    }
    return interval;
  }

  // Drop both outputs (the firmware's TIMER1_COMPB ISR).
  void endPulse() {
    fastHigh = false;
    slowHigh = false;
  }

  // Fire one tick out of band without disturbing the schedule — the hardware's
  // "advance a stopped sequencer by one step" gesture while paused.
  void singleTick() {
    tick();
    pulseLeftSec = kClkPulseSec;
  }

  bool beatOut() const { return superdivision() ? fastHigh : slowHigh; }
  bool divOut() const { return superdivision() ? slowHigh : fastHigh; }

  // ── dt-driven host (VCV Rack) ────────────────────────────────────────────
  //
  // Advances the countdown by `dt` seconds, ticking as many times as fit (a
  // 1600 BPM clock is ~37 ms apart, so at any sane sample rate that is at most
  // one — the loop is a safety net for very large dt, e.g. after a patch load).
  // The firmware does not call this: its Timer1 is the countdown.
  void step(float dt) {
    if (pulseLeftSec > 0.0f) {
      pulseLeftSec -= dt;
      if (pulseLeftSec <= 0.0f) {
        pulseLeftSec = 0.0f;
        endPulse();
      }
    }
    if (!enabled) return;

    nextTickSec -= dt;
    for (int guard = 0; nextTickSec <= 0.0f && guard < 16; guard++) {
      nextTickSec += tick();
      pulseLeftSec = kClkPulseSec;
    }
    if (nextTickSec <= 0.0f) nextTickSec = periodSec;  // pathological dt: resync
  }
};

// ── Tap tempo ──────────────────────────────────────────────────────────────
//
// Averages the last four tap intervals and reports a BPM. dt-driven so the
// firmware (which used micros()) and Rack (sample time) behave identically.
struct ClkTapTempo {
  static constexpr float kTimeoutSec = 2.0f;  // taps further apart start over
  static constexpr uint8_t kSamples = 4;

  uint16_t bpm = 0;  // 0 == "no tempo measured"
  float deltas[kSamples] = {0.0f, 0.0f, 0.0f, 0.0f};
  uint8_t deltaIdx = 0;
  float nowSec = 0.0f;
  float lastTapSec = -kTimeoutSec * 2.0f;  // so the first tap always starts over
  bool wasPressed = false;

  void cancel() {
    bpm = 0;
    deltaIdx = 0;
    lastTapSec = nowSec - kTimeoutSec * 2.0f;
  }

  // Advance the internal clock and process the (already debounced) button level.
  // Returns the current BPM estimate, or 0 if there isn't one yet.
  uint16_t tick(float dt, bool pressed) {
    nowSec += dt;
    if (pressed == wasPressed) return bpm;
    if (!pressed) {
      wasPressed = false;
      return bpm;
    }
    wasPressed = true;

    const float delta = nowSec - lastTapSec;
    if (delta > kTimeoutSec) {
      // Too long since the last tap: treat this as the first of a new series.
      cancel();
      lastTapSec = nowSec;
      return 0;
    }
    lastTapSec = nowSec;
    deltas[deltaIdx % kSamples] = delta;
    deltaIdx++;

    uint8_t limit = deltaIdx < kSamples ? deltaIdx : kSamples;
    float avg = 0.0f;
    for (uint8_t i = 0; i < limit; i++) avg += deltas[i];
    avg /= (float)limit;
    bpm = (avg > 1.0e-4f) ? (uint16_t)(60.0f / avg) : 0;
    return bpm;
  }

  // True while the measured tempo is still fresh (drives "show the tapped BPM").
  bool isActive() const {
    if (bpm == 0) return false;
    return (nowSec - lastTapSec) <= kTimeoutSec;
  }
};

// ── Pause latch ────────────────────────────────────────────────────────────
//
// One momentary gesture toggles the pause state; a second, independent edge
// detector lets a button double as "advance one step" while paused.
struct ClkPauseState {
  bool paused = false;
  bool held = false;
  bool tapHeld = false;

  // `gesture` is the level of whatever toggles pause (A+B together on hardware).
  void setState(bool gesture) {
    if (!held && gesture) paused = !paused;
    held = gesture;
  }

  // Rising-edge detector for the single-step button.
  bool isTap(bool pressed) {
    if (tapHeld == pressed) return false;
    tapHeld = pressed;
    return pressed;
  }

  bool isPaused() const { return paused; }

  void reset() { paused = false; }
};

// ── Change timeout ─────────────────────────────────────────────────────────
//
// "Has this value been touched in the last N seconds?" — used to show the base
// BPM while the encoder is moving and the CV-summed BPM once it settles.
struct ClkChangeTimeout {
  float timeoutSec = 1.5f;
  float sinceSec = 1.0e6f;

  void begin(float seconds) {
    timeoutSec = seconds;
    sinceSec = seconds * 2.0f;
  }
  void noteChanged() { sinceSec = 0.0f; }
  void tick(float dt) {
    if (sinceSec < 1.0e6f) sinceSec += dt;
  }
  bool isChanging() const { return sinceSec < timeoutSec; }
};

// ── 3-digit display formatting ─────────────────────────────────────────────
//
// `out` is LSB-first (out[0] is the rightmost digit), matching the hardware's
// digit array. Characters are limited to the set the 7-segment font can render:
// '0'-'9', ' ', '-', '_', '=', 'S', 'L', 'F'.
constexpr uint8_t kClkDigits = 3;

// Character at position `digit` (0 = units) of a signed value, space-padded, with
// a '-' immediately left of the most significant digit when negative.
inline char clkDigitChar(int16_t value, uint8_t digit) {
  bool negative = value < 0;
  if (negative) value = (int16_t)(-value);
  for (uint8_t d = 0; d < digit; d++) {
    value = (int16_t)(value / 10);
    if (value == 0) {
      if (d + 1 == digit && negative) return '-';
      return ' ';
    }
  }
  return (char)('0' + (value % 10));
}

inline void clkFormatNumber(char out[kClkDigits], int16_t value) {
  for (uint8_t i = 0; i < kClkDigits; i++) out[i] = clkDigitChar(value, i);
}

// Fill every digit with the same character (the save / load / reset prompts).
inline void clkFormatFill(char out[kClkDigits], char c) {
  for (uint8_t i = 0; i < kClkDigits; i++) out[i] = c;
}

inline void clkFormatBlank(char out[kClkDigits]) { clkFormatFill(out, ' '); }

// Paused: a single '=' in the middle digit.
inline void clkFormatPaused(char out[kClkDigits]) {
  clkFormatBlank(out);
  out[kClkDigits / 2] = '=';
}

// Subdivisions: positive values read as a plain number, negative ones as the
// fraction "1-N" (the 7-segment stand-in for 1/N).
inline void clkFormatSubdiv(char out[kClkDigits], int8_t subdiv) {
  if (subdiv < 0) {
    out[2] = '1';
    out[1] = '-';
    out[0] = (char)('0' + clkAbs8(subdiv));
  } else {
    clkFormatNumber(out, (int16_t)subdiv);
  }
}

// ── 7-segment font ─────────────────────────────────────────────────────────
//
// Standard segment naming, so the font is wiring-independent:
//
//        aaaa
//       f    b
//       f    b
//        gggg
//       e    c
//       e    c
//        dddd
//
// The firmware permutes these bits into whichever order its PCB traces use (the
// CLK's centre digit is wired differently from its neighbours to fit 3 HP); the
// Rack widget draws them directly. One font, two consumers.
constexpr uint8_t kSegA = 0x01;
constexpr uint8_t kSegB = 0x02;
constexpr uint8_t kSegC = 0x04;
constexpr uint8_t kSegD = 0x08;
constexpr uint8_t kSegE = 0x10;
constexpr uint8_t kSegF = 0x20;
constexpr uint8_t kSegG = 0x40;

inline uint8_t clkSegments(char c) {
  switch (c) {
    case '0': return kSegA | kSegB | kSegC | kSegD | kSegE | kSegF;
    case '1': return kSegB | kSegC;
    case '2': return kSegA | kSegB | kSegD | kSegE | kSegG;
    case '3': return kSegA | kSegB | kSegC | kSegD | kSegG;
    case '4': return kSegB | kSegC | kSegF | kSegG;
    case 'S':  // 'S' and '5' are the same glyph
    case '5': return kSegA | kSegC | kSegD | kSegF | kSegG;
    case '6': return kSegA | kSegC | kSegD | kSegE | kSegF | kSegG;
    case '7': return kSegA | kSegB | kSegC;
    case '8': return kSegA | kSegB | kSegC | kSegD | kSegE | kSegF | kSegG;
    case '9': return kSegA | kSegB | kSegC | kSegD | kSegF | kSegG;
    // '=' is the paused indicator: the four verticals, drawn as a pause bar.
    case '=': return kSegB | kSegC | kSegE | kSegF;
    case 'L': return kSegD | kSegE | kSegF;
    case 'F': return kSegA | kSegE | kSegF | kSegG;
    case '-': return kSegG;
    case '_': return kSegD;
    default: return 0;  // blank
  }
}

}  // namespace sc
