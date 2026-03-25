#include "ai/interest_scorer.h"
#include "ai/embedder.h"

#include <algorithm>
#include <cmath>

namespace sc {

InterestScorer::InterestScorer(const InterestConfig& cfg)
    : cfg_(cfg) {}

void InterestScorer::set_config(const InterestConfig& cfg) {
    cfg_ = cfg;
}

float InterestScorer::score(const TrackSlot& track,
                             const ObjectRecord* memory,
                             uint64_t now_ms) const {
    float s = 0.0f;

    // Novelty
    if (!memory) {
        s += cfg_.novelty_weight;
    } else {
        float age_hours = static_cast<float>(now_ms - memory->first_seen_ts) / 3600000.0f;
        s += cfg_.novelty_decay_weight * std::exp(-age_hours / cfg_.novelty_halflife_hours);
    }

    // Motion
    float speed = std::sqrt(track.vx * track.vx + track.vy * track.vy);
    s += cfg_.motion_weight * std::min(1.0f, speed / 30.0f);

    // Size saliency
    float area_ratio = (track.w * track.h) / (640.0f * 480.0f);
    s += cfg_.size_weight * std::min(1.0f, area_ratio * 10.0f);

    // Uncertainty
    if (memory && track.last_match_similarity < 0.85f) {
        s += cfg_.uncertainty_weight * (1.0f - track.last_match_similarity);
    }

    // Appearance change
    if (memory && !track.embedding.empty() && !memory->centroid.empty()) {
        float delta = 1.0f - cosine_similarity(track.embedding, memory->centroid);
        s += cfg_.change_weight * delta;
    }

    // Semantic boost from VLM
    if (memory) {
        s += memory->base_interest;
    }

    // Frequency decay
    if (memory) {
        s -= cfg_.frequency_decay * std::log(1.0f + memory->seen_count / 100.0f);
    }

    return std::max(0.0f, std::min(1.0f, s));
}

std::vector<uint32_t> InterestScorer::rank_and_select(
    std::vector<TrackSlot>& tracks,
    const ObjectMemory& memory,
    uint64_t now_ms,
    int top_k) const {

    // Score all tracks
    for (auto& t : tracks) {
        const ObjectRecord* mem = nullptr;
        if (t.memory_object_id >= 0) {
            mem = memory.get(static_cast<uint32_t>(t.memory_object_id));
        }
        t.interest_score = score(t, mem, now_ms);
    }

    // Collect eligible tracks (above minimum interest)
    struct Ranked { uint32_t id; float interest; };
    std::vector<Ranked> eligible;
    eligible.reserve(tracks.size());
    for (const auto& t : tracks) {
        if (t.interest_score >= cfg_.min_interest && t.miss_count == 0) {
            eligible.push_back({t.track_id, t.interest_score});
        }
    }

    // Sort by interest descending
    std::sort(eligible.begin(), eligible.end(),
              [](const Ranked& a, const Ranked& b) {
                  return a.interest > b.interest;
              });

    // Select top-K
    std::vector<uint32_t> top_k_ids;
    int k = std::min(top_k, static_cast<int>(eligible.size()));
    top_k_ids.reserve(k);
    for (int i = 0; i < k; i++) {
        top_k_ids.push_back(eligible[i].id);
    }

    return top_k_ids;
}

}  // namespace sc
