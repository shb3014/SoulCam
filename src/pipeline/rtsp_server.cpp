// ============================================================================
// Fully hardware-accelerated RTSP server implementation
//
// Key design decisions:
//   1. Single GStreamer pipeline (no shm hop) -- lowest latency
//   2. ISP outputs NV12 directly -- no RGA format conversion needed.
//      (NV12 UV valid from frame #2; frame #1 ISP warm-up has UV=0x80).
//      Falls back to UYVY+RGA if --src-fmt UYVY is used.
//   3. MPP for H.264 encoding -- HW video encoder
//   4. io-mode=2 (userptr) default; io-mode=4 (dmabuf) for zero-copy
//      when --dmabuf is passed.  DMA-BUF exports fd's from v4l2/ISP
//      that RGA + MPP can import directly -- no CPU-visible copies.
//   5. queue with leaky=downstream to drop frames if encoder is slow
// ============================================================================

#include "pipeline/rtsp_server.h"
#include "pipeline/overlay.h"
#include "util/logger.h"

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <thread>
#include <atomic>
#include <cstring>

namespace sc {

struct RtspServer {
    GMainLoop*              loop    = nullptr;
    GstRTSPServer*          server  = nullptr;
    GstRTSPMediaFactory*    factory = nullptr;
    std::thread             thread;
    std::atomic<bool>       running{false};
};

// ---------------------------------------------------------------------------
// Build the fully HW-accelerated launch string
// ---------------------------------------------------------------------------

std::string rtsp_build_launch(const Config& cfg) {
    const auto& st   = cfg.stream;
    const auto& rtsp = cfg.rtsp;

    std::string launch;

    // --- Source: ISP mainpath ---
    // io-mode=2: userptr (proven, CPU-visible copies)
    // io-mode=4: dmabuf  (zero-copy, ISP exports DMA-BUF fd's)
    int io_mode = cfg.use_dmabuf ? 4 : 2;
    launch += "( v4l2src device=" + cfg.isp.mainpath +
              " io-mode=" + std::to_string(io_mode) + " do-timestamp=true";
    launch += " ! video/x-raw,format=" + st.src_fmt +
              ",width=" + std::to_string(st.width) +
              ",height=" + std::to_string(st.height) +
              ",framerate=" + std::to_string(st.fps) + "/1";

    // --- Decouple capture from encode ---
    launch += " ! queue leaky=downstream max-size-buffers=3 max-size-time=0 max-size-bytes=0";

    // --- Color conversion + optional overlay ---
    if (cfg.enable_overlay && cfg.enable_ai) {
        // Overlay mode: NV12/UYVY → BGRA (RGA HW) → cairooverlay → NV12 (RGA HW)
        // The cairooverlay draws detection boxes on BGRA frames.
        launch += " ! rgaconvert"
                  " ! video/x-raw,format=BGRA"
                  ",width=" + std::to_string(st.width) +
                  ",height=" + std::to_string(st.height);
        launch += " ! cairooverlay name=overlay";
        launch += " ! rgaconvert"
                  " ! video/x-raw,format=NV12"
                  ",width=" + std::to_string(st.width) +
                  ",height=" + std::to_string(st.height);
    } else if (st.src_fmt == "UYVY" || st.src_fmt == "YUYV") {
        // Fallback mode: UYVY → NV12 (single RGA pass, zero CPU)
        launch += " ! rgaconvert"
                  " ! video/x-raw,format=NV12"
                  ",width=" + std::to_string(st.width) +
                  ",height=" + std::to_string(st.height);
    }
    // NV12 source: skip conversion — feed directly to mpph264enc

    // --- Encode: MPP hardware H.264 encoder ---
    if (rtsp.encoder == "mpp") {
        // mpph264enc: Rockchip MPP (Media Process Platform) hardware encoder.
        // bps = bits per second, gop = group-of-pictures (keyframe interval).
        launch += " ! mpph264enc"
                  " bps=" + std::to_string(rtsp.bitrate_kbps * 1000) +
                  " gop=" + std::to_string(rtsp.gop);
    } else {
        // Software fallback (for testing on non-Rockchip hardware)
        launch += " ! videoconvert ! video/x-raw,format=I420"
                  " ! x264enc tune=zerolatency speed-preset=ultrafast"
                  " bitrate=" + std::to_string(rtsp.bitrate_kbps) +
                  " key-int-max=" + std::to_string(rtsp.gop);
    }

    // --- Parse + RTP payload ---
    // config-interval=1: embed SPS/PPS in every IDR for quick stream join
    launch += " ! h264parse config-interval=1"
              " ! rtph264pay name=pay0 pt=96 config-interval=1"
              " )";

    return launch;
}

// ---------------------------------------------------------------------------
// RTSP server lifecycle
// ---------------------------------------------------------------------------

static void main_loop_func(RtspServer* srv) {
    SC_LOG_INFO("RTSP main loop started");
    g_main_loop_run(srv->loop);
    SC_LOG_INFO("RTSP main loop exited");
    srv->running = false;
}

RtspServer* rtsp_server_start(const Config& cfg) {
    // Ensure GStreamer RGA plugin is discoverable
    const char* existing = g_getenv("GST_PLUGIN_PATH");
    std::string plugin_path = SOULCAM_RGA_PLUGIN_PATH;
    if (existing && existing[0] != '\0') {
        plugin_path = plugin_path + ":" + existing;
    }
    g_setenv("GST_PLUGIN_PATH", plugin_path.c_str(), TRUE);
    SC_LOG_DEBUG("GST_PLUGIN_PATH=%s", plugin_path.c_str());

    auto* srv = new RtspServer();

    srv->loop = g_main_loop_new(nullptr, FALSE);
    srv->server = gst_rtsp_server_new();
    gst_rtsp_server_set_service(srv->server,
                                 std::to_string(cfg.rtsp.port).c_str());

    // Build the factory with our HW-accelerated pipeline
    std::string launch = rtsp_build_launch(cfg);
    SC_LOG_INFO("RTSP launch: %s", launch.c_str());

    srv->factory = gst_rtsp_media_factory_new();
    gst_rtsp_media_factory_set_shared(srv->factory, TRUE);
    gst_rtsp_media_factory_set_launch(srv->factory, launch.c_str());

    // Set transport mode: allow both UDP and TCP
    gst_rtsp_media_factory_set_protocols(srv->factory,
        (GstRTSPLowerTrans)(GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_TCP));

    // Mount the factory
    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(srv->server);
    gst_rtsp_mount_points_add_factory(mounts, cfg.rtsp.mount.c_str(), srv->factory);
    g_object_unref(mounts);

    // Hook overlay (if enabled)
    if (cfg.enable_overlay && cfg.enable_ai) {
        overlay_setup_factory(reinterpret_cast<GstElement*>(srv->factory),
                              cfg.stream.width, cfg.stream.height);
    }

    // Attach to default GLib main context
    guint id = gst_rtsp_server_attach(srv->server, nullptr);
    if (id == 0) {
        SC_LOG_ERROR("Failed to attach RTSP server");
        g_main_loop_unref(srv->loop);
        g_object_unref(srv->server);
        delete srv;
        return nullptr;
    }

    srv->running = true;
    srv->thread = std::thread(main_loop_func, srv);

    SC_LOG_INFO("RTSP server listening on rtsp://0.0.0.0:%d%s",
                cfg.rtsp.port, cfg.rtsp.mount.c_str());

    return srv;
}

void rtsp_server_stop(RtspServer* srv) {
    if (!srv) return;

    SC_LOG_INFO("Stopping RTSP server...");

    if (srv->loop && g_main_loop_is_running(srv->loop)) {
        g_main_loop_quit(srv->loop);
    }
    if (srv->thread.joinable()) {
        srv->thread.join();
    }
    if (srv->server) {
        g_object_unref(srv->server);
    }
    if (srv->loop) {
        g_main_loop_unref(srv->loop);
    }

    delete srv;
    SC_LOG_INFO("RTSP server stopped");
}

GMainLoop* rtsp_server_get_loop(RtspServer* srv) {
    return srv ? srv->loop : nullptr;
}

}  // namespace sc
