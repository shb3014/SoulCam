# Class-Agnostic Object Detection: Comprehensive Analysis for SoulCam

_Date: 2026-03-24_
_Platform: RK3566 (0.8 TOPS NPU, Cortex-A55 quad-core, 1 GB RAM)_
_Current detector: YOLOv8n (80 COCO classes, 640×640, ~35.7 FPS INT8 on RK3566)_

---

## 1. Problem Statement

The current SoulCam perception pipeline uses YOLOv8n, which can only detect objects
belonging to 80 COCO categories. This creates a fundamental **scalability ceiling**:

- A coffee mug on a desk? Detected as "cup" ✓
- A circuit board? **Invisible** ✗
- A specific plant species? Detected as "potted plant" ✓ (but no fine-grained identity)
- A guitar pick? **Invisible** ✗
- A power bank? **Invisible** ✗

The SoulCam's architecture already has the downstream components to handle arbitrary
objects: the **embedder** learns visual identity, the **VLM** provides semantic labels,
and the **object memory** stores everything persistently. The only bottleneck is the
front door — the detector that decides which regions of the image contain "something."

**Goal**: Replace or augment YOLO with a model that proposes object regions
**regardless of category**, feeding them into the existing embedder → memory → VLM
pipeline for identification.

---

## 2. Candidate Architectures

### 2.1 Option A: YOLO Single-Class Retrain

**Concept**: Take YOLOv8n (same architecture we already run), but retrain it on a
large-vocabulary dataset (Objects365: 365 categories, 2M images, 30M boxes) with
**all class labels merged into a single "object" class**. The model learns to output
bounding boxes for anything that looks like a discrete object.

| Property | Value |
|----------|-------|
| Architecture | YOLOv8n (identical to current) |
| Parameters | 3.16M (unchanged) |
| FLOPs | 1.10G at 320×320 |
| Input | 320×320 or 640×640 RGB |
| Output | Bounding boxes + objectness score |
| Training data | Objects365 (365 cats) or LVIS (1203 cats), all merged to 1 class |
| RKNN compatible | **Yes** — identical model architecture, proven pipeline |
| Expected RK3566 FPS | ~36 FPS at 640×640, ~100+ FPS at 320×320 (INT8) |
| NMS required | Yes (same as current) |

**Strengths**:
- Zero toolchain risk — same ONNX→RKNN conversion, same C++ inference code
- Same post-processing (NMS, box decode) already implemented in `detector.cpp`
- Objects365 pre-training has been shown to improve COCO AP by 5.6 points; the
  learned features are much more general than COCO-only
- Can potentially hot-swap with the current model (same `detector.h` API)
- Bounding boxes are needed downstream (cropping, IoU association, KCF init)

**Weaknesses**:
- Still fundamentally a **learned objectness** — the model can only propose things
  that resemble the 365/1203 training categories. Truly novel objects (e.g., custom
  PCBs, unusual art pieces) may still be missed
- The 80→365→1203 scaling helps a lot, but doesn't reach "universal objectness"
- Single-class NMS at high recall may produce more false positives (background clutter)
- Objects365 training requires significant compute (a V100 or better for days)

**Generalization estimate** (based on literature):
- OLN paper shows that standard RPN trained on COCO detects ~55% of novel LVIS categories
- Objects365-trained models improve cross-category recall by ~15-20% over COCO
- LVIS (1203 categories) covers most everyday objects comprehensively

---

### 2.2 Option B: CenterNet with Lightweight Backbone (Single-Class)

**Concept**: CenterNet is an anchor-free detector that outputs **heatmaps** of object
center probabilities plus width/height regression. It naturally lends itself to
class-agnostic detection by using a single heatmap channel. With a MobileNetV3 backbone,
it can be very lightweight.

| Property | Value |
|----------|-------|
| Architecture | CenterNet (heatmap + WH regression) |
| Backbone | MobileNetV3-Small or MobileNetV3-Large |
| Parameters | ~2.5M (Small) / ~5.5M (Large) |
| FLOPs | ~0.3G (Small) / ~0.7G (Large) at 320×320 |
| Input | 320×320 or 512×512 RGB |
| Output | Heatmap (1 channel) + WH regression (2 channels) |
| Training data | Objects365 or COCO, all classes merged |
| RKNN compatible | **Likely yes** — Conv + BN + ReLU only, no exotic ops |
| Expected RK3566 FPS | ~60-100 FPS at 320×320 (estimated from MobileNetV2 benchmark) |
| NMS required | No (peak extraction on heatmap, much simpler) |

