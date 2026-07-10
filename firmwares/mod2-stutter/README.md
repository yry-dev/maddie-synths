# MOD2 Stutter — beat repeat

> **Status: planned — no code yet.** Tier 4 (experimental), roadmap v3/v4, CPU: easy-medium.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

Clock-aware beat repeats: grab the last slice of audio and machine-gun it in
musical divisions. Built for drums — patch APC or the drum firmwares through it
with a clock into IN1 and it becomes a performance instrument.

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Repeat length: clocked divisions {1/32 … 1 bar} (unclocked: 20 ms – 1 s free) |
| POT2 (A1) | Behaviour: repeat count / decay per repeat / pitch-ramp amount |
| BUTTON + POT1 | Wet/dry mix |
| BUTTON + POT2 | Trigger probability (auto-stutter chance per division) |
| CV (A2) | Audio input |
| IN1 | Clock in (defines the musical grid) |
| IN2 | Stutter gate — repeats while high (the performance control) |
| BUTTON short | Flavour: Straight / Decaying / Pitch-ramp up / Pitch-ramp down |
| BUTTON long | Manual stutter latch |
| LED | Flashes per repeat |

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/StutterCore.h`: always-recording
   circular buffer (~90 KB ≈ 1.2 s covers a bar at 60 BPM in 4/4). On stutter
   start, lock the slice = last N samples; loop it with seam crossfades.
2. Clock follower (shared `ClockFollower` helper with delay/tremolo) keeps the
   grid; stutter onset quantizes to the next division boundary so button mashing
   still lands on time.
3. Pitch-ramp flavours: multiply read rate by k each repeat (k ≈ 1.06 / 0.94) —
   the classic riser/faller. Decaying: gain × 0.8 per repeat.
4. Probability mode: roll at each division while enabled — instant IDM.
5. Shares capture-loop machinery with `mod2-freeze`; build alongside it.

**Budgets:** modest RAM, trivial CPU.

## Open questions

- Should unstuttering resume live audio at the *current* time (dropout-free) or
  where the buffer left off (tape-machine feel)? Probably current time; test both.
