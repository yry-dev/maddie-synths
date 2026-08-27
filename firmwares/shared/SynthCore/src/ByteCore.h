#pragma once

// Byte — a 16-expression bytebeat emitter.
//
// A "bytebeat" is a single C expression over a free-running 32-bit counter `t`;
// the low 8 bits of the result ARE the audio sample. There is no oscillator, no
// envelope and no filter — the arithmetic is the instrument, so this core is
// almost entirely integer math and deliberately stays that way. Making it
// "nicer" (float phase, interpolation, band-limiting) would destroy the sound.
//
// Used by:
//   - firmwares/mod2-byte/mod2-byte.ino
//   - rack-plugins/src/mod2-byte.cpp
//
// Pure C++: include only sc_math.h / sc_dsp.h. NO Arduino.h, rack.hpp, Pico SDK.
// float only, no heap, no STL — must compile on AVR, RP2350 and desktop.
//
// License:
// Derived from the GRAINS `byte` firmware (github.com/eclab/grains) under
// the Apache License 2.0, Copyright 2024 Sean Luke. The upstream notice
// lives at firmwares/mod2-byte/LICENSE.md and Apache 2.0 requires it to ship
// with any copy of this header — the CC0 cores next to it have no such
// condition, so don't fold this one into them.

#include "sc_math.h"

