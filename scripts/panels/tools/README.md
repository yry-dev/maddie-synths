# Panel tools

Scripts for working with the KiCad faceplate PCBs. Each panel lives in its own
folder at the repo-root `panels/` dir, mirroring `firmwares/<name>/` — e.g.
`panels/mod2-claves/mod2-claves.kicad_pcb` (these scripts resolve it via the repo
root, so they work from any cwd).

These panels are **pure 2D graphics** — no footprints, pads, or nets. The front
art lives on **B.Silkscreen** (mirror-drawn, so it reads correctly from the
front), exposed-copper accents on **B.Mask**, and the board outline + pot/jack
holes are cut circles on **Edge.Cuts**. The real board is **19.8 × 128.5 mm (≈4 HP)**.

Requires KiCad 9 and Inkscape installed as macOS apps (CLIs are inside the
bundles; paths are hardcoded near the top of `kicad_to_panel.py`).

## `kicad_to_panel.py` — KiCad faceplate → VCV Rack panel SVG

```bash
python3 kicad_to_panel.py ../../../panels/mod2-claves/mod2-claves.kicad_pcb Claves [out_dir]
```

Plots `B.Silkscreen,B.Mask,Edge.Cuts` (mirrored), then recolors to the house
style (dark `#221b22` bg, light `#f0e6ee` silk, gold `#c9a84c` mask bars) and
clips silk to the board outline. Output is a Rack-ready SVG sized to the true
board.

**Key gotcha:** KiCad's `--page-size-mode 2` ("board area") crops to *all*
content including silk that overhangs the edge — NOT to Edge.Cuts. So we measure
the real board from the gray Edge.Cuts geometry *inside* the plot
(`edge_bbox()`), size the panel to that, and clip silk to it (like fabrication
trims silk at the board edge). Scale just works: Rack's nanosvg reads the mm
units at 75 DPI.

## `make_panels.py` — generate new panel projects from firmware specs

```bash
python3 make_panels.py           # writes the 13 modules in MOD1/MOD2 into repo-root panels/<name>/ folders
python3 make_panels.py --force   # also overwrite panels that already exist (default: skip existing)
```

Clones the cleanest same-form-factor template (`mod2-clap` for mod2,
`mod1-dual-ad-env` for mod1 — both expose every slot: title, 3 pots, button, LED, 4
jacks) and relabels each text slot by nearest position, stripping the stale font
`render_cache` so KiCad regenerates it. Labels come from each firmware's ASCII
panel diagram (in the `.ino` header — `awk '/╔|║|╚/' firmwares/<m>/*.ino`).

**Key gotcha:** the silk is mirror-drawn, so **front-left = high PCB-x,
front-right = low PCB-x**. Diagram pairs (e.g. `I1 I2`, `F1 F2`) list front-left
first, so the left item maps to the high-x anchor. The `M1()`/`M2()` helpers
already bake this in.

To add a module: append a `M1(...)`/`M2(...)` entry keyed by `mod1-<name>` /
`mod2-<name>`.

## Known rough edges in generated panels

- Some jack labels sit on the template's fixed-width B.Mask highlight boxes;
  longer labels overflow. Resize the mask rects in KiCad.
- The widest titles (RANDOM CV, TAP TEMPO, BREAKBEAT) nearly touch the edges
  even at the shrunk 2.1 mm title font.
