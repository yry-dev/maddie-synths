# MOD2 Wavefolder — digital West-Coast folder

> **Status: implemented** — `sc::WavefolderCore` + `mod2-wavefolder.ino` +
> `rack-plugins/src/mod2-wavefolder.cpp`. Tier 3, roadmap v2, CPU: easy.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

Digital approximation of a serge/Buchla-style folder. Great after Plaits or any
simple waveform — turns sines/triangles into rich harmonic sweeps.

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Fold amount (input gain into the folder, 1× – ~20×) |
| POT2 (A1) | Symmetry / offset (adds DC bias pre-fold → even harmonics, timbral "tilt") |
| BUTTON + POT1 | Wet/dry mix |
| BUTTON + POT2 | Post-fold low-pass (tame the top) |
| CV (A2) | Audio input |
| IN1 | (spare) |
| IN2 | Bypass gate |
| BUTTON short | Fold curve: triangle-reflect / sine (Buchla 259-ish) / stage-cascade (Serge-ish) |
| LED | Brightness follows fold density |

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/WavefolderCore.h`: gain + offset →
   folding function → DC blocker → LP → level compensation.
2. Curves: (a) mirror-reflect at ±1 repeatedly; (b) `sin(g·x)` folder;
   (c) 4-stage cascaded soft folders with per-stage thresholds.
3. **Anti-aliasing is the real work**: folding is brutally nonlinear at 36.6 kHz.
   Use 4× oversampling around the folder (shared halfband code with
   `DistortionCore`) and/or ADAA (antiderivative anti-aliasing) for the
   reflect curve. Budget allows 4× for a single-function effect.
4. Auto output-level compensation vs fold amount.

**Budgets:** no RAM; CPU moderate with 4× OS but fine for a single effect.

## Open questions

- ADAA vs oversampling (or both) — decide by measuring aliasing on a swept sine
  in the VCV Rack port of the core first.
