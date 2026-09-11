"""Renumber references to <prefix><number> so KiCad's annotation checker is happy.
Applied identically to the schematic and the PCB so the two stay linked."""
# License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.

import re, sys

def build_map(n_hdr=20):
    m = {}
    for n in range(1, n_hdr + 1):
        m[f"F{n}A"] = f"F{3*n-2}"      # -12V per-header fuse
        m[f"F{n}B"] = f"F{3*n-1}"      # +12V
        m[f"F{n}C"] = f"F{3*n}"        # +5V
        m[f"C{n}A"] = f"C{2*n-1}"      # +12V decoupling
        m[f"C{n}B"] = f"C{2*n}"        # -12V decoupling
    base = 3 * n_hdr
    m.update({"F_MN12": f"F{base+1}", "F_MP12": f"F{base+2}", "F_MP5": f"F{base+3}"})
    cb = 2 * n_hdr
    m.update({"C_B1": f"C{cb+1}", "C_B2": f"C{cb+2}", "C_B3": f"C{cb+3}"})
    m.update({"J_IN_T": f"J{n_hdr+1}", "J_IN_B": f"J{n_hdr+2}",
              "W_N12": f"J{n_hdr+3}", "W_GND": f"J{n_hdr+4}",
              "W_P12": f"J{n_hdr+5}", "W_P5": f"J{n_hdr+6}"})
    m.update({"D_R1": "D1", "D_R2": "D2", "D_R3": "D3",
              "D_T1": "D4", "D_T2": "D5", "D_T3": "D6",
              "D_L1": "D7", "D_L2": "D8", "D_L3": "D9"})
    m.update({"R_G1": "R1", "R_L1": "R2", "R_L2": "R3", "R_L3": "R4"})
    return m

PATTERNS = [r'(\(reference ")([^"]+)(")',
            r'(\(property "Reference" ")([^"]+)(")',
            r'(\(fp_text reference ")([^"]+)(")']

def apply(path, m):
    t = open(path).read()
    for p in PATTERNS:
        t = re.sub(p, lambda g: g.group(1) + m.get(g.group(2), g.group(2)) + g.group(3), t)
    open(path, "w").write(t)

if __name__ == "__main__":
    m = build_map()
    files = sys.argv[1:] or ["out/eurorack-busboard.kicad_sch",
                             "out/eurorack-busboard.kicad_pcb"]
    for f in files:
        apply(f, m)
    print(f"renumbered {len(m)} references")
