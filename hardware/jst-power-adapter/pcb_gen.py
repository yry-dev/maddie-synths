#!/usr/bin/env python3
"""Build both JST adapter boards (16-pin and 10-pin) with pcbnew.

Run with KiCad's bundled python (it has pcbnew), after gen.py:
  /Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/\
Versions/Current/bin/python3 pcb_gen.py

Each board is a few parts, so placement and every track are fixed
coordinates written out below. There is no router and no packer. The
script checks its own work before it saves:
  - every track end must land on a pad or via of the same net, on a
    copper layer that pad really has (SMD pads are F.Cu only);
  - the socket's pin 1 must be where the silk says, with the same
    handedness as a module's header (see place_socket()).
Then run kicad-cli DRC; it checks clearances, widths and silk.

Coordinates are in mm in the "socket frame": x runs along the socket,
socket pin column k (pins 2k+1, 2k+2) sits at x = 2.54k, and the two pin
rows sit at y = -1.27 (north) and y = +1.27 (south). Pins 1/2 (-12V) are
at x = 0, the west end. The board is offset by ORIGIN on the sheet.

Mechanical rules behind the numbers:
  - The female socket (bottom face) drops into the module's shrouded box
    header, and the board rests on the shroud top. KiCad's DIN 41651 box
    header is 9.0mm wide; some makers quote 8.9mm. Use 9.0, so the shroud
    spans y = +/-4.5. A J1 pin pokes out below the board, so its pad edge
    must sit 0.5mm or more outside the shroud: J1 pin y >= 4.5 + 0.5 +
    0.7 (pad radius) = 5.7. J1 runs beside the socket, never past its
    ends, so the shroud length (28-33mm for 2x8, 20-26mm for 2x5,
    depending on maker) does not constrain this layout.
  - The socket's solder tails stick up through the top. No body sits on
    them: every top part keeps its courtyard clear of y = +/-2.12 (the
    socket pad edges).
  - Tracks are 0.5mm (module currents are well under 1A on 1oz copper).
    Between socket pins there is 0.84mm of copper gap, too narrow for a
    0.5mm track at 0.2mm clearance, so nets cross the socket around its
    ends or on the bottom layer.
"""
# License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.

import json, os, pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
STD = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints"
MADDIE = os.path.normpath(os.path.join(HERE, "..", "lib", "maddie.pretty"))
ORIGIN = (100.0, 100.0)
W = 0.5                      # track width, every net
VIA_D, VIA_DRILL = 0.8, 0.4
T, B = "T", "B"              # layer shorthands for the track tables

J1_POS = (4.45, 5.7)         # J1 centre, rot 180: pin 1 west at x = 0.45
DIODE_Y = 4.2                # |y| of a diode row: courtyard clears y = 2.12

