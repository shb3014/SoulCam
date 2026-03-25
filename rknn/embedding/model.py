"""
MobileNetV3-Small embedding model for object re-identification.

Produces 128-D L2-normalized feature vectors from 128x128 RGB crops.
Designed for RK3566 NPU deployment via RKNN (INT8 quantized).

Architecture:
    MobileNetV3-Small backbone (ImageNet pretrained)
    → AdaptiveAvgPool2d(1)
    → Linear(576, 128)
    → L2 normalize

The backbone produces rich visual features; the projection head maps
them to a compact metric space where cosine similarity measures identity.
"""

from __future__ import annotations

import torch
import torch.nn as nn
import torch.nn.functional as F
from torchvision.models import mobilenet_v3_small, MobileNet_V3_Small_Weights


class EmbeddingModel(nn.Module):
    """
    Lightweight embedding CNN for general object re-identification.

    Args:
        embed_dim: Dimension of output embedding (default 128).
        pretrained: Use ImageNet-pretrained backbone weights.
        normalize: L2-normalize output during forward pass.
                   Disable for ONNX export (normalization done in C++).
    """

    BACKBONE_DIM = 576  # MobileNetV3-Small feature channels

    def __init__(
        self,
        embed_dim: int = 128,
        pretrained: bool = True,
        normalize: bool = True,
    ) -> None:
        super().__init__()
        self.embed_dim = embed_dim
        self.normalize = normalize

        weights = MobileNet_V3_Small_Weights.DEFAULT if pretrained else None
        backbone = mobilenet_v3_small(weights=weights)

        # Keep everything except the classifier
        self.features = backbone.features
        self.pool = nn.AdaptiveAvgPool2d(1)

        self.head = nn.Sequential(
            nn.Linear(self.BACKBONE_DIM, self.BACKBONE_DIM, bias=False),
            nn.BatchNorm1d(self.BACKBONE_DIM),
            nn.Hardswish(inplace=True),
            nn.Linear(self.BACKBONE_DIM, embed_dim, bias=False),
            nn.BatchNorm1d(embed_dim),
        )

        self._init_head()

    def _init_head(self) -> None:
        for m in self.head.modules():
            if isinstance(m, nn.Linear):
                nn.init.kaiming_normal_(m.weight, mode="fan_out")
            elif isinstance(m, nn.BatchNorm1d):
                nn.init.constant_(m.weight, 1.0)
                nn.init.constant_(m.bias, 0.0)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """
        Args:
            x: [B, 3, H, W] RGB tensor, values in [0, 1].
        Returns:
            [B, embed_dim] embedding vectors.
        """
        f = self.features(x)
        f = self.pool(f).flatten(1)
        e = self.head(f)
        if self.normalize:
            e = F.normalize(e, p=2, dim=1)
        return e


class EmbeddingModelForExport(nn.Module):
    """
    Wrapper that accepts UINT8 [0,255] NHWC input for RKNN export.

    RKNN applies mean/std normalization itself (configured during conversion),
    so this wrapper only handles the layout and type conversion that the
    ONNX graph needs to represent.
    """

    def __init__(self, model: EmbeddingModel) -> None:
        super().__init__()
        model.normalize = False  # C++ side does L2 norm with NEON
        self.model = model

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """
        Args:
            x: [B, 3, H, W] float tensor (RKNN handles uint8→float + norm).
        Returns:
            [B, embed_dim] raw (un-normalized) embeddings.
        """
        return self.model(x)


def build_model(
    embed_dim: int = 128,
    pretrained: bool = True,
    normalize: bool = True,
) -> EmbeddingModel:
    return EmbeddingModel(
        embed_dim=embed_dim,
        pretrained=pretrained,
        normalize=normalize,
    )


def count_parameters(model: nn.Module) -> dict[str, int]:
    total = sum(p.numel() for p in model.parameters())
    trainable = sum(p.numel() for p in model.parameters() if p.requires_grad)
    return {"total": total, "trainable": trainable}


if __name__ == "__main__":
    m = build_model(embed_dim=128, pretrained=False)
    m.eval()
    info = count_parameters(m)
    print(f"Parameters: {info['total']:,} total, {info['trainable']:,} trainable")

    x = torch.randn(1, 3, 128, 128)
    with torch.no_grad():
        y = m(x)
    print(f"Input:  {x.shape}")
    print(f"Output: {y.shape}")
    print(f"L2 norm: {y.norm(dim=1).item():.4f} (should be ~1.0)")
