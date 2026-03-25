"""
Metric learning losses for embedding model training.

Implements:
  - Batch-hard triplet loss (Hermans et al., 2017)
  - Batch-all triplet loss (softer alternative)
  - ArcFace (Deng et al., 2019) — classification-based metric learning
  - Multi-Similarity loss (Wang et al., 2019)

ArcFace trains as a classifier with angular margin, producing embeddings
that naturally cluster by class. Generally outperforms triplet losses on
datasets with many classes (like SOP with 11K+ classes).
"""

from __future__ import annotations

import torch
import torch.nn as nn
import torch.nn.functional as F


def pairwise_distances(embeddings: torch.Tensor) -> torch.Tensor:
    """
    Compute pairwise Euclidean distance matrix.

    For L2-normalized embeddings: d(a,b)^2 = 2 - 2*cos(a,b),
    so Euclidean distance directly reflects cosine similarity.
    """
    dot = embeddings @ embeddings.t()
    sq_norms = dot.diag()
    distances = sq_norms.unsqueeze(0) - 2.0 * dot + sq_norms.unsqueeze(1)
    distances = distances.clamp(min=0.0).sqrt()
    return distances


def get_masks(labels: torch.Tensor) -> tuple[torch.Tensor, torch.Tensor]:
    """Return (positive_mask, negative_mask) boolean tensors."""
    eq = labels.unsqueeze(0) == labels.unsqueeze(1)
    eye = torch.eye(len(labels), dtype=torch.bool, device=labels.device)
    pos_mask = eq & ~eye
    neg_mask = ~eq
    return pos_mask, neg_mask


class BatchHardTripletLoss(nn.Module):
    """
    For each anchor, select the hardest positive (largest distance) and
    hardest negative (smallest distance), then apply margin-based loss.

    loss = max(0, d(a, p_hard) - d(a, n_hard) + margin)

    With soft margin: loss = log(1 + exp(d(a,p) - d(a,n)))
    """

    def __init__(self, margin: float = 0.3, soft: bool = False) -> None:
        super().__init__()
        self.margin = margin
        self.soft = soft

    def forward(
        self,
        embeddings: torch.Tensor,
        labels: torch.Tensor,
    ) -> tuple[torch.Tensor, dict[str, float]]:
        dists = pairwise_distances(embeddings)
        pos_mask, neg_mask = get_masks(labels)

        # Hardest positive: max distance among positives
        pos_dists = dists.clone()
        pos_dists[~pos_mask] = 0.0
        hardest_pos, _ = pos_dists.max(dim=1)

        # Hardest negative: min distance among negatives
        neg_dists = dists.clone()
        neg_dists[~neg_mask] = float("inf")
        hardest_neg, _ = neg_dists.min(dim=1)

        if self.soft:
            loss = torch.log1p(torch.exp(hardest_pos - hardest_neg))
        else:
            loss = F.relu(hardest_pos - hardest_neg + self.margin)

        # Only count anchors that had at least one positive
        has_pos = pos_mask.any(dim=1)
        if has_pos.sum() == 0:
            return torch.tensor(0.0, device=embeddings.device), {}

        loss = loss[has_pos].mean()

        # Metrics for logging
        with torch.no_grad():
            active = (hardest_pos[has_pos] - hardest_neg[has_pos] + self.margin > 0).float().mean()
            stats = {
                "loss": loss.item(),
                "active_triplets": active.item(),
                "avg_pos_dist": hardest_pos[has_pos].mean().item(),
                "avg_neg_dist": hardest_neg[has_pos].mean().item(),
            }

        return loss, stats


class BatchAllTripletLoss(nn.Module):
    """
    Consider all valid (anchor, positive, negative) triplets in the batch.
    Averages over all triplets with positive loss (semi-hard + hard).
    """

    def __init__(self, margin: float = 0.3) -> None:
        super().__init__()
        self.margin = margin

    def forward(
        self,
        embeddings: torch.Tensor,
        labels: torch.Tensor,
    ) -> tuple[torch.Tensor, dict[str, float]]:
        dists = pairwise_distances(embeddings)
        pos_mask, neg_mask = get_masks(labels)

        # All (anchor, positive) pairs
        ap_dists = dists.unsqueeze(2)  # [B, B, 1]
        an_dists = dists.unsqueeze(1)  # [B, 1, B]

        triplet_loss = ap_dists - an_dists + self.margin  # [B, B, B]

        # Mask valid triplets: (a,p) must be positive pair, (a,n) must be negative
        valid = pos_mask.unsqueeze(2) & neg_mask.unsqueeze(1)
        triplet_loss = triplet_loss * valid.float()
        triplet_loss = F.relu(triplet_loss)

        # Average over non-zero losses
        n_active = (triplet_loss > 0).sum().float()
        if n_active > 0:
            loss = triplet_loss.sum() / n_active
        else:
            loss = torch.tensor(0.0, device=embeddings.device)

        n_valid = valid.sum().float()
        stats = {
            "loss": loss.item(),
            "active_triplets": (n_active / max(n_valid, 1.0)).item(),
        }
        return loss, stats


