#pragma once

// Pitch Shifter — classic delay-line "harmonizer" pitch shift core.
//
// Used by:
//   - firmwares/mod2-pitch-shifter/mod2-pitch-shifter.ino
//   - rack-plugins/src/mod2-pitch-shifter.cpp
//
// Real-time pitch shift of a mono input with no heap and no FFT: the standard
// two-tap granular / delay-line recipe. Input is written into a short ring
// buffer (~100 ms). A read tap sweeps that buffer as a sawtooth so its delay
// changes at rate (1 - ratio) samples/sample, which re-pitches the audio by
// `ratio`. A single tap would click every time the sawtooth wraps, so we run
// two taps 180 deg apart and equal-power crossfade them with a sin() window
// (sin^2 p + sin^2(p+0.5) == 1): while one tap fades through its wrap the other
// is at full gain in the middle of a grain. Grain length (POT2) trades pitch
// accuracy for transient smear — short for drums, long for pads.
//
// Modes: Octave-up (x2), Octave-down (x0.5), Free (POT1 -> +/-12 st, quantized
// to semitones by default) and Detune (two voices at +/- a few cents, summed —
// the "always sounds good" thickener). A shifted-signal feedback path (through
// a one-pole low-pass + soft limiter) gives shimmer-lite cascades.
//
// Everything the user touches is one-pole smoothed (no zipper; the shift ratio
// glides), the tap crossfade is equal-power (no clicks/flams) and the feedback
// state is flushed of denormals.
//
// The delay memory is a caller-provided int16 arena over sc::DelayLine — static
// SRAM in firmware, a std::vector sized per engine rate in Rack. Size it with
// pitchShifterArenaSamples().
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop.

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Shift mode (BUTTON short-press cycles these on hardware).
enum PitchShifterMode : uint8_t {
  PITCH_OCT_UP = 0,    // fixed +12 st  (x2)
  PITCH_OCT_DOWN = 1,  // fixed -12 st  (x0.5)
  PITCH_FREE = 2,      // POT1 continuous +/-12 st (semitone-detented)
  PITCH_DETUNE = 3,    // dual voice +/- detuneCents (chorus-like thickener)
  PITCH_MODE_COUNT = 4
};

// Grain buffer length. 100 ms of grain + the small floor delay + margin. At
// 36.6 kHz this is ~4 KB samples (~8 KB int16), well under the README's ~15 KB.
constexpr float kPitchBufferSec = 0.110f;
constexpr float kPitchMinGrainSec = 0.010f;  // POT2 = 0  -> tight, drum-friendly
constexpr float kPitchMaxGrainSec = 0.100f;  // POT2 = 1  -> smooth, pad-friendly
constexpr float kPitchMinDelay = 2.0f;       // floor so a tap never hits write

inline uint32_t pitchShifterArenaSamples(float fsHz) {
  return (uint32_t)(kPitchBufferSec * fsHz) + 16;
}

// POT1 -> shift amount in semitones over +/-12 st. `quantize` snaps to the
// nearest semitone (the Free-mode default per the README's open question);
// pass false for the continuous fine sweep used on the shift layer.
inline float pitchShifterSemitones(float pot01, bool quantize) {
  float st = mapClampf(pot01, 0.0f, 1.0f, -12.0f, 12.0f);
  if (quantize) st = floorf(st + 0.5f);
  return st;
}

// Semitones -> playback ratio (2^(st/12)).
inline float pitchRatioFromSemitones(float semitones) {
  return powf(2.0f, semitones * (1.0f / 12.0f));
}

// POT2 -> grain length in seconds (10 .. 100 ms).
inline float pitchShifterGrainSec(float pot01) {
  return mapClampf(pot01, 0.0f, 1.0f, kPitchMinGrainSec, kPitchMaxGrainSec);
}

// Flush tiny values to zero so a decaying feedback tail can't drop into
// denormal range (very expensive on some FPUs, and inaudible anyway).
inline float pitchFlushDenorm(float x) {
  return (x < 1e-15f && x > -1e-15f) ? 0.0f : x;
}

// One sweeping two-tap read head. Owns only its sawtooth phase; the ring buffer
// is shared (one input write per sample feeds every voice).
struct PitchGrainVoice {
  float phase = 0.0f;  // sawtooth position, 0..1

  void reset() { phase = 0.0f; }

  // Advance and read a pitch-shifted sample from `line`. `ratio` is the shift
  // factor (2 = +1 oct), `grainSamples` the current window length in samples.
  float read(const DelayLine& line, float ratio, float grainSamples) {
    // Delay must change at rate (1 - ratio) samples/sample to re-pitch by
    // `ratio`; delay = kPitchMinDelay + phase*grainSamples, so the phase
    // increment is (1 - ratio) / grainSamples.
    phase += (1.0f - ratio) / grainSamples;
    phase -= floorf(phase);  // frac(), valid for negative increments too

    float p2 = phase + 0.5f;
    p2 -= floorf(p2);

    const float d1 = kPitchMinDelay + phase * grainSamples;
    const float d2 = kPitchMinDelay + p2 * grainSamples;

    // Equal-power crossfade: each tap is silent at its own wrap (sin(pi*0)=0)
    // and full in the middle; sin^2 p + sin^2(p+0.5) == 1.
    const float w1 = sinf(kPi * phase);
    const float w2 = sinf(kPi * p2);
    return w1 * line.read(d1) + w2 * line.read(d2);
  }
};

