#!/usr/bin/env python3
"""House-dress the rabid.audio CLK ("The Count") faceplate.

```bash
python3 make_clk_panel.py              # writes panels/rabid-audio-clk/ (skips existing)
python3 make_clk_panel.py --force      # overwrite
python3 make_clk_panel.py --res        # also render rack-plugins/res/rabid-audio-clk.svg
python3 make_clk_panel.py --extract    # re-read geometry from a rabid-audio checkout
python3 make_clk_panel.py --list       # print the cached geometry
```

**The mechanics are theirs, the artwork is ours** -- the same split as the `fm-*`
family (`panels/DESIGN-RULES.md` section 10), and for the same reason: this plate
has to fit their PCB.

Every cutout is read out of `clock/clock/clock.kicad_pcb`, which carries the
faceplate as a group of hole footprints (`thonkiconn_hole`, `knob_hole`,
`toggle_sw_hole`, `3mm_led_hole`, `eurorack_screw_hole`) sitting beside the
circuit on the same sheet. The panel group is placed at a flat +60 mm offset in
both axes, so panel-local mm is just `pcb - 60` -- confirmed against all ten
holes and against their own `panel/panel-design.svg` drill layer, which agrees to
0.02 mm. Diameters stay theirs: jack 8.0 (not our 6.2), knob 9.0, button 5.2,
LED 3.1, and a 5.8 x 4.2 mounting slot.

Their layout is not ours and is reproduced as found:

  - **Jacks at the top, controls at the bottom.** Their `panel/README.md` states
    it as house style; it is the exact inverse of the mod1/mod2 grid.
  - **The display reads vertically.** Three SM460281N digits are stacked with a
    10.795 mm pitch behind an 8 x 32.4 mm window, so "120" runs down the panel
    rather than across it. That is what 3 HP costs, and it is the single most
    recognisable thing about the module.
  - **Rotated labels.** A 15.2 mm face with an 8 mm jack leaves 3.9 mm beside it,
    so their jack and knob labels are set at 90 degrees. Ours are too -- it is
    the panel's visual signature, not a workaround we get to skip.

What is ours is the dress: Comfortaa, the dark ground, gold B.Mask rules, the
knockout title and output labels, the input arrow and the signal-icon pair. The
fixed vertical grid of section 4 cannot survive their cutouts (see the module's
entry in DESIGN-RULES section 11) -- the header band between the top screw and
the first jack is 5.3 mm, so the brand runs vertically up the left margin and the
title takes the one horizontal slot that fits.

Coordinates below are FRONT-VIEW millimetres (x from the left edge as you look at
the module, y = dy from the top). Emission mirrors them into KiCad space
(`x_kicad = W - x_front`), because these plates are mounted back-side-out.
"""
# License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.
# The panel artwork this emits is licensed separately: see panels/LICENSE.md (CC BY-NC-SA 4.0).
# The cutout geometry cached below is read out of rabid.audio's own clock.kicad_pcb
# and stays theirs — the mechanics are theirs, the artwork is ours (see the note above).

import argparse
import json
import math
import pathlib
import re
import shutil
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from make_fm_panels import (BOLT, SPK_X, ASCENT, DESCENT, EDGE_W, FAB_OPTIONS,
                            HEADER, HEIGHT, KNOCKOUT_PAD, MARGIN, PANELS, REPO,
                            RULE_W, SIZE_CREDIT, SIZE_JACK,
                            em_arc, em_circle, em_glyph, em_line, em_text,
                            load_glyphs, text_w, write_pro)

SLUG = "rabid-audio-clk"
GEOM = pathlib.Path(__file__).resolve().parent / "rabid-audio-clk-panel.json"
DEFAULT_SRC = REPO.parent / "rabid-audio" / "clock" / "clock" / "clock.kicad_pcb"

# Their panel group sits at a flat +60 mm on the PCB sheet in both axes.
PANEL_ORIGIN = 60.0

# Footprint name -> (our semantic kind, is this a panel-group hole?)
HOLE_KINDS = {
    "thonkiconn_hole": "jack",
    "knob_hole": "knob",
    "toggle_sw_hole": "button",
    "3mm_led_hole": "led",
    "eurorack_screw_hole": "mount",
}

