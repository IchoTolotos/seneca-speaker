"""Where does microWakeWord's training time go? Times each component of the stock loop.

Usage: $WORK/venv-mww/bin/python -I profile_baseline.py [steps]
"""
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np  # noqa: E402

from common import base_config, build  # noqa: E402

STEPS = int(sys.argv[1]) if len(sys.argv) > 1 else 200


def t(label, fn, n=1):
    t0 = time.perf_counter()
    for _ in range(n):
        out = fn()
    dt = (time.perf_counter() - t0) / n
    print(f"{label:<55} {dt * 1000:10.2f} ms" + (f"  (avg of {n})" if n > 1 else ""), flush=True)
    return out


def main():
    import tensorflow as tf
    from tensorflow.python.util import tf_decorator
    from microwakeword import data as input_data
    from microwakeword import train as mww_train

    print("TF", tf.__version__, tf.config.list_physical_devices())
    flags, config, mixednet = build(base_config("bench"))
    dp = t("FeatureHandler init (load all features)", lambda: input_data.FeatureHandler(config))
    policy = {"freq_mix_prob": 0.0, "time_mask_max_size": 0, "time_mask_count": 0,
              "freq_mask_max_size": 0, "freq_mask_count": 0}
    get_batch = lambda: dp.get_data("training", batch_size=config["batch_size"],  # noqa: E731
                                    features_length=config["spectrogram_length"],
                                    truncation_strategy="default", augmentation_policy=policy)
    x, y, w = t("get_data(training, 128)", get_batch, STEPS)
    y = y.reshape(-1, 1)
    print("batch shape", x.shape, x.dtype)

    cutoffs = np.linspace(0.0, 1.0, 101).tolist()

    def make_model(heavy_metrics=True):
        m = mixednet.model(flags, config["training_input_shape"], config["batch_size"])
        metrics = [tf.keras.metrics.BinaryAccuracy(name="accuracy")]
        if heavy_metrics:
            metrics += [
                tf.keras.metrics.Recall(name="recall"), tf.keras.metrics.Precision(name="precision"),
                tf.keras.metrics.TruePositives(name="tp", thresholds=cutoffs),
                tf.keras.metrics.FalsePositives(name="fp", thresholds=cutoffs),
                tf.keras.metrics.TrueNegatives(name="tn", thresholds=cutoffs),
                tf.keras.metrics.FalseNegatives(name="fn", thresholds=cutoffs),
                tf.keras.metrics.AUC(name="auc"), tf.keras.metrics.BinaryCrossentropy(name="loss"),
            ]
        m.compile(optimizer=tf.keras.optimizers.Adam(), loss=tf.keras.losses.BinaryCrossentropy(), metrics=metrics)
        return m

    # 1) stock: eager (unwrapped) train function + heavy metrics
    m = make_model()
    m.make_train_function()
    _, m.train_function = tf_decorator.unwrap(m.train_function)
    for _ in range(10):
        m.train_on_batch(x, y, sample_weight=w)
    t("train_on_batch STOCK (eager, heavy metrics)", lambda: m.train_on_batch(x, y, sample_weight=w), STEPS)

    # 2) compiled tf.function, heavy metrics
    m = make_model()
    for _ in range(10):
        m.train_on_batch(x, y, sample_weight=w)
    t("train_on_batch compiled, heavy metrics", lambda: m.train_on_batch(x, y, sample_weight=w), STEPS)

    # 3) compiled, accuracy only
    m = make_model(heavy_metrics=False)
    for _ in range(10):
        m.train_on_batch(x, y, sample_weight=w)
    t("train_on_batch compiled, light metrics", lambda: m.train_on_batch(x, y, sample_weight=w), STEPS)

    # 4) validation as the stock loop does it every eval_step_interval (incl. rebuilding arrays)
    vm = make_model()
    vm.train_on_batch(x, y, sample_weight=w)
    t("get_data(validation) [rebuilt each eval]", lambda: dp.get_data(
        "validation", batch_size=128, features_length=config["spectrogram_length"],
        truncation_strategy="truncate_start"))
    t("get_data(validation_ambient, split) [rebuilt each eval]", lambda: dp.get_data(
        "validation_ambient", batch_size=128, features_length=config["spectrogram_length"],
        truncation_strategy="split"))
    t("validate_nonstreaming (stock, full)", lambda: mww_train.validate_nonstreaming(
        config, dp, vm, "validation"))


if __name__ == "__main__":
    main()
