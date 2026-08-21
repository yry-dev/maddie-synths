# maddie synths — Faceplate Design Rules

Derived by measuring all **51 existing `mod1-*` / `mod2-*` panels** in `panels/`. Every
figure below is a measured value, not a guess; where a convention is not unanimous the
counts are given (e.g. *46/51*). Sections marked **[PROPOSED]** are extrapolations where
the existing panels gave no precedent.

All vertical positions are **`dy` = millimetres below the top board edge**. Every panel is
128.5 mm tall, so `dy` is directly comparable across panels regardless of the KiCad origin
each file happens to use. Do **not** compare raw Y coordinates between files — mod1/mod2
sit at origin y=28.116, the sequencer faceplate at y=43.18.

> **`dy` for text is a BASELINE, not a centre.** Every text item on every mod panel is
> `(justify bottom)` (see §3), so its `(at … y)` is the bottom of the glyphs and the text
> grows *upward*. This is not cosmetic: it is why the rule lines at dy 6.38 and 11.88
> **bracket** the title bar instead of slicing through it. A centred text placed at the
> same dy sits ~half its height too low and collides with the lower rule.

---

## 1. Layer convention — read this first

**mod1/mod2 panels are mounted back-side-out.** All artwork lives on the **back** layers:

| Content | Layer | Count |
|---|---|---|
| All labels, brand, title, icons, graphics | `B.SilkS` | 533 texts + 9104 polys |
| Knockout (inverted) labels | `B.SilkS knockout` | 125 |
| Decorative rule lines | `B.Mask` | 5 per panel |
| `madelyn.sh` signature only | `F.SilkS knockout` | 51 (exactly 1 per panel) |

The rule: **primary artwork on the visible face, `madelyn.sh` alone on the hidden face.**

> ⚠️ The sequencer faceplate currently does the mirror image of this — artwork on
> `F.SilkS`/`F.Mask` and `madelyn.sh` on `B.SilkS`. It is structurally consistent (art on
> one face, signature on the other) but the *opposite face* from the mod1/mod2 family.
> Pick one before fabricating a mixed batch; they cannot be panelised together as-is.

---

## 2. Panel blank

All 51 mod panels are identical stock:

- **19.8 × 128.5 mm** (4 HP × 3 U). Panel centre line at **x = 9.9**.
- Mounting: a **rounded slot** top and bottom, not a round hole.
  - 5.0 mm wide × 3.5 mm tall, corner radius 0.875
  - built from 2 × `gr_line` + 2 × `gr_arc` on `Edge.Cuts`
  - centred at **x = 6.31**, spanning **dy 1.25–4.75** and **dy 123.65–127.15**

### Edge.Cuts encoding
Board outline and all cutouts must be **unfilled outlines** — `(fill no)` with a
**0.1 mm** stroke. Never `(fill yes)`, never zero-width. A filled polygon on `Edge.Cuts`
exports as a Gerber `G36` filled region, which CAM will not read as a routing path.

---

## 3. Typography

**Font: `Comfortaa` throughout** (680 of 709 texts).

The 29 exceptions are *real labels* that were never given a face and silently fall back to
KiCad's default stroke font — `Out` (×4, knockout), `tri`, `sin`, `fm`, `Hits`,
`and nand`, `or\nnor`, `max\nmin`, and several bare numerals. They are inconsistencies to
fix, not a second intentional style. Check the face on every new text item.

| Element | Size | Layer |
|---|---|---|
| Module title | 2.0–2.5 (see §4) | `B.SilkS knockout` |
| `maddie synths` brand | 1.6 | `B.SilkS` |
| Knob / primary control labels | 2.0 | `B.SilkS` |
| Section headers, jack labels | 1.6 | `B.SilkS` |
| Option / switch-position labels | 1.4 | `B.SilkS` |
| `madelyn.sh` | 2.2 | `F.SilkS knockout` |
| `pcb by hagiwo` | 1.4 | `B.SilkS` |

Silk stroke weight must clear the fab minimum. Comfortaa at small sizes with thin
thickness trips KiCad's `text_thickness` DRC warning — set thickness ≥ 0.15 mm and
prefer ≥ 0.2 mm for anything below size 1.6. Every existing mod panel carries 9–14 of
these warnings, so a handful is the family norm rather than a defect.

