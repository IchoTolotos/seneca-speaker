#!/usr/bin/env bash
# Rebuild the large, regenerable training data that is deleted between training sessions (~49 GB, ~15 min):
#   1. unpack the negative datasets (speech, no_speech, dinner_party training splits) from dl/neg/*.zip
#   2. pack them for the fast sampler (pack/data__negative_datasets__*)
#   3. Seneca (English) training split = also the quantization calibration set (features_seneca_en/*/training)
#   4. Seneca per-group training features -> pack/features_g_seneca_en__*, then drop the unpacked group copies
# Fresh augmentation means the rebuilt data is equivalent, not bit-identical. Splits are deterministic.
# Each step is skipped when its output already exists.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(cd "$HERE/.." && pwd)/work
PY=$WORK/venv-mww/bin/python
stage() { echo "=== $(date +%H:%M:%S) $*"; }

stage "1/4 unpack negative datasets"
for z in speech no_speech dinner_party; do
  if [ -d "$WORK/data/negative_datasets/$z/training" ]; then echo "exists: $z"; continue; fi
  unzip -q -o "$WORK/dl/neg/$z.zip" -d "$WORK/data/negative_datasets"
done

stage "2/4 pack negative datasets"
(cd "$HERE" && "$PY" -I pack.py)   # default FEATURES; missing dirs (e.g. hey chat features/) are skipped

export CLIPS=data_seneca_en FEAT_DIR=features_seneca_en GROUP_DIR=features_g_seneca_en
stage "3/4 Seneca training split (calibration set)"
SPLITS=training "$PY" -I "$HERE/../train/03_make_features.py" positive adversarial

stage "4/4 Seneca per-group training features + pack"
if ls -d "$WORK"/pack/features_g_seneca_en__* >/dev/null 2>&1; then
  echo "exists: pack/features_g_seneca_en__*"
else
  "$PY" -I "$HERE/make_group_features.py"
  "$PY" -I "$HERE/pack.py" $(cd "$WORK" && ls -d $GROUP_DIR/*)
  rm -rf "$WORK/$GROUP_DIR"   # only the packed copy is read by train_fast.py
fi
stage "done"
