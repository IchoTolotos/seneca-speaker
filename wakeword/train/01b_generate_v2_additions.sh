#!/usr/bin/env bash
# v2 additions (run after 01_generate_samples.sh):
#  * German-accented "hey chat" (IPA variants hˈeː tʃˈɛt etc.) spoken by the 800-speaker LibriTTS-R
#    generator and by the accented English ONNX voices (L2-ARCTIC, VCTK, ...) -> more speaker diversity
#    than the 14 German single-speaker voices alone (v1 FRR on German-voice positives was 45 %).
#  * more "hard" adversarial negatives (minimal pairs: hey chap/Chad/check/Chuck, okay/they/say chat ...).
set -euo pipefail
WORK=${WORK:-$(cd "$(dirname "$0")/.." && pwd)/work}
PY=$WORK/venv-piper/bin/python
export PYTHONPATH=$WORK/piper-sample-generator
V=$WORK/dl/voices; OUT=$WORK/data; HERE=$(cd "$(dirname "$0")" && pwd)
gen() { "$PY" -m piper_sample_generator "$@" 2>&1 | grep -vE "DEBUG|UserWarning|return torch|FutureWarning|warnings.warn" || true; }
EN_VOICES=""
for v in en_GB-vctk-medium en_US-l2arctic-medium en_US-arctic-medium en_GB-aru-medium \
         en_GB-semaine-medium en_GB-alba-medium en_GB-northern_english_male-medium \
         en_GB-southern_english_female-low en_US-kusal-medium en_US-reza_ibrahim-medium; do
  EN_VOICES="$EN_VOICES --model $V/$v.onnx"
done
gen "$HERE/phrases/positive_de_ipa.txt" --phoneme-input --model "$WORK/dl/models/en_US-libritts_r-medium.pt" \
  --max-samples 3000 --batch-size 100 --max-speakers 800 \
  --slerp-weights 0.3 0.5 0.7 --length-scales 0.8 0.95 1.1 1.3 \
  --noise-scales 0.667 0.8 0.98 --noise-scale-ws 0.6 0.8 1.0 \
  --output-dir "$OUT/positive/libritts_deipa" &
gen "$HERE/phrases/positive_de_ipa.txt" --phoneme-input $EN_VOICES --max-samples 1500 \
  --length-scales 0.85 1.0 1.2 --noise-scales 0.667 0.85 --noise-scale-ws 0.8 \
  --output-dir "$OUT/positive/en_onnx_deipa" &
gen "$HERE/phrases/adversarial_hard_en.txt" --model "$WORK/dl/models/en_US-libritts_r-medium.pt" \
  --max-samples 5000 --batch-size 100 --max-speakers 800 \
  --slerp-weights 0.3 0.5 0.7 --length-scales 0.8 0.95 1.1 1.3 \
  --noise-scales 0.667 0.8 0.98 --noise-scale-ws 0.6 0.8 1.0 \
  --output-dir "$OUT/adversarial/libritts_hard" &
wait
# "hey Chet Baker" (line 16 of adversarial_hard_en.txt, i.e. sample index % 20 == 15) is too close to the
# German-accent positive variant hˈeɪ tʃˈɛt -> drop those clips again (kept in the phrase file so the
# index -> phrase mapping used by 05_evaluate.py stays valid).
for f in "$OUT"/adversarial/libritts_hard/*.wav; do n=$(basename "$f" .wav); [ $((n % 20)) -eq 15 ] && rm -f "$f"; done
for d in "$OUT"/positive/* "$OUT"/adversarial/*; do echo "$d $(ls "$d" | wc -l)"; done
