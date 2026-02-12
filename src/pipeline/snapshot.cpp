// ============================================================================
// JPEG snapshot capture implementation
//
// One-shot GStreamer pipeline for JPEG capture:
//   v4l2src num-buffers=1 → video/x-raw,format=NV12 → jpegenc → appsink
//
// The pipeline is created, run to EOS, then destroyed per capture.
// Typical latency: ~100-200ms (dominated by V4L2 stream-on + first frame).
//
// HTTP server: minimal single-threaded HTTP/1.1 server using POSIX sockets.
// Only handles GET /snapshot and GET /snapshot/info.  No dependencies beyond
// POSIX and GStreamer.
// ============================================================================

#include "pipeline/snapshot.h"
#include "util/logger.h"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <thread>
#include <atomic>
#include <mutex>
#include <cstring>
#include <cstdio>
#include <sstream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace sc {

// ---------------------------------------------------------------------------
// One-shot JPEG capture via GStreamer
// ---------------------------------------------------------------------------

int snapshot_capture(const char* device, int width, int height,
                     int quality, std::vector<uint8_t>& out_jpeg) {
    out_jpeg.clear();

    // Build a one-shot pipeline:
    //   v4l2src device=<dev> num-buffers=1
    //     ! video/x-raw,format=NV12,width=W,height=H
    //     ! videoconvert
    //     ! video/x-raw,format=I420
    //     ! jpegenc quality=Q
    //     ! appsink name=sink
    //
    // We use videoconvert (SW) here because this is a one-shot capture
    // and the overhead is negligible for a single frame.  For continuous
    // capture, we'd use rgaconvert (HW).

    char pipeline_str[1024];
    snprintf(pipeline_str, sizeof(pipeline_str),
        "v4l2src device=%s num-buffers=1 "
        "! video/x-raw,format=NV12,width=%d,height=%d "
        "! videoconvert "
        "! video/x-raw,format=I420 "
        "! jpegenc quality=%d "
        "! appsink name=sink",
        device, width, height, quality);

    GError* err = nullptr;
    GstElement* pipeline = gst_parse_launch(pipeline_str, &err);
    if (!pipeline) {
        SC_LOG_ERROR("Snapshot: failed to create pipeline: %s",
                     err ? err->message : "unknown");
        if (err) g_error_free(err);
        return -1;
    }
    if (err) {
        SC_LOG_WARN("Snapshot: pipeline warning: %s", err->message);
        g_error_free(err);
    }

    // Get appsink
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (!sink) {
        SC_LOG_ERROR("Snapshot: appsink not found in pipeline");
        gst_object_unref(pipeline);
        return -1;
    }

    // Configure appsink: no signal emission, pull-based
    gst_app_sink_set_emit_signals(GST_APP_SINK(sink), FALSE);
    gst_app_sink_set_max_buffers(GST_APP_SINK(sink), 1);
    gst_app_sink_set_drop(GST_APP_SINK(sink), FALSE);

    // Run pipeline
    GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        SC_LOG_ERROR("Snapshot: pipeline failed to start");
        gst_object_unref(sink);
        gst_object_unref(pipeline);
        return -1;
    }

    int result = -1;

    // Pull the JPEG sample directly from appsink with a timeout.
    // With num-buffers=1, the pipeline produces exactly one buffer then EOS.
    // We must pull the sample BEFORE EOS flushes the appsink queue.
    // try_pull_sample blocks until a sample arrives or the timeout expires.
    GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink),
                                                      5 * GST_SECOND);
    if (sample) {
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        if (buffer) {
            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                out_jpeg.assign(map.data, map.data + map.size);
                gst_buffer_unmap(buffer, &map);
                result = 0;
                SC_LOG_INFO("Snapshot: captured %zu bytes JPEG (%dx%d q=%d) from %s",
                            out_jpeg.size(), width, height, quality, device);
            }
        }
        gst_sample_unref(sample);
    } else {
        // Check if there was an error
        GstBus* bus = gst_element_get_bus(pipeline);
        GstMessage* msg = gst_bus_pop_filtered(bus,
            static_cast<GstMessageType>(GST_MESSAGE_ERROR));
        if (msg) {
            GError* gerr = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(msg, &gerr, &debug);
            SC_LOG_ERROR("Snapshot: pipeline error: %s (%s)",
                         gerr ? gerr->message : "?", debug ? debug : "");
            if (gerr) g_error_free(gerr);
            if (debug) g_free(debug);
            gst_message_unref(msg);
        } else {
            SC_LOG_ERROR("Snapshot: pipeline timed out (5s)");
        }
        gst_object_unref(bus);
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(pipeline);

    return result;
}

int snapshot_capture(const Config& cfg, std::vector<uint8_t>& out_jpeg) {
    // Always use selfpath for snapshots.
    //
    // The mainpath (/dev/video8) is held by the RTSP GStreamer pipeline and
    // cannot be opened by a second V4L2 consumer simultaneously.
    //
    // The selfpath (/dev/video9) has its own ISP scaler and can be opened
    // independently.  When AI is running, the selfpath is at ai.width×ai.height.
    // When AI is not running and --snapshot is enabled, main.cpp ensures the
    // selfpath ISP is configured at stream.width×stream.height (or a smaller size).
    //
    // Selfpath resolution for snapshot:
    //   AI active:  use AI dimensions (640×480 typically)
    //   AI off:     use AI dimensions as default (configured by main.cpp)
    int snap_w = cfg.ai.width;
    int snap_h = cfg.ai.height;

    return snapshot_capture(cfg.isp.selfpath.c_str(), snap_w, snap_h, 85, out_jpeg);
}

