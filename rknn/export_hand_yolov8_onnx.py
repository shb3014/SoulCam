#!/usr/bin/env python3
"""
Export a hand YOLOv8 .pt model to RKNN-friendly ONNX.

The produced ONNX exposes 3 branches * 3 tensors:
  box_i       : [1, 64, H, W]
  score_i     : [1, C,  H, W]  (sigmoid probabilities or raw logits)
  score_sum_i : [1, 1,  H, W]  (sum over classes)
"""

from __future__ import annotations

import argparse
from pathlib import Path

import onnx
import torch
from ultralytics import YOLO


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Export hand YOLOv8 to 9-output ONNX.")
    parser.add_argument(
        "--pt",
        default="models/generated/hand_yolov8n.pt",
        help="Path to input .pt model.",
    )
    parser.add_argument(
        "--onnx",
        default="models/generated/hand_yolov8n_9out_sigmoid.onnx",
        help="Path to output .onnx model.",
    )
    parser.add_argument(
        "--imgsz",
        type=int,
        default=640,
        help="Square input size for export (default: 640).",
    )
    parser.add_argument(
        "--opset",
        type=int,
        default=12,
        help="ONNX opset version (default: 12).",
    )
    parser.add_argument(
        "--score-mode",
        choices=["sigmoid", "logit"],
        default="sigmoid",
        help=(
            "Output mode for score tensor: "
            "'sigmoid' for probabilities, 'logit' for raw cls logits."
        ),
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    pt_path = Path(args.pt)
    onnx_path = Path(args.onnx)
    onnx_path.parent.mkdir(parents=True, exist_ok=True)

    if not pt_path.exists():
        raise FileNotFoundError(f"Model not found: {pt_path}")

    model = YOLO(str(pt_path)).model.eval()
    detect = model.model[-1]

    if not hasattr(detect, "nl") or not hasattr(detect, "cv2") or not hasattr(detect, "cv3"):
        raise RuntimeError("Unexpected YOLO head structure; expected Detect with nl/cv2/cv3.")

    def custom_detect_forward(x):
        outs = []
        for i in range(detect.nl):
            box = detect.cv2[i](x[i])
            cls_logit = detect.cv3[i](x[i])
            if args.score_mode == "sigmoid":
                score = torch.sigmoid(cls_logit)
            else:
                score = cls_logit
            score_sum = torch.sigmoid(cls_logit).sum(dim=1, keepdim=True)
            outs.extend([box, score, score_sum])
        return tuple(outs)

    # Monkey-patch detect head so torch.onnx.export sees branchwise outputs.
    detect.forward = custom_detect_forward

    output_names = []
    for i in range(detect.nl):
        output_names += [f"box_{i}", f"score_{i}", f"score_sum_{i}"]

    dummy = torch.randn(1, 3, args.imgsz, args.imgsz)
    with torch.no_grad():
        torch.onnx.export(
            model,
            dummy,
            str(onnx_path),
            opset_version=args.opset,
            input_names=["images"],
            output_names=output_names,
            dynamic_axes=None,
            do_constant_folding=True,
        )

    graph = onnx.load(str(onnx_path)).graph
    print(f"Exported: {onnx_path}")
    print("Input:")
    for i in graph.input:
        dims = [d.dim_value for d in i.type.tensor_type.shape.dim]
        print(f"  {i.name}: {dims}")
    print("Outputs:")
    for o in graph.output:
        dims = [d.dim_value for d in o.type.tensor_type.shape.dim]
        print(f"  {o.name}: {dims}")


if __name__ == "__main__":
    main()
