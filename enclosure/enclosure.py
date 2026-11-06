"""Parametric enclosure for the reSpeaker XVF3800 (+XIAO ESP32-S3) and the Seeed Mono Enclosed Speaker 4R 5W.

"Puck tower" in three printed parts that assemble without anything crossing the board's path:
lid (top plate + upper wall, print upside down) holds the board LED/mic side up under the top plate;
body (lower wall with the speaker collar/grille and the bottom-plate bosses) is screwed to the lid from
inside; bottom plate carries the speaker and closes the body from below. The speaker stands upright below the board and fires forward (+y, the XIAO USB-C
side) through a grille in the wall; a flat collar inside the curved wall seals its face via a foam ring.

Coordinates: origin = board centre, z = 0 = bottom face of the PCB, +z = LED/mic side (up),
+y = XIAO USB-C edge, -y = XMOS USB-C (power) edge. All positions below come from Seeed's official
STEP model (respeaker_mic_array_xvf3800_1_with-xiao-0820.stp) and the 2D drawing.

Outputs (enclosure/out/): shell.step/.stl, bottom.step/.stl, assembly.step (with the board).
Run: work/venv-cad/bin/python enclosure/enclosure.py [--board path/to/board.stp]
"""
import math
import sys
from pathlib import Path

import cadquery as cq

# ---------------------------------------------------------------- parameters (mm)
WALL = 2.4            # outer wall thickness
TOP = 3.5             # top plate thickness: room for the 4 mm M2 insert + M2x6 screw tip under the top
BOTTOM = 2.4          # bottom plate thickness
BOARD_R = 50.0        # PCB radius
PCB_T = 1.11          # PCB thickness (model)
IR = 54.0             # inner radius: PCB tab under the jack r 52.8, XMOS USB-C r 53.0, joint bosses clear the PCB
OR = IR + WALL
GAP_ABOVE = 2.5       # PCB top -> underside of top plate: 3 mm foam mic rings compressed to 2.5 (LEDs are 0.93 tall)
Z_TOP_IN = PCB_T + GAP_ABOVE
Z_TOP = Z_TOP_IN + TOP
TOP_FILLET = 3.0

# board mounting: 3 x M2 through the PCB into heat inserts in bosses under the top plate
BOARD_HOLES = [(-21.35, 24.5), (21.35, 24.5), (0.0, -34.0)]
BOSS_D = 5.6
# Heat-set inserts from the kit (label "thread x length x OD"): M2x4x3.2 and M2.5x6x3.5.
# Screws: ISO 7380 button head, A2-70; shortest in the kit are M2x6 and M2.5x6.
M2_INSERT_OD, M2_INSERT_L = 3.2, 4.0
M2_INSERT_HOLE = 3.0  # pick the hole that grips best on the fit-test piece
M2_INSERT_DEPTH = 5.4  # insert 4 mm + the M2x6 tip (6 - 1.1 PCB = 4.9 mm into the boss) + margin
M2_SCREW_L, M2_HEAD_D, M2_HEAD_H = 6.0, 3.8, 1.3  # M2 x 6 from below through the PCB

# acoustics / light on the top
MIC_PORTS = [(-33.0, 33.0), (33.0, 33.0), (-33.0, -33.0), (33.0, -33.0)]
MIC_HOLE_D = 2.0
LED_R, LED_ANGLES = 40.0, [15 + 30 * k for k in range(12)]
LED_WINDOW_D = 5.0    # pocket from inside up to the white inlay (light path through the dark body)
STATUS_LED = (0.0, -16.2)
# Two-colour top (multi-material print, one fused part): body in any colour, a white ring inlay flush
# with the top surface over the 12 LEDs, plus a white dot over the status LED. The pockets under
# each LED reach the inlay, so the LEDs glow through INLAY_T of white plastic. Between the LEDs the
# body stays solid below the inlay, so the centre of the top stays firmly attached.
INLAY_T = 0.6         # 3 layers at 0.2 mm
RING_R_IN, RING_R_OUT = 37.0, 43.0
STATUS_DOT_D = 5.0

