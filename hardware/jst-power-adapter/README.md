# JST power adapters

Two small boards that feed one Eurorack module from a 5-pin JST PH cable.
The cable plugs into the top. A female socket on the bottom pushes straight
down onto the module's power header.

- `16pin/` fits a 16-pin header: +12V, -12V and +5V. Board 22.6 x 17.1 mm.
- `10pin/` fits a 10-pin header: +12V and -12V. Board 20.8 x 17.1 mm.

## Pinouts

J1, JST PH 5-pin (rev B, also on the silk):

| Pin | 1 | 2 | 3 | 4 | 5 |
|---|---|---|---|---|---|
| Rail | -12V | GND | +12V | GND | +5V |

This is the same map as the bus board headers (`hardware/eurorack-v2`,
J1-J20), so wire each cable pin n to pin n.

Both faces carry pin labels. The top silk names the J1 pins. The bottom
silk names the J1 pins again and each J2 socket column ("G" = GND, "NC" =
open on this board), and gives the board name and revision.

### Rev A boards

The bottom silk gives the revision. Rev A has pins 1 and 3 the other way
round: pin 1 = +12V and pin 3 = -12V, and its top silk reads
"+12 G -12 G +5". For a rev A adapter, cross the wires on pins 1 and 3 at
one end of the cable. With a straight cable, a rev A adapter gives the
module no +12V and no -12V, because the series diodes block both rails.
Nothing is damaged.

J2, the bottom socket, uses Doepfer numbering. Socket pin n meets module
header pin n.

| Socket pins | 16-pin board | 10-pin board |
|---|---|---|
| 1, 2 | -12V (red stripe) | -12V (red stripe) |
| 3, 4 | GND | GND |
| 5-8 | open | GND |
| 9, 10 | +12V | +12V |
| 11, 12 | +5V | - |
| 13-16 | open (CV/Gate) | - |

The 10-pin board does not use J1 pin 5.

Every rail goes through a series Schottky diode (D1 +12V, D2 -12V, D3 +5V).
A cable with swapped wires then cannot drive current backwards into the
module. On -12V the diode points toward the JST, because current flows out
of the module into the -12V supply. The diodes drop about 0.35V at module
currents, so the +5V output is about 4.6V. There are no capacitors, LEDs or
fuses: the bus board has a PTC fuse on each output.

## Plugging in the wrong way round

Line up the -12V arrow on the silk with the red stripe on the module.

**16-pin board.** A 180-degree mistake is safe. GND is on socket pins 3/4
only, and pins 5-8 are open. Turned round, adapter pin n meets module pin
17-n:

- adapter +12V and +5V meet module GND (pins 8-5);
- adapter GND meets module CV (pins 14, 13);
- adapter -12V meets module Gate (pins 16, 15);
- the module's own rail pins meet open adapter pins.

The supply GND never reaches the module GND, so no current has a return
path. +12V and +5V do meet on the module GND plane, but D3 stops +12V from
feeding back into the +5V output. This assumes the module leaves bus CV and
Gate unconnected, as most modules do.

**10-pin board.** A 180-degree mistake swaps +12V and -12V, and no wiring
can prevent that. The board has a notch in its edge and a silk arrow at the
-12V end. Check them before you apply power.

## BOM (per board)

| Ref | Part | LCSC | Qty 16-pin | Qty 10-pin |
|---|---|---|---|---|
| J1 | JST B5B-PH-K-S, PH 2.0mm 5-pin vertical | C157993 | 1 | 1 |
| J2 | Female socket 2x8, 2.54mm, straight (BOOMELE) | C30734 | 1 | - |
| J2 | Female socket 2x5, 2.54mm, straight, 8.5mm (ZHOURI 2.54-2*5) | C2977587 | - | 1 |
| D1-D3 | SS34 Schottky 3A 40V, SMA (MDD) | C8678 | 3 | 2 |

Solder J2 on the **bottom** face. Everything else goes on the top.

## JLCPCB assembly

Every part has an `LCSC Part #` field, plus `Manufacturer` and `MPN` fields copied from its JLCPCB part page. JLCPCB lists the JST header and both sockets as Extended parts, so each adds JLCPCB's Extended-part fee. The SS34 page did not give its class. Check stock before you order: the part pages did not show stock levels.

The toolkit writes the files for each panel to its own folder in
`panel/production/`: the Gerber zip, `bom.csv` and `positions.csv`. Git
ignores `production/`, so make the files again before you order.

To make the files, use the KiCad JLCPCB fabrication toolkit (Bouni). Each project folder has a `fabrication-toolkit-options.json` with the same settings as `eurorack-v2`. The toolkit writes the Gerbers, BOM and placement (CPL) files from these fields.

- J2 is a through-hole part on the **bottom** face, so choose assembly on both sides, or leave J2 off the order and solder it yourself.
- J1 and J2 are through-hole parts, so JLCPCB solders them in its through-hole process.
- The diodes use hand-solder SMA pads. These are larger than standard pads, and JLCPCB can assemble them.
- In JLCPCB's placement preview, check that each diode's cathode band is on the side marked on the silk, and that J1's pin 1 is at the silk triangle. Correct any wrong rotation in the preview before you confirm the order.

## Panels

`panelize.py` puts 12 copies of one board (3 columns x 4 rows) on a panel with mouse-bite tabs, rails, tooling holes and fiducials. Each panel is under 100 x 100 mm, so it fits JLCPCB's cheapest size tier. The settings are in `panel-preset.json`. The script docstring explains each setting.

Finish the board and silkscreen first: the script reads the boards as they are now. It needs KiKit in KiCad's Python:

```sh
KP=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
$KP -m pip install --user kikit                  # once per machine
$KP panelize.py                                  # both boards -> panel/
$KP panelize.py --boards 16 --rows 3 --cols 3    # one board, smaller grid
```

Run DRC on each panel after any board change. These two warning types are expected, because the bottom tabs sit next to J1: `npth_inside_courtyard` (mouse-bite holes under the J1 housing) and `silk_over_copper` (JLCPCB clips the J1 outline at the holes). A `hole_clearance` error means a mouse-bite hole is too near a track: move the track or the tab.

## Mechanical notes

- J2 drops into the module's shrouded box header. The J1 pins stick out
  below the board, so they sit beside the shroud with 0.5mm or more to
  spare (shroud taken as 9.0mm wide).
- The C30734 socket body is 8.5mm tall. Some box headers are about 9mm
  tall with a floor inside, so the socket can reach the floor before the
  board reaches the shroud top. This does no harm, but check it on a real
  header.

## Build

The KiCad files are generated. Do not hand-edit the `.kicad_sch` or
`.kicad_pcb`: the next run overwrites them. Both scripts write in place and
leave this README alone.

```sh
cd hardware/jst-power-adapter
python3 gen.py      # schematics, projects, lib tables, board_data.json
/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3 pcb_gen.py
```

`gen.py` prints every net with its pins. `pcb_gen.py` stops if a track end
misses its pad or if the socket's pin 1 is not at the -12V end.

Check both boards (`kicad-cli` is not on PATH):

```sh
KCLI=/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli
for n in 16 10; do
  $KCLI sch erc ${n}pin/jst-adapter-$n.kicad_sch
  $KCLI pcb drc --schematic-parity ${n}pin/jst-adapter-$n.kicad_pcb
done
```
