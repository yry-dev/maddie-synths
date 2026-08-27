# m-power — two-brick Eurorack PSU, 4HP module (rev A3)

Two isolated 12V wall bricks in; +12V / -12V / +5V out on ONE keyed
Mini-Fit Jr 2x3 plus ONE standard Eurorack 16-pin bus header. Packaged as
a 4HP 3U Eurorack module (19.80 x 110.0 mm PCB, **assembled on both
faces**, **2oz outer copper required**) with per-input PTC fuses and one
light-pipe LED per rail.

Revision history: rev A2 converted everything SMD-able to LCSC-stocked
SMD at 6HP (snapshot: .history/6hp-smd-revA2/). Rev A3 took it to 4HP by
(a) replacing the second Mini-Fit with a 9mm-wide 16-pin IDC bus header,
(b) two-sided assembly (Synthrotek Power UP style), (c) dropping the
USB-A accessory jack (even two-sided, its full-width through-pad band was
2-3 part-slots more than 4HP has), and (d) 0805 LEDs + panel light pipes
(a THT LED column is a through-hole wall the board cannot afford).
Rev A3b then took the OTHER Synthrotek lesson and put the six big
discretes BACK to through-hole, standing VERTICAL on the back face
(SBR1045 axials, MF-R300 discs on edge flanking the toggle, 6.3mm radial
bulk caps): a standing THT part packs as a dot where its SMD form is a
slab, and the emptied front face became the routing freeway that finally
made the board work. C7 alone stays SMD (no back slot left below the
connectors; the front gap is too short for a standing can). SMD vs THT
here is decided per-part by geometry, not ideology.

## How this project is built

The KiCad files are GENERATED, not hand-drawn. Two generators, run in order:

1. `gen.py` (plain python3) emits the schematic (embedded symbol lib, wire
   stubs + global labels for connectivity), the .kicad_pro (power nets
   pre-classed at 2.0mm), a placeholder .kicad_pcb, and `board_data.json` —
   the sidecar that carries refs/footprints/values/nets to stage 2.
2. `pcb_gen.py` must run under **KiCad's bundled python** (it imports
   `pcbnew`):
   `/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 pcb_gen.py`
   It consumes board_data.json, zone-packs the footprints into the 6HP
   outline, pours the GND planes and routes the rails, then overwrites
   the .kicad_pcb.

To change the circuit, edit gen.py and rerun both — do not hand-edit the
.kicad_sch or .kicad_pcb, they will be overwritten. gen.py prints a
net-by-net pin report; every net must have >= 2 pins.
IMPORTANT: the generators do NOT emit README.md, BOM.csv, or CLAUDE.md —
never delete the project dir before rerunning; they write into it in place.

`kicad-cli` is not on PATH; it lives at
`/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli`.

## Circuit summary

- Wart A (12V isolated brick): tip -> F1 PTC -> SW1 pole A -> D1 SBR1045 -> +12V.
- Wart B: tip -> F2 -> SW1 pole B -> D2 -> GND; its sleeve becomes -12V
  (works ONLY with isolated Class II bricks - safety-critical assumption).
- +5V: OKI-78SR-5 from +12V rail; feeds both output connectors.
- C3 electrolytic: POSITIVE leg to GND, negative to -12V.
- Rail LEDs: D3 white = +12V, D4 pink = -12V, D5 blue = +5V. 0805 SMD
  under 3mm press-fit light pipes; the pipe holes in the panel must line
  up with the LED positions in pcb_gen.py's FRONT_POS.
- Mini-Fit pinout (project convention, put on silk):
  1=-12V 2=GND 3=+5V 4=GND 5=GND 6=+12V.
- 16-pin bus header: standard Doepfer map, 1/2=-12V (red stripe) 3..8=GND
  9/10=+12V 11/12=+5V, CV/Gate no-connect.
- SW1 = one PCB-mount DPDT toggle (C&K 7201SYCQE) wired as DPST: each pole
  uses its common (2/5) and ONE throw (3/6); the unused throws float with an
  explicit no-connect. MUST be Q (silver) contacts, 5A @ 28VDC - the B (gold)
  option is 0.4 VA and will be destroyed by rail current.

## Mechanical

