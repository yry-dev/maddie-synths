#!/usr/bin/env python3
"""Generate panels/m-power/ - the 6HP Eurorack front panel for the m-power PSU.

Every hole in this panel is DERIVED from hardware/m-power/*.kicad_pcb, not
typed in here. The PSU board has no mounting holes: the panel is held on by
the parts themselves (the two barrel-jack bushings, the USB shell, and the
toggle bushing), so a hole in the wrong place is not a cosmetic problem, it
is a board that cannot be assembled. Re-run this after any placement change.

House conventions followed (see scripts/panels/tools/README.md):
  - graphics only: no footprints, pads or nets
  - front artwork on B.Silkscreen, mirror-drawn (the B face is the outer face)
  - `madelyn.sh` on F.Silkscreen, unmirrored (inner face, read from behind)
  - board outline + all cutouts on Edge.Cuts
  - Comfortaa, emitted without render_cache so KiCad rasterises from the font

Usage:  python3 make_mpower_panel.py [--force]
"""
# License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.
# The panel artwork this emits is licensed separately: see panels/LICENSE.md (CC BY-NC-SA 4.0).

import json
import pathlib
import re
import sys

import make_blanks as mb   # reuse the blanks' emitters so conventions stay in sync

HERE = pathlib.Path(__file__).resolve().parent
REPO = mb.REPO
PSU_PCB = REPO / "hardware" / "m-power" / "eurorack-psu-twobrick.kicad_pcb"
NAME = "m-power"
HP = 6

# ---------------------------------------------------------------- geometry --
WIDTH = mb.MECH[HP]["width"]        # 30.00mm - the Doepfer 6HP panel width
HEIGHT = mb.HEIGHT                  # 128.50mm
PSU_W, PSU_H = 29.80, 108.68        # asserted against the PCB below

# The panel (30.00mm) is deliberately WIDER than the board (29.80mm), so the
# panel overhangs by 0.10mm per side. That direction matters: a board wider
# than its panel stands proud behind it and fouls the neighbouring module.
# pcb_gen.py sets the board width for exactly this reason - see its WIDTH note
# for why 29.8 is the narrowest it can be without the outputs wrapping.
# Panel and board edges therefore do NOT line up: always convert through
# front_xy() rather than assuming a shared origin.
PCB_DX = (WIDTH - PSU_W) / 2.0                  # +0.10
PCB_DY = round((HEIGHT - PSU_H) / 2.0, 3)       # 9.910 - board centred vertically

# Hole sizes. Radii in mm; provenance in the comment, because these are the
# numbers that decide whether the module can be screwed together.
R_BARREL = 4.05   # Switchcraft PC722A 5/16-32 bushing = 7.94mm major dia,
                  # +~0.15mm clearance. Thread spec is from the vendor
                  # footprint descr in hardware/lib/maddie.pretty.
R_LED    = 1.75   # house LED radius (panel_map.py RKIND), suits a 3mm LED
R_TOGGLE = 3.30   # C&K 7201SYCQE 1/4-40 UNS-2A bushing. The datasheet's own
                  # panel-mounting table (page A-9) calls for a .250in
                  # (6.35mm) hole, which is a line-to-line fit on the 6.35mm
                  # major diameter; 6.6mm gives it 0.25mm to actually go in.
                  # The Y bushing also has an anti-rotation keyway (.022 x
                  # .234in notch) which this panel does NOT cut - the switch
                  # is held against rotation by its nut alone.
USB_W, USB_H = 13.6, 6.4   # USB-A receptacle shell + clearance. PROVISIONAL:
                           # measure the actual Molex 105057 shell before
                           # ordering - this one is a cutout, not a drill, so
                           # an undersized opening cannot be filed out neatly.

# Which refs become which cutout. Anything not listed is not a panel part.
ROUND_HOLES = {
    "J1": R_BARREL, "J2": R_BARREL,
    "D3": R_LED, "D4": R_LED, "D5": R_LED,
    "SW1": R_TOGGLE,
}
RECT_HOLES = {"J3": (USB_W, USB_H)}

