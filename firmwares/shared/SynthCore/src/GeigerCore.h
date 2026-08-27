#pragma once

// Geiger engine — three-output random trigger generator / Bernoulli gate.
//
// Used by:
//   - firmwares/mod1-geiger/mod1-geiger.ino  (one step per loop, dt from millis)
//   - rack-plugins/src/mod1-geiger.cpp       (driven at audio rate, dt = args.sampleTime)
//
// This is a *clocked utility*, not a voice: nothing is synthesised per sample.
// A clock edge draws fresh random numbers and latches the three trigger
// outputs; dt only runs the pulse-width timer down afterwards, which is what
// keeps the same code honest at the firmware's ~1 kHz loop and at Rack's
// 44.1 kHz+ audio rate.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h. No Arduino / Rack / Pico SDK.
// float only, no heap, no STL — must compile on AVR, RP2350 and desktop.
//
// License:
// Derived from the GRAINS `geiger` firmware (github.com/eclab/grains) under
// the Apache License 2.0, Copyright 2023 Sean Luke. The upstream notice
// lives at firmwares/mod1-geiger/LICENSE.md and Apache 2.0 requires it to ship
// with any copy of this header — the CC0 cores next to it have no such
// condition, so don't fold this one into them.

// sc_math.h only. sc_dsp.h would be the natural home for the PRNG, but it
// cannot be included from a MOD1 sketch: its Biquad names coefficients B0..B2 /
// A1..A2, and on AVR those are already macros (Arduino's binary.h, and the
// analog-pin defines), so the header fails to compile behind Arduino.h. The
// xorshift32 steps are therefore written out inline in nextUniform() below —
// the same three shifts as sc::xorshift32, so the streams still match.
#include "sc_math.h"

namespace sc {

// Trigger width. The original has no timer at all: it raises the outputs on one
// clock and clears them on the *next* one, which is also why it only draws new
// random numbers every second clock. Owning a real pulse timer here lets every
// clock produce a draw (see the divergence list in the sketch header) and gives
// a width that reads as a trigger to anything downstream.
static const float kGeigerTriggerSec = 0.010f;

// Bernoulli-mode threshold: upstream switches when the raw POT 3 reading passes
// 1000 out of 1023, i.e. the last ~2% of dial travel.
static const float kGeigerBernoulliThreshold = 1000.0f / 1023.0f;

struct GeigerParams {
	float prob1;     // 0..1 firing probability for output 1
	float prob2;     // 0..1 firing probability for output 2
	float prob3;     // 0..1 firing probability for output 3 (unused in Bernoulli)
	bool  bernoulli; // true = exactly one output fires per clock
};

// Convert a normalised 0..1 control into a probability.
//
// Upstream compares Arduino's random(1000) — an integer in 0..999 — against the
// raw 0..1023 ADC reading, so the probability is threshold/1000 and the top 2%
// of the dial is a deliberate "always fires" plateau ("we do 1000 rather than
// 1023 because we want fully-right to essentially be guaranteed to fire").
// Scaling by 1023/1000 and clamping reproduces that exactly, and means the
// firmware (adc/1023.f) and the Rack knob (0..1) agree.
inline float geigerProb(float v01) {
	return clampf(v01 * (1023.0f / 1000.0f), 0.0f, 1.0f);
}

// Map the three normalised pot readings to engine parameters.
inline GeigerParams geigerMapParams(float pot1, float pot2, float pot3) {
	GeigerParams p;
	p.prob1     = geigerProb(pot1);
	p.prob2     = geigerProb(pot2);
	p.prob3     = geigerProb(pot3);
	p.bernoulli = (pot3 > kGeigerBernoulliThreshold);
	return p;
}

// State of the three outputs for one process() call.
struct GeigerResult {
	bool out1;
	bool out2;
	bool out3;
	bool any;  // any output high — drives the LED
};

struct GeigerEngine {
	uint32_t rngState;
	bool     out1, out2, out3;
	float    pulseTimer;      // seconds of trigger left, 0 when idle
	float    timeSinceClock;  // seconds since the last clock edge (caps width)

	GeigerEngine()
		: rngState(0x9E3779B9u), out1(false), out2(false), out3(false),
		  pulseTimer(0.0f), timeSinceClock(1.0f) {}

	void reset() {
		out1 = out2 = out3 = false;
		pulseTimer = 0.0f;
		timeSinceClock = 1.0f;
	}

	// xorshift32 needs a non-zero state; callers seed from ADC noise (firmware)
	// or the host RNG (Rack) so two instances don't march in lockstep.
	void seed(uint32_t s) {
		if (s == 0) s = 1;
		rngState = s;
	}

	// Uniform float in [0, 1). Drawn the same way on every platform, so a given
	// seed produces the same trigger pattern in the firmware and in Rack.
	// xorshift32 (Marsaglia 2003) written out inline — see the include note.
	float nextUniform() {
		rngState ^= rngState << 13;
		rngState ^= rngState >> 17;
		rngState ^= rngState << 5;
		return (float)(rngState >> 8) * (1.0f / 16777216.0f);
	}

	// Advance by dt seconds.
	//   clockRose : true for exactly one call when a clock/trigger edge arrives
	//   p         : from geigerMapParams()
	GeigerResult process(float dt, bool clockRose, const GeigerParams& p) {
		timeSinceClock += dt;
		if (clockRose) {
			out1 = out2 = out3 = false;
			if (p.bernoulli) {
				// Exactly one output fires. The draws are sequential and the
				// second one only happens if the first failed, so output 2's
				// real odds are (1-prob1)*prob2 and output 3 takes the rest —
				// upstream's documented "set POT 1 to 1/3 and POT 2 to 1/2 for
				// an even split" only works because of that nesting.
				if (nextUniform() < p.prob1)      out1 = true;
				else if (nextUniform() < p.prob2) out2 = true;
				else                              out3 = true;
			} else {
				// Three independent coins: any combination can fire, including
				// none at all. This is the Geiger-counter behaviour proper.
				if (nextUniform() < p.prob1) out1 = true;
				if (nextUniform() < p.prob2) out2 = true;
				if (nextUniform() < p.prob3) out3 = true;
			}
			// Nominal 10 ms triggers, but never wider than half the incoming
			// clock period: above ~50 Hz a fixed width would be re-armed
			// before it expired and a run of consecutive wins would hold the
			// output high with no falling edge for a downstream envelope to
			// see. (Upstream got its edges for free by only drawing on every
			// second clock; we draw on every clock — see the sketch header.)
			float width = kGeigerTriggerSec;
			if (timeSinceClock < 2.0f * kGeigerTriggerSec)
				width = 0.5f * timeSinceClock;
			pulseTimer = width;
			timeSinceClock = 0.0f;
		} else if (pulseTimer > 0.0f) {
			pulseTimer -= dt;
			if (pulseTimer <= 0.0f) {
				pulseTimer = 0.0f;
				out1 = out2 = out3 = false;
			}
		}

		GeigerResult r;
		r.out1 = out1;
		r.out2 = out2;
		r.out3 = out3;
		r.any  = out1 || out2 || out3;
		return r;
	}
};

}  // namespace sc
