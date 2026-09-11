"""Generates the single-row 20-header 4-layer Eurorack bus board, 414 x 62 mm.

Twenty shrouded headers in one row, ten each side of a central input block.
Same netlist, refs and BOM as the staggered variant in pcbgen.py - only the
geometry differs, so eurorack-busboard.kicad_sch serves both.

Run this for the .kicad_pcb, then verify_singlerow.py for connectivity and
plane-continuity. Both must come back clean.
"""
# License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.

import uuid, math, os, re as _re
import libgen

_GFX = {}
for _n, _t in libgen.FOOTPRINTS.items():
    _items = [l for l in _re.findall(r'^  \((?:fp_line|fp_circle)[^\n]*\)$', _t, _re.M)
              if 'CrtYd' not in l]
    _m = _re.search(r'\(fp_text reference "REF\*\*" \(at 0 ([-\d.]+) 0\)', _t)
    _v = _re.search(r'\(fp_text value "[^"]*" \(at 0 ([-\d.]+) 0\)', _t)
    _GFX[_n] = (_items, float(_m.group(1)) if _m else -2.0,
                float(_v.group(1)) if _v else 2.0)

def U(): return str(uuid.uuid4())

# ---------------------------------------------------------------- parameters
BOARD_W = 414.0
BOARD_H = 62.0
HDR_L = [12.0 + 16.0 * i for i in range(10)]    # 12 .. 156
HDR_R = [258.0 + 16.0 * i for i in range(10)]   # 258 .. 402
HDR_X = HDR_L + HDR_R                   # every header identically oriented

def col_y(k):
    return 39.78 - (k - 1) * 2.54       # col 1 (-12V) at 39.78, red stripe down

BAND_P5_A  = (1.5, 7.0)
BAND_P12_A = (9.0, 19.0)
BAND_N12   = (43.0, 60.5)

W_STUB = 0.8
W_TRUNK = 2.0
W_IN = 1.2
W_SIG = 0.5
VIA_D, VIA_DR = 0.9, 0.45
VIA_PWR_D, VIA_PWR_DR = 1.2, 0.6
CLEAR = 0.25

# ---------------------------------------------------------------- containers
nets = {}
def net(name):
    if name not in nets:
        nets[name] = len(nets) + 1
    return nets[name]
net("GND"); net("+12V"); net("-12V"); net("+5V"); net("CV"); net("GATE")
net("IN_N12"); net("IN_P12"); net("IN_P5")

footprints = []
segments = []
vias = []
zones = []
edges = []
texts = []

# geometry records for the checker: (kind, layerset, net, data)
geo = []
ALLCU = {"F.Cu", "In1.Cu", "In2.Cu", "B.Cu"}


def add_seg(x1, y1, x2, y2, w, layer, netname):
    n = net(netname)
    segments.append(f'  (segment (start {x1:.4f} {y1:.4f}) (end {x2:.4f} {y2:.4f}) '
                    f'(width {w}) (layer "{layer}") (net {n}) (tstamp {U()}))')
    geo.append(("seg", {layer}, netname, (x1, y1, x2, y2, w)))


def add_track(pts, w, layer, netname):
    for i in range(len(pts) - 1):
        add_seg(pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1], w, layer, netname)


def add_via(x, y, netname, big=False):
    d, dr = (VIA_PWR_D, VIA_PWR_DR) if big else (VIA_D, VIA_DR)
    n = net(netname)
    vias.append(f'  (via (at {x:.4f} {y:.4f}) (size {d}) (drill {dr}) '
                f'(layers "F.Cu" "B.Cu") (net {n}) (tstamp {U()}))')
    geo.append(("via", set(ALLCU), netname, (x, y, d)))


def rot_pt(dx, dy, rot):
    r = math.radians(rot)
    # KiCad rotates counter-clockwise on screen; file Y is down.
    return dx * math.cos(r) + dy * math.sin(r), -dx * math.sin(r) + dy * math.cos(r)


