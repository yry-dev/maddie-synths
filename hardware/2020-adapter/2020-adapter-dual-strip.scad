// 2020 -> Eurorack threaded-strip adapter (dual-strip version)
//
// Variant of 2020-adapter.scad with captive strip channels on BOTH sides
// of the rail body (mirrored around Y=0).
//
// License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.

// ---- parameters ----------------------------------------------------------

length              = 15;  // rail length. My printer can only print 100mm wide so
                              // 84HP = 128.5; 104HP = 158.75; etc.

// 2020 slot profile (generic Misumi-style tapered T-slot).
slot_opening        = 5.3;    // gap between outer lips (+0.2 clearance)
slot_lip_depth      = 3;    // depth of the straight opening between lips
slot_chamfer_depth  = 2.5;    // depth of the tapered transition (lips flaring out)
slot_inner_width    = 9.1;    // width at the top of the chamfer (just past the lips)
slot_tip_width      = 5.8;    // width of the flat tip at the bottom of the chamfer

// Tab geometry
use_t_tab           = true;   // true = full T-profile matching the tapered slot
                              // false = simple rectangular stem only.
tab_clearance       = 0.3;    // shrink applied to head dimensions

// Main body sitting on the extrusion's outer face
body_width          = 32.0;   // Y extent of the shelf
body_thickness      = 6.0;    // Z thickness below the strip channel

// Eurorack threaded-strip channel (captive C-channel, opens toward +Z).
strip_width         = 5.6;    // Y width of each strip cavity
strip_height        = 3.6;    // Z height of each strip cavity
strip_lip           = 0.8;    // Z thickness of the top retaining lip
strip_y_offset      = 12;    // cavity center offset from body center (mirrored +/-)

// Screw-access slot cut through the top face above each strip
access_slot_width   = 3.4;    // clearance for M3 (use 2.9 for M2.5)

// Side walls that hang down past the extrusion's two side faces, so the
// adapter wraps the rail instead of hanging off the slot tab alone.
use_side_walls      = true;
rail_size           = 25.6;   // extrusion face width and depth (2020)
wall_clearance      = 0.2;    // gap between each wall and the extrusion side
wall_thickness      = 3.0;    // Y thickness of each side wall
wall_depth          = 20.0;   // Z length below the shelf; 20 covers the full side

// Corner rounding (applied to shelf and strip cavities)
fillet              = 0.3;

$fn = 64;

// ---- derived -------------------------------------------------------------

tab_stem_w    = slot_opening - 0.2;
tab_head_w    = slot_inner_width - 2*tab_clearance;
tab_tip_w     = slot_tip_width - 2*tab_clearance;
tab_chamfer_h = slot_chamfer_depth;
total_h       = body_thickness + strip_height + strip_lip;

assert(!use_side_walls
       || body_width/2 >= rail_size/2 + wall_clearance + wall_thickness,
       "body_width is too narrow to carry the side walls");

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

module side_walls_2d() {
    // Overlap the shelf by `fillet` so the walls fuse with the body.
    wall_inner_y = rail_size/2 + wall_clearance;
    for (side = [1, -1])
        translate([side > 0 ? wall_inner_y : -wall_inner_y - wall_thickness,
                   -wall_depth])
            rounded_rect_2d([wall_thickness, wall_depth + fillet*2], fillet);
}

module rail_profile_2d() {
    // Cut mirrored cavities and access slots at +/- strip_y_offset.
    difference() {
        union() {
            translate([-body_width/2, 0])
                rounded_rect_2d([body_width, total_h], fillet);
            tab_profile_2d();
            if (use_side_walls) side_walls_2d();
        }

        for (yoff = [strip_y_offset, -strip_y_offset]) {
            translate([yoff - strip_width/2, body_thickness])
                rounded_rect_2d([strip_width, strip_height], fillet);

            translate([yoff - access_slot_width/2,
                       body_thickness + strip_height])
                square([access_slot_width, strip_lip + 0.1]);
        }
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
