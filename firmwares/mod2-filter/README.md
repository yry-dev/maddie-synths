# MOD2 Filter — tilt EQ & cleanup filters

> **Status: implemented** (firmware + shared `sc::FilterCore` + VCV Rack port).
> Tier 5 (utility), CPU: easy.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.
> Covers the "EQ (tilt)" and "High-pass / Low-pass" brainstorm items as modes.

Simple corrective filtering — the unglamorous patch-saver: tame a harsh source,
clean rumble, tilt a mix element dark/bright. Also sneaks in a resonant SVF
mode because the hardware can, making it a bonus performance filter.

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Frequency (20 Hz – 16 kHz, exponential) / tilt centre |
| POT2 (A1) | Tilt amount (EQ mode, bipolar) or resonance (filter modes) |
| BUTTON + POT1 | Wet/dry mix (usually 100%, but parallel HP = quick "thickener") |
| BUTTON + POT2 | Output trim |
| CV (A2) | Audio input |
| IN1 | (spare — possible cutoff S&H trigger for steppy filter FX) |
| IN2 | Bypass gate |
| BUTTON short | Mode: Tilt EQ / Low-pass / High-pass / Band-pass |
| LED | Brightness follows output level |

## Modes

- **Tilt EQ** — pivot around POT1; POT2 CCW = darker, CW = brighter (±6 dB).
- **LP / HP** — 2-pole state-variable, POT2 = resonance (12 dB/oct; add a
  4-pole LP option if a ladder flavour is wanted later).
- **BP** — same SVF, band output.

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/FilterFxCore.h`: one Chamberlin/
   Cytomic-style SVF (stable to Nyquist with the trapezoidal form — use the
   Cytomic version) provides LP/HP/BP; tilt = low-shelf + high-shelf pair
   with opposite gains.
2. Coefficient lookup table over the exponential frequency sweep; interpolate
   at control rate.
3. Soft-clip the resonant output (self-oscillation at max res should be usable,
   not a crash).
4. Trivial project — a good "get the FX plumbing right" candidate along with
   tremolo and bitcrusher.

**Budgets:** negligible.

## Open questions

- Is a 4-pole/ladder LP mode worth it, or keep this strictly utility and leave
  character filters for a future dedicated firmware?
