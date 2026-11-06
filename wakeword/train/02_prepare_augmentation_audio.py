#!/usr/bin/env python3
"""Step 2: download/convert augmentation audio to 16 kHz mono int16 WAV.

Same sources as microWakeWord's basic_training_notebook.ipynb:
  * MIT environmental impulse responses (davidscripka/MIT_environmental_impulse_responses, 270 RIRs)
  * AudioSet balanced-train (agkphysics/AudioSet) -- notebook used bal_train09.tar; the HF repo now
    ships parquet shards, so we use data/bal_train/09.parquet (+ 21.parquet if present), 500 x 10 s clips each
  * FMA extra-small (mchl914/fma_xsmall, fma_xs.zip, 210 x 30 s music clips)

Run with the microWakeWord venv:  $WORK/venv-mww/bin/python -I 02_prepare_augmentation_audio.py
"""
import io
import json
import os
import subprocess
import sys
import urllib.request
from pathlib import Path

import numpy as np
import pyarrow.parquet as pq
import scipy.io.wavfile
import soundfile as sf
from scipy.signal import resample_poly

WORK = Path(os.environ.get("WORK", Path(__file__).resolve().parents[1] / "work"))
DL = WORK / "dl"
DATA = WORK / "data"


def to_16k_int16(audio: np.ndarray, sr: int) -> np.ndarray:
    if audio.ndim > 1:
        audio = audio.mean(axis=1)
    if sr != 16000:
        g = np.gcd(sr, 16000)
        audio = resample_poly(audio, 16000 // g, sr // g)
    return (np.clip(audio, -1, 1) * 32767).astype(np.int16)


def mit_rirs():
    out = DATA / "mit_rirs"
    out.mkdir(parents=True, exist_ok=True)
    api = "https://huggingface.co/api/datasets/davidscripka/MIT_environmental_impulse_responses/tree/main/16khz"
    files = [f["path"] for f in json.load(urllib.request.urlopen(api))]
    for p in files:
        dst = out / Path(p).name
        if dst.exists() and dst.stat().st_size > 0:
            continue
        url = "https://huggingface.co/datasets/davidscripka/MIT_environmental_impulse_responses/resolve/main/" + p
        subprocess.run(["curl", "-sSL", "--http1.1", "--retry", "5", "-o", str(dst), url], check=True)
    # normalise to int16 16 kHz
    for f in out.glob("*.wav"):
        a, sr = sf.read(f, dtype="float32")
        if sr != 16000 or a.dtype != np.int16:
            scipy.io.wavfile.write(f, 16000, to_16k_int16(a, sr))
    print("mit_rirs:", len(list(out.glob("*.wav"))))


def audioset():
    out = DATA / "audioset_16k"
    out.mkdir(parents=True, exist_ok=True)
    for shard in sorted((DL / "audioset").glob("*.parquet")):
        try:
            pf = pq.ParquetFile(shard)
        except Exception as e:  # incomplete download
            print("skip", shard, e)
            continue
        for rg in range(pf.num_row_groups):
            for row in pf.read_row_group(rg, columns=["video_id", "audio"]).to_pylist():
                dst = out / f"{row['video_id']}.wav"
                if dst.exists():
                    continue
                try:
                    a, sr = sf.read(io.BytesIO(row["audio"]["bytes"]), dtype="float32")
                except Exception:
                    # fall back to ffmpeg for formats libsndfile can't read
                    p = subprocess.run(["ffmpeg", "-v", "quiet", "-i", "pipe:0", "-ac", "1", "-ar", "16000", "-f", "s16le", "pipe:1"],
                                       input=row["audio"]["bytes"], capture_output=True)
                    if not p.stdout:
                        continue
                    scipy.io.wavfile.write(dst, 16000, np.frombuffer(p.stdout, np.int16))
                    continue
                scipy.io.wavfile.write(dst, 16000, to_16k_int16(a, sr))
    print("audioset_16k:", len(list(out.glob("*.wav"))))


def fma():
    out = DATA / "fma_16k"
    out.mkdir(parents=True, exist_ok=True)
    for mp3 in (DL / "fma").glob("**/*.mp3"):
        dst = out / (mp3.stem + ".wav")
        if dst.exists():
            continue
        subprocess.run(["ffmpeg", "-v", "quiet", "-y", "-i", str(mp3), "-ac", "1", "-ar", "16000", "-sample_fmt", "s16", str(dst)])
    print("fma_16k:", len(list(out.glob("*.wav"))))


if __name__ == "__main__":
    steps = sys.argv[1:] or ["rir", "audioset", "fma"]
    if "rir" in steps:
        mit_rirs()
    if "audioset" in steps:
        audioset()
    if "fma" in steps:
        fma()
