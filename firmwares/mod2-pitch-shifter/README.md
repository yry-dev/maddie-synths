# MOD2 Pitch Shifter — octave & granular shift

> **Status: planned — no code yet.** Tier 3, roadmap v4, CPU: **hard**.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

Real-time pitch shift of the input: simple ±1 octave for instant sub/sparkle,
or free granular shift for detune and micro-shimmer.

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Pitch (±12 st, semitone-detented; fine ±50 cents on shift layer) |
| POT2 (A1) | Character: grain size (10 – 100 ms) / smear |
| BUTTON + POT1 | Wet/dry mix |
| BUTTON + POT2 | Shifted-signal feedback (micro-shimmer cascades) |
| CV (A2) | Audio input |
| IN1 | (spare — possible pitch S&H trigger) |
| IN2 | Octave-down latch gate |
| BUTTON short | Mode: Octave-up / Octave-down / Free (POT1 continuous) / Detune (±20 cents dual-voice) |
| LED | Colourless hardware — blink rate encodes shift direction/size |

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/PitchShiftCore.h`: classic
   2-tap overlap-add granular shifter on a ~100 ms circular buffer —
   two read heads at rate `2^(semis/12)`, 50%-overlap Hann (or equal-power
   triangle) crossfade, tap resets phase-staggered.
2. Transient artifact control: keep grains short for drums, long for pads —
   that's exactly what POT2 exposes; no hidden magic.
3. Detune mode: two shifters at ±cents, summed — the "always sounds good" chorus
   alternative.
4. Feedback path through the shifter (with LP + limiter) gives shimmer-lite;
   this core is also the prerequisite for a future shimmer mode in `mod2-reverb`.
5. Fixed-point read-head phase (64-bit) to avoid drift; linear interp first,
   cubic if grain edges are audible.

**Budgets:** ~15 KB buffer; CPU: 2–4 read heads + windows ≈ medium-hard but
comfortably under the reverb tier. "Hard" rating is mostly about making it
*sound* good, not raw cycles.

## Open questions

- Windowed-sinc vs linear interpolation quality tradeoff at ±12 st.
- Whether Free mode should quantize to semitones by default (probably yes).
