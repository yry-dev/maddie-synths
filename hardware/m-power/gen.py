#!/usr/bin/env python3
"""KiCad 8 project: two-brick Eurorack PSU, 6HP module, 2x Mini-Fit Jr outputs."""
import uuid, os, json

def uid():
    return str(uuid.uuid4())

ROOT = uid()
PROJ = "eurorack-psu-twobrick"
# Write in place: emit into the directory this script lives in (see CLAUDE.md).
OUT = os.path.dirname(os.path.abspath(__file__))

F = "(effects (font (size 1.27 1.27)))"
FH = "(effects (font (size 1.27 1.27)) (hide yes))"

def twopin(name, body, p1name="~", p2name="~", extra=""):
    return f'''    (symbol "pup:{name}" (pin_numbers hide) (pin_names (offset 0.254)) (exclude_from_sim no) (in_bom yes) (on_board yes)
      (property "Reference" "{name[0]}" (at 0 3.81 0) {F})
      (property "Value" "{name}" (at 0 -3.81 0) {F})
      (property "Footprint" "" (at 0 0 0) {FH})
      (property "Datasheet" "" (at 0 0 0) {FH})
      (symbol "{name}_0_1"
{body}{extra}      )
      (symbol "{name}_1_1"
        (pin passive line (at -5.08 0 0) (length 2.54) (name "{p1name}" {F}) (number "1" {F}))
        (pin passive line (at 5.08 0 180) (length 2.54) (name "{p2name}" {F}) (number "2" {F}))
      )
    )
'''

RECT = '        (rectangle (start -2.54 1.016) (end 2.54 -1.016) (stroke (width 0.254) (type default)) (fill (type none)))\n'
CAPB = ('        (polyline (pts (xy -0.762 2.032) (xy -0.762 -2.032)) (stroke (width 0.3048) (type default)) (fill (type none)))\n'
        '        (polyline (pts (xy 0.762 2.032) (xy 0.762 -2.032)) (stroke (width 0.3048) (type default)) (fill (type none)))\n')
PLUS = ('        (polyline (pts (xy -2.286 1.778) (xy -1.27 1.778)) (stroke (width 0.2) (type default)) (fill (type none)))\n'
        '        (polyline (pts (xy -1.778 2.286) (xy -1.778 1.27)) (stroke (width 0.2) (type default)) (fill (type none)))\n')
DIODEB = ('        (polyline (pts (xy 1.27 0) (xy -1.27 1.27) (xy -1.27 -1.27) (xy 1.27 0)) (stroke (width 0.254) (type default)) (fill (type none)))\n'
          '        (polyline (pts (xy 1.27 1.27) (xy 1.27 -1.27)) (stroke (width 0.254) (type default)) (fill (type none)))\n')
SWB = ('        (circle (center -2.032 0) (radius 0.508) (stroke (width 0.254) (type default)) (fill (type none)))\n'
       '        (circle (center 2.032 0) (radius 0.508) (stroke (width 0.254) (type default)) (fill (type none)))\n'
       '        (polyline (pts (xy -1.524 0.254) (xy 1.778 1.778)) (stroke (width 0.254) (type default)) (fill (type none)))\n')

def multipin(name, ref, rect, pins):
    plines = "".join(
        f'        (pin {et} line (at {x} {y} {a}) (length 2.54) (name "{pn}" {F}) (number "{num}" {F}))\n'
        for (num, pn, x, y, a, et) in pins)
    return f'''    (symbol "pup:{name}" (pin_names (offset 0.508)) (exclude_from_sim no) (in_bom yes) (on_board yes)
      (property "Reference" "{ref}" (at 0 {rect[3]+2.54} 0) {F})
      (property "Value" "{name}" (at 0 {rect[1]-2.54} 0) {F})
      (property "Footprint" "" (at 0 0 0) {FH})
      (property "Datasheet" "" (at 0 0 0) {FH})
      (symbol "{name}_0_1"
        (rectangle (start {rect[0]} {rect[3]}) (end {rect[2]} {rect[1]}) (stroke (width 0.254) (type default)) (fill (type background)))
      )
      (symbol "{name}_1_1"
{plines}      )
    )
'''

