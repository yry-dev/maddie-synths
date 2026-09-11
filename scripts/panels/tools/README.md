# Panel tools

Scripts for working with the KiCad faceplate PCBs. Panels are grouped by platform
under the repo-root `panels/` dir, mirroring `firmwares/` — e.g.
`panels/hagiwo-mod2/mod2-claves/mod2-claves.kicad_pcb`. The generators still take
flat panel names ("mod2-claves", "blank-6hp", "fm-boost"); `panel_paths.py` is the
single place that maps a name to its real (grouped or flat) folder, so they work
from any cwd.

These panels are **pure 2D graphics** — no footprints, pads, or nets. The front
art lives on **B.Silkscreen** (mirror-drawn, so it reads correctly from the
front), exposed-copper accents on **B.Mask**, and the board outline + pot/jack
holes are cut circles on **Edge.Cuts**. The real board is **19.8 × 128.5 mm (≈4 HP)**.

Requires KiCad 9 and Inkscape installed as macOS apps (CLIs are inside the
bundles; paths are hardcoded near the top of `kicad_to_panel.py`).

## `kicad_to_panel.py` — KiCad faceplate → VCV Rack panel SVG

```bash
python3 kicad_to_panel.py ../../../panels/hagiwo-mod2/mod2-claves/mod2-claves.kicad_pcb Claves [out_dir]
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

### The shared generic Mod2 faceplate (`res/mod2-generic.svg`)

Every Mod2 module is the same physical hardware (HAGIWO's *general-purpose drum
module*) running different firmware, so any of them can display the real generic
hardware faceplate instead of its per-module labeled panel. In VCV Rack this is
offered per-module via right-click → **Panel → Labeled / Generic hardware**
(persisted with the patch); see the `Mod2Module` / `setMod2Panel` /
`appendMod2PanelMenu` helpers in `rack-plugins/src/plugin.hpp`.

The source PCB (HAGIWO's `FrontPanel/mod2.kicad_pcb`) is vendored into the repo
at `panels/hagiwo-mod2/mod2-generic/mod2-generic.kicad_pcb` and registered in
`regen_res.py`, so it regenerates like any other panel:

```bash
python3 kicad_to_panel.py ../../../panels/hagiwo-mod2/mod2-generic/mod2-generic.kicad_pcb mod2-generic ../../../rack-plugins/res
# or, with the PNG preview step: python3 regen_res.py mod2-generic
```

It has the same B.Silkscreen / B.Mask / Edge.Cuts layers the converter expects
and renders to the same 19.8 × 128.5 mm (≈4 HP) board as the labeled panels, so
every module's hole coordinates line up under either faceplate.

## `make_panels.py` — generate new panel projects from firmware specs

```bash
python3 make_panels.py           # writes the MOD1/MOD2 modules into panels/hagiwo-mod{1,2}/<name>/ folders
python3 make_panels.py --force   # also overwrite panels that already exist (default: skip existing)
```

Clones the cleanest same-form-factor template (`mod2-clap` for mod2,
`mod1-dual-ad-env` for mod1 — both expose every slot: title, 3 pots, button, LED, 4
jacks) and relabels each text slot by nearest position, stripping the stale font
`render_cache` so KiCad regenerates it. Labels come from each firmware's ASCII
panel diagram (in the `.ino` header — `awk '/╔|║|╚/' firmwares/*/<m>/*.ino`).

**Key gotcha:** the silk is mirror-drawn, so **front-left = high PCB-x,
front-right = low PCB-x**. Diagram pairs (e.g. `I1 I2`, `F1 F2`) list front-left
first, so the left item maps to the high-x anchor. The `M1()`/`M2()` helpers
already bake this in.

To add a module: append a `M1(...)`/`M2(...)` entry keyed by `mod1-<name>` /
`mod2-<name>`.

## `make_fm_panels.py` — house-style faceplates for the free-modular kits

```bash
python3 make_fm_panels.py                    # writes panels/freemodular/fm-<slug>/ (skips existing)
python3 make_fm_panels.py --force            # overwrite
python3 make_fm_panels.py --only Boost,RNG   # restrict to some modules
python3 make_fm_panels.py --list             # feature table + source coords per module
python3 make_fm_panels.py --verify           # assert every source hole survived, exactly
python3 make_fm_panels.py --extract [--fm-repo PATH]   # refresh fm-modules.json
```

Turns each of [free-modular](https://freemodular.org)'s 12 faceplate scripts into
a normal KiCad 9 panel project that is **mechanically theirs** and
**stylistically ours**: 12 panels from 2 HP to 10 HP, each with the outline,
mounting cutouts and every hole of the original, plus our layer convention,
vertical grid, Comfortaa typography, dial tick rings, I/O semantics and signal
icons (`panels/DESIGN-RULES.md`, and see §10 there for the family's deviations).

Nothing is re-typed by hand. `--extract` imports their `faceplate_maker`, stubs
out `save()`/inkscape, and records every component each script adds with its
resolved hole centre (`position + rotated(offset) + global offset`), radius,
label and `is_output` flag. The result is cached in `fm-modules.json` (committed),
so generation needs neither the sibling checkout nor svgwrite. Hole diameters
stay theirs — jack 6.3, pot 7.3, LED 5.1, encoder 9.3, 1/4" 9.5, OLED
rect + 4× M2.4 — because the plate has to fit their PCB; only the artwork is ours.

**Editorial layer.** `MODULES` in the script carries the per-module decisions the
geometry cannot: title, which signal icons, and `labels` / `outputs` entries
keyed by *source* coordinate (nearest within 3 mm, the same trick
`make_panels.py` uses — `--list` prints the coordinates to key against). That is
where their Clock's eight unlabelled "inputs" become numbered gate **outputs**,
Drift's two bare CV jacks get their knob names, and the Quantizer's 12 buttons
get note names (their black-key mask fixes the rotation: C at 12 o'clock).

**Key gotcha:** the mod1/mod2 family is mounted back-side-out, and these layouts
are *not* left/right symmetric, so every cutout is mirrored into KiCad space
(`x_kicad = W − x_source`). Flipping the finished plate puts each hole back where
their PCB expects it. All artwork is authored directly in KiCad space, which is
why an input arrow sits at (−3.7, +2.2) from its jack in the file but reads as
lower-right on the panel — and why `left`-justified mirrored text grows toward −x.

**Text metrics.** Label placement, title fitting and every collision test need
real Comfortaa widths, so the per-character em table is a least-squares fit over
the 156 distinct (string, size) pairs whose `render_cache` is stored in the
existing mod panels (residual σ 0.5 mm). No `render_cache` is written here, so
KiCad regenerates glyphs from the installed font — edit a string and it just
re-renders.

**These are ordinary KiCad projects.** Pure graphics (`gr_text`, `gr_line`,
`gr_circle`, `gr_arc`, `gr_poly`) on `B.SilkS` / `B.Mask` / `Edge.Cuts` — open
`panels/freemodular/fm-<slug>/fm-<slug>.kicad_pcb` in pcbnew and move, retype or delete
anything. Re-runs **skip** panels that already exist, so hand edits survive;
pass `--force` only when you want the generated version back.

Deviations are printed as they happen (a divider line with no room, a credit
that had to move off its grid line, an arrow that will not fit on a 2 HP face).
DRC is clean apart from the family-normal `text_thickness` warnings — Comfortaa
is a thin TrueType face and every mod panel carries 9–14 of them too.

## `make_clk_panel.py` — the rabid.audio CLK faceplate

```bash
python3 make_clk_panel.py            # writes panels/rabid-audio-clk/ (skips existing)
python3 make_clk_panel.py --force    # overwrite
python3 make_clk_panel.py --res      # also render rack-plugins/res/rabid-audio-clk.svg
python3 make_clk_panel.py --extract  # re-read geometry from a rabid-audio checkout
python3 make_clk_panel.py --list     # print the cached geometry
```

The plate for the [rabid.audio](https://rabid.audio/projects/synth/clk/) CLK
("The Count") port — and **the one panel here that is not in the house style**.
The `fm-*` family is *their mechanics, our artwork*; this is theirs twice over,
because a port should look like the module it ports. See §11 of
`panels/DESIGN-RULES.md` for the full list of rules it breaks.

**Nothing is measured by hand.** `--extract` reads
`clock/clock/clock.kicad_pcb`, which carries the faceplate as a group of hole
footprints (`thonkiconn_hole`, `knob_hole`, `toggle_sw_hole`, `3mm_led_hole`,
`eurorack_screw_hole`) beside the circuit on the same sheet. The group is placed
at a flat +60 mm in both axes, so panel-local mm is just `pcb − 60`; that holds
for all ten holes and matches their own `panel/panel-design.svg` drill layer to
0.02 mm. The display window is derived from the three SM460281N digit footprints
(10.795 mm pitch → an 8.0 × 32.1 mm window). Cached in
`rabid-audio-clk-panel.json` (committed), so generation needs neither their
checkout nor KiCad.

Their layout *and* their dress: jacks at the top, a display that reads
vertically, black ink on bare aluminium, a halftone field, a solid ink slab round
the window, tilted Marker Felt / American Typewriter labels, and their
`rabid.audio` wordmark instead of our brand. `kicad_to_panel.convert()` grew
`bg`/`fg`/`edge` parameters for this — everything else still defaults to the
house scheme.

**Key gotchas.** Rotated text anchors sideways: everything is `(justify bottom)`,
so at 90° the glyphs grow *off* the baseline horizontally (ascent toward −x in
KiCad space) and a label anchored on its hole grows into it. And the `make_fm_panels`
em table is fitted to Comfortaa — American Typewriter is 0.84 em, Marker Felt
0.75 em; measure a render before trusting a width.

Unlike the other generators this one places every item explicitly rather than
going through `make_fm_panels.build()`: the auto-placer encodes the house grid
this panel is deliberately abandoning, and cannot express tilted or turned
labels. It shares that module's header, S-expression conventions and glyph
loader, nothing more.

## `make_blanks.py` — generate blank faceplates with tiled silkscreen art

```bash
python3 make_blanks.py                    # writes panels/blanks/blank-<N>hp/ for N=1,2,3,4,5,6,7,8,10,11,12 (skips existing)
python3 make_blanks.py --force            # overwrite existing
python3 make_blanks.py --only 4hp,12hp    # restrict to some sizes
```

Emits standalone KiCad 9 pure-graphics PCBs for **blank** 3U eurorack faceplates
(HP sizes 1,2,3,4,5,6,7,8,10,11,12) — no firmware, no footprints. Each is
**reversible**: a decorative seamless pattern is tiled onto **both** F.Silkscreen
and B.Silkscreen, so either face can point outward. Outline + M3-clearance round
mounting holes follow the researched Doepfer mechanical table (`.omc/autopilot/spec.md`):
height 128.5 mm, holes at y=3.0/125.5, widths per the table. The header/layers/setup
and every S-expr shape (`gr_line`, `gr_circle`, `gr_poly`) are copied from
`hagiwo-mod2/mod2-comb` style; the `.kicad_pro` is cloned from `mod2-clap` with
`meta.filename` patched.

Patterns come from `blank-patterns/` — [pattern.monster](https://pattern.monster/)
seamless tiles (normalized to plain-black geometry; see `blank-patterns/index.md`
for source URLs, tile sizes, and the per-panel assignment table). **Those tiles
are commercially licensed and gitignored, so they are not in a fresh checkout** —
the licence covers using the patterns, not redistributing the tile files. Supply
your own `blank-patterns/pattern-*.svg` (the index lists each source URL) or
`make_blanks.py` exits with `no vendored patterns in …` and generates nothing.
The already-generated `panels/blanks/blank-*hp/` projects are unaffected. A stdlib-only
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
