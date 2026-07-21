#pragma once

// Stutter — clock-aware beat-repeat / glitch core.
//
// Used by:
//   - firmwares/mod2-stutter/mod2-stutter.ino
//   - rack-plugins/src/mod2-stutter.cpp
//
// Always-recording circular buffer (sc::DelayLine over a caller-provided int16
// arena). While idle the effect passes the dry input through and keeps the
// buffer fresh. When the stutter gate engages it LOCKS the last `sliceSec`
// seconds of audio and machine-guns that slice in musical divisions — the
// classic beat-repeat performance effect. Four flavours (BUTTON short-press
// cycles them on hardware):
//   STRAIGHT   — clean repeats of the locked slice.
//   DECAY      — each repeat is quieter (gain x decayFactor).
//   PITCH_UP   — each repeat reads faster (rate x k>1): the classic riser.
//   PITCH_DOWN — each repeat reads slower (rate x k<1): the faller.
// A per-division probability (POT2 shift layer on hardware) rolls at each
// division while engaged: when it fails, that division passes the live input
// (and the buffer keeps recording) — instant IDM. The LED flashes per repeat.
//
// Anti-click: the slice is read through a short raised-cosine grain window so
// the loop seam and the engage/release transition never click; engaging and
// releasing additionally crossfade dry<->wet over kEngageSec. When a division
// is skipped by probability the buffer records again, so the next capture
// grabs current audio (the "resume live at current time" behaviour — the
// documented default for the README's open question, dropout-free).
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop. Sample-rate independent: process(in, dt)
// advances the engine by the caller-supplied dt (seconds).

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Repeat flavour (BUTTON short-press cycles these on hardware).
enum StutterMode : uint8_t {
  STUTTER_STRAIGHT = 0,    // clean machine-gun repeats
  STUTTER_DECAY = 1,       // each repeat quieter
  STUTTER_PITCH_UP = 2,    // each repeat faster — riser
  STUTTER_PITCH_DOWN = 3,  // each repeat slower — faller
  STUTTER_MODE_COUNT = 4
};

// POT1 (unclocked) -> free repeat length in seconds: exponential 20 ms .. 1 s.
inline float stutterFreeSec(float pot01) {
  return 0.020f * powf(50.0f, clampf(pot01, 0.0f, 1.0f));  // 0.02 .. 1.0 s
}

// POT1 (clocked) -> musical division as a ratio of the measured clock period.
// With a quarter-note clock these span a 1/32 note (0.125 beat) up to a bar
// (4 beats). The platform multiplies the measured period by this ratio.
constexpr float kStutterClockRatios[8] = {
    0.125f, 0.25f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 1.0f, 2.0f, 4.0f};

inline float stutterClockRatio(float pot01) {
  int idx = (int)(clampf(pot01, 0.0f, 1.0f) * 7.999f);
  return kStutterClockRatios[idx];
}

// POT2 -> per-repeat gain multiplier for DECAY (1.0 = no decay .. 0.5 = -6 dB
// per repeat at full travel; the README's "gain x 0.8" sits around pot 0.4).
inline float stutterDecayFactor(float amount01) {
  return lerpf(1.0f, 0.5f, clampf(amount01, 0.0f, 1.0f));
}

// POT2 -> per-repeat pitch step for the ramp flavours (rate x (1 +/- step)).
// Full travel ~ +/-12% per repeat; the README's k ~ 1.06 / 0.94 is pot ~ 0.5.
inline float stutterPitchStep(float amount01) {
  return 0.12f * clampf(amount01, 0.0f, 1.0f);
}

// Longest slice the effect can lock; also sizes the always-recording buffer.
// 1.3 s covers a full bar at 60 BPM in 4/4 with headroom.
constexpr float kStutterBufferSec = 1.3f;

inline uint32_t stutterArenaSamples(float fsHz) {
  return (uint32_t)(kStutterBufferSec * fsHz) + 16;
}

struct StutterCore {
  // Parameters (write directly; see the mappers above).
  float sliceSec = 0.125f;              // repeat length target (seconds)
  float amount = 0.4f;                  // POT2 behaviour amount (0..1)
  float wet = 1.0f;                     // 0 dry .. 1 fully wet
  float probability = 1.0f;             // per-division trigger chance (1 = always)
  uint8_t mode = STUTTER_STRAIGHT;      // StutterMode
  bool engaged = false;                 // stutter gate/latch (platform-combined)

  // Anti-click timings.
  static constexpr float kSeamSec = 0.003f;    // grain-window edge (loop seam)
  static constexpr float kEngageSec = 0.004f;  // dry<->wet engage crossfade

  // State.
  DelayLine line;
  float sliceSecS = 0.125f;   // smoothed slice length (no zipper)
  float amountS = 0.4f;       // smoothed behaviour amount
  float wetS = 1.0f;          // smoothed wet/dry
  float divTimer = 0.0f;      // seconds into the current division
  float phase = 0.0f;         // slice read position (samples)
  float sliceLen = 1.0f;      // locked slice length (samples)
  float rate = 1.0f;          // playback rate (pitch ramp)
  float gain = 1.0f;          // per-repeat decay gain
  float engEnv = 0.0f;        // 0 dry .. 1 stutter (engage crossfade)
  float ledEnv = 0.0f;        // LED flash envelope (per repeat)
  int repeatIndex = 0;        // repeats since capture
  bool repeatActive = false;  // probability roll for the current division
  bool wasEngaged = false;    // engage edge detect
  uint32_t rng = 0x51557321u; // deterministic PRNG (portable, seedable)