# Where the panel feature actually sits relative to the footprint ORIGIN, in
# footprint-local mm.
#
# A footprint's origin is wherever its author chose to put it - very often pad
# 1 - and that is NOT the centre of the thing that pokes through the panel.
# This is not a cosmetic correction: taken from the origin, J3's cutout lands
# 3.5mm off the USB shell and breaks out through the edge of the panel, and
# every LED hole is 1.27mm off its lens.
#
# These are expressed UNROTATED; check_rotation() below refuses to run if any
# panel part is placed at an angle, rather than silently emitting a hole in
# the wrong place.
ANCHOR = {
    # Switchcraft PC722A, vendor footprint: origin is already on the barrel
    # axis - its F.Fab and F.CrtYd centres both land exactly on the origin.
    "J1": (0.0, 0.0), "J2": (0.0, 0.0),
    # maddie:SW_Toggle_DPDT_SubMini_PCMount - origin is the bushing axis by
    # construction (the six pads are symmetric about it).
    "SW1": (0.0, 0.0),
    # LED_THT:LED_D3.0mm - origin is the first pad; the lens is centred on the
    # midpoint of the 2.54mm pad pitch. Do NOT take this from the F.Fab bbox:
    # the polarity flat skews it 0.2mm, which would eat most of the 0.25mm
    # clearance the 1.75mm hole leaves around a 3mm lens.
    "D3": (1.27, 0.0), "D4": (1.27, 0.0), "D5": (1.27, 0.0),
    # USB-A receptacle - origin is pad 1. Here the F.Fab outline IS the 13.2mm
    # metal shell (F.CrtYd is 16.1mm and includes pad clearance), so the Fab
    # centre is the opening centre.
    "J3": (3.50, -1.90),
}

EDGE_W, HOLE_W = 0.05, 0.2          # Edge.Cuts stroke widths, per make_blanks


# ------------------------------------------------------------ PCB scraping --
def read_psu(path):
    """Return (bbox, {ref: (x, y)}) from the PSU board, in PCB coords."""
    t = path.read_text()
    xs, ys = [], []
    for chunk in t.split("(gr_")[1:]:
        if "Edge.Cuts" not in chunk[:800]:
            continue
        for m in re.finditer(r"\((?:start|end|mid|center)\s+([-\d.]+)\s+([-\d.]+)\)",
                             chunk[:800]):
            xs.append(float(m.group(1)))
            ys.append(float(m.group(2)))
    if not xs:
        sys.exit(f"no Edge.Cuts geometry in {path}")
    pos = {}
    for chunk in t.split("(footprint ")[1:]:
        at = re.search(r"\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)", chunk)
        ref = re.search(r'\(property "Reference" "([^"]+)"', chunk)
        if at and ref:
            pos[ref.group(1)] = (float(at.group(1)), float(at.group(2)),
                                 float(at.group(3) or 0.0))
    return (min(xs), min(ys), max(xs), max(ys)), pos


def anchored(ref, pos):
    """Board coords of the feature that pokes through the panel.

    ANCHOR is written for an unrotated footprint, so refuse rather than guess
    if the board has one of these placed at an angle - a hole quietly 3mm out
    is a panel that cannot be assembled and looks fine on screen.
    """
    px, py, rot = pos[ref]
    if abs(rot) > 1e-6:
        sys.exit("%s is rotated %.1f on the board but ANCHOR assumes 0. "
                 "Rotate the ANCHOR offset before trusting this panel." % (ref, rot))
    dx, dy = ANCHOR[ref]
    return px + dx, py + dy


def front_xy(px, py, bb):
    """PSU board coords -> panel-local FRONT view (x from the front-left edge)."""
    return px - bb[0] + PCB_DX, py - bb[1] + PCB_DY


MIN_WEB = 1.0   # least panel material left beside any cutout


