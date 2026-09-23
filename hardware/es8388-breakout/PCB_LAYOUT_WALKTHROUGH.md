# ES8388 Module — PCB Layout, step by step (KiCad)

Follow-along guide to lay out the `es8388-breakout` board by hand in Pcbnew, from a finished
schematic to JLCPCB-ready fab files. Work top-to-bottom; **checkpoints** mark good stopping points.

**Interface reminder (frozen):** the module plugs into the picoTracker Advance via two **1×9** headers.
- **J1** (digital): +3V3, GND, MCLK, BCK, LRCK, DIN, I2S_SDIN, SDA, SCL
- **J2** (analog): LINE_L, LINE_R, MIC_IN, L_AMP, R_AMP, GND, LINEOUT_L, LINEOUT_R, GND

The two header columns must sit **5.70 mm apart** (center-to-center), pin-1 rows aligned, 2.54 mm
pitch — matching mainboard J11/J12. (The mainboard's J12 was widened 6→9 to match; run *Update PCB
from Schematic* on the mainboard too, and confirm the 3 extra pins have board room.)

> ⚠️ **No keying.** Two identical 1×9 headers can be inserted swapped/reversed. Mark **pin 1** boldly
> on silk for both, keep the module's orientation obvious, and double-check before powering.

---

## Pcbnew mechanics (the "how" — read this first)

### Essential hotkeys
| Key | Action |
|---|---|
| `M` | Move footprint/item (grab, move, click to drop) |
| `R` | Rotate the thing under the cursor |
| `F` | Flip footprint to the other side of the board |
| `X` | **Route a track** (start the router) |
| `V` | Drop a **via** (while routing = switch layers) |
| `PgUp` / `PgDn` | Active layer → F.Cu (top) / B.Cu (bottom) |
| `B` | Fill/refill all copper zones |
| `Ctrl+B` | Un-fill zones (to see traces underneath) |
| `Delete` | Delete track/via/zone under cursor |
| `Backspace` | (while routing) undo last segment |
| `Esc` | Stop routing / cancel |
| `E` | Edit properties of the selected item |

### Moving parts into place
`M` over a footprint → it follows the cursor → click to drop. `R` rotates it. Type an exact position
with double-click → Position X/Y. The thin "airwires" (ratsnest) show what still needs routing.

### Routing a trace (step by step)
1. Press **`X`** (or click the green trace icon in the right toolbar) to start the router.
2. **Click a pad** to begin — a track rubber-bands from it; the ratsnest shows its target.
3. Click to drop **corner points** as you route toward the destination.
4. **Click the destination pad** (or double-click) to finish that connection.
5. Press **`Esc`** when done.
- **Change layer mid-route:** press **`V`** — it drops a via and flips you to the other copper layer;
  keep routing on the new layer. (Use this when a trace has to cross another.)
- **Switch layer without a via:** `PgUp` (top/F.Cu) / `PgDn` (bottom/B.Cu) before starting.
- **Track width** comes from the net class automatically (you set Power/Ground = 0.4 mm earlier). To
  override, pick a width in the toolbar dropdown before routing.
- **`Backspace`** undoes the last segment; **`Ctrl+Z`** undoes more.

### Ground pour / zone (step by step)
1. Select the layer for the pour — press **`PgDn`** for **B.Cu** (bottom).
2. **Place → Add Filled Zone** (or the hatched-rectangle toolbar icon).
3. Click the **first corner** of the zone → a dialog opens:
   - **Net = `GND`**, Layer = B.Cu, leave clearance/thermal defaults → **OK**.
4. Click each **corner** of a polygon around the whole board (go a bit past the edges — KiCad clips to
   Edge.Cuts). **Double-click** to close the outline.
5. It fills automatically; press **`B`** to (re)fill. Copper floods the layer on the GND net, keeping
   clearance from other nets and connecting GND pads with thermal spokes.
- Repeat on **F.Cu** (`PgUp`) if you want a top ground pour in the open areas too.
- **Always press `B` to refill after moving parts or routing** — zones don't auto-update.
- Edit a zone: click its edge → `E` (change net, clearance, thermal settings).

### Vias & stitching
- While routing, **`V`** drops a via and switches layers.
- **Stitching vias** tie the top and bottom GND pours together: route short via-only drops around the
  board, or **Place → Via**. For the QFN **exposed pad (EP)**, drop ~4–9 vias *inside* the pad so it
  connects down to the bottom GND pour (select the EP, add vias over it).

