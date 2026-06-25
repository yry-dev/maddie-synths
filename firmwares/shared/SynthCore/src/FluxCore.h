#pragma once

// Flux multi-mode physical-modelling / resonance / noise voice — the shared core
// of the Flux module.
//
// Used by:
//   - firmwares/mod2-flux/mod2-flux.ino  (one sample per PWM-wrap ISR)
//   - vcvrack/src/Flux.cpp               (one sample per process())
//
// Seven modes in three groups (selected by setMode, 0..6):
//   RESONANCE: 0 Modal (tuned resonator bank), 1 Karplus (plucked string)
//   NOISE:     2 White, 3 Pink (1/f), 4 S&H (stepped), 5 Quantum (Lorenz chaos)
//   TEXTURE:   6 Drone (evolving harmonic texture)
//
// The voice owns its own time-based behaviour (auto-excite for Modal, auto-pluck
// for Karplus, slow target-amp evolution for Drone) so both platforms get the
// same self-running sound; trigger() additionally fires a manual pluck/excite.
//
// Pure C++: depends only on sc_math.h and sc_dsp.h. No Arduino / Rack / Pico SDK;
// float-only, no heap, no STL.
//
// Sample-rate handling. Everything pitch-determining is sample-rate independent:
// oscillator/clock phase advances by freq*dt and delay-line / Nyquist maths use
// fs = 1/dt, so pitch tracks at any host rate. The Lorenz integration step in the
// Quantum mode is scaled by (kFluxAudioFs * dt) so its chaos "pitch" matches the
// firmware at any rate. The per-sample *decay / feedback / smoothing* coefficients
// (e.g. modal 0.9985 decay, Karplus 0.998 feedback, drone 0.00002 amp glide, the
// 0.08 Quantum smoother) are calibrated for the MOD2 audio rate (kFluxAudioFs ~=
// 36621 Hz); at other host rates the corresponding *decay times* drift slightly
// while pitch stays correct. The Karplus delay buffer is a fixed 4096 samples, so
// the lowest pitch is bounded by fs/4094 (well below the 32 Hz floor up to ~131
// kHz; above that the floor rises). These are deliberate, documented convergences.

#include "sc_math.h"
#include "sc_dsp.h"
#include "LorenzVoice.h"

namespace sc {

// MOD2 reference audio rate (150 MHz / 4096). Used only to keep the Quantum
// chaos rate identical to the firmware across host sample rates.
static constexpr float kFluxAudioFs = 150000000.0f / 4096.0f;  // ~36621.09 Hz

static constexpr int kFluxNumModes      = 7;
static constexpr int kFluxResModes      = 12;   // modal resonator bank size
static constexpr int kFluxKsBufferSize  = 4096; // Karplus delay line (power of two)
static constexpr int kFluxKsBufferMask  = kFluxKsBufferSize - 1;
static constexpr int kFluxPinkStages    = 6;    // Voss-McCartney rows
static constexpr int kFluxDroneHarmonics = 16;

class FluxVoice {
 public:
  // Current mode (0..6). Public so the firmware can cycle it with the button.
  uint8_t mode = 0;

  // Restore the power-on state (matches the firmware's setup()).
  void reset() {
    centerFreq = 440.0f;
    speed = 0.5f;
    aux = 0.5f;

    noiseState = 0x12345678u;

    for (int i = 0; i < kFluxResModes; ++i) {
      resModes[i].phase = 0.0f;
      resModes[i].velocity = 0.0f;
    }
    resExcitation = 1.0f;  // firmware fires an initial excitation at boot

    for (int i = 0; i < kFluxKsBufferSize; ++i) ksBuffer[i] = 0.0f;
    ksWriteIdx = 0;
    ksLastOut = 0.0f;
    ksLastOut2 = 0.0f;
    ksNeedsPluck = true;

    for (int i = 0; i < kFluxPinkStages; ++i) pinkRows[i] = 0.0f;
    pinkIndex = 0;
    pinkRunningSum = 0.0f;

    shValue = 0.0f;
    shTarget = 0.0f;
    shPhase = 0.0f;

    lorenz.reset();   // x=0.1, y=0, z=0 — same seed as the firmware
    quantumOut = 0.0f;

    for (int i = 0; i < kFluxDroneHarmonics; ++i) {
      dronePhase[i] = 0.0f;
      droneAmps[i] = 0.3f + 0.7f * rnd01();
      droneTargetAmps[i] = droneAmps[i];
    }

    exciteTimer = 0.0f;
    pluckTimer = 0.0f;
    droneTimer = 0.0f;

    dcBlocker = DcBlocker();
    lp1 = OutputLpBiquad();
    lp2 = OutputLpBiquad();

    fired = false;
  }

