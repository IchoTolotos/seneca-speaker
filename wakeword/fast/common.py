"""Shared setup for the fast trainer and benchmarks: builds the same flags/config/model as
microWakeWord's model_train_eval.py (mixednet, our 04_train.sh architecture) without its CLI."""
import argparse
import os
import tempfile
from pathlib import Path

import yaml

WORK = Path(os.environ.get("WORK", Path(__file__).resolve().parents[1] / "work"))

# Architecture used by 04_train.sh (kept identical so results are comparable).
MIXEDNET_ARGS = [
    "mixednet",
    # MWW_PW / MWW_FIRST env: wider variants (default = 04_train.sh architecture)
    "--pointwise_filters", os.environ.get("MWW_PW", "64,64,64,64"),
    "--repeat_in_block", "1, 1, 1, 1",
    "--mixconv_kernel_sizes", "[5], [7,11], [9,15], [23]",
    "--residual_connection", "0,0,0,0",
    "--first_conv_filters", os.environ.get("MWW_FIRST", "32"),
    "--first_conv_kernel_size", "5",
    "--stride", "3",
]

FEATURES = [
    {"features_dir": "features/positive", "sampling_weight": 2.0, "penalty_weight": 1.0,
     "truth": True, "truncation_strategy": "truncate_start", "type": "mmap"},
    {"features_dir": "features/adversarial", "sampling_weight": 3.0, "penalty_weight": 1.0,
     "truth": False, "truncation_strategy": "truncate_start", "type": "mmap"},
    {"features_dir": "data/negative_datasets/speech", "sampling_weight": 10.0, "penalty_weight": 1.0,
     "truth": False, "truncation_strategy": "random", "type": "mmap"},
    {"features_dir": "data/negative_datasets/dinner_party", "sampling_weight": 10.0, "penalty_weight": 1.0,
     "truth": False, "truncation_strategy": "random", "type": "mmap"},
    {"features_dir": "data/negative_datasets/no_speech", "sampling_weight": 5.0, "penalty_weight": 1.0,
     "truth": False, "truncation_strategy": "random", "type": "mmap"},
    {"features_dir": "data/negative_datasets/dinner_party_eval", "sampling_weight": 0.0, "penalty_weight": 1.0,
     "truth": False, "truncation_strategy": "split", "type": "mmap"},
]


# WW_FEAT=features_seneca -> validation/testing on that word's features instead of features/ (hey chat)
WW_FEAT = os.environ.get("WW_FEAT", "features")


def base_config(name: str) -> dict:
    """v1 settings from 04_train.sh."""
    feats = [dict(f) for f in FEATURES]
    for f in feats:
        if f["features_dir"].startswith("features/"):
            f["features_dir"] = WW_FEAT + f["features_dir"][len("features"):]
    return {
        "window_step_ms": 10,
        "train_dir": f"trained_models/{name}",
        "features": feats,
        "training_steps": [20000],
        "positive_class_weight": [1],
        "negative_class_weight": [20],
        "learning_rates": [0.001],
        "batch_size": 128,
        "time_mask_max_size": [0],
        "time_mask_count": [0],
        "freq_mask_max_size": [0],
        "freq_mask_count": [0],
        "eval_step_interval": 500,
        "clip_duration_ms": 1500,
        "target_minimization": 0.9,
        "minimization_metric": None,
        "maximization_metric": "average_viable_recall",
    }


def build(cfg: dict):
    """Returns (flags, config, model_module) exactly as model_train_eval would build them."""
    os.chdir(WORK)
    from microwakeword import mixednet
    from microwakeword.model_train_eval import load_config

    fd, path = tempfile.mkstemp(suffix=".yaml", prefix="fast_cfg_")
    with os.fdopen(fd, "w") as fh:
        yaml.dump(cfg, fh)

    parser = argparse.ArgumentParser()
    parser.add_argument("--training_config", default=path)
    sub = parser.add_subparsers(dest="model_name")
    mixednet.model_parameters(sub.add_parser("mixednet"))
    flags = parser.parse_args(["--training_config", path, *MIXEDNET_ARGS])
    config = load_config(flags, mixednet)
    return flags, config, mixednet
