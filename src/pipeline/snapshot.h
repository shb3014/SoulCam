#pragma once
// ============================================================================
// JPEG snapshot capture
//
// Provides a single-frame JPEG capture from the ISP, served via a minimal
// HTTP endpoint.  Used for:
//   - Tuya event notification thumbnails (NOTIFICATION_CONTENT_JPEG)
//   - Standalone HTTP snapshot (e.g., home automation dashboards)
//   - ONVIF GetSnapshotUri (if ONVIF is enabled)
//
// Architecture:
//   Request → one-shot GStreamer pipeline (v4l2src num-buffers=1 → jpegenc)
//           → JPEG buffer → HTTP response / in-memory buffer
//
// The snapshot pipeline uses selfpath (/dev/video9) when AI is running,
// or mainpath (/dev/video8) with a one-shot grab when AI is not active.
// The mainpath one-shot works because GstRtspMediaFactory only holds the
// device while a client is connected + a few seconds of keepalive.
//
// Thread safety: snapshot_capture() blocks the caller for ~100-200ms
// (V4L2 capture + JPEG encode).  The HTTP server handles requests
// sequentially on its own thread.
// ============================================================================

#include "soulcam.h"
#include <cstdint>
#include <vector>

namespace sc {

// Opaque handle for the snapshot HTTP server
struct SnapshotServer;

// Capture a single JPEG snapshot.  Returns the JPEG data in `out_jpeg`.
// Uses a one-shot GStreamer pipeline: v4l2src → jpegenc → appsink.
// `device`:  V4L2 device path (e.g., /dev/video9 for selfpath)
// `width`:   capture width
// `height`:  capture height
// `quality`: JPEG quality 1-100 (default: 85)
// Returns 0 on success, -1 on failure.
int snapshot_capture(const char* device, int width, int height,
                     int quality, std::vector<uint8_t>& out_jpeg);

// Convenience: capture using config defaults (selfpath if AI enabled,
// otherwise mainpath).
int snapshot_capture(const Config& cfg, std::vector<uint8_t>& out_jpeg);

// Start the snapshot HTTP server on `port`.
// GET /snapshot      → returns JPEG image (Content-Type: image/jpeg)
// GET /snapshot/info → returns JSON with capture parameters
// Returns nullptr on failure.
SnapshotServer* snapshot_server_start(const Config& cfg, int port);

// Stop the snapshot HTTP server.
void snapshot_server_stop(SnapshotServer* srv);

}  // namespace sc
