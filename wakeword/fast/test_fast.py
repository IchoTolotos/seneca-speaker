"""Fast, parallel version of microWakeWord's final test (quantized streaming TFLite ROC).

Same steps and math as model_train_eval.evaluate_model(..., test_tflite_streaming_quantized=1) +
test.tflite_streaming_model_roc(): export streaming SavedModel -> quantized TFLite (stock code),
moving average of 5, 25-slice cooldown, FRR on positive `testing` clips, FA/h on `testing_ambient`,
AUC of the FA/h-vs-FRR curve up to 2 FA/h (lower is better).
Differences (speed only):
  * long ambient tracks are split into chunks run in parallel processes; each chunk is preceded by
    WARMUP frames of context whose outputs are discarded, so the streaming state matches,
  * false accepts are counted by a sparse exact equivalent of compute_false_accepts_per_hour,
  * every clip/chunk runs on a fresh interpreter (stock leaks state between clips, which
    makes its numbers depend on the random clip order).

Usage: python -I test_fast.py <name> [--weights best_weights] [--workers 14] [--check]
"""
import argparse
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np  # noqa: E402
from numpy.lib.stride_tricks import sliding_window_view  # noqa: E402

WINDOW = 5
IGNORE = 25
CHUNK_FRAMES = int(os.environ.get("CHUNK_FRAMES", 30000))  # 5 min of 10 ms frames per job
WARMUP_FRAMES = 600   # > model receptive field (204 frames); outputs during warm-up are dropped

_model_args = None


def _init(model_path, stride):
    global _model_args
    _model_args = (model_path, stride)


def _predict(spec):
    # Stock Model.predict_spectrogram never resets the streaming state, so each clip inherits the
    # previous clip's state (order-dependent results), and interpreter.reset_all_variables() does not
    # clear these resource-variable states. A fresh interpreter per clip/chunk does (model is ~60 KB).
    from microwakeword.inference import Model
    return np.asarray(Model(*_model_args).predict_spectrogram(spec), np.float32)


def _ambient_chunk(args):
    spec, drop = args  # drop = number of output probabilities belonging to the warm-up context
    p = _predict(spec)
    return p[drop:]


def _positive(spec):
    p = _predict(spec)
    w = sliding_window_view(p[IGNORE:], WINDOW).mean(axis=-1)
    return float(np.max(w))