class FP:
    """A placed footprint. pads = list of (num, dx, dy, w, h, netname, tht)"""
    def __init__(self, lib, ref, value, x, y, rot=0, layer="F.Cu", silk_ref=True):
        self.lib, self.ref, self.value = lib, ref, value
        self.x, self.y, self.rot, self.layer = x, y, rot, layer
        self.silk_ref = silk_ref
        self.pads = []
        self.uuid = U()

    def pad(self, num, dx, dy, w, h, netname=None, tht=True, shape="oval", drill=1.0):
        px, py = rot_pt(dx, dy, self.rot)
        ax, ay = self.x + px, self.y + py
        pw, ph = (w, h) if self.rot % 180 == 0 else (h, w)
        self.pads.append((num, dx, dy, w, h, netname, tht, shape, drill, ax, ay, pw, ph))
        if netname:
            geo.append(("pad", set(ALLCU) if tht else {"F.Cu"}, netname,
                        (ax, ay, pw, ph)))
        return ax, ay

    def render(self):
        rot = "" if self.rot == 0 else " " + str(self.rot)
        out = [f'  (footprint "eurorack_bus:{self.lib}"',
               f'    (layer "{self.layer}")',
               f'    (tstamp {self.uuid})',
               f'    (at {self.x:.4f} {self.y:.4f}{rot})',
               f'    (attr {"through_hole" if any(p[6] for p in self.pads) else "smd"})',
               f'    (path "/{self.uuid}")',
               f'    (fp_text reference "{self.ref}" (at 0 {_GFX[self.lib][1]} {self.rot})'
               f' (layer "{"F.SilkS" if self.silk_ref else "F.Fab"}")'
               f' (effects (font (size 1 1) (thickness 0.15))) (tstamp {U()}))',
               f'    (fp_text value "{self.value}" (at 0 {_GFX[self.lib][2]} {self.rot}) (layer "F.Fab") hide'
               f' (effects (font (size 1 1) (thickness 0.15))) (tstamp {U()}))']
        for _g in _GFX[self.lib][0]:
            out.append(_re.sub(r'\(tstamp [^)]*\)', lambda m: f'(tstamp {U()})', _g))
        for (num, dx, dy, w, h, netname, tht, shape, drill, ax, ay, pw, ph) in self.pads:
            n = net(netname) if netname else 0
            nn = f' (net {n} "{netname}")' if netname else ""
            if shape == "np_circle":
                out.append(f'    (pad "" np_thru_hole circle (at {dx:.4f} {dy:.4f})'
                           f' (size {w} {h}) (drill {drill})'
                           f' (layers "F.Cu" "B.Cu" "F.Mask" "B.Mask") (tstamp {U()}))')
            elif tht:
                out.append(f'    (pad "{num}" thru_hole {shape} (at {dx:.4f} {dy:.4f}'
                           f'{"" if self.rot == 0 else " " + str(self.rot)}) (size {w} {h}) '
                           f'(drill {drill}) (layers "*.Cu" "*.Mask"){nn} (tstamp {U()}))')
            else:
                out.append(f'    (pad "{num}" smd roundrect (at {dx:.4f} {dy:.4f}'
                           f'{"" if self.rot == 0 else " " + str(self.rot)}) (size {w} {h}) '
                           f'(layers "F.Cu" "F.Paste" "F.Mask") (roundrect_rratio 0.25)'
                           f'{nn} (tstamp {U()}))')
        out.append("  )")
        return "\n".join(out)


def place_2pad_smd(lib, ref, value, x, y, rot, half, w, h, n1, n2, silk=True):
    f = FP(lib, ref, value, x, y, rot, silk_ref=silk)
    a = f.pad("1", -half, 0, w, h, n1, False)
    b = f.pad("2", half, 0, w, h, n2, False)
    footprints.append(f)
    return a, b


def place_ptc(ref, x, y, rot, n1, n2, val="1.1A PTC", silk=True):
    return place_2pad_smd("ER_PTC_1812", ref, val, x, y, rot, 2.05, 2.0, 3.6, n1, n2, silk)


def place_1206(ref, val, x, y, rot, n1, n2, silk=True):
    return place_2pad_smd("ER_Chip_1206", ref, val, x, y, rot, 1.5, 1.4, 1.75, n1, n2, silk)


def place_0805(ref, val, x, y, rot, n1, n2, lib="ER_Chip_0805", silk=True):
    return place_2pad_smd(lib, ref, val, x, y, rot, 0.95, 1.0, 1.45, n1, n2, silk)


def place_smb(ref, val, x, y, rot, nk, na):
    return place_2pad_smd("ER_Diode_SMB", ref, val, x, y, rot, 2.2, 2.2, 2.5, nk, na)


def place_smc(ref, val, x, y, rot, nk, na):
    return place_2pad_smd("ER_Diode_SMC", ref, val, x, y, rot, 3.1, 2.6, 3.6, nk, na)


def place_radial(ref, val, x, y, rot, nplus, nminus):
    f = FP("ER_CP_Radial_D8_P3.5", ref, val, x, y, rot)
    a = f.pad("1", -1.75, 0, 1.8, 1.8, nplus, True, "rect", 0.9)
    b = f.pad("2", 1.75, 0, 1.8, 1.8, nminus, True, "circle", 0.9)
    footprints.append(f)
    return a, b


