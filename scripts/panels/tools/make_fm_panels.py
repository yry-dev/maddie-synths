#!/usr/bin/env python3
"""Generate maddie-synths KiCad faceplates for the free-modular module family.

The free-modular project (https://freemodular.org, QuinnFreedman/modular) draws
its faceplates with a Python/SVG generator, one `make_<name>_faceplate.py` per
module. This script turns each of those into a KiCad 9 pure-graphics panel that
is *mechanically* the original (every cutout at the original coordinate and
diameter, so it bolts onto their PCB) but *stylistically* ours -- the layer
convention, vertical grid, typography, tick rings, I/O semantics and icon pair
of panels/DESIGN-RULES.md.

Two modes:

    make_fm_panels.py --extract [--fm-repo PATH]   # refresh fm-modules.json
    make_fm_panels.py [--force] [--only boost,rng] # write panels/fm-<slug>/

--extract imports free-modular's `faceplate_maker`, neuters save()/inkscape and
records every component that each faceplate script adds, resolving the hole
centre the same way the SVG does (position + rotated(offset) + global offset).
That keeps the geometry honest: nothing here is re-typed by hand. The cached
result (fm-modules.json) is committed so generation needs neither the sibling
checkout nor svgwrite.

Their hardware is CC-BY-SA 4.0, so every panel carries `pcb by free modular` in
the `pcb by hagiwo` slot at dy 127.34. That credit is a licence condition, not
decoration -- do not strip it.

Key gotcha: the mod1/mod2 family is mounted **back-side-out** (art on B.SilkS).
These layouts are not left/right symmetric, so every cutout is mirrored into
KiCad space (x_kicad = W - x_source); flipping the finished panel puts each hole
back where their PCB expects it. Artwork is authored directly in KiCad space,
which is why an input arrow sits at (-3.7, +2.2) from its jack here but reads as
lower-right on the finished panel.
"""
import argparse, json, math, pathlib, re, sys, uuid

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[2]
PANELS = REPO / "panels"
GEOM = HERE / "fm-modules.json"
PRO_TEMPLATE = PANELS / "mod2-clap" / "mod2-clap.kicad_pro"
DEFAULT_FM_REPO = REPO.parent / "free-modular"

# --------------------------------------------------------------------------
# House constants (all measured; see panels/DESIGN-RULES.md)
# --------------------------------------------------------------------------
HEIGHT = 128.5
BOARD_ORIGIN = (50.0, 30.0)      # board top-left on the A4 sheet

DY_BRAND = 5.38                  # "maddie synths" baseline
DY_TITLE = 10.77                 # module title baseline (knockout)
DY_SIGN = 119.65                 # "madelyn.sh" baseline (hidden face)
DY_CREDIT = 127.34               # "pcb by ..." baseline
RULE_HEADER = (6.38, 11.88)      # bracket the title bar -- always drawn
RULE_DIVIDERS = (71.38, 94.88)   # section dividers -- dropped/shifted if blocked
RULE_FOOTER = 121.74
RULE_W = 0.6                     # B.Mask stroke
RULE_OVER_L, RULE_OVER_R = 1.01, 0.52   # deliberate edge overrun

SLOT_DY = (3.0, 125.5)           # mounting slot centres
SLOT_X = 6.31                    # slot centre, from the near board edge
SLOT_STRAIGHT = 5.0              # straight run between the semicircular ends
SLOT_H = 3.5
EDGE_W = 0.1                     # Edge.Cuts stroke (unfilled outlines only)

RING_SIZE = 13.55                # tick-ring bounding box
ARROW_DX, ARROW_DY = -3.7, 2.2   # input arrow offset from the jack centre

SIZE_TITLE_MAX = 2.5
SIZE_BRAND = 1.6
SIZE_POT = 2.0
SIZE_JACK = 1.6
SIZE_SMALL = 1.4
SIZE_SIGN = 2.2
SIZE_CREDIT = 1.4
MARGIN = 0.7                     # keep silk this far inside the L/R edges

UUID_NS = uuid.UUID("f3ee3ded-0000-4000-8000-5eab1a2c3d40")
_uuid_n = [0]
def next_uuid():
    _uuid_n[0] += 1
    return str(uuid.uuid5(UUID_NS, str(_uuid_n[0])))

# --------------------------------------------------------------------------
# Per-module editorial layer.
#
# `labels` re-labels a feature by nearest source coordinate (<=3mm), the same
# nearest-slot trick make_panels.py uses. `outputs` flips is_output where the
# source script left the default (their Clock declares its 8 gate outputs as
# inputs). `icons` picks the signal pair: what the module actually emits.
# --------------------------------------------------------------------------
SPK_WAVE, SPK_X, BOLT, BOLT_SLASH = "speaker-wave", "speaker-x", "bolt", "bolt-slash"

# The Quantizer's 12 buttons are a chromatic keyboard drawn as a clock face:
# theta = 2*pi*i/12 from 3 o'clock, so with C at 12 o'clock i=0 is D#. The
# source's black-key mask [0,3,5,7,10] confirms that rotation uniquely. Their
# note glyphs sat at radius 0.35in; the names go in the same place.
NOTES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
def _quantizer_notes():
    cx, cy, r = 25.25, 34.86, 8.89
    return [dict(x=cx + r * math.cos(math.tau * i / 12),
                 y=cy + r * math.sin(math.tau * i / 12) + 0.45,
                 text=NOTES[(i + 3) % 12], size=1.4)
            for i in range(12)]

