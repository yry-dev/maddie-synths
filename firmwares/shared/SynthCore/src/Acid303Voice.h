#pragma once

// Acid303 — generative 303-style bass voice + Turing-machine sequencer.
//
// Shared core of the Acid303 module. Ported from
// firmwares/mod2-acid303/mod2-acid303.ino (HAGIWO Mod2, RP2350).
//
// One struct holds BOTH halves of the firmware:
//   - the sequencer (pattern generation, per-step probabilistic mutation, the
//     TURING length/probability mapping, scales and waveforms), and
//   - the monophonic voice (band-limited-ish oscillator bank, decay + bite
//     envelopes, gate envelope, portamento/slide and an output one-pole).
//
// The firmware runs its audio ISR at a fixed 31250 Hz and its sequencer off
// millis()/micros(). Here everything is driven by a caller-supplied `dt`
// (seconds) so the same code is sample-rate independent: per-sample decay /
// release / slide / smoothing coefficients are specified at the firmware's
// reference rate and re-derived for the host rate (coef^(kRefRate*dt)), and
// step/gate timing is measured in seconds. The audio path stays multiply-only
// (the few powf() calls happen on rate changes and per step, never per sample),
// so the core is still cheap enough for the RP2350 audio ISR.
//
// Pure C++: depends only on sc_math.h + sc_dsp.h (xorshift32). No Arduino /
// Rack / Pico SDK.
//
// Deviations from the firmware (documented, intentional):
//   - Randomness uses the portable sc::xorshift32 PRNG instead of Arduino
//     random(), so the core is deterministic from a fixed seed. Sequences
//     therefore differ from the firmware's analog-seeded random() but are
//     statistically identical.
//   - The firmware's subtle per-odd-step swing (a blocking delay() of up to
//     ~18% of the step) is omitted; it relied on busy-waiting in the control
//     loop, which has no sample-accurate equivalent and is barely audible.
//   - Transpose CV is summed as 1V/Oct semitones (host responsibility); the
//     firmware shared one ADC pin between POT3 and the CV jack.

#include "sc_math.h"
#include "sc_dsp.h"

namespace sc {

struct Acid303Voice {
  // Firmware audio ISR rate; per-sample coefficients below are specified here
  // and re-derived for the host rate so decay/slide times are rate independent.
  static constexpr float kRefRate = 31250.0f;
  static constexpr int kStepsMax = 16;

  enum ScaleMode { MINOR = 0, PHRYG = 1, DORIAN = 2, MAJOR = 3 };
  enum WaveMode { SAW = 0, SQUARE = 1, TRI = 2, PULSE = 3, SUPERSAW = 4, SINEISH = 5 };

  struct Step {
    int8_t note;   // scale degree, 0..11
    int8_t oct;    // -1..+1
    bool gate;     // false = rest
    bool accent;
    bool slide;    // slide implies a tie into the next step
  };

  // ── sequencer state ──────────────────────────────────────────────────────
  Step pattern[kStepsMax];
  int stepIndex = 0;
  int stepLen = 16;
  int scaleMode = MINOR;
  int waveMode = SUPERSAW;

  // ── mapped controls (host updates these each block) ───────────────────────
  float turingProb = 1.0f;  // POT1: probability of mutation (V curve, 1 at noon)
  float decay01 = 0.5f;     // POT2: amp decay + bite
  int rootSemis = 0;        // POT3 (+ CV): transpose in semitones
  bool accentHold = false;  // IN2: force-accent while high

  // ── free-run fallback (no external clock) ─────────────────────────────────
  bool freeRun = false;        // host: true when the clock input is unpatched
  float internalInterval = 0.125f;  // host: seconds per internal step

  // ── voice state ───────────────────────────────────────────────────────────
  uint32_t phase = 0, phaseInc = 0;
  float currentFreq = 110.0f, targetFreq = 110.0f;
  float ampVal = 0.0f, biteVal = 0.0f, gateVal = 0.0f;
  bool gateOpen = false;
  float lpZ = 0.0f;
  bool prevSlide = false;

  // reference (per-31250-sample) envelope coefs, set per step
  float ampDecayRef = 0.9990f, biteDecayRef = 0.9900f;