# ==========================================================================
# Extraction from their KiCad PCB (run rarely; the result is committed)
# ==========================================================================
def _blocks(text, tag):
    for m in re.finditer(r"\(%s[\s\"]" % tag, text):
        s, d = m.start(), 0
        for i in range(s, len(text)):
            if text[i] == "(":
                d += 1
            elif text[i] == ")":
                d -= 1
                if d == 0:
                    yield text[s:i + 1]
                    break


def extract(src: pathlib.Path):
    text = src.read_text(errors="replace")
    holes, digits = [], []
    for blk in _blocks(text, "footprint"):
        name = re.search(r'\(footprint\s+"([^"]+)"', blk).group(1).split(":")[-1]
        at = re.search(r"\(at ([-\d.]+) ([-\d.]+)", blk)
        if not at:
            continue
        x, y = float(at.group(1)), float(at.group(2))
        val = re.search(r'\(property "Value" "([^"]*)"', blk)
        val = val.group(1) if val else ""

        if name in HOLE_KINDS:
            # Hole size comes from the footprint's single oversized pad.
            pad = next(_blocks(blk, "pad"), "")
            size = re.search(r"\(size ([\d.]+) ([\d.]+)\)", pad)
            w, h = (float(size.group(1)), float(size.group(2))) if size else (0.0, 0.0)
            holes.append(dict(kind=HOLE_KINDS[name], net=val, source=name,
                              x=round(x - PANEL_ORIGIN, 4), y=round(y - PANEL_ORIGIN, 4),
                              w=w, h=h))
        elif "SM460281N" in name:
            # One 7-segment digit. Its window is derived from the stack below.
            fab = [(float(a), float(b))
                   for blk2 in _blocks(blk, "fp_line") if "CrtYd" in blk2 or "Fab" in blk2
                   for a, b in re.findall(r"\((?:start|end) ([-\d.]+) ([-\d.]+)\)", blk2)]
            if fab:
                digits.append(dict(y=round(y - PANEL_ORIGIN, 4),
                                   w=round(max(p[0] for p in fab) - min(p[0] for p in fab), 3),
                                   h=round(max(p[1] for p in fab) - min(p[1] for p in fab), 3)))

    if not holes:
        sys.exit(f"no panel holes found in {src} -- is this the CLK board?")

    # The digit stack gives the window height; its x comes from the LED pair,
    # which their own panel-design.svg aligns the window to.
    digits.sort(key=lambda d: d["y"])
    leds = sorted((h for h in holes if h["kind"] == "led"), key=lambda h: h["x"])
    span = (digits[-1]["y"] - digits[0]["y"]) + digits[0]["h"]
    window = dict(x=round(leds[0]["x"], 4), y=round(digits[0]["y"] - digits[0]["h"] / 2, 4),
                  w=round(leds[-1]["x"] - leds[0]["x"], 3), h=round(span, 3),
                  digits=len(digits), pitch=round(digits[1]["y"] - digits[0]["y"], 3))

    data = dict(
        source=str(src),
        note=("Panel-local mm (pcb - %g). Diameters are rabid.audio's, not ours."
              % PANEL_ORIGIN),
        width=15.2, height=HEIGHT, hp=3, holes=holes, window=window)
    GEOM.write_text(json.dumps(data, indent=1) + "\n")
    print(f"wrote {GEOM.relative_to(REPO)}: {len(holes)} holes, "
          f"{window['digits']}-digit window {window['w']} x {window['h']} mm")
    return data


def load():
    if not GEOM.exists():
        sys.exit(f"{GEOM.name} missing -- run --extract with a rabid-audio checkout")
    return json.loads(GEOM.read_text())


# ==========================================================================
# Artwork -- rabid.audio's, not ours
# ==========================================================================
# This is the one plate in the repo that does NOT wear the house dress. The
# maddie synths look (Comfortaa, aubergine ground, light silk, gold B.Mask rules,
# the brand on dy 5.38, the signal-icon pair) would make it read as one more
# mod1/mod2 panel, and it is not one -- it is The Count. So the plate reproduces
# their design language instead:
#
#   - black ink on bare aluminium, not light silk on a dark ground
#   - a heavy halftone dot field, which is the panel's dominant graphic
#   - a solid black slab framing the display window
#   - every label tilted a few degrees off square, several in filled or outlined
#     boxes, the long ones turned on their side
#   - a brush face for the title and a slab typewriter for everything else
#   - no brand, no rule lines, no icon row
#
# Only `madelyn.sh` survives, on the hidden face, where the converter never plots
# it -- attribution for the derived work that costs the design nothing.

