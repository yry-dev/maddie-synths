#pragma once

// Spiral 4Ever — auditory-illusion / Shepard-tone multi-mode generator.
//
// Shared core of the Spiral module. Used by:
//   - firmwares/mod2-spiral/mod2-spiral.ino  (audio ISR calls process(1/AUDIO_FS))
//   - rack-plugins/src/Spiral.cpp                  (process(args.sampleTime))
//
// Original "Spiral 4Ever" firmware by HAGIWO (MOD2, RP2350), refactored here so
// the firmware and the VCV Rack port run the exact same synthesis. Nine modes:
//   0 Shepard rising   1 Shepard falling   2 Barber pole
//   3 Risset rhythm    4 Tritone paradox   5 Tritone explorer
//   6 Shepard cluster maj   7 Shepard cluster min   8 Euler spiral
//
// Each mode sums sine partials spaced one octave apart, weighted by a
// raised-cosine (cos^2) amplitude window centred on `centerFreq` so partials
// fade in at the bottom and out at the top — the classic Shepard illusion of an
// endlessly rising/falling tone. A slow `sweepPosition` (0..1) glides every
// partial up one octave per cycle; as it wraps, the windowed mix is seamless.
//
// Pure C++: depends only on sc_math.h + sc_dsp.h. No Arduino / Rack / Pico SDK.
// float-only, no heap, no STL. Sample-rate independent: every rate is advanced
// by the caller-supplied `dt` (seconds), so both platforms sound identical.
//
// Implementation note vs. the original firmware: the firmware read its sine and
// triangle from 8192-point hermite/linear LUTs and accumulated phase in 32-bit
// fixed point. This core computes sinf()/triangle directly with a float phase in
// [0,1) (the same "compute the waveform directly" choice made for ClavesVoice),
// and folds the firmware's control-rate sweep/AM updates (originally stepped in
// loop() at ~1 kHz) into the audio-rate process(dt). Both are exact-rate
// equivalents of the originals; the amplitude smoothing is expressed as a
// time-constant so it is identical across sample rates.

#include "sc_math.h"
#include "sc_dsp.h"

namespace sc {

namespace spiral {

constexpr int   NUM_PARTIALS    = 12;
constexpr int   RISSET_LAYERS   = 8;
constexpr int   NUM_MODES       = 9;

constexpr float SHEPARD_BASE_HZ = 20.0f;   // frequency of partial 0 at octavePos 0

constexpr float MAJOR_THIRD     = 4.0f / 12.0f;
constexpr float MINOR_THIRD     = 3.0f / 12.0f;
constexpr float PERFECT_FIFTH   = 7.0f / 12.0f;

constexpr float CENTER_MIN_HZ   = 32.0f;
constexpr float CENTER_MAX_HZ   = 2000.0f;

// Amplitude-window smoothing expressed as a continuous rate (1/s). The firmware
// used a per-sample coefficient of 0.003 at its audio rate (150e6/4096 =
// 36621.09375 Hz); rate = -ln(1-0.003) * 36621.09375 reproduces that exact time
// constant at any sample rate via alpha = 1 - exp(-dt*rate).
constexpr float AMP_SMOOTH_RATE = 110.04f;

}  // namespace spiral

// Wrap a float phase into [0,1).
inline float spiralWrap01(float p) {
  p -= floorf(p);
  if (p < 0.0f) p += 1.0f;
  return p;
}

// Sine of a normalised phase in [0,1).
inline float spiralSine(float phase01) {
  return sinf(phase01 * kTwoPi);
}

// cos^2 amplitude window: 1 at distance 0, smoothly 0 at |distance| >= width.
inline float spiralCosSqWindow(float distance, float width) {
  float x = fabsf(distance) / width;
  if (x >= 1.0f) return 0.0f;
  float c = cosf(x * kPi * 0.5f);
  return c * c;
}

// Shepard amplitude for a partial at `freq` Hz given window centre/width (octaves).
inline float spiralShepardAmp(float freq, float center, float width) {
  if (freq < 18.0f || freq > 20000.0f) return 0.0f;
  float octaveDistance = log2f(freq / center);
  return spiralCosSqWindow(octaveDistance, width);
}

// Frequency of partial `idx` for the rising/falling glide at sweep position.
inline float spiralPartialFreq(int idx, float sweep, bool rising) {
  float octavePos = rising ? (static_cast<float>(idx) + sweep)
                           : (static_cast<float>(idx) + (1.0f - sweep));
  octavePos = fmodf(octavePos, static_cast<float>(spiral::NUM_PARTIALS));
  if (octavePos < 0.0f) octavePos += spiral::NUM_PARTIALS;
  return spiral::SHEPARD_BASE_HZ * powf(2.0f, octavePos);
}

struct SpiralPartial {
  float phase = 0.0f;      // 0..1
  float amplitude = 0.0f;
};

struct SpiralCore {
  // ---- public control state -------------------------------------------------
  int   currentMode   = 0;
  bool  directionUp   = true;
  float speedParam    = 0.5f;     // 0..1 sweep speed
  float auxParam      = 0.5f;     // 0..1 (env width / pitch class / spread)
  float centerFreq    = 440.0f;   // Hz, derived
  float envelopeWidth = 1.8f;     // octaves, derived per mode
  int   tritoneNoteClass = 0;     // 0..11 (mode 5)
  float eulerSpread   = 1.0f;     // mode 8

