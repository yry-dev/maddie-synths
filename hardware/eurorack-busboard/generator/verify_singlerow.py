"""Independent verification of the single-row board.

pcbgen's own check() only looks at clearance - it will happily pass a board
where a track goes nowhere. This checks the opposite question: is every net
actually joined up, and does every plane band survive the obstacles punched
through it?
"""
import sys
import pcbgen_singlerow as M

W, H = M.BOARD_W, M.BOARD_H
CLR = 0.4                                   # zone clearance used in the fill

# ---------------------------------------------------------------- items
ZONES = [("GND", "In1.Cu", 1.5, H - 1.5), ("GND", "B.Cu", 1.5, H - 1.5),
         ("+5V", "In2.Cu", *M.BAND_P5_A), ("+12V", "In2.Cu", *M.BAND_P12_A),
         ("-12V", "In2.Cu", *M.BAND_N12)]

items = list(M.geo)
for nm, layer, y0, y1 in ZONES:
    items.append(("zone", {layer}, nm, ((1.5 + W - 1.5) / 2, (y0 + y1) / 2,
                                        W - 3.0, y1 - y0)))


def touching(a, b):
    """True if two copper items overlap (i.e. are galvanically joined)."""
    k1, _, _, d1 = a
    k2, _, _, d2 = b
    if k1 == "seg" and k2 == "seg":
        return M.seg_seg_dist(d1, d2) <= 0
    if k1 == "seg":
        return M.seg_rect_dist(d1, M.to_rect(k2, d2) or d2) <= 0
    if k2 == "seg":
        return M.seg_rect_dist(d2, M.to_rect(k1, d1) or d1) <= 0
    r1 = M.to_rect(k1, d1) or d1
    r2 = M.to_rect(k2, d2) or d2
    return M.rect_rect_dist(r1, r2) <= 0


def connectivity():
    bynet = {}
    for it in items:
        if it[2] == "__HOLE__":
            continue
        bynet.setdefault(it[2], []).append(it)

    bad = []
    for nm, its in sorted(bynet.items()):
        parent = list(range(len(its)))

        def find(i):
            while parent[i] != i:
                parent[i] = parent[parent[i]]
                i = parent[i]
            return i

        for i in range(len(its)):
            for j in range(i + 1, len(its)):
                if not (its[i][1] & its[j][1]):
                    continue
                if touching(its[i], its[j]):
                    a, b = find(i), find(j)
                    if a != b:
                        parent[a] = b
        groups = {}
        for i in range(len(its)):
            groups.setdefault(find(i), []).append(its[i])
        if len(groups) > 1:
            bad.append((nm, len(its), [[(g[0], g[3]) for g in grp][:3]
                                       for grp in groups.values()]))
    return bad


def band_continuity():
    """A band is severed if obstacles span its full height at some X.

    Obstacles are through-hole pads, vias and drills belonging to other nets,
    grown by the fill clearance. Sampled every 0.1 mm across the board.
    """
    bad = []
    for nm, layer, y0, y1 in ZONES:
        if layer != "In2.Cu":
            continue
        obst = []
        for k, layers, net, d in M.geo:
            if layer not in layers or net == nm:
                continue
            if k == "via":
                x, y, dia = d
                obst.append((x - dia / 2 - CLR, x + dia / 2 + CLR,
                             y - dia / 2 - CLR, y + dia / 2 + CLR))
            elif k == "pad":
                x, y, pw, ph = d
                obst.append((x - pw / 2 - CLR, x + pw / 2 + CLR,
                             y - ph / 2 - CLR, y + ph / 2 + CLR))
        x = 2.0
        while x < W - 2.0:
            covered = []
            for xa, xb, ya, yb in obst:
                if xa <= x <= xb:
                    covered.append((max(ya, y0), min(yb, y1)))
            covered.sort()
            reach = y0
            for ca, cb in covered:
                if ca > reach + 1e-9:
                    break
                reach = max(reach, cb)
            if reach >= y1 - 1e-9:
                bad.append((nm, round(x, 2)))
                x += 5.0
                continue
            x += 0.1
    return bad


def on_board():
    bad = []
    for f in M.footprints:
        r = M.body_rect(f)
        if r and (r[0] < 0 or r[1] < 0 or r[2] > W or r[3] > H):
            bad.append((f.ref, [round(v, 2) for v in r]))
    for k, _, net, d in M.geo:
        if k == "seg":
            xs, ys, xe, ye, w = d
            pts = ((xs, ys), (xe, ye))
        else:
            x, y = d[0], d[1]
            w = max(d[2], d[3]) if k == "pad" else d[2]
            pts = ((x, y),)
        for px, py in pts:
            if not (w / 2 <= px <= W - w / 2 and w / 2 <= py <= H - w / 2):
                bad.append((net, k, [round(v, 2) for v in d]))
    return bad


if __name__ == "__main__":
    fails = 0
    nc = connectivity()
    print(f"nets: {len(set(i[2] for i in items if i[2] != '__HOLE__'))}"
          f"   unconnected: {len(nc)}")
    for nm, n, groups in nc:
        fails += 1
        print(f"  {nm}: {n} items in {len(groups)} pieces")
        for g in groups:
            print("     ", g)

    bc = band_continuity()
    print(f"severed plane bands: {len(bc)}")
    for nm, x in bc:
        fails += 1
        print(f"  {nm} pinched off at x={x}")

    ob = on_board()
    print(f"items outside the outline: {len(ob)}")
    for b in ob[:10]:
        fails += 1
        print("  ", b)

    sys.exit(1 if fails else 0)
