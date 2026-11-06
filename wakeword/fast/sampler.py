"""Vectorized replacement for microWakeWord's FeatureHandler.get_data("training", ...).

Same semantics, per sample:
  provider ~ sampling_weight, clip ~ uniform within provider,
  crop: truncate_start = last L frames, random = offset in [0, len-L), shorter clips left-padded with 0,
  uint16 features scaled by 0.0390625, optional SpecAugment (time/freq masks set to 0).
But a whole batch is one fancy-index gather per mmap instead of 128 Python calls.
"""
from pathlib import Path

import numpy as np
from mmap_ninja.ragged import RaggedMmap

NUM_FEATURES = 40
UINT16_SCALE = 0.0390625


class _Source:
    """One RaggedMmap viewed as a flat (frames, 40) array plus per-clip frame offsets."""

    def __init__(self, path: Path):
        rm = RaggedMmap(str(path))
        flat = rm.memmap
        self.frames = flat.reshape(-1, NUM_FEATURES)
        self.scale = UINT16_SCALE if flat.dtype == np.uint16 else None
        self.start = (np.asarray(rm.starts) // NUM_FEATURES).astype(np.int64)
        self.length = ((np.asarray(rm.ends) - np.asarray(rm.starts)) // NUM_FEATURES).astype(np.int64)


class _PackedSource:
    """pack.py output: one flat uint16 file per provider/split (shared via the OS page cache)."""

    def __init__(self, path: Path, warm: bool):
        self.frames = np.memmap(path / "frames.u16", dtype=np.uint16, mode="r").reshape(-1, NUM_FEATURES)
        self.scale = UINT16_SCALE
        self.start = np.load(path / "start.npy")
        self.length = np.load(path / "length.npy")
        if warm:  # touch every page once so random gathers never hit the SSD
            step = 4096 // 2 // NUM_FEATURES * 64
            int(self.frames[::step, 0].sum())


class _Provider:
    def __init__(self, root: Path, feat: dict, split: str, packed: bool = True, warm: bool = True):
        pdir = Path(root, "pack", feat["features_dir"].replace("/", "__"), split)
        if packed and (pdir / "length.npy").exists():
            self.sources = [_PackedSource(pdir, warm)]
        else:
            dirs = sorted(Path(root, feat["features_dir"], split).glob("**/*_mmap"))
            self.sources = [_Source(d) for d in dirs]
        self.label = float(feat["truth"])
        self.weight = float(feat["sampling_weight"])
        self.penalty = float(feat["penalty_weight"])
        self.strategy = feat["truncation_strategy"]
        # global clip index -> (source id, local index)
        counts = [len(s.start) for s in self.sources]
        self.n = int(sum(counts))
        self.src_of = np.repeat(np.arange(len(self.sources)), counts) if self.n else np.zeros(0, np.int64)
        self.local_of = np.concatenate([np.arange(c) for c in counts]) if self.n else np.zeros(0, np.int64)


class FastSampler:
    def __init__(self, root, features: list, length: int, split: str = "training", seed: int = 0,
                 packed: bool = True, warm: bool = True):
        self.L = length
        self.rng = np.random.default_rng(seed)
        self.providers = [p for p in (_Provider(Path(root), f, split, packed, warm)
                                      for f in features if f["sampling_weight"] > 0)
                          if p.n > 0]
        w = np.array([p.weight for p in self.providers])
        self.p = w / w.sum()
        self.ar = np.arange(length, dtype=np.int64)

    def _gather(self, prov: _Provider, clip_ids: np.ndarray, out: np.ndarray):
        L, ar = self.L, self.ar
        src_ids = prov.src_of[clip_ids]
        for s in np.unique(src_ids):
            sel = np.nonzero(src_ids == s)[0]
            src = prov.sources[s]
            loc = prov.local_of[clip_ids[sel]]
            start, length = src.start[loc], src.length[loc]
            longer = length > L
            if prov.strategy == "random":
                span = np.maximum(length - L, 1)
                off = np.where(longer, (self.rng.random(len(loc)) * span).astype(np.int64), 0)
            else:  # truncate_start (training default for our positives/adversarial)
                off = np.where(longer, length - L, 0)
            pad = np.where(longer, 0, L - length)  # left padding for short clips
            rel = ar[None, :] - pad[:, None]  # (n, L) frame index within clip, <0 => padding
            idx = start[:, None] + off[:, None] + np.maximum(rel, 0)
            block = src.frames[idx]  # (n, L, 40) gather
            block = block.astype(np.float32, copy=False)
            if src.scale is not None:
                block *= src.scale
            if (rel < 0).any():
                block[rel < 0] = 0.0
            out[sel] = block

    def batch(self, B: int, time_mask_max=0, time_mask_count=0, freq_mask_max=0, freq_mask_count=0):
        x = np.empty((B, self.L, NUM_FEATURES), np.float32)
        y = np.empty((B,), np.float32)
        w = np.empty((B,), np.float32)
        which = self.rng.choice(len(self.providers), size=B, p=self.p)
        for k, prov in enumerate(self.providers):
            rows = np.nonzero(which == k)[0]
            if not len(rows):
                continue
            clip_ids = self.rng.integers(0, prov.n, size=len(rows))
            tmp = np.empty((len(rows), self.L, NUM_FEATURES), np.float32)
            self._gather(prov, clip_ids, tmp)
            x[rows], y[rows], w[rows] = tmp, prov.label, prov.penalty
        if time_mask_count and time_mask_max:
            for _ in range(time_mask_count):
                t = (self.rng.random(B) * time_mask_max).astype(np.int64)
                t0 = (self.rng.random(B) * (self.L - t + 1)).astype(np.int64)
                m = (self.ar[None, :] >= t0[:, None]) & (self.ar[None, :] < (t0 + t)[:, None])
                x[m] = 0.0
        if freq_mask_count and freq_mask_max:
            fr = np.arange(NUM_FEATURES)
            for _ in range(freq_mask_count):
                f = (self.rng.random(B) * freq_mask_max).astype(np.int64)
                f0 = (self.rng.random(B) * (NUM_FEATURES - f + 1)).astype(np.int64)
                m = (fr[None, :] >= f0[:, None]) & (fr[None, :] < (f0 + f)[:, None])
                x.transpose(0, 2, 1)[m] = 0.0
        return x, y, w