  // ---- internal oscillator / sweep state ------------------------------------
  SpiralPartial partials[spiral::NUM_PARTIALS];
  SpiralPartial tritonePartials[spiral::NUM_PARTIALS * 2];
  SpiralPartial clusterPartials[3 * spiral::NUM_PARTIALS];
  SpiralPartial eulerPartials[spiral::NUM_PARTIALS];
  float rissetPhase[spiral::RISSET_LAYERS] = {0};
  float rissetAmp[spiral::RISSET_LAYERS]   = {0};

  float sweepPosition = 0.0f;     // 0..1
  float barberAMPhase = 0.0f;     // radians
  float eulerRotation = 0.0f;     // radians

  // Per-process amplitude-smoothing coefficient (set from dt each process()).
  float ampAlpha_ = 0.003f;

  // Output conditioning (identical chain to the firmware ISR).
  DcBlocker dc;
  OutputLpBiquad lp1, lp2;

  void reset() {
    for (int i = 0; i < spiral::NUM_PARTIALS; ++i) {
      partials[i] = SpiralPartial{};
      eulerPartials[i] = SpiralPartial{};
    }
    for (int i = 0; i < spiral::NUM_PARTIALS * 2; ++i) tritonePartials[i] = SpiralPartial{};
    for (int i = 0; i < 3 * spiral::NUM_PARTIALS; ++i) clusterPartials[i] = SpiralPartial{};
    for (int i = 0; i < spiral::RISSET_LAYERS; ++i) { rissetPhase[i] = 0.0f; rissetAmp[i] = 0.0f; }
    sweepPosition = 0.0f;
    barberAMPhase = 0.0f;
    eulerRotation = 0.0f;
    dc = DcBlocker{};
    lp1 = OutputLpBiquad{};
    lp2 = OutputLpBiquad{};
  }

  void setMode(int mode) {
    if (mode < 0) mode = 0;
    if (mode >= spiral::NUM_MODES) mode = spiral::NUM_MODES - 1;
    currentMode = mode;
  }

  void setDirection(bool up) { directionUp = up; }

