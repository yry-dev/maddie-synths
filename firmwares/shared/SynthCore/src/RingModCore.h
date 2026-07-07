#pragma once

// Ring Mod — carrier-oscillator ring modulator core.
//
// Used by:
//   - firmwares/mod2-ringmod/mod2-ringmod.ino
//   - rack-plugins/src/mod2-ringmod.cpp
//
// Classic ring modulation of the input against an internal carrier (a phase
// accumulator — phase-continuous across frequency changes). The carrier
// morphs sine -> triangle -> square (harsher products), and an AM blend
// leaks dry signal through the multiply (AM = half-dry). Three carrier
// modes cycle on the button:
//   FIXED — carrier at carrierHz (0.5 Hz - 5 kHz; sub-audio = tremolo-ish AM)
//   TRACK — carrier slews to trackRatio x the detected input pitch
//           (autocorrelation on a 4x-decimated buffer, ~50-1000 Hz, with a
//           confidence gate that freezes the carrier when tracking is bad)
//   S&H   — a new random carrier frequency on every trigger()
// trigger() also hard-syncs the carrier phase; the octaveDrop gate halves
// the carrier ("broken speaker"). The LED blinks at the carrier rate —
// visible sub-audio, solid above (ledLevel()).
//
// The pitch detector is deliberately split out of the audio path: process()
// only pushes decimated samples into a buffer; call analyzePitch() from a
// control-rate context (loop() in firmware, the audio thread in Rack — it
// runs every ~50 ms and costs ~45k MACs).
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop.

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Carrier mode (BUTTON short-press cycles these on hardware).
enum RingModMode : uint8_t {
  RINGMOD_FIXED = 0,
  RINGMOD_TRACK = 1,
  RINGMOD_SH = 2,
  RINGMOD_MODE_COUNT = 3
};

// POT1 -> carrier frequency: exponential 0.5 Hz (pot=0) .. 5 kHz (pot=1).
inline float ringModCarrierHz(float pot01) {
  return 0.5f * powf(10000.0f, clampf(pot01, 0.0f, 1.0f));
}

// POT1 in TRACK mode -> carrier/pitch ratio: exponential 0.25 .. 4.
inline float ringModTrackRatio(float pot01) {
  return 0.25f * powf(16.0f, clampf(pot01, 0.0f, 1.0f));
}

// --------------------------------------------------
// Time-domain pitch detector: normalized autocorrelation on a 4x-decimated
// buffer. push() is a few ops per audio sample; analyze() does the O(lags x
// window) search and must be called from a control-rate context.
// --------------------------------------------------
struct PitchTracker {
  static constexpr int kBufLen = 512;  // ~56 ms at 36.6 kHz / 4
  static constexpr int kWin = 256;     // correlation window

  float buf[kBufLen];
  int pos = 0;
  int decim = 0;
  float acc = 0.0f;
  bool ready = false;  // buffer full; push() pauses until analyze()

  void reset() {
    pos = 0;
    decim = 0;
    acc = 0.0f;
    ready = false;
  }

  // Call once per audio sample; averages groups of 4 (poor-man's decimation
  // filter). Stops filling once full so analyze() reads a stable snapshot.
  inline void push(float x) {
    if (ready) return;
    acc += x;
    if (++decim < 4) return;
    decim = 0;
    buf[pos] = acc * 0.25f;
    acc = 0.0f;
    if (++pos >= kBufLen) ready = true;
  }

  // Detected pitch in Hz, or 0 when unvoiced / low-confidence (the caller
  // should hold its previous value). `fsAudio` = the un-decimated rate.
  // Re-arms the buffer.
  float analyze(float fsAudio) {
    if (!ready) return 0.0f;
    const float fs4 = fsAudio * 0.25f;
    int minLag = (int)(fs4 / 1000.0f);  // 1 kHz ceiling
    if (minLag < 4) minLag = 4;
    int maxLag = (int)(fs4 / 50.0f);    // 50 Hz floor
    if (maxLag > kBufLen - kWin) maxLag = kBufLen - kWin;

    float e0 = 0.0f;
    for (int i = 0; i < kWin; i++) e0 += buf[i] * buf[i];

    float bestR = 0.0f;
    int bestLag = 0;
    for (int lag = minLag; lag <= maxLag; lag++) {
      float xy = 0.0f, ee = 0.0f;
      for (int i = 0; i < kWin; i++) {
        const float b = buf[i + lag];
        xy += buf[i] * b;
        ee += b * b;
      }
      // NSDF-style normalization: 2*sum(x*y) / (sum(x^2) + sum(y^2)).
      const float r = 2.0f * xy / (e0 + ee + 1e-9f);
      // Prefer the shortest lag that is nearly as good as the best so far
      // (octave-error guard): only replace on a clear improvement.
      if (r > bestR * 1.05f) {
        bestR = r;
        bestLag = lag;
      }
    }

    pos = 0;
    ready = false;  // re-arm (after the search: push() was paused)

    if (bestLag == 0 || bestR < 0.55f || e0 < 1e-4f) return 0.0f;
    return fs4 / (float)bestLag;
  }
};

