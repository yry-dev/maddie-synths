"""Generates eurorack-busboard.kicad_sch (single sheet, A1)."""
# License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.

import uuid, re
import libgen, pcbgen

# The PCB is the single source of truth for pin->net assignment, so the two
# files can never disagree about which pad of a polarised part goes where.
PADNET = {}
for _f in pcbgen.footprints:
    PADNET[_f.ref] = {p[0]: p[5] for p in _f.pads if p[5]}


def PN(ref, pin):
    return PADNET[ref][pin]


def U(): return str(uuid.uuid4())

ROOT = U()
items = []
refs_used = {}

def wire(x1, y1, x2, y2):
    items.append(f'''  (wire (pts (xy {x1:.4f} {y1:.4f}) (xy {x2:.4f} {y2:.4f}))
    (stroke (width 0) (type default)) (uuid "{U()}"))''')

def label(text, x, y, rot=0, just="left"):
    items.append(f'''  (label "{text}" (at {x:.4f} {y:.4f} {rot})
    (effects (font (size 1.27 1.27)) (justify {just} bottom)) (uuid "{U()}"))''')

def junction(x, y):
    items.append(f'  (junction (at {x:.4f} {y:.4f}) (diameter 0) (color 0 0 0 0) (uuid "{U()}"))')

def text(t, x, y, size=2.0):
    items.append(f'''  (text "{t}" (at {x:.4f} {y:.4f} 0)
    (effects (font (size {size} {size})) (justify left bottom)) (uuid "{U()}"))''')


DEFAULT_FP = {}
for _s in libgen.SYMS:
    _n = re.match(r'\s*\(symbol "([^"]+)"', _s).group(1)
    _m = re.search(r'\(property "Footprint" "([^"]*)"', _s)
    DEFAULT_FP[_n] = _m.group(1) if _m else ""


def place(lib, ref, value, x, y, pins, mirror=None, fp=None):
    """pins: {number: (local_x, local_y)} in library (Y-up) coords."""
    fp = DEFAULT_FP.get(lib, "") if fp is None else fp
    u = U()
    pinblk = "\n".join(f'    (pin "{k}" (uuid "{U()}"))' for k in pins)
    m = f"\n    (mirror {mirror})" if mirror else ""
    items.append(f'''  (symbol (lib_id "eurorack_bus:{lib}") (at {x:.4f} {y:.4f} 0) (unit 1){m}
    (in_bom yes) (on_board yes) (dnp no)
    (uuid "{u}")
    (property "Reference" "{ref}" (at {x:.4f} {y - 12.7:.4f} 0)
      (effects (font (size 1.27 1.27))))
    (property "Value" "{value}" (at {x:.4f} {y + 12.7:.4f} 0)
      (effects (font (size 1.27 1.27))))
    (property "Footprint" "{fp}" (at {x:.4f} {y:.4f} 0)
      (effects (font (size 1.27 1.27)) hide))
{pinblk}
    (instances (project "eurorack-busboard"
      (path "/{ROOT}" (reference "{ref}") (unit 1))))
  )''')
    return {k: (x + v[0], y - v[1]) for k, v in pins.items()}


# library pin maps (library coordinates, Y up)
P_HDR = {str(i + 1): (-12.7, 19.05 - i * 2.54) for i in range(16)}
P_2H = {"1": (-5.08, 0), "2": (5.08, 0)}          # PTC (pins at +/-5.08)
P_2D = {"1": (-3.81, 0), "2": (3.81, 0)}          # diode, TVS, LED (pins at +/-3.81)
P_2V = {"1": (0, 3.81), "2": (0, -3.81)}          # capacitors, resistor
P_CONN4 = {"1": (-5.08, 3.81), "2": (-5.08, 1.27), "3": (-5.08, -1.27), "4": (-5.08, -3.81)}
P_CONN2 = {"1": (-5.08, 1.27), "2": (-5.08, -1.27)}
P_CONN6 = {str(i + 1): (-5.08, 6.35 - i * 2.54) for i in range(6)}
P_PAD = {"1": (-3.81, 0)}


