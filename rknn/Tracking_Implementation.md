# Tracking Implementation

Technical reference for the SoulCam interframe tracking and unified target policy
system running on RK3566 (Cortex-A55 + Mali-G52 + RKNN NPU).

---

## 1. Architecture Overview

```
Camera (selfpath, 640x480 NV12 @30fps requested, actual varies with AE)
  │
  ▼
GStreamer pipeline (rgaconvert → RGB 640x480)
  │
  ▼
┌──────────────────────────────────────────────┐
│  AI Capture Pipeline  (ai_capture.cpp)       │
│                                              │
│  FPS throttle (credit-based, optional)       │
│  Compute frame dt (for time-aware Kalman)    │
│                                              │
│  Frame N (YOLO):                             │
│    Letterbox 640x480 → 640x640 (gray=128)   │
│    ModelPipeline → weighted scheduler        │
│      → slot 0: person model (yolov8n)        │
│      → slot 1: hand model (hand_yolov8n)     │
│    Un-letterbox detections → 640x480 coords  │
│    pick_target_detection() → reinit(det, dt) │
│    callback(all raw detections, 640x480)     │
│                                              │
│  Frame N+1..N+3 (Tracker):                   │
│    RGB→Gray (NEON, 640x480)                  │
│    → KCF correlate (PSR-blended with Kalman) │
│    → Kalman predict(dt)+update               │
│    → smooth_output (velocity-adaptive EMA)   │
│    callback({tracked detection}, 640x480)    │
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
| `src/pipeline/ai_capture.h/.cpp` | AI pipeline, target policy, YOLO scheduling, FPS throttling |
| `src/ai/model_pipeline.h/.cpp` | Multi-model weighted scheduler |
| `src/ai/detector.h/.cpp` | RKNN inference wrapper, NMS |
| `src/ai/hand_target_tracker.h/.cpp` | Detection-level target continuity |
| `src/main.cpp` | DP wiring, on_detections callback |
| `scripts/build.sh` | On-device build (CMake + make) |
| `scripts/deploy.sh` | Host → device rsync + build |
| `service/soulcam.service` | systemd unit file |

### Related documentation

| Document | Contents |
|----------|----------|
| `doc/build/BUILD_AND_DEPLOY.md` | Build system, deployment workflow, systemd service, device management |
| `soullink/docs/dp_catalog.md` | Full DP reference (all data points, not just tracker) |
| `doc/cmd/CMD.md` | Consolidated command reference (SSH, RTSP, MQTT, SoulLink CLI) |

---

## 2. Interframe Tracker

Lightweight CPU-only tracker that runs between YOLO frames.
Goal: maintain smooth bounding-box output at full camera FPS (~14-43 fps)
while YOLO only runs every Nth frame (~3-10 fps on NPU).

### 2.1 Components

| Component | Purpose | Cost (A55) |
|-----------|---------|------------|
| **KCF** (Kernelized Correlation Filter) | Visual tracking via HOG features + Gaussian kernel correlation in frequency domain | ~3-5 ms |
| **Kalman filter** | Time-aware constant-velocity motion model (4-state: cx, cy, vx, vy) | <0.01 ms |
| **Output EMA** | Velocity-adaptive exponential moving average with separate size smoothing | <0.01 ms |
| **FFT** | Radix-2 Cooley-Tukey, used by KCF for frequency-domain operations | (included in KCF) |
| **HOG** | 4×4 cells, 9 orientation bins, L2-normalized per cell | (included in KCF) |
| **RGB→Gray** | NEON-optimized grayscale conversion (BT.601 coefficients) | ~0.3 ms |

### 2.2 Frame-by-frame flow

**YOLO frame** (`run_yolo == true`):
1. Letterbox 640×480 → 640×640 (gray=128 padding, 80px top/bottom)
2. `model_pipeline_infer()` runs the scheduled model on NPU
3. Un-letterbox detections (subtract `pad_top`, clamp to camera height)
4. `pick_target_detection()` selects best detection + updates debounce
5. `tracker.reinit(det, gray, dt)`:
   - Velocity-adaptive EMA-blend new YOLO position with previous tracker
     position (uses `smooth_factor` as base, adapts by Kalman velocity)
   - Separate size smoothing: width/height blend at 0.4× the position alpha
   - If already tracking: `kalman.predict(dt) + kalman.update()` (preserves velocity)
   - If first frame: `kalman.init()` (cold start)
   - Extract grayscale patch → KCF `train()` (learn appearance template)

**Tracker frame** (`run_yolo == false`):
1. RGB→Gray conversion (NEON, 640×480)
2. `tracker.update(gray, dt)`:
   - `kalman.predict(dt)` → predicted center (time-aware)
   - Extract patch at predicted center → KCF `correlate()` → displacement + PSR
   - Gradual PSR blending: `psr_weight = clamp((psr - 5) / 10, 0, 1)`
     - PSR ≥ 15: full KCF displacement applied
     - PSR 5–15: linear blend of KCF displacement with Kalman-only
     - PSR ≤ 5: Kalman prediction only
   - `kalman.update(blended_pos)` when `psr_weight > 0`
3. `smooth_output()` → velocity-adaptive EMA with separate size smoothing

### 2.3 Critical design decisions

**Letterbox preprocessing**: The GStreamer pipeline outputs 640×480 RGB
(camera native resolution), not 640×640. A software letterbox pads to
640×640 with gray=128 before RKNN inference. This preserves the original
aspect ratio — the old pipeline stretched 640×480→640×640 via `rgaconvert`,
distorting by 33% vertically. YOLOv8 models are trained with letterbox
padding, so this matches the expected input distribution. After inference,
detections are un-letterboxed back to 640×480 coordinates. The tracker and
all downstream consumers work in 640×480 space.

Cost: ~0.15 ms (memset + memcpy for 1.2 MB). Negligible vs YOLO inference.

**Time-aware Kalman**: All `kalman.predict(dt)` calls receive the actual
frame-to-frame time step, normalized so dt=1.0 at 30fps (33.3 ms). With
auto-exposure, the camera rate varies between ~14–43 fps, meaning actual
frame intervals range from ~23–70 ms. Without time-aware dt, the Kalman
velocity prediction is wrong: at 14 fps a hand moves ~3× further per
frame than at 43 fps. The dt is computed in `on_new_sample()` from
`steady_clock` timestamps and clamped to [0.1, 5.0] to guard against
outliers.

**Kalman not reset on YOLO re-anchor**: Previously, every YOLO frame called
`kalman.init()`, destroying the learned velocity. Now uses `kalman.update()`
when already tracking, preserving motion continuity and eliminating
re-anchor jumps.

**Velocity-adaptive smoothing**: The output EMA alpha adapts based on the
Kalman velocity estimate:
- `vel > 30 px/frame`: alpha × 1.6 (less smoothing, faster response)
- `vel < 5 px/frame`: alpha × 0.5 (more smoothing, jitter suppressed)
- Between 5–30: base alpha unchanged

This eliminates the fixed tradeoff where static scenes were jittery or
fast motion was laggy. The same adaptive logic is applied in both
`smooth_output()` (every frame) and `reinit()` (YOLO re-anchor blending).

**Separate size smoothing**: Width/height uses `size_alpha = alpha × 0.4`,
so bbox dimensions change 2.5× slower than position. This prevents
distracting "breathing" (size oscillation) while position tracking remains
responsive.

**Gradual PSR blending**: Instead of a binary PSR threshold (≥7 → use KCF,
<7 → Kalman only), the KCF displacement is linearly blended:
`psr_weight = clamp((psr - 5) / 10, 0, 1)`. This gives smooth degradation:
at PSR=15+ KCF is fully trusted, at PSR=5 it's fully ignored, with
proportional blending in between. Eliminates the sudden position jump when
PSR crosses a hard threshold.

**KCF re-trained on every YOLO frame**: The KCF template is retrained from
scratch on each YOLO frame to prevent template drift. The online update
during tracker frames uses exponential blending (`visual_learning_rate`).

### 2.4 Configuration (InterframeTrackerConfig)

| Parameter | Default | Effect |
|-----------|---------|--------|
| `yolo_interval` | 4 | YOLO every N frames. 1 = tracker disabled. |
| `enable_visual` | true | KCF on/off. false = Kalman-only (cheaper, less accurate). |
| `visual_psr_threshold` | 7.0 | PSR threshold (used as reference; actual blending spans PSR 5–15). |
| `visual_learning_rate` | 0.125 | KCF online adaptation rate. |
| `visual_patch_size` | 64 | HOG/FFT patch size (must be power of 2). |
| `roi_padding` | 2.0 | Search region = bbox × this factor. |
| `kalman_process_noise` | 4.0 | Higher = more responsive to acceleration. |
| `kalman_measure_noise` | 1.0 | Higher = smoother but laggier. |
| `smooth_factor` | 0.5 | Output EMA base alpha (velocity-adaptive). 0=max smooth, 1=snap. Size uses 0.4× this value. |

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

1. **Letterbox preprocessing** (enabled by default): The pipeline preserves
   the 640×480 aspect ratio by padding to 640×640 with gray bars instead of
   stretching. This matches how YOLOv8 was trained and eliminates ~33%
   vertical distortion that degraded detection quality.
2. **Lower `model2_conf`** to 0.10–0.15 (via SoulFlow DP or store.json)
3. **Ensure proper lighting**: The model was trained on indoor hand images.
   Auto-exposure in low light reduces camera FPS and can cause motion blur.
4. **Hot-swap a better model**: Use `sysCmd` subcmd 7 to replace at runtime
5. **Input resolution**: Model auto-detects its expected input size from
   RKNN metadata; letterbox padding adapts automatically

---

## 7. FPS Throttling

The AI pipeline can be capped to a target FPS via `ai_target_fps` (DP 30).
When set to a non-zero value, `on_new_sample()` drops frames that arrive
before the next deadline (returns `GST_FLOW_OK` without processing). This
saves CPU/NPU power when the full pipeline rate is not needed.

### 7.1 Camera rate vs AI rate

The GStreamer pipeline requests `framerate=30/1` from v4l2src, but the ISP
selfpath framerate is determined by the sensor clock, not the V4L2 caps
filter. At 640×480, the ISP selfpath typically delivers **40+ fps**
(sensor-limited, not resolution-limited). Every delivered frame contains
unique sensor data — there are no duplicates.

Without `ai_target_fps`, the AI pipeline processes every camera frame,
so total AI FPS equals the actual camera delivery rate, not the
requested 30 fps.

The raw camera delivery rate is logged alongside the AI pipeline FPS every
5 seconds (`cam feed: XX.X fps`). When `ai_target_fps = 0` and the pipeline
can keep up, these two values match. A gap indicates the AI pipeline is
slower than the camera (frames being dropped by the appsink).

### 7.2 Credit-based timing

The throttle uses credit-based timing: after processing a frame, the
deadline advances by `1/target_fps` from the previous deadline (not from
`now`). This prevents aliasing when the camera frame interval doesn't
divide evenly into the target interval.

**Why this matters:** A naive snap-to-`now` approach causes systematic
frame skipping. Example with camera@42fps, target@30fps:
- Camera interval: 23.8 ms, target interval: 33.3 ms
- Snap-to-now: next frame arrives at +23.8 ms → too early → skip →
  next at +47.6 ms → process → effective rate = 42/2 = **21 fps**
- Credit-based: deadline carries fractional credit forward →
  effective rate ≈ **30 fps**

A catch-up guard snaps to `now` when more than one full interval behind
(e.g., after a slow YOLO frame or when the throttle is first enabled)
to prevent burst processing.

### 7.3 Configuration

- `ai_target_fps = 0` (default): unlimited, process every frame from
  the appsink (= camera delivery rate, typically 40+ fps).
- `ai_target_fps = 30`: cap at 30 fps. Both YOLO and tracker frames
  count toward this limit; the YOLO/tracker ratio is unchanged.
- The DP is hot-reloadable — changing it via `setDp` takes effect on
  the next frame with no restart needed.

---

## 8. Performance Characteristics

### 8.1 Measured on RK3566 (1.8 GHz A55, 0.8 TOPS NPU)

Note: ISP selfpath delivers ~40+ fps at 640×480 (sensor-limited). The
`framerate=30/1` v4l2src cap is not enforced by the ISP driver.

| Configuration | Total FPS | YOLO FPS | Tracker FPS | Notes |
|---------------|-----------|----------|-------------|-------|
| YOLO only (no tracker) | ~7-10 | 7-10 | 0 | Limited by NPU inference time |
| Tracker, `yolo_interval=4` | ~14.6 | 3.5–3.7 | 10.8–11.0 | ~60-75% NPU savings |
| HandPreferred mode | ~14.6 | 3.5 | 11.0 | Hand model faster than person model |
| Adaptive interval (static scene) | up to 20+ | ~2 | ~18 | YOLO rate drops for static targets |
| Unlimited (`ai_target_fps=0`) | 40+ | varies | varies | Full camera rate, highest CPU/NPU use |
| `ai_target_fps=30` | ~30 | varies | varies | Credit-based throttle, matches target |
| `ai_target_fps=10` | ~10 | ~2.5 | ~7.5 | FPS-capped, saves CPU power |
| `ai_target_fps=5` | ~5 | ~1.25 | ~3.75 | Minimal CPU load for low-priority use |

### 8.2 CPU cost breakdown

**Per YOLO frame (additional to NPU inference):**

| Operation | Time |
|-----------|------|
| Letterbox pad (640×480 → 640×640) | ~0.15 ms |
| Un-letterbox detections | <0.01 ms |

**Per tracker frame:**

| Operation | Time |
|-----------|------|
| RGB→Gray (640×480, NEON) | ~0.25 ms |
| Patch extraction (bilinear, 64×64) | ~0.1 ms |
| HOG features (16×16×9) | ~0.5 ms |
| FFT 2D (16×16, per channel) | ~1.5 ms |
| Gaussian kernel correlation | ~1.0 ms |
| Kalman predict(dt)+update | <0.01 ms |
| Velocity-adaptive EMA + size smoothing | <0.01 ms |
| **Total** | **~3-5 ms** |

---

## 9. SoulLink DP Reference

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
| 24 | `tracker_smooth_factor` | float | 0.5 | Output EMA base alpha (velocity-adaptive). Size uses 0.4× this. |
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
tracker_smooth_factor = 0.5        # base alpha; velocity-adaptive and size×0.4 applied automatically
tracker_adaptive_interval = true
tracker_hand_confirm = 3
tracker_hand_lost = 15
model2_conf = 0.15
adaptive_tracking = true
max_models_per_frame = 1
```

