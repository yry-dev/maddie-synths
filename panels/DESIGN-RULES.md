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

### Still open — artwork face
This panel is **front-side-out** (`F.SilkS`/`F.Mask`), the mirror of the mod1/mod2
back-side-out convention. It was left that way deliberately: flipping it is **not** a
layer rename. The cutouts are not left/right symmetric — the LED holes sit 3.81 mm left of
centre while their jacks are centred — so physically flipping the board moves every
asymmetric hole to the wrong side. Converting would mean mirroring all `Edge.Cuts`
geometry about x = 71.585 as well, and that geometry is currently verified to match
`sequencer_front_pcb` exactly. Decide before panelising these together.
