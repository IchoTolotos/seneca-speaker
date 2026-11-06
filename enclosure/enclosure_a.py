"""Enclosure design A (chosen): one-piece shell + bottom plate that carries the speaker.

- Shell: top plate (two-colour, white LED ring inlay) + the whole wall in one piece, no side seam.
  The board slides up from below and screws under the top plate (3 x M2x6 into inserts). Nothing
  inside the shell crosses the board's path: only 4 slim M2 bosses hugging the wall at angles where
  the board has nothing sticking out.
- Bottom plate: flat plate + a speaker bracket. The speaker is screwed onto the bracket by its tabs
  (2 x M3x8 into inserts, from behind) BEFORE the plate goes in, with full access. The bracket's front
  is curved to the wall (1 mm gap) with a recess around the cone holding a foam ring that seals against
  the wall behind the grille; a lead-in chamfer on top guides it in. 4 x M2x6 from below.
Shared geometry/parameters come from enclosure.py (board, ports, grille, LED inlay, speaker, hardware).

Run: work/venv-cad/bin/python enclosure/enclosure_a.py --board <board.stp>
"""
import math
import sys
from pathlib import Path

import cadquery as cq

sys.path.insert(0, str(Path(__file__).resolve().parent))
import enclosure as E  # noqa: E402
from enclosure import polar  # noqa: E402

# ---------------------------------------------------------------- A-specific parameters (mm)
BOSS_ANGLES = (40.0, 140.0, 220.0, 320.0)  # clear of RST (0), USB-C (+-90), jack (-70), mute (180)
BOSS_R, BOSS_D, BOSS_H = E.IR - 1.6, 4.6, 8.0  # inner edge r 50.1 clears the PCB (r 50) on its way up
M2_CLEAR, M2_HEAD_CB, M2_CB_DEPTH = 2.4, 4.3, 1.0
FRAME_CLEAR = 1.0       # bracket front face to the wall
SEAL_GAP = 2.5          # wall seal foam (3 mm) compressed into a recess around the cone
FRAME_HALF_W = E.SPK_TAB_PITCH / 2 + 4.5
LEAD_IN = 3.0           # chamfer on the bracket's top front edge
# foam rings, cut flat from the 3 mm EPDM sheet (wide enough to cut cleanly; see make_foam_templates)
WALL_SEAL_OD, FACE_SEAL_OD, SEAL_ID = 46.0, 44.0, 37.0   # face ring <= 44.6 (speaker face height)
MIC_SEAL_OD, MIC_SEAL_ID = 8.0, 3.0


def _wall_and_top(shell):
    annulus = cq.Workplane("XY").workplane(offset=E.Z_BOTTOM - 1).circle(E.OR + 1).circle(E.IR - 0.01).extrude(80)
    upper = cq.Workplane("XY").workplane(offset=E.PCB_T - 0.01).circle(E.OR + 1).extrude(20)
    return shell.intersect(annulus.union(upper))


def make_shell():
    s = _wall_and_top(E._shell_full())  # drops the v2 collar, posts and bottom bosses
    for ang in BOSS_ANGLES:
        x, y = polar(BOSS_R, ang)
        boss = cq.Workplane("XY").workplane(offset=E.Z_BOTTOM_IN).center(x, y).circle(BOSS_D / 2).extrude(BOSS_H)
        s = s.union(boss.intersect(cq.Workplane("XY").workplane(offset=E.Z_BOTTOM_IN).circle(E.OR - 0.2).extrude(BOSS_H)))
        s = s.cut(cq.Workplane("XY").workplane(offset=E.Z_BOTTOM_IN - 0.01).center(x, y).circle(E.M2_INSERT_HOLE / 2).extrude(E.M2_INSERT_DEPTH))
        # relief in the wall's bottom edge for the screw head (boss sits so far out the head reaches the wall)
        s = s.cut(cq.Workplane("XY").workplane(offset=E.Z_BOTTOM - 1).center(x, y).circle(M2_HEAD_CB / 2).extrude(M2_CB_DEPTH + 1.2))
    return s