  // `buf`/`n`: caller-owned arena (stutterArenaSamples() long).
  void init(int16_t* buf, uint32_t n) {
    line.init(buf, n);
    reset();
  }

  void reset() {
    line.clear();
    sliceSecS = sliceSec;
    amountS = amount;
    wetS = wet;
    divTimer = 0.0f;
    phase = 0.0f;
    sliceLen = 1.0f;
    rate = 1.0f;
    gain = 1.0f;
    engEnv = 0.0f;
    ledEnv = 0.0f;
    repeatIndex = 0;
    repeatActive = false;
    wasEngaged = false;
    rng = 0x51557321u;
  }

  // 0..1 brightness for the panel LED — flashes on each repeat.
  float ledLevel() const { return ledEnv; }

  // One 0..1 probability roll (portable, platform-identical sequence).
  float roll01() {
    return (float)(xorshift32(rng) >> 8) * (1.0f / 16777216.0f);
  }

  // Read the locked slice at fractional position `pos` (0 = oldest sample of
  // the slice, sliceLen = newest). The buffer write is frozen while a repeat
  // plays, so the slice stays addressable relative to line.writeIdx.
  float readSlice(float pos) const {
    const float delay = sliceLen - pos;  // pos 0 -> oldest (delay sliceLen)
    return line.read(clampf(delay, 1.0f, (float)(line.len - 2)));
  }

  // Lock the last sliceLen samples as the repeat slice and reset ramp state.
  void capture(float dt) {
    sliceLen = clampf(sliceSecS / dt, 8.0f, (float)(line.len - 2));
    phase = 0.0f;
    rate = 1.0f;
    gain = 1.0f;
    repeatIndex = 0;
  }

  // Apply the per-repeat flavour modification at a division boundary.
  void advanceFlavour() {
    repeatIndex++;
    switch (mode) {
      case STUTTER_DECAY:
        gain = clampf(gain * stutterDecayFactor(amountS), 0.05f, 1.0f);
        break;
      case STUTTER_PITCH_UP:
        rate = clampf(rate * (1.0f + stutterPitchStep(amountS)), 0.25f, 4.0f);
        break;
      case STUTTER_PITCH_DOWN:
        rate = clampf(rate * (1.0f - stutterPitchStep(amountS)), 0.25f, 4.0f);
        break;
      default:  // STUTTER_STRAIGHT: clean repeats (amount unused here)
        break;
    }
  }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    // --- one-pole smooth every user parameter (no zipper) ----------------
    const float sm = onePoleCoef(30.0f, dt);
    sliceSecS += (sliceSec - sliceSecS) * sm;
    amountS += (amount - amountS) * sm;
    wetS += (wet - wetS) * sm;

    // --- engage edge: force an immediate division so capture happens now.
    // Engage is deliberately immediate (not deferred to the next clock-grid
    // boundary): lowest latency for gate mashing. divTimer restarts the
    // division clock from this instant, so repeats stay on an internal grid
    // anchored at the engage point.
    if (engaged && !wasEngaged) {
      divTimer = sliceSecS;  // trip the boundary on this sample
      repeatActive = false;  // boundary logic decides via the probability roll
    }
    if (!engaged) {
      repeatActive = false;
      divTimer = 0.0f;
    }
    wasEngaged = engaged;

    // --- division boundary: reroll probability, (re)capture, advance ramp.
    if (engaged) {
      divTimer += dt;
      if (divTimer >= sliceSecS) {
        divTimer -= sliceSecS;
        if (divTimer >= sliceSecS) divTimer = 0.0f;  // clamp on huge dt
        const bool wasPlaying = repeatActive;
        repeatActive = (probability >= 0.999f) || (roll01() < probability);
        if (repeatActive) {
          if (!wasPlaying)
            capture(dt);        // fresh grab: lock current audio
          else
            advanceFlavour();   // continuing: decay / pitch ramp this repeat
          phase = 0.0f;         // restart the slice on the grid
          ledEnv = 1.0f;        // flash the LED for this repeat
        }
      }
    }

    // --- read the slice (frozen) or record + pass the live input --------
    float wetVoice;
    if (engaged && repeatActive) {
      // Buffer frozen (no write) so the locked slice stays intact.
      if (phase >= sliceLen) phase -= sliceLen;  // intra-division wrap (pitch up)

      // Short raised-cosine grain window on both slice edges: the read fades
      // in from silence and out to silence, so the seam at the phase reset
      // and at engage/release is click-free.
      const float edge = clampf(kSeamSec / dt, 1.0f, sliceLen * 0.5f);
      float win = 1.0f;
      if (phase < edge)
        win = raisedCosine(phase / edge);
      else if (phase > sliceLen - edge)
        win = raisedCosine((sliceLen - phase) / edge);

      const float s = readSlice(phase) * gain * win;
      wetVoice = lerpf(in, s, wetS);
      phase += rate;
    } else {
      // Idle or probability-skipped: keep the buffer fresh, pass the dry input.
      line.write(in);
      wetVoice = in;
    }

    // --- dry<->wet engage crossfade (extra click safety on the boundary).
    const float engStep = clampf(dt / kEngageSec, 0.0f, 1.0f);
    const float engTarget = (engaged && repeatActive) ? 1.0f : 0.0f;
    engEnv += (engTarget - engEnv) * engStep;
    if (engEnv < 1e-15f) engEnv = 0.0f;  // denormal flush

    // --- LED flash decay -------------------------------------------------
    ledEnv -= ledEnv * clampf(dt / 0.06f, 0.0f, 1.0f);
    if (ledEnv < 1e-15f) ledEnv = 0.0f;

    return lerpf(in, wetVoice, engEnv);
  }
};

}  // namespace sc