# ------------------------------------------------------------- variants
# Part placement: ref -> (x, y, rotation). Diodes at rot 0 have pad 1
# (cathode) on the west side.
#
# Shared plan for both boards:
#   north strip  D2 (-12V) west by socket pin 1, D1 (+12V) east by pins 9/10
#   south strip  J1 west (pin 1 = -12V at the west end), D3 east (16-pin)
#   -12V_IN      J1.1 -> bottom layer -> around the west end of the socket
#                -> via -> D2 cathode
#   +12V_IN      J1.3 -> bottom layer, south between pins 2 and 4, then
#                along the south edge -> around the east end of the socket
#                -> via -> D1 anode
#   GND          J1.2 straight north to socket pin 3/4; J1.4 joins J1.2
#                on the bottom, north of the J1 pins (+12V_IN leaves pin 3
#                to the south, so the link cannot pass there)
# Rev B (2026-11-12): J1 pins 1 and 3 swapped to match the bus board.
VARIANTS = {
    16: dict(
        edge=(-2.2, -8.3, 20.4, 8.8), notch=None,
        place={"J1": J1_POS + (180,), "D2": (3.0, -DIODE_Y, 0),
               "D1": (14.5, -DIODE_Y, 0), "D3": (15.5, DIODE_Y, 0)},
        vias=[(-1.0, -2.6, "-12V_IN"), (19.4, -4.2, "+12V_IN")],
        tracks=[
            # socket pin pairs (both rows of a column carry the same net)
            ("-12V", T, [(0, -1.27), (0, 1.27)]),
            ("GND", T, [(2.54, -1.27), (2.54, 1.27)]),
            ("+12V", T, [(10.16, -1.27), (10.16, 1.27)]),
            ("+5V", T, [(12.7, -1.27), (12.7, 1.27)]),
            # -12V: D2 anode -> socket pins 1/2, in the channel north of the pins
            ("-12V", T, [(5.5, -4.2), (4.35, -2.75), (0.8, -2.75), (0, -1.95), (0, -1.27)]),
            ("-12V_IN", T, [(-1.0, -2.6), (0.5, -4.2)]),
            ("-12V_IN", B, [(-1.0, -2.6), (-1.4, -2.2), (-1.4, 3.85), (0.45, 5.7)]),
            # +12V: D1 cathode -> socket pins 9/10
            ("+12V", T, [(12.0, -4.2), (10.16, -2.36), (10.16, -1.27)]),
            ("+12V_IN", T, [(19.4, -4.2), (17.0, -4.2)]),
            ("+12V_IN", B, [(4.45, 5.7), (4.45, 7.0), (5.25, 7.8), (18.6, 7.8),
                            (19.4, 7.0), (19.4, -4.2)]),
            # GND
            ("GND", T, [(2.45, 5.7), (2.54, 1.27)]),
            ("GND", B, [(2.45, 5.7), (3.7, 4.45), (5.2, 4.45), (6.45, 5.7)]),
            # +5V: J1.5 -> D3 anode (south of D3), D3 cathode -> pins 11/12
            ("+5V_IN", T, [(8.45, 5.7), (9.4, 6.65), (17.2, 6.65), (18.0, 5.85), (18.0, 4.2)]),
            ("+5V", T, [(13.0, 4.2), (12.7, 1.27)]),
        ]),
    10: dict(
        # The notch in the west edge marks the -12V end: rotation on a
        # 10-pin header swaps +12V and -12V and no wiring can prevent it.
        edge=(-3.7, -8.3, 17.1, 8.8), notch=(1.3, 3.0),
        place={"J1": J1_POS + (180,), "D2": (3.0, -DIODE_Y, 0),
               "D1": (12.3, -DIODE_Y, 0)},
        vias=[(-1.0, -2.6, "-12V_IN"), (14.8, -2.6, "+12V_IN")],
        tracks=[
            ("-12V", T, [(0, -1.27), (0, 1.27)]),
            # pins 3-8 are all GND: three column ties on top, one row tie below
            ("GND", T, [(2.54, -1.27), (2.54, 1.27)]),
            ("GND", T, [(5.08, -1.27), (5.08, 1.27)]),
            ("GND", T, [(7.62, -1.27), (7.62, 1.27)]),
            ("GND", B, [(2.54, 1.27), (5.08, 1.27), (7.62, 1.27)]),
            ("+12V", T, [(10.16, -1.27), (10.16, 1.27)]),
            ("-12V", T, [(5.5, -4.2), (4.35, -2.75), (0.8, -2.75), (0, -1.95), (0, -1.27)]),
            ("-12V_IN", T, [(-1.0, -2.6), (0.5, -4.2)]),
            ("-12V_IN", B, [(-1.0, -2.6), (-1.4, -2.2), (-1.4, 3.85), (0.45, 5.7)]),
            ("+12V", T, [(9.8, -4.2), (10.16, -3.0), (10.16, -1.27)]),
            ("+12V_IN", T, [(14.8, -2.6), (14.8, -4.2)]),
            ("+12V_IN", B, [(4.45, 5.7), (4.45, 7.0), (5.25, 7.8), (10.8, 7.8),
                            (11.6, 7.0), (11.6, -1.6), (12.6, -2.6), (14.8, -2.6)]),
            ("GND", T, [(2.45, 5.7), (2.54, 1.27)]),
            ("GND", B, [(2.45, 5.7), (3.7, 4.45), (5.2, 4.45), (6.45, 5.7)]),
        ]),
}