MODULES = {
    "Biodata": dict(
        slug="fm-biodata", title="BIODATA", icons=(SPK_X, BOLT),
        # every jack/pot is already labelled by the source
    ),
    "Boost": dict(
        slug="fm-boost", title="BOOST", icons=(SPK_WAVE, BOLT_SLASH),
    ),
    "Clock": dict(
        slug="fm-clock", title="CLOCK", icons=(SPK_X, BOLT),
        # the manual calls SW1 the "navigation knob" and SW2 play/stop; J1-J8
        # are the 8 gate outputs (the faceplate script left is_output at False)
        labels=[(30.31, 45.63, "nav"), (9.99, 45.63, "play"),
                (12.53, 64.68, "1"), (27.77, 64.68, "2"),
                (12.53, 79.92, "3"), (27.77, 79.92, "4"),
                (12.53, 95.16, "5"), (27.77, 95.16, "6"),
                (12.53, 110.4, "7"), (27.77, 110.4, "8")],
        outputs=[(12.53, 64.68), (27.77, 64.68), (12.53, 79.92), (27.77, 79.92),
                 (12.53, 95.16), (27.77, 95.16), (12.53, 110.4), (27.77, 110.4)],
        # SW1 is an EC11 encoder, not a pot -- endless, so no dial tick ring
        no_ring=[(30.31, 45.63)],
    ),
    "Drift": dict(
        slug="fm-drift", title="DRIFT", icons=(SPK_X, BOLT),
        # the two unlabelled jacks are the CV inputs the source ties to the
        # Speed / Texture knobs with bezier leads
        labels=[(4.92, 89.20, "Speed"), (15.08, 89.20, "Texture")],
    ),
    "Envelope": dict(
        slug="fm-envelope", title="ENVELOPE", icons=(SPK_X, BOLT),
        # the four stage rows are labelled on their jack (one label per mode);
        # the button cycles ADSR / AR / the two looping modes
        labels=[(7.45, 15.81, "mode")],
    ),
    "Lights": dict(
        slug="fm-lights", title="LIGHTS", icons=(SPK_X, BOLT_SLASH),
    ),
    "Logic": dict(
        slug="fm-logic", title="ALU", icons=(SPK_X, BOLT),
    ),
    "Mixer": dict(
        slug="fm-mixer", title="MIXER", icons=(SPK_WAVE, BOLT),
        # five input rows; each row's jack, switch and level knob share a y
        labels=[(6.18, 21.08, "1"), (6.18, 41.40, "2"), (6.18, 61.72, "3"),
                (6.18, 82.04, "4"), (6.18, 102.36, "5"),
                (31.58, 21.08, "1"), (31.58, 41.40, "2"), (31.58, 61.72, "3"),
                (31.58, 82.04, "4")],
    ),
    "OffsetAtten": dict(
        slug="fm-offset-atten", title="O/A", icons=(SPK_X, BOLT),
        # the source sets these two rotated 90 degrees beside each knob column
        labels=[(12.54, 17.54, "Offset"), (7.46, 37.86, "Atten"),
                (12.54, 72.15, "Offset"), (7.46, 92.47, "Atten")],
    ),
    "Output": dict(
        slug="fm-output", title="OUTPUT", icons=(SPK_WAVE, BOLT_SLASH),
        # the three 1/4" jacks are the module's real outputs (headphones + line
        # L/R); the source left them at is_output=False. dB marks go beside the
        # left VU column only -- the right column mirrors it.
        labels=[(20.15, 75.42, "Phones"), (20.15, 91.93, "L"), (20.15, 108.44, "R"),
                (8.72, 79.23, "+6", "left"), (8.72, 85.58, "+3", "left"),
                (8.72, 91.93, "0", "left"), (8.72, 98.28, "-9", "left"),
                (8.72, 104.63, "-18", "left")],
        outputs=[(20.15, 75.42), (20.15, 91.93), (20.15, 108.44)],
    ),
    "Quantizer": dict(
        slug="fm-quantizer", title="QUANTIZER", icons=(SPK_X, BOLT),
        labels=[(7.47, 64.07, "shift"), (32.23, 64.07, "load"), (43.03, 64.07, "save")],
        extras=[dict(x=12.55, y=77.40, text="channel a", size=1.6, knockout=True),
                dict(x=37.95, y=77.40, text="channel b", size=1.6, knockout=True)]
               + _quantizer_notes(),
    ),
    "RNG": dict(
        slug="fm-rng", title="RNG", icons=(SPK_X, BOLT),
    ),
}

# Component class -> (semantic kind, has tick ring)
KINDS = {
    "Potentiometer": ("pot", True),
    "SmallPotentiometer": ("pot", True),
    "D6R30": ("button", False),
    "TL1105SP": ("button", False),
    "Switch": ("switch", False),
    "SmallSwitch": ("switch", False),
    "LED": ("led", False),
    "SmallLED": ("led", False),
    "JackSocket": ("jack", False),
    "JackSocketCentered": ("jack", False),
    "JackSocketQuarterInch": ("jack", False),
    "M3Bolt": ("bolt", False),
    "OLEDSPI": ("oled", False),
    "OLED": ("oled", False),
}

# ==========================================================================
# Extraction (needs the free-modular checkout + svgwrite; run rarely)
# ==========================================================================
def extract(fm_repo: pathlib.Path):
    sys.path.insert(0, str(fm_repo))
    import runpy, os
    import faceplate_maker as fm

    fm.run_inkscape = lambda *a, **k: None
    fm.Module.save = lambda self: None
    fm.Module.draw = lambda self, function: None      # decorative SVG: redrawn our way

    cur = {}
    orig_init = fm.Module.__init__

    def init(self, hp, global_y_offset=0, title=None, **kw):
        orig_init(self, hp, global_y_offset=global_y_offset, title=title, **kw)
        cur.clear()
        cur.update(hp=hp, width=self.width, y_offset=global_y_offset,
                   source_title=title, features=[])
    fm.Module.__init__ = init

    orig_add = fm.Module.add

    def add(self, c):
        ox, oy = self.width / 2, cur["y_offset"]
        px, py = c.position
        f = {"kind": type(c).__name__}
        if isinstance(c, fm.OLED):
            sx, sy = c.screen_offset
            sw, sh = c.screen_size
            f["rect"] = [round(ox + px + sx, 4), round(oy + py + sy, 4),
                         round(sw, 4), round(sh, 4)]
            f["holes"] = [[round(ox + px + hx, 4), round(oy + py + hy, 4),
                           round(fm.inches(3 / 32) / 2, 4)]
                          for hx, hy in c._get_hole_locations()]
        else:
            f["c"] = [round(ox + px + c.offset[0], 4), round(oy + py + c.offset[1], 4)]
            f["r"] = round(c.radius, 4)
        for attr in ("label", "is_output", "left_text", "right_text", "label_above"):
            if hasattr(c, attr):
                v = getattr(c, attr)
                if isinstance(v, (str, bool)) or v is None:
                    f[attr] = v
        cur["features"].append(f)
        return orig_add(self, c)
    fm.Module.add = add

    out = {}
    for d in sorted(p for p in (fm_repo / "modules").iterdir()
                    if (p / "Faceplate").is_dir()):
        script = next(iter((d / "Faceplate").glob("make_*.py")), None)
        if script is None:
            continue
        cwd = os.getcwd()
        os.chdir(script.parent)
        sys.argv = [str(script)]
        try:
            runpy.run_path(str(script), run_name="__main__")
            out[d.name] = dict(cur)
            print(f"  {d.name:14} hp={cur['hp']:2}  w={cur['width']:5}  "
                  f"{len(cur['features'])} features")
        finally:
            os.chdir(cwd)
    GEOM.write_text(json.dumps(out, indent=1) + "\n")
    print(f"wrote {GEOM.relative_to(REPO)}")

