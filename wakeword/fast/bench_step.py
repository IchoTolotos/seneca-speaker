"""Benchmark only the optimizer step (synthetic batches): jit, threads, device, K steps per call.

Usage: python -I bench_step.py [--jit 0|1] [--intra N] [--inter N] [--k K] [--device cpu|gpu] [--steps N]
Prints ms per training step (per batch of 128).
"""
import argparse
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

ap = argparse.ArgumentParser()
ap.add_argument("--jit", type=int, default=0)
ap.add_argument("--intra", type=int, default=0)
ap.add_argument("--inter", type=int, default=0)
ap.add_argument("--k", type=int, default=1, help="optimizer steps per tf.function call")
ap.add_argument("--device", default="cpu")
ap.add_argument("--steps", type=int, default=400)
ap.add_argument("--batch", type=int, default=128)
a = ap.parse_args()

import numpy as np  # noqa: E402
import tensorflow as tf  # noqa: E402

if a.intra:
    tf.config.threading.set_intra_op_parallelism_threads(a.intra)
if a.inter:
    tf.config.threading.set_inter_op_parallelism_threads(a.inter)
if a.device == "cpu":
    tf.config.set_visible_devices([], "GPU")

from common import base_config, build  # noqa: E402

flags, config, mixednet = build(base_config("bench"))
B, T, F = a.batch, config["spectrogram_length"], 40
model = mixednet.model(flags, config["training_input_shape"], B)
opt = tf.keras.optimizers.Adam()
loss_fn = tf.keras.losses.BinaryCrossentropy()

rng = np.random.default_rng(0)
X = tf.constant(rng.standard_normal((a.k, B, T, F)).astype(np.float32))
Y = tf.constant((rng.random((a.k, B, 1)) < 0.2).astype(np.float32))
W = tf.constant(np.ones((a.k, B), np.float32))


@tf.function(jit_compile=bool(a.jit))
def k_steps(X, Y, W):
    total = tf.constant(0.0)
    for i in tf.range(a.k):
        with tf.GradientTape() as tape:
            p = model(X[i], training=True)
            loss = loss_fn(Y[i], p, sample_weight=W[i])
        grads = tape.gradient(loss, model.trainable_variables)
        opt.apply_gradients(zip(grads, model.trainable_variables))
        total += loss
    return total


for _ in range(3):
    k_steps(X, Y, W).numpy()
calls = max(1, a.steps // a.k)
t0 = time.perf_counter()
for _ in range(calls):
    k_steps(X, Y, W).numpy()
dt = (time.perf_counter() - t0) / (calls * a.k)
dev = [d.device_type for d in tf.config.get_visible_devices()]
print(f"RESULT jit={a.jit} intra={a.intra} inter={a.inter} k={a.k} batch={B} devices={dev} "
      f"ms_per_step={dt * 1000:.2f} steps_per_s={1 / dt:.0f}", flush=True)
