#!/usr/bin/env python3
"""Build the m-power PCB as a 6HP Eurorack module: place, route, pour.

Run with KiCad's bundled python (has pcbnew):
  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/\
Versions/Current/bin/python3 pcb_gen.py

Consumes board_data.json (from gen.py) so the schematic stays the single
source of truth. Three stages:

  1. PLACE   - footprints are skyline-packed into functional zones running
               down a 6HP (30.48mm) strip: input -> rectify/filter -> +5V reg
               -> indicators -> outputs. Zone ORDER carries the signal-flow
               story; inside a zone the packer is free to tuck short parts
               beside tall ones, which is what buys the height to fit 3U.
  2. ROUTE   - a two-layer grid maze router (Dijkstra, 45-degree steps, turn
               and via penalties). Track widths start at the netclass target
               and fall back until a path fits; the achieved width is
               reported per net. 1.5mm on 1oz outer copper carries ~4A and
               1.0mm ~3A, so the fallbacks stay comfortably above the 3A PTC
               hold current. Anything that fails is reported loudly rather
               than silently dropped.
  3. POUR    - GND zones on both layers. GND is the biggest net (22 pads) and
               a plane is the right answer for a PSU, so it is never routed
               as tracks. THT GND pads stitch the two layers together, and
               the filler clears around whatever B.Cu routing was needed.

Two pcbnew landmines this script works around, both of which segfault:
  - ZONE_FILLER().Fill() requires board.BuildConnectivity() to have run.
  - board.Remove() on a track leaves the connectivity graph holding a
    dangling pointer. So routing is computed entirely in Python and only
    materialised as PCB_TRACK/PCB_VIA objects once a width has succeeded;
    a failed attempt is simply discarded and never reaches the board.
"""
import json, math, os, heapq, pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = "eurorack-psu-twobrick"
STD = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints"
MADDIE = os.path.normpath(os.path.join(HERE, "..", "lib", "maddie.pretty"))

HP = 6
# The PCB must be NARROWER than the panel, never wider. A 6HP panel is 30.00mm
# (Doepfer's table, see scripts/panels/tools/make_blanks.py MECH), so a board
# at the raw 6 x 5.08 = 30.48mm pitch would stand 0.24mm proud on each side
# BEHIND a narrower panel and foul the neighbouring module. The panel may
# overhang the board; the board may never overhang the panel.
WIDTH = 29.8                 # 0.1mm of panel overhang per side
#
# SIDE and WIDTH are jointly constrained, and both edges of the window are
# measured, not guessed:
#   - SIDE >= 2.4. It is the clear channel down each long edge that the rails
#     run in. At 2.3 and below VIN_B stops routing outright.
#   - usable (= WIDTH - 2*SIDE) >= 24.90, which is what the two Mini-Fit
#     outputs need side by side (12.05 + GAP + 12.05). Below that they stop
#     sharing a shelf and the board jumps 108.7mm -> 131.3mm.
# Together those force WIDTH >= 24.90 + 4.8 = 29.70, so 29.8 with SIDE 2.4
# (usable 25.0) is the narrowest clean board. 29.6 and below wrap the outputs.
SIDE = 2.4                   # part keep-out from each long edge
ENDZONE = 2.5                # top/bottom margin (panel-mounted: no PCB holes)
GAP = 0.8                    # gap between packed parts
ZONE_GAP = 1.0               # extra breathing room between functional zones

EDGE_KEEPOUT = 0.55          # copper-to-board-edge (KiCad's default rule is 0.5)
CLEAR = 0.25                 # routing clearance (design rules say min 0.2)
G = 0.2                      # router grid pitch, mm
VIA_DIA = 1.0                # matches the Power netclass in the .kicad_pro
VIA_DRILL = 0.5

