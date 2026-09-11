#pragma once

// Tardy engine — the shared core of the Tardy trigger/gate delay module.
//
// Tardy re-echoes the gates or triggers at its inputs after a small, settable
// delay (up to ~853 ms), so drums and other fast material can be pushed into
// sync with a device that has latency. Each channel is a one-bit ring buffer:
// the incoming gate level is written *ahead* of the read head by the delay
// amount, and the read head emits whatever was scheduled for now. Turning the
// delay knob therefore re-times what is already in flight rather than flushing
// it — that is upstream's scheme, and (together with the smoothed knob) it is
// why a new delay setting takes a moment to settle.
//
// Used by:
//   - firmwares/mod1-tardy/mod1-tardy.ino   (paced from the main loop)
//   - rack-plugins/src/mod1-tardy.cpp       (per-sample at audio rate)
//
// Pure C++: depends only on sc_math.h. No Arduino / Rack / Pico SDK.
// float only, no heap, no STL — must compile on AVR, RP2350 and desktop.
//
// License:
// Derived from the GRAINS `tardy` firmware (github.com/eclab/grains) under
// the Apache License 2.0, Copyright 2023 Sean Luke. The upstream notice
// lives at firmwares/mod1-tardy/LICENSE.md and Apache 2.0 requires it to ship
// with any copy of this header — the CC0 cores next to it have no such
// condition, so don't fold this one into them.

#include "sc_math.h"

namespace sc {

// The engine's internal tick grid. Upstream did four analogReads per pass of
// loop(), which an ATmega328P completes ~2404 times a second, and it used that
// loop rate *as* its clock — so the delay it produced depended on how long the
// loop happened to take. We keep the same 2400 Hz grid but advance it from a
// caller-supplied dt, which makes the delay honest on both targets (and lets
// Rack run the same engine at 44.1/48/96 kHz without changing the timing).
constexpr float kTardyTickRate = 2400.0f;
constexpr float kTardyTickPeriod = 1.0f / kTardyTickRate;

// Ring length in ticks (upstream's BUFFERLEN). One bit per tick per channel,
// so the whole delay memory is kTardyChannels * 512 bytes — the ceiling here
// is AVR SRAM, not the algorithm.
constexpr uint16_t kTardyBufferBits = 4096;
constexpr uint16_t kTardyBufferBytes = kTardyBufferBits / 8;
constexpr uint8_t kTardyChannels = 2;

// Longest delay, in ticks: upstream's `_delay * 2 + 1` with _delay at full
// scale (1023), i.e. 2047 ticks = 852.9 ms at 2400 Hz. Must stay < the ring.
constexpr uint16_t kTardyMaxDelayTicks = 2047;

// Knob smoothing, applied once per tick. Upstream's `(_delay * 15 + adc) >> 4`
// is a one-pole with a 1/16 coefficient; in float it converges instead of
// stalling on the integer truncation, which is the whole reason upstream's
// header warns that the knob takes "a few seconds" to arrive.
constexpr float kTardyDelaySmoothing = 1.0f / 16.0f;

// Delay ranges (POT3). Full scale is upstream's ~853 ms; the two shorter
// settings just scale the same knob law so the latency-compensation use case
// this module exists for gets usable resolution near zero.
//   0 SHORT   1..257  ticks   (0.4 .. 107.1 ms)
//   1 MEDIUM  1..1024 ticks   (0.4 .. 426.7 ms)
//   2 LONG    1..2047 ticks   (0.4 .. 852.9 ms)  == upstream
inline float tardyRangeScale(uint8_t range) {
  if (range == 0) return 0.125f;
  if (range == 1) return 0.5f;
  return 1.0f;
}

struct TardyEngine {
  // One bit per tick per channel. bit(pos) holds the gate level scheduled to
  // come out when the read head reaches tick `pos`.
  uint8_t buffer[kTardyChannels][kTardyBufferBytes];

  uint16_t head;                     // read/write cursor (upstream's `time`)
  float delayAdc[kTardyChannels];    // smoothed knob, in upstream's 0..1023 units
  float delayTarget[kTardyChannels]; // raw knob, same units
  bool pending[kTardyChannels];      // input seen since the last tick (OR-latched)
  bool out[kTardyChannels];          // current output level per channel
  float acc;                         // seconds banked toward the next tick
  uint8_t range;                     // 0 short / 1 medium / 2 long
  bool link;                         // channel B follows channel A's knob