# openings in the wall (angle in degrees from +x, centre height z, size)
USB_PLUG_W, USB_PLUG_H = 13.0, 7.5  # USB-C plug overmold clearance (Raspberry Pi PSU plug fits)
PORTS = {
    "xmos_usb_power": dict(angle=-90.0, z=-1.27, w=USB_PLUG_W, h=USB_PLUG_H),
    "xiao_usb_flash": dict(angle=90.0, z=-2.36, w=USB_PLUG_W, h=USB_PLUG_H),
}
JACK = dict(angle=-70.0, z=-2.45, d=7.0)      # 3.5 mm jack (powered speaker later)
RST_PIN = dict(angle=0.0, z=-1.5, d=2.0)      # board reset: paperclip hole
# mute: separate plunger, inserted into the lid wall from inside before the board; the board's BUT_A
# (tip at r 50.25) captures it from behind. Flange stops it falling out, head stands proud outside.
MUTE_BTN = dict(angle=180.0, z=-1.5, hole_d=5.0, shaft_d=4.6, flange_d=7.0, button_r=50.25, gap=0.15, proud=0.8)

# lid <-> body joint (just below the board, above the speaker collar)
Z_SEAM = -7.6
JOINT_ANGLES = [40.0, 140.0, 250.0]   # clear of USB-C (+-90), jack (-70), RST (0), mute (180)
JOINT_R = 52.8                        # M2 insert boss centre: boss inner edge r 50.5 clears the PCB (r 50)
JOINT_BOSS_D, JOINT_BOSS_H = 4.6, 6.0
JOINT_TAB_T, JOINT_TAB_W = 2.5, 7.0   # body tab under each boss, M2x6 from below
LIP_H, LIP_T, LIP_CLEAR = 2.0, 1.2, 0.15  # centring lip on the body rim, inside the lid wall

# speaker (Seeed 114993346, datasheet drawing): body 49.6 x 44.6 x 22, two tabs 3.8 thick, 3.2-7.0 mm
# behind the face, holes 2 x Ø3.5 at 57.5 pitch (65.5 over the tabs). Mounted cone forward (+y), tabs
# left/right (x), hanging on two posts from the front wall: M3 inserts in the posts, M3x8 from behind.
SPK_L, SPK_W, SPK_H = 49.6, 44.6, 22.0   # width (x), height (z), depth (y)
SPK_X = 0.0
SPK_TAB_PITCH, SPK_TAB_HOLE, SPK_TAB_T = 57.5, 3.5, 3.8
SPK_TAB_FRONT, SPK_TAB_BACK = 3.2, 7.0   # tab faces behind the speaker face
SPK_TAB_W = 8.5                          # tab height (z), rounded end around the hole
SPK_CONE_D = 36.0     # opening in the sealing ring in front of the cone
SPK_FOAM = 2.5        # compressed foam ring between speaker face and sealing ring (3 mm EPDM)
SEAL_OD = 42.0        # round sealing collar: from the curved wall to a flat face in front of the speaker
COMPONENT_BOTTOM = -6.74  # lowest part under the PCB (SPK JST socket)
Z_SPK_TOP = COMPONENT_BOTTOM - 2.0
Z_SPK_BOTTOM = Z_SPK_TOP - SPK_W
FOAM_T = 3.0          # EPDM sheet, self-adhesive, 3 mm (modelled compressed)
Z_BOTTOM_IN = Z_SPK_BOTTOM - 1.0          # speaker hangs on its posts, 1 mm above the bottom plate
Z_BOTTOM = Z_BOTTOM_IN - BOTTOM
Z_SPK_C = (Z_SPK_TOP + Z_SPK_BOTTOM) / 2  # cone centre height
Y_SPK_FRONT = 41.0                        # speaker face: leaves >= 6.2 mm in front of the tabs for the M3 inserts
COLLAR_Y = Y_SPK_FRONT + SPK_FOAM         # flat face of the sealing ring
Y_SPK_BACK = Y_SPK_FRONT - SPK_H
Y_TAB_FRONT, Y_TAB_BACK = Y_SPK_FRONT - SPK_TAB_FRONT, Y_SPK_FRONT - SPK_TAB_BACK
POST_D = 7.2          # clears the speaker body side by 0.35 mm (post centre 28.75, body edge 24.8)
M3_INSERT_OD, M3_INSERT_L, M3_INSERT_HOLE, M3_INSERT_DEPTH = 4.6, 5.7, 4.4, 6.2  # kit: M3x5.7x4.6
M3_SCREW_L, M3_HEAD_D, M3_HEAD_H = 8.0, 5.7, 1.65                             # ISO 7380 M3x8
GRILLE_R, GRILLE_HOLE_D, GRILLE_PITCH = 17.0, 2.5, 3.6