### Refill + check loop
After any placement/routing change: **`B`** (refill zones) → **Inspect → Design Rules Checker → Run**.
Click a violation to jump to it. Fix, refill, re-run until clean.

---

## Phase 0 — Push the schematic to the board
1. Save the schematic (clears the lib-symbol-mismatch warning).
2. **Tools → Update PCB from Schematic** (F8) → **Update PCB**. All footprints drop in a heap with a
   ratsnest (thin lines = connections to route).
3. If any footprint is missing, fix the assignment in the schematic and re-run F8.

**Checkpoint 0:** U1 (QFN-28-EP), J1/J2 (1×9), and all R/C/L/FB footprints are on the canvas.

---

## Phase 1 — Board setup & rules
1. **File → Board Setup → Physical Stackup:** set **Copper layers = 2**.
2. **Board Setup → Design Rules → Constraints** (safe JLCPCB 2-layer values):
   - Min clearance **0.2 mm**, min track **0.2 mm**
   - Min via **0.6 mm** pad / **0.3 mm** drill
   - Min hole-to-hole **0.5 mm**
3. **Board Setup → Net Classes** — your schematic classes carried over. Set track widths:
   | Class | Track width | Via |
   |---|---|---|
   | Power, Ground | 0.4 mm | 0.6/0.3 |
   | Audio, I2C, Other (default) | 0.25 mm | 0.6/0.3 |
4. Set grid to **1.27 mm** for placement, **0.635 / 0.25 mm** for fine routing (hotkey `n`/`N` cycles).

**Checkpoint 1:** 2-layer board, DRC constraints and per-class widths set.

---

## Phase 2 — Board outline (Edge.Cuts)
1. Select the **Edge.Cuts** layer.
2. Draw a rectangle (Place → Rectangle, or the graphic line tool) for the board — start ~**26 × 24 mm**
   and adjust once parts are placed.
3. Put the **two headers along one edge** (they hang over the mainboard). Leave room on the opposite
   side/top for the QFN + passives.
4. Round or chamfer corners if you like (cosmetic).

**Checkpoint 2:** a closed Edge.Cuts outline exists (DRC later flags any gap).

---

## Phase 3 — Place the two mating headers (the critical geometry)
This is the one placement that *must* be exact, or the module won't seat.

1. Place **J1** near one edge, vertical (pins running along Y), pitch 2.54 mm.
2. Place **J2** exactly **5.70 mm** from J1 in X, **pin-1 aligned in Y**. Fastest exact method:
   - Double-click J1 → note its (X, Y). Double-click J2 → set its X = J1.X + 5.70, Y = J1.Y.
3. **Orientation:** the module sits on top of the mainboard with components up and male pins pointing
   **down** into the mainboard sockets. In top view, module pin-1 must land over mainboard pin-1 — so
   **no mirroring**; both headers same orientation as J11/J12.
4. **Verify before continuing:** open the mainboard PCB, note J11/J12 pin-1 corners, and confirm the
   module's J1/J2 pin-1 corners match when overlaid (3D viewer `Alt+3` on each helps). This is where a
   flipped/rotated mistake would hide.

**Checkpoint 3:** J1 & J2 are 5.70 mm apart, pin-1 rows aligned, orientation verified against the mainboard.

---

## Phase 4 — Place U1 and the passives
Rule: **decoupling caps as close to their pin as physically possible**, grouped by power domain.

1. **U1 (ES8388)** in the open area, rotated so its pin groups face their destinations:
   - digital pins (I2S/I2C, DVDD/PVDD) toward **J1**
   - analog pins (LIN/RIN/LOUT/ROUT, AVDD/HPVDD, refs) toward **J2**
2. **Decoupling** — hug the pins:
   - DVDD (2): 4.7 µF ∥ 0.1 µF; PVDD (3): 0.1 µF — near the digital side.
   - AVDD (17): 0.1 µF; HPVDD (16): 2× 100 µF ∥ 0.1 µF; FB1 + R1 (10 Ω) between them.
   - VREF (10), VMID (20), ADCVREF (19): 4.7 µF ∥ 0.1 µF each, tight to the pins.
3. **Series/coupling parts** on the path they belong to:
   - 33 Ω on each I2S/MCLK line, close to U1, between U1 and J1.
   - 22 Ω + 100 µF output DC-blocks between LOUT1/ROUT1 and J2.
   - 1 µF input/line-out coupling caps near their J2 pins.
   - 5.1 kΩ I2C pull-ups near U1; 0 Ω (CE) near pin 26.