  TardyEngine() { reset(); }

  void reset() {
    for (uint8_t ch = 0; ch < kTardyChannels; ++ch) {
      for (uint16_t i = 0; i < kTardyBufferBytes; ++i) buffer[ch][i] = 0;
      delayAdc[ch] = 0.0f;
      delayTarget[ch] = 0.0f;
      pending[ch] = false;
      out[ch] = false;
    }
    head = 0;
    acc = 0.0f;
    range = 2;
    link = false;
  }

  // Knob target for one channel, as a normalised 0..1 control. Held as
  // upstream's 0..1023 ADC units so the delay law is literally upstream's.
  void setDelay01(uint8_t ch, float pot01) {
    if (ch >= kTardyChannels) return;
    delayTarget[ch] = clampf(pot01, 0.0f, 1.0f) * 1023.0f;
  }

  // Jump the smoother straight to the knob (power-on / patch load), so the
  // first echo comes out at the set delay instead of sliding in from zero.
  void primeDelay(uint8_t ch, float pot01) {
    if (ch >= kTardyChannels) return;
    setDelay01(ch, pot01);
    delayAdc[ch] = delayTarget[ch];
  }

  void setRange(uint8_t r) { range = (r > 2) ? 2 : r; }

  void setLink(bool l) { link = l; }

  // Current delay of one channel, in ticks. Upstream's `_delay * 2 + 1`,
  // scaled by the range setting and clamped to what the ring can hold.
  uint16_t delayTicks(uint8_t ch) const {
    const float t = 1.0f + 2.0f * delayAdc[ch] * tardyRangeScale(range) + 0.5f;
    if (t <= 1.0f) return 1;
    if (t >= (float)kTardyMaxDelayTicks) return kTardyMaxDelayTicks;
    return (uint16_t)t;
  }

  float delaySeconds(uint8_t ch) const {
    return (float)delayTicks(ch) * kTardyTickPeriod;
  }

  // Advance by dt seconds, having seen gate levels inA/inB. Inputs are
  // OR-latched between ticks so a trigger shorter than one tick (very possible
  // at Rack's sample rate, impossible on the hardware) still gets echoed.
  // Returns true when at least one tick fired, i.e. when out[] may have moved.
  bool process(float dt, bool inA, bool inB) {
    if (inA) pending[0] = true;
    if (inB) pending[1] = true;

    acc += dt;
    // A patch load or a stalled loop can hand us a huge dt; bank a quarter of
    // a second at most rather than spinning through hundreds of ticks.
    if (acc > 0.25f) acc = 0.25f;

    bool ticked = false;
    while (acc >= kTardyTickPeriod) {
      acc -= kTardyTickPeriod;
      tick();
      ticked = true;
    }
    return ticked;
  }

 private:
  void tick() {
    for (uint8_t ch = 0; ch < kTardyChannels; ++ch) {
      const float target = (link && ch == 1) ? delayTarget[0] : delayTarget[ch];
      delayAdc[ch] += (target - delayAdc[ch]) * kTardyDelaySmoothing;

      // Read what was scheduled for now, then schedule the current input —
      // upstream's order, and it keeps a delay of one tick meaningful.
      out[ch] = readBit(ch, head);

      uint16_t at = head + delayTicks(ch);
      if (at >= kTardyBufferBits) at -= kTardyBufferBits;
      writeBit(ch, at, pending[ch]);
      pending[ch] = false;
    }

    if (++head >= kTardyBufferBits) head = 0;
  }

  // Upstream's bittest() reads `buffer[pos / 8] & (1 << (pos % 8)) != 0`, which
  // C parses as `buffer[pos / 8] & 1` because != binds tighter than &. That
  // returns bit 0 of the byte no matter which bit was asked for, so seven of
  // every eight scheduled gates were never emitted and the delay quantised to
  // 8 ticks. bitset()/bitclear() have no such bug, so the intent is plain: we
  // read the bit that was written.
  bool readBit(uint8_t ch, uint16_t pos) const {
    return (buffer[ch][pos >> 3] & (uint8_t)(1u << (pos & 7))) != 0;
  }

  void writeBit(uint8_t ch, uint16_t pos, bool value) {
    const uint8_t mask = (uint8_t)(1u << (pos & 7));
    if (value) {
      buffer[ch][pos >> 3] |= mask;
    } else {
      buffer[ch][pos >> 3] &= (uint8_t)~mask;
    }
  }
};

}  // namespace sc