# bottom plate screws: 4 x M2.5 into heat inserts in wall bosses
BOTTOM_SCREW_ANGLES = [0, 180, 225, 315]  # front-left/right stay free: the speaker tabs pass there
BOTTOM_BOSS_D, BOTTOM_BOSS_H = 7.0, 9.0
M25_INSERT_OD, M25_INSERT_L = 3.5, 6.0
M25_INSERT_HOLE, M25_INSERT_DEPTH = 3.3, 6.5
M25_CLEAR, M25_HEAD_D, M25_HEAD_DEPTH = 2.9, 5.2, 1.0  # ISO 7380 M2.5 head 4.7 x 1.5: sits 1 mm deep, feet are taller
M25_SCREW_L, M25_HEAD_H = 6.0, 1.5                     # M2.5 x 6 from below
FEET_R, FEET_ANGLES, FEET_D, FEET_DEPTH = 40.0, [0, 90, 180, 270], 11.0, 0.8
FIT = 0.3             # radial clearance bottom plate <-> wall


def polar(r, deg):
    a = math.radians(deg)
    return r * math.cos(a), r * math.sin(a)


def radial_box(angle, z, w, h, depth=WALL * 4, r=None, fillet=1.0):
    """Box through the wall at `angle`, w along the tangent, h tall, centred at height z."""
    r = (IR + OR) / 2 if r is None else r
    b = cq.Workplane("XY").box(depth, w, h)
    if fillet:
        b = b.edges("|X").fillet(min(fillet, h / 2 - 0.01, w / 2 - 0.01))
    x, y = polar(r, angle)
    return b.rotate((0, 0, 0), (0, 0, 1), angle).translate((x, y, z))


def radial_cyl(angle, z, d, depth=WALL * 4, r=None):
    r = (IR + OR) / 2 if r is None else r
    c = cq.Workplane("YZ").circle(d / 2).extrude(depth).translate((-depth / 2, 0, 0))
    x, y = polar(r, angle)
    return c.rotate((0, 0, 0), (0, 0, 1), angle).translate((x, y, z))