# ==========================================================================
# Glyph library -- lifted verbatim from existing panels so the new plates use
# the identical artwork (not a re-vectorisation of the same Heroicons).
# ==========================================================================
def _blocks(text, tag):
    out = []
    for m in re.finditer(r"\(%s\b" % tag, text):
        s = m.start()
        d = 0
        for i in range(s, len(text)):
            if text[i] == "(":
                d += 1
            elif text[i] == ")":
                d -= 1
                if d == 0:
                    out.append(text[s:i + 1])
                    break
    return out

def _silk_polys(panel):
    t = (PANELS / panel / f"{panel}.kicad_pcb").read_text()
    out = []
    for b in _blocks(t, "gr_poly"):
        if '(layer "B.SilkS")' not in b:
            continue
        out.append([(float(x), float(y))
                    for x, y in re.findall(r"\(xy ([-\d.]+) ([-\d.]+)\)", b)])
    return out

def _bbox(polys):
    xs = [p[0] for poly in polys for p in poly]
    ys = [p[1] for poly in polys for p in poly]
    return min(xs), min(ys), max(xs), max(ys)

def _cluster(polys, gap):
    groups = []
    for poly in polys:
        bb = _bbox([poly])
        for g in groups:
            gb = g["bb"]
            if not (bb[0] > gb[2] + gap or bb[2] < gb[0] - gap or
                    bb[1] > gb[3] + gap or bb[3] < gb[1] - gap):
                g["bb"] = (min(gb[0], bb[0]), min(gb[1], bb[1]),
                           max(gb[2], bb[2]), max(gb[3], bb[3]))
                g["polys"].append(poly)
                break
        else:
            groups.append({"bb": bb, "polys": [poly]})
    changed = True
    while changed:
        changed = False
        for i in range(len(groups)):
            for j in range(i + 1, len(groups)):
                a, b = groups[i]["bb"], groups[j]["bb"]
                if not (a[0] > b[2] + gap or a[2] < b[0] - gap or
                        a[1] > b[3] + gap or a[3] < b[1] - gap):
                    groups[i]["bb"] = (min(a[0], b[0]), min(a[1], b[1]),
                                       max(a[2], b[2]), max(a[3], b[3]))
                    groups[i]["polys"] += groups[j]["polys"]
                    groups.pop(j)
                    changed = True
                    break
            if changed:
                break
    return groups

def _centred(polys, scale=1.0):
    """Normalise a glyph so (0,0) is its bbox centre, optionally rescaling."""
    x0, y0, x1, y1 = _bbox(polys)
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    return {"polys": [[((x - cx) * scale, (y - cy) * scale) for x, y in poly]
                      for poly in polys],
            "w": (x1 - x0) * scale, "h": (y1 - y0) * scale}

ICON_ROW_H = 2.5      # the mod1 rows are 2.77 tall, the mod2 rows 2.23; split the
ICON_GAP = 0.85

def _icon_row(panel):
    """Split an existing panel's signal-icon row into its two glyphs.

    The row is the small multi-poly cluster just under the output label. A 0.6mm
    split keeps a glyph's own pieces together (the speaker's mute-X sits 0.39mm
    off its body) while separating the speaker from the bolt (>=0.97mm). On a
    back-side-out panel the *visible* left glyph is the one at HIGH x.
    """
    cs = [c for c in _cluster(_silk_polys(panel), 1.2)
          if c["bb"][2] - c["bb"][0] < 8 and c["bb"][3] - c["bb"][1] < 4
          and len(c["polys"]) > 1]
    row = min(cs, key=lambda c: abs((c["bb"][1] + c["bb"][3]) / 2 - 120.9))
    scale = ICON_ROW_H / (row["bb"][3] - row["bb"][1])
    halves = sorted(_cluster(row["polys"], 0.6), key=lambda c: -c["bb"][0])
    if len(halves) != 2:
        raise SystemExit(f"{panel}: icon row split into {len(halves)} parts, expected 2")
    return [_centred(h["polys"], scale) for h in halves]

def load_glyphs():
    """ring, arrow, speaker-wave, speaker-x, bolt, bolt-slash."""
    g = {}
    lfo = _cluster(_silk_polys("mod1-lfo"), 1.2)
    rings = [c for c in lfo if abs(c["bb"][2] - c["bb"][0] - RING_SIZE) < 0.1]
    g["ring"] = _centred(rings[0]["polys"])
    arrows = [c for c in lfo if len(c["polys"]) == 1 and 1.8 < c["bb"][2] - c["bb"][0] < 2.0]
    g["arrow"] = _centred(arrows[0]["polys"])
    g[SPK_WAVE], g[BOLT] = _icon_row("mod1-lfo")        # audio + CV
    g[SPK_X] = _icon_row("mod1-eg")[0]                  # CV only
    g[BOLT_SLASH] = _icon_row("mod2-clap")[1]           # audio only
    return g

# ==========================================================================
# Text metrics -- rough Comfortaa advance widths, good enough for collision
# tests and title fitting (KiCad regenerates the real glyphs from the font).
# ==========================================================================
_NARROW = set("iIljt.,;:'|!()[]")
_WIDE = set("mwMW")
_EM = {"lower": 0.837, "upper": 0.977, "digit": 0.729, "narrow": 0.463,
       "wide": 1.208, "space": 0.417, "punct": 0.753}
ASCENT, DESCENT, LINE_H = 1.12, 0.30, 1.70   # em fractions of `size`
KNOCKOUT_PAD = 0.75                          # KiCad's inverted-text box margin

def _char_w(ch):
    if ch == " ":
        return _EM["space"]
    if ch in _NARROW:
        return _EM["narrow"]
    if ch in _WIDE:
        return _EM["wide"]
    if ch.isdigit():
        return _EM["digit"]
    if ch.isupper():
        return _EM["upper"]
    if ch.islower():
        return _EM["lower"]
    return _EM["punct"]

def text_w(s, size):
    """Ink width in mm. The per-class em widths are a least-squares fit over the
    156 distinct (string, size) pairs whose render_cache is stored in the
    existing mod1/mod2 panels -- residual sigma 0.5mm."""
    return max((sum(_char_w(c) for c in line) for line in s.split("\n")), default=0) * size

def text_h(s, size):
    return ((s.count("\n")) * LINE_H + ASCENT + DESCENT) * size