# Functional zones, top -> bottom. Signal flow: bricks in at the top, rails
# out at the bottom. Order inside a zone is preserved by the packer.
# Everything the user sees or plugs into lives in `panel` at the TOP: both
# barrel jacks, the USB, and the three rail LEDs. The Mini-Fit rail outputs
# sit alone at the BOTTOM, the opposite end, since they face the case wiring.
#
# SW1 is a PCB-mount toggle: its bushing passes through the panel and its nut
# is the panel's fourth mechanical anchor, so its position here IS a panel hole
# position - the panel generator reads it straight out of the .kicad_pcb.
# It is deliberately NOT in the panel zone: that zone is already the most
# congested part of the board, and the switch is the one panel part with no
# reason to sit next to the jacks. Keeping it in `chain` puts it directly in
# the current path between the PTCs and the Schottkys, which is what lets
# N_FA/N_FB/N_SA/N_SB route at full width.
#
# The three LEDs share the panel zone and are all the same height, so the
# packer lands them on one shelf - an aligned indicator row on the panel. Their
# series resistors sit in the very next zone, close enough that N_L1/N_L2/N_L3
# stay short. Gluing each LED to its own resistor instead (see group_size)
# also routes, but the pairs wrap and the LEDs stop lining up.
#
# Zone ORDER was picked by sweeping orderings and checking BOTH trace width
# and routing failures. If you re-order, re-run and check for
# "FAILED TO ROUTE" - a low via count often just means nets silently failed.
# F1+SW1+F2 are GLUED into one cluster, and that is the single change that
# made 6HP route. SW1 is a hub: four power nets (N_FA/N_FB in from the PTCs,
# N_SA/N_SB out to the Schottkys) all land on one 4.70 x 4.83mm pin grid.
# Height-sorted packing puts its neighbours in three different height classes
# - SW1 13.2mm, the diodes 5.8mm, the PTCs 3.5mm - so it scattered them down
# the zone and stranded those nets whatever else was tried (rotating SW1,
# gluing it to either diode, moving it between zones: every one left 1-3 nets
# unrouted, and gluing it to a diode also cost 3.1mm of board).
#
# Rotating the PTC discs 90 is what makes the cluster legal. Laid flat an
# MF-R300 is 12.6 x 3.6mm, so F1+SW1+F2 side by side is 37.9mm - far past the
# 25.1mm strip, and group_size() rejects it. On edge each PTC is 3.6 x 12.6mm,
# the cluster is 20.7mm, and the PTCs land in the SAME height class as the
# switch, so the packer stops trying to separate them. The discs are radial
# 2-lead parts, so their rotation is free.
# Result: 0 unrouted, every power net at >= 1.5mm, 108.7mm.
ZONES = [
    # D3/D4/D5 glued so they share one shelf: they are panel-facing indicators
    # on spacers, and a straight row is what the panel drilling wants. Left
    # loose, D3 packs alongside the USB and the other two drop to the next row.
    ("panel",    ["J1", "J2", "J3", ["D3", "D4", "D5"]]),
    ("ledres",   ["R1", "R2", "R3"]),
    ("chain",    [["F1", "SW1", "F2"], "D1", "D2",
                  "C1", "C2", "C3", "C4"]),
    ("reg5v",    ["VR1", "C5", "C6", "C7"]),
    ("output",   ["J5", "J6"]),
]
ORDER = [r for _, refs in ZONES for g in refs
         for r in ([g] if isinstance(g, str) else g)]

# Rotate the wide connectors narrow so they fit the 30.48mm strip. F1/F2 are
# on edge so the F1+SW1+F2 cluster fits the strip at all - see the ZONES note.
ROT = {"J5": 90, "J6": 90, "F1": 90, "F2": 90}
# Footprint pad names that differ from the schematic pin numbers.
PAD_ALIAS = {"J3": {"SH": "5"}}

# Width fallbacks, widest first. The FLOOR of each list is a current rating,
# not a preference - the router must fail loudly rather than quietly undersize
# a power net.
#
# VIN_*/N_F*/N_S* are the main current path (jack -> PTC -> switch -> Schottky
# -> rail) and carry exactly what the rail carries, up to the 3A PTC hold
# current. On 1oz outer copper 1.5mm is ~4A and 1.0mm is ~2.4A at a 10C rise,
# so 1.5mm is the floor for all of them and for +/-12V. Do not add 1.0 or 0.8
# to those lists to "make it route" - that is how you get a 3A rail on a 2.4A
# trace.
#
# +5V is capped at 1.5A by the OKI-78SR-5 regardless of what the rail asks
# for, so 1.0mm (~2.4A) is still a comfortable 1.6x margin there.
# The LED nets carry ~4mA and the USB D+/D- pair carries no power at all.
NET_WIDTHS = {
    "+12V": [2.0, 1.5], "-12V": [2.0, 1.5],
    "+5V":  [2.0, 1.5, 1.0],
    "VIN_A": [2.0, 1.5], "VIN_B": [2.0, 1.5],
    "N_FA": [2.0, 1.5], "N_FB": [2.0, 1.5],
    "N_SA": [2.0, 1.5], "N_SB": [2.0, 1.5],
    "N_L1": [1.0, 0.6, 0.4, 0.3], "N_L2": [1.0, 0.6, 0.4, 0.3],
    "N_L3": [1.0, 0.6, 0.4, 0.3],
    "USB_DP": [0.6, 0.4, 0.3],
}
# Widest / most critical rails first so they get the free space.
ROUTE_ORDER = ["+12V", "-12V", "+5V", "VIN_A", "VIN_B", "N_FA", "N_FB",
               "N_SA", "N_SB", "N_L1", "N_L2", "N_L3", "USB_DP"]
