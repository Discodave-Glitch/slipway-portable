// Pocket case for the ESP32-S3-USB-OTG board (68 x 40 mm listing). Fully enclosed: openings only for the screen, both USB ends,
// the Type-C debug port, and the sideways-facing buttons (windows in the front/back walls). PLACEHOLDER DIMENSIONS - measure your board
// with calipers and change the numbers in the first block. Two parts: base tray and lid. Print both flat.
// Coordinates: X along the board length (plug end = 0), Y across, Z up. All sizes in mm.
// View/export: open in OpenSCAD (openscad.org), press F6, then File > Export > STL. Set part = "base", "lid" or "both".

part = "both";           // "base", "lid", "both" (both = side by side for printing), "assembled" (preview only)

// ---- measure these on the real board -------------------------------------------------------
pcb_l   = 56;     // board length WITHOUT the male USB-A plug
pcb_w   = 40;     // board width
pcb_t   = 1.6;    // PCB thickness
plug_l  = 12;     // how far the male USB-A plug sticks out of the board's left edge
plug_w  = 12.2;   // male plug width
plug_h  = 4.6;    // male plug height
sock_w  = 14.0;   // female USB-A socket width (host port), right edge
sock_h  = 6.8;    // female USB-A socket height
comp_h  = 7.5;    // tallest part above the PCB top (usually the USB-A socket)
under_h = 3.0;    // clearance under the PCB for pins/solder (also the standoff height)
hole_inset = 3.5; // corner mounting hole centre, distance from board edges
hole_d  = 2.2;    // board mounting hole diameter
screen_x = 16.8; screen_w = 23.0; screen_h = 24.0;   // LCD window: centre x from board left edge, size; centred across Y
btn_top = [10.7, 19.4];           // x of the two buttons on the top edge
btn_bot = [8.2, 19.4, 29.7];      // x of the three buttons on the bottom edge
btn_win_w = 6.0;                  // width of the window in the side wall at each (sideways-facing) button
btn_win_h = 4.6;                  // height of that window, measured from the PCB top surface
cable_w = 18.0; cable_h = 10.5;   // opening at the host end: wide enough for the A-to-C cable's plug body
usbc_x  = 45.8; usbc_w = 10.0; usbc_h = 4.0;          // Type-C debug port on the bottom (Y = 0) edge
// ---- case settings -------------------------------------------------------------------------
clr   = 0.4;      // gap around the board
wall  = 2.0;
floor_t = 1.6;
lid_t = 2.0;
screw_d = 1.8;    // pilot hole for M2 self-tapping screws
post_d  = 5.0;

in_l  = pcb_l + 2*clr;
in_w  = pcb_w + 2*clr;
out_l = in_l + 2*wall;
out_w = in_w + 2*wall;
base_h = floor_t + under_h + pcb_t + comp_h;      // top of cavity
pcb_z  = floor_t + under_h;                       // PCB underside
ox = wall + clr;  oy = wall + clr;                // board origin inside the case
hole_pts = [[hole_inset, hole_inset],[pcb_l-hole_inset, hole_inset],
            [hole_inset, pcb_w-hole_inset],[pcb_l-hole_inset, pcb_w-hole_inset]];

module base() {
  difference() {
    cube([out_l, out_w, base_h]);
    translate([wall, wall, floor_t]) cube([in_l, in_w, base_h]);
    // male plug slot (left wall)
    translate([-1, oy + pcb_w/2 - (plug_w+0.8)/2, pcb_z - 0.4]) cube([wall+2, plug_w+0.8, plug_h+pcb_t+0.8]);
    // host end opening (right wall): the cable's plug body is wider than the socket, so make room for it
    translate([out_l-wall-1, oy + pcb_w/2 - cable_w/2, pcb_z + pcb_t - 0.4]) cube([wall+2, cable_w, cable_h]);
    // windows for the sideways-facing buttons: bottom edge buttons through the front wall, top edge through the back wall
    for (x = btn_bot) translate([ox + x - btn_win_w/2, -1, pcb_z + pcb_t - 0.3]) cube([btn_win_w, wall+2, btn_win_h]);
    for (x = btn_top) translate([ox + x - btn_win_w/2, out_w - wall - 1, pcb_z + pcb_t - 0.3]) cube([btn_win_w, wall+2, btn_win_h]);
    // Type-C debug port opening (front wall)
    translate([ox + usbc_x - usbc_w/2 - 0.4, -1, pcb_z + pcb_t - 0.8]) cube([usbc_w+0.8, wall+2, usbc_h+1.2]);
  }
  // board posts through the corner holes + screw posts
  for (p = hole_pts) translate([ox + p[0], oy + p[1], 0]) {
    cylinder(d=post_d, h=pcb_z, $fn=32);
    cylinder(d=hole_d-0.3, h=pcb_z+pcb_t+1, $fn=24);
  }
}

module lid() {
  difference() {
    translate([0,0,0]) cube([out_l, out_w, lid_t]);
    // LCD window
    translate([ox + screen_x - screen_w/2, oy + pcb_w/2 - screen_h/2, -1]) cube([screen_w, screen_h, lid_t+2]);
  }
  // lip that drops into the opening so the lid cannot slide
  translate([wall+0.2, wall+0.2, -1.2]) difference() {
    cube([in_l-0.4, in_w-0.4, 1.2]);
    translate([1.6,1.6,-1]) cube([in_l-0.4-3.2, in_w-0.4-3.2, 3]);
  }
}

if (part == "base") base();
else if (part == "lid") translate([0,0,lid_t]) rotate([180,0,0]) translate([0,-out_w,0]) lid();
else if (part == "assembled") { base(); translate([0,0,base_h]) lid(); }
else { base(); translate([0, out_w + 8, 0]) translate([0,0,lid_t]) rotate([180,0,0]) translate([0,-out_w,0]) lid(); }
