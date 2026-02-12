// ============================================================================
// ONVIF Analytics Metadata implementation
//
// Generates ONVIF-compliant XML metadata from AI detections and serves
// them via a GStreamer RTSP appsrc stream.
//
// Key design decisions:
//   1. Separate RTSP mount (/cam/meta) -- does not interfere with video
//   2. appsrc-based -- we push XML buffers whenever new detections arrive
//   3. ONVIF normalized coordinates: x,y in [-1,+1]
//   4. Lightweight: no external XML library, just snprintf
//   5. Heartbeat: empty frames pushed at 1Hz to keep the stream alive
// ============================================================================

#include "pipeline/onvif_metadata.h"
#include "util/logger.h"

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/app/gstappsrc.h>

#include <cstring>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <mutex>
#include <thread>
#include <atomic>

namespace sc {

// ---------------------------------------------------------------------------
// ONVIF XML formatting
// ---------------------------------------------------------------------------

// Get current UTC time as ISO 8601 string
static std::string utc_now() {
    time_t t = time(nullptr);
    struct tm tm{};
    gmtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string onvif_format_xml(const std::vector<Detection>& dets,
                              int frame_w, int frame_h) {
    std::ostringstream xml;

    std::string ts = utc_now();

    xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<tt:MetadataStream xmlns:tt=\"http://www.onvif.org/ver10/schema\">\n"
        << "  <tt:VideoAnalytics>\n"
        << "    <tt:Frame UtcTime=\"" << ts << "\">\n";

    // Transformation: maps pixel coords to ONVIF normalized [-1,+1]
    // ONVIF uses a coordinate system where:
    //   (-1,-1) = top-left, (+1,+1) = bottom-right
    xml << "      <tt:Transformation>\n"
        << "        <tt:Translate x=\"-1.0\" y=\"-1.0\"/>\n"
        << "        <tt:Scale x=\"" << std::fixed << std::setprecision(6)
        << (2.0 / frame_w) << "\" y=\"" << (2.0 / frame_h) << "\"/>\n"
        << "      </tt:Transformation>\n";

    int obj_id = 1;
    for (const auto& d : dets) {
        // Convert pixel coordinates to ONVIF normalized [-1,+1]
        double left   = (2.0 * d.left   / frame_w) - 1.0;
        double top    = (2.0 * d.top    / frame_h) - 1.0;
        double right  = (2.0 * d.right  / frame_w) - 1.0;
        double bottom = (2.0 * d.bottom / frame_h) - 1.0;

        xml << "      <tt:Object ObjectId=\"" << obj_id++ << "\">\n"
            << "        <tt:Appearance>\n"
            << "          <tt:Shape>\n"
            << "            <tt:BoundingBox"
            << " left=\""   << std::setprecision(4) << left   << "\""
            << " top=\""    << top    << "\""
            << " right=\""  << right  << "\""
            << " bottom=\"" << bottom << "\""
            << "/>\n"
            << "          </tt:Shape>\n"
            << "          <tt:Class>\n"
            << "            <tt:Type Likelihood=\""
            << std::setprecision(3) << d.confidence << "\">"
            << (d.label ? d.label : "unknown")
            << "</tt:Type>\n"
            << "          </tt:Class>\n"
            << "        </tt:Appearance>\n"
            << "      </tt:Object>\n";
    }

    xml << "    </tt:Frame>\n"
        << "  </tt:VideoAnalytics>\n"
        << "</tt:MetadataStream>\n";

    return xml.str();
}

// ---------------------------------------------------------------------------
// ONVIF metadata RTSP stream
// ---------------------------------------------------------------------------

struct OnvifStream {
    GstRTSPServer*       server  = nullptr;
    GstRTSPMediaFactory* factory = nullptr;
    GMainLoop*           loop    = nullptr;
    std::thread          thread;
    std::atomic<bool>    running{false};

    // Shared state for pushing metadata to appsrc
    std::mutex           mtx;
    std::string          latest_xml;
    bool                 has_new_data = false;

    // Config
    int                  port = 8554;
    std::string          mount = "/cam/meta";
};

// appsrc need-data callback: push latest metadata XML
static void on_need_data(GstAppSrc* src, guint /*length*/, gpointer user_data) {
    auto* stream = static_cast<OnvifStream*>(user_data);

    std::string xml;
    {
        std::lock_guard<std::mutex> lock(stream->mtx);
        if (stream->has_new_data) {
            xml = stream->latest_xml;
            stream->has_new_data = false;
        }
    }

    if (xml.empty()) {
        // Heartbeat: push an empty metadata frame to keep stream alive
        xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              "<tt:MetadataStream xmlns:tt=\"http://www.onvif.org/ver10/schema\">\n"
              "  <tt:VideoAnalytics>\n"
              "    <tt:Frame UtcTime=\"" + utc_now() + "\"/>\n"
              "  </tt:VideoAnalytics>\n"
              "</tt:MetadataStream>\n";
    }

    GstBuffer* buf = gst_buffer_new_allocate(nullptr, xml.size(), nullptr);
    GstMapInfo map;
    gst_buffer_map(buf, &map, GST_MAP_WRITE);
    memcpy(map.data, xml.c_str(), xml.size());
    gst_buffer_unmap(buf, &map);

    // Timestamp the buffer (1 second intervals)
    static guint64 ts = 0;
    GST_BUFFER_PTS(buf) = ts;
    GST_BUFFER_DURATION(buf) = GST_SECOND;
    ts += GST_SECOND;

    gst_app_src_push_buffer(src, buf);
}

// Configure the media when a client connects
static void on_media_configure(GstRTSPMediaFactory* /*factory*/,
                                GstRTSPMedia* media,
                                gpointer user_data) {
    auto* stream = static_cast<OnvifStream*>(user_data);

    GstElement* element = gst_rtsp_media_get_element(media);
    if (!element) return;

    GstElement* appsrc = gst_bin_get_by_name(GST_BIN(element), "meta_src");
    if (appsrc) {
        // Configure appsrc
        g_object_set(appsrc,
                     "format", GST_FORMAT_TIME,
                     "is-live", TRUE,
                     "min-latency", (gint64)0,
                     nullptr);

        // Set caps for the appsrc
        GstCaps* caps = gst_caps_new_simple("application/x-onvif-metadata+xml",
                                             nullptr, nullptr);
        gst_app_src_set_caps(GST_APP_SRC(appsrc), caps);
        gst_caps_unref(caps);

        // Connect need-data callback
        GstAppSrcCallbacks callbacks = {};
        callbacks.need_data = on_need_data;
        gst_app_src_set_callbacks(GST_APP_SRC(appsrc), &callbacks, stream, nullptr);

        SC_LOG_DEBUG("ONVIF: appsrc configured for metadata stream");
        gst_object_unref(appsrc);
    } else {
        SC_LOG_WARN("ONVIF: meta_src element not found in pipeline");
    }

    gst_object_unref(element);
}

static void meta_loop_func(OnvifStream* stream) {
    SC_LOG_INFO("ONVIF metadata stream loop started");
    g_main_loop_run(stream->loop);
    SC_LOG_INFO("ONVIF metadata stream loop exited");
    stream->running = false;
}

OnvifStream* onvif_stream_start(const Config& cfg) {
    auto* stream = new OnvifStream();
    stream->port  = cfg.rtsp.port;
    stream->mount = "/cam/meta";

    // Get the existing RTSP server (we'll add a second mount point)
    // For simplicity, we create our own factory on the same server port.
    // The GstRTSPServer is already running in main -- we just need to
    // add a factory to its mount points.

    // Build the metadata pipeline:
    //   appsrc (XML) -> text/xml payload -> RTP
    std::string launch =
        "( appsrc name=meta_src is-live=true format=time"
        " ! application/x-onvif-metadata+xml"
        " ! rtpgstpay name=pay0 pt=107"
        " )";

    SC_LOG_INFO("ONVIF: metadata pipeline: %s", launch.c_str());

    stream->factory = gst_rtsp_media_factory_new();
    gst_rtsp_media_factory_set_shared(stream->factory, TRUE);
    gst_rtsp_media_factory_set_launch(stream->factory, launch.c_str());

    // Hook media-configure to set up appsrc callbacks
    g_signal_connect(stream->factory, "media-configure",
                     G_CALLBACK(on_media_configure), stream);

    // We need access to the existing server's mount points.
    // Create a new server on a different port for the metadata stream.
    stream->server = gst_rtsp_server_new();
    gst_rtsp_server_set_service(stream->server,
                                 std::to_string(cfg.rtsp.port + 1).c_str());

    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(stream->server);
    gst_rtsp_mount_points_add_factory(mounts, stream->mount.c_str(), stream->factory);
    g_object_unref(mounts);

    stream->loop = g_main_loop_new(nullptr, FALSE);

    guint id = gst_rtsp_server_attach(stream->server, nullptr);
    if (id == 0) {
        SC_LOG_ERROR("ONVIF: failed to attach metadata RTSP server");
        g_main_loop_unref(stream->loop);
        g_object_unref(stream->server);
        delete stream;
        return nullptr;
    }

    stream->running = true;
    stream->thread = std::thread(meta_loop_func, stream);

    SC_LOG_INFO("ONVIF metadata stream: rtsp://0.0.0.0:%d%s",
                cfg.rtsp.port + 1, stream->mount.c_str());

    return stream;
}

void onvif_stream_push(OnvifStream* stream,
                        const std::vector<Detection>& dets,
                        int frame_w, int frame_h) {
    if (!stream) return;

    std::string xml = onvif_format_xml(dets, frame_w, frame_h);

    std::lock_guard<std::mutex> lock(stream->mtx);
    stream->latest_xml  = std::move(xml);
    stream->has_new_data = true;
}

void onvif_stream_stop(OnvifStream* stream) {
    if (!stream) return;

    SC_LOG_INFO("Stopping ONVIF metadata stream...");

    if (stream->loop && g_main_loop_is_running(stream->loop)) {
        g_main_loop_quit(stream->loop);
    }
    if (stream->thread.joinable()) {
        stream->thread.join();
    }
    if (stream->server) {
        g_object_unref(stream->server);
    }
    if (stream->loop) {
        g_main_loop_unref(stream->loop);
    }

    delete stream;
    SC_LOG_INFO("ONVIF metadata stream stopped");
}

}  // namespace sc
