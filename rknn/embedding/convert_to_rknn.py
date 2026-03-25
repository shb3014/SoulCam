#!/usr/bin/env python3
"""
Convert embedding ONNX model to RKNN format for RK3566 NPU deployment.

Produces an INT8 quantized .rknn file optimized for the RK3566's 0.8 TOPS NPU.
Expects ~15-25ms inference latency per 128x128 crop.

Usage:
    # FP16 (no calibration needed, larger file, slightly slower)
    python convert_to_rknn.py --onnx models/embedding_model.onnx

    # INT8 quantized (requires calibration images, smallest + fastest)
    python convert_to_rknn.py --onnx models/embedding_model.onnx \
                              --quant --dataset calibration_list.txt

Calibration:
    Create calibration_list.txt with ~100-500 representative image paths.
    Use generate_calibration_images.py to extract from training data.

Prerequisites:
    pip install rknn-toolkit2
    (x86 Linux only — RKNN toolkit does not run on ARM)
"""

from __future__ import annotations

import argparse
from pathlib import Path

from rknn.api import RKNN


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Convert embedding ONNX to RKNN.")
    p.add_argument("--onnx", required=True, help="Input ONNX model path.")
    p.add_argument("--rknn", default="", help="Output RKNN path (auto if empty).")
    p.add_argument("--target", default="rk3566", help="Target platform.")
    p.add_argument("--quant", action="store_true", help="INT8 quantization.")
    p.add_argument("--dataset", default="", help="Calibration image list for INT8.")
    p.add_argument("--input-size", type=int, default=128)
    p.add_argument("--verbose", action="store_true")
    return p.parse_args()


def main() -> None:
    args = parse_args()
    onnx_path = Path(args.onnx)

    if not onnx_path.exists():
        raise FileNotFoundError(f"ONNX model not found: {onnx_path}")

    if args.quant and not args.dataset:
        raise ValueError("--dataset is required for INT8 quantization (--quant).")

    if args.quant and args.dataset and not Path(args.dataset).exists():
        raise FileNotFoundError(f"Calibration list not found: {args.dataset}")

    quant_tag = "i8" if args.quant else "fp16"
    rknn_path = args.rknn or str(
        onnx_path.parent / f"embedding_{args.target}_{quant_tag}.rknn"
    )
    Path(rknn_path).parent.mkdir(parents=True, exist_ok=True)

    rknn = RKNN(verbose=args.verbose)

    # The C++ embedder feeds raw UINT8 RGB pixels [0, 255].
    # RKNN normalizes: (pixel - mean) / std → model input.
    # With mean=[0,0,0] and std=[255,255,255], this maps [0,255] → [0,1],
    # matching the ToTensor() normalization used during training.
    rknn.config(
        mean_values=[[0, 0, 0]],
        std_values=[[255, 255, 255]],
        target_platform=args.target,
    )

    print(f"Loading ONNX: {onnx_path}")
    ret = rknn.load_onnx(model=str(onnx_path))
    if ret != 0:
        raise RuntimeError(f"load_onnx failed: {ret}")

    print(f"Building ({'INT8' if args.quant else 'FP16'})...")
    ret = rknn.build(
        do_quantization=args.quant,
        dataset=args.dataset if args.quant else None,
    )
    if ret != 0:
        raise RuntimeError(f"build failed: {ret}")

    print(f"Exporting: {rknn_path}")
    ret = rknn.export_rknn(rknn_path)
    if ret != 0:
        raise RuntimeError(f"export failed: {ret}")

    size_kb = Path(rknn_path).stat().st_size / 1024
    print(f"Done: {rknn_path} ({size_kb:.1f} KB)")

    # Run accuracy analysis if calibration data is available
    if args.quant and args.dataset:
        print("\nRunning accuracy analysis...")
        try:
            ret = rknn.accuracy_analysis(
                inputs=[args.dataset],
                target=args.target,
            )
            if ret == 0:
                print("Accuracy analysis complete (check output directory).")
        except Exception as e:
            print(f"Accuracy analysis skipped: {e}")

    rknn.release()

    print(f"\nDeployment:")
    print(f"  1. Upload to device:")
    print(f"     scp {rknn_path} ubuntu@192.168.1.45:/home/ubuntu/models/")
    print(f"  2. Set DP on device:")
    print(f"     perception_embedder_model = /home/ubuntu/models/{Path(rknn_path).name}")
    print(f"  3. Restart soulcam service")


if __name__ == "__main__":
    main()
