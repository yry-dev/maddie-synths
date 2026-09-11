# passive-mult — normalled 3.5mm multiple (buffer-less)

A fully passive Eurorack multiple whose behaviour changes with how you patch
it, using the **switched (normalling) lug** on the jacks — no op-amps, no power.

| You patch…            | You get                                          |
|-----------------------|--------------------------------------------------|
| only **J1** (top)     | signal on **all of J2…J8** → **1 → 7**           |
| **J1** and **J5**     | J1 → J2/J3/J4, J5 → J6/J7/J8 → **two 1 → 3**     |

## How it works

The eight jacks form two **hard-wired groups of four**:

- **Group 1 (bus A):** J1 *(in)* + J2, J3, J4 *(out)* — tips tied together
- **Group 2 (bus B):** J5 *(in)* + J6, J7, J8 *(out)* — tips tied together

The two groups are joined by **one wire: J5's normal lug (N) → bus A**. A mono
switched jack (PJ301M-12) shorts its N lug to its own tip *only while nothing is
plugged in*:

- **J5 empty** → N shorts to T, so **bus B = bus A**; the strip is one 8-way
  mult (**1 → 7**).
- **J5 patched** → the switch opens, bus B separates from bus A and carries
  whatever you plugged into J5 (**two 1 → 3**).

Only **J5** needs the switch. The six output jacks are hard-wired to their group
bus, so patching a cable to *take* an output never breaks the strip — the one
break point is J5, the "second input." In practice you'd fit switched jacks
everywhere (same part) and just leave the other N lugs unwired; wire more N→bus
links if you want more split points.

Still passive: **no buffering**, so a group's load is shared across its taps
(fine for gates/CV and audio into a few high-impedance inputs). Add a buffer per
output if you need isolation — the normalling logic is identical, that's the
only difference between a passive and a *buffered* mult.

Net check (from the exported netlist): bus A = `J1.T J2.T J3.T J4.T` **+ J5.N**,
bus B = `J5.T J6.T J7.T J8.T`, GND = all eight sleeves.

## How this is built

The KiCad files are **generated, not hand-drawn**. `gen.py` (plain `python3`)
is the source of truth; it emits `passive-mult.kicad_sch` (embedded `pup:`
symbol lib — a plain `Jack_TS` and a switched `Jack_TSN`; real bus wires with
junction dots and a visible "normal" bridge; a GND symbol) and
`passive-mult.kicad_pro`. To change the circuit, edit `gen.py` and rerun it —
do not hand-edit the `.kicad_sch`, it is overwritten:

```sh
python3 gen.py
```

## Validation

`kicad-cli` (at `/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli`) ERC is
clean of real violations — the only items reported are `lib_symbol_issues` (one
per symbol), the expected headless artifact of the embedded `pup:` symbol lib,
same as the m-power project. Zero unconnected pins, zero conflicts.

There is no PCB yet — this project is schematic-only.
