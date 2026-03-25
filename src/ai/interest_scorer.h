#pragma once
// ============================================================================
// Interest scorer -- determines which objects get attention
//
// Computes a real-time interest score for every detected object based on:
//   - Novelty (unrecognized or recently enrolled)
//   - Motion (moving objects draw attention)
//   - Size (larger objects are more salient)
//   - Uncertainty (low match confidence is interesting)
//   - Appearance change (object looks different from memory)
//   - Semantic interest (VLM-assigned base_interest)
//   - Frequency decay (familiar objects become boring)
//
// The interest score drives:
//   - Which K objects get active KCF tracking
//   - Embedding extraction priority
//   - VLM enrichment priority
// ============================================================================

#include "ai/multi_object_associator.h"
#include "ai/object_memory.h"

#include <vector>

namespace sc {

struct InterestConfig {
    float novelty_halflife_hours = 24.0f;
    float novelty_weight         = 0.4f;   // unrecognized bonus
    float novelty_decay_weight   = 0.2f;   // recognized but new
    float motion_weight          = 0.15f;
    float size_weight            = 0.10f;
    float uncertainty_weight     = 0.10f;
    float change_weight          = 0.10f;
    float frequency_decay        = 0.05f;
    float min_interest           = 0.10f;  // below this, don't even keep track alive
};

class InterestScorer {
public:
    explicit InterestScorer(const InterestConfig& cfg = {});

    void set_config(const InterestConfig& cfg);

    // Compute interest for a single track.
    float score(const TrackSlot& track,
                const ObjectRecord* memory,
                uint64_t now_ms) const;

    // Score all tracks and sort by interest. Returns top-K track IDs.
    std::vector<uint32_t> rank_and_select(
        std::vector<TrackSlot>& tracks,
        const ObjectMemory& memory,
        uint64_t now_ms,
        int top_k) const;

private:
    InterestConfig cfg_;
};

}  // namespace sc