def make_bracket():
    h = E.Z_SPK_TOP - E.Z_BOTTOM_IN
    zc = E.Z_BOTTOM_IN + h / 2
    f = cq.Workplane("XY").box(2 * FRAME_HALF_W, E.IR - E.Y_TAB_FRONT, h).translate((E.SPK_X, (E.IR + E.Y_TAB_FRONT) / 2, zc))
    f = f.intersect(cq.Workplane("XY").workplane(offset=E.Z_BOTTOM_IN).circle(E.IR - FRAME_CLEAR).extrude(h))
    # room for the speaker body's front part (in front of the tabs) and the face foam
    room = cq.Workplane("XY").box(E.SPK_L + 0.8, (E.Y_SPK_FRONT + E.SPK_FOAM) - (E.Y_TAB_FRONT - 1), E.SPK_W + 0.8).translate(
        (E.SPK_X, ((E.Y_SPK_FRONT + E.SPK_FOAM) + (E.Y_TAB_FRONT - 1)) / 2, (E.Z_SPK_TOP + E.Z_SPK_BOTTOM) / 2 + 0.4))
    f = f.cut(room)
    # cone opening
    f = f.cut(cq.Workplane("XZ", origin=(0, E.Y_TAB_FRONT - 1, 0)).center(E.SPK_X, E.Z_SPK_C).circle(E.SPK_CONE_D / 2).extrude(-(E.IR - E.Y_TAB_FRONT + 2)))
    # seal recess around the cone on the curved front
    ring = cq.Workplane("XZ", origin=(0, E.Y_TAB_FRONT, 0)).center(E.SPK_X, E.Z_SPK_C).circle(WALL_SEAL_OD / 2 + 0.5).circle(E.SPK_CONE_D / 2).extrude(-(E.IR - E.Y_TAB_FRONT + 1))
    outer = (cq.Workplane("XY").workplane(offset=E.Z_BOTTOM_IN - 1).circle(E.IR + 1).circle(E.IR - SEAL_GAP).extrude(h + 2))
    f = f.cut(ring.intersect(outer))
    # M3 insert holes for the speaker tabs
    for sx in (-1, 1):
        px = E.SPK_X + sx * E.SPK_TAB_PITCH / 2
        f = f.cut(cq.Workplane("XZ", origin=(0, E.Y_TAB_FRONT - 0.01, 0)).center(px, E.Z_SPK_C).circle(E.M3_INSERT_HOLE / 2).extrude(-E.M3_INSERT_DEPTH))
    # lead-in chamfer along the top front edge
    wedge = (cq.Workplane("XY").box(2 * FRAME_HALF_W + 2, LEAD_IN * 2, LEAD_IN * 2)
             .rotate((0, 0, 0), (1, 0, 0), 45).translate((E.SPK_X, E.IR - FRAME_CLEAR, E.Z_SPK_TOP)))
    return f.cut(wedge)


def make_bottom():
    b = cq.Workplane("XY").workplane(offset=E.Z_BOTTOM).circle(E.IR - E.FIT).extrude(E.BOTTOM)
    for ang in BOSS_ANGLES:
        x, y = polar(BOSS_R, ang)
        b = b.cut(cq.Workplane("XY").workplane(offset=E.Z_BOTTOM - 1).center(x, y).circle(M2_CLEAR / 2).extrude(E.BOTTOM + 2))
        b = b.cut(cq.Workplane("XY").workplane(offset=E.Z_BOTTOM - 0.01).center(x, y).circle(M2_HEAD_CB / 2).extrude(M2_CB_DEPTH))
    for ang in E.FEET_ANGLES:
        b = b.cut(cq.Workplane("XY").workplane(offset=E.Z_BOTTOM - 0.01).center(*polar(E.FEET_R, ang)).circle(E.FEET_D / 2).extrude(E.FEET_DEPTH))
    return b.union(make_bracket())


def make_hardware():
    hw = {}
    for i, (x, y) in enumerate(E.BOARD_HOLES):
        hw[f"insert_m2_top_{i}"] = (E._insert(x, y, E.PCB_T, E.M2_INSERT_OD, E.M2_INSERT_L, 2.0), "inserts_top")
        hw[f"screw_m2x6_board_{i}"] = (E._screw(x, y, -E.M2_HEAD_H, E.M2_HEAD_D, E.M2_HEAD_H, 2.0, E.M2_SCREW_L), "screws_board")
    for i, ang in enumerate(BOSS_ANGLES):
        x, y = polar(BOSS_R, ang)
        hw[f"insert_m2_bottom_{i}"] = (E._insert(x, y, E.Z_BOTTOM_IN, E.M2_INSERT_OD, E.M2_INSERT_L, 2.0), "inserts_bottom")
        zh = E.Z_BOTTOM + M2_CB_DEPTH - E.M2_HEAD_H - 0.02
        hw[f"screw_m2x6_bottom_{i}"] = (E._screw(x, y, zh, E.M2_HEAD_D, E.M2_HEAD_H, 2.0, E.M2_SCREW_L), "screws_bottom")
    for i, sx in enumerate((-1, 1)):
        px = E.SPK_X + sx * E.SPK_TAB_PITCH / 2
        ins = E._insert(0, 0, 0, E.M3_INSERT_OD, E.M3_INSERT_L, 3.0).rotate((0, 0, 0), (1, 0, 0), -90)
        hw[f"insert_m3_{i}"] = (ins.translate((px, E.Y_TAB_FRONT, E.Z_SPK_C)), "inserts_speaker")
        scr = E._screw(0, 0, 0, E.M3_HEAD_D, E.M3_HEAD_H, 3.0, E.M3_SCREW_L).rotate((0, 0, 0), (1, 0, 0), -90)
        hw[f"screw_m3x8_{i}"] = (scr.translate((px, E.Y_TAB_BACK - E.M3_HEAD_H - 0.02, E.Z_SPK_C)), "screws_speaker")
    return hw


