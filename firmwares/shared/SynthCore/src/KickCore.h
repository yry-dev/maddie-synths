#pragma once

// Kick drum voice — the shared core of the Kick module.
//
// Used by:
//   - firmwares/mod2-kick/mod2-kick.ino  (renders a table per trigger)
//   - rack-plugins/src/Kick.cpp               (renders live, one sample per process)
//
// A strike is a sine oscillator whose frequency sweeps exponentially from f0 to
// f1 (shaped by a curve exponent), multiplied by an exponential-decay amplitude
// envelope, soft-clipped (tanh), with a raised-cosine fade over the final 5% to
// kill the tail click. Both platforms drive it one sample at a time, passing
// their own dt, so it is sample-rate independent.
//
// Model / convergence note:
//   The original firmware built a 2048-point sine table at table-time dt and
//   then RESAMPLED it during playback by `pitchMultiplier` (so pitchMult both
//   raised the pitch and shortened the kick: playback length = T / pitchMult).
//   This core folds both effects into the live synthesis: `pitchMult` scales the
//   oscillator frequency *and* the rate the strike window advances, which is
//   numerically the same as the old resample. The firmware now fills its table
//   at the audio rate (process(1/AUDIO_FS), the Claves pattern) and plays it
//   back at a fixed rate instead of resampling — cleaner (no table
//   interpolation, no per-sample LUTs) and identical within float tolerance.
//   One consequence: pitchMult is latched at the strike instead of being read
//   live by the playback ISR (kicks are <0.6 s, so this is inaudible).
//
// Pure C++: depends only on sc_math.h / sc_dsp.h. No Arduino / Rack / Pico SDK.

#include "sc_math.h"
#include "sc_dsp.h"

namespace sc {

// Playback duration of the strike window (seconds) at pitchMult = 1. This is
// the firmware's T: the old 2048-sample table, resampled at pitchMult = 1,
// played back over exactly this long. Kept as a constant so the VCV Rack port
// produces an identically-long strike at any host sample rate.
constexpr float kKickStrikeDuration = 0.3f;

// Where the anti-click tail fade begins, as a fraction of the strike.
constexpr float kKickFadeStart = 0.95f;

// One rendered sample: audio in -1..+1 and the envelope value in 0..1 (the VCV
// port uses the latter for LED brightness).
struct KickFrame {
  float audio;
  float env;
};

struct KickVoice {
  bool playing = false;
  float strikePhase = 0.0f;  // 0..1 across the strike window
  float oscPhase = 0.0f;     // oscillator phase in radians

  // Parameters (engine units), set via setParams().
  float pitchMult = 1.0f;    // playback pitch / speed scaling (0.5..2.0)
  float softClipRate = 1.0f; // tanh soft-clip hardness (0.5..10)
  float decayRate = 5.0f;    // amplitude envelope decay (1..10)
  float f0 = 250.0f;         // sweep start frequency (Hz)
  float f1 = 50.0f;          // sweep end frequency (Hz)
  float curveExp = 0.6f;     // pitch-envelope curve exponent (0.1..2.0)

  float reduceLevel = 1.0f;  // accent attenuation captured at strike (1.0 or 0.5)

  // Set the six synthesis parameters (the firmware's two modes flattened into
  // one call; in VCV each is its own knob).
  void setParams(float pitchMult_, float softClip_, float decay_,
                 float startFreq_, float endFreq_, float curve_) {
    pitchMult = pitchMult_;
    softClipRate = softClip_;
    decayRate = decay_;
    f0 = startFreq_;
    f1 = endFreq_;
    curveExp = curve_;
  }

  // Begin a new strike. `reduceLevel_` is the accent attenuation (1.0 normal,
  // 0.5 when the accent input is high), captured for the whole strike.
  void strike(float reduceLevel_ = 1.0f) {
    reduceLevel = reduceLevel_;
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
  // strike window has elapsed (and clears `playing`).
  KickFrame process(float dt) {
    KickFrame fr;
    fr.audio = 0.0f;
    fr.env = 0.0f;
    if (!playing) return fr;

    const float p = strikePhase;

    // Exponential pitch sweep f0 -> f1, shaped by the curve exponent. Computed
    // directly with powf (the firmware used a 256-pt curve LUT + 8-segment
    // ratio LUT; direct powf is cleaner with no LUT quantization).
    const float xAdj = powf(p, curveExp);
    const float f = f0 * powf(f1 / f0, xAdj);

    // Exponential amplitude decay.
    float env = expf(-decayRate * p);

    // Sine, accent attenuation, envelope, then normalised tanh soft-clip.
    float y = softClipTanh(sinf(oscPhase) * reduceLevel * env, softClipRate);

    // Raised-cosine fade over the final 5% to prevent a click.
    if (p >= kKickFadeStart) {
      const float mu = (p - kKickFadeStart) / (1.0f - kKickFadeStart);
      const float fade = 1.0f - raisedCosine(mu);
      y *= fade;
      env *= fade;
    }

    fr.audio = y;
    fr.env = env;

    // Advance the oscillator (frequency scaled by pitchMult, like the old
    // resample) and the strike window (which also runs pitchMult faster).
    oscPhase += kTwoPi * f * pitchMult * dt;
    oscPhase -= kTwoPi * floorf(oscPhase * (1.0f / kTwoPi));
    strikePhase += (pitchMult / kKickStrikeDuration) * dt;
    if (strikePhase >= 1.0f) {
      playing = false;
      fr.audio = 0.0f;
      fr.env = 0.0f;
    }
    return fr;
  }
};

}  // namespace sc
