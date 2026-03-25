#pragma once
// ============================================================================
// Multi-object detection association
//
// Associates YOLO detections to persistent track slots across frames using
// IoU matching. Each TrackSlot carries spatial, identity, and interest state.
//
// This replaces the single-target HandTargetTracker paradigm for the
// perception pipeline, allowing all detected objects to be tracked and
// recognized independently.
// ============================================================================

#include "soulcam.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sc {

struct ScoredCrop {
    std::vector<uint8_t> rgb_data;
    int    width  = 0;
    int    height = 0;
    float  quality_score = 0.0f;
    uint64_t timestamp = 0;
};

struct TrackSlot {
    uint32_t track_id = 0;
    int      coarse_class_id = -1;
    std::string coarse_label;

    float cx = 0, cy = 0, w = 0, h = 0;
    float vx = 0, vy = 0;

    uint64_t first_seen_ts = 0;
    uint64_t last_seen_ts  = 0;
    int      age_frames    = 0;
    int      miss_count    = 0;

    int   memory_object_id      = -1;
    float last_match_similarity = 0.0f;
    std::vector<float> embedding;

    float interest_score = 0.0f;

    static constexpr int kMaxCrops = 5;
    ScoredCrop crops[kMaxCrops];
    int  crop_count  = 0;
    int  crop_cursor = 0;

    bool is_actively_tracked = false;
    int  kcf_slot_index      = -1;

    bool needs_embedding     = true;
    uint64_t last_embed_ts   = 0;

    enum class Status { Active, Enrolling, Enrolled };
    Status enrollment_status = Status::Active;
    int    stable_frames     = 0;
};

struct AssociatorConfig {
    int   max_tracks     = 64;
    int   miss_ttl       = 10;
    float iou_threshold  = 0.20f;
    int   enrollment_delay_frames = 5;
};

class MultiObjectAssociator {
public:
    explicit MultiObjectAssociator(const AssociatorConfig& cfg = {});

    void set_config(const AssociatorConfig& cfg);

    // Associate new YOLO detections with existing tracks.
    // Returns the updated track table (all active tracks).
    // timestamp_ms: current time in milliseconds since epoch.
    void update(const std::vector<Detection>& detections,
                uint64_t timestamp_ms);

    const std::vector<TrackSlot>& tracks() const { return tracks_; }
    std::vector<TrackSlot>& tracks_mut() { return tracks_; }

    TrackSlot* find_track(uint32_t track_id);

    void reset();

private:
    float compute_iou(const TrackSlot& t, const Detection& d) const;

    AssociatorConfig cfg_;
    std::vector<TrackSlot> tracks_;
    uint32_t next_track_id_ = 1;
};

}  // namespace sc
