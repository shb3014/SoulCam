#!/usr/bin/env python3
"""
Export trained embedding model to ONNX format.

The exported model:
  - Input:  [1, 3, 128, 128] float32 (NCHW, values 0-255)
  - Output: [1, 128] float32 (raw embedding, un-normalized)

RKNN handles uint8→float conversion and mean/std normalization.
The C++ embedder applies L2 normalization after inference.

Usage:
    python export_onnx.py --checkpoint checkpoints/best.pt
    python export_onnx.py --checkpoint checkpoints/best.pt --output models/embed.onnx
"""

from __future__ import annotations

import argparse
from pathlib import Path

import torch
import numpy as np

from model import build_model, EmbeddingModelForExport


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Export embedding model to ONNX.")
    p.add_argument("--checkpoint", required=True, help="Path to trained .pt checkpoint.")
    p.add_argument("--output", default="", help="Output ONNX path (default: auto).")
    p.add_argument("--embed-dim", type=int, default=128)
    p.add_argument("--input-size", type=int, default=128)
    p.add_argument("--opset", type=int, default=12,
                    help="ONNX opset version (rknn-toolkit2 supports 12+).")
    p.add_argument("--verify", action="store_true", default=True,
                    help="Verify ONNX output matches PyTorch.")
    return p.parse_args()


def main() -> None:
    args = parse_args()

    ckpt = torch.load(args.checkpoint, map_location="cpu", weights_only=False)

    model = build_model(embed_dim=args.embed_dim, pretrained=False, normalize=True)
    model.load_state_dict(ckpt["model"])
    model.eval()

    export_model = EmbeddingModelForExport(model)
    export_model.eval()

    out_path = args.output or str(
        Path(args.checkpoint).parent / "embedding_model.onnx"
    )
    Path(out_path).parent.mkdir(parents=True, exist_ok=True)

    dummy = torch.randn(1, 3, args.input_size, args.input_size)

    print(f"Exporting to {out_path}...")
    torch.onnx.export(
        export_model,
        dummy,
        out_path,
        input_names=["input"],
        output_names=["embedding"],
        dynamic_axes=None,
        opset_version=args.opset,
        do_constant_folding=True,
    )
    print(f"Exported: {out_path} ({Path(out_path).stat().st_size / 1024:.1f} KB)")

    if args.verify:
        verify_onnx(out_path, model, args.input_size, args.embed_dim)

    # Also generate a calibration image list template
    cal_path = Path(out_path).parent / "calibration_list.txt"
    if not cal_path.exists():
        cal_path.write_text(
            "# One image path per line for INT8 calibration.\n"
            "# Use ~100-500 representative images from training data.\n"
            "# Example:\n"
            "# /path/to/image1.jpg\n"
            "# /path/to/image2.jpg\n"
        )
        print(f"Created calibration list template: {cal_path}")

    print(f"\nNext step: python convert_to_rknn.py --onnx {out_path}")


def verify_onnx(
    onnx_path: str,
    pytorch_model: torch.nn.Module,
    input_size: int,
    embed_dim: int,
) -> None:
    """Verify ONNX output matches PyTorch output."""
    try:
        import onnxruntime as ort
    except ImportError:
        print("  (onnxruntime not installed, skipping ONNX verification)")
        return

    pytorch_model.normalize = False
    pytorch_model.eval()

    test_input = torch.randn(1, 3, input_size, input_size)

    with torch.no_grad():
        pt_out = pytorch_model(test_input).numpy()

    sess = ort.InferenceSession(onnx_path)
    ort_out = sess.run(None, {"input": test_input.numpy()})[0]

    max_diff = np.abs(pt_out - ort_out).max()
    mean_diff = np.abs(pt_out - ort_out).mean()

    status = "PASS" if max_diff < 1e-4 else "WARN"
    print(f"  ONNX verification: {status} (max_diff={max_diff:.6f}, mean_diff={mean_diff:.6f})")

    if max_diff >= 1e-4:
        print("  WARNING: ONNX output differs from PyTorch. Check opset compatibility.")


if __name__ == "__main__":
    main()