class ArcFaceLoss(nn.Module):
    """
    ArcFace / Additive Angular Margin loss for metric learning.

    Treats training as classification with angular margin penalty.
    The embedding features become discriminative in the cosine space,
    which is exactly what we need for re-ID.

    During training, requires a learnable weight matrix W of shape
    [num_classes, embed_dim]. After training, W is discarded and only
    the embedding backbone is exported.
    """

    def __init__(
        self,
        embed_dim: int,
        num_classes: int,
        scale: float = 30.0,
        margin: float = 0.5,
    ) -> None:
        super().__init__()
        self.scale = scale
        self.margin = margin
        self.W = nn.Parameter(torch.FloatTensor(num_classes, embed_dim))
        nn.init.xavier_uniform_(self.W)

    def forward(
        self,
        embeddings: torch.Tensor,
        labels: torch.Tensor,
    ) -> tuple[torch.Tensor, dict[str, float]]:
        # Normalize both embeddings and weights → cosine similarity
        W_norm = F.normalize(self.W, p=2, dim=1)
        cosine = F.linear(F.normalize(embeddings, p=2, dim=1), W_norm)

        # Add angular margin to the target class
        theta = cosine.acos().clamp(1e-7, 3.14159 - 1e-7)
        one_hot = F.one_hot(labels, num_classes=self.W.size(0)).float()
        target_cos = (theta + self.margin * one_hot).cos()

        # Where adding margin pushes cos below -1, fall back to cosine - sin*margin
        # (numerical stability for large angles)
        logits = self.scale * (cosine + one_hot * (target_cos - cosine))

        loss = F.cross_entropy(logits, labels)

        with torch.no_grad():
            pred = logits.argmax(dim=1)
            acc = (pred == labels).float().mean()
            stats = {
                "loss": loss.item(),
                "active_triplets": acc.item(),  # reuse field for accuracy
            }

        return loss, stats


class MultiSimilarityLoss(nn.Module):
    """
    Multi-Similarity loss (Wang et al., 2019).

    Considers three types of similarity: self-similarity, negative-relative,
    and positive-relative. Generally outperforms triplet-based losses.
    """

    def __init__(
        self,
        alpha: float = 2.0,
        beta: float = 50.0,
        base: float = 0.5,
    ) -> None:
        super().__init__()
        self.alpha = alpha
        self.beta = beta
        self.base = base

    def forward(
        self,
        embeddings: torch.Tensor,
        labels: torch.Tensor,
    ) -> tuple[torch.Tensor, dict[str, float]]:
        sim = embeddings @ embeddings.t()
        pos_mask, neg_mask = get_masks(labels)

        loss = torch.tensor(0.0, device=embeddings.device)
        count = 0

        for i in range(len(labels)):
            pos_sim = sim[i][pos_mask[i]]
            neg_sim = sim[i][neg_mask[i]]

            if len(pos_sim) == 0 or len(neg_sim) == 0:
                continue

            # Mining: keep positives harder than easiest negative
            # and negatives harder than hardest positive
            neg_max = neg_sim.max()
            pos_min = pos_sim.min()

            pos_sel = pos_sim[pos_sim < neg_max + 0.1]
            neg_sel = neg_sim[neg_sim > pos_min - 0.1]

            if len(pos_sel) == 0:
                pos_sel = pos_sim
            if len(neg_sel) == 0:
                neg_sel = neg_sim

            pos_loss = (1.0 / self.alpha) * torch.log1p(
                torch.exp(-self.alpha * (pos_sel - self.base)).sum()
            )
            neg_loss = (1.0 / self.beta) * torch.log1p(
                torch.exp(self.beta * (neg_sel - self.base)).sum()
            )

            loss = loss + pos_loss + neg_loss
            count += 1

        loss = loss / max(count, 1)

        stats = {"loss": loss.item(), "active_triplets": 0.0}
        return loss, stats


class CrossBatchMemory(nn.Module):
    """
    Optional: maintain a memory bank of recent embeddings to increase
    the effective number of negatives beyond the current batch.

    Useful when GPU memory limits batch size.
    """

    def __init__(self, embed_dim: int, memory_size: int = 4096) -> None:
        super().__init__()
        self.register_buffer("memory", torch.randn(memory_size, embed_dim))
        self.register_buffer("labels", torch.full((memory_size,), -1, dtype=torch.long))
        self.register_buffer("ptr", torch.zeros(1, dtype=torch.long))
        self.memory_size = memory_size

    @torch.no_grad()
    def update(self, embeddings: torch.Tensor, labels: torch.Tensor) -> None:
        bs = embeddings.size(0)
        ptr = int(self.ptr)
        if ptr + bs > self.memory_size:
            bs = self.memory_size - ptr
        self.memory[ptr: ptr + bs] = embeddings[:bs].detach()
        self.labels[ptr: ptr + bs] = labels[:bs].detach()
        self.ptr[0] = (ptr + bs) % self.memory_size
