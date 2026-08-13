# CLAUDE.md

Project-specific instructions for Claude Code. Read this before generating or
modifying code in this repository.

## What this repo is

Maddie's open-source Eurorack monorepo: Arduino firmwares for HAGIWO-style
modules, VCV Rack ports of those same firmwares, KiCad PCBs/faceplates, and the
build/release tooling that ties them together. There is no application server,
no package manager, and no test framework — builds are driven by `make` +
`arduino-cli` + the vendored VCV Rack SDK.

## Priority guidelines

1. **Shared-core architecture is load-bearing** — DSP lives once in
   `firmwares/shared/SynthCore/src/*Core.h` and is included by BOTH the Arduino
   sketch and the Rack module. Never duplicate an algorithm into a `.ino` or a
   Rack `.cpp`; put it in the core. Read `rack-plugins/PORTING.md` before
   touching any module.
2. **Core headers must stay platform-pure** — `*Core.h`, `sc_dsp.h`, and
   `sc_math.h` may include only `<math.h>`/`<stdint.h>`-level standard headers.
   Never `Arduino.h`, `rack.hpp`, the Pico SDK, or `Mod*Common`. They compile on
   AVR (Nano), RP2350, and desktop: `float` only, no heap.
   `scripts/check-vcv.fish` enforces this.
3. **Version compatibility** — Rack SDK **2.6.4** (vendored at
   `rack-plugins/.Rack-SDK`, pinned as `RACK_SDK_VERSION` in
   `.github/workflows/_rack.yml`). Boards: `arduino:avr:nano` for
   `mod1-*`/`hagiwo30-*`, `rp2040:rp2040:seeed_xiao_rp2350` for `mod2-*`
   (root `Makefile`). AVR is tiny — mind flash/RAM in shared code.
4. **Codebase patterns first** — copy the closest existing module; don't import
   outside conventions.
5. **`plugin.json` is canonical at the repo root** — `rack-plugins/plugin.json`
   is a gitignored build-time copy synced by `rack-plugins/Makefile`. Only ever
   edit the root one.

## Detected technology stack