---

## 10. Troubleshooting

### Hand not being tracked

1. Check `model2_labels` is set to `"hand"` in store.json
2. Check `adaptive_tracking = true`
3. Check `model2_path` points to a valid hand model
4. Verify in logs: `TargetPick: hand=N` should show hand detections when
   hand is in view. If `hand=0` consistently, lower `model2_conf`.
5. Verify model weights are equal in PersonFallback mode:
   `ModelPipeline: slot 0 [...] weight=1` and `slot 1 [...] weight=1`

### Bounding box jitter/shaking

1. Velocity-adaptive smoothing handles most jitter automatically: static
   scenes use half the base alpha, fast motion uses 1.6×. If still jittery,
   lower `tracker_smooth_factor` (e.g., 0.3) to increase the base smoothing.
2. Size "breathing" is suppressed by separate size smoothing (0.4× position
   alpha). If size still fluctuates, the base alpha reduction above helps.
3. Increase `kalman_measure_noise` for smoother Kalman output.
4. If using adaptive interval, ensure `tracker_max_skip` isn't too high
   (8 is good; higher allows more drift before correction).
5. Check PSR values in logs — consistently low PSR (<7) means KCF is
   struggling (poor visual texture, target too small, or wrong patch_size).
   The gradual PSR blending prevents sudden jumps at the threshold, but
   prolonged low PSR means tracking quality is degraded.
6. The output convergence time in frames is approximately `3 / smooth_factor`
   (e.g., 6 frames at 0.5, 10 frames at 0.3).

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
sudo journalctl -u soulcam -f | grep -E "(TargetPick|Target policy|AI pipeline|Letterbox)"

# Startup verification
sudo journalctl -u soulcam --since "30s ago" | grep -E "(slot|weight|scheduler|tracker|Target|Letterbox|capture)"
```

Expected log format:
```
AI pipeline: 14.6 fps total (YOLO 1.8 + tracker 12.8, vel=1.7 psr=9.9) | cam feed: 14.6 fps
```
- `cam feed` = raw camera delivery rate (before throttle)
- When `cam feed` ≈ AI total fps and both are low (~14 fps): camera is
  auto-exposure limited (low light), not an AI bottleneck
- When `cam feed` > AI total fps: AI processing is the bottleneck
- `Letterbox: 640x480 -> 640x640 (pad_top=80, gray=128)` confirms
  aspect-ratio-preserving preprocessing is active
