#!/usr/bin/env python3
"""
End-to-end training script for single-class YOLOv8n.

Downloads COCO (or Objects365), rewrites labels to single class,
and trains YOLOv8n. Handles the full pipeline so you can just run:

    python train_1cls.py                         # COCO (default, ~20GB, faster)
    python train_1cls.py --dataset objects365     # Objects365 (~50GB, broader)
    python train_1cls.py --epochs 100 --batch 32 # custom training params

The script is idempotent: re-running skips download and label rewriting
if already done.
"""

from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path


def rewrite_labels_dir(labels_dir: Path) -> int:
    """Rewrite all YOLO label files in a directory, setting class_id to 0."""
    marker = labels_dir / ".merged_1cls"
    if marker.exists():
        print(f"  Labels already merged: {labels_dir}")
        return 0

    count = 0
    for txt_path in sorted(labels_dir.rglob("*.txt")):
        if txt_path.name.startswith("."):
            continue
        lines = txt_path.read_text().strip().split("\n")
        new_lines = []
        for line in lines:
            parts = line.strip().split()
            if len(parts) >= 5:
                parts[0] = "0"
                new_lines.append(" ".join(parts))
        if new_lines:
            txt_path.write_text("\n".join(new_lines) + "\n")
            count += 1

    marker.write_text(f"Merged {count} files\n")
    return count


def download_coco(datasets_dir: Path) -> Path:
    """Trigger COCO download via Ultralytics if not present."""
    coco_dir = datasets_dir / "coco"
    train_imgs = coco_dir / "images" / "train2017"
    if train_imgs.exists() and len(list(train_imgs.iterdir())) > 100:
        print(f"COCO already downloaded: {coco_dir}")
        return coco_dir

    print("Downloading COCO via Ultralytics...")
    from ultralytics import YOLO
    model = YOLO("yolov8n.pt")
    model.val(data="coco.yaml", imgsz=640, batch=1, verbose=False)
    return coco_dir


def prepare_coco_1cls(datasets_dir: Path) -> str:
    """Download COCO and rewrite labels to single class."""
    coco_dir = download_coco(datasets_dir)

    train_labels = coco_dir / "labels" / "train2017"
    val_labels = coco_dir / "labels" / "val2017"

    if train_labels.exists():
        n = rewrite_labels_dir(train_labels)
        if n:
            print(f"  Rewrote {n} train label files")
    if val_labels.exists():
        n = rewrite_labels_dir(val_labels)
        if n:
            print(f"  Rewrote {n} val label files")

    script_dir = Path(__file__).parent
    return str(script_dir / "coco_1cls.yaml")


def prepare_obj365_1cls(datasets_dir: Path) -> str:
    """Download Objects365 and rewrite labels to single class."""
    obj_dir = datasets_dir / "Objects365"
    if not (obj_dir / "images").exists():
        print("Downloading Objects365 via Ultralytics...")
        print("(This is a ~50GB download and may take several hours)")
        from ultralytics import YOLO
        model = YOLO("yolov8n.pt")
        model.val(data="Objects365.yaml", imgsz=640, batch=1, verbose=False)

    for split_dir in obj_dir.glob("labels/*"):
        if split_dir.is_dir():
            n = rewrite_labels_dir(split_dir)
            if n:
                print(f"  Rewrote {n} label files in {split_dir.name}")

    script_dir = Path(__file__).parent
    return str(script_dir / "obj365_1cls.yaml")


def main() -> None:
    p = argparse.ArgumentParser(description="Train single-class YOLOv8n.")
    p.add_argument(
        "--dataset",
        choices=["coco", "objects365"],
        default="coco",
        help="Base dataset to merge into single class.",
    )
    p.add_argument("--epochs", type=int, default=50)
    p.add_argument("--batch", type=int, default=64)
    p.add_argument("--imgsz", type=int, default=640)
    p.add_argument("--device", default="0")
    p.add_argument("--resume", action="store_true", help="Resume training.")
    p.add_argument(
        "--datasets-dir",
        type=Path,
        default=None,
        help="Override datasets root directory.",
    )
    args = p.parse_args()

    datasets_dir = args.datasets_dir
    if datasets_dir is None:
        from ultralytics import settings
        datasets_dir = Path(settings.get("datasets_dir", "../datasets"))

    datasets_dir = datasets_dir.resolve()
    print(f"Datasets directory: {datasets_dir}")

    if args.dataset == "coco":
        data_yaml = prepare_coco_1cls(datasets_dir)
    else:
        data_yaml = prepare_obj365_1cls(datasets_dir)

    print(f"\nStarting training:")
    print(f"  Dataset config: {data_yaml}")
    print(f"  Epochs: {args.epochs}")
    print(f"  Batch size: {args.batch}")
    print(f"  Image size: {args.imgsz}")
    print(f"  Device: {args.device}")

    from ultralytics import YOLO
    model = YOLO("yolov8n.pt")
    results = model.train(
        data=data_yaml,
        epochs=args.epochs,
        imgsz=args.imgsz,
        batch=args.batch,
        device=args.device,
        resume=args.resume,
        project="runs/proposer",
        name="yolov8n_1cls",
        exist_ok=True,
    )

    best_pt = Path("runs/proposer/yolov8n_1cls/weights/best.pt")
    if best_pt.exists():
        print(f"\nTraining complete!")
        print(f"Best weights: {best_pt}")
        print(f"\nNext steps:")
        print(f"  1. Export: yolo export model={best_pt} format=onnx imgsz=640")
        print(f"  2. Convert: python convert_yolo_rknn.py --onnx {best_pt.with_suffix('.onnx')}")
    else:
        print(f"\nTraining finished. Check runs/proposer/yolov8n_1cls/ for results.")


if __name__ == "__main__":
    main()