def text_rect(x, y, s, size, justify="center", mirror=True):
    """Bounding box of a `bottom`-justified text whose baseline is at y.

    Mirrored (back-layer) text is justified as *read on the finished panel*, so a
    `left`-justified mirrored string grows toward -x in KiCad space. Getting this
    backwards is what would push the credit line off the board edge.
    """
    w, h = text_w(s, size), text_h(s, size)
    if justify == "center":
        x0 = x - w / 2
    elif (justify == "left") != mirror:      # left+front, or right+back
        x0 = x
    else:
        x0 = x - w
    return (x0, y - h + DESCENT * size, x0 + w, y + DESCENT * size)

# ==========================================================================
# Layout
# ==========================================================================
def rects_hit(a, b, pad=0.0):
    return not (a[0] > b[2] + pad or a[2] < b[0] - pad or
                a[1] > b[3] + pad or a[3] < b[1] - pad)

def rect_hits_circle(r, c, pad=0.0):
    cx, cy, rad = c
    nx = min(max(cx, r[0]), r[2])
    ny = min(max(cy, r[1]), r[3])
    return math.hypot(cx - nx, cy - ny) < rad + pad

class Layout:
    """Keepout bookkeeping.

    `hard` = holes, tick rings, mounting slots: nothing may sit on them.
    `soft` = already-placed silk (text, rules, glyphs). The fixed vertical grid
    (brand / title / rules / signature / credit) is checked against `hard` only,
    because the grid's own items are designed to sit 1mm apart -- treating them
    as mutual obstacles would shrink every title and drop every rule line.
    """
    def __init__(self, width):
        self.W = width
        self.hard_rects, self.hard_circles, self.soft = [], [], []

    def block(self, rect, hard=False):
        (self.hard_rects if hard else self.soft).append(rect)

    def block_circle(self, cx, cy, r):
        self.hard_circles.append((cx, cy, r))

    def in_board(self, rect):
        return (rect[0] >= MARGIN and rect[2] <= self.W - MARGIN
                and rect[1] >= 0.4 and rect[3] <= HEIGHT - 0.4)

    def free(self, rect, pad=0.3, soft=True, ignore=()):
        if not self.in_board(rect):
            return False
        if any(rects_hit(rect, b, pad) for b in self.hard_rects):
            return False
        if any(rect_hits_circle(rect, c, pad) for c in self.hard_circles
               if c not in ignore):
            return False
        return not soft or not any(rects_hit(rect, b, pad) for b in self.soft)

def nearest_band(y, pad, band_free):
    """Closest dy to `y` (within +/-8mm, 0.5 steps) whose rule band is clear."""
    cand = sorted([y + i * 0.5 * s for i in range(0, 17) for s in (1, -1)],
                  key=lambda v: abs(v - y))
    return next((yy for yy in cand if band_free(yy, pad)), None)

def mount_holes(width, hp):
    """Mounting cutouts: the house rounded slot, or a round M3 hole where a slot
    would not fit -- an 8.5mm stadium does not fit a 9.8mm face, so <=3 HP gets a
    3.2mm hole at free-modular's own off-centre position (their formula, mirrored
    into KiCad space), which leaves a usable strip for the brand and credit."""
    if hp <= 3:
        x = width - (width / 2 - 5.08 * (hp - 3) / 2)
        return [("hole", x, dy, 1.6) for dy in SLOT_DY]
    xs = [SLOT_X] + ([width - SLOT_X] if hp >= 6 else [])
    return [("slot", x, dy, None) for x in xs for dy in SLOT_DY]

# ==========================================================================
# S-expression emitters (board-local coords; sheet origin added here)
# ==========================================================================
OX, OY = BOARD_ORIGIN

def fmt(v):
    if v == 0:
        v = 0.0
    return f"{v:.4f}".rstrip("0").rstrip(".")

def em_line(x0, y0, x1, y1, width, layer):
    return (f"\t(gr_line\n\t\t(start {fmt(OX+x0)} {fmt(OY+y0)})\n\t\t(end {fmt(OX+x1)} {fmt(OY+y1)})\n"
            f"\t\t(stroke\n\t\t\t(width {fmt(width)})\n\t\t\t(type solid)\n\t\t)\n"
            f"\t\t(layer \"{layer}\")\n\t\t(uuid \"{next_uuid()}\")\n\t)\n")

def em_circle(cx, cy, r, width, layer):
    return (f"\t(gr_circle\n\t\t(center {fmt(OX+cx)} {fmt(OY+cy)})\n\t\t(end {fmt(OX+cx+r)} {fmt(OY+cy)})\n"
            f"\t\t(stroke\n\t\t\t(width {fmt(width)})\n\t\t\t(type solid)\n\t\t)\n"
            f"\t\t(fill no)\n\t\t(layer \"{layer}\")\n\t\t(uuid \"{next_uuid()}\")\n\t)\n")

def em_arc(sx, sy, mx, my, ex, ey, width, layer):
    return (f"\t(gr_arc\n\t\t(start {fmt(OX+sx)} {fmt(OY+sy)})\n\t\t(mid {fmt(OX+mx)} {fmt(OY+my)})\n"
            f"\t\t(end {fmt(OX+ex)} {fmt(OY+ey)})\n"
            f"\t\t(stroke\n\t\t\t(width {fmt(width)})\n\t\t\t(type solid)\n\t\t)\n"
            f"\t\t(layer \"{layer}\")\n\t\t(uuid \"{next_uuid()}\")\n\t)\n")

def em_poly(pts, layer):
    rows = ["\t\t\t" + " ".join(f"(xy {fmt(OX+x)} {fmt(OY+y)})" for x, y in pts[k:k + 4])
            for k in range(0, len(pts), 4)]
    return (f"\t(gr_poly\n\t\t(pts\n" + "\n".join(rows) + "\n\t\t)\n"
            f"\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type solid)\n\t\t)\n"
            f"\t\t(fill yes)\n\t\t(layer \"{layer}\")\n\t\t(uuid \"{next_uuid()}\")\n\t)\n")

def em_text(x, y, s, size, layer="B.SilkS", knockout=False, mirror=True,
            justify="center", thickness=None, bold=False, angle=0):
    j = [] if justify == "center" else [justify]
    j.append("bottom")
    if mirror:
        j.append("mirror")
    thickness = thickness if thickness is not None else max(0.15, size * 0.115)
    lay = f"{layer}\" knockout" if knockout else f"{layer}\""
    s = s.replace("\n", "\\n")     # KiCad stores line breaks as an escape, not a raw newline
    return (f"\t(gr_text \"{s}\"\n"
            f"\t\t(at {fmt(OX+x)} {fmt(OY+y)} {fmt(angle)})\n"
            f"\t\t(layer \"{lay})\n\t\t(uuid \"{next_uuid()}\")\n"
            f"\t\t(effects\n\t\t\t(font\n\t\t\t\t(face \"Comfortaa\")\n"
            f"\t\t\t\t(size {fmt(size)} {fmt(size)})\n\t\t\t\t(thickness {fmt(thickness)})\n"
            + ("\t\t\t\t(bold yes)\n" if bold else "")
            + f"\t\t\t)\n\t\t\t(justify {' '.join(j)})\n\t\t)\n\t)\n")

