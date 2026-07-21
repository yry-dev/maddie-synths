# MOD2 Frequency Shifter — Bode-style spectral shift

> **Status: implemented.** Tier 3, roadmap v2/v3, CPU: medium.
> Firmware (`mod2-freq-shifter.ino`) + shared `sc::FreqShifterCore` +
> VCV Rack port (`rack-plugins/src/mod2-freq-shifter.cpp`) are all in place.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

Very Buchla: shifts every partial by a fixed Hz amount (not a ratio), turning
harmonic sources inharmonic — barberpole phasing at small shifts, clangor at
large ones. Implemented as a Hilbert-transform single-sideband modulator.

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Shift amount (±1 kHz, centre-detented zero, exponential-ish taper near 0) |
| POT2 (A1) | Feedback (shifted output back into input → barberpole spirals) |
| BUTTON + POT1 | Wet/dry mix |
| BUTTON + POT2 | Up/down sideband blend (both = ring-mod-like) |
| CV (A2) | Audio input |
| IN1 | Shift direction flip gate |
| IN2 | Bypass gate |
| BUTTON short | Range: ±20 Hz (barberpole) / ±200 Hz / ±1 kHz |
| LED | Rotation rate ∝ shift amount |

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/FreqShifterCore.h`:
   - Hilbert pair via two parallel 6th-order allpass chains (90° phase-difference
     network, standard polyphase coefficients) — cheap and proven.
   - Quadrature oscillator at |shift| Hz; SSB output = I·cos − Q·sin (up) or
     I·cos + Q·sin (down).
2. Sub-Hz precision near zero for slow barberpole — 64-bit phase accumulator.
3. Feedback path with soft limiter + DC block (spiraling feedback is the magic).
4. Validate the allpass network coefficients in the Rack port first (easy to
   A/B against Rack's own Bode shifters).

**Budgets:** ~12 biquad-ish allpasses + one oscillator ≈ well within CPU;
no meaningful RAM.

## Open questions

- Allpass-network accuracy below ~50 Hz (image leakage) — acceptable, or add a
  gentle input HP to hide it?
