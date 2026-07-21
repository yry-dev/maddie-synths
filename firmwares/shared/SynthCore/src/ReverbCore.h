#pragma once

// Reverb — hall & plate algorithmic reverb core.
//
// Used by:
//   - firmwares/mod2-reverb/mod2-reverb.ino
//   - rack-plugins/src/mod2-reverb.cpp
//
// A Dattorro-style figure-8 tank (Griesinger topology): a pre-delay feeds four
// series input-diffusion allpasses, then a symmetric two-branch tank. Each
// branch is [decay-diffusion allpass -> delay -> damping one-pole -> decay-
// diffusion allpass -> delay], the two branches cross-feed each other through
// the decay gain, and a handful of taps are summed to a mono wet signal.
//   - HALL  = long delay times, pre-delay engaged, darker damping.
//   - PLATE = ~55% shorter delays, no pre-delay, more input diffusion,
//             brighter damping (denser, faster build-up).
// Two of the decay-diffusion allpasses are gently LFO-modulated to break up
// the metallic ring. loop gain is strictly < 1 at max Decay (musically
// "infinite"); FREEZE pins it to exactly 1 with damping off and the input
// muted, so the tail holds indefinitely without blowing up.
//
// Memory: ONE caller-provided int16 arena, partitioned internally into 13
// sub-delay-lines proportional to their nominal times (see init()). int16 was
// chosen over float per the README RAM budget: the whole tank is ~0.84 s of
// delay, so ~31k samples at the firmware's 36.6 kHz => ~61 KB (≈80 KB at
// 48 kHz) — well under the 200 KB budget, and half what float would cost. The
// int16 storage also auto-flushes denormal tails (|x| < ~3e-5 quantizes to 0),
// so only the float one-pole states need explicit flushing.
//
// Sample-rate independent: every delay length is a time in seconds converted
// to samples with the caller's dt. reverbArenaSamples(fs) sizes the arena from
// the same fs the caller drives process() with, so the per-line sample
// capacities and the requested read delays stay consistent at any rate.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop. (Per-sample cost is ~8 allpasses + 4 delay
// reads + 2 one-poles + 2 cheap LFOs, no transcendentals in the hot path — it
// fits the RP2350 ~4000-cycle budget; the tank is the most expensive planned
// mod2 FX, so prototype/tune it here in the Rack port first.)

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Voicing (BUTTON short-press cycles these on hardware).
enum ReverbMode : uint8_t {
  REVERB_HALL = 0,   // long, dark tail
  REVERB_PLATE = 1,  // shorter, brighter, denser
  REVERB_MODE_COUNT = 2
};

// --- Tank tuning -----------------------------------------------------------
// Nominal delay times (seconds) for the 13 lines, in partition order. These
// are the classic Dattorro/Griesinger plate values (referred to 29.76 kHz),
// mutually prime in samples so the modal density stays smooth. The tank delays
// are further scaled by Size (POT1) and the mode (Plate shortens them).
enum ReverbLine : uint8_t {
  RVB_PREDELAY = 0,
  RVB_ID1, RVB_ID2, RVB_ID3, RVB_ID4,  // input diffusion allpasses
  RVB_DD1L, RVB_D1L, RVB_DD2L, RVB_D2L,  // left branch
  RVB_DD1R, RVB_D1R, RVB_DD2R, RVB_D2R,  // right branch
  RVB_NLINES
};

// Nominal (maximum, Size=1, Hall) time of each line, seconds.
inline const float* reverbLineTimes() {
  static const float t[RVB_NLINES] = {
      0.080f,                                    // pre-delay (Hall max)
      0.004771f, 0.003595f, 0.012735f, 0.009307f,  // input diffusers
      0.022579f, 0.149625f, 0.060481f, 0.124999f,  // left  dd1,d1,dd2,d2
      0.030509f, 0.141695f, 0.089244f, 0.106280f   // right dd1,d1,dd2,d2
  };
  return t;
}

// Sum of the nominal line times — the total delay memory in seconds.
constexpr float kReverbTotalSec = 0.080f + 0.004771f + 0.003595f + 0.012735f +
                                  0.009307f + 0.022579f + 0.149625f +
                                  0.060481f + 0.124999f + 0.030509f +
                                  0.141695f + 0.089244f + 0.106280f;

// Total int16 arena the reverb needs at sample rate `fsHz` (with guard).
constexpr uint32_t reverbArenaSamples(float fsHz) {
  return (uint32_t)(kReverbTotalSec * fsHz) + 64;
}

// --- POT -> unit mapping free functions (shared by firmware + Rack) --------
// POT1 -> Size: scales the tank delay lengths (and Hall pre-delay). Never
// exceeds 1.0 so reads always stay inside their allocated line capacity.
inline float reverbSizeScale(float pot01) {
  return 0.35f + 0.65f * clampf(pot01, 0.0f, 1.0f);
}

