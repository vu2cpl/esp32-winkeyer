# Enclosure — 3D-printable case

A two-part FDM case for the keyer: **tray** (floor + walls) and **lid**,
both printable without supports. Parametric OpenSCAD source in
[`winkeyer-case.scad`](winkeyer-case.scad); STLs in [`stl/`](stl/).

Outside about **145 × 89 × 45 mm**.

![Front: OLED window, key LED, speed pot](preview-front.png)
![Back, lid lifted: RCA row K1 P1 K2 P2 FSK, paddle and 5 V below, USB-C](preview-back.png)

| Panel | What is on it |
|---|---|
| Front | 1.3" OLED window, speed pot (engraved WPM), 3 mm key LED (KEY) |
| Back | upper row: 5 × RCA — K1, P1, K2, P2, FSK; lower row: PDL (3.5 mm stereo), 5 V DC; USB-C |
| Lid | sound holes over the piezo, which clips into a ring underneath |
| Sides | vent slots near the top |
| Inside | 30 × 40 mm opto perfboard on four posts, back-left, behind the RCA sockets |

## Measure before you print

Every part dimension is a named parameter at the top of the `.scad` file,
and the ones below vary between vendors — a jack hole 0.5 mm off wastes the
print. **For VU2CPL's build all of them were measured or confirmed on
2026-09-13** (marked below); if your parts differ, measure yours and re-run
the fit checks:

| Parameter | Default | Part |
|---|---|---|
| `jack_hole` | 6.3 mm | paddle 3.5 mm jack thread — **measured 6 mm** |
| `rca_hole` | 8.3 mm | RCA socket thread (KEY/PTT/FSK) — **measured 8 mm** |
| `dc_hole` | 8.3 mm | DC jack thread — **measured 8 mm** |
| `pot_hole` | 7.0 mm | pot bushing — **measured 6.7 mm** |
| `pot_tab_dz` | 8.0 mm | pot anti-rotation tab from the shaft — **measured ~8 mm**; recess is 3.6 mm so ±0.5 mm still seats |
| `oled_pcb_w/h` | 35 × 33 mm | OLED board — **measured** |
| `oled_glass_w/h` | 34 × 23 mm | OLED glass — **measured**; sits in a 0.8 mm pocket that lines the window up |
| `oled_win_w/h` | 32 × 20 mm | OLED window — **chosen** against that glass |
| `oled_hole_dx/dz` | 30.4 × 28.4 mm | only if you turn `oled_posts` on — not measured |
| `kit_l`, `kit_w`, `usb_dz` | 55.3 × 28.3, 1.6 mm | devkit PCB, USB-C centre above the PCB — **confirmed** |
| `piezo_d` | 12.4 mm | passive piezo body — **measured 12 mm** |

A quick check before the full print: set `part="tray"`, add a
`projection(cut=true)` or just print the back wall alone at 100% as a thin
test strip, and try the jacks and USB plug in it.

## Print

- PETG or PLA, 0.2 mm layers, 3 perimeters, 20% infill.
- **Tray**: open side up. **Lid**: outside face down (the STL is already
  oriented that way).
- No supports. The OLED window's top edge is a 30 mm bridge; the round
  holes in the walls are small enough to print without teardrops.

```bash
cd enclosure
openscad -o stl/tray.stl -D 'part="tray"' --backend Manifold winkeyer-case.scad
openscad -o stl/lid.stl  -D 'part="lid"'  --backend Manifold winkeyer-case.scad
```

## Fit checks — run these after changing any dimension

The `.scad` carries stand-ins for the real parts (devkit with its pin rows
and Dupont leads, OLED glass/PCB/header, pot, the paddle jack, RCA sockets and DC jack with
their nuts, the opto board, piezo). Three modes intersect them, and **each must render
empty** — OpenSCAD prints `Current top level object is empty.`:

```bash
for c in check_tray_lid check_parts_tray check_parts_lid; do
  openscad -o /tmp/$c.stl -D "part=\"$c\"" --backend Manifold winkeyer-case.scad 2>&1 | grep -E "empty|Vertices"
done
```

A `Vertices:` line instead means a collision. `part="ghost"` renders the
tray see-through with the stand-ins in green to find it. The first draft
failed all three: the paddle jack ran into a corner boss, the devkit's
corner into another, and the lid lip into the tray's rounded inside
corners — which is why the box is 140 mm inside rather than 130.

The stand-ins use typical part sizes. They catch layout mistakes; they do
not replace measuring your own parts.

## Hardware

- 4 × M3 × 10 countersunk screws. Default bosses (`boss_hole = 2.6`) take
  self-tapping M3 straight into the plastic; for repeated opening set
  `boss_hole = 4.0` and fit M3 heat-set inserts.
- 4 × 10 mm rubber feet (recesses on the underside).
- A small square of foam tape on the lid's hold-down post.
- 5 × panel RCA sockets (8 mm thread) for K1, P1, K2, P2, FSK; 1 × 3.5 mm
  stereo panel jack for the paddle; a 5.5/2.1 mm panel DC jack; a 3 mm LED
  + 330 Ω.
- 30 × 40 mm perfboard, 5 × PC817, 5 × 330 Ω, 4 × M2.5 × 6 self-tapping
  screws. Drill the board's corners 2.5 mm in from each edge to meet the
  posts.

## Assembly notes

- **The devkit rests on two shelves under its short ends**, 20 mm off the
  floor so Dupont leads on the header pins fit underneath. The pin rows run
  along the long edges, which are left clear. It is held by its USB-C port
  in the back wall, fences at the antenna end, and the lid's post pressing
  on the WROOM shield through the foam. Lower it in, then slide it back so
  the USB-C shell enters the wall.
- **OLED**: drop the glass into its pocket on the inside of the front wall —
  that lines the window up — and hold the PCB with a few dots of hot glue
  on its edges (keep glue off the glass). The mounting posts are off
  (`oled_posts = false`): the hole spacing was never measured, and with
  23 mm glass their shoulders crowd it.
- **Opto board** (back-left, four posts): one PC817 per RCA socket.
  GPIO → 330 Ω → PC817 pin 1 (anode); pin 2 (cathode) → keyer GND; pin 4
  (collector) → RCA centre; pin 3 (emitter) → RCA shell. Keep every RCA
  shell off the keyer's ground — that isolation is the reason for the opto.
  GPIOs: K1 33, P1 32, K2 18, P2 19, FSK 27.
- **Key LED**: 3 mm LED + 330 Ω from GPIO2 to GND, in parallel with the
  onboard LED. GPIO2 is a strapping pin, but an LED to GND keeps it low at
  boot, which is the state it needs.
- **DC jack: 5 V only**, into the devkit's 5V/VIN pin. The devkit's
  regulator runs hot well before 12 V, and the schematic notes a
  470–1000 µF capacitor at the board if it browns out on keying.
- The antenna end faces the front. Keep the jacks' metal and wiring from
  bunching over the WROOM antenna.
