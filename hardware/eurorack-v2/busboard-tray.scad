// eurorack-v2 bus board tray, for one 2020 extrusion
//
// An open tray that holds the m-power bus board (eurorack-busboard-v2,
// 389.5 x 54 mm) on the face of a 2020 extrusion. The board sits on a ledge
// that keeps its through-hole pins off the floor. A small lip along the
// rim covers 1 mm of each board edge, so the board cannot slip or lift out.
// The top stays open for the JST cables.
//
// The board is longer than the MK3S bed, so the tray prints as two halves
// that meet at the board's centre. Each half has one closed end and one open
// end. To assemble:
//   1. Slide the left half onto the left end of the board, under the lips.
//   2. Slide the right half onto the right end until the halves meet.
//   3. Drop T-nuts into the 2020 slot and screw both halves down.
// The board holds the two halves in line; the screws hold them on the rail.
//
// The rim has a notch where parts reach the board edge: J21 (Mini-Fit),
// C43, D3 and D6 on the top edge, and J22 (Mini-Fit) on the bottom edge.
// Both Mini-Fit plugs enter through these notches.
//
// Coordinates: the tray is centred on the origin, X along the board, Y across
// it, Z up from the extrusion face. Board positions below are in KiCad units
// (origin at the board's top-left corner, y down), converted by bx() / by().
//
// Print each half floor-down. No supports: the lip is a 1.4 mm overhang.
//
// License: MIT, Copyright (c) 2026 Madelyn Yeary. See LICENSE.md at the repo root.

include <BOSL2/std.scad>

/* [Part] */
// Which part to show. Export "left" and "right" for printing.
part = "assembled";     // [assembled, left, right]
// Show a ghost of the board in the "assembled" view
show_board = true;

/* [Fit] */
// Gap between the board edge and the tray wall
edge_gap = 0.4;         // [0.2:0.05:0.8]
// Board width the lip covers along each edge
lip_overlap = 1.0;      // [0.6:0.1:1.5]
// Height above the floor of the board's underside (pins are about 3 mm)
seat_h = 5.0;           // [4:0.5:8]
// Width of the ledge the board sits on, from the tray wall
ledge_w = 2.5;          // [1.5:0.5:4]

/* [Structure] */
// Wall thickness
wall = 2.4;             // [1.6:0.4:4]
// Cut weight-saving windows in the floor beside the 2020 contact strip
floor_windows = true;

/* [Rail screws] */
// Screw clearance hole: 5.5 for M5 (2020 T-nuts), 4.5 for M4
screw_d = 5.5;          // [3.4, 4.5, 5.5]
// Screw positions along the board, in KiCad x (mm). Each lies between two JST
// headers, where no pin passes through the board.
screw_x = [37.27, 149.27, 240.77, 352.77];

/* [Hidden] */
$fn = 64;
epsilon = 0.01;
layer_height = 0.2;     // general utility part

BOARD = [389.5, 54, 1.6];               // from Edge.Cuts in the .kicad_pcb
split_x = BOARD.x / 2;                  // KiCad x where the two halves meet

floor_t = 12 * layer_height;            // 2.4 mm
channel_h = BOARD.z + 2 * layer_height; // slot the board edge slides in
lip_t = 6 * layer_height;               // 1.2 mm
chamfer = 0.6;

seat_z = floor_t + seat_h;              // underside of the board
lip_z = seat_z + channel_h;             // underside of the lip

POCKET = [BOARD.x + 2 * edge_gap, BOARD.y + 2 * edge_gap];
OUTER = [POCKET.x + 2 * wall, POCKET.y + 2 * wall, lip_z + lip_t];

// Rim notches, in KiCad x. The ranges are the part outlines plus 1.5 mm.
//   Top edge (KiCad y = 0):     J21 183.3-197.1, C43 202-210, D3 to 215.3,
//                               D6 to 223.5.
//   Bottom edge (KiCad y = 54): J22 183.3-197.1.
notch_top = [181.8, 225.0];
notch_bottom = [181.8, 198.6];