A 4HP *panel* is 20.00 mm (Doepfer's table), and the PCB is **19.80 mm** —
deliberately narrower. The panel may overhang the board; the board must
never overhang the panel, or it fouls the neighbouring module. The board
is a fixed **110.0 mm** tall (HEIGHT in pcb_gen.py — the ~110mm 3U bound,
open item 1) and is **two-sided**: front face = panel furniture (jacks,
toggle, LEDs) + the SMD filter chain inside the ~9-11mm panel-to-PCB gap
(nothing on the front is taller than 7.7mm); back face = the two output
connectors (rearward), the OKI, and the reg bypasses. Total depth ~31-35mm.

The PCB carries **no mounting holes** — do not add any without rethinking
the panel attachment.

The panel is held by **three** points: the two threaded barrel-jack
bushings and the toggle's 1/4-40 bushing nut (the USB jack was the fourth
anchor before rev A3 dropped it — check panel rigidity on a real build).
SW1's body is only 8.89 mm deep, so it clears the panel gap and solders
directly to the board — its PCB position *is* a panel hole position, and
the panel generator reads it straight out of the .kicad_pcb.

Panel-facing hardware runs down the top half (jacks, toggle, two light
pipes); the -12V light pipe sits near the board bottom next to J6's -12V
pins. The outputs face REARWARD from the back face at the bottom.

Placement is corridor-driven: pcb_gen.py reserves explicit ROUTING
corridors (the back-left channel for -12V, the front-right highway for
+12V/+5V) as packer keep-outs plus router "tolls", and the router runs a
rip-up-and-retry loop with congestion history (PathFinder-lite). Every
position note in pcb_gen.py's FRONT_POS/ZONES blocks records a measured
failure that placement choice fixes — treat them as load-bearing.

Board area is the binding constraint at this width. Every part that was
dropped to make 6HP close was a deliberate trade, recorded below.

## Deliberate omissions (do not "add back" without re-checking area)

- **SMCJ13A TVS clamps** on each brick input — dropped for area. The PTC
  fuses were kept instead: the TVS relied on the PTC to trip anyway, so the
  PTC is the load-bearing half of that pair. Cost: no fast overvoltage clamp,
  so a 15-19V brick or an AC wart is caught more slowly (or not at all).
- **Reverse-polarity indicator LEDs** (per-input LED + 1N4148) — dropped.
  Reverse polarity is still *safe* (blocked by the series Schottky); you just
  get no visual "wrong brick" cue.
- **Third Mini-Fit output (J7)** — dropped; 2 outputs remain.
- A non-isolated brick on wart B **cannot be detected electrically** at all —
  isolation is not measurable from the DC jack. Only mechanical/procedural
  defenses apply (matched bricks, silk warning).

## Validation conventions

- After regenerating: `kicad-cli sch erc` and `kicad-cli pcb drc`.
- **Current state (rev A3b, 4HP vertical-THT): 12 of 13 nets
  machine-routed and collision-verified; N_SA is the deliberate
  hand-finish net (SW1.3 -> D1 anode, one ~20mm airwire).**
  - Board 19.80 x 110.0 mm, two-sided, seed 407 (deterministic).
  - ERC: 25 `lib_symbol_issues` warnings - the known headless artifact of
    the embedded `pup` lib. 0 real errors.
  - DRC: **1 unconnected item** (N_SA's airwire - expected until the
    hand-finish; see README), **0 shorts, 0 clearance violations, 0 mask
    bridges, 0 courtyard violations**, 2 `starved_thermal` (J5's and
    C3's GND pins: one thermal spoke to an orphan island each, but both
    are THT and reach the opposite pour - minor), 31 silkscreen
    cosmetics (THT outlines crowding at 4HP).
  - Power-path widths are CURRENT RATINGS on the REQUIRED 2oz copper:
    +/-12V and the switch-cluster nets floor at 1.0mm (~4A) with a 0.8mm
    (~3.2A) last resort on the cluster; +5V runs 0.8/0.6mm because the
    OKI caps it at 1.5A. Do not lower floors further and do not order 1oz.
  - The router self-verifies: every attempt's geometry is checked for
    inter-net collisions and offenders are stripped and re-routed (or
    reported FAILED) - a rare grid-model leak once let two nets overlap
    by 0.05mm, and DRC `shorting_items` must stay at 0.
