#!/usr/bin/env python3
"""Regenerate every VCV Rack panel from its KiCad faceplate.

For each Rack module it (1) renders rack-plugins/res/<Name>.svg from the mapped
panels/<folder>/<folder>.kicad_pcb via kicad_to_panel.convert(), then (2)
rasterizes that SVG to a PNG preview in panels/renders/<folder>.png via Inkscape
(named by panel folder, e.g. mod2-spiral.png).

Usage:
  regen_res.py                 # regenerate all modules
  regen_res.py Kick Clap       # only the named modules
"""
import sys, subprocess, pathlib
from kicad_to_panel import convert

ROOT = pathlib.Path(__file__).resolve().parents[3]
PANELS = ROOT / "panels"
RES = ROOT / "rack-plugins" / "res"
RENDERS = ROOT / "panels" / "renders"
INKSCAPE = "/Applications/Inkscape.app/Contents/MacOS/inkscape"
PNG_DPI = 200  # ~156 x 1012 px for a 19.8 x 128.5 mm panel

# Rack module name (res/<Name>.svg) -> panel folder (panels/<folder>/<folder>.kicad_pcb)
MAP = {
    "mod1-butterfly": "mod1-butterfly",
    "mod2-claves": "mod2-claves",
    "mod1-eg": "mod1-eg",
    "mod1-dual-ad-env": "mod1-dual-ad-env",
    "mod1-lfo": "mod1-lfo",
    "mod1-euclidean": "mod1-euclidean",
    "mod1-logic-pair": "mod1-logic-pair",
    "mod1-random-cv": "mod1-random-cv",
    "mod1-random-lag": "mod1-random-lag",
    "mod1-trigger-burst": "mod1-trigger-burst",
    "mod1-tap-tempo": "mod1-tap-tempo",
    "mod1-terrain-lfo": "mod1-terrain-lfo",
    "mod2-vco": "mod2-vco",
    "mod2-square-vco": "mod2-square-vco",
    "mod2-clap": "mod2-clap",
    "mod2-hihat": "mod2-hihat",
    "mod2-kick": "mod2-kick",
    "mod2-fm-drum": "mod2-fm-drum",
    "mod2-flux": "mod2-flux",
    "mod2-spiral": "mod2-spiral",
    "mod2-acid303": "mod2-acid303",
    "mod2-breakbeats": "mod2-breakbeats",
    "mod2-sample": "mod2-sample",
}


def to_png(svg: pathlib.Path, png: pathlib.Path):
    subprocess.run(
        [INKSCAPE, str(svg), "--export-type=png",
         f"--export-filename={png}", f"--export-dpi={PNG_DPI}"],
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def main():
    wanted = sys.argv[1:]
    names = wanted or list(MAP)
    bad = [n for n in names if n not in MAP]
    if bad:
        sys.exit(f"unknown module(s): {bad}\nknown: {sorted(MAP)}")
    RENDERS.mkdir(exist_ok=True)
    for name in names:
        pcb = PANELS / MAP[name] / f"{MAP[name]}.kicad_pcb"
        svg = convert(str(pcb), name, RES)          # -> rack-plugins/res/<Name>.svg
        to_png(pathlib.Path(svg), RENDERS / f"{MAP[name]}.png")  # -> panels/renders/<folder>.png
    print(f"done: {len(names)} panel(s) -> res SVG + panels/renders PNG")


if __name__ == "__main__":
    main()
