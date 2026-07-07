#pragma once

// Delay FX — clean/dirty mono digital delay core.
//
// Used by:
//   - firmwares/mod2-delay/mod2-delay.ino
//   - rack-plugins/src/mod2-delay.cpp
//
// Bread-and-butter delay over sc::DelayLine (caller-provided int16 buffer).
// Fractional read with linear interpolation. Time changes crossfade between
// two read heads (~35 ms raised-cosine) so sweeping the knob never pitch-zips.
// Feedback reaches ~110% and the write is always soft-saturated, so it
// self-oscillates musically instead of clipping the int16 buffer. Two modes:
//   CLEAN — flat feedback path.
//   DIRTY — feedback through a soft saturator + gentle one-pole low-pass,
//           darkening each repeat (bucket-brigade flavour).
// A hold gate (IN2 on hardware) mutes the input and pins feedback at unity for
// momentary infinite repeats; on release the held audio spills over and decays
// at the knob's feedback setting.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop.

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Feedback-path colour (BUTTON short-press toggles these on hardware).
enum DelayFxMode : uint8_t {
  DELAYFX_CLEAN = 0,  // transparent repeats
  DELAYFX_DIRTY = 1,  // saturated + darkened repeats
  DELAYFX_MODE_COUNT = 2
};

// POT1 -> delay time in seconds: exponential taper 10 ms (pot=0) .. maxSec.
inline float delayFxTimeSec(float pot01, float maxSec) {
  return 0.010f * powf(maxSec / 0.010f, clampf(pot01, 0.0f, 1.0f));
}

// POT2 -> feedback amount: 0 .. ~110% (soft-limited into self-oscillation).
inline float delayFxFeedback(float pot01) {
  return 1.1f * clampf(pot01, 0.0f, 1.0f);
}

// Clock-synced ratios: with a clock on IN1, POT1 picks a musical division /
// multiple of the measured period instead of an absolute time.
constexpr float kDelayFxClockRatios[8] = {
    0.25f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 0.75f, 1.0f, 1.5f, 2.0f};

inline float delayFxClockRatio(float pot01) {
  int idx = (int)(clampf(pot01, 0.0f, 1.0f) * 7.999f);
  return kDelayFxClockRatios[idx];
}

struct DelayFxCore {
  // Parameters (write directly; see the mappers above).
  float timeSec = 0.5f;              // target delay time
  float feedback = 0.4f;             // 0 .. ~1.1
  float wet = 0.5f;                  // 0 dry .. 1 fully wet
  uint8_t mode = DELAYFX_CLEAN;      // DelayFxMode
  bool hold = false;                 // gate: infinite repeat, input muted

  static constexpr float kXfadeSec = 0.035f;   // read-head crossfade length
  static constexpr float kRetuneRatio = 0.005f;  // time change that retriggers

  // State.
  DelayLine line;
  float headA = 4410.0f;  // active read delay (samples)
  float headB = 4410.0f;  // incoming read delay while crossfading
  float xfade = 1.0f;     // 0 -> 1 crossfade position (1 = settled on A)
  float fbLp = 0.0f;      // DIRTY one-pole low-pass state
  bool primed = false;    // first process() snaps the heads to the target

  // `buf`/`n`: caller-owned arena; usable delay is n-2 samples.
  void init(int16_t* buf, uint32_t n) {
    line.init(buf, n);
    reset();
  }

  void reset() {
    line.clear();
    xfade = 1.0f;
    fbLp = 0.0f;
    primed = false;  // heads snap on the next process(), once dt is known
  }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    const float maxDelay = (float)(line.len - 2);
    const float target = clampf(timeSec / dt, 1.0f, maxDelay);
    if (!primed) {
      headA = headB = target;
      primed = true;
    }

    // Retune = raised-cosine crossfade from the settled head to a new one at
    // the target delay. Only start a new fade once the last one finishes, so
    // a moving knob chases the target in clickless ~35 ms hops.
    if (xfade >= 1.0f && fabsf(target - headA) > headA * kRetuneRatio) {
      headB = target;
      xfade = 0.0f;
    }
    float echo;
    if (xfade < 1.0f) {
      xfade += dt / kXfadeSec;
      if (xfade >= 1.0f) {
        headA = headB;
        xfade = 1.0f;
        echo = line.read(headA);
      } else {
        const float mix = raisedCosine(xfade);
        echo = lerpf(line.read(headA), line.read(headB), mix);
      }
    } else {
      echo = line.read(headA);
    }

    // Feedback path: DIRTY saturates and darkens each repeat pass.
    float fb = echo;
    if (mode == DELAYFX_DIRTY) {
      fb = softClipTanh(fb * 1.5f, 2.0f);
      fbLp += (fb - fbLp) * onePoleCoef(2500.0f, dt);
      fb = fbLp;
    }

    // Hold gate: input muted and the echo recirculates raw — the saturator
    // would shave a little level every pass and the "infinite" repeat would
    // sag. (DelayLine::write still clamps, so the buffer stays safe.)
    if (hold) {
      line.write(echo);
    } else {
      // The write is soft-saturated: >100% feedback self-oscillates into a
      // warm limit instead of wrapping the int16 buffer.
      line.write(softSat(in + fb * feedback));
    }

    return in + (echo - in) * wet;
  }
};

}  // namespace sc
