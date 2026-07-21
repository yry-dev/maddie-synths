#pragma once

// Karplus — dedicated Karplus-Strong plucked-string voice / audio processor.
//
// Used by:
//   - firmwares/mod2-karplus/mod2-karplus.ino
//   - rack-plugins/src/mod2-karplus.cpp
//
// A single tuned feedback delay line (the "string") is excited either by an
// internal shaped noise burst (a pluck) or by external audio on the CV jack
// (a continuous "bow"). It is the Karplus mode of FluxCore promoted to a
// dedicated engine, upgraded with fractional-delay tuning (Flux's integer
// period drifts sharp up high), a colourable exciter, an in-loop damping
// filter, a DC blocker and an energy limiter. Three modes:
//   PLUCK — triggered plucks ring and decay naturally (feedback < 1).
//   BOW   — the external input is summed continuously into the loop at high
//           gain, so the string is driven/scraped and "sings" as long as
//           audio is patched (feed it noise or a pad and it hums a pitch).
//   DRONE — near-unity feedback: the string self-sustains; the energy limiter
//           keeps it from blowing up.
//
// Control model (matches the firmware):
//   pitchHz  — fundamental, semitone-quantized over 4 octaves (karplusPitchHz)
//   damping  — 0 bright / long sustain .. 1 dark / short (loop LP + feedback)
//   colour   — excitation brightness, 0 dark noise burst .. 1 bright
//   wet      — 0 = the raw excitation signal (dry), 1 = the string (wet)
//   dampGate — palm-mute: chokes the loop while true (IN2)
// pluck() fires the internal exciter (IN1 trigger / button long-press); a
// retrigger adds a fresh burst *into* the still-ringing line rather than
// clearing it, so fast replucks never click.
//
// The delay memory is a caller-provided int16 arena over sc::DelayLine —
// static SRAM in firmware, a std::vector sized per engine rate in Rack. Size
// it for one period at the lowest pitch (karplusArenaSamples()). The int16
// storage also flushes the loop signal to true silence once the string decays
// below one LSB (~3e-5), so a stopped string is exactly zero (denormal-safe).
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop.

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Voicing (BUTTON short-press cycles these on hardware).
enum KarplusMode : uint8_t {
  KARPLUS_PLUCK = 0,   // triggered plucks decay naturally
  KARPLUS_BOW = 1,     // continuous external-audio drive
  KARPLUS_DRONE = 2,   // near-unity loop, self-sustaining
  KARPLUS_MODE_COUNT = 3
};

// Pitch floor: A1. POT1 quantizes to semitones over 4 octaves (A1..A5) — it's
// a melodic module, same range/quantization as ResonatorCore.
constexpr float kKarplusBaseHz = 55.0f;

inline float karplusPitchHz(float pot01) {
  const float semis = floorf(clampf(pot01, 0.0f, 1.0f) * 48.0f + 0.5f);
  return kKarplusBaseHz * powf(2.0f, semis * (1.0f / 12.0f));
}

// Arena size for `fsHz`: one full period at the lowest pitch, plus guard
// samples for the fractional read. ~1.3 KB at 36.6 kHz (well under the 2 KB
// per-string budget).
inline uint32_t karplusArenaSamples(float fsHz) {
  return (uint32_t)(fsHz / kKarplusBaseHz) + 8;
}

struct KarplusCore {
  // Parameters (write directly; smoothed internally — no zipper).
  float pitchHz = 220.0f;          // fundamental (semitone-quantized)
  float damping = 0.5f;            // 0 bright/long .. 1 dark/short
  float colour = 0.5f;             // excitation brightness (0 dark .. 1 bright)
  float wet = 1.0f;                // 0 dry (excitation) .. 1 wet (string)
  uint8_t mode = KARPLUS_PLUCK;    // KarplusMode
  bool dampGate = false;           // palm-mute the loop while true

  // State.
  DelayLine line;                  // the string
  float loopLp = 0.0f;             // in-loop damping (brightness) filter state
  DcBlocker dc;                    // in-loop DC blocker -> decays to true zero
  float exciteEnv = 0.0f;          // pluck burst envelope
  float exciteLp = 0.0f;           // burst colour filter state
  uint32_t rng = 0x4B504C31u;      // "KPL1"
  float energy = 0.0f;             // smoothed |string| for LED + limiter

  // Smoothed parameter shadows (one-pole, anti-zipper).
  float sPitchHz = 220.0f, sDamping = 0.5f, sColour = 0.5f, sWet = 1.0f;

  // Cached per-dt work.
  float lastDt = -1.0f;
  float exciteCoef = 0.99f;        // per-sample burst decay (~3 ms)