struct RingModCore {
  // Parameters (write directly; see the mappers above).
  float carrierHz = 100.0f;  // FIXED-mode carrier frequency
  float trackRatio = 1.0f;   // TRACK-mode carrier/pitch ratio
  float shape = 0.0f;        // 0 sine .. 0.5 triangle .. 1 square
  float amBlend = 0.0f;      // 0 pure ring mod .. 1 AM (half-dry leak)
  float wet = 1.0f;          // 0 dry .. 1 fully wet
  uint8_t mode = RINGMOD_FIXED;
  bool octaveDrop = false;   // IN2 gate: carrier an octave down

  // State.
  float phase = 0.0f;
  float trackHz = 220.0f;   // last confident detected pitch
  float slewHz = 100.0f;    // slewed carrier frequency actually played
  float shHz = 100.0f;      // held random frequency (S&H mode)
  float lastCarrier = 0.0f; // last carrier sample (for the LED)
  uint32_t rng = 0x524D3163;  // "RM1c"
  PitchTracker tracker;

  void reset() {
    phase = 0.0f;
    trackHz = 220.0f;
    slewHz = carrierHz;
    shHz = carrierHz;
    lastCarrier = 0.0f;
    tracker.reset();
  }

  // IN1 rising edge: hard-sync the carrier; in S&H mode also sample a new
  // random frequency (exponential 50 Hz - 2 kHz).
  void trigger() {
    phase = 0.0f;
    if (mode == RINGMOD_SH)
      shHz = 50.0f * powf(40.0f, (float)(xorshift32(rng) >> 8) *
                                     (1.0f / 16777216.0f));
  }

  // Run the TRACK-mode pitch search if a buffer is ready. Call from a
  // control-rate context (see the header comment). Cheap no-op otherwise.
  void analyzePitch(float fsAudio) {
    const float hz = tracker.analyze(fsAudio);
    if (hz > 0.0f) trackHz = hz;  // confidence gate: hold otherwise
  }

  // 0..1 brightness for the panel LED — blinks at the carrier rate
  // (sub-audio visible, solid-looking above).
  float ledLevel() const { return 0.5f + 0.5f * lastCarrier; }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    if (mode == RINGMOD_TRACK) tracker.push(in);

    // Carrier frequency by mode, slewed (phase stays continuous anyway,
    // the slew just glides TRACK/S&H jumps instead of stepping them).
    float target = carrierHz;
    if (mode == RINGMOD_TRACK)
      target = clampf(trackHz * trackRatio, 0.5f, 8000.0f);
    else if (mode == RINGMOD_SH)
      target = shHz;
    if (octaveDrop) target *= 0.5f;
    slewHz += (target - slewHz) * onePoleCoef(20.0f, dt);

    phase += slewHz * dt;
    phase -= floorf(phase);

    // Carrier: sine -> triangle -> square morph.
    const float s = sinf(kTwoPi * phase);
    const float t = 1.0f - 4.0f * fabsf(phase - 0.5f);
    float carrier;
    if (shape < 0.5f) {
      carrier = s + (t - s) * (shape * 2.0f);
    } else {
      const float sq = clampf(t * 8.0f, -1.0f, 1.0f);  // softened square
      carrier = t + (sq - t) * (shape * 2.0f - 1.0f);
    }
    lastCarrier = carrier;

    // AM blend leaks dry through the multiply: AM = in * (0.5 + 0.5*c).
    const float m = carrier + (0.5f + 0.5f * carrier - carrier) * amBlend;
    const float wetSig = in * m;

    return in + (wetSig - in) * wet;
  }
};

}  // namespace sc
