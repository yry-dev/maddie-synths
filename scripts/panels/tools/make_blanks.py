#!/usr/bin/env python3
"""Generate blank 3U eurorack faceplate KiCad projects with tiled silkscreen art.

Emits standalone KiCad 9 pure-graphics PCBs (no footprints/nets) for HP sizes
2,3,4,5,6,7,8,10,12 into panels/blank-<N>hp/. Each panel is a reversible blank:
a decorative seamless pattern (vendored pattern.monster tiles) is tiled onto BOTH
F.Silkscreen and B.Silkscreen so either face can point outward. Board outline and
M3-clearance mounting holes follow the researched Doepfer mechanical table in
.omc/autopilot/spec.md. The header/layers/setup/net structure and every S-expr
shape (gr_line, gr_circle, gr_poly) are copied from panels/mod2-comb style.

Everything is stdlib-only and deterministic: pattern-to-panel assignment is seeded,
uuids come from uuid5 on a fixed namespace, so re-runs reproduce byte-identical
files. Non-destructive by default (skip existing); pass --force to overwrite,
--only 4hp,12hp to restrict. Vendored tiles live in blank-patterns/.
"""
import sys, json, math, pathlib, random, uuid
import xml.dom.minidom as minidom

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parents[2]
PANELS = REPO / "panels"
PATTERNS_DIR = HERE / "blank-patterns"
PRO_TEMPLATE = PANELS / "mod2-clap" / "mod2-clap.kicad_pro"

# ---------------------------------------------------------------------------
# Mechanical table (from .omc/autopilot/spec.md -- FINAL, do not tweak).
# Height always 128.5mm; hole centers at y=3.0 and y=125.5 (122.5mm spacing).
# Holes 3.2mm round (M3 clearance). x measured from the panel's left edge.
# ---------------------------------------------------------------------------
HEIGHT = 128.5
HOLE_Y = (3.0, 125.5)
HOLE_DIA = 3.2                      # round M3-clearance hole
HOLE_R = HOLE_DIA / 2.0            # 1.6
BOARD_ORIGIN = (50.0, 30.0)        # board top-left on the A4 sheet

def _holes(width, xs):
    return [(x, y) for x in xs for y in HOLE_Y]

MECH = {
    1:  {"width": 5.00,  "holes": _holes(5.00,  [2.50])},
    2:  {"width": 9.80,  "holes": _holes(9.80,  [4.90])},
    3:  {"width": 14.94, "holes": _holes(14.94, [7.50])},
    4:  {"width": 20.00, "holes": _holes(20.00, [7.50, 12.58])},
    5:  {"width": 25.10, "holes": _holes(25.10, [7.50, 17.66])},
    6:  {"width": 30.00, "holes": _holes(30.00, [7.50, 22.74])},
    7:  {"width": 35.26, "holes": _holes(35.26, [7.50, 27.82])},
    8:  {"width": 40.30, "holes": _holes(40.30, [7.50, 32.90])},
    10: {"width": 50.50, "holes": _holes(50.50, [7.50, 43.06])},
    11: {"width": 55.58, "holes": _holes(55.58, [7.50, 48.14])},
    12: {"width": 60.60, "holes": _holes(60.60, [7.50, 53.22])},
}

MARGIN = 0.7          # silk kept this far inside the L/R board edge
# Screw bands: art occupies only the region BETWEEN the screws. The strips above
# BAND_TOP and below BAND_BOT stay blank (screw holes + rail area live there), so
# silk is clipped to a straight band edge, not bitten by per-hole keepouts.
BAND_TOP = 6.5
BAND_BOT = 122.0
KEEPOUT = HOLE_R + 1.0  # 2.6mm radius; holes sit in the blank bands so this is moot
MIN_STROKE = 0.15     # silkscreen legibility floor
TILE_TARGET_MM = 11.0 # longest tile side maps to ~11mm (bold, print-safe silk)
CHORD_TOL_MM = 0.05   # curve/arc flattening tolerance at final scale

# Per-pattern tile-target override (0-based index -> mm) for sparse fill motifs
# (few polys per tile) so even narrow panels get a dense, non-stub field.
SCALE_OVERRIDE = {6: 8.0, 7: 8.0}  # pattern-07 diamonds, pattern-08 japanese ribbon

# Per-panel forced pattern assignment (hp -> (front_idx, back_idx), 0-based tile
# index). Used where the seeded draw gives a too-plain field on a very narrow
# face. Applied on top of the deterministic draw so the pool still exhausts.
#   idx8=scales-1  idx9=squares-2(mosaic)  idx5=plus-1  idx16=leaves-2
PANEL_OVERRIDE = {
    1: (8, 9),    # 5mm face: fine seigaiha scales / dense square mosaic
    2: (9, 16),   # 9.8mm face: dense square mosaic / leaves
}
# Per-panel tile-target override (hp -> mm): shrink tiles on the tiniest faces so
# the motif repeats several times across the width instead of a lone column.
PANEL_SCALE = {1: 5.0, 2: 7.0}

