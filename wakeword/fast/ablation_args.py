"""Print --train-features for a data ablation: group weights proportional to window counts, with the
same totals as the default config (positives 2, adversarial 3, negatives 10/10/5)."""
import os
import sys
from pathlib import Path

import numpy as np

WORK = Path(__file__).resolve().parents[1] / "work"
GROUP_DIR = os.environ.get("GROUP_DIR", "features_g")  # features_g_seneca for the Seneca word
NEG = ["data/negative_datasets/speech:10", "data/negative_datasets/dinner_party:10", "data/negative_datasets/no_speech:5"]


def n(d):
    return len(np.load(WORK / "pack" / d.replace("/", "__") / "training" / "length.npy"))


def spec(groups, total, penalty=1.0):
    dirs = [f"{GROUP_DIR}/{g}" for g in groups]
    counts = [n(d) for d in dirs]
    return [f"{d}:{total * c / sum(counts):.4f}:{penalty}" for d, c in zip(dirs, counts)]


POS_V1 = ["positive_libritts", "positive_en_onnx", "positive_de_onnx"]
POS_DE = ["positive_libritts_deipa", "positive_en_onnx_deipa"]
ADV_V1 = ["adversarial_libritts", "adversarial_en_onnx", "adversarial_de_onnx", "adversarial_de_ipa"]
ADV_HARD = ["adversarial_libritts_hard"]
SETS = {"A": (POS_V1, ADV_V1), "B": (POS_V1 + POS_DE, ADV_V1), "C": (POS_V1, ADV_V1 + ADV_HARD),
        "D": (POS_V1 + POS_DE, ADV_V1 + ADV_HARD),
        "EN": (["positive_libritts", "positive_en_onnx"], ADV_V1)}  # English-only positives (Seneca)

if __name__ == "__main__":
    # ablation_args.py SET [HARD_WEIGHT [HARD_PENALTY]]  -> optionally add libritts_hard on top
    pos, adv = SETS[sys.argv[1]]
    extra = []
    if len(sys.argv) > 2:
        extra = [f"{GROUP_DIR}/adversarial_libritts_hard:{sys.argv[2]}:{sys.argv[3] if len(sys.argv) > 3 else 1.0}"]
    print(" ".join(spec(pos, 2.0) + spec(adv, 3.0) + extra + NEG))
