// ============================================================
//  ESP32 WinKeyer — 3D-printable enclosure (FDM)
//
//  Two parts, no supports:
//    tray  — floor + four walls, prints open side up
//    lid   — prints OUTSIDE FACE DOWN (lip, piezo ring, hold-down post up)
//
//  Render:
//    openscad -o stl/tray.stl -D 'part="tray"' --backend Manifold winkeyer-case.scad
//    openscad -o stl/lid.stl  -D 'part="lid"'  --backend Manifold winkeyer-case.scad
//  Preview everything assembled:  part="assembly"   exploded: part="exploded"
//
//  Coordinates: x = left→right seen from the FRONT, y = front (0) → back,
//  z = up. Panel feature positions are given in INTERIOR coordinates
//  (measured from the inside face of the left wall / the floor top), so
//  changing wall thickness does not move anything relative to the parts.
//
//  Every part dimension below marked  MEASURE  is a typical value for that
//  part and varies between vendors. Check it with calipers before printing
//  — a 0.5 mm error in a jack hole is a wasted print.
// ============================================================

part = "assembly";          // "tray" | "lid" | "assembly" | "exploded"

$fn = 48;
eps = 0.01;

// ── shell ─────────────────────────────────────────────────
wall   = 2.4;
floor_t = 2.4;
lid_t  = 2.4;
corner_r = 4;
in_w = 140;                 // interior width  (x) — 130 put the devkit into a corner boss
in_d = 84;                  // interior depth  (y)
in_h = 40;                  // interior height (z), floor top to lid underside

out_w = in_w + 2 * wall;
out_d = in_d + 2 * wall;
tray_h = floor_t + in_h;

// Lid screws: 4 × M3 countersunk into corner bosses.
boss_d    = 7.5;
boss_hole = 2.6;            // 2.6 = M3 self-tapping into plastic; 4.0 = M3 heat-set insert
boss_hole_depth = 12;
lid_hole  = 3.4;
lid_csk_d = 6.6;            // countersink head diameter

// Alignment lip under the lid.
lip_h = 3;
lip_t = 1.6;
lip_gap = 0.3;              // clearance to the tray wall, per side

// ── front panel ───────────────────────────────────────────
// 1.3" SH1106 OLED module, glass against the inside of the front wall.
oled_x = 40;                // module centre, interior x
oled_z = 19;                // module centre, above floor top
oled_pcb_w = 35;            // board measured 35 × 33 (2026-09-13)
oled_pcb_h = 33;
oled_win_w = 32;            // window — chosen 32 × 20 against the 34 × 23 glass (2026-09-13)
oled_win_h = 20;
oled_glass_w = 34;          // glass measured 34 × 23 (2026-09-13)
oled_glass_h = 23;
oled_pocket_depth = 0.8;    // the glass drops into this, so the window lines up by itself
oled_pocket_clear = 0.5;    // total, per axis
oled_win_dz = 1.5;          // glass/window centre above the PCB centre — only moves the PCB stand-in
// Mounting posts through the PCB holes. OFF: the hole spacing was never
// measured, and the pocket plus a few dots of hot glue on the PCB edges holds
// the module without depending on it. Turn on only with measured spacing —
// with 23 mm glass the post shoulders crowd the glass.
oled_posts = false;
oled_hole_dx = 30.4;        // MEASURE if oled_posts
oled_hole_dz = 28.4;        // MEASURE if oled_posts
oled_standoff = 1.6;        // glass thickness
oled_pin_d = 1.8;
oled_post_d = 2.8;

// 16 mm 10k linear pot, M7 bushing.
pot_x = 104;
pot_z = 20;
pot_hole = 7.0;             // bushing measured 6.7 mm (2026-09-13) + 0.3 clearance
pot_tab = true;             // blind hole for the anti-rotation tab
pot_tab_dz = 8.0;           // tab offset above the shaft — measured "around 8 mm" (2026-09-13)
pot_tab_d = 3.6;            // oversized for that "around": a blind recess, so slack costs nothing

// Key-down LED (3 mm) — wired to GPIO2 in parallel with the onboard LED.
led_x = 72;
led_z = 24;
led_hole = 3.2;

// ── back panel ────────────────────────────────────────────
// Positions are left→right SEEN FROM THE FRONT (interior x).
// Paddle: 3.5 mm stereo jack, lower row.
jack_hole = 6.3;            // thread measured 6 mm (2026-09-13) + 0.3 clearance
pdl_x = 16;
pdl_z = 12;

