# Shared-core architecture: firmware ⇄ VCV Rack

This repo ports HAGIWO MOD1 / MOD2 firmwares to VCV Rack modules. To avoid
re-implementing each module's DSP twice (once in the `.ino`, once in the Rack
`process()`), the **algorithm lives in one place** and both platforms call it.

```
                ┌────────────────────────────────────────────┐
                │  firmwares/shared/SynthCore/src/*.h          │
                │  pure C++ voice cores (no Arduino, no Rack)  │
                │   - sc_math.h   shared math/mapping          │
                │   - sc_dsp.h    shared DSP (Biquad, noise,   │
                │                 soft-clip, DC block, Q12) —  │
                │                 pure ports of Mod2Common DSP │
                │   - <Name>Core.h  one engine per module      │
                └───────────────┬───────────────┬──────────────┘
                                │               │
            #include <LorenzVoice.h>     #include <LorenzVoice.h>
                                │               │
        ┌───────────────────────▼──┐   ┌────────▼───────────────────────┐
        │ firmwares/mod1-butterfly │   │ rack-plugins/src/mod1-butterfly.cpp       │
        │ analogRead → core → OCRx │   │ params → core → setVoltage      │
        └──────────────────────────┘   └─────────────────────────────────┘
```

## Why this works on three targets at once

The core compiles on **AVR (MOD1 / Arduino Nano)**, **RP2350 (MOD2)**, and the
**desktop (VCV Rack)**. To stay portable it depends on `<math.h>` / `<stdint.h>`
only — never `Arduino.h`, `rack.hpp`, or the Pico SDK — and uses `float`, no heap
allocation, and no STL containers (AVR-friendly).

The core is **sample-rate independent**: every engine advances by a caller-supplied
`dt` (seconds). The firmware drives it at its native loop / audio rate; Rack drives
it at `args.sampleTime`. Same math, same sound.

## How each platform finds the core

- **Firmware:** `SynthCore` is a normal Arduino library under
  `firmwares/shared/` (which `make` and `scripts/build-fw.fish` already pass to
  `arduino-cli --libraries`). Sketches just `#include <LorenzVoice.h>`.
- **VCV Rack:** `rack-plugins/Makefile` adds `-I../firmwares/shared/SynthCore/src` to
  `CXXFLAGS`, so the plugin sees the same headers.

## The Voice pattern

Each engine is a small struct with:

- plain state members (`reset()` returns them to the firmware's power-on values),
- a parameter-mapping free function that turns **normalised 0..1 controls** into
  engine units (so the knob math is shared, not just the DSP),
- a per-sample (or per-step) `process()`/`step()` taking `dt`.

Platform code stays thin and owns only I/O:

| Concern            | Firmware                        | VCV Rack                          |
|--------------------|---------------------------------|-----------------------------------|
| Read a pot         | `analogRead(A0)/1023.f`         | `params[X].getValue()` (0..1)     |
| Trigger edge       | `EdgeInput` / pin IRQ           | `dsp::SchmittTrigger`             |
| Audio / CV out     | PWM `OCRx` / `pwm_set_chan_level` | `outputs[X].setVoltage()`       |
| LED                | `digitalWrite` / PWM            | `lights[X].setBrightness()`       |
| Persisted toggle   | `EEPROM`                        | Rack module JSON                  |

## Recipe: porting the next firmware

1. **Find the algorithm** in the `.ino` — the part that isn't `analogRead`,
   `digitalWrite`, PWM register pokes, or `millis()` bookkeeping.
2. **Add a core header** `firmwares/shared/SynthCore/src/<Name>Voice.h` in
   `namespace sc`: state + `reset()` + a `…MapParams(pot0,pot1,pot2,…)` mapper +
   a `process(dt,…)`/`step(…)`. Reuse helpers from `sc_math.h`.
3. **Thin the firmware:** replace the inline math with the core; keep all I/O.
   Verify `make <firmware>` still builds.
4. **Write the Rack module** `rack-plugins/src/<Name>.cpp`: `config()` the same
   pots/jacks/LED, feed normalised controls into the core, scale core output to
   Rack voltages. Register it in `plugin.cpp`, `plugin.hpp`, `plugin.json`.
5. **Build both** (`make <firmware>` and `cd rack-plugins && make`).

See `LorenzVoice.h` + `mod1-butterfly` + `mod1-butterfly.cpp` (CV/control example) and
`ClavesVoice.h` + `mod2-claves` + `mod2-claves.cpp` (audio example) as references.

## Behavior notes (intended convergences)

A single shared core has to pick one behavior where the firmware and the VCV
port historically differed. These choices are deliberate:

- **Butterfly sigma/rho/beta are now continuous on the firmware too.** The old
  `mod1-butterfly` sketch mapped its pots with Arduino's `map()`, which is
  *integer* math — `beta` snapped to `{1,2,3,4}`, `sigma` to `{5..20}`, `rho` to
  `{20..50}`. The VCV port was already continuous (`crossfade`). The shared
  `lorenzMapParams` is continuous for both, so the firmware gains smooth control
  (an improvement for a chaotic CV source, and the quantization was an artifact
  of integer `map()`, not a design choice). This is the one real change to
  firmware output.
- **Step-size boundaries** use `340/1023` and `681/1023` for both platforms.
  This is *exactly* the firmware's `select3FromAdc` (`<=340` / `<=681`); it
  shifts the VCV knob boundaries by ~0.1% versus the old `1/3, 2/3`.
- **Claves waveform** is computed with `sinf`/`asinf` directly rather than the
  firmware's 64-point interpolated LUT. The result is slightly cleaner (no LUT
  interpolation harmonics); the strike envelope and length match within float
  tolerance.