def hstub(pt, dx, name):
    """horizontal wire from a pin plus a net label"""
    x, y = pt
    wire(x, y, x + dx, y)
    label(name, x + dx, y, 0, "left" if dx > 0 else "right")


def vstub(pt, dy, name):
    x, y = pt
    wire(x, y, x, y + dy)
    label(name, x, y + dy, 90, "left")


# ---------------------------------------------------------------- header cells
NAMES = {1: "N12", 2: "N12", 9: "P12", 10: "P12", 11: "P5", 12: "P5"}
for i in range(20):
    n = i + 1
    col, row = i % 4, i // 4
    cx, cy = 82.55 + col * 190.5, 63.5 + row * 127.0
    p = place("ER_BusHeader_16", f"J{n}", "16-pin bus", cx, cy, P_HDR)
    for k in range(1, 17):
        hstub(p[str(k)], -7.62, PN(f"J{n}", str(k)))

    fx = cx - 63.5
    for j, (suffix, rail, val) in enumerate((("A", "-12V", "1.1A PTC"),
                                             ("B", "+12V", "1.1A PTC"),
                                             ("C", "+5V", "0R5 PTC"))):
        fy = cy - 25.4 + j * 12.7
        fr = f"F{n}{suffix}"
        q = place("ER_PTC", fr, val, fx, fy, P_2H,
                  fp="eurorack_bus:ER_Chip_1206" if suffix == "C" else None)
        hstub(q["1"], -10.16, PN(fr, "1"))
        hstub(q["2"], 10.16, PN(fr, "2"))

    for j, (suffix, rail) in enumerate((("A", "+12V"), ("B", "-12V"))):
        qx, qy = cx - 63.5 + j * 25.4, cy + 25.4
        cr = f"C{n}{suffix}"
        q = place("ER_C", cr, "100n", qx, qy, P_2V)
        vstub(q["1"], -7.62, PN(cr, "1"))
        vstub(q["2"], 7.62, PN(cr, "2"))

# ---------------------------------------------------------------- input block
BX, BY = 952.5, 152.4
text("POWER ENTRY / PROTECTION", BX - 20.32, BY - 63.5, 3.0)
text("Molex Mini-Fit Jr 2x3: 1=GND 2=-12V 3=GND 4=+5V 5=GND 6=+12V", BX - 20.32, BY - 55.88, 2.0)
text("Feed ONE connector only - never both from two supplies", BX - 20.32, BY - 48.26, 2.0)

for k, ref in enumerate(("J_IN_T", "J_IN_B")):
    j = place("ER_Conn_1x06", ref, "MiniFitJr 2x3", BX + k * 50.8, BY, P_CONN6,
              fp="eurorack_bus:ER_MiniFitJr_2x03_RA")
    for pin_no in ("1", "2", "3", "4", "5", "6"):
        hstub(j[pin_no], -10.16, PN(ref, pin_no))

for k, (ref, nm) in enumerate((("W_N12", "IN_N12"), ("W_GND", "GND"),
                               ("W_P12", "IN_P12"), ("W_P5", "IN_P5"))):
    q = place("ER_WirePad", ref, "14AWG pad", BX + 114.3, BY - 15.24 + k * 10.16, P_PAD)
    hstub(q["1"], -10.16, PN(ref, "1"))

for k, (ref, a, b, val) in enumerate((("F_MN12", "IN_N12", "-12V", "2.6A PTC"),
                                      ("F_MP12", "IN_P12", "+12V", "2.6A PTC"),
                                      ("F_MP5", "IN_P5", "P5_FUSED", "1.1A PTC"))):
    q = place("ER_PTC", ref, val, BX + 177.8, BY - 25.4 + k * 12.7, P_2H)
    hstub(q["1"], -10.16, PN(ref, "1"))
    hstub(q["2"], 10.16, PN(ref, "2"))

# ideal diode: blocks reverse current into the supply when the rack is off
qq = place("ER_PMOS", "Q1", "AO3401A", BX + 177.8, BY + 25.4,
           {"1": (-5.08, 0), "2": (0, 5.08), "3": (0, -5.08)})