def em_glyph(glyph, cx, cy, layer="B.SilkS"):
    return "".join(em_poly([(cx + x, cy + y) for x, y in poly] + [(cx + poly[0][0], cy + poly[0][1])],
                           layer)
                   for poly in glyph["polys"])

HEADER = (
    "(kicad_pcb\n\t(version 20241229)\n\t(generator \"pcbnew\")\n\t(generator_version \"9.0\")\n"
    "\t(general\n\t\t(thickness 1.6)\n\t\t(legacy_teardrops no)\n\t)\n\t(paper \"A4\")\n"
    "\t(layers\n"
    "\t\t(0 \"F.Cu\" signal)\n\t\t(2 \"B.Cu\" signal)\n"
    "\t\t(9 \"F.Adhes\" user \"F.Adhesive\")\n\t\t(11 \"B.Adhes\" user \"B.Adhesive\")\n"
    "\t\t(13 \"F.Paste\" user)\n\t\t(15 \"B.Paste\" user)\n"
    "\t\t(5 \"F.SilkS\" user \"F.Silkscreen\")\n\t\t(7 \"B.SilkS\" user \"B.Silkscreen\")\n"
    "\t\t(1 \"F.Mask\" user)\n\t\t(3 \"B.Mask\" user)\n"
    "\t\t(17 \"Dwgs.User\" user \"User.Drawings\")\n\t\t(19 \"Cmts.User\" user \"User.Comments\")\n"
    "\t\t(21 \"Eco1.User\" user \"User.Eco1\")\n\t\t(23 \"Eco2.User\" user \"User.Eco2\")\n"
    "\t\t(25 \"Edge.Cuts\" user)\n\t\t(27 \"Margin\" user)\n"
    "\t\t(31 \"F.CrtYd\" user \"F.Courtyard\")\n\t\t(29 \"B.CrtYd\" user \"B.Courtyard\")\n"
    "\t\t(35 \"F.Fab\" user)\n\t\t(33 \"B.Fab\" user)\n\t)\n"
    "\t(setup\n\t\t(pad_to_mask_clearance 0)\n\t\t(allow_soldermask_bridges_in_footprints no)\n"
    "\t\t(tenting front back)\n\t\t(pcbplotparams\n"
    "\t\t\t(layerselection 0x00000000_00000000_55555555_5755f5fa)\n"
    "\t\t\t(plot_on_all_layers_selection 0x00000000_00000000_00000000_00000000)\n"
    "\t\t\t(disableapertmacros no)\n\t\t\t(usegerberextensions no)\n\t\t\t(usegerberattributes yes)\n"
    "\t\t\t(usegerberadvancedattributes yes)\n\t\t\t(creategerberjobfile no)\n\t\t\t(gerberprecision 5)\n"
    "\t\t\t(dashed_line_dash_ratio 12.000000)\n\t\t\t(dashed_line_gap_ratio 3.000000)\n"
    "\t\t\t(svgprecision 4)\n\t\t\t(plotframeref no)\n\t\t\t(mode 1)\n\t\t\t(useauxorigin no)\n"
    "\t\t\t(hpglpennumber 1)\n\t\t\t(hpglpenspeed 20)\n\t\t\t(hpglpendiameter 15.000000)\n"
    "\t\t\t(pdf_front_fp_property_popups yes)\n\t\t\t(pdf_back_fp_property_popups yes)\n"
    "\t\t\t(pdf_metadata yes)\n\t\t\t(pdf_single_document no)\n\t\t\t(dxfpolygonmode yes)\n"
    "\t\t\t(dxfimperialunits yes)\n\t\t\t(dxfusepcbnewfont yes)\n\t\t\t(psnegative no)\n"
    "\t\t\t(psa4output no)\n\t\t\t(plot_black_and_white yes)\n\t\t\t(sketchpadsonfab no)\n"
    "\t\t\t(plotpadnumbers no)\n\t\t\t(hidednponfab no)\n\t\t\t(sketchdnponfab yes)\n"
    "\t\t\t(crossoutdnponfab yes)\n\t\t\t(subtractmaskfromsilk no)\n\t\t\t(outputformat 1)\n"
    "\t\t\t(mirror no)\n\t\t\t(drillshape 0)\n\t\t\t(scaleselection 1)\n\t\t\t(outputdirectory \"./\")\n"
    "\t\t)\n\t)\n\t(net 0 \"\")\n"
)

def em_outline(W):
    """Board rectangle + the two rounded mounting slots (stadium outlines)."""
    p = [em_line(0, 0, W, 0, EDGE_W, "Edge.Cuts"),
         em_line(W, 0, W, HEIGHT, EDGE_W, "Edge.Cuts"),
         em_line(W, HEIGHT, 0, HEIGHT, EDGE_W, "Edge.Cuts"),
         em_line(0, HEIGHT, 0, 0, EDGE_W, "Edge.Cuts")]
    return p

def em_slot(cx, cy):
    r = SLOT_H / 2
    x0, x1 = cx - SLOT_STRAIGHT / 2, cx + SLOT_STRAIGHT / 2
    y0, y1 = cy - r, cy + r
    return [em_line(x0, y0, x1, y0, EDGE_W, "Edge.Cuts"),
            em_line(x0, y1, x1, y1, EDGE_W, "Edge.Cuts"),
            em_arc(x0, y1, x0 - r, cy, x0, y0, EDGE_W, "Edge.Cuts"),
            em_arc(x1, y0, x1 + r, cy, x1, y1, EDGE_W, "Edge.Cuts")]

# ==========================================================================
# Panel builder
# ==========================================================================
class Warn:
    def __init__(self, name):
        self.name, self.msgs = name, []
    def __call__(self, msg):
        self.msgs.append(msg)

