# MOD2 Tremolo — amplitude modulation, clock-syncable

> **Status: implemented.** Firmware (`mod2-tremolo.ino`), shared core
> (`../shared/SynthCore/src/TremoloCore.h`) and VCV Rack port
> (`../../rack-plugins/src/mod2-tremolo.cpp`) are all in place. Tier 2, roadmap
> v2 (or earlier — easiest FX of all), CPU: easy. Hardware/audio-in caveats: see
> `../mod2-fx/README.md`.
> The "Auto Pan" brainstorm item folds in here: with one mono output, auto-pan
> degenerates to tremolo; revisit as true pan if stereo hardware ever exists.

Very easy to implement, very useful live: LFO-driven VCA with tap/clock sync.

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Rate (0.1 – 30 Hz), or clock division when IN1 is clocked |
| POT2 (A1) | Depth (0 – 100%, top end goes square-ish full chop) |
| BUTTON + POT1 | Wet/dry (here: depth floor / output trim) |
| CV (A2) | Audio input |
| IN1 | Clock in — rate locks to divisions {1/4…4×} of the clock |
| IN2 | LFO phase reset (sync chops to downbeats) |
| BUTTON short | LFO shape: sine / triangle / square (smoothed) / ramp down |
| BUTTON long | Tap tempo |
| LED | Follows the LFO — instant visual tempo check |

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/TremoloCore.h`: one LFO, one
   multiply. Smooth the square shape with a one-pole (~2 ms) to avoid clicks.
2. Clock sync: same tap/division logic as `mod2-delay` — share a small
   `ClockFollower` helper in SynthCore.
3. Depth law: equal-loudness-ish curve so 50% depth sounds like half.
4. This is the recommended **first FX firmware to build**: it exercises the whole
   audio-in → process → PWM-out path with trivially verifiable behaviour.

**Budgets:** negligible everything.

## Open questions

- Harmonic-tremolo mode (crossfade lows/highs alternately, brown-face amp style)?
  Two one-pole filters + the existing LFO — cheap, characterful, maybe mode 5.
