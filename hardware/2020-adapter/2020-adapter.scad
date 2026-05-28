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
//
// Overall cross-section (Y-Z plane, looking down +X):
//
//                  +---+  +---+           <-- z = total_h  (shelf top)
//                  |   +--+   |               retaining lip (strip_lip thick)
//                  |  strip   |           <-- z = body_thickness + strip_height
//                  |  cavity  |               (strip_height tall, strip_width wide)
//                  |          |
//      +-----------+----------+--------+  <-- z = body_thickness
//      |                               |       (body_thickness tall, body_width wide)
//      |         shelf / body          |
//      +-------------+ +---------------+  <-- z = 0   (extrusion outer face)
//                    | |
//                    | |     stem            slot_lip_depth
//                   /   \
//                  /     \   chamfer         slot_chamfer_depth
//                 +-------+
//                 |       |  bottom square   slot_bottom_depth
//                 +-------+

// ---- parameters ----------------------------------------------------------

length              = 30;  // rail length. My printer can only print 100mm wide so
                              // 84HP = 128.5; 104HP = 158.75; etc.

// 2020 slot profile (generic Misumi-style tapered T-slot).
// The slot cross-section from the outer face inward is:
//   1. straight opening between the lips      (slot_lip_depth deep, slot_opening wide)
//   2. tapered chamfer flaring outward         (slot_chamfer_depth deep)
//   3. square cavity at the bottom             (slot_bottom_depth deep, slot_inner_width wide)
//
// Slot cross-section (the tab matches this exactly, minus tab_clearance):
//
//   =====+         +=====   <-- outer face of extrusion (z = 0)
//        | stem    |             |
//        |         |             | slot_lip_depth     width = slot_opening
//        |         |             |
//       /           \            |
//      /  chamfer    \           | slot_chamfer_depth
//     /               \          |
//    +-----------------+         |
//    |                 |         | slot_bottom_depth  width = slot_inner_width
//    |  bottom square  |         |
//    +-----------------+         |
//
slot_opening        = 6.2;    // gap between outer lips (+0.2 clearance)
slot_lip_depth      = 1.8;    // depth of the straight opening between lips
slot_chamfer_depth  = 1.5;    // depth of the tapered transition (lips flaring out)
slot_bottom_depth   = 2.5;    // depth of the square cavity behind the chamfer
slot_inner_width    = 11.6;   // width of the square cavity (at bottom of chamfer)

// Tab geometry
use_t_tab           = true;   // true = full T-profile matching the tapered slot
                              // (stem + chamfer + bottom square).
                              // false = simple rectangular stem only.
tab_clearance       = 0.3;    // shrink applied to head dimensions

// Main body sitting on the extrusion's outer face
body_width          = 16.0;   // Y extent of the shelf
body_thickness      = 3.0;    // Z thickness below the strip channel

// Eurorack threaded-strip channel (captive C-channel, opens toward +Z).
//
// Strip channel cross-section:
//
//        +---+            +---+    <-- z = total_h            (shelf top)
//        |lip|            |lip|         | strip_lip
//        |   +------------+   |    <-- z = bt + strip_height  (cavity top, lip bottom)
//        |                    |         |
//        |   strip cavity     |         | strip_height
//        |                    |         |
//        +--------------------+    <-- z = body_thickness     (cavity bottom)
//        <---- strip_width ---->
//             <- access_slot_width ->     (cut down through the lip)
//
// The retaining shoulder in Y on each side is (strip_width - access_slot_width)/2.
strip_width         = 5.6;    // Y width of the strip slot
strip_height        = 3.0;    // Z height of the strip slot (was 2.6)
strip_lip           = 0.8;    // Z thickness of the top retaining lip.
                              // The access slot is cut through this lip; the
                              // lip overhang in Y is (strip_width-access_slot_width)/2.
strip_y_offset      = 3.0;    // Y center of the channel relative to body center

