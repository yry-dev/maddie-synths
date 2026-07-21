# MOD2 Glitch Delay — random skips, reverse chunks, tape cuts

> **Status: implemented.** Core `sc::GlitchDelayCore` + `mod2-glitch-delay.ino` +
> `rack-plugins/src/mod2-glitch-delay.cpp`. Tier 4 (experimental), roadmap v3, CPU: medium.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

A delay whose read head misbehaves on purpose: random skips, reversed chunks,
stutter re-reads, tape-cut splices. Fits the ecosystem's experimental streak —
this is the one that sounds like nothing else in a small system.

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Base delay time / chunk length (clock-divisions when IN1 clocked) |
| POT2 (A1) | Chaos amount: probability + intensity of glitch events (0 = clean delay) |
| BUTTON + POT1 | Wet/dry mix |
| BUTTON + POT2 | Feedback |
| CV (A2) | Audio input |
| IN1 | Clock — quantizes glitch events to musical time (recommended!) |
| IN2 | Force-glitch gate (guaranteed event while high) |
| BUTTON short | Glitch palette: Skips / Reverse / Stutter / All |
| BUTTON long | Tap tempo |
| LED | Flashes on each glitch event |

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/GlitchDelayCore.h`: circular buffer
   (~300 KB ≈ 4 s) + an **event scheduler**: at each chunk boundary (clock-
   quantized), roll against chaos to pick {normal, skip-to-random-offset,
   reverse-read, re-read-last-chunk, half/double-speed}.
2. Every event transition gets a 1–5 ms crossfade between read heads —
   two-head architecture (current + next) makes all glitch types click-free
   with one mechanism.
3. Weighted event tables per palette mode; chaos scales both probability and
   how far events deviate.
4. Deterministic-feel option: seed the RNG from the clock count so a given
   loop glitches repeatably (huge for live sets) — worth it, nearly free.
5. Shares buffer/read-head machinery with `DelayFxCore` — build after mod2-delay.

**Budgets:** RAM ~300 KB; CPU light (it's still just delay reads + fades).

## Open questions

- Best default event-weight table — tune by ear in the Rack port with a drum loop.