// KEY / PTT / FSK to the rigs: RCA sockets, upper row — never 3.5 mm jacks
// (Manoj, 2026-09-13). Each is the output of its own PC817 on the opto board.
rca_hole = 8.3;             // thread measured 8 mm (2026-09-13) + 0.3 clearance
rca_z = 28;                 // above the lower row; nuts stay under the lid lip
rca_x0 = 16;                // nut clears the back-left boss
rca_pitch = 16;             // RCA nuts are wide; last body ends clear of the devkit
rca_labels = ["K1", "P1", "K2", "P2", "FSK"];

// DC power jack (5.5/2.1 panel type), lower row. 5 V ONLY — see README.
dc_x = 36;
dc_z = 12;
dc_hole = 8.3;              // thread measured 8 mm (2026-09-13) + 0.3 clearance

// ── opto board ────────────────────────────────────────────
// 30 × 40 perfboard: 5 × PC817 + 5 × 330 Ω + a header to the devkit, on four
// posts at the back-left, in front of the RCA bodies and behind the OLED.
opto_w = 40;                // x
opto_d = 30;                // y
opto_x = 12;                // board corner, interior
opto_y = 24;
opto_standoff = 6;          // room for the solder side
opto_hole_inset = 2.5;      // drill the perfboard's corners to match
opto_post_d = 5.5;
opto_screw_hole = 2.2;      // M2.5 self-tapping

// ── ESP32 devkit (38-pin, USB-C) ──────────────────────────
kit_l = 55.3;               // confirmed against the board (2026-09-13)
kit_w = 28.3;               // confirmed
kit_t = 1.6;
kit_x = 118;                // centre, interior x: PCB corner clears the back-right boss,
                            // pin row clears the DC jack body
kit_z = 20;                 // PCB underside above floor top: room for Dupont leads on the pins
kit_gap_back = 1.0;         // PCB end to inside of back wall (USB-C shell overhangs it)
usb_dz = 1.6;               // USB-C centre above PCB top — confirmed (2026-09-13)
usb_w = 9.6;
usb_h = 4.0;
usb_plug_w = 13.0;          // recess for the cable's overmould
usb_plug_h = 7.4;
usb_plug_depth = 1.2;
shield_top = 3.1;           // WROOM shield height above the devkit PCB
shield_from_front = 15.75;  // shield centre from the antenna-end edge
foam = 2.0;                 // foam pad between the hold-down post and the shield

shelf_w = 16;               // under the short ends only — pin rows run along the long edges
shelf_len = 6;

// ── lid ───────────────────────────────────────────────────
piezo_x = 60;
piezo_y = 50;               // interior y
piezo_d = 12.4;             // passive piezo body measured 12 mm (2026-09-13) + 0.4 clearance
piezo_ring_h = 4;
sound_hole = 1.8;

// ── side vents ────────────────────────────────────────────
vents = true;
vent_w = 2; vent_h = 16; vent_n = 6; vent_pitch = 6;

// ============================================================

// Interior → absolute.
function ax(x) = wall + x;
function ay(y) = wall + y;
function az(z) = floor_t + z;

kit_y1 = in_d - kit_gap_back;          // interior y of the USB end
kit_y0 = kit_y1 - kit_l;               // antenna end

module rrect(w, d, h, r) {
  hull() for (x = [r, w - r], y = [r, d - r]) translate([x, y, 0]) cylinder(r = r, h = h);
}

module rslot(w, h, depth) {             // rounded slot in the x-z plane, extruded along +y
  r = min(w, h) / 2;
  rotate([-90, 0, 0]) hull()
    for (x = [-(w / 2 - r), w / 2 - r]) translate([x, 0, 0]) cylinder(r = r, h = depth);
}

module front_hole(x, z, d) {
  translate([ax(x), -1, az(z)]) rotate([-90, 0, 0]) cylinder(d = d, h = wall + 2);
}
module back_hole(x, z, d) {
  translate([ax(x), out_d - wall - 1, az(z)]) rotate([-90, 0, 0]) cylinder(d = d, h = wall + 2);
}

module front_label(x, z, s) {           // engraved, readable from the front
  translate([ax(x), 0.6, az(z)]) rotate([90, 0, 0])
    linear_extrude(1.2) text(s, size = 3.2, halign = "center", valign = "center",
                             font = "Liberation Sans:style=Bold");
}
module back_label(x, z, s) {            // engraved, readable from behind
  translate([ax(x), out_d - 0.6, az(z)]) rotate([90, 0, 180])
    linear_extrude(1.2) text(s, size = 3, halign = "center", valign = "center",
                             font = "Liberation Sans:style=Bold");
}