lib = "  (lib_symbols\n"
lib += twopin("R", RECT)
lib += twopin("C", CAPB)
lib += twopin("CP", CAPB, p1name="+", extra=PLUS)
lib += twopin("F", RECT)
lib += twopin("SW", SWB)
lib += twopin("D_Schottky", DIODEB, p1name="K", p2name="A")
lib += twopin("D_TVS", DIODEB, p1name="K", p2name="A")
lib += twopin("LED", DIODEB, p1name="K", p2name="A")
lib += multipin("OKI78SR5", "U", (-7.62, -5.08, 7.62, 5.08), [
    ("1", "VIN",  -10.16,  2.54,   0, "power_in"),
    ("2", "GND",  -10.16, -2.54,   0, "power_in"),
    ("3", "VOUT",  10.16,  2.54, 180, "power_out"),
])
lib += multipin("BarrelJack", "J", (-5.08, -5.08, 2.54, 5.08), [
    ("1", "TIP",  5.08,  2.54, 180, "passive"),
    ("2", "SLV",  5.08, -2.54, 180, "passive"),
])
lib += multipin("USB_A", "J", (-5.08, -7.62, 5.08, 7.62), [
    # Accessory power-OUT jack: it consumes the board rails, so VBUS/GND are
    # passive here (not power_out). Driving is asserted via PWR_FLAGs instead.
    ("1", "VBUS", -7.62,  5.08, 0, "passive"),
    ("2", "D-",   -7.62,  2.54, 0, "passive"),
    ("3", "D+",   -7.62,  0,    0, "passive"),
    ("4", "GND",  -7.62, -2.54, 0, "passive"),
    ("5", "SH",   -7.62, -5.08, 0, "passive"),
])
lib += multipin("MiniFit_2x03", "J", (-5.08, -5.08, 5.08, 5.08), [
    ("1", "P1", -7.62,  2.54,   0, "passive"),
    ("2", "P2", -7.62,  0,      0, "passive"),
    ("3", "P3", -7.62, -2.54,   0, "passive"),
    ("4", "P4",  7.62,  2.54, 180, "passive"),
    ("5", "P5",  7.62,  0,    180, "passive"),
    ("6", "P6",  7.62, -2.54, 180, "passive"),
])
# SW1: ONE sub-miniature DPDT toggle, PCB-mount, with a threaded bushing that
# passes through the front panel and is secured by a nut. That nut is load
# bearing - it is the panel's FOURTH attachment point, alongside the USB jack
# and the two barrel-jack bushings. This replaces the pair of 1x02 headers an
# earlier revision used: those needed an off-board panel rocker on flying
# leads, which carried no mechanical load at all (a snap-in rocker's body
# projects ~15-20mm behind the panel, well past the ~9-11mm panel-to-PCB gap
# set by the barrel jacks, so it cannot reach the board to be soldered).
#
# Wired as DPST from a DPDT ON-ON part: each pole uses its common and ONE
# throw. In the other position the common lands on the unused throw, which
# goes nowhere - so the rail is open. The unused throws (pins 3 and 6) are
# deliberately left floating and marked no-connect; see NC in place().
lib += multipin("SW_DPDT", "SW", (-5.08, -5.08, 5.08, 5.08), [
    ("1", "A_NC", -7.62,  2.54,   0, "passive"),   # pole A, unused throw
    ("2", "A_C",  -7.62,  0,      0, "passive"),   # pole A, common
    ("3", "A_NO", -7.62, -2.54,   0, "passive"),   # pole A, used throw
    ("4", "B_NC",  7.62,  2.54, 180, "passive"),   # pole B, unused throw
    ("5", "B_C",   7.62,  0,    180, "passive"),   # pole B, common
    ("6", "B_NO",  7.62, -2.54, 180, "passive"),   # pole B, used throw
])
# PWR_FLAG: a single power_out pin that tells ERC a rail is intentionally
# driven. '#' reference keeps it out of the BOM/board. Connects like a left
# pin (endpoint at -5.08) so the generic stub+global-label wiring reaches it.
lib += f'''    (symbol "pup:PWR_FLAG" (power) (pin_numbers hide) (pin_names (offset 0) hide) (exclude_from_sim yes) (in_bom no) (on_board no)
      (property "Reference" "#FLG" (at 0 3.81 0) {FH})
      (property "Value" "PWR_FLAG" (at 0 -3.81 0) {F})
      (property "Footprint" "" (at 0 0 0) {FH})
      (property "Datasheet" "" (at 0 0 0) {FH})
      (property "ki_keywords" "flag power" (at 0 0 0) {FH})
      (symbol "PWR_FLAG_0_1"
        (polyline (pts (xy -2.54 0) (xy -1.905 1.016) (xy -0.635 1.016) (xy 0 0) (xy -0.635 -1.016) (xy -1.905 -1.016) (xy -2.54 0)) (stroke (width 0.254) (type default)) (fill (type none)))
      )
      (symbol "PWR_FLAG_1_1"
        (pin power_out line (at -5.08 0 0) (length 2.54) (name "pwr" {F}) (number "1" {F}))
      )
    )
'''
lib += "  )\n"

