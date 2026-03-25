# Cascading Perception Pipeline — Implementation Progress

_Started: 2026-03-18 · Last updated: 2026-03-24_
_Board: RK3566 (0.8 TOPS NPU) · Camera: OV5647 160° fisheye_

---

## Goal

Build a cascading perception system that can:
- **Identify** an unbounded number of objects it has ever seen
- **Track** only the most interesting K objects in real-time (KCF + Kalman)
- **Learn** semantic context via a cloud VLM API (labels, attributes, interest)
- Run the full pipeline at near real-time on the RK3566 platform

### Key constraints

| Constraint | Approach |
|------------|----------|
| 0.8 TOPS NPU (single core) | Interleaved scheduling: YOLO on YOLO frames, embeddings on tracker frames |
| Limited RAM (~1 GB) | Tiered hot/cold memory; ring-buffer crops; L2-normalized 128-D embeddings |
| No on-device training | Non-parametric learning: centroid + exemplar accumulation |
| Real-time requirement | Cheap recognition for all, expensive tracking for top-K only |

---

## Phase 0: Multi-Object Association + Crop Extraction

**Status: COMPLETED** (2026-03-19)

### Steps

- [x] Define `TrackSlot` struct with spatial, lifecycle, identity, interest,
      and crop-history fields
- [x] Define `ScoredCrop` struct for quality-scored crop storage (ring buffer)
- [x] Implement `MultiObjectAssociator` class:
  - [x] IoU computation between tracks and detections
  - [x] Predicted position step (linear velocity model)
  - [x] Greedy assignment (highest IoU first, above threshold)
  - [x] New track creation for unmatched detections
  - [x] Age-out logic (miss_count > TTL → remove track)
  - [x] Stable-frame counter for enrollment gating
- [x] Implement `CropExtractor` class:
  - [x] Margin-padded bounding box extraction from RGB frame
  - [x] Quality scoring (size, truncation ratio, simple blur estimate)
  - [x] Ring-buffer insertion into TrackSlot crops[]
- [x] Implement `TrackerPool` class:
  - [x] K-slot pool of `InterframeTracker` instances
  - [x] `assign_slots()`: assign/release slots by top-K interest IDs
  - [x] `update_all()`: propagate KCF+Kalman for all active slots
  - [x] `reinit_slot()`: re-anchor tracker from fresh YOLO detection

### Files created

| File | Lines | Purpose |
|------|-------|---------|
| `ai/multi_object_associator.h` | 98 | TrackSlot + AssociatorConfig + class decl |
| `ai/multi_object_associator.cpp` | ~130 | IoU, predict, assign, age-out |
| `ai/crop_extractor.h` | 41 | CropExtractorConfig + class decl |
| `ai/crop_extractor.cpp` | ~90 | Extract, quality-score, ring-buffer insert |
| `ai/tracker_pool.h` | 66 | TrackerPool class decl |
| `ai/tracker_pool.cpp` | ~120 | Slot management, KCF lifecycle |

---

## Phase 1: Embedding Model + Interleaved NPU Scheduling

**Status: COMPLETED** (2026-03-19)

### Steps

- [x] Define `EmbedderConfig` (model path, embed dim, input size)
- [x] Implement `Embedder` (C-style create/destroy/infer API):
  - [x] RKNN model loading and query
  - [x] Bilinear resize of crops to model input size
  - [x] RKNN inference (UINT8 NHWC input → float output)
  - [x] L2 normalization with NEON SIMD (4-wide dot product + scale)
  - [x] Stub mode when RKNN is unavailable (pixel-statistic features)
- [x] Implement `cosine_similarity()` with NEON acceleration
- [x] Implement `EmbeddingQueue`:
  - [x] Thread-safe priority queue (interest-score ordering)
  - [x] `push()`: replace existing request for same track_id
  - [x] `process_one()`: dequeue highest priority, run embedder, write result
  - [x] Called on tracker frames for interleaved NPU usage

### Files created

| File | Lines | Purpose |
|------|-------|---------|
| `ai/embedder.h` | 45 | EmbedderConfig + C API declarations |
| `ai/embedder.cpp` | ~268 | RKNN inference, bilinear resize, NEON ops |
| `ai/embedding_queue.h` | 53 | EmbedRequest + EmbeddingQueue class |
| `ai/embedding_queue.cpp` | ~70 | Priority queue + process_one |

---

## Phase 2: Unbounded Object Memory Bank

**Status: COMPLETED** (2026-03-19)

### Steps

- [x] Define `ObjectRecord` struct:
  - [x] Visual identity: centroid embedding + multi-view exemplars (max 10)
  - [x] Semantic metadata: name, description, attributes, tags
  - [x] Temporal: first/last seen, seen count, session count
  - [x] Base interest (VLM-assigned)
  - [x] Crop file paths on disk
- [x] Define `MatchResult` with three confidence tiers: None / Uncertain / Confident
- [x] Implement `ObjectMemory`:
  - [x] `match()`: class-filtered cosine similarity against centroid + exemplars
  - [x] `enroll()`: create new ObjectRecord, compute initial centroid
  - [x] `observe()`: update centroid (running average), add novel exemplars
  - [x] `enrich()`: integrate VLM result (name, description, attributes, base_interest)
  - [x] `save()`: minimal JSON serialization of memory to disk
  - [x] `load()`: stub (needs full JSON parser for cold-tier loading)
  - [x] `save_crops()`: write reference crops as raw files to memory_dir

### Files created

| File | Lines | Purpose |
|------|-------|---------|
| `ai/object_memory.h` | 137 | ObjectRecord + MatchResult + MemoryConfig + class |
| `ai/object_memory.cpp` | ~200 | match, enroll, observe, enrich, save, load |

---

## Phase 3: Interest Scoring + Tracking Allocation

**Status: COMPLETED** (2026-03-19)

### Steps

- [x] Define `InterestConfig` with tunable weights for each signal component
- [x] Implement `InterestScorer`:
  - [x] `score()`: composite interest from novelty, motion, size, uncertainty,
        appearance change, VLM base_interest, frequency decay
  - [x] `rank_and_select()`: sort all tracks by interest, return top-K IDs
  - [x] Interest score written back into TrackSlot for downstream use

### Interest formula

