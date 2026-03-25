#!/usr/bin/env python3
"""
Evaluation utilities for the embedding model.

Standalone usage:
    python evaluate.py --checkpoint checkpoints/best.pt \
                       --dataset sop --data-root /path/to/SOP

Provides:
  - Recall@K (standard retrieval metric for re-ID)
  - Same-class / different-class cosine similarity distributions
  - Threshold analysis for match_confident / match_uncertain tuning
"""

from __future__ import annotations

import argparse
from pathlib import Path

import torch
import torch.nn.functional as F

from model import build_model


# ---------------------------------------------------------------------------
# Recall@K
# ---------------------------------------------------------------------------

def compute_recall_at_k(
    embeddings: torch.Tensor,
    labels: torch.Tensor,
    ks: tuple[int, ...] = (1, 2, 4, 8),
) -> list[float]:
    """
    Compute Recall@K: fraction of queries whose nearest neighbor(s)
    share the same label.

    For large datasets, processes in chunks to avoid OOM on the
    similarity matrix.
    """
    n = embeddings.size(0)
    max_k = max(ks)
    recalls = [0.0] * len(ks)

    chunk_size = min(256, n)
    for start in range(0, n, chunk_size):
        end = min(start + chunk_size, n)
        chunk = embeddings[start:end]

        # Cosine similarity (embeddings are L2-normalized)
        sim = chunk @ embeddings.t()

        # Zero out self-similarity
        for i in range(end - start):
            sim[i, start + i] = -1.0

        _, topk_idx = sim.topk(max_k, dim=1)
        topk_labels = labels[topk_idx]
        query_labels = labels[start:end].unsqueeze(1)
        matches = (topk_labels == query_labels)

        for ki, k_val in enumerate(ks):
            recalls[ki] += matches[:, :k_val].any(dim=1).float().sum().item()

    recalls = [r / n for r in recalls]
    return recalls


# ---------------------------------------------------------------------------
# Similarity distribution analysis
# ---------------------------------------------------------------------------

def compute_similarity_stats(
    embeddings: torch.Tensor,
    labels: torch.Tensor,
    max_pairs: int = 100_000,
) -> dict[str, float]:
    """
    Compute mean/std of cosine similarity for same-class and
    different-class pairs. These guide threshold tuning for
    ObjectMemory's match_confident and match_uncertain.
    """
    n = embeddings.size(0)

    # Sample random pairs to avoid O(n^2) computation
    idx_a = torch.randint(0, n, (max_pairs,))
    idx_b = torch.randint(0, n, (max_pairs,))
    valid = idx_a != idx_b
    idx_a, idx_b = idx_a[valid], idx_b[valid]

    sims = (embeddings[idx_a] * embeddings[idx_b]).sum(dim=1)
    same = labels[idx_a] == labels[idx_b]
    diff = ~same

    stats: dict[str, float] = {}
    if same.sum() > 0:
        pos_sims = sims[same]
        stats["same_class_sim_mean"] = pos_sims.mean().item()
        stats["same_class_sim_std"] = pos_sims.std().item()
        stats["same_class_sim_min"] = pos_sims.min().item()
    else:
        stats["same_class_sim_mean"] = 0.0
        stats["same_class_sim_std"] = 0.0
        stats["same_class_sim_min"] = 0.0

    if diff.sum() > 0:
        neg_sims = sims[diff]
        stats["diff_class_sim_mean"] = neg_sims.mean().item()
        stats["diff_class_sim_std"] = neg_sims.std().item()
        stats["diff_class_sim_max"] = neg_sims.max().item()
    else:
        stats["diff_class_sim_mean"] = 0.0
        stats["diff_class_sim_std"] = 0.0
        stats["diff_class_sim_max"] = 0.0

    return stats


