"""Full fit + assembly-path check for design A, parallel on all cores.

0 static   : closed device, every part incl. inserts, screws, foam (intended contacts allowed)
1 board    : board slides up into the shell from below (mute button + top inserts + mic foam in place)
2 speaker  : speaker (with its face foam) moves forward onto the bracket's back face (bottom plate on the bench)
3 bottom   : bottom plate + bracket + mounted speaker + seal foams slides up into the shell (board mounted)

Run: work/venv-cad/bin/python enclosure/check_a.py --board <board.stp>
"""
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
G = {}


def allowed(a, b):
    pair = {a.split("[")[0], b.split("[")[0]}
    names = " ".join(pair)
    if "insert" in names and pair & {"shell", "bottom"}:
        return True   # heat-set inserts sit in slightly smaller holes on purpose
    if "screw" in names and "insert" in names:
        return True   # screw thread inside its insert
    if "board" in pair and "screw_m2x6_board" in names:
        return True   # board screws pass through the PCB holes
    return False


def build(board_path):
    import cadquery as cq
    import check_assembly as C
    import enclosure as E
    import enclosure_a as A
    board = cq.importers.importStep(board_path).translate((-150.0, 80.0, 0.0))
    board_s = [(f"board[{i}]", s) for i, s in enumerate(board.solids().vals())]
    shell, bottom = A.make_shell().val(), A.make_bottom().val()
    plunger, inlay, spk = E.make_mute_plunger().val(), E.make_led_inlay().val(), E.make_speaker_dummy().val()
    hw = {n: (p.val(), g) for n, (p, g) in A.make_hardware().items()}
    fm = {n: (p.val(), g) for n, (p, g) in A.make_foam().items()}
    sel = lambda d, gs: [(n, s) for n, (s, g) in d.items() if g in gs]
    parts = [("shell", shell), ("inlay", inlay), ("bottom", bottom), ("mute_button", plunger), ("speaker", spk)] \
        + [(n, s) for n, (s, g) in hw.items()] + [(n, s) for n, (s, g) in fm.items()]
    G.update(C=C, board_s=board_s, parts=parts,
             board_near=[(n, s) for n, s in board_s if C.near_obstacles(s)],
             shell_fixed=[("shell", shell), ("mute_button", plunger)] + sel(hw, ("inserts_top",)) + sel(fm, ("foam_mic",)),
             spk_moving=[("speaker", spk)] + sel(fm, ("foam_speaker",)),
             bench=[("bottom", bottom)] + sel(hw, ("inserts_speaker",)) + sel(fm, ("foam_wall",)),
             lower=[("bottom", bottom), ("speaker", spk)] + sel(hw, ("inserts_speaker", "screws_speaker")) + sel(fm, ("foam_speaker", "foam_wall")),
             upper=[("shell", shell), ("mute_button", plunger)] + sel(hw, ("inserts_top", "screws_board", "inserts_bottom")) + sel(fm, ("foam_mic",)) + board_s)


def step(task):
    kind, d = task
    C = G["C"]
    if kind == "static":
        if d == "parts":
            P, out = G["parts"], []
            for i, (an, a) in enumerate(P):
                for bn, b in P[i + 1:]:
                    if not allowed(an, bn):
                        out += C.clashes([(an, a)], [(bn, b)])
            return kind, d, out
        i, n = d
        return kind, d, [c for c in C.clashes(G["board_s"][i::n], G["parts"]) if not allowed(c[0], c[1])]
    if kind == "board":
        return kind, d, C.clashes(G["board_near"], G["shell_fixed"], d)[:3]
    if kind == "speaker":
        return kind, d, C.clashes(G["spk_moving"], G["bench"], d)[:3]
    return kind, d, [c for c in C.clashes(G["lower"], G["upper"], d) if not allowed(c[0], c[1])][:3]


def main():
    board_path = sys.argv[sys.argv.index("--board") + 1]
    tasks = [("static", "parts")] + [("static", (i, 16)) for i in range(16)] \
        + [("board", -1.0 * k) for k in range(75, 0, -1)] \
        + [("speaker", (0.0, -0.5 * k, 0.0)) for k in range(20, 0, -1)] \
        + [("bottom", -1.0 * k) for k in range(75, 0, -1)]
    t0, res = time.time(), {"static": [], "board": [], "speaker": [], "bottom": []}
    with ProcessPoolExecutor(max_workers=os.cpu_count(), initializer=build, initargs=(board_path,)) as ex:
        for f in as_completed([ex.submit(step, t) for t in tasks]):
            k, d, hit = f.result()
            if hit:
                res[k].append((str(d), hit[:4]))
    names = {"static": "closed device (all parts, hardware, foam)", "board": "board up into the shell",
             "speaker": "speaker onto the bracket", "bottom": "bottom plate + speaker up into the shell"}
    for k in res:
        print(f"{names[k]}:", sorted(res[k])[:8] or "ok")
    print(f"{len(tasks)} steps on {os.cpu_count()} processes in {time.time() - t0:.0f} s;",
          "ALL OK" if not any(res.values()) else "PROBLEMS FOUND", flush=True)


if __name__ == "__main__":
    main()