PINS = {
    "R":  {"1": (-5.08, 0, "L"), "2": (5.08, 0, "R")},
    "C":  {"1": (-5.08, 0, "L"), "2": (5.08, 0, "R")},
    "CP": {"1": (-5.08, 0, "L"), "2": (5.08, 0, "R")},
    "F":  {"1": (-5.08, 0, "L"), "2": (5.08, 0, "R")},
    "SW": {"1": (-5.08, 0, "L"), "2": (5.08, 0, "R")},
    "D_Schottky": {"1": (-5.08, 0, "L"), "2": (5.08, 0, "R")},
    "D_TVS":      {"1": (-5.08, 0, "L"), "2": (5.08, 0, "R")},
    "LED":        {"1": (-5.08, 0, "L"), "2": (5.08, 0, "R")},
    "OKI78SR5": {"1": (-10.16, 2.54, "L"), "2": (-10.16, -2.54, "L"),
                 "3": (10.16, 2.54, "R")},
    "BarrelJack": {"1": (5.08, 2.54, "R"), "2": (5.08, -2.54, "R")},
    "USB_A": {"1": (-7.62, 5.08, "L"), "2": (-7.62, 2.54, "L"),
              "3": (-7.62, 0, "L"), "4": (-7.62, -2.54, "L"),
              "5": (-7.62, -5.08, "L")},
    "MiniFit_2x03": {"1": (-7.62, 2.54, "L"), "2": (-7.62, 0, "L"),
                     "3": (-7.62, -2.54, "L"), "4": (7.62, 2.54, "R"),
                     "5": (7.62, 0, "R"), "6": (7.62, -2.54, "R")},
    "SW_DPDT": {"1": (-7.62, 2.54, "L"), "2": (-7.62, 0, "L"),
                "3": (-7.62, -2.54, "L"), "4": (7.62, 2.54, "R"),
                "5": (7.62, 0, "R"), "6": (7.62, -2.54, "R")},
    "PWR_FLAG": {"1": (-5.08, 0, "L")},
}
TOPS = {"R": 3.81, "C": 3.81, "CP": 3.81, "F": 3.81, "SW": 3.81,
        "D_Schottky": 3.81, "D_TVS": 3.81, "LED": 3.81, "OKI78SR5": 7.62,
        "BarrelJack": 7.62, "USB_A": 10.16, "MiniFit_2x03": 7.62,
        "SW_DPDT": 7.62, "PWR_FLAG": 3.81}

symbols = []
def place(sym, ref, value, x, y, fp, nets):
    symbols.append((sym, ref, value, x, y, fp, nets))

