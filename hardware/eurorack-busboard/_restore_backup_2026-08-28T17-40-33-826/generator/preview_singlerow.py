"""Renders a top view of the single-row board to board-preview-singlerow.png.

Not part of the fab output - it exists so the layout can be eyeballed. Numeric
checks catch clearance and connectivity; they do not catch "the input block
ended up in a silly place".
"""
# License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, Circle
import pcbgen_singlerow as M

COL = {"F.Cu": "#c0392b", "B.Cu": "#2471a3", "In1.Cu": "#7f8c8d", "In2.Cu": "#27ae60"}
BAND = {"+5V": ("#f39c12", M.BAND_P5_A), "+12V": ("#e74c3c", M.BAND_P12_A),
        "-12V": ("#8e44ad", M.BAND_N12)}


def draw(ax, x0, x1, label):
    ax.add_patch(Rectangle((0, 0), M.BOARD_W, M.BOARD_H, fc="#0d3b1e", ec="k", lw=1.2))
    for nm, (c, (y0, y1)) in BAND.items():
        ax.add_patch(Rectangle((1.5, y0), M.BOARD_W - 3, y1 - y0, fc=c, alpha=0.16, ec="none"))

    for kind, layers, net, d in M.geo:
        if kind != "seg":
            continue
        layer = "F.Cu" if "F.Cu" in layers else "B.Cu"
        xs, ys, xe, ye, w = d
        ax.plot([xs, xe], [ys, ye], color=COL[layer], lw=max(0.4, w * 1.6),
                solid_capstyle="round", alpha=0.9, zorder=3)

    for kind, layers, net, d in M.geo:
        if kind == "via":
            ax.add_patch(Circle((d[0], d[1]), d[2] / 2, fc="#ecf0f1", ec="#34495e",
                                lw=0.2, zorder=4))
        elif kind == "pad":
            x, y, pw, ph = d
            fc = "#7f8c8d" if net == "__HOLE__" else "#f1c40f"
            ax.add_patch(Rectangle((x - pw / 2, y - ph / 2), pw, ph, fc=fc,
                                   ec="none", alpha=0.95, zorder=4))

    for f in M.footprints:
        r = M.body_rect(f)
        if not r:
            continue
        ax.add_patch(Rectangle((r[0], r[1]), r[2] - r[0], r[3] - r[1],
                               fc="none", ec="#ecf0f1", lw=0.5, alpha=0.75, zorder=5))
        if x1 - x0 < 140:
            ax.text(f.x, r[1] - 0.6, f.ref, color="#ecf0f1", fontsize=3.4,
                    ha="center", va="bottom", zorder=6)

    ax.set_xlim(x0, x1)
    ax.set_ylim(M.BOARD_H + 2, -2)          # KiCad Y is down
    ax.set_aspect("equal")
    ax.set_title(label, fontsize=8)
    ax.tick_params(labelsize=6)


fig, axes = plt.subplots(2, 1, figsize=(17, 7.5))
draw(axes[0], -4, M.BOARD_W + 4,
     f"single-row bus board, {M.BOARD_W:.0f} x {M.BOARD_H:.0f} mm - full board")
draw(axes[1], 160, 258, "input block (x 160..258) - red F.Cu, blue B.Cu")
fig.tight_layout()
fig.savefig("out/board-preview-singlerow.png", dpi=200, facecolor="white")
print("wrote out/board-preview-singlerow.png")
