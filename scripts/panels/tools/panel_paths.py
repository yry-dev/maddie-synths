#!/usr/bin/env python3
"""Shared panel-folder path resolution.

Panels are grouped by platform under panels/ (they used to be flat):

    panels/hagiwo-mod1/<mod1-*>/      panels/hagiwo-mod2/<mod2-*>/
    panels/blanks/<blank-*hp>/        panels/freemodular/<fm-*>/

A few one-off panels stay flat at panels/<name>/ (rabid-audio-clk, m-power,
3ch-lfo, strides). Every generator still works in FLAT names -- "mod2-clap",
"blank-6hp", "fm-boost" -- so this module is the single place that maps a bare
name to its real (possibly grouped) folder, for BOTH reading an existing panel
and choosing where a new one gets written. Keep the grouping rules here; don't
re-derive `panels/<name>` anywhere else.
"""
# License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.
# The panel artwork these tools handle is licensed separately: see panels/LICENSE.md (CC BY-NC-SA 4.0).

import pathlib

# scripts/panels/tools/ -> repo root is parents[3]
PANELS = pathlib.Path(__file__).resolve().parents[3] / "panels"

# name-prefix -> platform sub-folder under panels/. Order matters only in that
# each prefix is unique; a name matching none is a flat top-level panel.
_GROUPS = (
    ("mod1-",  "hagiwo-mod1"),
    ("mod2-",  "hagiwo-mod2"),
    ("blank-", "blanks"),
    ("fm-",    "freemodular"),
)


def group_for(name):
    """The platform sub-folder a panel of this name belongs in, or None (flat)."""
    for prefix, group in _GROUPS:
        if name.startswith(prefix):
            return group
    return None


def dest_dir(name):
    """Folder a panel named `name` is WRITTEN to (group-aware; need not exist yet)."""
    group = group_for(name)
    return PANELS / group / name if group else PANELS / name


def panel_dir(name):
    """Resolve an EXISTING panel folder for `name`.

    Tries the group-implied location, then the flat location, then any single
    panels/*/<name> match, and returns the first that actually holds the panel's
    .kicad_pcb (or .kicad_pro). Raises FileNotFoundError if none does, so a stale
    name fails loudly instead of silently reading nothing.
    """
    cands = []
    group = group_for(name)
    if group:
        cands.append(PANELS / group / name)
    cands.append(PANELS / name)
    for d in cands:
        if (d / f"{name}.kicad_pcb").exists() or (d / f"{name}.kicad_pro").exists():
            return d
    for sub in sorted(PANELS.glob(f"*/{name}")):
        if (sub / f"{name}.kicad_pcb").exists() or (sub / f"{name}.kicad_pro").exists():
            return sub
    raise FileNotFoundError(
        f"panel {name!r}: no folder under {PANELS} contains {name}.kicad_pcb")


def panel_pcb(name):
    """Path to an existing panel's .kicad_pcb (group-aware)."""
    return panel_dir(name) / f"{name}.kicad_pcb"


def panel_pro(name):
    """Path to an existing panel's .kicad_pro (group-aware)."""
    return panel_dir(name) / f"{name}.kicad_pro"
