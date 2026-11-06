#!/usr/bin/env bash
# "Seneca" wake word: same generator, voices and clip counts as the "hey chat" data
# (01_generate_samples.sh + 01b_generate_v2_additions.sh), so the two words compare fairly.
# eSpeak pronounces it sˈɛnɛkə (English voices) and zˈeːneːkɑː (German voices); German-accented
# variants for the English voices come from phrases/seneca/positive_de_ipa.txt.
#
# Output: $WORK/data_seneca/{positive,adversarial}/<group>/*.wav
set -euo pipefail
WORK=${WORK:-$(cd "$(dirname "$0")/.." && pwd)/work}
PY=$WORK/venv-piper/bin/python
export PYTHONPATH=$WORK/piper-sample-generator
V=$WORK/dl/voices
OUT=$WORK/data_seneca
HERE=$(cd "$(dirname "$0")" && pwd)
P=$HERE/phrases/seneca
mkdir -p "$OUT"
gen() { "$PY" -m piper_sample_generator "$@" 2>&1 | grep -vE "DEBUG|UserWarning|return torch|FutureWarning|warnings.warn" || true; }
LIBRITTS=(--model "$WORK/dl/models/en_US-libritts_r-medium.pt" --batch-size 100 --max-speakers 800
          --slerp-weights 0.3 0.5 0.7 --length-scales 0.8 0.95 1.1 1.3
          --noise-scales 0.667 0.8 0.98 --noise-scale-ws 0.6 0.8 1.0)
ONNX_OPTS=(--length-scales 0.85 1.0 1.2 --noise-scales 0.667 0.85 --noise-scale-ws 0.8)
EN_VOICES=()
for v in en_GB-vctk-medium en_US-l2arctic-medium en_US-arctic-medium en_GB-aru-medium \
         en_GB-semaine-medium en_GB-alba-medium en_GB-northern_english_male-medium \
         en_GB-southern_english_female-low en_US-kusal-medium en_US-reza_ibrahim-medium; do
  EN_VOICES+=(--model "$V/$v.onnx")
done
DE_VOICES=()  # de_DE-mls-medium excluded (babbles), see 01_generate_samples.sh
for v in de_DE-thorsten_emotional-medium de_DE-thorsten-medium de_DE-kerstin-low \
         de_DE-ramona-low de_DE-karlsson-low de_DE-pavoque-low de_DE-eva_k-x_low; do
  DE_VOICES+=(--model "$V/$v.onnx")
done

# ---------------- positives (14,500) ----------------
gen "$P/positive_en.txt" "${LIBRITTS[@]}" --max-samples 6000 --output-dir "$OUT/positive/libritts" &
gen "$P/positive_en.txt" "${EN_VOICES[@]}" "${ONNX_OPTS[@]}" --max-samples 2500 --output-dir "$OUT/positive/en_onnx" &
gen "$P/positive_de.txt" "${DE_VOICES[@]}" "${ONNX_OPTS[@]}" --max-samples 1500 --output-dir "$OUT/positive/de_onnx" &
gen "$P/positive_de_ipa.txt" --phoneme-input "${LIBRITTS[@]}" --max-samples 3000 --output-dir "$OUT/positive/libritts_deipa" &
gen "$P/positive_de_ipa.txt" --phoneme-input "${EN_VOICES[@]}" "${ONNX_OPTS[@]}" --max-samples 1500 --output-dir "$OUT/positive/en_onnx_deipa" &
wait
# ---------------- adversarial negatives (16,000) ----------------
gen "$P/adversarial_en.txt" "${LIBRITTS[@]}" --max-samples 6000 --output-dir "$OUT/adversarial/libritts" &
gen "$P/adversarial_en.txt" "${EN_VOICES[@]}" "${ONNX_OPTS[@]}" --max-samples 2000 --output-dir "$OUT/adversarial/en_onnx" &
gen "$P/adversarial_de.txt" "${DE_VOICES[@]}" "${ONNX_OPTS[@]}" --max-samples 2000 --output-dir "$OUT/adversarial/de_onnx" &
gen "$P/adversarial_de_ipa.txt" --phoneme-input "${DE_VOICES[@]}" "${ONNX_OPTS[@]}" --max-samples 1000 --output-dir "$OUT/adversarial/de_ipa" &
gen "$P/adversarial_hard_en.txt" "${LIBRITTS[@]}" --max-samples 5000 --output-dir "$OUT/adversarial/libritts_hard" &
wait
echo "generation done"; for d in "$OUT"/positive/* "$OUT"/adversarial/*; do echo "$d $(ls "$d" | wc -l)"; done
