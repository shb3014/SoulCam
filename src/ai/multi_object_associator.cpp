#include "ai/multi_object_associator.h"
#include "util/logger.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace sc {

MultiObjectAssociator::MultiObjectAssociator(const AssociatorConfig& cfg)
    : cfg_(cfg) {
    tracks_.reserve(cfg_.max_tracks);
}

void MultiObjectAssociator::set_config(const AssociatorConfig& cfg) {
    cfg_ = cfg;
}

float MultiObjectAssociator::compute_iou(const TrackSlot& t, const Detection& d) const {
    float tl = t.cx - t.w * 0.5f;
    float tt = t.cy - t.h * 0.5f;
    float tr = t.cx + t.w * 0.5f;
    float tb = t.cy + t.h * 0.5f;

    float dl = static_cast<float>(d.left);
    float dt_top = static_cast<float>(d.top);
    float dr = static_cast<float>(d.right);
    float db = static_cast<float>(d.bottom);

    float inter_l = std::max(tl, dl);
    float inter_t = std::max(tt, dt_top);
    float inter_r = std::min(tr, dr);
    float inter_b = std::min(tb, db);

    if (inter_r <= inter_l || inter_b <= inter_t) return 0.0f;

    float inter_area = (inter_r - inter_l) * (inter_b - inter_t);
    float t_area = t.w * t.h;
    float d_area = (dr - dl) * (db - dt_top);
    float union_area = t_area + d_area - inter_area;

    return (union_area > 0.0f) ? inter_area / union_area : 0.0f;
}

void MultiObjectAssociator::update(const std::vector<Detection>& detections,
                                    uint64_t timestamp_ms) {
    const int num_dets = static_cast<int>(detections.size());
    const int num_tracks = static_cast<int>(tracks_.size());

    // Predict track positions forward using velocity
    for (auto& t : tracks_) {
        t.cx += t.vx;
        t.cy += t.vy;
    }

    // Compute IoU matrix and greedy assignment
    std::vector<bool> det_matched(num_dets, false);
    std::vector<bool> track_matched(num_tracks, false);

    struct Match { int track_idx; int det_idx; float iou; };
    std::vector<Match> candidates;
    candidates.reserve(num_dets * num_tracks);

    for (int ti = 0; ti < num_tracks; ti++) {
        for (int di = 0; di < num_dets; di++) {
            float iou = compute_iou(tracks_[ti], detections[di]);
            if (iou >= cfg_.iou_threshold) {
                candidates.push_back({ti, di, iou});
            }
        }
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Match& a, const Match& b) { return a.iou > b.iou; });

    for (const auto& m : candidates) {
        if (track_matched[m.track_idx] || det_matched[m.det_idx]) continue;

        auto& t = tracks_[m.track_idx];
        const auto& d = detections[m.det_idx];

        float new_cx = (d.left + d.right) * 0.5f;
        float new_cy = (d.top + d.bottom) * 0.5f;
        float new_w  = static_cast<float>(d.right - d.left);
        float new_h  = static_cast<float>(d.bottom - d.top);

        t.vx = new_cx - t.cx;
        t.vy = new_cy - t.cy;
        t.cx = new_cx;
        t.cy = new_cy;
        t.w  = new_w;
        t.h  = new_h;

        t.coarse_class_id = d.cls_id;
        if (d.label) t.coarse_label = d.label;

        t.last_seen_ts = timestamp_ms;
        t.age_frames++;
        t.miss_count = 0;

        if (t.memory_object_id == -1) {
            t.stable_frames++;
        }

        track_matched[m.track_idx] = true;
        det_matched[m.det_idx] = true;
    }

    // Create new tracks for unmatched detections
    for (int di = 0; di < num_dets; di++) {
        if (det_matched[di]) continue;
        if (static_cast<int>(tracks_.size()) >= cfg_.max_tracks) break;

        const auto& d = detections[di];
        TrackSlot t;
        t.track_id = next_track_id_++;
        t.coarse_class_id = d.cls_id;
        if (d.label) t.coarse_label = d.label;
        t.cx = (d.left + d.right) * 0.5f;
        t.cy = (d.top + d.bottom) * 0.5f;
        t.w  = static_cast<float>(d.right - d.left);
        t.h  = static_cast<float>(d.bottom - d.top);
        t.vx = 0;
        t.vy = 0;
        t.first_seen_ts = timestamp_ms;
        t.last_seen_ts  = timestamp_ms;
        t.age_frames = 1;
        t.miss_count = 0;
        t.needs_embedding = true;
        t.stable_frames = 1;
        tracks_.push_back(std::move(t));
    }

    // Age unmatched tracks and remove dead ones
    for (int ti = num_tracks - 1; ti >= 0; ti--) {
        if (!track_matched[ti]) {
            tracks_[ti].miss_count++;
        }
    }

    tracks_.erase(
        std::remove_if(tracks_.begin(), tracks_.end(),
            [this](const TrackSlot& t) { return t.miss_count > cfg_.miss_ttl; }),
        tracks_.end());
}

TrackSlot* MultiObjectAssociator::find_track(uint32_t track_id) {
    for (auto& t : tracks_) {
        if (t.track_id == track_id) return &t;
    }
    return nullptr;
}

void MultiObjectAssociator::reset() {
    tracks_.clear();
    next_track_id_ = 1;
}

}  // namespace sc
