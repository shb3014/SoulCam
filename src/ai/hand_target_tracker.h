#pragma once

#include "soulcam.h"

#include <string>
#include <vector>

namespace sc {

struct HandTargetTrackerConfig {
    // Prefer detections with this label (case-insensitive).
    // If no detection matches this label and fallback_to_all_labels is true,
    // all detections are considered.
    std::string preferred_label = "hand";
    bool fallback_to_all_labels = true;

    // Continue current target if IoU is high enough OR center is still nearby.
    float match_iou_threshold = 0.20f;
    float match_center_distance_factor = 0.80f;  // multiplied by sqrt(target_area)

    // Keep target alive for a few missed frames before reset.
    int lost_ttl_frames = 8;

    // Candidate can replace current target only when it's larger enough
    // for several consecutive frames (debounced switch).
    float switch_area_ratio = 1.25f;
    int switch_hold_frames = 6;
    int switch_cooldown_frames = 10;

    // Advanced feature (Phase 3): allow earlier switch when another hand
    // approaches fast (area grows quickly) and is close to current target size.
    bool enable_fast_approach_switch = false;
    float fast_approach_min_growth = 0.18f;     // per-frame relative growth
    float fast_approach_area_ratio = 0.90f;     // challenger >= ratio * current
    int fast_approach_hold_frames = 3;          // consecutive growth frames
};

class HandTargetTracker {
public:
    explicit HandTargetTracker(const HandTargetTrackerConfig& cfg = {});

    void reset();
    void set_config(const HandTargetTrackerConfig& cfg, bool reset_state = true);

    // Returns 0 or 1 detection (single target policy).
    std::vector<Detection> update(const std::vector<Detection>& detections);

private:
    struct State {
        bool has_target = false;
        Detection target{};
        float target_area = 0.0f;
        int lost_frames = 0;
        int cooldown_frames = 0;

        bool has_pending = false;
        Detection pending{};
        int pending_hold = 0;
        float pending_prev_area = 0.0f;
        int pending_fast_hold = 0;
    };

    int choose_initial_target(const std::vector<Detection>& dets,
                              const std::vector<int>& candidate_idx) const;
    int find_current_match(const std::vector<Detection>& dets,
                           const std::vector<int>& candidate_idx) const;
    int find_switch_candidate(const std::vector<Detection>& dets,
                              const std::vector<int>& candidate_idx,
                              int current_idx) const;
    std::vector<int> collect_candidates(const std::vector<Detection>& dets) const;

    bool same_detection(const Detection& a, const Detection& b) const;

    HandTargetTrackerConfig cfg_;
    State st_;
};

}  // namespace sc
