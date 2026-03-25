"""
Dataset loaders for embedding model training.

Supports:
  1. Stanford Online Products (SOP) — standard general object re-ID benchmark
  2. Generic folder dataset — any folder structure: root/<class_id>/image.*

Both produce (image, class_label) pairs suitable for metric learning.
"""

from __future__ import annotations

import csv
import os
import random
from pathlib import Path
from typing import Callable

import torch
from PIL import Image
from torch.utils.data import Dataset, DataLoader, Sampler
from torchvision import transforms


# ---------------------------------------------------------------------------
# Transforms
# ---------------------------------------------------------------------------

def train_transforms(input_size: int = 128) -> transforms.Compose:
    return transforms.Compose([
        transforms.Resize((input_size + 16, input_size + 16)),
        transforms.RandomCrop(input_size),
        transforms.RandomHorizontalFlip(p=0.5),
        transforms.ColorJitter(brightness=0.3, contrast=0.3, saturation=0.2, hue=0.05),
        transforms.RandomGrayscale(p=0.05),
        transforms.ToTensor(),
    ])


def eval_transforms(input_size: int = 128) -> transforms.Compose:
    return transforms.Compose([
        transforms.Resize((input_size, input_size)),
        transforms.ToTensor(),
    ])


def rknn_calibration_transforms(input_size: int = 128) -> transforms.Compose:
    """Deterministic transforms for RKNN INT8 calibration images."""
    return transforms.Compose([
        transforms.Resize((input_size, input_size)),
        transforms.ToTensor(),
    ])


# ---------------------------------------------------------------------------
# Stanford Online Products
# ---------------------------------------------------------------------------

class StanfordOnlineProducts(Dataset):
    """
    Stanford Online Products dataset.

    Download: https://cvgl.stanford.edu/projects/lifted_struct/
    Expected structure:
        root/
          Ebay_train.txt
          Ebay_test.txt
          bicycle_final/
          cabinet_final/
          ...

    The txt files have columns: image_id class_id super_class_id path
    """

    def __init__(
        self,
        root: str | Path,
        split: str = "train",
        transform: Callable | None = None,
    ) -> None:
        self.root = Path(root)
        self.transform = transform

        txt = "Ebay_train.txt" if split == "train" else "Ebay_test.txt"
        txt_path = self.root / txt
        if not txt_path.exists():
            raise FileNotFoundError(
                f"SOP metadata not found: {txt_path}\n"
                f"Download from https://cvgl.stanford.edu/projects/lifted_struct/"
            )

        self.samples: list[tuple[str, int]] = []
        self.class_to_indices: dict[int, list[int]] = {}

        with open(txt_path) as f:
            reader = csv.reader(f, delimiter=" ")
            next(reader)  # skip header
            for row in reader:
                _, class_id, _, img_path = row[0], int(row[1]), int(row[2]), row[3]
                self.samples.append((str(self.root / img_path), class_id))
                idx = len(self.samples) - 1
                self.class_to_indices.setdefault(class_id, []).append(idx)

        self.classes = sorted(self.class_to_indices.keys())
        self.num_classes = len(self.classes)
        self.class_to_label = {c: i for i, c in enumerate(self.classes)}

    def __len__(self) -> int:
        return len(self.samples)

    def __getitem__(self, idx: int) -> tuple[torch.Tensor, int]:
        path, class_id = self.samples[idx]
        img = Image.open(path).convert("RGB")
        label = self.class_to_label[class_id]
        if self.transform:
            img = self.transform(img)
        return img, label


# ---------------------------------------------------------------------------
# Generic folder dataset
# ---------------------------------------------------------------------------

