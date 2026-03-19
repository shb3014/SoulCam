# Tracking Implementation

Technical reference for the SoulCam interframe tracking and unified target policy
system running on RK3566 (Cortex-A55 + Mali-G52 + RKNN NPU).

---

## 1. Architecture Overview

```
Camera (selfpath, 640x480 NV12 @30fps)
  │
  ▼
GStreamer pipeline (rgaconvert → RGB 640x640)
  │
  ▼
┌──────────────────────────────────────────────┐
│  AI Capture Pipeline  (ai_capture.cpp)       │
│                                              │
│  Frame N (YOLO):                             │
│    ModelPipeline → weighted scheduler        │
│      → slot 0: person model (yolov8n)        │
│      → slot 1: hand model (hand_yolov8n)     │
│    pick_target_detection() → reinit tracker  │
│    callback(all raw detections)              │
│                                              │
│  Frame N+1..N+3 (Tracker):                   │
│    RGB→Gray (NEON) → KCF correlate           │
│    → Kalman predict+update → smooth_output   │
│    callback({tracked detection})             │
└──────────────────────────────────────────────┘
  │
  ▼
on_detections (main.cpp)
  → HandTargetTracker (target continuity)
  → ONVIF metadata stream
  → Rive animation renderer
  → SoulLink AI detection packets
```

### Key files

| File | Role |
|------|------|
| `src/ai/interframe_tracker.h/.cpp` | KCF + Kalman tracker, FFT, HOG |
| `src/pipeline/ai_capture.h/.cpp` | AI pipeline, target policy, YOLO scheduling |
| `src/ai/model_pipeline.h/.cpp` | Multi-model weighted scheduler |
| `src/ai/detector.h/.cpp` | RKNN inference wrapper, NMS |
| `src/ai/hand_target_tracker.h/.cpp` | Detection-level target continuity |
| `src/main.cpp` | DP wiring, on_detections callback |

---

## 2. Interframe Tracker

Lightweight CPU-only tracker that runs between YOLO frames.
Goal: maintain smooth bounding-box output at full camera FPS (~14-43 fps)
while YOLO only runs every Nth frame (~3-10 fps on NPU).

### 2.1 Components

| Component | Purpose | Cost (A55) |
|-----------|---------|------------|
| **KCF** (Kernelized Correlation Filter) | Visual tracking via HOG features + Gaussian kernel correlation in frequency domain | ~3-5 ms |
| **Kalman filter** | Constant-velocity motion model (4-state: cx, cy, vx, vy) | <0.01 ms |
| **Output EMA** | Exponential moving average on output bbox for jitter suppression | <0.01 ms |
| **FFT** | Radix-2 Cooley-Tukey, used by KCF for frequency-domain operations | (included in KCF) |
| **HOG** | 4×4 cells, 9 orientation bins, L2-normalized per cell | (included in KCF) |
| **RGB→Gray** | NEON-optimized grayscale conversion (BT.601 coefficients) | ~0.3 ms |

### 2.2 Frame-by-frame flow

**YOLO frame** (`run_yolo == true`):
1. `model_pipeline_infer()` runs the scheduled model on NPU
2. `pick_target_detection()` selects best detection + updates debounce
3. `tracker.reinit(det, gray)`:
   - EMA-blend new YOLO position with previous tracker position (`smooth_factor`)
   - If already tracking: `kalman.predict() + kalman.update()` (preserves velocity)
   - If first frame: `kalman.init()` (cold start)
   - Extract grayscale patch → KCF `train()` (learn appearance template)

**Tracker frame** (`run_yolo == false`):
1. RGB→Gray conversion (NEON)
2. `tracker.update(gray)`:
   - `kalman.predict()` → predicted center
   - Extract patch at predicted center → KCF `correlate()` → displacement + PSR
   - If PSR ≥ threshold: `kalman.update(measured_pos)` → corrected position
   - If PSR < threshold: use Kalman prediction only (degraded mode)
3. `smooth_output()` → velocity-adaptive EMA low-pass on output coordinates

### 2.3 Critical design decisions

**Kalman not reset on YOLO re-anchor**: Previously, every YOLO frame called
`kalman.init()`, destroying the learned velocity. Now uses `kalman.update()`
when already tracking, preserving motion continuity and eliminating
re-anchor jumps.

**Output EMA**: A low-pass filter (`smooth_factor`, default 0.6) is applied to
the final output coordinates on every frame. This suppresses both YOLO
detection noise and KCF tracking jitter. The same factor controls:
- YOLO re-anchor blending (how fast to adopt new YOLO position)
- Output smoothing (how fast the output bbox follows the internal state)