def build(mod_name, data, spec, glyphs, warn):
    W = data["width"]
    hp = data["hp"]
    parts = [HEADER]
    lay = Layout(W)

    # ---- cutouts (mirrored into KiCad space; radii stay the source's) -----
    cuts = []          # dicts: kind, x, y, r, label, is_output, ring
    label_over = spec.get("labels", [])
    out_over = spec.get("outputs", [])

    def nearest_override(x, y, table):
        best, bd = None, 9.0
        for entry in table:
            d = (entry[0] - x) ** 2 + (entry[1] - y) ** 2
            if d < bd:
                bd, best = d, entry
        return best

    for f in data["features"]:
        kind, ring = KINDS[f["kind"]]
        if kind == "oled":
            rx, ry, rw, rh = f["rect"]
            x0, x1 = W - (rx + rw), W - rx
            parts += [em_line(x0, ry, x1, ry, EDGE_W, "Edge.Cuts"),
                      em_line(x1, ry, x1, ry + rh, EDGE_W, "Edge.Cuts"),
                      em_line(x1, ry + rh, x0, ry + rh, EDGE_W, "Edge.Cuts"),
                      em_line(x0, ry + rh, x0, ry, EDGE_W, "Edge.Cuts")]
            lay.block((x0, ry, x1, ry + rh), hard=True)
            for hx, hy, hr in f["holes"]:
                parts.append(em_circle(W - hx, hy, hr, EDGE_W, "Edge.Cuts"))
                lay.block_circle(W - hx, hy, hr)
            continue
        x, y = W - f["c"][0], f["c"][1]
        r = f["r"]
        parts.append(em_circle(x, y, r, EDGE_W, "Edge.Cuts"))
        lay.block_circle(x, y, r)
        label = f.get("label") or None
        ov = nearest_override(f["c"][0], f["c"][1], label_over)
        if ov:
            label = ov[2]
        is_out = bool(f.get("is_output"))
        if nearest_override(f["c"][0], f["c"][1], out_over):
            is_out = True
        if nearest_override(f["c"][0], f["c"][1], spec.get("no_ring", [])):
            ring = False
        cuts.append(dict(kind=kind, x=x, y=y, r=r, ring=ring, label=label,
                         is_output=is_out, side=(ov[3] if ov and len(ov) > 3 else None),
                         left_text=f.get("left_text"), right_text=f.get("right_text"),
                         label_above=bool(f.get("label_above"))))

    # tick rings: continuous pots only (selectors/encoders get discrete labels)
    for c in cuts:
        if c["ring"]:
            parts.append(em_glyph(glyphs["ring"], c["x"], c["y"]))
            h = RING_SIZE / 2
            lay.block((c["x"] - h, c["y"] - h, c["x"] + h, c["y"] + h), hard=True)

    # ---- board outline + mounting cutouts ---------------------------------
    parts += em_outline(W)
    for shape, mx, my, mr in mount_holes(W, hp):
        if shape == "slot":
            parts += em_slot(mx, my)
            half = SLOT_STRAIGHT / 2 + SLOT_H / 2
            lay.block((mx - half, my - SLOT_H / 2, mx + half, my + SLOT_H / 2), hard=True)
        else:
            parts.append(em_circle(mx, my, mr, EDGE_W, "Edge.Cuts"))
            lay.block_circle(mx, my, mr)

    # ---- B.Mask rule lines (before the text, so labels can dodge them) -----
    def rule(y):
        return em_line(-RULE_OVER_L, y, W + RULE_OVER_R, y, RULE_W, "B.Mask")

    def band_free(y, pad, soft=True):
        band = (MARGIN, y - RULE_W / 2 - pad, W - MARGIN, y + RULE_W / 2 + pad)
        return lay.free(band, pad=0.0, soft=soft)

    for y in RULE_HEADER + (RULE_FOOTER,):
        # header pair and footer are hard grid: drawn even across a cutout, since
        # a mask stripe over a hole is cosmetic, and the bracket is the identity
        parts.append(rule(y))
        lay.block((-RULE_OVER_L, y - 0.6, W + RULE_OVER_R, y + 0.6))
        if not band_free(y, 0.2, soft=False):
            warn(f"rule line at dy {y} crosses a cutout")
    # Section dividers, pass 1: claim the wide-open gaps (5mm of air either side)
    # before any label is placed, so the panel keeps the family's three-band
    # structure wherever the layout genuinely has room for it.
    pending = []
    for y in RULE_DIVIDERS:
        alt = nearest_band(y, 5.0, band_free)
        if alt is None:
            pending.append(y)
            continue
        if abs(alt - y) > 0.01:
            warn(f"divider at dy {y} shifted to {alt:.2f}")
        parts.append(rule(alt))
        lay.block((-RULE_OVER_L, alt - 0.6, W + RULE_OVER_R, alt + 0.6))

    # ---- fixed grid text ---------------------------------------------------
    # brand: one line where the panel is wide enough, else the family's stack,
    # shrinking only on the faces too narrow to hold it beside the mounting hole
    brand = ([("maddie synths", SIZE_BRAND)] if hp >= 8 else []) + \
            [("maddie\nsynths", s) for s in (SIZE_BRAND, 1.4, 1.2, 1.0, 0.9)]
    parts.append(place_fixed(lay, brand, DY_BRAND, W, warn, "brand", +1))
    # title: largest size <= 2.5 whose knockout box clears the edges and any
    # cutout at title height
    title = spec["title"]
    size = SIZE_TITLE_MAX
    while size > 1.4:
        r = text_rect(W / 2, DY_TITLE, title, size)
        box = (r[0] - KNOCKOUT_PAD, r[1] - KNOCKOUT_PAD * 0.6,
               r[2] + KNOCKOUT_PAD, r[3] + KNOCKOUT_PAD * 0.6)
        if lay.free(box, pad=0.0, soft=False):
            break
        size = round(size - 0.1, 2)
    parts.append(em_text(W / 2, DY_TITLE, title, size, knockout=True,
                         thickness=0.2, bold=True))
    lay.block(text_rect(W / 2, DY_TITLE, title, size))
    # madelyn.sh lives on the hidden face -- no silk collision, no mirror; only
    # constraint is the board edge, so it shrinks on the narrow faces
    sign_size = min(SIZE_SIGN, SIZE_SIGN * (W - 2 * MARGIN) / max(
        text_w("madelyn.sh", SIZE_SIGN), 1e-6))
    parts.append(em_text(W / 2, DY_SIGN, "madelyn.sh", round(sign_size, 2),
                         layer="F.SilkS", knockout=True, mirror=False, thickness=0.5))
    # attribution -- CC-BY-SA 4.0 on their hardware makes this a licence
    # condition, so it degrades to a shorter form rather than being dropped
    credit = [("pcb by free modular", SIZE_CREDIT),
              ("pcb by\nfree modular", SIZE_CREDIT),
              ("free\nmodular", SIZE_CREDIT),
              ("free\nmodular", 1.2), ("free\nmodular", 1.0), ("free\nmodular", 0.85)]
    parts.append(place_fixed(lay, credit, DY_CREDIT, W, warn, "credit", -1,
                             thickness=0.1))

    # ---- control labels ----------------------------------------------------
    for c in sorted(cuts, key=lambda c: (c["kind"] != "jack", c["y"])):
        parts += label_cut(lay, c, glyphs, warn)

    # ---- editorial extras (section headers, the note ring) ----------------
    for e in spec.get("extras", []):
        x, y, s, size = W - e["x"], e["y"], e["text"], e["size"]
        r = text_rect(x, y, s, size)
        if not lay.free(r, pad=0.15):
            warn(f"extra {s!r} at ({e['x']:.1f},{y:.1f}) overlaps something")
        lay.block(r)
        parts.append(em_text(x, y, s, size, knockout=e.get("knockout", False)))

    # Section dividers, pass 2: whatever pass 1 could not fit gets one more try
    # now that the labels are down, on a narrower 2.4mm band that must also clear
    # the text. A divider gives way to a label, never the other way round -- a
    # stripe that makes a row flip its label reads worse than no stripe, and a
    # panel with no divider is the sequencer faceplate's precedent (section 9).
    for y in pending:
        alt = nearest_band(y, 2.4, band_free)
        if alt is None:
            warn(f"divider at dy {y} has no free position -- omitted")
            continue
        warn(f"divider at dy {y} shifted to {alt:.2f}")
        parts.append(rule(alt))
        lay.block((-RULE_OVER_L, alt - 0.6, W + RULE_OVER_R, alt + 0.6))

    # ---- signal-level icon pair -------------------------------------------
    parts += place_icons(lay, glyphs, spec["icons"], W, warn)

    parts.append(")\n")
    return "".join(parts)

