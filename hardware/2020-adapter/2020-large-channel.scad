// 2020 adapter with one full-width top cavity.
//
// License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.

// ---- parameters ----------------------------------------------------------

length              = 5;  // rail length. My printer can only print 100mm wide so
                              // 84HP = 128.5; 104HP = 158.75; etc.

// 2020 slot profile (generic Misumi-style tapered T-slot).
slot_opening        = 5.3;    // gap between outer lips (+0.2 clearance)
slot_lip_depth      = 3.6;    // depth of the straight opening between lips
slot_chamfer_depth  = 2.5;    // depth of the tapered transition (lips flaring out)
slot_inner_width    = 9.1;    // width at the top of the chamfer (just past the lips)
slot_tip_width      = 5.8;    // width of the flat tip at the bottom of the chamfer

// Tab geometry
use_t_tab           = true;   // true = full T-profile matching the tapered slot
                              // false = simple rectangular stem only.
tab_clearance       = 0.3;    // shrink applied to head dimensions

// Main body sitting on the extrusion's outer face
body_width          = 27.0;   // Y extent of the shelf
body_thickness      = 3.0;    // Z thickness below the strip channel

// Single full-width top cavity.
strip_width         = body_width - 1.0; // make cavity span the entire face
strip_height        = 1.0;         // Z height of the cavity
strip_lip           = 1.5;         // Z thickness above the cavity

// Full-width access cut through the top face.
access_slot_width   = body_width - 5;

// Corner rounding (applied to shelf and strip cavities)
fillet              = 0.3;

$fn = 64;

// ---- derived -------------------------------------------------------------

tab_stem_w    = slot_opening - 0.2;
tab_head_w    = slot_inner_width - 2*tab_clearance;
tab_tip_w     = slot_tip_width - 2*tab_clearance;
tab_chamfer_h = slot_chamfer_depth;
total_h       = body_thickness + strip_height + strip_lip;

// ---- 2D cross-section (in the Y-Z plane) --------------------------------

module rounded_rect_2d(size, r) {
    if (r <= 0) {
        square(size);
    } else {
        hull()
            for (x = [r, size[0] - r])
                for (y = [r, size[1] - r])
                    translate([x, y]) circle(r);
    }
}

module tab_profile_2d() {
    if (use_t_tab) {
        z_stem_bot    = -slot_lip_depth;
        z_chamfer_bot = z_stem_bot - tab_chamfer_h;
        polygon(points = [
            [-tab_stem_w/2, 0],
            [ tab_stem_w/2, 0],
            [ tab_stem_w/2, z_stem_bot],
            [ tab_head_w/2, z_stem_bot],
            [ tab_tip_w/2,  z_chamfer_bot],
            [-tab_tip_w/2,  z_chamfer_bot],
            [-tab_head_w/2, z_stem_bot],
            [-tab_stem_w/2, z_stem_bot],
        ]);
    } else {
        translate([-tab_stem_w/2, -slot_lip_depth])
            square([tab_stem_w, slot_lip_depth]);
    }
}

module rail_profile_2d() {
    // Cut one centered cavity and one centered top access cut.
    difference() {
        union() {
            translate([-body_width/2, 0])
                rounded_rect_2d([body_width, total_h], fillet);
            tab_profile_2d();
        }

        translate([-strip_width/2, body_thickness])
            rounded_rect_2d([strip_width, strip_height], fillet);

        translate([-access_slot_width/2,
                   body_thickness + strip_height])
            square([access_slot_width, strip_lip + 0.1]);
    }
}

// ---- 3D ------------------------------------------------------------------

module rail() {
    rotate([90, 0, 90])
        linear_extrude(height = length, convexity = 4)
            rail_profile_2d();
}

rail();

// Uncomment to preview just the cross-section:
//rail_profile_2d();
