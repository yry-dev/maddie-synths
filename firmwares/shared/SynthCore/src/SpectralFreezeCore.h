#pragma once

// Spectral Freeze — FFT-domain infinite sustain / phase-vocoder freeze core.
//
// Used by:
//   - firmwares/mod2-spectral-freeze/mod2-spectral-freeze.ino
//   - rack-plugins/src/mod2-spectral-freeze.cpp
//
// Freeze the *spectrum* rather than the waveform: an STFT analyses the input,
// and on freeze the per-bin magnitudes are held while each bin's phase keeps
// advancing (by its nominal centre-bin rate + optional random jitter). The
// result is an endless, motionless-yet-alive pad captured from any instant of
// sound. Resynthesis is an inverse FFT + windowed 75%-overlap-add, so the
// unfrozen path reconstructs the input to unity and the freeze engages/releases
// with a click-free Cartesian-domain crossfade.
//
// Signal flow (classic phase vocoder):
//   input --window--> FFT --[hold mag / advance phase]--> IFFT --window--> OLA
//
// Structure (README: 1024-pt FFT, Hann, hop 256 = 75% overlap):
//   - N = 1024 point complex radix-2 FFT (twiddle + window tables filled by
//     init(); NO compile-time float tables, NO heap, NO STL).
//   - Hop H = 256 samples. Latency = N - H = 768 samples (~21 ms @ 36.6 kHz,
//     ~17 ms @ 44.1 kHz) — irrelevant for a freeze effect. Frame timing scales
//     with the sample rate but the frozen texture is perceptually rate-stable
//     (magnitudes are held; phase advance is in radians/hop, not Hz).
//   - Per-hop cost (one FFT + one IFFT + per-bin mag/phase) is amortised over
//     the 256-sample hop. On the RP2350 the firmware offloads the frame work to
//     the second core / loop() behind ring buffers (see the .ino); in VCV Rack
//     process() runs the burst inline at the hop boundary.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL. (This is a
// MOD2/desktop-only engine — far too heavy for the AVR MOD1 target — but it
// still follows the SynthCore portability rules.)

#include <math.h>
#include <stdint.h>
#include "sc_math.h"
#include "sc_dsp.h"

namespace sc {

// Freeze character (BUTTON short-press cycles these on hardware).
enum SpectralFreezeMode : uint8_t {
  SPFREEZE_SINGLE = 0,  // single captured frame (crispest)
  SPFREEZE_AVG = 1,     // ~4-frame averaged spectrum (smoother, less flutter)
  SPFREEZE_DRIFT = 2,   // slow bounded random walk of the bin magnitudes (alive)
  SPFREEZE_MODE_COUNT = 3
};

// POT1 -> per-bin phase-randomisation (shimmer/animation) depth, radians/hop.
inline float spectralShimmerRad(float pot01) {
  return 0.6f * clampf(pot01, 0.0f, 1.0f);  // 0 = coherent tone .. ~0.6 rad jitter
}

// POT2 -> spectral tilt slope in nepers across the band (center 0.5 = flat,
// 0 = dark/low tilt, 1 = bright/high tilt; ~+/-12 dB edge-to-edge).
inline float spectralTiltSlope(float pot01) {
  return (clampf(pot01, 0.0f, 1.0f) - 0.5f) * 2.0f * 1.40f;
}

// BUTTON+POT2 -> freeze engage/release crossfade ("attack/blend") time,
// 5 ms (snap) .. ~2 s (slow swell), exponential taper. 0.3 ~= the old 30 ms.
inline float spectralAttackSec(float pot01) {
  return 0.005f * powf(400.0f, clampf(pot01, 0.0f, 1.0f));
}

struct SpectralFreezeCore {
  // ---- fixed structure (README: 1024 / hop 256 / 75% overlap) -------------
  static const int N = 1024;         // FFT size
  static const int H = 256;          // hop (75% overlap)
  static const int HALF = N / 2;     // Nyquist bin index
  static const int NB = N / 2 + 1;   // number of unique bins (DC..Nyquist)
  static const int LAT = N - H;      // reconstruction latency (samples)