def place_fuse(ref, val, x, y, n1, n2):
    f = FP("ER_Fuseholder_5x20_Clips", ref, val, x, y, 0)
    a = f.pad("1", -11.3, 0, 2.4, 2.4, n1, True, "rect", 1.3)
    b = f.pad("2", 11.3, 0, 2.4, 2.4, n2, True, "oval", 1.3)
    footprints.append(f)
    return a, b


def place_hole(ref, x, y):
    f = FP("ER_MountingHole_M3", ref, "M3", x, y, 0)
    f.pad("", 0, 0, 3.2, 3.2, None, True, "np_circle", 3.2)
    footprints.append(f)
    geo.append(("pad", set(ALLCU), "__HOLE__", (x, y, 4.2, 4.2)))



def place_sot23(ref, val, x, y, rot, ng, ns, nd):
    f = FP("ER_SOT23", ref, val, x, y, rot)
    g = f.pad("1", -0.95, 1.15, 0.9, 1.2, ng, False)
    s = f.pad("2", 0.95, 1.15, 0.9, 1.2, ns, False)
    d = f.pad("3", 0.0, -1.15, 0.9, 1.2, nd, False)
    footprints.append(f)
    return g, s, d


def place_header(ref, x, nets_map):
    f = FP("ER_BusHeader_2x08_Shrouded", ref, "BUS", x, 30.89, 0)
    for k in range(1, 9):
        dy = 8.89 - (k - 1) * 2.54
        nm = nets_map[k]
        f.pad(str(2 * k - 1), -1.27, dy, 1.7, 1.7, nm, True, "rect" if k == 1 else "oval")
        f.pad(str(2 * k), 1.27, dy, 1.7, 1.7, nm, True, "oval")
    footprints.append(f)


# ---------------------------------------------------------------- headers
for i, xc in enumerate(HDR_X):
    n = i + 1
    hn12, hp12, hp5 = f"H{n}_N12", f"H{n}_P12", f"H{n}_P5"
    place_header(f"J{n}", xc, {1: hn12, 2: "GND", 3: "GND", 4: "GND",
                               5: hp12, 6: hp5, 7: "CV", 8: "GATE"})
    # -12V down to the band along the bottom edge
    add_seg(xc - 1.27, col_y(1), xc + 1.27, col_y(1), W_STUB, "F.Cu", hn12)
    rail, hdr = place_ptc(f"F{n}A", xc + 1.27, 47.0, 90, "-12V", hn12)
    add_track([(xc + 1.27, col_y(1)), hdr], W_STUB, "F.Cu", hn12)
    add_track([rail, (xc + 1.27, 53.0)], W_STUB, "F.Cu", "-12V")
    add_via(xc + 1.27, 53.0, "-12V", big=True)
    # +12V up to band A
    add_seg(xc - 1.27, col_y(5), xc + 1.27, col_y(5), W_STUB, "F.Cu", hp12)
    hdr, rail = place_ptc(f"F{n}B", xc + 7.5, 19.0, 90, hp12, "+12V")
    add_track([(xc + 1.27, col_y(5)), (xc + 7.5, col_y(5)), hdr], W_STUB, "F.Cu", hp12)
    add_track([rail, (xc + 7.5, 14.0)], W_STUB, "F.Cu", "+12V")
    add_via(xc + 7.5, 14.0, "+12V", big=True)
    # +5V up to band A
    add_seg(xc - 1.27, col_y(6), xc + 1.27, col_y(6), W_STUB, "F.Cu", hp5)
    hdr, rail = place_1206(f"F{n}C", "0R5 PTC", xc + 5.0, 12.1, 90, hp5, "+5V")
    add_track([(xc + 1.27, col_y(6)), (xc + 5.0, col_y(6)), hdr], W_STUB, "F.Cu", hp5)
    add_track([rail, (xc + 5.0, 4.5)], W_STUB, "F.Cu", "+5V")
    add_via(xc + 5.0, 4.5, "+5V")
    # decoupling
    g1, p1 = place_0805(f"C{n}A", "100n", xc - 5.5, 16.0, 90, "GND", "+12V")
    add_track([p1, (xc - 5.5, 12.5)], W_STUB, "F.Cu", "+12V"); add_via(xc - 5.5, 12.5, "+12V")
    add_track([g1, (xc - 5.5, 20.5)], W_STUB, "F.Cu", "GND"); add_via(xc - 5.5, 20.5, "GND")
    n1, g2 = place_0805(f"C{n}B", "100n", xc - 6.2, 47.0, 90, "-12V", "GND")
    add_track([n1, (xc - 6.2, 51.0)], W_STUB, "F.Cu", "-12V"); add_via(xc - 6.2, 51.0, "-12V")
    add_track([g2, (xc - 6.2, 41.0)], W_STUB, "F.Cu", "GND"); add_via(xc - 6.2, 41.0, "GND")

