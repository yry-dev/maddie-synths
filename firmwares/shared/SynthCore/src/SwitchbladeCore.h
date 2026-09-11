#pragma once

// Switchblade engine — a CV conditioner: sum two inputs, then either lag-smooth
// or fuzz the result, then run it through an attenuverter.
//
// Used by:
//   - firmwares/mod1-switchblade/mod1-switchblade.ino  (one process() per loop)
//   - rack-plugins/src/mod1-switchblade.cpp            (at audio sample rate)
//
// Pure C++: depends only on sc_math.h. No Arduino / Rack / Pico SDK.
//
// This is a CONTROL-RATE engine, not a per-sample one. Upstream does its whole
// signal chain inside Mozzi's updateControl() at CONTROL_RATE = 256 Hz and lets
// updateAudio() emit the held value, so every time constant in here — the
// one-pole lag, the 3-tap median window, the rate the fuzz noise steps at — is
// defined against 256 Hz. Rather than re-derive them per sample (which would
// make the sound host-sample-rate dependent, and would cost a powf per sample on
// an ATmega328P), process() accumulates dt and ticks the chain at exactly
// kSwitchbladeControlHz, holding its output in between. Both platforms then get
// upstream's numbers verbatim at any dt. The cost is that the output is a 256 Hz
// staircase — which is what the original module puts out too.
//
// License:
// Derived from the GRAINS `switchblade` firmware (github.com/eclab/grains) under
// the Apache License 2.0, Copyright 2023 Sean Luke. The upstream notice lives at
// firmwares/mod1-switchblade/LICENSE.md and Apache 2.0 requires it to ship with
// any copy of this header — the CC0 cores next to it have no such condition, so
// don't fold this one into them.

// sc_dsp.h would give us xorshift32/noise1f, but it cannot be included from an
// AVR sketch: its biquad coefficients are named B0..B2, and Arduino's binary.h
// has already made those object-like macros. The fuzz PRNG is four lines, so it
// lives on the voice instead — also keeping it out of sc:: where RandomLagCore.h
// already declares an xorshift32 of its own.
#include "sc_math.h"

namespace sc {

// Upstream's Mozzi CONTROL_RATE. Every time constant below is relative to it.
constexpr float kSwitchbladeControlHz = 256.0f;

// Median of three — upstream's MEDIAN_OF_THREE macro, spelled as a function so
// the arguments are evaluated once each. Used to de-glitch the summed CV read.
inline float switchbladeMedian3(float a, float b, float c) {
  if (a <= b) return (b <= c) ? b : ((a < c) ? c : a);
  return (a <= c) ? a : ((b < c) ? c : b);
}

// Parameters mapped from the three normalised 0..1 pots.
struct SwitchbladeParams {
  float atten;      // 0..1    input-A attenuation (or the manual level in MAN mode)
  float gain;       // -1..+1  attenuverter: -1 fully inverted, 0 silent, +1 unity
  float lagWeight;  // 0..127/128  one-pole retention per control tick (0 = off)
  float fuzz;       // 0..511/1023 noise width (0 = off); exclusive with lagWeight
};

// Map the three pots. Pass analogRead(pin)/1023.f from the firmware or a 0..1
// VCV param directly.
//   pot1 — input-A attenuation           (GRAINS POT 1)
//   pot2 — attenuverter                  (GRAINS POT 2)
//   pot3 — lag (CCW half) / fuzz (CW half), the two sharing one dial
//          exactly as upstream does      (GRAINS POT 3)
inline SwitchbladeParams switchbladeMapParams(float pot1, float pot2, float pot3) {
  SwitchbladeParams p;

  p.atten = clampf(pot1, 0.0f, 1.0f);

  // Attenuverter: dial centre is silence, either end is unity with a sign.
  // Upstream reaches only 511/512 going positive (its `a - 512` never hits 512);
  // we make the two halves symmetric at exactly +/-1.
  p.gain = clampf(pot2, 0.0f, 1.0f) * 2.0f - 1.0f;

  // POT 3 is split at its centre detent. Work in upstream's 0..1023 ADC domain
  // so the two halves' break point and slopes are the original's.
  const float adc = clampf(pot3, 0.0f, 1.0f) * 1023.0f;
  if (adc < 512.0f) {
    // CCW half — lag. Upstream: smoothing = (511 - adc) >> 2, a 0..127 integer
    // used as an n/128 retention weight, so full CCW keeps 127/128 of the old
    // value per tick (~0.5 s time constant at 256 Hz). The >>2 truncation is
    // kept rather than smoothed out: retention runs into 1.0 so steeply that
    // dropping it would stretch full-CCW from 0.5 s to 2 s.
    // Clamped at both ends: upstream's ADC is an integer so 511 is its last lag
    // step, but a float dial sits at 511.5 dead centre and would otherwise floor
    // to -1 and hand back a negative retention weight.
    float sm = floorf((511.0f - adc) * 0.25f);
    if (sm < 0.0f) sm = 0.0f;
    if (sm > 127.0f) sm = 127.0f;
    p.lagWeight = sm * (1.0f / 128.0f);
    p.fuzz = 0.0f;
  } else {
    // CW half — fuzz. Upstream: rand = adc - 512, a 0..511 width in the 0..1023
    // domain, so full CW is +/-25% of full scale.
    p.lagWeight = 0.0f;
    p.fuzz = (adc - 512.0f) * (1.0f / 1023.0f);
  }

  return p;
}

// The engine. All signals are unipolar 0..1 CV, mid-scale (0.5) being the
// attenuverter's pivot and its resting output.
struct SwitchbladeVoice {
  // MOD1 addition, set from outside: GRAINS has a hardware switch that takes
  // POT 1 off IN 1 and makes it a manual level. MOD1 has no such switch, so the
  // panel button toggles this instead. false = attenuate input A (the default,
  // and the mode upstream's header tells you to set the switch to).
  bool manualMode = false;

