#pragma once
// ============================================================================
// Crop extractor -- extract and quality-score object crops from RGB frames
//
// For each YOLO detection, extracts the bounding box region with margin,
// scores it for quality (size, truncation, blur), and stores it in the
// track's crop ring buffer.
// ============================================================================

#include "ai/multi_object_associator.h"
#include "soulcam.h"

#include <vector>

namespace sc {

struct CropExtractorConfig {
    int   min_crop_size   = 48;
    float margin_ratio    = 0.10f;
    float max_edge_ratio  = 0.50f;   // reject if >50% of area is outside frame
};

class CropExtractor {
public:
    explicit CropExtractor(const CropExtractorConfig& cfg = {});

    // Extract crops for all detections and store in corresponding tracks.
    void extract(const uint8_t* rgb, int img_w, int img_h,
                 const std::vector<Detection>& detections,
                 std::vector<TrackSlot>& tracks,
                 uint64_t timestamp_ms);

private:
    float score_quality(const uint8_t* crop_rgb, int cw, int ch,
                        int img_w, int img_h,
                        int left, int top, int right, int bottom) const;

    CropExtractorConfig cfg_;
};

}  // namespace sc