struct PitchShifterCore {
  // Parameters (write directly; see the mapper free functions above).
  uint8_t mode = PITCH_FREE;   // PitchShifterMode
  float semitones = 0.0f;      // Free-mode pitch, -12..+12 st
  float grainSec = 0.050f;     // POT2 grain length (kPitchMin..MaxGrainSec)
  float wet = 1.0f;            // 0 dry .. 1 fully shifted (mix)
  float feedback = 0.0f;       // 0..1 shifted-signal feedback (shimmer)
  float detuneCents = 18.0f;   // Detune-mode spread (+/- cents per voice)

  // Smoothed working values (one-pole; ratio glides, others de-zipper).
  float ratioSA = 1.0f;  // voice A smoothed ratio
  float ratioSB = 1.0f;  // voice B smoothed ratio (Detune only)
  float grainS = 0.050f;
  float wetS = 1.0f;
  float fbS = 0.0f;

  // State.
  DelayLine line;
  PitchGrainVoice voiceA;
  PitchGrainVoice voiceB;
  float fbLp = 0.0f;      // low-passed feedback source (previous shifted out)
  float ledPhase = 0.0f;  // LED indicator LFO, 0..1
  bool primed = false;    // first process() snaps smoothers to their targets

  // `buf`/`n`: caller-owned arena (pitchShifterArenaSamples() long).
  void init(int16_t* buf, uint32_t n) {
    line.init(buf, n);
    reset();
  }

  void reset() {
    line.clear();
    voiceA.reset();
    voiceB.reset();
    fbLp = 0.0f;
    ledPhase = 0.0f;
    primed = false;
  }

  // Target ratios for the current mode. Voice B is only used in Detune.
  void targetRatios(float& rA, float& rB) const {
    switch (mode) {
      case PITCH_OCT_UP:   rA = 2.0f; rB = 2.0f; break;
      case PITCH_OCT_DOWN: rA = 0.5f; rB = 0.5f; break;
      case PITCH_DETUNE: {
        const float c = detuneCents * (1.0f / 1200.0f);  // cents -> exponent
        rA = powf(2.0f, c);
        rB = powf(2.0f, -c);
        break;
      }
      case PITCH_FREE:
      default:
        rA = rB = pitchRatioFromSemitones(clampf(semitones, -12.0f, 12.0f));
        break;
    }
  }

  // 0..1 brightness for the panel LED. Pulses faster the further from unison,
  // so the blink rate reads out shift size; direction shows as a bias.
  float ledLevel() const {
    const float dir = (ratioSA >= 1.0f) ? 0.65f : 0.35f;  // up brighter than down
    return clampf(dir + 0.35f * sinf(kTwoPi * ledPhase), 0.0f, 1.0f);
  }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    float rA, rB;
    targetRatios(rA, rB);

    if (!primed) {
      ratioSA = rA;
      ratioSB = rB;
      grainS = grainSec;
      wetS = wet;
      fbS = feedback;
      primed = true;
    }

    // Smooth every user parameter. The ratio glides more slowly (~8 Hz) so
    // pitch changes portamento rather than jump; the rest de-zipper fast.
    const float glide = onePoleCoef(8.0f, dt);
    const float smooth = onePoleCoef(40.0f, dt);
    ratioSA += (rA - ratioSA) * glide;
    ratioSB += (rB - ratioSB) * glide;
    grainS += (grainSec - grainS) * smooth;
    wetS += (wet - wetS) * smooth;
    fbS += (clampf(feedback, 0.0f, 1.0f) - fbS) * smooth;

    // Grain length in samples, kept inside the buffer.
    const float maxGrain = (float)line.len - kPitchMinDelay - 2.0f;
    const float grainSamples =
        clampf(grainS / dt, kPitchMinGrainSec / dt, maxGrain);

    // Write input + low-passed shifted feedback. The write soft-saturates and
    // DelayLine::write clamps, so >100% feedback warms into a limit instead of
    // wrapping the int16 buffer.
    const float fbGain = 0.92f * fbS;
    line.write(softSat(in + fbGain * fbLp));

    // Read the shifted signal (one voice, or two summed for Detune).
    float shifted;
    if (mode == PITCH_DETUNE) {
      shifted = 0.5f * (voiceA.read(line, ratioSA, grainSamples) +
                        voiceB.read(line, ratioSB, grainSamples));
    } else {
      shifted = voiceA.read(line, ratioSA, grainSamples);
    }

    // Feedback source: one-pole low-pass (~4 kHz) darkens each shimmer pass.
    fbLp += (shifted - fbLp) * onePoleCoef(4000.0f, dt);
    fbLp = pitchFlushDenorm(fbLp);

    // LED LFO: 1 Hz at unison up to ~7 Hz at +/-1 oct.
    const float ledHz = 1.0f + 6.0f * clampf(fabsf(ratioSA - 1.0f), 0.0f, 1.0f);
    ledPhase += ledHz * dt;
    ledPhase -= floorf(ledPhase);

    return in + (shifted - in) * wetS;
  }
};

}  // namespace sc