  float accum = 0.0f;    // seconds banked toward the next control tick
  float hist1 = 0.0f;    // summed input, one tick ago
  float hist2 = 0.0f;    // summed input, two ticks ago
  float lag = 0.0f;      // one-pole state
  bool  lagPrimed = false;
  float held = 0.5f;     // last control-tick result, emitted until the next one
  uint32_t rng = 0x5b1adeu;

  void reset() {
    accum = 0.0f;
    hist1 = 0.0f;
    hist2 = 0.0f;
    lag = 0.0f;
    lagPrimed = false;
    held = 0.5f;
    rng = 0x5b1adeu;
  }

  void seed(uint32_t s) { rng = (s != 0u) ? s : 0x5b1adeu; }

  // xorshift32 folded into one white-noise sample in -1..+1. Deterministic and
  // identical on every target, so firmware and Rack fuzz alike from one seed.
  float nextNoise() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return (float)(rng >> 8) * (2.0f / 16777216.0f) - 1.0f;
  }

  // One control tick of upstream's updateControl(), in the order it runs.
  void controlStep(float inA, float inB, const SwitchbladeParams& p) {
    // 1. Sum. Input A passes through the attenuator; input B joins at unity.
    //    In manual mode POT 1 supplies the level itself and input A is ignored,
    //    which is GRAINS's "Man" switch position.
    const float in = (manualMode ? p.atten : inA * p.atten) + inB;

    // 2. De-glitch the sum with a 3-tap median before anything reacts to it.
    float cv = switchbladeMedian3(in, hist2, hist1);
    hist2 = hist1;
    hist1 = in;

    cv = clampf(cv, 0.0f, 1.0f);

    // 3. Lag OR fuzz — never both, because they share POT 3. Upstream calls this
    //    out in its own header as the module's central compromise; keeping it is
    //    the point of the port.
    if (p.lagWeight > 0.0f) {
      // Upstream primes its one-pole with `if (oldIn == 0) oldIn = in`, which is
      // meant to seed the filter on first use but re-fires any time the state
      // lands exactly on zero (a real possibility for a 0-referenced CV). A flag
      // does the intended job without the re-arm.
      if (!lagPrimed) {
        lag = cv;
        lagPrimed = true;
      } else {
        lag = lag * p.lagWeight + cv * (1.0f - p.lagWeight);
      }
      cv = lag;
    } else {
      // Keep the lag tracking while it is bypassed, so turning POT 3 back CCW
      // resumes from the current signal instead of jumping to a stale value.
      lag = cv;
      lagPrimed = true;
      if (p.fuzz > 0.0f) {
        // Upstream's addNoise() is `in + random(width) - 256`: a fixed -256
        // offset against a width that runs 0..511, so the noise is only centred
        // at full CW and the dial's centre detent drops the signal by a quarter
        // of full scale instead of passing it through. Centring the noise on the
        // width restores the "Normal" that upstream's own POT 3 legend promises,
        // and keeps its full-CW amplitude (+/-256/1023) exactly.
        cv += nextNoise() * p.fuzz * 0.5f;
        cv = clampf(cv, 0.0f, 1.0f);
      }
    }

    // 4. Attenuvert about mid-scale.
    //    Upstream's negative branch computes `(512*512) - input * (512 - a)`,
    //    whose 512*512 term survives the >>9 as a full-scale offset and pins the
    //    whole CCW half of POT 2 at maximum output. That contradicts its own
    //    "Fully Inverted <- Fully Attenuated -> Normal" legend, so we do what the
    //    legend says: one signed gain, applied about the pivot.
    held = clampf((cv - 0.5f) * p.gain + 0.5f, 0.0f, 1.0f);
  }

  // Advance by dt seconds, running the control chain when enough time has piled
  // up. Firmware passes its measured loop period; Rack passes args.sampleTime.
  void process(float dt, float inA, float inB, const SwitchbladeParams& p) {
    constexpr float kPeriod = 1.0f / kSwitchbladeControlHz;

    accum += dt;
    if (accum >= kPeriod) {
      accum -= kPeriod;
      // One tick per call, never a catch-up loop: an AVR that fell behind must
      // not then spend an unbounded stretch in here. Anything still banked past
      // a whole period was a stall, so drop it and resync.
      if (accum > kPeriod) accum = 0.0f;
      controlStep(inA, inB, p);
    }
  }

  // Processed CV, 0..1, mid-scale at rest.
  float out() const { return held; }

  // MOD1 addition: the same CV mirrored about mid-scale, for the second jack.
  float inverted() const { return 1.0f - held; }
};

}  // namespace sc
