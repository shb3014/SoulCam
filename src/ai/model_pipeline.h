#pragma once
// ============================================================================
// Multi-model pipeline orchestrator
//
// Manages multiple RKNN model slots running on the same input frame.
// Each slot has its own Detector*, frame counter, and configuration.
//
// Design rationale:
//   - RK3566 NPU is single-core (0.8 TOPS), so models run sequentially
//     per frame. Multiple rknn_context instances are time-multiplexed.
//   - Frame skipping allows mixing a fast primary model (every frame)
//     with a slower high-accuracy model (every N frames).
//   - All models share the same RGB input from the GStreamer capture
//     pipeline (selfpath → RGA → appsink).
//
// Example configurations:
//   Slot 0: yolov8n (fast, every frame)     → general detection ~22 fps
//   Slot 1: yolov8s (accurate, skip=2)      → high-conf every 3rd frame
//   Slot 2: face_det (custom, skip=4)       → face detection every 5th frame
//
// All detections are tagged with model_id and merged into a single
// output vector per frame.
// ============================================================================

#include "soulcam.h"
#include <vector>

namespace sc {

struct ModelPipeline;

// Create a multi-model pipeline.
// The primary model (slot 0) is always created from primary_cfg.
// Additional slots come from extra_models.
// Returns nullptr only if the primary model fails to load.
ModelPipeline* model_pipeline_create(const RknnConfig& primary_cfg,
                                      const std::vector<ModelSlotConfig>& extra_models);

// Destroy the pipeline and release all NPU resources.
void model_pipeline_destroy(ModelPipeline* mp);

// Run all active models on a frame (sequentially on NPU).
// Detections from all models are appended to 'out', each tagged with model_id.
// Models with skip_frames > 0 only run on their scheduled frames.
// Returns 0 on success, -1 if pipeline is null.
int model_pipeline_infer(ModelPipeline* mp,
                          const uint8_t* data, size_t size,
                          int width, int height, int channels,
                          std::vector<Detection>& out);

// Get the primary model's input dimensions (for GStreamer pipeline sizing).
void model_pipeline_get_input_size(const ModelPipeline* mp,
                                    int& width, int& height, int& channels);

// ---------------------------------------------------------------------------
// Runtime model management (all thread-safe)
// ---------------------------------------------------------------------------

// Add a new model slot. Returns the slot index (>= 1), or -1 on failure.
int model_pipeline_add_model(ModelPipeline* mp, const ModelSlotConfig& cfg);

// Remove a model slot by index. Slot 0 (primary) cannot be removed.
// Returns 0 on success, -1 on failure.
int model_pipeline_remove_model(ModelPipeline* mp, int slot_idx);

// Swap the model in a specific slot. Returns 0 on success.
int model_pipeline_swap_model(ModelPipeline* mp, int slot_idx,
                               const RknnConfig& new_cfg);

// Enable or disable a model slot.
void model_pipeline_enable_model(ModelPipeline* mp, int slot_idx, bool enable);

// Get the number of model slots (including primary).
int model_pipeline_count(const ModelPipeline* mp);

// Get info about a model slot. Returns empty config if slot_idx is invalid.
ModelSlotConfig model_pipeline_get_slot_info(const ModelPipeline* mp, int slot_idx);

}  // namespace sc