def _shell_full():
    h = Z_TOP - Z_BOTTOM
    s = cq.Workplane("XY").workplane(offset=Z_BOTTOM).circle(OR).extrude(h)
    s = s.faces(">Z").edges().fillet(TOP_FILLET)
    s = s.cut(cq.Workplane("XY").workplane(offset=Z_BOTTOM).circle(IR).extrude(Z_TOP_IN - Z_BOTTOM))
    # board bosses (from PCB top up to the top plate) with insert holes
    for x, y in BOARD_HOLES:
        boss = cq.Workplane("XY").workplane(offset=PCB_T).center(x, y).circle(BOSS_D / 2).extrude(GAP_ABOVE + 0.01)
        s = s.union(boss)
        s = s.cut(cq.Workplane("XY").workplane(offset=PCB_T).center(x, y).circle(M2_INSERT_HOLE / 2).extrude(M2_INSERT_DEPTH))
    # mic holes and LED light windows
    for x, y in MIC_PORTS:
        s = s.cut(cq.Workplane("XY").workplane(offset=Z_TOP_IN - 1).center(x, y).circle(MIC_HOLE_D / 2).extrude(TOP + 2))
    for p in [polar(LED_R, a) for a in LED_ANGLES] + [STATUS_LED]:
        s = s.cut(cq.Workplane("XY").workplane(offset=Z_TOP_IN - 0.01).center(*p).circle(LED_WINDOW_D / 2).extrude(TOP - INLAY_T + 0.02))
    s = s.cut(make_led_inlay())
    # round sealing collar: tube from the curved wall to a flat face at COLLAR_Y around the cone
    wall_in = cq.Workplane("XY").workplane(offset=Z_BOTTOM).circle(IR + 0.5).extrude(Z_TOP_IN - Z_BOTTOM)
    ring = (cq.Workplane("XZ", origin=(0, COLLAR_Y, 0)).center(SPK_X, Z_SPK_C).circle(SEAL_OD / 2).circle(SPK_CONE_D / 2)
            .extrude(-(IR + 1 - COLLAR_Y)))  # XZ normal is -y: negative extrude goes towards +y (the wall)
    s = s.union(ring.intersect(wall_in))
    # speaker posts: from the wall to the front face of each tab, M3 insert from the tab side
    for sx in (-1, 1):
        px = SPK_X + sx * SPK_TAB_PITCH / 2
        post = cq.Workplane("XZ", origin=(0, Y_TAB_FRONT, 0)).center(px, Z_SPK_C).circle(POST_D / 2).extrude(-(IR + 1 - Y_TAB_FRONT))
        s = s.union(post.intersect(wall_in))
        s = s.cut(cq.Workplane("XZ", origin=(0, Y_TAB_FRONT - 0.01, 0)).center(px, Z_SPK_C).circle(M3_INSERT_HOLE / 2).extrude(-M3_INSERT_DEPTH))
    # front grille: hex pattern of holes through the wall, parallel to y
    pts = []
    rows = int(GRILLE_R * 2 / (GRILLE_PITCH * 0.866)) + 2
    for i in range(-rows, rows + 1):
        zz = i * GRILLE_PITCH * 0.866
        for j in range(-rows, rows + 1):
            xx = j * GRILLE_PITCH + (GRILLE_PITCH / 2 if i % 2 else 0)
            if math.hypot(xx, zz) + GRILLE_HOLE_D / 2 <= GRILLE_R:
                pts.append((SPK_X + xx, Z_SPK_C + zz))
    # holes start inside the cone opening (curved wall is closer at the grille edge) and exit the wall
    s = s.cut(cq.Workplane("XZ", origin=(0, COLLAR_Y + 1.0, 0)).pushPoints(pts).circle(GRILLE_HOLE_D / 2).extrude(-(OR - COLLAR_Y + 1)))
    # wall openings
    for p in PORTS.values():
        s = s.cut(radial_box(p["angle"], p["z"], p["w"], p["h"]))
    s = s.cut(radial_cyl(JACK["angle"], JACK["z"], JACK["d"]))
    s = s.cut(radial_cyl(RST_PIN["angle"], RST_PIN["z"], RST_PIN["d"]))
    # mute plunger hole
    s = s.cut(radial_cyl(MUTE_BTN["angle"], MUTE_BTN["z"], MUTE_BTN["hole_d"]))
    # bottom-plate bosses with inserts
    for ang in BOTTOM_SCREW_ANGLES:
        x, y = polar(IR - BOTTOM_BOSS_D / 2 + 0.3, ang)
        boss = cq.Workplane("XY").workplane(offset=Z_BOTTOM_IN).center(x, y).circle(BOTTOM_BOSS_D / 2).extrude(BOTTOM_BOSS_H)
        boss = boss.intersect(cq.Workplane("XY").workplane(offset=Z_BOTTOM_IN).circle(OR - 0.1).extrude(BOTTOM_BOSS_H))
        s = s.union(boss)
        s = s.cut(cq.Workplane("XY").workplane(offset=Z_BOTTOM_IN - 0.01).center(x, y).circle(M25_INSERT_HOLE / 2).extrude(M25_INSERT_DEPTH))
    return s


def _half(z0, z1):
    return cq.Workplane("XY").workplane(offset=z0).circle(OR + 5).extrude(z1 - z0)