FP_R   = "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P2.54mm_Vertical"
FP_C   = "Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm"
FP_CP  = "Capacitor_THT:CP_Radial_D10.0mm_P5.00mm"  # fits EEU-FR1H101B / EEU-FS2A470L
FP_LED = "LED_THT:LED_D3.0mm"
# D1/D2 = Diodes SBR1045SD1-T, DO-201AD axial THT. Mounted VERTICAL to save
# board area in the 6HP strip (same part, one lead folded).
FP_D   = "Diode_THT:D_DO-201AD_P5.08mm_Vertical_AnodeUp"
FP_OKI = "Converter_DCDC:Converter_DCDC_Murata_OKI-78SR_Vertical"
FP_JK  = "maddie:BarrelJack_Switchcraft_PC722A_Vertical"
FP_MF  = "Connector_Molex:Molex_Mini-Fit_Jr_5566-06A2_2x03_P4.20mm_Vertical"
# J3 must face FORWARD through the panel (the PCB sits parallel to it), so the
# USB-A has to be a vertical/upright receptacle, not an edge-facing horizontal
# one. Shield pads are named "SH" in this footprint - pcb_gen.py aliases them.
FP_USB = "Connector_USB:USB_A_Molex_105057_Vertical"
# SW1 = sub-miniature DPDT toggle, PCB-mount, 1/4-40 threaded bushing through
# the panel. Custom footprint - see hardware/lib/maddie.pretty. UNVERIFIED
# against a purchased part; confirm the pad grid before ordering (Open items).
FP_SW  = "maddie:SW_Toggle_DPDT_SubMini_PCMount"
# F1/F2 = Bourns MF-R300 (LCSC C208487); custom fp in the shared maddie lib.
FP_F   = "maddie:Fuse_Bourns_MF-R300"

R1Y, R2Y, R3Y, R4Y, R5Y = 38.1, 76.2, 114.3, 152.4, 195.58

# Row 1: wart A -> +12V
place("BarrelJack", "J1", "12V wart A", 38.1, R1Y, FP_JK, {"1": "VIN_A", "2": "GND"})
place("F", "F1", "MF-R300 3A", 63.5, R1Y, FP_F, {"1": "VIN_A", "2": "N_FA"})
place("SW_DPDT", "SW1", "DPDT toggle (wired DPST)", 88.9, round((38.1 + 76.2) / 2, 2),
      FP_SW, {"2": "N_FA", "3": "N_SA", "1": None,      # pole A: common, throw, NC
              "5": "N_FB", "6": "N_SB", "4": None})     # pole B: common, throw, NC
place("D_Schottky", "D1", "SBR1045", 114.3, R1Y, FP_D, {"2": "N_SA", "1": "+12V"})
place("CP", "C1", "100uF 50V", 139.7, R1Y, FP_CP, {"1": "+12V", "2": "GND"})
place("C", "C2", "100nF 50V", 165.1, R1Y, FP_C, {"1": "+12V", "2": "GND"})
place("R", "R1", "2.2k", 190.5, R1Y, FP_R, {"1": "+12V", "2": "N_L1"})
place("LED", "D3", "LED white +12V", 215.9, R1Y, FP_LED, {"2": "N_L1", "1": "GND"})

# Row 2: wart B -> -12V (positive tied to GND)
place("BarrelJack", "J2", "12V wart B", 38.1, R2Y, FP_JK, {"1": "VIN_B", "2": "-12V"})
place("F", "F2", "MF-R300 3A", 63.5, R2Y, FP_F, {"1": "VIN_B", "2": "N_FB"})
# (pole B of SW1 lives in row 1 - one physical switch breaks both rails)
place("D_Schottky", "D2", "SBR1045", 114.3, R2Y, FP_D, {"2": "N_SB", "1": "GND"})
place("CP", "C3", "100uF 50V (+ to GND)", 139.7, R2Y, FP_CP, {"1": "GND", "2": "-12V"})
place("C", "C4", "100nF 50V", 165.1, R2Y, FP_C, {"1": "GND", "2": "-12V"})
place("R", "R2", "2.2k", 190.5, R2Y, FP_R, {"1": "GND", "2": "N_L2"})
place("LED", "D4", "LED pink -12V", 215.9, R2Y, FP_LED, {"2": "N_L2", "1": "-12V"})

