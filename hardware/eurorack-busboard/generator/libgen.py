"""Generates the project-local KiCad footprint (.pretty) and symbol (.kicad_sym) libraries."""
# License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.

import os, uuid, math

def U():
    return str(uuid.uuid4())

SILK = "F.SilkS"
FAB = "F.Fab"
CRT = "F.CrtYd"

def line(x1, y1, x2, y2, layer, w=0.12):
    return (f'  (fp_line (start {x1:.4f} {y1:.4f}) (end {x2:.4f} {y2:.4f})'
            f' (stroke (width {w}) (type solid)) (layer "{layer}") (tstamp {U()}))')

def rect(x1, y1, x2, y2, layer, w=0.12):
    return "\n".join([line(x1, y1, x2, y1, layer, w), line(x2, y1, x2, y2, layer, w),
                      line(x2, y2, x1, y2, layer, w), line(x1, y2, x1, y1, layer, w)])

def circle(cx, cy, r, layer, w=0.12):
    return (f'  (fp_circle (center {cx:.4f} {cy:.4f}) (end {cx+r:.4f} {cy:.4f})'
            f' (stroke (width {w}) (type solid)) (fill none) (layer "{layer}") (tstamp {U()}))')

def pad_tht(num, x, y, size, drill, shape="circle"):
    return (f'  (pad "{num}" thru_hole {shape} (at {x:.4f} {y:.4f}) (size {size[0]:.4f} {size[1]:.4f})'
            f' (drill {drill:.4f}) (layers "*.Cu" "*.Mask") (tstamp {U()}))')

def pad_smd(num, x, y, w, h):
    return (f'  (pad "{num}" smd roundrect (at {x:.4f} {y:.4f}) (size {w:.4f} {h:.4f})'
            f' (layers "F.Cu" "F.Paste" "F.Mask") (roundrect_rratio 0.25) (tstamp {U()}))')

def pad_np(x, y, d):
    return (f'  (pad "" np_thru_hole circle (at {x:.4f} {y:.4f}) (size {d:.4f} {d:.4f})'
            f' (drill {d:.4f}) (layers "F.Cu" "B.Cu" "F.Mask" "B.Mask") (tstamp {U()}))')

def fp(name, descr, tags, body, attr="through_hole", refy=-2.0, valy=2.0):
    return f'''(footprint "{name}"
  (version 20221018)
  (generator "eurorack_busgen")
  (layer "F.Cu")
  (descr "{descr}")
  (tags "{tags}")
  (attr {attr})
  (fp_text reference "REF**" (at 0 {refy} 0) (layer "F.SilkS")
    (effects (font (size 1 1) (thickness 0.15))) (tstamp {U()}))
  (fp_text value "{name}" (at 0 {valy} 0) (layer "F.Fab")
    (effects (font (size 1 1) (thickness 0.15))) (tstamp {U()}))
{body}
)
'''

# ----------------------------------------------------------------------------
# Footprint definitions
# ----------------------------------------------------------------------------
FOOTPRINTS = {}

# --- 16-pin shrouded Eurorack bus header, vertical, column axis along local Y.
#     Column 1 (-12V, red stripe) is at local y = +8.89 (i.e. "down" in board coords).
b = []
for k in range(1, 9):
    y = 8.89 - (k - 1) * 2.54
    for row, x in ((0, -1.27), (1, 1.27)):
        n = 2 * k - 1 + row
        shape = "rect" if n == 1 else "oval"
        b.append(pad_tht(n, x, y, (1.7, 1.7), 1.0, shape))
