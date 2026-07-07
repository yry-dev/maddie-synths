#pragma once

// Tape Echo — worn-tape delay core.
//
// Used by:
//   - firmwares/mod2-tape-echo/mod2-tape-echo.ino
//   - rack-plugins/src/mod2-tape-echo.cpp
//
// Same sc::DelayLine engine as DelayFxCore, but the read head is a tape
// transport: the write head advances 1 sample/sample while the read head
// advances at a servo-controlled `rate`, so time changes are heard as
// tape-speed pitch bends (the key feel — repeats glide, never crossfade).
// On top of that:
//   - Wow (slow sine on the read position) + flutter (faster jitter + filtered
//     noise), both scaled by the "tape age" macro.
//   - Feedback path: soft saturation -> one-pole low-pass whose cutoff drops
//     with age -> occasional dropouts (short random gain dips at high age).
//   - Splice gate: read rate ramps exponentially to 0 (tape stop — pitch
//     dives, output fades) and lurches back up to re-catch the write head on
//     release.
// The read/write gap accumulates in double: 1-rate deltas can be ~1e-3 sample
// against a gap of ~1e5 samples, which float32 quantizes audibly (README's
// 64-bit-phase note). RP2350 has a hardware double coprocessor, so this stays
// cheap on the firmware too.
//
// Pure C++: depends only on sc_math.h / sc_dsp.h (<math.h>/<stdint.h>). No
// Arduino.h, rack.hpp or Pico SDK; no heap, no STL — compiles on AVR, RP2350
// and the desktop.

#include "sc_dsp.h"
#include "sc_math.h"

namespace sc {

// POT1 -> delay time in seconds: exponential taper 30 ms (pot=0) .. maxSec.
inline float tapeEchoTimeSec(float pot01, float maxSec) {
  return 0.030f * powf(maxSec / 0.030f, clampf(pot01, 0.0f, 1.0f));
}

// Shift-layer pot -> feedback amount: 0 .. ~110%.
inline float tapeEchoFeedback(float pot01) {
  return 1.1f * clampf(pot01, 0.0f, 1.0f);
}

struct TapeEchoCore {
  // Parameters (write directly; see the mappers above).
  float timeSec = 0.4f;   // target delay time (tape speed servos toward it)
  float age = 0.3f;       // 0 new tape .. 1 dying: wow/flutter+HF loss+sat+dropouts
  float feedback = 0.5f;  // 0 .. ~1.1
  float wet = 0.5f;       // 0 dry .. 1 fully wet
  bool splice = false;    // gate: tape-stop lurch while high

  // Servo/transport tuning.
  static constexpr float kServoSec = 0.22f;   // gap settle time constant
  static constexpr float kRateMin = 0.0f;
  static constexpr float kRateMax = 2.5f;
  static constexpr float kSpliceSec = 0.06f;  // tape-stop ramp time constant

  // State.
  DelayLine line;
  double gap = 12000.0;      // read/write head distance in samples
  float spliceEnv = 1.0f;    // 1 running .. 0 stopped
  float wowPhase = 0.0f;
  float flutterPhase = 0.0f;
  float flutterNoise = 0.0f;  // low-passed noise component
  uint32_t rng = 0x7a9e0421u;
  float fbLp = 0.0f;          // feedback low-pass state
  float dropGain = 1.0f;      // dropout gain (slewed)
  float dropTimer = 0.0f;     // seconds of dropout remaining
  float led = 0.0f;           // 0..1 "tape health" flicker for the panel LED
  bool primed = false;        // first process() snaps the gap to the target

  // `buf`/`n`: caller-owned arena. Size it beyond the max delay time — the
  // gap grows 1 sample/sample during a tape stop, and the extra room is what
  // makes the release lurch audible before the gap clamps.
  void init(int16_t* buf, uint32_t n) {
    line.init(buf, n);
    reset();
  }