def check_inside(ref, fx, fy, w, h):
    """Refuse to emit a cutout that leaves no panel around it.

    The first run of this generator put J3's opening 0.2mm from the left edge
    (it was anchored on pad 1 rather than the shell). That is a panel that
    snaps in the router, and nothing downstream would have caught it: KiCad
    DRC is happy to cut a slot through the board edge.
    """
    for lo, name in ((fx - w / 2.0, "left"), (fy - h / 2.0, "top")):
        if lo < MIN_WEB:
            sys.exit("%s cutout leaves only %.2fmm of panel at the %s edge "
                     "(want >= %.1fmm). Check ANCHOR[%r] and the board layout."
                     % (ref, lo, name, MIN_WEB, ref))
    for hi, lim, name in ((fx + w / 2.0, WIDTH, "right"),
                          (fy + h / 2.0, HEIGHT, "bottom")):
        if hi > lim - MIN_WEB:
            sys.exit("%s cutout leaves only %.2fmm of panel at the %s edge "
                     "(want >= %.1fmm). Check ANCHOR[%r] and the board layout."
                     % (ref, lim - hi, name, MIN_WEB, ref))


def to_pcb_x(fx):
    """Front-view x -> panel .kicad_pcb x. The front face is the B side, so the
    file is drawn mirrored; this is the single place that flip happens."""
    return WIDTH - fx


# --------------------------------------------------------------- emitters ---
def rect_cut(cx, cy, w, h):
    """Rectangular Edge.Cuts cutout centred on (cx, cy) in panel pcb coords."""
    x0, x1 = cx - w / 2.0, cx + w / 2.0
    y0, y1 = cy - h / 2.0, cy + h / 2.0
    return "".join(mb.em_line(a, b, c, d, EDGE_W, "Edge.Cuts") for a, b, c, d in
                   ((x0, y0, x1, y0), (x1, y0, x1, y1),
                    (x1, y1, x0, y1), (x0, y1, x0, y0)))


def label(fx, fy, text, size=1.4, thick=0.15, angle=0):
    """Front-view label -> mirrored B.Silkscreen text.

    Newlines are escaped rather than written raw: a literal newline inside a
    quoted s-expression string makes the whole .kicad_pcb unloadable, and the
    only symptom is kicad-cli saying "Failed to load board" with no line
    number. The blanks never hit this because none of them wrap.
    """
    text = text.replace("\n", "\\n")
    return mb.em_text(to_pcb_x(fx), fy, angle, text, "B.SilkS", True, size, thick)


