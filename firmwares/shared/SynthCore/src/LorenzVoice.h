#pragma once

// Lorenz-attractor voice — the shared core of the Butterfly module.
//
// Used by:
//   - firmwares/mod1-butterfly/mod1-butterfly.ino  (one Euler step per loop)
//   - vcvrack/src/Butterfly.cpp                     (sub-stepped at audio rate)
//
// Pure C++: depends only on sc_math.h. No Arduino / Rack / Pico SDK.

#include "sc_math.h"

namespace sc {

// Lorenz parameters mapped from the three panel pots, plus the integration step
// size selected by POT1 (firmware's select3FromAdc on the same pot).
struct LorenzParams {
  float sigma;
  float rho;
  float beta;
  float dt;
};

// Map the normalised panel controls (0..1) to Lorenz parameters, exactly like
// mod1-butterfly: POT1 -> sigma (and step size), POT2 -> rho, POT3 -> beta.
// `slow` is the latched slow-mode toggle. Pass adc/1023 from firmware or the
// raw 0..1 knob value from VCV Rack — both produce identical results.
inline LorenzParams lorenzMapParams(float pot1, float pot2, float pot3, bool slow) {
  LorenzParams p;
  p.sigma = mapClampf(pot1, 0.0f, 1.0f, slow ? 1.0f : 5.0f, slow ? 10.0f : 20.0f);
  p.rho = mapClampf(pot2, 0.0f, 1.0f, 20.0f, 50.0f);
  p.beta = mapClampf(pot3, 0.0f, 1.0f, 1.0f, 4.0f);

  const uint8_t stepMode = select3(pot1);
  if (slow) {
    p.dt = (stepMode == 0) ? 0.0001f : (stepMode == 1) ? 0.0005f : 0.001f;
  } else {
    p.dt = (stepMode == 0) ? 0.001f : (stepMode == 1) ? 0.005f : 0.01f;
  }
  return p;
}

// The attractor state. x/y are naturally bipolar (~+/-30), z is unipolar
// (~0..50). reset() restores the firmware's power-on seed.
struct LorenzVoice {
  float x = 0.1f;
  float y = 0.0f;
  float z = 0.0f;

  void reset() {
    x = 0.1f;
    y = 0.0f;
    z = 0.0f;
  }

  // Advance the system by one Euler step of size `dt`. Self-heals to the seed
  // if extreme settings blow the state up to NaN/Inf.
  inline void step(float sigma, float rho, float beta, float dt) {
    const float dx = sigma * (y - x);
    const float dy = x * (rho - z) - y;
    const float dz = x * y - beta * z;
    x += dx * dt;
    y += dy * dt;
    z += dz * dt;
    if (!isFiniteF(x) || !isFiniteF(y) || !isFiniteF(z)) reset();
  }
};

}  // namespace sc
