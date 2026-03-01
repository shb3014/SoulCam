#include "ai/hand_target_tracker.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <string_view>

namespace sc {

namespace {

static float box_w(const Detection& d) {
    return static_cast<float>(std::max(0, d.right - d.left));
}

static float box_h(const Detection& d) {
    return static_cast<float>(std::max(0, d.bottom - d.top));
}

static float box_area(const Detection& d) {
    return box_w(d) * box_h(d);
}

static float box_iou(const Detection& a, const Detection& b) {
    const float x1 = static_cast<float>(std::max(a.left, b.left));
    const float y1 = static_cast<float>(std::max(a.top, b.top));
    const float x2 = static_cast<float>(std::min(a.right, b.right));
    const float y2 = static_cast<float>(std::min(a.bottom, b.bottom));
    const float iw = std::max(0.0f, x2 - x1);
    const float ih = std::max(0.0f, y2 - y1);
    const float inter = iw * ih;
    const float ua = box_area(a) + box_area(b) - inter;
    return (ua > 1e-6f) ? (inter / ua) : 0.0f;
}

static float center_distance(const Detection& a, const Detection& b) {
    const float ax = 0.5f * static_cast<float>(a.left + a.right);
    const float ay = 0.5f * static_cast<float>(a.top + a.bottom);
    const float bx = 0.5f * static_cast<float>(b.left + b.right);
    const float by = 0.5f * static_cast<float>(b.top + b.bottom);
    const float dx = ax - bx;
    const float dy = ay - by;
    return std::sqrt(dx * dx + dy * dy);
}

static bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

HandTargetTracker::HandTargetTracker(const HandTargetTrackerConfig& cfg) : cfg_(cfg) {}

void HandTargetTracker::reset() {
    st_ = State{};
}

void HandTargetTracker::set_config(const HandTargetTrackerConfig& cfg, bool reset_state) {
    cfg_ = cfg;
    if (reset_state) reset();
}

std::vector<int> HandTargetTracker::collect_candidates(const std::vector<Detection>& dets) const {
    std::vector<int> preferred;
    std::vector<int> all;
    preferred.reserve(dets.size());
    all.reserve(dets.size());

    for (size_t i = 0; i < dets.size(); ++i) {
        const auto& d = dets[i];
        if (box_area(d) <= 1.0f) continue;
        all.push_back(static_cast<int>(i));
        if (d.label && iequal(d.label, cfg_.preferred_label)) {
            preferred.push_back(static_cast<int>(i));
        }
    }

    if (!preferred.empty()) return preferred;
    if (cfg_.fallback_to_all_labels) return all;
    return {};
}

int HandTargetTracker::choose_initial_target(const std::vector<Detection>& dets,
                                             const std::vector<int>& candidate_idx) const {
    int best_idx = -1;
    float best_score = -std::numeric_limits<float>::infinity();
    for (int idx : candidate_idx) {
        const float area = box_area(dets[idx]);
        const float score = area + 0.15f * dets[idx].confidence * area;
        if (score > best_score) {
            best_score = score;
            best_idx = idx;
        }
    }
    return best_idx;
}

int HandTargetTracker::find_current_match(const std::vector<Detection>& dets,
                                          const std::vector<int>& candidate_idx) const {
    int best_idx = -1;
    float best_score = -std::numeric_limits<float>::infinity();

    const float target_scale = std::sqrt(std::max(st_.target_area, 1.0f));
    const float max_center_dist = cfg_.match_center_distance_factor * target_scale;

    for (int idx : candidate_idx) {
        const auto& cand = dets[idx];
        const float iou = box_iou(st_.target, cand);
        const float dist = center_distance(st_.target, cand);
        const bool pass = (iou >= cfg_.match_iou_threshold) || (dist <= max_center_dist);
        if (!pass) continue;

        const float dist_norm = dist / (max_center_dist + 1e-6f);
        const float score = 2.0f * iou - 0.5f * dist_norm + 0.25f * cand.confidence;
        if (score > best_score) {
            best_score = score;
            best_idx = idx;
        }
    }

    return best_idx;
}

int HandTargetTracker::find_switch_candidate(const std::vector<Detection>& dets,
                                             const std::vector<int>& candidate_idx,
                                             int current_idx) const {
    int best_idx = -1;
    float best_area = 0.0f;
    for (int idx : candidate_idx) {
        if (idx == current_idx) continue;
        const float area = box_area(dets[idx]);
        if (area > best_area) {
            best_area = area;
            best_idx = idx;
        }
    }
    return best_idx;
}

bool HandTargetTracker::same_detection(const Detection& a, const Detection& b) const {
    if (box_iou(a, b) >= 0.30f) return true;
    const float scale = std::sqrt(std::max(box_area(a), 1.0f));
    return center_distance(a, b) <= (0.6f * scale);
}

std::vector<Detection> HandTargetTracker::update(const std::vector<Detection>& detections) {
    std::vector<Detection> out;
    const std::vector<int> candidates = collect_candidates(detections);

    if (!st_.has_target) {
        const int idx = choose_initial_target(detections, candidates);
        if (idx < 0) return out;

        st_.has_target = true;
        st_.target = detections[idx];
        st_.target_area = box_area(st_.target);
        st_.lost_frames = 0;
        st_.cooldown_frames = 0;
        st_.has_pending = false;
        st_.pending_hold = 0;
        st_.pending_prev_area = 0.0f;
        st_.pending_fast_hold = 0;
        out.push_back(st_.target);
        return out;
    }

    const int current_idx = find_current_match(detections, candidates);
    if (current_idx < 0) {
        st_.lost_frames++;
        st_.has_pending = false;
        st_.pending_hold = 0;
        st_.pending_prev_area = 0.0f;
        st_.pending_fast_hold = 0;
        if (st_.lost_frames > cfg_.lost_ttl_frames) {
            reset();
            return out;
        }
        // Keep outputting the last target for short temporary misses.
        out.push_back(st_.target);
        return out;
    }

    st_.target = detections[current_idx];
    st_.target_area = box_area(st_.target);
    st_.lost_frames = 0;
    if (st_.cooldown_frames > 0) {
        st_.cooldown_frames--;
        st_.has_pending = false;
        st_.pending_hold = 0;
        st_.pending_prev_area = 0.0f;
        st_.pending_fast_hold = 0;
        out.push_back(st_.target);
        return out;
    }

    const int switch_idx = find_switch_candidate(detections, candidates, current_idx);
    if (switch_idx >= 0) {
        const auto& challenger = detections[switch_idx];
        const float challenger_area = box_area(challenger);
        const bool bigger_enough = challenger_area > (st_.target_area * cfg_.switch_area_ratio);
        const bool near_enough = challenger_area >= (st_.target_area * cfg_.fast_approach_area_ratio);

        if (!bigger_enough && !cfg_.enable_fast_approach_switch) {
            st_.has_pending = false;
            st_.pending_hold = 0;
            st_.pending_prev_area = 0.0f;
            st_.pending_fast_hold = 0;
            out.push_back(st_.target);
            return out;
        }

        const bool same_pending = st_.has_pending && same_detection(st_.pending, challenger);
        float growth = 0.0f;
        if (same_pending && st_.pending_prev_area > 1.0f) {
            growth = (challenger_area - st_.pending_prev_area) / st_.pending_prev_area;
        }

        if (same_pending) {
            if (bigger_enough) {
                st_.pending_hold++;
            } else {
                st_.pending_hold = 0;
            }

            if (cfg_.enable_fast_approach_switch && near_enough &&
                growth >= cfg_.fast_approach_min_growth) {
                st_.pending_fast_hold++;
            } else {
                st_.pending_fast_hold = 0;
            }
        } else {
            st_.has_pending = true;
            st_.pending_hold = bigger_enough ? 1 : 0;
            st_.pending_fast_hold = 0;
        }

        st_.pending = challenger;
        st_.pending_prev_area = challenger_area;

        const bool ready_normal = st_.pending_hold >= cfg_.switch_hold_frames;
        const bool ready_fast = cfg_.enable_fast_approach_switch &&
                                st_.pending_fast_hold >= cfg_.fast_approach_hold_frames;
        if (ready_normal || ready_fast) {
            st_.target = challenger;
            st_.target_area = challenger_area;
            st_.cooldown_frames = cfg_.switch_cooldown_frames;
            st_.has_pending = false;
            st_.pending_hold = 0;
            st_.pending_prev_area = 0.0f;
            st_.pending_fast_hold = 0;
        }
    } else {
        st_.has_pending = false;
        st_.pending_hold = 0;
        st_.pending_prev_area = 0.0f;
        st_.pending_fast_hold = 0;
    }

    out.push_back(st_.target);
    return out;
}

}  // namespace sc
