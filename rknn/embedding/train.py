#!/usr/bin/env python3
"""
Train the MobileNetV3-Small embedding model for object re-identification.

Usage:
    # ArcFace on Stanford Online Products (recommended)
    python train.py --dataset sop --data-root /path/to/SOP --loss arcface

    # Triplet loss (batch-hard or batch-all)
    python train.py --dataset sop --data-root /path/to/SOP --loss batch_hard

    # Custom folder dataset
    python train.py --dataset folder --data-root /path/to/my_dataset --loss arcface

    # Resume from checkpoint
    python train.py --dataset sop --data-root /path/to/SOP --resume checkpoints/best.pt

See README.md for full instructions.
"""

from __future__ import annotations

import argparse
import json
import math
import time
from pathlib import Path

import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import DataLoader

from model import build_model, count_parameters
from dataset import (
    build_train_loader, build_eval_loader,
    StanfordOnlineProducts, FolderDataset, train_transforms,
)
from losses import BatchHardTripletLoss, BatchAllTripletLoss, ArcFaceLoss
from evaluate import compute_recall_at_k, compute_similarity_stats


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Train embedding model.")

    p.add_argument("--dataset", choices=["sop", "folder"], required=True)
    p.add_argument("--data-root", required=True, help="Dataset root directory.")
    p.add_argument("--eval-root", default="", help="Eval dataset root (if different).")

    p.add_argument("--embed-dim", type=int, default=128)
    p.add_argument("--input-size", type=int, default=128)
    p.add_argument("--epochs", type=int, default=60)
    p.add_argument("--batch-size", type=int, default=128,
                    help="Batch size (for arcface). PK sampling uses --p * --k.")
    p.add_argument("--p", type=int, default=16, help="Classes per batch (triplet).")
    p.add_argument("--k", type=int, default=4, help="Samples per class (triplet).")
    p.add_argument("--lr", type=float, default=3e-4)
    p.add_argument("--wd", type=float, default=1e-4, help="Weight decay.")
    p.add_argument("--warmup-epochs", type=int, default=3)
    p.add_argument("--margin", type=float, default=0.5,
                    help="ArcFace angular margin (default 0.5) or triplet margin.")
    p.add_argument("--arcface-scale", type=float, default=30.0)
    p.add_argument("--loss", choices=["batch_hard", "batch_all", "arcface"],
                    default="arcface")
    p.add_argument("--soft-margin", action="store_true")

    p.add_argument("--backbone-lr-scale", type=float, default=0.1,
                    help="LR multiplier for backbone (lower = less forgetting).")
    p.add_argument("--freeze-backbone-epochs", type=int, default=2,
                    help="Freeze backbone for this many initial epochs.")

    p.add_argument("--workers", type=int, default=4)
    p.add_argument("--out-dir", default="checkpoints")
    p.add_argument("--resume", default="")
    p.add_argument("--eval-every", type=int, default=5)
    p.add_argument("--save-every", type=int, default=5)
    p.add_argument("--amp", action="store_true", help="Mixed precision training.")
    p.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")

    return p.parse_args()


def set_backbone_frozen(model: nn.Module, frozen: bool) -> None:
    for param in model.features.parameters():
        param.requires_grad = not frozen


def build_optimizer(
    model: nn.Module,
    criterion: nn.Module,
    lr: float,
    wd: float,
    backbone_lr_scale: float,
) -> optim.Optimizer:
    backbone_params = list(model.features.parameters())
    head_params = list(model.head.parameters())
    criterion_params = [p for p in criterion.parameters() if p.requires_grad]

    param_groups = [
        {"params": backbone_params, "lr": lr * backbone_lr_scale},
        {"params": head_params, "lr": lr},
    ]
    if criterion_params:
        param_groups.append({"params": criterion_params, "lr": lr})

    return optim.AdamW(param_groups, weight_decay=wd)


def warmup_cosine_schedule(
    optimizer: optim.Optimizer,
    epoch: int,
    total_epochs: int,
    warmup_epochs: int,
) -> float:
    if epoch < warmup_epochs:
        factor = (epoch + 1) / warmup_epochs
    else:
        progress = (epoch - warmup_epochs) / max(1, total_epochs - warmup_epochs)
        factor = 0.5 * (1 + math.cos(math.pi * progress))
    factor = max(factor, 1e-3)

    for pg in optimizer.param_groups:
        pg["lr"] = pg["initial_lr"] * factor

    return optimizer.param_groups[1]["lr"]  # head LR