  // effective (per host-sample) coefs, recomputed when dt changes / per step
  float rateExp = 1.0f;  // kRefRate * dt
  float ampDecayEff = 0.9990f, biteDecayEff = 0.9900f;
  float gateRelEff = 0.86f;
  float slideEff = 0.035f;
  float lpEff = 0.22f;
  float lastDt = 0.0f;

  // ── step / gate timing (seconds) ──────────────────────────────────────────
  float stepDur = 0.125f;
  float gateLen = 0.0625f;
  float gateTimer = 0.0f;
  float sinceClock = 1e9f;   // time since last external clock edge
  bool hadClock = false;
  float internalTimer = 0.0f;

  // ── PRNG ──────────────────────────────────────────────────────────────────
  uint32_t rng = 0x1234567u;
  inline uint32_t rnd() { return xorshift32(rng); }
  inline int rndN(int n) { return (int)(rnd() % (uint32_t)n); }  // [0, n)

  // ── scales ─────────────────────────────────────────────────────────────────
  int8_t pickScaleDegree() {
    static const int8_t MIN_[7] = {0, 2, 3, 5, 7, 8, 10};
    static const int8_t PHR_[7] = {0, 1, 3, 5, 7, 8, 10};
    static const int8_t DOR_[7] = {0, 2, 3, 5, 7, 9, 10};
    static const int8_t MAJ_[7] = {0, 2, 4, 5, 7, 9, 11};
    const int8_t* s = MIN_;
    switch (scaleMode) {
      case MINOR:  s = MIN_; break;
      case PHRYG:  s = PHR_; break;
      case DORIAN: s = DOR_; break;
      case MAJOR:  s = MAJ_; break;
    }
    return s[rndN(7)];
  }

  static inline float midiToFreq(float midi) {
    return 440.0f * powf(2.0f, (midi - 69.0f) / 12.0f);
  }

  // ── TURING mapping (POT1) ───────────────────────────────────────────────────
  // x in 0..1: length is 8 on the left half, 16 on the right (and 16 near noon);
  // mutation probability is a V curve, 1 at centre, 0 at the ends.
  void setTuring(float x) {
    x = clampf(x, 0.0f, 1.0f);
    float dist = fabsf(x - 0.5f) / 0.5f;
    float p = clampf(1.0f - dist, 0.0f, 1.0f);
    int len = (x < 0.5f) ? 8 : 16;
    if (p > 0.92f) len = 16;  // noon = random + full length
    turingProb = p;
    stepLen = len;
  }

  void setTranspose(int semis) { rootSemis = semis; }
  void cycleScale() { scaleMode = (scaleMode + 1) & 3; }
  void cycleWave() { waveMode = (waveMode + 1) % 6; }

  // ── pattern generation ──────────────────────────────────────────────────────
  void enforceRhythmConstraints() {
    pattern[0].gate = true;
    int restRun = 0;
    for (int i = 0; i < stepLen; i++) {
      if (!pattern[i].gate) restRun++;
      else restRun = 0;
      if (restRun > 3) {
        pattern[i].gate = true;
        restRun = 0;
      }
      if (pattern[i].slide) pattern[i].gate = true;
    }
    for (int i = 0; i < stepLen; i += 4) pattern[i].gate = true;  // downbeats
  }

  void regen() {
    for (int i = 0; i < kStepsMax; i++) {
      bool gate = (int)(rnd() % 100u) < 78;
      bool accent = (int)(rnd() % 100u) < 28;
      bool slide = (int)(rnd() % 100u) < 22;
      if ((i % 4) == 0) gate = (int)(rnd() % 100u) < 92;
      pattern[i].gate = gate;
      pattern[i].accent = accent;
      pattern[i].slide = slide;
      pattern[i].note = pickScaleDegree();
      int octPick = (int)(rnd() % 100u);
      pattern[i].oct = (octPick < 18) ? +1 : (octPick < 35 ? -1 : 0);
    }
    // force a small acid slide run
    int start = rndN(kStepsMax - 4);
    for (int k = 0; k < 4; k++) {
      pattern[start + k].gate = true;
      pattern[start + k].slide = (k != 0);
      if (k == 2) pattern[start + k].accent = true;
    }
    stepLen = 16;
    enforceRhythmConstraints();
  }

