# MOD2 Reverse Delay — classic ambient reverse echo

> **Status: planned — no code yet.** Tier 4 (roadmap v3 "Ambient"), CPU: medium.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

Repeats play backwards: swelling, pre-echo ambient trails. Kept separate from
`mod2-delay` because reverse playback needs its own chunked read architecture
and deserves its own character controls.

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Chunk time (100 ms – 2 s; clock divisions when IN1 clocked) |
| POT2 (A1) | Feedback (re-reversing feedback — even repeats come back forward-ish) |
| BUTTON + POT1 | Wet/dry mix |
| BUTTON + POT2 | Envelope shaping: fade-in amount per reversed chunk (swell ↔ hard) |
| CV (A2) | Audio input |
| IN1 | Clock / tap |
| IN2 | Direction flip gate (momentary forward = "normal delay" pockets) |
| BUTTON short | Mode: Reverse / Alternating (fwd, rev, fwd…) / Octave-up reverse (2× read) |
| BUTTON long | Tap tempo |
| LED | Ramps up during each reverse sweep (visualizes the swell) |

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/ReverseDelayCore.h`: circular buffer
   (~300 KB ≈ 4 s). Write head runs forward; read plays each chunk backwards:
   while chunk N records, chunk N−1 plays reversed (double-buffered chunks).
2. Chunk-edge crossfades (~10 ms) + per-chunk fade-in envelope (POT2 shift) —
   reverse delays live or die on de-clicking.
3. Feedback: reversed output mixed back into the write path through LP + limiter.
4. Octave-up reverse mode: read at 2× while reversed = the "shimmer-lite" trick.
5. Shares buffer machinery with `DelayFxCore`/`GlitchDelayCore`; the glitch
   delay's reverse-chunk event is basically this effect's core loop — build them
   together.

**Budgets:** RAM ~300 KB; CPU light.

## Open questions

- Whether alternating mode should be its own or fold into `mod2-glitch-delay`'s
  palette once both exist (dedupe later, plan both for now).
