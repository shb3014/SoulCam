// ============================================================================
// Multi-model pipeline implementation
//
// Orchestrates multiple RKNN model slots on the same input frame.
// Thread-safe: all slot management operations are mutex-protected.
// The inference path uses a shared lock so that slot management (add/remove)
// doesn't corrupt state during an active inference.
//
// Supports two scheduling modes:
//   1) legacy run-all: all active slots run (with skip_frames gating)
//   2) weighted: weighted round-robin selects up to N slots/frame
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

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <algorithm>
#include <sstream>
#include <vector>
#include <unistd.h>

namespace sc {

// ---------------------------------------------------------------------------
// Internal slot structure
// ---------------------------------------------------------------------------
struct ModelSlot {
    Detector*       detector = nullptr;
    ModelSlotConfig config;
    int             frame_counter = 0;  // Counts frames since last run
    int             slot_id = 0;        // Stable ID for this slot

    // Runtime stats
    uint64_t        infer_count = 0;
    uint64_t        scheduled_count = 0;
    uint64_t        skipped_count = 0;
    double          infer_total_ms = 0.0;
    double          infer_last_ms = 0.0;
    double          infer_max_ms = 0.0;
    long            rss_delta_kb = -1;   // Estimated load-time RSS delta
    uint64_t        model_file_bytes = 0;
};

struct ModelPipeline {
    std::mutex           mtx;           // Protects slots vector
    std::vector<ModelSlot> slots;
    int                  next_slot_id = 0;
    uint64_t             frame_number = 0;  // Global frame counter
    ModelPipelineOptions opts;

