// 2020 -> Eurorack threaded-strip adapter
//
// Slides into the slot of a 2020 aluminum extrusion (end-loaded) and exposes
// a captive channel along its length for a Eurorack threaded strip (or a
// row of M2.5/M3 T-nuts sized to the same cross-section).
//
// Print orientation: stand the part on one end (cross-section facing up) for
// best dimensional accuracy on the slot and tab. Alternatively lay it on the
// back face -- the strip-channel lips are a ~1 mm overhang and bridge fine.
//
// Coordinates: X = along the extrusion, Y = across the extrusion face,
// Z = away from the extrusion face. Origin is on the extrusion's outer face
// centerline, at the near end.

// ---- parameters ----------------------------------------------------------

length              = 10;  // rail length. My printer can only print 100mm wide so
                              // 84HP = 128.5; 104HP = 158.75; etc.

// 2020 slot profile (generic European/Misumi-compatible)
slot_opening        = 6.2;    // gap between outer lips (+0.2 clearance)
slot_inner_width    = 11.6;   // width inside the slot, behind the lips
slot_lip_depth      = 1.8;    // Z thickness of the lip (depth of the stem)
slot_inner_depth    = 4.4;    // depth of the cavity behind the lips

// Tab geometry
use_t_tab           = true;   // true = T-profile, locks behind the lips
                              // false = simple rectangle (slides freely)
tab_clearance       = 0.3;    // shrink applied to T-head

// Main body sitting on the extrusion's outer face
body_width          = 16.0;   // Y extent of the shelf
body_thickness      = 3.0;    // Z thickness below the strip channel

// Eurorack threaded-strip channel (captive C-channel, opens toward +Z)
strip_width         = 5.6;    // Y width of the strip slot
strip_height        = 2.6;    // Z height of the strip slot
strip_lip           = 1.0;    // Y overhang on each side retaining the strip
strip_y_offset      = 3.0;    // Y center of the channel relative to body center

// Screw-access slot cut through the top face above the strip
access_slot_width   = 3.4;    // clearance for M3 (use 2.9 for M2.5)

$fn = 64;

// ---- derived -------------------------------------------------------------

tab_stem_w = slot_opening - 0.2;
tab_head_w = slot_inner_width - 2*tab_clearance;
tab_head_h = slot_inner_depth - tab_clearance;
total_h    = body_thickness + strip_height;  // shelf top above extrusion face

// ---- 2D cross-section (in the Y-Z plane) ---------------------------------

module tab_profile_2d() {
    // stem fits between the lips
    translate([-tab_stem_w/2, -slot_lip_depth])
        square([tab_stem_w, slot_lip_depth]);
    if (use_t_tab) {
        translate([-tab_head_w/2, -slot_lip_depth - tab_head_h])
            square([tab_head_w, tab_head_h]);
    }
}

module body_profile_2d() {
    difference() {
        // outer shelf
        translate([-body_width/2, 0])
            square([body_width, total_h]);

        // captive strip cavity: full width strip_width, but a narrower
        // mouth (strip_width - 2*strip_lip) opening to the top.
        translate([strip_y_offset - strip_width/2, body_thickness])
            square([strip_width, strip_height]);

        // access slot through the top face
        translate([strip_y_offset - access_slot_width/2,
                   body_thickness + strip_height - 0.01])
            square([access_slot_width, strip_lip + 0.02]);
    }
}

module rail_profile_2d() {
    // Reconstruct the captive channel by carving the cavity and the access
    // slot separately so the retaining lips remain attached to the shelf.
    difference() {
        union() {
            translate([-body_width/2, 0])
                square([body_width, total_h]);
            tab_profile_2d();
        }
        // strip cavity
        translate([strip_y_offset - strip_width/2, body_thickness])
            square([strip_width, strip_height]);
        // access slot
        translate([strip_y_offset - access_slot_width/2,
                   body_thickness + strip_height - 0.001])
            square([access_slot_width, strip_lip + 0.1]);
    }
}

// ---- 3D ------------------------------------------------------------------

module rail() {
    // Sweep the Y-Z cross-section along +X.
    rotate([90, 0, 90])
        linear_extrude(height = length, convexity = 4)
            rail_profile_2d();
}

rail();

// Uncomment to preview just the cross-section:
//rail_profile_2d();
