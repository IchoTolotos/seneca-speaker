#!/usr/bin/env python3
"""Step 3: augment the synthetic clips and store microfrontend spectrogram features as RaggedMmaps.

Mirrors the augmentation / SpectrogramGeneration cells of microWakeWord's basic_training_notebook.ipynb
(augmentation_duration_s=3.2, jitter 0.195-0.205 s, slide_frames=10 for train/val, 1 for test,
train repetition 2), but loads WAVs with soundfile instead of `datasets.Audio` (datasets>=4 needs
torchcodec for decoding) and runs augmentation in a multiprocessing pool.

Usage: $WORK/venv-mww/bin/python -I 03_make_features.py positive|adversarial
"""
import multiprocessing as mp
import os
import random
import sys
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy.signal import resample_poly

WORK = Path(os.environ.get("WORK", Path(__file__).resolve().parents[1] / "work"))
DATA = WORK / "data"  # augmentation sources (noise, RIRs) always come from here
CLIPS = WORK / os.environ.get("CLIPS", "data")  # synthetic clips: <CLIPS>/<kind>/<group>/*.wav
FEAT = WORK / os.environ.get("FEAT_DIR", "features")
SPLITS = os.environ.get("SPLITS", "testing,validation,training").split(",")
MAX_DUR_S = 1.3  # after trimming; clip_duration_ms for training is 1500
SEED = 10

AUG = None


def init_worker(seed):
    global AUG
    from microwakeword.audio.augmentation import Augmentation

    np.random.seed(seed + os.getpid() % 10000)
    random.seed(seed + os.getpid() % 10000)
    AUG = Augmentation(
        augmentation_duration_s=3.2,
        augmentation_probabilities={
            "SevenBandParametricEQ": 0.1,
            "TanhDistortion": 0.1,
            "PitchShift": 0.1,
            "BandStopFilter": 0.1,
            "AddColorNoise": 0.1,
            "AddBackgroundNoise": 0.75,
            "Gain": 1.0,
            "RIR": 0.5,
        },
        impulse_paths=[str(DATA / "mit_rirs")],
        background_paths=[str(DATA / "fma_16k"), str(DATA / "audioset_16k")],
        background_min_snr_db=-5,
        background_max_snr_db=10,
        min_jitter_s=0.195,
        max_jitter_s=0.205,
    )


def load_clip(path):
    """Load wav -> float32 16 kHz, trim leading/trailing low-energy audio. Returns None if too long."""
    a, sr = sf.read(path, dtype="float32")
    if a.ndim > 1:
        a = a.mean(axis=1)
    if sr != 16000:
        g = np.gcd(sr, 16000)
        a = resample_poly(a, 16000 // g, sr // g).astype(np.float32)
    # energy trim (10 ms frames, -40 dB relative to peak frame)
    hop = 160
    n = len(a) // hop
    if n == 0:
        return None
    rms = np.sqrt(np.mean(a[: n * hop].reshape(n, hop) ** 2, axis=1) + 1e-12)
    keep = np.where(rms > rms.max() * 0.01)[0]
    a = a[max(0, (keep[0] - 3) * hop): min(len(a), (keep[-1] + 4) * hop)]
    if len(a) / 16000 > MAX_DUR_S:
        return None
    return a


def make_specs(args):
    path, slide_frames, repeat = args
    from microwakeword.audio.audio_utils import generate_features_for_clip

    clip = load_clip(path)
    if clip is None:
        return []
    out = []
    for _ in range(repeat):
        aug = AUG.augment_clip(clip)
        spec = generate_features_for_clip(aug, 10)
        if slide_frames is None or slide_frames == 1:
            out.append(spec)
        else:
            L = spec.shape[0] - slide_frames + 1
            for i in range(slide_frames):
                out.append(spec[i: i + L].copy())
    return out


def build(kind):
    from mmap_ninja.ragged import RaggedMmap

    src = CLIPS / kind
    files = sorted(str(p) for p in src.glob("*/*.wav"))
    rng = random.Random(SEED)
    rng.shuffle(files)
    n = len(files)
    n_test = n_val = int(0.1 * n)
    splits = {
        "testing": (files[:n_test], 1, 1),
        "validation": (files[n_test: n_test + n_val], 10, 1),
        "training": (files[n_test + n_val:], 10, 2 if kind == "positive" else 1),
    }
    with open(FEAT / f"{kind}_splits.txt", "w") as fh:
        for s, (fl, _, _) in splits.items():
            for f in fl:
                fh.write(f"{s}\t{f}\n")
    for split, (fl, slide, rep) in splits.items():
        if split not in SPLITS:
            continue
        out_dir = FEAT / kind / split / f"{kind}_mmap"
        if out_dir.exists():
            print("exists, skipping", out_dir)
            continue
        out_dir.parent.mkdir(parents=True, exist_ok=True)
        with mp.Pool(max(1, os.cpu_count() - 2), initializer=init_worker, initargs=(SEED,)) as pool:
            def gen():
                done = 0
                for specs in pool.imap_unordered(make_specs, [(f, slide, rep) for f in fl], chunksize=8):
                    done += 1
                    if done % 1000 == 0:
                        print(kind, split, done, "/", len(fl), flush=True)
                    yield from specs

            RaggedMmap.from_generator(out_dir=str(out_dir), sample_generator=gen(), batch_size=2000, verbose=False)
        print("wrote", out_dir, flush=True)


if __name__ == "__main__":
    FEAT.mkdir(parents=True, exist_ok=True)
    for k in sys.argv[1:]:
        build(k)