# J1 pin labels on the top silk, between the socket and the J1 housing.
# Odd pins carry the rails; "G" marks the two GND pins.
J1_LABELS = {16: ["-12", "G", "+12", "G", "+5"], 10: ["-12", "G", "+12", "G", "NC"]}
LABEL_Y = 3.0
# The same J1 labels go on the bottom silk, south of the J1 pads.
J1_LABEL_Y_BOTTOM = 7.55
# J2 rail labels on the bottom silk, one for each socket column (pins 2k+1,
# 2k+2), in the band north of the socket outline. "NC" = open on this board.
J2_LABELS = {16: ["-12", "G", "NC", "NC", "+12", "+5", "NC", "NC"],
             10: ["-12", "G", "G", "G", "+12"]}
J2_LABEL_Y = -3.6


# ------------------------------------------------------------- helpers
def V(x, y):
    return pcbnew.VECTOR2I(pcbnew.FromMM(ORIGIN[0] + x), pcbnew.FromMM(ORIGIN[1] + y))

def mm(v):
    return (round(pcbnew.ToMM(v.x) - ORIGIN[0], 3), round(pcbnew.ToMM(v.y) - ORIGIN[1], 3))

def libpath(lib):
    return MADDIE if lib == "maddie" else "%s/%s.pretty" % (STD, lib)

def load_fp(board, fpid):
    lib, name = fpid.split(":", 1)
    fp = pcbnew.FootprintLoad(libpath(lib), name)
    if fp is None:
        raise SystemExit("footprint %s not found" % fpid)
    fp.SetFPID(pcbnew.LIB_ID(lib, name))
    # Flip() segfaults on a footprint that has no board, so add it first.
    board.Add(fp)
    return fp

def handedness(fp):
    """Sign of (pin2 - pin1) x (pin3 - pin1) in top-view board coords."""
    p = {pad.GetNumber(): pad.GetPosition() for pad in fp.Pads()}
    ax, ay = p["2"].x - p["1"].x, p["2"].y - p["1"].y
    bx, by = p["3"].x - p["1"].x, p["3"].y - p["1"].y
    return (ax * by - ay * bx) > 0


def place_socket(board, fp):
    """Flip the socket to the bottom face and put pin 1 at the west end.

    How the pin-1 mapping is verified. A module's header sits on the
    module's top face, and the adapter's socket hangs from the adapter's
    bottom face. With the adapter on the header, both boards are seen
    from the same side (from above), so socket pad n must sit at the same
    place as header pad n in ordinary top-view board coordinates. A
    rotation can fix position; only a mirror can break it. So:
      1. the socket's pin 1 must be at the end the silk marks -12V
         (checked: pin 1 lands at x = 0 and pins 3, 5 ... run east);
      2. the socket's handedness, (pin2 - pin1) x (pin3 - pin1), must
         equal that of KiCad's stock IDC box header (on the top face,
         not flipped), which is the module side of the mating pair.
    KiCad's PinSocket footprints are drawn mirror-image to PinHeader, so
    an unflipped socket fails check 2 and the flipped one passes. Both
    checks run here and stop the build if either fails.
    Belt and braces: each socket column (2k+1, 2k+2) carries one net, so
    even a row mirror would not change any connection.
    """
    fp.Flip(fp.GetPosition(), pcbnew.FLIP_DIRECTION_TOP_BOTTOM)
    for deg in (0, 90, 180, 270):
        fp.SetOrientationDegrees(deg)
        p = {pad.GetNumber(): pad.GetPosition() for pad in fp.Pads()}
        d3 = (pcbnew.ToMM(p["3"].x - p["1"].x), pcbnew.ToMM(p["3"].y - p["1"].y))
        if abs(d3[0] - 2.54) < 1e-3 and abs(d3[1]) < 1e-3:
            break
    else:
        raise SystemExit("no socket rotation puts pin 3 east of pin 1")
    dy2 = pcbnew.ToMM(p["2"].y - p["1"].y)
    pin1 = (0.0, -1.27 if dy2 > 0 else 1.27)
    off = V(*pin1) - p["1"]
    fp.SetPosition(fp.GetPosition() + off)

    # Pad positions are valid on a footprint that never joins the board.
    module_side = handedness(
        pcbnew.FootprintLoad(libpath("Connector_IDC"), "IDC-Header_2x08_P2.54mm_Vertical"))
    if handedness(fp) != module_side:
        raise SystemExit("socket handedness does not match a module header")
    if mm(fp.FindPadByNumber("1").GetPosition()) != pin1:
        raise SystemExit("socket pin 1 is not at the -12V (west) end")
    return pin1