**Strengths**:
- NMS-free: peak detection on a heatmap is trivial (local maxima with threshold)
- MobileNetV3 backbone already validated on RK3566 (our embedder uses it)
- Naturally produces center+size, which maps cleanly to our `TrackSlot` struct
- Very fast — MobileNetV3-Small at 224×224 runs at ~200 FPS on RK3566 NPU;
  with FPN neck and regression heads the overhead is moderate
- Heatmap output is dense — discovers small objects better than YOLO at low resolution
- CenterNet-MobileNetV3 implementations exist on GitHub with ONNX export

**Weaknesses**:
- Not a standard pre-built RKNN model — requires custom training + conversion
- WH regression head accuracy may be lower than YOLO's box prediction at small scales
- Less mature ecosystem than YOLO for edge deployment
- Heatmap resolution is 1/4 of input (80×80 for 320×320 input), which limits
  localization precision for very small objects
- Need to build training pipeline from scratch (though simpler than YOLO training)

**Key reference**: MobileNetV3-CenterNet achieves 99.4% recognition accuracy on
industrial detection tasks at 53 FPS on server GPU and 14 FPS on iPad (unquantized).
INT8 quantized on RK3566 NPU should be significantly faster.

---

### 2.3 Option C: OCDet (Object Center Detection)

**Concept**: A 2024 framework specifically designed for NPU edge deployment. Uses
MobileNetV4 backbone + Semantic FPN to predict center-probability heatmaps.
Achieves 64% lower NPU latency than YOLO11 with 42% fewer parameters.

| Property | Value |
|----------|-------|
| Architecture | Semantic FPN + peak identification |
| Backbone | MobileNetV4 (Nano to Extra-large variants) |
| Parameters | 1.51M (N) / 1.59M (S) / 8.25M (M) |
| FLOPs | 0.54G (N) / 0.94G (S) / 3.54G (M) at 320×320 |
| Input | 320×320 RGB |
| Output | Per-class heatmaps (centers only, no bounding boxes) |
| Training data | COCO (can adapt to Objects365) |
| Benchmark NPU | i.MX 8M Plus (2.3 TOPS) — 10.94ms (N), 24.25ms (S) |
| RKNN compatible | **Unknown** — MobileNetV4 uses some ops not validated on RKNN |
| Expected RK3566 FPS | ~15-30 FPS (N variant, estimated scaling from 2.3→0.8 TOPS) |
| NMS required | No (peak identification on heatmap) |

**Strengths**:
- Purpose-built for NPU deployment; authors explicitly optimize for NPU-unfriendly
  op avoidance (no GELU, H-Swish, SE-blocks, LayerNorm, MHSA)
