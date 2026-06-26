#!/usr/bin/env python3
"""Check that every plugin.json module has a panel SVG, an extern declaration,
and an addModel() call. Helper for scripts/check-vcv.fish.

Usage: _check_vcv_reg.py <rack_plugins_dir>
Exit non-zero on any mismatch.
"""
import json
import os
import re
import sys


def main() -> int:
    vcv = sys.argv[1]
    # plugin.json is canonical at the repo root (parent of rack-plugins/);
    # fall back to the build-time copy inside rack-plugins/ if needed.
    plugin_json = os.path.join(os.path.dirname(os.path.abspath(vcv)), "plugin.json")
    if not os.path.exists(plugin_json):
        plugin_json = os.path.join(vcv, "plugin.json")
    hpp = open(os.path.join(vcv, "src", "plugin.hpp")).read()
    cpp = open(os.path.join(vcv, "src", "plugin.cpp")).read()
    extern = set(re.findall(r"extern\s+Model\*\s+(\w+)", hpp))
    added = set(re.findall(r"addModel\(\s*(\w+)\s*\)", cpp))

    modules = json.load(open(plugin_json))["modules"]
    problems = []
    for m in modules:
        slug = m["slug"]
        # convention: createModel<...>("Slug") -> model var is "model" + Slug,
        # but the source may diverge, so check the .cpp for the slug's model line.
        svg = os.path.join(vcv, "res", slug + ".svg")
        if not os.path.exists(svg):
            problems.append(f"{slug}: missing res/{slug}.svg")
        # find the model variable registered to this slug in any module .cpp
        model_var = None
        for fn in os.listdir(os.path.join(vcv, "src")):
            if not fn.endswith(".cpp"):
                continue
            txt = open(os.path.join(vcv, "src", fn)).read()
            mm = re.search(r"(\w+)\s*=\s*createModel<[^>]+>\(\"" + re.escape(slug) + r"\"\)", txt)
            if mm:
                model_var = mm.group(1)
                break
        if not model_var:
            problems.append(f'{slug}: no createModel<...>("{slug}") found in any src/*.cpp')
            continue
        if model_var not in extern:
            problems.append(f"{slug}: {model_var} missing `extern` in plugin.hpp")
        if model_var not in added:
            problems.append(f"{slug}: {model_var} missing `addModel()` in plugin.cpp")

    if problems:
        for p in problems:
            print("  FAIL " + p)
        return 1
    print(f"  ok   {len(modules)} modules: each has SVG + extern + addModel")
    return 0


if __name__ == "__main__":
    sys.exit(main())
