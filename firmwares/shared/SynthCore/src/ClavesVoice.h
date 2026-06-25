#pragma once

// Claves percussion voice — the shared core of the Claves module.
//
// Used by:
//   - firmwares/mod2-claves/mod2-claves.ino  (renders TABLE_SZ samples per strike)
//   - vcvrack/src/Claves.cpp                  (renders live, one sample per process)
//
// A strike is a fixed-length window (kStrikeDuration seconds) of a sine<->triangle
// morph multiplied by an exponential-decay envelope, with a raised-cosine fade
// over the final 5% to kill the click. Both platforms drive it one sample at a
// time, passing their own dt, so it is sample-rate independent.
//
// Pure C++: depends only on sc_math.h. No Arduino / Rack / Pico SDK.

#include "sc_math.h"

namespace sc {

// Strike length in seconds. Mirrors the firmware's 8192-sample table at the
// MOD2 audio rate (AUDIO_FS = 150 MHz / 4096 = 36621.09375 Hz), derived from
// the same constants so dt/strikeDuration is exactly 1/8192 during the
// firmware table fill. Kept as a constant so the VCV Rack port produces an
// identically-long strike at any host sample rate.
constexpr float kClavesStrikeDuration = (8192.0f * 4096.0f) / 150000000.0f;  // ~0.2237 s

// Where the anti-click tail fade begins, as a fraction of the strike.
constexpr float kClavesFadeStart = 0.95f;

// Sine/triangle morph at normalised phase `phase01` (0..1). morph=0 -> sine,
// morph=1 -> triangle (triangle built as (2/pi)*asin(sin)). This is the exact
// waveform shared by firmware and VCV Rack.
inline float sineTriMorph(float phase01, float morph) {
  const float ph = phase01 * kTwoPi;
  const float s = sinf(ph);
  const float t = (2.0f / kPi) * asinf(clampf(s, -1.0f, 1.0f));
  return s * (1.0f - morph) + t * morph;
}

// One rendered sample: audio in -1..+1 and the envelope value in 0..1 (the
// firmware uses the latter for LED brightness).
struct ClavesFrame {
  float audio;
  float env;
};

struct ClavesVoice {
  bool playing = false;
  float strikePhase = 0.0f;  // 0..1 across the strike window
  float oscPhase = 0.0f;     // 0..1 oscillator phase
  float decayRate = 5.0f;    // envelope decay (1..10)
  float waveMorph = 0.0f;    // 0 = sine, 1 = triangle
  float freq = 200.0f;       // oscillator frequency (Hz)

  // Begin a new strike with freshly sampled controls.
  void strike(float decayRate_, float waveMorph_, float freqHz) {
    decayRate = decayRate_;
    waveMorph = waveMorph_;
    freq = freqHz;
    strikePhase = 0.0f;
    oscPhase = 0.0f;
    playing = true;
  }

  void reset() {
    playing = false;
    strikePhase = 0.0f;
    oscPhase = 0.0f;
  }

  // Render one sample and advance by `dt` seconds. Returns silence once the
  // strike window has elapsed (and clears `playing`). `strikeDuration` is the
  // window length in seconds (default kClavesStrikeDuration).
  ClavesFrame process(float dt, float strikeDuration = kClavesStrikeDuration) {
    ClavesFrame f;
    f.audio = 0.0f;
    f.env = 0.0f;
    if (!playing) return f;

    float env = expf(-decayRate * strikePhase);
    float y = sineTriMorph(oscPhase, waveMorph) * env;

    // Raised-cosine fade over the final 5% to prevent a click.
    if (strikePhase >= kClavesFadeStart) {
      const float mu = (strikePhase - kClavesFadeStart) / (1.0f - kClavesFadeStart);
      const float fade = 1.0f - raisedCosine(mu);
      y *= fade;
      env *= fade;
    }

    f.audio = y;
    f.env = env;

    // Advance oscillator (wrapped) and the strike window.
    oscPhase += freq * dt;
    oscPhase -= floorf(oscPhase);
    strikePhase += dt / strikeDuration;
    if (strikePhase >= 1.0f) {
      playing = false;
      f.audio = 0.0f;
      f.env = 0.0f;
    }
    return f;
  }
};

}  // namespace sc
