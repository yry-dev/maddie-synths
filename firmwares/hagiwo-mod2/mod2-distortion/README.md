# MOD2 Distortion — multi-algorithm drive

> **Status: implemented** (shared `sc::DistortionCore` + VCV Rack port). Tier 1 (must-have), roadmap v1, CPU: easy.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

Five drive flavours behind one button. Amazing with APC, the drum firmwares,
and Braids.

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Drive amount (input gain into the shaper, auto-compensated output level) |
| POT2 (A1) | Tone (post-shaper tilt: dark ← → bright) |
| BUTTON + POT1 | Wet/dry mix (parallel drive!) |
| CV (A2) | Audio input |
| IN1 | (spare) |
| IN2 | Bypass gate |
| BUTTON short | Algorithm: Soft / Hard / Tube / Foldback / Fuzz |
| LED | Algorithm blink code; brightness follows output level |

## Algorithms

- **Soft saturation** — tanh, gentle knee.
- **Hard clip** — straight clip with slight pre-emphasis.
- **Tube** — asymmetric transfer (even harmonics), mild bias shift with level.
- **Foldback** — reflect-at-threshold wavefolding-style clip.
- **Fuzz** — heavy asymmetric gain + gate-y low-level cutoff.

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/DistortionCore.h`: per-algorithm
   waveshaper functions, shared gain-staging and tone stage.
2. **Anti-aliasing matters more than the shapers**: 2× oversample the shaper
   (upsample → shape → halfband down). Cheap at this CPU tier; without it,
   hard clip/foldback alias badly at 36.6 kHz.
3. Auto output-level compensation so switching algorithms doesn't jump volume.
4. Tone stage: one-pole tilt EQ post-shaper.
5. DC blocker after asymmetric shapers (tube/fuzz).

**Budgets:** trivial RAM; CPU easy even with 2× oversampling.

## Open questions

- Whether Fuzz needs an input envelope follower for the "dying battery" gate feel.