- Dramatically higher recall than YOLO — up to 245% improvement in recall at
  equivalent model sizes (46.6% R vs YOLO11n's 19.8% R)
- Smallest variant (OCDet-N) is half the params of YOLOv8n
- Open-source implementation available (github.com/chen-xin-94/ocdet)
- Published comparison table directly against YOLO11 and YOLOv8

**Weaknesses**:
- **No bounding boxes** — only outputs center coordinates. We need boxes for
  cropping, IoU association, and KCF initialization. Would need to either:
  (a) add WH regression heads (modifying the architecture), or
  (b) estimate box from heatmap spread + scale prior
- MobileNetV4 RKNN compatibility is unverified — MobileNetV4 is newer than
  MobileNetV3 and may use ops not in rknn-toolkit2's supported set
- Benchmarked on i.MX 8M Plus (2.3 TOPS), not RK3566 (0.8 TOPS) — latency
  would roughly scale by 2.3/0.8 ≈ 2.9x, putting OCDet-N at ~32ms (~31 FPS)
  and OCDet-S at ~70ms (~14 FPS)
- Per-class heatmaps (80 channels for COCO) — for class-agnostic use, would
  need to retrain with a single channel or max-pool across channels
- Research paper (Nov 2024), not battle-tested in production

**Performance comparison from paper** (i.MX 8M Plus, 2.3 TOPS):

| Model | Params | FLOPs | Latency | CAS | Recall |
|-------|--------|-------|---------|-----|--------|
| YOLOv8n | 3.16M | 1.10G | 26.95ms | 0.240 | 19.5% |
| YOLO11n | 2.62M | 0.82G | 30.77ms | 0.241 | 19.8% |
| OCDet-N | 1.51M | 0.54G | 10.94ms | 0.297 | **46.6%** |
| OCDet-S | 1.59M | 0.94G | 24.25ms | 0.313 | **50.7%** |

---

### 2.4 Option D: OLN (Object Localization Network)

**Concept**: Classification-free proposal network that learns objectness purely
through localization quality (centerness + IoU overlap with any ground truth object).
Specifically designed for open-world generalization.

| Property | Value |
|----------|-------|
| Architecture | Faster R-CNN RPN-style, modified heads |
| Backbone | ResNet-50-FPN (standard), no lightweight variant |
| Parameters | ~26M (ResNet-50-FPN) |
| FLOPs | ~16G at 800×800 |
| Input | Variable (typically 800×800) |
| Output | Class-agnostic bounding box proposals + objectness score |
| Training data | COCO (generalizes to novel categories) |
| RKNN compatible | **No** — ResNet-50-FPN is too heavy for RK3566 NPU |
| Expected RK3566 FPS | <5 FPS (far too slow) |

**Strengths**:
- Best cross-category generalization: detects objects from categories never seen
  during training (validated on LVIS, Objects365, EpicKitchens, RoboNet)
- Theoretically the purest form of "objectness" — no class bias at all
- Directly outputs bounding boxes (compatible with our pipeline)
- ONNX implementation available

**Weaknesses**:
- **Far too heavy** for RK3566 — ResNet-50-FPN at 26M params, ~16 GFLOPs
- No lightweight variant exists
- Two-stage architecture (RPN + head) adds complexity
- Would need to be re-architected with MobileNet backbone to be viable,
  which is essentially building a new model

**Verdict**: Excellent research contribution but **not deployable** on our hardware
in its current form.

---

### 2.5 Option E: SAM Variants (MobileSAM, EdgeSAM, NanoSAM)

**Concept**: Segment Anything Model distilled to lightweight encoders for mobile/edge
deployment. Can segment any object given point or box prompts.

| Property | MobileSAM | EdgeSAM | NanoSAM (PPHGV2-B1) |
|----------|-----------|---------|---------------------|
| Encoder params | 6.95M | 9.6M | ~5M |
| Encoder size | 26.6 MB | ~20 MB | 12.7 MB |
| Decoder params | 6.16M | ~6M | ~6M |
| Total size | 40.7 MB | ~30 MB | ~18 MB |
| Encoder latency | 8ms (GPU) | ~5ms (iPhone 14) | 110ms (CPU) |
| Requires prompts | **Yes** | **Yes** | **Yes** |
| RKNN compatible | Uncertain | Uncertain | Uncertain |

**Strengths**:
- Can segment literally any object
- Active development community
- MobileSAM + EdgeSAM achieve good segmentation quality

**Weaknesses**:
- **Require prompts** (points or boxes) — they are not standalone detectors.
  You still need another model to tell SAM *where* to look. This creates
  a chicken-and-egg problem: we need a detector to prompt SAM, but we want
  SAM to replace the detector
- Designed for segmentation masks, not detection — overkill for our needs
  (we only need bounding boxes for crop extraction)
- Even the smallest variant (NanoSAM) at 12.7 MB is large by RK3566 standards
- CPU inference at 110ms for encoder alone is too slow
- NPU compatibility with ViT-based decoders is questionable on RKNN
- Total model size (18-41 MB) would consume significant device storage

**Verdict**: Wrong tool for the job. SAM variants are segmenters, not detectors.
They solve a different problem and can't replace the detection stage.

---

### 2.6 Option F: YOLO-World (Open Vocabulary)

**Concept**: YOLO-World extends YOLOv8 with vision-language capabilities,
enabling detection of any object described by text prompt. Uses offline vocabulary
embedding for efficient inference.

| Property | Value |
|----------|-------|
| Architecture | YOLOv8 + RepVL-PAN |
| Backbone | YOLOv8s backbone + CLIP text encoder |
| Parameters | ~11M+ (smallest) |
| FLOPs | ~3.5G+ |
| Input | 640×640 RGB + text vocabulary |
| Output | Bounding boxes + class scores per vocabulary word |
| RKNN compatible | **No** — requires text encoder + vision-language fusion |
| Expected RK3566 FPS | <5 FPS (too heavy) |

**Strengths**:
- True open vocabulary — detect anything describable by text
- No retraining needed for new categories
- Built on proven YOLOv8 architecture

**Weaknesses**:
- **Much too heavy** for RK3566 — smallest variant is 11M+ params with 3.5G+ FLOPs
- Requires a text encoder at inference (or pre-computed vocabulary embeddings)
- RepVL-PAN uses attention mechanisms not supported by RKNN
- Even with offline vocabulary, the model itself is too large
- Designed for V100 GPU deployment

**Verdict**: Excellent concept but **completely impractical** for RK3566.

---

### 2.7 Option G: RT-DETR Lightweight Variants

**Concept**: Real-time detection transformer, eliminates NMS, uses efficient
hybrid encoder. LRT-DETR variant achieves 12M params.

| Property | Value |
|----------|-------|
| Architecture | CNN backbone + transformer decoder |
| Parameters | 12M (LRT-DETR) to 32M (RT-DETR-L) |
| FLOPs | ~8-20G |
| RKNN compatible | **No** — transformer decoder uses LayerNorm, MHSA |
| Expected RK3566 FPS | <5 FPS |

**Verdict**: Transformer components are **not NPU-friendly** on RKNN.
The OCDet paper explicitly confirms this: "LayerNorm and MHSA in transformer
architectures are not well optimized for NPUs." Not viable.

---

## 3. RK3566 NPU Baseline Benchmarks

All measurements INT8 quantized on RK3566 NPU (0.8 TOPS):

| Model | Input | Params | FPS |
|-------|-------|--------|-----|
| MobileNetV2 | 224×224 | 3.4M | 197.4 |
| ResNet50-v2 | 224×224 | 25.6M | 40.6 |
| YOLOv5n | 640×640 | 1.9M | 41.6 |
| YOLOv5s | 640×640 | 7.2M | 19.9 |
| YOLOv6n | 640×640 | 4.7M | 50.2 |
| YOLOv8n | 640×640 | 3.16M | 35.7 |
| YOLOv8s | 640×640 | 11.2M | 15.4 |
| YOLOv10n | 640×640 | 2.3M | 12.5 |

**Key insight**: MobileNetV2 classification at 224×224 runs at ~200 FPS.
A MobileNetV3-Small backbone at 320×320 with a lightweight FPN neck should
be achievable at 40-80 FPS on the RK3566 NPU.

---

## 4. Comparative Analysis Matrix

| Criterion | A: YOLO-1cls | B: CenterNet-MBv3 | C: OCDet | D: OLN | E: SAM | F: YOLO-World |
|-----------|-------------|-------------------|----------|--------|--------|---------------|
| **RKNN proven** | ✓✓✓ | ✓✓ | ✓ | ✗ | ✗ | ✗ |
| **Code changes** | Minimal | Moderate | Moderate | Major | Major | Major |
| **Outputs boxes** | ✓ | ✓ | ✗ (centers) | ✓ | masks | ✓ |
| **Params** | 3.16M | 2.5-5.5M | 1.51-1.59M | 26M | 13-19M | 11M+ |
| **Est. RK3566 FPS** | ~36 | ~50-80 | ~15-30 | <5 | <10 | <5 |
| **Novel obj recall** | Medium | Medium | Medium-High | High | Universal | Universal |
| **Training effort** | Low | Medium | Medium | N/A | N/A | N/A |
| **Post-processing** | NMS | Peak detect | Peak detect | NMS | Prompt | NMS |
| **Risk** | Very low | Low | Medium | N/A | N/A | N/A |

---

## 5. Detailed Feasibility for Top 3 Candidates

### 5.1 Option A: YOLO Single-Class (Risk: Very Low)

**Implementation plan**:
1. Download Objects365 dataset (~50 GB)
2. Convert annotations: merge all 365 categories → class 0 ("object")
3. Train YOLOv8n with Ultralytics: `yolo train model=yolov8n.pt data=obj365_1cls.yaml`
4. Export: `yolo export model=best.pt format=onnx imgsz=640`
5. Convert with existing `convert_to_rknn.py` (same pipeline as current model)
6. Hot-swap model file on device — **zero C++ code changes**

**What changes in the perception pipeline**:
- `detector.cpp`: no change (already decodes boxes, NMS works identically)
- `multi_object_associator.cpp`: `coarse_class` becomes always "object" —
  class-filtered matching in `object_memory.cpp` becomes class-agnostic
  (match against all objects, not just same-class). ~5 lines of change.
- `perception_engine.cpp`: minor — VLM enrichment becomes the primary
  source of class information instead of YOLO class labels

**Trade-off**: Easy to implement but doesn't solve the fundamental problem —
the model still can't detect objects that look unlike anything in Objects365.
However, Objects365 covers 365 categories (4.5× COCO), and training on it
produces features that generalize significantly better.

**Estimated improvement**: From 80 detectable categories to effectively ~500+
(365 explicit + generalization to similar objects).

### 5.2 Option B: CenterNet-MobileNetV3 (Risk: Low)

**Implementation plan**:
1. Adapt existing `rknn/embedding/model.py` backbone (MobileNetV3-Small)
   with a CenterNet-style head: 1-ch heatmap + 2-ch WH regression
2. Train on Objects365, single class, using CornerNet-style heatmap loss
3. Export to ONNX, convert to RKNN with existing toolchain
4. New C++ inference module (`ai/proposer.h`) — lighter than detector.cpp
   since output is just heatmap + WH (no NMS, no DFL decode, no class softmax)

**What changes in the perception pipeline**:
- New `Proposer` class (~150 LOC) replaces `Detector` for object proposals
- `multi_object_associator.cpp`: receives boxes from Proposer instead of
  Detector. Functionally identical.
- `perception_engine.cpp`: call `Proposer::infer()` instead of YOLO on
  "YOLO frames". The rest of the cascade is unchanged.
- Optionally: run YOLO on a slow schedule (every 5th frame) for coarse
  class labels, while Proposer runs every frame for proposals.
  This is a **hybrid approach**.

**Performance estimate**:
- MobileNetV3-Small backbone at 320×320: ~60-80 FPS on NPU
- FPN neck adds ~20% overhead: ~50-65 FPS
- Heatmap + WH heads add ~5% overhead: ~48-62 FPS
- Post-processing (peak detection) is CPU-side, trivial
- **Leaves ample NPU headroom for embedder interleaving**

### 5.3 Option C: OCDet-N (Risk: Medium)

**Implementation plan**:
1. Clone OCDet repository, verify MobileNetV4 backbone
2. Check MobileNetV4 RKNN op compatibility (critical gate — may fail here)
3. Retrain on Objects365 with single heatmap channel
4. **Add WH regression heads** to get bounding boxes (original only outputs centers)
5. Export to ONNX, attempt RKNN conversion
6. New C++ inference module for heatmap → box extraction

**What changes in the perception pipeline**:
Same as Option B, but with higher risk due to:
- MobileNetV4 ops potentially unsupported by RKNN
- Need to modify OCDet architecture (add WH heads) — departing from the paper
- Training framework is custom (not Ultralytics), steeper learning curve

**Why still interesting**: OCDet's recall numbers are dramatically better than
YOLO's — 46.6% recall vs 19.8% for nano variants. This means it catches 2.4x
more objects. If the RKNN compatibility hurdle can be cleared, it's the best
pure-detection option.

---

## 6. Hybrid Architecture: The Practical Sweet Spot

Instead of fully replacing YOLO, a **hybrid approach** maximizes capability
while minimizing risk:

```
Frame N+0:  Proposer (CenterNet-MBv3, ~8ms) → proposals → association → crops
Frame N+1:  Embedder (MBv3-Small, ~15ms) → process highest-priority crop
Frame N+2:  Proposer (CenterNet-MBv3, ~8ms) → proposals → association → crops
Frame N+3:  Embedder (MBv3-Small, ~15ms) → process next crop
  ...
Frame N+K:  YOLO (YOLOv8n, ~28ms) → coarse class labels for newly enrolled objects
```

**How it works**:
1. **Proposer runs every frame**: Fast class-agnostic MobileNetV3-CenterNet
   proposes all object-like regions. This is the primary detection path.
2. **Embedder interleaves**: On alternate frames, the NPU runs the embedder
   for re-identification (already implemented).
3. **YOLO runs infrequently**: Every K-th frame (e.g., every 10th), YOLO runs
   to provide coarse class labels ("person", "chair", etc.) for objects that
   the Proposer detected but the VLM hasn't enriched yet. This gives fast
   approximate labels while VLM enrichment is pending.

**NPU scheduling**:
- Proposer: ~8ms per frame at 320×320
- Embedder: ~15ms per crop
- YOLO: ~28ms, but only every 10th frame → amortized ~2.8ms/frame
- Total average NPU time per frame: ~13ms (mix of proposer + embedder frames)
- Target throughput: ~20+ FPS for proposals, which is sufficient for association

**Advantages of hybrid**:
- Proposer catches ALL objects (not just COCO-80)
- YOLO provides familiar class labels for common objects (user-friendly)
- VLM handles fine-grained and novel identification
- Graceful degradation: if Proposer struggles, YOLO still catches common objects
- YOLO model stays as-is (no retraining)

---

## 7. Training Data: Objects365 vs LVIS vs COCO

| Dataset | Categories | Images | Boxes | Best for |
|---------|-----------|--------|-------|----------|
| COCO | 80 | 118K | 860K | Baseline, but too narrow |
| Objects365 | 365 | 2M | 30M | **Best balance**: broad coverage, huge scale |
| LVIS | 1203 | 120K | 2M | Most categories, but fewer images per category |
| OpenImages | 600 | 1.7M | 15M | Large scale, but annotation quality varies |

**Recommendation**: Train on **Objects365** (single class) for the proposer/YOLO-1cls.
It has the best combination of category diversity and training data volume.
LVIS has more categories but far fewer images per category, which hurts single-class
objectness learning. Objects365's 30M boxes provide extremely robust objectness signal.

---

## 8. Quantitative Impact Estimate

### Current system (YOLO-80):
- Detectable object types: ~80 (COCO categories)
- Novel object recall: 0% (invisible to the system)
- Time to identify new object: Instant (if in COCO-80), never (if not)

### Option A (YOLO-1cls on Objects365):
- Detectable object types: ~500+ (365 explicit + generalization)
- Novel object recall: ~30-40% (significantly better than COCO-80)
- Time to identify new object: 1-2 seconds (embed → match → VLM enrich)

### Option B (CenterNet-MBv3 on Objects365):
- Detectable object types: ~500+ (same training data as Option A)
- Novel object recall: ~35-45% (heatmap-based detection has higher recall)
- Time to identify new object: 1-2 seconds
- FPS advantage: ~50+ vs ~36 for YOLO

### Hybrid (Proposer + YOLO + Embedder):
- Detectable object types: ~500+ from proposer, with COCO-80 fast labels
- Novel object recall: ~35-45%
- Time to identify new object: 1-2s for embedding, 5-30s for VLM label
- Best overall user experience (fast labels + broad coverage)

---

## 9. Recommendation

### Phase 1 (Low risk, immediate): Option A — YOLO Single-Class on Objects365

**Rationale**: This is a model-swap-only change. No C++ code modifications beyond
a ~5-line tweak to disable class-filtered matching in ObjectMemory. Uses the exact
same ONNX→RKNN→deploy pipeline we already have. We can validate the improvement
within a single training session.

**Estimated effort**: 1-2 days (download data, train, convert, deploy)

### Phase 2 (Medium risk, when ready): Option B — CenterNet-MobileNetV3 Proposer

**Rationale**: This gives us the fastest class-agnostic proposal path, freeing NPU
time for more embedding passes. The MobileNetV3-Small backbone is already RKNN-proven
(it's our embedder backbone). CenterNet's heatmap output has inherently higher recall
than regression-based box prediction.

**Estimated effort**: 3-5 days (build training pipeline, train, new C++ proposer class)

### Phase 3 (Optional, research): Hybrid Proposer + YOLO + VLM

**Rationale**: Once the Proposer works, add YOLO back on a slow schedule for coarse
labels. This gives the best user experience — fast labels for common objects,
progressive enrichment for novel ones.

**Estimated effort**: 1-2 days (scheduling logic in `perception_engine.cpp`)

---

## 10. Architecture Impact on Existing Code

### Minimal-change path (Option A only):

| File | Change | LOC |
|------|--------|-----|
| `ai/object_memory.cpp` | Skip class filter in `match()` when class is "object" | ~5 |
| Model file on device | Replace `yolov8n.rknn` with `yolov8n_obj365_1cls.rknn` | 0 (config) |
| `store.json` | Point model path to new file | 0 (runtime) |

### Full proposer path (Option B):

| File | Change | LOC |
|------|--------|-----|
| New: `ai/proposer.h` | ProposerConfig + Proposer class declaration | ~50 |
| New: `ai/proposer.cpp` | RKNN heatmap inference + peak extraction + box assembly | ~200 |
| `ai/perception_engine.h` | Add Proposer* member | ~5 |
| `ai/perception_engine.cpp` | Use Proposer on proposal frames, YOLO on label frames | ~40 |
| `ai/object_memory.cpp` | Class-agnostic matching mode | ~10 |
| `CMakeLists.txt` | Add proposer.cpp | 1 |
| New: `rknn/proposer/` | Training scripts (similar structure to embedding/) | ~500 |

---

## 11. Open Questions

1. ~~**Objects365 download**: Dataset is ~50 GB. Do we have sufficient disk space
   on the WSL2 machine for training?~~
   **Resolved**: WSL2 disk has >450 GB available. COCO used for first iteration;
   Objects365 is feasible for follow-up training.
2. **MobileNetV4 on RKNN**: Has anyone successfully converted MobileNetV4 to
   RKNN? This determines OCDet's viability. (Could test with a simple
   classification model first.)
3. **False positive rate**: Single-class detection at high recall will inevitably
   increase false positives (background textures misidentified as objects).
   The embedder + VLM pipeline can filter these, but at what cost?
4. **NPU time budget**: Current YOLO takes ~28ms per frame. If we run the proposer
   at ~8ms + embedder at ~15ms, we're at ~23ms average — faster than current.
   But we lose per-frame YOLO class labels. Is this trade-off acceptable?
5. **RKNN 2.3.2 auto_hybrid bug**: The `_set_fp16_hybrid` function crashes with
   a `KeyError` for any ONNX graph containing Sigmoid output nodes. This forces
   `auto_hybrid=False`, causing ~1-2 detection loss vs FP32 on borderline objects.
   Need to check if newer rknn-toolkit2 versions fix this.
6. **Score mode mismatch in stock model**: The Rockchip-provided `yolov8n.rknn`
   exports sigmoid probabilities, but `detector.cpp` treats 9-output models as
   logit outputs. This causes an effective confidence threshold of ~0.0, passing
   nearly all proposals through. Should `detector.cpp` auto-detect score mode,
   or should the stock model be re-exported with logit scores?

---

## 12. Phase 1 Results: Option A Implementation (2026-03-27)

### 12.1 Training

Trained YOLOv8n single-class on **COCO** (118K images, 860K boxes, all 80 classes
merged to class 0 "object") as a quick-turnaround first pass before the full
Objects365 run. Hardware: NVIDIA GeForce GTX 1660 SUPER (6 GB VRAM).

| Setting | Value |
|---------|-------|
| Base model | `yolov8n.pt` (pretrained COCO) |
| Dataset | COCO train2017 / val2017, all classes → "object" |
| Epochs | 50 |
| Image size | 640×640 |
| Batch size | 16 |
| Training time | ~34.6 hours |

**Final metrics (epoch 50)**:

| Metric | Epoch 1 | Epoch 50 |
|--------|---------|----------|
| mAP@50 | 0.523 | **0.607** |
| mAP@50-95 | 0.333 | **0.411** |
| Precision | 0.625 | **0.705** |
| Recall | 0.466 | **0.518** |
| box_loss | 1.173 | 1.112 |
| cls_loss | 1.348 | 1.035 |

Model file: `runs/detect/runs/proposer/yolov8n_1cls/weights/best.pt` (6.0 MB)

### 12.2 ONNX Export: 9-Output Format Required

The standard Ultralytics `yolo export format=onnx` produces a **single** combined
output tensor `(1, 5, 8400)`. However, `detector.cpp` expects **9 separate outputs**
(3 scales × 3 per scale: box, score, score_sum) — the same format as the existing
hand model and the Rockchip-provided YOLOv8n.

**Fix**: Used the existing `rknn/export_hand_yolov8_onnx.py` script, which
monkey-patches the Detect head to expose per-branch outputs:

```
box_i   : [1, 64, H, W]     (DFL box regression)
score_i : [1, 1,  H, W]     (class logit or sigmoid, 1 class)
score_sum_i : [1, 1, H, W]  (sigmoid sum, used as pre-filter)
```

where `i ∈ {0, 1, 2}` for the three detection scales (80×80, 40×40, 20×20).

### 12.3 RKNN Conversion: auto_hybrid Bug and Score Mode

**Bug encountered**: RKNN toolkit 2.3.2 crashes with `KeyError: 'score_0'` in
`quantizer.py:_set_fp16_hybrid` during INT8 quantization. This is triggered by
any Sigmoid node in the ONNX graph (not specific to output naming). The bug
affects sigmoid-mode exports; logit-mode exports avoid it because no Sigmoid
nodes appear at the graph output.

**Workaround**: Export with `--score-mode logit` (raw class logits in score
tensors, sigmoid only in score_sum), which avoids the Sigmoid output nodes.
This is the correct mode for `detector.cpp`, which sets `score_is_logit = true`
for 9-output models and handles the logit→probability conversion internally.

**Quantization variants tested**:

| Variant | Algorithm | Calibration | Hybrid | Size | On-device FPS | Detections |
|---------|-----------|-------------|--------|------|---------------|------------|
| v1 (initial) | normal | 200 images | auto_hybrid=False | 4.4 MB | **20.4** | 3-5 |
| v3_kl | kl_divergence | 200 images | auto_hybrid=False | 4.4 MB | 18.1 | 1-4 |
| v3_normal500 | normal | 500 images | auto_hybrid=False | 4.4 MB | ~19 | similar |
| v3_hybrid1 | normal | 200 images | hybrid_level=1 | 5.6 MB | **7.8** | 1 |
| v3_mmse | mmse | 500 images | auto_hybrid=False | — | OOM killed | — |

**Best variant**: v1 (normal algorithm, 200 calibration images, auto_hybrid=False).
The `hybrid_level=1` model is too slow (FP16 layers fall back to CPU on RK3566).
KL divergence and additional calibration images did not improve detection count.

### 12.4 On-Device Deployment

| Metric | Old model (80-class) | New model (1-class) |
|--------|---------------------|---------------------|
| Model file | `yolov8n.rknn` (Rockchip) | `yolov8n_1cls_rk3566_i8.rknn` |
| RKNN outputs | 9 | 9 |
| Classes | 80 | 1 ("object") |
| Model size | 6.1 MB | 4.4 MB |
| AI pipeline FPS | ~13.6 | **~20.4** |
| `ai_labels` DP | `""` (default COCO) | `"object"` |

**C++ changes deployed**:
- `object_memory.cpp`: class filter bypass when `coarse_class == "object"`
  (1 line, as planned in Section 10)
- `store.json`: `ai_model_path` and `ai_labels` updated

### 12.5 Detection Count Investigation

Initial comparison showed the old model reporting ~35 detections vs ~3-5 for
the new model on what appeared to be the same scene. Investigation revealed
this was **not a regression** but a combination of two factors:

**1. The old RKNN model has a score mode mismatch.**

`detector.cpp` assumes 9-output models export raw logits in the score tensor
(line 582: `score_is_logit = (per_branch == 3)`). It converts the confidence
threshold to logit space: `conf_to_logit(0.25)` ≈ -1.1. However, the
Rockchip-provided `yolov8n.rknn` exports sigmoid **probabilities** [0, 1].
When INT8-quantized probabilities (always ≥ 0) are compared against a logit
threshold of -1.1, **everything passes** — the threshold is effectively ~0.0.
This explains the inflated detection count from the old model.

**2. Same-frame FP32 comparison shows parity.**

Running both models (FP32, unquantized) on the same camera frame:

| Model | Detections (conf ≥ 0.25) | Objects |
|-------|-------------------------|---------|
| 80-class YOLOv8n (stock) | 4 | couch (0.88), vase (0.71), toilet (0.27), chair (0.27) |
| 1-class YOLOv8n (ours) | **5** | object (0.90), object (0.59), object (0.56), object (0.44), object (0.36) |

The 1-class model detects **more** objects than the 80-class model on the same
frame. The INT8-quantized on-device model (3-5 detections) loses 1-2 borderline
objects due to quantization, which is expected for `auto_hybrid=False`.

### 12.6 Known Limitations and Next Steps

**Quantization accuracy gap**: The `auto_hybrid=False` conversion loses ~1-2
borderline detections vs FP32. The RKNN 2.3.2 `_set_fp16_hybrid` bug prevents
using the normal auto-hybrid path. Potential mitigations:
- Upgrade to a newer rknn-toolkit2 where the bug may be fixed
- Train on Objects365 (30M boxes, 365 categories) for stronger objectness signal
  that survives quantization better
- Use the airockchip/ultralytics fork for ONNX export (may produce a graph
  structure that avoids the RKNN bug)

**Score mode mismatch in old model**: The Rockchip-provided `yolov8n.rknn`
exports sigmoid probabilities but `detector.cpp` interprets them as logits,
resulting in near-zero effective threshold. This should be investigated — the old
model may have been providing many false-positive detections to the perception
pipeline.

**Objects365 training**: The current model was trained on COCO (80 classes, 860K
boxes). Retraining on Objects365 (365 classes, 30M boxes) should significantly
improve novel-object recall, as planned in the original recommendation.

---

## 13. References

- [OCDet paper](https://arxiv.org/abs/2411.15653) — Chen et al., Nov 2024
- [OLN paper](https://arxiv.org/abs/2108.06753) — Kim et al., Aug 2021
- [Objects365 dataset](https://www.objects365.org/) — Shao et al., ICCV 2019
- [CenterNet](https://github.com/xingyizhou/CenterNet) — Zhou et al., 2019
- [MobileNetV4](https://arxiv.org/abs/2404.10518) — Qin et al., ECCV 2024
- [YOLO-World](https://arxiv.org/abs/2401.17270) — Cheng et al., CVPR 2024
- [R-FCN-3000](https://arxiv.org/abs/1712.01802) — Singh et al., 2017
- [RK3566 NPU benchmarks](https://www.scensmart.com/news/comparison-of-ai-model-performance-of-rockchip-mainstream-socs-such-as-rk3588-rk3576-rk3568-rv1126-etc/)
- [PF-RPN](https://arxiv.org/abs/2603.17554) — Prompt-Free Universal RPN, Mar 2026