// POT2 -> Decay: loop feedback gain. Musical range; top ≈ "infinite" but still
// strictly < 1 (FREEZE is the only thing that reaches exactly 1).
inline float reverbDecayGain(float pot01) {
  return 0.30f + 0.68f * clampf(pot01, 0.0f, 1.0f);  // 0.30 .. 0.98
}

// Shift-layer POT2 -> Damping amount 0..1 (higher = darker tail).
inline float reverbDampingAmount(float pot01) {
  return clampf(pot01, 0.0f, 1.0f);
}

// Cheap sine-ish LFO in -1..+1 from a 0..1 phase (no transcendentals in the
// hot path — same rounded-triangle shape as ChorusCore).
inline float reverbLfo(float phase01) {
  phase01 -= floorf(phase01);
  const float tri = 4.0f * fabsf(phase01 - 0.5f) - 1.0f;
  return tri * (1.5f - 0.5f * tri * tri);
}

// Flush denormal/sub-audible float state to true zero (int16 lines self-flush).
inline float reverbFlush(float x) {
  return (x > -1.0e-18f && x < 1.0e-18f) ? 0.0f : x;
}

struct ReverbCore {
  // Parameters (write directly; see the mappers above). One-pole smoothed
  // inside process() so knob moves never zip or click.
  float sizeScale = reverbSizeScale(0.6f);   // 0.35 .. 1.0
  float decayGain = reverbDecayGain(0.6f);   // 0.30 .. 0.98
  float damping = 0.5f;                       // 0..1 (tone)
  float wet = 0.4f;                           // 0 dry .. 1 wet
  uint8_t mode = REVERB_HALL;                 // ReverbMode
  bool freeze = false;                        // hold the tail (gain=1)

  // State.
  DelayLine line[RVB_NLINES];
  float dampL = 0.0f, dampR = 0.0f;  // damping one-pole states (need flushing)
  float modPhaseA = 0.0f;            // tank modulation LFOs (0..1)
  float modPhaseB = 0.37f;
  // Smoothed parameter shadows.
  float sizeS = 0.6f, decayS = 0.6f, dampS = 0.5f, wetS = 0.4f;
  float ledEnv = 0.0f;  // tail-level follower for the panel LED
  bool primed = false;

  // Diffusion allpass coefficients (Dattorro): input diffusion is stronger in
  // Plate. Decay-diffusion coefficients are fixed.
  static constexpr float kInDiff1 = 0.75f;   // ID1/ID2
  static constexpr float kInDiff2 = 0.625f;  // ID3/ID4
  static constexpr float kDecayDiff1 = 0.70f;
  static constexpr float kDecayDiff2 = 0.50f;
  static constexpr float kOutGain = 0.6f;    // wet tap sum scaling

  // `arena`/`len`: one caller-owned int16 buffer, reverbArenaSamples() long.
  void init(int16_t* arena, uint32_t len) {
    const float* t = reverbLineTimes();
    float total = 0.0f;
    for (int i = 0; i < RVB_NLINES; i++) total += t[i];
    uint32_t off = 0;
    for (int i = 0; i < RVB_NLINES; i++) {
      uint32_t cap = (uint32_t)((t[i] / total) * (float)len);
      if (cap < 8) cap = 8;
      if (off + cap > len) cap = (len > off + 8) ? (len - off) : 8;
      line[i].init(arena + off, cap);
      off += cap;
    }
    reset();
  }

  void reset() {
    for (int i = 0; i < RVB_NLINES; i++) line[i].clear();
    dampL = dampR = 0.0f;
    modPhaseA = 0.0f;
    modPhaseB = 0.37f;
    sizeS = sizeScale;
    decayS = decayGain;
    dampS = damping;
    wetS = wet;
    ledEnv = 0.0f;
    primed = false;
  }

  // 0..1 brightness for the panel LED — follows the tail level.
  float ledLevel() const { return clampf(ledEnv, 0.0f, 1.0f); }

  // Schroeder allpass over one sub-line: |H| = 1, stable for |g| < 1.
  //   V = in - g*D ; write V ; out = D + g*V
  static inline float allpass(DelayLine& l, float delaySamp, float g,
                              float in) {
    const float d = l.read(delaySamp);
    const float v = in - g * d;
    l.write(v);
    return d + g * v;
  }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    if (!primed) {
      sizeS = sizeScale;
      decayS = decayGain;
      dampS = damping;
      wetS = wet;
      primed = true;
    }

    // --- one-pole smooth every user parameter (no zipper) ----------------
    const float pc = onePoleCoef(6.0f, dt);    // slow glide for size/decay/tone
    const float wc = onePoleCoef(30.0f, dt);   // mix can move a little quicker
    sizeS += (sizeScale - sizeS) * pc;
    decayS += (decayGain - decayS) * pc;
    dampS += (damping - dampS) * pc;
    wetS += (wet - wetS) * wc;

