#!/usr/bin/env python3
"""
On-device validation of the RKNN embedding model.

Run this ON THE RK3566 DEVICE (not on x86) to verify:
  1. Model loads and runs on the NPU
  2. Inference latency is within budget (<25ms)
  3. Embeddings are consistent (same input → same output)
  4. Different inputs produce different embeddings

This uses rknnlite2 (the on-device runtime) rather than rknn-toolkit2.

Usage (on device):
    python3 validate_on_device.py --model /home/ubuntu/models/embedding_rk3566_i8.rknn
    python3 validate_on_device.py --model /path/to/model.rknn --image /path/to/test.jpg
"""

from __future__ import annotations

import argparse
import time

import numpy as np

try:
    from rknnlite.api import RKNNLite
    HAS_RKNNLITE = True
except ImportError:
    HAS_RKNNLITE = False


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Validate RKNN embedding model on device.")
    p.add_argument("--model", required=True, help="Path to .rknn model file.")
    p.add_argument("--image", default="", help="Optional test image (JPG/PNG).")
    p.add_argument("--input-size", type=int, default=128)
    p.add_argument("--embed-dim", type=int, default=128)
    p.add_argument("--warmup", type=int, default=5, help="Warmup iterations.")
    p.add_argument("--iterations", type=int, default=50, help="Timing iterations.")
    return p.parse_args()


def l2_normalize(v: np.ndarray) -> np.ndarray:
    norm = np.linalg.norm(v)
    return v / max(norm, 1e-12)


def cosine_sim(a: np.ndarray, b: np.ndarray) -> float:
    a_n = l2_normalize(a.flatten())
    b_n = l2_normalize(b.flatten())
    return float(np.dot(a_n, b_n))


def main() -> None:
    args = parse_args()

    if not HAS_RKNNLITE:
        print("ERROR: rknnlite2 not available. This script must run on the RK3566 device.")
        print("Install: pip3 install rknnlite2")
        return

    print(f"Model: {args.model}")
    print(f"Input: {args.input_size}x{args.input_size}x3 (UINT8 RGB NHWC)")
    print(f"Expected output: {args.embed_dim}-D float vector")

    rknn = RKNNLite()
    ret = rknn.load_rknn(args.model)
    if ret != 0:
        print(f"FAIL: load_rknn returned {ret}")
        return

    ret = rknn.init_runtime(core_mask=RKNNLite.NPU_CORE_0)
    if ret != 0:
        print(f"FAIL: init_runtime returned {ret}")
        rknn.release()
        return

    print("Model loaded and runtime initialized.\n")

    # Prepare test inputs
    sz = args.input_size
    if args.image:
        from PIL import Image
        img = Image.open(args.image).convert("RGB").resize((sz, sz))
        input_a = np.array(img, dtype=np.uint8)
    else:
        input_a = np.random.randint(0, 255, (sz, sz, 3), dtype=np.uint8)

    input_b = np.random.randint(0, 255, (sz, sz, 3), dtype=np.uint8)
    input_a_copy = input_a.copy()

    # --- Test 1: Basic inference ---
    print("--- Test 1: Basic Inference ---")
    outputs = rknn.inference(inputs=[input_a])
    if outputs is None or len(outputs) == 0:
        print("FAIL: inference returned None/empty")
        rknn.release()
        return

    embed = outputs[0].flatten()
    print(f"  Output shape: {outputs[0].shape}")
    print(f"  Output dim: {len(embed)}")
    print(f"  Output range: [{embed.min():.4f}, {embed.max():.4f}]")
    print(f"  L2 norm: {np.linalg.norm(embed):.4f}")

    if len(embed) != args.embed_dim:
        print(f"  WARNING: Expected {args.embed_dim}-D, got {len(embed)}-D")

    embed_norm = l2_normalize(embed)
    print(f"  L2 norm after normalize: {np.linalg.norm(embed_norm):.6f}")
    print(f"  PASS\n")

    # --- Test 2: Consistency ---
    print("--- Test 2: Consistency (same input → same output) ---")
    out1 = rknn.inference(inputs=[input_a])[0].flatten()
    out2 = rknn.inference(inputs=[input_a_copy])[0].flatten()
    sim = cosine_sim(out1, out2)
    diff = np.abs(out1 - out2).max()
    print(f"  Cosine similarity: {sim:.6f} (should be ~1.0)")
    print(f"  Max absolute diff: {diff:.6f}")
    status = "PASS" if sim > 0.999 else "WARN"
    print(f"  {status}\n")

    # --- Test 3: Discrimination ---
    print("--- Test 3: Discrimination (different inputs → different outputs) ---")
    out_a = rknn.inference(inputs=[input_a])[0].flatten()
    out_b = rknn.inference(inputs=[input_b])[0].flatten()
    sim_ab = cosine_sim(out_a, out_b)
    print(f"  Cosine similarity (A vs B): {sim_ab:.4f}")
    print(f"  (random inputs should give sim < 0.5, ideally < 0.3)")
    status = "PASS" if sim_ab < 0.5 else "WARN"
    print(f"  {status}\n")

    # --- Test 4: Latency ---
    print(f"--- Test 4: Latency ({args.warmup} warmup + {args.iterations} timed) ---")
    for _ in range(args.warmup):
        rknn.inference(inputs=[input_a])

    latencies = []
    for _ in range(args.iterations):
        t0 = time.perf_counter()
        rknn.inference(inputs=[input_a])
        dt = (time.perf_counter() - t0) * 1000
        latencies.append(dt)

    latencies = np.array(latencies)
    print(f"  Mean:   {latencies.mean():.2f} ms")
    print(f"  Median: {np.median(latencies):.2f} ms")
    print(f"  P95:    {np.percentile(latencies, 95):.2f} ms")
    print(f"  P99:    {np.percentile(latencies, 99):.2f} ms")
    print(f"  Min:    {latencies.min():.2f} ms")
    print(f"  Max:    {latencies.max():.2f} ms")

    budget = 25.0
    status = "PASS" if np.median(latencies) < budget else "FAIL"
    print(f"  Budget: <{budget:.0f} ms → {status}\n")

    # --- Summary ---
    print("=" * 50)
    print("SUMMARY")
    print(f"  Model:       {args.model}")
    print(f"  Output dim:  {len(embed)}")
    print(f"  Latency:     {np.median(latencies):.1f} ms (median)")
    print(f"  Consistent:  {'yes' if sim > 0.999 else 'no'}")
    print(f"  Discriminative: {'yes' if sim_ab < 0.5 else 'check needed'}")

    all_pass = (
        len(embed) == args.embed_dim
        and sim > 0.999
        and np.median(latencies) < budget
    )
    print(f"\n  Overall: {'ALL PASS' if all_pass else 'ISSUES DETECTED'}")

    if all_pass:
        print(f"\n  Ready to deploy! Set DP:")
        print(f"    perception_embedder_model = {args.model}")

    rknn.release()


if __name__ == "__main__":
    main()
