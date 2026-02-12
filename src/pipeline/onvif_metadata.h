#pragma once
// ============================================================================
// ONVIF Analytics Metadata
//
// Formats AI detection results as ONVIF-compliant XML metadata and serves
// them via a secondary RTSP stream.  This enables standards-compliant
// integration with NVRs and VMS (Milestone, Genetec, etc.).
//
// ONVIF metadata schema: http://www.onvif.org/ver10/schema
//
// Two integration modes:
//   1. RTSP metadata stream: /cam/meta  (RTP-payloaded ONVIF XML)
//   2. HTTP query: scene_hub.py /onvif/metadata  (pull-based)
//
// The RTSP metadata stream carries detection events as ONVIF
// tt:MetadataStream / tt:VideoAnalytics / tt:Frame / tt:Object.
//
// Coordinates use the ONVIF normalized coordinate system:
//   x: -1.0 (left) to +1.0 (right)
//   y: -1.0 (top)  to +1.0 (bottom)
// ============================================================================

#include "soulcam.h"
#include <vector>
#include <string>

namespace sc {

// Format a set of detections as an ONVIF tt:MetadataStream XML document.
// The coordinates are normalized to [-1,+1] from the frame dimensions.
std::string onvif_format_xml(const std::vector<Detection>& dets,
                              int frame_w, int frame_h);

// Opaque handle to the metadata RTSP stream.
struct OnvifStream;

// Create and start the ONVIF metadata RTSP stream.
// This mounts a secondary RTSP endpoint (default: /cam/meta) that
// carries ONVIF XML metadata over RTP.
// The RTSP server must already be running.
OnvifStream* onvif_stream_start(const Config& cfg);

// Push new detection data to the ONVIF metadata stream.
// Call this from the AI detection callback.
void onvif_stream_push(OnvifStream* stream,
                        const std::vector<Detection>& dets,
                        int frame_w, int frame_h);

// Stop and destroy the ONVIF metadata stream.
void onvif_stream_stop(OnvifStream* stream);

}  // namespace sc