// Screw-access slot cut through the top face above the strip
access_slot_width   = 3.4;    // clearance for M3 (use 2.9 for M2.5)

// Corner rounding (applied to the shelf and the strip cavity; not to the tab,
// which must keep sharp corners to match the slot). Reduces stress at the
// retaining lip and makes the strip slide more smoothly. Set to 0 to disable.
fillet              = 0.3;

$fn = 64;

// ---- derived -------------------------------------------------------------

tab_stem_w    = slot_opening - 0.2;
tab_head_w    = slot_inner_width - 2*tab_clearance;
tab_chamfer_h = slot_chamfer_depth;                         // no clearance: chamfer matches slot angle
tab_bottom_h  = slot_bottom_depth - tab_clearance;          // a hair shy so it bottoms-out cleanly
total_h       = body_thickness + strip_height + strip_lip;  // shelf top above extrusion face
                                                            // (lip sits above the strip cavity)

// ---- 2D cross-section (in the Y-Z plane) ---------------------------------

// A rectangle of bounding-box `size` with corners rounded to radius `r`,
// anchored at the origin like square(). Built as hull() of four corner
// circles -- same trick as ServerNinja's extrusion library. r=0 falls
// back to a plain square so the helper is always safe to use.
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
        // Tapered T tab matching a Misumi-style 2020 slot:
        //   stem (between lips) -> chamfer (flares out) -> square bottom.
        // Z=0 is the top of the tab (flush with the underside of the shelf);
        // the tab extends in -Z.
        //
        // Polygon vertices (numbered in winding order):
        //
        //          0 +-------+ 1            <-- z = 0
        //            |       |
        //            |       |              <-- stem (width = tab_stem_w)
        //            |       |
        //          7 +       + 2            <-- z = z_stem_bot
        //           /         \
        //          /           \            <-- chamfer
        //         /             \
        //      6 +---------------+ 3        <-- z = z_chamfer_bot
        //        |               |
        //        |               |          <-- bottom (width = tab_head_w)
        //        |               |
        //      5 +---------------+ 4        <-- z = z_bottom
        z_stem_bot    = -slot_lip_depth;
        z_chamfer_bot = z_stem_bot - tab_chamfer_h;
        z_bottom      = z_chamfer_bot - tab_bottom_h;
        polygon(points = [
            [-tab_stem_w/2, 0],            // 0
            [ tab_stem_w/2, 0],            // 1
            [ tab_stem_w/2, z_stem_bot],   // 2
            [ tab_head_w/2, z_chamfer_bot],// 3
            [ tab_head_w/2, z_bottom],     // 4
            [-tab_head_w/2, z_bottom],     // 5
            [-tab_head_w/2, z_chamfer_bot],// 6
            [-tab_stem_w/2, z_stem_bot],   // 7
        ]);
    } else {
        // Simple rectangular stem only (no engagement with the slot's flared cavity).
        translate([-tab_stem_w/2, -slot_lip_depth])
            square([tab_stem_w, slot_lip_depth]);
    }
}

module rail_profile_2d() {
    // Carve the strip cavity (full width, stops below the lip) and the access
    // slot (narrower, cut through the lip down to the cavity). What remains
    // between them on each side is the retaining lip.
    difference() {
        union() {
            // Shelf: rounded rectangle so the outer corners aren't sharp.
            translate([-body_width/2, 0])
                rounded_rect_2d([body_width, total_h], fillet);
            tab_profile_2d();
        }
        // Strip cavity: rounded so the inside corners (especially where the
        // lip meets the side wall) don't stress-concentrate, and so the strip
        // slides without catching.
        translate([strip_y_offset - strip_width/2, body_thickness])
            rounded_rect_2d([strip_width, strip_height], fillet);
        // Access slot: kept square -- it's small and just punches through the
        // lip, so fillets here would eat into the lip thickness.
        translate([strip_y_offset - access_slot_width/2,
                   body_thickness + strip_height])
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
