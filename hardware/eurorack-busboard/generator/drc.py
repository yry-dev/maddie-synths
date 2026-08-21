"""Runs real KiCad DRC headlessly: fill zones, then WriteDRCReport.

kicad-cli 7.0.11 has no `pcb drc` subcommand (that arrived in KiCad 8), but the
pcbnew Python module exposes both the zone filler and the DRC engine, which is
the same engine the GUI runs.

Usage: drc.py board.kicad_pcb [report.txt]
"""
# License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.

import sys, os, re
import pcbnew

src = sys.argv[1]
report = sys.argv[2] if len(sys.argv) > 2 else "/tmp/drc-report.txt"

board = pcbnew.LoadBoard(src)
print(f"loaded {os.path.basename(src)}: "
      f"{len(board.GetFootprints())} footprints, {len(board.GetTracks())} tracks, "
      f"{board.GetNetCount()} nets, {len(board.Zones())} zones")

# The boards ship with zones defined but unfilled; DRC on unfilled zones would
# miss every copper-pour interaction, so fill first, exactly as pressing B does.
filler = pcbnew.ZONE_FILLER(board)
filler.Fill(board.Zones())
print("zones filled")

ok = pcbnew.WriteDRCReport(board, report, pcbnew.EDA_UNITS_MILLIMETRES, True)
print(f"DRC report written: {ok}")

txt = open(report).read()
counts = {}
for line in txt.splitlines():
    m = re.search(r'\(([a-z_]+)\)\s*$', line.strip())
    if m:
        counts[m.group(1)] = counts.get(m.group(1), 0) + 1

for head in ("violations", "unconnected items", "schematic parity"):
    m = re.search(rf'\*\*.*{head}.*\((\d+)\)', txt, re.I)
    if m:
        print(f"  {head}: {m.group(1)}")

if counts:
    print("  by rule:")
    for k, v in sorted(counts.items(), key=lambda x: -x[1]):
        print(f"     {v:5d}  {k}")
else:
    print("  no rule-tagged lines found")
