#pragma once

// Booker voice — a Hammond tonewheel organ with a Leslie, as nine additive
// drawbars over one fundamental.
//
// Used by:
//   - firmwares/mod2-booker/mod2-booker.ino  (one sample per PWM-wrap ISR)
//   - rack-plugins/src/mod2-booker.cpp       (one sample per process())
//
// Ported from Sean Luke's GRAINS `booker` firmware. Upstream is a Mozzi sketch:
// nine int8 sine oscillators at 16384 Hz, summed through a table of drawbar
// registrations, with a control-rate LFO standing in for the Leslie. Everything
// musical about it survives here — the ratios, the 16 registrations, the drawbar
// taper, the level relationships between registrations — while the arithmetic
// becomes float at whatever rate the caller runs:
//
//   TONEWHEELS  Nine sines at fixed integer multiples of the fundamental,
//               upstream's `drawbarFrequencies` verbatim: 1, 2, 3, 4, 6, 8, 10,
//               12, 16. Relative to the 8' drawbar (ratio 2) those are the real
//               Hammond footages 16', 8', 5⅓', 4', 2⅔', 2', 1⅗', 1⅓', 1'. Note
//               that puts 8' second and 5⅓' third, where a Hammond console has
//               them the other way round; it is upstream's ordering and the
//               registration table below is voiced for it, so both are kept as
//               they are.
//   DRAWBARS    Stop positions 0..8 map through upstream's `drawBarAmplitudes`
//               taper (0, 7, 18, 32, 48, 65, 85, 105, 127 — roughly 3 dB a
//               stop), normalised to 0..1 here.
//   LEVEL       Upstream never normalises the sum, so a registration gets louder
//               and dirtier the more drawbars are out: one drawbar peaks at a
//               quarter of full scale, Full Organ runs 2.3x past the clip point.
//               kBookerOutputGain reproduces that exact relationship (see its
//               comment for the arithmetic), because it is how the instrument
//               behaves and how the 16 registrations are balanced against each
//               other.
//   LESLIE      Upstream's fixed 5.66 Hz rotor modulating pitch and amplitude,
//               here with upstream's own "classic slower speed" of 0.66 Hz as a
//               second setting and an inertial ramp between the two.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h. No Arduino / Rack / Pico SDK;
// float-only, no heap, no STL.
//
// License:
// Derived from the GRAINS `booker` firmware (github.com/eclab/grains) under
// the Apache License 2.0, Copyright 2023 Sean Luke. The upstream notice lives
// at firmwares/mod2-booker/LICENSE.md and Apache 2.0 requires it to ship with
// any copy of this header — the CC0 cores next to it have no such condition, so
// don't fold this one into them.

#include "sc_math.h"
#include "sc_dsp.h"