namespace sc {

// ── Rate / range constants ──────────────────────────────────────────────────

// How many bytebeat expressions the switch below knows about.
constexpr int kByteNumExpressions = 16;

// Bytebeat "sample rates". The expression is evaluated at one of these, NOT at
// the host sample rate, and the result is held until the next evaluation —
// which is what keeps the timbre identical whether this runs in the firmware's
// 36.6 kHz ISR or in Rack at 44.1/48/96 kHz.
//
// Upstream is locked to Mozzi's 16384 Hz and its header says so with regret:
// "You can't really tune BYTE: to do so would require changing the sampling
// rate, and Mozzi doesn't make it easy to do that ... Maybe later." Neither
// target here is Mozzi, so "later" is now: 8000 Hz is the rate the classic
// viznut formulas were written for, and the other three are the usual
// alternates. Index 2 (16384 Hz) is upstream's, and is the power-on default.
constexpr int kByteNumRates = 4;
constexpr int kByteDefaultRate = 2;  // 16384 Hz == upstream's Mozzi rate

inline float byteRateHz(int index) {
  switch (index) {
    case 0: return 8000.0f;   // "traditional" bytebeat rate
    case 1: return 11025.0f;
    case 3: return 22050.0f;
    default: return 16384.0f;  // upstream (Mozzi AUDIO_RATE)
  }
}

// Quantise a normalised 0..1 control into `n` equal steps (0..n-1). Upstream
// derives its selectors by shifting the raw 10-bit ADC (`adc>>6` -> 0..15,
// `adc>>5` -> 0..31, `adc>>2` -> 0..255); for adc/1023 fed in here that is the
// same partition, so firmware and Rack land on identical steps.
inline int byteStepFromPot(float v01, int n) {
  int s = (int)(clampf(v01, 0.0f, 1.0f) * (float)n);
  if (s >= n) s = n - 1;
  if (s < 0) s = 0;
  return s;
}

// ── Defined-behaviour operators for the expression table ────────────────────
//
// Several upstream formulas shift by a count derived from `t`, which grows past
// 31 within seconds, and one of them divides by a value that reaches zero. In C
// both are undefined, and the three targets disagree in practice (x86 masks the
// shift count to 5 bits; Cortex-M33 and avr-gcc's bit-at-a-time shift loop both
// yield 0). A shared core cannot afford that, so shifts of 32 or more resolve to
// 0 — matching the two targets this port actually ships on — and division or
// modulo by zero yields 0 instead of trapping.

inline uint32_t bbShr(uint32_t v, uint32_t n) { return n >= 32u ? 0u : (v >> n); }
inline uint32_t bbDiv(uint32_t a, uint32_t b) { return b == 0u ? 0u : a / b; }

// Wrap a float into a 32-bit integer without invoking the undefined behaviour of
// an out-of-range float->int conversion. Only BYTEBEAT_12 needs this: its value
// grows as t^3/1e7 and leaves int range in under a second.
inline int32_t bbWrap32(float v) {
  if (!isFiniteF(v)) return 0;
  const float m = fmodf(v, 4294967296.0f);          // -2^32 .. 2^32
  return (int32_t)(uint32_t)(m < 0.0f ? m + 4294967296.0f : m);
}

// ── The expression table ────────────────────────────────────────────────────
//
// All sixteen are upstream's BYTEBEAT_1..BYTEBEAT_16 verbatim in behaviour; the
// verbatim source text is quoted above each case so the parenthesisation can be
// checked by eye. The only edits are the safe operators above, explicit
// parentheses (C's precedence is unchanged by them), and `1e7f` for upstream's
// `1e7` — on the Nano `double` IS `float`, so the f suffix restores the
// hardware's arithmetic rather than changing it.
//
// `x` is upstream's auxiliary CV variable. None of the stock sixteen use it; it
// exists so that anyone editing this table has a live control to reach for.
inline uint8_t byteEval(int expr, uint32_t t, uint32_t x) {
  (void)x;
  uint32_t v = 0;
  switch (expr) {
    // t*(42&t>>10)
    case 0:
      v = t * (42u & (t >> 10));
      break;
    // t|t%255|t%257
    case 1:
      v = t | (t % 255u) | (t % 257u);
      break;
    // t*(((t>>11)&(t>>8))&(123&(t>>3)))
    case 2:
      v = t * (((t >> 11) & (t >> 8)) & (123u & (t >> 3)));
      break;
    // t*(t>>((t>>9)|(t>>8))&(63&(t>>4)))
    case 3:
      v = t * (bbShr(t, (t >> 9) | (t >> 8)) & (63u & (t >> 4)));
      break;
    // (t>>6|t|t>>(t>>16))*10+((t>>11)&7)
    case 4:
      v = (((t >> 6) | t | bbShr(t, t >> 16)) * 10u) + ((t >> 11) & 7u);
      break;
    // (t|(t>>9|t>>7))*t&(t>>11|t>>9)
    case 5:
      v = ((t | ((t >> 9) | (t >> 7))) * t) & ((t >> 11) | (t >> 9));
      break;
    // t*5&(t>>7)|t*3&(t*4>>10)
    case 6:
      v = ((t * 5u) & (t >> 7)) | ((t * 3u) & ((t * 4u) >> 10));
      break;
    // (t>>7|t|t>>6)*10+4*(t&t>>13|t>>6)
    case 7:
      v = (((t >> 7) | t | (t >> 6)) * 10u) + (4u * ((t & (t >> 13)) | (t >> 6)));
      break;
    // ((t&4096)?((t*(t^t%255)|(t>>4))>>1):(t>>3)|((t&8192)?t<<2:t))
    case 8:
      v = (t & 4096u) ? ((((t * (t ^ (t % 255u))) | (t >> 4)) >> 1))
                      : ((t >> 3) | ((t & 8192u) ? (t << 2) : t));
      break;
    // ((t*(t>>8|t>>9)&46&t>>8))^(t&t>>13|t>>6)
    case 9:
      v = (((t * ((t >> 8) | (t >> 9))) & 46u) & (t >> 8)) ^
          ((t & (t >> 13)) | (t >> 6));
      break;
    // (t*5&t>>7)|(t*3&t>>10)
    case 10:
      v = ((t * 5u) & (t >> 7)) | ((t * 3u) & (t >> 10));
      break;
    // (int)(t/1e7*t*t+t)%127|t>>4|t>>5|t%127+(t>>16)|t
    case 11: {
      const float f = (float)t / 1e7f * (float)t * (float)t + (float)t;
      // Upstream's `(int)` is 16-bit on the Nano and out of range besides, so the
      // exact wrap is the one thing here that cannot be reproduced faithfully;
      // this wraps to 32 bits, which changes expression 12's grain (it is
      // float-precision noise on every target either way).
      const uint32_t m = (uint32_t)(bbWrap32(f) % 127);
      v = m | (t >> 4) | (t >> 5) | ((t % 127u) + (t >> 16)) | t;
      break;
    }
    // ((t/2*(15&(0x234568a0>>(t>>8&28))))|t/2>>(t>>11)^t>>12)+(t/16&t&24)
    case 12:
      v = (((t / 2u) * (15u & (0x234568a0ul >> ((t >> 8) & 28u)))) |
           (bbShr(t / 2u, t >> 11) ^ (t >> 12))) +
          ((t / 16u) & t & 24u);
      break;
    // (t&t%255)-(t*3&t>>13&t>>6)
    case 13:
      v = (t & (t % 255u)) - (((t * 3u) & (t >> 13)) & (t >> 6));
      break;
    // t>>4|t&((t>>5)/(t>>7-(t>>15)&-t>>7-(t>>15)))
    // Additive binds tighter than shift, so the shift count is 7-(t>>15) — an
    // unsigned subtraction that wraps huge once t passes 8*32768, from which
    // point bbShr zeroes the divisor and the formula settles into t>>4.
    case 14: {
      const uint32_t sh = 7u - (t >> 15);
      v = (t >> 4) | (t & bbDiv(t >> 5, bbShr(t, sh) & bbShr(0u - t, sh)));
      break;
    }
    // t*(((t>>9)&10)|((t>>11)&24)^((t>>10)&15&(t>>15)))
    case 15:
      v = t * (((t >> 9) & 10u) |
               (((t >> 11) & 24u) ^ (((t >> 10) & 15u) & (t >> 15))));
      break;
    default:
      v = 0;
      break;
  }
  return (uint8_t)(255u & v);  // upstream: `(uint8_t)(255 & (BYTEBEAT_n))`
}

// ── Frame ───────────────────────────────────────────────────────────────────

struct ByteFrame {
  float audio;  // -1..+1
  float env;    // 0..1 (LED brightness — bytebeat has no envelope, so |audio|)
};

// ── Voice ───────────────────────────────────────────────────────────────────

struct ByteVoice {
  // Parameters, in upstream's own units so the arithmetic below is unchanged.
  uint8_t expression = 0;   // 0..15   upstream `expression` (POT2 >> 6)
  uint8_t pitchStep = 8;    // 1..16   upstream `frequency`  (POT1 >> 6, +1)
  uint8_t gainStep = 16;    // 0..31   upstream `gain`       (POT3 >> 5)
  uint8_t aux = 0;          // 0..255  upstream `x`          (IN3 >> 2)
  uint8_t rateIndex = kByteDefaultRate;