### Justification — mandatory
Text justification is **not optional styling here**; the vertical grid in §4 depends on it.
Measured across all 709 texts:

| `(justify …)` | Count | Used for |
|---|---|---|
| `bottom mirror` | 554 | ordinary back-layer text |
| `left bottom mirror` | 64 | left-aligned back-layer text |
| `left bottom` | 51 | the one front-layer item per panel |
| `right bottom mirror` | 39 | right-aligned back-layer text |
| `bottom` | 1 | — |

Two rules follow:

1. **Always `bottom`.** No text on any mod panel is vertically centred. `at y` is the
   baseline.
2. **`mirror` iff the text is on a back layer.** Back-layer text without it reads
   backwards — KiCad flags this as `nonmirrored_text_on_back_layer` (`mod1-eg` has one
   such bug today, on its `Out` label).

Because mod panels are back-side-out, nearly everything gets `mirror`. **On a front-side-out
panel the polarity inverts**: `F.SilkS` art takes `bottom` with *no* mirror, and only the
hidden-face `B.SilkS` signature takes `bottom mirror`.

---

## 4. The fixed vertical grid

These positions are **unanimous across all 51 panels** unless noted. Treat them as hard
constraints — this is what makes a rack of these modules line up.

| dy | Element | Layer |
|---|---|---|
| **1.25–4.75** | Top mounting slot | `Edge.Cuts` |
| **5.38** | `maddie synths`, two lines, size 1.6 — *51/51* | `B.SilkS` |
| **6.38** | Rule line 1 | `B.Mask` |
| **10.77** | **Module title**, knockout — *46/51 exactly; range 10.76–10.88* | `B.SilkS knockout` |
| **11.88** | Rule line 2 | `B.Mask` |
| **71.38** | Rule line 3 (section divider) | `B.Mask` |
| **~91.5** | Audio/CV icon row (§6) | `B.SilkS` |
| **94.88** | Rule line 4 (section divider) | `B.Mask` |
| **119.65** | `madelyn.sh`, knockout, size 2.2 — *44/51 exactly* | `F.SilkS knockout` |
| **121.74** | Rule line 5 | `B.Mask` |
| **123.65–127.15** | Bottom mounting slot | `Edge.Cuts` |
| **127.34** | `pcb by hagiwo`, size 1.4 — *51/51* | `B.SilkS` |

### Rule lines
Five horizontal lines on **`B.Mask`**, stroke width **0.6 mm** (255/255 — unanimous).
They are mask *openings*: they print as bare-FR4 stripes, not silkscreen. Lines 1 and 2
bracket the title bar; 3 and 4 divide the control area from the I/O area.

They deliberately **overrun the board edges** so the stripe reaches the panel edge with no
sliver of mask left at the end. The overhang is not symmetric — measured x-spans, relative
to the left board edge on a 19.8 mm panel (all 51/51):

| dy | x span | Overhang L / R |
|---|---|---|
| 6.38 | −0.34 → 19.97 | 0.34 / 0.17 |
| 11.88 | −1.01 → 20.32 | 1.01 / 0.52 |
| 71.38 | −1.01 → 20.32 | 1.01 / 0.52 |
| 94.88 | −1.01 → 20.32 | 1.01 / 0.52 |
| 121.74 | 0.04 → 21.37 | −0.04 / 1.57 |

The bottom line is the odd one out: it starts 0.04 mm *inside* the left edge and overruns
the right by 1.57 mm. Consistent across all 51 panels, so it is reproduced rather than
corrected — but it looks like drift rather than intent. **[PROPOSED]** for new panels,
standardise on the −1.01 → 20.32 span.

### Module title
- Always **knockout** — white text in a filled black box.
- The knockout is a **box hugging the text plus a small margin, not a full-width bar.**
  On a 4 HP panel a well-sized title nearly fills the width so it *reads* as a bar; on a
  wider panel it will visibly be a box. That is correct — see `mod1-butterfly`.
