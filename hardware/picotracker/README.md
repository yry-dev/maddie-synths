# picotracker — picoTracker alt-PCB and Advance-lite conversion

Two KiCad projects for a handheld [picoTracker](https://github.com/democloid/picoTracker)
build. Neither started here. Both are copies of my fork of an upstream repo, and
that fork stays the working copy. Edit there, then copy back here (see "Keeping in
sync").

## Where this came from

- **Upstream:** <https://github.com/ijnekenamay/picotracker_alt-pcb> by
  ijnekenamay, MIT, 2024. It is an alternative PCB for the democloid picoTracker,
  a cheap handheld that runs LittleGPTracker. Its README carries the build notes,
  the BOM, and the photos.
- **My fork:** <https://github.com/yry-dev/picotracker_alt-pcb>, branch
  `advance-conversion`. This copy was taken from commit `c4c6534` on 2026-09-23.
  The fork's `main` is identical to upstream `main`.

## Folders

- `kicad/` — the upstream alt-PCB: main board (`pico_tracker`) and back panel
  (`pico_tracker_back`). Last fork commit `2aa7a73`. Changes from upstream:
  - All passives moved from SMD to through-hole: R1–R5 to axial DIN0207, D1 to
    DO-35, U2 (6N138) to a plain DIP-8.
  - J9, the kill switch, is now a C&K JS202011AQN right-angle DPDT slide switch.
  - Back-panel alignment holes redrawn.
  - Files saved in KiCad 10 format. BOM CSV regenerated. Interactive BOM added
    in `bom/ibom.html`.
- `kicad_advance/` — the "Advance-lite" conversion of the same board. Last fork
  commit `f3b0294`. It targets a Pimoroni Pico LiPo 2 in the existing Pico
  footprint, a 3×3 diode key matrix, and a 1×9 codec header (J12) for an ES8388
  audio module. The design plan is `ADVANCE_CONVERSION_PLAN.md` in the fork.

Reference images (`3d_BP.png`, `DIY_schematic.png`, `Pico_pinhaiti.png`,
`adafruit_products_MIDI_FW_sch.png`, `kinble.pdf`) and the `pico_tracker.csv`
BOM come from upstream unchanged unless noted above.

## Keeping in sync

Only tracked files are copied. Fabrication output, backups, and `.history/` are
ignored in both repos.

```sh
cd ~/Documents/code/music/picotracker_alt-pcb
git ls-files kicad kicad_advance \
  | rsync -a --files-from=- . ~/Documents/code/music/maddie-synths/hardware/picotracker/
```

Update the commit hashes above after each sync.

## License

MIT. See `LICENSE.md` in this folder. It keeps the upstream copyright notice, as
the MIT license requires, and adds mine for the changes.
