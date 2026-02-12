// ============================================================================
// Detection overlay implementation
//
// Uses GStreamer cairooverlay to draw detection bounding boxes + labels
// on the RTSP stream. The draw callback reads the latest detections
// from the AI thread via a mutex-protected shared buffer.
//
// Requires: libcairo2-dev, gstreamer1.0-plugins-good (cairooverlay)
// ============================================================================

#include "pipeline/overlay.h"
#include "util/logger.h"

#include <cstring>
#include <algorithm>

#if SOULCAM_HAVE_CAIRO
#include <cairo/cairo.h>
#include <gst/rtsp-server/rtsp-server.h>
#endif

namespace sc {

// ---------------------------------------------------------------------------
// Shared detection state (singleton)
// ---------------------------------------------------------------------------

static SharedDetections g_shared;

SharedDetections& overlay_shared_dets() { return g_shared; }

void overlay_update(const std::vector<Detection>& dets, int src_w, int src_h) {
    std::lock_guard<std::mutex> lock(g_shared.mtx);
    g_shared.dets  = dets;
    g_shared.src_w = src_w;
    g_shared.src_h = src_h;
}

// ---------------------------------------------------------------------------
// Cairo draw callback
// ---------------------------------------------------------------------------

#if SOULCAM_HAVE_CAIRO

struct OverlayCtx {
    int stream_w = 1280;
    int stream_h = 960;
};

// Color palette for different classes (RGBA, 0-1 range)
struct Color { double r, g, b; };
static const Color PALETTE[] = {
    {0.0, 1.0, 0.3},   // green  (person)
    {1.0, 0.3, 0.0},   // red-orange
    {0.0, 0.6, 1.0},   // blue
    {1.0, 1.0, 0.0},   // yellow
    {1.0, 0.0, 1.0},   // magenta
    {0.0, 1.0, 1.0},   // cyan
    {1.0, 0.5, 0.0},   // orange
    {0.5, 0.0, 1.0},   // purple
};
static constexpr int PALETTE_SIZE = sizeof(PALETTE) / sizeof(PALETTE[0]);

static void on_draw(GstElement* /*overlay*/, cairo_t* cr,
                     guint64 /*timestamp*/, guint64 /*duration*/,
                     gpointer user_data) {
    auto* ctx = static_cast<OverlayCtx*>(user_data);

    // Snapshot the current detections
    std::vector<Detection> dets;
    int src_w, src_h;
    {
        std::lock_guard<std::mutex> lock(g_shared.mtx);
        dets  = g_shared.dets;
        src_w = g_shared.src_w;
        src_h = g_shared.src_h;
    }

    if (dets.empty() || src_w <= 0 || src_h <= 0) return;

    // Scale from AI model space to RTSP stream space
    double sx = static_cast<double>(ctx->stream_w) / src_w;
    double sy = static_cast<double>(ctx->stream_h) / src_h;

    cairo_set_line_width(cr, 2.5);

    for (const auto& d : dets) {
        const Color& col = PALETTE[d.cls_id % PALETTE_SIZE];

        double x = d.left   * sx;
        double y = d.top    * sy;
        double w = (d.right - d.left) * sx;
        double h = (d.bottom - d.top) * sy;

        // Draw bounding box
        cairo_set_source_rgba(cr, col.r, col.g, col.b, 0.85);
        cairo_rectangle(cr, x, y, w, h);
        cairo_stroke(cr);

        // Draw label background
        char label[128];
        snprintf(label, sizeof(label), "%s %.0f%%",
                 d.label ? d.label : "?", d.confidence * 100);

        cairo_set_font_size(cr, 16);
        cairo_text_extents_t ext;
        cairo_text_extents(cr, label, &ext);

        double lx = x;
        double ly = y - ext.height - 6;
        if (ly < 0) ly = y + 2;  // below box if near top edge

        cairo_set_source_rgba(cr, col.r, col.g, col.b, 0.65);
        cairo_rectangle(cr, lx, ly, ext.width + 8, ext.height + 6);
        cairo_fill(cr);

        // Draw label text
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
        cairo_move_to(cr, lx + 4, ly + ext.height + 2);
        cairo_show_text(cr, label);
    }
}

// ---------------------------------------------------------------------------
// RTSP media-configure hook
// ---------------------------------------------------------------------------

static void on_media_configure(GstRTSPMediaFactory* /*factory*/,
                                GstRTSPMedia* media,
                                gpointer user_data) {
    auto* ctx = static_cast<OverlayCtx*>(user_data);

    GstElement* element = gst_rtsp_media_get_element(media);
    if (!element) return;

    GstElement* overlay = gst_bin_get_by_name(GST_BIN(element), "overlay");
    if (overlay) {
        g_signal_connect(overlay, "draw", G_CALLBACK(on_draw), ctx);
        SC_LOG_DEBUG("Overlay: hooked cairooverlay draw signal");
        gst_object_unref(overlay);
    } else {
        SC_LOG_WARN("Overlay: cairooverlay element 'overlay' not found in pipeline");
    }
    gst_object_unref(element);
}

void overlay_setup_factory(GstElement* factory, int stream_w, int stream_h) {
    auto* ctx = new OverlayCtx();  // leaked intentionally (lives for app lifetime)
    ctx->stream_w = stream_w;
    ctx->stream_h = stream_h;

    g_signal_connect(factory, "media-configure",
                     G_CALLBACK(on_media_configure), ctx);
    SC_LOG_INFO("Overlay: enabled on RTSP stream (%dx%d)", stream_w, stream_h);
}

#else  // no cairo

void overlay_setup_factory(GstElement*, int, int) {
    SC_LOG_WARN("Overlay: Cairo not available (install libcairo2-dev)");
}

#endif  // SOULCAM_HAVE_CAIRO

}  // namespace sc
