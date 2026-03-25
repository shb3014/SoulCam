#!/usr/bin/env python3
"""
Generate calibration images for RKNN INT8 quantization.

Extracts a representative subset from the training dataset, resizes to
the model's input size, and saves as individual files with a list file.

Usage:
    python generate_calibration_images.py \
        --dataset sop --data-root /path/to/SOP \
        --output-dir calibration/ --count 200
"""

from __future__ import annotations

import argparse
import random
from pathlib import Path

from PIL import Image


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Generate RKNN calibration images.")
    p.add_argument("--dataset", choices=["sop", "folder"], required=True)
    p.add_argument("--data-root", required=True)
    p.add_argument("--output-dir", default="calibration")
    p.add_argument("--count", type=int, default=200,
                    help="Number of calibration images (100-500 recommended).")
    p.add_argument("--input-size", type=int, default=128)
    p.add_argument("--seed", type=int, default=42)
    return p.parse_args()


def collect_image_paths(dataset_type: str, data_root: str) -> list[str]:
    if dataset_type == "sop":
        import csv
        root = Path(data_root)
        paths = []
        for txt in ["Ebay_train.txt"]:
            txt_path = root / txt
            if not txt_path.exists():
                continue
            with open(txt_path) as f:
                reader = csv.reader(f, delimiter=" ")
                next(reader)
                for row in reader:
                    paths.append(str(root / row[3]))
        return paths
    else:
        root = Path(data_root)
        exts = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}
        return [str(p) for p in root.rglob("*") if p.suffix.lower() in exts]


def main() -> None:
    args = parse_args()
    random.seed(args.seed)

    paths = collect_image_paths(args.dataset, args.data_root)
    if not paths:
        raise ValueError(f"No images found in {args.data_root}")

    count = min(args.count, len(paths))
    selected = random.sample(paths, count)

    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    list_file = out_dir / "calibration_list.txt"
    entries = []

    for i, src_path in enumerate(selected):
        try:
            img = Image.open(src_path).convert("RGB")
            img = img.resize((args.input_size, args.input_size), Image.BILINEAR)
            dst = out_dir / f"cal_{i:04d}.jpg"
            img.save(dst, quality=95)
            entries.append(str(dst.resolve()))
        except Exception as e:
            print(f"  Skip {src_path}: {e}")

    list_file.write_text("\n".join(entries) + "\n")
    print(f"Generated {len(entries)} calibration images in {out_dir}/")
    print(f"List file: {list_file}")


if __name__ == "__main__":
    main()
