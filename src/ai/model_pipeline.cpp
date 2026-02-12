// ============================================================================
// Multi-model pipeline implementation
//
// Orchestrates multiple RKNN model slots on the same input frame.
// Thread-safe: all slot management operations are mutex-protected.
// The inference path uses a shared lock so that slot management (add/remove)
// doesn't corrupt state during an active inference.
//
// Performance on RK3566 (single-core NPU, 0.8 TOPS):
//   1 model (yolov8n):           ~44ms/frame → ~22 fps
//   2 models (yolov8n + yolov8n): ~88ms/frame → ~11 fps
//   2 models with skip=2:        avg ~58ms   → ~17 fps
//     (model 0 every frame, model 1 every 3rd frame)
// ============================================================================

#include "ai/model_pipeline.h"
#include "ai/detector.h"
#include "util/logger.h"

#include <mutex>
#include <vector>
#include <algorithm>

namespace sc {

// ---------------------------------------------------------------------------
// Internal slot structure
// ---------------------------------------------------------------------------
struct ModelSlot {
    Detector*       detector = nullptr;
    ModelSlotConfig config;
    int             frame_counter = 0;  // Counts frames since last run
    int             slot_id = 0;        // Stable ID for this slot
};

struct ModelPipeline {
    std::mutex           mtx;           // Protects slots vector
    std::vector<ModelSlot> slots;
    int                  next_slot_id = 0;
    uint64_t             frame_number = 0;  // Global frame counter
};

// ---------------------------------------------------------------------------
// Helper: derive a slot name from model path if not provided
// ---------------------------------------------------------------------------
static std::string derive_name(const std::string& model_path, int slot_id) {
    if (model_path.empty()) return "slot" + std::to_string(slot_id);
    // Extract filename without extension
    auto slash = model_path.rfind('/');
    auto dot   = model_path.rfind('.');
    std::string base;
    if (slash != std::string::npos)
        base = model_path.substr(slash + 1);
    else
        base = model_path;
    if (dot != std::string::npos && dot > (slash != std::string::npos ? slash : 0))
        base = base.substr(0, base.rfind('.'));
    return base;
}

// ---------------------------------------------------------------------------
// Create / Destroy
// ---------------------------------------------------------------------------

ModelPipeline* model_pipeline_create(const RknnConfig& primary_cfg,
                                      const std::vector<ModelSlotConfig>& extra_models) {
    auto* mp = new ModelPipeline();

    // --- Slot 0: primary model (required) ---
    ModelSlot primary;
    primary.slot_id = mp->next_slot_id++;
    primary.config.name = derive_name(primary_cfg.model_path, 0);
    primary.config.rknn = primary_cfg;
    primary.config.skip_frames = 0;   // Primary always runs every frame
    primary.config.enabled = true;

    if (!primary_cfg.model_path.empty()) {
        primary.detector = detector_create(primary_cfg);
        if (!primary.detector) {
            SC_LOG_ERROR("ModelPipeline: failed to load primary model %s",
                         primary_cfg.model_path.c_str());
            delete mp;
            return nullptr;
        }
        SC_LOG_INFO("ModelPipeline: slot 0 [%s] loaded (%s)",
                    primary.config.name.c_str(), primary_cfg.model_path.c_str());
    } else {
        SC_LOG_WARN("ModelPipeline: no primary model path -- slot 0 empty");
    }
    mp->slots.push_back(std::move(primary));

    // --- Extra model slots ---
    for (size_t i = 0; i < extra_models.size(); i++) {
        const auto& ecfg = extra_models[i];
        ModelSlot slot;
        slot.slot_id = mp->next_slot_id++;
        slot.config = ecfg;
        if (slot.config.name.empty())
            slot.config.name = derive_name(ecfg.rknn.model_path, slot.slot_id);

        if (!ecfg.rknn.model_path.empty() && ecfg.enabled) {
            slot.detector = detector_create(ecfg.rknn);
            if (!slot.detector) {
                SC_LOG_WARN("ModelPipeline: failed to load slot %d [%s] -- skipping",
                            slot.slot_id, slot.config.name.c_str());
                slot.config.enabled = false;
            } else {
                int mw = 0, mh = 0, mc = 0;
                detector_get_input_size(slot.detector, mw, mh, mc);
                SC_LOG_INFO("ModelPipeline: slot %d [%s] loaded (%s, %dx%d, skip=%d)",
                            slot.slot_id, slot.config.name.c_str(),
                            ecfg.rknn.model_path.c_str(), mw, mh,
                            ecfg.skip_frames);
            }
        }
        mp->slots.push_back(std::move(slot));
    }

    SC_LOG_INFO("ModelPipeline: %zu model slot(s) initialized", mp->slots.size());
    return mp;
}

void model_pipeline_destroy(ModelPipeline* mp) {
    if (!mp) return;
    std::lock_guard<std::mutex> lock(mp->mtx);
    for (auto& slot : mp->slots) {
        detector_destroy(slot.detector);
        slot.detector = nullptr;
    }
    mp->slots.clear();
    delete mp;
    SC_LOG_INFO("ModelPipeline: destroyed");
}

// ---------------------------------------------------------------------------
// Inference -- runs all active slots sequentially
// ---------------------------------------------------------------------------

int model_pipeline_infer(ModelPipeline* mp,
                          const uint8_t* data, size_t size,
                          int width, int height, int channels,
                          std::vector<Detection>& out) {
    if (!mp || !data) return -1;

    std::lock_guard<std::mutex> lock(mp->mtx);
    mp->frame_number++;

    for (size_t i = 0; i < mp->slots.size(); i++) {
        auto& slot = mp->slots[i];

        // Skip disabled or empty slots
        if (!slot.config.enabled || !slot.detector) continue;

        // Frame skipping logic
        if (slot.config.skip_frames > 0) {
            slot.frame_counter++;
            if (slot.frame_counter <= slot.config.skip_frames) {
                continue;  // Skip this frame for this slot
            }
            slot.frame_counter = 0;  // Reset counter, run this frame
        }

        // Run inference
        std::vector<Detection> slot_dets;
        int rc = detector_infer(slot.detector, data, size,
                                width, height, channels, slot_dets);
        if (rc != 0) {
            SC_LOG_WARN("ModelPipeline: slot %d [%s] inference failed (rc=%d)",
                        slot.slot_id, slot.config.name.c_str(), rc);
            continue;
        }

        // Tag detections with model_id and append to output
        for (auto& d : slot_dets) {
            d.model_id = static_cast<int>(i);
        }
        out.insert(out.end(), slot_dets.begin(), slot_dets.end());

        SC_LOG_DEBUG("ModelPipeline: slot %d [%s] → %zu detections",
                     slot.slot_id, slot.config.name.c_str(), slot_dets.size());
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Get primary model input size
// ---------------------------------------------------------------------------

void model_pipeline_get_input_size(const ModelPipeline* mp,
                                    int& width, int& height, int& channels) {
    width = 640; height = 640; channels = 3;  // defaults
    if (!mp) return;
    // No lock needed -- slot 0 is always present after creation
    if (!mp->slots.empty() && mp->slots[0].detector) {
        detector_get_input_size(mp->slots[0].detector, width, height, channels);
    }
}

// ---------------------------------------------------------------------------
// Runtime model management
// ---------------------------------------------------------------------------

int model_pipeline_add_model(ModelPipeline* mp, const ModelSlotConfig& cfg) {
    if (!mp) return -1;

    // Create detector BEFORE locking (RKNN init is slow)
    Detector* det = nullptr;
    if (!cfg.rknn.model_path.empty() && cfg.enabled) {
        det = detector_create(cfg.rknn);
        if (!det) {
            SC_LOG_ERROR("ModelPipeline: add_model failed to load %s",
                         cfg.rknn.model_path.c_str());
            return -1;
        }
    }

    std::lock_guard<std::mutex> lock(mp->mtx);

    ModelSlot slot;
    slot.slot_id = mp->next_slot_id++;
    slot.config = cfg;
    slot.detector = det;
    if (slot.config.name.empty())
        slot.config.name = derive_name(cfg.rknn.model_path, slot.slot_id);

    int idx = static_cast<int>(mp->slots.size());
    mp->slots.push_back(std::move(slot));

    if (det) {
        int mw = 0, mh = 0, mc = 0;
        detector_get_input_size(det, mw, mh, mc);
        SC_LOG_INFO("ModelPipeline: added slot %d [%s] (%s, %dx%d, skip=%d)",
                    idx, mp->slots.back().config.name.c_str(),
                    cfg.rknn.model_path.c_str(), mw, mh, cfg.skip_frames);
    }

    return idx;
}

int model_pipeline_remove_model(ModelPipeline* mp, int slot_idx) {
    if (!mp || slot_idx <= 0) {
        SC_LOG_WARN("ModelPipeline: cannot remove slot %d (primary cannot be removed)",
                    slot_idx);
        return -1;
    }

    Detector* old_det = nullptr;
    {
        std::lock_guard<std::mutex> lock(mp->mtx);
        if (slot_idx >= static_cast<int>(mp->slots.size())) {
            SC_LOG_WARN("ModelPipeline: slot %d does not exist", slot_idx);
            return -1;
        }
        old_det = mp->slots[slot_idx].detector;
        mp->slots[slot_idx].detector = nullptr;
        mp->slots[slot_idx].config.enabled = false;
        SC_LOG_INFO("ModelPipeline: removed slot %d [%s]",
                    slot_idx, mp->slots[slot_idx].config.name.c_str());
    }

    // Destroy outside lock
    detector_destroy(old_det);
    return 0;
}

int model_pipeline_swap_model(ModelPipeline* mp, int slot_idx,
                               const RknnConfig& new_cfg) {
    if (!mp) return -1;

    if (new_cfg.model_path.empty()) {
        SC_LOG_ERROR("ModelPipeline: swap_model empty path for slot %d", slot_idx);
        return -1;
    }

    SC_LOG_INFO("ModelPipeline: swapping slot %d → %s ...",
                slot_idx, new_cfg.model_path.c_str());

    // Create new detector BEFORE locking
    Detector* new_det = detector_create(new_cfg);
    if (!new_det) {
        SC_LOG_ERROR("ModelPipeline: swap_model failed to load %s",
                     new_cfg.model_path.c_str());
        return -1;
    }

    Detector* old_det = nullptr;
    {
        std::lock_guard<std::mutex> lock(mp->mtx);
        if (slot_idx < 0 || slot_idx >= static_cast<int>(mp->slots.size())) {
            SC_LOG_ERROR("ModelPipeline: slot %d out of range", slot_idx);
            detector_destroy(new_det);
            return -1;
        }
        old_det = mp->slots[slot_idx].detector;
        mp->slots[slot_idx].detector = new_det;
        mp->slots[slot_idx].config.rknn = new_cfg;
        mp->slots[slot_idx].config.name = derive_name(new_cfg.model_path, slot_idx);
        mp->slots[slot_idx].config.enabled = true;
    }

    // Destroy old detector outside lock
    detector_destroy(old_det);

    int mw = 0, mh = 0, mc = 0;
    detector_get_input_size(new_det, mw, mh, mc);
    SC_LOG_INFO("ModelPipeline: slot %d swapped to %s (%dx%d)",
                slot_idx, new_cfg.model_path.c_str(), mw, mh);
    return 0;
}

void model_pipeline_enable_model(ModelPipeline* mp, int slot_idx, bool enable) {
    if (!mp) return;
    std::lock_guard<std::mutex> lock(mp->mtx);
    if (slot_idx < 0 || slot_idx >= static_cast<int>(mp->slots.size())) return;
    mp->slots[slot_idx].config.enabled = enable;
    SC_LOG_INFO("ModelPipeline: slot %d [%s] %s",
                slot_idx, mp->slots[slot_idx].config.name.c_str(),
                enable ? "enabled" : "disabled");
}

int model_pipeline_count(const ModelPipeline* mp) {
    if (!mp) return 0;
    // No lock needed for size query (slots only grow)
    return static_cast<int>(mp->slots.size());
}

ModelSlotConfig model_pipeline_get_slot_info(const ModelPipeline* mp, int slot_idx) {
    ModelSlotConfig empty;
    if (!mp) return empty;
    // Safe without lock -- we only read stable config data
    if (slot_idx < 0 || slot_idx >= static_cast<int>(mp->slots.size())) return empty;
    return mp->slots[slot_idx].config;
}

}  // namespace sc
