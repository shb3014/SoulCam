# Class-Agnostic Object Proposer

Training, export, and deployment pipeline for class-agnostic detection
models. Enables the SoulCam perception system to detect **any** object,
not just the 80 COCO classes the current YOLOv8n model recognizes.

## Option A: YOLOv8n Single-Class Retrain on Objects365

Retrains the same YOLOv8n architecture on Objects365 (365 diverse
categories) with all classes merged into a single "object" class.
Zero C++ architecture changes — only the `.rknn` model file and one
label DP change.

### 1. Install dependencies

```bash
cd rknn/proposer
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

Or reuse the existing embedding venv:

```bash
source rknn/embedding/.venv/bin/activate
pip install ultralytics
```

### 2. Download Objects365

Ultralytics can download automatically during training, but the dataset
is ~50 GB. To download manually:

```bash
# Ultralytics auto-download (will download to ~/datasets/Objects365/):
yolo detect train model=yolov8n.pt data=Objects365.yaml epochs=1 imgsz=640
# (cancel after download completes)

# Or download from https://www.objects365.org/download.html
```

### 3. Prepare single-class labels

```bash
# Copy dataset and rewrite all labels to class 0
python merge_classes.py --setup \
    --src ~/datasets/Objects365 \
    --dst /data/Objects365_1cls

# Or rewrite labels in-place (modifies original files):
python merge_classes.py --labels-dir /data/Objects365/labels/train
python merge_classes.py --labels-dir /data/Objects365/labels/val
```

### 4. Train

```bash
yolo detect train \
    model=yolov8n.pt \
    data=obj365_1cls.yaml \
    epochs=50 \
    imgsz=640 \
    batch=64 \
    device=0
```

Training takes ~12-24 hours on a single GPU depending on hardware.
Best weights saved to `runs/detect/train/weights/best.pt`.

### 5. Export to ONNX

```bash
yolo export model=runs/detect/train/weights/best.pt format=onnx imgsz=640
# Produces: runs/detect/train/weights/best.onnx
```

### 6. Generate calibration images

```bash
python merge_classes.py --calibration \
    --src /data/Objects365_1cls/images/val \
    --count 200 \
    --output calibration_list.txt
```

### 7. Convert to RKNN

```bash
# INT8 quantized (recommended)
python convert_yolo_rknn.py \
    --onnx runs/detect/train/weights/best.onnx \
    --quant --dataset calibration_list.txt

# FP16 (for testing, no calibration needed)
python convert_yolo_rknn.py \
    --onnx runs/detect/train/weights/best.onnx
```

Produces: `runs/detect/train/weights/yolov8n_obj365_1cls_rk3566_i8.rknn`

### 8. Deploy to device

```bash
# Upload model
scp runs/detect/train/weights/yolov8n_obj365_1cls_rk3566_i8.rknn \
    ubuntu@192.168.1.45:/home/ubuntu/models/

# Update store.json on device
ssh ubuntu@192.168.1.45
sudo python3 -c "
import json, pathlib
p = pathlib.Path('/var/lib/soulcam/store.json')
d = json.loads(p.read_text()) if p.exists() else {}
d['ai_model_path'] = '/home/ubuntu/models/yolov8n_obj365_1cls_rk3566_i8.rknn'
d['ai_labels'] = 'object'
p.write_text(json.dumps(d, indent=2))
"

# Rebuild soulcam (picks up object_memory.cpp change)
cd ~/SoulCam && bash scripts/build.sh

# Restart
sudo systemctl restart soulcam
```

### 9. Verify

```bash
# Check model loaded
journalctl -u soulcam --no-pager -n 50 | grep -i "rknn\|model\|classes"

# Check detections appear (should show "object" label)
journalctl -u soulcam -f | grep -i "detect\|object"

# Check object memory enrollment
cat /var/lib/soulcam/memory/memory.json | python3 -m json.tool | head -40
```

### What changes on the C++ side

Only one line in `src/ai/object_memory.cpp`:

```cpp
// Before: strict class match
if (obj.coarse_class != coarse_class) continue;

// After: skip class filter for single-class model output
if (coarse_class != "object" && obj.coarse_class != coarse_class) continue;
```

This allows the embedding-based matching to work across all objects
regardless of class, since the single-class model labels everything
as "object".

The detector itself (`detector.cpp`) needs no changes — it auto-detects
the number of classes from the model's score tensor shape and uses the
`ai_labels` DP for label names.

## File Index

| File | Purpose |
|------|---------|
| `obj365_1cls.yaml` | Ultralytics dataset config (1 class) |
| `merge_classes.py` | Rewrite Objects365 labels to class 0 + calibration list gen |
| `convert_yolo_rknn.py` | ONNX → RKNN conversion for YOLOv8n |
| `requirements.txt` | Python dependencies |

## Benchmark Protocol

See the [trial plan](../../.cursor/plans/class-agnostic_detector_trials_4e27de4e.plan.md)
for the full standardized benchmark protocol shared across all options.