def faph_sparse(tracks, cutoffs, ignore, stride, step_s):
    """Exact equivalent of microwakeword.test.compute_false_accepts_per_hour."""
    fa = np.zeros(len(cutoffs))
    hours = 0.0
    for p in tracks:
        hours += len(p) * stride * step_s / 3600.0
        for ci, c in enumerate(cutoffs):
            idx = np.flatnonzero(p > c)
            nxt = ignore - 1  # first index where the initial cooldown has expired
            n = 0
            for i in idx:
                if i >= nxt:
                    n += 1
                    nxt = i + ignore
            fa[ci] += n
    return fa / hours


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--weights", default="best_weights")
    ap.add_argument("--workers", type=int, default=max(1, os.cpu_count() - 4))
    ap.add_argument("--check", action="store_true", help="also run the stock FA/h function and compare")
    ap.add_argument("--skip-export", action="store_true")
    ap.add_argument("--wide", action="store_true",
                    help="ambient = testing_ambient + validation_ambient (~3x the hours; validation only picked the checkpoint)")
    a = ap.parse_args()

    os.environ.setdefault("CUDA_VISIBLE_DEVICES", "-1")  # export/quantization on CPU
    import tensorflow as tf  # noqa: F401
    from common import WORK, base_config, build
    from microwakeword import data as input_data
    from microwakeword import test as mww_test
    from microwakeword import utils
    from microwakeword.layers import modes

    times = {}
    t0 = time.perf_counter()
    flags, config, mixednet = build(base_config(a.name))
    dp = input_data.FeatureHandler(config)
    times["load"] = time.perf_counter() - t0

    folder = "tflite_stream_state_internal_quant"
    model_path = os.path.join(config["train_dir"], folder, "stream_state_internal_quant.tflite")
    if not a.skip_export:
        t0 = time.perf_counter()
        model = mixednet.model(flags, shape=config["training_input_shape"], batch_size=1)
        model.load_weights(os.path.join(config["train_dir"], a.weights) + ".weights.h5")
        utils.convert_model_saved(model, config, folder="stream_state_internal",
                                  mode=modes.Modes.STREAM_INTERNAL_STATE_INFERENCE)
        utils.convert_saved_model_to_tflite(
            config, audio_processor=dp,
            path_to_model=os.path.join(config["train_dir"], "stream_state_internal"),
            folder=os.path.join(config["train_dir"], folder),
            fname="stream_state_internal_quant.tflite", quantize=True)
        times["export"] = time.perf_counter() - t0

    t0 = time.perf_counter()
    ambient, _, _ = dp.get_data("testing_ambient", batch_size=config["batch_size"],
                                features_length=config["spectrogram_length"], truncation_strategy="none")
    if a.wide:
        va, _, _ = dp.get_data("validation_ambient", batch_size=config["batch_size"],
                               features_length=config["spectrogram_length"], truncation_strategy="none")
        ambient = list(ambient) + list(va)
    test_x, test_y, _ = dp.get_data("testing", batch_size=config["batch_size"],
                                    features_length=config["spectrogram_length"], truncation_strategy="none")
    positives = [test_x[i] for i in range(len(test_x)) if test_y[i]]
    # held-out sound-alike clips (features/adversarial testing split): do they trigger?
    adversarial = [test_x[i] for i in range(len(test_x)) if not test_y[i]]
    stride = config["stride"]
    jobs, owner = [], []  # owner[i] = ambient track index for job i
    for ti, spec in enumerate(ambient):
        for s in range(0, spec.shape[0], CHUNK_FRAMES):
            ws = max(0, s - WARMUP_FRAMES)
            ws -= (s - ws) % stride  # keep the stride phase aligned with the full-track run
            e = min(spec.shape[0], s + CHUNK_FRAMES)
            jobs.append((spec[ws:e], (s - ws) // stride))
            owner.append(ti)
    times["prepare"] = time.perf_counter() - t0

    t0 = time.perf_counter()
    with ProcessPoolExecutor(a.workers, initializer=_init, initargs=(model_path, stride)) as ex:
        amb_parts = ex.map(_ambient_chunk, jobs, chunksize=1)
        pos_scores = list(ex.map(_positive, positives, chunksize=16))
        adv_scores = list(ex.map(_positive, adversarial, chunksize=16))
        amb_parts = list(amb_parts)
    times["inference"] = time.perf_counter() - t0

    t0 = time.perf_counter()
    raw = [np.concatenate([p for p, o in zip(amb_parts, owner) if o == ti]) for ti in range(len(ambient))]
    tracks = [sliding_window_view(p, WINDOW).mean(axis=-1) for p in raw]
    cutoffs = np.arange(0, 1.01, 0.01)
    faph = faph_sparse(tracks, cutoffs, IGNORE, stride, config["window_step_ms"] / 1000)
    pos = np.array(pos_scores)
    frr = np.array([1 - (pos > c).sum() / len(pos) for c in cutoffs])
    x, y, cut = mww_test.generate_roc_curve(false_accepts_per_hour=faph, false_rejections=frr, cutoffs=cutoffs)
    auc = float(np.trapezoid(y, x))
    times["metrics"] = time.perf_counter() - t0

    if a.check:
        t0 = time.perf_counter()
        stock = mww_test.compute_false_accepts_per_hour(tracks, cutoffs, IGNORE, stride=stride,
                                                        step_s=config["window_step_ms"] / 1000)
        times["stock_faph_check"] = time.perf_counter() - t0
        print("faph identical to stock:", bool(np.allclose(stock, faph)))

    out = os.path.join(config["train_dir"], folder, "fast_roc_wide.txt" if a.wide else "fast_roc.txt")
    with open(out, "w") as fh:
        fh.write(f"AUC {auc:.5f}\n")
        for i in range(len(x)):
            fh.write(f"Cutoff {cut[i]:.2f}: frr={y[i]:.4f}; faph={x[i]:.3f}\n")
    # Operating point = lowest cutoff with 0 FA/h on the ambient set (what we'd ship); report the
    # miss rate and the sound-alike trigger rate there, plus at cutoff 0.5 for reference.
    adv = np.array(adv_scores)
    op = next((c for c in cutoffs if faph[int(round(c * 100))] == 0), 1.0)
    adv_far_op = float((adv > op).mean()) if len(adv) else float("nan")
    frr_op = float(1 - (pos > op).mean())
    adv_far_05 = float((adv > 0.5).mean()) if len(adv) else float("nan")
    # Balanced error: best (miss + sound-alike trigger)/2 over cutoffs that keep 0 FA/h on ambient.
    bal = min(((1 - (pos > c).mean()) + (adv > c).mean()) / 2 for c in cutoffs if c >= op) \
        if len(adv) else float("nan")
    hours = sum(len(p) for p in raw) * stride * config["window_step_ms"] / 1000 / 3600
    # FRR at <= 0.5 FA/h: rests on several ambient events instead of a single one (less noisy)
    ok = [i for i, c in enumerate(cutoffs) if faph[i] <= 0.5]
    frr05 = float(min(frr[i] for i in ok)) if ok else 1.0
    zero = [i for i in range(len(x)) if x[i] == 0]
    frr0 = float(y[zero[-1]]) if zero else 1.0
    with open(out, "a") as fh:
        fh.write(f"operating cutoff {op:.2f}: frr={frr_op:.4f} adversarial_far={adv_far_op:.4f} "
                 f"(n_adv={len(adv)}); adversarial_far@0.5={adv_far_05:.4f}; balanced={bal:.4f}\n")
    print(f"RESULT{'_WIDE' if a.wide else ''} {a.name} AUC {auc:.5f} frr@0faph {frr0:.4f} frr@0.5faph {frr05:.4f} op {op:.2f} frr_op {frr_op:.4f} "
          f"advfar_op {adv_far_op:.4f} bal {bal:.4f} advfar@0.5 {adv_far_05:.4f} positives {len(pos)} adversarial {len(adv)} "
          f"ambient_h {hours:.2f} "
          f"times {{{', '.join(f'{k}: {v:.1f}s' for k, v in times.items())}}}", flush=True)


if __name__ == "__main__":
    main()