# ------------------------------------------------- CV / GATE buses (B.Cu)
for ys, nm in ((24.54, "CV"), (22.0, "GATE")):
    add_track([(min(HDR_X) - 1.27, ys), (max(HDR_X) + 1.27, ys)], W_SIG, "B.Cu", nm)

# ---------------------------------------------------------------- input block
# Single row, 62 mm tall. The block owns the one X window the header clusters
# leave free: header 10's +12V fuse ends at x=165.2 and header 11's decoupling
# starts at x=251.9, so everything below lives in x = 166.5 .. 251.5.
#
# LANE ASSIGNMENT
# The free middle band (y 19..43, between the +12V and -12V pours on In2.Cu) is
# only 24 mm tall, and every input path has to cross it in the same stretch of X.
# Rather than let paths find their own way, each one is given a named corridor
# here and the routing below refers only to these maps. Three rules keep it
# collision-free:
#   * horizontal runs own a Y, vertical runs own an X, and no two paths of
#     different nets share one;
#   * the three plane bridges go on B.Cu, which in this window carries nothing
#     but the GND pour, CV and GATE - so they never enter the F.Cu congestion;
#   * anything that must cross a connector's pad field detours to a named
#     escape Y that clears both pad rows and the mounting-hole keepouts.
MF_NETS = {"1": "GND", "2": "IN_N12", "3": "GND",
           "4": "IN_P5", "5": "GND", "6": "IN_P12"}

BLK_X0, BLK_X1 = 166.5, 251.5           # free window between the header clusters

MF_T = (186.0, 13.9)                    # top connector    (vents off the top edge)
MF_B = (216.0, 48.1)                    # bottom connector (vents off the bottom edge)

# B.Cu input trunks. 4.5 mm apart: enough for the 4.0 mm wire pads to stack on
# them without a body clash, and clear of CV (24.54) and GATE (22.0).
TRUNK = {"IN_P5": 27.5, "IN_P12": 32.0, "IN_N12": 36.5}
TRUNK_X0, TRUNK_X1 = 168.0, 228.0       # feed via .. wire pad

# Connector escapes: net -> (vertical corridor X, detour Y or None for straight).
# IN_N12 is the one pin on each connector boxed in by GND pads on both sides, so
# it steps out past the pad field first and drops on its own corridor.
JOG_T = {"IN_P5": (186.0, None), "IN_P12": (194.4, None), "IN_N12": (182.0, 10.6)}
JOG_B = {"IN_P5": (216.0, None), "IN_P12": (207.6, None), "IN_N12": (220.0, 51.4)}

# Plane bridges, all on B.Cu: net -> (band Y, crossing Y, x_left, x_right).
BRIDGE = {"+5V":  (4.5,  2.6, 180.0, 200.0),
          "+12V": (14.0, 10.3, 180.0, 200.0),
          "-12V": (53.0, 59.4, 202.0, 222.0)}

RISER_X = 177.5                         # main-fuse output risers to +12V / -12V
FUSE_X = 172.5                          # main-fuse column
LED_X = (232.5, 236.0, 242.0)           # GND via, LED, resistor
LED_LANE = {"+12V": (245.0, 17.0), "+5V": (248.0, 6.0), "-12V": (248.0, 46.0)}


def place_minifit(ref, x, y, rot):
    f = FP("ER_MiniFitJr_2x03_RA", ref, "PWR IN", x, y, rot)
    pads = {}
    for i, (dx, dy) in enumerate(((0, 0), (4.2, 0), (8.4, 0),
                                  (0, 5.5), (4.2, 5.5), (8.4, 5.5))):
        n = str(i + 1)
        pads[n] = f.pad(n, dx, dy, 2.7, 3.7, MF_NETS[n], True,
                        "rect" if i == 0 else "oval", 1.8)
    # The two 3 mm mounting holes punch every layer, so they are declared to the
    # checker. Without this the bridges could be routed straight through them.
    for hdx in (0.0, 8.4):
        f.pad("", hdx, -7.3, 3.0, 3.0, None, True, "np_circle", 3.0)
        hx, hy = rot_pt(hdx, -7.3, rot)
        geo.append(("pad", set(ALLCU), "__HOLE__", (x + hx, y + hy, 4.0, 4.0)))
    footprints.append(f)
    return pads


ptop = place_minifit("J_IN_T", MF_T[0], MF_T[1], 0)
pbot = place_minifit("J_IN_B", MF_B[0], MF_B[1], 180)