def train_one_epoch(
    model: nn.Module,
    loader,
    criterion: nn.Module,
    optimizer: optim.Optimizer,
    device: str,
    use_amp: bool = False,
    scaler=None,
) -> dict[str, float]:
    model.train()
    total_loss = 0.0
    total_metric = 0.0
    n_batches = 0

    for imgs, labels in loader:
        imgs = imgs.to(device, non_blocking=True)
        labels = labels.to(device, non_blocking=True)

        optimizer.zero_grad(set_to_none=True)

        if use_amp and scaler is not None:
            with torch.amp.autocast("cuda"):
                embeddings = model(imgs)
                loss, stats = criterion(embeddings.float(), labels)
            scaler.scale(loss).backward()
            scaler.unscale_(optimizer)
            nn.utils.clip_grad_norm_(
                list(model.parameters()) + list(criterion.parameters()),
                max_norm=5.0,
            )
            scaler.step(optimizer)
            scaler.update()
        else:
            embeddings = model(imgs)
            loss, stats = criterion(embeddings, labels)
            loss.backward()
            nn.utils.clip_grad_norm_(
                list(model.parameters()) + list(criterion.parameters()),
                max_norm=5.0,
            )
            optimizer.step()

        total_loss += loss.item()
        total_metric += stats.get("active_triplets", 0.0)
        n_batches += 1

    return {
        "train_loss": total_loss / max(n_batches, 1),
        "active_triplets": total_metric / max(n_batches, 1),
    }


@torch.no_grad()
def evaluate(
    model: nn.Module,
    loader,
    device: str,
    ks: tuple[int, ...] = (1, 2, 4, 8),
) -> dict[str, float]:
    model.eval()
    all_embeddings = []
    all_labels = []

    for imgs, labels in loader:
        imgs = imgs.to(device, non_blocking=True)
        embeddings = model(imgs)
        all_embeddings.append(embeddings.cpu())
        all_labels.append(labels)

    all_embeddings = torch.cat(all_embeddings)
    all_labels = torch.cat(all_labels)

    recalls = compute_recall_at_k(all_embeddings, all_labels, ks=ks)
    sim_stats = compute_similarity_stats(all_embeddings, all_labels)

    result = {}
    for k_val, r in zip(ks, recalls):
        result[f"R@{k_val}"] = r
    result.update(sim_stats)
    return result


