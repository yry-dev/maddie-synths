# MOD2 Delay — clean/dirty digital delay

> **Status: implemented.** Tier 1 (must-have), roadmap v1, CPU: medium.
> Hardware/audio-in caveats: see `../mod2-fx/README.md`.

Bread-and-butter mono delay; the single most useful FX firmware. Covers 90% of
patches on its own. Also absorbs the "Clocked Delay" brainstorm item: IN1 clock
quantizes delay time to musical divisions of the incoming BPM.

DSP lives in the shared `sc::DelayFxCore`
(`firmwares/shared/SynthCore/src/DelayFxCore.h`), also used by the VCV Rack
port (`rack-plugins/src/mod2-delay.cpp`).

## Controls

| Control | Function |
|---|---|
| POT1 (A0) | Delay time (10 ms – 5 s, exponential taper), or clock division when clocked |
| POT2 (A1) | Feedback (0 – ~110%, soft-limited into self-oscillation) |
| BUTTON + POT1 | Wet/dry mix (saved to flash) |
| CV (A2) | Audio input |
| IN1 | Clock in — delay time locks to {1/4, 1/3, 1/2, 2/3, 3/4, 1, 1.5, 2}× of the measured period, picked by POT1 |
| IN2 | Hold gate — HIGH mutes the input and pins feedback at unity (momentary infinite repeat; spillover decays at the feedback knob on release) |
| BUTTON short | Mode: Clean / Dirty (saved; LED blinks mode ID) |
| BUTTON long | Tap tempo — presses ≥0.6 s count as taps, timestamped at the press edge; the interval between the last two sets the time (POT1 reclaims it with pickup) |
| LED | Blinks at the delay time |

## Modes

- **Clean** — linear-interpolated read head, flat feedback path.
- **Dirty** — feedback path through soft saturation + gentle one-pole low-pass
  (~2.5 kHz), darkening repeats (bucket-brigade flavour).
- Ping-pong deferred: needs stereo out, current hardware is mono.

## Implementation notes

1. Core in `firmwares/shared/SynthCore/src/DelayFxCore.h`: `sc::DelayLine`
   circular buffer (int16, 366 KB SRAM arena = 5.0 s at ~36.6 kHz), fractional
   read with linear interp, feedback tap with per-mode colour stage.
2. Time changes crossfade between two read heads (~35 ms raised-cosine) — no
   pitch-zip artifacts. (The slew-the-read-rate feel lives in mod2-tape-echo.)
3. Clock sync: IN1 period measured sample-accurately in the audio ISR;
   POT1 quantizes to {1/4, 1/3, 1/2, 2/3, 3/4, 1, 1.5, 2}×. Clock mode decays
   4 periods after the last tick.
4. The buffer write is always soft-saturated, so >100% feedback self-oscillates
   into a warm limit instead of wrapping the int16 buffer. During hold the echo
   recirculates raw (the saturator would sag the "infinite" repeat).
5. Sketch `mod2-delay.ino` owns only I/O plumbing via `Mod2Common` (audio-rate
   ADC input + DC blocker, guarded pot reads, shift layer with pot pickup,
   EEPROM-persisted wet/mode).

**Budgets:** RAM 366 KB of 520 KB (71% with statics), CPU trivial.

## Open questions

- Max time vs. flash headroom once presets are added — 5 s fits today with
  ~148 KB SRAM spare; drop to 4 s if presets ever contend.
- Whether Dirty mode should also add subtle wow (currently left to
  mod2-tape-echo).
