#!/usr/bin/env python3
"""Generate KiCad panel projects for mod1/mod2 firmwares that lack one.

Clones the cleanest same-form-factor template (mod2-clap for mod2, mod1-dual-ad-env
for mod1 -- both expose every slot we need: title, 3 pots, button, LED, 4 jacks)
and relabels each text slot by nearest position, stripping the stale font
render_cache so KiCad/kicad-cli regenerate it from the string. Labels come from
each firmware's ASCII panel diagram. Pure-graphics panels (no footprints), so
this is safe text surgery.

Non-destructive by default: panels whose files already exist are skipped so a
re-run never clobbers hand-edited KiCad work. Pass --force (-f) to overwrite.
"""
import re, sys, shutil, pathlib

# Each panel lives in its own folder at repo-root panels/<name>/<name>.kicad_pcb.
# This script lives in scripts/panels/tools/, so go up to the repo root then into panels/.
PANELS = pathlib.Path(__file__).resolve().parents[3] / "panels"

def find_block_end(t, start):
    d = 0
    for i in range(start, len(t)):
        if t[i] == '(': d += 1
        elif t[i] == ')':
            d -= 1
            if d == 0: return i + 1
    return -1

def gr_texts(t):
    out = []
    for m in re.finditer(r'\(gr_text ', t):
        s = m.start(); e = find_block_end(t, s); blk = t[s:e]
        sm = re.match(r'\(gr_text "((?:[^"\\]|\\.)*)"', blk)
        am = re.search(r'\(at ([\-\d.]+) ([\-\d.]+)', blk)
        if sm and am:
            out.append((s, e, sm.group(1), float(am.group(1)), float(am.group(2))))
    return out

def strip_render_cache(blk):
    i = blk.find('(render_cache')
    if i < 0: return blk
    e = find_block_end(blk, i)
    # also swallow leading whitespace/newline before the cache
    j = i
    while j > 0 and blk[j-1] in ' \t\n': j -= 1
    return blk[:j] + blk[e:]

def relabel(template, dst_name, slots, force=False):
    """slots: list of (x, y, new_string). Nearest gr_text within 3mm gets it.

    Skips (does not write) if the destination panel already exists, unless force."""
    out_dir = PANELS / dst_name
    pcb = out_dir / f"{dst_name}.kicad_pcb"
    pro = out_dir / f"{dst_name}.kicad_pro"
    if not force and (pcb.exists() or pro.exists()):
        print(f"  {dst_name:22} SKIP (exists; pass --force to overwrite)")
        return
    t = (PANELS / template / f"{template}.kicad_pcb").read_text()
    blocks = gr_texts(t)
    edits = {}  # block_index -> new_string
    for tx, ty, new in slots:
        best, bd = None, 9e9
        for idx, (s, e, st, x, y) in enumerate(blocks):
            d = (x - tx) ** 2 + (y - ty) ** 2
            if d < bd and idx not in edits:
                bd, best = d, idx
        if best is None or bd > 9.0:  # >3mm
            print(f"  WARN {dst_name}: no slot near ({tx},{ty}) for {new!r}")
            continue
        edits[best] = new
    # apply from the end so spans stay valid
    for idx in sorted(edits, key=lambda i: blocks[i][0], reverse=True):
        s, e, old, x, y = blocks[idx]
        blk = t[s:e]
        blk = blk.replace(f'"{old}"', f'"{edits[idx]}"', 1)
        if y < 45:  # title: shrink font so long names fit the 19.8 mm board
            blk = re.sub(r'\(size [\d.]+ [\d.]+\)', '(size 2.1 2.1)', blk)
            blk = re.sub(r'\(thickness [\d.]+\)', '(thickness 0.35)', blk)
        blk = strip_render_cache(blk)
        t = t[:s] + blk + t[e:]
    out_dir.mkdir(exist_ok=True)
    pcb.write_text(t)
    shutil.copy(PANELS / template / f"{template}.kicad_pro", pro)
    print(f"  {dst_name:22} <- {template}")

