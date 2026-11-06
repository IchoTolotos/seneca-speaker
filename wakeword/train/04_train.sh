#!/usr/bin/env bash
# Step 4: write training_parameters.yaml and train the streaming MixedNet model exactly as in
# microWakeWord's basic_training_notebook.ipynb, plus our adversarial-negative feature set.
# Output: $WORK/trained_models/$NAME/tflite_stream_state_internal_quant/stream_state_internal_quant.tflite
# Several runs can train in parallel (one config file per NAME); each uses ~2 cores.
set -euo pipefail
WORK=${WORK:-$(cd "$(dirname "$0")/.." && pwd)/work}
CONFIG=${CONFIG:-v1}   # v1 = notebook defaults (20k steps); v2 = longer, LR decay, SpecAugment, heavier adversarial weight
# v3a = v1 + 40k steps with LR decay; v3b = v3a + adversarial sampling 5 / penalty 1.5 (v2 without SpecAugment)
NAME=${NAME:-hey_chat}
cd "$WORK"

"$WORK/venv-mww/bin/python" -I - "$CONFIG" "$NAME" <<'EOF'
import sys, yaml
cfg, name = sys.argv[1], sys.argv[2]
config = {
    "window_step_ms": 10,
    "train_dir": f"trained_models/{name}",
    "features": [
        {"features_dir": "features/positive", "sampling_weight": 2.0, "penalty_weight": 1.0,
         "truth": True, "truncation_strategy": "truncate_start", "type": "mmap"},
        # synthetic confusable phrases ("hey", "chat", "hey that", "hey Chad", "ja klar", "hey Schatz", ...)
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
    ],
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
if cfg in ("v3a", "v3b"):
    config["training_steps"] = [30000, 10000]
    config["positive_class_weight"] = [1, 1]
    config["negative_class_weight"] = [20, 20]
    config["learning_rates"] = [0.001, 0.0002]
    for k in ("time_mask_max_size", "time_mask_count", "freq_mask_max_size", "freq_mask_count"):
        config[k] = [0, 0]
    if cfg == "v3b":
        config["features"][1]["sampling_weight"] = 5.0
        config["features"][1]["penalty_weight"] = 1.5
if cfg == "v2":
    config["features"][1]["sampling_weight"] = 5.0   # adversarial
    config["features"][1]["penalty_weight"] = 1.5
    config["training_steps"] = [30000, 10000]
    config["positive_class_weight"] = [1, 1]
    config["negative_class_weight"] = [20, 20]
    config["learning_rates"] = [0.001, 0.0002]
    config["time_mask_max_size"] = [5, 5]
    config["time_mask_count"] = [1, 1]
    config["freq_mask_max_size"] = [5, 5]
    config["freq_mask_count"] = [1, 1]
config["train_dir"] = f"trained_models/{name}"
yaml.dump(config, open(f"training_parameters_{name}.yaml", "w"))
EOF

"$WORK/venv-mww/bin/python" -m microwakeword.model_train_eval \
  --training_config="training_parameters_${NAME}.yaml" \
  --train "${TRAIN:-1}" \
  --restore_checkpoint 1 \
  --test_tf_nonstreaming 0 \
  --test_tflite_nonstreaming 0 \
  --test_tflite_nonstreaming_quantized 0 \
  --test_tflite_streaming 0 \
  --test_tflite_streaming_quantized 1 \
  --use_weights "best_weights" \
  mixednet \
  --pointwise_filters "64,64,64,64" \
  --repeat_in_block "1, 1, 1, 1" \
  --mixconv_kernel_sizes '[5], [7,11], [9,15], [23]' \
  --residual_connection "0,0,0,0" \
  --first_conv_filters 32 \
  --first_conv_kernel_size 5 \
  --stride 3
