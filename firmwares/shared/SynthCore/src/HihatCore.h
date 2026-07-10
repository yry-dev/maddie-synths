#pragma once

// Hi-hat percussion voice — shared core for the Hihat module.
//
// Used by:
//   - firmwares/mod2-hihat/mod2-hihat.ino  (fills TABLE_SZ-sample table per strike)
//   - rack-plugins/src/Hihat.cpp                (renders live, one sample per process)
//
// A strike is a fixed-length noise burst (kHihatStrikeDuration seconds) of
// band-pass filtered noise (sc::Biquad, Q=0.8) shaped by an exponential decay
// envelope, with fade-in (73 samples) and fade-out (40 samples) matching the
// firmware timing. Noise is either blue (first-difference of white, matching the
// firmware's generateBlueNoise()) or white, both generated via sc::xorshift32.
//
// Amplitude chain mirrors the firmware: the BPF output is multiplied by a body
// fade gain of 1.2 (firmware `float fade = 1.2f` during the main window), then by
// MASTER_ATTEN * AMP_SCALE = 0.8 * 3.5 = 2.8 (kHihatAmpScale) and clamped to
// -1..+1. This drives the output into saturation at the start of each strike,
// matching the firmware's characteristic clipping texture.
//
// Pure C++: depends only on sc_dsp.h (-> sc_math.h -> <math.h>/<stdint.h>).
// No Arduino.h, no rack.hpp, no Pico SDK.

#include "sc_dsp.h"

namespace sc {

// Strike duration matching TABLE_SZ=30000 samples at MOD2 AUDIO_FS (150 MHz / 4096).
constexpr float kHihatStrikeDuration = (30000.0f * 4096.0f) / 150000000.0f;  // 0.8192 s

// Fade fractions matching the firmware constants (FADE_IN_SMP=73, FADE_OUT_SMP=40).
constexpr float kHihatFadeInFrac  = 73.0f  / 30000.0f;  // ~0.002433
constexpr float kHihatFadeOutFrac = 40.0f  / 30000.0f;  // ~0.001333

// Body fade gain: the firmware applies `float fade = 1.2f` to samples that are
// outside the fade-in/fade-out windows.
constexpr float kHihatBodyFade = 1.2f;

// Amplitude scale: MASTER_ATTEN (0.8) * AMP_SCALE (3.5) from the firmware.
// With kHihatBodyFade applied first, effective body gain = 1.2 * 2.8 = 3.36.
constexpr float kHihatAmpScale = 2.8f;

// Noise mode (mirrors firmware noiseMode: 0=Blue, 1=White).
enum HihatNoiseMode { kHihatBlue = 0, kHihatWhite = 1 };

// One rendered sample: audio in -1..+1 (saturates at drive levels above ~0.3)
// and the envelope value 0..1 (suitable for LED brightness).
struct HihatFrame {
    float audio;
    float env;
};

struct HihatVoice {
    bool  playing     = false;
    float strikePhase = 0.0f;    // 0..1 across the full strike window
    float env         = 0.0f;    // current envelope amplitude
    float expK        = 1.0f;    // per-sample decay multiplier
    float prevWhite   = 0.0f;    // previous white noise sample (blue noise differentiation)
    uint32_t rngState = 12345u;  // xorshift32 PRNG state (must be non-zero; deterministic)
    HihatNoiseMode noiseMode = kHihatBlue;
    Biquad bpf;

    void reset() {
        playing = false;
        strikePhase = 0.0f;
        env = 0.0f;
        bpf.reset();
    }

    // Begin a new strike, capturing all parameters at this moment.
    //
    // decayBase  : 0.1..9.1   maps pot0: 0.1 + 9*norm0
    // decayCurve : 0.2..5.2   maps pot1: 0.2 + 5*norm1
    // fcHz       : 100..16000  maps pot2: 100 + 15900*norm2 (BPF centre frequency)
    // mode       : kHihatBlue or kHihatWhite
    // fs         : caller's sample rate in Hz; firmware passes AUDIO_FS (36621.09375)
    void strike(float decayBase, float decayCurve, float fcHz,
                HihatNoiseMode mode, float fs = 36621.09375f) {
        // Per-sample decay coefficient. At fs == AUDIO_FS this equals
        // expf(-decayBase * decayCurve / TABLE_SZ), matching the firmware exactly.
        expK = expf(-decayBase * decayCurve / (kHihatStrikeDuration * fs));
        noiseMode = mode;
        bpf.setBandpass(clampf(fcHz, 20.0f, fs * 0.499f), 0.8f, fs);
        bpf.reset();
        strikePhase = 0.0f;
        env = 1.0f;
        prevWhite = 0.0f;
        playing = true;
    }

    // Render one sample and advance by dt seconds (dt = 1 / sampleRate).
    // Returns silence once the strike window has elapsed (and clears `playing`).
    HihatFrame process(float dt) {
        HihatFrame f = {0.0f, 0.0f};
        if (!playing) return f;

        // --- Noise source ---
        float noiseSample;
        if (noiseMode == kHihatWhite) {
            noiseSample = noise1f(rngState);
        } else {
            // Blue noise: first-difference of white, scaled to -1..+1.
            // Matches firmware generateBlueNoise(): b = (w - prev) * 0.5f.
            const float w = noise1f(rngState);
            noiseSample = clampf((w - prevWhite) * 0.5f, -1.0f, 1.0f);
            prevWhite = w;
        }

        // --- Band-pass filter ---
        float y = bpf.process(noiseSample * env);

        // --- Fade ---
        // Firmware: fade = i/FADE_IN_SMP (rising), 1.2f (body), or
        //   (TABLE_SZ-i-1)/FADE_OUT_SMP (falling). We express the same as
        //   fractions of strikePhase.
        float fade;
        if (strikePhase < kHihatFadeInFrac) {
            fade = strikePhase / kHihatFadeInFrac;
        } else if (strikePhase > 1.0f - kHihatFadeOutFrac) {
            fade = (1.0f - strikePhase) / kHihatFadeOutFrac;
        } else {
            fade = kHihatBodyFade;
        }
        y *= fade;

        // --- Output (amplitude and LED env) ---
        f.audio = clampf(y * kHihatAmpScale, -1.0f, 1.0f);
        f.env   = clampf(env * fade, 0.0f, 1.0f);

        // --- Advance state ---
        env *= expK;
        strikePhase += dt / kHihatStrikeDuration;
        if (strikePhase >= 1.0f) {
            playing = false;
            f.audio = 0.0f;
            f.env   = 0.0f;
        }

        return f;
    }
};

}  // namespace sc
