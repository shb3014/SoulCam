# Embedding Model for Object Re-Identification

Training, export, and deployment pipeline for the perception system's
embedding model. Produces 128-D L2-normalized feature vectors from
128×128 RGB crops, deployed as an INT8-quantized RKNN model on the
RK3566 NPU.

## Architecture

**MobileNetV3-Small** backbone with a 128-D metric learning head.

| Property | Value |
|----------|-------|
| Backbone | MobileNetV3-Small (ImageNet pretrained) |
| Input | 128×128 RGB (UINT8 NHWC on device) |
| Output | 128-D float embedding |
| Parameters | ~2.5M total |
| RKNN size | ~1.5 MB (INT8) |
| NPU latency | ~15-25 ms per crop (RK3566) |
| Training loss | Batch-hard triplet loss with margin 0.3 |

## Quick Start

### 1. Install dependencies

```bash
pip install -r requirements.txt
```

### 2. Download dataset

**Stanford Online Products** (recommended for general object re-ID):

```bash
# Download from https://cvgl.stanford.edu/projects/lifted_struct/
# Extract to e.g. /data/Stanford_Online_Products/
# Should contain: Ebay_train.txt, Ebay_test.txt, and category folders
```

**Or use a custom folder dataset:**

```
/data/my_objects/
  class_001/
    img1.jpg
    img2.jpg
  class_002/
    ...
```

### 3. Train

```bash
# Stanford Online Products
python train.py --dataset sop --data-root /data/Stanford_Online_Products \
                --epochs 60 --amp

# Custom folder dataset
python train.py --dataset folder --data-root /data/my_objects --epochs 60

# Resume training
python train.py --dataset sop --data-root /data/SOP --resume checkpoints/best.pt
```

Key training parameters:
- `--p 16 --k 4`: 16 classes × 4 samples = batch size 64
- `--lr 1e-4`: AdamW with warmup + cosine annealing
- `--freeze-backbone-epochs 2`: warm up head before fine-tuning backbone
- `--backbone-lr-scale 0.1`: backbone learns 10× slower than head
- `--margin 0.3`: triplet loss margin
- `--amp`: mixed precision (faster on GPU, same accuracy)

### 4. Evaluate

```bash
python evaluate.py --checkpoint checkpoints/best.pt \
                   --dataset sop --data-root /data/SOP
```

Outputs:
- Recall@1/2/4/8 (retrieval accuracy)
- Same-class vs different-class similarity distributions
- Recommended `match_confident` and `match_uncertain` thresholds

**Target quality:**
- Recall@1 > 0.60 on SOP test set (good for general re-ID)
- Same-class similarity mean > 0.80
- Different-class similarity max < 0.60

### 5. Export to ONNX

```bash
python export_onnx.py --checkpoint checkpoints/best.pt
# Produces: checkpoints/embedding_model.onnx
```

### 6. Generate calibration images (for INT8)

```bash
python generate_calibration_images.py \
    --dataset sop --data-root /data/SOP \
    --output-dir calibration/ --count 200
```

### 7. Convert to RKNN

```bash
# INT8 quantized (recommended for deployment)
python convert_to_rknn.py \
    --onnx checkpoints/embedding_model.onnx \
    --quant --dataset calibration/calibration_list.txt

# FP16 (no calibration needed, for testing)
python convert_to_rknn.py --onnx checkpoints/embedding_model.onnx
```

Produces: `checkpoints/embedding_rk3566_i8.rknn`

### 8. Deploy to device

```bash
# Upload model
scp checkpoints/embedding_rk3566_i8.rknn \
    ubuntu@192.168.1.45:/home/ubuntu/models/

# Set DP via store.json
ssh ubuntu@192.168.1.45
sudo python3 -c "
import json, pathlib
p = pathlib.Path('/var/lib/soulcam/store.json')
d = json.loads(p.read_text()) if p.exists() else {}
d['perception_embedder_model'] = '/home/ubuntu/models/embedding_rk3566_i8.rknn'
p.write_text(json.dumps(d, indent=2))
"
sudo systemctl restart soulcam
```

### 9. Validate on device

```bash
# Copy validation script to device
scp validate_on_device.py ubuntu@192.168.1.45:/tmp/

# Run on device
ssh ubuntu@192.168.1.45
python3 /tmp/validate_on_device.py \
    --model /home/ubuntu/models/embedding_rk3566_i8.rknn
```

Checks: model loads, inference latency <25ms, output consistency,
discrimination between different inputs.

## How It Connects to SoulCam

The trained model plugs directly into the existing embedder:

```
src/ai/embedder.h   -- EmbedderConfig.model_path points to .rknn file
src/ai/embedder.cpp  -- RKNN init, bilinear resize, inference, L2 norm
```

The embedder already handles:
- Bilinear resize of any crop to 128×128
- RKNN model loading and inference (UINT8 NHWC input)
- L2 normalization of output with NEON SIMD
- Stub mode fallback when no model is provided

Only the `.rknn` model file and the `perception_embedder_model` DP need
to be set. No C++ code changes required.

## File Index

| File | Purpose |
|------|---------|
| `model.py` | MobileNetV3-Small + 128-D head definition |
| `dataset.py` | SOP + folder dataset loaders, PK sampler |
| `losses.py` | Batch-hard/all triplet loss with mining |
| `train.py` | Full training loop (warmup, cosine LR, eval) |
| `evaluate.py` | Recall@K, similarity stats, threshold analysis |
| `export_onnx.py` | PyTorch → ONNX export with verification |
| `convert_to_rknn.py` | ONNX → RKNN (INT8/FP16) for RK3566 |
| `generate_calibration_images.py` | Extract calibration subset for INT8 |
| `validate_on_device.py` | On-device latency + correctness validation |
| `requirements.txt` | Python dependencies |

## Training Tips

- **More data helps**: If you have domain-specific objects (the things
  your SoulCam will actually see), add them as a folder dataset and
  train on a mix.
- **Hard mining converges fast**: With batch-hard triplet loss, the model
  typically converges in 30-60 epochs on SOP.
- **INT8 accuracy**: The quantization error is usually <2% Recall@1 drop.
  If it's worse, increase calibration images to 500.
- **Fine-tune for your domain**: Start from the SOP-trained checkpoint,
  then fine-tune on your own data with a lower learning rate (1e-5).
