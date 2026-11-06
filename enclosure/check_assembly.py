"""Fit and assembly-path checks for the enclosure.

Static: every part against every other part in the closed device.
Paths (the steps someone actually does, in order):
  1. mute button into the lid wall from inside, board slides up into the lid (and onto its screws);
  2. lid + board come down onto the body;
  3. speaker goes up into the body (held back), then forward onto its posts;
  4. bottom plate slides up into the body.
Each moving group is swept in small steps and checked against everything already in place.
Foam is modelled compressed, so contact (not overlap) is expected; tiny overlaps < TOL are ignored.

Run: work/venv-cad/bin/python enclosure/check_assembly.py --board <board.stp>
"""
import sys
from pathlib import Path

import cadquery as cq

sys.path.insert(0, str(Path(__file__).resolve().parent))
import enclosure as E  # noqa: E402

TOL = 0.05  # mm^3
SPK_BACKOFF = 6.0  # mm the speaker is held back while it goes up, then pushed forward onto the posts


def bbox_overlap(a, b, m=0.0):
    return not (a.xmax < b.xmin - m or a.xmin > b.xmax + m or a.ymax < b.ymin - m or a.ymin > b.ymax + m
                or a.zmax < b.zmin - m or a.zmin > b.zmax + m)


def clashes(moving, fixed, dz=0.0):
    """moving/fixed: lists of (name, solid); dz = z offset or (dx, dy, dz). Returns [(a, b, volume)]."""
    out = []
    dv = cq.Vector(*dz) if isinstance(dz, tuple) else cq.Vector(0, 0, dz)
    for an, a in moving:
        a2 = a.translate(dv) if dv.Length > 0 else a
        ab = a2.BoundingBox()
        for bn, b in fixed:
            if not bbox_overlap(ab, b.BoundingBox()):
                continue
            v = a2.intersect(b).Volume()
            if v > TOL:
                out.append((an, bn, round(v, 2)))
    return out


def near_obstacles(sol):
    """Board parts that could meet something while sliding vertically: near the wall, a boss, a mic
    foam ring or the mute button. Everything else only meets the top plate at the final position,
    which the static check covers."""
    import math
    b = sol.BoundingBox()
    corners = [(b.xmin, b.ymin), (b.xmin, b.ymax), (b.xmax, b.ymin), (b.xmax, b.ymax)]
    if max(math.hypot(x, y) for x, y in corners) > 49.0:
        return True
    feet = [(x, y, E.BOSS_D / 2 + 0.5) for x, y in E.BOARD_HOLES] + [(x, y, 3.5) for x, y in E.MIC_PORTS]
    for fx, fy, fr in feet:
        if b.xmin - fr <= fx <= b.xmax + fr and b.ymin - fr <= fy <= b.ymax + fr:
            return True
    return False


def solids(name, shape):
    return [(f"{name}[{i}]" if len(shape.Solids()) > 1 else name, s) for i, s in enumerate(shape.Solids())]


def main():
    board = cq.importers.importStep(sys.argv[sys.argv.index("--board") + 1]).translate((-150.0, 80.0, 0.0))
    board_s = [(f"board[{i}]", s) for i, s in enumerate(board.solids().vals())]
    board_near = [(n, s) for n, s in board_s if near_obstacles(s)]
    print(f"board: {len(board_s)} parts, {len(board_near)} near walls/bosses checked on the paths", flush=True)
    lid, body, bottom = E.make_lid().val(), E.make_body().val(), E.make_bottom().val()
    plunger, inlay, spk = E.make_mute_plunger().val(), E.make_led_inlay().val(), E.make_speaker_dummy().val()
    hw = {n: p.val() for n, (p, g) in E.make_hardware().items()}
    foam = {n: p.val() for n, (p, g) in E.make_foam().items()}
    grp = {n: g for n, (p, g) in E.make_hardware().items()} | {n: g for n, (p, g) in E.make_foam().items()}
    ok = True

    # ---- static: closed device
    parts = [("lid", lid), ("inlay", inlay), ("body", body), ("bottom", bottom), ("mute_button", plunger), ("speaker", spk)]
    parts += [(n, s) for n, s in hw.items()] + [(n, s) for n, s in foam.items()]
    static = clashes(board_s, parts)
    for i, (an, a) in enumerate(parts):
        for bn, b in parts[i + 1:]:
            if an.startswith("insert") and bn in ("lid", "body"):
                continue  # heat-set inserts are pressed into slightly smaller holes on purpose
            if bn.startswith("insert") and an in ("lid", "body"):
                continue
            if (an.startswith("screw") and bn.startswith("insert")) or (bn.startswith("screw") and an.startswith("insert")):
                continue  # screw thread inside its insert
            static += clashes([(an, a)], [(bn, b)])
    # the board's own screws pass through the PCB holes: ignore screw-vs-PCB contact
    static = [c for c in static if not (c[0].startswith("board") and c[1].startswith("screw_m2x"))]
    print("static (closed device):", static or "ok", flush=True)
    ok &= not static

    # ---- path 1: board slides up into the lid (mute button already in its hole)
    lid_fixed = [("lid", lid), ("mute_button", plunger)] + [(n, s) for n, s in hw.items() if grp[n] in ("inserts_top", "inserts_joint")] \
        + [(n, s) for n, s in foam.items() if grp[n] == "foam_mic"]
    p1 = []
    for k in range(32, 0, -1):  # from 16 mm below the final position up to it
        dz = -0.5 * k
        hit = clashes(board_near, lid_fixed, dz)
        if hit:
            p1.append((dz, hit[:3]))
    print("path 1 board -> lid:", p1 or "ok", flush=True)
    ok &= not p1

    # ---- path 2: lid with board comes down onto the body
    lid_group = [("lid", lid), ("mute_button", plunger)] + board_near
    p2 = []
    for k in range(28, 0, -1):
        dz = 0.5 * k
        hit = clashes(lid_group, [("body", body)], dz)
        if hit:
            p2.append((dz, hit[:3]))
    print("path 2 lid+board -> body:", p2 or "ok", flush=True)
    ok &= not p2

    # ---- path 3: speaker goes up into the body held SPK_BACKOFF behind its final spot, then forward
    #      onto the posts (face seal compresses on the last mm, so the face foam is not swept)
    spk_group = [("speaker", spk)]
    fixed3 = [("body", body)] + [(n, s) for n, s in hw.items() if grp[n] in ("inserts_speaker", "inserts_bottom")] + board_s
    p3 = []
    for k in range(70, 0, -1):
        hit = clashes(spk_group, fixed3, (0.0, -SPK_BACKOFF, -1.0 * k))
        if hit:
            p3.append(("up", -k, hit[:3]))
    for k in range(int(SPK_BACKOFF * 2), 0, -1):
        hit = clashes(spk_group, fixed3, (0.0, -0.5 * k, 0.0))
        if hit:
            p3.append(("forward", -0.5 * k, hit[:3]))
    print("path 3 speaker -> posts:", p3 or "ok", flush=True)
    ok &= not p3

    # ---- path 4: bottom plate slides up into the body (speaker already mounted)
    fixed4 = [("body", body), ("speaker", spk)] + [(n, s) for n, s in hw.items() if grp[n] == "screws_speaker"] + board_s
    p4 = []
    for k in range(40, 0, -1):
        hit = clashes([("bottom", bottom)], fixed4, -1.0 * k)
        if hit:
            p4.append((-k, hit[:3]))
    print("path 4 bottom plate -> body:", p4 or "ok", flush=True)
    ok &= not p4
    print("ALL OK" if ok else "PROBLEMS FOUND")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