**KCF re-trained on every YOLO frame**: The KCF template is retrained from
scratch on each YOLO frame to prevent template drift. The online update
during tracker frames uses exponential blending (`visual_learning_rate`).

### 2.4 Configuration (InterframeTrackerConfig)

| Parameter | Default | Effect |
|-----------|---------|--------|
| `yolo_interval` | 4 | YOLO every N frames. 1 = tracker disabled. |
| `enable_visual` | true | KCF on/off. false = Kalman-only (cheaper, less accurate). |
| `visual_psr_threshold` | 7.0 | Below this PSR, KCF result is discarded. |
| `visual_learning_rate` | 0.125 | KCF online adaptation rate. |
| `visual_patch_size` | 64 | HOG/FFT patch size (must be power of 2). |
| `roi_padding` | 2.0 | Search region = bbox × this factor. |
| `kalman_process_noise` | 4.0 | Higher = more responsive to acceleration. |
| `kalman_measure_noise` | 1.0 | Higher = smoother but laggier. |
| `smooth_factor` | 0.6 | Output EMA alpha. 0=max smooth, 1=snap. |

---

## 3. Adaptive YOLO Scheduler

When `tracker_adaptive_interval` is enabled, the system dynamically decides
when to run YOLO based on tracking confidence rather than a fixed interval.

### 3.1 Decision logic (`should_run_yolo`)

```
if force_yolo_next → always YOLO (mode switch, tracker reset)
if tracker not tracking → always YOLO (no active target, avoids dead frames)

Non-adaptive mode (default):
  YOLO every yolo_interval frames (fixed cadence)

Adaptive mode:
  if frames_since_yolo >= max_skip → always YOLO
  if frames_since_yolo <  min_skip → never YOLO
  otherwise → compute urgency score:
    PSR < 7    → urgency += 3  (tracker losing confidence)
    PSR < 10   → urgency += 1
    PSR > 15   → urgency -= 1  (tracker very confident)
    vel > 30   → urgency += 2  (fast motion)
    vel > 15   → urgency += 1
    vel < 5    → urgency -= 1  (static target)
    conf < 0.4 → urgency += 1.5  (last YOLO was uncertain)
    conf > 0.8 → urgency -= 0.5
  run YOLO if urgency >= 2.0
```

**Important**: The `force_yolo_next` and not-tracking checks are evaluated
before the adaptive/non-adaptive branching so they work regardless of mode.
Previously `force_yolo_next` was only checked inside the adaptive branch,
causing mode-switch forced YOLO frames to be silently dropped in non-adaptive
mode (the default). This was the root cause of person fallback not triggering
promptly.

### 3.2 Benefits

- Static scene with confident tracking: YOLO runs at `min_skip` rate → saves NPU
- Fast motion or losing track: YOLO runs more frequently → maintains accuracy
- Bounded by `[min_skip, max_skip]` to prevent extremes

---

## 4. Unified Target Policy

Hand-preferred with person-fallback detection policy, integrated into
`ai_capture.cpp`. Controls which detection the tracker follows and which
model gets NPU priority.

### 4.1 Modes

| Mode | Behavior | Model scheduling |
|------|----------|-----------------|
| **PersonFallback** | Both models enabled with equal weight (1:1). Track person if available. Accumulate hand confirmations. | Slot 0 + Slot 1 alternate (50/50) |
| **HandPreferred** | Only hand model enabled (person disabled). Track hand exclusively. Count consecutive hand-lost frames. | Slot 1 only (weight=10) |

### 4.2 State machine

```
                   hand confirmed ≥ N
  PersonFallback ─────────────────────→ HandPreferred
       ↑                                      │
       │         hand lost ≥ M                │
       └──────────────────────────────────────┘
```

**PersonFallback → HandPreferred** (accumulating, non-consecutive):
- Only `hand_confirm_count++` when hand is actually detected
- When hand model didn't run (person-model frame), counters unchanged
- Threshold: `tracker_hand_confirm` (default 3)

**HandPreferred → PersonFallback** (consecutive):
- `hand_lost_count++` every YOLO frame without hand detection
- `hand_confirm_count` reset to 0 on any miss
- Threshold: `tracker_hand_lost` (default 5, recommended 15)

### 4.3 Mode transition actions

