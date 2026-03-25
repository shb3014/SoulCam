#include "ai/crop_extractor.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace sc {

CropExtractor::CropExtractor(const CropExtractorConfig& cfg)
    : cfg_(cfg) {}

float CropExtractor::score_quality(const uint8_t* crop_rgb, int cw, int ch,
                                    int img_w, int img_h,
                                    int left, int top, int right, int bottom) const {
    float score = 1.0f;

    // Size penalty: smaller crops get lower quality
    float area = static_cast<float>(cw * ch);
    float size_factor = std::min(1.0f, area / (128.0f * 128.0f));
    score *= (0.3f + 0.7f * size_factor);

    // Edge truncation penalty
    int clipped_left   = std::max(0, -left);
    int clipped_top    = std::max(0, -top);
    int clipped_right  = std::max(0, right - img_w);
    int clipped_bottom = std::max(0, bottom - img_h);
    float total_clip = static_cast<float>(clipped_left + clipped_right +
                                          clipped_top + clipped_bottom);
    float perimeter = static_cast<float>(2 * (cw + ch));
    if (perimeter > 0) {
        float clip_ratio = total_clip / perimeter;
        score *= (1.0f - clip_ratio);
    }

    // Variance of grayscale (blur proxy) -- simple and cheap
    if (crop_rgb && cw > 0 && ch > 0) {
        int npix = cw * ch;
        int step = std::max(1, npix / 256);  // subsample for speed
        float sum = 0, sum2 = 0;
        int count = 0;
        for (int i = 0; i < npix; i += step) {
            float g = 0.299f * crop_rgb[i * 3] + 0.587f * crop_rgb[i * 3 + 1] +
                      0.114f * crop_rgb[i * 3 + 2];
            sum += g;
            sum2 += g * g;
            count++;
        }
        if (count > 1) {
            float mean = sum / count;
            float var = sum2 / count - mean * mean;
            float sharpness = std::min(1.0f, var / 1000.0f);
            score *= (0.3f + 0.7f * sharpness);
        }
    }

    return score;
}

void CropExtractor::extract(const uint8_t* rgb, int img_w, int img_h,
                             const std::vector<Detection>& detections,
                             std::vector<TrackSlot>& tracks,
                             uint64_t timestamp_ms) {
    // Build a quick lookup from detection bbox to track
    // (assumes association has already been run, tracks have updated positions)
    for (auto& track : tracks) {
        if (track.miss_count > 0) continue;

        int det_w = static_cast<int>(track.w);
        int det_h = static_cast<int>(track.h);
        if (det_w < cfg_.min_crop_size || det_h < cfg_.min_crop_size) continue;

        int margin_x = static_cast<int>(det_w * cfg_.margin_ratio);
        int margin_y = static_cast<int>(det_h * cfg_.margin_ratio);

        int left   = static_cast<int>(track.cx - track.w * 0.5f) - margin_x;
        int top    = static_cast<int>(track.cy - track.h * 0.5f) - margin_y;
        int right  = static_cast<int>(track.cx + track.w * 0.5f) + margin_x;
        int bottom = static_cast<int>(track.cy + track.h * 0.5f) + margin_y;

        // Check edge truncation
        float original_area = static_cast<float>((right - left) * (bottom - top));
        int cl = std::max(0, left);
        int ct = std::max(0, top);
        int cr = std::min(img_w, right);
        int cb = std::min(img_h, bottom);

        if (cr <= cl || cb <= ct) continue;

        float visible_area = static_cast<float>((cr - cl) * (cb - ct));
        if (original_area > 0 && visible_area / original_area < (1.0f - cfg_.max_edge_ratio))
            continue;

        int cw = cr - cl;
        int ch = cb - ct;

        // Extract crop directly into the ring slot to avoid per-frame
        // allocate/move churn, which can cause long-run heap growth.
        int idx = track.crop_cursor % TrackSlot::kMaxCrops;
        ScoredCrop& crop = track.crops[idx];
        crop.width = cw;
        crop.height = ch;
        crop.timestamp = timestamp_ms;
        crop.rgb_data.resize(cw * ch * 3);

        for (int y = 0; y < ch; y++) {
            const uint8_t* src = rgb + ((ct + y) * img_w + cl) * 3;
            uint8_t* dst = crop.rgb_data.data() + y * cw * 3;
            std::memcpy(dst, src, cw * 3);
        }

        crop.quality_score = score_quality(crop.rgb_data.data(), cw, ch,
                                            img_w, img_h, left, top, right, bottom);
        track.crop_cursor++;
        if (track.crop_count < TrackSlot::kMaxCrops) track.crop_count++;
    }
}

}  // namespace sc
