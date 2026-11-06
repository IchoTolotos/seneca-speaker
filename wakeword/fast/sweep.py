"""Run train_fast + test_fast for a list of configs; append to results.jsonl and print a table.

Usage: python3 sweep.py "<name>|<train_fast args>" ...
Each run: name, wall time of training and testing, AUC (lower=better), FRR at 0 FA/h.
"""
import json
import re
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
WORK = Path(__file__).resolve().parents[1] / "work"
RESULTS = HERE / "results.jsonl"
METAL_PY = str(WORK / "venv-metal/bin/python")
CPU_PY = str(WORK / "venv-mww/bin/python")


def run(name: str, args: str):
    py = CPU_PY if "--device cpu" in args else METAL_PY
    t0 = time.time()
    tr = subprocess.run([py, "-I", str(HERE / "train_fast.py"), "--name", name, *args.split()],
                        capture_output=True, text=True)
    t_train = time.time() - t0
    summary = {}
    try:
        summary = json.load(open(WORK / "trained_models" / name / "fast_summary.json"))
    except Exception:
        print(tr.stdout[-2000:], tr.stderr[-3000:])
    t0 = time.time()
    te = subprocess.run([CPU_PY, "-I", str(HERE / "test_fast.py"), name], capture_output=True, text=True)
    t_test = time.time() - t0
    tw = subprocess.run([CPU_PY, "-I", str(HERE / "test_fast.py"), name, "--skip-export", "--wide"],
                        capture_output=True, text=True)
    mw = re.search(r"AUC ([\d.]+) frr@0faph ([\d.]+) frr@0.5faph ([\d.]+)", tw.stdout)
    m5 = re.search(r"frr@0.5faph ([\d.]+)", te.stdout)
    m = re.search(r"AUC ([\d.]+) frr@0faph ([\d.]+)", te.stdout)
    ma = re.search(r"advfar_op ([\d.]+)", te.stdout)
    mb = re.search(r" bal ([\d.]+)", te.stdout)
    rec = {"name": name, "args": args, "train_s": round(t_train, 1), "test_s": round(t_test, 1),
           "auc": float(m.group(1)) if m else None, "frr0": float(m.group(2)) if m else None,
           "advfar": float(ma.group(1)) if ma else None, "bal": float(mb.group(1)) if mb else None,
           "frr05": float(m5.group(1)) if m5 else None,
           "auc_wide": float(mw.group(1)) if mw else None, "frr0_wide": float(mw.group(2)) if mw else None,
           "frr05_wide": float(mw.group(3)) if mw else None,
           "best_step": summary.get("best", {}).get("step"), "samples_per_s": summary.get("samples_per_s"),
           "timings": summary.get("timings_s"), "time": time.strftime("%H:%M:%S")}
    if not m:
        print(te.stdout[-1500:], te.stderr[-1500:])
    with open(RESULTS, "a") as fh:
        fh.write(json.dumps(rec) + "\n")
    print(f"{name:<22} train {rec['train_s']:6.1f}s test {rec['test_s']:5.1f}s  AUC {rec['auc']}  "
          f"frr@0 {rec['frr0']}  advFAR {rec['advfar']}  bal {rec['bal']}  WIDE auc {rec['auc_wide']} frr@0 {rec['frr0_wide']} frr@.5 {rec['frr05_wide']}  best@{rec['best_step']}  {rec['samples_per_s']}/s   [{args}]", flush=True)
    return rec


if __name__ == "__main__":
    import os
    # macOS idle-sleeps after 1 min here and freezes the runs; stay awake while this sweep is alive.
    subprocess.Popen(["caffeinate", "-is", "-w", str(os.getpid())])
    for spec in sys.argv[1:]:
        name, args = spec.split("|", 1)
        run(name.strip(), args.strip())
