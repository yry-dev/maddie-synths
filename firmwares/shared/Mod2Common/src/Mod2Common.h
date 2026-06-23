#pragma once

#include <Arduino.h>
#include <math.h>
#include "hardware/pwm.h"
#include "hardware/irq.h"

// Shared helpers for MOD2 firmwares (Seeed Xiao RP2350).
//
// These cover the parts that are identical across the family of sketches that
// use the ~36.6 kHz dual-slice PWM audio path: the panel pin map, the PWM /
// IRQ setup, and a handful of DSP and control building blocks. Synthesis
// engines stay in each sketch; only the shared scaffolding lives here.
namespace mod2 {

// --------------------------------------------------
// MOD2 panel pin map (Seeed Xiao RP2350)
// --------------------------------------------------
// Note: a `*_PIN` suffix is used (not a `PIN_*` prefix) because the RP2350
// core already defines a `PIN_LED` macro that would otherwise collide.
constexpr uint8_t POT1_PIN = A0;
constexpr uint8_t POT2_PIN = A1;
constexpr uint8_t POT3_PIN = A2;   // shared with CV input
constexpr uint8_t CV_PIN = A2;     // CV in (shared with POT3)
constexpr uint8_t IN1_PIN = 7;     // gate / trigger / clock in
constexpr uint8_t IN2_PIN = 0;     // gate / accent in (or pulse out)
constexpr uint8_t OUT_PIN = 1;     // PWM audio output
constexpr uint8_t LED_PIN = 5;
constexpr uint8_t BUTTON_PIN = 6;

// Helper PWM slice pin used only to generate the audio-rate wrap IRQ.
constexpr uint8_t PWM_TIMER_PIN = 2;

// --------------------------------------------------
// Audio engine constants (dual-slice PWM scheme)
// --------------------------------------------------
constexpr float SYS_CLOCK = 150000000.0f;     // RP2350 system clock (Hz)
constexpr uint16_t PWM_AUDIO_WRAP = 1023;     // 10-bit audio resolution
constexpr uint16_t PWM_TIMER_WRAP = 4095;     // /4096 -> ~36.6 kHz wrap IRQ
constexpr float AUDIO_FS = SYS_CLOCK / (PWM_TIMER_WRAP + 1.0f);  // ~36621 Hz
constexpr float PWM_FS = PWM_AUDIO_WRAP;       // 10-bit full-scale value
constexpr float PWM_MID = PWM_AUDIO_WRAP / 2.0f;  // mid-scale (silence)

// Configure the standard MOD2 audio path:
//  - GPIO1 (PIN_OUT) as a 10-bit PWM audio output, and
//  - GPIO2 (PIN_PWM_TIMER) as a free-running slice whose wrap fires
//    PWM_IRQ_WRAP at ~36.6 kHz, with `handler` installed and enabled.
// Returns the two slice numbers via the out-parameters.
void initAudioPwm(uint &audioSlice, uint &timerSlice, irq_handler_t handler);

// Configure `pin` as a 10-bit PWM output (e.g. LED brightness) and return its
// slice number. The level is driven on PWM_CHAN_B (odd GPIOs such as 5).
uint initPwmOutput10bit(uint8_t pin);

// --------------------------------------------------
// Envelope / waveshaping helpers
// --------------------------------------------------
// Per-sample multiplier for an exponential decay reaching exp(-decayRate)
// over `samples` steps. Compute once, then multiply the running level by it.
inline float expDecayCoef(float decayRate, float samples) {
  return expf(-decayRate / samples);
}

// Raised-cosine ramp, 0..1 as `mu` goes 0..1. Use for smooth (anti-click)
// fades: out = (1 - r) * value + r * target, with r = raisedCosine(mu).
inline float raisedCosine(float mu) {
  return (1.0f - cosf(mu * PI)) * 0.5f;
}

// tanh soft-clip normalised so the output still reaches +/-1 at the input
// extremes. `rate` controls clip hardness (larger = harder).
inline float softClipTanh(float x, float rate) {
  return tanhf(rate * x) / tanhf(rate);
}

// --------------------------------------------------
// Fixed-point sample playback helpers (Q12)
// --------------------------------------------------
constexpr uint32_t FRAC_BITS = 12;
constexpr uint32_t FRAC_ONE = 1UL << FRAC_BITS;
constexpr uint32_t FRAC_MASK = FRAC_ONE - 1;

// Read a little-endian 16-bit PCM sample at sample index `idx` from a byte
// array (e.g. a sample table stored in flash).
inline int16_t readPCM16LE(const volatile uint8_t *base, uint32_t idx) {
  uint32_t byteIndex = idx << 1;  // idx * 2
  return (int16_t)(base[byteIndex] | (base[byteIndex + 1] << 8));
}

// Linear interpolation between two 16-bit samples using a Q12 fraction.
inline int16_t lerpFixed(int16_t s1, int16_t s2, uint32_t frac) {
  return (int16_t)(((int32_t)s1 * (int32_t)(FRAC_ONE - frac) +
                    (int32_t)s2 * (int32_t)frac) >>
                   FRAC_BITS);
}

// --------------------------------------------------
// 2-pole biquad band-pass (RBJ cookbook, constant skirt, peak gain = Q)
// --------------------------------------------------
struct Biquad {
  float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  float x1 = 0, x2 = 0, y1 = 0, y2 = 0;

