#pragma once

// Freeze — buffer capture & seamless loop core.
//
// Used by:
//   - firmwares/mod2-freeze/mod2-freeze.ino
//   - rack-plugins/src/mod2-freeze.cpp
//
// The simplest "deep" effect: an always-recording circular buffer captures the
// incoming audio; FREEZE stops the write head and plays a windowed loop out of
// the frozen buffer, so a held chord / drone survives even after the input goes
// silent. POT1 sets the loop length (5 ms .. full buffer — short lengths turn a
// tone into a pitched buzz-drone), POT2 scans the loop window through the
// captured buffer (position 0 = the freshest slice, right behind the freeze
// point; position 1 = the oldest). A shift layer (BUTTON+POT) sets the frozen
// vs. live mix and the loop-seam crossfade length. Three playback modes cycle on
// a short button press: Forward / Ping-pong / Half-speed (a free octave-down pad
// via a 0.5x read rate). A re-capture trigger grabs fresh audio without leaving
// freeze.
//
// The loop seam is the whole quality battle: an equal-power crossfade blends the
// loop tail into a guard region just past the loop end, so the wrap never
// clicks. Engaging / releasing freeze also crossfades live<->frozen over ~10 ms.
//
// The capture memory is a caller-provided int16 arena over a private circular
// buffer (no heap, per the SynthCore rules): the firmware hands it a ~400 KB
// static SRAM arena (freezeArenaSamples() ~ 5.5 s at 36.6 kHz — the RP2350 RAM
// budget), the Rack port a std::vector sized for the engine rate. int16 storage
// halves the RAM vs. float and matches the PWM path's resolution.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop. Sample-rate independent: process(in, dt) advances
// the loop by the caller's dt, and reads are in buffer samples so pitch matches
// on any host.

#include <math.h>
#include <stdint.h>

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Playback direction / rate (BUTTON short-press cycles these on hardware).
enum FreezePlayMode : uint8_t {
  FREEZE_FORWARD = 0,    // loop forward at pitch
  FREEZE_PINGPONG = 1,   // bounce forward<->backward (click-free turnarounds)
  FREEZE_HALFSPEED = 2,  // forward at 0.5x — octave-down pad
  FREEZE_MODE_COUNT = 3
};

// Full capture length (also the longest loop). ~5.5 s fills ~400 KB of int16 at
// the MOD2 audio rate — the RP2350's usable audio RAM (see mod2-fx/README.md).
constexpr float kFreezeBufferSec = 5.5f;

// Shortest loop window; below this the loop is a pitched buzz rather than audio.
constexpr float kFreezeMinLoopSec = 0.005f;

// Longest loop-seam crossfade ("smeared wash"); the minimum is kept at 5 ms so
// even a "tight" loop still crossfades and never clicks.
constexpr float kFreezeMaxXfadeSec = 0.250f;

// Arena size (samples) for a given sample rate. +guard so the seam crossfade's
// look-ahead read always lands inside the recorded ring.
inline uint32_t freezeArenaSamples(float fsHz) {
  return (uint32_t)(kFreezeBufferSec * fsHz) + 16;
}

// POT1 -> loop length in seconds: exponential taper 5 ms (pot=0) .. maxSec.
inline float freezeLoopLenSec(float pot01, float maxSec) {
  return kFreezeMinLoopSec *
         powf(maxSec / kFreezeMinLoopSec, clampf(pot01, 0.0f, 1.0f));
}

// BUTTON+POT2 -> seam crossfade length in seconds: 5 ms .. 250 ms.
inline float freezeXfadeSec(float pot01) {
  return kFreezeMinLoopSec +
         clampf(pot01, 0.0f, 1.0f) * (kFreezeMaxXfadeSec - kFreezeMinLoopSec);
}

// Flush sub-denormal filter state to zero (portable, no <cfloat> / intrinsics).
inline float freezeFlush(float x) {
  return (x < 1e-18f && x > -1e-18f) ? 0.0f : x;
}