On every mode switch:
1. Reset debounce counters to 0
2. Reset interframe tracker (`tracker.reset()`)
3. Force next frame to run YOLO (`force_yolo_next = true`)
4. Adjust model weights via `model_pipeline_set_slot_weight` / `enable_model`

### 4.4 Why asymmetric debounce

In PersonFallback, the hand model only runs ~50% of YOLO frames (weighted
scheduling). If `hand_confirm_count` were reset on every non-hand frame, it
could never accumulate to the threshold. The fix: only reset/decrement when
we have positive evidence (HandPreferred mode, where hand model runs every
frame). In PersonFallback, no-hand frames are ambiguous (model may not have
run) and don't affect the counter.

---

## 5. Model Pipeline (Weighted Scheduler)

Multi-model inference with weighted round-robin scheduling.

### 5.1 Scheduling

- Models are assigned integer weights (default 1).
- A schedule array is built: `[0, 0, ..., 1, 1, ...]` with each index
  repeated by its weight.
- A cursor advances through the schedule, one model per YOLO frame
  (when `max_models_per_frame = 1`).
- Detections from each model are tagged with `model_id = slot_index`.

### 5.2 Weight dynamics

| State | Slot 0 (person) | Slot 1 (hand) |
|-------|-----------------|---------------|
| PersonFallback (initial) | weight=1, enabled | weight=1, enabled |
| HandPreferred | disabled | weight=10, enabled |
| PersonFallback (after fallback) | weight=1, enabled | weight=1, enabled |

---

## 6. Hand Detection Model

### 6.1 Current model

- **File**: `hand_yolov8n_rk3566_i8_20260301.rknn`
- **Architecture**: YOLOv8n, INT8 quantized for RKNN
- **Input**: 640×640 RGB
- **Classes**: 1 (hand)
- **Inference**: ~90-130 ms on RK3566 NPU

### 6.2 Accuracy tuning

| Parameter | DP | Default | Notes |
|-----------|----|---------|-------|
| Confidence threshold | `model2_conf` (DP 12) | 0.15 | Lower catches more hands but may increase false positives. Try 0.10–0.20. |
| NMS threshold | (inherited from primary) | 0.45 | Single-class model, NMS less critical. |
| Labels | `model2_labels` (DP 108) | `"hand"` | Must be set for correct label assignment. |

### 6.3 Improving accuracy without retraining

1. **Lower `model2_conf`** to 0.10–0.15 (via SoulFlow DP or store.json)
2. **Ensure proper lighting**: The model was trained on indoor hand images
3. **Hot-swap a better model**: Use `sysCmd` subcmd 7 to replace at runtime
4. **Input resolution**: Model auto-detects its expected input size from
   RKNN metadata; the pipeline resizes via RGA hardware

---

## 7. Performance Characteristics

### 7.1 Measured on RK3566 (1.8 GHz A55, 0.8 TOPS NPU)

| Configuration | Total FPS | YOLO FPS | Tracker FPS | Notes |
|---------------|-----------|----------|-------------|-------|
| YOLO only (no tracker) | ~7-10 | 7-10 | 0 | Limited by NPU inference time |
| Tracker, `yolo_interval=4` | ~14.6 | 3.5–3.7 | 10.8–11.0 | ~60-75% NPU savings |
| HandPreferred mode | ~14.6 | 3.5 | 11.0 | Hand model faster than person model |
| Adaptive interval (static scene) | up to 20+ | ~2 | ~18 | YOLO rate drops for static targets |
| `ai_target_fps=10` | 10 | ~2.5 | ~7.5 | FPS-capped, saves CPU power |
| `ai_target_fps=5` | 5 | ~1.25 | ~3.75 | Minimal CPU load for low-priority use |

### 7.2 CPU cost breakdown (per tracker frame)

| Operation | Time |
|-----------|------|
| RGB→Gray (640×640, NEON) | ~0.3 ms |
| Patch extraction (bilinear, 64×64) | ~0.1 ms |
| HOG features (16×16×9) | ~0.5 ms |
| FFT 2D (16×16, per channel) | ~1.5 ms |
| Gaussian kernel correlation | ~1.0 ms |
| Kalman predict+update | <0.01 ms |
| Output EMA | <0.01 ms |
| **Total** | **~3-5 ms** |

---

## 8. SoulLink DP Reference

All tracker and target policy parameters are exposed as SoulLink DPs,
modifiable at runtime via `setDp` (MQTT or SoulFlow debug panel).
Changes take effect immediately without restart.