  // `buf`/`n`: caller-owned arena (karplusArenaSamples() long).
  void init(int16_t* buf, uint32_t n) {
    line.init(buf, n);
    reset();
  }

  void reset() {
    line.clear();
    loopLp = 0.0f;
    dc.x1 = dc.y1 = 0.0f;
    exciteEnv = 0.0f;
    exciteLp = 0.0f;
    energy = 0.0f;
    sPitchHz = pitchHz;
    sDamping = damping;
    sColour = colour;
    sWet = wet;
    lastDt = -1.0f;
  }

  // Fire the internal exciter (IN1 trigger / button long-press). Adds a fresh
  // burst into the loop; does not clear the line, so replucks don't click.
  void pluck() { exciteEnv = 1.0f; }
  void strike() { pluck(); }  // alias (matches the ResonatorCore vocabulary)

  // 0..1 brightness for the panel LED — follows the string's energy.
  float ledLevel() const { return clampf(energy * 3.0f, 0.0f, 1.0f); }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    // --- parameter smoothing (~20 Hz one-pole; kills zipper on every knob) --
    const float ps = onePoleCoef(20.0f, dt);
    sPitchHz += (pitchHz - sPitchHz) * ps;
    sDamping += (damping - sDamping) * ps;
    sColour += (colour - sColour) * ps;
    sWet += (wet - sWet) * ps;

    if (dt != lastDt) {
      exciteCoef = expf(-dt / 0.003f);  // ~3 ms noise-burst envelope
      lastDt = dt;
    }

    const float fs = 1.0f / dt;

    // --- excitation ------------------------------------------------------
    // Internal pluck: a short noise burst, colour = one-pole brightness of the
    // noise (dark low-passed .. bright raw). Injected additively into the loop.
    float burst = 0.0f;
    if (exciteEnv > 0.0001f) {
      const float nz = noise1f(rng);
      const float cutoff = lerpf(500.0f, 12000.0f, clampf(sColour, 0.0f, 1.0f));
      exciteLp += (nz - exciteLp) * onePoleCoef(cutoff, dt);
      const float shaped = lerpf(exciteLp, nz, clampf(sColour, 0.0f, 1.0f));
      burst = shaped * exciteEnv;
      exciteEnv *= exciteCoef;
    }

    // External audio drives the string continuously. Bow leans on it hard so
    // patched audio makes the string sing; Pluck/Drone still let a hot input
    // excite the string quietly (the "scraped string" trick).
    const float extGain = (mode == KARPLUS_BOW) ? 0.5f : 0.15f;
    const float excitation = burst + in * extGain;

    // --- read the string at the tuned (fractional) delay -----------------
    const float delaySamples =
        clampf(fs / sPitchHz, 2.0f, (float)(line.len - 2));
    const float s = line.read(delaySamples);  // the string output tap

    // --- in-loop damping low-pass (string brightness) --------------------
    const float brightHz = lerpf(6500.0f, 700.0f, clampf(sDamping, 0.0f, 1.0f));
    loopLp += (s - loopLp) * onePoleCoef(brightHz, dt);
    if (fabsf(loopLp) < 1e-18f) loopLp = 0.0f;  // denormal flush

    // --- DC blocker in the loop (so a decayed string reaches true zero) ---
    float looped = dc.process(loopLp);
    if (fabsf(looped) < 1e-18f) {
      looped = 0.0f;
      dc.x1 = dc.y1 = 0.0f;  // flush the blocker's denormal tail
    }

    // --- feedback gain (decay), with palm-mute + energy limiter ----------
    float fb;
    if (mode == KARPLUS_DRONE)
      fb = 0.99994f;  // near-unity self-sustain
    else
      fb = lerpf(0.9997f, 0.982f, clampf(sDamping, 0.0f, 1.0f));
    if (dampGate) fb *= 0.85f;  // palm mute -> faster decay
    // Pull feedback back as energy rises so Drone / hot Bow can't run away
    // past the soft-clip and turn to fuzz.
    if (energy > 0.7f) fb *= 0.7f / energy;

    // Soft-clip the value written back so the loop is unconditionally stable.
    const float writeVal = softSat(excitation + looped * fb);
    line.write(writeVal);

    // --- energy tracker (LED + limiter) ----------------------------------
    energy += (fabsf(s) - energy) * onePoleCoef(8.0f, dt);

    // Wet/dry: dry = the excitation signal, wet = the ringing string.
    const float dry = excitation;
    return dry + (s - dry) * clampf(sWet, 0.0f, 1.0f);
  }
};

}  // namespace sc
