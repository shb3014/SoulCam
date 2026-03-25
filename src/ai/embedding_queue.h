#pragma once
// ============================================================================
// Embedding queue -- thread-safe crop queue for interleaved NPU scheduling
//
// Crops are queued by the YOLO frame handler and processed on tracker frames
// when the NPU is idle. The queue is priority-ordered by interest score
// (higher interest = processed first).
// ============================================================================

#include "ai/embedder.h"
#include "ai/multi_object_associator.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace sc {

struct EmbedRequest {
    uint32_t track_id;
    std::vector<uint8_t> crop_rgb;
    int      crop_w;
    int      crop_h;
    float    priority;   // higher = processed first (interest score)
};

class EmbeddingQueue {
public:
    explicit EmbeddingQueue(Embedder* embedder);

    // Push a crop for embedding extraction (thread-safe).
    void push(uint32_t track_id,
              const uint8_t* crop_rgb, int crop_w, int crop_h,
              float priority = 0.0f);

    // Process one pending request (call on tracker frames).
    // If a request is processed, the result embedding is written into the
    // corresponding TrackSlot.
    // Returns true if a request was processed.
    bool process_one(std::vector<TrackSlot>& tracks);

    // Number of pending requests.
    int pending() const;

    void clear();

private:
    Embedder* embedder_;    // not owned
    mutable std::mutex mtx_;
    std::vector<EmbedRequest> queue_;
};

}  // namespace sc
