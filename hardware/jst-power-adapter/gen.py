#!/usr/bin/env python3
"""KiCad 10 projects: JST PH 5-pin to Eurorack power adapters, 16-pin and 10-pin.

One generator for both variants. For each variant it writes, into
16pin/ and 10pin/ next to this script:

  jst-adapter-<n>.kicad_sch   schematic (wire stubs + global labels)
  jst-adapter-<n>.kicad_pro   project, 0.5mm tracks / 0.2mm clearance
  sym-lib-table, fp-lib-table the shared maddie library, two levels up
  board_data.json             sidecar for pcb_gen.py (refs, footprints, nets)

Then run pcb_gen.py under KiCad's bundled python to build both boards.
gen.py writes in place and never touches README.md.

The schematic embeds exact copies of the library symbols (Device,
Connector_Generic, and maddie). An exact copy keeps ERC free of
lib_symbol_mismatch warnings.
"""
# License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.

import json, math, os, re, uuid

HERE = os.path.dirname(os.path.abspath(__file__))
KICAD_SYM = "/Applications/KiCad/KiCad.app/Contents/SharedSupport/symbols"
MADDIE_SYM = os.path.normpath(os.path.join(HERE, "..", "lib", "maddie.kicad_sym"))
LIBFILES = {
    "Device": os.path.join(KICAD_SYM, "Device.kicad_sym"),
    "Connector_Generic": os.path.join(KICAD_SYM, "Connector_Generic.kicad_sym"),
    "maddie": MADDIE_SYM,
}

def uid():
    return str(uuid.uuid4())

F = "(effects (font (size 1.27 1.27)))"
FH = "(effects (font (size 1.27 1.27)) (hide yes))"

# ---------------------------------------------------------------- parts
# Diode: SS34 Schottky, 3A 40V, SMA. LCSC C8678 (MDD SS34), checked on
# https://jlcpcb.com/partdetail/SS34/C8678 : 40V, 3A, Vf 550mV @ 3A.
# At module currents (well under 1A) Vf is nearer 0.35V, so the +5V
# output sits at about 4.6V. Hand-solder pads: this is a DIY build.
DIODE = dict(lib_id="Device:D_Schottky", value="SS34",
             fp="Diode_SMD:D_SMA_Handsoldering",
             fields={"LCSC Part #": "C8678", "Manufacturer": "MDD", "MPN": "SS34"})
JST = dict(lib_id="maddie:JST_PH_1x05_Vertical", value="B5B-PH-K-S",
           fp="maddie:CONN-TH_B5B-PH-K-S-LF-SN",
           fields={"LCSC Part #": "C157993", "Manufacturer": "JST", "MPN": "B5B-PH-K-S(LF)(SN)"})
# JLCPCB assembly: the fabrication toolkit (see fabrication-toolkit-options.json)
# reads "LCSC Part #" for the BOM. MPN and Manufacturer are copied verbatim
# from each JLCPCB part page, so a manual BOM upload also matches.
# Bottom connector: a plain female 2.54mm socket, mounted on the BOTTOM
# face. It drops into the module's shrouded box header (or onto a bare
# pin header). LCSC: C30734 (BOOMELE 2x8, 8.5mm), C2977587 (ZHOURI 2x5,
# 8.5mm). C2977587 replaced BOOMELE C30419, which LCSC had no stock of
# (2026-11-11); both fit the stock 2x5 socket footprint.
SOCKET = {
    16: dict(lib_id="Connector_Generic:Conn_02x08_Odd_Even", value="Socket 2x08 2.54mm",
             fp="Connector_PinSocket_2.54mm:PinSocket_2x08_P2.54mm_Vertical",
             fields={"LCSC Part #": "C30734", "Manufacturer": "BOOMELE", "MPN": "2.54-2*8PFemale"}),
    10: dict(lib_id="Connector_Generic:Conn_02x05_Odd_Even", value="Socket 2x05 2.54mm",
             fp="Connector_PinSocket_2.54mm:PinSocket_2x05_P2.54mm_Vertical",
             fields={"LCSC Part #": "C2977587", "Manufacturer": "ZHOURI", "MPN": "2.54-2*5"}),
}