- What matters: **0 shorts, 0 clearance violations, 0 copper-to-edge, and
  exactly ONE unconnected item (VIN_B) before the hand-finish.**

## Known pcbnew scripting landmines

All three of these segfault rather than raising, and cost real debugging time:

1. `ZONE.SetOutline(poly)` does **not** take ownership of the SHAPE_POLY_SET.
   Python garbage-collects it and the zone is left holding a dangling
   pointer. Use `ZONE.AddPolygon(chain)`, which copies.
2. `ZONE_FILLER.Fill()` needs `board.BuildConnectivity()` to have run.
3. `ZONE_FILLER.Fill()` also crashes on a board from `CreateEmptyBoard()` the
   moment a zone has pads under it: `knockoutThermalReliefs` asks the DRC
   engine for each pad's zone-connection rule and the engine has no rules
   loaded. Fix: `SaveBoard()` then `LoadBoard()` before filling. This is why
   pcb_gen.py writes the board, reloads it, fills, and writes it again.
4. `board.Remove(track)` leaves the connectivity graph holding a dangling
   pointer. pcb_gen.py therefore computes routing entirely in Python and only
   materialises tracks once a width has succeeded - never add-then-remove.

## SMD-specific router behaviour (rev A2)

- Pads carry their real copper layers: THT pads block/connect both layers,
  SMD pads only F.Cu. Treating an SMD pad as two-layer lets the router
  "connect" from B.Cu where there is no copper - the net is open on the
  real board while every report says routed.
- Every SMD GND pad gets a **stitch via at its own centre** (via-in-pad),
  appended to via_pts BEFORE routing so other nets avoid it. The F.Cu pour
  drops orphan islands, so without the via an SMD GND pad can sit on
  removed copper; the via reaches the solid B.Cu plane directly.

## Open items

1. **Hand-route N_SA in pcbnew after every regeneration** (SW1's pole-A
   throw -> D1's anode; one ~20mm airwire, ~1 minute with push-and-shove
   at >=1.0mm). This is the routine finishing step, not a defect - the
   4HP two-sided board is one net beyond what the grid router can
   negotiate (~1000 attempts across every configuration tried, always
   exactly one short of closure; the searcher deliberately strands an
   easy cluster hop rather than a rail - see HAND_COST in pcb_gen.py).
2. **Verify the ~110mm rail clearance by measurement.** The board is now
   exactly 110.0mm - the uncited bound itself. Measure the case; if real
   clearance is less, HEIGHT must shrink and placement re-fought.
3. Panel: `panels/m-power/` still reflects the 6HP layout. Regenerate
   from the new .kicad_pcb - 4HP blank, jack/toggle holes from FRONT_POS,
   three 3mm light-pipe holes (two mid-right, one in the J5/J6 gap band).
4. Verify footprints against purchased parts: Mini-Fit 5566, the 16-pin
   shrouded header (mark the red-stripe end on silk), PC722A (settled,
   vendor footprint), SW1 (grid from C&K datasheet p.A-10), 3mm light
   pipes vs the panel-to-PCB gap.
5. LCSC picks needing verification (parts search was down during the
   redesign): C7 (47uF 63V 6.3mm SMD can), C1/C3 (100uF 50V 6.3x11
   radial THT). F1/F2 (MF-R300, C208487) and D1/D2 (SBR1045SD1-T,
   Mouser) are the proven 6HP parts again.
6. Same for the 0805 LEDs (white/pink/blue) and light pipes.
7. D1/D2 dissipate ~1.4W each at the full 3A hold current in SMC
   packages; 2oz copper + pours are the heatsink. Sanity-check thermally
   on the first real build at sustained load.
8. The fab/ directory and m-power-gerbers.zip are STALE 6HP artifacts -
   regenerate after the VIN_B hand-finish, and only then.
9. BOM margin philosophy stands: C3 orientation (+ to GND), 50V/63V
   electrolytic ratings - do not "optimize" down.
10. Nothing in this project is committed to git; the 6HP design lives
    only in `.history/6hp-smd-revA2/`.
