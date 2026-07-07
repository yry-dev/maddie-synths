# MOD2 Resonator — Rings-like resonator bank

> **Status: implemented** (shared `sc::ResonatorCore` + VCV Rack port). Tier 3 (very modular), roadmap v1, CPU: medium.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

Expansion of the resonance half of `mod2-flux` into a dedicated audio-**processing**
resonator: external audio (or gate-triggered internal noise bursts) excites a bank
of tuned resonators — Rings-like modal, comb clusters, and sympathetic strings.
Effects commercial modules rarely include; very much "our lane".

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Pitch / fundamental (quantizable) |
| POT2 (A1) | Structure: partial spread / string detune spread |
| BUTTON + POT1 | Wet/dry mix |
| BUTTON + POT2 | Damping / decay time |
| CV (A2) | Audio input (excitation) |
| IN1 | Strike trigger — fires an internal noise-burst exciter (works with nothing patched) |
| IN2 | Damp gate (choke the bank while high) |
| BUTTON short | Mode: Modal / Comb cluster / Sympathetic |
| BUTTON long | Strike |
| LED | Follows bank energy |

## Modes

- **Modal** — 8–16 two-pole resonators at (in)harmonic partial ratios; structure
  morphs harmonic → bell-like stiff-string ratios.
- **Comb cluster** — 3–4 tuned feedback combs at chord-ish intervals.
- **Sympathetic** — one bright driven string + 4 quieter Karplus strings tuned to
  fifths/octaves that ring in sympathy (input feeds them at low gain).

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/ResonatorCore.h` — start by lifting
   the modal + Karplus code paths out of `FluxCore.h` and generalizing them to
   accept external excitation instead of only internal noise.
2. Modal filters as state-variable resonators (stable under sweeps); amplitude
   per mode rolls off with index, tweaked by structure.
3. CPU scaling knob: number of active modes adapts (16 modes ≈ 16 SVFs — fine;
   verify against the 4 000 cycles/sample budget with margin for combs mode).
4. Pitch quantization to semitones by default (it's a melodic module).
5. Rack port shares the core; Flux stays untouched.

**Budgets:** combs/strings need a few KB each — trivial. CPU is the constraint
in modal mode; profile early.

## Open questions

- Should V/oct tracking of POT1 via a future CV-expander matter here, or is
  pitch-by-knob enough for a 4HP FX?