FACE_TITLE = "Marker Felt"        # nearest installed match for their brush title
FACE_LABEL = "American Typewriter"  # their labels and wordmark are a bold slab

# Their ink is black on bare aluminium.
INK = "#141414"
PLATE = "#b6b3ae"
CUTOUT = "#2b2b2b"

TILT = 6.0        # degrees off square -- nothing on their panel is level
# Advance width per character, measured off a render rather than guessed: both
# faces are appreciably wider than Comfortaa, and the first pass ran SWING off
# the board edge. American Typewriter 0.84 em, Marker Felt 0.75 em.
EM_LABEL = 0.84
EM_TITLE = 0.75

OX, OY = 50.0, 30.0   # board origin on the A4 sheet, matching the other panels

_uid = [0]


def uid():
    _uid[0] += 1
    return "c1c0%04x-0000-4000-8000-%012x" % (_uid[0], 0xC10C0000 + _uid[0])


def f(v):
    return ("%.4f" % (0.0 if v == 0 else v)).rstrip("0").rstrip(".")


def txt(x, y, s, size, *, face=FACE_LABEL, layer="B.SilkS", knockout=False,
        mirror=True, justify="center", angle=0.0, thickness=None, bold=False):
    j = ([] if justify == "center" else [justify]) + ["bottom"] + (["mirror"] if mirror else [])
    thickness = thickness if thickness is not None else max(0.18, size * 0.16)
    lay = '%s" knockout' % layer if knockout else '%s"' % layer
    s = s.replace("\n", "\\n")
    return (f'\t(gr_text "{s}"\n\t\t(at {f(OX + x)} {f(OY + y)} {f(angle)})\n'
            f'\t\t(layer "{lay})\n\t\t(uuid "{uid()}")\n\t\t(effects\n\t\t\t(font\n'
            f'\t\t\t\t(face "{face}")\n\t\t\t\t(size {f(size)} {f(size)})\n'
            f'\t\t\t\t(thickness {f(thickness)})\n'
            + ("\t\t\t\t(bold yes)\n" if bold else "")
            + f'\t\t\t)\n\t\t\t(justify {" ".join(j)})\n\t\t)\n\t)\n')


def line(x0, y0, x1, y1, width, layer):
    return (f'\t(gr_line\n\t\t(start {f(OX + x0)} {f(OY + y0)})\n'
            f'\t\t(end {f(OX + x1)} {f(OY + y1)})\n\t\t(stroke\n\t\t\t(width {f(width)})\n'
            f'\t\t\t(type solid)\n\t\t)\n\t\t(layer "{layer}")\n\t\t(uuid "{uid()}")\n\t)\n')


def arc(sx, sy, mx, my, ex, ey, width, layer):
    return (f'\t(gr_arc\n\t\t(start {f(OX + sx)} {f(OY + sy)})\n'
            f'\t\t(mid {f(OX + mx)} {f(OY + my)})\n\t\t(end {f(OX + ex)} {f(OY + ey)})\n'
            f'\t\t(stroke\n\t\t\t(width {f(width)})\n\t\t\t(type solid)\n\t\t)\n'
            f'\t\t(layer "{layer}")\n\t\t(uuid "{uid()}")\n\t)\n')


def circle(cx, cy, r, width, layer, fill=False):
    return (f'\t(gr_circle\n\t\t(center {f(OX + cx)} {f(OY + cy)})\n'
            f'\t\t(end {f(OX + cx + r)} {f(OY + cy)})\n\t\t(stroke\n\t\t\t(width {f(width)})\n'
            f'\t\t\t(type solid)\n\t\t)\n\t\t(fill {"yes" if fill else "no"})\n'
            f'\t\t(layer "{layer}")\n\t\t(uuid "{uid()}")\n\t)\n')


def poly(pts, layer):
    rows = ["\t\t\t" + " ".join(f"(xy {f(OX + x)} {f(OY + y)})" for x, y in pts[k:k + 4])
            for k in range(0, len(pts), 4)]
    return (f'\t(gr_poly\n\t\t(pts\n' + "\n".join(rows) + "\n\t\t)\n"
            f'\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type solid)\n\t\t)\n'
            f'\t\t(fill yes)\n\t\t(layer "{layer}")\n\t\t(uuid "{uid()}")\n\t)\n')