def make_lid():
    """Top plate + upper wall down to Z_SEAM, with the joint bosses (M2 inserts from below)."""
    s = _shell_full().intersect(_half(Z_SEAM, Z_TOP + 1))
    for ang in JOINT_ANGLES:
        x, y = polar(JOINT_R, ang)
        boss = cq.Workplane("XY").workplane(offset=Z_SEAM).center(x, y).circle(JOINT_BOSS_D / 2).extrude(JOINT_BOSS_H)
        s = s.union(boss.intersect(cq.Workplane("XY").workplane(offset=Z_SEAM).circle(OR - 0.2).extrude(JOINT_BOSS_H)))
        s = s.cut(cq.Workplane("XY").workplane(offset=Z_SEAM - 0.01).center(x, y).circle(M2_INSERT_HOLE / 2).extrude(M2_INSERT_DEPTH))
    return s


def make_body():
    """Lower wall from Z_SEAM down: speaker collar + grille, bottom-plate bosses, joint tabs, centring lip."""
    s = _shell_full().intersect(_half(Z_BOTTOM - 1, Z_SEAM))
    a_tan = lambda ang: (-math.sin(math.radians(ang)), math.cos(math.radians(ang)))
    for ang in JOINT_ANGLES:
        x, y = polar(JOINT_R, ang)
        r0 = JOINT_R - JOINT_BOSS_D / 2 - 0.4
        tab = radial_box(ang, Z_SEAM - JOINT_TAB_T / 2, JOINT_TAB_W, JOINT_TAB_T, depth=IR + 0.6 - r0, r=(r0 + IR + 0.6) / 2, fillet=0)
        s = s.union(tab)
        s = s.cut(cq.Workplane("XY").workplane(offset=Z_SEAM - JOINT_TAB_T - 0.01).center(x, y).circle(1.2).extrude(JOINT_TAB_T + 0.02))
        # room for the M2 button head under the tab (it reaches into the wall a little)
        s = s.cut(cq.Workplane("XY").workplane(offset=Z_SEAM - JOINT_TAB_T - M2_HEAD_H - 1.5).center(x, y).circle(M2_HEAD_D / 2 + 0.3).extrude(M2_HEAD_H + 1.5))
    lip = cq.Workplane("XY").workplane(offset=Z_SEAM).circle(IR - LIP_CLEAR).circle(IR - LIP_CLEAR - LIP_T).extrude(LIP_H)
    for ang in JOINT_ANGLES:
        x, y = polar(JOINT_R, ang)
        lip = lip.cut(cq.Workplane("XY").workplane(offset=Z_SEAM - 1).center(x, y).circle(JOINT_BOSS_D / 2 + 0.6).extrude(LIP_H + 2))
    return s.union(lip)


def make_mute_plunger():
    """Separate mute button: flange inside the lid wall, shaft through the hole, head proud outside."""
    m = MUTE_BTN
    r_in = m["button_r"] + m["gap"]
    flange = radial_cyl(m["angle"], m["z"], m["flange_d"], depth=IR - 0.1 - r_in, r=(r_in + IR - 0.1) / 2)
    r_out = OR + m["proud"]
    shaft = radial_cyl(m["angle"], m["z"], m["shaft_d"], depth=r_out - (IR - 0.1), r=(IR - 0.1 + r_out) / 2)
    return flange.union(shaft)


def make_shell():
    """Lid + body as one compound (for checks and renders of the closed device)."""
    return cq.Workplane("XY").add(cq.Compound.makeCompound([make_lid().val(), make_body().val()]))


def make_led_inlay():
    """White inlay (second material of the shell): ring over the LED ring + dot over the status LED."""
    z0 = Z_TOP - INLAY_T
    ring = cq.Workplane("XY").workplane(offset=z0).circle(RING_R_OUT).circle(RING_R_IN).extrude(INLAY_T)
    dot = cq.Workplane("XY").workplane(offset=z0).center(*STATUS_LED).circle(STATUS_DOT_D / 2).extrude(INLAY_T)
    return ring.union(dot)