# ---- plane bridges (B.Cu) -------------------------------------------------
# Each connector's pads and mounting holes thin its bands to a couple of mm of
# inner copper. These carry the rail around the obstruction on the back side,
# where the only other copper is the GND pour and the two signal buses.
for nm, (yb, yc, x0, x1) in BRIDGE.items():
    add_track([(x0, yb), (x0, yc), (x1, yc), (x1, yb)], W_TRUNK, "B.Cu", nm)
    add_via(x0, yb, nm, big=True)
    add_via(x1, yb, nm, big=True)

# ---- connector pins -> B.Cu trunks ----------------------------------------
for pads, jog in ((ptop, JOG_T), (pbot, JOG_B)):
    for pin, nm in MF_NETS.items():
        if nm == "GND":                 # GND pins reach the In1.Cu pour directly
            continue
        px, py = pads[pin]
        jx, jy = jog[nm]
        if jy is None:
            add_track([(px, py), (px, TRUNK[nm])], W_IN, "F.Cu", nm)
        else:
            add_track([(px, py), (px, jy), (jx, jy), (jx, TRUNK[nm])], W_IN, "F.Cu", nm)
        add_via(jx, TRUNK[nm], nm)

for nm, y in TRUNK.items():
    add_track([(TRUNK_X0, y), (TRUNK_X1, y)], W_IN, "B.Cu", nm)
    add_via(TRUNK_X0, y, nm, big=True)

# heavy-wire pads, one per input net, stacked on the right end of the trunks
for ref, nm in (("W_P5", "IN_P5"), ("W_P12", "IN_P12"), ("W_N12", "IN_N12")):
    f = FP("ER_WirePad_2.2mm", ref, nm, TRUNK_X1, TRUNK[nm], 0)
    f.pad("1", 0, 0, 4.0, 4.0, nm, True, "circle", 2.2)
    footprints.append(f)
f = FP("ER_WirePad_2.2mm", "W_GND", "GND", TRUNK_X1, 41.0, 0)
f.pad("1", 0, 0, 4.0, 4.0, "GND", True, "circle", 2.2)
footprints.append(f)

# ---- main fuses ------------------------------------------------------------
# One per rail, laid horizontally on its own trunk Y so the input side needs no
# routing at all: trunk -> feed via -> fuse. Outputs leave on the riser corridor.
for ref, nm, out, val in (("F_MP5", "IN_P5", "P5_FUSED", "1.1A PTC"),
                          ("F_MP12", "IN_P12", "+12V", "2.6A PTC"),
                          ("F_MN12", "IN_N12", "-12V", "2.6A PTC")):
    y = TRUNK[nm]
    a, b = place_ptc(ref, FUSE_X, y, 0, nm, out, val)
    add_track([(TRUNK_X0, y), a], W_IN, "F.Cu", nm)

add_track([(FUSE_X + 2.05, TRUNK["IN_P12"]), (RISER_X, TRUNK["IN_P12"]),
           (RISER_X, 14.0)], W_STUB, "F.Cu", "+12V")
add_via(RISER_X, 14.0, "+12V", big=True)
add_track([(FUSE_X + 2.05, TRUNK["IN_N12"]), (RISER_X, TRUNK["IN_N12"]),
           (RISER_X, 53.0)], W_STUB, "F.Cu", "-12V")
add_via(RISER_X, 53.0, "-12V", big=True)

# ---- ideal diode on +5V ----------------------------------------------------
# Sits in the pocket above the fuse column, left of the +12V riser.
g, sq, d = place_sot23("Q1", "AO3401A", 172.0, 22.0, 0, "N_G1", "P5_FUSED", "+5V")
add_track([(FUSE_X + 2.05, TRUNK["IN_P5"]), (176.0, TRUNK["IN_P5"]),
           (176.0, sq[1]), sq], W_STUB, "F.Cu", "P5_FUSED")
add_track([d, (172.0, 4.5)], W_STUB, "F.Cu", "+5V")
add_via(172.0, 4.5, "+5V", big=True)
r1, r2 = place_0805("R_G1", "100k", 169.0, 21.5, 180, "N_G1", "GND")
add_track([g, r1], W_STUB, "F.Cu", "N_G1")
add_track([r2, (167.0, 21.5), (167.0, 19.0)], W_STUB, "F.Cu", "GND")
add_via(167.0, 19.0, "GND")