  void mutateStepAt(int idx, float prob) {
    float pitchP = prob;
    float gateP = prob * 0.60f;
    float accP = prob * 0.40f;
    float slideP = prob * 0.35f;
    float octP = 0.08f + 0.22f * prob;

    if ((int)(rnd() % 1000u) < (int)(pitchP * 1000.0f))
      pattern[idx].note = pickScaleDegree();

    if ((int)(rnd() % 1000u) < (int)(gateP * 1000.0f)) {
      bool downbeat = ((idx % 4) == 0);
      int r = (int)(rnd() % 100u);
      pattern[idx].gate = downbeat ? (r < 92) : (r < 78);
    }

    if ((int)(rnd() % 1000u) < (int)(accP * 1000.0f))
      pattern[idx].accent = ((int)(rnd() % 100u) < 32);

    if ((int)(rnd() % 1000u) < (int)(slideP * 1000.0f))
      pattern[idx].slide = ((int)(rnd() % 100u) < 26);

    if ((int)(rnd() % 1000u) < (int)(octP * 1000.0f)) {
      int r = (int)(rnd() % 100u);
      if (r < 50) pattern[idx].oct = 0;
      else if (r < 85) pattern[idx].oct = +1;
      else pattern[idx].oct = -1;
    }

    if (pattern[idx].slide) pattern[idx].gate = true;
  }

  // ── rate / oscillator helpers ───────────────────────────────────────────────
  void updateRate(float dt) {
    lastDt = dt;
    rateExp = kRefRate * dt;
    ampDecayEff = powf(ampDecayRef, rateExp);
    biteDecayEff = powf(biteDecayRef, rateExp);
    gateRelEff = powf(0.86f, rateExp);
    slideEff = 1.0f - powf(1.0f - 0.035f, rateExp);
    lpEff = 1.0f - powf(1.0f - 0.22f, rateExp);
  }

  inline void setOscFreq(float f, float dt) {
    float inc = f * 4294967296.0f * dt;  // f * 2^32 / sampleRate
    if (inc < 1.0f) inc = 1.0f;
    phaseInc = (uint32_t)inc;
  }

  inline float oscSample(uint32_t p) const {
    switch (waveMode) {
      case SAW:
        return ((int32_t)(p >> 8) / 8388608.0f) - 1.0f;
      case SQUARE:
        return (p & 0x80000000u) ? 1.0f : -1.0f;
      case TRI: {
        float s = ((int32_t)(p >> 8) / 8388608.0f) - 1.0f;
        return 2.0f * fabsf(s) - 1.0f;
      }
      case PULSE:
        return ((p >> 30) == 0) ? 1.0f : -1.0f;  // ~25%
      case SUPERSAW: {
        uint32_t p2 = p + (phaseInc * 3);
        uint32_t p3 = p - (phaseInc * 2);
        float s1 = ((int32_t)(p >> 8) / 8388608.0f) - 1.0f;
        float s2 = ((int32_t)(p2 >> 8) / 8388608.0f) - 1.0f;
        float s3 = ((int32_t)(p3 >> 8) / 8388608.0f) - 1.0f;
        return (s1 + 0.6f * s2 + 0.6f * s3) * 0.55f;
      }
      case SINEISH: {
        float s = ((int32_t)(p >> 8) / 8388608.0f) - 1.0f;
        float t = 2.0f * fabsf(s) - 1.0f;
        return t - (t * t * t) * 0.25f;
      }
    }
    return 0.0f;
  }