b.append(rect(-4.5, -11.7, 4.5, 11.7, SILK))
b.append(rect(-4.5, -11.7, 4.5, 11.7, FAB, 0.1))
# polarising slot indication on the +x long side
b.append(line(4.5, -2.0, 4.5, 2.0, SILK, 0.4))
# pin-1 / red stripe marker
b.append(line(-4.5, 10.6, -6.2, 10.6, SILK, 0.4))
b.append(circle(-6.9, 10.6, 0.35, SILK, 0.3))
FOOTPRINTS["ER_BusHeader_2x08_Shrouded"] = fp(
    "ER_BusHeader_2x08_Shrouded",
    "Eurorack 16-pin (2x8) shrouded IDC bus header, 2.54mm, vertical. Col1=-12V (red stripe)",
    "eurorack idc header shrouded", "\n".join(b), refy=-13.5, valy=13.5)

# --- PTC resettable fuse, 1812
b = [pad_smd(1, -2.05, 0, 2.0, 3.6), pad_smd(2, 2.05, 0, 2.0, 3.6),
     rect(-2.3, -1.85, 2.3, 1.85, FAB, 0.1),
     line(-3.3, -2.1, 3.3, -2.1, SILK), line(-3.3, 2.1, 3.3, 2.1, SILK),
]
FOOTPRINTS["ER_PTC_1812"] = fp("ER_PTC_1812", "PTC resettable fuse, 1812 (4532 metric)",
                               "ptc fuse 1812", "\n".join(b), "smd", -3.0, 3.0)

# --- 1206 chip (used for the +5V per-header fuse/link position)
b = [pad_smd(1, -1.5, 0, 1.4, 1.75), pad_smd(2, 1.5, 0, 1.4, 1.75),
     rect(-1.6, -0.8, 1.6, 0.8, FAB, 0.1),
     line(-2.3, -1.1, 2.3, -1.1, SILK), line(-2.3, 1.1, 2.3, 1.1, SILK),
]
FOOTPRINTS["ER_Chip_1206"] = fp("ER_Chip_1206", "1206 chip component (PTC / 0R link / resistor)",
                                "1206 chip", "\n".join(b), "smd", -2.0, 2.0)

# --- 0805 chip
b = [pad_smd(1, -0.95, 0, 1.0, 1.45), pad_smd(2, 0.95, 0, 1.0, 1.45),
     rect(-1.0, -0.6, 1.0, 0.6, FAB, 0.1),
     line(-1.6, -0.9, 1.6, -0.9, SILK), line(-1.6, 0.9, 1.6, 0.9, SILK),
]
FOOTPRINTS["ER_Chip_0805"] = fp("ER_Chip_0805", "0805 chip component (capacitor / resistor)",
                                "0805 chip", "\n".join(b), "smd", -1.8, 1.8)

# --- 0805 LED (pin 1 = cathode marked)
b = [pad_smd(1, -0.95, 0, 1.0, 1.45), pad_smd(2, 0.95, 0, 1.0, 1.45),
     rect(-1.0, -0.6, 1.0, 0.6, FAB, 0.1),
     line(-2.0, -0.9, -2.0, 0.9, SILK, 0.3),
]
FOOTPRINTS["ER_LED_0805"] = fp("ER_LED_0805", "0805 LED, pad 1 = cathode (bar side)",
                               "led 0805", "\n".join(b), "smd", -1.8, 1.8)

# --- SMB diode (TVS / Schottky), pin 1 = cathode
b = [pad_smd(1, -2.2, 0, 2.2, 2.5), pad_smd(2, 2.2, 0, 2.2, 2.5),
     rect(-1.8, -1.55, 1.8, 1.55, FAB, 0.1),
     line(-3.9, -1.8, -3.9, 1.8, SILK, 0.3),
]
FOOTPRINTS["ER_Diode_SMB"] = fp("ER_Diode_SMB", "DO-214AA (SMB) diode, pad 1 = cathode",
                                "diode smb do-214aa", "\n".join(b), "smd", -2.7, 2.7)

