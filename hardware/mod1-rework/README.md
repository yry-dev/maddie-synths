# mod1 — HAGIWO generic CV/Gate module (Arduino Nano, +12V only)

KiCad schematic reverse-engineered from `hardware/mod1.webp` (the HAGIWO MOD_1
schematic screenshot). **Schematic only — no PCB yet, by request.**

## How this project is built

The `.kicad_sch` / `.kicad_pro` are **GENERATED**, not hand-drawn. `gen.py` (plain
python3) emits an embedded symbol library and a small layout engine that draws a
**real wired schematic**: functional blocks (pot channels, the F1–F4 input
front-ends, NeoPixel, button, power input) are connected with actual orthogonal
wire segments, junctions and power symbols. Only the many-pin Arduino Nano's pins
and cross-block signal hops are labelled — as a human would; you don't draw thirty
wires into an MCU. All coordinates are snapped to KiCad's 1.27 mm connection grid.

The **netlist** is reproduced faithfully — verified byte-for-byte against the
earlier label-only revision by diffing `kicad-cli sch export netlist` output
(every multi-pin net has identical pin membership). Change the circuit by editing
`gen.py` and rerunning; do not hand-edit the `.kicad_sch`, it will be overwritten.

```sh
python3 gen.py                       # regenerate mod1.kicad_sch + mod1.kicad_pro
/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli sch erc mod1.kicad_sch
```

`gen.py` prints a net-by-net pin report and hard-fails if any two pins from
different nets land on the same coordinate (a silent short) or if any net has
fewer than 2 pins. To change the circuit, edit `gen.py` and rerun — do not
hand-edit the `.kicad_sch`, it will be overwritten.

ERC is clean apart from the expected headless-only `lib_symbol_issues` warnings
(the symbol lib is embedded, with no on-disk copy for the CLI to diff against —
same benign artifact documented in `hardware/m-power/CLAUDE.md`).

## Circuit summary

- **MCU:** Arduino Nano v3.x. Powered from bus **+12V only** (VIN); its onboard
  regulator sources the internal +5V rail that feeds the pots and input clamps.
  **Socketed, not soldered** — the Nano plugs into two 1×15 2.54 mm female headers
  soldered into the `Module:Arduino_Nano` footprint (which *is* that 2×15 THT hole
  field), so it lifts out for reflashing/replacement. BOM has the header sockets,
  not the Nano itself.
- **3 pots** (RV1–3, 100k) → series 10k → filter cap 0.01u → A0/A1/A2.
- **F1–F4 flexible I/O jacks** (`AudioJack2_SwitchT`), each protected with a 1k
  series R + dual 1N5819WS Schottky clamp to +5V/GND:
  - **F1** → `A3_D17` (A3 *is* D17 on a Nano — one pin). DC-coupled, 100k pulldown, 0.01u filter.
  - **F2** → `A4_D9`, **F3** → `A5_D10`: A4↔D9 and A5↔D10 are **hardware-shorted**
    so the jack works as an analog input *or* a PWM output. Each has a 1u input
    cap to GND, 100k pulldown, 0.01u filter.
  - **F4** → `D11`. 1u input cap, clamp only (no pulldown/filter).
- **Status NeoPixel** LED1 (Adafruit 4776 RGBW Mini Button) on D3 (data). The
  Nano is 5V logic so it drives the pixel directly — no level shifter. R10 is the
  data series resistor (470R), C13 the local decoupling. Cap firmware brightness
  so the pixel's draw stays gentle on the Nano's onboard 5V regulator.
- **Push button** SW1 on D4 (internal pull-up), 10k series.
- **4th pot** RV4 (9mm, 100k) on the spare ADC-only pin **A6** (10k + 0.01u,
  same pattern as POT1–3). A7 is still free.
- **Power:** `Conn_02x8` eurorack header J5. Pins **9,10 = +12V** in (through D5
  1N4148WS reverse-protection to the internal +12V rail, C1 10u bulk); pins
  **3,4,5,6 = GND**; pins **1,2** shorted (keying); 7,8,11–16 unused. (Only +12V
  and GND are used — this is a +12V-only module.)

## Notes / assumptions

- Footprints are provisional placeholders (SMD 0805 R/C, SOD-323 diodes, etc.) so
  the symbols carry sensible metadata; verify/replace before any PCB work.
- Reference designators, values, and the pin-to-net mapping are transcribed
  directly from the source image and match the HAGIWO pin assignment in
  `hardware/mod1-description.md`.