# JST J1 pinout (also on the silk): 1=-12V 2=GND 3=+12V 4=GND 5=+5V. This is
# the bus board's map (hardware/eurorack-v2, J1-J20), so cables are wired
# pin n to pin n. Rev A had pins 1 and 3 the other way round.
J1_NETS = {"1": "-12V_IN", "2": "GND", "3": "+12V_IN", "4": "GND", "5": "+5V_IN"}

# Socket maps, Doepfer numbering. None = deliberate no-connect.
#
# 16-pin: GND is ONLY on pins 3/4, and pins 5-8 are left open. That is the
# rotation guard. If the adapter goes on rotated 180 degrees, adapter pin n
# meets module pin 17-n:
#   adapter +12V (9,10) -> module GND (8,7)
#   adapter +5V (11,12) -> module GND (6,5)
#   adapter GND (3,4)   -> module CV (14,13)
#   adapter -12V (1,2)  -> module Gate (16,15)
#   module -12V/+12V/+5V land on adapter pins 16,15 / 8,7 / 6,5: all open.
# The supply GND never reaches the module GND, so no current has a return
# path and nothing flows. +12V and +5V do meet on the module GND plane; D3
# blocks +12V from back-feeding the +5V output (its cathode is the output).
# This assumes the module leaves bus CV/Gate unconnected, as most do.
SOCKET_NETS = {
    16: {"1": "-12V", "2": "-12V", "3": "GND", "4": "GND",
         "5": None, "6": None, "7": None, "8": None,
         "9": "+12V", "10": "+12V", "11": "+5V", "12": "+5V",
         "13": None, "14": None, "15": None, "16": None},
    # 10-pin: rotation swaps +12V and -12V and no wiring can prevent it.
    # The board carries an edge notch and a silk arrow at the -12V end.
    10: {"1": "-12V", "2": "-12V", "3": "GND", "4": "GND", "5": "GND",
         "6": "GND", "7": "GND", "8": "GND", "9": "+12V", "10": "+12V"},
}

def parts(n):
    """(ref, part, x, y, rot, pin->net) for one variant. Sheet units mm."""
    j1 = dict(J1_NETS)
    if n == 10:
        j1["5"] = None          # the 10-pin bus has no +5V
    p = [
        ("J1", JST, 50.8, 88.9, 0, j1),
        # Pin 1 = K, pin 2 = A (KiCad diode convention). rot 180 draws the
        # anode on the left so current reads left to right.
        ("D1", DIODE, 101.6, 76.2, 180, {"2": "+12V_IN", "1": "+12V"}),
        # -12V: current flows OUT of the module into the -12V supply, so the
        # anode is the module side and the cathode is the JST side.
        ("D2", DIODE, 101.6, 88.9, 180, {"2": "-12V", "1": "-12V_IN"}),
    ]
    if n == 16:
        p.append(("D3", DIODE, 101.6, 101.6, 180, {"2": "+5V_IN", "1": "+5V"}))
    p.append(("J2", SOCKET[n], 152.4, 88.9, 0, SOCKET_NETS[n]))
    return p