# --- SMC diode (bulk reverse-polarity clamp), pin 1 = cathode
b = [pad_smd(1, -3.1, 0, 2.6, 3.6), pad_smd(2, 3.1, 0, 2.6, 3.6),
     rect(-2.8, -2.3, 2.8, 2.3, FAB, 0.1),
     line(-5.0, -2.6, -5.0, 2.6, SILK, 0.3),
]
FOOTPRINTS["ER_Diode_SMC"] = fp("ER_Diode_SMC", "DO-214AB (SMC) diode, pad 1 = cathode",
                                "diode smc do-214ab", "\n".join(b), "smd", -3.5, 3.5)

# --- Radial electrolytic, 8mm body, 3.5mm pitch
b = [pad_tht(1, -1.75, 0, (1.8, 1.8), 0.9, "rect"), pad_tht(2, 1.75, 0, (1.8, 1.8), 0.9),
     circle(0, 0, 4.0, SILK), circle(0, 0, 4.0, FAB, 0.1),
     line(-3.3, -2.2, -3.3, 2.2, SILK, 0.3),
]
FOOTPRINTS["ER_CP_Radial_D8_P3.5"] = fp("ER_CP_Radial_D8_P3.5",
                                        "Radial electrolytic capacitor, 8mm dia, 3.5mm pitch, pad 1 = +",
                                        "capacitor electrolytic radial", "\n".join(b), "through_hole", -5.2, 5.2)

# --- 5x20mm cartridge fuse clip holder (two clips, 22.6mm apart)
b = [pad_tht(1, -11.3, 0, (2.4, 2.4), 1.3, "rect"), pad_tht(2, 11.3, 0, (2.4, 2.4), 1.3),
     rect(-10.5, -3.0, 10.5, 3.0, SILK), rect(-10.5, -3.0, 10.5, 3.0, FAB, 0.1),
]
FOOTPRINTS["ER_Fuseholder_5x20_Clips"] = fp("ER_Fuseholder_5x20_Clips",
                                            "5x20mm cartridge fuse, PCB clip pair (22.6mm pitch)",
                                            "fuse holder 5x20 clip", "\n".join(b), "through_hole", -5.0, 5.0)

# --- 4-position screw terminal, 5.08mm pitch
b = []
for i in range(4):
    x = -7.62 + i * 5.08
    b.append(pad_tht(i + 1, x, 0, (2.6, 2.6), 1.4, "rect" if i == 0 else "circle"))
b.append(rect(-10.16, -4.5, 10.16, 4.5, SILK))
b.append(rect(-10.16, -4.5, 10.16, 4.5, FAB, 0.1))
FOOTPRINTS["ER_TerminalBlock_4P_5.08"] = fp("ER_TerminalBlock_4P_5.08",
                                            "4-way PCB screw terminal, 5.08mm pitch",
                                            "terminal block screw 5.08", "\n".join(b), "through_hole", -6.0, 6.0)

# --- Molex KK396 4-pin header (alternate power input)
b = []
for i in range(4):
    x = -5.94 + i * 3.96
    b.append(pad_tht(i + 1, x, 0, (2.4, 2.4), 1.3, "rect" if i == 0 else "circle"))
b.append(rect(-8.2, -3.5, 8.2, 3.5, SILK))
FOOTPRINTS["ER_Molex_KK396_4P"] = fp("ER_Molex_KK396_4P", "Molex KK 396 (0.156in) 4-pin header",
                                     "molex kk396 connector", "\n".join(b), "through_hole", -5.0, 5.0)

# --- 2-pin 2.54mm header (CV/Gate bus chaining)
b = [pad_tht(1, -1.27, 0, (1.7, 1.7), 1.0, "rect"), pad_tht(2, 1.27, 0, (1.7, 1.7), 1.0),
     rect(-2.6, -1.5, 2.6, 1.5, SILK)]
FOOTPRINTS["ER_PinHeader_1x02_2.54"] = fp("ER_PinHeader_1x02_2.54", "2-pin 2.54mm header",
                                          "pin header", "\n".join(b), "through_hole", -3.0, 3.0)

