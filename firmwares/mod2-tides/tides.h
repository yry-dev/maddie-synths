// License:
// GPLv3, (c) 2025 — see LICENSE.md in this directory. Part of the Mutable
// Instruments Tides port (based on mi_Ugens by Volker Boehm); the upstream MI
// sources are MIT. GPLv3 carries conditions the CC0 modules in this repo do
// not: the notice and source-availability terms travel with every copy.

// tides.h - Tides DSP engine for MOD2
// Based on Mutable Instruments Tides2 PolySlopeGenerator

#pragma once

#include <STMLIB.h>
#include <TIDES.h>

// Block size for audio processing
#define BLOCK_SIZE 16

// Mode names for debug output
const char* outputModeNames[] = {"GATES", "AMPLITUDES", "PHASES", "FREQUENCIES"};
const char* rampModeNames[] = {"AD", "LOOPING", "AR"};

// DSP parameters (directly set from controls)
float freq_in = 440.0f;      // Frequency in Hz (default A4)
float shape_in = 0.5f;       // Shape parameter 0-1
float slope_in = 0.5f;       // Slope parameter 0-1
float smooth_in = 0.5f;      // Smoothness parameter 0-1
float trigger_in = 0.0f;     // Trigger input 0-1 (gate for envelope modes)
int output_mode_in = 1;      // Output mode: 0=GATES, 1=AMPLITUDES, 2=PHASES, 3=FREQUENCIES
int ramp_mode_in = 1;        // Ramp mode: 0=AD, 1=LOOPING, 2=AR
int range_in = 1;            // Range: 0=CONTROL (slow LFO), 1=AUDIO (audible)

// Trigger edge detection (like braids)
bool last_trigger = false;

// Smoothed parameters for zipper-free control
float freq_smooth = 440.0f;
float shape_smooth = 0.5f;
float slope_smooth = 0.5f;
float smooth_smooth = 0.5f;

// Smoothing coefficient (lower = smoother, more latency)
const float SMOOTH_COEFF = 0.02f;

// Gate state tracking
stmlib::GateFlags previous_gate_flags = stmlib::GATE_FLAG_LOW;

// Shift/level value handed to Render() for each output mode. The hardware has
// a single jack, so each mode "emulates" Tides' four outputs by tapping or
// mixing the channels (see the conversion loop below); shift is fixed per mode
// to make that tap/mix sound right. Internally Render() maps shift 0..1 to
// -1..+1, and each mode uses it differently:
//   GATES       shift only scales channel 0, which this mode doesn't tap ->
//               1.0 (harmless).
//   AMPLITUDES  channels crossfade on an index of |internal shift| * 5.1;
//               channel 0 peaks when that index is 1.0, i.e. shift ~= 0.598
//               (0.5 mutes channel 0 entirely!).
//   PHASES      shift spreads the channel phases by internal/3 each; 0.875
//               -> quarter-phase offsets (0, ¼, ½, ¾), the widest stack.
//   FREQUENCIES shift picks the frequency-ratio table row (index =
//               round(shift * 20)); 0.7 -> row 14, the C-E-G-C chord.
inline float shiftForOutputMode(int mode) {
  switch (mode) {
    case 0:  return 1.0f;    // GATES
    case 1:  return 0.598f;  // AMPLITUDES
    case 2:  return 0.875f;  // PHASES
    default: return 0.7f;    // FREQUENCIES
  }
}

// Voice structure
struct Voice {
  tides::PolySlopeGenerator poly_slope_generator;
  int16_t buffer[BLOCK_SIZE];  // 16-bit signed for PWMAudio
};

Voice voices[1];

// Initialize voices
void initVoices() {
  voices[0].poly_slope_generator.Init();

  // Clear buffer
  for (int i = 0; i < BLOCK_SIZE; i++) {
    voices[0].buffer[i] = 0;
  }
}

// Update Tides audio - called from main loop when buffer needs refill
void updateTidesAudio() {
  // Smooth parameters to avoid zipper noise
  freq_smooth += SMOOTH_COEFF * (freq_in - freq_smooth);
  shape_smooth += SMOOTH_COEFF * (shape_in - shape_smooth);
  slope_smooth += SMOOTH_COEFF * (slope_in - slope_smooth);
  smooth_smooth += SMOOTH_COEFF * (smooth_in - smooth_smooth);

  // Output buffer
  tides::PolySlopeGenerator::OutputSample out[BLOCK_SIZE];

  // Convert frequency to normalized form (cycles per sample)
  // Tides expects frequency in range 0.0 to 0.25 max
  float normalized_freq = freq_smooth / (float)SAMPLERATE;

  // Clamp to valid range
  if (normalized_freq < 0.0001f) normalized_freq = 0.0001f;
  if (normalized_freq > 0.25f) normalized_freq = 0.25f;

  // Build gate flags array
  stmlib::GateFlags gate_flags[BLOCK_SIZE];
  bool gate_high = (trigger_in > 0.5f);

  for (int i = 0; i < BLOCK_SIZE; i++) {
    gate_flags[i] = stmlib::ExtractGateFlags(previous_gate_flags, gate_high);
    previous_gate_flags = gate_flags[i];
  }

  // Render the poly slope generator. Shift is fixed per output mode so the
  // single OUT1 channel stays at full level (see shiftForOutputMode above).
  voices[0].poly_slope_generator.Render(
    static_cast<tides::RampMode>(ramp_mode_in),
    static_cast<tides::OutputMode>(output_mode_in),
    static_cast<tides::Range>(range_in),
    normalized_freq,
    slope_smooth,        // pw / slope parameter
    shape_smooth,        // shape parameter
    smooth_smooth,       // smoothness parameter
    shiftForOutputMode(output_mode_in),
    gate_flags,
    nullptr,             // no external ramp input
    out,
    BLOCK_SIZE
  );

  // Convert to 16-bit signed for PWMAudio. Channels are normalized floats
  // (roughly -1..+1 after folding), so full scale is *32768 with a saturating
  // clip, as in poetaster's TidesEngineScarp reference.
  //
  // Single-jack multi-output emulation: real Tides puts these four channels on
  // four jacks, where the output modes sound completely different. On OUT1
  // alone channel 0 is nearly identical in every mode, so each mode instead
  // taps or mixes the channels that make it distinct:
  //   GATES       channel 1: the un-shaped ramp (bright raw saw when looping
  //               at audio rate; the gate wave in AD/AR).
  //   AMPLITUDES  channel 0: the classic shaped slope.
  //   PHASES      all four quarter-phase copies mixed -> comb/chorus thickening.
  //   FREQUENCIES all four ratio'd channels mixed -> a C-E-G-C chord stack.
  // Mix gains keep summed peaks near ±1; rare overshoots hit the clip.
  for (int i = 0; i < BLOCK_SIZE; i++) {
    float s;
    switch (output_mode_in) {
      case 0:   // GATES: raw ramp
        s = out[i].channel[1];
        break;
      case 2:   // PHASES: 4-phase stack
        s = 0.4f * (out[i].channel[0] + out[i].channel[1] +
                    out[i].channel[2] + out[i].channel[3]);
        break;
      case 3:   // FREQUENCIES: chord mix
        s = 0.3f * (out[i].channel[0] + out[i].channel[1] +
                    out[i].channel[2] + out[i].channel[3]);
        break;
      default:  // AMPLITUDES: classic slope
        s = out[i].channel[0];
        break;
    }
    voices[0].buffer[i] = stmlib::Clip16(static_cast<int32_t>(s * 32768.0f));
  }
}