  // Map normalised controls into engine units. `centerOctaves` is the V/oct-style
  // exponential control for the window centre (firmware: A0/1023 * 5). `speed01`
  // and `aux01` are the raw 0..1 pot values. The per-mode meaning of aux mirrors
  // the firmware loop().
  void setParams(float centerOctaves, float speed01, float aux01) {
    centerFreq = spiral::CENTER_MIN_HZ * powf(2.0f, centerOctaves);
    centerFreq = clampf(centerFreq, spiral::CENTER_MIN_HZ, spiral::CENTER_MAX_HZ);

    speedParam = clampf(speed01, 0.0f, 1.0f);
    auxParam   = clampf(aux01, 0.0f, 1.0f);

    switch (currentMode) {
      case 0: case 1: case 2: case 4: case 6: case 7:
        envelopeWidth = 1.0f + auxParam * 2.0f;
        break;
      case 5:
        tritoneNoteClass = static_cast<int>(auxParam * 11.99f);
        envelopeWidth = 1.8f;
        break;
      case 8:
        eulerSpread = 0.5f + auxParam * 1.5f;
        envelopeWidth = 1.5f + auxParam * 0.8f;
        break;
      default:  // mode 3 (Risset): aux drives tempo inside the synth
        envelopeWidth = 1.8f;
        break;
    }
  }

  inline float smoothAmp(float current, float target) {
    return current + (target - current) * ampAlpha_;
  }

  // Render one audio sample in -1..+1 and advance all state by `dt` seconds.
  float process(float dt) {
    ampAlpha_ = 1.0f - expf(-dt * spiral::AMP_SMOOTH_RATE);

    float sample = 0.0f;
    switch (currentMode) {
      case 0:  sample = synthShepard(true, dt);              break;
      case 1:  sample = synthShepard(false, dt);             break;
      case 2:  sample = synthBarberPole(directionUp, dt);    break;
      case 3:  sample = synthRisset(directionUp, dt);        break;
      case 4:  sample = synthTritone(dt);                    break;
      case 5:  sample = synthTritoneExplorer(dt);            break;
      case 6:  sample = synthCluster(directionUp, false, dt); break;
      case 7:  sample = synthCluster(directionUp, true, dt);  break;
      case 8:
      default: sample = synthEulerSpiral(dt);                break;
    }

    // Advance the slow sweep (firmware: cyclesPerSec applied per loop ms).
    const float cyclesPerSec = 0.008f * powf(120.0f, speedParam);
    sweepPosition = spiralWrap01(sweepPosition + cyclesPerSec * dt);

    if (currentMode == 2) {
      barberAMPhase += cyclesPerSec * kTwoPi * 0.35f * dt;
      if (barberAMPhase >= kTwoPi) barberAMPhase -= kTwoPi;
    }
    if (currentMode == 8) {
      eulerRotation += cyclesPerSec * kTwoPi * 0.2f * dt;
      if (eulerRotation >= kTwoPi) eulerRotation -= kTwoPi;
    }

    // Output conditioning chain (same as firmware ISR).
    sample = dc.process(sample);
    sample = lp1.process(sample);
    sample = lp2.process(sample);
    sample = softSat(sample);
    return sample;
  }

  // ---- synthesis modes ------------------------------------------------------

  float synthShepard(bool rising, float dt) {
    float mix = 0.0f, total = 0.0f;
    for (int i = 0; i < spiral::NUM_PARTIALS; ++i) {
      float freq = spiralPartialFreq(i, sweepPosition, rising);
      float target = spiralShepardAmp(freq, centerFreq, envelopeWidth);
      partials[i].amplitude = smoothAmp(partials[i].amplitude, target);
      float amp = partials[i].amplitude;
      if (amp < 0.0005f) continue;
      partials[i].phase = spiralWrap01(partials[i].phase + freq * dt);
      mix += spiralSine(partials[i].phase) * amp;
      total += amp;
    }
    if (total > 0.01f) mix /= total;
    return mix * 0.92f;
  }