# --- Solder pad for heavy wire (trunk entry)
b = [pad_tht(1, 0, 0, (4.0, 4.0), 2.2, "circle"), circle(0, 0, 2.6, SILK),
]
FOOTPRINTS["ER_WirePad_2.2mm"] = fp("ER_WirePad_2.2mm", "Solder pad for 14-16AWG wire, 2.2mm hole",
                                    "wire pad solder", "\n".join(b), "through_hole", -3.6, 3.6)

# --- M3 mounting hole
b = [pad_np(0, 0, 3.2), circle(0, 0, 3.2, SILK)]
FOOTPRINTS["ER_MountingHole_M3"] = fp("ER_MountingHole_M3", "M3 non-plated mounting hole",
                                      "mounting hole m3", "\n".join(b), "through_hole exclude_from_pos_files exclude_from_bom",
                                      -4.2, 4.2)


# --- SOT-23-3 (P-channel MOSFET for the +5V ideal diode). 1=G 2=S 3=D
b = [pad_smd(1, -0.95, 1.15, 0.9, 1.2), pad_smd(2, 0.95, 1.15, 0.9, 1.2),
     pad_smd(3, 0.0, -1.15, 0.9, 1.2),
     rect(-0.7, -0.75, 0.7, 0.75, FAB, 0.1),
     line(-1.6, -1.9, 1.6, -1.9, SILK), line(-1.6, 1.9, 1.6, 1.9, SILK),
]
FOOTPRINTS["ER_SOT23"] = fp("ER_SOT23", "SOT-23-3 (MOSFET: 1=Gate 2=Source 3=Drain)",
                            "sot-23 mosfet", "\n".join(b), "smd", -2.8, 2.8)


# --- Molex Mini-Fit Jr. 5569 / 39-30-1060: 2x3, 4.2mm, right-angle, snap-in peg.
#     Pad coordinates taken from the official KiCad Connector_Molex footprint.
#     Pins 1-3 are the row nearest the housing; 4-6 the row further back.
b = []
for _i, (_x, _y) in enumerate(((0, 0), (4.2, 0), (8.4, 0), (0, 5.5), (4.2, 5.5), (8.4, 5.5))):
    b.append(pad_tht(_i + 1, _x, _y, (2.7, 3.7), 1.8, "rect" if _i == 0 else "oval"))
b.append(pad_np(0.0, -7.3, 3.0))
b.append(pad_np(8.4, -7.3, 3.0))
b.append(rect(-2.7, -13.9, 11.1, -1.1, FAB, 0.1))
b.append(line(-2.81, -0.99, -2.81, -14.01, SILK))
b.append(line(-2.81, -14.01, 11.21, -14.01, SILK))
b.append(line(11.21, -14.01, 11.21, -0.99, SILK))
b.append(line(-2.81, -0.99, -2.0, -0.99, SILK))
b.append(line(10.4, -0.99, 11.21, -0.99, SILK))
b.append(line(-2.0, 0.6, -3.2, 0.0, SILK, 0.3))
b.append(line(-3.2, 0.0, -2.0, -0.6, SILK, 0.3))
FOOTPRINTS["ER_MiniFitJr_2x03_RA"] = fp(
    "ER_MiniFitJr_2x03_RA",
    "Molex Mini-Fit Jr 5569 (39-30-1060) 2x3 4.2mm right-angle header, snap-in peg",
    "molex mini-fit jr 5569 power", "\n".join(b), "through_hole", -15.1, 8.6)


# ----------------------------------------------------------------------------
# Symbol library
# ----------------------------------------------------------------------------
def prop(name, value, x, y, rot=0, hide=False, size=1.27):
    h = " hide" if hide else ""
    return (f'    (property "{name}" "{value}" (at {x:.4f} {y:.4f} {rot})\n'
            f'      (effects (font (size {size} {size})){h}))')