```
interest = 0.4 * novelty_signal       // unrecognized or recently enrolled
         + 0.2 * novelty_decay        // recognized but new (<24h half-life)
         + 0.15 * normalized_velocity // moving objects draw attention
         + 0.10 * normalized_area     // larger objects more salient
         + 0.10 * (1 - match_conf)    // uncertain identity is interesting
         + 0.10 * embedding_delta     // appearance changed since memory
         + base_interest              // VLM semantic signal
         - 0.05 * log(1 + seen_count) // familiar objects get boring
```

### Files created

| File | Lines | Purpose |
|------|-------|---------|
| `ai/interest_scorer.h` | 61 | InterestConfig + InterestScorer class |
| `ai/interest_scorer.cpp` | ~80 | score(), rank_and_select() |

---

## Phase 4: VLM API Integration

**Status: COMPLETED** (2026-03-19)

### Steps

- [x] Define `VlmConfig`, `VlmEnrichRequest`, `VlmEnrichResult`
- [x] Implement `VlmClient`:
  - [x] Background worker thread with condition variable wake-up
  - [x] `enqueue()`: non-blocking request submission with queue cap
  - [x] `build_prompt()`: structured JSON-only prompt for object identification
  - [x] `process_request()`:
    - [x] With libcurl: HTTP POST to OpenAI-style API, parse response content
    - [x] Without libcurl (stub): generate placeholder labels from coarse class
  - [x] `parse_response()`: minimal JSON field extraction (no external JSON dep)
  - [x] Result callback wired to `ObjectMemory::enrich()`
- [x] CMake: optional libcurl detection (`pkg_check_modules(CURL libcurl)`)
- [x] Compile guard: `#ifdef SOULCAM_HAVE_CURL` for all curl code

### Files created

| File | Lines | Purpose |
|------|-------|---------|
| `ai/vlm_client.h` | 91 | VlmConfig + request/result + VlmClient class |
| `ai/vlm_client.cpp` | ~265 | Worker thread, curl HTTP, prompt, parse |

---

## Phase 5: Perception Engine + Integration

**Status: COMPLETED** (2026-03-20)

### Steps

- [x] Define `PerceivedObject` and `PerceptionFrame` output types
- [x] Define `PerceptionConfig` aggregating all component configs
- [x] Implement `PerceptionEngine`:
  - [x] Constructor: instantiate all 8 component objects, load memory
  - [x] Destructor: save memory, clean up all components
  - [x] `process_yolo_frame()`: full cascade (associate → crop → match →
        enroll → queue embed → score → assign trackers → reinit KCF)
  - [x] `process_tracker_frame()`: KCF update + one embedding extraction
  - [x] `build_output()`: convert internal state to PerceptionFrame
  - [x] `try_enroll()`: enrollment logic with VLM enrichment queue
- [x] Integrate into `ai_capture.cpp`:
  - [x] Conditional `PerceptionEngine` instantiation from config
  - [x] Route YOLO and tracker frames through perception when enabled
  - [x] Convert PerceptionFrame → Detection vector for legacy callback compat
- [x] Add `PerceptionPipelineConfig` to `soulcam.h`
- [x] Add perception DPs in `store_config.h/.cpp`, wired via `store_to_config()` in `main.cpp`
- [x] Wire perception output to SoulLink MQTT:
  - [x] New `PerceptionObject` struct in `soullink/module.h`
  - [x] `submitPerceptions()` method with `soulcam.perceptions.v1` JSON schema
  - [x] Message id=5 to distinguish from legacy detections (id=4)
- [x] Update SoulFlow `DebugRtspNode.tsx`:
  - [x] Parse perceptions.v1 messages (msg.id === 5)
  - [x] Interest-based color gradient (green → red)
  - [x] Thick dashed borders for actively tracked objects
  - [x] VLM name, interest badge (★), tracking icon (⟐) in labels
  - [x] Status bar: `obj:<count> mem:<total> trk:<active>`
- [x] Add all 9 new .cpp files to `CMakeLists.txt`
- [x] Add optional libcurl dependency to CMake

### Files created

| File | Lines | Purpose |
|------|-------|---------|
| `ai/perception_engine.h` | 152 | PerceivedObject, PerceptionFrame, PerceptionConfig, engine class |
| `ai/perception_engine.cpp` | ~283 | Full pipeline orchestration |

### Files modified

| File | Change |
|------|--------|
| `soulcam.h` | Added `PerceptionPipelineConfig` struct and `Config::perception` member |
| `main.cpp` | CLI options (160-166), startup logging, perception→SoulLink bridge |
| `pipeline/ai_capture.h` | Added `ai_capture_get_perception()` accessor |
| `pipeline/ai_capture.cpp` | Conditional perception engine init + frame routing |
| `soullink/module.h` | `PerceptionObject` struct + `submitPerceptions()` decl |
| `soullink/module.cpp` | `submitPerceptions()` implementation with perceptions.v1 JSON |
| `CMakeLists.txt` | 9 new source files + optional libcurl |
| `DebugRtspNode.tsx` (SoulFlow) | Perception overlay rendering + msg.id=5 handler |

---

## Build & Deploy Log

