# m-power — two-brick Eurorack PSU, 6HP module

Two isolated 12V wall bricks in, +12V / -12V / +5V out on two identical
keyed Mini-Fit Jr connectors, plus a USB-A 5V accessory jack. Built as a
6HP 3U Eurorack module: 29.80 x 108.68 mm PCB behind a 30.00 mm panel.

## Topology recap

- Wart A: tip -> PTC F1 -> switch pole A -> Schottky D1 -> +12V rail. Sleeve = GND.
- Wart B (isolated, so we may reference it freely): tip -> PTC F2 -> switch pole B
  -> Schottky D2 -> GND. Its sleeve therefore sits 12V below ground = -12V rail.
- +5V: OKI-78SR-5 buck module off the +12V rail; feeds the rail LED, the bus,
  and the USB-A jack (D+ tied to D- so devices treat it as a charger port).
- SW1 is ONE PCB-mount DPDT toggle (C&K 7201SYCQE) wired as DPST: each pole
  uses its common and one throw, and the unused throws float. It must be the
  **Q (silver) contact** option, rated 5A @ 28VDC - the otherwise-identical B
  (gold) option is rated 0.4 VA and will be destroyed by rail current.
- Rail LEDs: D3 white = +12V, D4 pink = -12V, D5 blue = +5V, all on spacers
  so they reach the front panel.

## Mini-Fit pinout (both outputs identical)

    pin 1 = -12V    pin 4 = GND
    pin 2 = GND     pin 5 = GND
    pin 3 = +5V     pin 6 = +12V

Mini-Fit housings are keyed, so cables cannot be reversed — but the pin map is
this project's own convention, not a standard. Wire the receiving end of every
cable and the input header on each distro board to the SAME map, and put the
map on the silkscreen next to each connector. Crimp one wire per pin
(Mini-Fit contacts handle far more current than these rails will see).

## Mechanical

The PCB is **29.80 x 108.68 mm** behind a **30.00 mm** panel. The panel is
deliberately the wider of the two: a board wider than its panel stands proud
behind it and fouls the neighbouring module. 29.8 mm is as narrow as this
board goes — below that the two Mini-Fit outputs stop fitting side by side and
the board grows to 131 mm, far past the 3U height.

There are **no PCB mounting holes**. The panel is held on by four parts
soldered to the board: the USB jack (J3), the two threaded barrel-jack
bushings (J1, J2), and the toggle's 1/4-40 bushing nut (SW1). The toggle's
body is only 8.9 mm deep, so it passes through the panel and still reaches the
PCB — push it through the panel, line the terminals up with the board, then
solder.

Everything you see or plug into is at the **top** of the board, in the first
~32mm: both barrel jacks, the USB, and the three rail LEDs in a straight row.
The two Mini-Fit rail outputs are alone at the **bottom**, facing the case
wiring. Panel drilling has to match those PCB positions, so design the panel
from the board, not the other way round.

## Safety rules (same as any flipped-brick design)

1. Both bricks MUST be isolated (Class II / double-insulated) REGULATED 12V
   supplies, center positive. Never use an earth-referenced supply for wart B.
2. Rails sit ~0.5V below the brick voltage due to the series Schottky - normal.
3. No current limiting beyond the bricks and the input PTCs. Power off before
   plugging distro cables.
4. Rail sequencing is not guaranteed: if one brick drops, modules see a single
   rail. Use bricks of the same type and switch both with the DPST.

## Input protection

Each brick input carries a Bourns MF-R300 PTC (3A hold) in series, ahead of
the switch. Behaviour with the wrong brick:

- **Reverse polarity:** blocked by the series Schottky, nothing conducts.
  Note there is no indicator for this — the module simply stays dark.
- **Overcurrent / shorted distro cable:** PTC heats and trips, rail collapses.
  Unplug, let the PTC cool, fix the fault.
- **15-19V brick:** the rails follow the brick. The TVS clamps that would have
  caught this were dropped to fit 6HP (see CLAUDE.md), so nothing trips
  quickly here — the OKI regulator is rated to 36V in, but the electrolytics
  and any attached modules are the exposure. **Check the brick before plugging.**
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

## Footprints to verify before ordering

- Mini-Fit Jr: assigned Molex_Mini-Fit_Jr_5566-06A2_2x03_P4.20mm_Vertical.
  Library naming for the 5566 series varies between KiCad releases - confirm
  the footprint matches your exact housing and check header height against
  your case clearance.
- USB-A: assigned USB_A_Molex_105057_Vertical. It MUST be an upright/vertical
  receptacle so the port faces forward through the panel — an edge-mount
  "horizontal" part would aim sideways into the case. Substitute to match what
  you buy, but keep the orientation.
- Barrel jacks J1/J2 and PTCs F1/F2 are settled (PC722A vendor footprint,
  MF-R300 hand-built from the datasheet).

## Smoke test

1. Wart A only, switch on: white +12 LED lights, +11.4 to +11.7V on pin 6 vs pin 2.
2. Add wart B: pink -12 LED, -11.4 to -11.7V on pin 1.
3. Blue +5 LED, 5.0V on pin 3 and on USB VBUS.
4. Load test one output with a small module before wiring the whole case.
