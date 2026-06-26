#pragma once

// TR-808-style clap percussion voice — shared core for the Clap module.
//
// Used by:
//   firmwares/mod2-clap/mod2-clap.ino  (fills TABLE_SZ-sample buffer per strike)
//   rack-plugins/src/Clap.cpp               (renders live, one sample per process call)
//
// Three noise bursts (0 ms, 15 ms, 30 ms from trigger): the first two are
// 4 ms gates; the third launches an exponential tail whose length is set by
// decayMs. All noise is band-pass filtered with a 2-pole biquad. The burst
// and tail envelopes match the original HAGIWO firmware exactly.
//
// Both platforms call process(dt) once per sample at their own native rate, so
// the voice is sample-rate independent: identical physical timing, identical
// filter response at their respective Nyquists.
//
// Pure C++: depends only on sc_dsp.h (which includes sc_math.h).
// No Arduino.h / rack.hpp / Pico SDK.

#include "sc_dsp.h"

namespace sc {

struct ClapFrame {
    float audio;  // -1..+1
    float env;    // 0..1  (LED brightness follows envelope)
};

struct ClapCore {
    // ---- Firmware-matching constants ----------------------------------------
    static constexpr float AUDIO_FS         = 150000000.0f / 4096.0f; // 36621.09375 Hz
    static constexpr float BURST_LEN_S      = 0.004f;   // 4 ms gated burst duration
    static constexpr float PULSE_INTERVAL_S = 0.015f;   // 15 ms between burst starts
    static constexpr int   BURSTS           = 3;         // total burst count
    static constexpr float DECAY_CURVE      = 2.0f;     // tail power (squared-exp shape)
    static constexpr float AMP_SCALE        = 3.5f;     // post-BPF gain
    static constexpr float MASTER_ATTEN     = 0.8f;     // −1.9 dB global attenuation
    // Fade-in: 70 samples at AUDIO_FS (~1.9 ms) — mirrors firmware FADE_IN_SMP
    static constexpr float FADE_IN_S        = 70.0f / AUDIO_FS;
    // Fade-out: 40 samples at AUDIO_FS (~1.1 ms) — mirrors firmware FADE_OUT_SMP
    static constexpr float FADE_OUT_S       = 40.0f / AUDIO_FS;
    // Total active duration: firmware TABLE_SZ = 22000 samples at AUDIO_FS (~0.6 s)
    static constexpr float TOTAL_DURATION_S = 22000.0f / AUDIO_FS;

    // ---- Voice state --------------------------------------------------------
    bool      playing  = false;
    float     t        = 0.0f;         // seconds elapsed since trigger
    float     tailEnv  = 0.0f;         // running tail level; starts at 1.0 when tail begins
    float     expK     = 0.0f;         // per-sample tail decay (computed at strike time)
    uint32_t  rngState = 0x1337dead;   // xorshift32 seed — must be non-zero; NOT reset on strike
    sc::Biquad bpf;

    // Strike: latch new parameters, reset timing, and begin playback.
    //   decayMs — tail decay time constant (20–200 ms)
    //   fc      — BPF centre frequency (50–8000 Hz)
    //   q       — BPF Q factor (0.5–4.0)
    //   fs      — caller's sample rate (Hz).
    //             Firmware passes AUDIO_FS; VCV Rack passes args.sampleRate.
    //             expK is computed from fs so the tail length is identical in
    //             physical time on both platforms.
    void strike(float decayMs, float fc, float q, float fs) {
        bpf.reset();
        bpf.setBandpass(fc, q, fs);
        expK    = expf(-1.0f / (decayMs / 1000.0f * fs));
        tailEnv = 1.0f;
        t       = 0.0f;
        playing = true;
    }

    void reset() {
        playing = false;
        t       = 0.0f;
        tailEnv = 0.0f;
        bpf.reset();
    }

    // Advance one sample (dt seconds). Returns {0,0} when not playing.
    ClapFrame process(float dt) {
        ClapFrame f = {0.0f, 0.0f};
        if (!playing) return f;

        // --- Burst/tail envelope ---
        float curEnv = 0.0f;
        const float tailStart = float(BURSTS - 1) * PULSE_INTERVAL_S; // 0.030 s

        if (t < tailStart) {
            // Gated noise bursts 0 and 1 (4 ms each, at 0 ms and 15 ms)
            for (int b = 0; b < BURSTS - 1; ++b) {
                float ts = float(b) * PULSE_INTERVAL_S;
                if (t >= ts && t < ts + BURST_LEN_S) {
                    curEnv = 1.0f;
                    break;
                }
            }
        } else {
            // Burst 2: exponential tail with curvature (mirrors firmware powf logic)
            curEnv  = powf(tailEnv, DECAY_CURVE);
            tailEnv *= expK;
        }

        // Anti-click fade-in at the very start (mirrors firmware FADE_IN_SMP ramp)
        if (t < FADE_IN_S)
            curEnv *= t / FADE_IN_S;

        // Noise source → BPF → gain stage
        float noise    = sc::noise1f(rngState);
        float filtered = bpf.process(noise * curEnv);
        float out      = sc::clampf(filtered * AMP_SCALE * MASTER_ATTEN, -1.0f, 1.0f);

        // Anti-click fade-out over the final ~40 samples (mirrors firmware FADE_OUT_SMP)
        if (t > TOTAL_DURATION_S - FADE_OUT_S) {
            float fo = sc::clampf((TOTAL_DURATION_S - t) / FADE_OUT_S, 0.0f, 1.0f);
            out    *= fo;
            curEnv *= fo;
        }

        f.audio = out;
        f.env   = curEnv;

        t += dt;
        if (t >= TOTAL_DURATION_S)
            playing = false;

        return f;
    }
};

}  // namespace sc
