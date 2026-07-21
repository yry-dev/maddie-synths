#pragma once

// Freq Shifter — Bode-style single-sideband frequency shifter core.
//
// Used by:
//   - firmwares/mod2-freq-shifter/mod2-freq-shifter.ino
//   - rack-plugins/src/mod2-freq-shifter.cpp
//
// Very Buchla: shifts every partial by a fixed Hz amount (not a ratio), so a
// harmonic source turns inharmonic — a barberpole shimmer at small shifts,
// clangor at large ones. This is a Bode/Hilbert single-sideband modulator, not
// ring modulation: ring mod produces BOTH sum and difference sidebands, an SSB
// shifter keeps only one.
//
// Signal path:
//   1. A wide-band Hilbert transform splits the input into two quadrature
//      copies I and Q (90 deg apart). It is built from a two-path IIR
//      phase-difference network: two cascades of second-order allpasses (the
//      z^-2 "polyphase halfband" form  A(z) = (a - z^-2)/(1 - a z^-2)), plus a
//      one-sample delay on the second path. The 8 fixed coefficients below are
//      Niemitalo's 8th-order halfband set; used as a phase splitter the two
//      branches stay ~90 deg apart from ~0.003*fs up to nearly Nyquist. Because
//      the coefficients are fixed (not recomputed per rate), the network is
//      cheap and behaves the same relative to the sample rate on every target.
//   2. A quadrature carrier e^{j*theta} is advanced by phasor rotation (a
//      complex multiply per sample + a cheap renormalisation) instead of a
//      sinf() per sample — one cos/sin pair is taken only when the shift
//      frequency changes. This also gives sub-Hz precision for slow barberpole
//      shifts without a wide phase accumulator.
//   3. SSB combine:  USB = I*cos - Q*sin  (shift up by |f|)
//                    LSB = I*cos + Q*sin  (shift down by |f|)
//      The `sideband` control crossfades USB<->LSB; at 0.5 the Q term cancels
//      and only I*cos survives, i.e. plain amplitude/ring-mod (both sidebands).
//   4. A feedback path (shifted output soft-limited + DC-blocked back into the
//      input) spirals the barberpole — the magic of the effect.
//
// A gentle ~30 Hz input high-pass tames the allpass network's image leakage
// below ~50 Hz (the README's open question — the HP is the chosen answer, and
// it is applied in the shared core so firmware and Rack behave identically).
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; float only, no heap, no STL — compiles on
// AVR, RP2350 and the desktop.

#include <math.h>
#include <stdint.h>

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// Shift range (BUTTON short-press cycles these on hardware).
enum FreqShifterRange : uint8_t {
  FREQSHIFT_20 = 0,    // +/-20 Hz  — slow barberpole phasing
  FREQSHIFT_200 = 1,   // +/-200 Hz — inharmonic shimmer
  FREQSHIFT_1000 = 2,  // +/-1 kHz  — full clangor
  FREQSHIFT_RANGE_COUNT = 3
};

// Range select -> maximum |shift| in Hz.
inline float freqShifterRangeHz(uint8_t range) {
  const float r[3] = {20.0f, 200.0f, 1000.0f};
  return r[range < FREQSHIFT_RANGE_COUNT ? range : FREQSHIFT_1000];
}

// POT1 -> signed shift in Hz. Bipolar: pot=0.5 is 0 Hz (centre detent), with a
// signed-square taper so there is fine control near zero (slow barberpole) and
// coarse control toward the extremes. `rangeHz` from freqShifterRangeHz().
inline float freqShifterShiftHz(float pot01, float rangeHz) {
  float v = clampf(pot01, 0.0f, 1.0f) * 2.0f - 1.0f;  // -1 .. +1
  const float detent = 0.02f;
  if (v > -detent && v < detent) return 0.0f;         // centre detent = 0 Hz
  // Rescale past the detent so the range stays continuous, then square-taper.
  float s = (v > 0.0f ? v - detent : v + detent) / (1.0f - detent);
  return s * fabsf(s) * rangeHz;
}

// POT2 -> feedback amount (0 .. 0.98; kept below 1 so the spiral stays stable).
inline float freqShifterFeedback(float pot01) {
  return clampf(pot01, 0.0f, 1.0f) * 0.98f;
}

// --------------------------------------------------
// Second-order z^-2 allpass: A(z) = (a - z^-2)/(1 - a z^-2), the polyphase
// halfband form. |a| < 1 -> stable, magnitude flat.
// --------------------------------------------------
struct AllpassZ2 {
  float a = 0.0f;
  float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;

  void reset() { x1 = x2 = y1 = y2 = 0.0f; }

  inline float process(float x) {
    // y[n] = a*(x[n] + y[n-2]) - x[n-2]
    const float y = a * (x + y2) - x2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
  }
};

// Niemitalo 8th-order halfband coefficients, split into the two allpass paths.
// Path 0 -> in-phase (I); path 1 (+ a one-sample delay) -> quadrature (Q).
// These are the a values for the (a - z^-2)/(1 - a z^-2) sections above.
constexpr float kHilbertA0[4] = {0.6923877778065425f, 0.9360654322959897f,
                                 0.9882295226342303f, 0.9987488452737916f};