  // Select a mode (0..6). Re-arms the resonance modes the firmware re-arms on a
  // short button press.
  void setMode(uint8_t m) {
    if (m >= kFluxNumModes) m = kFluxNumModes - 1;
    if (m == mode) return;
    mode = m;
    if (mode == 0) resExcitation = 1.0f;
    if (mode == 1) ksNeedsPluck = true;
  }

  // Update live controls. `freqHz` is the centre/pitch frequency, `speed01` the
  // rate / chaos-rate control, `aux01` the per-mode character/brightness/slew.
  void setParams(float freqHz, float speed01, float aux01) {
    centerFreq = freqHz;
    speed = speed01;
    aux = aux01;
  }

  // Manual trigger (button long-press / external gate). Plucks Karplus or
  // excites the modal bank, exactly like the firmware long-press.
  void trigger() {
    if (mode == 0) resExcitation = 1.2f;
    else if (mode == 1) ksNeedsPluck = true;
    fired = true;
  }

  // Returns and clears the "a trigger fired" flag (auto or manual) for the LED.
  bool consumeFired() {
    bool r = fired;
    fired = false;
    return r;
  }

  // Render one sample (audio in -1..+1) and advance by `dt` seconds. Includes the
  // shared output stage (DC block, low-pass, soft-sat) so both platforms sound
  // identical.
  float process(float dt) {
    const float fs = 1.0f / dt;

    advanceTimers(dt);

    float sample = 0.0f;
    switch (mode) {
      case 0:  sample = synthModal(dt, fs);   break;
      case 1:  sample = synthKarplus(dt, fs); break;
      case 2:  sample = synthWhite();         break;
      case 3:  sample = synthPink();          break;
      case 4:  sample = synthSH(dt);          break;
      case 5:  sample = synthQuantum(dt);     break;
      case 6:
      default: sample = synthDrone(dt, fs);   break;
    }

    sample = dcBlocker.process(sample);

    // Noise modes get a single low-pass; tonal modes get the steeper two-pole.
    if (mode >= 2 && mode <= 5) {
      sample = lp1.process(sample);
    } else {
      sample = lp1.process(sample);
      sample = lp2.process(sample);
    }

    return softSat(sample);
  }

 private:
  // ----- live controls -----
  float centerFreq = 440.0f;
  float speed = 0.5f;
  float aux = 0.5f;

  // ----- noise -----
  uint32_t noiseState = 0x12345678u;

  // 0..1 random helper sharing the noise state.
  inline float rnd01() {
    return (float)(xorshift32(noiseState) >> 8) * (1.0f / 16777216.0f);
  }

  // ----- modal resonator bank -----
  struct ResMode { float phase; float velocity; };
  ResMode resModes[kFluxResModes];
  float resExcitation = 1.0f;

  // ----- Karplus-Strong -----
  float ksBuffer[kFluxKsBufferSize];
  int   ksWriteIdx = 0;
  float ksLastOut = 0.0f;
  float ksLastOut2 = 0.0f;
  bool  ksNeedsPluck = true;

  // ----- pink noise (Voss-McCartney) -----
  float pinkRows[kFluxPinkStages];
  int   pinkIndex = 0;
  float pinkRunningSum = 0.0f;

