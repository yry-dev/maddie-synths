# MOD2 Tape Echo — worn-tape delay

> **Status: implemented.** Tier 1 (must-have), roadmap v2, CPU: medium.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

Delay with a tape-machine model in the loop: wow/flutter on the read head,
saturation and high-frequency loss in the feedback path, and a "tape age" macro
that degrades everything at once. Great on APC.

DSP lives in the shared `sc::TapeEchoCore`
(`firmwares/shared/SynthCore/src/TapeEchoCore.h`), also used by the VCV Rack
port (`rack-plugins/src/mod2-tape-echo.cpp`).

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Delay time (30 ms – 2 s); time changes glide with tape-speed pitch bend |
| POT2 (A1) | Tape age macro: wow/flutter depth + HF loss + saturation + dropout rate |
| BUTTON + POT1 | Wet/dry mix (saved to flash) |
| BUTTON + POT2 | Feedback amount (second shift param, saved) |
| CV (A2) | Audio input |
| IN1 | Tap tempo / clock — each measured period sets the time directly |
| IN2 | "Splice" gate — momentary tape-stop/start lurch |
| BUTTON long | Tap tempo — presses ≥0.6 s count as taps, timestamped at the press edge (POT1 reclaims the time with pickup) |
| LED | Flickers with flutter and dropouts — visualizes tape health |

## Implementation notes

1. Core in `firmwares/shared/SynthCore/src/TapeEchoCore.h`, built on the same
   `sc::DelayLine` buffer engine as `DelayFxCore` (220 KB SRAM arena = 3.0 s of
   tape: 2 s max delay + 1 s of tape-stop headroom).
2. Time changes are modelled as tape-speed change: a servo drives the read
   *rate* (0.3×–2.5×) until the read/write gap settles on the target (~220 ms
   time constant), so repeats pitch-bend like a real echo unit. The gap
   accumulates in double (RP2350 has a hardware double coprocessor) — float32
   quantizes the tiny 1−rate deltas audibly, per the original 64-bit-phase note.
3. Wow: 0.9 Hz sine on the read position (±3 ms at full age); flutter: 8 Hz
   jitter + 30 Hz-filtered noise (±0.4 ms). Both scale with age².
4. Feedback path: soft saturator (drive rises with age) → one-pole low-pass
   whose cutoff drops 8 kHz → 1.2 kHz with age → random ~25 ms dropout gain
   dips above half age.
5. Tape-stop gesture on IN2: the transport envelope ramps the read rate
   exponentially to 0 (~60 ms) and back; the wet signal fades with it, and the
   gap the stop opened is re-caught with a pitch-up lurch on release.

**Budgets:** 220 KB buffer of 520 KB (43% with statics), CPU light — a couple
of filters + LFOs per sample.

## Open questions

- Whether age should also add a little pre-echo/crosstalk for realism (cheap,
  fun — currently omitted).