# Panels are mirror-drawn (B-side), so FRONT-left = high PCB-x, FRONT-right = low
# PCB-x. Diagrams list pairs front-left then front-right, so the left item maps to
# the high-x anchor and the right item to the low-x anchor.

# clap slot anchors (mod2): title, p1,p2,p3, btn, led, I1,I2(top), OUT,CV(bottom).
# Bottom row matches the hand-designed panels + real hardware: OUT bottom-left,
# CV bottom-right (front view). Front-left = high PCB-x (B-silk is mirror-drawn).
def M2(title, p1, p2, p3, btn, led, i1, i2, cv, out):
    return [(65.89,38.89,title),(65.57,59.84,p1),(65.57,78.48,p2),(65.84,96.75,p3),
            (56.78,107.64,btn),(60.45,116.88,led),
            (70.08,134.38,i1),(60.81,134.42,i2),(70.23,146.99,out),(60.92,147.10,cv)]

# dual-ad slot anchors (mod1): title, p1,p2,p3, btn, led, F1,F2(top), F3,F4(bottom)
def M1(title, p1, p2, p3, btn, led, f1, f2, f3, f4):
    return [(65.89,38.89,title),(65.57,59.84,p1),(65.57,78.48,p2),(65.84,96.75,p3),
            (63.50,109.41,btn),(60.45,116.88,led),
            (70.08,134.38,f1),(60.81,134.38,f2),(70.12,146.76,f3),(60.92,147.10,f4)]

B = " "  # blank (N/A)

MOD2 = {
 "mod2-kick":      M2("KICK","Pitch","Clip","Env","param","env","Clock","Accent","CV","Out"),
 "mod2-breakbeats":M2("BREAKBEAT","Speed","Length","Slice","trig/loop","loop","Trig","EOP","CV","Out"),
 "mod2-flux":      M2("FLUX","Freq","Rate","Char","mode",B,B,B,"CV","Out"),
 "mod2-fm-drum":   M2("FM DRUM","Pitch","Ratio","Index","mode","mode","Trig","Accent","CV","Out"),
 "mod2-hihat":     M2("HIHAT","Decay","Curve","Freq","type","env","Trig","Accent","CV","Out"),
 "mod2-acid303":   M2("ACID303","Turing","Decay","Trans","scale","step","Clock","Accent","CV","Out"),
 "mod2-radio":     M2("RADIO","Speed","Dir","Start","trig","trig","Trig","Loop","CV","Out"),
 "mod2-sample":    M2("SAMPLE","Speed","Group","Index","trig","trig","Trig","Sel","CV","Out"),
 "mod2-spiral":    M2("SPIRAL","Freq","Speed","Width","mode","dir",B,B,"CV","Out"),
 "mod2-square-vco":M2("SQ VCO","Tune","Octave","V/Oct","chip","mode",B,B,"CV","Out"),
}
MOD1 = {
 "mod1-butterfly":    M1("BUTTERFLY","Sigma","Rho","Beta","slow","step","Reset","X","Y","Z"),
 "mod1-random-cv":    M1("RANDOM CV","Steps","Level","Prob","update","cv","Clock","Update","CV","Trig"),
 "mod1-tap-tempo":    M1("TAP TEMPO","Mult","Div","Div","tap","1x","4x","Var","Div","Div"),
 "mod1-trigger-burst":M1("BURST","Num","Div","Clock","trig","trig","Clock","Trig","NumCV","Trig"),
}

if __name__ == "__main__":
    force = "--force" in sys.argv[1:] or "-f" in sys.argv[1:]
    if force:
        print("(--force: overwriting any existing panel files)")
    print("mod2 (clap base):")
    for name, slots in MOD2.items():
        relabel("mod2-clap", name, slots, force)
    print("mod1 (dual-ad base):")
    for name, slots in MOD1.items():
        relabel("mod1-dual-ad-env", name, slots, force)