- **Size is chosen to fit the panel width**, not fixed: 2.1 (29 panels), 2.5 (13),
  2.3 (4), 2.0 (2), 2.2/2.25/2.4 (1 each). Use the **largest size ≤ 2.5 that fits**,
  where "fits" means the knockout box clears the panel edges *and* any cutout at title
  height.
- 6 of 51 (the older ones) pad the string with underscores — `_       LFO       _` — to
  force the box out to full width. **Deprecated; do not use on new panels** — it is what
  puts `mod1-lfo` and `mod2-vco` into `silk_edge_clearance` DRC violation, because the
  padded box overruns the board outline.

### Brand line
`maddie synths` set as **two lines** (`maddie\nsynths`) on every panel, because 4 HP is
too narrow for one. **[PROPOSED]** On panels ≥ 8 HP a single line is fine, but keep the
**first** line's baseline at dy 5.38 so it matches a 4 HP module beside it in the rack.

---

## 5. Controls and cutouts

Measured from `mod1-lfo` (representative):

| Feature | ⌀ | Position |
|---|---|---|
| Potentiometer / encoder | **8.0** | on centre line, x 9.77 |
| Momentary button | **4.5** | x 14.61, dy 78.57 |
| Indicator LED | **3.5** | x 14.46, dy 87.92 |
| Jack (Thonkiconn) | **6.2** | two columns, x **5.09** and **14.49** |

Jacks are laid out on a **2-column grid**, rows at dy 99.3 and 112.28 (≈13 mm pitch).

### Dial tick rings
Continuous pots get a **13.55 × 13.55 mm** tick-mark ring (60 polys) centred on the shaft
hole. **Rotary/selector switches do not** — they get discrete position labels around the
knob instead (e.g. `sq` `saw` `tri` `fm` `sin` `fm2` at size 1.4). On `mod1-lfo` the two
continuous pots have rings; the 6-position selector between them does not.

Source artwork: `assets/Knob_Dials_WBG.png`.

---

## 6. I/O labelling — the semantic rules

### Outputs → knockout label
An output jack's label is **inverted (knockout)**: white text on a filled black box.
Observed: `Out` (46 panels), plus `X`, `Z`, `EoC` on multi-output modules. Typically at
dy ≈ 118.9.

### Inputs → plain label + right-angle arrow
An input jack's label is **plain silkscreen** (no knockout) and is accompanied by a
right-angle arrow glyph pointing **into** the jack.

- Asset: **`assets/arrow-turn-right.svg`** (Heroicons v2, 24×24 solid, single path)
- Rendered size: **1.91 × 1.74 mm**
- Placement: offset **+3.7 mm horizontally and +2.2 mm below** the jack centre, i.e.
  tucked into the jack's lower-right as viewed
- Because it sits on `B.SilkS` it is mirrored, so the source `arrow-turn-right`
  **reads as a turn-left arrow** on the finished panel. This is intentional — the arrow
  must point at the jack.

Panels carry 3 input arrows (36 panels), 2 (8), 1 (6), 0 (1).

### Signal-level icon pair
Every panel carries a **two-glyph row at dy ≈ 91.5**, under the output label, declaring
what the module's output actually is. All four glyphs are **Heroicons v2, 24×24 solid**,
already vectorised in `assets/`:

| Meaning | Asset | Appearance | Polys |
|---|---|---|---|
| Outputs **audio** level | `assets/speaker-wave.svg` | speaker + 2 sound arcs | 3 |
| Does **not** output audio | `assets/speaker-x.svg` | speaker with **✕** replacing the arcs | 2 |
| Outputs **CV** level | `assets/bolt.svg` | plain lightning bolt | 1 |
| Does **not** output CV | `assets/bolt-slash.svg` | bolt cut by a diagonal slash | 3 |

**Note the two negatives are drawn differently** — the speaker uses a *mute ✕*, the bolt
uses a *strike-through slash*. That is the Heroicons house style; keep it rather than
inventing a matching pair.

Layout: **speaker on the left, bolt on the right** as viewed. Row is right-aligned under
the `output` label. Total row width ≈ 5.3–6.0 mm, glyph height ≈ 2.2–2.8 mm.

Both slots are **always populated** — you never omit an icon, you show its negative form.