- **Closed-form replaces lookup tables in several voices** to keep the mod1/AVR
  cores RAM-light: LFO waveforms (was six PROGMEM tables), EG / Dual-AD exp
  curves, and Euclidean patterns (on-the-fly Bjorklund vs stored tables).
  Mathematically equivalent to the tables within float precision.
- **Clap / Hihat accent (IN2) is applied post-clip.** The firmware scaled the
  signal by the −6 dB accent *before* the ±1 clamp (for clap, even before the
  band-pass); the shared cores clamp first, so an accented hit is a clean −6 dB
  copy rather than a thinner, less-driven tone. This is consistent on **both**
  platforms (not a cross-platform divergence) and only affects accented strikes.
  Kick / FM-Drum thread accent *before* the clip (matching their originals).
- **SquareVCO V/Oct** uses the idealised `2^v` closed form instead of the
  firmware's 1024-entry hardware-calibrated `voctMap` LUT (~4.2-oct span → exact
  5-oct). Same class of change as the Butterfly `map()` convergence; VCV tracks
  true 1V/Oct.
- **VcoCore output low-pass** uses a fixed per-sample coefficient (0.18), so its
  cutoff is mildly sample-rate dependent (~1.05 kHz at the firmware's 36.6 kHz vs
  ~1.26 kHz at 44.1 kHz) — a small brightness drift. Left as a fixed coefficient
  deliberately to avoid a per-sample `expf` in the audio path.

- **EG / Dual-AD envelope times** use a shared 33-point log-interpolated table
  (`sc::envPotDivisor`) that reproduces the firmware's `kEnvelopePotAdjust`
  compander (duration = 40960/D ms). Exact at the sample points (40 / 142 / 519
  / 2276 ms), ≤4.5% across the musical range; the earlier power-law / pure-exp
  fits were 2.5–3× off mid-dial and were replaced.
- **PRNG**: all random voices use portable `sc::xorshift32` (not Arduino
  `random()`), so firmware and Rack produce bit-identical sequences from the
  same seed.

### `rabid-audio-clk` — a clock, not a voice

`ClkCore.h` is the first core that is a *scheduler* rather than a per-sample
engine, so the split falls in a different place: the core owns the tempo,
subdivision, swing and pulse state machine and answers one question — *how long
until the next tick* — while each platform supplies the time source. The
firmware's Timer1 COMPA interrupt writes that interval into `OCR1A`; Rack's
`ClkEngine::step(dt)` counts it down per sample. Same ticks, same swing, same
`beatOut()`/`divOut()` levels.

- **The 1586-entry PROGMEM `CLOCK_LOOKUP` table is gone.** Every entry equalled
  `trunc(937500 / clock)` — i.e. `trunc(period_seconds × 15625)` for Timer1's
  64 µs tick — so the engine works in seconds and the sketch scales on the way
  out. Verified against all 1586 entries: bit-identical, and ~3.2 KB of AVR flash
  returned (the sketch builds at 24 % of an ATmega328P).
