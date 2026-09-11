# MOD2 FX — multi-algorithm effects platform

> **Status: implemented (v1.0).** `mod2-fx.ino` hosts all 16 algorithms behind
> `sc::FxPlatformCore` (`firmwares/shared/SynthCore/src/FxPlatformCore.h`), shared
> with the VCV Rack port `rack-plugins/src/mod2-fx.cpp`. This folder also holds the
> shared hardware notes that all the individual `mod2-*` FX plans reference.

One firmware, many effects: the MOD2 answer to an FX Aid / Pico DSP. The button cycles
algorithms, the LED identifies the current one by blink pattern, and every algorithm
uses the identical control mapping so each effect is immediately familiar.

Planned algorithm set (each also has its own standalone-firmware plan folder):
delay, tape echo, reverb (hall/plate), chorus/ensemble, flanger, phaser, bitcrusher,
distortion, wavefolder, resonator, karplus, comb, ring mod, pitch shift, freeze,
glitch delay.

## Shared hardware notes (read this first — applies to all mod2 FX plans)

MOD2 (Seeed Xiao RP2350) has **no dedicated audio input**. I/O is: 3 pots (A0/A1/A2),
2 digital gate ins (GPIO7/GPIO0), 1 CV in on A2 **shared with POT3**, button, LED, and
one 10-bit dual-slice PWM audio out at ~36.6 kHz.

**Audio input plan:** sample the CV jack (A2) with the RP2350 ADC at the audio rate
(~36.6 kHz, synced to the PWM interrupt). Consequences:

- **POT3 is unavailable** as a knob while audio is patched (same ADC node). All FX
  control maps therefore use only POT1 + POT2 live, plus a shift layer.
- **Wet/dry convention:** hold BUTTON + turn POT1 = wet/dry mix (shift layer), value
  persisted to flash. Short press = algorithm/mode change, long press = per-effect
  action (tap, freeze, etc.). Keep this identical across every FX firmware.
- **Open hardware question (blocking):** the CV front end was designed for slow CV —
  verify its input bias, AC behaviour, bandwidth and headroom for ±5 V audio before
  committing. May need a small mod or an adapter (see `hardware/2020-adapter/`).
  Signal will be 12-bit unipolar at the ADC; remove DC with a one-pole high-pass in
  software and dither/noise-shape back out to the 10-bit PWM.

**Budgets** (apply to every plan): RP2350 @ 150 MHz, single-precision FPU, ~520 KB SRAM.
At 36.6 kHz / 16-bit mono, usable delay memory after code/stack is roughly 400 KB ≈ 5.5 s.
Per-sample budget ≈ 4 000 cycles — comfortable for everything except reverb/granular/
spectral, which need block processing and careful memory layout (see those plans).

## Control mapping (identical for every algorithm)

| Control | Function |
|---|---|
| POT1 (A0) | Main parameter (time / rate / amount) |
| POT2 (A1) | Character (feedback / depth / tone) |
| BUTTON + POT1 | Wet/dry mix (shift layer, saved) |
| CV jack (A2) | **Audio input** |
| IN1 (GPIO7) | Clock / tap / trigger (per algorithm) |
| IN2 (GPIO0) | Gate action: freeze / reverse / bypass (per algorithm) |
| BUTTON short | Next algorithm |
| BUTTON long | Per-algorithm action |
| LED | Algorithm ID blink code; activity while running |

## Platform features (beyond individual effects)

- Preset storage: 8–16 slots in flash (algorithm + parameters), double-buffered writes.
- Smooth parameter interpolation everywhere (one-pole smoothing) — no zipper noise.
- Clock sync on IN1 for all time-based effects (delay, tremolo, glitch, stutter).
- All-pass through the MIDI-to-CV ecosystem: MIDI clock arrives as analog clock on IN1.

## Implementation plan

1. Build the individual effects first as standalone firmwares (see the per-effect
   folders); each puts its DSP in `firmwares/shared/SynthCore/src/<Name>Core.h`
   (pure C++, no Arduino deps) so it is reusable here **and** in VCV Rack ports.
2. Define a tiny common interface (`process(in) -> out`, `setParam(i, v)`, `trigger()`)
   so cores are swappable behind the button.
3. mod2-fx itself is then mostly glue: algorithm table, LED blink codes, preset
   storage, shared ADC/PWM plumbing from `Mod2Common`.
4. Flash-size check per algorithm added; drop to a curated set if RAM contends
   (delay-line memory cannot be shared between simultaneous algorithms, but only one
   runs at a time — reuse one big arena buffer).

## Roadmap (from the brainstorm)

- v1 Essentials: delay, distortion, bitcrusher, chorus, resonator
- v2 Performance: tape echo, flanger, phaser, ring mod, wavefolder
- v3 Ambient: hall/plate reverb, reverse delay, freeze, glitch delay
- v4 Experimental: granular, pitch shift, spectral freeze, multi-tap delay
