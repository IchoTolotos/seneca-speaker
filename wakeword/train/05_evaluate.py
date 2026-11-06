#!/usr/bin/env python3
"""Step 5: evaluate the quantized streaming model the way ESPHome runs it.

* False accepts / hour on held-out ambient audio (precomputed microWakeWord features):
    - testing_ambient   = DiPCo dinner-party recording   (dinner_party_eval/testing_ambient)
    - validation_ambient = CHiME-6 dev dinner party      (dinner_party_eval/validation_ambient)
  Detection = mean of the last `window` streaming probabilities > cutoff, 25-slice (0.75 s) refractory
  period (microwakeword.test.compute_false_accepts_per_hour).
* False reject rate on the held-out 10 % positive test clips (never seen in training), re-augmented with
  the same augmentation pipeline (noise/RIR), broken down by voice source, plus on clean clips.
* False accept rate on held-out adversarial clips, broken down by phrase.

Usage: $WORK/venv-mww/bin/python -I 05_evaluate.py <model.tflite> [out.json]
"""
import json
import os
import sys
from collections import defaultdict
from pathlib import Path

import numpy as np
from mmap_ninja.ragged import RaggedMmap
from numpy.lib.stride_tricks import sliding_window_view

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
WORK = Path(os.environ.get("WORK", Path(__file__).resolve().parents[1] / "work"))
NEG = WORK / "data" / "negative_datasets" / "dinner_party_eval"

from microwakeword.audio.audio_utils import generate_features_for_clip  # noqa: E402
from microwakeword.inference import Model  # noqa: E402
from microwakeword.test import compute_false_accepts_per_hour  # noqa: E402

import importlib.util  # noqa: E402

_spec = importlib.util.spec_from_file_location("mf", HERE / "03_make_features.py")
mf = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mf)

CUTOFFS = np.round(np.arange(0.50, 1.0, 0.01), 2)
WINDOWS = [3, 5, 7, 10]
STRIDE = 3
IGNORE = 25


def moving_avg(p, w):
    p = np.asarray(p, dtype=np.float32)
    if len(p) < w:
        return np.zeros(1, np.float32)
    return sliding_window_view(p, w).mean(axis=-1)


def ambient_probs(model_path, mmap_dir):
    m = Model(model_path, stride=STRIDE)
    out = []
    for spec in RaggedMmap(str(mmap_dir)):
        out.append(np.asarray(m.predict_spectrogram(spec), np.float32))
    return out


def faph(probs, w):
    avg = [moving_avg(p, w) for p in probs]
    return compute_false_accepts_per_hour(avg, CUTOFFS, IGNORE, stride=STRIDE, step_s=0.01)


def clip_max(model, spec, w):
    p = model.predict_spectrogram(spec)
    return float(moving_avg(p[IGNORE:], w).max())


def main():
    model_path = sys.argv[1]
    out_json = sys.argv[2] if len(sys.argv) > 2 else None
    res = {"cutoffs": CUTOFFS.tolist(), "windows": WINDOWS}

    # ---- ambient FA/h
    for name, d in [("testing_ambient_dipco", NEG / "testing_ambient" / "dipco_u01_ch1_mmap"),
                    ("validation_ambient_chime6", NEG / "validation_ambient" / "chime6_dev_eval_mmap")]:
        probs = ambient_probs(model_path, d)
        hours = sum(len(p) for p in probs) * STRIDE * 0.01 / 3600
        res[name] = {"hours": hours, "faph": {w: faph(probs, w).tolist() for w in WINDOWS}}
        print(f"{name}: {hours:.2f} h", flush=True)

    # ---- positives / adversarial from held-out test split
    mf.init_worker(1234)
    splits = [l.rstrip("\n").split("\t") for l in open(WORK / "features" / "positive_splits.txt")]
    pos_test = [f for s, f in splits if s == "testing"]
    splits = [l.rstrip("\n").split("\t") for l in open(WORK / "features" / "adversarial_splits.txt")]
    adv_test = [f for s, f in splits if s == "testing"]

    phrase_files = {
        "libritts": HERE / "phrases" / "adversarial_en.txt",
        "en_onnx": HERE / "phrases" / "adversarial_en.txt",
        "de_onnx": HERE / "phrases" / "adversarial_de.txt",
        "de_ipa": HERE / "phrases" / "adversarial_de_ipa.txt",
        "libritts_hard": HERE / "phrases" / "adversarial_hard_en.txt",
    }
    phrase_lists = {k: [l.strip() for l in open(v, encoding="utf-8") if l.strip()] for k, v in phrase_files.items()}

    model = Model(model_path, stride=STRIDE)
    pos = defaultdict(lambda: defaultdict(list))  # source -> window -> [max prob]
    pos_clean = defaultdict(list)
    for f in pos_test:
        clip = mf.load_clip(f)
        if clip is None:
            continue
        src = Path(f).parent.name
        spec = generate_features_for_clip(mf.AUG.augment_clip(clip), 10)
        clean = generate_features_for_clip(np.pad(clip, (32000, 3200)), 10)
        for w in WINDOWS:
            pos[src][w].append(clip_max(model, spec, w))
            pos["all"][w].append(clip_max(model, spec, w))
            pos_clean[w].append(clip_max(model, clean, w))

    adv = defaultdict(lambda: defaultdict(list))
    for f in adv_test:
        clip = mf.load_clip(f)
        if clip is None:
            continue
        src = Path(f).parent.name
        idx = int(Path(f).stem)
        phrase = phrase_lists[src][idx % len(phrase_lists[src])]
        key = f"{phrase} [{'de' if src.startswith('de') else 'en'}]"
        spec = generate_features_for_clip(mf.AUG.augment_clip(clip), 10)
        for w in WINDOWS:
            m = clip_max(model, spec, w)
            adv[key][w].append(m)
            adv["ALL"][w].append(m)

    def rate_above(vals, c):
        return float(np.mean(np.asarray(vals) > c)) if vals else float("nan")

    res["frr"] = {src: {w: [1 - rate_above(v[w], c) for c in CUTOFFS] for w in WINDOWS} for src, v in pos.items()}
    res["frr"]["clean_all"] = {w: [1 - rate_above(pos_clean[w], c) for c in CUTOFFS] for w in WINDOWS}
    res["n_pos"] = {src: len(v[WINDOWS[0]]) for src, v in pos.items()}
    res["adversarial_far"] = {k: {w: [rate_above(v[w], c) for c in CUTOFFS] for w in WINDOWS} for k, v in adv.items()}
    res["n_adv"] = {k: len(v[WINDOWS[0]]) for k, v in adv.items()}

    # ---- summary table
    for w in WINDOWS:
        print(f"\n=== sliding window {w} ===")
        print("cutoff  FRR(aug)  FRR(clean)  FA/h DiPCo  FA/h CHiME6  adversarial FAR")
        for i, c in enumerate(CUTOFFS):
            if round(c * 100) % 5 and c < 0.9:
                continue
            print(f"{c:.2f}    {res['frr']['all'][w][i]:.3f}     {res['frr']['clean_all'][w][i]:.3f}       "
                  f"{res['testing_ambient_dipco']['faph'][w][i]:.3f}       {res['validation_ambient_chime6']['faph'][w][i]:.3f}"
                  f"        {res['adversarial_far']['ALL'][w][i]:.3f}")
    if out_json:
        json.dump(res, open(out_json, "w"), indent=1)


if __name__ == "__main__":
    main()