PLANE_NET = "GND"            # poured, never routed


def libpath(lib):
    return MADDIE if lib == "maddie" else "%s/%s.pretty" % (STD, lib)


data = json.load(open(os.path.join(HERE, "board_data.json")))
byref = {c["ref"]: c for c in data["components"]}
board = pcbnew.CreateEmptyBoard()

nets = {}
def get_net(name):
    if name not in nets:
        n = pcbnew.NETINFO_ITEM(board, name)
        board.Add(n)
        nets[name] = n
    return nets[name]


def bbox(fp):
    try:
        bb = fp.GetBoundingBox(False, False)
    except TypeError:
        bb = fp.GetBoundingBox()
    T = pcbnew.ToMM
    return T(bb.GetWidth()), T(bb.GetHeight()), -T(bb.GetLeft()), -T(bb.GetTop())


# ------------------------------------------------------------------ 1. PLACE
fps, missing = {}, []
for ref in ORDER:
    c = byref.get(ref)
    if not c:
        missing.append(ref)
        continue
    lib, fpname = c["fp"].split(":", 1)
    fp = pcbnew.FootprintLoad(libpath(lib), fpname)
    if fp is None:
        missing.append(ref)
        continue
    fp.SetReference(ref)
    fp.SetValue(c["value"])
    # 6HP is cramped: default 1mm reference text collides with everything.
    # Shrink the refs and push values to F.Fab so the silkscreen stays legible.
    # 0.8mm is the floor - KiCad's default min-text-height rule rejects 0.6.
    rt = fp.Reference()
    rt.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(0.8), pcbnew.FromMM(0.8)))
    rt.SetTextThickness(pcbnew.FromMM(0.12))
    vt = fp.Value()
    vt.SetLayer(pcbnew.F_Fab)
    vt.SetVisible(False)
    fp.SetOrientationDegrees(ROT.get(ref, 0))
    fp.SetPosition(pcbnew.VECTOR2I(0, 0))
    alias = PAD_ALIAS.get(ref, {})
    for pad in fp.Pads():
        num = pad.GetNumber()
        net = c["nets"].get(alias.get(num, num))
        if net:
            pad.SetNet(get_net(net))
    fps[ref] = fp

def group_size(grp):
    """Footprint or glued group -> (total width incl. inner gaps, max height)."""
    refs = [grp] if isinstance(grp, str) else grp
    dims = [bbox(fps[r]) for r in refs]
    return (sum(d[0] for d in dims) + GAP * (len(refs) - 1),
            max(d[1] for d in dims))


STEP = 0.1                   # skyline profile resolution, mm


class Skyline:
    """Bottom-left skyline packer over the usable strip.

    Shelf packing charged every part on a row the height of that row's tallest
    member. In the chain zone that meant the 3.5mm fuses and the ceramics each
    paid for the 13.2mm switch, wasting 14.2mm over the zone - more than the
    ~3mm the board needed to lose to fit 3U. This keeps a per-column height
    profile instead, so a short part tucks in beside a tall one.

    Every placement stamps a GAP halo into the profile, so "fits" already means
    "fits with assembly clearance" and callers never add GAP themselves.
    """

    def __init__(self, width, y0):
        self.n = int(round(width / STEP))
        self.prof = [y0] * self.n
        self.maxy = y0

    @staticmethod
    def _cells(mm):
        return int(math.ceil(mm / STEP - 1e-9))

    def place(self, w, h):
        """Lowest, then leftmost, spot for a w x h box.

        Returns (x, y) as mm offsets from the strip origin. Only ever called
        with w <= strip width - group_size()'s guard rejects anything wider
        before we get here, so there is always a candidate.
        """
        need = self._cells(w)
        best_y, best_x = None, 0
        for i in range(0, self.n - need + 1):
            y = max(self.prof[i:i + need])
            if best_y is None or y < best_y - 1e-9:
                best_y, best_x = y, i
        halo = self._cells(GAP)
        for j in range(max(0, best_x - halo), min(self.n, best_x + need + halo)):
            self.prof[j] = max(self.prof[j], best_y + h + GAP)
        self.maxy = max(self.maxy, best_y + h)
        return best_x * STEP, best_y


