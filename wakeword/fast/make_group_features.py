"""Build TRAINING features per data group (e.g. positive/libritts_deipa) so experiments can switch
groups on/off and reweight them. Uses the split assignment in features/<kind>_splits.txt, so the
validation/testing sets stay the fixed combined ones and results remain comparable.
Same augmentation and slide/repeat settings as 03_make_features.py.

Output: $WORK/features_g/<kind>_<group>/training/<kind>_<group>_mmap  (then run pack.py-style packing)
Usage: $WORK/venv-mww/bin/python -I make_group_features.py [kind/group ...]   (default: all groups)
"""
import importlib
import multiprocessing as mp
import os
import sys
from collections import defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "train"))  # spawn workers inherit sys.path and re-import by name
mf = importlib.import_module("03_make_features")


def groups():
    out = defaultdict(list)
    for kind in ("positive", "adversarial"):
        for line in open(mf.FEAT / f"{kind}_splits.txt"):
            split, path = line.rstrip("\n").split("\t")
            if split == "training":
                out[f"{kind}/{Path(path).parent.name}"].append(path)
    return out


def build(key, files):
    from mmap_ninja.ragged import RaggedMmap

    kind, group = key.split("/")
    name = f"{kind}_{group}"
    out_dir = mf.WORK / os.environ.get("GROUP_DIR", "features_g") / name / "training" / f"{name}_mmap"
    if out_dir.exists():
        print("exists", out_dir)
        return
    out_dir.parent.mkdir(parents=True, exist_ok=True)
    rep = 2 if kind == "positive" else 1
    with mp.Pool(max(1, os.cpu_count() - 2), initializer=mf.init_worker, initargs=(mf.SEED,)) as pool:
        def gen():
            for specs in pool.imap_unordered(mf.make_specs, [(f, 10, rep) for f in files], chunksize=8):
                yield from specs

        RaggedMmap.from_generator(out_dir=str(out_dir), sample_generator=gen(), batch_size=2000, verbose=False)
    print("wrote", out_dir, len(files), "clips", flush=True)


if __name__ == "__main__":
    g = groups()
    for key in (sys.argv[1:] or sorted(g)):
        build(key, g[key])
