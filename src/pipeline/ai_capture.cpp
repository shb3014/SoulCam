// ============================================================================
// AI capture pipeline implementation (multi-model)
//
// Captures from ISP selfpath (/dev/video9) and runs multi-model RKNN inference.
//
// GStreamer pipeline (hardware-accelerated):
//   v4l2src (NV12) -> rgaconvert (HW: NV12->RGB + resize) -> appsink
//
// rgaconvert uses the Rockchip RGA 2D engine for format conversion
// and scaling, achieving 0% CPU usage.
//
// Each frame from appsink is fed to the ModelPipeline, which runs
// all active model slots sequentially on the NPU and merges detections.
// ============================================================================

#include "pipeline/ai_capture.h"
#include "ai/model_pipeline.h"
#include "util/logger.h"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video-info.h>

#include <thread>
#include <atomic>
#include <cstring>

namespace sc {

struct AiCapture {
    GstElement*     pipeline = nullptr;
    GstElement*     appsink  = nullptr;
    GMainLoop*      loop     = nullptr;
    std::thread     thread;
    std::atomic<bool> running{false};

    // Multi-model pipeline (internally thread-safe)
    ModelPipeline*  model_pipe = nullptr;

    // User callback
    AiCallback      callback;

    // Config
    int width  = 640;
    int height = 480;
};

// ---------------------------------------------------------------------------
// GStreamer pipeline construction
// ---------------------------------------------------------------------------

static std::string build_ai_pipeline(const Config& cfg,
                                      int model_w = 0, int model_h = 0) {
    const auto& ai  = cfg.ai;
    const auto& isp = cfg.isp;

    int out_w = (model_w > 0) ? model_w : ai.width;
    int out_h = (model_h > 0) ? model_h : ai.height;

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
                ",width=" + std::to_string(out_w) +
                ",height=" + std::to_string(out_h);

    pipeline += " ! appsink name=ai_sink emit-signals=true"
                " max-buffers=2 drop=true sync=false";

    return pipeline;
}

// ---------------------------------------------------------------------------
// Appsink callback -- runs multi-model inference on each frame
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

    // Run all model slots on this frame
    if (cap->model_pipe) {
        std::vector<Detection> detections;
        int rc = model_pipeline_infer(cap->model_pipe,
                                       map.data, map.size,
                                       cap->width, cap->height, 3,
                                       detections);
        if (rc == 0 && cap->callback) {
            cap->callback(detections, cap->width, cap->height);
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

    // Initialize multi-model pipeline
    int model_w = 0, model_h = 0, model_c = 0;
    if (!cfg.rknn.model_path.empty()) {
        cap->model_pipe = model_pipeline_create(cfg.rknn, cfg.extra_models);
        if (!cap->model_pipe) {
            SC_LOG_ERROR("Failed to create model pipeline -- AI will capture but not infer");
        } else {
            model_pipeline_get_input_size(cap->model_pipe, model_w, model_h, model_c);
            cap->width  = model_w;
            cap->height = model_h;
            SC_LOG_INFO("AI pipeline will resize %dx%d -> %dx%d (primary model input)",
                        cfg.ai.width, cfg.ai.height, model_w, model_h);
        }
    } else {
        SC_LOG_WARN("No RKNN model specified (--model); AI capture only, no inference");
    }

    // Build GStreamer pipeline with resize to primary model dimensions
    std::string launch = build_ai_pipeline(cfg, model_w, model_h);
    SC_LOG_INFO("AI pipeline: %s", launch.c_str());

    GError* error = nullptr;
    cap->pipeline = gst_parse_launch(launch.c_str(), &error);
    if (!cap->pipeline) {
        SC_LOG_ERROR("Failed to parse AI pipeline: %s",
                     error ? error->message : "unknown");
        g_clear_error(&error);
        model_pipeline_destroy(cap->model_pipe);
        delete cap;
        return nullptr;
    }
    g_clear_error(&error);

    // Get appsink and connect callback
    cap->appsink = gst_bin_get_by_name(GST_BIN(cap->pipeline), "ai_sink");
    if (!cap->appsink) {
        SC_LOG_ERROR("Failed to find ai_sink in pipeline");
        gst_object_unref(cap->pipeline);
        model_pipeline_destroy(cap->model_pipe);
        delete cap;
        return nullptr;
    }

    GstAppSinkCallbacks callbacks = {};
    callbacks.new_sample = on_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(cap->appsink), &callbacks, cap, nullptr);

    // Bus message handler
    GstBus* bus = gst_element_get_bus(cap->pipeline);
    gst_bus_add_watch(bus, on_bus_message, cap);
    gst_object_unref(bus);

    // Main loop
    cap->loop = g_main_loop_new(nullptr, FALSE);
    cap->running = true;
    cap->thread = std::thread(ai_thread_func, cap);

    int model_count = model_pipeline_count(cap->model_pipe);
    SC_LOG_INFO("AI capture started: selfpath %dx%d, %d model slot(s)",
                cap->width, cap->height, model_count);
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

    model_pipeline_destroy(cap->model_pipe);
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

int ai_capture_model_count(AiCapture* cap) {
    if (!cap || !cap->model_pipe) return 0;
    return model_pipeline_count(cap->model_pipe);
}

ModelSlotConfig ai_capture_get_model_info(AiCapture* cap, int slot_idx) {
    if (!cap || !cap->model_pipe) return {};
    return model_pipeline_get_slot_info(cap->model_pipe, slot_idx);
}

}  // namespace sc