NOTES = {
    16: [
        "J1 (JST PH, bus board pinout): 1=-12V  2=GND  3=+12V  4=GND  5=+5V",
        "J2: female 2x8 socket on the BOTTOM face, mates with the module's 16-pin header. Doepfer numbering.",
        "J2 map: 1,2=-12V  3,4=GND  5-8=open  9,10=+12V  11,12=+5V  13-16=open (CV/Gate dropped)",
        "Rotation guard: GND is only on 3/4. Rotated 180 deg, +12V/+5V land on module GND (8..5),",
        "adapter GND lands on CV (14,13), and the module rails land on open pins: no return path,",
        "so no current flows. D3 stops +12V back-feeding the +5V output through the module GND.",
        "D1-D3: series Schottky on every rail, protects against a mis-crimped cable.",
    ],
    10: [
        "J1 (JST PH, bus board pinout): 1=-12V  2=GND  3=+12V  4=GND  5=+5V (not used here)",
        "J2: female 2x5 socket on the BOTTOM face, mates with the module's 10-pin header. Doepfer numbering.",
        "J2 map: 1,2=-12V  3-8=GND  9,10=+12V",
        "Rotation on a 10-pin header swaps +12V and -12V. No wiring can stop that, so the board",
        "has an edge notch and a silk arrow at the -12V end. Match it to the red stripe.",
        "D1-D2: series Schottky on each rail, protects against a mis-crimped cable.",
    ],
}

# ---------------------------------------------------------------- s-expr
def extract_symbol(path, name):
    """Return the exact text of top-level symbol `name` from a .kicad_sym."""
    text = open(path).read()
    m = re.search(r'\n\t?\s*\(symbol "%s"\s' % re.escape(name), text)
    if not m:
        raise SystemExit("symbol %s not found in %s" % (name, path))
    start = text.index("(", m.start())
    depth, i, instr = 0, start, False
    while True:
        c = text[i]
        if instr:
            if c == "\\":
                i += 1
            elif c == '"':
                instr = False
        elif c == '"':
            instr = True
        elif c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
        i += 1

def parse(text):
    """Tiny s-expression reader: lists of atoms; strings lose their quotes."""
    toks = re.findall(r'"(?:\\.|[^"\\])*"|\(|\)|[^\s()"]+', text)
    stack = [[]]
    for t in toks:
        if t == "(":
            stack.append([])
        elif t == ")":
            done = stack.pop()
            stack[-1].append(done)
        else:
            stack[-1].append(t[1:-1] if t.startswith('"') else t)
    return stack[0][0]

def pins_of(tree):
    """pin number -> (x, y, angle, name) in symbol coords (y up)."""
    out = {}
    def walk(node):
        if isinstance(node, list) and node:
            if node[0] == "pin":
                at = next(x for x in node if isinstance(x, list) and x[0] == "at")
                num = next(x for x in node if isinstance(x, list) and x[0] == "number")
                name = next(x for x in node if isinstance(x, list) and x[0] == "name")
                out[num[1]] = (float(at[1]), float(at[2]), float(at[3]), name[1])
                return
            for ch in node:
                walk(ch)
    walk(tree)
    return out

LIBSYM = {}
def libsym(lib_id):
    """(embedded text renamed to lib_id, pin table)."""
    if lib_id not in LIBSYM:
        lib, name = lib_id.split(":")
        txt = extract_symbol(LIBFILES[lib], name)
        if "(extends" in txt:
            raise SystemExit("%s uses extends; embed its parent too" % lib_id)
        embedded = txt.replace('(symbol "%s"' % name, '(symbol "%s"' % lib_id, 1)
        LIBSYM[lib_id] = (embedded, pins_of(parse(txt)))
    return LIBSYM[lib_id]

def rot(x, y, deg):
    """Rotate (x, y) counter-clockwise in symbol coords (y up)."""
    a = math.radians(deg)
    c, s = round(math.cos(a)), round(math.sin(a))
    return x * c - y * s, x * s + y * c

def r2(v):
    return round(v + 0.0, 2)