namespace sc {

constexpr int kBookerDrawbars = 9;
constexpr int kBookerRegistrations = 16;

// Upstream plays the C three octaves below middle C at 0 V — the first entry of
// its 1536-point pitch table, and the frequency of the 16' drawbar (ratio 1).
constexpr float kBookerBaseHz = 32.7f;

// Leslie rates, both upstream's own numbers: LESLIE_FREQUENCY is 5.66 Hz ("the
// 450 speed"), and its header names 0.66 Hz as "the classic slower speed",
// rejected there only because a single fixed rate cannot be that slow. As the
// chorale half of a two-speed rotor it is exactly right.
constexpr float kBookerLeslieSlowHz = 0.66f;
constexpr float kBookerLeslieFastHz = 5.66f;

// A horn on a belt drive does not change speed instantly; these are the time
// constants of the glide between the two rates. Spin-down is the slower of the
// two, as it is on the real cabinet (nothing is driving it but friction).
constexpr float kBookerLeslieSpinUpSec = 1.0f;
constexpr float kBookerLeslieSpinDownSec = 1.6f;

// Amplitude depth, from upstream's LESLIE_VOLUME scale: setting v adds
// (255 >> (9 - v)) to a gain whose full scale is 256, so its deepest setting (6)
// is 31/256 of the level. Upstream ships v = 1, which shifts by 8 and therefore
// adds exactly nothing — its default Leslie is pitch-only. We take its deepest.
constexpr float kBookerLeslieAmDepth = 31.0f / 256.0f;

// Pitch depth as a fraction of the fundamental. Upstream adds a constant
// +/-(127/128) Hz to the fundamental itself, which is an *absolute* offset: it
// is +/-52 cents at the bottom C, +/-13 cents two octaves up and inaudible at the
// top of the range. A rotating horn's Doppler shift is a constant ratio, so
// this port makes it one, at +/-1 % (~17 cents) — inside the span upstream
// sweeps through, and now the same in every register.
constexpr float kBookerLesliePitchDepth = 0.010f;

// Level. Upstream sums nine int8 oscillators against int8 drawbar amplitudes and
// returns ((sum >> 8) * gain) >> 8 with gain = 1 + (pot >> 2), i.e. 1..256, into
// a Mozzi output that clips at +/-244. Normalising the oscillators and drawbars
// to 0..1, that whole chain is out = 16129/65536 * gain / 244 * sum, so at full
// volume (gain 256) each unit of summed drawbar amplitude is 0.2582 of full
// scale. Keeping the constant keeps upstream's balance: the single-drawbar
// "Sine" registration peaks at a quarter of full scale, Full Organ (sum 9) runs
// 2.3x past the ceiling and overdrives, exactly as the original does.
constexpr float kBookerOutputGain = 0.2582f;

// Key action. Upstream has no gate at all — it drones — so a note has no
// envelope to copy. A Hammond's key contacts make and break almost instantly,
// so these are only long enough to keep the PWM stage from clicking.
constexpr float kBookerAttackSec = 0.003f;
constexpr float kBookerReleaseSec = 0.010f;

// Leslie settings, in the order the hardware button cycles them.
enum BookerLeslieMode {
  BOOKER_LESLIE_OFF = 0,   // upstream with //#define LESLIE_ON
  BOOKER_LESLIE_SLOW = 1,  // chorale
  BOOKER_LESLIE_FAST = 2   // tremolo — upstream's shipped setting
};

// Harmonic ratio of drawbar `i`, upstream's drawbarFrequencies table.
inline float bookerRatio(int i) {
  static const uint8_t kRatios[kBookerDrawbars] = {1, 2, 3, 4, 6, 8, 10, 12, 16};
  return (float)kRatios[i];
}

// Amplitude of drawbar stop position 0..8, upstream's drawBarAmplitudes taper
// normalised so a fully-out drawbar is 1.0.
inline float bookerStopAmplitude(uint8_t stop) {
  static const uint8_t kStopAmp[9] = {0, 7, 18, 32, 48, 65, 85, 105, 127};
  return (float)kStopAmp[stop] * (1.0f / 127.0f);
}

// Stop position of drawbar `drawbar` in registration `reg` — upstream's
// `drawbars` table verbatim, including its choice of which 16 of the 99
// registrations in its comment block to ship.
inline uint8_t bookerStop(int reg, int drawbar) {
  static const uint8_t kReg[kBookerRegistrations][kBookerDrawbars] = {
      {8, 8, 8, 8, 8, 8, 8, 8, 8},  // 888888888 Full Organ
      {8, 8, 5, 3, 2, 4, 5, 8, 8},  // 885324588 Blues
      {8, 8, 8, 8, 0, 0, 0, 0, 0},  // 888800000 Booker T. Jones 1
      {8, 8, 8, 6, 3, 0, 0, 0, 0},  // 888630000 Booker T. Jones 2
      {8, 7, 8, 0, 0, 0, 4, 5, 6},  // 878000456 Bright Comping
      {8, 4, 3, 0, 0, 0, 0, 0, 0},  // 843000000 Dark Comping
      {8, 0, 8, 8, 0, 8, 0, 0, 8},  // 808808008 Gospel 1
      {8, 8, 8, 0, 0, 0, 0, 0, 8},  // 888000008 Gospel 2
      {8, 6, 8, 6, 6, 6, 5, 6, 8},  // 868666568 Greg Allman 1
      {8, 8, 8, 6, 0, 0, 0, 0, 0},  // 888600000 Greg Allman 2
      {8, 8, 6, 8, 0, 0, 3, 0, 0},  // 886800300 Paul Shaffer 1
      {8, 8, 8, 7, 6, 8, 8, 8, 8},  // 888768888 Paul Shaffer 2
      {8, 8, 8, 8, 7, 8, 6, 7, 8},  // 888878678 Paul Shaffer 3
      {8, 0, 8, 0, 0, 0, 0, 0, 8},  // 808000008 Reggae
      {0, 8, 0, 0, 0, 0, 0, 0, 0},  // 080000000 Sine
      {8, 7, 6, 5, 4, 3, 2, 1, 1},  // 876543211 Strings
  };
  return kReg[reg][drawbar];
}

// Registration index from a normalised 0..1 control. Upstream computes
// `(organ + adc * 16) >> 10`, which is floor(adc * 16 / 1024) with the previous
// index folded in as a one-count hysteresis; the hysteresis is worth less than
// the ADC noise it was meant to absorb, so this is the plain division.
inline int bookerRegistrationSelect(float v01) {
  const int idx = (int)(clampf(v01, 0.0f, 1.0f) * (float)kBookerRegistrations);
  return idx >= kBookerRegistrations ? kBookerRegistrations - 1 : idx;
}

// Fundamental (16' drawbar) for a 1 V/octave input, upstream's 0 V note at 0 V.
// Upstream reaches this through a 1536-entry PROGMEM table of its ADC steps;
// the table is equal-tempered at 1/17 of a semitone per step, so the closed form
// reproduces it and returns ~6 KB of flash (the same trade PORTING.md records
// for SquareVCO's voctMap).
inline float bookerFreqFromVolts(float volts) {
  return kBookerBaseHz * powf(2.0f, volts);
}

// One sine cycle, phase in turns (0..1). Nine of these run per sample, so this
// is an odd minimax polynomial over [-pi, pi] rather than a libm call: ~2e-5
// peak error (-94 dB, below the 10-bit PWM floor), no table, and bit-identical
// on AVR, RP2350 and the desktop so the firmware and the Rack port agree.
inline float bookerSine(float turns) {
  const float z = (turns - 0.5f) * kTwoPi;  // [-pi, pi)
  const float z2 = z * z;
  const float s =
      z * (1.0f + z2 * (-1.66666546e-1f +
                        z2 * (8.33216076e-3f +
                              z2 * (-1.98047539e-4f + z2 * 2.60190306e-6f))));
  return -s;  // sin(2*pi*t - pi) == -sin(2*pi*t)
}

struct BookerFrame {
  float audio;  // -1..+1
  float env;    // 0..1 — key level, pulsing at the rotor rate (LED)
};

struct BookerVoice {
  // ── controls: set these at control rate, then call process() per sample ──
  float freq = kBookerBaseHz;  // Hz of the 16' drawbar (the fundamental)
  float volume = 1.0f;         // 0..1, upstream's gain pot
  int leslieMode = BOOKER_LESLIE_FAST;  // upstream ships the Leslie on
  bool gate = true;                     // false releases the note; upstream drones

