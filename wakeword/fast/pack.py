"""One-time: repack each feature provider's split into one flat uint16 file for fast in-RAM sampling.

$WORK/pack/<features_dir with / -> __>/<split>/{frames.u16, start.npy, length.npy}
  frames.u16  (total_frames, 40) uint16, value = feature / 0.0390625 (lossless for microfrontend features)
  start, length  per clip, in frames

Usage: python -I pack.py [split ...]   (default: training)
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np  # noqa: E402
from mmap_ninja.ragged import RaggedMmap  # noqa: E402

from common import FEATURES, WORK  # noqa: E402

NUM_FEATURES = 40
SCALE = 0.0390625


def pack_dir(features_dir: str, split: str) -> Path:
    return WORK / "pack" / features_dir.replace("/", "__") / split


def pack(features_dir: str, split: str):
    out = pack_dir(features_dir, split)
    mmaps = sorted(Path(WORK, features_dir, split).glob("**/*_mmap"))
    if not mmaps:
        return
    if (out / "length.npy").exists():
        print("exists", out)
        return
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    starts, lengths, offset = [], [], 0
    with open(out / "frames.u16.tmp", "wb") as fh:
        for m in mmaps:
            rm = RaggedMmap(str(m))
            flat = rm.memmap
            if flat.dtype == np.uint16:
                frames = np.asarray(flat)
            else:
                frames = np.round(np.asarray(flat, dtype=np.float32) / SCALE).astype(np.uint16)
            frames.tofile(fh)
            s = np.asarray(rm.starts) // NUM_FEATURES
            ln = (np.asarray(rm.ends) - np.asarray(rm.starts)) // NUM_FEATURES
            starts.append(s + offset)
            lengths.append(ln)
            offset += len(frames) // NUM_FEATURES
    np.save(out / "start.npy", np.concatenate(starts).astype(np.int64))
    np.save(out / "length.npy", np.concatenate(lengths).astype(np.int64))
    (out / "frames.u16.tmp").rename(out / "frames.u16")
    print(f"packed {features_dir}/{split}: {offset} frames, {offset * 80 / 1e9:.2f} GB, {time.time() - t0:.0f}s",
          flush=True)


if __name__ == "__main__":
    # python pack.py                       -> training split of the default FEATURES
    # python pack.py features_g/x ...      -> training split of the given feature dirs
    dirs = [d for d in sys.argv[1:] if "/" in d]
    if dirs:
        for d in dirs:
            pack(d, "training")
    else:
        for split in (sys.argv[1:] or ["training"]):
            for f in FEATURES:
                pack(f["features_dir"], split)
