# MOD2 Comb — tuned comb filter

> **Status: planned — no code yet.** Tier 3, roadmap v2, CPU: easy.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

A dedicated, playable comb filter: feedforward + feedback comb with tuned delay.
Wonderful for metallic sounds and physical-modeling flavours; the simplest way
to make any source "ring". (The multi-comb *cluster* lives in `mod2-resonator`;
this is the single, precise, performable one.)

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Frequency / delay tune (20 Hz – 2 kHz equivalent, quantize option) |
| POT2 (A1) | Feedback (bipolar: CCW negative comb, CW positive; extremes ring) |
| BUTTON + POT1 | Wet/dry mix |
| BUTTON + POT2 | Damping (LP in the feedback path) |
| CV (A2) | Audio input |
| IN1 | (spare — possible tune S&H trigger) |
| IN2 | Feedback kill gate |
| BUTTON short | Mode: FB comb / FF comb / Both (nested) |
| LED | Follows resonant energy |

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/CombCore.h`: fractional delay
   (allpass-interpolated for accurate tuning), feedback path with one-pole
   damping and soft limiter, optional feedforward tap.
2. Positive vs negative feedback selects harmonic series vs odd-harmonics
   (hollow, square-ish) — that's why POT2 is bipolar.
3. Semitone quantization on POT1 by default; free-tune on the shift layer.
4. Soft-limit feedback so >100% settings self-oscillate musically instead of
   clipping — at full feedback this doubles as a sine-ish drone source.

**Budgets:** ~4 KB RAM, CPU trivial. Another good early build after tremolo.

## Open questions

- Whether to share the allpass-tuned delay-loop code with `KarplusCore`
  (they're 80% the same structure — probably one shared `TunedLoop` helper).