4. Keep the **analog cluster** (J2 side) physically separated from the **digital cluster** (J1 side).

**Checkpoint 4:** every cap sits next to its pin; analog and digital halves are visually distinct; ratsnest looks short and untangled.

---

## Phase 5 — Ground pours & the AGND↔GND star
1. Add a **GND zone (copper pour)** on **B.Cu** covering the whole board (Place → Zone → net `GND`).
2. Optionally a second GND pour on **F.Cu** in open areas.
3. **Single-point star:** the QFN **exposed pad (EP)** is the join between analog and digital ground.
   Keep the analog-side ground and digital-side ground as one net here (you already merged them in the
   schematic), but route so digital return currents don't cross the analog section — let the EP be the
   hub. Stitch the EP down with a **via array** (4–9 vias) into the bottom GND pour.
4. Add a few **stitching vias** tying top and bottom GND together around the board.

**Checkpoint 5:** filled GND pours top/bottom (press `B` to refill), EP vias down to ground.

---

## Phase 6 — Route
Order: power → ground (mostly poured) → clocks → the rest.
1. **Power:** +3V3 from J1.1 to DVDD/PVDD; +3V3 → FB1 → +3V3A → AVDD and → 10 Ω → HPVDD. Use the
   0.4 mm Power width. Keep the HPVDD bulk caps on the amp side of the 10 Ω.
2. **Ground:** mostly handled by the pour; just ensure each ground pin has a via/stub into it.
3. **Clocks first & short:** MCLK, BCK, LRCK — keep them short and direct, ground nearby. MCLK is the
   fastest; give it the cleanest path.
4. **I2S data, I2C, then analog.** Keep the **analog audio traces away from the digital clocks**;
   don't run them parallel. Cross at 90° if you must.
5. Analog in/out (LINE, MIC, L_AMP/R_AMP, LINEOUT) stay on the J2 side over analog ground.

**Checkpoint 6:** ratsnest fully consumed (no thin lines left); refill pours (`B`).

---

## Phase 7 — Silkscreen
1. Label **pin 1** on both J1 and J2 clearly (a dot or "1"), plus the header names ("DIG"/"ANA") and a
   couple of key net names — insurance against the no-keying swap risk.
2. Add the board name/rev, and an orientation cue (arrow toward the mainboard edge).
3. Move any overlapping reference designators off pads (silk over pads = DRC/print issues).

**Checkpoint 7:** pin-1 and orientation obvious; no silk on pads.

---

## Phase 8 — DRC
1. **Inspect → Design Rules Checker → Run DRC.**
2. Resolve: clearance violations, unrouted nets, unconnected items, silk-over-pad.
3. Re-run until clean (or only intentional, understood warnings remain).

**Checkpoint 8:** DRC clean.

---

## Phase 9 — Fabrication outputs (JLCPCB)
Two ways:

**A) Fabrication Toolkit plugin (easiest — one click):** if you have the "Fabrication Toolkit"
(Bennymeg) plugin, click its toolbar button → it drops a `production/` folder with Gerbers, drill,
**BOM**, and **CPL/position** files already in JLC format. (The picoTracker repo already uses this.)

**B) Manual:**
1. **File → Plot:** layers F.Cu, B.Cu, F.Silkscreen, B.Silkscreen, F.Mask, B.Mask, Edge.Cuts →
   Gerber → **Plot**. Then **Generate Drill Files** (Excellon). Zip the Gerbers + drill together.
2. **File → Fabrication Outputs → Component Placement (.pos)** → CSV, mm, separate files → this is the
   **CPL** for assembly.
3. **BOM:** export a BOM with the **LCSC** field (from the schematic) — JLC needs MPN/LCSC per line.
4. Upload the Gerber zip to JLCPCB; for assembly, add the BOM + CPL and match the LCSC parts.

**Checkpoint 9:** a zip of Gerbers+drill (and BOM+CPL if assembling) ready to upload.

---

## Order-time sanity checks
- **Header mate:** overlay the module vs mainboard in 3D — pin-1 to pin-1, 5.70 mm spacing, correct
  orientation. (Two 1×9 = no keying, so this is on you.)
- **Board space on the mainboard:** J12 grew 6→9; confirm the 3 extra pins don't collide with other
  parts/traces there, and re-route if needed after *Update PCB from Schematic*.
- **DC-block caps** on L_AMP/R_AMP present (the codec HP outputs are VMID-biased).
- **EP** grounded with vias; decoupling tight to pins.