def make_foam():
    f = {}
    for i, (x, y) in enumerate(E.MIC_PORTS):
        f[f"foam_mic_{i}"] = (cq.Workplane("XY").workplane(offset=E.PCB_T).center(x, y).circle(MIC_SEAL_OD / 2).circle(MIC_SEAL_ID / 2).extrude(E.GAP_ABOVE), "foam_mic")
    f["foam_speaker_face"] = (cq.Workplane("XZ", origin=(0, E.Y_SPK_FRONT, 0)).center(E.SPK_X, E.Z_SPK_C)
                              .circle(FACE_SEAL_OD / 2).circle(SEAL_ID / 2).extrude(-E.SPK_FOAM), "foam_speaker")
    ring = cq.Workplane("XZ", origin=(0, E.Y_TAB_FRONT, 0)).center(E.SPK_X, E.Z_SPK_C).circle(WALL_SEAL_OD / 2).circle(SEAL_ID / 2).extrude(-(E.IR - E.Y_TAB_FRONT + 1))
    band = cq.Workplane("XY").workplane(offset=E.Z_BOTTOM_IN).circle(E.IR - 0.02).circle(E.IR - SEAL_GAP + 0.02).extrude(E.Z_SPK_TOP - E.Z_BOTTOM_IN)
    f["foam_wall_seal"] = (ring.intersect(band), "foam_wall")
    return f


def make_foam_templates():
    """Flat print-and-trace templates (1.5 mm) for the foam rings: lay on the EPDM sheet, cut around
    the outside and the inside with a cutter. Each has a small tab to hold it; sizes engraved."""
    T = 1.5
    items = [("wall seal", WALL_SEAL_OD, SEAL_ID, (-26.0, 0.0)), ("speaker face", FACE_SEAL_OD, SEAL_ID, (26.0, 0.0))] \
        + [("mic", MIC_SEAL_OD, MIC_SEAL_ID, (-15.0 + 10.0 * k, -33.0)) for k in range(4)]
    out = None
    for name, od, idd, (x, y) in items:
        w = cq.Workplane("XY").center(x, y).circle(od / 2).circle(idd / 2).extrude(T)
        if od > 20:
            w = w.union(cq.Workplane("XY").center(x, y + od / 2 + 3).rect(12, 8).extrude(T))  # grip tab
            try:
                w = w.cut(cq.Workplane("XY").workplane(offset=T - 0.4).center(x, y + od / 2 + 3)
                          .text(f"{od:g}/{idd:g}", 3.2, 0.5, halign="center", valign="center"))
            except Exception:
                pass
        out = w if out is None else out.union(w)
    return out


def main():
    out = Path(__file__).resolve().parent / "out"
    out.mkdir(exist_ok=True)
    shell, bottom = make_shell(), make_bottom()
    inlay, plunger = E.make_led_inlay(), E.make_mute_plunger()
    for name, part in (("shell", shell), ("shell_inlay_white", inlay), ("bottom", bottom), ("mute_button", plunger),
                       ("foam_cutting_templates", make_foam_templates())):
        cq.exporters.export(part, str(out / f"{name}.step"))
        cq.exporters.export(part, str(out / f"{name}.stl"), tolerance=0.05, angularTolerance=0.1)
    asm = cq.Assembly(name="gpt_live_speaker_enclosure_A")
    asm.add(shell, name="shell", color=cq.Color(0.22, 0.23, 0.25, 1))
    asm.add(inlay, name="shell_inlay_white", color=cq.Color(0.97, 0.97, 0.95, 1))
    asm.add(plunger, name="mute_button", color=cq.Color(0.97, 0.97, 0.95, 1))
    asm.add(bottom, name="bottom_with_speaker_bracket", color=cq.Color(0.3, 0.3, 0.32, 1))
    asm.add(E.make_speaker_dummy(), name="speaker", color=cq.Color(0.1, 0.1, 0.1, 1))
    for name, (part, g) in make_hardware().items():
        asm.add(part, name=name, color=cq.Color(0.8, 0.62, 0.25, 1) if name.startswith("insert") else cq.Color(0.6, 0.62, 0.66, 1))
    for name, (part, g) in make_foam().items():
        asm.add(part, name=name, color=cq.Color(0.15, 0.2, 0.3, 1))
    if "--board" in sys.argv:
        b = cq.importers.importStep(sys.argv[sys.argv.index("--board") + 1]).translate((-150.0, 80.0, 0.0))
        asm.add(b, name="respeaker_board", color=cq.Color(0.05, 0.25, 0.1, 1))
        cq.exporters.export(b, str(out / "board_placed.stl"), tolerance=0.1, angularTolerance=0.3)
    asm.export(str(out / "assembly.step"))
    cq.exporters.export(E.make_speaker_dummy(), str(out / "speaker_dummy.stl"))
    for n, p in (("shell", shell), ("bottom", bottom)):
        v = p.val(); print(n, "solids", len(v.Solids()), "valid", v.isValid())
    print(f"A: outer diameter {2 * E.OR:.1f} mm, height {E.Z_TOP - E.Z_BOTTOM:.1f} mm (+ feet)", flush=True)


if __name__ == "__main__":
    main()