  // ---- parameters (write directly; see the mappers above) -----------------
  float shimmer = 0.0f;   // POT1 phase jitter depth (radians/hop)
  float tiltSlope = 0.0f; // POT2 spectral tilt (nepers, +/- across band)
  float wet = 1.0f;       // MIX: 0 dry .. 1 frozen/live texture
  float attackSec = 0.03f;// BUTTON+POT2: freeze engage/release crossfade time
  uint8_t mode = SPFREEZE_SINGLE;
  bool freeze = false;    // IN2 freeze gate

  // ---- precomputed tables (filled by init()) ------------------------------
  float window[N];        // periodic Hann (analysis == synthesis window)
  float wr[HALF];         // twiddle cos(2*pi*t/N)
  float wi[HALF];         // twiddle sin(2*pi*t/N)
  uint16_t brev[N];       // bit-reversal permutation
  float nomOmega[NB];     // nominal per-hop phase advance per bin (radians)
  float normFreq[NB];     // bin centre 0..1 (for the tilt shaper)
  float invNorm = 1.0f;   // 1 / overlap-add COLA constant

  // ---- STFT state ---------------------------------------------------------
  float gInFIFO[N];       // sliding input frame (linear, shifted by H each hop)
  float gOutAccum[N];     // overlap-add accumulator (shifted by H each hop)
  float gOutHop[H];       // finalised output samples for the current hop
  float re[N], im[N];     // FFT workspace (also carries the live spectrum)
  float frozenMag[NB];    // held magnitude spectrum
  float capturedMag[NB];  // reference for DRIFT mean-reversion
  float phaseAcc[NB];     // running synthesis phase per bin
  float freezeMix = 0.0f; // 0 live .. 1 frozen (smoothed per hop, anti-click)
  int rover = 0;          // sample cursor within the current hop (0..H-1)
  bool prevFreeze = false;
  int captureXfade = 0;   // hops left in a spectral-domain capture crossfade
  uint32_t rng = 0x1234abcdUL;
  float shimmerLfo = 0.0f;// 0..1 LED breathe phase (advances with shimmer)
  float ledEnv = 0.0f;    // smoothed input envelope for the unfrozen LED
  bool pendingCapture = false;

  // -------------------------------------------------------------------------
  void init() {
    for (int i = 0; i < N; i++)
      window[i] = 0.5f - 0.5f * cosf(kTwoPi * (float)i / (float)N);
    for (int t = 0; t < HALF; t++) {
      wr[t] = cosf(kTwoPi * (float)t / (float)N);
      wi[t] = sinf(kTwoPi * (float)t / (float)N);
    }
    // Bit-reversal table for radix-2 (log2 N = 10 for N = 1024).
    int bits = 0;
    for (int t = N; t > 1; t >>= 1) bits++;
    for (int i = 0; i < N; i++) {
      uint32_t x = (uint32_t)i, r = 0;
      for (int b = 0; b < bits; b++) { r = (r << 1) | (x & 1u); x >>= 1; }
      brev[i] = (uint16_t)r;
    }
    for (int k = 0; k < NB; k++) {
      // Centre-bin phase advance over one hop, wrapped to (-pi, pi].
      float w = fmodf(kTwoPi * (float)k * (float)H / (float)N, kTwoPi);
      if (w > kPi) w -= kTwoPi;
      nomOmega[k] = w;
      normFreq[k] = (float)k / (float)HALF;
    }
    // COLA constant: sum of window^2 over the hop grid (1.5 for Hann@75%).
    float c = 0.0f;
    for (int j = 0; j < N / H; j++) {
      float w = window[(j * H) % N];
      c += w * w;
    }
    invNorm = (c > 1e-6f) ? (1.0f / c) : 1.0f;
    reset();
  }

  void reset() {
    for (int i = 0; i < N; i++) { gInFIFO[i] = 0.0f; gOutAccum[i] = 0.0f; }
    for (int i = 0; i < H; i++) gOutHop[i] = 0.0f;
    for (int k = 0; k < NB; k++) {
      frozenMag[k] = 0.0f; capturedMag[k] = 0.0f; phaseAcc[k] = 0.0f;
    }
    freezeMix = 0.0f;
    rover = 0;
    prevFreeze = false;
    captureXfade = 0;
    pendingCapture = false;
    shimmerLfo = 0.0f;
    ledEnv = 0.0f;
    rng = 0x1234abcdUL;
  }

