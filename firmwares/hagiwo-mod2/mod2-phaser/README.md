# MOD2 Phaser — 4/6/8-stage allpass phaser

> **Status: implemented** (shared `sc::PhaserCore` + VCV Rack port). Tier 2, roadmap v2, CPU: medium.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

Cascaded first-order allpass stages swept by an LFO. Especially good after APC.

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Rate (0.02 – 8 Hz); full CCW = manual sweep via POT2 |
| POT2 (A1) | Feedback / resonance (notch depth → vowely peaks) |
| BUTTON + POT1 | Wet/dry mix (50% = classic phaser null) |
| BUTTON + POT2 | Sweep depth / centre |
| CV (A2) | Audio input |
| IN1 | LFO retrigger / clock sync |
| IN2 | Bypass gate |
| BUTTON short | Stages: 4 / 6 / 8 |
| LED | Follows LFO |

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/PhaserCore.h`: chain of first-order
   allpass sections `y = a·x + x1 - a·y1`, shared sweep coefficient, global
   feedback from last stage to input.
2. Sweep the allpass corner exponentially ~100 Hz – 4 kHz; slight per-stage
   detune (stage n corner × small offset) for a lusher notch spread.
3. Coefficient updates at control rate (every 32 samples) with linear interp —
   allpass coeffs are cheap but exp() isn't; use a small lookup table.
4. Feedback path DC-blocked and soft-clipped.

**Budgets:** negligible RAM; CPU easy-medium (8 stages ≈ 8 multiplies/sample).

## Open questions

- Whether an envelope-follower sweep mode is worth adding (auto-wah territory) —
  probably yes, it's nearly free once the follower exists in `mod2-flanger`.