// 2020 face width. The floor windows stay outside this strip plus a margin.
rail_w = 20;
window_margin = 3;
window_len = 30;
window_rib = 6;

// KiCad board coordinates -> tray coordinates.
function bx(x) = x - BOARD.x / 2;
function by(y) = BOARD.y / 2 - y;

echo(str("Tray outer: ", OUTER.x, " x ", OUTER.y, " x ", OUTER.z, " mm"));
echo(str("Each half: about ", OUTER.x / 2, " mm long"));
echo(str("Board underside at z = ", seat_z, " mm, lip underside at z = ", lip_z, " mm"));

module notch(range, side) {
    // side = +1 for the top edge (KiCad y = 0), -1 for the bottom edge.
    // Cuts the rim from the board underside up, through the wall, so the
    // ledge under the board stays whole.
    w = range[1] - range[0];
    translate([bx(range[0]) + w / 2, side * (POCKET.y / 2 + wall / 2), seat_z])
        cuboid([w, wall + 2 * lip_overlap + 2 * edge_gap + 1, OUTER.z],
               anchor=BOTTOM);
}

module floor_windows() {
    inner_y = POCKET.y / 2 - ledge_w;       // inside edge of the ledge
    y0 = rail_w / 2 + window_margin;
    y1 = inner_y - window_margin;
    usable = POCKET.x - 2 * ledge_w - 2 * window_margin;
    n = floor((usable + window_rib) / (window_len + window_rib));
    pitch = window_len + window_rib;
    if (y1 - y0 > 3)
        for (side = [-1, 1])
            xcopies(spacing=pitch, n=n)
                translate([0, side * (y0 + y1) / 2, -epsilon])
                    cuboid([window_len, y1 - y0, floor_t + 2 * epsilon],
                           rounding=2, edges="Z", anchor=BOTTOM);
}

module tray() {
    difference() {
        cuboid(OUTER, chamfer=chamfer, anchor=BOTTOM);

        // Open area inside the ledge, down to the floor.
        up(floor_t)
            cuboid([POCKET.x - 2 * ledge_w, POCKET.y - 2 * ledge_w, OUTER.z],
                   rounding=1, edges="Z", anchor=BOTTOM);

        // Slot the board edge slides in, between the ledge and the lip.
        up(seat_z)
            cuboid([POCKET.x, POCKET.y, channel_h], anchor=BOTTOM);

        // Opening through the lip.
        up(lip_z - epsilon)
            cuboid([BOARD.x - 2 * lip_overlap, BOARD.y - 2 * lip_overlap,
                    lip_t + 2 * epsilon],
                   chamfer=-lip_t / 2, edges=TOP, anchor=BOTTOM);

        notch(notch_top, 1);
        notch(notch_bottom, -1);

        // Rail screws on the board centreline, which is the 2020 centreline.
        for (x = screw_x)
            translate([bx(x), 0, -epsilon])
                cyl(d=screw_d, h=floor_t + 2 * epsilon, anchor=BOTTOM);

        if (floor_windows) floor_windows();
    }
}

module half(which) {
    // The cut is at the board centre, so each half keeps one closed end.
    big = OUTER.x;
    intersection() {
        tray();
        translate([bx(split_x), 0, 0])
            cuboid([big, OUTER.y + 1, OUTER.z * 3],
                   anchor=(which == "left" ? RIGHT : LEFT));
    }
}

module board_ghost() {
    %up(seat_z) cuboid(BOARD, anchor=BOTTOM);
}

if (part == "assembled") {
    // Leave a 1 mm gap at the split so both halves are visible.
    left(0.5) half("left");
    right(0.5) half("right");
    if (show_board) board_ghost();
} else if (part == "left") {
    // One object for the slicer, laid floor-down.
    right(OUTER.x / 4) half("left");
} else if (part == "right") {
    left(OUTER.x / 4) half("right");
}