# Row 3: +5V via OKI-78SR-5 + USB
place("OKI78SR5", "VR1", "OKI-78SR-5/1.5-W36-C", 63.5, R3Y, FP_OKI,
      {"1": "+12V", "2": "GND", "3": "+5V"})
place("C", "C5", "100nF 50V", 101.6, R3Y, FP_C, {"1": "+12V", "2": "GND"})
place("C", "C6", "470nF 50V", 127.0, R3Y, FP_C, {"1": "+5V", "2": "GND"})
place("CP", "C7", "47uF 100V", 152.4, R3Y, FP_CP, {"1": "+5V", "2": "GND"})
place("R", "R3", "680", 177.8, R3Y, FP_R, {"1": "+5V", "2": "N_L3"})
place("LED", "D5", "LED blue +5V", 203.2, R3Y, FP_LED, {"2": "N_L3", "1": "GND"})
place("USB_A", "J3", "USB-A 5V out", 241.3, R3Y, FP_USB,
      {"1": "+5V", "2": "USB_DP", "3": "USB_DP", "4": "GND", "5": "GND"})

# Row 4: two Mini-Fit Jr outputs (identical pinout)
mf = {"1": "-12V", "2": "GND", "3": "+5V", "4": "GND", "5": "GND", "6": "+12V"}
place("MiniFit_2x03", "J5", "Mini-Fit out", 63.5, R4Y, FP_MF, dict(mf))
place("MiniFit_2x03", "J6", "Mini-Fit out", 127.0, R4Y, FP_MF, dict(mf))

# Row 5: ERC "driven" markers. +12V is sourced through diode D1 (a passive pin)
# and GND from the isolated bricks, so ERC needs a PWR_FLAG on each. +5V is
# already driven by VR1 VOUT and -12V has no power-input pins -> no flag needed
# (flagging +5V would re-introduce a power-output collision).
place("PWR_FLAG", "#FLG01", "PWR_FLAG", 63.5, R5Y, "", {"1": "+12V"})
place("PWR_FLAG", "#FLG02", "PWR_FLAG", 101.6, R5Y, "", {"1": "GND"})

body = []
netmap = {}
for (sym, ref, value, x, y, fp, nets) in symbols:
    x = round(38.1 + (x - 38.1) * 1.2, 2)  # widen pitch; stays on 1.27 grid
    top = TOPS[sym]
    props = (
        f'    (property "Reference" "{ref}" (at {x} {round(y-top-1.27,2)} 0) {F})\n'
        f'    (property "Value" "{value}" (at {x} {round(y+top+1.27,2)} 0) {F})\n'
        f'    (property "Footprint" "{fp}" (at {x} {y} 0) {FH})\n'
        f'    (property "Datasheet" "" (at {x} {y} 0) {FH})\n'
    )
    pin_uuids = "".join(f'    (pin "{n}" (uuid "{uid()}"))\n' for n in PINS[sym])
    body.append(
        f'  (symbol (lib_id "pup:{sym}") (at {x} {y} 0) (unit 1)\n'
        f'    (exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no)\n'
        f'    (uuid "{uid()}")\n' + props + pin_uuids +
        f'    (instances (project "{PROJ}" (path "/{ROOT}" (reference "{ref}") (unit 1))))\n'
        f'  )\n')
    for pnum, net in nets.items():
        px, py, side = PINS[sym][pnum]
        pxs, pys = round(x + px, 2), round(y - py, 2)   # pin endpoint on sheet
        if net is None:
            # Deliberately floating pin (SW1's unused throws). Without an
            # explicit no-connect ERC reports it as an unconnected-pin ERROR,
            # which would bury the real errors we do care about.
            body.append(f'  (no_connect (at {pxs} {pys}) (uuid "{uid()}"))\n')
            continue
        if side == "L":
            lx, ly = round(pxs - 2.54, 2), pys           # stub extends left
            ang, just = 180, "right"
        else:
            lx, ly = round(pxs + 2.54, 2), pys           # stub extends right
            ang, just = 0, "left"
        body.append(
            f'  (wire (pts (xy {pxs} {pys}) (xy {lx} {ly}))\n'
            f'    (stroke (width 0) (type default))\n'
            f'    (uuid "{uid()}")\n'
            f'  )\n')
        body.append(
            f'  (global_label "{net}" (shape passive) (at {lx} {ly} {ang}) (fields_autoplaced yes)\n'
            f'    (effects (font (size 1.27 1.27)) (justify {just}))\n'
            f'    (uuid "{uid()}")\n'
            f'    (property "Intersheetrefs" "${{INTERSHEET_REFS}}" (at {lx} {ly} 0) {FH})\n'
            f'  )\n')
        netmap.setdefault(net, []).append(f"{ref}.{pnum}")