  // ----- sample & hold -----
  float shValue = 0.0f;
  float shTarget = 0.0f;
  float shPhase = 0.0f;  // 0..1 clock phase

  // ----- quantum (Lorenz chaos) -----
  LorenzVoice lorenz;
  float quantumOut = 0.0f;

  // ----- harmonic drone -----
  float dronePhase[kFluxDroneHarmonics];      // 0..1 per harmonic
  float droneAmps[kFluxDroneHarmonics];
  float droneTargetAmps[kFluxDroneHarmonics];

  // ----- self-running timers (seconds) -----
  float exciteTimer = 0.0f;
  float pluckTimer = 0.0f;
  float droneTimer = 0.0f;

  // ----- shared output stage -----
  DcBlocker dcBlocker;
  OutputLpBiquad lp1, lp2;

  bool fired = false;

  // --------------------------------------------------
  // Time-based auto behaviour (was the firmware's loop() bookkeeping).
  // --------------------------------------------------
  void advanceTimers(float dt) {
    if (mode == 0) {
      exciteTimer += dt;
      const float interval = (3000.0f - speed * 2700.0f) * 0.001f;  // seconds
      if (exciteTimer >= interval) {
        resExcitation = 0.7f + aux * 0.5f;
        exciteTimer = 0.0f;
        fired = true;
      }
    } else if (mode == 1) {
      pluckTimer += dt;
      const float interval = (3500.0f - speed * 3200.0f) * 0.001f;
      if (pluckTimer >= interval) {
        ksNeedsPluck = true;
        pluckTimer = 0.0f;
        fired = true;
      }
    } else if (mode == 6) {
      droneTimer += dt;
      const float interval = (4000.0f - speed * 3500.0f) * 0.001f;
      if (droneTimer >= interval) {
        droneTimer = 0.0f;
        for (int i = 0; i < kFluxDroneHarmonics; ++i)
          droneTargetAmps[i] = 0.1f + 0.9f * rnd01();
      }
    }
  }

  // --------------------------------------------------
  // Mode 0: modal resonator bank
  // --------------------------------------------------
  float synthModal(float dt, float fs) {
    float mix = 0.0f;

    float excite = 0.0f;
    if (resExcitation > 0.001f) {
      excite = noise1f(noiseState) * resExcitation;
      resExcitation *= 0.994f;
    }

    const float baseDecay = 0.9985f + speed * 0.0014f;

    for (int i = 0; i < kFluxResModes; ++i) {
      float modeRatio;
      if (aux < 0.25f) {
        // Harmonic (string-like)
        modeRatio = static_cast<float>(i + 1);
      } else if (aux < 0.5f) {
        // Slightly inharmonic (piano-like)
        const float n = static_cast<float>(i + 1);
        const float inharmonicity = (aux - 0.25f) * 4.0f * 0.001f;
        modeRatio = n * (1.0f + inharmonicity * n * n);
      } else if (aux < 0.75f) {
        // More inharmonic (marimba-like)
        const float ratios[12] = {1.0f, 2.76f, 5.4f, 8.93f, 13.34f, 18.64f,
                                   24.82f, 31.87f, 39.81f, 48.62f, 58.31f, 68.88f};
        const float blend = (aux - 0.5f) * 4.0f;
        const float harm = static_cast<float>(i + 1);
        modeRatio = harm * (1.0f - blend) + ratios[i] * blend;
      } else {
        // Bell-like (very inharmonic)
        const float ratios[12] = {1.0f, 1.88f, 2.83f, 3.76f, 4.67f, 5.52f,
                                   6.35f, 7.15f, 7.93f, 8.69f, 9.43f, 10.15f};
        modeRatio = ratios[i];
      }

      const float modeFreq = centerFreq * modeRatio;
      if (modeFreq > fs * 0.45f) continue;

      float modeDecay = baseDecay - static_cast<float>(i) * 0.0003f;
      if (modeDecay < 0.99f) modeDecay = 0.99f;

      const float phaseInc = modeFreq * kTwoPi * dt;

      const float exciteGain = 1.0f / (1.0f + static_cast<float>(i) * 0.4f);
      resModes[i].velocity += excite * exciteGain;

      resModes[i].phase += phaseInc;
      if (resModes[i].phase > kTwoPi) resModes[i].phase -= kTwoPi;

      resModes[i].velocity *= modeDecay;

      const float modeGain = 1.0f / (1.0f + static_cast<float>(i) * 0.25f);
      mix += resModes[i].velocity * sinf(resModes[i].phase) * modeGain;
    }

    return mix * 0.35f;
  }