def threshold_analysis(
    embeddings: torch.Tensor,
    labels: torch.Tensor,
    thresholds: tuple[float, ...] = (0.50, 0.55, 0.60, 0.65, 0.70, 0.75, 0.80, 0.85, 0.90),
    max_pairs: int = 100_000,
) -> list[dict[str, float]]:
    """
    For each threshold, compute precision and recall of the binary
    "same identity" decision. Helps pick match_confident / match_uncertain.
    """
    n = embeddings.size(0)
    idx_a = torch.randint(0, n, (max_pairs,))
    idx_b = torch.randint(0, n, (max_pairs,))
    valid = idx_a != idx_b
    idx_a, idx_b = idx_a[valid], idx_b[valid]

    sims = (embeddings[idx_a] * embeddings[idx_b]).sum(dim=1)
    same = (labels[idx_a] == labels[idx_b]).float()

    results = []
    for t in thresholds:
        predicted = (sims >= t).float()
        tp = (predicted * same).sum().item()
        fp = (predicted * (1 - same)).sum().item()
        fn = ((1 - predicted) * same).sum().item()

        precision = tp / max(tp + fp, 1)
        recall = tp / max(tp + fn, 1)
        f1 = 2 * precision * recall / max(precision + recall, 1e-8)

        results.append({
            "threshold": t,
            "precision": precision,
            "recall": recall,
            "f1": f1,
        })

    return results


# ---------------------------------------------------------------------------
# Standalone evaluation
# ---------------------------------------------------------------------------

def main() -> None:
    p = argparse.ArgumentParser(description="Evaluate embedding model.")
    p.add_argument("--checkpoint", required=True)
    p.add_argument("--dataset", choices=["sop", "folder"], required=True)
    p.add_argument("--data-root", required=True)
    p.add_argument("--embed-dim", type=int, default=128)
    p.add_argument("--input-size", type=int, default=128)
    p.add_argument("--batch-size", type=int, default=64)
    p.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    args = p.parse_args()

    from dataset import build_eval_loader

    model = build_model(embed_dim=args.embed_dim, pretrained=False, normalize=True)
    ckpt = torch.load(args.checkpoint, map_location=args.device, weights_only=False)
    model.load_state_dict(ckpt["model"])
    model = model.to(args.device).eval()

    split = "test" if args.dataset == "sop" else "train"
    loader = build_eval_loader(
        args.dataset, args.data_root,
        input_size=args.input_size,
        batch_size=args.batch_size,
        split=split,
    )

    print(f"Evaluating on {len(loader.dataset)} images...")

    all_emb, all_lbl = [], []
    with torch.no_grad():
        for imgs, labels in loader:
            emb = model(imgs.to(args.device))
            all_emb.append(emb.cpu())
            all_lbl.append(labels)

    all_emb = torch.cat(all_emb)
    all_lbl = torch.cat(all_lbl)

    print("\n--- Recall@K ---")
    ks = (1, 2, 4, 8)
    recalls = compute_recall_at_k(all_emb, all_lbl, ks=ks)
    for k_val, r in zip(ks, recalls):
        print(f"  R@{k_val}: {r:.4f}")

    print("\n--- Similarity Distribution ---")
    sim_stats = compute_similarity_stats(all_emb, all_lbl)
    print(f"  Same class:  mean={sim_stats['same_class_sim_mean']:.4f} "
          f"±{sim_stats['same_class_sim_std']:.4f} "
          f"min={sim_stats['same_class_sim_min']:.4f}")
    print(f"  Diff class:  mean={sim_stats['diff_class_sim_mean']:.4f} "
          f"±{sim_stats['diff_class_sim_std']:.4f} "
          f"max={sim_stats['diff_class_sim_max']:.4f}")

    print("\n--- Threshold Analysis ---")
    print(f"  {'Threshold':>10s}  {'Precision':>10s}  {'Recall':>10s}  {'F1':>10s}")
    for entry in threshold_analysis(all_emb, all_lbl):
        print(f"  {entry['threshold']:10.2f}  {entry['precision']:10.4f}  "
              f"{entry['recall']:10.4f}  {entry['f1']:10.4f}")

    print("\n--- Recommended SoulCam config ---")
    gap = sim_stats["same_class_sim_mean"] - sim_stats["diff_class_sim_max"]
    if gap > 0.15:
        conf = (sim_stats["same_class_sim_mean"] + sim_stats["diff_class_sim_max"]) / 2 + 0.05
        uncert = conf - 0.15
        print(f"  match_confident = {conf:.2f}")
        print(f"  match_uncertain = {uncert:.2f}")
    else:
        print("  WARNING: same/diff class similarity overlap is too narrow!")
        print("  Model may need more training. Using defaults:")
        print("  match_confident = 0.80")
        print("  match_uncertain = 0.65")


if __name__ == "__main__":
    main()
