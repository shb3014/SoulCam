#pragma once
// ============================================================================
// Fully hardware-accelerated RTSP server
//
// Pipeline (zero CPU pixel work):
//
//   v4l2src (/dev/video8, UYVY)
//     -> queue (decouple capture thread)
//     -> rgaconvert (UYVY -> NV12, RGA 2D engine -- hardware)
//     -> mpph264enc (NV12 -> H.264, Hantro/MPP -- hardware)
//     -> h264parse
//     -> rtph264pay
//     -> RTSP
//
// All pixel-domain operations (color convert + encode) are done by dedicated
// hardware engines.  The CPU only handles:
//   - V4L2 QBUF/DQBUF ioctls (DMA buffer passing)
//   - RTP payloading (lightweight memcpy)
//   - RTSP signaling (TCP session management)
//
// Comparison with the previous Python pipeline:
//   Old: v4l2src -> shmsink -> shmsrc -> videoconvert(SW!) -> mpph264enc -> RTSP
//   New: v4l2src -> rgaconvert(HW) -> mpph264enc(HW) -> RTSP
//
// The "shm hop" is eliminated too -- single pipeline, no IPC overhead.
// ============================================================================

#include "soulcam.h"
#include <gst/gst.h>

namespace sc {

// Opaque handle to the RTSP server.
struct RtspServer;

// Create and start the RTSP server.  Blocks until the server is listening.
// Returns nullptr on failure.
RtspServer* rtsp_server_start(const Config& cfg);

// Stop and destroy the RTSP server.
void rtsp_server_stop(RtspServer* srv);

// Get the GLib main loop (for integration with other GLib event sources).
GMainLoop* rtsp_server_get_loop(RtspServer* srv);

// Build the GStreamer launch string for the RTSP factory.
// Exposed for testing / logging.
std::string rtsp_build_launch(const Config& cfg);

// Returns true if the RTSP media pipeline is healthy (PLAYING state).
// Returns false if no media exists yet (no client has connected) or
// the pipeline has entered ERROR/NULL state.
bool rtsp_server_is_healthy(RtspServer* srv);

// Force-recreate the RTSP media factory, tearing down any existing
// media/pipeline.  Use after is_healthy() returns false to recover.
bool rtsp_server_reset(RtspServer* srv);

}  // namespace sc
