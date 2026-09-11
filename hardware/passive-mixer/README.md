# passive-mixer — 3-into-1 resistor summing mixer

A fully passive Eurorack mixer: three inputs summed into one output through
equal series resistors, no active parts.

```
IN1 --[R1 100k]--+
IN2 --[R2 100k]--+---> MIX ---> OUT
IN3 --[R3 100k]--+
```

4 jacks (Thonkiconn PJ301M-12, TS) + 3 resistors. The series resistors are the
point: they stop the source outputs from fighting each other (a direct tie
lets a low-impedance source sink its neighbours) and turn the node into a real
sum. Inherent trade-off of passive summing — no make-up gain, so the output is
an **attenuated average** that sags further as more inputs are patched and with
the downstream input impedance. Fine for CV and casual audio; add a buffer/amp
stage for unity gain. Sleeves share a common ground; the PJ301M normalling lug
is unused.

100k is a sane default. Lower values (e.g. 10k) stiffen the node and lift the
level but load the sources harder; keep the three equal for an even mix.

## How this is built

The KiCad files are **generated, not hand-drawn**. `gen.py` (plain `python3`)
is the source of truth; it emits `passive-mixer.kicad_sch` (embedded `pup:`
symbol lib; jack → resistor → MIX bus drawn as real wires with junction dots,
plus a GND symbol) and `passive-mixer.kicad_pro`. To change the circuit, edit
`gen.py` and rerun it — do not hand-edit the `.kicad_sch`, it is overwritten:

```sh
python3 gen.py
```

It prints a net-by-net pin report; every net must have ≥ 2 pins.

## Validation

`kicad-cli` ERC is clean of real violations — the only items reported are
`lib_symbol_issues` (one per symbol), the expected headless artifact of the
embedded `pup:` symbol lib. Zero unconnected pins, zero conflicts.

There is no PCB yet — this project is schematic-only.
