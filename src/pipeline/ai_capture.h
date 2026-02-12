#pragma once
// ============================================================================
// AI capture pipeline -- selfpath -> multi-model RKNN inference
//
// Architecture:
//
//   v4l2src (/dev/video9, selfpath, NV12 640x480)
//     -> queue (decouple from ISP capture rate)
//     -> rgaconvert (HW: NV12 -> RGB + resize to model input)
//     -> appsink (pull frames into C++ for RKNN inference)
//
// Multi-model support:
//   Each frame from appsink is fed to a ModelPipeline which runs
//   multiple RKNN models sequentially on the NPU.  Each model can
//   have independent confidence thresholds and frame skip settings.
//
// Why selfpath for AI:
//   - ISP hardware does the downscale (no CPU/RGA cost)
//   - Independent from the stream path (mainpath)
//   - Both paths share the same ISP processing (AWB, AE, CCM, etc.)
//   - AI gets consistent, ISP-processed frames at its preferred resolution
// ============================================================================

#include "soulcam.h"
#include "ai/detector.h"

#include <functional>
#include <vector>

namespace sc {

// Callback invoked on each detection result (from all model slots).
using AiCallback = std::function<void(const std::vector<Detection>& detections,
                                       int frame_width, int frame_height)>;

struct AiCapture;

// Start the AI capture pipeline with multi-model support.
// The callback is invoked from the AI thread for every processed frame.
// Returns nullptr on failure.
AiCapture* ai_capture_start(const Config& cfg, AiCallback cb);

// Stop the AI capture pipeline and release resources.
void ai_capture_stop(AiCapture* cap);

// Hot-swap the RKNN model in a specific slot at runtime.
// slot_idx=0 is the primary model (backwards compatible with single-model).
// Returns 0 on success, -1 on failure (old model remains active).
int ai_capture_swap_model(AiCapture* cap, const RknnConfig& new_cfg,
                           int slot_idx = 0);

// Add a new model slot at runtime. Returns slot index or -1 on failure.
int ai_capture_add_model(AiCapture* cap, const ModelSlotConfig& cfg);

// Remove a model slot by index (slot 0 cannot be removed).
int ai_capture_remove_model(AiCapture* cap, int slot_idx);

// Enable or disable a model slot at runtime.
void ai_capture_enable_model(AiCapture* cap, int slot_idx, bool enable);

// Get the number of model slots.
int ai_capture_model_count(AiCapture* cap);

// Get info about a model slot.
ModelSlotConfig ai_capture_get_model_info(AiCapture* cap, int slot_idx);

}  // namespace sc