def main() -> None:
    args = parse_args()
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"Device: {args.device}")
    print(f"Dataset: {args.dataset} @ {args.data_root}")
    print(f"Loss: {args.loss}")

    model = build_model(
        embed_dim=args.embed_dim,
        pretrained=True,
        normalize=(args.loss != "arcface"),
    ).to(args.device)

    info = count_parameters(model)
    print(f"Model: {info['total']:,} params ({info['trainable']:,} trainable)")

    # Build loaders — ArcFace uses shuffled DataLoader, triplet uses PK sampler
    if args.loss == "arcface":
        transform = train_transforms(args.input_size)
        if args.dataset == "sop":
            train_ds = StanfordOnlineProducts(args.data_root, split="train",
                                               transform=transform)
        else:
            train_ds = FolderDataset(args.data_root, transform=transform)
        train_loader = DataLoader(
            train_ds, batch_size=args.batch_size, shuffle=True,
            num_workers=args.workers, pin_memory=True, drop_last=True,
        )
        num_classes = train_ds.num_classes
    else:
        train_loader = build_train_loader(
            dataset_type=args.dataset, root=args.data_root,
            input_size=args.input_size, p=args.p, k=args.k,
            num_workers=args.workers,
        )
        num_classes = train_loader.dataset.num_classes

    print(f"Train: {len(train_loader.dataset)} images, {num_classes} classes")

    eval_root = args.eval_root or args.data_root
    eval_split = "test" if args.dataset == "sop" else "train"
    eval_loader = build_eval_loader(
        dataset_type=args.dataset, root=eval_root,
        input_size=args.input_size, split=eval_split,
        num_workers=args.workers,
    )
    print(f"Eval:  {len(eval_loader.dataset)} images")

    # Build loss
    if args.loss == "arcface":
        criterion = ArcFaceLoss(
            embed_dim=args.embed_dim, num_classes=num_classes,
            scale=args.arcface_scale, margin=args.margin,
        ).to(args.device)
        metric_label = "acc"
    elif args.loss == "batch_hard":
        criterion = BatchHardTripletLoss(margin=args.margin, soft=args.soft_margin)
        metric_label = "active"
    else:
        criterion = BatchAllTripletLoss(margin=args.margin)
        metric_label = "active"

    optimizer = build_optimizer(
        model, criterion, args.lr, args.wd, args.backbone_lr_scale
    )
    for pg in optimizer.param_groups:
        pg["initial_lr"] = pg["lr"]

    scaler = torch.amp.GradScaler("cuda") if args.amp and args.device == "cuda" else None

    start_epoch = 0
    best_recall = 0.0

    if args.resume:
        ckpt = torch.load(args.resume, map_location=args.device, weights_only=False)
        model.load_state_dict(ckpt["model"])
        if "criterion" in ckpt and hasattr(criterion, "load_state_dict"):
            criterion.load_state_dict(ckpt["criterion"])
        optimizer.load_state_dict(ckpt["optimizer"])
        start_epoch = ckpt.get("epoch", 0) + 1
        best_recall = ckpt.get("best_recall", 0.0)
        print(f"Resumed from epoch {start_epoch}, best R@1={best_recall:.4f}")

    log_path = out_dir / "train_log.json"
    log_entries: list[dict] = []

    for epoch in range(start_epoch, args.epochs):
        t0 = time.time()

        frozen = epoch < args.freeze_backbone_epochs
        set_backbone_frozen(model, frozen)

        lr = warmup_cosine_schedule(optimizer, epoch, args.epochs, args.warmup_epochs)

        train_stats = train_one_epoch(
            model, train_loader, criterion, optimizer, args.device,
            use_amp=args.amp, scaler=scaler,
        )
        dt = time.time() - t0

        line = (f"Epoch {epoch:3d}/{args.epochs}  "
                f"loss={train_stats['train_loss']:.4f}  "
                f"{metric_label}={train_stats['active_triplets']:.2%}  "
                f"lr={lr:.2e}  "
                f"{'[frozen]' if frozen else ''}  "
                f"({dt:.1f}s)")

        eval_stats = {}
        if (epoch + 1) % args.eval_every == 0 or epoch == args.epochs - 1:
            # For ArcFace eval, we need normalize=True temporarily
            orig_norm = model.normalize
            model.normalize = True
            eval_stats = evaluate(model, eval_loader, args.device)
            model.normalize = orig_norm

            r1 = eval_stats["R@1"]
            line += f"  R@1={r1:.4f}"

            if r1 > best_recall:
                best_recall = r1
                save_checkpoint(model, criterion, optimizer, epoch, best_recall,
                                out_dir / "best.pt")
                line += " *best*"

        print(line)

        if (epoch + 1) % args.save_every == 0:
            save_checkpoint(model, criterion, optimizer, epoch, best_recall,
                            out_dir / f"epoch_{epoch:03d}.pt")

        log_entries.append({
            "epoch": epoch,
            "lr": lr,
            **train_stats,
            **eval_stats,
            "time_s": dt,
        })

    save_checkpoint(model, criterion, optimizer, args.epochs - 1, best_recall,
                    out_dir / "final.pt")

    with open(log_path, "w") as f:
        json.dump(log_entries, f, indent=2)

    print(f"\nTraining complete. Best R@1 = {best_recall:.4f}")
    print(f"Checkpoints in {out_dir}/")
    print(f"Next step: python export_onnx.py --checkpoint {out_dir / 'best.pt'}")


def save_checkpoint(
    model: nn.Module,
    criterion: nn.Module,
    optimizer: optim.Optimizer,
    epoch: int,
    best_recall: float,
    path: Path,
) -> None:
    state = {
        "model": model.state_dict(),
        "optimizer": optimizer.state_dict(),
        "epoch": epoch,
        "best_recall": best_recall,
    }
    if hasattr(criterion, "state_dict"):
        state["criterion"] = criterion.state_dict()
    torch.save(state, path)


if __name__ == "__main__":
    main()