#### Observed usage
| Combination | Panels | Meaning |
|---|---|---|
| `speaker-wave` + `bolt-slash` | 40 (all mod2) | audio out, not CV |
| `speaker-x` + `bolt` | 10 (mod1) | CV out, not audio |
| `speaker-wave` + `bolt` | 1 (`mod1-lfo`) | both — LFO has a 10× fast range reaching audio rates |

---

## 7. Section headers

Sub-areas of the panel get a **knockout header bar**, size 1.6, e.g. `frequency range`
(dy 74.79), `engine select` (dy 74.12) — placed just below the `B.Mask` rule at 71.38.

---

## 8. Attribution

- `pcb by hagiwo` at dy 127.34, size 1.4 — present on **51/51** panels. These designs
  derive from HAGIWO's work; keep the credit on any panel that does.
- `madelyn.sh` knockout at dy 119.65 on the hidden face.

---

## 9. Conformance of `hardware/sequencerv2/sequencer_faceplate_pcb`

These rules have been **applied** to that panel. State after the pass:

| Item | Spec | This panel | |
|---|---|---|---|
| `maddie synths` | dy 5.38 baseline | dy 5.38, `bottom` | ✓ |
| Module title | dy 10.77, knockout, ≤ 2.5 | dy 10.77, knockout, **2.2** | ✓ largest that fits |
| `madelyn.sh` | dy 119.65, knockout, hidden face | dy 119.65, knockout, `B.SilkS` | ✓ |
| Rule lines | 0.6 mm, `−1.01 → +0.52` overhang | 4 lines, correct overhang | ◐ see below |
| Output labels | knockout | `1`–`6` knockout | ✓ |
| Input arrow | `arrow-turn-right`, +3.7 / +2.2 from jack | on `CLK IN` | ✓ |
| Signal icon row | always present | `speaker-x` + `bolt` | ✓ CV/gate, not audio |
| Font | Comfortaa | Comfortaa | ✓ |
| Justification | `bottom`, mirror iff back layer | correct for front-out | ✓ |
| Edge.Cuts encoding | unfilled, 0.1 mm | unfilled, 0.1 mm | ✓ |
| Panel size | — | 40.3 × 128.5 (8 HP) | ✓ correct 3 U height |
| `pcb by hagiwo` | dy 127.34 | absent | n/a — not a hagiwo design |

**DRC: 4 violations, all `text_thickness`** — within the family norm of 9–14.

### Two deliberate deviations

**Only 4 rule lines, not 5.** The header pair (6.38, 11.88) and footer (121.74) are exact.
The mod dividers at **71.38 and 94.88 are unusable on this panel** — they land on the
`CLK IN` label and on the `3`/`4` output labels respectively. They are replaced by a
single adapted divider at **dy 56.00**, separating the screen/encoder control block from
the I/O block. There is no legal position for a second divider: the gap between the
`CLK IN` label and the first LED row is 0.6 mm.

**Icon row is centred at dy 116.5, not right-aligned at dy 91.5.** The spec position falls
between the second LED row and second jack row here. It is moved below the last jack row,
where it is clear of both the jacks (1.55 mm) and the footer rule.

### Still open — artwork face (sequencer)
This panel is **front-side-out** (`F.SilkS`/`F.Mask`), the mirror of the mod1/mod2
back-side-out convention. It was left that way deliberately: flipping it is **not** a
layer rename. The cutouts are not left/right symmetric — the LED holes sit 3.81 mm left of
centre while their jacks are centred — so physically flipping the board moves every
asymmetric hole to the wrong side. Converting would mean mirroring all `Edge.Cuts`
geometry about x = 71.585 as well, and that geometry is currently verified to match
`sequencer_front_pcb` exactly. Decide before panelising these together.

---

## 10. The `fm-*` family — free-modular kits in house dress