# ---- bulk caps, clamps and TVS --------------------------------------------
# All three band rows use the same pattern: cap on the left, reverse-polarity
# clamp, then TVS turned round so the two GND pads face each other and share a
# via. Rail vias are big, GND vias small - the +5V pour is only 5.5 mm tall and
# a 1.2 mm via with clearance would pinch it.
place_radial("C_B3", "220u/25V", 206.0, 5.5, 180, "+5V", "GND")
place_radial("C_B2", "220u/25V", 206.0, 14.5, 0, "+12V", "GND")
place_radial("C_B1", "220u/25V", 196.0, 53.0, 0, "GND", "-12V")

for rail, y, rv in (("+5V", 4.5, 210.5), ("+12V", 14.0, 210.5)):
    k, aa = place_smc(f"D_R{'3' if rail == '+5V' else '2'}", "SS54",
                      216.0, y, 0, rail, "GND")
    add_track([k, (rv, y)], W_STUB, "F.Cu", rail); add_via(rv, y, rail, big=True)
    kt, at = place_smb(f"D_T{'3' if rail == '+5V' else '2'}",
                       "SMBJ6.0A" if rail == "+5V" else "SMBJ13A",
                       228.0, y, 180, rail, "GND")
    add_track([aa, (222.5, y)], W_STUB, "F.Cu", "GND")
    add_track([at, (222.5, y)], W_STUB, "F.Cu", "GND")
    add_via(222.5, y, "GND")
    add_track([kt, (233.0, y)], W_STUB, "F.Cu", rail); add_via(233.0, y, rail, big=True)

# -12V row: bodies are wider here, so the two GND returns get their own vias.
kt, at = place_smb("D_T1", "SMBJ13A", 232.0, 53.0, 0, "GND", "-12V")
add_track([kt, (227.0, 53.0)], W_STUB, "F.Cu", "GND"); add_via(227.0, 53.0, "GND")
add_track([at, (234.2, 57.0), (237.0, 57.0)], W_STUB, "F.Cu", "-12V")
add_via(237.0, 57.0, "-12V", big=True)
k, aa = place_smc("D_R1", "SS54", 243.0, 53.0, 0, "GND", "-12V")
add_track([k, (239.9, 49.0)], W_STUB, "F.Cu", "GND"); add_via(239.9, 49.0, "GND")
add_track([aa, (249.0, 53.0)], W_STUB, "F.Cu", "-12V"); add_via(249.0, 53.0, "-12V", big=True)

# ---- rail LEDs -------------------------------------------------------------
# Right end of the free band, past the wire pads. Each pair sits on its own Y
# and reaches its rail on the lane assigned in LED_LANE, so the three verticals
# never share an X range.
gv, lx, rx = LED_X
for ref, rref, y, rail, node, val, rval in (
        ("D_L2", "R_L2", 29.0, "+12V", "N_L2", "LED_GRN", "3k9"),
        ("D_L3", "R_L3", 35.0, "+5V", "N_L3", "LED_YEL", "1k"),
        ("D_L1", "R_L1", 41.0, "-12V", "N_L1", "LED_RED", "3k9")):
    lane, viay = LED_LANE[rail]
    if rail == "-12V":                  # GND -> LED -> R -> -12V
        node_pad, gnd_pad = place_0805(ref, val, lx, y, 180, node, "GND", "ER_LED_0805")
    else:                               # rail -> R -> LED -> GND
        gnd_pad, node_pad = place_0805(ref, val, lx, y, 0, "GND", node, "ER_LED_0805")
    r1, r2 = place_0805(rref, rval, rx, y, 0, node, rail)
    add_track([node_pad, r1], W_STUB, "F.Cu", node)
    add_track([gnd_pad, (gv, y)], W_STUB, "F.Cu", "GND"); add_via(gv, y, "GND")
    add_track([r2, (lane, y), (lane, viay)], W_STUB, "F.Cu", rail)
    add_via(lane, viay, rail, big=True)

# H9 supports the middle of the board: the header clusters leave a 108 mm span
# between H4 and H5, and this is a board people push connectors into.
# Mounting holes. At 16 mm pitch a header plus its fuses and decoupling occupies
# essentially the whole pitch at mid-height, so there is no free X for a hole on
# the y=31 line the staggered board uses. y=57.5 is clear the whole way across -
# below every -12V fuse and its via, above the board edge - and the input-block
# window takes two more where the free band is genuinely empty.
for i, (hx, hy) in enumerate(((20.0, 57.5), (76.0, 57.5), (132.0, 57.5),
                              (188.0, 57.5), (300.0, 57.5), (356.0, 57.5),
                              (402.0, 57.5), (200.0, 41.0), (232.0, 18.0))):
    place_hole(f"H{i+1}", hx, hy)