usable = WIDTH - 2 * SIDE
cy = ENDZONE
zone_bounds = []
for zname, refs in ZONES:
    # A zone entry may be a ref or a list of refs to keep side by side. Gluing
    # each LED to its own series resistor turns N_L1/N_L2/N_L3 into 2-pad nets
    # a few mm long; left to pack independently they end up at opposite ends
    # of a zone and the router cannot connect them at all.
    present = []
    for g in refs:
        sub = [g] if isinstance(g, str) else list(g)
        sub = [r for r in sub if r in fps]
        if sub:
            present.append(sub[0] if isinstance(g, str) else sub)
    if not present:
        continue
    top = cy
    sky = Skyline(usable, cy)
    # Tallest-first. Skyline packing is only as good as its insertion order:
    # placing a tall part late leaves it nowhere to sit but on top of
    # everything, so the tall parts go down first and the short ones fill in
    # around them. Python's sort is stable, so parts of equal height keep their
    # authored order and routing locality survives where it costs nothing.
    present.sort(key=lambda g: -group_size(g)[1])
    for grp in present:
        gw, gh = group_size(grp)
        # A glued group never wraps internally, so one wider than the strip
        # would hang off the board edge - and the only symptom is a couple of
        # copper_edge_clearance errors that look like a routing problem.
        if gw > usable:
            raise SystemExit(
                "glued group %s is %.2fmm wide but only %.2fmm is usable at "
                "%dHP with SIDE=%.1f - split the group or widen the board"
                % (grp, gw, usable, HP, SIDE))
        gx, gy = sky.place(gw, gh)
        cx = SIDE + gx
        for ref in ([grp] if isinstance(grp, str) else grp):
            fp = fps[ref]
            w, h, offx, offy = bbox(fp)
            fp.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(cx + offx),
                                           pcbnew.FromMM(gy + offy)))
            board.Add(fp)
            cx += w + GAP
    cy = sky.maxy
    zone_bounds.append((zname, top, cy))
    cy += ZONE_GAP

HEIGHT = round(cy - ZONE_GAP + ENDZONE, 2)

def edge(x1, y1, x2, y2):
    s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(x1), pcbnew.FromMM(y1)))
    s.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(x2), pcbnew.FromMM(y2)))
    s.SetLayer(pcbnew.Edge_Cuts)
    s.SetWidth(pcbnew.FromMM(0.15))
    board.Add(s)

edge(0, 0, WIDTH, 0)
edge(WIDTH, 0, WIDTH, HEIGHT)
edge(WIDTH, HEIGHT, 0, HEIGHT)
edge(0, HEIGHT, 0, 0)

# No PCB mounting holes: the panel holds the board via the soldered switch,
# USB jack, and the two threaded barrel-jack bushings.

# ------------------------------------------------------------------ 2. ROUTE
# Cells are indexed (layer, iy, ix) with layer 0 = F.Cu, 1 = B.Cu.
NX = int(WIDTH / G) + 1
NY = int(HEIGHT / G) + 1
PLANE = NX * NY
LAYER_OF = {0: pcbnew.F_Cu, 1: pcbnew.B_Cu}
T = pcbnew.ToMM

