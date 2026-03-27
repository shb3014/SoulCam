#!/usr/bin/env python3
"""
Convert YOLOv8n single-class ONNX model to RKNN format for RK3566.

Produces an INT8-quantized .rknn file compatible with the existing
detector.cpp post-processing (DFL decode + NMS). The model architecture
is identical to the stock YOLOv8n — only the number of classes changes
from 80 to 1.

Usage:
    # FP16 (quick test, no calibration)
    python convert_yolo_rknn.py --onnx best.onnx

    # INT8 quantized (production, needs calibration images)
    python convert_yolo_rknn.py --onnx best.onnx \
        --quant --dataset calibration_list.txt

    # Generate calibration list first:
    python merge_classes.py --calibration \
        --src /data/Objects365_1cls/images/val \
        --count 200 --output calibration_list.txt

Prerequisites:
    pip install rknn-toolkit2
    (x86 Linux only)
"""

from __future__ import annotations

import argparse
from pathlib import Path

from rknn.api import RKNN


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Convert YOLOv8n single-class ONNX to RKNN."
    )
    p.add_argument("--onnx", required=True, help="Input ONNX model path.")
    p.add_argument("--rknn", default="", help="Output RKNN path (auto if empty).")
    p.add_argument("--target", default="rk3566", help="Target platform.")
    p.add_argument("--quant", action="store_true", help="INT8 quantization.")
    p.add_argument(
        "--dataset", default="", help="Calibration image list for INT8."
    )
    p.add_argument("--input-size", type=int, default=640)
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
        onnx_path.parent
        / f"yolov8n_obj365_1cls_{args.target}_{quant_tag}.rknn"
    )
    Path(rknn_path).parent.mkdir(parents=True, exist_ok=True)

    rknn = RKNN(verbose=args.verbose)

    # YOLOv8 expects [0,255] uint8 input; Ultralytics applies /255 inside
    # the model. RKNN pass-through: mean=0, std=255 maps [0,255] -> [0,1].
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

    size_mb = Path(rknn_path).stat().st_size / (1024 * 1024)
    print(f"Done: {rknn_path} ({size_mb:.1f} MB)")

    rknn.release()

    model_name = Path(rknn_path).name
    print(f"\nDeployment steps:")
    print(f"  1. scp {rknn_path} ubuntu@192.168.1.45:/home/ubuntu/models/")
    print(f'  2. Set store.json: "ai_model_path": "/home/ubuntu/models/{model_name}"')
    print(f'  3. Set store.json: "ai_labels": "object"')
    print(f"  4. sudo systemctl restart soulcam")


if __name__ == "__main__":
    main()