struct FreezeCore {
  // Parameters (write directly; see the mappers above).
  float loopLen = 0.5f;             // target loop length (seconds)
  float position = 0.0f;            // 0..1 window position in the buffer
  float xfadeLen = 0.030f;          // seam crossfade length (seconds)
  float wet = 1.0f;                 // frozen vs. live blend (0 dry .. 1 frozen)
  uint8_t mode = FREEZE_FORWARD;    // FreezePlayMode
  bool freeze = false;              // gate/latch: stop the write head & loop

  static constexpr float kEnterSec = 0.010f;  // live<->frozen crossfade time

  // Capture memory (caller-owned; freezeArenaSamples() long).
  int16_t* buf = nullptr;
  uint32_t cap = 0;
  uint32_t writeIdx = 0;   // next slot to write while recording

  // State.
  bool frozen = false;         // internal latch mirroring `freeze` edges
  float anchor = 0.0f;         // buffer index of the newest frozen sample
  float phase = 0.0f;          // read position within the loop (samples)
  int8_t dir = 1;              // ping-pong direction
  float freezeMix = 0.0f;      // 0 live .. 1 frozen (kEnterSec ramp)
  uint32_t recaptureLeft = 0;  // samples of fresh audio still to grab
  bool recaptureReq = false;   // set by recapture(), consumed in process()

  // Smoothed parameters (one-pole, no zipper).
  float loopLenS = 0.5f;
  float positionS = 0.0f;
  float xfadeS = 0.030f;
  float wetS = 1.0f;
  bool primed = false;

  // Live-input level for the LED.
  float inEnv = 0.0f;

  // `b`/`n`: caller-owned arena (freezeArenaSamples() long).
  void init(int16_t* b, uint32_t n) {
    buf = b;
    cap = n;
    reset();
  }

  void reset() {
    if (buf)
      for (uint32_t i = 0; i < cap; i++) buf[i] = 0;
    writeIdx = 0;
    frozen = false;
    anchor = 0.0f;
    phase = 0.0f;
    dir = 1;
    freezeMix = 0.0f;
    recaptureLeft = 0;
    recaptureReq = false;
    inEnv = 0.0f;
    primed = false;
  }

  // Grab fresh audio into the buffer without leaving freeze (IN2 on hardware).
  // ISR-safe: just latches a request the next process() consumes.
  void recapture() { recaptureReq = true; }

  // 0..1 brightness for the panel LED — solid when frozen, follows the input
  // level while live.
  float ledLevel() const {
    return clampf(freezeMix + (1.0f - freezeMix) * inEnv, 0.0f, 1.0f);
  }

  // Read the frozen buffer at fractional index `idx` (wrapped into the ring),
  // linear interpolation, int16 -> float.
  inline float readBuf(float idx) const {
    const float c = (float)cap;
    idx -= c * floorf(idx / c);  // wrap into [0, cap)
    uint32_t i0 = (uint32_t)idx;
    if (i0 >= cap) i0 = 0;
    uint32_t i1 = i0 + 1;
    if (i1 >= cap) i1 = 0;
    const float fr = idx - floorf(idx);
    const float s0 = (float)buf[i0] * (1.0f / 32768.0f);
    const float s1 = (float)buf[i1] * (1.0f / 32768.0f);
    return s0 + (s1 - s0) * fr;
  }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    if (!buf || cap < 8) return in;

    // --- parameter smoothing (one-pole ~20 Hz) -------------------------
    if (!primed) {
      loopLenS = loopLen;
      positionS = position;
      xfadeS = xfadeLen;
      wetS = wet;
      primed = true;
    }
    const float pc = onePoleCoef(20.0f, dt);
    loopLenS += (loopLen - loopLenS) * pc;
    positionS += (position - positionS) * pc;
    xfadeS += (xfadeLen - xfadeS) * pc;
    wetS += (wet - wetS) * pc;

    // --- input level (LED) ---------------------------------------------
    inEnv += (fabsf(in) - inEnv) * onePoleCoef(15.0f, dt);
    inEnv = freezeFlush(inEnv);