    const float* T = reverbLineTimes();
    const bool plate = (mode == REVERB_PLATE);

    // Mode retuning: Plate shortens the tank, disables pre-delay, diffuses more
    // and damps brighter.
    const float modeScale = plate ? 0.55f : 1.0f;
    const float tankScale = sizeS * modeScale;
    const float g_in1 = plate ? 0.80f : kInDiff1;
    const float g_in2 = plate ? 0.68f : kInDiff2;

    // Damping one-pole coefficient: Hall darker (more low-pass) than Plate.
    // coef is the amount of the *new* sample let through (1 = no damping).
    float damp = dampS;
    if (plate) damp *= 0.7f;                 // Plate stays brighter
    float lpKeep = 1.0f - clampf(damp * 0.9f, 0.0f, 0.9f);  // 0.1 .. 1.0
    if (freeze) lpKeep = 1.0f;               // freeze: damping off

    // Decay/loop gain. Freeze pins it to exactly 1 (with damping off) so the
    // tail is preserved; otherwise strictly < 1.
    const float decay = freeze ? 1.0f : clampf(decayS, 0.0f, 0.995f);

    // --- input path: pre-delay -> 4 diffusion allpasses ------------------
    line[RVB_PREDELAY].write(in);
    float preSec = plate ? 0.0f : (T[RVB_PREDELAY] * sizeS);
    float x = line[RVB_PREDELAY].read(clampf(preSec / dt, 1.0f, 1.0e9f));
    x = allpass(line[RVB_ID1], T[RVB_ID1] / dt, g_in1, x);
    x = allpass(line[RVB_ID2], T[RVB_ID2] / dt, g_in1, x);
    x = allpass(line[RVB_ID3], T[RVB_ID3] / dt, g_in2, x);
    x = allpass(line[RVB_ID4], T[RVB_ID4] / dt, g_in2, x);
    const float tankIn = freeze ? 0.0f : x;  // freeze mutes new input

    // --- tank modulation LFOs (break up metallic ring) -------------------
    modPhaseA += 0.47f * dt;  modPhaseA -= floorf(modPhaseA);
    modPhaseB += 0.73f * dt;  modPhaseB -= floorf(modPhaseB);
    const float modSamp = 0.00035f / dt;  // ~0.35 ms excursion
    const float modA = modSamp * reverbLfo(modPhaseA);
    const float modB = modSamp * reverbLfo(modPhaseB);

    // Cross-feedback taps: read the OPPOSITE branch's output delay first
    // (its content is the previous state -> the loop delay).
    const float fbFromR = line[RVB_D2R].read(T[RVB_D2R] / dt * tankScale);
    const float fbFromL = line[RVB_D2L].read(T[RVB_D2L] / dt * tankScale);

    // --- LEFT branch -----------------------------------------------------
    float lSig = tankIn + decay * fbFromR;
    lSig = allpass(line[RVB_DD1L], T[RVB_DD1L] / dt * 0.97f + modA, kDecayDiff1,
                   lSig);
    line[RVB_D1L].write(lSig);
    float lD = line[RVB_D1L].read(T[RVB_D1L] / dt * tankScale);
    dampL += (lD - dampL) * lpKeep;
    dampL = reverbFlush(dampL);
    float lPost = dampL * decay;
    lPost = allpass(line[RVB_DD2L], T[RVB_DD2L] / dt, kDecayDiff2, lPost);
    line[RVB_D2L].write(lPost);

    // --- RIGHT branch ----------------------------------------------------
    float rSig = tankIn + decay * fbFromL;
    rSig = allpass(line[RVB_DD1R], T[RVB_DD1R] / dt * 0.97f + modB, kDecayDiff1,
                   rSig);
    line[RVB_D1R].write(rSig);
    float rD = line[RVB_D1R].read(T[RVB_D1R] / dt * tankScale);
    dampR += (rD - dampR) * lpKeep;
    dampR = reverbFlush(dampR);
    float rPost = dampR * decay;
    rPost = allpass(line[RVB_DD2R], T[RVB_DD2R] / dt, kDecayDiff2, rPost);
    line[RVB_D2R].write(rPost);

    // --- output: mono sum of a few decorrelated tank taps ----------------
    const float wetSig = kOutGain * (lD + rD + 0.6f * (lPost + rPost));

    // LED tail follower (peak-ish envelope of the wet signal).
    const float mag = fabsf(wetSig);
    ledEnv += (mag - ledEnv) * (mag > ledEnv ? 0.3f : onePoleCoef(2.0f, dt));

    return in + (wetSig - in) * wetS;
  }
};

}  // namespace sc