def make_bottom():
    b = cq.Workplane("XY").workplane(offset=Z_BOTTOM).circle(IR - FIT).extrude(BOTTOM)
    # cut-outs where the shell's bottom bosses sit (bosses rest on the plate, plate sits inside the wall)
    for ang in BOTTOM_SCREW_ANGLES:
        x, y = polar(IR - BOTTOM_BOSS_D / 2 + 0.3, ang)
        b = b.cut(cq.Workplane("XY").workplane(offset=Z_BOTTOM - 1).center(x, y).circle(M25_CLEAR / 2).extrude(BOTTOM + 2))
        b = b.cut(cq.Workplane("XY").workplane(offset=Z_BOTTOM - 0.01).center(x, y).circle(M25_HEAD_D / 2).extrude(M25_HEAD_DEPTH))
    # feet recesses
    for ang in FEET_ANGLES:
        b = b.cut(cq.Workplane("XY").workplane(offset=Z_BOTTOM - 0.01).center(*polar(FEET_R, ang)).circle(FEET_D / 2).extrude(FEET_DEPTH))
    return b


def _insert(x, y, z0, od, length, idia):
    return (cq.Workplane("XY").workplane(offset=z0).center(x, y).circle(od / 2).circle(idia / 2).extrude(length)
            .edges().chamfer(0.15))


def _screw(x, y, z_head_bottom, head_d, head_h, d, length):
    head = cq.Workplane("XY").workplane(offset=z_head_bottom).center(x, y).circle(head_d / 2).extrude(head_h)
    head = head.faces("<Z").edges().fillet(head_h * 0.45)
    head = head.cut(cq.Workplane("XY").workplane(offset=z_head_bottom - 0.01).center(x, y).rect(head_d * 0.55, head_d * 0.14).extrude(head_h * 0.6))
    head = head.cut(cq.Workplane("XY").workplane(offset=z_head_bottom - 0.01).center(x, y).rect(head_d * 0.14, head_d * 0.55).extrude(head_h * 0.6))
    shaft = cq.Workplane("XY").workplane(offset=z_head_bottom + head_h).center(x, y).circle(d / 2).extrude(length)
    return head.union(shaft)


def make_hardware():
    """Inserts and screws as separate bodies: name -> (shape, group). Groups move together in the viewer."""
    hw = {}
    for i, (x, y) in enumerate(BOARD_HOLES):
        # insert pressed into the boss under the top plate, opening flush with the boss face
        hw[f"insert_m2_{i}"] = (_insert(x, y, PCB_T, M2_INSERT_OD, M2_INSERT_L, 2.0), "inserts_top")
        # M2 screw from below through the PCB
        hw[f"screw_m2x{M2_SCREW_L:g}_{i}"] = (_screw(x, y, -M2_HEAD_H, M2_HEAD_D, M2_HEAD_H, 2.0, M2_SCREW_L), "screws_board")
    for i, sx in enumerate((-1, 1)):
        px = SPK_X + sx * SPK_TAB_PITCH / 2
        ins = _insert(0, 0, 0, M3_INSERT_OD, M3_INSERT_L, 3.0).rotate((0, 0, 0), (1, 0, 0), -90)
        hw[f"insert_m3_{i}"] = (ins.translate((px, Y_TAB_FRONT, Z_SPK_C)), "inserts_speaker")
        scr = _screw(0, 0, 0, M3_HEAD_D, M3_HEAD_H, 3.0, M3_SCREW_L).rotate((0, 0, 0), (1, 0, 0), -90)
        hw[f"screw_m3x{M3_SCREW_L:g}_{i}"] = (scr.translate((px, Y_TAB_BACK - M3_HEAD_H - 0.02, Z_SPK_C)), "screws_speaker")
    for i, ang in enumerate(JOINT_ANGLES):
        x, y = polar(JOINT_R, ang)
        hw[f"insert_joint_m2_{i}"] = (_insert(x, y, Z_SEAM, M2_INSERT_OD, M2_INSERT_L, 2.0), "inserts_joint")
        hw[f"screw_joint_m2x{M2_SCREW_L:g}_{i}"] = (_screw(x, y, Z_SEAM - JOINT_TAB_T - M2_HEAD_H, M2_HEAD_D, M2_HEAD_H, 2.0, M2_SCREW_L), "screws_joint")
    for i, ang in enumerate(BOTTOM_SCREW_ANGLES):
        x, y = polar(IR - BOTTOM_BOSS_D / 2 + 0.3, ang)
        hw[f"insert_m25_{i}"] = (_insert(x, y, Z_BOTTOM_IN, M25_INSERT_OD, M25_INSERT_L, 2.5), "inserts_bottom")
        zh = Z_BOTTOM + M25_HEAD_DEPTH - M25_HEAD_H - 0.02  # head seated in the counterbore
        hw[f"screw_m25x{M25_SCREW_L:g}_{i}"] = (_screw(x, y, zh, 4.7, M25_HEAD_H, 2.5, M25_SCREW_L), "screws_bottom")
    return hw