def place_fixed(lay, forms, dy, W, warn, what, slide, thickness=None):
    """Place a fixed-grid string on its grid line.

    `forms` is a preference-ordered list of (text, size) -- e.g. the credit line
    as one line, then two, then just the project name. Each is tried centred and
    then slid sideways past a mounting cutout, which is exactly what mod1-lfo
    does with its own brand and credit. Only if no form fits anywhere on the line
    does the placer step `slide` mm at a time off the grid, and that is reported
    as a deviation.
    """
    def attempt(y, soft):
        for text, size in forms:
            for x, just in ((W / 2, "center"),
                            (W - MARGIN, "left"),      # mirrored: grows toward -x
                            (MARGIN, "right")):
                r = text_rect(x, y, text, size, just)
                if lay.free(r, pad=0.2, soft=soft):
                    lay.block(r)
                    return em_text(x, y, text, size, justify=just, thickness=thickness)
        return None

    got = attempt(dy, soft=False)
    if got:
        return got
    for i in range(1, 25):
        y = dy + i * 0.5 * slide
        got = attempt(y, soft=True)
        if got:
            warn(f"{what} moved to dy {y:.2f} (nothing free on the grid line)")
            return got
    text, size = forms[-1]
    warn(f"{what} has no clear position at dy {dy} -- centred anyway")
    lay.block(text_rect(W / 2, dy, text, size))
    return em_text(W / 2, dy, text, size, thickness=thickness)

def label_cut(lay, c, glyphs, warn):
    """Label one cutout, plus the input arrow on input jacks."""
    out = []
    if c["kind"] == "jack" and not c["is_output"]:
        # the arrow deliberately tucks into its own jack's corner, so that hole
        # is exempt; try the mirrored position, then above, before giving up
        own = (c["x"], c["y"], c["r"])
        g = glyphs["arrow"]
        # The house offset is measured against a 6.2mm jack; free-modular's jacks
        # are 6.3mm, so push the glyph out along the same diagonal by whatever it
        # takes to keep its corner off the hole (it would otherwise trip
        # silk_edge_clearance, which the mod panels do not).
        d0 = math.hypot(ARROW_DX, ARROW_DY)
        corner = math.hypot(g["w"] / 2, g["h"] / 2)
        k = max(1.0, (c["r"] + 0.25 + corner) / d0)
        adx, ady = ARROW_DX * k, ARROW_DY * k
        for dx, dy in ((adx, ady), (-adx, ady), (adx, -ady), (-adx, -ady)):
            ax, ay = c["x"] + dx, c["y"] + dy
            r = (ax - g["w"] / 2, ay - g["h"] / 2, ax + g["w"] / 2, ay + g["h"] / 2)
            if lay.free(r, pad=0.1, ignore=(own,)):
                out.append(em_glyph(g, ax, ay))
                lay.block(r)
                break
        else:
            warn(f"no room for the input arrow on {c['label'] or c['kind']}")

    if c["kind"] == "switch" and (c["left_text"] or c["right_text"]):
        for text, sign in ((c["left_text"], 1), (c["right_text"], -1)):
            if not text:
                continue
            # `left` on the finished panel is +x in mirrored KiCad space
            x = c["x"] + sign * (c["r"] + 0.6 + text_w(text, SIZE_SMALL) / 2)
            y = c["y"] + SIZE_SMALL / 2
            r = text_rect(x, y, text, SIZE_SMALL)
            if lay.free(r):
                out.append(em_text(x, y, text, SIZE_SMALL))
                lay.block(r)
            else:
                warn(f"switch label {text!r} collides -- omitted")
        return out

    label = c["label"]
    if not label:
        return out
    if c["kind"] == "pot":
        size, clear = SIZE_POT, RING_SIZE / 2 if c["ring"] else c["r"]
    elif c["kind"] == "jack":
        size, clear = SIZE_JACK, c["r"]
    else:
        size, clear = SIZE_SMALL, c["r"]
    knock = c["kind"] == "jack" and c["is_output"]

    # preference: outputs read below their jack, inputs above; pots below the
    # ring. An explicit `side` in the override table wins (dB marks, etc).
    below_first = knock or (c["kind"] == "pot")
    if c["kind"] == "jack" and c["label_above"]:
        below_first = False

    def stacked(below, size):
        y = (c["y"] + clear + 0.9 + size) if below else (c["y"] - clear - 0.9 - DESCENT * size)
        return (c["x"], y, "center")

    def beside(left, size):
        # `left` as read on the finished panel = +x in mirrored KiCad space
        s = 1 if left else -1
        return (c["x"] + s * (clear + 0.8 + text_w(label, size) / 2),
                c["y"] + size * 0.45, "center")

    while size >= 1.2:
        if c["side"]:
            order = {"left": [beside(True, size), beside(False, size)],
                     "right": [beside(False, size), beside(True, size)],
                     "above": [stacked(False, size), stacked(True, size)],
                     "below": [stacked(True, size), stacked(False, size)]}[c["side"]]
            cands = order + [stacked(below_first, size)]
        else:
            cands = [stacked(below_first, size), stacked(not below_first, size),
                     beside(True, size), beside(False, size)]
        for x, y, just in cands:
            r = text_rect(x, y, label, size, just)
            if lay.free(r):
                out.append(em_text(x, y, label, size, knockout=knock, justify=just))
                lay.block(r if not knock else (r[0] - 0.4, r[1] - 0.3, r[2] + 0.4, r[3] + 0.3))
                return out
        size = round(size - 0.2, 2)
    warn(f"label {label!r} ({c['kind']}) has no clear position -- placed below anyway")
    y = c["y"] + clear + 0.9 + SIZE_SMALL
    out.append(em_text(c["x"], y, label, SIZE_SMALL, knockout=knock))
    lay.block(text_rect(c["x"], y, label, SIZE_SMALL))
    return out