constexpr float kHilbertA1[4] = {0.4021921162426558f, 0.8561710882420950f,
                                 0.9722909545651841f, 0.9952884791278916f};

struct FreqShifterCore {
  // Parameters (write directly; see the mappers above).
  float shiftHz = 0.0f;    // signed carrier frequency (Hz); + = up, - = down
  float feedback = 0.0f;   // 0 .. ~0.98
  float sideband = 0.0f;   // 0 up (USB) .. 0.5 both (ring-mod) .. 1 down (LSB)
  float wet = 1.0f;        // 0 dry .. 1 fully wet
  bool flip = false;       // IN1 gate: negate the shift direction
  uint8_t range = FREQSHIFT_1000;  // FreqShifterRange (informational; POT maps it)

  // State — Hilbert network.
  AllpassZ2 apI[4];
  AllpassZ2 apQ[4];
  float qDelay = 0.0f;     // one-sample delay on the Q path

  // State — quadrature carrier (phasor rotation).
  float carCos = 1.0f, carSin = 0.0f;  // current carrier e^{j theta}
  float rotCos = 1.0f, rotSin = 0.0f;  // per-sample rotation increment
  float rotHzUsed = 1e30f;             // shift the increment was computed for

  // State — smoothing + feedback.
  float shiftSm = 0.0f;    // smoothed shift (Hz)
  float sideSm = 0.0f;     // smoothed sideband blend
  float fbState = 0.0f;    // last shifted sample fed back
  float inHpLp = 0.0f;     // input high-pass (one-pole LP subtracted)
  DcBlocker fbDc;          // DC block in the feedback loop

  void reset() {
    for (int i = 0; i < 4; i++) {
      apI[i].reset();
      apQ[i].reset();
    }
    qDelay = 0.0f;
    carCos = 1.0f;
    carSin = 0.0f;
    rotCos = 1.0f;
    rotSin = 0.0f;
    rotHzUsed = 1e30f;
    shiftSm = 0.0f;
    sideSm = 0.0f;
    fbState = 0.0f;
    inHpLp = 0.0f;
    fbDc = DcBlocker();
    for (int i = 0; i < 4; i++) {
      apI[i].a = kHilbertA0[i];
      apQ[i].a = kHilbertA1[i];
    }
  }

  // 0..1 brightness for the panel LED — rotates at the carrier (shift) rate;
  // visibly spins for slow barberpole shifts, solid-looking above.
  float ledLevel() const { return 0.5f + 0.5f * carSin; }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    // --- smooth the user params (no zipper, especially on shift) ---------
    const float pc = onePoleCoef(30.0f, dt);
    shiftSm += (shiftHz - shiftSm) * pc;
    sideSm += (sideband - sideSm) * pc;

    // --- gentle ~30 Hz input high-pass (hides sub-50 Hz image leakage) ---
    inHpLp += (in - inHpLp) * onePoleCoef(30.0f, dt);
    const float hp = in - inHpLp;

    // --- feedback into the Hilbert input --------------------------------
    const float x = hp + feedback * fbState;

    // --- Hilbert transform: two allpass cascades, Q delayed one sample --
    float iSig = x;
    for (int k = 0; k < 4; k++) iSig = apI[k].process(iSig);
    float q = x;
    for (int k = 0; k < 4; k++) q = apQ[k].process(q);
    const float qSig = qDelay;  // z^-1 on the Q path
    qDelay = q;

    // --- carrier phasor: recompute the rotation increment only when the
    //     (smoothed) shift changed, then rotate + renormalise ------------
    const float f = flip ? -shiftSm : shiftSm;
    if (fabsf(f - rotHzUsed) > 1e-3f) {
      const float ang = kTwoPi * f * dt;
      rotCos = cosf(ang);
      rotSin = sinf(ang);
      rotHzUsed = f;
    }
    const float nc = carCos * rotCos - carSin * rotSin;
    const float ns = carSin * rotCos + carCos * rotSin;
    // Cheap magnitude renormalisation (Newton step for 1/sqrt near 1).
    const float corr = 1.5f - 0.5f * (nc * nc + ns * ns);
    carCos = nc * corr;
    carSin = ns * corr;

    // --- single-sideband combine ----------------------------------------
    const float usb = iSig * carCos + qSig * carSin;  // shift up by |f|
    const float lsb = iSig * carCos - qSig * carSin;  // shift down by |f|
    // Soft-saturate the shifted signal: the SSB combine (and any feedback
    // buildup) can transiently exceed +/-1, and softSat keeps the barberpole
    // spiral bounded and musical (nearly linear at normal levels).
    const float shifted = softSat(usb + (lsb - usb) * sideSm);

    // --- feedback state: DC-block the (already limited) output -----------
    fbState = fbDc.process(shifted);

    return in + (shifted - in) * wet;
  }
};

}  // namespace sc
