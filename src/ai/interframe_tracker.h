#pragma once
// ============================================================================
// Lightweight interframe tracker (Kalman + KCF)
//
// Runs on CPU between YOLO inference frames to provide smooth, high-frequency
// bounding box updates.  The Kalman filter predicts motion while the KCF
// (Kernelized Correlation Filter) provides visual correction using HOG
// features extracted from grayscale frame data.
//
// Typical usage:
//   - Every N-th frame: run YOLO, then call reinit() with the best detection
//   - Other frames:     call update() with the grayscale image to get a
//                        tracked bbox at near-zero latency (~3-5 ms on A55)
//
// Design constraints (RK3566 / Cortex-A55):
//   - No external dependencies (FFT + HOG built-in, NEON-optimized)
//   - Single-target tracking (matches HandTargetTracker policy)
//   - Works on grayscale (NV12 Y-plane or converted from RGB)
// ============================================================================

#include "soulcam.h"
#include <vector>

namespace sc {

struct InterframeTrackerConfig {
    int   yolo_interval        = 4;      // Run YOLO every N frames (1 = every frame, i.e. disabled)
    bool  enable_visual        = true;   // Enable KCF visual tracking (false = Kalman-only)
    float visual_psr_threshold = 7.0f;   // Below this PSR, fall back to Kalman prediction
    float visual_learning_rate = 0.125f; // KCF filter adaptation rate (0..1)
    int   visual_patch_size    = 64;     // ROI size for KCF (must be power of 2)
    float roi_padding          = 2.0f;   // Search region = bbox * this factor
    float kalman_process_noise = 4.0f;   // Kalman process noise (higher = more responsive)
    float kalman_measure_noise = 1.0f;   // Kalman measurement noise (higher = smoother)
    float smooth_factor        = 0.6f;   // EMA smoothing factor (0=max smooth, 1=snap)
};

class InterframeTracker {
public:
    explicit InterframeTracker(const InterframeTrackerConfig& cfg = {});
    ~InterframeTracker();

    InterframeTracker(const InterframeTracker&) = delete;
    InterframeTracker& operator=(const InterframeTracker&) = delete;

    void set_config(const InterframeTrackerConfig& cfg);

    // Reinitialize tracker with a YOLO detection and the current grayscale frame.
    // Called on YOLO frames to anchor the tracker to a confirmed detection.
    // dt: normalized time step (1.0 = 33.3ms / 30fps). Used for Kalman prediction.
    void reinit(const Detection& det, const uint8_t* gray, int img_w, int img_h,
                float dt = 1.0f);

    // Predict-only (no visual input). Very fast (<0.01ms).
    // Use when grayscale frame is unavailable.
    Detection predict(float dt = 1.0f);

    // Update with visual correction (Kalman + KCF). ~3-5ms on A55.
    // Returns the tracked detection with PSR-based confidence.
    // dt: normalized time step (1.0 = 33.3ms / 30fps).
    Detection update(const uint8_t* gray, int img_w, int img_h, float dt = 1.0f);

    // Reset to no-track state.
    void reset();

    bool  is_tracking() const { return tracking_; }
    float psr() const { return last_psr_; }

    // Estimated velocity (pixels/frame) from Kalman state.
    float velocity() const;

private:
    Detection smooth_output(float cx, float cy, float w, float h,
                            float confidence, int model_id);

    struct Impl;
    Impl* impl_;

    InterframeTrackerConfig cfg_;
    Detection last_det_{};
    bool  tracking_  = false;
    float last_psr_  = 0.0f;
};

}  // namespace sc