    // Weighted scheduler state
    std::vector<int>     weighted_schedule;  // slot indices repeated by weight
    size_t               weighted_cursor = 0;
    bool                 schedule_dirty = true;
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

static int clamp_weight(int w) {
    if (w < 1) return 1;
    if (w > 100) return 100;
    return w;
}

static int clamp_max_models_per_frame(int n) {
    if (n < 1) return 1;
    if (n > 8) return 8;
    return n;
}

static long process_rss_kb() {
    std::ifstream f("/proc/self/statm");
    long pages_total = 0;
    long pages_rss = 0;
    if (!(f >> pages_total >> pages_rss)) return -1;
    long page_kb = static_cast<long>(sysconf(_SC_PAGESIZE) / 1024);
    if (page_kb <= 0) page_kb = 4;
    return pages_rss * page_kb;
}

static uint64_t model_file_size_bytes(const std::string& path) {
    std::error_code ec;
    auto sz = std::filesystem::file_size(path, ec);
    return ec ? 0ULL : static_cast<uint64_t>(sz);
}

static void rebuild_weighted_schedule_locked(ModelPipeline* mp) {
    mp->weighted_schedule.clear();
    mp->weighted_cursor = 0;

    if (!mp->opts.weighted_scheduler) {
        mp->schedule_dirty = false;
        return;
    }

    for (size_t i = 0; i < mp->slots.size(); ++i) {
        const auto& slot = mp->slots[i];
        if (!slot.config.enabled || !slot.detector) continue;
        const int w = clamp_weight(slot.config.run_weight);
        for (int k = 0; k < w; ++k) {
            mp->weighted_schedule.push_back(static_cast<int>(i));
        }
    }
    mp->schedule_dirty = false;
}

static int pick_weighted_slot_locked(ModelPipeline* mp, const std::vector<uint8_t>& used) {
    if (mp->weighted_schedule.empty()) return -1;
    const size_t tries = mp->weighted_schedule.size();
    for (size_t t = 0; t < tries; ++t) {
        int idx = mp->weighted_schedule[mp->weighted_cursor];
        mp->weighted_cursor = (mp->weighted_cursor + 1) % mp->weighted_schedule.size();
        if (idx >= 0 &&
            idx < static_cast<int>(mp->slots.size()) &&
            (idx >= static_cast<int>(used.size()) || used[idx] == 0) &&
            mp->slots[idx].config.enabled &&
            mp->slots[idx].detector) {
            return idx;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Create / Destroy
// ---------------------------------------------------------------------------

ModelPipeline* model_pipeline_create(const RknnConfig& primary_cfg,
                                      const std::vector<ModelSlotConfig>& extra_models,
                                      const ModelPipelineOptions& options) {
    auto* mp = new ModelPipeline();
    mp->opts = options;
    mp->opts.primary_model_weight = clamp_weight(mp->opts.primary_model_weight);
    mp->opts.max_models_per_frame = clamp_max_models_per_frame(mp->opts.max_models_per_frame);

    // --- Slot 0: primary model (required) ---
    ModelSlot primary;
    primary.slot_id = mp->next_slot_id++;
    primary.config.name = derive_name(primary_cfg.model_path, 0);
    primary.config.rknn = primary_cfg;
    primary.config.skip_frames = 0;   // Primary always runs every frame
    primary.config.run_weight = mp->opts.primary_model_weight;
    primary.config.enabled = true;

    if (!primary_cfg.model_path.empty()) {
        const long rss_before = process_rss_kb();
        primary.detector = detector_create(primary_cfg);
        if (!primary.detector) {
            SC_LOG_ERROR("ModelPipeline: failed to load primary model %s",
                         primary_cfg.model_path.c_str());
            delete mp;
            return nullptr;
        }
        const long rss_after = process_rss_kb();
        if (rss_before >= 0 && rss_after >= 0)
            primary.rss_delta_kb = std::max(0L, rss_after - rss_before);
        primary.model_file_bytes = model_file_size_bytes(primary_cfg.model_path);
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
            const long rss_before = process_rss_kb();
            slot.detector = detector_create(ecfg.rknn);
            if (!slot.detector) {
                SC_LOG_WARN("ModelPipeline: failed to load slot %d [%s] -- skipping",
                            slot.slot_id, slot.config.name.c_str());
                slot.config.enabled = false;
            } else {
                const long rss_after = process_rss_kb();
                if (rss_before >= 0 && rss_after >= 0)
                    slot.rss_delta_kb = std::max(0L, rss_after - rss_before);
                slot.model_file_bytes = model_file_size_bytes(ecfg.rknn.model_path);
                int mw = 0, mh = 0, mc = 0;
                detector_get_input_size(slot.detector, mw, mh, mc);
                SC_LOG_INFO("ModelPipeline: slot %d [%s] loaded (%s, %dx%d, skip=%d, weight=%d)",
                            slot.slot_id, slot.config.name.c_str(),
                            ecfg.rknn.model_path.c_str(), mw, mh,
                            ecfg.skip_frames, clamp_weight(slot.config.run_weight));
            }
        }
        mp->slots.push_back(std::move(slot));
    }

    rebuild_weighted_schedule_locked(mp);
    SC_LOG_INFO("ModelPipeline: %zu model slot(s) initialized", mp->slots.size());
    SC_LOG_INFO("ModelPipeline: scheduler=%s, max_models_per_frame=%d",
                mp->opts.weighted_scheduler ? "weighted" : "run-all",
                mp->opts.max_models_per_frame);
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

    auto run_slot = [&](int idx) {
        if (idx < 0 || idx >= static_cast<int>(mp->slots.size())) return;
        auto& slot = mp->slots[idx];
        if (!slot.config.enabled || !slot.detector) return;

        slot.scheduled_count++;
        const auto t0 = std::chrono::steady_clock::now();

        std::vector<Detection> slot_dets;
        int rc = detector_infer(slot.detector, data, size,
                                width, height, channels, slot_dets);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        slot.infer_last_ms = ms;
        slot.infer_total_ms += ms;
        slot.infer_max_ms = std::max(slot.infer_max_ms, ms);

        if (rc != 0) {
            SC_LOG_WARN("ModelPipeline: slot %d [%s] inference failed (rc=%d)",
                        slot.slot_id, slot.config.name.c_str(), rc);
            return;
        }

        slot.infer_count++;
        for (auto& d : slot_dets) {
            d.model_id = idx;
        }
        out.insert(out.end(), slot_dets.begin(), slot_dets.end());

        SC_LOG_DEBUG("ModelPipeline: slot %d [%s] -> %zu detections (%.2f ms)",
                     slot.slot_id, slot.config.name.c_str(), slot_dets.size(), ms);
    };

    if (mp->opts.weighted_scheduler) {
        if (mp->schedule_dirty) rebuild_weighted_schedule_locked(mp);
        int runs = clamp_max_models_per_frame(mp->opts.max_models_per_frame);
        std::vector<uint8_t> used(mp->slots.size(), 0);
        for (int n = 0; n < runs; ++n) {
            int idx = pick_weighted_slot_locked(mp, used);
            if (idx < 0) break;
            run_slot(idx);
            if (idx >= 0 && idx < static_cast<int>(used.size())) used[idx] = 1;
        }
        return 0;
    }

    // Legacy run-all scheduler (optionally gated by per-slot skip_frames).
    for (int i = 0; i < static_cast<int>(mp->slots.size()); ++i) {
        auto& slot = mp->slots[i];
        if (!slot.config.enabled || !slot.detector) continue;

        if (slot.config.skip_frames > 0) {
            slot.frame_counter++;
            if (slot.frame_counter <= slot.config.skip_frames) {
                slot.skipped_count++;
                continue;
            }
            slot.frame_counter = 0;
        }

        run_slot(i);
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
    long rss_delta_kb = -1;
    const long rss_before = process_rss_kb();
    if (!cfg.rknn.model_path.empty() && cfg.enabled) {
        det = detector_create(cfg.rknn);
        if (!det) {
            SC_LOG_ERROR("ModelPipeline: add_model failed to load %s",
                         cfg.rknn.model_path.c_str());
            return -1;
        }
        const long rss_after = process_rss_kb();
        if (rss_before >= 0 && rss_after >= 0)
            rss_delta_kb = std::max(0L, rss_after - rss_before);
    }

    std::lock_guard<std::mutex> lock(mp->mtx);

    ModelSlot slot;
    slot.slot_id = mp->next_slot_id++;
    slot.config = cfg;
    slot.config.run_weight = clamp_weight(slot.config.run_weight);
    slot.detector = det;
    slot.rss_delta_kb = rss_delta_kb;
    slot.model_file_bytes = model_file_size_bytes(cfg.rknn.model_path);
    if (slot.config.name.empty())
        slot.config.name = derive_name(cfg.rknn.model_path, slot.slot_id);

    int idx = static_cast<int>(mp->slots.size());
    mp->slots.push_back(std::move(slot));
    mp->schedule_dirty = true;

    if (det) {
        int mw = 0, mh = 0, mc = 0;
        detector_get_input_size(det, mw, mh, mc);
        SC_LOG_INFO("ModelPipeline: added slot %d [%s] (%s, %dx%d, skip=%d, weight=%d)",
                    idx, mp->slots.back().config.name.c_str(),
                    cfg.rknn.model_path.c_str(), mw, mh, cfg.skip_frames,
                    mp->slots.back().config.run_weight);
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
        mp->slots[slot_idx].rss_delta_kb = 0;
        mp->schedule_dirty = true;
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
    const long rss_before = process_rss_kb();
    Detector* new_det = detector_create(new_cfg);
    if (!new_det) {
        SC_LOG_ERROR("ModelPipeline: swap_model failed to load %s",
                     new_cfg.model_path.c_str());
        return -1;
    }
    const long rss_after = process_rss_kb();
    long rss_delta_kb = -1;
    if (rss_before >= 0 && rss_after >= 0)
        rss_delta_kb = std::max(0L, rss_after - rss_before);

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
        mp->slots[slot_idx].rss_delta_kb = rss_delta_kb;
        mp->slots[slot_idx].model_file_bytes = model_file_size_bytes(new_cfg.model_path);
        mp->schedule_dirty = true;
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
    mp->schedule_dirty = true;
    SC_LOG_INFO("ModelPipeline: slot %d [%s] %s",
                slot_idx, mp->slots[slot_idx].config.name.c_str(),
                enable ? "enabled" : "disabled");
}

int model_pipeline_set_slot_weight(ModelPipeline* mp, int slot_idx, int run_weight) {
    if (!mp) return -1;
    std::lock_guard<std::mutex> lock(mp->mtx);
    if (slot_idx < 0 || slot_idx >= static_cast<int>(mp->slots.size())) return -1;
    mp->slots[slot_idx].config.run_weight = clamp_weight(run_weight);
    mp->schedule_dirty = true;
    SC_LOG_INFO("ModelPipeline: slot %d [%s] weight=%d",
                slot_idx, mp->slots[slot_idx].config.name.c_str(),
                mp->slots[slot_idx].config.run_weight);
    return 0;
}

int model_pipeline_count(const ModelPipeline* mp) {
    if (!mp) return 0;
    std::lock_guard<std::mutex> lock(const_cast<ModelPipeline*>(mp)->mtx);
    return static_cast<int>(mp->slots.size());
}

ModelSlotConfig model_pipeline_get_slot_info(const ModelPipeline* mp, int slot_idx) {
    ModelSlotConfig empty;
    if (!mp) return empty;
    std::lock_guard<std::mutex> lock(const_cast<ModelPipeline*>(mp)->mtx);
    if (slot_idx < 0 || slot_idx >= static_cast<int>(mp->slots.size())) return empty;
    return mp->slots[slot_idx].config;
}

std::string model_pipeline_debug_status(const ModelPipeline* mp) {
    if (!mp) return "ModelPipeline: null";

    auto* mut = const_cast<ModelPipeline*>(mp);
    std::lock_guard<std::mutex> lock(mut->mtx);

    std::ostringstream oss;
    const long rss_kb = process_rss_kb();
    uint64_t total_infer = 0;
    long total_model_overhead_kb = 0;
    int loaded = 0;
    int enabled = 0;

    for (const auto& s : mp->slots) {
        if (s.config.enabled) enabled++;
        if (s.detector) {
            loaded++;
            if (s.rss_delta_kb > 0) total_model_overhead_kb += s.rss_delta_kb;
        }
        total_infer += s.infer_count;
    }

    oss << "ModelPipeline Debug\n"
        << "  scheduler: " << (mp->opts.weighted_scheduler ? "weighted" : "run-all")
        << ", max_models_per_frame=" << mp->opts.max_models_per_frame << "\n"
        << "  frames=" << mp->frame_number
        << ", slots=" << mp->slots.size()
        << ", enabled=" << enabled
        << ", loaded=" << loaded << "\n"
        << "  process_rss_kb=" << rss_kb
        << ", est_model_overhead_kb=" << total_model_overhead_kb
        << ", total_infer_calls=" << total_infer << "\n";

    for (size_t i = 0; i < mp->slots.size(); ++i) {
        const auto& s = mp->slots[i];
        const double avg_ms = (s.infer_count > 0)
                                ? (s.infer_total_ms / static_cast<double>(s.infer_count))
                                : 0.0;
        const double run_share = (mp->frame_number > 0)
                                ? (100.0 * static_cast<double>(s.scheduled_count) /
                                   static_cast<double>(mp->frame_number))
                                : 0.0;
        const double model_mb = static_cast<double>(s.model_file_bytes) / (1024.0 * 1024.0);

        oss << "  slot " << i
            << " [" << s.config.name << "]"
            << " enabled=" << (s.config.enabled ? "yes" : "no")
            << " loaded=" << (s.detector ? "yes" : "no")
            << " weight=" << clamp_weight(s.config.run_weight)
            << " skip=" << s.config.skip_frames
            << " model_mb=" << std::fixed << std::setprecision(2) << model_mb
            << " est_overhead_kb=" << s.rss_delta_kb
            << " scheduled=" << s.scheduled_count
            << " skipped=" << s.skipped_count
            << " run_share=" << std::setprecision(1) << run_share << "%"
            << " infer=" << s.infer_count
            << " avg_ms=" << std::setprecision(2) << avg_ms
            << " last_ms=" << s.infer_last_ms
            << " max_ms=" << s.infer_max_ms
            << "\n";
    }

    return oss.str();
}

}  // namespace sc