boss_xy = [for (x = [boss_d / 2, in_w - boss_d / 2], y = [boss_d / 2, in_d - boss_d / 2]) [x, y]];

// ============================================================
module tray() {
  difference() {
    union() {
      difference() {
        rrect(out_w, out_d, tray_h, corner_r);
        translate([wall, wall, floor_t]) rrect(in_w, in_d, in_h + 1, max(corner_r - wall, 0.5));
      }
      // corner bosses, merged into the walls
      for (p = boss_xy) translate([ax(p[0]), ay(p[1]), 0]) cylinder(d = boss_d, h = tray_h);

      // OLED posts on the inside of the front wall (optional, see oled_posts)
      if (oled_posts) for (sx = [-1, 1], sz = [-1, 1])
        translate([ax(oled_x + sx * oled_hole_dx / 2), wall - eps, az(oled_z + sz * oled_hole_dz / 2)])
          rotate([-90, 0, 0]) {
            cylinder(d = oled_post_d, h = oled_standoff + eps);
            cylinder(d = oled_pin_d, h = oled_standoff + 2.5);
          }

      // devkit shelves under both short ends
      for (y = [kit_y1 - shelf_len, kit_y0])
        translate([ax(kit_x - shelf_w / 2), ay(y), floor_t - eps]) cube([shelf_w, shelf_len, kit_z + eps]);
      // fences at the antenna end: stop it sliding forward or sideways
      translate([ax(kit_x - shelf_w / 2), ay(kit_y0 - 1.5 - 2), floor_t - eps])
        cube([shelf_w, 2, kit_z + kit_t + 2]);
      for (sx = [-1, 1])
        translate([ax(kit_x + sx * (kit_w / 2 + 1.1)) - 1, ay(kit_y0), floor_t - eps])
          cube([2, 3, kit_z + kit_t + 2]);

      // opto perfboard posts
      for (px = [opto_hole_inset, opto_w - opto_hole_inset], py = [opto_hole_inset, opto_d - opto_hole_inset])
        translate([ax(opto_x + px), ay(opto_y + py), floor_t - eps])
          cylinder(d = opto_post_d, h = opto_standoff + eps);
    }

    // screw holes
    for (p = boss_xy) translate([ax(p[0]), ay(p[1]), tray_h - boss_hole_depth])
      cylinder(d = boss_hole, h = boss_hole_depth + 1);
    for (px = [opto_hole_inset, opto_w - opto_hole_inset], py = [opto_hole_inset, opto_d - opto_hole_inset])
      translate([ax(opto_x + px), ay(opto_y + py), floor_t + 1])
        cylinder(d = opto_screw_hole, h = opto_standoff + 1);

    // front panel
    // window: a rectangle with small corner radii (a full-radius slot at
    // 32 × 20 would be a stadium), and behind it the pocket the glass sits in
    translate([ax(oled_x), -1, az(oled_z + oled_win_dz)]) rotate([-90, 0, 0])
      linear_extrude(wall + 2) offset(r = 1.5) square([oled_win_w - 3, oled_win_h - 3], center = true);
    translate([ax(oled_x) - (oled_glass_w + oled_pocket_clear) / 2, wall - oled_pocket_depth,
               az(oled_z + oled_win_dz) - (oled_glass_h + oled_pocket_clear) / 2])
      cube([oled_glass_w + oled_pocket_clear, oled_pocket_depth + 1, oled_glass_h + oled_pocket_clear]);
    front_hole(pot_x, pot_z, pot_hole);
    if (pot_tab)
      translate([ax(pot_x), wall - 1.2, az(pot_z + pot_tab_dz)]) rotate([-90, 0, 0])
        cylinder(d = pot_tab_d, h = 1.2 + eps);
    front_hole(led_x, led_z, led_hole);
    front_label(pot_x, pot_z - 9.5, "WPM");
    front_label(led_x, led_z - 5, "KEY");

    // back panel: paddle jack and DC low, RCA row above
    back_hole(pdl_x, pdl_z, jack_hole);
    back_label(pdl_x, pdl_z - 7, "PDL");
    for (i = [0 : len(rca_labels) - 1]) {
      back_hole(rca_x0 + i * rca_pitch, rca_z, rca_hole);
      back_label(rca_x0 + i * rca_pitch, rca_z - rca_hole / 2 - 3.2, rca_labels[i]);
    }
    back_hole(dc_x, dc_z, dc_hole);
    back_label(dc_x, dc_z - dc_hole / 2 - 3.5, "5V");
    usb_z = kit_z + kit_t + usb_dz;
    translate([ax(kit_x), out_d - wall - 1, az(usb_z)]) rslot(usb_w, usb_h, wall + 2);
    translate([ax(kit_x), out_d - usb_plug_depth, az(usb_z)]) rslot(usb_plug_w, usb_plug_h, usb_plug_depth + 1);
    back_label(kit_x, usb_z - 7, "USB");

    // side vents, high up
    if (vents)
      for (x = [-1, out_w - wall - 1], i = [0 : vent_n - 1])
        translate([x, ay(in_d / 2 + (i - (vent_n - 1) / 2) * vent_pitch) - vent_w / 2, tray_h - 8 - vent_h])
          cube([wall + 2, vent_w, vent_h]);

    // rubber-foot recesses
    for (x = [12, out_w - 12], y = [12, out_d - 12])
      translate([x, y, -eps]) cylinder(d = 10.5, h = 0.8);
  }
}

