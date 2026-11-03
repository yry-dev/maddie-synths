// Eurorack rail riser
//
// Slides into the threaded-strip channel of a standard Eurorack rail and
// carries a new captive channel on top for the threaded strip. Panels then
// rest on the riser's top face, so they sit `total_h` further out than on the
// bare rail (riser_height + strip_height + strip_lip). Use it to gain depth
// behind modules that are too deep for the case.
//
// Angled cases: where the row you raise meets an unraised neighbour at a
// bend toward the user (a curved desk case), the raised panel edges and the
// riser lean out over the neighbour's edge screws. They clear the
// neighbour's panels, but a screwdriver cannot reach those screws. The
// [Case junction] parameters echo the overlap and warn when it covers the
// screw heads.
//
// Coordinates: X = along the rail, Y = across the rail face, Z = away from
// the rail face. Origin is on the rail's outer face centerline, at the near
// end.
//
// Cross-section (Y-Z plane, looking down +X):
//
//           +--+  +--+            <-- z = total_h               (top)
//           |  +--+  |                top retaining lip (strip_lip)
//           |  strip |            <-- z = riser_height + strip_height
//           | cavity |                (holds the threaded strip)
//           |        |
//           |        |            <-- z = riser_height
//           |  solid |                riser (riser_height tall)
//    =======+--+  +--+=======     <-- z = 0  (rail outer face)
//     rail lip |  |  rail lip         neck through the rail's slot
//           +--+  +--+            <-- z = -rail_lip
//           |  slider|                fills the rail's strip channel
//           +--------+            <-- z = -(rail_lip + rail_channel_depth)
//
// The default channel sizes copy the strip cavity in 2020-adapter.scad,
// which already fits the strip. MEASURE YOUR RAIL before printing: the
// rail_* values are estimates, and the rail's slot opening and lip
// thickness differ between makers (Tiptop, Doepfer, Vector, no-name).
//
// Print orientation: lay the part on its side (one Y face on the bed). The
// cavity roofs then bridge a short span, and the neck, which takes the pull
// of the module screws, has its layers running along the load.
//
// License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.

/* [Length] */
// Rail length (mm). 1 HP = 5.08 mm; 84HP = 426.7, so print in segments.
length              = 100;    // [5:1:250]

/* [Rail channel (bottom slider)] */
// Inside width of the rail's strip channel
rail_channel_width  = 5.6;    // [3:0.1:12]
// Inside height of the rail's strip channel, below the lip
rail_channel_depth  = 3.6;    // [1:0.1:6]
// Width of the rail's slot opening between the lips
rail_opening        = 4.0;    // [2.5:0.1:8]
// Thickness of the rail's retaining lip
rail_lip            = 1.5;    // [0.5:0.1:4]
// Gap per side between the slider and the rail (aluminium does not flex)
rail_clearance      = 0.25;   // [0:0.05:0.6]

/* [Riser] */
// Solid height between the rail face and the floor of the new strip cavity.
// Panels lift by riser_height + strip_height + strip_lip.
riser_height        = 4.0;    // [1:0.5:30]
// Y width of the riser body
body_width          = 10.0;   // [6:0.5:30]

/* [Top channel (threaded strip)] */
// Y width of the strip cavity
strip_width         = 5.6;    // [3:0.1:12]
// Z height of the strip cavity
strip_height        = 3.6;    // [1:0.1:6]
// Z thickness of the top retaining lip
strip_lip           = 0.8;    // [0.4:0.1:3]
// Slot through the top lip: 3.4 clears M3, 2.9 clears M2.5
access_slot_width   = 3.4;    // [2:0.1:6]

/* [Case junction] */
// Bend between this row and the unraised row next to it, in degrees.
// Positive = the rows bend toward you (curved desk), 0 = flat case.
junction_angle      = 45;     // [-90:5:90]

/* [Hidden] */
$fn = 64;
epsilon = 0.01;
layer_height = 0.2;           // general utility part
fillet = 0.3;                 // corner break on outer and cavity corners

