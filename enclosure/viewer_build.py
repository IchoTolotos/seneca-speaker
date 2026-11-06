"""Build a self-contained 3D viewer (three.js, explode slider) of the enclosure assembly.

Writes enclosure/out/viewer.html. Meshes are tessellated from the CadQuery parts and Seeed's board
STEP, decimated, quantised to int16 and embedded; three.js is inlined, so the page needs no server.

Run: work/venv-cad/bin/python enclosure/viewer_build.py --board <board.stp> --three <three.module.min.js>
"""
import base64
import json
import sys
from pathlib import Path

import cadquery as cq
import numpy as np
import vtk
from vtk.util import numpy_support as ns

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import enclosure as E  # noqa: E402
import enclosure_a as A  # noqa: E402

Q = 0.005  # mm per int16 step (range +-163 mm)


def arg(name):
    return sys.argv[sys.argv.index(name) + 1]


def tess(shape, tol=0.08, ang=0.25):
    v, t = shape.tessellate(tol, ang)
    return np.array([(p.x, p.y, p.z) for p in v], dtype=np.float64), np.array(t, dtype=np.int64)


def decimate(v, t, target):
    if len(t) <= target:
        return v, t
    pts = vtk.vtkPoints(); pts.SetData(ns.numpy_to_vtk(v, deep=True))
    cells = vtk.vtkCellArray()
    conn = np.hstack([np.full((len(t), 1), 3), t]).ravel()
    cells.SetCells(len(t), ns.numpy_to_vtkIdTypeArray(conn, deep=True))
    pd = vtk.vtkPolyData(); pd.SetPoints(pts); pd.SetPolys(cells)
    clean = vtk.vtkCleanPolyData(); clean.SetInputData(pd); clean.Update()
    d = vtk.vtkQuadricDecimation(); d.SetInputConnection(clean.GetOutputPort())
    d.SetTargetReduction(1 - target / len(t)); d.VolumePreservationOn(); d.Update()
    o = d.GetOutput()
    v2 = ns.vtk_to_numpy(o.GetPoints().GetData()).astype(np.float64)
    t2 = ns.vtk_to_numpy(o.GetPolys().GetConnectivityArray()).reshape(-1, 3).astype(np.int64)
    return v2, t2


def pack(v, t):
    q = np.clip(np.round(v / Q), -32767, 32767).astype("<i2")
    u16 = len(v) < 65536
    idx = t.astype("<u2" if u16 else "<u4")
    return base64.b64encode(q.tobytes()).decode(), base64.b64encode(idx.tobytes()).decode(), len(t), u16


CDN = "https://unpkg.com/three@0.160.0/build/three.module.min.js"
# compact variant for inline display (<512 kB): three.js from the CDN, coarser meshes, fixed height
COMPACT_CSS = ("html,body{margin:0;font:13px var(--font-sans,system-ui);color:var(--foreground,#222)}"
               "#wrap{position:relative;height:620px;border-radius:var(--radius,8px);overflow:hidden;background:#f3f2ee}"
               "#c{position:absolute;inset:0;width:100%;height:100%}#ui{position:absolute;color:#222;")


