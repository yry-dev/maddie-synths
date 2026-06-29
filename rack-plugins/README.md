# Maddie Synths — VCV Rack plugin

VCV Rack 2 ports of the firmwares in [`../firmwares/`](../firmwares/). One plugin
(`MaddieSynths`), one module per firmware.

| Module    | Firmware             | What it does                          |
| --------- | -------------------- | ------------------------------------- |
| Butterfly | `mod1-butterfly.ino` | Lorenz attractor chaotic CV generator |
| Claves    | `mod2-claves.ino`    | Claves / woodblock percussion voice   |

Each module mirrors its hardware: 3 pots, 1 push button, 1 LED, plus the board's
jacks. Panels are drawn at 6 HP for legibility; the real hardware is 4 HP.

- **Butterfly** (Mod1): SIGMA / RHO / BETA pots, SLOW button, F1 = reset trigger
  in, F2/F3/F4 = X/Y/Z out.
- **Claves** (Mod2): DECAY / WAVE / PITCH pots, TRIG button, IN1 = trigger in,
  CV = 1V/Oct pitch, IN2 = unused (mirrors the hardware), OUT = audio.

## Build

The plugin manifest is the canonical `plugin.json` at the **repo root**; the
Makefile here syncs a copy into this folder at build time (gitignored).

The Rack SDK is vendored (gitignored) at `.Rack-SDK/`. From this folder:

```bash
make            # build plugin.dylib
make install    # build + copy into Rack's user plugins folder, then restart Rack
make clean      # remove build output
```

Or from the repo root (builds firmware and the plugin from one Makefile):

```bash
make rack          # build the plugin
make rack-install  # build + install into Rack
make rack-clean    # remove build output
make everything    # build all firmwares + the plugin
```

To build against a different SDK: `make RACK_DIR=/path/to/Rack-SDK` (the
`rack*` targets forward `RACK_DIR` too).

If the vendored SDK is missing (fresh clone), re-download it:

```bash
curl -sL -o sdk.zip https://vcvrack.com/downloads/Rack-SDK-2.6.4-mac-arm64.zip
unzip -q sdk.zip && rm sdk.zip && mv Rack-SDK .Rack-SDK
```

(Use the matching `-mac-x64`, `-lin-x64`, or `-win-x64` build on other platforms.)

## Adding a module (porting another firmware)

1. Add `src/<Name>.cpp` defining a `Module`, a `ModuleWidget`, and
   `Model* model<Name> = createModel<...>("<Name>");`. Copy `mod1-butterfly.cpp` as a
   starting point.
2. Declare `extern Model* model<Name>;` in `src/plugin.hpp` and
   `p->addModel(model<Name>);` in `src/plugin.cpp`.
3. Add a `res/<Name>.svg` panel sized to the module's HP (8 HP = 40.64 mm wide,
   128.5 mm tall; SVG units are px at 75 DPI, so 1 mm = 2.952756 px).
4. Add a `modules` entry to the root `plugin.json`.
5. `make install` and restart Rack.

### Porting notes

The firmware runs an Arduino loop with PWM outputs; Rack gives you a real clock
and ±voltages. When porting:

- Hardware pots that did double duty (e.g. Butterfly's Sigma pot also picked a
  discrete integration step) can be split into proper separate controls.
- Replace loop-rate-dependent timing with a sample-rate-independent step driven
  by `args.sampleTime`, sub-stepped at a fixed internal dt for numeric stability.
- PWM outputs (0..255, ~0–5 V after filtering) become explicit voltages —
  bipolar signals as ±5 V, unipolar as 0–10 V.