def pin(x, y, rot, length, name, number, etype="passive", style="line"):
    return (f'      (pin {etype} {style} (at {x:.4f} {y:.4f} {rot}) (length {length:.4f})\n'
            f'        (name "{name}" (effects (font (size 1.27 1.27))))\n'
            f'        (number "{number}" (effects (font (size 1.27 1.27)))))')


def sym(name, ref, value, footprint, graphics, pins, hide_pin_names=True, hide_pin_numbers=False,
        extra_props=""):
    hpn = "\n    (pin_names (offset 0.762) hide)" if hide_pin_names else "\n    (pin_names (offset 0.762))"
    hnum = "\n    (pin_numbers hide)" if hide_pin_numbers else ""
    return f'''  (symbol "{name}"{hnum}{hpn}
    (in_bom yes)
    (on_board yes)
{prop("Reference", ref, 0, 6.35)}
{prop("Value", value, 0, -6.35)}
{prop("Footprint", footprint, 0, 0, 0, True)}
{prop("Datasheet", "", 0, 0, 0, True)}
{prop("Description", "", 0, 0, 0, True)}
{extra_props}
    (symbol "{name}_0_1"
{graphics}
    )
    (symbol "{name}_1_1"
{pins}
    )
  )'''


def s_rect(x1, y1, x2, y2, fill="none", w=0.254):
    return (f'      (rectangle (start {x1:.4f} {y1:.4f}) (end {x2:.4f} {y2:.4f})\n'
            f'        (stroke (width {w}) (type default)) (fill (type {fill})))')


def s_poly(pts, fill="none", w=0.254):
    p = " ".join(f"(xy {x:.4f} {y:.4f})" for x, y in pts)
    return (f'      (polyline (pts {p})\n'
            f'        (stroke (width {w}) (type default)) (fill (type {fill})))')


def s_circle(cx, cy, r, fill="none", w=0.254):
    return (f'      (circle (center {cx:.4f} {cy:.4f}) (radius {r:.4f})\n'
            f'        (stroke (width {w}) (type default)) (fill (type {fill})))')


SYMS = []

# 16-pin bus header symbol
g = [s_rect(-7.62, 21.59, 7.62, -21.59, "background")]
names = ["-12V", "-12V", "GND", "GND", "GND", "GND", "GND", "GND",
         "+12V", "+12V", "+5V", "+5V", "CV", "CV", "GATE", "GATE"]
p = []
for i in range(16):
    y = 19.05 - i * 2.54
    p.append(pin(-12.7, y, 0, 5.08, names[i], str(i + 1), "passive"))
SYMS.append(sym("ER_BusHeader_16", "J", "ER_BusHeader_16",
                "eurorack_bus:ER_BusHeader_2x08_Shrouded",
                "\n".join(g), "\n".join(p), hide_pin_names=False))

# PTC fuse
g = [s_rect(-2.54, 1.016, 2.54, -1.016), s_poly([(-2.54, 0), (2.54, 0)])]
p = [pin(-5.08, 0, 0, 2.54, "~", "1"), pin(5.08, 0, 180, 2.54, "~", "2")]
SYMS.append(sym("ER_PTC", "F", "PTC", "eurorack_bus:ER_PTC_1812", "\n".join(g), "\n".join(p)))

# Cartridge fuse
g = [s_rect(-3.81, 1.016, 3.81, -1.016), s_poly([(-3.81, 0), (3.81, 0)])]
p = [pin(-6.35, 0, 0, 2.54, "~", "1"), pin(6.35, 0, 180, 2.54, "~", "2")]
SYMS.append(sym("ER_Fuse", "F", "Fuse", "eurorack_bus:ER_Fuseholder_5x20_Clips",
                "\n".join(g), "\n".join(p)))

# Capacitor (non-polarised)
g = [s_poly([(-2.54, 0.635), (2.54, 0.635)], w=0.508),
     s_poly([(-2.54, -0.635), (2.54, -0.635)], w=0.508)]