# ---------------------------------------------------------------- zones
def zone(netname, layer, pts, prio=0):
    n = net(netname)
    p = " ".join(f"(xy {x:.4f} {y:.4f})" for x, y in pts)
    zones.append(f'''  (zone (net {n}) (net_name "{netname}") (layer "{layer}") (tstamp {U()})
    (name "{netname}_{layer}_{prio}") (hatch edge 0.5) (priority {prio})
    (connect_pads (clearance 0.4))
    (min_thickness 0.25) (filled_areas_thickness no)
    (fill yes (thermal_gap 0.4) (thermal_bridge_width 0.6))
    (polygon (pts {p})))''')


def band(y0, y1):
    return [(1.5, y0), (BOARD_W - 1.5, y0), (BOARD_W - 1.5, y1), (1.5, y1)]

zone("GND", "In1.Cu", band(1.5, BOARD_H - 1.5))
zone("+5V", "In2.Cu", band(*BAND_P5_A), 1)
zone("+12V", "In2.Cu", band(*BAND_P12_A), 2)
zone("-12V", "In2.Cu", band(*BAND_N12), 3)
zone("GND", "B.Cu", band(1.5, BOARD_H - 1.5))

# ---------------------------------------------------------------- edges & text
for (x1, y1, x2, y2) in ((0, 0, BOARD_W, 0), (BOARD_W, 0, BOARD_W, BOARD_H),
                         (BOARD_W, BOARD_H, 0, BOARD_H), (0, BOARD_H, 0, 0)):
    edges.append(f'  (gr_line (start {x1} {y1}) (end {x2} {y2}) '
                 f'(stroke (width 0.1) (type solid)) (layer "Edge.Cuts") (tstamp {U()}))')

def gtext(s, x, y, size=1.4, layer="F.SilkS", rot=0):
    texts.append(f'  (gr_text "{s}" (at {x:.4f} {y:.4f} {rot}) (layer "{layer}") (tstamp {U()})'
                 f' (effects (font (size {size} {size}) (thickness {size/6:.2g})) (justify left)))')

gtext("EURORACK BUS 20x  -  -12V / RED STRIPE FACES CENTRE LINE", 8.0, 59.2, 1.2)
gtext("FEED ONE INPUT ONLY", 233.0, 21.0, 1.2)

# ---------------------------------------------------------------- checker
# Real component body sizes (mm), from manufacturer data - NOT nominal package
# names. Copper clearance says nothing about whether a part physically fits.
BODY = {"ER_BusHeader_2x08_Shrouded": (9.0, 23.4), "ER_PTC_1812": (5.4, 3.45),
        "ER_Chip_1206": (3.2, 1.6), "ER_Chip_0805": (2.0, 1.25),
        "ER_LED_0805": (2.0, 1.25), "ER_Diode_SMB": (5.6, 3.6),
        "ER_Diode_SMC": (8.0, 5.6), "ER_SOT23": (3.0, 1.4),
        "ER_MiniFitJr_2x03_RA": (14.8, 22.3), "ER_CP_Radial_D8_P3.5": (8.0, 8.0),
        "ER_TerminalBlock_4P_5.08": (20.3, 9.0), "ER_WirePad_2.2mm": (4.0, 4.0)}


def body_rect(f):
    if f.lib not in BODY:
        return None
    w, h = BODY[f.lib]
    if f.rot % 180:
        w, h = h, w
    return (f.x - w / 2, f.y - h / 2, f.x + w / 2, f.y + h / 2)


def check_bodies(clr=0.3):
    rs = [(f.ref, body_rect(f)) for f in footprints if f.lib in BODY]
    bad = []
    for i in range(len(rs)):
        for j in range(i + 1, len(rs)):
            a, b = rs[i][1], rs[j][1]
            dx = min(a[2], b[2]) - max(a[0], b[0])
            dy = min(a[3], b[3]) - max(a[1], b[1])
            if dx > -clr and dy > -clr:
                bad.append((rs[i][0], rs[j][0], round(dx, 2), round(dy, 2)))
    return bad

def seg_rect_dist(seg, rect):
    x1, y1, x2, y2, w = seg
    cx, cy, rw, rh = rect
    hx, hy = rw / 2, rh / 2
    best = 1e9
    steps = max(2, int(math.hypot(x2 - x1, y2 - y1) / 0.25) + 1)
    for i in range(steps + 1):
        t = i / steps
        px, py = x1 + (x2 - x1) * t, y1 + (y2 - y1) * t
        dx = max(abs(px - cx) - hx, 0.0)
        dy = max(abs(py - cy) - hy, 0.0)
        best = min(best, math.hypot(dx, dy) - w / 2)
    return best