def rot(px, py, cx, cy, deg):
    """Rotate (px,py) about (cx,cy). KiCad angles run counter-clockwise."""
    a = math.radians(-deg)
    dx, dy = px - cx, py - cy
    return cx + dx * math.cos(a) - dy * math.sin(a), cy + dx * math.sin(a) + dy * math.cos(a)


def outline_box(cx, cy, w, h, deg, width=0.18):
    """A tilted rectangular outline -- their light label boxes."""
    c = [(cx - w / 2, cy - h / 2), (cx + w / 2, cy - h / 2),
         (cx + w / 2, cy + h / 2), (cx - w / 2, cy + h / 2)]
    c = [rot(px, py, cx, cy, deg) for px, py in c]
    return "".join(line(c[i][0], c[i][1], c[(i + 1) % 4][0], c[(i + 1) % 4][1],
                        width, "B.SilkS") for i in range(4))


def filled_rect(x0, y0, x1, y1, layer="B.SilkS"):
    return poly([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], layer)


def halftone(regions, holes, avoid, pitch=0.72, r_near=0.30, r_far=0.045):
    """Their dot field: a halftone that fades as it leaves the display.

    `regions` are (x0, y0, x1, y1) bands to fill; dots are skipped where they
    would foul a cutout or a label. Radius ramps with distance from `r_near`'s
    reference band so the field reads as a gradient rather than a screen.
    """
    out = []
    for (x0, y0, x1, y1, ref0, ref1) in regions:
        ny = int((y1 - y0) / pitch)
        nx = int((x1 - x0) / pitch)
        for iy in range(ny + 1):
            y = y0 + iy * pitch
            # distance outside the reference band, normalised over 22 mm
            d = max(0.0, ref0 - y, y - ref1) / 34.0
            r = max(r_far, r_near * (1.0 - min(1.0, d)) ** 1.6)
            if r <= r_far * 1.02:
                continue
            for ix in range(nx + 1):
                x = x0 + ix * pitch + (pitch / 2 if iy % 2 else 0)
                if x > x1:
                    continue
                if any((x - hx) ** 2 + (y - hy) ** 2 < (hr + r + 0.45) ** 2
                       for hx, hy, hr in holes):
                    continue
                if any(ax0 - r < x < ax1 + r and ay0 - r < y < ay1 + r
                       for ax0, ay0, ax1, ay1 in avoid):
                    continue
                out.append(circle(x, y, r, 0, "B.SilkS", fill=True))
    return out


