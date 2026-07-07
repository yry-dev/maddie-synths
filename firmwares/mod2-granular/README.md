# MOD2 Granular — granular delay / cloud textures

> **Status: planned — no code yet.** Tier 4 (experimental), roadmap v4, CPU: **hard**.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

Tiny grains scattered from a live-recorded buffer: Clouds-adjacent smears,
stutters and shimmering textures from any input. The flagship experimental FX.

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Grain size (10 – 250 ms) |
| POT2 (A1) | Texture macro: density + position-spray + pitch-jitter together |
| BUTTON + POT1 | Wet/dry mix |
| BUTTON + POT2 | Grain pitch (octaves/fifths quantized around unity) |
| CV (A2) | Audio input |
| IN1 | Grain trigger (external clock spawns grains → rhythmic granular) |
| IN2 | Freeze gate (stop recording; granulate the held buffer) |
| BUTTON short | Envelope/character: Smooth (Hann) / Perc (expodec) / Reverse grains |
| LED | Flickers per grain spawn — density is visible |

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/GranularCore.h`:
   - Record buffer ~350 KB (≈ 4.8 s), int16.
   - Fixed pool of **6–8 simultaneous grains**; each = read pos, rate, phase,
     window. Steal the oldest grain when the pool is full.
   - Windows from a shared 256-entry table (Hann / expodec / reversed).
2. Scheduler: internal density-driven spawning (stochastic) or IN1-triggered.
   Spray = uniform random offset around the nominal read position.
3. CPU plan: 8 grains × (interp read + window multiply) ≈ the real budget item;
   int16 reads with float mix keeps it feasible. Cap grain count dynamically if
   the ISR overruns (count spare cycles, degrade gracefully).
4. Pitch per grain from the quantized set {−12, −7, 0, +7, +12} + jitter — avoids
   sour continuous detune while staying lush.
5. Build **after** `mod2-freeze` (shares capture-buffer code) and
   `mod2-pitch-shifter` (shares windowed-read-head code).

**Budgets:** RAM near max; CPU the tightest after reverb — prototype grain count
in the Rack port first.

## Open questions

- 6 vs 8 grains on real hardware; whether density should auto-duck grain count
  at small grain sizes (overlap explosion).
