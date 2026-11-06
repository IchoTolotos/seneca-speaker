"""Fast microWakeWord trainer: same model, data semantics and best-model metric as the stock
`train.train()`, but
  * batches come from the vectorized FastSampler, built by background threads (prefetch queue),
  * any batch size (large batches make Metal worthwhile), any device (Metal GPU or CPU),
  * validation arrays are built once and cached instead of on every eval,
  * eval every `--eval-every` steps.
Writes best_weights/last_weights into trained_models/<name>/ so the stock exporter/test can run on it.

Usage (Metal):  CUDA_VISIBLE_DEVICES=0 $WORK/venv-metal/bin/python -I train_fast.py --name x --batch 4096 --steps 1000
"""
import argparse
import json
import os
import queue
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

ap = argparse.ArgumentParser()
ap.add_argument("--name", required=True)
ap.add_argument("--batch", type=int, default=4096)
ap.add_argument("--steps", type=int, default=1000, help="total optimizer steps")
ap.add_argument("--lr", type=float, default=0.001)
ap.add_argument("--lr-final", type=float, default=None, help="if set: LR for the last --decay-frac of steps")
ap.add_argument("--decay-frac", type=float, default=0.25)
ap.add_argument("--schedule", default="step", choices=["step", "cosine"],
                help="step: --lr then --lr-final; cosine: linear warm-up to --lr, cosine decay to --lr-min")
ap.add_argument("--warmup", type=int, default=0, help="warm-up steps (cosine schedule)")
ap.add_argument("--lr-min", type=float, default=0.0)
ap.add_argument("--neg-weight", type=float, default=20.0)
ap.add_argument("--pos-weight", type=float, default=1.0)
ap.add_argument("--adv-sampling", type=float, default=None, help="override adversarial sampling_weight")
ap.add_argument("--adv-penalty", type=float, default=None)
ap.add_argument("--time-mask", type=int, nargs=2, default=[0, 0], metavar=("MAX", "COUNT"))
ap.add_argument("--freq-mask", type=int, nargs=2, default=[0, 0], metavar=("MAX", "COUNT"))
ap.add_argument("--train-features", nargs="+", default=None, metavar="DIR:WEIGHT[:PENALTY]",
                help="training sources instead of the default combined ones, e.g. "
                     "features_g/positive_libritts:2 features_g/adversarial_libritts_hard:1:1.5 "
                     "data/negative_datasets/speech:10 (truth/strategy inferred from the path)")
ap.add_argument("--ema", type=float, default=0.0, help="EMA decay of weights (e.g. 0.99); evaluated/saved instead of raw")
ap.add_argument("--eval-every", type=int, default=100)
ap.add_argument("--workers", type=int, default=6)
ap.add_argument("--prefetch", type=int, default=8)
ap.add_argument("--device", default="gpu", choices=["gpu", "cpu"])
ap.add_argument("--seed", type=int, default=0)
a = ap.parse_args()

if a.device == "gpu":
    os.environ.setdefault("CUDA_VISIBLE_DEVICES", "0")  # microWakeWord hides the Mac GPU otherwise

import numpy as np  # noqa: E402
import tensorflow as tf  # noqa: E402

if a.device == "cpu":
    tf.config.set_visible_devices([], "GPU")

from common import WORK, base_config, build  # noqa: E402
from sampler import FastSampler  # noqa: E402

T_START = time.perf_counter()
timings = {}


def mark(k, t0):
    timings[k] = timings.get(k, 0.0) + time.perf_counter() - t0


cfg = base_config(a.name)
cfg["batch_size"] = a.batch
if a.adv_sampling is not None:
    cfg["features"][1]["sampling_weight"] = a.adv_sampling
if a.adv_penalty is not None:
    cfg["features"][1]["penalty_weight"] = a.adv_penalty
flags, config, mixednet = build(cfg)
os.makedirs(config["train_dir"], exist_ok=True)
L = config["spectrogram_length"]
val_config = dict(config)  # validation always on the fixed combined sets (comparable across runs)
train_features = config["features"]
if a.train_features:
    train_features = []
    for spec in a.train_features:
        parts = spec.split(":")
        d, wgt = parts[0], float(parts[1])
        pen = float(parts[2]) if len(parts) > 2 else 1.0
        positive = "/positive" in d or d.split("/")[-1].startswith("positive")
        negative_long = "negative_datasets" in d
        train_features.append({"features_dir": d, "sampling_weight": wgt, "penalty_weight": pen,
                               "truth": positive, "type": "mmap",
                               "truncation_strategy": "random" if negative_long else "truncate_start"})

# ---------- data ----------
t0 = time.perf_counter()
samplers = [FastSampler(WORK, train_features, L, seed=a.seed * 1000 + i, warm=(i == 0)) for i in range(a.workers)]
mark("sampler_init", t0)
aug = dict(time_mask_max=a.time_mask[0], time_mask_count=a.time_mask[1],
           freq_mask_max=a.freq_mask[0], freq_mask_count=a.freq_mask[1])
q: "queue.Queue" = queue.Queue(maxsize=a.prefetch)
stop = threading.Event()


def producer(s):
    while not stop.is_set():
        x, y, w = s.batch(a.batch, **aug)
        w = w * np.where(y > 0.5, a.pos_weight, a.neg_weight).astype(np.float32)
        q.put((x, y.reshape(-1, 1), w))


threads = [threading.Thread(target=producer, args=(s,), daemon=True) for s in samplers]
for th in threads:
    th.start()