titles = [
    (25.4, R1Y-15.24, "WART A: 12V isolated brick -> PTC -> SW1 pole A -> Schottky -> +12V rail"),
    (25.4, R2Y-15.24, "WART B: 12V isolated brick, positive tied to GND through SW1 pole B + Schottky; sleeve becomes -12V"),
    (25.4, R3Y-13.97, "+5V: OKI-78SR-5 buck from +12V rail, feeds rail LED, USB-A accessory jack"),
    (25.4, R4Y-13.97, "OUTPUTS: 2x Mini-Fit Jr 2x3.  Pin map (both identical): 1=-12V 2=GND 3=+5V / 4=GND 5=GND 6=+12V"),
]
for (tx, ty, s) in titles:
    body.append(
        f'  (text "{s}" (exclude_from_sim no) (at {tx} {round(ty,2)} 0)\n'
        f'    (effects (font (size 1.7 1.7) (bold yes)) (justify left))\n'
        f'    (uuid "{uid()}")\n  )\n')

sch = (
    f'(kicad_sch (version 20231120) (generator "eeschema") (generator_version "8.0")\n'
    f'  (uuid "{ROOT}")\n'
    f'  (paper "A3")\n'
    f'  (title_block (title "m-power: two-brick Eurorack PSU  +12V / -12V / +5V, 6HP, 2x Mini-Fit out") (date "2026-07-28") (rev "A")\n'
    f'    (comment 1 "Both bricks MUST be isolated Class II regulated 12V supplies, center positive")\n'
    f'    (comment 2 "SW1 = one PCB-mount DPDT toggle wired DPST; bushing+nut anchors the panel")\n'
    f'  )\n'
    + lib + "".join(body) +
    f'  (sheet_instances (path "/" (page "1")))\n'
    f')\n')

with open(f"{OUT}/{PROJ}.kicad_sch", "w") as f:
    f.write(sch)

problems = [f"NET {n} has only {p}" for n, p in netmap.items() if len(p) < 2]
print(f"{len(symbols)} symbols, {len(netmap)} nets")
for net, pins in sorted(netmap.items()):
    print(f"  {net:8s} ({len(pins)}): {', '.join(pins)}")
print("PROBLEMS:", problems if problems else "none - all nets >= 2 pins")