def build(bb, pos):
    p = [mb.HEADER]

    # ---- outline -----------------------------------------------------------
    for a, b, c, d in ((0, 0, WIDTH, 0), (WIDTH, 0, WIDTH, HEIGHT),
                       (WIDTH, HEIGHT, 0, HEIGHT), (0, HEIGHT, 0, 0)):
        p.append(mb.em_line(a, b, c, d, EDGE_W, "Edge.Cuts"))

    # ---- Eurorack mounting holes (from the blanks' researched table) --------
    for hx, hy in mb.MECH[HP]["holes"]:
        p.append(mb.em_circle(hx, hy, mb.HOLE_R, HOLE_W, "Edge.Cuts"))

    # ---- part cutouts, derived from the PSU board --------------------------
    placed = {}
    for ref, r in ROUND_HOLES.items():
        if ref not in pos:
            sys.exit(f"{ref} is not on the PSU board - panel would be wrong. "
                     f"Re-run hardware/m-power/pcb_gen.py first.")
        fx, fy = front_xy(*anchored(ref, pos), bb)
        placed[ref] = (fx, fy)
        check_inside(ref, fx, fy, 2 * r, 2 * r)
        p.append(mb.em_circle(to_pcb_x(fx), fy, r, HOLE_W, "Edge.Cuts"))
    for ref, (w, h) in RECT_HOLES.items():
        if ref not in pos:
            sys.exit(f"{ref} is not on the PSU board")
        fx, fy = front_xy(*anchored(ref, pos), bb)
        placed[ref] = (fx, fy)
        check_inside(ref, fx, fy, w, h)
        p.append(rect_cut(to_pcb_x(fx), fy, w, h))

    # ---- artwork -----------------------------------------------------------
    # Title sits above the barrel jacks, below the top rail.
    top = min(placed["J1"][1], placed["J2"][1]) - R_BARREL
    p.append(label(WIDTH / 2.0, top - 2.6, "m-power", size=2.4, thick=0.35))

    # Brick inputs. Wart B is the one that must be isolated, so it is named
    # rather than left to the builder to infer from the schematic.
    for ref, txt in (("J1", "A"), ("J2", "B")):
        fx, fy = placed[ref]
        p.append(label(fx, fy + R_BARREL + 2.4, txt, size=1.6, thick=0.2))

    # Rail LEDs, labelled with the rail each one indicates.
    for ref, txt in (("D3", "+12"), ("D4", "-12"), ("D5", "+5")):
        fx, fy = placed[ref]
        p.append(label(fx, fy + R_LED + 2.3, txt, size=1.3, thick=0.15))

    # Toggle: mark which way is on. The C&K lever contacts the BOTTOM throw
    # when the lever is up, and pins 3/6 (the bottom pair) are the ones wired
    # into the rails - so lever up = on.
    fx, fy = placed["SW1"]
    p.append(label(fx, fy - R_TOGGLE - 2.0, "on", size=1.3, thick=0.15))

    # The safety-critical assumption, on the panel where it cannot be lost.
    # Wart B's sleeve is driven to -12V, which is only safe if that brick is
    # floating; an earthed supply there shorts the negative rail to earth.
    mid = (placed["SW1"][1] + HEIGHT - 12.0) / 2.0
    p.append(label(WIDTH / 2.0, mid, "ISOLATED\nCLASS II\n12V ONLY",
                   size=1.5, thick=0.18))

    # Branding: house style puts 'maddie synths' on the front face and the URL
    # on the inner face. Both are CENTRED - em_text centres on (x, y), it does
    # not left-align, so a margin-relative x here would hang half the string
    # off the edge of the panel.
    # y = HEIGHT - 5.55 and size 1.25 are the blanks' own 4-to-8HP tier, chosen
    # there to clear the mounting holes; keep them in step.
    p.append(label(WIDTH / 2.0, HEIGHT - 5.55, "maddie synths",
                   size=1.25, thick=0.15))
    # The URL sits higher so it clears the hole band (holes reach y=123.9)
    # rather than relying on its own smaller size to do it.
    p.append(mb.em_text(WIDTH / 2.0, HEIGHT - 9.0, 0, "madelyn.sh",
                        "F.SilkS", False, 1.4, 0.15))

    p.append(")\n")
    return "".join(p)


def main():
    force = "--force" in sys.argv or "-f" in sys.argv
    if not PSU_PCB.exists():
        sys.exit(f"missing {PSU_PCB}")
    bb, pos = read_psu(PSU_PCB)
    w, h = round(bb[2] - bb[0], 2), round(bb[3] - bb[1], 2)
    if abs(w - PSU_W) > 0.05 or abs(h - PSU_H) > 0.05:
        sys.exit(f"PSU board is {w} x {h} mm, expected {PSU_W} x {PSU_H}. "
                 f"Update PSU_W/PSU_H and re-check PCB_DY before regenerating.")

    out = mb.PANELS / NAME
    pcb = out / f"{NAME}.kicad_pcb"
    if pcb.exists() and not force:
        sys.exit(f"{pcb} exists - pass --force to overwrite")
    out.mkdir(parents=True, exist_ok=True)

    mb._uuid_counter[0] = 900000          # own range, so blanks stay stable
    pcb.write_text(build(bb, pos))
    mb.write_pro(out / f"{NAME}.kicad_pro", NAME)

    print(f"{NAME}: panel {WIDTH} x {HEIGHT} mm ({HP}HP), "
          f"board {w} x {h} mm at dy={PCB_DY}")
    for ref in list(ROUND_HOLES) + list(RECT_HOLES):
        fx, fy = front_xy(*anchored(ref, pos), bb)
        kind = (f"r{ROUND_HOLES[ref]}" if ref in ROUND_HOLES
                else "%.1fx%.1f" % RECT_HOLES[ref])
        print(f"    {ref:4} front ({fx:6.2f}, {fy:7.2f})  {kind}")
    print(f"-> {pcb}")


if __name__ == "__main__":
    main()