  float synthBarberPole(bool rising, float dt) {
    float mix = 0.0f, total = 0.0f;
    float amNorm = barberAMPhase / kTwoPi;
    for (int i = 0; i < spiral::NUM_PARTIALS; ++i) {
      float freq = spiralPartialFreq(i, sweepPosition, rising);
      float target = spiralShepardAmp(freq, centerFreq, envelopeWidth * 0.65f);
      partials[i].amplitude = smoothAmp(partials[i].amplitude, target);
      float amp = partials[i].amplitude;
      if (amp < 0.0005f) continue;
      partials[i].phase = spiralWrap01(partials[i].phase + freq * dt);
      float partialOffset = static_cast<float>(i) / spiral::NUM_PARTIALS;
      float am = 0.75f + 0.25f * sinf(kTwoPi * (amNorm + partialOffset));
      mix += spiralSine(partials[i].phase) * amp * am;
      total += amp;
    }
    if (total > 0.01f) mix /= total;
    return mix * 0.92f;
  }

  float synthRisset(bool accelerating, float dt) {
    float mix = 0.0f, total = 0.0f;
    float tempoMult = 0.3f + auxParam * 2.0f;
    float baseBPM = 72.0f * tempoMult;
    for (int i = 0; i < spiral::RISSET_LAYERS; ++i) {
      float layerRatio = powf(2.0f, static_cast<float>(i));
      float sweepMod = accelerating ? powf(2.0f, sweepPosition)
                                     : powf(2.0f, 1.0f - sweepPosition);
      float pulseFreq = (baseBPM / 60.0f) * layerRatio * sweepMod;
      float octaveFromCenter = log2f(layerRatio * sweepMod) - (spiral::RISSET_LAYERS / 2.0f);
      float target = spiralCosSqWindow(octaveFromCenter, envelopeWidth);
      rissetAmp[i] = smoothAmp(rissetAmp[i], target);
      float amp = rissetAmp[i];
      if (amp < 0.001f) continue;
      rissetPhase[i] = spiralWrap01(rissetPhase[i] + pulseFreq * dt);
      float phase01 = rissetPhase[i];
      float pulse;
      if (phase01 < 0.12f) {
        float t = phase01 / 0.12f;
        pulse = 0.5f + 0.5f * cosf(kPi * t);
      } else {
        pulse = 0.0f;
      }
      mix += pulse * amp;
      total += amp;
    }
    if (total > 0.01f) mix /= total;
    return mix * 2.0f - 1.0f;
  }

  float synthTritone(float dt) {
    float mix = 0.0f, total = 0.0f;
    for (int set = 0; set < 2; ++set) {
      float tritoneOffset = (set == 0) ? 0.0f : 0.5f;
      for (int i = 0; i < spiral::NUM_PARTIALS; ++i) {
        float sweep = fmodf(sweepPosition + tritoneOffset, 1.0f);
        float freq = spiralPartialFreq(i, sweep, true);
        int idx = set * spiral::NUM_PARTIALS + i;
        float target = spiralShepardAmp(freq, centerFreq, envelopeWidth);
        tritonePartials[idx].amplitude = smoothAmp(tritonePartials[idx].amplitude, target);
        float amp = tritonePartials[idx].amplitude;
        if (amp < 0.0005f) continue;
        tritonePartials[idx].phase = spiralWrap01(tritonePartials[idx].phase + freq * dt);
        mix += spiralSine(tritonePartials[idx].phase) * amp;
        total += amp;
      }
    }
    if (total > 0.01f) mix /= total;
    return mix * 0.88f;
  }

  float synthTritoneExplorer(float dt) {
    float mix = 0.0f, total = 0.0f;
    float pitchOffset = static_cast<float>(tritoneNoteClass) / 12.0f;
    for (int set = 0; set < 2; ++set) {
      float offset = (set == 0) ? pitchOffset : fmodf(pitchOffset + 0.5f, 1.0f);
      for (int i = 0; i < spiral::NUM_PARTIALS; ++i) {
        float octavePos = fmodf(static_cast<float>(i) + offset, static_cast<float>(spiral::NUM_PARTIALS));
        float freq = spiral::SHEPARD_BASE_HZ * powf(2.0f, octavePos);
        int idx = set * spiral::NUM_PARTIALS + i;
        float target = spiralShepardAmp(freq, centerFreq, envelopeWidth);
        tritonePartials[idx].amplitude = smoothAmp(tritonePartials[idx].amplitude, target);
        float amp = tritonePartials[idx].amplitude;
        if (amp < 0.0005f) continue;
        tritonePartials[idx].phase = spiralWrap01(tritonePartials[idx].phase + freq * dt);
        mix += spiralSine(tritonePartials[idx].phase) * amp;
        total += amp;
      }
    }
    if (total > 0.01f) mix /= total;
    return mix * 0.88f;
  }