  // Request a fresh spectrum grab (IN1, "capture"): crossfades the held
  // spectrum toward the current live spectrum in the frequency domain.
  void triggerCapture() { pendingCapture = true; }

  // 0..1 panel LED brightness: solid & shimmer-breathing when frozen, a dim
  // input-follower otherwise (README: "solid when frozen; shimmer modulates").
  float ledLevel() const {
    if (freeze)
      return 0.6f + 0.4f * (0.5f - 0.5f * cosf(kTwoPi * shimmerLfo));
    return 0.05f + 0.9f * ledEnv;
  }

  // -------------------------------------------------------------------------
  // Complex radix-2 FFT (in place). inverse=false: forward, e^{-i...};
  // inverse=true: e^{+i...} and 1/N scaling.
  void cfft(float* xr, float* xi, bool inverse) {
    for (int i = 0; i < N; i++) {
      int j = brev[i];
      if (j > i) {
        float t = xr[i]; xr[i] = xr[j]; xr[j] = t;
        t = xi[i]; xi[i] = xi[j]; xi[j] = t;
      }
    }
    for (int len = 2; len <= N; len <<= 1) {
      int half = len >> 1;
      int tstep = N / len;
      for (int i = 0; i < N; i += len) {
        for (int j = 0; j < half; j++) {
          int t = j * tstep;
          float c = wr[t];
          float s = inverse ? wi[t] : -wi[t];  // twiddle = c + i s
          int a = i + j, b = a + half;
          float vr = xr[b] * c - xi[b] * s;
          float vi = xr[b] * s + xi[b] * c;
          xr[b] = xr[a] - vr; xi[b] = xi[a] - vi;
          xr[a] += vr;        xi[a] += vi;
        }
      }
    }
    if (inverse) {
      const float g = 1.0f / (float)N;
      for (int i = 0; i < N; i++) { xr[i] *= g; xi[i] *= g; }
    }
  }