  void reset() { x1 = x2 = y1 = y2 = 0; }

  // Set band-pass coefficients for centre frequency `fc` at sample rate `fs`.
  void setBandpass(float fc, float q, float fs);

  // Process one input sample and advance the filter state.
  inline float process(float x0) {
    float y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x0;
    y2 = y1;
    y1 = y0;
    return y0;
  }
};

// --------------------------------------------------
// Output-stage helpers (shared by the wavetable-synth sketches)
// --------------------------------------------------
// Cubic soft-saturator / smooth limiter. Output saturates to +/-1.
inline float softSat(float x) {
  if (x > 3.0f) return 1.0f;
  if (x < -3.0f) return -1.0f;
  float x2 = x * x;
  return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// One-pole DC-blocking high-pass.
struct DcBlocker {
  float x1 = 0, y1 = 0;
  float alpha = 0.998f;
  inline float process(float in) {
    float out = in - x1 + alpha * y1;
    x1 = in;
    y1 = out;
    return out;
  }
};

// Fixed 2-pole low-pass used to smooth the PWM output before the DAC.
struct OutputLpBiquad {
  float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  inline float process(float in) {
    constexpr float B0 = 0.1518f, B1 = 0.3036f, B2 = 0.1518f;
    constexpr float A1 = -0.5765f, A2 = 0.1838f;
    float out = B0 * in + B1 * x1 + B2 * x2 - A1 * y1 - A2 * y2;
    x2 = x1;
    x1 = in;
    y2 = y1;
    y1 = out;
    return out;
  }
};

// --------------------------------------------------
// Noise helpers
// --------------------------------------------------
// Fill `buf` with `n` samples of white noise in the range -1..+1 (uses rand()).
void fillWhiteNoise(float *buf, uint32_t n);

// xorshift32 PRNG. Seed `state` with any non-zero value.
inline uint32_t xorshift32(uint32_t &state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

// --------------------------------------------------
// Smoothed potentiometer reader (circular-buffer average)
// --------------------------------------------------
template <int N>
class PotSmoother {
 public:
  // Pre-fill the averaging window with the current pin value.
  void prime(uint8_t pin) {
    float v = analogRead(pin) / 1023.0f;
    for (int i = 0; i < N; i++) buf_[i] = v;
  }

  // Read the pin, push into the window, and return the window average (0..1).
  float read(uint8_t pin) {
    buf_[idx_] = analogRead(pin) / 1023.0f;
    idx_ = (idx_ + 1) % N;
    float sum = 0;
    for (int i = 0; i < N; i++) sum += buf_[i];
    return sum / N;
  }

 private:
  float buf_[N] = {0};
  int idx_ = 0;
};

// --------------------------------------------------
// "Pickup" parameter: avoids value jumps when a pot is reassigned to a new
// parameter, by waiting until the pot crosses its stored normalised position.
// --------------------------------------------------
struct PickupParam {
  float value = 0;          // current parameter value (engine units)
  float targetValue = 0;    // normalised (0..1) target to pick up
  bool pickupActive = false;
  float lastPotValue = 0;   // last raw pot reading (0..1)
};

// Returns true once the pot (currentPotValue, 0..1) has "caught" the stored
// target and the parameter may be updated; false while still waiting.
bool checkPickup(PickupParam &param, float currentPotValue,
                 float threshold = 0.02f);

}  // namespace mod2
