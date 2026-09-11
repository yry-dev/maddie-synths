"""Writes JLCPCB assembly files: jlc-bom.csv and jlc-cpl.csv.

Usage: jlc.py [path/to/board.kicad_pcb] [output-suffix]
Defaults to the staggered board and unsuffixed filenames.
"""
# License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.

import re, csv, subprocess, collections, os, shutil, sys
import parts

PCB = sys.argv[1] if len(sys.argv) > 1 else "out/eurorack-busboard.kicad_pcb"
SUFFIX = sys.argv[2] if len(sys.argv) > 2 else ""
NOPART = {"-"}


def read_footprints():
    p = open(PCB).read()
    out = []
    for blk in p.split('  (footprint "')[1:]:
        lib = blk.split('"')[0]
        ref = re.search(r'\(fp_text reference "([^"]+)"', blk).group(1)
        val = re.search(r'\(fp_text value "([^"]*)"', blk).group(1)
        out.append((ref, val, lib.split(":")[-1]))
    return out


def refkey(r):
    m = re.match(r"([A-Za-z]+)(\d+)", r)
    return (m.group(1), int(m.group(2))) if m else (r, 0)


def write_bom(fps):
    g = collections.defaultdict(list)
    for ref, val, lib in fps:
        if parts.lcsc(val) in NOPART:
            continue
        g[(val, lib)].append(ref)
    rows = []
    for (val, lib), refs in sorted(g.items(), key=lambda x: refkey(x[1][0])):
        rows.append([val, ",".join(sorted(refs, key=refkey)), lib,
                     parts.lcsc(val) or "TBD", parts.spec(val)])
    with open(f"out/jlc-bom{SUFFIX}.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Comment", "Designator", "Footprint", "LCSC Part #", "Spec to search"])
        w.writerows(rows)
    return rows


def positions_from_file():
    """Fallback for when kicad-cli is absent.

    KiCad's pos export writes the footprint origin with Y negated; verified
    row-for-row against the kicad-cli output for the staggered board.
    """
    out = []
    for blk in open(PCB).read().split('  (footprint "')[1:]:
        ref = re.search(r'\(fp_text reference "([^"]+)"', blk).group(1)
        layer = re.search(r'\(layer "([^"]+)"\)', blk).group(1)
        at = re.search(r'\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)', blk)
        x, y, rot = float(at.group(1)), float(at.group(2)), float(at.group(3) or 0)
        out.append({"Ref": ref, "PosX": f"{x:.6f}", "PosY": f"{-y:.6f}",
                    "Side": "top" if layer == "F.Cu" else "bottom",
                    "Rot": f"{rot:.6f}"})
    return out


def write_cpl(fps):
    if shutil.which("kicad-cli") is None:
        recs = positions_from_file()
        return _emit_cpl(fps, recs)
    subprocess.run(["kicad-cli", "pcb", "export", "pos", "-o", "/tmp/pos.csv",
                    "--format", "csv", "--units", "mm", "--side", "both", PCB],
                   check=True, capture_output=True)
    with open("/tmp/pos.csv") as f:
        return _emit_cpl(fps, list(csv.DictReader(f)))


def _emit_cpl(fps, recs):
    skip = {r for r, v, l in fps if parts.lcsc(v) in NOPART}
    rows = []
    if True:
        for rec in recs:
            ref = rec["Ref"]
            if ref in skip:
                continue
            rows.append([ref, rec["PosX"], rec["PosY"],
                         "Top" if rec["Side"].lower().startswith("t") else "Bottom",
                         rec["Rot"]])
    rows.sort(key=lambda r: refkey(r[0]))
    with open(f"out/jlc-cpl{SUFFIX}.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Designator", "Mid X", "Mid Y", "Layer", "Rotation"])
        w.writerows(rows)
    return rows


if __name__ == "__main__":
    fps = read_footprints()
    b = write_bom(fps)
    c = write_cpl(fps)
    tbd = [r[0] for r in b if r[3] == "TBD"]
    print(f"jlc-bom.csv: {len(b)} line items, {sum(len(r[1].split(',')) for r in b)} parts")
    print(f"jlc-cpl.csv: {len(c)} placements")
    print(f"awaiting LCSC numbers for {len(tbd)}: {tbd}")
