#pragma once
#include <stdint.h>

/*
	SamplePlayer — pure-C++ fixed-rate PCM sample playback voice.

	Shared core for the mod2-sample and mod2-breakbeats firmwares' Rack ports
	(and adoptable by the firmwares themselves). Mirrors their playback engine:
	a fractional read pointer advanced by `step` source-samples per output
	sample, with linear interpolation between 16-bit little-endian PCM samples.
	One-shot by default; optional loop restarts the current slice.

	Sample data is a byte array of 16-bit little-endian mono PCM (the format
	produced by scripts/wav_to_sample.py). `data` points at the first byte of
	the slice to play; `len` is the slice length in 16-bit samples.

	Depends only on <stdint.h> — no Arduino/Rack headers — so both platforms
	share it. The caller computes `step` from the desired playback rate and the
	ratio of source rate to output rate, e.g.
		step = playbackRate * sourceRateHz / engineRateHz
	which keeps pitch correct independent of the host sample rate.
*/
namespace sc {

struct SamplePlayer {
	const uint8_t* data = nullptr;  // first byte of the slice (16-bit LE PCM)
	uint32_t len = 0;               // slice length in 16-bit samples
	double pos = 0.0;               // fractional read index within the slice
	double step = 1.0;              // source samples advanced per output sample
	bool playing = false;
	bool loop = false;

	static inline float readNorm(const uint8_t* b, uint32_t i) {
		uint32_t k = i << 1;  // 2 bytes per sample
		int16_t s = (int16_t)((uint16_t)b[k] | ((uint16_t)b[k + 1] << 8));
		return s * (1.0f / 32768.0f);  // -1 .. ~+1
	}

	// Start (or restart) playback of a slice.
	void trigger(const uint8_t* sliceStart, uint32_t sliceLen, double stp, bool lp) {
		data = sliceStart;
		len = sliceLen;
		step = stp;
		loop = lp;
		pos = 0.0;
		playing = (data != nullptr && len > 0);
	}

	void stop() { playing = false; }

	// One output sample in -1..1. If `ended` is non-null it is set true on the
	// sample where a playback pass completes (each pass, including loop wraps),
	// suitable for driving an end-of-cycle pulse.
	float process(bool* ended = nullptr) {
		if (ended) *ended = false;
		if (!playing || !data) return 0.0f;

		uint32_t idx = (uint32_t)pos;
		if (idx >= len) {
			if (ended) *ended = true;
			if (loop) {
				pos -= (double)len;  // wrap, preserving fractional phase
				idx = (uint32_t)pos;
				if (idx >= len) { pos = 0.0; idx = 0; }  // guard huge steps
			} else {
				playing = false;
				return 0.0f;
			}
		}

		double frac = pos - (double)idx;
		float s1 = readNorm(data, idx);
		float s2 = (idx + 1 < len) ? readNorm(data, idx + 1) : 0.0f;
		pos += step;
		return s1 + (s2 - s1) * (float)frac;
	}
};

}  // namespace sc