- Firmware: Arduino C++ sketches built with `arduino-cli` (repo-local config
  `arduino-cli.yaml`; RP2040 core from Earle Philhower's board index).
- VCV Rack plugin: C++11-style Rack 2 plugin (`rack-plugins/`, slug
  `maddie-synths`, MIT), built via the SDK's `plugin.mk`.
- Scripts: fish (`scripts/*.fish`) wrapping Python 3 (`scripts/_*.py`,
  `scripts/panels/`, `scripts/tools/`). No Python deps manifest.
- Hardware: KiCad projects (`hardware/`, `panels/`), OpenSCAD
  (`hardware/2020-adapter`).
- No test framework, no linter/formatter config. Verification is
  `scripts/check-vcv.fish` + CI builds.

## Project structure

- `firmwares/<name>/<name>.ino` — one sketch per module. The root `Makefile`
  discovers any `firmwares/*/` containing a same-named `.ino`; the folder name
  IS the build target and its prefix picks the board (`mod1-`/`hagiwo30-` →
  AVR Nano, `mod2-` → RP2350).
- `firmwares/shared/` — repo-local Arduino library root (`--libraries` path):
  `SynthCore` (platform-pure voice cores), `Mod1Common`, `Mod2Common`,
  `Hagiwo30Common`, `Hagiwo30Sequencers`. Standard Arduino library layout
  (`library.properties` + `src/`).
- `rack-plugins/` — the VCV Rack plugin. `src/<firmware-name>.cpp` (one module
  per firmware, same kebab-case name), `src/plugin.{hpp,cpp}` (registration),
  `src/compat/` (desktop shims like `pgmspace.h`), `res/` (panel SVGs),
  `PORTING.md` (the architecture doc).
- `panels/`, `hardware/` — KiCad faceplates and PCBs. `hardware/lib/` holds the
  shared KiCad symbol/footprint libs; `hardware/m-power/` has its own
  `CLAUDE.md` — read it before working there.
- `scripts/` — `new-vcv-module.fish` (scaffolds core header + Rack module +
  panel stub and registers them), `check-vcv.fish` (verify harness),
  `build-fw.fish`, `upload-fw.fish`, `setup-arduino.fish`.
- `dist/` — gitignored build output (`dist/<firmware>/`).
- `.github/workflows/` — `ci.yml` (change detection → builds only what
  changed), `_rack.yml` / `_firmware.yml` (reusable builders), `release.yml`
  (tag-driven publishing).

## Codebase pattern lookup

When adding or changing a module, find the closest existing sibling and match it:

1. Naming is kebab-case and mirrored across layers: firmware
   `firmwares/mod2-vco/mod2-vco.ino`, Rack `rack-plugins/src/mod2-vco.cpp`,
   panel `panels/mod2-vco/`, core `SynthCore/src/VcoCore.h` (PascalCase
   `<Name>Core.h`), model `modelVCO`.
2. New Rack modules go through `scripts/new-vcv-module.fish`, which inserts at
   the `SCAFFOLD:` marker comments in `plugin.hpp`/`plugin.cpp` and edits the
   root `plugin.json`. Don't hand-register a module a different way.
3. Every Rack module `.cpp` opens with a block comment documenting the
   pot/button/LED/jack mapping and how it diverges from the firmware (see
   `rack-plugins/src/mod2-vco.cpp`); firmwares open with an ASCII panel diagram
   (see `firmwares/mod2-vco/mod2-vco.ino`). Keep this.
4. Mod2 Rack modules extend `Mod2Module` (panel-style persistence) and use
   `setMod2Panel`/`appendMod2PanelMenu`; knobs that mirror the hardware's
   reverse-wired pots use `Reversed<>` (`rack-plugins/src/plugin.hpp`).
5. Comment density is deliberately high and explains *why* (hardware quirks,
   licensing, build constraints) — match it. Tabs in Rack C++ sources.

## WIP / excluded modules — the established mechanism

Some modules are deliberately excluded from the Rack build. Exclusion lives in
three synchronized places: `WIP_SOURCES` in `rack-plugins/Makefile`, commented
`extern Model*` lines in `plugin.hpp`/`plugin.cpp`, and the `wipModules` array
in the root `plugin.json`. To enable or disable a module, move it through all
three — never just one.

Two exclusion reasons, don't conflate them:
- **WIP originals** (no upstream firmware) — excluded until reviewed.
- **License-gated**: `mod2-breakbeats` and `mod2-sample` need a generated
  `sample.h` (Patreon-gated PCM data) that is gitignored and must never be
  committed. `mod2-braids`/`mod2-tides` need un-vendored Mutable Instruments
  libraries (install steps in the root `README.md`). CI tolerates these compile
  failures on purpose (`_firmware.yml` warns and skips).

## Testing / verification

There are no unit tests. The verify harness is:

- `scripts/check-vcv.fish` — builds the plugin, validates panel SVG XML, checks
  core-header purity, checks each used core is included by both a firmware and
  a Rack `.cpp`, and checks `plugin.json` slug ↔ SVG ↔ model registration.
  Run it after any porting work.
- `make dist` locally / CI — batch firmware build that skips (not aborts on)
  per-firmware failures; `make <firmware>` fails hard for a single target.

## Commands

- List firmware targets: `make list`
- Build all firmwares: `make` (output in `dist/<name>/`)
- Build one firmware: `make mod1-euclidean`
- Flash: `make upload FW=<name> PORT=<port>` (discover ports: `make board-list`)
- Build Rack plugin: `make rack` (vendored SDK) or `make rack RACK_DIR=~/Rack-SDK`
- Install into Rack: `make rack-install`; package: `make rack-dist`
- Everything: `make everything`; clean: `make clean` / `make rack-clean` / `make clean-all`
- Scaffold a Rack module: `scripts/new-vcv-module.fish <Slug> "<Name>" "<tags>" "<desc>"`
- Verify: `scripts/check-vcv.fish`

## Versioning / releases

Tag-driven (`release.yml`): `vX.Y.Z-msrack` = stable release,
`vX.Y.Z-next-msrack` = pre-release. CI derives the plugin version from the tag
and stamps `plugin.json` at build time; stable releases also move the rolling
`latest` tag. Don't hand-edit the version for a release — push a tag.

## What not to do

- Do not edit `rack-plugins/plugin.json` (build artifact) — edit the root
  `plugin.json`.
- Do not commit `sample.h`, `dist/`, `*.rpt`, `panels/**/production/`, or KiCad
  backup files — all gitignored for a reason.
- Do not add Arduino/Rack/Pico includes, doubles, or heap allocation to
  `SynthCore` headers.
- Do not vendor the Mutable Instruments libraries or any Patreon-gated content
  into the repo.
- Do not introduce a test framework, linter, or new library without being asked.
- Do not bypass the three-place WIP mechanism when enabling/disabling modules.

## When in doubt

Read the nearest sibling module (firmware + core + Rack `.cpp`) and
`rack-plugins/PORTING.md`, and match them. If no precedent exists, ask before
inventing one.