  // --------------------------------------------------
  // Mode 1: Karplus-Strong plucked string
  // --------------------------------------------------
  float synthKarplus(float dt, float fs) {
    if (ksNeedsPluck) {
      int period = static_cast<int>(fs / centerFreq);
      if (period > kFluxKsBufferSize - 1) period = kFluxKsBufferSize - 1;
      if (period < 2) period = 2;

      // Shaped noise burst (raised-cosine window + a short impulse front).
      for (int i = 0; i < period; ++i) {
        const float env =
            0.5f - 0.5f * cosf(kTwoPi * static_cast<float>(i) / static_cast<float>(period));
        const float impulse =
            (i < period / 8) ? (1.0f - static_cast<float>(i) / (period / 8.0f)) : 0.0f;
        const float noise = noise1f(noiseState);
        ksBuffer[i] = (noise * 0.7f + impulse * 0.3f) * env;
      }
      for (int i = period; i < kFluxKsBufferSize; ++i) ksBuffer[i] = 0.0f;

      ksWriteIdx = period;
      ksNeedsPluck = false;
      ksLastOut = 0.0f;
      ksLastOut2 = 0.0f;
    }

    float delayLength = fs / centerFreq;
    if (delayLength > kFluxKsBufferSize - 2) delayLength = kFluxKsBufferSize - 2;
    if (delayLength < 2.0f) delayLength = 2.0f;

    float readPos = static_cast<float>(ksWriteIdx) - delayLength;
    while (readPos < 0.0f) readPos += kFluxKsBufferSize;

    const int idx0 = static_cast<int>(readPos) & kFluxKsBufferMask;
    const int idx1 = (idx0 + 1) & kFluxKsBufferMask;
    const int idx_1 = (idx0 - 1 + kFluxKsBufferSize) & kFluxKsBufferMask;
    const int idx2 = (idx0 + 2) & kFluxKsBufferMask;

    const float frac = readPos - floorf(readPos);

    // Catmull-Rom interpolation.
    const float y_1 = ksBuffer[idx_1];
    const float y0 = ksBuffer[idx0];
    const float y1 = ksBuffer[idx1];
    const float y2 = ksBuffer[idx2];

    const float c0 = y0;
    const float c1 = 0.5f * (y1 - y_1);
    const float c2 = y_1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    const float c3 = 0.5f * (y2 - y_1) + 1.5f * (y0 - y1);

    const float sample = ((c3 * frac + c2) * frac + c1) * frac + c0;

    // Two-point averaging filter with controllable damping.
    const float filterCoef = 0.2f + aux * 0.5f;
    float filtered = ksLastOut * filterCoef + sample * (1.0f - filterCoef);

    // One-pole brightness control.
    const float brightness = 0.3f + aux * 0.6f;
    filtered = ksLastOut2 + (filtered - ksLastOut2) * brightness;
    ksLastOut2 = filtered;

    ksLastOut = sample;

    const float feedback = 0.998f - (1.0f - aux) * 0.015f;
    ksBuffer[ksWriteIdx] = filtered * feedback;
    ksWriteIdx = (ksWriteIdx + 1) & kFluxKsBufferMask;

    return filtered * 0.95f;
  }

  // --------------------------------------------------
  // Mode 2: white noise
  // --------------------------------------------------
  float synthWhite() {
    const float level = 0.4f + aux * 0.55f;
    return noise1f(noiseState) * level;
  }

