#!/usr/bin/env python3
"""Convert a KiCad panel-PCB faceplate into a VCV Rack panel SVG.

Front art lives on B.Silkscreen (mirrored to read correctly from the front).
The module width MUST equal the Edge.Cuts width -- KiCad's --page-size-mode 2
crops to *all* content (silk overhang included), so we instead measure the real
board from the gray Edge.Cuts geometry in the plot and clip the silk to it, the
same way fabrication trims silk at the board edge.

Usage: kicad_to_panel.py <board.kicad_pcb> <Name> [out_dir]
"""
import re, sys, subprocess, pathlib

KCLI = "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"
SILK_COLOR = "#E8B2A7"   # KiCad B.Silkscreen plot color
EDGE_COLOR = "#D0D2CD"   # KiCad Edge.Cuts plot color
MASK_COLOR = "#02FFEE"   # KiCad B.Mask plot color (soldermask openings = bare copper)
PANEL_BG   = "#221b22"   # Maddie Synths house background
PANEL_FG   = "#f0e6ee"   # silk -> light
PANEL_EDGE = "#48384a"   # board outline / holes -> faint accent
FINISH_GOLD   = "#c9a84c"  # ENIG gold
FINISH_SILVER = "#c8ccd0"  # silver / HASL
PANEL_H_MM = 128.5       # Eurorack panel height

def plot(pcb, out):
    subprocess.run([KCLI, "pcb", "export", "svg",
                    "--layers", "B.Silkscreen,B.Mask,Edge.Cuts",
                    "--page-size-mode", "2", "--mirror", "--exclude-drawing-sheet",
                    "-o", str(out), str(pcb)], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def path_points(d):
    """Yield absolute (x,y) anchor points of an SVG path, skipping arc radii/flags."""
    toks = re.findall(r'[MmLlHhVvCcSsQqTtAaZz]|-?\d*\.?\d+(?:[eE]-?\d+)?', d)
    i, cx, cy, cmd = 0, 0.0, 0.0, None
    n = len(toks)
    def num():
        nonlocal i
        v = float(toks[i]); i += 1; return v
    while i < n:
        t = toks[i]
        if re.match(r'[A-Za-z]', t):
            cmd = t; i += 1
            if cmd in 'Zz':
                continue
        rel = cmd.islower()
        c = cmd.upper()
        if c in ('M', 'L', 'T'):
            x, y = num(), num()
            if rel: x, y = cx + x, cy + y
            cx, cy = x, y; yield x, y
        elif c == 'H':
            x = num(); cx = cx + x if rel else x; yield cx, cy
        elif c == 'V':
            y = num(); cy = cy + y if rel else y; yield cx, cy
        elif c in ('C', 'S', 'Q'):
            k = 3 if c == 'C' else 2
            pts = [(num(), num()) for _ in range(k)]
            if rel: pts = [(cx + a, cy + b) for a, b in pts]
            for a, b in pts: yield a, b
            cx, cy = pts[-1]
        elif c == 'A':
            num(); num(); num(); num(); num()      # rx ry rot large sweep
            x, y = num(), num()
            if rel: x, y = cx + x, cy + y
            cx, cy = x, y; yield x, y
        else:
            i += 1

def edge_bbox(svg):
    """Bounding box of the gray Edge.Cuts geometry (outline + holes).

    KiCad colours each element via its wrapping <g style="...#D0D2CD...">, so we
    select groups by colour and parse the paths/circles inside each.
    """
    xs, ys = [], []
    for chunk in svg.split('<g ')[1:]:
        gt = chunk.find('>')
        if gt < 0 or EDGE_COLOR not in chunk[:gt]:
            continue
        end = chunk.find('</g>')
        body = chunk[gt + 1: end if end >= 0 else None]
        for cm in re.finditer(r'<circle\b[^>]*>', body):
            el = cm.group(0)
            cx = float(re.search(r'cx="([\-\d.]+)"', el).group(1))
            cy = float(re.search(r'cy="([\-\d.]+)"', el).group(1))
            r  = float(re.search(r'r="([\-\d.]+)"', el).group(1))
            xs += [cx - r, cx + r]; ys += [cy - r, cy + r]
        for dm in re.finditer(r'\bd="([^"]+)"', body):
            for x, y in path_points(dm.group(1)):
                xs.append(x); ys.append(y)
    return min(xs), min(ys), max(xs), max(ys)

def convert(pcb, name, out_dir, finish=FINISH_GOLD):
    out_dir = pathlib.Path(out_dir)
    raw = out_dir / f"_{name}.raw.svg"
    plot(pcb, raw)
    svg = raw.read_text()
    x0, y0, x1, y1 = edge_bbox(svg)
    w = x1 - x0
    # force exact panel height, centered on the measured board
    yc = (y0 + y1) / 2
    py0 = yc - PANEL_H_MM / 2
    inner = svg[svg.index('>', svg.index('<svg')) + 1: svg.rindex('</svg>')]
    inner = re.sub(r'<title>.*?</title>', '', inner, flags=re.S)
    inner = (inner.replace(SILK_COLOR, PANEL_FG)
                  .replace(MASK_COLOR, finish)
                  .replace(EDGE_COLOR, PANEL_EDGE))
    inner = inner.replace('fill:#000000; fill-opacity:1.0000;stroke:#000000',
                          'fill:none;stroke:none')
    panel = f'''<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="{w:.4f}mm" height="{PANEL_H_MM}mm" viewBox="{x0:.4f} {py0:.4f} {w:.4f} {PANEL_H_MM}">
  <defs>
    <clipPath id="board"><rect x="{x0:.4f}" y="{py0:.4f}" width="{w:.4f}" height="{PANEL_H_MM}"/></clipPath>
  </defs>
  <rect x="{x0:.4f}" y="{py0:.4f}" width="{w:.4f}" height="{PANEL_H_MM}" fill="{PANEL_BG}"/>
  <g clip-path="url(#board)">
{inner}
  </g>
</svg>
'''
    out = out_dir / f"{name}.svg"
    out.write_text(panel)
    raw.unlink()
    print(f"{name}: board {w:.3f} x {PANEL_H_MM} mm  ({w/5.08:.2f} HP)  -> {out}")
    return out

if __name__ == "__main__":
    pcb, name = sys.argv[1], sys.argv[2]
    out_dir = sys.argv[3] if len(sys.argv) > 3 else pathlib.Path(__file__).parent
    convert(pcb, name, out_dir)