p = [pin(0, 3.81, 270, 3.175, "~", "1"), pin(0, -3.81, 90, 3.175, "~", "2")]
SYMS.append(sym("ER_C", "C", "100n", "eurorack_bus:ER_Chip_0805", "\n".join(g), "\n".join(p)))

# Polarised capacitor
g = [s_rect(-2.54, 0.508, 2.54, 1.016, "outline", 0.0),
     s_poly([(-2.54, -0.508), (2.54, -0.508)], w=0.508),
     s_poly([(-1.778, 2.286), (-1.778, 1.524)], w=0.254),
     s_poly([(-2.159, 1.905), (-1.397, 1.905)], w=0.254)]
p = [pin(0, 3.81, 270, 2.794, "~", "1"), pin(0, -3.81, 90, 3.302, "~", "2")]
SYMS.append(sym("ER_CP", "C", "100u/25V", "eurorack_bus:ER_CP_Radial_D8_P3.5",
                "\n".join(g), "\n".join(p)))

# Resistor
g = [s_rect(-1.016, 2.54, 1.016, -2.54)]
p = [pin(0, 3.81, 270, 1.27, "~", "1"), pin(0, -3.81, 90, 1.27, "~", "2")]
SYMS.append(sym("ER_R", "R", "1k", "eurorack_bus:ER_Chip_0805", "\n".join(g), "\n".join(p)))

# Diode (pin 1 = cathode, matching the footprint)
g = [s_poly([(-1.27, 1.27), (-1.27, -1.27)], w=0.508),
     s_poly([(1.27, 1.27), (-1.27, 0), (1.27, -1.27), (1.27, 1.27)], "outline")]
p = [pin(-3.81, 0, 0, 2.54, "K", "1"), pin(3.81, 0, 180, 2.54, "A", "2")]
SYMS.append(sym("ER_D", "D", "SS54", "eurorack_bus:ER_Diode_SMC", "\n".join(g), "\n".join(p)))

# TVS (unidirectional, pin 1 = cathode)
g = [s_poly([(-1.27, 1.27), (-1.27, -1.27), (-2.032, -1.27)], w=0.508),
     s_poly([(-1.27, 1.27), (-0.508, 1.27)], w=0.508),
     s_poly([(1.27, 1.27), (-1.27, 0), (1.27, -1.27), (1.27, 1.27)], "outline")]
p = [pin(-3.81, 0, 0, 2.54, "K", "1"), pin(3.81, 0, 180, 2.54, "A", "2")]
SYMS.append(sym("ER_TVS", "D", "SMBJ13A", "eurorack_bus:ER_Diode_SMB", "\n".join(g), "\n".join(p)))

# LED (pin 1 = cathode)
g = [s_poly([(-1.27, 1.27), (-1.27, -1.27)], w=0.508),
     s_poly([(1.27, 1.27), (-1.27, 0), (1.27, -1.27), (1.27, 1.27)], "outline"),
     s_poly([(0.254, 2.032), (1.524, 3.302)], w=0.152),
     s_poly([(1.524, 3.302), (1.524, 2.286)], w=0.152),
     s_poly([(1.524, 3.302), (0.508, 3.302)], w=0.152)]
p = [pin(-3.81, 0, 0, 2.54, "K", "1"), pin(3.81, 0, 180, 2.54, "A", "2")]
SYMS.append(sym("ER_LED", "D", "LED", "eurorack_bus:ER_LED_0805", "\n".join(g), "\n".join(p)))

# 4-pin power input connector
g = [s_rect(-2.54, 5.08, 2.54, -5.08, "background")]
p = [pin(-5.08, 3.81, 0, 2.54, "1", "1"), pin(-5.08, 1.27, 0, 2.54, "2", "2"),
     pin(-5.08, -1.27, 0, 2.54, "3", "3"), pin(-5.08, -3.81, 0, 2.54, "4", "4")]
SYMS.append(sym("ER_Conn_1x04", "J", "Conn_1x04", "eurorack_bus:ER_TerminalBlock_4P_5.08",
                "\n".join(g), "\n".join(p), hide_pin_names=False))