  // --------------------------------------------------
  // Mode 3: pink noise (Voss-McCartney)
  // --------------------------------------------------
  float pinkNoise() {
    int numZeros = __builtin_ctz(pinkIndex + 1);
    if (numZeros >= kFluxPinkStages) numZeros = kFluxPinkStages - 1;

    pinkRunningSum -= pinkRows[numZeros];
    pinkRows[numZeros] = noise1f(noiseState) * 0.5f;
    pinkRunningSum += pinkRows[numZeros];

    pinkIndex = (pinkIndex + 1) & 63;

    return (pinkRunningSum + noise1f(noiseState) * 0.5f) * 0.22f;
  }

  float synthPink() {
    const float level = 0.5f + aux * 0.5f;
    return pinkNoise() * level;
  }

  // --------------------------------------------------
  // Mode 4: sample & hold (stepped noise)
  // --------------------------------------------------
  float synthSH(float dt) {
    float sampleFreq = centerFreq * 0.05f;
    if (sampleFreq < 0.5f) sampleFreq = 0.5f;
    if (sampleFreq > 500.0f) sampleFreq = 500.0f;

    shPhase += sampleFreq * dt;
    if (shPhase >= 1.0f) {  // clock wrap = grab a new value
      shPhase -= 1.0f;
      shTarget = noise1f(noiseState);
    }

    // Slew rate from aux (0 = instant, 1 = very smooth).
    const float slewRate = 0.01f + (1.0f - aux) * 0.99f;
    shValue += (shTarget - shValue) * slewRate;

    return shValue * 0.95f;
  }

  // --------------------------------------------------
  // Mode 5: quantum (Lorenz attractor chaos)
  // --------------------------------------------------
  float synthQuantum(float dt) {
    const float sigma = 10.0f;
    const float rho = 28.0f;
    const float beta = 8.0f / 3.0f;

    // Integration step from speed, scaled so the chaos rate matches the firmware
    // at any host sample rate (== 1 at kFluxAudioFs).
    const float stepDt = (0.00005f + speed * 0.0006f) * (kFluxAudioFs * dt);

    for (int step = 0; step < 2; ++step) {
      lorenz.step(sigma, rho, beta, stepDt);
    }

    // Soft bounds (mirror the firmware clamps).
    if (lorenz.x > 50.0f) lorenz.x = 50.0f;
    if (lorenz.x < -50.0f) lorenz.x = -50.0f;
    if (lorenz.y > 50.0f) lorenz.y = 50.0f;
    if (lorenz.y < -50.0f) lorenz.y = -50.0f;
    if (lorenz.z > 80.0f) lorenz.z = 80.0f;
    if (lorenz.z < 0.0f) lorenz.z = 0.1f;

    const float chaosOut = (lorenz.x * 0.7f + lorenz.y * 0.3f) / 25.0f;
    const float uncertainty = noise1f(noiseState) * 0.2f * aux;

    quantumOut += (chaosOut + uncertainty - quantumOut) * 0.08f;
    return quantumOut;
  }

  // --------------------------------------------------
  // Mode 6: harmonic drone
  // --------------------------------------------------
  float synthDrone(float dt, float fs) {
    float mix = 0.0f;

    for (int i = 0; i < kFluxDroneHarmonics; ++i) {
      const int harmonic = i + 1;
      const float freq = centerFreq * static_cast<float>(harmonic);
      if (freq > fs * 0.45f) continue;

      droneAmps[i] += (droneTargetAmps[i] - droneAmps[i]) * 0.00002f;

      dronePhase[i] += freq * dt;
      dronePhase[i] -= floorf(dronePhase[i]);

      const float baseAmp = 1.0f / (1.0f + 0.3f * static_cast<float>(harmonic));
      mix += sinf(dronePhase[i] * kTwoPi) * baseAmp * droneAmps[i];
    }

    return mix * 0.16f;
  }
};

}  // namespace sc
