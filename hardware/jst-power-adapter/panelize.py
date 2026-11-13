#!/usr/bin/env python3
"""Panelize the JST power adapter boards for JLCPCB with KiKit.

Run with KiCad's bundled python, which must have KiKit installed:
  KP=/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3
  $KP -m pip install --user kikit        # once per machine
  $KP panelize.py                        # both boards, 3 x 4 grid
  $KP panelize.py --boards 16 --rows 3 --cols 3
  $KP panelize.py --out /tmp/panels      # write somewhere else

Run it AFTER you finish the board and silkscreen. It reads the generated
16pin/ and 10pin/ boards as they are, so hand edits made in pcbnew after
pcb_gen.py are included. It never writes to the source boards.

Output: panel/jst-adapter-<n>-panel.kicad_pcb (plus its .kicad_pro, which
carries the board's design rules into the panel). Make the JLCPCB files
from the panel with the fabrication toolkit, the same way as for one board.

Why each choice in panel-preset.json:
- One design per panel. JLCPCB charges a fee for each extra design in one
  file, so the 16-pin and 10-pin boards get separate panels.
- Mouse-bite tabs, not V-cut. The boards have rounded corners and the
  10-pin board has a notch, and a V-cut must run straight across the panel.
- Tabs on the top and bottom edges only. In KiKit, "vcount" counts the
  tabs on the top and bottom edges and "hcount" counts them on the sides,
  so the preset sets "hcount": 0, "vcount": 1. The -12V_IN and
  +12V_IN tracks run around the west and east ends of the board on B.Cu,
  and a side tab put mouse-bite holes 0.1 mm from +12V_IN. Each column of
  boards hangs from the rails. If you move a track near the top or bottom
  edge, run DRC on the panel and look for hole_clearance errors.
- Top and bottom rails with three tooling holes and three fiducials. JLCPCB
  assembly needs these on very small boards; bare-board orders ignore them.
- The rail text is only the board name. Order these panels with JLCPCB's
  "remove order number" option. To let JLCPCB print its number on the rail
  instead, add "JLCJLCJLCJLC" to the text below and pick "Specify a
  location" when you order; with the remove option, those letters would
  print as plain text.
- Size check: a 3 x 4 grid of 22.6 x 17.1 mm boards with rails is about
  72 x 87 mm, inside JLCPCB's cheapest 100 x 100 mm tier. The script
  prints the panel size and warns above 100 mm.
"""
# License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.

import argparse, json, os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
BOARDS = {16: "16pin/jst-adapter-16", 10: "10pin/jst-adapter-10"}
PRESET = os.path.join(HERE, "panel-preset.json")
TIER_MM = 100.0   # JLCPCB's cheapest size tier, both sides


def panel_size(pcb_path):
    """Return the (width, height) in mm of the Edge.Cuts bounding box."""
    import pcbnew
    board = pcbnew.LoadBoard(pcb_path)
    box = board.GetBoardEdgesBoundingBox()
    return pcbnew.ToMM(box.GetWidth()), pcbnew.ToMM(box.GetHeight())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--boards", type=int, nargs="+", choices=sorted(BOARDS),
                    default=sorted(BOARDS, reverse=True),
                    help="which boards to panelize (default: both)")
    ap.add_argument("--rows", type=int, help="override the preset's grid rows")
    ap.add_argument("--cols", type=int, help="override the preset's grid columns")
    ap.add_argument("--out", default=os.path.join(HERE, "panel"),
                    help="output directory (default: panel/ next to this script)")
    args = ap.parse_args()

    with open(PRESET) as f:
        preset = json.load(f)
    if args.rows:
        preset["layout"]["rows"] = args.rows
    if args.cols:
        preset["layout"]["cols"] = args.cols
    os.makedirs(args.out, exist_ok=True)

    failed = []
    for n in args.boards:
        src = os.path.join(HERE, BOARDS[n] + ".kicad_pcb")
        name = f"jst-adapter-{n}-panel"
        dst = os.path.join(args.out, name + ".kicad_pcb")
        if not os.path.exists(src):
            sys.exit(f"missing {src}: run gen.py and pcb_gen.py first")
        # Rail text names the board. The boards have no title block, so
        # KiKit's {boardTitle} variable would print nothing.
        preset["text"]["text"] = f"jst-adapter-{n}"
        # KiKit takes the preset as a file, so write the per-board copy to
        # a temp file instead of editing panel-preset.json.
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as tmp:
            json.dump(preset, tmp, indent=2)
            preset_path = tmp.name
        cmd = [sys.executable, "-m", "kikit.ui", "panelize",
               "--preset", preset_path, src, dst]
        print(f"== {name}: {preset['layout']['rows']} rows x "
              f"{preset['layout']['cols']} cols")
        try:
            ok = subprocess.run(cmd).returncode == 0
        finally:
            os.unlink(preset_path)
        if not ok:
            failed.append(name)
            continue
        # Copy the source project and footprint table so the panel keeps the
        # board's design rules (track width, clearance, JLCPCB silk minimums)
        # and finds the maddie library. panel/ sits at the same depth as
        # 16pin/ and 10pin/, so the table's ${KIPRJMOD}/../../lib path works.
        srcdir = os.path.dirname(src)
        shutil.copyfile(os.path.join(HERE, BOARDS[n] + ".kicad_pro"),
                        os.path.join(args.out, name + ".kicad_pro"))
        shutil.copyfile(os.path.join(srcdir, "fp-lib-table"),
                        os.path.join(args.out, "fp-lib-table"))
        w, h = panel_size(dst)
        note = "" if max(w, h) <= TIER_MM else \
            f"  WARNING: over {TIER_MM:.0f} mm, so not in the cheapest tier"
        print(f"   {dst}\n   panel {w:.1f} x {h:.1f} mm{note}")

    if failed:
        sys.exit("panelize failed for: " + ", ".join(failed))


if __name__ == "__main__":
    main()
