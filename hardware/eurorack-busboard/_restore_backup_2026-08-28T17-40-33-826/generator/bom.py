"""Writes the grouped bill of materials, BOM.csv.

Usage: bom.py [path/to/board.kicad_pcb] [output-suffix]

Unlike jlc-bom.csv this lists every fitted item including the mechanical ones
(mounting holes, wire pads, the through-hole connectors), so it is the sheet to
order from by hand. Validated by regenerating the staggered board's shipped
BOM.csv byte-for-byte.
"""
# License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.

import re, csv, collections, sys
import parts

PCB = sys.argv[1] if len(sys.argv) > 1 else "out/eurorack-busboard.kicad_pcb"
SUFFIX = sys.argv[2] if len(sys.argv) > 2 else ""


def read_footprints():
    out = []
    for blk in open(PCB).read().split('  (footprint "')[1:]:
        lib = blk.split('"')[0].split(":")[-1]
        ref = re.search(r'\(fp_text reference "([^"]+)"', blk).group(1)
        val = re.search(r'\(fp_text value "([^"]*)"', blk).group(1)
        out.append((ref, val, lib))
    return out


def refkey(r):
    m = re.match(r"([A-Za-z]+)(\d+)", r)
    return (m.group(1), int(m.group(2))) if m else (r, 0)


def write_bom(fps):
    g = collections.defaultdict(list)
    for ref, val, lib in fps:
        g[(lib, val)].append(ref)
    rows = []
    for (lib, val), refs in sorted(g.items()):
        rows.append([len(refs), val, f"eurorack_bus:{lib}",
                     parts.lcsc(val) or "TBD",
                     " ".join(sorted(refs, key=refkey)), parts.spec(val)])
    path = f"out/BOM{SUFFIX}.csv"
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["Qty", "Value", "Footprint", "LCSC", "References", "Part / spec"])
        w.writerows(rows)
    return path, rows


if __name__ == "__main__":
    path, rows = write_bom(read_footprints())
    tbd = [r[1] for r in rows if r[3] == "TBD"]
    print(f"{path}: {len(rows)} line items, {sum(r[0] for r in rows)} parts")
    print(f"awaiting LCSC numbers for {len(tbd)}: {tbd}")