pad_rects = []       # (x0, y0, x1, y1, netname) - THT, so blocks both layers
pad_cells = {}       # netname -> {ref.pad: set(cells on both layers)}
pad_centre = {}      # netname -> {ref.pad: (x, y)} - real centre, for snapping
for ref, fp in fps.items():
    for pad in fp.Pads():
        bb = pad.GetBoundingBox()
        x0, y0 = T(bb.GetLeft()), T(bb.GetTop())
        x1, y1 = T(bb.GetRight()), T(bb.GetBottom())
        nn = pad.GetNetname()
        pos = pad.GetPosition()
        if nn:
            pad_centre.setdefault(nn, {})["%s.%s" % (ref, pad.GetNumber())] = \
                (T(pos.x), T(pos.y))
        pad_rects.append((x0, y0, x1, y1, nn))
        cs = set()
        for iy in range(max(int(y0 / G), 0), min(int(y1 / G) + 2, NY)):
            for ix in range(max(int(x0 / G), 0), min(int(x1 / G) + 2, NX)):
                cs.add(iy * NX + ix)
                cs.add(PLANE + iy * NX + ix)
        if nn and cs:
            pad_cells.setdefault(nn, {})["%s.%s" % (ref, pad.GetNumber())] = cs

track_caps = []      # (x1, y1, x2, y2, width, layer, netname)
via_pts = []         # (x, y, netname)


def build_blocked(netname, w):
    """Cells whose centre cannot host a track centreline of width w."""
    blocked = bytearray(2 * PLANE)
    r = w / 2.0 + CLEAR

    def stamp(x0, y0, x1, y1, layers):
        ix0 = max(int((x0 - r) / G), 0)
        ix1 = min(int((x1 + r) / G) + 1, NX - 1)
        iy0 = max(int((y0 - r) / G), 0)
        iy1 = min(int((y1 + r) / G) + 1, NY - 1)
        for L in layers:
            off = L * PLANE
            for iy in range(iy0, iy1 + 1):
                base = off + iy * NX
                for ix in range(ix0, ix1 + 1):
                    blocked[base + ix] = 1

    for (x0, y0, x1, y1, nn) in pad_rects:
        if nn != netname:
            stamp(x0, y0, x1, y1, (0, 1))
    for (ax, ay, bx, by, tw, L, nn) in track_caps:
        if nn == netname:
            continue
        hw = tw / 2.0
        steps = max(int(((bx - ax) ** 2 + (by - ay) ** 2) ** 0.5 / (G / 2)), 1)
        for s in range(steps + 1):
            t = s / float(steps)
            px, py = ax + (bx - ax) * t, ay + (by - ay) * t
            stamp(px - hw, py - hw, px + hw, py + hw, (L,))
    for (vx, vy, nn) in via_pts:
        if nn != netname:
            hv = VIA_DIA / 2.0
            stamp(vx - hv, vy - hv, vx + hv, vy + hv, (0, 1))

    m = EDGE_KEEPOUT + w / 2.0                       # board-edge keep-out
    for iy in range(NY):
        y = iy * G
        edge_row = y < m or y > HEIGHT - m
        for L in (0, 1):
            base = L * PLANE + iy * NX
            if edge_row:
                for ix in range(NX):
                    blocked[base + ix] = 1
            else:
                for ix in range(NX):
                    if ix * G < m or ix * G > WIDTH - m:
                        blocked[base + ix] = 1
    return blocked


DIRS = [(1, 0, 10), (-1, 0, 10), (0, 1, 10), (0, -1, 10),
        (1, 1, 14), (1, -1, 14), (-1, 1, 14), (-1, -1, 14)]
VIA_COST = 60                # discourage layer changes; B.Cu is the GND plane


def route(sources, targets, blocked):
    """Dijkstra over the two-layer grid. Returns a cell path or None."""
    INF = float("inf")
    dist, prev, heap = {}, {}, []
    for c in sources:
        dist[(c, -1)] = 0
        heapq.heappush(heap, (0, c, -1))
    while heap:
        d, c, pd = heapq.heappop(heap)
        if d > dist.get((c, pd), INF):
            continue
        if c in targets:
            path, key = [c], (c, pd)
            while key in prev:
                key = prev[key]
                path.append(key[0])
            path.reverse()
            return path
        L, rest = divmod(c, PLANE)
        cy_, cx_ = divmod(rest, NX)
        for di, (dx, dy, step) in enumerate(DIRS):
            nx_, ny_ = cx_ + dx, cy_ + dy
            if nx_ < 0 or nx_ >= NX or ny_ < 0 or ny_ >= NY:
                continue
            n = L * PLANE + ny_ * NX + nx_
            if blocked[n] and n not in targets:
                continue
            nd = d + step + (6 if pd != -1 and di != pd else 0)
            if nd < dist.get((n, di), INF):
                dist[(n, di)] = nd
                prev[(n, di)] = (c, pd)
                heapq.heappush(heap, (nd, n, di))
        n = (1 - L) * PLANE + rest                    # via to the other layer
        if not blocked[n] or n in targets:
            nd = d + VIA_COST
            if nd < dist.get((n, -1), INF):
                dist[(n, -1)] = nd
                prev[(n, -1)] = (c, pd)
                heapq.heappush(heap, (nd, n, -1))
    return None


