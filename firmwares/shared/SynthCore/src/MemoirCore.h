#pragma once

// Memoir engine — a two-track CV + gate recorder with a 512-frame buffer.
//
// One take is 512 frames of 9-bit CV plus a 1-bit gate, packed two bytes per
// frame into a fixed 1024-byte buffer.  The frame rate is the divider: upstream
// ran its control loop at 128 Hz and committed one frame every `rate` ticks, so
// 512 frames stretch over 4 s (rate 1) up to 32 s (rate 8).  Because rate is
// read live, a take recorded at one length can be replayed at another — record
// 4 s and smear it over 32, or record 32 and cram it into 4.
//
// The buffer is deliberately 1024 bytes because that is exactly the ATmega328P's
// EEPROM size: the shell copies the whole thing out on `takeStoreRequest()` and
// back in at boot, which is how a recording survives a power cycle.  The engine
// itself never touches persistence — it is platform-pure and only says "now".
//
// Used by:
//   - firmwares/mod1-memoir/mod1-memoir.ino
//   - rack-plugins/src/mod1-memoir.cpp
//
// Pure C++: include only sc_math.h / sc_dsp.h. NO Arduino.h, rack.hpp, Pico SDK.
// float only, no heap, no STL — must compile on AVR, RP2350 and desktop.
//
// License:
// Derived from the GRAINS `memoir` firmware (github.com/eclab/grains) under
// the Apache License 2.0, Copyright 2023 Sean Luke. The upstream notice
// lives at firmwares/mod1-memoir/LICENSE.md and Apache 2.0 requires it to ship
// with any copy of this header — the CC0 cores next to it have no such
// condition, so don't fold this one into them.

#include "sc_math.h"

namespace sc {

struct MemoirFrame {
  float cv;    // 0..1, zero-order held between frame boundaries
  bool gate;   // gate track, also held
  float led;   // 0..1 panel LED
};

struct MemoirEngine {
  // Storage geometry, unchanged from upstream.  9 bits of CV leaves bit 1 of
  // the high byte free for the gate, which is what makes a frame fit in two:
  //
  //   --- DATA X+1 ---   ---- DATA X ----
  //   7 6 5 4 3 2 1 0    7 6 5 4 3 2 1 0
  //               G C    C C C C C C C C
  static const uint16_t kBytes = 1024;
  static const uint16_t kFrames = kBytes / 2;  // 512
  static const uint16_t kCvMax = 511;          // 9 bits

  // Upstream's Mozzi CONTROL_RATE, kept as the timebase so the length settings
  // land on the same 4/8/.../32 s that the original documents.
  static const uint8_t kBaseRateHz = 128;
  static const uint8_t kMinRate = 1;
  static const uint8_t kMaxRate = 8;

  static constexpr float kBlinkHz = 4.0f;  // record-LED blink

  enum State { kIdle = 0, kRecord = 1, kPlay = 2 };

  uint8_t data[kBytes];

  uint8_t state = kIdle;
  uint16_t cursor = 0;      // byte cursor into data[]
  bool committed = true;    // the current take has been padded + handed over
  bool storeRequest = false;
  uint8_t rate = kMinRate;

  bool recPending = false;  // triggers are latched, then consumed on a frame
  bool playPending = false;

  float accum = 0.0f;   // seconds since the last frame boundary
  float blink = 0.0f;   // 0..1 LED blink phase
  uint16_t cv = 0;      // 9-bit held CV: live input while recording, buffer while playing
  bool gate = false;

  // Transport reset only — data[] is the persisted recording and belongs to the
  // shell (EEPROM on MOD1, patch JSON in Rack), so it survives this.  Call
  // erase() as well for a genuine factory reset.
  void reset() {
    state = kIdle;
    cursor = 0;
    committed = true;
    storeRequest = false;
    recPending = false;
    playPending = false;
    accum = 0.0f;
    blink = 0.0f;
    cv = 0;
    gate = false;
  }

  void erase() {
    for (uint16_t i = 0; i < kBytes; i++) data[i] = 0;
  }

  // Raw buffer access for the shell's persistence layer.
  uint8_t* bytes() { return data; }
  const uint8_t* bytes() const { return data; }
  uint16_t byteCount() const { return kBytes; }

  // True once, when a take has just been padded and is ready to be written out.
  bool takeStoreRequest() {
    const bool pending = storeRequest;
    storeRequest = false;
    return pending;
  }

