# eurorack

My open source eurorack experiments.

## Repo layout

```text
.
├── firmwares/                  Arduino sketches — one folder per module
│   ├── mod1-*/                 Arduino Nano modules  (mod1-euclidean.ino, …)
│   ├── mod2-*/                 XIAO RP2350 modules   (mod2-vco.ino, …)
│   ├── hagiwo30-*/             HAGIWO #30 sequencer variants (Nano)
│   └── shared/                 repo-local Arduino library root
│       ├── SynthCore/src/      platform-pure DSP cores (*Core.h) shared with VCV
│       ├── Mod1Common/         Nano board helpers (pins, ADC, PWM)
│       ├── Mod2Common/         RP2350 board helpers (audio I/O, panel modes)
│       ├── Hagiwo30Common/     OLED + encoder UI for the #30 platform
│       └── Hagiwo30Sequencers/ sequencer engines for the #30 platform
│
├── rack-plugins/               VCV Rack 2 plugin (slug: maddie-synths)
│   ├── src/<firmware-name>.cpp one Rack module per firmware, same kebab name
│   ├── src/plugin.{hpp,cpp}    model declarations + registration
│   ├── src/compat/             desktop shims (pgmspace.h, …)
│   ├── res/                    panel SVGs (one per module)
│   ├── .Rack-SDK/              vendored Rack SDK 2.6.4
│   └── PORTING.md              firmware → Rack architecture doc  ← read first
│
├── panels/<module>/            KiCad faceplate projects (+ blank-NHP/ blanks)
├── hardware/                   KiCad PCBs and mechanical parts
│   ├── lib/                    shared KiCad symbol + footprint libraries
│   ├── m-power/                eurorack PSU (has its own CLAUDE.md)
│   ├── eurorack-busboard/      bus board
│   ├── sequencerv2/            sequencer PCB
│   └── 2020-adapter/           OpenSCAD 2020-extrusion rail adapter
│
├── scripts/                    fish wrappers around Python helpers
│   ├── check-vcv.fish          verify harness (build + purity + registration)
│   ├── new-vcv-module.fish     scaffold a core header + Rack module + panel
│   ├── build-fw.fish / upload-fw.fish / setup-arduino.fish
│   └── panels/tools/           KiCad → SVG panel generation
│
├── assets/                     shared icons/art used by panels and docs
├── dist/                       build output (gitignored)
├── .github/workflows/          ci.yml, _rack.yml, _firmware.yml, release.yml
├── Makefile                    firmware + Rack plugin build entry point
├── arduino-cli.yaml            repo-local arduino-cli config (board indexes)
└── plugin.json                 canonical Rack manifest (rack-plugins/ copy is generated)
```

Naming is mirrored across layers: `firmwares/mod2-vco/mod2-vco.ino`,
`rack-plugins/src/mod2-vco.cpp`, `panels/mod2-vco/`, and
`firmwares/shared/SynthCore/src/VcoCore.h` are the same module.

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

Install external libraries used by the `hagiwo30-*` sketches (the same set CI
installs in [`.github/workflows/_firmware.yml`](.github/workflows/_firmware.yml)):

- arduino-cli lib install "Encoder" "FastGPIO" "Adafruit GFX Library" "Adafruit SSD1306"

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
- Build for a different board target (`MOD1_FQBN` covers `mod1-*`/`hagiwo30-*`,
  `MOD2_FQBN` covers `mod2-*`):
  - make MOD2_FQBN=rp2040:rp2040:rpipico
- Build a single firmware target:
  - make mod1-trigger-burst
- List discovered firmware targets:
  - make list
- Flash a firmware (discover ports with `make board-list`):
  - make upload FW=mod1-euclidean PORT=/dev/ttyACM0
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

## CI and releases

[`.github/workflows/ci.yml`](.github/workflows/ci.yml) runs on every push and
pull request and builds only what changed: the Rack plugin when
`rack-plugins/**`, `plugin.json`, or `firmwares/shared/SynthCore/**` is touched,
and each firmware whose sketch changed (all of them when anything under
`firmwares/shared/` changes). The actual builds live in two reusable workflows:

- [`_rack.yml`](.github/workflows/_rack.yml) — `make rack-dist` against a
  freshly downloaded Rack SDK for Linux, macOS (x64 + arm64), and Windows.
- [`_firmware.yml`](.github/workflows/_firmware.yml) — `arduino-cli` builds
  grouped by board (`.hex` for AVR, `.uf2` for RP2040). Individual compile
  failures are tolerated and skipped, mirroring `make dist` — the repo
  intentionally carries some WIP sketches that don't compile in a clean
  checkout (license-gated `sample.h`, un-vendored Mutable libraries).

[`release.yml`](.github/workflows/release.yml) runs on release tags, builds
every Rack platform and every firmware, and publishes a GitHub release whose
assets are one `.vcvplugin` per platform plus one flashable binary per
firmware. Push a tag to trigger it:

- `vYYMM.DD` → dated release (e.g. `v2608.15`)
- `vYYMM.DD-next` → pre-release / nightly (version stamped with the commit)

Use `scripts/release-tag.fish` to cut one from today's date rather than typing
the tag by hand.

The workflow derives the plugin version from the tag, so the tag is the single
source of truth for a release's version. The date lands in the
`MINOR.REVISION` half only — the `MAJOR` stays `2` because VCV reads it as the
Rack generation the plugin was built for, not as our release number, so
`v2608.15` publishes plugin version `2.2608.15`. Stable (non pre-release) tags also
force-move a rolling `latest` tag to the same commit with the same assets, so
download URLs like `.../releases/download/latest/mod2-vco.uf2` always resolve
to the newest stable build. A manual `workflow_dispatch` run builds everything
without publishing, for testing.

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
