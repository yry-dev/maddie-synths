# plan: get m-power back to a buildable 6HP

Updated 2026-08-25 (rev A3: 4HP two-sided). **The board builds.** 19.80 x
110.0 mm, two-sided, 2oz copper, 13 nets (USB and its USB_DP net are gone;
J6 is now a 16-pin bus header). Historical notes below describe the 6HP
revisions - see .history/6hp-smd-revA2/ for that design. Rev A2 text: all 14
nets
routed, every power-path net at >= 1.5mm, and a matching generated panel.

This file started as a plan against a board that was 113.0mm with 4 unrouted
nets. That state is gone; what follows records what actually fixed it (because
several of the obvious moves did *not*) and what is still open.

Everything here is measured, not estimated, unless explicitly marked.

---

## 1. Where it landed

    board        19.80 x 110.0 mm 4HP   (was 29.80x101.5 6HP-SMD, 108.68 THT)
    routing      14/14 nets, 0 unrouted (was 4 unrouted)
    widths       all power nets >= 1.5mm
    board DRC    0 unconnected, 0 clearance, 0 shorts, 0 copper-to-edge
                 30 remaining: 25 silk_overlap + 3 silk_over_copper (cosmetic)
                 + 2 starved_thermal (real but minor - see Open)
    sch ERC      0 errors (26 lib_symbol_issues = headless artifact)
    panel        panels/m-power/, 6 text_thickness warnings only

Zones: panel 2.5-27.9, ledres 28.9-31.9, chain 32.9-67.9, reg5v 68.9-83.3,
output 84.3-106.2.

---

## 2. What fixed it

### Skyline packing replaced shelf packing  (113.0 -> 107.7 mm)

The old packer laid each zone out in horizontal shelves, and every shelf cost
the height of its *tallest* member. The chain zone mixed a 13.2mm switch with
5.8mm diodes and 3.5mm fuses, so the short parts each paid for the switch:
14.2mm wasted in that one zone when only ~3mm was needed. `Skyline` in
pcb_gen.py keeps a per-column height profile instead, so a short part tucks in
beside a tall one.

### The F1+SW1+F2 cluster, with the PTCs rotated 90  (fixed all 4 switch nets)

This was the one that mattered, and it took several wrong turns to find.

SW1 is a hub: four power nets (N_FA/N_FB in from the PTCs, N_SA/N_SB out to
the Schottkys) land on one 4.70 x 4.83mm pin grid. Height-sorted packing puts
its neighbours in three different height classes, so it scattered them down
the zone and stranded those nets. Rotating the PTC discs on edge turns them
from 12.6 x 3.6 into 3.6 x 12.6 - the same height class as the switch, and
narrow enough that F1+SW1+F2 is 20.7mm against the 25.0mm strip. Laid flat
that cluster is 37.9mm and `group_size()` rejects it outright.

Result: N_FA and N_FB now route in **4 segments each**.

**Measured dead ends - do not re-try these.** Every one leaves nets unrouted:

| attempt | result |
|---|---|
| rotate SW1 180 / 90 / 270 | N_FB + one N_S* still fail |
| glue SW1 to D1 or to D2 | fails *and* costs 3.1mm (110.8mm) |
| use the other throw (pins 1/4) | no better than 3/6 |
| rotate the diodes too | N_SB fails |
| reorder ROUTE_ORDER (5 orderings) | just moves which net fails |

### The PCB is now narrower than the panel  (30.48 -> 29.80 mm)

A 6HP *panel* is 30.00mm; the board was 30.48mm, so it stood 0.24mm proud on
each side behind a narrower panel and would foul the neighbouring module. An
earlier version of this file called that "harmless (it is behind the panel)" -
that was wrong.

29.8 is the floor, and both edges of the window are measured:

- `SIDE >= 2.4` - the clear channel the rails run in. At 2.3 VIN_B stops
  routing entirely.
- `usable >= 24.90mm` - what the two Mini-Fit outputs need side by side. At
  24.8 they stop sharing a shelf and the board jumps to **131.3mm**.

Together: `WIDTH >= 24.90 + 4.8 = 29.70`. 29.6 and below were confirmed to
wrap. That leaves only 0.10mm of panel overhang per side, which is inside
typical PCB outline tolerance (+/-0.15mm) - if more clearance is wanted the
only real lever is the Mini-Fit pair, since it is 27% of all part area.