  // Length pot -> 1..8, i.e. 4..32 seconds.  Upstream wrote `adc >> 7 + 1`,
  // which C parses as `adc >> 8` and so only ever reached 0..3; this is the
  // 1..8 its own documentation describes.
  static uint8_t rateFromPot(float pot01) {
    const float v = clampf(pot01, 0.0f, 1.0f) * (float)kMaxRate;
    uint8_t r = (uint8_t)v + 1;
    if (r > kMaxRate) r = kMaxRate;
    return r;
  }

  void setRate(uint8_t r) {
    rate = (r < kMinRate) ? kMinRate : ((r > kMaxRate) ? kMaxRate : r);
  }

  float frameIntervalSec() const { return (float)rate / (float)kBaseRateHz; }
  float lengthSec() const { return (float)kFrames * frameIntervalSec(); }

  // Latched: both are consumed at the next frame boundary, which is where
  // upstream sampled its trigger jacks too.  Recording from the top erases.
  void triggerRecord() { recPending = true; }
  void triggerPlay() { playPending = true; }

  bool recording() const { return state == kRecord && !committed; }
  bool playing() const { return state == kPlay; }
  uint16_t cvRaw() const { return cv; }  // 0..511, ready for a 9-bit PWM register

  // Advance by dt seconds.  cvIn is 0..1 and gateIn is the gate track; both are
  // only sampled at a frame boundary, and only while recording.
  MemoirFrame process(float dt, float cvIn, bool gateIn) {
    const float interval = frameIntervalSec();
    accum += dt;
    if (accum >= interval) {
      accum -= interval;
      // A single stall must not fast-forward the take: the firmware's EEPROM
      // commit blocks for seconds, and it should resume from now rather than
      // dump the backlog into the buffer at once.
      if (accum >= interval) accum = 0.0f;
      advanceFrame(cvIn, gateIn);
    }

    blink += dt * kBlinkHz;
    while (blink >= 1.0f) blink -= 1.0f;

    MemoirFrame f;
    f.cv = (float)cv / (float)kCvMax;
    f.gate = gate;
    // The LED is ours — GRAINS has no indicator at all, and without one there is
    // no way to tell a running take from a finished one.  Blink while the take
    // is rolling, follow the CV on playback, dark otherwise.
    if (recording()) {
      f.led = (blink < 0.5f) ? 1.0f : 0.0f;
    } else if (state == kPlay) {
      f.led = f.cv;
    } else {
      f.led = 0.0f;
    }
    return f;
  }

 private:
  static uint16_t quantise(float cv01) {
    return (uint16_t)(clampf(cv01, 0.0f, 1.0f) * (float)kCvMax + 0.5f);
  }

  void writeFrame(uint16_t v, bool g) {
    cv = v;
    gate = g;
    data[cursor] = (uint8_t)(v & 255);
    data[cursor + 1] = (uint8_t)((v >> 8) | (g ? 2 : 0));
    cursor += 2;
  }

  void readFrame() {
    cv = (uint16_t)data[cursor] | ((uint16_t)(data[cursor + 1] & 1) << 8);
    gate = ((data[cursor + 1] >> 1) & 1) != 0;
    cursor += 2;
  }

  // Upstream's "stretch": copy each remaining frame from the one before it, so a
  // take shorter than the buffer holds its final value for the rest of the
  // playback instead of running into whatever the previous take left there.
  // (Upstream wrote data[i] and data[i+1] per step, which duplicates every write
  // and runs one byte past the end of the array on the last iteration.)
  void pad() {
    for (uint16_t i = cursor; i < kBytes; i++) data[i] = data[i - 2];
  }

  void commit() {
    if (cursor >= 2 && cursor < kBytes) pad();
    committed = true;
    storeRequest = true;
  }

  void advanceFrame(float cvIn, bool gateIn) {
    // Upstream's ordering, kept: a record trigger wins, and a play trigger is
    // then considered in the same frame — so REC followed immediately by PLAY
    // commits the (one frame long) take and replays it.
    if (recPending) {
      recPending = false;
      committed = false;
      cursor = 0;
      state = kRecord;
      writeFrame(quantise(cvIn), gateIn);  // frame 0 is captured on the trigger
    } else if (state == kRecord && !committed) {
      if (cursor < kBytes - 1) writeFrame(quantise(cvIn), gateIn);
      if (cursor >= kBytes) commit();  // the take ran its full length
    }

    if (playPending) {
      playPending = false;
      if (state == kRecord && !committed) commit();  // save a take in progress
      cursor = 0;
      state = kPlay;
      readFrame();
    } else if (state == kPlay) {
      if (cursor < kBytes - 1) readFrame();
      // At the end the cursor parks and the last frame is held on both outputs.
      if (cursor >= kBytes) cursor = kBytes;
    }
  }
};

}  // namespace sc