`panels/fm-*` are twelve plates (2–10 HP) for the [free-modular](https://freemodular.org)
open-hardware modules, generated by `scripts/panels/tools/make_fm_panels.py`. They
are the first panels here that are **not** 4 HP mod1/mod2 stock, so they are where
this document's width-dependent `[PROPOSED]` clauses got exercised.

**The split.** Mechanics are theirs, artwork is ours. Every cutout keeps the
original coordinate *and diameter* — jack ⌀6.3 (not our 6.2), pot ⌀7.3 (not 8.0),
LED ⌀5.1/3.1, button ⌀5.1, encoder ⌀9.3, 1/4" jack ⌀9.5, plus a rectangular OLED
window — because the plate has to fit their PCB. Everything in §§1, 3, 4, 6, 7, 8
is applied unchanged.

**Widths** follow their formula `floor(hp × 5.08 × 10 − 3)/10`: 9.8 / 20.0 / 30.1 /
40.3 / 50.5 mm for 2/4/6/8/10 HP. That agrees with our own stock at every size but
6 HP (30.1 vs the 30.0 in `make_blanks.py`).

**Back-side-out is preserved**, and this is the resolution of the question left
open for the sequencer in §9: since the artwork face is fixed by the family, the
*geometry* is what moves. Every cutout is mirrored into KiCad space
(`x_kicad = W − x_source`) so that flipping the finished plate restores the
original layout. Mirroring is therefore free here — unlike the sequencer, whose
Edge.Cuts is verified against a companion PCB.

### Width-dependent rules, now settled
| Item | Rule |
|---|---|
| Mounting | rounded slot (§2) at x 6.31, plus a second at W−6.31 for **hp ≥ 6**. Below 4 HP the 8.5 mm stadium does not fit the face, so 1–3 HP gets a ⌀3.2 round hole at free-modular's own off-centre position, leaving a strip for the brand |
| Rule lines | span **−1.01 → W+0.52**, i.e. the §4 [PROPOSED] overhang, footer included |
| Brand | single-line `maddie synths` at **hp ≥ 8**, stacked below that, shrinking only where a mounting cutout leaves no room beside it (2 HP) |
| Title | unchanged: largest size ≤ 2.5 whose knockout box clears the edges and any cutout at title height. No underscore padding |
| `madelyn.sh` | shrinks below 2.2 only when the face is narrower than the string |

### Deviations, and why they are deliberate
- **Divider lines are advisory, and yield to labels.** They are placed in two
  passes: first into any gap with 5 mm of air either side (before labels exist,
  so the wide-open panels keep the family's three-band structure), then — for
  whichever line found no such gap — one more attempt at 2.4 mm *after* every
  label is down, so it has to clear the text too. A line that still does not fit
  is dropped: Clock, Mixer, Offset/Atten and the Output VU ladder carry none.
  The ordering matters. Placing dividers first made whole rows flip their labels
  above/below to dodge a stripe (their Clock's outputs alternated 1-2 above, 3-4
  below), and an inconsistent row of labels reads far worse than a missing
  stripe. The header pair and footer are never dropped — they are the identity.
- **Input arrows sit slightly further out.** The §6 offset (+3.7, +2.2) is
  measured against a ⌀6.2 jack; against their ⌀6.3 the glyph corner lands 0.1 mm
  inside the hole and trips `silk_edge_clearance`. The arrow is pushed along the
  same diagonal by the minimum that clears it. On the 2 HP face (9.8 mm wide,
  centred jacks) there is no room for an arrow at all and it is omitted.
- **Attribution reads `pcb by free modular`.** Their hardware is CC-BY-SA 4.0, so
  the credit is a licence condition rather than a courtesy; it takes the
  `pcb by hagiwo` slot at dy 127.34 and degrades to a stacked shorter form rather
  than being dropped. Only the 2 HP plate cannot fit it on the grid line, where it
  moves up to dy ~120.
- **Encoders get no tick ring.** The Clock's knob is an EC11, endless, so it
  follows the §5 selector rule rather than the pot rule.
- **DRC**: zero errors; 5–17 `text_thickness` warnings per panel, the family norm.

---

## 11. `rabid-audio-clk` — the exception: someone else's panel

`panels/rabid-audio-clk` is the plate for the [rabid.audio](https://rabid.audio/projects/synth/clk/)
CLK ("The Count") port, generated by `scripts/panels/tools/make_clk_panel.py`.
**It deliberately breaks almost every rule above, and that is the point.**

The `fm-*` family (§10) is *their mechanics, our artwork*. This one is theirs
twice over. A port should be recognisable as the module it ports; dressing The
Count in Comfortaa and aubergine turns it into one more mod1/mod2 plate, which is
exactly the failure mode this section exists to record.

### What is read from their board

Every hole comes out of `clock/clock/clock.kicad_pcb`, which carries the
faceplate as a group of hole footprints beside the circuit on the same sheet. The
group sits at a flat +60 mm in both axes, so panel-local mm is just `pcb − 60` —
verified against all ten holes and against their own `panel/panel-design.svg`
drill layer, which agrees to 0.02 mm. Cached in
`scripts/panels/tools/rabid-audio-clk-panel.json` (committed), so regeneration
needs neither their checkout nor KiCad. Diameters stay theirs, and not one is a
house size:

| Feature | ⌀ | vs. §5 |
|---|---|---|
| Jack (Thonkiconn) | **8.0** | ours is 6.2 |
| Encoder (EC11) | **9.0** | ours is 8.0 |
| Button | **5.2** | ours is 4.5 |
| LED | **3.1** | ours is 3.5 |
| Mounting | **5.8 × 4.2** slot | ours is 5.0 × 3.5 |

### Which rules it breaks, and why

| Rule | Here |
|---|---|
| §2 blank | 15.2 × 128.5 (3 HP), their width |
| §3 Comfortaa | **Marker Felt** for the title, **American Typewriter** for everything else — their brush-and-slab pairing. Nothing is set level: every label is tilted ~6° |
| §4 vertical grid | **Gone.** Jacks live at the *top* (their `panel/README.md` states it as house style), so there is no dy 5.38 brand, no dy 10.77 title line, no dy 71.38/94.88 dividers |
| §4 rule lines | **None.** No B.Mask stripes at all |
| §6 icon row | **Omitted.** Their panel has no such convention |
| §8 attribution | `rabid.audio` as a bold wordmark at the foot, their placement. No `maddie synths` brand anywhere |
| Colour | **Black ink on bare aluminium** (`#141414` on `#b6b3ae`), not light silk on `#221b22`. `kicad_to_panel.convert()` takes `bg`/`fg`/`edge` for exactly this |

The one house rule kept is §1's split: `madelyn.sh` sits on the hidden face,
where the converter never plots it, so the derived work is still signed at no
cost to the design.

### What replaces them

- **A halftone dot field**, densest beside the display and fading out past the
  encoder — the panel's dominant graphic. ~900 filled `gr_circle`s, carved away
  from every hole and every label that sits in the field.
- **A solid ink slab** framing the display window, with `BPM` knocked out below it.
- **Turned labels**: an 8 mm jack on a 15.2 mm face leaves 2.5–3.2 mm beside it,
  so the jack, CV and TAP labels are set at 90°. Outputs get their solid knockout
  boxes; the input and TAP stay plain, and the two button labels get outlined
  boxes — their own hierarchy, which happens to encode signal direction.
- **The display reads vertically.** Three SM460281N digits on a 10.795 mm pitch
  behind an 8.0 × 32.1 mm window, so `120` runs down the face. The VCV widget
  stacks its digits the same way.

> ⚠️ **Rotated text anchors sideways.** Everything is `(justify bottom)` (§3), so
> `at` is the baseline. Turned 90° that stops being a vertical subtlety and
> becomes a horizontal one: the glyphs grow *off* the anchor, ascent toward −x in
> KiCad space and descent toward +x, plus the knockout box's margin. Anchor a
> turned label on the hole it sits beside and it grows straight into it.

> ⚠️ **Text metrics are per-face.** The §3 em table is fitted to Comfortaa.
> American Typewriter is 0.84 em and Marker Felt 0.75 em — measure a render
> before trusting a width, or a label runs off the board (`SWING` did).

**DRC: zero errors; 4 `silk_edge_clearance`, 1 `silk_overlap`, 1
`text_thickness`.** The edge-clearance four are the ink slab meeting the display
cutout, which is the design — a 0.2 mm gap would print an aluminium halo around
the window. The overlap is `THE` tucked into `COUNT`, likewise deliberate.
