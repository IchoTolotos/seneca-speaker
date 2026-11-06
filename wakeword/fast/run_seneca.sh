#!/usr/bin/env bash
# Seneca end to end: features (same augmentation/splits as hey chat) -> per-group training features
# -> pack -> best hey-chat recipe (Bh_5k) with 3 seeds (2 on Metal, 1 on CPU in parallel).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(cd "$HERE/.." && pwd)/work
PY=$WORK/venv-mww/bin/python
# override for variants, e.g. TAG=sen_en CLIPS=data_seneca_en FEAT_DIR=features_seneca_en SET=EN
export CLIPS=${CLIPS:-data_seneca} FEAT_DIR=${FEAT_DIR:-features_seneca}
export GROUP_DIR=${GROUP_DIR:-features_g_${FEAT_DIR#features_}} WW_FEAT=$FEAT_DIR
TAG=${TAG:-sen}; SET=${SET:-B}; LOG=$WORK/$CLIPS
stage() { echo "=== $(date +%H:%M:%S) $*"; }

stage "1/4 features (all splits: training is also the quantization calibration set)"
"$PY" -I "$HERE/../train/03_make_features.py" positive adversarial
stage "2/4 per-group training features"
"$PY" -I "$HERE/make_group_features.py"
stage "3/4 pack"
"$PY" -I "$HERE/pack.py" $(cd "$WORK" && ls -d $GROUP_DIR/*)
stage "4/4 train + test (Bh_5k recipe, 3 seeds)"
H=$("$PY" -I "$HERE/ablation_args.py" $SET 0.93 0.5)
R="--batch 4096 --steps 5000 --schedule cosine --lr 0.003 --warmup 100 --eval-every 500"
cd "$HERE"
python3 sweep.py "${TAG}_Bh_5k_s2_cpu|$R --device cpu --seed 2 --train-features $H" > "$LOG/sweep_cpu.log" 2>&1 &
python3 sweep.py "${TAG}_Bh_5k_s0|$R --seed 0 --train-features $H" "${TAG}_Bh_5k_s1|$R --seed 1 --train-features $H"
wait
cat "$LOG/sweep_cpu.log"
stage "done"
