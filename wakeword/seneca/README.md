# "Seneca" wake word (microWakeWord, streaming, int8)

English pronunciation only (SEN-e-ka). 134 KB TFLite Micro model, 10 ms feature step, 3x stride.

## Results (synthetic test voices; real voices still to be measured)
Test: 1,450 held-out synthetic "Seneca" clips, ~15 h of dinner-party audio (DiPCo + CHiME-6), 1,585 sound-alikes.
Model chosen by validation score (seed 2 of 3), not by test score.

| probability_cutoff | missed wake words | false wake-ups / hour |
|---|---|---|
| 0.72 (default) | 11.9 % | 0.07 (≈ 1 per 15 h) |
| 0.51 | 8.9 % | 0.20 |
| 0.36 | ~8 % | 0.5 |

Sound-alikes (Senegal, Monica, Sedona, Rebecca, German Senke/Monika/…): ~5–7 % trigger at the default cutoff
(the best "Hey Chat" model: ~29 %). Recipe average over 3 seeds: 8.3 % missed at 0.5 FA/h.

`tensor_arena_size` in the manifest is a conservative estimate. On the ESP32-S3 runtime
(2026-10-28), `arena_used_bytes()` measured 39,436 bytes; the runtime currently reserves 256 KB
in PSRAM. Acoustic triggering and a wake-started voice call passed. Real-voice reliability and
attached-speaker AEC remain unmeasured. `roc_15h_ambient.txt` has the full cutoff table.

## Recipe (reproduce)
```
bash wakeword/train/01t_generate_seneca_en.sh
TAG=sen_en CLIPS=data_seneca_en FEAT_DIR=features_seneca_en SET=EN bash wakeword/fast/run_seneca.sh   # data + baseline
cd wakeword/fast && export CLIPS=data_seneca_en FEAT_DIR=features_seneca_en GROUP_DIR=features_g_seneca_en \
  WW_FEAT=features_seneca_en MWW_PW=128,128,128,128 MWW_FIRST=64
H=$(../work/venv-mww/bin/python -I ablation_args.py EN 0.93 0.5)
python3 sweep.py "sen_en_pw2_w128_s2|--batch 4096 --steps 5000 --schedule cosine --lr 0.003 --warmup 100 \
  --eval-every 500 --pos-weight 2 --seed 2 --train-features $H"
```
Architecture: mixednet, pointwise 128x4, first conv 64/5, kernels [5],[7,11],[9,15],[23], stride 3.
Training data: 14.5k English "Seneca" clips (Piper, 800+10 voices), 16k sound-alikes (hard ones at half penalty),
speech/dinner-party/no-speech negatives; positives weighted 2x.
