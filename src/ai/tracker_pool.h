#pragma once
// ============================================================================
// KCF + Kalman Tracker Pool
//
// Manages K instances of InterframeTracker for simultaneous multi-object
// tracking. Track slots are assigned dynamically based on interest scores:
// the top-K most interesting objects get active KCF tracking between
// YOLO frames; the rest are only passively recognized.
// ============================================================================

#include "ai/interframe_tracker.h"
#include "ai/multi_object_associator.h"

#include <vector>

namespace sc {

class TrackerPool {
public:
    explicit TrackerPool(int max_trackers, const InterframeTrackerConfig& cfg = {});
    ~TrackerPool();

    TrackerPool(const TrackerPool&) = delete;
    TrackerPool& operator=(const TrackerPool&) = delete;

    void set_config(const InterframeTrackerConfig& cfg);

    // Assign tracker slots to the given track IDs (top-K by interest).
    // Releases slots from tracks not in the new set; initializes new ones.
    void assign_slots(std::vector<TrackSlot>& tracks,
                      const std::vector<uint32_t>& top_k_ids,
                      const uint8_t* gray, int img_w, int img_h,
                      float dt);

    // Update all active trackers with a new grayscale frame.
    // Writes updated positions back into the corresponding TrackSlots.
    void update_all(std::vector<TrackSlot>& tracks,
                    const uint8_t* gray, int img_w, int img_h,
                    float dt);

    // Reinit a specific tracker slot from a YOLO detection.
    void reinit_slot(int slot_idx, const Detection& det,
                     const uint8_t* gray, int img_w, int img_h,
                     float dt);

    int max_trackers() const { return max_k_; }
    int active_count() const;

    void reset();

private:
    struct Slot {
        InterframeTracker* tracker = nullptr;
        uint32_t assigned_track_id = 0;
        bool     active = false;
    };

    int find_slot_for_track(uint32_t track_id) const;
    int find_free_slot() const;

    int max_k_;
    InterframeTrackerConfig cfg_;
    std::vector<Slot> slots_;
};

}  // namespace sc
