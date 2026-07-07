# MOD2 Bitcrusher — bit depth & sample-rate reduction

> **Status: implemented.** First MOD2 FX firmware — validates the audio-in path
> (CV jack sampled by the ADC at ~36.6 kHz inside the PWM ISR).
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

Classic digital degradation: quantize the signal to fewer bits and resample it
at a lower (unfiltered) rate for aliasing crunch. Amazing with APC, the drum
firmwares, and Braids.

The DSP lives in the shared `firmwares/shared/SynthCore/src/BitcrusherCore.h`
(`sc::BitcrusherCore`) and is also used by the VCV Rack port
(`rack-plugins/src/mod2-bitcrusher.cpp`, slug `mod2-bitcrusher`).

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Sample-rate reduction (36.6 kHz → ~200 Hz, exponential taper) |
| POT2 (A1) | Bit depth (16 → 1 bits, continuous — truncate/mask crossfade adjacent depths; dither is naturally continuous) |
| BUTTON + POT1 | Wet/dry mix (persisted to flash; rate knob uses pickup after release) |
| CV (A2) | **Audio input** (POT3 therefore unavailable) |
| IN1 | Sample-rate clock override — crush rate follows external clock while pulses arrive (works at audio rate: crush-rate FM) |
| IN2 | Bypass gate (HIGH = dry) |
| BUTTON short | Quantizer style: truncate / TPDF dither / AND-mask (LED blinks style number; persisted) |
| LED | Brightness follows crushed output |

## Implementation notes

1. Core in `BitcrusherCore.h`: a phase-accumulator sample-and-hold for rate
   reduction + a quantize step. Host-tested numerically (dither statistics,
   crossfade continuity, external-clock capture).
2. Continuous bit depth: truncate/AND-mask crossfade between adjacent integer
   depths; dither mode adds ±1 LSB TPDF so POT2 sweeps smoothly either way.
3. And-mask mode: bitwise AND of the 16-bit word with a top-bits mask —
   two's-complement AND floors negatives hard; "broken ROM".
4. Deliberately **no** anti-alias filtering on the downsampler — aliasing is
   the point.
5. One-pole smoothing on pots only (the crush itself stays stepped); one-pole
   DC blocker on the ADC input (unipolar bias removal).
6. External clock detection: IN1 edges are detected sample-accurately in the
   ISR; internal rate resumes ~0.3 s after the last pulse.
7. Loop-side pot reads briefly mask the audio IRQ — the ISR shares the ADC mux
   for the audio input, and an interleaved `analogRead` would return the wrong
   channel.

**Measured budgets:** 56 KB flash (2%), 10 KB RAM (1%) — negligible, as
planned. IN1-clocked crush at audio-rate gate frequencies works in the core
(edge detection is per-sample at 36.6 kHz); stability on real hardware still
to be confirmed.
