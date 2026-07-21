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

## `make_blanks.py` — generate blank faceplates with tiled silkscreen art

```bash
python3 make_blanks.py                    # writes panels/blank-<N>hp/ for N=1,2,3,4,5,6,7,8,10,11,12 (skips existing)
python3 make_blanks.py --force            # overwrite existing
python3 make_blanks.py --only 4hp,12hp    # restrict to some sizes
```

Emits standalone KiCad 9 pure-graphics PCBs for **blank** 3U eurorack faceplates
(HP sizes 1,2,3,4,5,6,7,8,10,11,12) — no firmware, no footprints. Each is
**reversible**: a decorative seamless pattern is tiled onto **both** F.Silkscreen
and B.Silkscreen, so either face can point outward. Outline + M3-clearance round
mounting holes follow the researched Doepfer mechanical table (`.omc/autopilot/spec.md`):
height 128.5 mm, holes at y=3.0/125.5, widths per the table. The header/layers/setup
and every S-expr shape (`gr_line`, `gr_circle`, `gr_poly`) are copied from `mod2-comb`
style; the `.kicad_pro` is cloned from `mod2-clap` with `meta.filename` patched.

Patterns come from `blank-patterns/` — vendored [pattern.monster](https://pattern.monster/)
seamless tiles (normalized to plain-black geometry; see `blank-patterns/index.md`
for source URLs, tile sizes, and the per-panel assignment table). A stdlib-only
minimal SVG parser (paths incl. arcs/beziers, rect/circle/polygon, group/pattern
transforms) turns each tile into silk; the tiler scales the tile's longest side to
~11 mm, replicates it across the face, and clips polylines (Liang–Barsky) and
polygons (Sutherland–Hodgman) to a **screw band**: full width minus a 0.7 mm L/R
margin, but vertically only `y ∈ [6.5, 122.0]` — the strips above/below (where the
screw holes and mounting rail live) carry no *pattern*, giving a clean straight
band edge instead of per-hole keepout bites. Everything is **deterministic**
(seeded pattern draw + uuid5 uuids) so re-runs reproduce byte-identical files.

**Branding.** Those screw strips carry `maddie synths` in **Comfortaa** (the same
face the mod1/mod2 plates use) as `gr_text` on both silk faces — the top band
upright and the bottom band rotated 180° (reads right-side-up when the module is
mounted upside down); back-face text uses `(justify mirror)` so it reads correctly
from the back. Size tiers by width so the text clears the 3.2 mm holes and the
pattern: ≥8 HP one 1.8 mm line at the screw line between the holes, 4–7 HP one
1.25 mm line below the holes, 1–3 HP a stacked `maddie`/`synths` at 0.8 mm (tiny
but intentional). No `render_cache` is written, so KiCad regenerates the glyphs
from the installed font.

**Key gotcha:** background-covering shapes are dropped by *actual filled area*
(shoelace ≥90% of the tile), not bounding box — a thin motif that merely spans the
tile (e.g. the Japanese-ribbon fill) is kept, only true solid backgrounds go.
Stroked motifs render as many `gr_line` segments; filled motifs as `gr_poly` — so
fill patterns legitimately have far fewer silk items than stroke patterns.

To add a pattern: drop a normalized tile SVG into `blank-patterns/` as
`pattern-NN.svg` (a `<pattern>` with `width`/`height` + black geometry) and it
joins the seeded pool automatically. Tuning knobs near the top of the script:
`SCALE_OVERRIDE` (per-motif tile size), `PANEL_OVERRIDE` (force a specific
front/back pattern on a given HP — used to give the tiny 1 HP / 2 HP faces dense
motifs), and `PANEL_SCALE` (shrink tiles on the narrowest faces so the motif
repeats several times across the width). The pool (18) is smaller than the face
count (11 panels × 2 sides = 22), so the seeded draw exhausts all patterns once
before any repeat.

## Known rough edges in generated panels

- Some jack labels sit on the template's fixed-width B.Mask highlight boxes;
  longer labels overflow. Resize the mask rects in KiCad.
- The widest titles (RANDOM CV, TAP TEMPO, BREAKBEAT) nearly touch the edges
  even at the shrunk 2.1 mm title font.