def seg_seg_dist(a, b):
    """Distance between two thick segments. Returns negative if they overlap.
    Handles the crossing case: two segments can intersect while every endpoint
    is far from the other segment, which a pure endpoint-distance test misses."""
    ax1, ay1, ax2, ay2, aw = a
    bx1, by1, bx2, by2, bw = b

    def cross(ox, oy, px, py, qx, qy):
        return (px - ox) * (qy - oy) - (py - oy) * (qx - ox)

    d1 = cross(ax1, ay1, ax2, ay2, bx1, by1)
    d2 = cross(ax1, ay1, ax2, ay2, bx2, by2)
    d3 = cross(bx1, by1, bx2, by2, ax1, ay1)
    d4 = cross(bx1, by1, bx2, by2, ax2, ay2)
    if ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0)):
        return -(aw + bw) / 2          # true crossing

    def d(px, py, x1, y1, x2, y2):
        dx, dy = x2 - x1, y2 - y1
        L = dx * dx + dy * dy
        t = 0 if L == 0 else max(0, min(1, ((px - x1) * dx + (py - y1) * dy) / L))
        return math.hypot(px - (x1 + t * dx), py - (y1 + t * dy))

    best = min(d(ax1, ay1, bx1, by1, bx2, by2), d(ax2, ay2, bx1, by1, bx2, by2),
               d(bx1, by1, ax1, ay1, ax2, ay2), d(bx2, by2, ax1, ay1, ax2, ay2))
    return best - aw / 2 - bw / 2


def rect_rect_dist(a, b):
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    dx = max(abs(ax - bx) - (aw + bw) / 2, 0.0)
    dy = max(abs(ay - by) - (ah + bh) / 2, 0.0)
    return math.hypot(dx, dy)


def to_rect(kind, data):
    if kind == "pad":
        return data
    if kind == "via":
        x, y, d = data
        return (x, y, d, d)
    return None


def check():
    bad = []
    n = len(geo)
    for i in range(n):
        k1, l1, n1, d1 = geo[i]
        for j in range(i + 1, n):
            k2, l2, n2, d2 = geo[j]
            if n1 == n2 and n1 != "__HOLE__":
                continue
            if not (l1 & l2):
                continue
            if k1 == "seg" and k2 == "seg":
                dist = seg_seg_dist(d1, d2)
            elif k1 == "seg":
                dist = seg_rect_dist(d1, to_rect(k2, d2))
            elif k2 == "seg":
                dist = seg_rect_dist(d2, to_rect(k1, d1))
            else:
                dist = rect_rect_dist(to_rect(k1, d1), to_rect(k2, d2))
            if dist < CLEAR:
                bad.append((round(dist, 3), k1, n1, d1, k2, n2, d2))
    return bad


# ---------------------------------------------------------------- emit
LAYERS = '''    (0 "F.Cu" signal)
    (1 "In1.Cu" signal "GND")
    (2 "In2.Cu" signal "PWR")
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
    (42 "Eco1.User" user "User.Eco1")
    (43 "Eco2.User" user "User.Eco2")
    (44 "Edge.Cuts" user)
    (45 "Margin" user)
    (46 "B.CrtYd" user "B.Courtyard")
    (47 "F.CrtYd" user "F.Courtyard")
    (48 "B.Fab" user)
    (49 "F.Fab" user)'''


def emit(path):
    netlines = "\n".join(f'  (net {v} "{k}")' for k, v in sorted(nets.items(), key=lambda x: x[1]))
    body = "\n".join([f.render() for f in footprints])
    txt = f'''(kicad_pcb
  (version 20221018)
  (generator "eurorack_busgen")
  (general (thickness 1.6))
  (paper "A2")
  (layers
{LAYERS}
  )
  (setup
    (pad_to_mask_clearance 0.05)
  )
  (net 0 "")
{netlines}
{body}
{chr(10).join(edges)}
{chr(10).join(texts)}
{chr(10).join(segments)}
{chr(10).join(vias)}
{chr(10).join(zones)}
)
'''
    with open(path, "w") as f:
        f.write(txt)


if __name__ == "__main__":
    os.makedirs("out", exist_ok=True)
    emit("out/eurorack-busboard-singlerow.kicad_pcb")
    bad = check()
    print(f"footprints={len(footprints)} segments={len(segments)} vias={len(vias)} "
          f"nets={len(nets)} geo={len(geo)}")
    print(f"clearance violations: {len(bad)}")
    bb = check_bodies()
    print(f"component body overlaps: {len(bb)}")
    for x in bb[:10]:
        print("   ", x)
    for b in sorted(bad)[:40]:
        print("  ", b)