- **Swing is applied to seconds, not to a truncated timer count.** The original
  rounded the base count first and then scaled it by `swing/198`, amplifying its
  own rounding error; the shared core scales the period and truncates once. Over
  194 544 compared ticks (every BPM × 7 subdivisions × 6 swing settings) the two
  differ by at most **2 timer counts** (≤ 5e-5 of the interval), always in the
  core's favour. At swing 0 they are bit-identical.
- **`OCR1A` is clamped instead of wrapping.** At the slowest tempo with deep
  negative swing the interval reaches 5.52 s, past what a 16-bit compare can
  hold; the original wrote it anyway and the register wrapped, turning that into
  1.32 s. The firmware now clamps at 65535 counts (4.19 s), so the swing simply
  stops deepening. Rack has no such register and keeps the full 5.52 s, so this
  is the one corner where the two targets diverge — 456 of those 194 544 ticks.
- **Subdivision range is unified at `-8..4`.** The original's two setters
  disagreed: `setSubdivisions` clamped to `[-4, 8]` while `incrementSubdivisions`
  clamped to `[-8, 4]`. The core keeps the range the encoder can actually reach.
- **EEPROM validation widened.** The original rejected any stored tempo above
  300 BPM although the engine goes to 400, silently discarding a saved fast
  tempo; the sketch now range-checks against the engine's real limits, and checks
  the subdivision and swing fields too.
- **One 7-segment font, two consumers.** `sc::clkSegments()` returns a standard
  a–g mask; the sketch permutes it into its two PCB bit orders (the CLK's centre
  digit is wired differently to fit 3 HP) and the Rack widget draws it. Verified
  to reproduce both of the original's hard-coded tables exactly.

Divergences that are Rack-side product decisions rather than numeric ones:

- **The modal encoder is kept; the buttons latch instead of holding.** On the
  hardware you HOLD DIV or SWING and turn the encoder, and DIV+SWING together
  toggles pause. A mouse cannot hold a button and turn a knob at once, so in Rack
  the two buttons *latch* — which also makes the pause chord reachable. The
  encoder is a relative widget with no value of its own: it adds detents to
  whichever of the three real parameters is selected, and a click (a press that
  never moved) is the EC11's push-switch, i.e. tap tempo. `BPM_PARAM`,
  `DIV_PARAM` and `SWING_PARAM` remain ordinary `configParam()`s, so presets,
  patch save/load and the right-click menu keep working even though only one
  knob is drawn.
- **No save / load / factory-reset gestures.** They exist because the hardware
  has EEPROM and no other way to keep a tempo; Rack persists parameters with the
  patch, and "Initialize" is the factory reset. The hold-to-confirm gestures were
  layered on the encoder push, which is a click here and cannot express a hold.
- **The panel is theirs, not ours — mechanics and artwork both.** Every cutout is
  read out of their `clock.kicad_pcb`, so the Rack widget's coordinates are the
  real module's: jacks at the top, a display that reads vertically, an encoder and
  two buttons. The plate also wears their design language rather than the house
  style — black ink on bare aluminium, a halftone field, tilted slab-typewriter
  labels — because a port should be recognisable as the module it ports. This is
  the only panel in the repo that opts out; see `panels/DESIGN-RULES.md` §11.
- **It powers on at 120 BPM showing tempo**, neither latch down, matching the
  hardware's own power-on state.

Known minor deviations (documented, not fixed — low impact):

- **RandomCV** fires its gate immediately on the clock edge; the firmware
  delayed it ~10 ms. Gate width matches.
- **VcoCore** output low-pass is a fixed coefficient → mild sample-rate-dependent
  brightness (see above).
- A few cores re-declare small helpers (`xorshift32`, `select6`) locally rather
  than from `sc_math.h`/`sc_dsp.h`. This is not just style: **`sc_dsp.h` cannot
  be included from any AVR (MOD1) sketch** — its Biquad members `B0..B2`/`A1..A2`
  are macros behind Arduino.h on AVR (binary.h and the analog-pin defines), so
  the header fails to compile there. Cores consumed by MOD1 firmwares must
  inline what they need; write PRNG steps inside a member function (see
  `GeigerCore.h::nextUniform()`) rather than as a free `sc::xorshift32`, which
  would collide if a translation unit ever included two such cores.

Everything else is numerically equivalent to the originals within float
precision (verified by build on all three targets plus host-side numeric tests
of the cores, including the EG/Dual-AD envelope times, the LogicPair gate
levels, and the RandomLag walk velocity).
