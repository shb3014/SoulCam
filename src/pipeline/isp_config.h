#pragma once
// ============================================================================
// ISP dual-path configuration
//
// Configures the rkisp v21 media graph so BOTH paths run simultaneously:
//
//   Sensor (OV5647 Bayer SGBRG10)
//     -> CSI-DPHY -> rkisp-csi-subdev -> rkisp-isp-subdev (pad0 sink)
//     -> ISP HW processing (demosaic, AWB, AE, CCM, gamma, ...)
//     -> pad2 source -> mainpath /dev/video8  (UYVY 1280x960  -> RTSP)
//     -> selfpath     /dev/video9  (NV12  640x480   -> AI)
//
// The ISP hardware has two independent output scalers:
//   - mainpath:  large buffer, up to full sensor resolution
//   - selfpath:  smaller buffer, independent resolution/format
// Both share the same ISP processing (3A, AWB, CCM, etc.)
//
// This eliminates the need for:
//   - Software tee/split
//   - Software scaling (videoscale)
//   - Extra RGA conversion for the AI path
// ============================================================================

#include "soulcam.h"

namespace sc {

// Configure the full ISP media graph for dual-path operation.
// Returns 0 on success, -1 on error.
int isp_configure(const Config& cfg);

// Configure only mainpath (for RTSP-only mode without AI).
int isp_configure_mainpath(const Config& cfg);

// Query current ISP mainpath format (for verification).
// Returns 0 on success.
int isp_query_mainpath(const Config& cfg);

}  // namespace sc