  // ── step trigger ────────────────────────────────────────────────────────────
  void triggerStep(int idx) {
    Step& s = pattern[idx];
    if (s.slide) s.gate = true;

    int midi = 45 + rootSemis + s.note + (12 * s.oct);
    bool isAcc = s.accent || accentHold;
    targetFreq = midiToFreq((float)midi);

    float d = decay01;
    float baseDecay = 0.9955f + d * 0.00435f;
    ampDecayRef = baseDecay + (isAcc ? 0.0022f : 0.0f);
    if (ampDecayRef > 0.99995f) ampDecayRef = 0.99995f;
    biteDecayRef = 0.9900f + d * 0.00880f;
    ampDecayEff = powf(ampDecayRef, rateExp);
    biteDecayEff = powf(biteDecayRef, rateExp);

    bool tieFromPrev = prevSlide;
    prevSlide = s.slide;

    float gateFrac = isAcc ? 0.78f : 0.50f;
    if (s.slide) gateFrac = 1.05f;  // tie through
    gateLen = stepDur * gateFrac;

    if (s.gate && !tieFromPrev) {
      ampVal = 1.0f + (isAcc ? 0.35f : 0.0f);
      biteVal = 1.0f + (isAcc ? 0.65f : 0.0f);
    }

    if (!s.slide) currentFreq = targetFreq;
  }

  // Advance one step. `external` = driven by a clock edge (measures the step
  // duration from the elapsed time) vs the internal free-run timer.
  void advance(bool external) {
    if (external) {
      if (hadClock && sinceClock >= 0.020f && sinceClock <= 0.500f)
        stepDur = sinceClock;
      hadClock = true;
      sinceClock = 0.0f;
    } else {
      stepDur = internalInterval;
    }
    stepIndex++;
    if (stepIndex >= stepLen) stepIndex = 0;
    mutateStepAt(stepIndex, turingProb);
    enforceRhythmConstraints();
    gateTimer = 0.0f;
    triggerStep(stepIndex);
    gateOpen = pattern[stepIndex].gate;
  }

  // Call on an external clock rising edge.
  void clock() { advance(true); }

  void reset() {
    rng = 0x1234567u;
    scaleMode = MINOR;
    waveMode = SUPERSAW;
    phase = 0;
    phaseInc = 0;
    currentFreq = targetFreq = 110.0f;
    ampVal = biteVal = gateVal = 0.0f;
    gateOpen = false;
    lpZ = 0.0f;
    prevSlide = false;
    hadClock = false;
    sinceClock = 1e9f;
    internalTimer = 0.0f;
    stepDur = 0.125f;
    gateLen = 0.0625f;
    gateTimer = 0.0f;
    lastDt = 0.0f;
    rateExp = 1.0f;
    regen();
    stepIndex = 0;
    triggerStep(stepIndex);
    gateOpen = pattern[stepIndex].gate;
    gateVal = 1.0f;  // gate starts open on step 0
  }

  // LED level: the amp envelope, so the panel pulses on each gated step.
  inline float lightLevel() const { return clampf(ampVal, 0.0f, 1.0f); }

  // Render one audio sample in -1..+1 and advance by `dt` seconds.
  float process(float dt) {
    if (dt != lastDt) updateRate(dt);

    // timers
    sinceClock += dt;
    gateTimer += dt;
    if (gateOpen && gateTimer >= gateLen) gateOpen = false;

    // internal free-run advance if no external clock for >1.5 s
    if (freeRun && sinceClock > 1.5f) {
      internalTimer += dt;
      if (internalTimer >= internalInterval) {
        internalTimer = 0.0f;
        advance(false);
      }
    }

    // portamento (slide)
    currentFreq += (targetFreq - currentFreq) * slideEff;
    setOscFreq(currentFreq, dt);

    // oscillator
    phase += phaseInc;
    float osc = oscSample(phase);

    // envelopes
    ampVal *= ampDecayEff;
    if (ampVal < 0.00001f) ampVal = 0.0f;
    biteVal *= biteDecayEff;
    if (biteVal < 0.00001f) biteVal = 0.0f;

    // "bite" drives harmonic intensity, especially on accent
    float bite = 0.25f + biteVal * 0.55f;
    float y = clampf(osc * bite, -1.0f, 1.0f);

    // output smoothing one-pole
    lpZ += lpEff * (y - lpZ);
    y = lpZ;

    // gate-length envelope
    if (gateOpen) gateVal = 1.0f;
    else gateVal *= gateRelEff;
    if (gateVal < 0.00001f) gateVal = 0.0f;

    float amp = ampVal * 0.65f * gateVal;
    return clampf(y * amp, -1.0f, 1.0f);
  }
};

}  // namespace sc