### The SW1 footprint grid is no longer invented

It was built from the moulded-base dimensions because the terminal span was
not separately dimensioned on the outline drawing. The C&K 7000-series
datasheet **page A-10** has the actual PC mounting pattern for the "C"
termination: **.185in (4.70mm)** between terminals along a column, **.190in
(4.83mm)** between rows, **.073in (1.85mm) round** holes. The old footprint
had 5.08mm pitch and oval holes - both wrong; the row pitch happened to be
right. Part is **C&K 7201SYCQE**.

This mattered: correcting it changed which nets failed, which is exactly why
the earlier N_SA/N_SB analysis was flagged untrustworthy.

### Two panel bugs, both of which would have produced an unbuildable panel

1. **Cutouts were anchored on the footprint origin**, which is usually pad 1,
   not the centre of the thing that pokes through. J3's opening landed 3.5mm
   off the USB shell and 0.2mm from the panel edge; every LED hole was 1.27mm
   off its lens. Fixed with an explicit `ANCHOR` table (per-part provenance)
   plus `check_inside()`, which now refuses to emit a cutout that leaves less
   than 1.0mm of panel beside it. Nothing downstream would have caught this -
   KiCad DRC will happily cut a slot through the board edge.
2. **`madelyn.sh` was placed as if `em_text` left-aligned.** It centres, so
   half the string hung off the panel. Both branding lines are centred now.

A third, smaller one: a literal newline inside a quoted s-expression string
makes the whole .kicad_pcb unloadable, and the only symptom is `kicad-cli`
saying "Failed to load board" with no line number. `label()` escapes them now.

---

## 3. Still open

1. **Measure the rail clearance.** `pcb_gen.py:~640` warns above 110mm with no
   cited source. At 101.5mm the board passes with ~8.5mm to spare, so this
   is no longer tight. Still worth a tape measure to close it out.
2. (rev A2) The J3 starved_thermal is gone; DRC now shows 2 on D5's GND
   pad (1 spoke/layer, still connected - minor). Old note: N_L3
   (the +5V indicator, 4mA) routes in 27 segments and 3 vias, wrapping around
   the USB on both layers, because +5V is 1.5mm, routes 3rd, and takes the
   channel D5/R3 need. That fences the shield pads onto a copper puddle that
   cannot reach the main pour. Minor functionally - the pads are THT so they
   still reach the opposite-layer plane - but the sprawl is worth removing.
   Measured candidates, neither free yet:
   - flip R3: N_L3 drops to 5 segments, 0 starved, but N_FB/N_SB break
   - LED nets before +5V: 0 starved, but +5V fails to route
3. **Provisional panel numbers.** The USB-A cutout is 13.6 x 6.4mm from a
   generic shell - measure the real part, because it is a routed cutout and an
   undersized one cannot be filed out neatly. The toggle hole is 6.6mm against
   the datasheet's 6.35mm (deliberate, for fit). And confirm the barrel jacks
   at panel y=18.2mm clear the top rail in the real case.
4. Verify the Mini-Fit 5566 variant and the USB-A footprint against the parts
   actually bought. J1/J2, F1/F2 and SW1 are settled.
5. PC722A STEP orientation is wrong in the 3D view (pads are fine).
6. R3 is 680R, sized for a red LED; 470R would brightness-match the blue one.
   Deliberately unchanged.
7. Nothing here is committed to git yet.

---

## 4. Things not to reach for

Unchanged from the original plan, and all still true:

- Do **not** lower a `NET_WIDTHS` floor to make a route succeed. Those floors
  are current ratings - 1.5mm is ~4A on 1oz outer copper and the PTCs hold 3A.
- Do **not** shrink the SW1 courtyard below 0.25mm KLC clearance.
- Do **not** reduce `SIDE` below 2.4 (measured: VIN_B stops routing).
- Do **not** "optimise" the Mini-Fit outputs, the 10A Schottkys, the 3A PTCs
  or the 50V/100V electrolytics away to save area. Those are the deliberate
  margin choices in CLAUDE.md, and they are the reason this needs 6HP where a
  4HP commercial design does not.
