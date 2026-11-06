"""Check FastSampler against the stock sampler (statistics) and measure its throughput.

Usage: python -I bench_sampler.py
"""
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np  # noqa: E402

from common import WORK, base_config, build  # noqa: E402
from sampler import FastSampler  # noqa: E402

flags, config, _ = build(base_config("bench"))
L = config["spectrogram_length"]
t0 = time.perf_counter()
fs = FastSampler(WORK, config["features"], L)
print(f"FastSampler init {time.perf_counter() - t0:.2f}s; providers:",
      [(p.n, p.weight, p.strategy) for p in fs.providers])

# --- correctness vs stock (same marginal statistics) ---
from microwakeword import data as input_data  # noqa: E402

dp = input_data.FeatureHandler(config)
policy = {"freq_mix_prob": 0.0, "time_mask_max_size": 0, "time_mask_count": 0,
          "freq_mask_max_size": 0, "freq_mask_count": 0}
xs, ys = [], []
for _ in range(40):
    x, y, _ = dp.get_data("training", batch_size=128, features_length=L,
                          truncation_strategy="default", augmentation_policy=policy)
    xs.append(x), ys.append(y)
xs, ys = np.concatenate(xs), np.concatenate(ys)
fx, fy, _ = fs.batch(len(ys))
print("shape stock", xs.shape, "fast", fx.shape)
print(f"positive rate stock {ys.mean():.3f} fast {fy.mean():.3f}")
for name, sel_s, sel_f in [("positives", ys == 1, fy == 1), ("negatives", ys == 0, fy == 0)]:
    a, b = xs[sel_s], fx[sel_f]
    print(f"{name}: mean {a.mean():.3f} vs {b.mean():.3f} | std {a.std():.3f} vs {b.std():.3f} | "
          f"zero-frac {(a == 0).mean():.4f} vs {(b == 0).mean():.4f}")

# --- throughput ---
for B in (128, 1024, 4096):
    n = max(3, 20000 // B)
    t0 = time.perf_counter()
    for _ in range(n):
        fs.batch(B)
    dt = (time.perf_counter() - t0) / n
    print(f"FastSampler B={B:5d}: {dt * 1000:8.2f} ms/batch  {B / dt:9.0f} samples/s")

# threads (numpy gather releases the GIL for large copies)
for workers in (2, 4, 8):
    samplers = [FastSampler(WORK, config["features"], L, seed=i) for i in range(workers)]
    B, n = 4096, 24
    with ThreadPoolExecutor(workers) as ex:
        t0 = time.perf_counter()
        list(ex.map(lambda i: samplers[i % workers].batch(B), range(n)))
        dt = time.perf_counter() - t0
    print(f"threads={workers}: {B * n / dt:9.0f} samples/s")