  void reset() {
    line.clear();
    primed = false;  // gap snaps to the target on the next process()
    spliceEnv = 1.0f;
    wowPhase = flutterPhase = 0.0f;
    flutterNoise = 0.0f;
    rng = 0x7a9e0421u;
    fbLp = 0.0f;
    dropGain = 1.0f;
    dropTimer = 0.0f;
    led = 0.0f;
  }

  // Advance one sample of `dt` seconds and return the wet/dry output.
  float process(float in, float dt) {
    const float maxGap = (float)(line.len - 2);
    const float targetGap = clampf(timeSec / dt, 1.0f, maxGap);
    if (!primed) {
      gap = (double)targetGap;
      primed = true;
    }
    const float a2 = age * age;  // most degradations come in quadratically

    // --- transport: servo the read rate so the gap settles on the target.
    // rate = spliceEnv * (1 + err/settle) -> exponential glide with a pitch
    // bend proportional to how far the time knob just moved.
    spliceEnv += ((splice ? 0.0f : 1.0f) - spliceEnv) * (dt / kSpliceSec);
    const float err = (float)(gap - (double)targetGap);
    // The servo alone stays >= 0.3x (a long jump pitch-dives, never stalls);
    // only the splice envelope can take the transport all the way to 0.
    float rate = spliceEnv * clampf(1.0f + err * (dt / kServoSec), 0.3f, kRateMax);
    rate = clampf(rate, kRateMin, kRateMax);
    gap += (double)(1.0f - rate);
    if (gap > (double)maxGap) gap = (double)maxGap;  // stop ran out of tape
    if (gap < 1.0) gap = 1.0;

    // --- wow & flutter: position modulation on the read head, depth ~ age².
    wowPhase += 0.9f * dt;
    if (wowPhase >= 1.0f) wowPhase -= 1.0f;
    flutterPhase += 8.0f * dt;
    if (flutterPhase >= 1.0f) flutterPhase -= 1.0f;
    flutterNoise += (noise1f(rng) - flutterNoise) * onePoleCoef(30.0f, dt);
    const float flutter = sinf(kTwoPi * flutterPhase) * 0.6f + flutterNoise;
    // wow up to +/-3 ms, flutter up to +/-0.4 ms of read-position offset
    const float modSec = a2 * (0.0030f * sinf(kTwoPi * wowPhase) + 0.0004f * flutter);
    const float echoRaw = line.read((float)gap + modSec / dt);

    // --- dropouts: at high age, brief random gain dips in the echo signal.
    if (dropTimer > 0.0f) {
      dropTimer -= dt;
    } else if (a2 > 0.25f) {
      // expected rate ramps 0 -> ~1.5 dips/sec across the top half of the pot
      const float p = (a2 - 0.25f) * 2.0f * dt;
      if ((float)(xorshift32(rng) >> 8) * (1.0f / 16777216.0f) < p)
        dropTimer = 0.025f;
    }
    dropGain += ((dropTimer > 0.0f ? 0.25f : 1.0f) - dropGain) * onePoleCoef(120.0f, dt);
    const float echo = echoRaw * dropGain;

    // --- feedback path: saturation drive and HF loss both worsen with age.
    float fb = softClipTanh(echo * (1.0f + 1.5f * age), 1.5f + 2.0f * age);
    const float fc = 8000.0f * powf(1200.0f / 8000.0f, age);
    fbLp += (fb - fbLp) * onePoleCoef(fc, dt);
    fb = fbLp;

    line.write(softSat(in + fb * feedback));

    // --- LED: tape health — steady when new, flickering when worn.
    led = clampf(0.85f - 0.5f * a2 + 0.45f * a2 * flutter, 0.0f, 1.0f) *
          (0.3f + 0.7f * dropGain) * spliceEnv;

    // Tape-stop also fades the wet signal: a read head at rate ~0 would
    // otherwise hold a DC value. Normal servo glides (rate < 1) stay at
    // full level — only the splice envelope fades.
    const float wetSig = echo * spliceEnv;
    return in + (wetSig - in) * wet;
  }
};

}  // namespace sc