class FolderDataset(Dataset):
    """
    Generic image folder dataset for metric learning.

    Structure:
        root/
          class_001/
            img1.jpg
            img2.jpg
          class_002/
            ...

    Each subfolder is a unique identity (class).
    """

    EXTENSIONS = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}

    def __init__(
        self,
        root: str | Path,
        transform: Callable | None = None,
    ) -> None:
        self.root = Path(root)
        self.transform = transform

        self.samples: list[tuple[str, int]] = []
        self.class_to_indices: dict[int, list[int]] = {}
        self.classes: list[str] = []

        class_dirs = sorted(
            d for d in self.root.iterdir()
            if d.is_dir() and not d.name.startswith(".")
        )

        for label, class_dir in enumerate(class_dirs):
            self.classes.append(class_dir.name)
            for img_path in sorted(class_dir.iterdir()):
                if img_path.suffix.lower() in self.EXTENSIONS:
                    self.samples.append((str(img_path), label))
                    idx = len(self.samples) - 1
                    self.class_to_indices.setdefault(label, []).append(idx)

        self.num_classes = len(self.classes)
        if self.num_classes == 0:
            raise ValueError(f"No class subdirectories found in {root}")

    def __len__(self) -> int:
        return len(self.samples)

    def __getitem__(self, idx: int) -> tuple[torch.Tensor, int]:
        path, label = self.samples[idx]
        img = Image.open(path).convert("RGB")
        if self.transform:
            img = self.transform(img)
        return img, label


# ---------------------------------------------------------------------------
# PK Sampler for metric learning
# ---------------------------------------------------------------------------

class PKSampler(Sampler):
    """
    Sample P classes, K images per class per batch.

    This ensures every batch contains multiple instances of each class,
    which is required for triplet mining to work effectively.
    """

    def __init__(
        self,
        class_to_indices: dict[int, list[int]],
        p: int = 16,
        k: int = 4,
    ) -> None:
        self.class_to_indices = {
            c: list(idxs) for c, idxs in class_to_indices.items()
            if len(idxs) >= 2  # need at least 2 samples per class for pairs
        }
        self.classes = list(self.class_to_indices.keys())
        self.p = min(p, len(self.classes))
        self.k = k
        self._len = self.p * self.k * (len(self.classes) // self.p)

    def __iter__(self):
        shuffled_classes = self.classes.copy()
        random.shuffle(shuffled_classes)

        for i in range(0, len(shuffled_classes) - self.p + 1, self.p):
            batch_classes = shuffled_classes[i: i + self.p]
            for cls in batch_classes:
                indices = self.class_to_indices[cls]
                if len(indices) >= self.k:
                    selected = random.sample(indices, self.k)
                else:
                    selected = random.choices(indices, k=self.k)
                yield from selected

    def __len__(self) -> int:
        return self._len


# ---------------------------------------------------------------------------
# DataLoader builders
# ---------------------------------------------------------------------------

def build_train_loader(
    dataset_type: str,
    root: str,
    input_size: int = 128,
    p: int = 16,
    k: int = 4,
    num_workers: int = 4,
) -> DataLoader:
    """Build a training DataLoader with PK sampling."""
    transform = train_transforms(input_size)

    if dataset_type == "sop":
        ds = StanfordOnlineProducts(root, split="train", transform=transform)
    elif dataset_type == "folder":
        ds = FolderDataset(root, transform=transform)
    else:
        raise ValueError(f"Unknown dataset type: {dataset_type}")

    sampler = PKSampler(ds.class_to_indices, p=p, k=k)
    return DataLoader(
        ds,
        batch_size=p * k,
        sampler=sampler,
        num_workers=num_workers,
        pin_memory=True,
        drop_last=True,
    )


def build_eval_loader(
    dataset_type: str,
    root: str,
    input_size: int = 128,
    batch_size: int = 64,
    num_workers: int = 4,
    split: str = "test",
) -> DataLoader:
    """Build an evaluation DataLoader."""
    transform = eval_transforms(input_size)

    if dataset_type == "sop":
        ds = StanfordOnlineProducts(root, split=split, transform=transform)
    elif dataset_type == "folder":
        ds = FolderDataset(root, transform=transform)
    else:
        raise ValueError(f"Unknown dataset type: {dataset_type}")

    return DataLoader(
        ds,
        batch_size=batch_size,
        shuffle=False,
        num_workers=num_workers,
        pin_memory=True,
    )