def silk_text(board, s, x, y, layer, h, w=None, th=0.18, angle=0, just="center"):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(s)
    t.SetLayer(layer)
    t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(w or h), pcbnew.FromMM(h)))
    t.SetTextThickness(pcbnew.FromMM(th))
    t.SetTextAngleDegrees(angle)
    if just == "left":
        t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_LEFT)
    elif just == "right":
        t.SetHorizJustify(pcbnew.GR_TEXT_H_ALIGN_RIGHT)
    if layer == pcbnew.B_SilkS:
        t.SetMirrored(True)      # reads correctly when the board is turned over
    t.SetPosition(V(x, y))
    board.Add(t)
    return t

def silk_poly(board, pts, layer):
    s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_POLY)
    s.SetPolyPoints([V(x, y) for x, y in pts])
    s.SetFilled(True)
    s.SetWidth(pcbnew.FromMM(0.15))
    s.SetLayer(layer)
    board.Add(s)

def edge_line(board, a, b):
    s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_SEGMENT)
    s.SetStart(V(*a))
    s.SetEnd(V(*b))
    s.SetWidth(pcbnew.FromMM(0.1))
    s.SetLayer(pcbnew.Edge_Cuts)
    board.Add(s)

def edge_arc(board, start, mid, end):
    s = pcbnew.PCB_SHAPE(board, pcbnew.SHAPE_T_ARC)
    s.SetArcGeometry(V(*start), V(*mid), V(*end))
    s.SetWidth(pcbnew.FromMM(0.1))
    s.SetLayer(pcbnew.Edge_Cuts)
    board.Add(s)

def outline(board, x0, y0, x1, y1, notch):
    """Rectangle with 1mm corner radii; optional notch (depth, height) centred
    on the west edge at y = 0."""
    r, k = 1.0, 1.0 - 0.70711
    edge_line(board, (x0 + r, y0), (x1 - r, y0))
    edge_arc(board, (x1 - r, y0), (x1 - r * k, y0 + r * k), (x1, y0 + r))
    edge_line(board, (x1, y0 + r), (x1, y1 - r))
    edge_arc(board, (x1, y1 - r), (x1 - r * k, y1 - r * k), (x1 - r, y1))
    edge_line(board, (x1 - r, y1), (x0 + r, y1))
    edge_arc(board, (x0 + r, y1), (x0 + r * k, y1 - r * k), (x0, y1 - r))
    if notch:
        d, h = notch
        for a, b in [((x0, y1 - r), (x0, h / 2)), ((x0, h / 2), (x0 + d, h / 2)),
                     ((x0 + d, h / 2), (x0 + d, -h / 2)), ((x0 + d, -h / 2), (x0, -h / 2)),
                     ((x0, -h / 2), (x0, y0 + r))]:
            edge_line(board, a, b)
    else:
        edge_line(board, (x0, y1 - r), (x0, y0 + r))
    edge_arc(board, (x0, y0 + r), (x0 + r * k, y0 + r * k), (x0 + r, y0))


