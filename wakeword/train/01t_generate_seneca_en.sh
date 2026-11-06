#!/usr/bin/env bash
# "Seneca", English pronunciation only (sˈɛnɛkə): 14,500 positives from the English voices
# (same total as hey chat), adversarial clips shared with data_seneca (symlinked).
set -euo pipefail
WORK=${WORK:-$(cd "$(dirname "$0")/.." && pwd)/work}
PY=$WORK/venv-piper/bin/python
export PYTHONPATH=$WORK/piper-sample-generator
V=$WORK/dl/voices; OUT=$WORK/data_seneca_en; HERE=$(cd "$(dirname "$0")" && pwd); P=$HERE/phrases/seneca
mkdir -p "$OUT/positive" "$OUT/adversarial"
gen() { "$PY" -m piper_sample_generator "$@" 2>&1 | grep -vE "DEBUG|UserWarning|return torch|FutureWarning|warnings.warn" || true; }
EN_VOICES=()
for v in en_GB-vctk-medium en_US-l2arctic-medium en_US-arctic-medium en_GB-aru-medium \
         en_GB-semaine-medium en_GB-alba-medium en_GB-northern_english_male-medium \
         en_GB-southern_english_female-low en_US-kusal-medium en_US-reza_ibrahim-medium; do
  EN_VOICES+=(--model "$V/$v.onnx")
done
gen "$P/positive_en.txt" --model "$WORK/dl/models/en_US-libritts_r-medium.pt" --batch-size 100 --max-speakers 800 \
  --slerp-weights 0.3 0.5 0.7 --length-scales 0.8 0.95 1.1 1.3 --noise-scales 0.667 0.8 0.98 --noise-scale-ws 0.6 0.8 1.0 \
  --max-samples 10000 --output-dir "$OUT/positive/libritts" &
gen "$P/positive_en.txt" "${EN_VOICES[@]}" --length-scales 0.85 1.0 1.2 --noise-scales 0.667 0.85 --noise-scale-ws 0.8 \
  --max-samples 4500 --output-dir "$OUT/positive/en_onnx" &
wait
for d in "$WORK"/data_seneca/adversarial/*; do ln -sfn "$d" "$OUT/adversarial/$(basename "$d")"; done
for d in "$OUT"/positive/* "$OUT"/adversarial/*; do echo "$d $(ls "$d"/ | wc -l)"; done