// ---------------------------------------------------------------------------
// Minimal HTTP server for /snapshot endpoint
// ---------------------------------------------------------------------------

struct SnapshotServer {
    Config              cfg;
    int                 port        = 8088;
    int                 listen_fd   = -1;
    std::thread         thread;
    std::atomic<bool>   running{false};
};

// Handle a single HTTP request
static void handle_client(SnapshotServer* srv, int client_fd) {
    char buf[2048];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) { close(client_fd); return; }
    buf[n] = '\0';

    // Parse request line
    char method[16] = {}, path[256] = {};
    sscanf(buf, "%15s %255s", method, path);

    if (strcmp(method, "GET") != 0) {
        const char* resp = "HTTP/1.1 405 Method Not Allowed\r\n"
                           "Content-Length: 0\r\n\r\n";
        send(client_fd, resp, strlen(resp), 0);
        close(client_fd);
        return;
    }

    if (strcmp(path, "/snapshot") == 0 || strcmp(path, "/snapshot.jpg") == 0) {
        // Capture JPEG
        std::vector<uint8_t> jpeg;
        int rc = snapshot_capture(srv->cfg, jpeg);

        if (rc == 0 && !jpeg.empty()) {
            char header[512];
            snprintf(header, sizeof(header),
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %zu\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: close\r\n"
                "\r\n", jpeg.size());
            send(client_fd, header, strlen(header), 0);
            send(client_fd, jpeg.data(), jpeg.size(), 0);
        } else {
            const char* body = "{\"error\":\"capture failed\"}";
            char header[256];
            snprintf(header, sizeof(header),
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %zu\r\n"
                "Connection: close\r\n"
                "\r\n", strlen(body));
            send(client_fd, header, strlen(header), 0);
            send(client_fd, body, strlen(body), 0);
        }
    } else if (strcmp(path, "/snapshot/info") == 0) {
        std::ostringstream json;
        json << "{\"device\":\"" << srv->cfg.isp.selfpath << "\""
             << ",\"width\":" << srv->cfg.ai.width
             << ",\"height\":" << srv->cfg.ai.height
             << ",\"quality\":85"
             << ",\"format\":\"JPEG\""
             << ",\"ai_active\":" << (srv->cfg.enable_ai ? "true" : "false")
             << "}";
        std::string body = json.str();
        char header[256];
        snprintf(header, sizeof(header),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n", body.size());
        send(client_fd, header, strlen(header), 0);
        send(client_fd, body.c_str(), body.size(), 0);
    } else {
        const char* body = "Not Found";
        char header[256];
        snprintf(header, sizeof(header),
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: %zu\r\n"
            "Connection: close\r\n"
            "\r\n", strlen(body));
        send(client_fd, header, strlen(header), 0);
        send(client_fd, body, strlen(body), 0);
    }

    close(client_fd);
}

static void server_thread(SnapshotServer* srv) {
    SC_LOG_INFO("Snapshot HTTP server listening on port %d", srv->port);

    while (srv->running) {
        // Accept with timeout (using poll)
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(srv->listen_fd, &readfds);

        struct timeval tv = {1, 0};  // 1 second timeout
        int sel = select(srv->listen_fd + 1, &readfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(srv->listen_fd,
                                reinterpret_cast<sockaddr*>(&client_addr),
                                &client_len);
        if (client_fd < 0) continue;

        // Set recv timeout
        struct timeval recv_tv = {3, 0};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &recv_tv, sizeof(recv_tv));

        handle_client(srv, client_fd);
    }

    SC_LOG_INFO("Snapshot HTTP server stopped");
}

SnapshotServer* snapshot_server_start(const Config& cfg, int port) {
    auto* srv = new SnapshotServer();
    srv->cfg = cfg;
    srv->port = port;

    // Create TCP socket
    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) {
        SC_LOG_ERROR("Snapshot server: socket() failed");
        delete srv;
        return nullptr;
    }

    int opt = 1;
    setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(srv->listen_fd, reinterpret_cast<const sockaddr*>(&addr),
             sizeof(addr)) < 0) {
        SC_LOG_ERROR("Snapshot server: bind() failed on port %d", port);
        close(srv->listen_fd);
        delete srv;
        return nullptr;
    }

    if (listen(srv->listen_fd, 4) < 0) {
        SC_LOG_ERROR("Snapshot server: listen() failed");
        close(srv->listen_fd);
        delete srv;
        return nullptr;
    }

    srv->running = true;
    srv->thread = std::thread(server_thread, srv);

    return srv;
}

void snapshot_server_stop(SnapshotServer* srv) {
    if (!srv) return;

    srv->running = false;
    if (srv->listen_fd >= 0) {
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
    if (srv->thread.joinable()) {
        srv->thread.join();
    }
    delete srv;
}

}  // namespace sc