  // -------------------------------------------------------------------------
  // Heavy per-hop frame work: analyse the current input frame, build the
  // synthesis spectrum (freeze / tilt / shimmer), inverse-FFT and overlap-add.
  void computeFrame() {
    // --- analysis: window the sliding frame, forward FFT -------------------
    for (int i = 0; i < N; i++) { re[i] = gInFIFO[i] * window[i]; im[i] = 0.0f; }
    cfft(re, im, false);

    // --- freeze engage edge: snapshot the reference for DRIFT --------------
    if (freeze && !prevFreeze) {
      for (int k = 0; k < NB; k++) capturedMag[k] = frozenMag[k];
    }
    const bool doCapture = pendingCapture;
    pendingCapture = false;
    if (doCapture) captureXfade = 4;  // 4-hop spectral-domain crossfade

    // --- freeze crossfade amount (per-hop one-pole toward the gate) --------
    // dt-free: the ramp is measured in hops; H/AUDIO_FS ~ 7 ms per hop, so a
    // ~30 ms attack is a few hops. OLA smooths it into a click-free fade.
    static const float kHopSec = 0.007f;  // nominal hop length for the ramp
    float coef = kHopSec / (attackSec > 1e-4f ? attackSec : 1e-4f);
    if (coef > 1.0f) coef = 1.0f;

    const float xfadeStep = (captureXfade > 0) ? (1.0f / (float)captureXfade) : 0.0f;

    for (int k = 0; k < NB; k++) {
      const float liveMag = sqrtf(re[k] * re[k] + im[k] * im[k]);
      const float livePhase = atan2f(im[k], re[k]);

      if (!freeze) {
        // Track the live spectrum so a freeze always holds the latest sound.
        if (mode == SPFREEZE_AVG)
          frozenMag[k] += (liveMag - frozenMag[k]) * 0.25f;  // ~4-frame smooth
        else
          frozenMag[k] = liveMag;
        capturedMag[k] = frozenMag[k];
        phaseAcc[k] = livePhase;  // keep phase locked to live (identity resynth)
      } else {
        // Optional IN1 capture: crossfade the held magnitude toward live.
        if (doCapture) phaseAcc[k] = livePhase;
        if (captureXfade > 0)
          frozenMag[k] += (liveMag - frozenMag[k]) * xfadeStep;
        // DRIFT: gentle bounded random walk around the captured magnitude.
        if (mode == SPFREEZE_DRIFT) {
          const float noise = noise1f(rng);
          frozenMag[k] += (capturedMag[k] - frozenMag[k]) * 0.002f +
                          noise * capturedMag[k] * 0.02f;
          if (frozenMag[k] < 0.0f) frozenMag[k] = 0.0f;
          const float hi = capturedMag[k] * 1.6f;
          if (frozenMag[k] > hi) frozenMag[k] = hi;
        }
        // Advance the synthesis phase: nominal centre rate + shimmer jitter.
        float adv = nomOmega[k];
        if (shimmer > 0.0f && k != 0 && k != HALF)
          adv += shimmer * noise1f(rng);
        phaseAcc[k] += adv;
        if (phaseAcc[k] > kPi) phaseAcc[k] -= kTwoPi;
        else if (phaseAcc[k] < -kPi) phaseAcc[k] += kTwoPi;
      }
    }
    if (captureXfade > 0) captureXfade--;

    // Compute the freeze mix per hop (Cartesian blend of live vs frozen).
    // We ramp a member so successive hops step smoothly.
    freezeMix += ((freeze ? 1.0f : 0.0f) - freezeMix) * coef;

    // --- build the synthesis spectrum (DC..Nyquist), tilt-shaped -----------
    for (int k = 0; k < NB; k++) {
      const float g = expf(tiltSlope * (normFreq[k] - 0.5f));  // spectral tilt
      const float fr = frozenMag[k] * cosf(phaseAcc[k]);
      const float fi = frozenMag[k] * sinf(phaseAcc[k]);
      float sr = (re[k] + (fr - re[k]) * freezeMix) * g;
      float si = (im[k] + (fi - im[k]) * freezeMix) * g;
      if (k == 0 || k == HALF) si = 0.0f;  // keep DC/Nyquist real
      re[k] = sr; im[k] = si;
    }
    // Hermitian mirror so the inverse transform is purely real.
    for (int k = 1; k < HALF; k++) {
      re[N - k] = re[k];
      im[N - k] = -im[k];
    }

    // --- inverse FFT + windowed overlap-add --------------------------------
    cfft(re, im, true);
    for (int i = 0; i < N; i++)
      gOutAccum[i] += re[i] * window[i] * invNorm;

    // Finalise this hop's H output samples (they now carry all overlapping
    // frames), then slide the accumulator and input frame down by one hop.
    for (int i = 0; i < H; i++) gOutHop[i] = gOutAccum[i];
    for (int i = 0; i < N - H; i++) {
      gInFIFO[i] = gInFIFO[i + H];
      gOutAccum[i] = gOutAccum[i + H];
    }
    for (int i = N - H; i < N; i++) { gInFIFO[i] = 0.0f; gOutAccum[i] = 0.0f; }
  }

  // -------------------------------------------------------------------------
  // Per-sample entry point (VCV Rack + host tests). Feeds one input sample,
  // runs the heavy frame at the hop boundary, and returns one output sample.
  // `dt` is accepted for API symmetry / future rate-aware tweaks.
  float process(float in, float dt) {
    (void)dt;
    // LED envelope + shimmer LFO (cheap per-sample bookkeeping).
    const float a = fabsf(in);
    ledEnv += (a - ledEnv) * 0.001f;
    shimmerLfo += (0.15f + shimmer) * 0.0007f;
    if (shimmerLfo >= 1.0f) shimmerLfo -= 1.0f;

    gInFIFO[rover + (N - H)] = in;   // write into the newest hop region
    float outSpec = gOutHop[rover];  // read the finalised output sample
    rover++;
    if (rover >= H) {
      computeFrame();
      prevFreeze = freeze;
      rover = 0;
    }

    // Denormal flush + wet/dry against the dry input.
    if (!(outSpec > 1e-25f) && !(outSpec < -1e-25f)) outSpec = 0.0f;
    float out = in + (outSpec - in) * clampf(wet, 0.0f, 1.0f);
    return clampf(out, -1.5f, 1.5f);
  }
};

}  // namespace sc
