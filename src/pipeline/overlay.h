#pragma once
// ============================================================================
// Detection overlay for RTSP stream
//
// Thread-safe shared detection state that bridges AI detections
// (from selfpath/RKNN) to the RTSP stream overlay (via cairooverlay).
//
// When overlay is enabled, the RTSP pipeline becomes:
//   v4l2src(UYVY) → rgaconvert(→BGRA) → cairooverlay → rgaconvert(→NV12)
//                                          ↑ draw boxes
//                                       → mpph264enc → RTSP
//
// The cairooverlay draw callback reads the latest detections and renders
// bounding boxes + labels on the BGRA frame.
// ============================================================================

#include "soulcam.h"
#include <vector>
#include <mutex>
#include <gst/gst.h>

namespace sc {

// Thread-safe shared detection state
struct SharedDetections {
    std::mutex              mtx;
    std::vector<Detection>  dets;
    int                     src_w = 640;   // AI model space width
    int                     src_h = 640;   // AI model space height
};

// Global shared detections (set from AI callback, read from overlay draw)
SharedDetections& overlay_shared_dets();

// Update the shared detections (call from AI callback thread)
void overlay_update(const std::vector<Detection>& dets, int src_w, int src_h);

// Hook the cairooverlay element on an RTSP media factory.
// Call after the factory is created but before clients connect.
// stream_w/h: the RTSP stream dimensions (for coordinate scaling).
void overlay_setup_factory(GstElement* factory, int stream_w, int stream_h);

}  // namespace sc
