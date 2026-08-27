# MOD2 Ring Mod — carrier-oscillator ring modulator

> **Status: implemented** (shared `sc::RingModCore` + VCV Rack port). Tier 3, roadmap v2, CPU: easy.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

Classic ring modulation of the input against an internal carrier oscillator.
Excellent with APC. (True external-carrier ring mod needs two audio ins — the
internal carrier is the mono-hardware answer, and tracking/scan modes make it
more playable than a bare multiply anyway.)

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Carrier frequency (0.5 Hz – 5 kHz, exponential — sub-audio = tremolo-ish AM) |
| POT2 (A1) | Carrier shape morph: sine → triangle → square (harsher products) |
| BUTTON + POT1 | Wet/dry mix |
| BUTTON + POT2 | AM ↔ ring-mod blend (how much dry leaks through the multiply) |
| CV (A2) | Audio input |
| IN1 | Carrier hard-sync / retrigger |
| IN2 | Carrier octave-drop gate (instant "broken speaker") |
| BUTTON short | Mode: Fixed / Track (carrier follows input pitch) / S&H (random carrier per IN1) |
| LED | Blinks at carrier rate (visible sub-audio, solid above) |

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/RingModCore.h`: wavetable carrier
   (sine table + shape morph), one multiply per sample. Trivial DSP.
2. **Track mode** is the interesting part: cheap time-domain pitch detector
   (autocorrelation on a decimated buffer, ~50–1000 Hz range) slews the carrier
   to a ratio of the detected pitch — clangorous but harmonically related.
3. Diode-ringmod flavour option: waveshape the product slightly (asymmetric)
   for the vintage non-ideal sound.
4. Carrier phase continuity on freq changes (phase accumulator, no resets).

**Budgets:** trivial, except Track mode's autocorrelation — run it at control
rate on a 4×-decimated buffer (~9 kHz), well within budget.

## Open questions

- Pitch detector robustness on drums/noise (APC) — needs a confidence gate that
  freezes the carrier when tracking is garbage.