UUID_NS = uuid.UUID("b1a2c3d4-0000-4000-8000-5eab1a2c3d40")
_uuid_counter = [0]
def next_uuid():
    _uuid_counter[0] += 1
    return str(uuid.uuid5(UUID_NS, str(_uuid_counter[0])))

# ---------------------------------------------------------------------------
# 2x3 affine matrices: (a,b,c,d,e,f) -> x'=a*x+c*y+e, y'=b*x+d*y+f
# ---------------------------------------------------------------------------
IDENT = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)

def mat_mul(m, n):
    a, b, c, d, e, f = m
    a2, b2, c2, d2, e2, f2 = n
    return (a*a2 + c*b2,      b*a2 + d*b2,
            a*c2 + c*d2,      b*c2 + d*d2,
            a*e2 + c*f2 + e,  b*e2 + d*f2 + f)

def mat_apply(m, x, y):
    a, b, c, d, e, f = m
    return (a*x + c*y + e, b*x + d*y + f)

def parse_transform(s):
    """Parse an SVG transform list into a single matrix (leftmost = outermost)."""
    m = IDENT
    if not s:
        return m
    import re
    for name, args in re.findall(r"(\w+)\s*\(([^)]*)\)", s):
        nums = [float(v) for v in re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", args)]
        if name == "translate":
            tx = nums[0] if nums else 0.0
            ty = nums[1] if len(nums) > 1 else 0.0
            t = (1, 0, 0, 1, tx, ty)
        elif name == "scale":
            sx = nums[0] if nums else 1.0
            sy = nums[1] if len(nums) > 1 else sx
            t = (sx, 0, 0, sy, 0, 0)
        elif name == "rotate":
            ang = math.radians(nums[0]) if nums else 0.0
            ca, sa = math.cos(ang), math.sin(ang)
            t = (ca, sa, -sa, ca, 0, 0)
            if len(nums) >= 3:
                cx, cy = nums[1], nums[2]
                t = mat_mul((1, 0, 0, 1, cx, cy), mat_mul(t, (1, 0, 0, 1, -cx, -cy)))
        elif name == "matrix" and len(nums) == 6:
            t = tuple(nums)
        else:
            continue
        m = mat_mul(m, t)
    return m

# ---------------------------------------------------------------------------
# Curve / arc flattening (tile-local units; tol derived from final scale later)
# ---------------------------------------------------------------------------
def _flat_cubic(p0, p1, p2, p3, tol, out, depth=0):
    x0, y0 = p0; x3, y3 = p3
    dx, dy = x3 - x0, y3 - y0
    d1 = abs((p1[0]-x3)*dy - (p1[1]-y3)*dx)
    d2 = abs((p2[0]-x3)*dy - (p2[1]-y3)*dx)
    if depth > 16 or (d1 + d2) ** 2 <= tol * tol * (dx*dx + dy*dy) + 1e-12:
        out.append(p3)
        return
    p01 = ((x0+p1[0])/2, (y0+p1[1])/2)
    p12 = ((p1[0]+p2[0])/2, (p1[1]+p2[1])/2)
    p23 = ((p2[0]+x3)/2, (p2[1]+y3)/2)
    a = ((p01[0]+p12[0])/2, (p01[1]+p12[1])/2)
    b = ((p12[0]+p23[0])/2, (p12[1]+p23[1])/2)
    mid = ((a[0]+b[0])/2, (a[1]+b[1])/2)
    _flat_cubic(p0, p01, a, mid, tol, out, depth+1)
    _flat_cubic(mid, b, p23, p3, tol, out, depth+1)

def flatten_cubic(p0, p1, p2, p3, tol):
    out = []
    _flat_cubic(p0, p1, p2, p3, tol, out)
    return out

def flatten_quad(p0, p1, p2, tol):
    c1 = (p0[0] + 2.0/3*(p1[0]-p0[0]), p0[1] + 2.0/3*(p1[1]-p0[1]))
    c2 = (p2[0] + 2.0/3*(p1[0]-p2[0]), p2[1] + 2.0/3*(p1[1]-p2[1]))
    return flatten_cubic(p0, c1, c2, p2, tol)

def flatten_arc(p0, rx, ry, phi, laf, sf, p1, tol):
    """SVG elliptical arc endpoint->center conversion, then sample to a polyline."""
    x1, y1 = p0; x2, y2 = p1
    if rx == 0 or ry == 0 or (x1 == x2 and y1 == y2):
        return [p1]
    rx, ry = abs(rx), abs(ry)
    cosp, sinp = math.cos(phi), math.sin(phi)
    dx, dy = (x1-x2)/2, (y1-y2)/2
    x1p = cosp*dx + sinp*dy
    y1p = -sinp*dx + cosp*dy
    lam = x1p*x1p/(rx*rx) + y1p*y1p/(ry*ry)
    if lam > 1:
        s = math.sqrt(lam); rx *= s; ry *= s
    num = rx*rx*ry*ry - rx*rx*y1p*y1p - ry*ry*x1p*x1p
    den = rx*rx*y1p*y1p + ry*ry*x1p*x1p
    co = math.sqrt(max(0.0, num/den)) if den else 0.0
    if laf == sf:
        co = -co
    cxp = co*rx*y1p/ry
    cyp = -co*ry*x1p/rx
    cx = cosp*cxp - sinp*cyp + (x1+x2)/2
    cy = sinp*cxp + cosp*cyp + (y1+y2)/2
    def ang(ux, uy, vx, vy):
        d = math.hypot(ux, uy)*math.hypot(vx, vy)
        c = max(-1.0, min(1.0, (ux*vx+uy*vy)/d)) if d else 1.0
        a = math.acos(c)
        return -a if (ux*vy-uy*vx) < 0 else a
    th1 = ang(1, 0, (x1p-cxp)/rx, (y1p-cyp)/ry)
    dth = ang((x1p-cxp)/rx, (y1p-cyp)/ry, (-x1p-cxp)/rx, (-y1p-cyp)/ry)
    if not sf and dth > 0:
        dth -= 2*math.pi
    elif sf and dth < 0:
        dth += 2*math.pi
    n = max(2, int(math.ceil(abs(dth)/(2*math.acos(max(0.0, 1-tol/max(rx, ry))) or 0.2))))
    n = min(n, 256)
    pts = []
    for i in range(1, n+1):
        t = th1 + dth*i/n
        ex = cosp*rx*math.cos(t) - sinp*ry*math.sin(t) + cx
        ey = sinp*rx*math.cos(t) + cosp*ry*math.sin(t) + cy
        pts.append((ex, ey))
    return pts

# ---------------------------------------------------------------------------
# SVG path 'd' parser -> list of subpaths [(closed_bool, [pts...]), ...]
# ---------------------------------------------------------------------------
def parse_path(d, tol):
    # Character scanner: correctly handles implicit repeated commands, numbers
    # packed against command letters, and SVG arc flags packed as single digits
    # (e.g. "a3 3 0 01-4 4" -> large-arc=0, sweep=1).
    n = len(d)
    pos = [0]
    def skip_sep():
        while pos[0] < n and d[pos[0]] in " \t\r\n,":
            pos[0] += 1
    def num():
        skip_sep()
        s = pos[0]; j = s
        if j < n and d[j] in "+-":
            j += 1
        seen_dot = False; seen_e = False
        while j < n:
            c = d[j]
            if c.isdigit():
                j += 1
            elif c == "." and not seen_dot and not seen_e:
                seen_dot = True; j += 1
            elif c in "eE" and not seen_e:
                seen_e = True; j += 1
                if j < n and d[j] in "+-":
                    j += 1
            else:
                break
        pos[0] = j
        return float(d[s:j])
    def flag():
        skip_sep()
        c = d[pos[0]]; pos[0] += 1
        return c == "1"
    def peek_cmd():
        skip_sep()
        return d[pos[0]] if pos[0] < n else None

    subs = []
    cur = None; start = None; pts = []; closed = False
    prev_cmd = None; prev_ctrl = None
    def flush(cl):
        if pts and len(pts) > 1:
            subs.append((cl, list(pts)))
    while True:
        skip_sep()
        if pos[0] >= n:
            break
        t = d[pos[0]]
        if t.isalpha():
            cmd = t; pos[0] += 1
        else:
            cmd = prev_cmd if prev_cmd not in (None, "Z", "z") else "L"
        rel = cmd.islower()
        C = cmd.upper()
        if C == "M":
            if pts:
                flush(closed)
            x = num(); y = num()
            if rel and cur:
                x += cur[0]; y += cur[1]
            cur = (x, y); start = cur; pts = [cur]; closed = False
            prev_cmd = "l" if rel else "L"
            prev_ctrl = None
        elif C == "L":
            x = num(); y = num()
            if rel: x += cur[0]; y += cur[1]
            cur = (x, y); pts.append(cur); prev_ctrl = None; prev_cmd = cmd
        elif C == "H":
            x = num()
            if rel: x += cur[0]
            cur = (x, cur[1]); pts.append(cur); prev_ctrl = None; prev_cmd = cmd
        elif C == "V":
            y = num()
            if rel: y += cur[1]
            cur = (cur[0], y); pts.append(cur); prev_ctrl = None; prev_cmd = cmd
        elif C == "C":
            x1 = num(); y1 = num(); x2 = num(); y2 = num(); x = num(); y = num()
            if rel:
                x1 += cur[0]; y1 += cur[1]; x2 += cur[0]; y2 += cur[1]; x += cur[0]; y += cur[1]
            pts.extend(flatten_cubic(cur, (x1, y1), (x2, y2), (x, y), tol))
            prev_ctrl = (x2, y2); cur = (x, y); prev_cmd = cmd
        elif C == "S":
            x2 = num(); y2 = num(); x = num(); y = num()
            if rel:
                x2 += cur[0]; y2 += cur[1]; x += cur[0]; y += cur[1]
            if prev_cmd and prev_cmd.upper() in ("C", "S") and prev_ctrl:
                x1 = 2*cur[0]-prev_ctrl[0]; y1 = 2*cur[1]-prev_ctrl[1]
            else:
                x1, y1 = cur
            pts.extend(flatten_cubic(cur, (x1, y1), (x2, y2), (x, y), tol))
            prev_ctrl = (x2, y2); cur = (x, y); prev_cmd = cmd
        elif C == "Q":
            x1 = num(); y1 = num(); x = num(); y = num()
            if rel:
                x1 += cur[0]; y1 += cur[1]; x += cur[0]; y += cur[1]
            pts.extend(flatten_quad(cur, (x1, y1), (x, y), tol))
            prev_ctrl = (x1, y1); cur = (x, y); prev_cmd = cmd
        elif C == "T":
            x = num(); y = num()
            if rel: x += cur[0]; y += cur[1]
            if prev_cmd and prev_cmd.upper() in ("Q", "T") and prev_ctrl:
                x1 = 2*cur[0]-prev_ctrl[0]; y1 = 2*cur[1]-prev_ctrl[1]
            else:
                x1, y1 = cur
            pts.extend(flatten_quad(cur, (x1, y1), (x, y), tol))
            prev_ctrl = (x1, y1); cur = (x, y); prev_cmd = cmd
        elif C == "A":
            rx = num(); ry = num(); phi = math.radians(num())
            laf = flag(); sf = flag(); x = num(); y = num()
            if rel: x += cur[0]; y += cur[1]
            pts.extend(flatten_arc(cur, rx, ry, phi, laf, sf, (x, y), tol))
            prev_ctrl = None; cur = (x, y); prev_cmd = cmd
        elif C == "Z":
            if start:
                pts.append(start); cur = start
            flush(True)
            pts = []; closed = False; prev_ctrl = None; prev_cmd = cmd
        else:
            break
    if pts:
        flush(closed)
    return subs

# ---------------------------------------------------------------------------
# Shape model: {"kind": "poly"|"line", "pts": [...], "closed": bool, "width": w}
#   poly  -> filled gr_poly
#   line  -> stroked gr_line segments
# ---------------------------------------------------------------------------
def _style(el, inherited):
    st = dict(inherited)
    for k in ("fill", "stroke", "stroke-width", "stroke-linecap"):
        if el.hasAttribute(k):
            st[k] = el.getAttribute(k)
    if el.hasAttribute("style"):
        for decl in el.getAttribute("style").split(";"):
            if ":" in decl:
                k, v = decl.split(":", 1)
                st[k.strip()] = v.strip()
    return st

def _num(el, name, default=0.0):
    v = el.getAttribute(name)
    if not v:
        return default
    v = v.strip()
    if v.endswith("%"):
        return float(v[:-1]) / 100.0  # caller resolves against tile dim
    try:
        return float(v)
    except ValueError:
        return default

def emit_shapes(subs, style, tw, th, tol):
    """Classify parsed subpaths into fill polygons and/or stroked polylines."""
    out = []
    fill = style.get("fill", "black")
    stroke = style.get("stroke", "none")
    sw = style.get("stroke-width", "1")
    try:
        sw = float(sw)
    except ValueError:
        sw = 1.0
    has_fill = fill not in ("none", "", None)
    has_stroke = stroke not in ("none", "", None)
    if not has_fill and not has_stroke:
        has_fill = True  # SVG default fill is black
    tile_area = tw * th
    for closed, pts in subs:
        if has_fill and closed and len(pts) >= 3:
            # actual filled area (shoelace), not bbox: keeps thin motifs that
            # merely span the tile, drops true solid backgrounds (area == tile).
            area = 0.0
            for k in range(len(pts)-1):
                area += pts[k][0]*pts[k+1][1] - pts[k+1][0]*pts[k][1]
            area = abs(area) / 2.0
            if tile_area and area >= 0.9 * tile_area:
                continue  # background-covering solid fill -> drop
            out.append({"kind": "poly", "pts": pts, "width": 0.0})
        if has_stroke:
            out.append({"kind": "line", "pts": pts, "width": sw})
        elif has_fill and not closed and len(pts) >= 2:
            # open filled subpath: render as thin stroke so it isn't lost
            out.append({"kind": "line", "pts": pts, "width": 0.0})
    return out

def walk(node, mat, style, tw, th, tol, acc):
    for child in node.childNodes:
        if child.nodeType != child.ELEMENT_NODE:
            continue
        tag = child.localName or child.tagName
        st = _style(child, style)
        m = mat_mul(mat, parse_transform(child.getAttribute("transform"))) if child.hasAttribute("transform") else mat
        shapes = None
        if tag == "g":
            walk(child, m, st, tw, th, tol, acc)
            continue
        elif tag == "path":
            subs = parse_path(child.getAttribute("d"), tol)
            shapes = emit_shapes(subs, st, tw, th, tol)
        elif tag == "rect":
            x = _num(child, "x"); y = _num(child, "y")
            w = child.getAttribute("width"); h = child.getAttribute("height")
            w = tw if w.strip().endswith("%") else _num(child, "width")
            h = th if h.strip().endswith("%") else _num(child, "height")
            pts = [(x, y), (x+w, y), (x+w, y+h), (x, y+h), (x, y)]
            shapes = emit_shapes([(True, pts)], st, tw, th, tol)
        elif tag in ("circle", "ellipse"):
            cx = _num(child, "cx"); cy = _num(child, "cy")
            if tag == "circle":
                rx = ry = _num(child, "r")
            else:
                rx = _num(child, "rx"); ry = _num(child, "ry")
            n = 48
            pts = [(cx+rx*math.cos(2*math.pi*k/n), cy+ry*math.sin(2*math.pi*k/n)) for k in range(n+1)]
            shapes = emit_shapes([(True, pts)], st, tw, th, tol)
        elif tag == "line":
            pts = [(_num(child, "x1"), _num(child, "y1")), (_num(child, "x2"), _num(child, "y2"))]
            shapes = emit_shapes([(False, pts)], st, tw, th, tol)
        elif tag in ("polyline", "polygon"):
            import re
            nums = [float(v) for v in re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", child.getAttribute("points"))]
            pts = list(zip(nums[0::2], nums[1::2]))
            closed = tag == "polygon"
            if closed and pts:
                pts = pts + [pts[0]]
            shapes = emit_shapes([(closed, pts)], st, tw, th, tol)
        else:
            continue
        for s in shapes:
            s["pts"] = [mat_apply(m, px, py) for px, py in s["pts"]]
            acc.append(s)

def load_tile(svg_path):
    """Return (tile_w, tile_h, shapes[]) from a vendored pattern SVG."""
    dom = minidom.parse(str(svg_path))
    pats = dom.getElementsByTagName("pattern")
    if pats:
        pat = pats[0]
        tw = _num(pat, "width", 0.0) or 100.0
        th = _num(pat, "height", 0.0) or 100.0
        root = pat
    else:
        root = dom.documentElement
        vb = root.getAttribute("viewBox").split()
        if len(vb) == 4:
            tw, th = float(vb[2]), float(vb[3])
        else:
            tw = _num(root, "width", 100.0); th = _num(root, "height", 100.0)
    tol = max(tw, th) * (CHORD_TOL_MM / TILE_TARGET_MM)
    acc = []
    walk(root, IDENT, {"fill": "black", "stroke": "none"}, tw, th, tol, acc)
    return tw, th, acc

# ---------------------------------------------------------------------------
# Clipping: Liang-Barsky (lines) + Sutherland-Hodgman (polys) to margin rect,
# plus hole-keepout rejection.
# ---------------------------------------------------------------------------
def clip_segment(p0, p1, rect):
    x0, y0 = p0; x1, y1 = p1
    xmin, ymin, xmax, ymax = rect
    dx, dy = x1-x0, y1-y0
    t0, t1 = 0.0, 1.0
    for p, q in ((-dx, x0-xmin), (dx, xmax-x0), (-dy, y0-ymin), (dy, ymax-y0)):
        if p == 0:
            if q < 0:
                return None
        else:
            r = q/p
            if p < 0:
                if r > t1: return None
                if r > t0: t0 = r
            else:
                if r < t0: return None
                if r < t1: t1 = r
    return ((x0+t0*dx, y0+t0*dy), (x0+t1*dx, y0+t1*dy))

def clip_polygon(pts, rect):
    xmin, ymin, xmax, ymax = rect
    def clip_edge(poly, inside, inter):
        if not poly:
            return []
        out = []
        for i in range(len(poly)):
            cur = poly[i]; prv = poly[i-1]
            ci, pi = inside(cur), inside(prv)
            if ci:
                if not pi:
                    out.append(inter(prv, cur))
                out.append(cur)
            elif pi:
                out.append(inter(prv, cur))
        return out
    poly = pts[:-1] if len(pts) > 1 and pts[0] == pts[-1] else pts[:]
    edges = [
        (lambda p: p[0] >= xmin, lambda a, b: _ix(a, b, 0, xmin)),
        (lambda p: p[0] <= xmax, lambda a, b: _ix(a, b, 0, xmax)),
        (lambda p: p[1] >= ymin, lambda a, b: _ix(a, b, 1, ymin)),
        (lambda p: p[1] <= ymax, lambda a, b: _ix(a, b, 1, ymax)),
    ]
    for inside, inter in edges:
        poly = clip_edge(poly, inside, inter)
        if not poly:
            return None
    return poly

def _ix(a, b, axis, val):
    if axis == 0:
        t = (val - a[0]) / (b[0] - a[0]) if b[0] != a[0] else 0.0
        return (val, a[1] + t*(b[1]-a[1]))
    t = (val - a[1]) / (b[1] - a[1]) if b[1] != a[1] else 0.0
    return (a[0] + t*(b[0]-a[0]), val)

def seg_point_dist(p, a, b):
    ax, ay = a; bx, by = b; px, py = p
    dx, dy = bx-ax, by-ay
    L2 = dx*dx + dy*dy
    if L2 == 0:
        return math.hypot(px-ax, py-ay)
    t = max(0.0, min(1.0, ((px-ax)*dx + (py-ay)*dy)/L2))
    return math.hypot(px-(ax+t*dx), py-(ay+t*dy))

def point_in_poly(p, poly):
    x, y = p; inside = False
    n = len(poly); j = n-1
    for i in range(n):
        xi, yi = poly[i]; xj, yj = poly[j]
        if ((yi > y) != (yj > y)) and (x < (xj-xi)*(y-yi)/((yj-yi) or 1e-12) + xi):
            inside = not inside
        j = i
    return inside

def seg_hits_hole(a, b, holes):
    for hx, hy in holes:
        if seg_point_dist((hx, hy), a, b) < KEEPOUT:
            return True
    return False

def poly_hits_hole(poly, holes):
    for hx, hy in holes:
        if point_in_poly((hx, hy), poly):
            return True
        for i in range(len(poly)):
            if seg_point_dist((hx, hy), poly[i-1], poly[i]) < KEEPOUT:
                return True
    return False

# ---------------------------------------------------------------------------
# KiCad S-expr emitters (board-relative pts; origin added here)
# ---------------------------------------------------------------------------
OX, OY = BOARD_ORIGIN

def fmt(v):
    if v == 0:
        v = 0.0  # normalise -0.0
    return f"{v:.4f}".rstrip("0").rstrip(".")

def em_line(x0, y0, x1, y1, width, layer):
    return (f"\t(gr_line\n\t\t(start {fmt(OX+x0)} {fmt(OY+y0)})\n\t\t(end {fmt(OX+x1)} {fmt(OY+y1)})\n"
            f"\t\t(stroke\n\t\t\t(width {fmt(width)})\n\t\t\t(type solid)\n\t\t)\n"
            f"\t\t(layer \"{layer}\")\n\t\t(uuid \"{next_uuid()}\")\n\t)\n")

def em_circle(cx, cy, r, width, layer):
    return (f"\t(gr_circle\n\t\t(center {fmt(OX+cx)} {fmt(OY+cy)})\n\t\t(end {fmt(OX+cx+r)} {fmt(OY+cy)})\n"
            f"\t\t(stroke\n\t\t\t(width {fmt(width)})\n\t\t\t(type solid)\n\t\t)\n"
            f"\t\t(fill no)\n\t\t(layer \"{layer}\")\n\t\t(uuid \"{next_uuid()}\")\n\t)\n")

def em_poly(pts, layer):
    rows = []
    for k in range(0, len(pts), 4):
        chunk = pts[k:k+4]
        rows.append("\t\t\t" + " ".join(f"(xy {fmt(OX+x)} {fmt(OY+y)})" for x, y in chunk))
    body = "\n".join(rows)
    return (f"\t(gr_poly\n\t\t(pts\n{body}\n\t\t)\n"
            f"\t\t(stroke\n\t\t\t(width 0)\n\t\t\t(type solid)\n\t\t)\n"
            f"\t\t(fill yes)\n\t\t(layer \"{layer}\")\n\t\t(uuid \"{next_uuid()}\")\n\t)\n")

def em_text(x, y, angle, text, layer, mirror, size, thick):
    # Comfortaa branding, centered on (x,y). No render_cache -> KiCad regenerates
    # glyphs from the installed font. Back face gets (justify mirror) so it reads
    # correctly viewed through the board.
    just = "\t\t\t(justify mirror)\n" if mirror else ""
    return (f"\t(gr_text \"{text}\"\n"
            f"\t\t(at {fmt(OX+x)} {fmt(OY+y)} {fmt(angle)})\n"
            f"\t\t(layer \"{layer}\")\n\t\t(uuid \"{next_uuid()}\")\n"
            f"\t\t(effects\n\t\t\t(font\n\t\t\t\t(face \"Comfortaa\")\n"
            f"\t\t\t\t(size {fmt(size)} {fmt(size)})\n\t\t\t\t(thickness {fmt(thick)})\n\t\t\t)\n"
            f"{just}\t\t)\n\t)\n")

# "maddie synths" branding in the screw bands, width-tiered so it clears the
# 3.2mm holes (edges reach y=4.6 top / 123.9 bottom) and never touches the
# pattern region [6.5, 122.0]. Top band upright; bottom band 180deg + mirrored
# in y so it reads right-side-up when the module is mounted upside down.
def branding(hp, width, layer, mirror):
    cx = width / 2.0
    out = []
    def pair(lines_top):  # lines_top: [(text, y, size, thick), ...] for the top band
        for text, y, size, thick in lines_top:
            out.append(em_text(cx, y, 0, text, layer, mirror, size, thick))
            out.append(em_text(cx, HEIGHT - y, 180, text, layer, mirror, size, thick))
    if hp >= 8:                       # single line, at the screw line between holes
        pair([("maddie synths", 3.0, 1.8, 0.18)])
    elif hp >= 4:                     # single line, below the holes
        pair([("maddie synths", 5.55, 1.25, 0.15)])
    else:                             # stacked two lines below the centered hole
        pair([("maddie", 5.05, 0.8, 0.13), ("synths", 5.95, 0.8, 0.13)])
    return out

# ---------------------------------------------------------------------------
# Tiler: lay a tile across the face, clip, emit silk onto `layer`.
# ---------------------------------------------------------------------------
def tile_silk(tw, th, shapes, width, height, holes, layer, target_mm):
    # Clip to the between-screws band: full width (minus L/R margin), y in the
    # [BAND_TOP, BAND_BOT] strip -> straight top/bottom band edges.
    rect = (MARGIN, BAND_TOP, width-MARGIN, BAND_BOT)
    # Uniform scale keyed to the tile's LONGEST side so tall tiles still repeat
    # densely down the 128.5mm panel (keeps the pattern bold and well-covered).
    s = target_mm / max(tw, th)
    cell_w = tw * s
    cell_h = th * s
    nx = int(math.ceil(width / cell_w)) + 2
    ny = int(math.ceil(height / cell_h)) + 2
    out = []
    for j in range(-1, ny+1):
        for i in range(-1, nx+1):
            ox, oy = i * cell_w, j * cell_h
            for sh in shapes:
                mpts = [(ox + px*s, oy + py*s) for px, py in sh["pts"]]
                if sh["kind"] == "poly":
                    cp = clip_polygon(mpts, rect)
                    if not cp or len(cp) < 3:
                        continue
                    if poly_hits_hole(cp, holes):
                        continue
                    out.append(em_poly(cp + [cp[0]], layer))
                else:
                    w = max(MIN_STROKE, sh["width"] * s)
                    for k in range(len(mpts)-1):
                        seg = clip_segment(mpts[k], mpts[k+1], rect)
                        if not seg:
                            continue
                        if seg_hits_hole(seg[0], seg[1], holes):
                            continue
                        if math.hypot(seg[1][0]-seg[0][0], seg[1][1]-seg[0][1]) < 1e-4:
                            continue
                        out.append(em_line(seg[0][0], seg[0][1], seg[1][0], seg[1][1], w, layer))
    return out

# ---------------------------------------------------------------------------
# Board file assembly
# ---------------------------------------------------------------------------
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

def build_board(hp, front_shapes, back_shapes):
    spec = MECH[hp]
    W = spec["width"]; holes = spec["holes"]
    parts = [HEADER]
    # outline (Edge.Cuts, 0.05)
    parts.append(em_line(0, 0, W, 0, 0.05, "Edge.Cuts"))
    parts.append(em_line(W, 0, W, HEIGHT, 0.05, "Edge.Cuts"))
    parts.append(em_line(W, HEIGHT, 0, HEIGHT, 0.05, "Edge.Cuts"))
    parts.append(em_line(0, HEIGHT, 0, 0, 0.05, "Edge.Cuts"))
    # mounting holes
    for hx, hy in holes:
        parts.append(em_circle(hx, hy, HOLE_R, 0.2, "Edge.Cuts"))
    # silk
    ftw, fth, fsh, ftarget = front_shapes
    btw, bth, bsh, btarget = back_shapes
    parts += tile_silk(ftw, fth, fsh, W, HEIGHT, holes, "F.SilkS", ftarget)
    parts += tile_silk(btw, bth, bsh, W, HEIGHT, holes, "B.SilkS", btarget)
    # branding text in the screw bands (front upright, back mirrored)
    parts += branding(hp, W, "F.SilkS", mirror=False)
    parts += branding(hp, W, "B.SilkS", mirror=True)
    parts.append(")\n")
    return "".join(parts)

def write_pro(dst_pro, name):
    # Clone the template project verbatim, patch only meta.filename (per plan).
    d = json.load(open(PRO_TEMPLATE))
    d.setdefault("meta", {})["filename"] = f"{name}.kicad_pro"
    json.dump(d, open(dst_pro, "w"), indent=2)

# ---------------------------------------------------------------------------
# Pattern assignment (seeded, distinct front/back, no reuse until pool dry)
# ---------------------------------------------------------------------------
def assign_patterns(pool, hps, seed=20260709):
    rng = random.Random(seed)
    deck = []
    def refill():
        d = pool[:]
        rng.shuffle(d)
        return d
    deck = refill()
    assignment = {}
    for hp in hps:
        picks = []
        guard = 0
        while len(picks) < 2 and guard < 1000:
            guard += 1
            if not deck:
                deck = refill()
            cand = deck.pop()
            if cand in picks:
                deck.insert(0, cand)          # distinct front/back; retry later
                if len(pool) < 2:
                    break
                continue
            picks.append(cand)
        if len(picks) < 2:
            picks.append(picks[0])            # degenerate pool of size 1
        assignment[hp] = (picks[0], picks[1])
    return assignment

# ---------------------------------------------------------------------------
def main():
    args = sys.argv[1:]
    force = "--force" in args or "-f" in args
    only = None
    if "--only" in args:
        val = args[args.index("--only")+1]
        only = {int(x.replace("hp", "")) for x in val.split(",")}

    tiles = sorted(PATTERNS_DIR.glob("pattern-*.svg"))
    if not tiles:
        sys.exit(f"no vendored patterns in {PATTERNS_DIR}")
    pool = list(range(len(tiles)))
    loaded = {}
    for idx in pool:
        tw, th, sh = load_tile(tiles[idx])
        target = SCALE_OVERRIDE.get(idx, TILE_TARGET_MM)
        loaded[idx] = (tw, th, sh, target)

    hps = [hp for hp in sorted(MECH) if only is None or hp in only]
    assignment = assign_patterns(pool, hps)
    # Forced per-panel picks override the deterministic draw (draw still runs for
    # every panel, so the pool-exhaustion order is unchanged for the rest).
    for hp, pick in PANEL_OVERRIDE.items():
        if hp in assignment:
            assignment[hp] = pick

    assign_rows = []
    for hp in hps:
        f_idx, b_idx = assignment[hp]
        name = f"blank-{hp}hp"
        out_dir = PANELS / name
        pcb = out_dir / f"{name}.kicad_pcb"
        pro = out_dir / f"{name}.kicad_pro"
        assign_rows.append((hp, tiles[f_idx].name, tiles[b_idx].name))
        if not force and (pcb.exists() or pro.exists()):
            print(f"  {name:12} SKIP (exists; --force to overwrite)")
            continue
        _uuid_counter[0] = hp * 100000  # deterministic per-board uuid namespace slice
        fload, bload = loaded[f_idx], loaded[b_idx]
        ptgt = PANEL_SCALE.get(hp)      # tiny faces: denser tiles across the width
        if ptgt:
            fload = (fload[0], fload[1], fload[2], ptgt)
            bload = (bload[0], bload[1], bload[2], ptgt)
        text = build_board(hp, fload, bload)
        out_dir.mkdir(parents=True, exist_ok=True)
        pcb.write_text(text)
        write_pro(pro, name)
        fs = text.count('(layer "F.SilkS")'); bs = text.count('(layer "B.SilkS")')
        print(f"  {name:12} <- F:{tiles[f_idx].name} B:{tiles[b_idx].name}  (F.SilkS={fs} B.SilkS={bs})")

    # record assignment into blank-patterns/index.md (assignment table section)
    write_assignment_table(tiles, assign_rows)
    print(f"\nDone. {len(hps)} panel(s) targeted; {len(tiles)} patterns in pool.")

def write_assignment_table(tiles, rows):
    idx_md = PATTERNS_DIR / "index.md"
    marker = "<!-- ASSIGNMENT (auto-generated by make_blanks.py) -->"
    table = [marker, "", "## Per-panel pattern assignment", "",
             "| panel | front (F.SilkS) | back (B.SilkS) |", "|---|---|---|"]
    for hp, f, b in rows:
        table.append(f"| blank-{hp}hp | {f} | {b} |")
    block = "\n".join(table) + "\n"
    if idx_md.exists():
        txt = idx_md.read_text()
        if marker in txt:
            txt = txt[:txt.index(marker)].rstrip() + "\n\n" + block
        else:
            txt = txt.rstrip() + "\n\n" + block
        idx_md.write_text(txt)

if __name__ == "__main__":
    main()