// ============================================================
// Modelled in PRINT orientation: outside face on the bed at z = 0, inside
// features pointing up. Flipping it onto the tray mirrors y, so anything
// placed over a tray position uses y' = in_d - y.
module lid() {
  fy = function (y) ay(in_d - y);
  post_len = in_h - (kit_z + kit_t + shield_top) - foam;
  difference() {
    union() {
      rrect(out_w, out_d, lid_t, corner_r);
      // alignment lip, notched at the bosses and behind the OLED module
      difference() {
        translate([wall + lip_gap, wall + lip_gap, lid_t - eps])
          cube([in_w - 2 * lip_gap, in_d - 2 * lip_gap, lip_h + eps]);
        translate([wall + lip_gap + lip_t, wall + lip_gap + lip_t, lid_t - 1])
          cube([in_w - 2 * (lip_gap + lip_t), in_d - 2 * (lip_gap + lip_t), lip_h + 2]);
        // +3, not +2: the lip's square corner otherwise pokes into the
        // tray's rounded inside corner just outside the notch
        for (p = boss_xy) translate([ax(p[0]), fy(p[1]), lid_t - 1]) cylinder(d = boss_d + 3, h = lip_h + 2);
        translate([ax(oled_x - oled_pcb_w / 2 - 1), fy(8), lid_t - 1])
          cube([oled_pcb_w + 2, 8 + wall, lip_h + 2]);
      }
      // piezo ring
      translate([ax(piezo_x), fy(piezo_y), lid_t - eps]) difference() {
        cylinder(d = piezo_d + 3, h = piezo_ring_h);
        translate([0, 0, -1]) cylinder(d = piezo_d, h = piezo_ring_h + 2);
      }
      // hold-down post onto the WROOM shield (stick a foam pad on its tip)
      translate([ax(kit_x), fy(kit_y0 + shield_from_front), lid_t - eps])
        cylinder(d = 6, h = post_len + eps);
    }
    // countersunk screw holes (head on the bed side)
    for (p = boss_xy) translate([ax(p[0]), fy(p[1]), -eps]) {
      cylinder(d = lid_hole, h = lid_t + 1);
      cylinder(d1 = lid_csk_d, d2 = lid_hole, h = (lid_csk_d - lid_hole) / 2);
    }
    // sound holes over the piezo
    translate([ax(piezo_x), fy(piezo_y), -1]) {
      cylinder(d = sound_hole, h = lid_t + 2);
      for (a = [0 : 60 : 300]) rotate(a) translate([3.2, 0, 0]) cylinder(d = sound_hole, h = lid_t + 2);
    }
  }
}

// ============================================================
module lid_in_place(lift = 0) {
  translate([0, out_d, tray_h + lid_t + lift]) rotate([180, 0, 0]) lid();
}