    // --- freeze edge tracking ------------------------------------------
    // Rising edge: snapshot the newest sample as the loop anchor and stop the
    // write head. Falling edge: resume recording. While live, keep the anchor
    // trailing the write head so a fresh freeze is ready instantly.
    if (freeze && !frozen) {
      frozen = true;
      anchor = (float)writeIdx;
      phase = 0.0f;
      dir = 1;
    } else if (!freeze && frozen && recaptureLeft == 0) {
      frozen = false;
    }

    // --- recapture: grab a fresh loop's worth without leaving freeze ----
    if (recaptureReq) {
      recaptureReq = false;
      if (frozen) {
        // Refresh at least the loop window (bounded to the buffer).
        uint32_t grab = (uint32_t)(loopLenS / dt) + 2;
        if (grab > cap - 4) grab = cap - 4;
        recaptureLeft = grab;
      }
    }

    // --- record head ----------------------------------------------------
    const bool writing = !frozen || recaptureLeft > 0;
    if (writing) {
      buf[writeIdx] = (int16_t)(clampf(in, -1.0f, 1.0f) * 32767.0f);
      if (++writeIdx >= cap) writeIdx = 0;
      if (!frozen) anchor = (float)writeIdx;  // trail the head while live
      if (recaptureLeft > 0 && --recaptureLeft == 0)
        anchor = (float)writeIdx;  // re-anchor onto the fresh audio
    }

    // --- freeze enter/exit crossfade (~10 ms) ---------------------------
    const float target = frozen ? 1.0f : 0.0f;
    freezeMix += (target - freezeMix) * clampf(dt / kEnterSec, 0.0f, 1.0f);
    freezeMix = freezeFlush(freezeMix);

    // --- loop geometry (all in buffer samples) -------------------------
    const float minLen = clampf(kFreezeMinLoopSec / dt, 2.0f, (float)(cap - 4));
    float L = clampf(loopLenS / dt, minLen, (float)(cap - 4));
    // Seam crossfade must fit inside the loop; leave headroom.
    float X = clampf(xfadeS / dt, 1.0f, L * 0.5f);
    // Window slides through whatever is left after the loop + seam guard, so the
    // look-ahead tail read (start + L + X) always stays inside the recorded ring
    // and never wraps onto stale audio.
    float span = (float)(cap - 2) - (L + X);
    if (span < 0.0f) span = 0.0f;
    const float start = anchor - (L + X) - clampf(positionS, 0.0f, 1.0f) * span;

    // --- loop read ------------------------------------------------------
    float loopOut;
    if (mode == FREEZE_PINGPONG) {
      loopOut = readBuf(start + phase);
    } else {
      // Forward / half-speed: equal-power crossfade over the first X samples
      // after each wrap. The fading-out tail (start + L + phase) continues
      // seamlessly from where the previous cycle ended (start + L), so the loop
      // boundary never clicks.
      if (phase < X) {
        const float t = phase / X;             // 0..1 across the seam
        const float g = t * (kPi * 0.5f);
        loopOut = sinf(g) * readBuf(start + phase) +
                  cosf(g) * readBuf(start + L + phase);
      } else {
        loopOut = readBuf(start + phase);
      }
    }

    // --- advance the read position -------------------------------------
    if (mode == FREEZE_PINGPONG) {
      phase += (float)dir;
      if (phase >= L) { phase = L - (phase - L); dir = -1; }
      if (phase <= 0.0f) { phase = -phase; dir = 1; }
      phase = clampf(phase, 0.0f, L);
    } else {
      phase += (mode == FREEZE_HALFSPEED) ? 0.5f : 1.0f;
      if (phase >= L) phase -= L;
    }

    // --- frozen vs. live blend -----------------------------------------
    const float effWet = clampf(wetS, 0.0f, 1.0f) * freezeMix;
    return in + (loopOut - in) * effWet;
  }
};

}  // namespace sc
