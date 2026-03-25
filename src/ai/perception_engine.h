#pragma once
// ============================================================================
// Perception Engine -- cascading pipeline orchestrator
//
// Ties together all perception pipeline components:
//   Phase 0: Multi-object association + crop extraction
//   Phase 1: Embedding extraction (interleaved on tracker frames)
//   Phase 2: Object memory bank (unbounded recognition)
//   Phase 3: Interest scoring (attention allocation)
//   Phase 4: VLM enrichment (async cloud)
//
// Called from ai_capture.cpp on each frame. On YOLO frames, runs the full
// cascade (detect -> associate -> recognize -> score -> assign tracking).
// On tracker frames, updates KCF trackers and processes embedding queue.
//
// Output: PerceptionFrame containing all detected/tracked objects with
// identity, interest, and semantic metadata.
// ============================================================================

#include "ai/crop_extractor.h"
#include "ai/embedder.h"
#include "ai/embedding_queue.h"
#include "ai/interest_scorer.h"
#include "ai/multi_object_associator.h"
#include "ai/object_memory.h"
#include "ai/tracker_pool.h"
#include "ai/vlm_client.h"
#include "soulcam.h"

#include <cstdint>
#include <string>
#include <vector>

namespace sc {

// Output representation for a single perceived object
struct PerceivedObject {
    uint32_t track_id;
    int      cls_id;
    std::string cls_label;
    float    confidence;
    int      left, top, right, bottom;

    // Identity
    int      object_id = -1;        // -1 if unrecognized
    std::string object_name;        // VLM name or "class #id"
    float    match_confidence = 0;

    // Attention
    float    interest = 0.0f;
    bool     actively_tracked = false;
};

struct PerceptionFrame {
    std::vector<PerceivedObject> objects;
    int frame_width  = 0;
    int frame_height = 0;
    int total_memory_objects = 0;
    int active_trackers = 0;
};

struct PerceptionConfig {
    // Top-K active tracking slots
    int max_tracked_objects = 5;

    // Embedding model
    EmbedderConfig embedder;

    // Memory
    MemoryConfig memory;

    // Interest
    InterestConfig interest;

    // Association
    AssociatorConfig associator;

    // Crop extraction
    CropExtractorConfig crop;

    // VLM
    VlmConfig vlm;

    // Interframe tracker (reused from existing config)
    InterframeTrackerConfig tracker;

    // Enable the perception pipeline (false = legacy single-target mode)
    bool enabled = false;
};

using PerceptionCallback = std::function<void(const PerceptionFrame&)>;

class PerceptionEngine {
public:
    explicit PerceptionEngine(const PerceptionConfig& cfg);
    ~PerceptionEngine();

    PerceptionEngine(const PerceptionEngine&) = delete;
    PerceptionEngine& operator=(const PerceptionEngine&) = delete;

    // Process a YOLO frame: detect -> associate -> recognize -> score -> assign.
    // rgb: raw RGB frame (640x480)
    // gray: grayscale frame (640x480, pre-computed by ai_capture)
    // detections: YOLO output (already un-letterboxed to camera coords)
    PerceptionFrame process_yolo_frame(
        const uint8_t* rgb, const uint8_t* gray,
        int img_w, int img_h,
        const std::vector<Detection>& detections,
        float dt);

    // Process a tracker frame: KCF update + embedding extraction.
    PerceptionFrame process_tracker_frame(
        const uint8_t* gray,
        int img_w, int img_h,
        float dt);

    // Get current track table (for external consumers).
    const std::vector<TrackSlot>& tracks() const;

    // Get the memory bank.
    const ObjectMemory& memory() const { return *memory_; }

    // Save memory to disk.
    void save_memory();

    // Load memory from disk.
    void load_memory();

    void set_perception_callback(PerceptionCallback cb);

private:
    PerceptionFrame build_output(int img_w, int img_h);
    void try_enroll(TrackSlot& track, uint64_t now_ms);
    void queue_embedding_for_track(TrackSlot& track, const uint8_t* rgb,
                                    int img_w, int img_h);

    PerceptionConfig cfg_;

    MultiObjectAssociator* associator_ = nullptr;
    TrackerPool*           tracker_pool_ = nullptr;
    CropExtractor*         crop_extractor_ = nullptr;
    Embedder*              embedder_ = nullptr;
    EmbeddingQueue*        embed_queue_ = nullptr;
    ObjectMemory*          memory_ = nullptr;
    InterestScorer*        interest_ = nullptr;
    VlmClient*             vlm_ = nullptr;

    PerceptionCallback     callback_;
    uint64_t               now_ms_ = 0;
};

}  // namespace sc
