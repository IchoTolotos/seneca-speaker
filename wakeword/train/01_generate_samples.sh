#!/usr/bin/env bash
# Step 1: synthesize positive ("hey chat") and adversarial negative clips with
# piper-sample-generator v3.2.0 (https://github.com/rhasspy/piper-sample-generator).
#
# Requires: $WORK/venv-piper (see README "Setup"), generator .pt + voices in $WORK/dl.
# Output:   $WORK/data/positive/{libritts,en_onnx,de_onnx} and $WORK/data/adversarial/{...}
set -euo pipefail
WORK=${WORK:-$(cd "$(dirname "$0")/.." && pwd)/work}
PY=$WORK/venv-piper/bin/python
export PYTHONPATH=$WORK/piper-sample-generator   # .pt generator pickles reference piper_train
V=$WORK/dl/voices
OUT=$WORK/data
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
N_POS_LIBRITTS=${N_POS_LIBRITTS:-6000}
N_POS_EN_ONNX=${N_POS_EN_ONNX:-2500}
N_POS_DE_ONNX=${N_POS_DE_ONNX:-1500}
N_NEG_LIBRITTS=${N_NEG_LIBRITTS:-6000}
N_NEG_EN_ONNX=${N_NEG_EN_ONNX:-2000}
N_NEG_DE_ONNX=${N_NEG_DE_ONNX:-2000}

gen() { "$PY" -m piper_sample_generator "$@" 2>&1 | grep -vE "DEBUG|UserWarning|return torch|FutureWarning|warnings.warn" || true; }

EN_VOICES=""
for v in en_GB-vctk-medium en_US-l2arctic-medium en_US-arctic-medium en_GB-aru-medium \
         en_GB-semaine-medium en_GB-alba-medium en_GB-northern_english_male-medium \
         en_GB-southern_english_female-low en_US-kusal-medium en_US-reza_ibrahim-medium; do
  EN_VOICES="$EN_VOICES --model $V/$v.onnx"
done
# NOTE: de_DE-mls-medium (both the .pt generator and the .onnx voice) produces
# multi-second babble even for plain German text -> excluded. Single-speaker
# German voices below are clean.
DE_VOICES=""
for v in de_DE-thorsten_emotional-medium de_DE-thorsten-medium de_DE-kerstin-low \
         de_DE-ramona-low de_DE-karlsson-low de_DE-pavoque-low de_DE-eva_k-x_low; do
  DE_VOICES="$DE_VOICES --model $V/$v.onnx"
done

# ---------------- positives ----------------
# (a) LibriTTS-R generator: 800 speakers, SLERP speaker mixing (MPS accelerated)
gen "$HERE/phrases/positive_en.txt" --model "$WORK/dl/models/en_US-libritts_r-medium.pt" \
  --max-samples "$N_POS_LIBRITTS" --batch-size 100 --max-speakers 800 \
  --slerp-weights 0.3 0.5 0.7 --length-scales 0.8 0.95 1.1 1.3 \
  --noise-scales 0.667 0.8 0.98 --noise-scale-ws 0.6 0.8 1.0 \
  --output-dir "$OUT/positive/libritts" &
# (b) accent-diverse English ONNX voices (UK/Scottish/Irish/L2 English ...)
gen "$HERE/phrases/positive_en.txt" $EN_VOICES --max-samples "$N_POS_EN_ONNX" \
  --length-scales 0.85 1.0 1.2 --noise-scales 0.667 0.85 --noise-scale-ws 0.8 \
  --output-dir "$OUT/positive/en_onnx" &
# (c) German voices speaking English-ish "hey chat" via IPA (German-accented variants)
gen "$HERE/phrases/positive_de_ipa.txt" --phoneme-input $DE_VOICES --max-samples "$N_POS_DE_ONNX" \
  --length-scales 0.85 1.0 1.2 --noise-scales 0.667 0.85 --noise-scale-ws 0.8 \
  --output-dir "$OUT/positive/de_onnx" &
wait

# ---------------- adversarial negatives ----------------
gen "$HERE/phrases/adversarial_en.txt" --model "$WORK/dl/models/en_US-libritts_r-medium.pt" \
  --max-samples "$N_NEG_LIBRITTS" --batch-size 100 --max-speakers 800 \
  --slerp-weights 0.3 0.5 0.7 --length-scales 0.8 0.95 1.1 1.3 \
  --noise-scales 0.667 0.8 0.98 --noise-scale-ws 0.6 0.8 1.0 \
  --output-dir "$OUT/adversarial/libritts" &
gen "$HERE/phrases/adversarial_en.txt" $EN_VOICES --max-samples "$N_NEG_EN_ONNX" \
  --length-scales 0.85 1.0 1.2 --noise-scales 0.667 0.85 --noise-scale-ws 0.8 \
  --output-dir "$OUT/adversarial/en_onnx" &
gen "$HERE/phrases/adversarial_de.txt" $DE_VOICES --max-samples "$N_NEG_DE_ONNX" \
  --length-scales 0.85 1.0 1.2 --noise-scales 0.667 0.85 --noise-scale-ws 0.8 \
  --output-dir "$OUT/adversarial/de_onnx" &
# German voices saying English-ish confusables (hey / chat / hey jack / hey check ...) via IPA
gen "$HERE/phrases/adversarial_de_ipa.txt" --phoneme-input $DE_VOICES --max-samples 1000 \
  --length-scales 0.85 1.0 1.2 --noise-scales 0.667 0.85 --noise-scale-ws 0.8 \
  --output-dir "$OUT/adversarial/de_ipa" &
wait
echo "generation done"; for d in "$OUT"/positive/* "$OUT"/adversarial/*; do echo "$d $(ls "$d" | wc -l)"; done
