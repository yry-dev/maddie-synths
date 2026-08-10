# m-power — two-brick Eurorack PSU, 6HP module (rev A1)

Two isolated 12V wall bricks in; +12V / -12V / +5V out on two keyed Mini-Fit
Jr 2x3 connectors, plus a USB-A 5V accessory jack. Packaged as a 6HP 3U
Eurorack module (29.80 x 108.68 mm PCB) with per-input PTC fuses and one
panel LED per rail.

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
- +5V: OKI-78SR-5 from +12V rail; feeds bus + USB-A (D+ tied to D-).
- C3 electrolytic: POSITIVE leg to GND, negative to -12V.
- Rail LEDs: D3 white = +12V, D4 pink = -12V, D5 blue = +5V. Panel-mounted
  on spacers, so their PCB positions must line up with the panel holes.
- Mini-Fit pinout (both identical, project convention, put on silk):
  1=-12V 2=GND 3=+5V 4=GND 5=GND 6=+12V.
- SW1 = one PCB-mount DPDT toggle (C&K 7201SYCQE) wired as DPST: each pole
  uses its common (2/5) and ONE throw (3/6); the unused throws float with an
  explicit no-connect. MUST be Q (silver) contacts, 5A @ 28VDC - the B (gold)
  option is 0.4 VA and will be destroyed by rail current.

## Mechanical

A 6HP *panel* is 30.00 mm (Doepfer's table), and the PCB is **29.80 mm** —
deliberately narrower. The panel may overhang the board; the board must never
overhang the panel, or it fouls the neighbouring module. 29.8 is the narrowest
the board can go: see the WIDTH note in pcb_gen.py, where SIDE >= 2.4 (rail
routing) and usable >= 24.90 mm (the two Mini-Fit outputs sharing one shelf)
together set the floor. Below it the outputs wrap and the board jumps to
131 mm.

The PCB carries **no mounting holes** — do not add any without rethinking the
panel attachment.

The panel is held by **four** points: the USB jack, the two threaded
barrel-jack bushings, and the toggle's 1/4-40 bushing nut. SW1 is a PCB-mount
toggle whose body is only 8.89 mm deep, so it clears the panel-to-PCB gap and
solders directly to the board — its position on the PCB *is* a panel hole
position, and the panel generator reads it straight out of the .kicad_pcb.
(The earlier snap-in rocker could not do this: its body projects 15-20 mm,
well past the gap, so it had to hang off flying leads and carried no load.)

**Everything the user sees or plugs into is at the TOP of the board**, inside
the first ~32mm: both barrel jacks, the USB, and the three rail LEDs in an
aligned row. The Mini-Fit rail outputs sit alone at the BOTTOM, the opposite
end, because they face the case wiring rather than the player.

F1/F2/SW1 are **glued into one cluster** and the two PTC discs are rotated 90
to make that cluster fit the strip. This is load-bearing, not tidiness: SW1 is
a hub carrying four power nets, and when the packer was free to scatter its
neighbours those nets would not route at full width at all. See the ZONES
comment in pcb_gen.py.

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
- **Current state: ERC 0 errors, DRC 0 errors of substance.**
  - Board 29.80 x 108.68 mm. All 14 nets route, 0 unconnected.
  - ERC's 26 `lib_symbol_issues` warnings are a headless artifact of the
    embedded `pup` symbol lib and are expected.
  - Board DRC reports 30 violations: 25 `silk_overlap` + 3 `silk_over_copper`
    (reference designators and barrel-jack outline circles crowding at 6HP,
    cosmetic), plus **2 `starved_thermal` on J3's USB shield pads** - see
    Open items, this one is real but minor.
  - Panel DRC reports 6 `text_thickness` warnings. These are NOT a defect and
    NOT specific to this panel: KiCad emits them for every Comfortaa string
    on every panel in this repo (blank-6hp has 4 of them). Do not chase them,
    and do not expect a panel to reach literal zero.
  - Every power-path net routes at >= 1.5mm; see NET_WIDTHS in pcb_gen.py for
    why that floor exists.
  - What matters: **0 unconnected items, 0 clearance violations, 0 shorts,
    0 copper-to-edge violations.**
- kiutils round-trip parse was used as a pre-KiCad sanity check (see history).

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

## Open items

1. **Verify the ~110mm rail clearance by measurement.** `pcb_gen.py` warns
   above 110mm, but that number has no cited source. The board is 108.68mm so
   it currently passes, with ~1.3mm of headroom - which is thin enough that
   the real number matters. Measure the case and record it.
2. Verify footprints against purchased parts: Mini-Fit 5566 variant and the
   USB-A. J1/J2 (PC722A), F1/F2 (MF-R300) and SW1 are settled - PC722A is the
   vendor footprint + STEP, MF-R300 is hand-built from the datasheet (inline
   pads at 5.10 mm, NOT the diagonal MF-RHT pattern), and SW1's grid is from
   C&K datasheet page A-10 (4.70mm along each column, 4.83mm between rows,
   1.85mm round holes).
3. **2 `starved_thermal` on J3's USB shield pads.** Cause is measured, not
   guessed: N_L3 (the +5V indicator, 4mA) routes in 27 segments and 3 vias,
   wrapping around the USB on both layers, because +5V is 1.5mm, routes 3rd,
   and takes the channel D5/R3 need. That detour fences the shield pads onto a
   copper puddle that cannot reach the main pour. Functionally minor - the
   pads are THT so they still reach the opposite-layer plane - but the N_L3
   sprawl is ugly and worth fixing. Measured candidates: flipping R3 gets
   N_L3 to 5 segments and 0 starved but breaks N_FB/N_SB; routing the LED
   nets before +5V gets 0 starved but breaks +5V. Neither is free yet.
4. The PC722A STEP model orientation is wrong in the 3D view (barrel points
   sideways) - pads are correct; needs a `(rotate)` fix in the .kicad_mod.
5. The panel is generated (`panels/m-power/`), but three of its numbers are
   still provisional: the **USB-A cutout 13.6 x 6.4mm** (measure the real
   shell - it is a routed cutout, not a drill, so undersized cannot be filed
   out neatly), the 6.6mm toggle hole vs the datasheet's 6.35mm, and whether
   the barrel jacks at panel y=18.2mm clear the top rail in the real case.
6. Routing is machine-generated by a grid maze router. It is DRC-clean and
   the rails are at full netclass width, but it is not hand-tuned: expect
   staircase corners and no deliberate star-grounding. Rip up and re-route by
   hand in pcbnew if you want it pretty.
7. `R3` is 680R, sized for a red LED. With the blue +5V LED (Vf ~3.2V) D5
   draws ~2.6mA vs ~3.8mA for the other two. 470R would brightness-match.
   Flagged previously, deliberately not changed.
8. BOM.csv carries LCSC/Mouser picks; C3 orientation and the 50V/100V
   ratings are deliberate (margin philosophy) - do not "optimize" them down.
9. Nothing in this project is committed to git yet.