# ---------------------------------------------------------------- emit
def build(n):
    proj = "jst-adapter-%d" % n
    out = os.path.join(HERE, "%dpin" % n)
    os.makedirs(out, exist_ok=True)
    root = uid()
    body, netmap, nc_pins, board = [], {}, [], []
    used = []
    for (ref, part, x, y, r, nets) in parts(n):
        lib_id = part["lib_id"]
        if lib_id not in used:
            used.append(lib_id)
        _, pins = libsym(lib_id)
        missing = set(pins) - set(nets)
        if missing:
            raise SystemExit("%s: pins %s have no net and no explicit NC" % (ref, sorted(missing)))
        su = uid()
        nc_nets = {}
        fields = {"Reference": ref, "Value": part["value"], "Footprint": part["fp"],
                  "Datasheet": ""}
        fields.update(part["fields"])
        # The tall 2x8 socket symbol needs its ref/value text further out.
        tall = 8.89 if ref == "J2" else 0
        props = ""
        for key, val in fields.items():
            if key == "Reference":
                props += f'    (property "{key}" "{val}" (at {x} {r2(y - 6.35 - tall)} 0) {F})\n'
            elif key == "Value":
                props += f'    (property "{key}" "{val}" (at {x} {r2(y + 6.35 + tall)} 0) {F})\n'
            else:
                props += f'    (property "{key}" "{val}" (at {x} {y} 0) {FH})\n'
        pin_uuids = "".join(f'    (pin "{p}" (uuid "{uid()}"))\n' for p in sorted(pins, key=int))
        body.append(
            f'  (symbol (lib_id "{lib_id}") (at {x} {y} {r}) (unit 1)\n'
            f'    (exclude_from_sim no) (in_bom yes) (on_board yes) (dnp no)\n'
            f'    (uuid "{su}")\n' + props + pin_uuids +
            f'    (instances (project "{proj}" (path "/{root}" (reference "{ref}") (unit 1))))\n'
            f'  )\n')
        for pnum, net in nets.items():
            px, py, pa, pname = pins[pnum]
            px, py = rot(px, py, r)
            sx, sy = r2(x + px), r2(y - py)            # pin endpoint on the sheet
            if net is None:
                # Deliberately open pin. The explicit flag keeps ERC clean
                # and records the decision in the schematic.
                body.append(f'  (no_connect (at {sx} {sy}) (uuid "{uid()}"))\n')
                nc_pins.append(f"{ref}.{pnum}")
                # eeschema's netlist names a lone pin's net like this; the
                # board uses the same name so DRC schematic parity matches.
                # A pin whose name repeats its number (or is blank) drops the name.
                tag = "" if pname in (pnum, "~", "") else pname + "-"
                nc_nets[pnum] = f"unconnected-({ref}-{tag}Pad{pnum})"
                continue
            # The stub runs away from the body: opposite the pin direction.
            dx, dy = rot(-math.cos(math.radians(pa)), -math.sin(math.radians(pa)), r)
            lx, ly = r2(sx + 2.54 * dx), r2(sy - 2.54 * dy)
            ang, just = (180, "right") if dx < 0 else (0, "left")
            body.append(
                f'  (wire (pts (xy {sx} {sy}) (xy {lx} {ly}))\n'
                f'    (stroke (width 0) (type default))\n'
                f'    (uuid "{uid()}")\n'
                f'  )\n')
            body.append(
                f'  (global_label "{net}" (shape passive) (at {lx} {ly} {ang}) (fields_autoplaced yes)\n'
                f'    (effects (font (size 1.27 1.27)) (justify {just}))\n'
                f'    (uuid "{uid()}")\n'
                f'    (property "Intersheetrefs" "${{INTERSHEET_REFS}}" (at {lx} {ly} 0) {FH})\n'
                f'  )\n')
            netmap.setdefault(net, []).append(f"{ref}.{pnum}")
        board.append({"ref": ref, "uuid": su, "fp": part["fp"], "value": part["value"],
                      "fields": part["fields"],
                      "nets": {p: v for p, v in nets.items() if v}, "nc_nets": nc_nets})

    for k, s in enumerate(NOTES[n]):
        body.append(
            f'  (text "{s}" (exclude_from_sim no) (at 25.4 {r2(25.4 + k * 3.81)} 0)\n'
            f'    (effects (font (size 1.7 1.7)) (justify left))\n'
            f'    (uuid "{uid()}")\n  )\n')

    lib = "  (lib_symbols\n" + "".join(libsym(l)[0] + "\n" for l in used) + "  )\n"
    sch = (
        f'(kicad_sch (version 20260306) (generator "eeschema") (generator_version "10.0")\n'
        f'  (uuid "{root}")\n'
        f'  (paper "A4")\n'
        f'  (title_block (title "{proj}: JST PH 5-pin to Eurorack {n}-pin power adapter") (date "2026-11-11") (rev "A")\n'
        f'    (comment 1 "Generated by gen.py - do not hand-edit")\n'
        f'    (comment 2 "J2 is mounted on the bottom face; socket pin n mates with module header pin n")\n'
        f'  )\n'
        + lib + "".join(body) +
        f'  (sheet_instances (path "/" (page "1")))\n'
        f')\n')
    with open(os.path.join(out, proj + ".kicad_sch"), "w") as f:
        f.write(sch)

    # Every power net is the same class: 0.5mm tracks, 0.2mm clearance. The
    # module draws well under 1A, so 0.5mm on 1oz copper has margin. The
    # rules also hold JLC's silk limits (1.0mm text, 0.15mm stroke) so DRC
    # flags any text that would not print.
    pro = {
        "board": {"design_settings": {"rules": {
            "min_clearance": 0.2, "min_track_width": 0.5,
            "min_copper_edge_clearance": 0.5,
            "min_text_height": 1.0, "min_text_thickness": 0.15}}},
        "boards": [],
        "libraries": {"pinned_footprint_libs": [], "pinned_symbol_libs": []},
        "meta": {"filename": proj + ".kicad_pro", "version": 1},
        "net_settings": {
            "classes": [{"bus_width": 12, "clearance": 0.2, "line_style": 0, "name": "Default",
                         "pcb_color": "rgba(0, 0, 0, 0.000)",
                         "schematic_color": "rgba(0, 0, 0, 0.000)",
                         "track_width": 0.5, "via_diameter": 0.8, "via_drill": 0.4,
                         "wire_width": 6}],
            "meta": {"version": 3}, "net_colors": None,
            "netclass_assignments": None, "netclass_patterns": []},
        "pcbnew": {"last_paths": {}, "page_layout_descr_file": ""},
        "schematic": {"legacy_lib_dir": "", "legacy_lib_list": [], "meta": {"version": 1}},
        "sheets": [[root, "Root"]],
        "text_variables": {},
    }
    with open(os.path.join(out, proj + ".kicad_pro"), "w") as f:
        json.dump(pro, f, indent=2)

    # The projects sit two levels below hardware/, hence ../../lib.
    with open(os.path.join(out, "sym-lib-table"), "w") as f:
        f.write('(sym_lib_table\n  (version 7)\n'
                '  (lib (name "maddie")(type "KiCad")(uri "${KIPRJMOD}/../../lib/maddie.kicad_sym")'
                '(options "")(descr "Shared maddie-synths custom symbols"))\n)\n')
    with open(os.path.join(out, "fp-lib-table"), "w") as f:
        f.write('(fp_lib_table\n  (version 7)\n'
                '  (lib (name "maddie")(type "KiCad")(uri "${KIPRJMOD}/../../lib/maddie.pretty")'
                '(options "")(descr "Shared maddie-synths custom footprints"))\n)\n')

    with open(os.path.join(out, "board_data.json"), "w") as f:
        json.dump({"project": proj, "pins": n, "components": board}, f, indent=1)

    print("%s: %d symbols, %d nets -> %s" % (proj, len(board), len(netmap), out))
    for net, pins in sorted(netmap.items()):
        print("  %-8s (%d): %s" % (net, len(pins), ", ".join(pins)))
    print("  NC       (%d): %s" % (len(nc_pins), ", ".join(nc_pins) or "-"))
    problems = ["%s has only %s" % (k, v) for k, v in netmap.items() if len(v) < 2]
    print("  PROBLEMS:", problems if problems else "none - every net has >= 2 pins")
    return not problems


if __name__ == "__main__":
    ok = all([build(16), build(10)])
    raise SystemExit(0 if ok else 1)