// ── stand-ins for the real parts, for fit checks only ─────
// Typical sizes, shrunk a hair so parts that merely TOUCH (the devkit on its
// shelves, the OLED glass on the wall) do not count as collisions.
module parts() {
  s = 0.05;
  // devkit: PCB, WROOM shield, USB-C shell, and header pins + Dupont
  // housings hanging underneath along both long edges
  translate([ax(kit_x - kit_w / 2) + s, ay(kit_y0) + s, az(kit_z) + s]) cube([kit_w - 2 * s, kit_l - 2 * s, kit_t - 2 * s]);
  translate([ax(kit_x - 9), ay(kit_y0) + s, az(kit_z + kit_t) + s]) cube([18, 25.5, shield_top]);
  translate([ax(kit_x - 4.5), ay(kit_y1 - 6.5), az(kit_z + kit_t) + s]) cube([9, 7.5 - s, 3.2]);
  for (sx = [-1, 1])
    translate([ax(kit_x + sx * 12.7) - 1.27, ay(kit_y0 + 3.5), az(1)])
      cube([2.54, kit_l - 7, kit_z - 1 - s]);
  // OLED: glass, then PCB with its holes, then a 4-pin header on the back
  gy = wall - oled_pocket_depth;          // glass front face, down in its pocket
  translate([ax(oled_x) - oled_glass_w / 2 + s, gy + s, az(oled_z + oled_win_dz) - oled_glass_h / 2 + s])
    cube([oled_glass_w - 2 * s, oled_standoff - 2 * s, oled_glass_h - 2 * s]);
  translate([0, gy + oled_standoff + s, 0]) difference() {
    translate([ax(oled_x - oled_pcb_w / 2), 0, az(oled_z - oled_pcb_h / 2)]) cube([oled_pcb_w, 1.6, oled_pcb_h]);
    for (sx = [-1, 1], sz = [-1, 1])
      translate([ax(oled_x + sx * oled_hole_dx / 2), -1, az(oled_z + sz * oled_hole_dz / 2)])
        rotate([-90, 0, 0]) cylinder(d = 2.2, h = 4);
  }
  translate([ax(oled_x - 5.1), gy + oled_standoff + 1.6 + s, az(oled_z + oled_pcb_h / 2 - 3.5)]) cube([10.2, 11, 2.54]);
  // pot body + lugs, key LED
  translate([ax(pot_x), wall + s, az(pot_z)]) rotate([-90, 0, 0]) cylinder(d = 17, h = 10);
  translate([ax(pot_x - 5), wall + 10, az(pot_z - 10)]) cube([10, 6, 4]);
  translate([ax(led_x), wall + s, az(led_z)]) rotate([-90, 0, 0]) cylinder(d = 3.8, h = 12);
  // paddle jack, RCA sockets, DC jack: nut on the inside face, body behind it
  translate([ax(pdl_x), out_d - wall - s, az(pdl_z)]) rotate([90, 0, 0]) {
    cylinder(d = 10.5, h = 2); cylinder(d = 10, h = 17);
  }
  for (i = [0 : len(rca_labels) - 1])
    translate([ax(rca_x0 + i * rca_pitch), out_d - wall - s, az(rca_z)]) rotate([90, 0, 0]) {
      cylinder(d = 13, h = 2.5); cylinder(d = 12, h = 20);
    }
  // opto perfboard and the parts on it
  translate([ax(opto_x) + s, ay(opto_y) + s, az(opto_standoff) + s]) {
    cube([opto_w - 2 * s, opto_d - 2 * s, 1.6]);
    translate([2, 2, 1.6]) cube([opto_w - 4, opto_d - 4, 11]);
  }
  translate([ax(dc_x), out_d - wall - s, az(dc_z)]) rotate([90, 0, 0]) {
    cylinder(d = 14, h = 2.5); cylinder(d = 13, h = 19);
  }
  // piezo hanging in its ring under the lid
  translate([ax(piezo_x), ay(piezo_y), tray_h - 9.5 - s]) cylinder(d = 12, h = 9.5);
}

if (part == "tray") tray();
else if (part == "lid") lid();
else if (part == "none") {}
// Each check should render EMPTY. The lid is lifted 0.02 mm so resting on
// the rim does not register as a collision.
else if (part == "check_tray_lid")  intersection() { tray(); lid_in_place(0.02); }
else if (part == "check_parts_tray") intersection() { parts(); tray(); }
else if (part == "check_parts_lid")  intersection() { parts(); lid_in_place(0.02); }
else if (part == "ghost") { color("#3a3a40", 0.35) tray(); color("#2aff5a") parts(); }
else if (part == "exploded") { color("#3a3a40") tray(); color("#ffaa22", 0.9) lid_in_place(35); }
else { color("#3a3a40") tray(); color("#ffaa22", 0.6) lid_in_place(0); }
