# MOD2 Spectral Freeze — FFT-domain infinite sustain

> **Status: implemented.** Core (`shared/SynthCore/src/SpectralFreezeCore.h`),
> firmware sketch, and VCV Rack port done; prototyped/verified on the Rack side
> first (README point 5). Tier 4 (experimental), roadmap v4,
> CPU: **hard — ambitious but fun**, the moonshot of the FX set.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

Freeze the *spectrum* rather than the waveform: capture magnitude + phase,
then resynthesize with randomized phase advance — an endless, motionless-yet-
alive pad from any instant of sound. Unlike `mod2-freeze`, there is no loop
point and no rhythm — pure suspended texture.

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Shimmer/animation: per-bin phase-randomization rate |
| POT2 (A1) | Spectral tilt / bandpass focus of the frozen spectrum |
| BUTTON + POT1 | Wet/dry (frozen vs live) |
| BUTTON + POT2 | Attack/blend time into freeze |
| CV (A2) | Audio input |
| IN1 | Capture trigger (grab a new spectrum, crossfade in spectral domain!) |
| IN2 | Freeze gate |
| BUTTON short | Mode: Single frame / 4-frame averaged (smoother) / Drifting (slow random bin walk) |
| LED | Solid when frozen; shimmer rate modulates brightness |

## Implementation plan

1. Core in `firmwares/shared/SynthCore/src/SpectralFreezeCore.h`:
   - STFT: 1024-point FFT, Hann, 75% overlap (hop 256 ≈ 7 ms @ 36.6 kHz).
   - Freeze = hold bin magnitudes; each hop, advance each bin's phase by its
     nominal rate + random jitter scaled by POT1 (classic phase-vocoder freeze).
   - IFFT + overlap-add out.
2. Use the CMSIS-DSP q15/float radix FFT for RP2350 (M33 + FPU); 1024-pt float
   FFT per 256-sample hop must be profiled — **this is the go/no-go item**.
   Fallback: 512-point / 50% overlap (grainier but half the cost).
3. Run FFT work on **core 1** of the RP2350, ping-pong hop buffers to the audio
   ISR on core 0 — first firmware to need the second core; the pattern is
   reusable for future heavy FX.
4. Latency ≈ one window (28 ms) — irrelevant for a freeze effect.
5. Prototype entirely in the Rack port first; hardware port only after the
   sound is right.

**Budgets:** ~30 KB working RAM (buffers + twiddles) — fine. CPU is the whole
question; core-1 offload should make it comfortably feasible.

## Open questions

- CMSIS-DSP availability/ergonomics in the arduino-pico toolchain (else vendor a
  small radix-2 FFT).
- Whether Drifting mode should slowly crossfade between two captured frames.
