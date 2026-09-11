# m-power — two-brick Eurorack PSU, 4HP module

Two isolated 12V wall bricks in, +12V / -12V / +5V out on one keyed
Mini-Fit Jr connector plus one standard Eurorack 16-pin bus header. Built
as a 4HP 3U Eurorack module: 19.80 x 110.0 mm PCB behind a 20.00 mm panel,
assembled on BOTH faces. **The PCB requires 2oz outer copper** — see
BOM.csv. The big discretes (Schottkys, PTC discs, bulk electrolytics) are
VERTICAL through-hole on the back face — a standing part packs as a dot
where its SMD form is a slab, the density lesson taken from the Synthrotek
Power UP. Small parts are SMD; the front face carries the panel furniture
and little else, which is what makes the board routable. The USB-A accessory jack of the
6HP revision is gone — even two-sided, its full-width through-pad band
did not fit 4HP (the 6HP design is snapshotted in .history/6hp-smd-revA2).

## Topology recap

- Wart A: tip -> PTC F1 -> switch pole A -> Schottky D1 -> +12V rail. Sleeve = GND.
- Wart B (isolated, so we may reference it freely): tip -> PTC F2 -> switch pole B
  -> Schottky D2 -> GND. Its sleeve therefore sits 12V below ground = -12V rail.
- +5V: OKI-78SR-5 buck module off the +12V rail; feeds the rail LED and
  both output connectors.
- SW1 is ONE PCB-mount DPDT toggle (C&K 7201SYCQE) wired as DPST: each pole
  uses its common and one throw, and the unused throws float. It must be the
  **Q (silver) contact** option, rated 5A @ 28VDC - the otherwise-identical B
  (gold) option is rated 0.4 VA and will be destroyed by rail current.
- Rail LEDs: D3 white = +12V, D4 pink = -12V, D5 blue = +5V - 0805 SMD
  LEDs under 3mm press-fit light pipes in the panel. Two pipes sit
  mid-panel right, the -12V pipe near the bottom by the output headers
  (its cathode needs a routed -12V track, and that is where -12V lives).

## Outputs

Mini-Fit Jr 2x3 (J5), this project's own convention — wire the receiving
end of every cable to the SAME map and silk it next to the connector:

    pin 1 = -12V    pin 4 = GND
    pin 2 = GND     pin 5 = GND
    pin 3 = +5V     pin 6 = +12V

Eurorack 16-pin bus header (J6), standard Doepfer map: pins 1/2 = -12V
(red stripe), 3–8 = GND, 9/10 = +12V, 11/12 = +5V, CV/Gate not connected.
It accepts a standard 16-pin ribbon straight onto a busboard.

## Mechanical

The PCB is **19.80 x 110.0 mm** behind a **20.00 mm** panel. The panel is
deliberately the wider of the two: a board wider than its panel stands
proud behind it and fouls the neighbouring module. 4HP is possible at all
because the board is assembled on both faces (Synthrotek Power UP style)
and the second Mini-Fit became a 9mm-wide 16-pin bus header.

Total depth is roughly 30–35 mm: barrel-jack standoff in front of the
board, plus the standing MF-R300 discs, radial caps, OKI and the
Mini-Fit / IDC shrouds rearward — check your case before ordering.

There are **no PCB mounting holes**. The panel is held on by three parts
soldered to the board: the two threaded barrel-jack bushings (J1, J2) and
the toggle's 1/4-40 bushing nut (SW1). With the USB gone the old fourth
anchor is gone too — check panel rigidity on the first build.

Panel-facing hardware runs down the top half: jacks, then the toggle, then
the two light pipes; the -12V pipe sits near the bottom. The outputs point
REARWARD from the back face at the bottom. Panel drilling has to match the
PCB positions, so generate the panel from the board, not the other way
round.

## Safety rules (same as any flipped-brick design)

1. Both bricks MUST be isolated (Class II / double-insulated) REGULATED 12V
   supplies, center positive. Never use an earth-referenced supply for wart B.
2. Rails sit ~0.5V below the brick voltage due to the series Schottky - normal.
3. No current limiting beyond the bricks and the input PTCs. Power off before
   plugging distro cables.
4. Rail sequencing is not guaranteed: if one brick drops, modules see a single
   rail. Use bricks of the same type and switch both with the DPST.

## Input protection

Each brick input carries a Bourns MF-R300 PTC (3A hold), standing on edge
on the back face beside the switch, in series, ahead of
the switch. Behaviour with the wrong brick:

- **Reverse polarity:** blocked by the series Schottky, nothing conducts.
  Note there is no indicator for this — the module simply stays dark.
- **Overcurrent / shorted distro cable:** PTC heats and trips, rail collapses.
  Unplug, let the PTC cool, fix the fault.
- **15-19V brick:** the rails follow the brick. The TVS clamps that would
  have caught this were dropped for area long ago (see CLAUDE.md), so
  nothing trips quickly here — the OKI is rated to 36V in, but the
  electrolytics and attached modules are the exposure. **Check the brick.**
- **12VAC transformer wart:** half-rectified into the rails; same caveat.
- **Swapped bricks between jacks:** harmless - both inputs are identical.
- **Non-isolated supply on wart B:** F2 limits ground-loop fault current, but
  this case cannot be protected at board level at all. Use matched Class II
  bricks.

## Regenerating

    python3 gen.py
    /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 pcb_gen.py

The first writes the schematic + `board_data.json`; the second places, pours
and routes the PCB from that sidecar. See CLAUDE.md before editing either.
The router is a rip-up-and-retry search and takes ~10 minutes; it is
deterministic (seed 407 committed in pcb_gen.py), so a plain rerun
reproduces the same board.

**The generator routes 12 of the 13 nets, collision-verified.** A 4HP
two-sided board is at the edge of what the grid router can negotiate;
across every configuration tried (~1000 search attempts) the best result
is exactly one net short, so one net is left as a deliberate, documented
hand-finish. With the committed seed it is **N_SA** — two pads, SW1's
pole-A throw down to D1's anode, one ~20mm airwire.
Hand-route it in pcbnew after any regeneration, before exporting gerbers
(1.0mm or wider — 2oz copper makes that ~4A vs the 3A PTC hold; pcbnew's
ratsnest shows the single airwire and the push-and-shove router closes it
in under a minute). DRC's unconnected-items check is the guard: it must
report exactly ONE airwire before the hand-finish, and zero after.

## Footprints to verify before ordering

- Mini-Fit Jr: assigned Molex_Mini-Fit_Jr_5566-06A2_2x03_P4.20mm_Vertical.
  Confirm against your exact housing; it mounts on the BACK face.
- 16-pin header: standard IDC-Header_2x08_P2.54mm_Vertical; any keyed
  DC3-16 shrouded header fits. Mark the red-stripe end on silk.
- Barrel jacks J1/J2 are settled (PC722A vendor footprint + STEP); F1/F2
  use the proven custom MF-R300 footprint (maddie lib, inline pads at
  5.10 mm, built from the datasheet).
- Light pipes: 3mm press-fit; length must match the panel-to-PCB gap.

## Smoke test

1. Wart A only, switch on: white +12 LED lights, +11.4 to +11.7V on pin 6 vs pin 2.
2. Add wart B: pink -12 LED, -11.4 to -11.7V on pin 1.
3. Blue +5 LED, 5.0V on Mini-Fit pin 3 and bus header pins 11/12.
4. Load test one output with a small module before wiring the whole case.