def place_icons(lay, glyphs, names, W, warn):
    left, right = glyphs[names[0]], glyphs[names[1]]
    gap = 0.9
    total_w = left["w"] + gap + right["w"]
    total_h = max(left["h"], right["h"])
    # nominal dy 91.5 first, then downward through the I/O half (the icons
    # belong with the outputs), and only then back up into the control half
    ys = ([91.5, 94.0, 89.0, 96.5, 87.0]
          + [y / 2 for y in range(2 * 85, 2 * 120)]
          + [y / 2 for y in range(2 * 84, 2 * 16, -1)])
    for y in ys:
        for xr in (MARGIN + 0.2 + total_w, W / 2 + total_w / 2, W - MARGIN - 0.2):
            # xr = right edge of the pair in KiCad space (= visible left)
            rect = (xr - total_w, y - total_h / 2, xr, y + total_h / 2)
            if lay.free(rect, pad=0.7):
                lay.block(rect)
                # visible-left glyph (speaker) sits at high x on a mirrored panel
                out = [em_glyph(left, xr - left["w"] / 2, y),
                       em_glyph(right, xr - left["w"] - gap - right["w"] / 2, y)]
                return out
    warn("no free position for the signal icon pair -- omitted")
    return []

def write_pro(dst, name):
    d = json.loads(PRO_TEMPLATE.read_text())
    d.setdefault("meta", {})["filename"] = f"{name}.kicad_pro"
    dst.write_text(json.dumps(d, indent=2) + "\n")

# Fabrication Toolkit settings, copied from the mod panels so a gerber export
# from one of these behaves like an export from any other panel in the repo.
FAB_OPTIONS = (PANELS / "mod2-clap" / "fabrication-toolkit-options.json")

# ==========================================================================
def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--extract", action="store_true",
                    help="re-read geometry from a free-modular checkout")
    ap.add_argument("--fm-repo", type=pathlib.Path, default=DEFAULT_FM_REPO)
    ap.add_argument("--force", "-f", action="store_true",
                    help="overwrite panels that already exist")
    ap.add_argument("--only", help="comma-separated module names (e.g. Boost,RNG)")
    ap.add_argument("--verify", action="store_true",
                    help="check every generated panel still carries each source hole "
                         "at the mirrored coordinate with the source diameter")
    ap.add_argument("--list", action="store_true",
                    help="print each module's features with the source coordinates "
                         "used to key the `labels` / `outputs` override tables")
    args = ap.parse_args()

    if args.verify:
        geom = json.loads(GEOM.read_text())
        bad = 0
        for name, spec in MODULES.items():
            data, slug = geom[name], spec["slug"]
            pcb = PANELS / slug / f"{slug}.kicad_pcb"
            if not pcb.exists():
                print(f"  {slug:18} MISSING")
                bad += 1
                continue
            t = pcb.read_text()
            circles = []
            for b in _blocks(t, "gr_circle"):
                if '(layer "Edge.Cuts")' not in b:
                    continue
                c = re.search(r"\(center ([-\d.]+) ([-\d.]+)\)", b)
                e = re.search(r"\(end ([-\d.]+) ([-\d.]+)\)", b)
                circles.append((float(c.group(1)) - OX, float(c.group(2)) - OY,
                                float(e.group(1)) - float(c.group(1))))
            miss = []
            W = data["width"]
            want = []
            for f in data["features"]:
                if "c" in f:
                    want.append((W - f["c"][0], f["c"][1], f["r"]))
                else:
                    want += [(W - hx, hy, hr) for hx, hy, hr in f["holes"]]
            for x, y, r in want:
                if not any(abs(cx - x) < 1e-3 and abs(cy - y) < 1e-3 and abs(cr - r) < 1e-3
                           for cx, cy, cr in circles):
                    miss.append((x, y, r))
            print(f"  {slug:18} {len(want) - len(miss):3}/{len(want):3} holes match"
                  + ("" if not miss else f"   MISSING {miss}"))
            bad += len(miss)
        print("OK -- every hole is where the source put it" if not bad
              else f"FAILED: {bad} discrepancies")
        return 1 if bad else 0

    if args.list:
        geom = json.loads(GEOM.read_text())
        for name, spec in MODULES.items():
            data = geom[name]
            print(f"\n== {name}  ({data['hp']} HP, {data['width']} mm)")
            for f in data["features"]:
                pos = f"{f['c'][0]:6.2f},{f['c'][1]:6.2f}" if "c" in f else \
                      f"rect {f['rect'][0]:.2f},{f['rect'][1]:.2f}"
                flag = "out" if f.get("is_output") else ("in" if f.get("label") is not None
                                                         and KINDS[f["kind"]][0] == "jack" else "")
                print(f"   {f['kind']:22} {pos}  {flag:3} {f.get('label') or ''}")
        return

    if args.extract:
        extract(args.fm_repo.resolve())
        return

    geom = json.loads(GEOM.read_text())
    glyphs = load_glyphs()
    only = {s.strip().lower() for s in args.only.split(",")} if args.only else None
    for name, spec in MODULES.items():
        if only and name.lower() not in only and spec["slug"].lower() not in only:
            continue
        data = geom[name]
        slug = spec["slug"]
        out_dir = PANELS / slug
        pcb = out_dir / f"{slug}.kicad_pcb"
        if pcb.exists() and not args.force:
            print(f"  {slug:18} SKIP (exists; pass --force)")
            continue
        warn = Warn(slug)
        text = build(name, data, spec, glyphs, warn)
        out_dir.mkdir(exist_ok=True)
        pcb.write_text(text)
        write_pro(out_dir / f"{slug}.kicad_pro", slug)
        (out_dir / "fabrication-toolkit-options.json").write_text(FAB_OPTIONS.read_text())
        print(f"  {slug:18} {data['hp']:2} HP  {data['width']:5} x {HEIGHT} mm  "
              f"<- {name}")
        for m in warn.msgs:
            print(f"      ! {m}")

if __name__ == "__main__":
    main()
