#!/usr/bin/env python3
"""Print a panel's holes in panel-local mm (as used by mm2px in the Rack .cpp),
each tagged with its kind (POT/BTN/LED/JACK) and the nearest printed label.

Everything is read straight from the .kicad_pcb in unmirrored PCB coords, then
mapped to the front/panel-local frame the converter SVG uses:
    local_x = board_max_x - pcb_x      (B-silk is mirror-drawn)
    local_y = pcb_y - board_min_y
(Verified against the Claves hole centres extracted from the rendered SVG.)

Usage: panel_map.py <name|path-to.kicad_pcb>
"""
import re, sys, pathlib

# Each panel lives in its own folder at repo-root panels/<name>/<name>.kicad_pcb.
# This script lives in scripts/panels/tools/, so go up to the repo root then into panels/.
PANELS = pathlib.Path(__file__).resolve().parents[3] / "panels"
RKIND = {4.0: "POT", 3.1: "JACK", 2.25: "BTN", 1.75: "LED"}

def edge_bbox(t):
    xs, ys = [], []
    for chunk in t.split('(gr_')[1:]:
        if 'Edge.Cuts' not in chunk[:2000]:
            continue
        for cm in re.finditer(r'\((?:start|end|mid|center)\s+([\-\d.]+)\s+([\-\d.]+)\)', chunk[:2000]):
            xs.append(float(cm.group(1))); ys.append(float(cm.group(2)))
    return min(xs), min(ys), max(xs), max(ys)

def holes(t):
    out = []
    for chunk in t.split('(gr_circle')[1:]:
        head = chunk[:2000]
        if 'Edge.Cuts' not in head:
            continue
        c = re.search(r'\(center ([\-\d.]+) ([\-\d.]+)\)', chunk)
        e = re.search(r'\(end ([\-\d.]+) ([\-\d.]+)\)', chunk)
        if not (c and e):
            continue
        cx, cy = float(c.group(1)), float(c.group(2))
        ex, ey = float(e.group(1)), float(e.group(2))
        out.append((cx, cy, ((ex - cx) ** 2 + (ey - cy) ** 2) ** 0.5))
    return out

def labels(t):
    out = []
    for m in re.finditer(r'\(gr_text "((?:[^"\\]|\\.)*)"\s*\(at ([\-\d.]+) ([\-\d.]+)', t):
        out.append((m.group(1).replace('\\n', ' '), float(m.group(2)), float(m.group(3))))
    return out

def panel_map(pcb):
    t = pathlib.Path(pcb).read_text()
    bx0, by0, bx1, by1 = edge_bbox(t)
    labs = labels(t)
    rows = []
    for cx, cy, r in holes(t):
        lx, ly = bx1 - cx, cy - by0
        kind = RKIND.get(round(r, 2), f"r{r:.1f}")
        # nearest label (unmirrored coords); skip frame text
        best, bd = "", 9e9
        for s, x, y in labs:
            if s in ('madelyn.sh', 'pcb by hagiwo', 'maddie synths'):
                continue
            d = (x - cx) ** 2 + (y - cy) ** 2
            if d < bd:
                bd, best = d, s
        rows.append((kind, lx, ly, r, best))
    return rows

if __name__ == "__main__":
    arg = sys.argv[1]
    pcb = arg if arg.endswith('.kicad_pcb') else str(PANELS / arg / f"{arg}.kicad_pcb")
    print(f"# {pathlib.Path(pcb).stem}")
    for kind, lx, ly, r, lab in sorted(panel_map(pcb), key=lambda z: (z[2], z[1])):
        print(f"  {kind:5} ({lx:6.2f},{ly:7.2f})  label={lab!r}")
