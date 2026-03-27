#!/usr/bin/env python3
"""
Rewrite Objects365 YOLO-format labels to single class (class 0).

Objects365 (in YOLO format) stores one .txt per image with lines:
    <class_id> <cx> <cy> <w> <h>

This script rewrites every label file so class_id is always 0,
merging all 365 classes into a single "object" class.

It also supports downloading via the Ultralytics Objects365 integration
and setting up the directory structure.

Usage:
    # If you already have Objects365 in YOLO format:
    python merge_classes.py --labels-dir /data/Objects365/labels/train
    python merge_classes.py --labels-dir /data/Objects365/labels/val

    # Full pipeline: download + convert + organize
    python merge_classes.py --setup --src /data/Objects365 --dst /data/Objects365_1cls

    # Generate a calibration image list for RKNN INT8 quantization
    python merge_classes.py --calibration --src /data/Objects365_1cls/images/val \
                            --count 200 --output calibration_list.txt
"""

from __future__ import annotations

import argparse
import os
import random
import shutil
from pathlib import Path


def rewrite_labels(labels_dir: Path, dry_run: bool = False) -> int:
    """Rewrite all YOLO label files in-place, setting class_id to 0."""
    count = 0
    for txt_path in sorted(labels_dir.rglob("*.txt")):
        lines = txt_path.read_text().strip().split("\n")
        new_lines = []
        for line in lines:
            parts = line.strip().split()
            if len(parts) >= 5:
                parts[0] = "0"
                new_lines.append(" ".join(parts))
        if new_lines:
            if not dry_run:
                txt_path.write_text("\n".join(new_lines) + "\n")
            count += 1
    return count


def setup_dataset(src: Path, dst: Path) -> None:
    """
    Copy an existing Objects365 YOLO-format dataset to a new location
    and rewrite all labels to single class.

    Expected source layout:
        src/images/train/  src/images/val/
        src/labels/train/  src/labels/val/
    """
    dst.mkdir(parents=True, exist_ok=True)

    for split in ("train", "val"):
        img_src = src / "images" / split
        img_dst = dst / "images" / split
        lbl_src = src / "labels" / split
        lbl_dst = dst / "labels" / split

        if img_src.exists() and not img_dst.exists():
            print(f"Symlinking {img_src} -> {img_dst}")
            img_dst.parent.mkdir(parents=True, exist_ok=True)
            os.symlink(str(img_src.resolve()), str(img_dst))

        if lbl_src.exists():
            if not lbl_dst.exists():
                print(f"Copying {lbl_src} -> {lbl_dst}")
                shutil.copytree(str(lbl_src), str(lbl_dst))
            print(f"Rewriting labels in {lbl_dst}...")
            n = rewrite_labels(lbl_dst)
            print(f"  Rewrote {n} label files")


def generate_calibration_list(
    images_dir: Path, output: Path, count: int
) -> None:
    """Generate a text file with random image paths for RKNN INT8 calibration."""
    exts = {".jpg", ".jpeg", ".png", ".bmp"}
    all_images = [
        p for p in sorted(images_dir.rglob("*")) if p.suffix.lower() in exts
    ]
    if not all_images:
        raise FileNotFoundError(f"No images found in {images_dir}")

    selected = random.sample(all_images, min(count, len(all_images)))
    output.write_text("\n".join(str(p.resolve()) for p in selected) + "\n")
    print(f"Wrote {len(selected)} image paths to {output}")


def main() -> None:
    p = argparse.ArgumentParser(
        description="Merge Objects365 classes into single 'object' class."
    )
    p.add_argument(
        "--labels-dir",
        type=Path,
        help="Rewrite label files in this directory (in-place).",
    )
    p.add_argument("--dry-run", action="store_true", help="Don't modify files.")
    p.add_argument(
        "--setup",
        action="store_true",
        help="Copy dataset and rewrite labels.",
    )
    p.add_argument("--src", type=Path, help="Source dataset root.")
    p.add_argument("--dst", type=Path, help="Destination dataset root.")
    p.add_argument(
        "--calibration",
        action="store_true",
        help="Generate calibration image list.",
    )
    p.add_argument("--count", type=int, default=200, help="Calibration count.")
    p.add_argument("--output", type=Path, help="Output calibration list path.")

    args = p.parse_args()

    if args.labels_dir:
        n = rewrite_labels(args.labels_dir, dry_run=args.dry_run)
        tag = "(dry run) " if args.dry_run else ""
        print(f"{tag}Rewrote {n} label files in {args.labels_dir}")
    elif args.setup:
        if not args.src or not args.dst:
            p.error("--setup requires --src and --dst")
        setup_dataset(args.src, args.dst)
    elif args.calibration:
        if not args.src or not args.output:
            p.error("--calibration requires --src (images dir) and --output")
        generate_calibration_list(args.src, args.output, args.count)
    else:
        p.print_help()


if __name__ == "__main__":
    main()