| Date | Event | Result |
|------|-------|--------|
| 2026-03-19 | Initial implementation of Phases 0-4 | All files created, not yet compiled on device |
| 2026-03-20 | Phase 5 integration + SoulLink + SoulFlow | Full integration complete |
| 2026-03-20 | First device build attempt | Failed: `PerceptionPipelineConfig` used before definition |
| 2026-03-20 | Fix: move struct before Config | Failed: case 65 collided with 'A' (ASCII 65) |
| 2026-03-20 | Fix: remap perception CLI options to 160-166 | Failed: missing `#include <string>` in embedder.h |
| 2026-03-20 | Migrate perception config from CLI to DP system | CLI flags removed, 15 DPs added |
| 2026-03-20 | Fix: add include + RKNN_QUERY_IN_OUT_NUM | **Build succeeded** on RK3566 |
| 2026-03-20 | Service restart | Runs correctly, reports "Perception pipeline: disabled" (not enabled via CLI) |
| 2026-03-23 | V-0 rerun (strict restart-window check) | Failed: reproducible `status=6/ABRT` on service restart |
| 2026-03-23 | Fix: `model_pipeline_destroy()` lock/delete ordering | Removed use-after-free on mutex unlock during teardown |
| 2026-03-23 | Fix: shutdown callback ordering (`g_ai_for_policy`, appsink callbacks) | Teardown race reduced; V-0 now stable |
| 2026-03-23 | V-0 baseline verification | **PASS** (no ABRT, perception disabled log present, RTSP and AI FPS OK) |
| 2026-03-23 | V-1 perception startup verification | **PASS** (all expected startup signatures found, no startup crash) |
| 2026-03-23 | V-2 multi-object association verification | **PASS** (AI FPS logs and frame progression observed, no crash in 30s window) |
| 2026-03-24 | TODO-1: Embedding model training pipeline created | 9 Python scripts at `rknn/embedding/` |
| 2026-03-24 | Trained MobileNetV3-Small on SOP (ArcFace, 60 epochs) | **R@1=0.6591** on 60K test images |
| 2026-03-24 | Exported ONNX → converted INT8 RKNN (1.9 MB) | Deployed to device, embedder loads OK on NPU |
| 2026-03-23 | Long-run instability investigation | Found `oom-kill` events (`status=9/KILL`, anon RSS ~1.6-1.7 GB) with perception enabled |
| 2026-03-23 | Fix: stable label lifetime in perception->legacy bridge | Replaced transient `c_str()` pointers with interned process-lifetime labels |
| 2026-03-23 | Fix: drain embedding queue on YOLO frames | Prevented unbounded queued crop accumulation when tracker frames are sparse/absent |
| 2026-03-23 | Post-fix soak (ongoing) | No new OOM markers in last 12 minutes; RSS growth rate significantly reduced; continue monitoring |

---

## Verification Test Plan

Step-by-step tests to validate each perception feature in isolation.
Run sequentially -- each step builds on the previous one being confirmed.

All tests use SSH to device. Perception is configured via DPs (no CLI flags).

```bash
SSH="sshpass -p 'shb084ww' ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null ubuntu@192.168.1.45"
LOGS="echo 'shb084ww' | sudo -S journalctl -u soulcam --since '30s ago' --no-pager 2>&1"
SC="/home/ubuntu/SoulCam/build/soulcam"
MODEL="/home/ubuntu/YoloV8-NPU/rk3566/yolov8n.rknn"
STORE="/var/lib/soulcam/store.json"

# Helper: set a DP value in store.json on device (requires jq or python)
# Usage: set_dp <key> <value>
# The simplest method is to edit store.json directly or use mosquitto_pub.
```

---

### V-0: Baseline — legacy mode still works

**Goal:** Confirm the build didn't break existing (non-perception) operation.

```bash
# Restart with perception disabled (default DP state)
$SSH "echo 'shb084ww' | sudo -S systemctl restart soulcam"
sleep 5
$SSH "$LOGS"
```

**Pass criteria:**
- [x] No crash or ABRT in logs
- [x] Log contains `Perception pipeline: disabled`
- [x] RTSP stream accessible: `ffprobe -rtsp_transport tcp -v quiet rtsp://192.168.1.45:8554/cam`
- [x] If AI is enabled in service, `AI pipeline: XX.X fps` appears in logs after ~10s

---

### V-1: Perception pipeline startup

**Goal:** Verify `enable_perception` DP activates the pipeline and all components initialize.

```bash
# Enable perception DP in store.json, then restart
$SSH "echo 'shb084ww' | sudo -S python3 -c \"
import json, pathlib
p = pathlib.Path('$STORE')
d = json.loads(p.read_text()) if p.exists() else {}
d['enable_perception'] = True
p.write_text(json.dumps(d, indent=2))
\""
$SSH "echo 'shb084ww' | sudo -S systemctl restart soulcam"
sleep 8
$SSH "$LOGS"
```

**Pass criteria (search startup logs):**
- [x] `Perception pipeline: enabled (max_tracked=5, embedder=(stub), vlm=disabled)`
- [x] `PerceptionEngine: initialized (max_tracked=5, memory=0 objects)`
- [x] `Embedder: no model path, running in stub mode` (or `RKNN not available`)
- [x] `Perception pipeline enabled: max_tracked=5`
- [x] No segfault, no crash during first 15 seconds
- [x] RTSP stream still works alongside perception

---

### V-2: Multi-object association

**Goal:** Verify YOLO detections are associated to persistent track slots.

```bash
# Ensure enable_perception=true in store.json (see V-1), then restart with verbose
$SSH "echo 'shb084ww' | sudo -S systemctl restart soulcam"
sleep 8
# Point camera at scene with 2+ objects, check logs
$SSH "$LOGS" | tail -60
```

**Pass criteria:**
- [x] `AI pipeline: XX.X fps total (YOLO XX.X + tracker XX.X` appears (FPS logging works)
- [x] No crash after 30 seconds of running
- [x] Frame count progresses (perception processes both YOLO and tracker frames)

**Manual check (add temporary logging if needed):**
To see track creation, add a one-time log line in `multi_object_associator.cpp`
after new track creation, or check via SoulLink messages (V-9).

---

### V-3: Crop extraction + memory directory

**Goal:** Verify crops are extracted and the memory directory is populated.

```bash
# Set perception_memory_dir DP to test dir, restart
$SSH "echo 'shb084ww' | sudo -S python3 -c \"
import json, pathlib
p = pathlib.Path('$STORE')
d = json.loads(p.read_text()) if p.exists() else {}
d['perception_memory_dir'] = '/tmp/soulcam_test_memory'
p.write_text(json.dumps(d, indent=2))
\""
$SSH "echo 'shb084ww' | sudo -S systemctl restart soulcam"
sleep 30

# Check if memory directory was created and has content
$SSH "ls -la /tmp/soulcam_test_memory/ 2>&1"
$SSH "find /tmp/soulcam_test_memory/ -type f 2>&1 | head -20"
```

**Pass criteria:**
- [ ] `/tmp/soulcam_test_memory/` directory exists
- [ ] `memory.json` file exists (even if small/empty initially)
- [ ] Crop image files present (e.g. `crops/obj_1_*.raw` or similar)
- [ ] Log shows `ObjectMemory: enrolled object #N class=...`

---

### V-4: Embedding pipeline (stub mode)