# 2-pin connector
g = [s_rect(-2.54, 2.54, 2.54, -2.54, "background")]
p = [pin(-5.08, 1.27, 0, 2.54, "1", "1"), pin(-5.08, -1.27, 0, 2.54, "2", "2")]
SYMS.append(sym("ER_Conn_1x02", "J", "Conn_1x02", "eurorack_bus:ER_PinHeader_1x02_2.54",
                "\n".join(g), "\n".join(p), hide_pin_names=False))

# Single wire pad
g = [s_circle(0, 0, 1.27, "background")]
p = [pin(-3.81, 0, 0, 2.54, "1", "1")]
SYMS.append(sym("ER_WirePad", "J", "WirePad", "eurorack_bus:ER_WirePad_2.2mm",
                "\n".join(g), "\n".join(p)))

# Mounting hole
g = [s_circle(0, 0, 1.27, "none")]
SYMS.append(sym("ER_MountingHole", "H", "MountingHole", "eurorack_bus:ER_MountingHole_M3",
                "\n".join(g), "      ", hide_pin_names=True))


# P-channel MOSFET (ideal-diode / reverse-blocking element)
g = [s_poly([(-2.54, 0), (-2.54, 2.54)]), s_poly([(-2.54, 0), (-2.54, -2.54)]),
     s_poly([(-1.27, 2.286), (-1.27, 1.016)], w=0.508),
     s_poly([(-1.27, 0.635), (-1.27, -0.635)], w=0.508),
     s_poly([(-1.27, -1.016), (-1.27, -2.286)], w=0.508),
     s_poly([(-1.27, 1.651), (0, 1.651), (0, 2.54)]),
     s_poly([(-1.27, -1.651), (0, -1.651), (0, -2.54)]),
     s_poly([(-1.27, 0), (0, 0), (0, 1.651)]),
     s_poly([(-0.508, 0), (0.762, 0.635), (0.762, -0.635), (-0.508, 0)], "outline")]
p = [pin(-5.08, 0, 0, 2.54, "G", "1", "input"),
     pin(0, 5.08, 270, 2.54, "S", "2"),
     pin(0, -5.08, 90, 2.54, "D", "3")]
SYMS.append(sym("ER_PMOS", "Q", "AO3401A", "eurorack_bus:ER_SOT23",
                "\n".join(g), "\n".join(p), hide_pin_names=False))


# 6-pin Mini-Fit Jr power input
g = [s_rect(-2.54, 7.62, 2.54, -7.62, "background")]
p = [pin(-5.08, 6.35, 0, 2.54, "1", "1"), pin(-5.08, 3.81, 0, 2.54, "2", "2"),
     pin(-5.08, 1.27, 0, 2.54, "3", "3"), pin(-5.08, -1.27, 0, 2.54, "4", "4"),
     pin(-5.08, -3.81, 0, 2.54, "5", "5"), pin(-5.08, -6.35, 0, 2.54, "6", "6")]
SYMS.append(sym("ER_Conn_1x06", "J", "MiniFitJr 2x3", "eurorack_bus:ER_MiniFitJr_2x03_RA",
                "\n".join(g), "\n".join(p), hide_pin_names=False))


def write_libs(root):
    pretty = os.path.join(root, "eurorack_bus.pretty")
    os.makedirs(pretty, exist_ok=True)
    for name, text in FOOTPRINTS.items():
        with open(os.path.join(pretty, name + ".kicad_mod"), "w") as f:
            f.write(text)
    lib = ('(kicad_symbol_lib\n  (version 20211014)\n  (generator "eurorack_busgen")\n'
           + "\n".join(SYMS) + "\n)\n")
    with open(os.path.join(root, "eurorack_bus.kicad_sym"), "w") as f:
        f.write(lib)
    return len(FOOTPRINTS), len(SYMS)


if __name__ == "__main__":
    print(write_libs("out"))
