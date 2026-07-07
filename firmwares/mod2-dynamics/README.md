# MOD2 Dynamics — one-knob compressor + limiter

> **Status: planned — no code yet.** Tier 5 (utility), CPU: easy.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.
> Covers both the "Compressor" and "Limiter" brainstorm items as modes.

Often-overlooked utility: a simple one-knob compressor to glue/fatten, and a
brickwall-ish limiter to protect the output. Especially useful in front of the
10-bit PWM output, where headroom is precious — squashing *before* quantization
audibly improves the module's perceived quality.

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Amount (one knob: threshold+ratio+makeup swept together, à la "smash") |
| POT2 (A1) | Release time (30 ms – 1 s); attack auto-scaled |
| BUTTON + POT1 | Dry blend (parallel / NY compression) |
| BUTTON + POT2 | Output trim |
| CV (A2) | Audio input |
| IN1 | Sidechain trigger — gate ducks the audio (envelope duck, pump without a second audio in) |
| IN2 | Bypass gate |
| BUTTON short | Mode: Compressor / Limiter / Ducker (IN1-driven) |
| LED | Gain-reduction meter (brighter = more GR) — genuinely useful |

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/DynamicsCore.h`: peak/RMS-blend
   envelope follower → gain computer (soft-knee, ratio from Amount) → smoothed
   gain with separate attack/release one-poles → makeup gain.
2. Limiter mode: high ratio, fast attack with ~1 ms lookahead (tiny delay
   buffer) so it truly catches peaks before the PWM clips.
3. Ducker mode: IN1 gate fires a decaying envelope that attenuates the audio —
   sidechain pumping in a mono system, great with the kick firmware's trigger
   mult'd in.
4. All gain math in dB-linear hybrid (lookup for log/exp at control rate).

**Budgets:** trivial; ~40 samples of lookahead RAM.

## Open questions

- One-knob curve tuning (where threshold/ratio/makeup land along the sweep) —
  tune against APC and the drum firmwares.
