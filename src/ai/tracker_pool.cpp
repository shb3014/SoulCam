#include "ai/tracker_pool.h"
#include "util/logger.h"

#include <algorithm>
#include <set>

namespace sc {

TrackerPool::TrackerPool(int max_trackers, const InterframeTrackerConfig& cfg)
    : max_k_(max_trackers), cfg_(cfg) {
    slots_.resize(max_k_);
    for (auto& s : slots_) {
        s.tracker = new InterframeTracker(cfg_);
    }
}

TrackerPool::~TrackerPool() {
    for (auto& s : slots_) {
        delete s.tracker;
    }
}

void TrackerPool::set_config(const InterframeTrackerConfig& cfg) {
    cfg_ = cfg;
    for (auto& s : slots_) {
        s.tracker->set_config(cfg_);
    }
}

int TrackerPool::find_slot_for_track(uint32_t track_id) const {
    for (int i = 0; i < max_k_; i++) {
        if (slots_[i].active && slots_[i].assigned_track_id == track_id)
            return i;
    }
    return -1;
}

int TrackerPool::find_free_slot() const {
    for (int i = 0; i < max_k_; i++) {
        if (!slots_[i].active) return i;
    }
    return -1;
}

int TrackerPool::active_count() const {
    int n = 0;
    for (const auto& s : slots_) {
        if (s.active) n++;
    }
    return n;
}

void TrackerPool::assign_slots(std::vector<TrackSlot>& tracks,
                                const std::vector<uint32_t>& top_k_ids,
                                const uint8_t* gray, int img_w, int img_h,
                                float dt) {
    std::set<uint32_t> desired(top_k_ids.begin(), top_k_ids.end());

    // Release slots for tracks no longer in top-K
    for (auto& s : slots_) {
        if (s.active && desired.find(s.assigned_track_id) == desired.end()) {
            s.tracker->reset();
            s.active = false;

            for (auto& t : tracks) {
                if (t.track_id == s.assigned_track_id) {
                    t.is_actively_tracked = false;
                    t.kcf_slot_index = -1;
                    break;
                }
            }
            s.assigned_track_id = 0;
        }
    }

    // Assign slots to new top-K tracks
    for (uint32_t tid : top_k_ids) {
        if (find_slot_for_track(tid) >= 0) continue;  // already assigned

        int slot = find_free_slot();
        if (slot < 0) break;  // pool full

        TrackSlot* ts = nullptr;
        for (auto& t : tracks) {
            if (t.track_id == tid) { ts = &t; break; }
        }
        if (!ts) continue;

        // Initialize KCF tracker from current bbox
        Detection init_det{};
        init_det.cls_id = ts->coarse_class_id;
        init_det.label = ts->coarse_label.c_str();
        init_det.confidence = 1.0f;
        init_det.left   = static_cast<int>(ts->cx - ts->w * 0.5f);
        init_det.top    = static_cast<int>(ts->cy - ts->h * 0.5f);
        init_det.right  = static_cast<int>(ts->cx + ts->w * 0.5f);
        init_det.bottom = static_cast<int>(ts->cy + ts->h * 0.5f);

        slots_[slot].tracker->reinit(init_det, gray, img_w, img_h, dt);
        slots_[slot].assigned_track_id = tid;
        slots_[slot].active = true;

        ts->is_actively_tracked = true;
        ts->kcf_slot_index = slot;
    }
}

void TrackerPool::update_all(std::vector<TrackSlot>& tracks,
                              const uint8_t* gray, int img_w, int img_h,
                              float dt) {
    for (int i = 0; i < max_k_; i++) {
        auto& s = slots_[i];
        if (!s.active || !s.tracker->is_tracking()) continue;

        Detection tracked = s.tracker->update(gray, img_w, img_h, dt);

        for (auto& t : tracks) {
            if (t.track_id == s.assigned_track_id) {
                t.cx = (tracked.left + tracked.right) * 0.5f;
                t.cy = (tracked.top + tracked.bottom) * 0.5f;
                t.w  = static_cast<float>(tracked.right - tracked.left);
                t.h  = static_cast<float>(tracked.bottom - tracked.top);
                break;
            }
        }
    }
}

void TrackerPool::reinit_slot(int slot_idx, const Detection& det,
                               const uint8_t* gray, int img_w, int img_h,
                               float dt) {
    if (slot_idx < 0 || slot_idx >= max_k_) return;
    auto& s = slots_[slot_idx];
    if (s.active) {
        s.tracker->reinit(det, gray, img_w, img_h, dt);
    }
}

void TrackerPool::reset() {
    for (auto& s : slots_) {
        s.tracker->reset();
        s.active = false;
        s.assigned_track_id = 0;
    }
}

}  // namespace sc
