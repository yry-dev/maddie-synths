# MOD2 Flanger — swept comb flanger

> **Status: implemented** (shared `sc::FlangerCore` + VCV Rack port). Tier 2, roadmap v2, CPU: medium.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

Short modulated delay with feedback: jet-swoosh through-zero-ish comb sweeps.

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Rate (0.02 – 5 Hz) — full CCW = manual mode (POT2 sweeps by hand) |
| POT2 (A1) | Feedback (bipolar: CCW negative, CW positive; centre = none) |
| BUTTON + POT1 | Wet/dry mix |
| BUTTON + POT2 | Depth / sweep range |
| CV (A2) | Audio input |
| IN1 | LFO retrigger / clock sync |
| IN2 | Bypass gate |
| BUTTON short | LFO shape: triangle / sine / envelope-follow |
| LED | Follows LFO position |

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/FlangerCore.h`: single fractional
   tap over a ~20 ms buffer (sweep 0.2 – 12 ms), feedback around the delay
   with a soft limiter, wet+dry sum for the comb.
2. Bipolar feedback (invert wet) gives the two classic flavours (peaks vs notches).
3. Exponential sweep mapping (perceived sweep is log in delay time).
4. Envelope-follow mode: sweep driven by input level instead of LFO — great on drums.
5. DC blocker in the feedback loop; clamp feedback below oscillation except the
   last 5% of the pot (let it scream deliberately).

**Budgets:** ~1.5 KB RAM, CPU light.

## Open questions

- True through-zero (needs a dry-path delay of half the sweep) — cheap, decide by ear.