  // ── state ──
  float amp[kBookerDrawbars];    // drawbar amplitudes for the current registration
  float phase[kBookerDrawbars];  // tonewheel phases, in turns
  float lesliePhase = 0.0f;
  float leslieRate = 0.0f;  // Hz, glides towards the selected rate
  float keyLevel = 0.0f;
  int registration = -1;  // set through setRegistration(), which recomputes amp[]

  BookerVoice() { reset(); }

  void reset() {
    for (int i = 0; i < kBookerDrawbars; ++i) {
      phase[i] = 0.0f;
      amp[i] = 0.0f;
    }
    lesliePhase = 0.0f;
    leslieRate = 0.0f;
    keyLevel = 0.0f;
    registration = -1;
    setRegistration(0);
  }

  // Select one of the 16 drawbar registrations and expand it into amp[].
  void setRegistration(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= kBookerRegistrations) idx = kBookerRegistrations - 1;
    if (idx == registration) return;
    registration = idx;
    for (int i = 0; i < kBookerDrawbars; ++i)
      amp[i] = bookerStopAmplitude(bookerStop(idx, i));
  }

  BookerFrame process(float dt) {
    // ── Leslie rotor ──────────────────────────────────────────────────────
    // The rate itself is what ramps, so the modulation stays phase-continuous
    // across a speed change instead of jumping.
    const float targetRate = (leslieMode == BOOKER_LESLIE_SLOW)   ? kBookerLeslieSlowHz
                             : (leslieMode == BOOKER_LESLIE_FAST) ? kBookerLeslieFastHz
                                                                  : 0.0f;
    const float tau = (targetRate > leslieRate) ? kBookerLeslieSpinUpSec
                                                : kBookerLeslieSpinDownSec;
    leslieRate += (targetRate - leslieRate) * clampf(dt / tau, 0.0f, 1.0f);

    lesliePhase += leslieRate * dt;
    if (lesliePhase >= 1.0f) lesliePhase -= (float)(int)lesliePhase;
    const float lfo = bookerSine(lesliePhase);

    // Depth follows rotor speed, which is both physically right (Doppler shift
    // is proportional to how fast the horn is moving) and what lets the whole
    // effect fade away cleanly as it coasts to a stop when switched off.
    const float spin = leslieRate * (1.0f / kBookerLeslieFastHz);  // 0..1
    const float pitchMod = 1.0f + kBookerLesliePitchDepth * spin * lfo;
    const float amMod = 1.0f - kBookerLeslieAmDepth * spin * 0.5f * (1.0f - lfo);

    // ── key envelope ──────────────────────────────────────────────────────
    if (gate) {
      keyLevel += dt / kBookerAttackSec;
      if (keyLevel > 1.0f) keyLevel = 1.0f;
    } else {
      keyLevel -= dt / kBookerReleaseSec;
      if (keyLevel < 0.0f) keyLevel = 0.0f;
    }

    // ── nine tonewheels ───────────────────────────────────────────────────
    // Upstream's header admits its top drawbars pass Nyquist and alias, and that
    // fixing it would cost more than an ATmega has. It costs one compare here,
    // so partials fade out over the top fifth of the band instead of folding
    // back. A real tonewheel generator runs out of wheels near 6 kHz too.
    const float nyquist = 0.5f / dt;
    const float fadeStart = 0.80f * nyquist;
    const float fadeSpan = 0.15f * nyquist;

    const float f0 = freq * pitchMod;
    float sum = 0.0f;
    for (int i = 0; i < kBookerDrawbars; ++i) {
      const float fi = f0 * bookerRatio(i);
      // Advance every phase, even for a silent drawbar, so pulling one out
      // mid-note phases in where it would have been rather than from zero.
      phase[i] += fi * dt;
      if (phase[i] >= 1.0f) phase[i] -= (float)(int)phase[i];

      float g = amp[i];
      if (g <= 0.0f) continue;
      if (fi > fadeStart) {
        g *= clampf((fadeStart + fadeSpan - fi) / fadeSpan, 0.0f, 1.0f);
        if (g <= 0.0f) continue;
      }
      sum += g * bookerSine(phase[i]);
    }

    // Upstream's Mozzi output hard-clips; softSat rounds the same ceiling off,
    // so a driven Full Organ grinds rather than buzzing at the sample grid.
    BookerFrame f;
    f.audio = softSat(kBookerOutputGain * volume * amMod * keyLevel * sum);
    // LED: lit while a note sounds, dipping once per rotor turn so the panel
    // shows the Leslie's speed (and shows it settling after a speed change).
    f.env = keyLevel * (1.0f - 0.85f * spin * 0.5f * (1.0f - lfo));
    return f;
  }
};

}  // namespace sc