def main():
    board = cq.importers.importStep(arg("--board")).translate((-150.0, 80.0, 0.0))
    solids = sorted(board.solids().vals(), key=lambda s: -s.Volume())
    # design A, explode order top -> bottom: shell, board (+ its screws), bottom plate with bracket + speaker
    shapes = [("shell", "Shell (top + whole wall, one piece), any colour", A.make_shell().val(), "#3a3d42", [0, 0, 150], 0.08),
              ("inlay", "Shell: white LED ring + status dot (2nd colour)", E.make_led_inlay().val(), "#f7f6f0", [0, 0, 150], 0.05),
              ("mute", "Mute button (separate, from inside)", E.make_mute_plunger().val(), "#f7f6f0", [-40, 0, 115], 0.05),
              ("pcb", "reSpeaker board (PCB)", solids[0], "#1f6b3a", [0, 0, 65], 0.08),
              ("components", "Board components + XIAO", cq.Compound.makeCompound(solids[1:]), "#2c2f36", [0, 0, 65], 0.15),
              ("bottom", "Bottom plate + speaker bracket", A.make_bottom().val(), "#55585f", [0, 0, -75], 0.08),
              ("speaker", "Speaker (Seeed 4R 5W, datasheet size)", E.make_speaker_dummy().val(), "#151517", [0, -35, -75], 0.08)]
    hw = A.make_hardware()
    def group(*gs):
        return cq.Compound.makeCompound([sh.val() for sh, gg in hw.values() if gg in gs])
    shapes += [("inserts_shell", "Inserts in the shell (3x M2 board, 4x M2 bottom)", group("inserts_top", "inserts_bottom"), "#c9a14a", [0, 0, 150], 0.05),
               ("inserts_bracket", "Inserts in the bracket (2x M3 speaker)", group("inserts_speaker"), "#c9a14a", [0, 0, -75], 0.05),
               ("screws_board", "Screws M2x6: board -> shell", group("screws_board"), "#9aa0a8", [0, 0, 40], 0.05),
               ("screws_speaker", "Screws M3x8: speaker tabs -> bracket (from behind)", group("screws_speaker"), "#9aa0a8", [0, -60, -75], 0.05),
               ("screws_bottom", "Screws M2x6: bottom plate -> shell", group("screws_bottom"), "#9aa0a8", [0, 0, -105], 0.05)]
    fm = A.make_foam()
    def fgroup(g):
        return cq.Compound.makeCompound([sh.val() for sh, gg in fm.values() if gg == g])
    shapes += [("foam_mic", "Foam: 4 mic seal rings Ø8/3 (3 mm EPDM)", fgroup("foam_mic"), "#3b5b8c", [0, 0, 105], 0.05),
               ("foam_speaker", "Foam: speaker face ring Ø44/37", fgroup("foam_speaker"), "#3b5b8c", [0, -18, -75], 0.05),
               ("foam_wall", "Foam: wall seal ring Ø46/37 (bracket recess)", fgroup("foam_wall"), "#3b5b8c", [0, 12, -75], 0.05)]
    meshes = {n: tess(sh, tol) for n, _, sh, _, _, tol in shapes}
    template = (HERE / "viewer_template.html").read_text()
    for variant, targets in (("full", dict(shell=60000, inlay=4000, mute=1500, pcb=20000, components=60000, bottom=30000, speaker=1500,
                                           inserts_shell=8000, inserts_bracket=3000, screws_board=6000, screws_speaker=3000, screws_bottom=8000,
                                           foam_mic=2000, foam_speaker=2000, foam_wall=2000)),
                             ("compact", dict(shell=9000, inlay=800, mute=300, pcb=2500, components=7000, bottom=4000, speaker=400,
                                              inserts_shell=800, inserts_bracket=300, screws_board=600, screws_speaker=400, screws_bottom=800,
                                              foam_mic=400, foam_speaker=400, foam_wall=400))):
        parts = []
        for name, label, _, color, explode, _ in shapes:
            v, t = decimate(*meshes[name], targets[name])
            pv, pi, n, u16 = pack(v, t)
            parts.append(dict(name=name, label=label, color=color, explode=explode, opacity=1.0, pos=pv, idx=pi, u16=u16))
            print(f"{variant} {name}: {n} triangles")
        html = template.replace("/*__PARTS__*/null", json.dumps(dict(q=Q, parts=parts), separators=(",", ":")))
        if variant == "full":
            three = Path(arg("--three")).read_text()
            html = html.replace("/*__THREE__*/", three.replace("</script>", "<\\/script>"))
            out = HERE / "out" / "viewer.html"
        else:
            html = html.replace("/*__THREE__*/", "")
            html = html.replace("/*__THREE_LOAD__*/import(URL.createObjectURL(new Blob([document.getElementById('three-src').textContent], {type: 'text/javascript'})))",
                                f"import('{CDN}')")
            css_start = html.index("/*__CSS__*/")
            css_end = html.index("#ui{position:absolute;") + len("#ui{position:absolute;")
            html = html[:css_start] + COMPACT_CSS + html[css_end:]
            out = HERE / "out" / "viewer_compact.html"
        out.write_text(html)
        print(f"wrote {out} ({out.stat().st_size / 1e3:.0f} kB)")


if __name__ == "__main__":
    main()
