"""Add wide-ambient metrics to existing results.jsonl entries (re-tests the existing tflite)."""
import json, re, subprocess, sys
from pathlib import Path
HERE = Path(__file__).resolve().parent
PY = str(Path(__file__).resolve().parents[1] / "work/venv-mww/bin/python")
names = sys.argv[1:]
for n in names:
    out = {}
    for wide in (False, True):
        r = subprocess.run([PY, "-I", str(HERE / "test_fast.py"), n, "--skip-export"] + (["--wide"] if wide else []),
                           capture_output=True, text=True).stdout
        m = re.search(r"AUC ([\d.]+) frr@0faph ([\d.]+) frr@0.5faph ([\d.]+)", r)
        out["wide" if wide else "test"] = m.groups() if m else None
    print(f"{n:<18} test auc/frr0/frr.5 {out['test']}   WIDE {out['wide']}", flush=True)