def make_fit_test():
    """Small print-first coupon: insert holes and USB-C plug openings in three sizes each.
    Use the best-fitting values for M2_INSERT_HOLE, M25_INSERT_HOLE, M3_INSERT_HOLE and USB_PLUG_W/H."""
    T = 8.0
    t = cq.Workplane("XY").box(104, 36, T).translate((0, 0, T / 2))
    t = t.cut(cq.Workplane("XY").box(104, 16, T).translate((0, -10, T / 2 + WALL)))  # thin strip = wall thickness
    for i, d in enumerate((2.8, 3.0, 3.2)):
        t = t.cut(cq.Workplane("XY").workplane(offset=T - M2_INSERT_DEPTH).center(-34 + i * 9, 10).circle(d / 2).extrude(M2_INSERT_DEPTH + 0.01))
    for i, d in enumerate((3.1, 3.3, 3.5)):
        t = t.cut(cq.Workplane("XY").workplane(offset=T - M25_INSERT_DEPTH).center(-4 + i * 9, 10).circle(d / 2).extrude(M25_INSERT_DEPTH + 0.01))
    for i, d in enumerate((4.2, 4.4, 4.6)):
        t = t.cut(cq.Workplane("XY").workplane(offset=T - M3_INSERT_DEPTH).center(26 + i * 10, 10).circle(d / 2).extrude(M3_INSERT_DEPTH + 0.01))
    for i, (w, h) in enumerate(((12.0, 6.5), (13.0, 7.5), (14.0, 8.5))):
        t = t.cut(cq.Workplane("XY").center(-26 + i * 26, -10).rect(w, h).extrude(3).edges("|Z").fillet(1.5))
    t = t.cut(cq.Workplane("XY").workplane(offset=-0.5).center(45, -10).circle(MIC_HOLE_D / 2).extrude(T + 1))
    labels = [("2.8", -34, 3), ("3.0", -25, 3), ("3.2", -16, 3), ("3.1", -4, 3), ("3.3", 5, 3), ("3.5", 14, 3),
              ("4.2", 26, 3), ("4.4", 36, 3), ("4.6", 46, 3),
              ("M2", -25, 15.2), ("M2.5", 5, 15.2), ("M3", 36, 15.2), ("mic", 45, -3.2), ("12x6.5", -26, -3.2), ("13x7.5", 0, -3.2), ("14x8.5", 26, -3.2)]
    for txt, x, y in labels:
        try:
            t = t.cut(cq.Workplane("XY").workplane(offset=(T if y > 0 else WALL) - 0.4).center(x, y).text(txt, 3.0, 0.5, halign="center", valign="center"))
        except Exception:
            pass  # text needs a font; the coupon works without labels (sizes increase left to right)
    return t


def make_foam():
    """3 mm EPDM foam parts at their compressed size: name -> (shape, group)."""
    f = {}
    for i, (x, y) in enumerate(MIC_PORTS):  # mic seals: stuck under the top plate, pressed onto the PCB
        f[f"foam_mic_{i}"] = (cq.Workplane("XY").workplane(offset=PCB_T).center(x, y).circle(3.0).circle(1.25).extrude(GAP_ABOVE), "foam_mic")
    # speaker face seal: ring around the cone between the speaker face and the sealing collar
    f["foam_speaker_face"] = (cq.Workplane("XZ", origin=(0, Y_SPK_FRONT, 0)).center(SPK_X, Z_SPK_C)
                              .circle(SEAL_OD / 2).circle(SPK_CONE_D / 2 + 0.5).extrude(-SPK_FOAM), "foam_speaker_face")
    return f