| DP | Name | Type | Default | Description |
|---:|------|------|---------|-------------|
| 18 | `tracker_yolo_interval` | u32 | 4 | YOLO every N frames. 1 = disable tracker. |
| 19 | `tracker_enable_mosse` | bool | true | Enable KCF visual tracking. |
| 20 | `tracker_mosse_psr` | float | 7.0 | KCF PSR threshold. |
| 21 | `tracker_mosse_learn_rate` | float | 0.125 | KCF adaptation rate. |
| 22 | `tracker_mosse_patch_size` | u32 | 64 | KCF ROI size. |
| 23 | `tracker_roi_padding` | float | 2.0 | Search region multiplier. |
| 24 | `tracker_smooth_factor` | float | 0.6 | Output EMA alpha (0=smooth, 1=snap). |
| 25 | `tracker_adaptive_interval` | bool | false | Dynamic YOLO scheduling. |
| 26 | `tracker_max_skip` | u32 | 8 | Max tracker-only frames. |
| 27 | `tracker_min_skip` | u32 | 2 | Min frames between YOLO. |
| 28 | `tracker_hand_confirm` | u32 | 3 | Hand confirmations to switch mode. |
| 29 | `tracker_hand_lost` | u32 | 5 | Hand misses to fall back. Recommend 15. |
| 12 | `model2_conf` | float | 0.15 | Hand model confidence threshold. |
| 13 | `adaptive_tracking` | bool | false | Enable unified target policy. |
| 30 | `ai_target_fps` | u32 | 0 | AI pipeline target FPS (0 = unlimited). |

### Recommended configuration for hand tracking

```
tracker_yolo_interval = 4
tracker_enable_mosse = true
tracker_smooth_factor = 0.5
tracker_adaptive_interval = false
tracker_hand_confirm = 3
tracker_hand_lost = 15
model2_conf = 0.15
adaptive_tracking = true
max_models_per_frame = 1
```

---

## 9. Troubleshooting

### Hand not being tracked

1. Check `model2_labels` is set to `"hand"` in store.json
2. Check `adaptive_tracking = true`
3. Check `model2_path` points to a valid hand model
4. Verify in logs: `TargetPick: hand=N` should show hand detections when
   hand is in view. If `hand=0` consistently, lower `model2_conf`.
5. Verify model weights are equal in PersonFallback mode:
   `ModelPipeline: slot 0 [...] weight=1` and `slot 1 [...] weight=1`

### Bounding box jitter/shaking

1. Lower `tracker_smooth_factor` (e.g., 0.4) for more smoothing.
2. Increase `kalman_measure_noise` for smoother Kalman output
3. If using adaptive interval, ensure `tracker_max_skip` isn't too high
   (8 is good; higher allows more drift before correction)
4. Check PSR values in logs — consistently low PSR (<7) means KCF is
   struggling (poor visual texture, target too small, or wrong patch_size)
5. Lower `tracker_smooth_factor` further (e.g., 0.3) for very heavy smoothing
   at the cost of responsiveness. The output convergence time in frames is
   approximately `3 / smooth_factor` (e.g., 5 frames at 0.6, 10 frames at 0.3).

### Mode oscillation (rapid hand↔person switching)

1. Increase `tracker_hand_lost` to 15–20 (higher tolerance for occasional misses)
2. Increase `tracker_hand_confirm` to 5 if switching to hand too eagerly
3. Oscillation is normal at the edge of hand visibility; the debounce
   parameters control the tradeoff between responsiveness and stability

### Thread safety: setting tracker_yolo_interval at runtime

The DP change listener runs on the MQTT thread while `on_new_sample` runs
on the GStreamer appsink thread. To avoid use-after-free, the tracker object
is **never deleted** at runtime — only reset. Setting `yolo_interval=1`
resets the tracker and sets the interval; the appsink thread sees
`yolo_interval <= 1` and stops using the tracker. The object stays idle
in memory (a few KB) until re-enabled.

### Target policy works without tracker

The `pick_target_detection()` state machine (mode switching, model weight
scheduling) runs on every YOLO frame regardless of whether the interframe
tracker is active. This ensures hand/person mode switching and model
scheduling work correctly even with `tracker_yolo_interval=1` (every
frame is YOLO, no tracker).

### Logs to check

```bash
# Live logs
sudo journalctl -u soulcam -f | grep -E "(TargetPick|Target policy|AI pipeline)"

# Startup verification
sudo journalctl -u soulcam --since "30s ago" | grep -E "(slot|weight|scheduler|tracker|Target)"
```
