// ============================================================================
// AI capture pipeline implementation (multi-model + interframe tracker)
//
// Captures from ISP selfpath (/dev/video9) and runs multi-model RKNN inference.
//
// GStreamer pipeline (hardware-accelerated):
//   v4l2src (NV12) -> rgaconvert (HW: NV12->RGB + resize) -> appsink
//
// rgaconvert uses the Rockchip RGA 2D engine for format conversion
// and scaling, achieving 0% CPU usage.
//
// When interframe tracking is enabled (yolo_interval > 1):
//   - Every N-th frame: full YOLO inference on NPU (~44ms)
//   - Other frames: lightweight Kalman+KCF tracking on CPU (~3-5ms)
//   This reduces NPU load by 60-80% while maintaining smooth tracking output.
//
// Adaptive interval mode:
//   When enabled, the scheduler dynamically decides whether to run YOLO
//   based on KCF PSR, estimated velocity, and last YOLO confidence.
// ============================================================================

#include "pipeline/ai_capture.h"
#include "ai/model_pipeline.h"
#include "ai/interframe_tracker.h"
#include "util/logger.h"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video-info.h>

#include <thread>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

namespace sc {

struct AiCapture {
    GstElement*     pipeline = nullptr;
    GstElement*     appsink  = nullptr;
    GMainLoop*      loop     = nullptr;
    GMainContext*   context  = nullptr;
    std::thread     thread;
    std::atomic<bool> running{false};

    ModelPipeline*  model_pipe = nullptr;

    // Interframe tracker (Kalman + KCF)
    InterframeTracker* tracker = nullptr;
    int             yolo_interval = 1;
    uint64_t        frame_counter = 0;
    std::vector<uint8_t> gray_buf;

    // Adaptive interval state
    bool            adaptive_enabled = false;
    int             max_skip = 8;
    int             min_skip = 2;
    int             frames_since_yolo = 0;
    float           last_yolo_conf = 0.0f;
    bool            force_yolo_next = false;

    // Unified target policy: hand-preferred + person-fallback
    struct TargetPolicy {
        bool       enabled = false;
        TargetMode mode = TargetMode::PersonFallback;
        int        hand_slot = 1;
        int        person_slot = 0;
        int        weight_high = 10;
        int        weight_low = 1;
        int        hand_confirm_count = 0;
        int        hand_lost_count = 0;
        int        hand_confirm_threshold = 3;
        int        hand_lost_threshold = 5;
        int        pick_call_count = 0;
    } target;

    // FPS target throttle (0 = unlimited)
    std::atomic<int> target_fps{0};
    using Clock = std::chrono::steady_clock;
    Clock::time_point last_process_time = Clock::now();

    // FPS statistics (logged every 5 seconds)
    Clock::time_point fps_last_log = Clock::now();
    uint32_t fps_total_frames = 0;
    uint32_t fps_yolo_frames  = 0;
    uint32_t fps_cam_frames   = 0;  // raw camera delivery count (before throttle)

    AiCallback      callback;

    int width  = 640;
    int height = 480;

    // Letterbox: capture at camera resolution, pad to model input size.
    // Preserves aspect ratio for better YOLO accuracy.
    int cap_w = 640;     // camera capture width (RGB from rgaconvert)
    int cap_h = 480;     // camera capture height
    int model_w = 640;   // model input width (after letterbox)
    int model_h = 640;   // model input height (after letterbox)
    int lb_pad_top = 0;  // letterbox padding (top/bottom)
    std::vector<uint8_t> lb_buf;  // letterboxed RGB buffer (model_w * model_h * 3)

