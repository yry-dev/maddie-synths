# MOD2 Karplus — dedicated Karplus-Strong voice/processor

> **Status: implemented** (firmware `mod2-karplus.ino`, shared `sc::KarplusCore`,
> VCV Rack port `rack-plugins/src/mod2-karplus.cpp`). Tier 3, roadmap v2, CPU: medium.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

Karplus-Strong already exists as one Flux mode; this promotes it to a dedicated
firmware with real depth: excitation choices, damping/brightness control, and —
because it's an FX platform — the ability to pluck the string **with external
audio** through the CV jack instead of internal noise.

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Pitch (semitone-quantized) |
| POT2 (A1) | Damping / brightness (loop filter cutoff + decay) |
| BUTTON + POT1 | Wet/dry (dry = excitation signal) |
| BUTTON + POT2 | Excitation colour (noise burst dark↔bright / pick position) |
| CV (A2) | Audio input — continuous excitation (bowed/scraped string!) |
| IN1 | Pluck trigger (internal exciter) |
| IN2 | Damp gate (palm mute while high) |
| BUTTON short | Mode: Pluck / Bow (continuous input drive) / Drone (near-unity loop) |
| BUTTON long | Manual pluck |
| LED | Blinks on pluck, follows string energy |

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/KarplusCore.h`, seeded from the
   Karplus path in `FluxCore.h`: fractional delay loop + loop filter
   (one-pole LP + allpass for fine-tuning the period).
2. The allpass fractional tuning is the upgrade over Flux — Flux's integer
   period is out of tune up high; fix that here.
3. Pick position: comb-filter the excitation burst (delay + subtract) before
   injection.
4. Bow mode: continuously sum attenuated external audio into the loop —
   this is the distinctive FX trick (feed it APC or noise and it "sings").
5. DC blocker in the loop; energy limiter so Drone mode can't blow up.

**Budgets:** one string ≈ 2 KB; CPU light. Could afford 2–3 voice round-robin
polyphony on triggers if monophonic retrigger clicks annoy.

## Open questions

- Round-robin 2-voice mode vs pure mono with fast crossfade retrigger.
