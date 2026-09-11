# MOD2 Reverb — hall & plate

> **Status: implemented** — `sc::ReverbCore` + firmware + VCV Rack port.
> Tier 1 (must-have), roadmap v3, CPU: **hard**.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.
> Covers both the "Hall Reverb" and "Plate Reverb" brainstorm items as two modes.

Mono-in/mono-out algorithmic reverb. **Hall**: big, long, darker tail — pads and
ambient. **Plate**: shorter, brighter, denser early build-up — excellent on Plaits
and percussion.

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Size (pre-delay + delay-line scaling) |
| POT2 (A1) | Decay (feedback gain; top of range ≈ infinite) |
| BUTTON + POT1 | Wet/dry mix |
| BUTTON + POT2 | Damping / tone |
| CV (A2) | Audio input |
| IN1 | (spare — possible ducking trigger) |
| IN2 | Freeze gate (decay → infinite while high) |
| BUTTON short | Mode: Hall / Plate |
| LED | Slow pulse in Hall, fast in Plate; brightness follows tail level |

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/ReverbCore.h`. Start from a
   Dattorro-style figure-8 tank (2 branches: allpass diffusers → modulated
   delay → damping LPF), mono-summed output. Plate mode = shorter delays,
   more input diffusion, brighter damping; Hall = longer delays, pre-delay,
   darker damping.
2. **All delay lines int16** to fit RAM; keep the filter/feedback math in float.
   Total tank memory target ≤ 200 KB.
3. Slight LFO modulation on two tank delays to kill metallic ringing.
4. Size changes: scale delay lengths with crossfade or accept a brief smear —
   test which sounds acceptable.
5. CPU: this is the most expensive planned effect. Budget it first: count
   allpasses/filters per sample; if tight, run the tank at 18.3 kHz (half rate)
   with up/down halfband filters — classic trick, fine for reverb tails.

**Budgets:** ~200 KB RAM, CPU near the ceiling — prototype the tank in the VCV
Rack port first (same core), then optimize on hardware.

## Open questions

- Half-rate tank vs full-rate: decide by ear on hardware.
- Whether Shimmer (pitch-shifted feedback) is worth a third mode later —
  depends on the pitch-shifter core from `mod2-pitch-shifter`.