  float synthCluster(bool rising, bool minor, float dt) {
    float mix = 0.0f, total = 0.0f;
    float intervals[3] = {0.0f, minor ? spiral::MINOR_THIRD : spiral::MAJOR_THIRD, spiral::PERFECT_FIFTH};
    float voiceGains[3] = {1.0f, 0.72f, 0.6f};
    for (int voice = 0; voice < 3; ++voice) {
      for (int i = 0; i < spiral::NUM_PARTIALS; ++i) {
        float octavePos = rising ? (static_cast<float>(i) + sweepPosition + intervals[voice])
                                  : (static_cast<float>(i) + (1.0f - sweepPosition) + intervals[voice]);
        octavePos = fmodf(octavePos, static_cast<float>(spiral::NUM_PARTIALS));
        if (octavePos < 0.0f) octavePos += spiral::NUM_PARTIALS;
        float freq = spiral::SHEPARD_BASE_HZ * powf(2.0f, octavePos);
        int idx = voice * spiral::NUM_PARTIALS + i;
        float target = spiralShepardAmp(freq, centerFreq, envelopeWidth * 0.8f) * voiceGains[voice];
        clusterPartials[idx].amplitude = smoothAmp(clusterPartials[idx].amplitude, target);
        float amp = clusterPartials[idx].amplitude;
        if (amp < 0.0005f) continue;
        clusterPartials[idx].phase = spiralWrap01(clusterPartials[idx].phase + freq * dt);
        mix += spiralSine(clusterPartials[idx].phase) * amp;
        total += amp;
      }
    }
    if (total > 0.01f) mix /= total;
    return mix * 0.88f;
  }

  float synthEulerSpiral(float dt) {
    float mix = 0.0f, total = 0.0f;
    for (int i = 0; i < spiral::NUM_PARTIALS; ++i) {
      float angle = eulerRotation + static_cast<float>(i) * kPi * 0.25f;
      float radius = 0.5f + static_cast<float>(i) * 0.15f * eulerSpread;
      float octaveOffset = radius * cosf(angle);
      float harmonicMix = 0.5f + 0.5f * sinf(angle);
      float baseOctave = static_cast<float>(i) + sweepPosition;
      baseOctave = fmodf(baseOctave + octaveOffset * 0.5f, static_cast<float>(spiral::NUM_PARTIALS));
      if (baseOctave < 0.0f) baseOctave += spiral::NUM_PARTIALS;
      float freq = spiral::SHEPARD_BASE_HZ * powf(2.0f, baseOctave);
      float target = spiralShepardAmp(freq, centerFreq, envelopeWidth);
      eulerPartials[i].amplitude = smoothAmp(eulerPartials[i].amplitude, target);
      float amp = eulerPartials[i].amplitude;
      if (amp < 0.0005f) continue;
      eulerPartials[i].phase = spiralWrap01(eulerPartials[i].phase + freq * dt);
      float s = spiralSine(eulerPartials[i].phase);
      // Exact triangle from the same phase (firmware used a band-limited
      // additive triangle LUT; the direct form matches the ClavesVoice choice).
      float tri = (2.0f / kPi) * asinf(clampf(s, -1.0f, 1.0f));
      float wave = s * (1.0f - harmonicMix * 0.4f) + tri * harmonicMix * 0.4f;
      mix += wave * amp;
      total += amp;
    }
    if (total > 0.01f) mix /= total;
    return mix * 0.9f;
  }
};

}  // namespace sc
