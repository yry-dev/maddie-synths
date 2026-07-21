# MOD2 Freeze — buffer capture & loop

> **Status: implemented** — firmware (`mod2-freeze.ino`), shared core
> (`SynthCore/src/FreezeCore.h`) and VCV Rack port (`rack-plugins/src/mod2-freeze.cpp`).
> Tier 4 (experimental), roadmap v3, CPU: easy-medium.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

Capture a slice of the incoming audio and loop it seamlessly: instant drones,
pads from percussion, "hold that chord" moments. The simplest deep effect.

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Loop length (5 ms – full buffer; short = pitched buzz-drone) |
| POT2 (A1) | Loop-window position within the captured buffer |
| BUTTON + POT1 | Wet/dry (frozen vs live blend) |
| BUTTON + POT2 | Crossfade length (tight loop ↔ smeared wash) |
| CV (A2) | Audio input |
| IN1 | Freeze gate (freeze while high — or latch mode via button setting) |
| IN2 | Re-capture trigger (grab new audio without unfreezing) |
| BUTTON short | Playback: Forward / Ping-pong / Half-speed (octave down) |
| BUTTON long | Manual freeze latch |
| LED | Solid when frozen, follows input level when live |

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/FreezeCore.h`: always-recording
   circular buffer (~400 KB ≈ 5.5 s). Freeze = stop the write head; playback
   reads a windowed loop with equal-power crossfade at the seam.
2. Crossfaded loop seam is the entire quality battle: window length from
   BUTTON+POT2, minimum 5 ms even at "tight".
3. Freeze/unfreeze itself crossfades live↔frozen over ~10 ms.
4. Half-speed playback via 0.5× read rate (linear interp) — free octave-down pads.
5. Later nicety: light random walk of window position ("drift") to keep long
   freezes alive — one parameter away from granular territory without the cost.

**Budgets:** RAM-maxed by design, CPU trivial. Excellent early build —
also the substrate for `mod2-granular` and `mod2-stutter` (same capture buffer).

## Open questions

- Gate-freeze vs latch-freeze as default (probably gate on IN1, latch on button).
