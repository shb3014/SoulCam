#!/usr/bin/env python3
"""
Convert RKNN-friendly hand ONNX to RK3566 RKNN.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from rknn.api import RKNN


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Convert hand ONNX to RKNN.")
    parser.add_argument(
        "--onnx",
        default="models/generated/hand_yolov8n_9out.onnx",
        help="Path to input ONNX model.",
    )
    parser.add_argument(
        "--rknn",
        default="models/generated/hand_yolov8n_rk3566_i8_20260301.rknn",
        help="Path to output RKNN model.",
    )
    parser.add_argument(
        "--target",
        default="rk3566",
        help="Target platform (default: rk3566).",
    )
    parser.add_argument(
        "--quant",
        action="store_true",
        help="Enable INT8 quantization.",
    )
    parser.add_argument(
        "--dataset",
        default="",
        help="Calibration dataset list path for INT8 quantization.",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Enable verbose RKNN logs.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    onnx_path = Path(args.onnx)
    rknn_path = Path(args.rknn)
    rknn_path.parent.mkdir(parents=True, exist_ok=True)

    if not onnx_path.exists():
        raise FileNotFoundError(f"ONNX not found: {onnx_path}")
    if args.quant and not args.dataset:
        raise ValueError("--dataset is required when --quant is enabled.")

    rknn = RKNN(verbose=args.verbose)
    rknn.config(
        mean_values=[[0, 0, 0]],
        std_values=[[255, 255, 255]],
        target_platform=args.target,
    )

    ret = rknn.load_onnx(model=str(onnx_path))
    print(f"load_onnx ret = {ret}")
    if ret != 0:
        raise SystemExit(ret)

    ret = rknn.build(
        do_quantization=args.quant,
        dataset=args.dataset if args.quant else None,
    )
    print(f"build ret = {ret}")
    if ret != 0:
        raise SystemExit(ret)

    ret = rknn.export_rknn(str(rknn_path))
    print(f"export ret = {ret}")
    if ret != 0:
        raise SystemExit(ret)

    rknn.release()
    print(f"DONE {rknn_path}")


if __name__ == "__main__":
    main()
