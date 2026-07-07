# MOD2 Chorus — Juno-style chorus + ensemble

> **Status: implemented** (shared `sc::ChorusCore` + VCV Rack port). Tier 2, roadmap v1, CPU: medium.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.
> Covers both the "Chorus" and "Ensemble" brainstorm items as modes.

Classic modulated short-delay thickening. Fantastic for pads and Plaits.
Mono-in, mono-out (the Juno's stereo spread is faked by summing two
anti-phase-modulated voices — still lush in mono).

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Rate (0.1 – 8 Hz) |
| POT2 (A1) | Depth (modulation excursion) |
| BUTTON + POT1 | Wet/dry mix |
| CV (A2) | Audio input |
| IN1 | (spare) |
| IN2 | Bypass gate |
| BUTTON short | Mode: Chorus I / Chorus II / Ensemble |
| LED | Breathes at LFO rate |

## Modes

- **Chorus I / II** — Juno-style: 2 delay taps (~1–5 ms base) modulated by one
  triangle LFO in anti-phase; II is faster/deeper.
- **Ensemble** — string-machine style: 3 taps modulated by 3 phase-offset LFOs,
  each LFO a sum of slow + fast components (the classic 0.6 Hz + 6 Hz recipe).

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/ChorusCore.h`: one short delay
   buffer (~15 ms, tiny), N fractional read taps with linear interp, LFO bank.
2. Triangle LFOs with slight sine-shaping; ensemble mode uses 3× (slow+fast) pairs.
3. Interpolation quality matters here (moving taps) — use linear first, upgrade
   to cubic if zipper/graininess is audible.
4. Gentle high-pass in the wet path to keep low end mono-solid.

**Budgets:** ~1 KB delay RAM; CPU light (a few taps + LFOs).

## Open questions

- Whether Chorus II should add feedback for a flanger-adjacent flavour, or keep
  that strictly in `mod2-flanger`.
