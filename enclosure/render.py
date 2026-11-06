"""Offscreen preview renders of enclosure/out/*.stl (VTK ships with CadQuery)."""
import sys
from pathlib import Path
import vtk

OUT = Path(__file__).resolve().parent / "out"
COL = {"shell": (0.25, 0.26, 0.28), "shell_inlay_white": (0.97, 0.97, 0.95), "mute_button": (0.97, 0.97, 0.95), "foam_cutting_templates": (0.85, 0.85, 0.8), "bottom": (0.35, 0.35, 0.38), "board_placed": (0.1, 0.45, 0.2), "speaker_dummy": (0.12, 0.12, 0.12)}


def actor(name, opacity=1.0, clip=None):
    r = vtk.vtkSTLReader(); r.SetFileName(str(OUT / f"{name}.stl"))
    src = r
    if clip is not None:
        plane = vtk.vtkPlane(); plane.SetOrigin(0, 0, 0); plane.SetNormal(*clip)
        c = vtk.vtkClipPolyData(); c.SetInputConnection(r.GetOutputPort()); c.SetClipFunction(plane); src = c
    m = vtk.vtkPolyDataMapper(); m.SetInputConnection(src.GetOutputPort())
    a = vtk.vtkActor(); a.SetMapper(m); a.GetProperty().SetColor(*COL[name]); a.GetProperty().SetOpacity(opacity)
    return a


def render(fname, names, pos, up=(0, 0, 1), clip=None, opac=None, size=(1100, 800)):
    ren = vtk.vtkRenderer(); ren.SetBackground(1, 1, 1)
    for n in names:
        ren.AddActor(actor(n, (opac or {}).get(n, 1.0), clip if n in ("shell", "bottom") else None))
    cam = ren.GetActiveCamera(); cam.SetPosition(*pos); cam.SetFocalPoint(0, 0, -12); cam.SetViewUp(*up)
    ren.ResetCamera(); cam.Zoom(1.25)
    win = vtk.vtkRenderWindow(); win.SetOffScreenRendering(1); win.AddRenderer(ren); win.SetSize(*size); win.Render()
    w2i = vtk.vtkWindowToImageFilter(); w2i.SetInput(win); w2i.Update()
    wr = vtk.vtkPNGWriter(); wr.SetFileName(str(OUT / fname)); wr.SetInputConnection(w2i.GetOutputPort()); wr.Write()


closed = ["shell", "shell_inlay_white", "bottom", "mute_button"]
render("view_top.png", closed, (180, -260, 260))
render("view_back_ports.png", closed, (40, -330, 60))
render("view_bottom.png", closed, (150, -200, -300))
render("view_front_grille.png", closed + ["speaker_dummy"], (60, 330, 120))
render("view_section.png", ["shell", "bottom", "board_placed", "speaker_dummy"], (-350, 0, 20), clip=(1, 0, 0))
render("view_bottom_plate_speaker.png", ["bottom", "speaker_dummy"], (200, -250, 160))
render("view_mute_side.png", closed, (-330, -60, 60))
render("view_foam_templates.png", ["foam_cutting_templates"], (0, -120, 220))
print("ok")