**Goal:** Verify stub embeddings are computed and written to tracks.

Stub mode produces pixel-statistic features (not useful for re-ID but proves the pipeline runs).

```bash
# Perception already enabled with test memory dir (V-3). Restart and check logs.
$SSH "echo 'shb084ww' | sudo -S systemctl restart soulcam"
sleep 30
$SSH "$LOGS" | grep -i "enroll\|embed\|memory"
```

**Pass criteria:**
- [ ] `ObjectMemory: enrolled object #1 class=person` (or whatever class is detected)
- [ ] No errors from Embedder (no `rknn_init failed` -- should skip RKNN gracefully)
- [ ] Enrollment happens within first 5-10 seconds of seeing an object
      (enrollment_delay_frames = 5 by default)

---

### V-5: Object memory — enrollment and save

**Goal:** Verify objects are enrolled into memory and persisted to disk.

```bash
# Perception enabled with test memory dir (V-3). Restart, run 30s, then stop cleanly.
$SSH "echo 'shb084ww' | sudo -S systemctl restart soulcam"
sleep 30
$SSH "echo 'shb084ww' | sudo -S systemctl stop soulcam"
$SSH "$LOGS" | tail -30

# Check saved memory
$SSH "cat /tmp/soulcam_test_memory/memory.json 2>&1 | head -50"
```

**Pass criteria:**
- [ ] `memory.json` contains valid JSON with at least one object
- [ ] JSON has fields: `object_id`, `coarse_class`, `centroid` (float array),
      `exemplars`, `name`, `first_seen_ts`, `last_seen_ts`, `seen_count`
- [ ] Log shows `ObjectMemory: saved N objects to /tmp/soulcam_test_memory/memory.json`
- [ ] Centroid array has 128 floats (embed_dim = 128)

---

### V-6: VLM stub enrichment

**Goal:** Verify stub VLM generates placeholder labels for enrolled objects.

```bash
# Perception enabled with test memory dir (V-3). Restart and check VLM logs.
$SSH "echo 'shb084ww' | sudo -S systemctl restart soulcam"
sleep 30
$SSH "$LOGS" | grep -i "VlmClient\|enriched"
```

**Pass criteria:**
- [ ] `VlmClient: stub enrichment for #1 -> "person #1"` (or similar per-class label)
- [ ] `ObjectMemory: enriched #1 -> "person #1"`
- [ ] `memory.json` shows `"name": "person #1"` (or `"<class> #<id>"`)

**Note:** Real VLM (non-stub) requires libcurl + API key. See TODO-2.

---

### V-7: Interest scoring + tracker allocation

**Goal:** Verify interest scores are computed and top-K objects get KCF trackers.

```bash
# Set perception_max_tracked=2 via DP, restart. Point camera at 3+ objects.
$SSH "echo 'shb084ww' | sudo -S python3 -c \"
import json, pathlib
p = pathlib.Path('$STORE')
d = json.loads(p.read_text()) if p.exists() else {}
d['perception_max_tracked'] = 2
p.write_text(json.dumps(d, indent=2))
\""
$SSH "echo 'shb084ww' | sudo -S systemctl restart soulcam"
sleep 30
$SSH "$LOGS" | tail -40
```

**Pass criteria:**
- [ ] Process runs without crash with max_tracked=2
- [ ] When 3+ objects visible, only 2 should have `tracked: true` in SoulLink output
- [ ] FPS logs show `YOLO XX.X + tracker XX.X` (tracker frames are running)

**Deeper verification (requires SoulLink/MQTT or SoulFlow):**
Subscribe to MQTT topic and check `"tracked":true` count matches `perception_max_tracked` DP value.

---

### V-8: SoulLink perception messages (MQTT)

**Goal:** Verify perception data is published via MQTT with the correct schema.

```bash
# On device: subscribe to SoulLink MQTT output topic
# (assumes mosquitto-clients installed, and soullink is using default broker 127.0.0.1)
$SSH "timeout 20 mosquitto_sub -h 127.0.0.1 -t 'soulcam/debug/out/#' -C 5 2>&1"
```

Run soulcam with perception in parallel (in another terminal or via service).

**Pass criteria:**
- [ ] Messages received on MQTT topic
- [ ] Message contains `"id": 5` (kMsgPerceptions, not kMsgAIDetections=4)
- [ ] Message contains `"schema": "soulcam.perceptions.v1"`
- [ ] Each object has: `trackId`, `clsId`, `label`, `box` (array of 4 normalized floats),
      `interest` (float), `tracked` (bool)
- [ ] If object is enrolled: `identity` block with `objectId`, `name`, `matchConf`
- [ ] Top-level fields: `total_mem` (int), `active_trk` (int), `fw`, `fh`

---

### V-9: SoulFlow UI overlay

**Goal:** Verify perception data renders correctly in the SoulFlow desktop app.

**Prerequisites:** SoulFlow running on host, connected to device's MQTT broker.

```
1. Open SoulFlow, navigate to the Debug RTSP node
2. Connect to rtsp://192.168.1.45:8554/cam
3. Ensure SoulLink connection is active (green indicator)
4. Ensure `enable_perception=true` in store.json, restart soulcam service
```

**Pass criteria:**
- [ ] Bounding boxes appear on detected objects
- [ ] Box color follows interest gradient (green = low interest, red = high)
- [ ] Actively tracked objects have **thick dashed** borders
- [ ] Passively recognized objects have thin borders (gray-ish)
- [ ] Labels show VLM name (e.g. "person #1") and interest badge (e.g. "★42")
- [ ] Actively tracked objects show ⟐ icon
- [ ] Status bar at bottom shows `obj:<N> mem:<M> trk:<K>` format
- [ ] When msg.id=5 arrives, legacy detection rendering (msg.id=4) is not shown

---

### V-10: Memory persistence across restarts

**Goal:** Verify memory survives a restart (currently limited by `load()` being a stub).

```bash
# Run 1: enroll some objects (perception + test memory dir already set in V-3)
$SSH "echo 'shb084ww' | sudo -S systemctl restart soulcam"
sleep 30
$SSH "echo 'shb084ww' | sudo -S systemctl stop soulcam"
$SSH "$LOGS" | tail -10

# Check what was saved
$SSH "wc -c /tmp/soulcam_test_memory/memory.json"

# Run 2: restart and check if memory loads
$SSH "echo 'shb084ww' | sudo -S systemctl restart soulcam"
sleep 15
$SSH "$LOGS" | grep -i "memory\|load"
```