pro = '''{
  "board": {"design_settings": {"rules": {"min_clearance": 0.2, "min_track_width": 0.25}}},
  "boards": [],
  "libraries": {"pinned_footprint_libs": [], "pinned_symbol_libs": []},
  "meta": {"filename": "%s.kicad_pro", "version": 1},
  "net_settings": {
    "classes": [
      {"bus_width": 12, "clearance": 0.3, "line_style": 0, "name": "Default",
       "pcb_color": "rgba(0, 0, 0, 0.000)", "schematic_color": "rgba(0, 0, 0, 0.000)",
       "track_width": 0.5, "via_diameter": 0.8, "via_drill": 0.4, "wire_width": 6},
      {"bus_width": 12, "clearance": 0.4, "line_style": 0, "name": "Power",
       "pcb_color": "rgba(0, 0, 0, 0.000)", "schematic_color": "rgba(0, 0, 0, 0.000)",
       "track_width": 2.0, "via_diameter": 1.0, "via_drill": 0.5, "wire_width": 6}
    ],
    "meta": {"version": 3},
    "net_colors": null,
    "netclass_assignments": null,
    "netclass_patterns": [
      {"netclass": "Power", "pattern": "+12V"},
      {"netclass": "Power", "pattern": "-12V"},
      {"netclass": "Power", "pattern": "+5V"},
      {"netclass": "Power", "pattern": "GND"},
      {"netclass": "Power", "pattern": "VIN_*"},
      {"netclass": "Power", "pattern": "N_*"}
    ]
  },
  "pcbnew": {"last_paths": {}, "page_layout_descr_file": ""},
  "schematic": {"legacy_lib_dir": "", "legacy_lib_list": [], "meta": {"version": 1}},
  "sheets": [["%s", "Root"]],
  "text_variables": {}
}
''' % (PROJ, ROOT)
with open(f"{OUT}/{PROJ}.kicad_pro", "w") as f:
    f.write(pro)

def grline(x1, y1, x2, y2):
    return (f'  (gr_line (start {x1} {y1}) (end {x2} {y2})'
            f' (stroke (width 0.1) (type default)) (layer "Edge.Cuts") (uuid "{uid()}"))\n')
def grcircle(cx, cy, r):
    return (f'  (gr_circle (center {cx} {cy}) (end {cx+r} {cy})'
            f' (stroke (width 0.1) (type default)) (fill none) (layer "Edge.Cuts") (uuid "{uid()}"))\n')

X0, Y0, W, H = 30, 30, 50, 80
edges = (grline(X0, Y0, X0+W, Y0) + grline(X0+W, Y0, X0+W, Y0+H) +
         grline(X0+W, Y0+H, X0, Y0+H) + grline(X0, Y0+H, X0, Y0))
holes = "".join(grcircle(x, y, 1.6) for (x, y) in
                [(X0+5, Y0+5), (X0+W-5, Y0+5), (X0+5, Y0+H-5), (X0+W-5, Y0+H-5)])
pcb = f'''(kicad_pcb (version 20240108) (generator "pcbnew") (generator_version "8.0")
  (general (thickness 1.6) (legacy_teardrops no))
  (paper "A4")
  (layers
    (0 "F.Cu" signal)
    (31 "B.Cu" signal)
    (32 "B.Adhes" user "B.Adhesive")
    (33 "F.Adhes" user "F.Adhesive")
    (34 "B.Paste" user)
    (35 "F.Paste" user)
    (36 "B.SilkS" user "B.Silkscreen")
    (37 "F.SilkS" user "F.Silkscreen")
    (38 "B.Mask" user)
    (39 "F.Mask" user)
    (40 "Dwgs.User" user "User.Drawings")
    (41 "Cmts.User" user "User.Comments")
    (44 "Edge.Cuts" user)
    (45 "Margin" user)
    (46 "B.CrtYd" user "B.Courtyard")
    (47 "F.CrtYd" user "F.Courtyard")
    (48 "B.Fab" user)
    (49 "F.Fab" user)
  )
  (setup
    (pad_to_mask_clearance 0)
    (allow_soldermask_bridges_in_footprints no)
  )
  (net 0 "")
{edges}{holes})
'''
with open(f"{OUT}/{PROJ}.kicad_pcb", "w") as f:
    f.write(pcb)

# Sidecar consumed by pcb_gen.py (runs under KiCad's bundled python + pcbnew)
# to place footprints and wire nets. PWR_FLAGs (fp="") are virtual - skip them.
bd = [{"ref": r, "fp": fp, "value": v, "nets": nets}
      for (sym, r, v, x, y, fp, nets) in symbols if fp]
with open(f"{OUT}/board_data.json", "w") as f:
    json.dump({"components": bd}, f, indent=1)

print("files written to", OUT)