# Validation: stock metric, arrays cached once.
from microwakeword import data as input_data  # noqa: E402
from microwakeword import train as mww_train  # noqa: E402

t0 = time.perf_counter()
dp = input_data.FeatureHandler(val_config)
_orig_get_data, _cache = dp.get_data, {}


def cached_get_data(mode, *args, **kw):
    if mode == "training":
        return _orig_get_data(mode, *args, **kw)
    key = (mode, kw.get("truncation_strategy"))
    if key not in _cache:
        _cache[key] = _orig_get_data(mode, *args, **kw)
    return _cache[key]


dp.get_data = cached_get_data
mark("featurehandler_init", t0)

# ---------- model ----------
model = mixednet.model(flags, config["training_input_shape"], a.batch)
eval_model = mixednet.model(flags, config["training_input_shape"], config.get("eval_batch", 1024))
cutoffs = np.linspace(0.0, 1.0, 101).tolist()
eval_model.compile(
    optimizer=tf.keras.optimizers.Adam(), loss=tf.keras.losses.BinaryCrossentropy(),
    metrics=[tf.keras.metrics.BinaryAccuracy(name="accuracy"), tf.keras.metrics.Recall(name="recall"),
             tf.keras.metrics.Precision(name="precision"),
             tf.keras.metrics.TruePositives(name="tp", thresholds=cutoffs),
             tf.keras.metrics.FalsePositives(name="fp", thresholds=cutoffs),
             tf.keras.metrics.TrueNegatives(name="tn", thresholds=cutoffs),
             tf.keras.metrics.FalseNegatives(name="fn", thresholds=cutoffs),
             tf.keras.metrics.AUC(name="auc"), tf.keras.metrics.BinaryCrossentropy(name="loss")])
opt = tf.keras.optimizers.Adam(learning_rate=a.lr)
loss_fn = tf.keras.losses.BinaryCrossentropy()


@tf.function
def train_step(x, y, w):
    with tf.GradientTape() as tape:
        p = model(x, training=True)
        loss = loss_fn(y, p, sample_weight=w)
    grads = tape.gradient(loss, model.trainable_variables)
    opt.apply_gradients(zip(grads, model.trainable_variables))
    return loss


ema_vars = [tf.Variable(v, trainable=False) for v in model.weights] if a.ema > 0 else None
ema_float = [tf.as_dtype(v.dtype).is_floating for v in model.weights]  # Keras 3: v.dtype is a str


@tf.function
def ema_update():
    for e, v, f in zip(ema_vars, model.weights, ema_float):
        if f:
            e.assign(a.ema * e + (1.0 - a.ema) * v)
        else:
            e.assign(v)


def eval_weights():
    return [e.numpy() for e in ema_vars] if ema_vars is not None else model.get_weights()


best = {"metric": -1.0, "step": 0}
history = []
decay_start = int(a.steps * (1 - a.decay_frac)) if a.lr_final is not None else a.steps + 1
t_train = time.perf_counter()
import math  # noqa: E402


def lr_at(step):
    if a.schedule == "cosine":
        if step <= a.warmup:
            return a.lr * step / max(1, a.warmup)
        frac = (step - a.warmup) / max(1, a.steps - a.warmup)
        return a.lr_min + 0.5 * (a.lr - a.lr_min) * (1 + math.cos(math.pi * frac))
    return a.lr_final if (a.lr_final is not None and step >= decay_start) else a.lr


for step in range(1, a.steps + 1):
    opt.learning_rate.assign(lr_at(step))
    t0 = time.perf_counter()
    x, y, w = q.get()
    mark("wait_for_batch", t0)
    t0 = time.perf_counter()
    loss = train_step(x, y, w)
    if ema_vars is not None:
        ema_update()
    if step % 20 == 0 or step == a.steps:
        loss = float(loss)  # sync point, keeps the timing honest
    mark("train_step", t0)
    if step % a.eval_every == 0 or step == a.steps:
        t0 = time.perf_counter()
        eval_model.set_weights(eval_weights())
        m = mww_train.validate_nonstreaming(config, dp, eval_model, "validation")
        mark("validation", t0)
        metric = float(m["average_viable_recall"])
        history.append({"step": step, "loss": float(loss), **{k: float(v) for k, v in m.items()}})
        print(f"step {step:6d} loss {float(loss):.4f} avr {metric:.4f} recall@0faph {m['recall_at_no_faph']:.3f} "
              f"faph {m['ambient_false_positives_per_hour']:.2f}  [{time.perf_counter() - t_train:.0f}s]", flush=True)
        if metric > best["metric"]:
            best = {"metric": metric, "step": step}
            eval_model.save_weights(os.path.join(config["train_dir"], "best_weights.weights.h5"))
eval_model.set_weights(eval_weights())
eval_model.save_weights(os.path.join(config["train_dir"], "last_weights.weights.h5"))
stop.set()
timings["train_loop_total"] = time.perf_counter() - t_train
timings["wall_total"] = time.perf_counter() - T_START
summary = {"args": vars(a), "train_features": train_features, "best": best, "timings_s": {k: round(v, 2) for k, v in timings.items()},
           "samples_per_s": round(a.steps * a.batch / timings["train_loop_total"]), "history": history}
json.dump(summary, open(os.path.join(config["train_dir"], "fast_summary.json"), "w"), indent=1)
print(json.dumps({k: summary[k] for k in ("best", "timings_s", "samples_per_s")}), flush=True)
os._exit(0)  # don't wait for producer threads blocked on a full queue