def path_to_geometry(path, w, netname, snap=()):
    """Turn a cell path into track capsules + vias, merging collinear runs.

    `snap` holds the true centres of the pads this path starts and ends on.
    The router terminates on any cell of a pad's *bounding box*, which for a
    round pad can be a corner outside the actual copper - KiCad then reports
    the track as dangling and the net as unconnected. Stubbing out to the
    real pad centre is what actually makes the connection.
    """
    caps, vias = [], []
    for (cell, (px, py)) in snap:
        rest = cell % PLANE
        cx_, cy_ = (rest % NX) * G, (rest // NX) * G
        if (cx_, cy_) != (px, py):
            caps.append((cx_, cy_, px, py, w, cell // PLANE, netname))
    run = [path[0]]
    for c in path[1:]:
        if (c % PLANE) == (run[-1] % PLANE) and c // PLANE != run[-1] // PLANE:
            caps += run_to_caps(run, w, netname)      # flush before the via
            rest = c % PLANE
            vias.append(((rest % NX) * G, (rest // NX) * G, netname))
            run = [c]
        else:
            run.append(c)
    caps += run_to_caps(run, w, netname)
    return caps, vias


def run_to_caps(run, w, netname):
    if len(run) < 2:
        return []
    L = run[0] // PLANE
    pts = [((c % PLANE) % NX, (c % PLANE) // NX) for c in run]
    keep = [pts[0]]
    for i in range(1, len(pts) - 1):
        ax, ay = pts[i - 1]
        bx, by = pts[i]
        cx_, cy_ = pts[i + 1]
        if (bx - ax, by - ay) != (cx_ - bx, cy_ - by):
            keep.append(pts[i])
    keep.append(pts[-1])
    caps = []
    for i in range(len(keep) - 1):
        x1, y1 = keep[i][0] * G, keep[i][1] * G
        x2, y2 = keep[i + 1][0] * G, keep[i + 1][1] * G
        if (x1, y1) != (x2, y2):     # zero-length tracks crash BuildConnectivity
            caps.append((x1, y1, x2, y2, w, L, netname))
    return caps


results = {}
for netname in ROUTE_ORDER:
    items = list(pad_cells.get(netname, {}).items())
    if len(items) < 2:
        results[netname] = ("skip", 0.0, 0, 0)
        continue
    centres = pad_centre.get(netname, {})
    own = set()
    cell_pad = {}                           # cell -> padkey, for endpoint snap
    for key, g in items:
        own |= g
        for c in g:
            cell_pad[c] = key
    for w in NET_WIDTHS[netname]:
        blocked = build_blocked(netname, w)
        for c in own:                       # our own pads are always reachable
            blocked[c] = 0
        connected = set(items[0][1])
        remaining = [(k, set(g)) for k, g in items[1:]]
        caps, vias, ok = [], [], True
        while remaining:
            targets = set()
            for _, g in remaining:
                targets |= g
            path = route(connected, targets, blocked)
            if path is None:
                ok = False
                break
            snap = [(c, centres[cell_pad[c]])
                    for c in (path[0], path[-1])
                    if c in cell_pad and cell_pad[c] in centres]
            c2, v2 = path_to_geometry(path, w, netname, snap)
            caps += c2
            vias += v2
            connected |= set(path)
            hit = path[-1]
            for i, (k, g) in enumerate(remaining):
                if hit in g:
                    connected |= g
                    remaining.pop(i)
                    break
            for c in path:                  # our own new copper never blocks us
                blocked[c] = 0
        if ok:
            # Only commit to the shared obstacle model once the whole net fits.
            track_caps.extend(caps)
            via_pts.extend(vias)
            results[netname] = ("ok", w, len(caps), len(vias))
            break
    else:
        results[netname] = ("FAILED", 0.0, 0, 0)

# Materialise the accepted geometry. Nothing was ever added-then-removed.
for (x1, y1, x2, y2, w, L, nn) in track_caps:
    t = pcbnew.PCB_TRACK(board)
    t.SetStart(pcbnew.VECTOR2I(pcbnew.FromMM(x1), pcbnew.FromMM(y1)))
    t.SetEnd(pcbnew.VECTOR2I(pcbnew.FromMM(x2), pcbnew.FromMM(y2)))
    t.SetWidth(pcbnew.FromMM(w))
    t.SetLayer(LAYER_OF[L])
    t.SetNet(nets[nn])
    board.Add(t)
for (vx, vy, nn) in via_pts:
    v = pcbnew.PCB_VIA(board)
    v.SetPosition(pcbnew.VECTOR2I(pcbnew.FromMM(vx), pcbnew.FromMM(vy)))
    v.SetWidth(pcbnew.FromMM(VIA_DIA))
    v.SetDrill(pcbnew.FromMM(VIA_DRILL))
    v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
    v.SetNet(nets[nn])
    board.Add(v)

# ------------------------------------------------------------------- 3. POUR
gnd = get_net(PLANE_NET)
poured = []
for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
    z = pcbnew.ZONE(board)
    z.SetLayer(layer)
    z.SetNet(gnd)
    z.SetLocalClearance(pcbnew.FromMM(CLEAR))
    # F.Cu routing chops the front pour into fragments. Drop any island that
    # does not reach the main pour - otherwise a pad can end up thermally
    # bonded to a floating puddle of copper (DRC starved_thermal). THT GND
    # pads still reach the solid B.Cu plane, so nothing is lost electrically.
    z.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
    chain = pcbnew.SHAPE_LINE_CHAIN()
    i = EDGE_KEEPOUT
    for (x, y) in [(i, i), (WIDTH - i, i), (WIDTH - i, HEIGHT - i), (i, HEIGHT - i)]:
        chain.Append(pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y)))
    chain.SetClosed(True)
    # AddPolygon() copies the chain into the zone. ZONE.SetOutline() does NOT
    # take ownership of the SHAPE_POLY_SET, so the moment python garbage
    # collects it the zone holds a dangling pointer and BuildConnectivity
    # segfaults - do not "simplify" this back to SetOutline().
    z.AddPolygon(chain)
    board.Add(z)
    poured.append(z)

out = os.path.join(HERE, "%s.kicad_pcb" % PROJ)

# Save, reload, THEN fill. ZONE_FILLER -> knockoutThermalReliefs asks the DRC
# engine for the zone-connection rule of every pad it touches, and on a board
# from CreateEmptyBoard() the DRC engine has no rules loaded, so it derefs
# null and segfaults (crash is inside DRC_ENGINE::EvalRules). LoadBoard()
# initialises the engine properly. Zones with no pads under them fill fine
# either way, which is what makes this look intermittent.
pcbnew.SaveBoard(out, board)
board = pcbnew.LoadBoard(out)
board.BuildConnectivity()          # ZONE_FILLER also segfaults without this
pcbnew.ZONE_FILLER(board).Fill(list(board.Zones()))
pcbnew.SaveBoard(out, board)

print("6HP module: %d footprints, %d nets, board %.2f x %.1f mm"
      % (len(fps), len(nets), WIDTH, HEIGHT))
for zname, top, bot in zone_bounds:
    print("  zone %-9s y %6.1f -> %6.1f mm" % (zname, top, bot))
print("  %-7s poured on F.Cu + B.Cu (not routed)" % PLANE_NET)
failed = []
for n in ROUTE_ORDER:
    kind, w, segs, vias = results[n]
    if kind == "ok":
        note = "" if w >= NET_WIDTHS[n][0] else "   <-- narrowed from %.1fmm" % NET_WIDTHS[n][0]
        print("  %-7s routed %2d seg, %d via @ %.1fmm%s" % (n, segs, vias, w, note))
    elif kind == "skip":
        print("  %-7s skipped (fewer than 2 pads on the board)" % n)
    else:
        print("  %-7s *** FAILED TO ROUTE ***" % n)
        failed.append(n)
print("  total %d tracks, %d vias" % (len(track_caps), len(via_pts)))
print("-> %s" % out)
if missing:
    print("MISSING FOOTPRINTS:", missing)
if failed:
    print("UNROUTED NETS:", failed)
if HEIGHT > 110:
    print("WARNING: %.1fmm exceeds ~110mm 3U usable height" % HEIGHT)