def build(data):
    W, H = data["width"], data["height"]
    parts = [HEADER]
    hole = {}
    for h in data["holes"]:
        hole.setdefault(h["kind"], []).append(h)
    for v in hole.values():
        v.sort(key=lambda h: h["y"])
    jacks, leds, knob, buttons = hole["jack"], hole["led"], hole["knob"][0], hole["button"]
    win = data["window"]

    # Mirror front-view x into KiCad space: these plates mount back-side-out, so
    # flipping the finished board restores their layout exactly.
    def kx(x):
        return W - x

    # ---- board outline ----------------------------------------------------
    parts += [line(0, 0, W, 0, EDGE_W, "Edge.Cuts"),
              line(W, 0, W, H, EDGE_W, "Edge.Cuts"),
              line(W, H, 0, H, EDGE_W, "Edge.Cuts"),
              line(0, H, 0, 0, EDGE_W, "Edge.Cuts")]

    # ---- their cutouts, at their diameters --------------------------------
    keepout = []   # (x, y, r) in KiCad space, for the halftone
    for h in data["holes"]:
        x, y = kx(h["x"]), h["y"]
        if h["kind"] == "mount":
            r = h["h"] / 2
            x0, x1 = x - h["w"] / 2 + r, x + h["w"] / 2 - r
            parts += [line(x0, y - r, x1, y - r, EDGE_W, "Edge.Cuts"),
                      line(x0, y + r, x1, y + r, EDGE_W, "Edge.Cuts"),
                      arc(x0, y + r, x0 - r, y, x0, y - r, EDGE_W, "Edge.Cuts"),
                      arc(x1, y - r, x1 + r, y, x1, y + r, EDGE_W, "Edge.Cuts")]
            keepout.append((x, y, h["w"] / 2))
        else:
            parts.append(circle(x, y, h["w"] / 2, EDGE_W, "Edge.Cuts"))
            keepout.append((x, y, h["w"] / 2))

    wx0, wx1 = kx(win["x"] + win["w"]), kx(win["x"])
    wy0, wy1 = win["y"], win["y"] + win["h"]
    parts += [line(wx0, wy0, wx1, wy0, EDGE_W, "Edge.Cuts"),
              line(wx1, wy0, wx1, wy1, EDGE_W, "Edge.Cuts"),
              line(wx1, wy1, wx0, wy1, EDGE_W, "Edge.Cuts"),
              line(wx0, wy1, wx0, wy0, EDGE_W, "Edge.Cuts")]

    # ---- the black slab behind the display --------------------------------
    # Their panel's centre of gravity: a solid block of ink with the window in
    # it. Drawn as four bars around the cutout rather than one rect, so the
    # window's own edge stays crisp.
    sx0, sx1 = wx0 - 0.85, wx1 + 0.85
    sy0, sy1 = wy0 - 1.10, wy1 + 1.10
    parts += [filled_rect(sx0, sy0, sx1, wy0),
              filled_rect(sx0, wy1, sx1, sy1),
              filled_rect(sx0, wy0, wx0, wy1),
              filled_rect(wx1, wy0, sx1, wy1)]

    # ---- the halftone field ----------------------------------------------
    # Densest beside the slab, fading up into the jack column and down past the
    # encoder. Labels are carved out of it so they stay legible.
    # Dots are carved away from every label that sits in the field, or the
    # screen prints straight through the type. Rects are KiCad-space.
    avoid = [(sx0 - 0.5, sy0 - 0.5, sx1 + 0.5, sy1 + 0.5),   # the slab
             (5.00, 78.40, 10.20, 82.20),                    # BPM
             (0.30, 87.20, 3.30, 96.20),                     # TAP, turned
             (1.00, 101.20, 7.20, 107.40)]                   # DIV button label
    parts += halftone(
        regions=[(0.55, 40.0, W - 0.55, 103.0, sy0, sy1)],
        holes=keepout, avoid=avoid)

    # ---- title ------------------------------------------------------------
    # "THE" small over "COUNT" large, tilted, in the band between the mounting
    # slot and the first jack.
    # Both lines clear the mounting slot (which ends dy 5.1) and the first jack
    # (which starts dy 10.4). COUNT is all caps with no descenders, so its ink
    # stops at the baseline even though its text box does not.
    parts.append(txt(kx(2.90), 6.80, "THE", 1.6, face=FACE_TITLE, angle=TILT,
                     thickness=0.26, bold=True))
    parts.append(txt(kx(7.55), 10.05, "COUNT", 3.0, face=FACE_TITLE, angle=TILT,
                     thickness=0.45, bold=True))

    # ---- jack labels ------------------------------------------------------
    # Outputs get their solid ink boxes, the input an outlined one -- the same
    # distinction their panel draws, and it happens to encode signal direction.
    def rot_label(text, size, band_hi, y, knock, tilt):
        """Place a turned label whose glyphs must stop at `band_hi` (KiCad x)."""
        anchor = band_hi - 0.30 * size - (0.5 if knock else 0.0)
        return txt(anchor, y, text, size, angle=90 + tilt, knockout=knock,
                   thickness=0.22 if knock else 0.18)

    jack_hi = kx(jacks[0]["x"] + jacks[0]["w"] / 2) - 0.25
    for j, label in zip(jacks[:2], ("BEAT", "DIV")):
        parts.append(rot_label(label, 1.5, jack_hi, j["y"], True, TILT))

    # The input keeps their lighter treatment -- plain, not boxed. Its 2.55 mm
    # margin cannot hold an outline as well, and the contrast with the two solid
    # output boxes still reads as "this one goes the other way".
    cv = jacks[2]
    parts.append(txt(W - MARGIN - 0.25 - 0.45, jacks[2]["y"], "CV", 1.5,
                     angle=90 + TILT, thickness=0.20))

    # ---- encoder ----------------------------------------------------------
    # Their BPM sits in the ink slab; ours sits just under it, knocked out of its
    # own box so it reads the same way against the halftone.
    parts.append(txt(kx(W / 2), sy1 + 2.35, "BPM", 1.6, knockout=True, thickness=0.24))

    tap_size = 1.4
    parts.append(txt(kx(knob["x"] + knob["w"] / 2) - 0.25 - 0.30 * tap_size, knob["y"],
                     "TAP", tap_size, angle=90 + TILT, thickness=0.20))

    # ---- buttons ----------------------------------------------------------
    # Their button labels sit in outlined boxes. The buttons are off-centre, so
    # all 7 mm of clear face is on one side of them -- the VISIBLE right, which is
    # KiCad-left. Getting that backwards puts every box on top of its button.
    for b, label in zip(buttons, ("DIV", "SWING")):
        size = 1.1
        w = len(label) * EM_LABEL * size
        bx = kx(b["x"] + b["w"] / 2 + 1.00) - w / 2
        parts.append(txt(bx, b["y"] + size * 0.45, label, size, angle=TILT, thickness=0.20))
        parts.append(outline_box(bx, b["y"] + size * 0.10, w + 1.10,
                                 (ASCENT + DESCENT) * size + 0.60, TILT))

    # The chord, spelled out the way their panel spells it. It goes horizontally
    # in the 5.6 mm gap BETWEEN the buttons -- the side strips are already spoken
    # for, and turned text there would run straight through a button.
    parts.append(txt(kx(W / 2), (buttons[0]["y"] + buttons[1]["y"]) / 2 + 1.55,
                     "PAUSE\n(Hold to RESET)", 1.05, angle=TILT, thickness=0.17))

    # ---- wordmark ---------------------------------------------------------
    parts.append(txt(kx(W / 2), 123.10, "rabid.audio", 1.75, thickness=0.28, bold=True))
    # Hidden face: never plotted into the Rack panel, so it costs the design
    # nothing and still marks the derived work as ours.
    parts.append(txt(kx(W / 2), 121.20, "madelyn.sh", 1.5, layer="F.SilkS",
                     face=FACE_LABEL, knockout=True, mirror=False, thickness=0.35))

    parts.append(")\n")
    return "".join(parts)