  // Upstream's rate divider. Below the centre detent it evaluates the formula
  // only every `maxCount` ticks (slower, lower); above it, it evaluates every
  // tick but advances `t` by `increment` (faster, and a different waveform,
  // because skipping values of t is not the same as playing them slower).
  uint8_t maxCount = 1;
  uint8_t increment = 1;
  uint8_t count = 1;

  uint32_t t = 0;
  uint8_t lastExpression = 0;

  float phase = 0.0f;  // fractional base-rate tick accumulator
  float held = 0.0f;   // last evaluated sample, held between ticks

  void reset() {
    t = 0;
    count = 1;
    phase = 0.0f;
    held = 0.0f;
    lastExpression = expression;
  }

  // Upstream's D8 reset input: restart the sequence from t = 0.
  void restart() { t = 0; }

  // POT1 — pitch scaling. Upstream: `frequency = (adc>>6)+1`, then a branch at
  // 8 that splits "play fewer times" from "advance t faster".
  void setPitch(float pot01) {
    pitchStep = (uint8_t)(byteStepFromPot(pot01, 16) + 1);  // 1..16
    if (pitchStep >= 8) {
      maxCount = 1;
      increment = (uint8_t)(pitchStep - 7);  // 1..9
    }
    else {
      maxCount = (uint8_t)(9 - pitchStep);   // 8..2
      increment = 1;
    }
  }

  // POT3/CV — which formula. Upstream restarts `t` whenever the selection
  // changes, so a scan across the pot re-attacks each formula from its opening.
  void setExpression(float v01) {
    expression = (uint8_t)byteStepFromPot(v01, kByteNumExpressions);
    if (expression != lastExpression) {
      t = 0;
      lastExpression = expression;
    }
  }

  // POT2 — output level (upstream's POT3).
  void setLevel(float pot01) { gainStep = (uint8_t)byteStepFromPot(pot01, 32); }

  // IN2 — the auxiliary variable `x`, 0..255 (upstream's IN3 CV).
  void setAux(float v01) { aux = (uint8_t)byteStepFromPot(v01, 256); }

  void setRateIndex(int index) {
    if (index < 0) index = 0;
    if (index >= kByteNumRates) index = kByteNumRates - 1;
    rateIndex = (uint8_t)index;
  }

  // One bytebeat tick at the selected base rate — upstream's updateAudio().
  void tick() {
    count--;
    if (count != 0) return;
    count = maxCount;
    t += increment;

    const int32_t r = (int32_t)byteEval(expression, t, aux);
    // Upstream: `last = ((result - 128) * gain) >> 4`, gain 0..31, so gain 16 is
    // unity and the top of the dial overdrives by ~1.9x. Upstream let that wrap
    // in Mozzi's output stage; here it clamps, which on a signal this square
    // reads as drive rather than as the crackle a wrap would give.
    int32_t s = (r - 128) * (int32_t)gainStep;
    s >>= 4;
    held = clampf((float)s / 128.0f, -1.0f, 1.0f);
  }

  // Advance by `dt` seconds, running the bytebeat at its own rate and holding
  // the last value in between (zero-order hold — the same staircase the 16 kHz
  // original fed its PWM, so no extra brightness or aliasing is introduced).
  ByteFrame process(float dt) {
    phase += dt * byteRateHz(rateIndex);
    int steps = (int)phase;
    phase -= (float)steps;
    if (steps > 64) steps = 64;  // guard: a pathological dt must not stall here
    while (steps-- > 0) tick();

    ByteFrame f;
    f.audio = held;
    f.env = held < 0.0f ? -held : held;
    return f;
  }
};

}  // namespace sc