# ------------------------------------------------------------- build
def build(n):
    spec = VARIANTS[n]
    folder = os.path.join(HERE, "%dpin" % n)
    data = json.load(open(os.path.join(folder, "board_data.json")))
    proj = data["project"]
    board = pcbnew.CreateEmptyBoard()
    nets = {}

    def net(name):
        if name not in nets:
            ni = pcbnew.NETINFO_ITEM(board, name)
            board.Add(ni)
            nets[name] = ni
        return nets[name]

    fps = {}
    for c in data["components"]:
        ref = c["ref"]
        fp = load_fp(board, c["fp"])
        fp.SetReference(ref)
        fp.SetValue(c["value"])
        fp.SetPath(pcbnew.KIID_PATH("/" + c["uuid"]))
        for k, v in c["fields"].items():
            fp.SetField(k, v)
            fp.GetField(k).SetVisible(False)     # BOM data, not silk
        fp.Value().SetVisible(False)
        for pad in fp.Pads():
            num = pad.GetNumber()
            name = c["nets"].get(num) or c["nc_nets"].get(num)
            if name:
                pad.SetNet(net(name))
        if ref == "J2":
            place_socket(board, fp)
            fp.Reference().SetVisible(False)
        else:
            x, y, r = spec["place"][ref]
            fp.SetOrientationDegrees(r)
            fp.SetPosition(V(x, y))
        fps[ref] = fp

    # Fixed-position checks: the track tables below assume these pads.
    want = {("J1", "1"): (0.45, 5.7), ("J1", "5"): (8.45, 5.7)}
    for ref in ("D1", "D2", "D3"):
        if ref in fps:
            x, y, _ = spec["place"][ref]
            want[(ref, "1")] = (round(x - 2.5, 3), y)      # cathode, west
    for (ref, num), xy in want.items():
        got = mm(fps[ref].FindPadByNumber(num).GetPosition())
        if got != xy:
            raise SystemExit("%s.%s at %s, expected %s" % (ref, num, got, xy))

    # J1: the shared library footprint points its 3D model at
    # ${KIPRJMOD}/../lib, which is right for projects one level below
    # hardware/. These projects are two levels down, so fix the path here.
    # Index the vector: a for-loop over Models() edits throwaway copies.
    models = fps["J1"].Models()
    for i in range(len(models)):
        models[i].m_Filename = models[i].m_Filename.replace(
            "${KIPRJMOD}/../lib/", "${KIPRJMOD}/../../lib/")
    fps["J1"].Reference().SetVisible(False)

    # Diode refs: inside each body outline, upright, JLC-legal size.
    for ref in ("D1", "D2", "D3"):
        if ref in fps:
            t = fps[ref].Reference()
            t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(0.9), pcbnew.FromMM(1.0)))
            t.SetTextThickness(pcbnew.FromMM(0.15))
            t.SetTextAngleDegrees(90)
            x, y, _ = spec["place"][ref]
            t.SetPosition(V(x, y))

    # ---- copper
    vias = []
    for (x, y, name) in spec["vias"]:
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(V(x, y))
        v.SetWidth(pcbnew.FromMM(VIA_D))
        v.SetDrill(pcbnew.FromMM(VIA_DRILL))
        v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
        v.SetNet(net(name))
        board.Add(v)
        vias.append(v)

    def anchored(pt, name, layer):
        """True if pt is on copper of net `name` that exists on `layer`."""
        p = V(*pt)
        for fp in fps.values():
            for pad in fp.Pads():
                if pad.GetNetname() == name and pad.IsOnLayer(layer) and pad.HitTest(p):
                    return True
        return any(v.GetNetname() == name and v.HitTest(p) for v in vias)

    for (name, lay, pts) in spec["tracks"]:
        layer = pcbnew.F_Cu if lay == T else pcbnew.B_Cu
        for end in (pts[0], pts[-1]):
            if not anchored(end, name, layer):
                raise SystemExit("%s track end %s is not on a %s pad/via on %s"
                                 % (name, end, name, lay))
        for a, b in zip(pts, pts[1:]):
            t = pcbnew.PCB_TRACK(board)
            t.SetStart(V(*a))
            t.SetEnd(V(*b))
            t.SetWidth(pcbnew.FromMM(W))
            t.SetLayer(layer)
            t.SetNet(net(name))
            board.Add(t)

    # ---- outline
    x0, y0, x1, y1 = spec["edge"]
    outline(board, x0, y0, x1, y1, spec["notch"])

    # ---- silk, top
    F, Bk = pcbnew.F_SilkS, pcbnew.B_SilkS
    band_y = (y0 + -6.0) / 2          # between the board edge and the diode courtyards
    # Arrow points west, at the -12V end (socket pins 1/2, the red stripe).
    silk_poly(board, [(-1.8, band_y), (-0.4, band_y - 0.75), (-0.4, band_y + 0.75)], F)
    top_h = 1.2 if n == 16 else 1.0
    silk_text(board, "-12V / RED STRIPE", 0.1, band_y, F, top_h, th=0.2, just="left")
    for k, s in enumerate(J1_LABELS[n]):
        silk_text(board, s, 0.45 + 2.0 * k, LABEL_Y, F, 1.0, w=0.8, th=0.15)

    # ---- silk, bottom (mirrored so it reads when the board is turned over)
    # Three rows north of the socket: revision, red-stripe marker, J2 labels.
    silk_poly(board, [(-1.8, -5.35), (-0.4, -6.0), (-0.4, -4.7)], Bk)
    # Mirrored text runs the other way, so "right" justify grows east here.
    silk_text(board, "-12V / RED STRIPE", 0.1, -5.35, Bk, top_h, th=0.2, just="right")
    # y = -7.0, not nearer the edge: the panel's mouse-bite holes on the top
    # tab reach y = -7.85 and would clip the text.
    silk_text(board, "%s rev B" % proj, -1.8, -7.0, Bk, 1.0, th=0.15, just="right")
    for k, s in enumerate(J2_LABELS[n]):
        silk_text(board, s, 2.54 * k, J2_LABEL_Y, Bk, 1.0, w=0.8, th=0.15)
    for k, s in enumerate(J1_LABELS[n]):
        silk_text(board, s, 0.45 + 2.0 * k, J1_LABEL_Y_BOTTOM, Bk, 1.0, w=0.8, th=0.15)
    # URL in the free band between the socket outline (y ~2.7) and the J1
    # pads (top edge y ~5.0). It may cross B.Cu tracks (mask covers them),
    # but not pads. Centred on the board width.
    silk_text(board, "https://madelyn.sh", (x0 + x1) / 2, 3.85, Bk, 1.0, w=0.8, th=0.15)

    # Every text must lie inside the outline.
    for d in board.GetDrawings():
        if isinstance(d, pcbnew.PCB_TEXT):
            bb = d.GetEffectiveTextShape().BBox()   # the strokes, not the text box
            l, t_ = mm(bb.GetOrigin())
            r_, b_ = mm(bb.GetEnd())
            if l < x0 or r_ > x1 or t_ < y0 or b_ > y1:
                raise SystemExit("silk text %r runs off the board (%.2f..%.2f, %.2f..%.2f)"
                                 % (d.GetText(), l, r_, t_, b_))

    out = os.path.join(folder, proj + ".kicad_pcb")
    # aSkipSettings=True: keep gen.py's .kicad_pro (net class, DRC rules).
    # Without it pcbnew overwrites the project file with bare defaults.
    pcbnew.SaveBoard(out, board, True)
    print("%s: board %.1f x %.1f mm, %d footprints, %d tracks, %d vias -> %s"
          % (proj, x1 - x0, y1 - y0, len(fps), len(board.GetTracks()) - len(vias),
             len(vias), out))
    print("  socket pin 1 at (0, %+.2f) socket frame, bottom face, handedness matches a module header"
          % mm(fps["J2"].FindPadByNumber("1").GetPosition())[1])


if __name__ == "__main__":
    build(16)
    build(10)