# ==========================================================================
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--force", "-f", action="store_true", help="overwrite an existing panel")
    ap.add_argument("--res", action="store_true", help="also render the Rack panel SVG")
    ap.add_argument("--extract", action="store_true", help="re-read their clock.kicad_pcb")
    ap.add_argument("--src", type=pathlib.Path, default=DEFAULT_SRC)
    ap.add_argument("--list", action="store_true", help="print the cached geometry")
    args = ap.parse_args()

    if args.extract:
        extract(args.src.resolve())
        return

    data = load()
    if args.list:
        print(f"# {SLUG}  {data['width']} x {data['height']} mm ({data['hp']} HP)")
        print(f"# from {data['source']}")
        for h in sorted(data["holes"], key=lambda h: (h["y"], h["x"])):
            size = (f"{h['w']} x {h['h']}" if h["kind"] == "mount" else f"d{h['w']}")
            print(f"  {h['kind']:7} ({h['x']:6.3f},{h['y']:7.3f})  {size:11} {h['net']}")
        w = data["window"]
        print(f"  window  ({w['x']:6.3f},{w['y']:7.3f})  {w['w']} x {w['h']}  "
              f"{w['digits']} digits, pitch {w['pitch']}")
        return

    out_dir = PANELS / SLUG
    pcb = out_dir / f"{SLUG}.kicad_pcb"
    if pcb.exists() and not args.force:
        print(f"{SLUG}: exists, skipping (--force to overwrite)")
    else:
        out_dir.mkdir(parents=True, exist_ok=True)
        pcb.write_text(build(data))
        write_pro(out_dir / f"{SLUG}.kicad_pro", SLUG)
        if FAB_OPTIONS.exists():
            shutil.copyfile(FAB_OPTIONS, out_dir / FAB_OPTIONS.name)
        print(f"{SLUG}: {data['width']} x {data['height']} mm ({data['hp']} HP) "
              f"-> {pcb.relative_to(REPO)}")

    if args.res:
        from kicad_to_panel import convert
        # Their scheme, not ours: black ink on bare aluminium.
        convert(str(pcb), SLUG, REPO / "rack-plugins" / "res",
                bg=PLATE, fg=INK, edge=CUTOUT)


if __name__ == "__main__":
    main()
