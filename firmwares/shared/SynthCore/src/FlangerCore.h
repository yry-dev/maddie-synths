#pragma once

// Flanger — swept-comb flanger core.
//
// Used by:
//   - firmwares/mod2-flanger/mod2-flanger.ino
//   - rack-plugins/src/mod2-flanger.cpp
//
// Short modulated delay with feedback: a single fractional tap sweeps 0.2 -
// 12 ms (exponential — perceived sweep is log in delay time) over a ~20 ms
// buffer, with bipolar feedback around the delay (CCW negative / CW positive;
// the two classic flavours). The feedback path is DC-blocked and
// soft-saturated, and stays below oscillation except the last few percent of
// the control (deliberately allowed to scream). Three sweep sources cycle on
// the button: triangle LFO / sine LFO / envelope follower (sweep driven by
// the input level — great on drums). Full-CCW rate = manual mode: the sweep
// position is set by hand (manualPos). The LED follows the sweep (ledLevel()).
//
// The delay memory is a caller-provided int16 arena over sc::DelayLine —
// static SRAM in firmware, a std::vector sized per engine rate in Rack. Size
// it for kFlangerBufferSec of audio (flangerArenaSamples()).
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop.

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Sweep source (BUTTON short-press cycles these on hardware).
enum FlangerShape : uint8_t {
  FLANGER_TRI = 0,   // triangle LFO
  FLANGER_SINE = 1,  // sine LFO
  FLANGER_ENV = 2,   // envelope follower drives the sweep
  FLANGER_SHAPE_COUNT = 3
};

// POT1 -> LFO rate in Hz: exponential taper 0.02 Hz (pot=0) .. 5 Hz (pot=1).
inline float flangerRateHz(float pot01) {
  return 0.02f * powf(250.0f, clampf(pot01, 0.0f, 1.0f));
}

// Full-CCW rate pot = manual mode (sweep by hand instead of the LFO).
inline bool flangerManual(float pot01) {
  return pot01 < 0.03f;
}

// POT2 -> signed feedback. Centre deadzone = none; CCW negative, CW positive.
// Magnitude stays below oscillation (0.9) until the last 5% of travel, which
// pushes on to 1.05 — let it scream deliberately (the loop soft-sat limits it).
inline float flangerFeedback(float pot01) {
  const float t = clampf(pot01, 0.0f, 1.0f) * 2.0f - 1.0f;
  const float mag = fabsf(t);
  if (mag < 0.08f) return 0.0f;
  const float m = (mag - 0.08f) / 0.92f;
  const float fb = 0.9f * m + (m > 0.95f ? (m - 0.95f) * 3.0f : 0.0f);
  return t < 0.0f ? -fb : fb;
}

// Sweep bounds (exponential between these), and the arena that holds them.
constexpr float kFlangerMinMs = 0.2f;
constexpr float kFlangerMaxMs = 12.0f;
constexpr float kFlangerBufferSec = 0.020f;

inline uint32_t flangerArenaSamples(float fsHz) {
  return (uint32_t)(kFlangerBufferSec * fsHz) + 16;
}

struct FlangerCore {
  // Parameters (write directly; see the mappers above).
  float rateHz = 0.3f;        // LFO rate (ignored in manual / env modes)
  bool manual = false;        // true: sweep position comes from manualPos
  float manualPos = 0.5f;     // 0..1 hand sweep (manual mode)
  float feedback = 0.0f;      // signed, see flangerFeedback()
  float depth = 0.7f;         // 0..1 sweep range
  float wet = 0.5f;           // 0 dry .. 1 fully wet
  uint8_t shape = FLANGER_TRI;

  // State.
  DelayLine line;
  float phase = 0.0f;     // LFO phase 0..1
  float envFollow = 0.0f; // envelope follower level
  float fbSample = 0.0f;  // last tap, fed back into the line
  float lastPos = 0.0f;   // last sweep position 0..1 (for the LED)
  DcBlocker fbDc;

  // `buf`/`n`: caller-owned arena (flangerArenaSamples() long).
  void init(int16_t* buf, uint32_t n) {
    line.init(buf, n);
    reset();
  }

  void reset() {
    line.clear();
    phase = 0.0f;
    envFollow = 0.0f;
    fbSample = 0.0f;
    lastPos = 0.0f;
    fbDc = DcBlocker();
  }

  // IN1 rising edge: restart the LFO sweep.
  void retrigger() { phase = 0.0f; }

  // 0..1 brightness for the panel LED — follows the sweep position.
  float ledLevel() const { return lastPos; }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    // Envelope follower (fast attack, slow release) — the ENV sweep source.
    const float mag = fabsf(in);
    envFollow += (mag - envFollow) *
                 onePoleCoef(mag > envFollow ? 30.0f : 2.0f, dt);

    // Sweep position 0..1.
    float pos;
    if (manual) {
      pos = manualPos;
    } else if (shape == FLANGER_ENV) {
      pos = clampf(envFollow * 2.0f, 0.0f, 1.0f);
    } else {
      phase += rateHz * dt;
      phase -= floorf(phase);
      pos = (shape == FLANGER_SINE)
                ? 0.5f - 0.5f * cosf(kTwoPi * phase)
                : 1.0f - fabsf(2.0f * phase - 1.0f);  // triangle
    }
    pos *= depth;
    lastPos = pos;

    // Exponential sweep: pos=0 -> 12 ms, pos=1 -> 0.2 ms (sweeping "up").
    const float delayMs =
        kFlangerMaxMs * powf(kFlangerMinMs / kFlangerMaxMs, pos);

    // Feedback around the delay, then the comb: wet tap + dry sum.
    line.write(softSat(in + feedback * fbSample));
    const float tap = line.read(clampf(delayMs * 0.001f / dt, 1.0f, 1e9f));
    fbSample = fbDc.process(tap);

    return in + (tap - in) * wet;
  }
};

}  // namespace sc