// Eurorack 3U spacing: 133.35 mm row pitch, 128.5 mm panels, mounting holes
// 3 mm in from the panel edge (122.5 mm apart).
panel_edge_gap   = (133.35 - 128.5) / 2;  // panel edge to the row boundary
hole_inset       = 3.0;                   // panel edge to hole centre
screw_head_r     = 2.75;                  // M3 head

// ---- derived -------------------------------------------------------------

slider_w = rail_channel_width - 2*rail_clearance;
slider_h = rail_channel_depth - rail_clearance;     // clearance at the floor only;
                                                     // the screws pull it up to the lip
neck_w   = rail_opening - 2*rail_clearance;
total_h  = riser_height + strip_height + strip_lip;

echo(str("Slider: ", slider_w, " x ", slider_h, " mm, neck ", neck_w, " mm"));
echo(str("Panels lift by ", total_h, " mm"));

// Overlap at the junction. Work in the raised row's frame: s = distance past
// the row boundary (negative = inside this row), z = height above the rail
// face. Rotating by junction_angle gives the point's position along the
// neighbour's face (t, from the boundary) and in front of it (n).
function along_neighbour(s, z) = s*cos(junction_angle) + z*sin(junction_angle);
function front_of_neighbour(s, z) = -s*sin(junction_angle) + z*cos(junction_angle);

strip_to_boundary = panel_edge_gap + hole_inset;  // strip centre to row boundary
riser_corner      = [-(strip_to_boundary - body_width/2), total_h];
panel_corner      = [-panel_edge_gap, total_h];
screw_head_edge   = strip_to_boundary - screw_head_r; // nearest edge of the
                                                      // neighbour's screw head
riser_t = along_neighbour(riser_corner[0], riser_corner[1]);
panel_t = along_neighbour(panel_corner[0], panel_corner[1]);

echo(str("Junction at ", junction_angle, " deg: riser corner reaches ", riser_t,
         " mm over the neighbour (", front_of_neighbour(riser_corner[0], riser_corner[1]),
         " mm out); panel edge reaches ", panel_t, " mm over (",
         front_of_neighbour(panel_corner[0], panel_corner[1]), " mm out)"));
if (max(riser_t, panel_t) > screw_head_edge)
    echo(str("WARNING: the riser or raised panels cover the neighbour's edge screws",
             " (head edge at ", screw_head_edge, " mm). Mount the neighbour's modules",
             " first, or lower riser_height."));
echo(str("Length = ", length / 5.08, " HP"));

assert(neck_w < slider_w, "the neck must be narrower than the slider, or nothing holds it in the rail");
assert(access_slot_width < strip_width, "the access slot must be narrower than the strip cavity");
assert(body_width - strip_width >= 2, "body_width leaves walls under 1 mm beside the strip cavity");

// ---- 2D cross-section (x = across the rail, y = away from the rail) ------

// A rectangle of bounding-box `size` with corners rounded to radius `r`,
// anchored at the origin like square(). Same helper as 2020-adapter.scad.
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

module riser_profile_2d() {
    difference() {
        union() {
            // Riser body + top channel walls.
            translate([-body_width/2, 0])
                rounded_rect_2d([body_width, total_h], fillet);
            // Neck through the rail's lip. Overlaps the body and slider by
            // epsilon so the union fuses.
            translate([-neck_w/2, -rail_lip - epsilon])
                square([neck_w, rail_lip + 2*epsilon]);
            // Slider in the rail's channel.
            translate([-slider_w/2, -rail_lip - slider_h])
                rounded_rect_2d([slider_w, slider_h], fillet);
        }
        // Strip cavity.
        translate([-strip_width/2, riser_height])
            rounded_rect_2d([strip_width, strip_height], fillet);
        // Screw-access slot through the top lip. Kept square so it does not
        // thin the lip.
        translate([-access_slot_width/2, riser_height + strip_height - epsilon])
            square([access_slot_width, strip_lip + 2*epsilon]);
    }
}

// ---- 3D ------------------------------------------------------------------

module rail_riser() {
    // Sweep the cross-section along +X.
    rotate([90, 0, 90])
        linear_extrude(height = length, convexity = 4)
            riser_profile_2d();
}

// One object for the slicer
rail_riser();

// Uncomment to preview just the cross-section:
//riser_profile_2d();