hstub(qq["1"], -10.16, PN("Q1", "1"))
vstub(qq["2"], -7.62, PN("Q1", "2"))
vstub(qq["3"], 7.62, PN("Q1", "3"))
rg = place("ER_R", "R_G1", "100k", BX + 152.4, BY + 25.4, P_2V)
vstub(rg["1"], -7.62, PN("R_G1", "1"))
vstub(rg["2"], 7.62, PN("R_G1", "2"))

for k, (ref, kn, an) in enumerate((("D_R1", "GND", "-12V"),
                                   ("D_R2", "+12V", "GND"),
                                   ("D_R3", "+5V", "GND"))):
    q = place("ER_D", ref, "SS54", BX, BY + 50.8 + k * 12.7, P_2D)
    hstub(q["1"], -10.16, PN(ref, "1"))
    hstub(q["2"], 10.16, PN(ref, "2"))

for k, (ref, val, kn, an) in enumerate((("D_T1", "SMBJ13A", "GND", "-12V"),
                                        ("D_T2", "SMBJ13A", "+12V", "GND"),
                                        ("D_T3", "SMBJ6.0A", "+5V", "GND"))):
    q = place("ER_TVS", ref, val, BX + 76.2, BY + 50.8 + k * 12.7, P_2D)
    hstub(q["1"], -10.16, PN(ref, "1"))
    hstub(q["2"], 10.16, PN(ref, "2"))

for k, (ref, val, pn, mn) in enumerate((("C_B1", "220u/25V", "GND", "-12V"),
                                        ("C_B2", "220u/25V", "+12V", "GND"),
                                        ("C_B3", "220u/25V", "+5V", "GND"))):
    q = place("ER_CP", ref, val, BX + 152.4 + k * 25.4, BY + 60.96, P_2V)
    vstub(q["1"], -7.62, PN(ref, "1"))
    vstub(q["2"], 7.62, PN(ref, "2"))

for k, (dref, rref, val, kn, an, rtop, rbot) in enumerate((
        ("D_L1", "R_L1", "RED", "N_L1", "GND", "N_L1", "-12V"),
        ("D_L2", "R_L2", "GRN", "GND", "N_L2", "+12V", "N_L2"),
        ("D_L3", "R_L3", "YEL", "GND", "N_L3", "+5V", "N_L3"))):
    y = BY + 101.6 + k * 30.48
    q = place("ER_LED", dref, val, BX, y, P_2D)
    hstub(q["1"], -10.16, PN(dref, "1"))
    hstub(q["2"], 10.16, PN(dref, "2"))
    r = place("ER_R", rref, "1k" if rref == "R_L3" else "3k9", BX + 50.8, y, P_2V)
    vstub(r["1"], -7.62, PN(rref, "1"))
    vstub(r["2"], 7.62, PN(rref, "2"))

for k in range(8):
    place("ER_MountingHole", f"H{k+1}", "M3", BX + 228.6, BY - 25.4 + k * 10.16, {})

text("Every header is individually fused on all three rails.", 20.32, 574.04, 2.5)
text("Rail nets: +12V / -12V / +5V / GND. CV and GATE are bussed, unfused.",
     20.32, 581.66, 2.5)

# ---------------------------------------------------------------- emit
lib_syms = []
for s in libgen.SYMS:
    name = re.match(r'\s*\(symbol "([^"]+)"', s).group(1)
    lib_syms.append(s.replace(f'(symbol "{name}"', f'(symbol "eurorack_bus:{name}"', 1))

out = f'''(kicad_sch
  (version 20230121)
  (generator "eurorack_busgen")
  (uuid "{ROOT}")
  (paper "A1")
  (title_block
    (title "Eurorack Fused Power Bus Board")
    (date "")
    (rev "A")
    (comment 1 "13 x 16-pin bus headers, individually fused on -12V / +12V / +5V")
  )
  (lib_symbols
{chr(10).join(lib_syms)}
  )
{chr(10).join(items)}
  (sheet_instances (path "/" (page "1")))
)
'''

if __name__ == "__main__":
    with open("out/eurorack-busboard.kicad_sch", "w") as f:
        f.write(out)
    print("symbols placed:", out.count("(lib_id"))