def make_speaker_dummy():
    """Speaker body + the two mounting tabs (from the datasheet drawing)."""
    yc = (Y_SPK_BACK + Y_SPK_FRONT) / 2
    body = cq.Workplane("XY").workplane(offset=Z_SPK_BOTTOM).center(SPK_X, yc).rect(SPK_L, SPK_H).extrude(SPK_W)
    for sx in (-1, 1):
        hx = SPK_X + sx * SPK_TAB_PITCH / 2
        tab = (cq.Workplane("XZ", origin=(0, Y_TAB_FRONT, 0)).center((hx + SPK_X + sx * (SPK_L / 2 - 1)) / 2, Z_SPK_C)
               .rect(abs(hx - (SPK_X + sx * (SPK_L / 2 - 1))), SPK_TAB_W).extrude(SPK_TAB_T))
        tab = tab.union(cq.Workplane("XZ", origin=(0, Y_TAB_FRONT, 0)).center(hx, Z_SPK_C).circle(SPK_TAB_W / 2).extrude(SPK_TAB_T))
        tab = tab.cut(cq.Workplane("XZ", origin=(0, Y_TAB_FRONT + 0.1, 0)).center(hx, Z_SPK_C).circle(SPK_TAB_HOLE / 2).extrude(SPK_TAB_T + 0.2))
        body = body.union(tab)
    return body


def main():
    out = Path(__file__).resolve().parent / "out"
    out.mkdir(exist_ok=True)
    lid, body, bottom = make_lid(), make_body(), make_bottom()
    inlay, plunger = make_led_inlay(), make_mute_plunger()
    for name, part in (("lid", lid), ("lid_inlay_white", inlay), ("body", body), ("bottom", bottom), ("mute_button", plunger)):
        cq.exporters.export(part, str(out / f"{name}.step"))
        cq.exporters.export(part, str(out / f"{name}.stl"), tolerance=0.05, angularTolerance=0.1)
    asm = cq.Assembly(name="gpt_live_speaker_enclosure")
    asm.add(lid, name="lid", color=cq.Color(0.22, 0.23, 0.25, 1))
    asm.add(inlay, name="lid_inlay_white", color=cq.Color(0.97, 0.97, 0.95, 1))
    asm.add(body, name="body", color=cq.Color(0.22, 0.23, 0.25, 1))
    asm.add(plunger, name="mute_button", color=cq.Color(0.97, 0.97, 0.95, 1))
    asm.add(bottom, name="bottom", color=cq.Color(0.3, 0.3, 0.32, 1))
    asm.add(make_speaker_dummy(), name="speaker_dummy", color=cq.Color(0.1, 0.1, 0.1, 1))
    for name, (part, group) in make_hardware().items():
        col = cq.Color(0.8, 0.62, 0.25, 1) if name.startswith("insert") else cq.Color(0.6, 0.62, 0.66, 1)
        asm.add(part, name=name, color=col)
    for name, (part, group) in make_foam().items():
        asm.add(part, name=name, color=cq.Color(0.15, 0.2, 0.3, 1))
    fit = make_fit_test()
    cq.exporters.export(fit, str(out / "fit_test.step"))
    cq.exporters.export(fit, str(out / "fit_test.stl"), tolerance=0.05, angularTolerance=0.1)
    board = sys.argv[sys.argv.index("--board") + 1] if "--board" in sys.argv else None
    if board:
        b = cq.importers.importStep(board).translate((-150.0, 80.0, 0.0))
        asm.add(b, name="respeaker_board", color=cq.Color(0.05, 0.25, 0.1, 1))
        cq.exporters.export(b, str(out / "board_placed.stl"), tolerance=0.1, angularTolerance=0.3)
    asm.save(str(out / "assembly.step"))
    cq.exporters.export(make_speaker_dummy(), str(out / "speaker_dummy.stl"))
    print(f"outer diameter {2 * OR:.1f} mm, height {Z_TOP - Z_BOTTOM:.1f} mm (+ feet)")
    print(f"z: top {Z_TOP:.2f}, speaker top {Z_SPK_TOP:.2f}, bottom {Z_BOTTOM:.2f}")


if __name__ == "__main__":
    main()
