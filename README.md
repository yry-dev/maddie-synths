# eurorack

My open source eurorack experiments.

## Tooling (macOS/linux + Homebrew)

- Install tools:
  - brew install arduino-cli python
- Optional GUI IDE:
  - brew install --cask arduino-ide

## Repo-local Arduino CLI config

This repo uses `arduino-cli.yaml` at the root.

It currently includes RP2040 board index support and works for both:

- Arduino Nano (official core)
- Raspberry Pi Pico (Earle Philhower core)

## One-time core install

- Arduino Nano / AVR:
  - arduino-cli core install arduino:avr --config-file ./arduino-cli.yaml
- Raspberry Pi Pico / RP2040:
  - arduino-cli core install rp2040:rp2040 --config-file ./arduino-cli.yaml

## One-time library install

Install external libraries used by the Testbild sketches:

- arduino-cli lib install "Encoder" "FastGPIO" "Adafruit SSD1306" "digitalWriteFast"

`Adafruit SSD1306` pulls `Adafruit GFX Library` and `Adafruit BusIO` automatically.

### MOD2 Braids / Tides (Mutable Instruments port)

`mod2-braids` and `mod2-tides` depend on Mutable Instruments DSP code and two
helper libraries that are **not** vendored in this repo. Install them once into
your Arduino libraries folder (default `~/Documents/Arduino/libraries`).

1. Library Manager dependencies:

   - arduino-cli lib install "Bounce2" "RPI_PICO_TimerInterrupt"

2. Mutable Instruments libraries from [poetaster/arduinoMI](https://github.com/poetaster/arduinoMI)
   (the modules live in separate submodule repos — clone the three we need over
   HTTPS straight into the libraries folder):

   - `git clone https://github.com/poetaster/STMLIB.git ~/Documents/Arduino/libraries/STMLIB`
   - `git clone https://github.com/poetaster/BRAIDS.git ~/Documents/Arduino/libraries/BRAIDS`
   - `git clone https://github.com/poetaster/TIDES.git  ~/Documents/Arduino/libraries/TIDES`

3. **TIDES packaging fix.** The `TIDES` library ships its sub-sources as `.cc`
   files *and* amalgamates them in `src/tides_all.cpp`, so arduino-cli compiles
   them twice and the link fails with `multiple definition` errors. `BRAIDS`
   already uses `.inc` for the same trick; make `TIDES` match by renaming its
   three sub-sources and updating the includes:

   ```sh
   cd ~/Documents/Arduino/libraries/TIDES/src
   for f in resources poly_slope_generator ramp_extractor; do
     mv "tides2/$f.cc" "tides2/$f.inc"
   done
   sed -i '' -E 's#(tides2/[a-z_]+)\.cc#\1.inc#' tides_all.cpp
   ```

`PWMAudio.h` (used by both) ships with the `rp2040:rp2040` core, so no extra
install is needed for it. After this, `make mod2-braids` and `make mod2-tides`
build cleanly.

## Makefile firmware builds

- Build every firmware into `dist/<firmware>/`:
  - make
- Build every firmware for a different board target:
  - make FQBN=rp2040:rp2040:rpipico
- Build a single firmware target:
  - make mod1-trigger-burst
- List discovered firmware targets:
  - make list
- Remove build output:
  - make clean

The Makefile discovers every sketch folder under `firmwares/` that contains a same-named `.ino` entry file and excludes `firmwares/shared/`.

## VCV Rack plugin builds

The same root Makefile also drives the VCV Rack plugin in [`rack-plugins/`](rack-plugins/)
(which shares the platform-agnostic voice cores in `firmwares/shared/SynthCore`):

- Build the plugin:
  - make rack
- Build + install into Rack's user plugins folder:
  - make rack-install
- Package a distributable `.vcvplugin`:
  - make rack-dist
- Remove the plugin build output:
  - make rack-clean
- Build everything (all firmwares + the plugin):
  - make everything
- Build against a non-vendored Rack SDK:
  - make rack RACK_DIR=~/Rack-SDK

The plugin manifest is the canonical `plugin.json` at the repo root; the
`rack-plugins/` Makefile syncs a build-time copy into its own folder (which the
Rack SDK requires) so the manifest lives in exactly one place. See
[`rack-plugins/README.md`](rack-plugins/README.md) and
[`rack-plugins/PORTING.md`](rack-plugins/PORTING.md) for details.

### Releasing the plugin (CI)

[`.github/workflows/rack-plugin.yml`](.github/workflows/rack-plugin.yml) builds the
plugin for macOS (x64 + arm64), Linux, and Windows and publishes a GitHub release
with the `.vcvplugin` packages. It runs `make rack-dist` against a freshly
downloaded Rack SDK on each platform, so it exercises the same build path as a
local build. Push a tag to trigger it:

- `vX.Y.Z-msrack` → versioned release (e.g. `v0.0.1-msrack`)
- `vX.Y.Z-next-msrack` → pre-release / nightly (version stamped with the commit)

The workflow derives the plugin version from the tag, so the tag is the single
source of truth for a release's version. A manual `workflow_dispatch` run builds
all platforms without publishing, for testing.

## Shared library code

Use `firmwares/shared/` as the repo-local Arduino library root. Each library should use the standard Arduino layout:

- `firmwares/shared/<LibraryName>/library.properties`
- `firmwares/shared/<LibraryName>/src/<LibraryName>.h`
- `firmwares/shared/<LibraryName>/src/<LibraryName>.cpp`

This repo includes a starter library at `firmwares/shared/Mod1Common`.

## Hardware

### 2020 rail adapter (`hardware/2020-adapter`)

A 3D-printable slide-in adapter that turns standard **2020 aluminum extrusion**
into a Eurorack mounting rail. It end-loads into the extrusion's T-slot and
exposes a captive C-channel along its length that holds a Eurorack threaded
strip (or a row of M3/M2.5 T-nuts), so you can build a Eurorack case out of
2020 rails instead of buying dedicated vertical rails.

- One T-profile tab slides into the 2020 slot; the tab matches the generic
  Misumi-style tapered T-slot (narrow stem flaring to a chamfered head) so it
  seats without rocking.
- The top face has a screw-access slot cut through a retaining lip, so the
  threaded strip stays captive but module screws can still reach it from the
  front.

Files:

- `2020-adapter.scad` — parametric OpenSCAD source. All slot, tab, body, and
  strip-channel dimensions are editable parameters at the top of the file.
- `2020-adapter.stl`, `2020-adapter-40mm.stl`, `2020-adapter-50mm.stl`,
  `2020-adapter-90.stl`, `2020-adapter-100mm.stl` — pre-exported STLs at a few
  rail lengths.

Printing / sizing notes:

- The `length` parameter is the rail length in mm. Most printers cap out around
  100 mm wide; print multiple segments end to end for wider cases (84HP ≈
  128.5 mm, 104HP ≈ 158.75 mm).
- Default screw access is sized for M3; set `access_slot_width = 2.9` for M2.5.
- For best dimensional accuracy on the slot and tab, stand the part on one end
  (cross-section facing up); laying it on its back face also works since the
  channel lips bridge as a ~1 mm overhang.