**Expected current behavior (load is a stub):**
- [ ] Run 1: `ObjectMemory: saved N objects to .../memory.json`
- [ ] Run 2: `ObjectMemory: found saved memory at .../memory.json (full JSON parsing TBD, XXX bytes)`
- [ ] Run 2: objects are NOT re-identified (load doesn't parse yet)

**After TODO-3 is completed, re-run and expect:**
- [ ] Run 2: `ObjectMemory: loaded N objects from .../memory.json`
- [ ] Run 2: previously-enrolled objects recognized without re-enrollment

---

### V-11: Performance comparison

**Goal:** Measure overhead of perception pipeline vs legacy mode.

```bash
# Test A: Legacy mode (no perception)
$SSH "cd /home/ubuntu/SoulCam && timeout 30 $SC --ai --model $MODEL 2>&1" \
  | grep "AI pipeline:"

# Test B: Perception mode (enable_perception=true already set)
$SSH "echo 'shb084ww' | sudo -S systemctl restart soulcam"
sleep 30
$SSH "$LOGS" | grep "AI pipeline:"

# CPU comparison
$SSH "cd /home/ubuntu/SoulCam && $SC --ai --model $MODEL &"
sleep 10
$SSH "top -bn1 -p \$(pgrep soulcam) | tail -2"
$SSH "kill \$(pgrep soulcam)"
sleep 2
$SSH "echo 'shb084ww' | sudo -S systemctl restart soulcam"
sleep 10
$SSH "top -bn1 -p \$(pgrep soulcam) | tail -2"
$SSH "kill \$(pgrep soulcam)"
```

**Record results:**

| Metric | Legacy (no perception) | With perception | Acceptable? |
|--------|----------------------|-----------------|-------------|
| AI total fps | ___ | ___ | >15 fps |
| YOLO fps | ___ | ___ | within 10% of legacy |
| CPU % | ___ | ___ | <25% |
| Memory (RSS) | ___ MB | ___ MB | <60 MB |

**Pass criteria:**
- [ ] Perception overhead < 5 fps total degradation
- [ ] CPU increase < 10 percentage points
- [ ] Memory increase < 20 MB (embedding vectors + track state)
- [ ] No frame drops on RTSP stream (still 30 fps)

---

### V-12: Cleanup

After all tests pass:

```bash
# Remove test memory directory
$SSH "rm -rf /tmp/soulcam_test_memory"

# Restore normal service operation
$SSH "echo 'shb084ww' | sudo -S systemctl start soulcam"
```

---

### Verification summary

| Test | Feature | Status |
|------|---------|--------|
| V-0 | Legacy mode not broken | [x] |
| V-1 | Perception startup | [x] |
| V-2 | Multi-object association | [x] |
| V-3 | Crop extraction + memory dir | [ ] |
| V-4 | Embedding pipeline (stub) | [ ] |
| V-5 | Memory enrollment + save | [ ] |
| V-6 | VLM stub enrichment | [ ] |
| V-7 | Interest scoring + tracker allocation | [ ] |
| V-8 | SoulLink MQTT messages | [ ] |
| V-9 | SoulFlow UI overlay | [ ] |
| V-10 | Memory persistence (stub load) | [ ] |
| V-11 | Performance regression | [ ] |

---

## Context Reference Index

All file paths needed to work on any remaining TODO. Open these alongside this
document when starting a new chat session.

### Core architecture (read first)

| File | What it tells you |
|------|-------------------|
| `doc/framework/SOULCAM_FRAMEWORK.md` | Full architecture, pipeline diagrams, all CLI options, verification results |
| `doc/build/BUILD_AND_DEPLOY.md` | How to build, deploy (rsync), restart service, verify. SSH creds, device IP, paths |
| `src/soulcam.h` | All config structs including `PerceptionPipelineConfig`; `Detection` type shared by AI and overlay |
| `src/CMakeLists.txt` | Build system: source list, optional deps (RKNN, Cairo, libcurl, OpenCV, Rive, Tuya) |
| `src/main.cpp` | CLI arg parsing (perception opts at cases 160-166), startup orchestration, `on_detections` callback that bridges perception→SoulLink |

### Perception pipeline modules (the 9 new files)

| File | What it does |
|------|-------------|
| `src/ai/multi_object_associator.h/.cpp` | `TrackSlot` struct (the central per-object state), `MultiObjectAssociator` (IoU association) |
| `src/ai/tracker_pool.h/.cpp` | `TrackerPool`: K-slot KCF tracker management; `assign_slots()`, `update_all()` |
| `src/ai/crop_extractor.h/.cpp` | `CropExtractor`: quality-scored crop extraction, ring-buffer per track |
| `src/ai/embedder.h/.cpp` | `Embedder`: RKNN embedding model wrapper (128-D), `l2_normalize()`, `cosine_similarity()`. Stub mode if no model |
| `src/ai/embedding_queue.h/.cpp` | `EmbeddingQueue`: priority queue processed on tracker frames (interleaved NPU) |
| `src/ai/object_memory.h/.cpp` | `ObjectMemory`: `ObjectRecord`, match/enroll/observe/enrich/save/load. **`load()` is currently a stub** |
| `src/ai/interest_scorer.h/.cpp` | `InterestScorer`: composite scoring formula, `rank_and_select()` returns top-K |
| `src/ai/vlm_client.h/.cpp` | `VlmClient`: async background thread, libcurl HTTP POST (stub without curl), prompt builder, response parser |
| `src/ai/perception_engine.h/.cpp` | `PerceptionEngine`: orchestrator wiring all above; `process_yolo_frame()`, `process_tracker_frame()`, `build_output()` |

### Integration points (how perception connects to the rest)

| File | Relevant section |
|------|-----------------|
| `src/pipeline/ai_capture.h` | `ai_capture_get_perception()` accessor |
| `src/pipeline/ai_capture.cpp` | Lines ~460-530: perception frame routing in `on_new_sample`; Lines ~735-770: `PerceptionEngine` init from config |
| `src/soullink/module.h` | `PerceptionObject` struct, `submitPerceptions()` declaration |
| `src/soullink/module.cpp` | `submitPerceptions()` implementation: `soulcam.perceptions.v1` JSON schema, message id=5 |
| `SoulFlow: src/renderer/src/components/debug/DebugRtspNode.tsx` | Perception overlay rendering: interest coloring, identity labels, msg.id=5 handler |

### Existing AI infrastructure (legacy, still active)

| File | What it does |
|------|-------------|
| `src/ai/detector.h/.cpp` | RKNN YOLO wrapper: model load, DFL decode, NMS, NEON post-processing |
| `src/ai/model_pipeline.h/.cpp` | Multi-model slot orchestrator (N models, frame skipping, weighted scheduling) |
| `src/ai/interframe_tracker.h/.cpp` | Single-target KCF + Kalman tracker (reused by `TrackerPool`) |
| `src/ai/hand_target_tracker.h/.cpp` | Legacy hand/person single-target tracker |

### SoulLink / DP system

| File | What it tells you |
|------|-------------------|
| `soullink/docs/dp_catalog.md` | Full DP (Data Point) reference: IDs, types, defaults, descriptions |
| `soullink/docs/protocol_mapping.md` | SoulLink MQTT message format, cmd IDs, topic structure |
| `soullink/SOULCAM_SOULLINK_CLIENT_PLAN.md` | SoulLink integration plan and architecture |
| `src/store/store_config.h` | DP enum definitions and default values |
| `src/store/store.h/.cpp` | Persistent DP store (JSON file at `/var/lib/soulcam/store.json`) |

### Embedding model training pipeline

| File | What it does |
|------|-------------|
| `rknn/embedding/README.md` | Full training-to-deployment instructions |
| `rknn/embedding/model.py` | MobileNetV3-Small + 128-D metric learning head |
| `rknn/embedding/dataset.py` | SOP + folder datasets, PK sampler for triplet mining |
| `rknn/embedding/losses.py` | Batch-hard triplet loss, cross-batch memory |
| `rknn/embedding/train.py` | Training loop, warmup+cosine LR, AMP, checkpointing |
| `rknn/embedding/evaluate.py` | Recall@K, similarity stats, threshold analysis |
| `rknn/embedding/export_onnx.py` | PyTorch → ONNX with verification |
| `rknn/embedding/convert_to_rknn.py` | ONNX → RKNN (INT8, RK3566) |
| `rknn/embedding/validate_on_device.py` | On-device latency + correctness validation |

### Device & hardware references

| File | What it tells you |
|------|-------------------|
| `doc/isp/NV12_Y_ZERO_BUG.md` | ISP warm-up behavior, NV12 first-frame artifact |
| `doc/issues/RKAIQ_FPS_AND_AE_OSCILLATION.md` | AE oscillation, FPS issues, RKAIQ tuning |
| `doc/rtsp/KEY_POINTS.md` | RTSP stack, ISP dual-path topology, device nodes |
| `doc/aiq/KEY_POINTS.md` | RKAIQ calibration, OV5647 sensor investigation |
| `rknn/include/rknn_api.h` | RKNN SDK C API (rknn_init, rknn_query, rknn_inputs_set, etc.) |

### SoulFlow project (separate workspace)

| File | What it does |
|------|-------------|
| `/mnt/d/WebProjects/SoulFlow/src/renderer/src/components/debug/DebugRtspNode.tsx` | RTSP debug node: overlay rendering, SoulLink message handling, perception UI |

### Build & deploy quick reference

```
Device:   ubuntu@192.168.1.45   password: shb084ww
Project:  /home/ubuntu/SoulCam (device)  /home/shb3014/embeddedProjects/SoulCam (host)
Build:    ssh → cd /home/ubuntu/SoulCam && bash scripts/build.sh
Service:  sudo systemctl restart soulcam
Logs:     sudo journalctl -u soulcam --since '30s ago' --no-pager
Full deploy one-liner: see doc/build/BUILD_AND_DEPLOY.md §5.1 and §9
```

### Design origin

The perception pipeline design was developed in this prior chat session:
[Perception pipeline design](1a5ed9a8-7e4b-4e3a-923b-8464ab49607f)

---

## Remaining Work

### TODO-1: Train/deploy embedding model

**Priority: must-do** · Touches: `src/ai/embedder.h/.cpp`
**Status: PIPELINE READY** (2026-03-24) — training scripts complete, awaiting dataset + training run

- [x] Research lightweight re-ID architectures → selected **MobileNetV3-Small**
      (2.5M params, ImageNet pretrained, well-supported by rknn-toolkit2)
- [x] Build full training pipeline at `rknn/embedding/`:
  - [x] `model.py`: MobileNetV3-Small backbone + 576→128-D embedding head
        (Linear→BN→HardSwish→Linear→BN), L2-normalized output
  - [x] `dataset.py`: Stanford Online Products loader + generic folder dataset +
        PK sampler (P classes × K samples per batch for triplet mining)
  - [x] `losses.py`: Batch-hard triplet loss (hardest positive + hardest negative
        per anchor, margin 0.3), batch-all variant, cross-batch memory bank
  - [x] `train.py`: Full training loop with warmup + cosine LR, backbone
        freeze/unfreeze, AMP support, Recall@K eval, checkpoint management
  - [x] `evaluate.py`: Recall@K retrieval, same/diff class similarity stats,
        threshold analysis for `match_confident`/`match_uncertain` tuning
  - [x] `export_onnx.py`: PyTorch → ONNX export with onnxruntime verification
  - [x] `generate_calibration_images.py`: Extract representative subset for INT8
  - [x] `convert_to_rknn.py`: ONNX → RKNN (INT8 quantized, RK3566 target,
        mean=[0,0,0] std=[255,255,255] matching C++ embedder expectations)
  - [x] `validate_on_device.py`: On-device validation (latency, consistency,
        discrimination) using rknnlite2
- [x] Train on Stanford Online Products (ArcFace loss, 60 epochs, GTX 1660 Super)
  - Best R@1 = 0.6591 on SOP test set (60,502 images, 11,316 classes)
  - R@2 = 0.7124, R@4 = 0.7597, R@8 = 0.7999
  - Loss: ArcFace (scale=30, margin=0.5) — triplet losses stalled at R@1=0.29
- [x] Export to ONNX (340.9 KB, verification PASS: max_diff=0.000005)
- [x] Convert with rknn-toolkit2 to INT8 RKNN (1.9 MB, `embedding_rk3566_i8.rknn`)
- [x] Deploy to device at `/home/ubuntu/models/embedding_rk3566_i8.rknn`
- [x] Set `perception_embedder_model` DP in store.json
- [x] Verified on device: model loads, NPU inference runs, objects enrolled
  - `Embedder: loaded (input=128x128, output_dim=128)` in logs
  - Objects enrolled: laptop, tv, potted plant, chair, handbag, etc.
  - AI FPS: ~6.5 (YOLO + embedding interleaved on single NPU core)
- [ ] Measure per-crop inference latency (needs rknnlite2 or timing instrumentation)
- [ ] Tune match_confident/match_uncertain thresholds for ArcFace similarity
      distribution (same-class mean ~0.27, diff-class mean ~0.00 on SOP)

**Architecture decision:**
MobileNetV3-Small was chosen over OSNet-x0.25 and EfficientNet-Lite0 because:
- Best rknn-toolkit2 compatibility (standard torchvision model, clean ONNX)
- 2.5M params → ~1.5 MB INT8 RKNN file
- ImageNet pretraining gives strong feature initialization
- Well-proven at 128×128 input resolution
- The 576→128 projection head uses BN + HardSwish for RKNN-friendly quantization

**Pipeline files created:**

| File | Purpose |
|------|---------|
| `rknn/embedding/model.py` | MobileNetV3-Small + 128-D embedding head |
| `rknn/embedding/dataset.py` | SOP + folder loaders, PK sampler, transforms |
| `rknn/embedding/losses.py` | Batch-hard/all triplet loss, cross-batch memory |
| `rknn/embedding/train.py` | Training loop, LR schedule, eval, checkpointing |
| `rknn/embedding/evaluate.py` | Recall@K, similarity analysis, threshold tuning |
| `rknn/embedding/export_onnx.py` | ONNX export with onnxruntime verification |
| `rknn/embedding/generate_calibration_images.py` | INT8 calibration image extraction |
| `rknn/embedding/convert_to_rknn.py` | ONNX → RKNN conversion (INT8/FP16) |
| `rknn/embedding/validate_on_device.py` | On-device latency + correctness tests |
| `rknn/embedding/requirements.txt` | Python dependencies |
| `rknn/embedding/README.md` | Full instructions |

**Context files:**
- `src/ai/embedder.h` -- `EmbedderConfig` struct (model_path, embed_dim, input_size)
- `src/ai/embedder.cpp` -- RKNN init, bilinear resize, inference, stub path
- `rknn/include/rknn_api.h` -- RKNN SDK API reference
- `src/ai/object_memory.h` -- `MemoryConfig.match_confident` / `match_uncertain` thresholds
- `doc/build/BUILD_AND_DEPLOY.md` §8 -- model management, upload, hot-swap

**Notes:**
- Currently runs in stub mode (pixel-statistic features, not useful for real re-ID)
- The embedder already handles resize, RKNN init, and L2 normalization; only the
  model file is missing
- Input is UINT8 NHWC RGB, output is float vector; any model matching this
  contract works
- RKNN conversion uses mean=[0,0,0] std=[255,255,255] to match the ToTensor()
  [0,1] normalization used during training

---

### TODO-2: Install libcurl + configure VLM API

**Priority: must-do** · Touches: `src/ai/vlm_client.cpp`, device packages

- [ ] SSH to device and install: `sudo apt install libcurl4-openssl-dev`
- [ ] Rebuild (CMake will auto-detect and define `SOULCAM_HAVE_CURL=1`)
- [ ] Choose a VLM API (OpenAI GPT-4o, Anthropic Claude, local Ollama, etc.)
- [ ] Configure via DPs: `perception_vlm_api_url`, `perception_vlm_api_key`, `perception_vlm_enabled`
- [ ] Test enrichment: enable perception, let an object enroll, verify VLM
      callback populates `ObjectRecord.name`, `.description`, `.base_interest`
- [ ] Consider adding base64-encoded crop images in the API request body for
      true multi-modal enrichment (currently text-only prompt)

**Context files:**
- `src/ai/vlm_client.h` -- `VlmConfig`, `VlmEnrichRequest`, `VlmEnrichResult`
- `src/ai/vlm_client.cpp` -- `#ifdef SOULCAM_HAVE_CURL` code path, `build_prompt()`,
  `parse_response()`, curl setup
- `src/CMakeLists.txt` -- lines 70-78: libcurl detection
- `src/ai/object_memory.h` -- `ObjectRecord` fields populated by `enrich()`
- `src/ai/perception_engine.cpp` -- VLM result callback wired in constructor

---

### TODO-3: Implement ObjectMemory::load()

**Priority: must-do** · Touches: `src/ai/object_memory.cpp`

- [ ] Implement JSON parsing in `ObjectMemory::load()` to reconstruct
      `ObjectRecord` entries from disk (currently a stub that logs a warning)
- [ ] Parse: object_id, coarse_class, centroid, exemplars, name, description,
      attributes, tags, temporal metadata, base_interest, crop_paths
- [ ] Consider using the existing `soullink/json.h` helper or adding a minimal
      JSON parser (no external dependency preferred)
- [ ] Handle cold-tier: if memory exceeds `hot_tier_max`, only load recent objects
      into RAM, keep older ones indexed on disk for on-demand loading
- [ ] Add versioning to the JSON format for future schema migration
- [ ] Test: enroll objects → restart soulcam → verify objects are reloaded and
      re-identified without re-enrollment

**Context files:**
- `src/ai/object_memory.h` -- `ObjectRecord` struct, `MemoryConfig` (storage_dir,
  hot_tier_max, cold_demote_days)
- `src/ai/object_memory.cpp` -- `save()` (writes JSON), `load()` (stub), `save_crops()`
- `src/soullink/json.h` -- lightweight JSON builder (may be reusable for parsing)
- `src/store/store.cpp` -- example of JSON parsing in the codebase (store.json load)

**Notes:**
- `save()` already writes a well-structured JSON file to `{storage_dir}/memory.json`
- `load()` needs to parse that same format back
- Embedding vectors are stored as float arrays; ensure precision is preserved

---

### TODO-4: Performance profiling with perception enabled

**Priority: must-do** · Touches: runtime measurement, no code changes expected

- [ ] Enable perception: set `enable_perception=true` DP, restart
- [ ] Measure: total AI fps, per-frame latency breakdown (YOLO, association,
      crop extraction, embedding, scoring, KCF update)
- [ ] Add timing instrumentation in `perception_engine.cpp` (optional, use
      `std::chrono` around each stage in `process_yolo_frame` and
      `process_tracker_frame`)
- [ ] Measure memory usage: `top -bn1 -p $(pgrep soulcam)` and `smaps`
- [ ] Compare: perception-enabled vs legacy single-target mode
- [ ] Identify bottlenecks and optimize if needed

**Context files:**
- `src/ai/perception_engine.cpp` -- `process_yolo_frame()` and `process_tracker_frame()`
  are the main timing targets
- `src/pipeline/ai_capture.cpp` -- FPS logging (lines ~89-93: `fps_last_log`,
  `fps_total_frames`, `fps_yolo_frames`)
- `doc/framework/SOULCAM_FRAMEWORK.md` §Verification Results -- baseline performance
  numbers (RTSP+AI: ~11% CPU, ~22.7 AI fps)
- `doc/build/BUILD_AND_DEPLOY.md` §10 -- how to verify on device

---

### TODO-5: Tune interest weights

**Priority: must-do** · Touches: `src/ai/interest_scorer.h/.cpp`, `src/soulcam.h`

- [ ] Expose interest weight parameters as DPs for runtime tuning
- [ ] Run perception in target environment, observe which objects get attention
- [ ] Adjust weights in `InterestConfig`: novelty, motion, size, uncertainty,
      change, frequency decay, min_interest threshold
- [ ] Consider environment-specific presets (office, outdoor, retail, etc.)
- [ ] Optionally: add interest weight CLI flags or SoulLink sysCmd for remote tuning

**Context files:**
- `src/ai/interest_scorer.h` -- `InterestConfig` struct with all weight fields
- `src/ai/interest_scorer.cpp` -- `score()` formula implementation
- `src/soulcam.h` -- `PerceptionPipelineConfig` (currently only exposes
  `interest_novelty_halflife`, `interest_motion_weight`, `interest_threshold`)
- `src/main.cpp` -- CLI option handling for perception (cases 160-166)
- `soullink/docs/dp_catalog.md` -- existing DP structure for runtime config
- `src/store/store_config.h` -- DP enum definitions

---

### TODO-6: Cold-tier object storage

**Priority: nice-to-have** · Touches: `src/ai/object_memory.h/.cpp`

- [ ] When `objects_.size() > hot_tier_max`, demote least-recently-seen objects
      to cold tier (serialize to per-class JSON files on disk)
- [ ] On `match()`, if no hot-tier match found for a class, load cold-tier
      objects for that class on demand
- [ ] Add LRU eviction tracking to `ObjectRecord` (last_accessed timestamp)
- [ ] Test with >1000 objects to verify RAM stays bounded

**Context files:**
- `src/ai/object_memory.h` -- `MemoryConfig.hot_tier_max`, `cold_demote_days`
- `src/ai/object_memory.cpp` -- current in-memory `objects_` map

---

### TODO-7: Memory compaction (merge duplicates)

**Priority: nice-to-have** · Touches: `src/ai/object_memory.h/.cpp`

- [ ] After enrollment, check if any existing ObjectRecords of the same class
      have centroid similarity > `merge_threshold` (default 0.90)
- [ ] If so, merge: combine exemplars, average centroids, keep richer metadata
- [ ] This handles cases where the same object is enrolled twice from different
      viewing sessions before it could be recognized

**Context files:**
- `src/ai/object_memory.h` -- `MemoryConfig.merge_threshold`
- `src/ai/embedder.h` -- `cosine_similarity()` function

---

### TODO-8: Multi-modal VLM enrichment (image upload)

**Priority: nice-to-have** · Touches: `src/ai/vlm_client.cpp`

- [ ] Base64-encode crop RGB data (or convert to JPEG first)
- [ ] Build OpenAI vision API request with `image_url` content blocks
- [ ] Send actual object images to VLM for richer identification
- [ ] Currently the prompt is text-only which limits VLM's ability to identify
      specific object instances

**Context files:**
- `src/ai/vlm_client.cpp` -- `process_request()` body builder, currently text-only
- `src/ai/vlm_client.h` -- `VlmEnrichRequest.crop_jpegs` (already carries crop data)
- `src/ai/perception_engine.cpp` -- `try_enroll()` populates `req.crop_jpegs`

---

### TODO-9: SoulFlow object memory browser

**Priority: nice-to-have** · Touches: SoulFlow project

- [ ] Add a panel/drawer in SoulFlow that lists all known objects from memory
- [ ] Show: thumbnail crop, VLM name, class, seen count, last seen, interest
- [ ] Allow clicking an object to highlight it in the RTSP overlay
- [ ] Requires a new SoulLink message or query API to fetch memory contents

**Context files:**
- `/mnt/d/WebProjects/SoulFlow/src/renderer/src/components/debug/DebugRtspNode.tsx` --
  existing overlay and SoulLink message handling
- `src/soullink/module.h` -- where to add a new query message type
- `soullink/docs/protocol_mapping.md` -- SoulLink message format reference

---

### TODO-10: SoulFlow interest weight tuning

**Priority: nice-to-have** · Touches: SoulFlow project + SoulCam DPs

- [ ] Add slider controls in SoulFlow for each interest weight
- [ ] Wire to SoulLink `setDp` commands for real-time parameter adjustment
- [ ] Show live interest score distribution across tracked objects
- [ ] Requires new DPs for interest weights (see TODO-5)

**Context files:**
- `/mnt/d/WebProjects/SoulFlow/src/renderer/src/components/debug/DebugRtspNode.tsx`
- `src/ai/interest_scorer.h` -- weight fields to expose
- `soullink/docs/dp_catalog.md` -- DP system reference
- `src/store/store_config.h` -- where to define new DP IDs