    // Frame timing for time-aware Kalman
    Clock::time_point last_frame_time{};
    bool has_frame_time = false;
    static constexpr float kRefIntervalMs = 33.33f; // reference dt=1.0 at 30fps
};

// ---------------------------------------------------------------------------
// GStreamer pipeline construction
// ---------------------------------------------------------------------------

static std::string build_ai_pipeline(const Config& cfg, int cap_w, int cap_h) {
    const auto& ai  = cfg.ai;
    const auto& isp = cfg.isp;

    std::string pipeline;

    int io_mode = cfg.use_dmabuf ? 4 : 2;
    pipeline += "v4l2src device=" + isp.selfpath +
                " io-mode=" + std::to_string(io_mode) + " do-timestamp=true"
                " ! video/x-raw,format=" + ai.src_fmt +
                ",width=" + std::to_string(ai.width) +
                ",height=" + std::to_string(ai.height) +
                ",framerate=" + std::to_string(ai.fps) + "/1";

    pipeline += " ! queue leaky=downstream max-size-buffers=2"
                " max-size-time=0 max-size-bytes=0";

    pipeline += " ! rgaconvert"
                " ! video/x-raw,format=RGB"
                ",width=" + std::to_string(cap_w) +
                ",height=" + std::to_string(cap_h);

    pipeline += " ! appsink name=ai_sink emit-signals=true"
                " max-buffers=2 drop=true sync=false";

    return pipeline;
}

// ---------------------------------------------------------------------------
// RGB -> Grayscale conversion (for interframe tracker)
// ---------------------------------------------------------------------------

static void rgb_to_gray(const uint8_t* rgb, uint8_t* gray, int w, int h) {
    const int npix = w * h;
#ifdef __ARM_NEON
    const uint8x8_t coeff_r = vdup_n_u8(77);
    const uint8x8_t coeff_g = vdup_n_u8(150);
    const uint8x8_t coeff_b = vdup_n_u8(29);
    int i = 0;
    for (; i + 8 <= npix; i += 8) {
        uint8x8x3_t px = vld3_u8(rgb + i * 3);
        uint16x8_t acc = vmull_u8(px.val[0], coeff_r);
        acc = vmlal_u8(acc, px.val[1], coeff_g);
        acc = vmlal_u8(acc, px.val[2], coeff_b);
        vst1_u8(gray + i, vshrn_n_u16(acc, 8));
    }
    for (; i < npix; i++) {
        gray[i] = static_cast<uint8_t>(
            (77u * rgb[i*3] + 150u * rgb[i*3+1] + 29u * rgb[i*3+2]) >> 8);
    }
#else
    for (int i = 0; i < npix; i++) {
        gray[i] = static_cast<uint8_t>(
            (77u * rgb[i*3] + 150u * rgb[i*3+1] + 29u * rgb[i*3+2]) >> 8);
    }
#endif
}

// ---------------------------------------------------------------------------
// Letterbox: pad camera frame (cap_w x cap_h) to model input (model_w x model_h)
// with gray=128 bars, preserving aspect ratio. Returns pointer to letterboxed buffer.
// ---------------------------------------------------------------------------

static void letterbox_pad(const uint8_t* src, int cap_w, int cap_h,
                           uint8_t* dst, int model_w, int model_h,
                           int pad_top) {
    const int row_bytes = model_w * 3;
    std::memset(dst, 128, pad_top * row_bytes);
    std::memcpy(dst + pad_top * row_bytes, src, cap_w * cap_h * 3);
    const int pad_bottom = model_h - cap_h - pad_top;
    if (pad_bottom > 0)
        std::memset(dst + (pad_top + cap_h) * row_bytes, 128, pad_bottom * row_bytes);
}

// Un-letterbox: adjust detection coordinates from model space back to camera space.
static void unletterbox_detections(std::vector<Detection>& dets,
                                    int pad_top, int cap_h) {
    for (auto& d : dets) {
        d.top    = std::max(0, std::min(d.top - pad_top, cap_h));
        d.bottom = std::max(0, std::min(d.bottom - pad_top, cap_h));
    }
}

static int pick_best_detection(const std::vector<Detection>& dets) {
    int best = -1;
    float best_score = -1.0f;
    for (int i = 0; i < static_cast<int>(dets.size()); i++) {
        float area = static_cast<float>(std::max(0, dets[i].right - dets[i].left)) *
                     static_cast<float>(std::max(0, dets[i].bottom - dets[i].top));
        float score = area * dets[i].confidence;
        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Target-aware detection picker with debounce + model weight scheduling
// ---------------------------------------------------------------------------

static int pick_target_detection(AiCapture* cap,
                                  const std::vector<Detection>& dets) {
    auto& tp = cap->target;
    if (!tp.enabled) return pick_best_detection(dets);

    int best_hand = -1, best_person = -1;
    float best_hand_score = -1.0f, best_person_score = -1.0f;
    bool hand_seen = false;
    int hand_count = 0, person_count = 0, other_count = 0;

    for (int i = 0; i < static_cast<int>(dets.size()); i++) {
        float area = static_cast<float>(std::max(0, dets[i].right - dets[i].left)) *
                     static_cast<float>(std::max(0, dets[i].bottom - dets[i].top));
        float score = area * dets[i].confidence;

        if (dets[i].model_id == tp.hand_slot) {
            hand_seen = true;
            hand_count++;
            if (score > best_hand_score) {
                best_hand_score = score;
                best_hand = i;
            }
        } else if (dets[i].model_id == tp.person_slot) {
            person_count++;
            if (score > best_person_score) {
                best_person_score = score;
                best_person = i;
            }
        } else {
            other_count++;
        }
    }

    tp.pick_call_count++;
    if (tp.pick_call_count % 20 == 1) {
        SC_LOG_INFO("TargetPick: dets=%zu hand=%d person=%d other=%d "
                    "mode=%s confirm=%d/%d lost=%d/%d",
                    dets.size(), hand_count, person_count, other_count,
                    tp.mode == TargetMode::HandPreferred ? "hand" : "person",
                    tp.hand_confirm_count, tp.hand_confirm_threshold,
                    tp.hand_lost_count, tp.hand_lost_threshold);
    }

    // Debounce state machine.
    // In PersonFallback: hand model runs ~50% of YOLO frames (equal weights).
    //   Only increment confirm when hand IS seen; don't reset when hand model
    //   may not have run (avoids killing the counter on person-model frames).
    // In HandPreferred: hand model runs every YOLO frame (person disabled).
    //   No hand = real loss; count every frame.
    if (hand_seen) {
        tp.hand_confirm_count++;
        tp.hand_lost_count = 0;
    } else if (tp.mode == TargetMode::HandPreferred) {
        tp.hand_lost_count++;
        tp.hand_confirm_count = 0;
    }
    // In PersonFallback with no hand seen: leave counters unchanged
    // (hand model may not have run this frame due to weighted scheduling)

    TargetMode prev_mode = tp.mode;

    if (tp.mode == TargetMode::PersonFallback &&
        tp.hand_confirm_count >= tp.hand_confirm_threshold) {
        tp.mode = TargetMode::HandPreferred;
    } else if (tp.mode == TargetMode::HandPreferred &&
               tp.hand_lost_count >= tp.hand_lost_threshold) {
        tp.mode = TargetMode::PersonFallback;
    }

    if (tp.mode != prev_mode) {
        const char* mode_name = (tp.mode == TargetMode::HandPreferred)
                                ? "hand_preferred" : "person_fallback";
        SC_LOG_INFO("Target policy: switch to %s (confirm=%d, lost=%d)",
                    mode_name, tp.hand_confirm_count, tp.hand_lost_count);

        tp.hand_confirm_count = 0;
        tp.hand_lost_count = 0;
        if (cap->tracker) cap->tracker->reset();
        cap->force_yolo_next = true;

        if (cap->model_pipe) {
            if (tp.mode == TargetMode::HandPreferred) {
                model_pipeline_set_slot_weight(cap->model_pipe, tp.hand_slot, tp.weight_high);
                model_pipeline_enable_model(cap->model_pipe, tp.person_slot, false);
            } else {
                // Equal weights so hand model gets fair scheduling for detection
                model_pipeline_enable_model(cap->model_pipe, tp.person_slot, true);
                model_pipeline_set_slot_weight(cap->model_pipe, tp.person_slot, 1);
                model_pipeline_set_slot_weight(cap->model_pipe, tp.hand_slot, 1);
            }
        }
    }

    // Return best detection matching current target mode
    if (tp.mode == TargetMode::HandPreferred && best_hand >= 0)
        return best_hand;
    if (best_person >= 0) return best_person;
    return pick_best_detection(dets);
}

// ---------------------------------------------------------------------------
// Adaptive YOLO interval scheduler
// ---------------------------------------------------------------------------

static bool should_run_yolo(AiCapture* cap) {
    if (!cap->tracker || cap->yolo_interval <= 1) return true;

    if (cap->force_yolo_next) {
        cap->force_yolo_next = false;
        return true;
    }

    if (!cap->tracker->is_tracking()) return true;

    if (!cap->adaptive_enabled) {
        return (cap->frame_counter % static_cast<uint64_t>(cap->yolo_interval) == 0);
    }

    if (cap->frames_since_yolo >= cap->max_skip) return true;
    if (cap->frames_since_yolo < cap->min_skip) return false;

    float psr = cap->tracker->psr();
    float vel = cap->tracker->velocity();

    // Urgency scoring: higher = more need for YOLO
    float urgency = 0.0f;

    if (psr < 7.0f) urgency += 3.0f;
    else if (psr < 10.0f) urgency += 1.0f;
    else if (psr > 15.0f) urgency -= 1.0f;

    if (vel > 30.0f) urgency += 2.0f;
    else if (vel > 15.0f) urgency += 1.0f;
    else if (vel < 5.0f) urgency -= 1.0f;

    if (cap->last_yolo_conf < 0.4f) urgency += 1.5f;
    else if (cap->last_yolo_conf > 0.8f) urgency -= 0.5f;

    return urgency >= 2.0f;
}

// ---------------------------------------------------------------------------
// Appsink callback
// ---------------------------------------------------------------------------

static GstFlowReturn on_new_sample(GstAppSink* sink, gpointer user_data) {
    auto* cap = static_cast<AiCapture*>(user_data);

    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstCaps*   caps   = gst_sample_get_caps(sample);
    if (!buffer || !caps) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    cap->fps_cam_frames++;

    int tfps = cap->target_fps.load(std::memory_order_relaxed);
    if (tfps > 0) {
        auto now = AiCapture::Clock::now();
        auto min_interval = std::chrono::microseconds(1000000LL / tfps);
        constexpr auto kJitterTolerance = std::chrono::microseconds(2000);

        if (now + kJitterTolerance < cap->last_process_time + min_interval) {
            gst_buffer_unmap(buffer, &map);
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }

        cap->last_process_time += min_interval;
        if (cap->last_process_time + min_interval < now) {
            cap->last_process_time = now;
        }
    }

    const bool tracker_active = cap->tracker && cap->yolo_interval > 1;
    const bool run_yolo = !tracker_active || should_run_yolo(cap);
    cap->frame_counter++;
    cap->fps_total_frames++;
    if (run_yolo) cap->fps_yolo_frames++;

    {
        auto now = AiCapture::Clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - cap->fps_last_log).count();
        if (elapsed >= 5000) {
            float secs = elapsed / 1000.0f;
            float cam_fps   = cap->fps_cam_frames   / secs;
            float total_fps = cap->fps_total_frames / secs;
            float yolo_fps  = cap->fps_yolo_frames  / secs;
            if (tracker_active) {
                SC_LOG_INFO("AI pipeline: %.1f fps total (YOLO %.1f + tracker %.1f, vel=%.1f psr=%.1f) | cam feed: %.1f fps",
                            total_fps, yolo_fps, total_fps - yolo_fps,
                            cap->tracker ? cap->tracker->velocity() : 0.0f,
                            cap->tracker ? cap->tracker->psr() : 0.0f,
                            cam_fps);
            } else {
                SC_LOG_INFO("AI pipeline: %.1f fps (YOLO only) | cam feed: %.1f fps",
                            yolo_fps, cam_fps);
            }
            cap->fps_total_frames = 0;
            cap->fps_yolo_frames  = 0;
            cap->fps_cam_frames   = 0;
            cap->fps_last_log     = now;
        }
    }

    // Compute dt for time-aware Kalman (normalized so dt=1.0 at 30fps)
    float frame_dt = 1.0f;
    {
        auto now = AiCapture::Clock::now();
        if (cap->has_frame_time) {
            float elapsed_ms = std::chrono::duration<float, std::milli>(
                now - cap->last_frame_time).count();
            frame_dt = std::max(0.1f, std::min(elapsed_ms / AiCapture::kRefIntervalMs, 5.0f));
        }
        cap->last_frame_time = now;
        cap->has_frame_time = true;
    }

    if (run_yolo && cap->model_pipe) {
        // Letterbox camera frame to model input size
        const uint8_t* infer_data = map.data;
        int infer_w = cap->cap_w;
        int infer_h = cap->cap_h;
        if (cap->lb_pad_top > 0) {
            letterbox_pad(map.data, cap->cap_w, cap->cap_h,
                          cap->lb_buf.data(), cap->model_w, cap->model_h,
                          cap->lb_pad_top);
            infer_data = cap->lb_buf.data();
            infer_w = cap->model_w;
            infer_h = cap->model_h;
        }

        std::vector<Detection> detections;
        int rc = model_pipeline_infer(cap->model_pipe,
                                       infer_data, infer_w * infer_h * 3,
                                       infer_w, infer_h, 3,
                                       detections);
        if (rc == 0) {
            if (cap->lb_pad_top > 0)
                unletterbox_detections(detections, cap->lb_pad_top, cap->cap_h);

            cap->frames_since_yolo = 0;

            int best = cap->target.enabled
                ? pick_target_detection(cap, detections)
                : pick_best_detection(detections);

            if (tracker_active && best >= 0) {
                cap->last_yolo_conf = detections[best].confidence;
                const int npix = cap->cap_w * cap->cap_h;
                cap->gray_buf.resize(npix);
                rgb_to_gray(map.data, cap->gray_buf.data(), cap->cap_w, cap->cap_h);
                cap->tracker->reinit(detections[best],
                                     cap->gray_buf.data(),
                                     cap->cap_w, cap->cap_h,
                                     frame_dt);
            }

            if (cap->callback) {
                cap->callback(detections, cap->cap_w, cap->cap_h);
            }
        }
    } else if (tracker_active && cap->tracker->is_tracking()) {
        cap->frames_since_yolo++;
        const int npix = cap->cap_w * cap->cap_h;
        cap->gray_buf.resize(npix);
        rgb_to_gray(map.data, cap->gray_buf.data(), cap->cap_w, cap->cap_h);

        Detection tracked = cap->tracker->update(cap->gray_buf.data(),
                                                  cap->cap_w, cap->cap_h,
                                                  frame_dt);

        if (cap->callback) {
            std::vector<Detection> dets = {tracked};
            cap->callback(dets, cap->cap_w, cap->cap_h);
        }
    }

    gst_buffer_unmap(buffer, &map);
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

// ---------------------------------------------------------------------------
// Bus message handler
// ---------------------------------------------------------------------------

static gboolean on_bus_message(GstBus* bus, GstMessage* msg, gpointer user_data) {
    auto* cap = static_cast<AiCapture*>(user_data);
    (void)bus;

    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError* err = nullptr;
            gchar*  dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            SC_LOG_ERROR("AI pipeline error: %s", err ? err->message : "unknown");
            if (dbg) SC_LOG_DEBUG("  debug: %s", dbg);
            g_clear_error(&err);
            g_free(dbg);
            if (cap->loop) g_main_loop_quit(cap->loop);
            break;
        }
        case GST_MESSAGE_EOS:
            SC_LOG_INFO("AI pipeline EOS");
            if (cap->loop) g_main_loop_quit(cap->loop);
            break;
        default:
            break;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// AI thread main loop
// ---------------------------------------------------------------------------

static void ai_thread_func(AiCapture* cap) {
    SC_LOG_INFO("AI capture thread started");

    GstStateChangeReturn ret = gst_element_set_state(cap->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        SC_LOG_ERROR("Failed to start AI pipeline");
        cap->running = false;
        return;
    }

    g_main_loop_run(cap->loop);

    gst_element_set_state(cap->pipeline, GST_STATE_NULL);
    cap->running = false;
    SC_LOG_INFO("AI capture thread exited");
}

// ---------------------------------------------------------------------------
// Helper: build InterframeTrackerConfig from Config
// ---------------------------------------------------------------------------

static InterframeTrackerConfig make_tracker_config(const Config& cfg) {
    InterframeTrackerConfig tcfg;
    tcfg.yolo_interval        = std::max(1, cfg.tracker_yolo_interval);
    tcfg.enable_visual        = cfg.tracker_enable_mosse;
    tcfg.visual_psr_threshold = cfg.tracker_mosse_psr_threshold;
    tcfg.visual_learning_rate = cfg.tracker_mosse_learning_rate;
    tcfg.visual_patch_size    = cfg.tracker_mosse_patch_size;
    tcfg.roi_padding          = cfg.tracker_roi_padding;
    tcfg.smooth_factor        = cfg.tracker_smooth_factor;
    return tcfg;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

AiCapture* ai_capture_start(const Config& cfg, AiCallback cb) {
    if (!cfg.enable_ai) {
        SC_LOG_INFO("AI pipeline disabled (use --ai to enable)");
        return nullptr;
    }

    auto* cap = new AiCapture();
    cap->callback = std::move(cb);
    cap->width  = cfg.ai.width;
    cap->height = cfg.ai.height;

    int model_w = 0, model_h = 0, model_c = 0;
    if (!cfg.rknn.model_path.empty()) {
        ModelPipelineOptions mp_opts;
        mp_opts.weighted_scheduler = cfg.weighted_scheduler;
        mp_opts.max_models_per_frame = cfg.max_models_per_frame;
        mp_opts.primary_model_weight = cfg.primary_model_weight;
        cap->model_pipe = model_pipeline_create(cfg.rknn, cfg.extra_models, mp_opts);
        if (!cap->model_pipe) {
            SC_LOG_ERROR("Failed to create model pipeline -- AI will capture but not infer");
        } else {
            model_pipeline_get_input_size(cap->model_pipe, model_w, model_h, model_c);
            SC_LOG_INFO("AI model input: %dx%d, camera: %dx%d",
                        model_w, model_h, cfg.ai.width, cfg.ai.height);
        }
    } else {
        SC_LOG_WARN("No RKNN model specified (--model); AI capture only, no inference");
    }

    // Letterbox setup: capture at camera resolution, pad to model size
    cap->cap_w = cfg.ai.width;
    cap->cap_h = cfg.ai.height;
    cap->model_w = (model_w > 0) ? model_w : cfg.ai.width;
    cap->model_h = (model_h > 0) ? model_h : cfg.ai.height;
    cap->width  = cap->cap_w;
    cap->height = cap->cap_h;

    if (cap->cap_w == cap->model_w && cap->cap_h != cap->model_h &&
        cap->model_h > cap->cap_h) {
        cap->lb_pad_top = (cap->model_h - cap->cap_h) / 2;
        cap->lb_buf.resize(cap->model_w * cap->model_h * 3);
        SC_LOG_INFO("Letterbox: %dx%d -> %dx%d (pad_top=%d, gray=128)",
                    cap->cap_w, cap->cap_h, cap->model_w, cap->model_h,
                    cap->lb_pad_top);
    } else if (cap->cap_w != cap->model_w || cap->cap_h != cap->model_h) {
        SC_LOG_WARN("Camera %dx%d != model %dx%d, no letterbox (rgaconvert will stretch)",
                    cap->cap_w, cap->cap_h, cap->model_w, cap->model_h);
    }

    cap->yolo_interval = std::max(1, cfg.tracker_yolo_interval);
    if (cap->yolo_interval > 1) {
        auto tcfg = make_tracker_config(cfg);
        cap->tracker = new InterframeTracker(tcfg);
        SC_LOG_INFO("Interframe tracker enabled: YOLO every %d frames, KCF=%s, patch=%d, smooth=%.2f",
                    cap->yolo_interval,
                    tcfg.enable_visual ? "on" : "off",
                    tcfg.visual_patch_size,
                    tcfg.smooth_factor);
    }

    cap->adaptive_enabled = cfg.tracker_adaptive_interval;
    cap->max_skip = std::max(1, cfg.tracker_max_skip);
    cap->min_skip = std::max(0, cfg.tracker_min_skip);
    if (cap->adaptive_enabled) {
        SC_LOG_INFO("Adaptive YOLO interval: enabled (max_skip=%d, min_skip=%d)",
                    cap->max_skip, cap->min_skip);
    }

    // AI pipeline FPS target
    if (cfg.ai_target_fps > 0) {
        cap->target_fps.store(cfg.ai_target_fps, std::memory_order_relaxed);
        SC_LOG_INFO("AI pipeline target FPS: %d", cfg.ai_target_fps);
    }

    // Unified target policy (hand-preferred / person-fallback)
    cap->target.enabled = cfg.test_adaptive_hand_person;
    cap->target.hand_slot = cfg.test_hand_slot;
    cap->target.person_slot = cfg.test_person_slot;
    cap->target.weight_high = cfg.test_weight_high;
    cap->target.weight_low = cfg.test_weight_low;
    cap->target.hand_confirm_threshold = cfg.tracker_hand_confirm;
    cap->target.hand_lost_threshold = cfg.tracker_hand_lost;
    if (cap->target.enabled) {
        cap->target.mode = TargetMode::PersonFallback;
        SC_LOG_INFO("Target policy: enabled (hand_slot=%d, person_slot=%d, "
                    "confirm=%d, lost=%d)",
                    cap->target.hand_slot, cap->target.person_slot,
                    cap->target.hand_confirm_threshold,
                    cap->target.hand_lost_threshold);
        // Equal weights in PersonFallback so hand model gets fair scheduling
        if (cap->model_pipe) {
            model_pipeline_set_slot_weight(cap->model_pipe,
                cap->target.person_slot, 1);
            model_pipeline_set_slot_weight(cap->model_pipe,
                cap->target.hand_slot, 1);
        }
    }

    std::string launch = build_ai_pipeline(cfg, cap->cap_w, cap->cap_h);
    SC_LOG_INFO("AI pipeline: %s", launch.c_str());

    GError* error = nullptr;
    cap->pipeline = gst_parse_launch(launch.c_str(), &error);
    if (!cap->pipeline) {
        SC_LOG_ERROR("Failed to parse AI pipeline: %s",
                     error ? error->message : "unknown");
        g_clear_error(&error);
        model_pipeline_destroy(cap->model_pipe);
        delete cap->tracker;
        delete cap;
        return nullptr;
    }
    g_clear_error(&error);

    cap->appsink = gst_bin_get_by_name(GST_BIN(cap->pipeline), "ai_sink");
    if (!cap->appsink) {
        SC_LOG_ERROR("Failed to find ai_sink in pipeline");
        gst_object_unref(cap->pipeline);
        model_pipeline_destroy(cap->model_pipe);
        delete cap->tracker;
        delete cap;
        return nullptr;
    }

    GstAppSinkCallbacks callbacks = {};
    callbacks.new_sample = on_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(cap->appsink), &callbacks, cap, nullptr);

    cap->context = g_main_context_new();
    cap->loop = g_main_loop_new(cap->context, FALSE);

    GstBus* bus = gst_element_get_bus(cap->pipeline);
    GSource* bus_src = gst_bus_create_watch(bus);
    g_source_set_callback(bus_src, (GSourceFunc)on_bus_message, cap, nullptr);
    g_source_attach(bus_src, cap->context);
    g_source_unref(bus_src);
    gst_object_unref(bus);

    cap->running = true;
    cap->thread = std::thread(ai_thread_func, cap);

    int model_count = model_pipeline_count(cap->model_pipe);
    SC_LOG_INFO("AI capture started: capture %dx%d, model %dx%d, letterbox=%s, %d model slot(s)",
                cap->cap_w, cap->cap_h, cap->model_w, cap->model_h,
                cap->lb_pad_top > 0 ? "yes" : "no", model_count);
    return cap;
}

void ai_capture_stop(AiCapture* cap) {
    if (!cap) return;

    SC_LOG_INFO("Stopping AI capture...");

    if (cap->loop && g_main_loop_is_running(cap->loop)) {
        g_main_loop_quit(cap->loop);
    }
    if (cap->thread.joinable()) {
        cap->thread.join();
    }

    if (cap->appsink) gst_object_unref(cap->appsink);
    if (cap->pipeline) gst_object_unref(cap->pipeline);
    if (cap->loop) g_main_loop_unref(cap->loop);
    if (cap->context) g_main_context_unref(cap->context);

    model_pipeline_destroy(cap->model_pipe);
    delete cap->tracker;
    delete cap;

    SC_LOG_INFO("AI capture stopped");
}

// ---------------------------------------------------------------------------
// Model management (delegate to ModelPipeline)
// ---------------------------------------------------------------------------

int ai_capture_swap_model(AiCapture* cap, const RknnConfig& new_cfg, int slot_idx) {
    if (!cap || !cap->model_pipe) return -1;
    return model_pipeline_swap_model(cap->model_pipe, slot_idx, new_cfg);
}

int ai_capture_add_model(AiCapture* cap, const ModelSlotConfig& cfg) {
    if (!cap || !cap->model_pipe) return -1;
    return model_pipeline_add_model(cap->model_pipe, cfg);
}

int ai_capture_remove_model(AiCapture* cap, int slot_idx) {
    if (!cap || !cap->model_pipe) return -1;
    return model_pipeline_remove_model(cap->model_pipe, slot_idx);
}

void ai_capture_enable_model(AiCapture* cap, int slot_idx, bool enable) {
    if (!cap || !cap->model_pipe) return;
    model_pipeline_enable_model(cap->model_pipe, slot_idx, enable);
}

int ai_capture_set_model_weight(AiCapture* cap, int slot_idx, int run_weight) {
    if (!cap || !cap->model_pipe) return -1;
    return model_pipeline_set_slot_weight(cap->model_pipe, slot_idx, run_weight);
}

int ai_capture_model_count(AiCapture* cap) {
    if (!cap || !cap->model_pipe) return 0;
    return model_pipeline_count(cap->model_pipe);
}

ModelSlotConfig ai_capture_get_model_info(AiCapture* cap, int slot_idx) {
    if (!cap || !cap->model_pipe) return {};
    return model_pipeline_get_slot_info(cap->model_pipe, slot_idx);
}

std::string ai_capture_debug_status(AiCapture* cap) {
    if (!cap || !cap->model_pipe) return "AI capture/model pipeline not running";
    std::string status = model_pipeline_debug_status(cap->model_pipe);
    if (cap->tracker) {
        status += "\nInterframeTracker: yolo_interval=" + std::to_string(cap->yolo_interval) +
                  " adaptive=" + std::string(cap->adaptive_enabled ? "yes" : "no") +
                  " tracking=" + std::string(cap->tracker->is_tracking() ? "yes" : "no") +
                  " psr=" + std::to_string(cap->tracker->psr()) +
                  " vel=" + std::to_string(cap->tracker->velocity()) +
                  " since_yolo=" + std::to_string(cap->frames_since_yolo);
    }
    if (cap->target.enabled) {
        status += "\nTargetPolicy: mode=" +
                  std::string(cap->target.mode == TargetMode::HandPreferred
                              ? "hand" : "person") +
                  " hand_confirm=" + std::to_string(cap->target.hand_confirm_count) +
                  " hand_lost=" + std::to_string(cap->target.hand_lost_count);
    }
    return status;
}

void ai_capture_reset_tracker(AiCapture* cap) {
    if (!cap || !cap->tracker) return;
    cap->tracker->reset();
    SC_LOG_INFO("Interframe tracker reset");
}

void ai_capture_set_target_fps(AiCapture* cap, int fps) {
    if (!cap) return;
    int clamped = (fps < 0) ? 0 : fps;
    cap->target_fps.store(clamped, std::memory_order_relaxed);
    SC_LOG_INFO("AI pipeline target FPS: %s",
                clamped > 0 ? std::to_string(clamped).c_str() : "unlimited");
}

void ai_capture_update_tracker_config(AiCapture* cap, const Config& cfg) {
    if (!cap) return;

    const int new_interval = std::max(1, cfg.tracker_yolo_interval);
    const bool was_active  = cap->tracker && cap->yolo_interval > 1;
    const bool will_active = new_interval > 1;

    auto tcfg = make_tracker_config(cfg);

    if (!was_active && will_active) {
        cap->tracker = new InterframeTracker(tcfg);
        cap->frame_counter = 0;
        cap->frames_since_yolo = 0;
        SC_LOG_INFO("Interframe tracker created via DP: interval=%d KCF=%s patch=%d smooth=%.2f",
                    new_interval, tcfg.enable_visual ? "on" : "off",
                    tcfg.visual_patch_size, tcfg.smooth_factor);
    } else if (was_active && !will_active) {
        cap->tracker->reset();
        SC_LOG_INFO("Interframe tracker disabled via DP (interval=1, object kept alive)");
    } else if (was_active && will_active) {
        cap->tracker->set_config(tcfg);
        SC_LOG_INFO("Interframe tracker reconfigured via DP: interval=%d KCF=%s psr=%.1f smooth=%.2f",
                    new_interval, tcfg.enable_visual ? "on" : "off",
                    tcfg.visual_psr_threshold, tcfg.smooth_factor);
    }

    cap->yolo_interval = new_interval;
    cap->adaptive_enabled = cfg.tracker_adaptive_interval;
    cap->max_skip = std::max(1, cfg.tracker_max_skip);
    cap->min_skip = std::max(0, cfg.tracker_min_skip);

    // Update target policy
    bool was_target = cap->target.enabled;
    cap->target.enabled = cfg.test_adaptive_hand_person;
    cap->target.hand_slot = cfg.test_hand_slot;
    cap->target.person_slot = cfg.test_person_slot;
    cap->target.weight_high = cfg.test_weight_high;
    cap->target.weight_low = cfg.test_weight_low;
    cap->target.hand_confirm_threshold = cfg.tracker_hand_confirm;
    cap->target.hand_lost_threshold = cfg.tracker_hand_lost;
    if (cap->target.enabled && !was_target) {
        cap->target.mode = TargetMode::PersonFallback;
        cap->target.hand_confirm_count = 0;
        cap->target.hand_lost_count = 0;
        SC_LOG_INFO("Target policy: enabled via DP (confirm=%d, lost=%d)",
                    cap->target.hand_confirm_threshold,
                    cap->target.hand_lost_threshold);
    }
}

bool ai_capture_target_policy_enabled(AiCapture* cap) {
    return cap && cap->target.enabled;
}

TargetMode ai_capture_get_target_mode(AiCapture* cap) {
    if (!cap) return TargetMode::PersonFallback;
    return cap->target.mode;
}

int ai_capture_get_target_slot(AiCapture* cap) {
    if (!cap || !cap->target.enabled) return -1;
    return (cap->target.mode == TargetMode::HandPreferred)
        ? cap->target.hand_slot : cap->target.person_slot;
}

}  // namespace sc
