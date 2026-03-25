#include "ai/embedding_queue.h"
#include "util/logger.h"

#include <algorithm>

namespace sc {

EmbeddingQueue::EmbeddingQueue(Embedder* embedder)
    : embedder_(embedder) {}

void EmbeddingQueue::push(uint32_t track_id,
                           const uint8_t* crop_rgb, int crop_w, int crop_h,
                           float priority) {
    std::lock_guard<std::mutex> lock(mtx_);

    // Replace existing request for same track (latest crop wins)
    for (auto& req : queue_) {
        if (req.track_id == track_id) {
            req.crop_rgb.assign(crop_rgb, crop_rgb + crop_w * crop_h * 3);
            req.crop_w = crop_w;
            req.crop_h = crop_h;
            req.priority = priority;
            return;
        }
    }

    EmbedRequest req;
    req.track_id = track_id;
    req.crop_rgb.assign(crop_rgb, crop_rgb + crop_w * crop_h * 3);
    req.crop_w = crop_w;
    req.crop_h = crop_h;
    req.priority = priority;
    queue_.push_back(std::move(req));
}

bool EmbeddingQueue::process_one(std::vector<TrackSlot>& tracks) {
    EmbedRequest req;

    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (queue_.empty()) return false;

        // Pick highest-priority request
        auto best = std::max_element(queue_.begin(), queue_.end(),
            [](const EmbedRequest& a, const EmbedRequest& b) {
                return a.priority < b.priority;
            });

        req = std::move(*best);
        queue_.erase(best);
    }

    // Run embedding inference (outside lock)
    std::vector<float> embedding;
    int rc = embedder_infer(embedder_,
                            req.crop_rgb.data(), req.crop_w, req.crop_h,
                            embedding);

    if (rc != 0) return false;

    // Write result into the track
    for (auto& t : tracks) {
        if (t.track_id == req.track_id) {
            t.embedding = std::move(embedding);
            t.needs_embedding = false;
            auto now = std::chrono::steady_clock::now();
            t.last_embed_ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()).count();
            break;
        }
    }

    return true;
}

int EmbeddingQueue::pending() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return static_cast<int>(queue_.size());
}

void EmbeddingQueue::clear() {
    std::lock_guard<std::mutex> lock(mtx_);
    queue_.clear();
}

}  // namespace sc
