// ============================================================================
// ISP dual-path configuration implementation
//
// Uses media-ctl and v4l2-ctl CLI tools (same approach as the working shell
// scripts).  This is the most reliable method on Rockchip -- the media-ctl
// ioctls are stable and well-tested, whereas direct C V4L2 media ioctls
// require careful pad/link enumeration.
// ============================================================================

#include "pipeline/isp_config.h"
#include "util/logger.h"

#include <cstdlib>
#include <cstdio>
#include <string>

namespace sc {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int run_cmd(const std::string& cmd) {
    SC_LOG_DEBUG("exec: %s", cmd.c_str());
    int rc = std::system(cmd.c_str());
    if (rc != 0) {
        SC_LOG_WARN("command returned %d: %s", rc, cmd.c_str());
    }
    return rc;
}

/// Format a media-ctl --set-v4l2 command.
static std::string media_set(const std::string& media_dev,
                              const std::string& entity,
                              int pad,
                              const std::string& fmt_str) {
    // media-ctl -d /dev/media1 --set-v4l2 '"entity":pad[fmt]'
    return "media-ctl -d " + media_dev +
           " --set-v4l2 '\"" + entity + "\":" + std::to_string(pad) +
           "[" + fmt_str + "]'";
}

/// Format a v4l2-ctl command.
static std::string v4l2_set_fmt(const std::string& dev,
                                 int w, int h,
                                 const std::string& pixfmt) {
    return "v4l2-ctl -d " + dev +
           " --set-fmt-video=width=" + std::to_string(w) +
           ",height=" + std::to_string(h) +
           ",pixelformat=" + pixfmt;
}

static std::string v4l2_set_crop(const std::string& dev,
                                  int w, int h) {
    return "v4l2-ctl -d " + dev +
           " --set-selection=target=crop,left=0,top=0,width=" +
           std::to_string(w) + ",height=" + std::to_string(h);
}

// ---------------------------------------------------------------------------
// ISP configuration: shared media graph (sensor -> ISP input)
// ---------------------------------------------------------------------------
static int configure_isp_input(const Config& cfg) {
    const auto& isp = cfg.isp;
    const auto& sen = cfg.sensor;
    const std::string sz = std::to_string(sen.width) + "x" + std::to_string(sen.height);
    const std::string mbus = sen.mbus_fmt + "/" + sz;

    SC_LOG_INFO("Configuring ISP input: %s", mbus.c_str());

    // 1) Sensor output pad
    int rc = run_cmd(media_set(isp.media_dev, isp.sensor_entity, 0,
                                "fmt:" + mbus + " field:none"));
    if (rc) return -1;

    // 2) CSI DPHY input
    rc = run_cmd(media_set(isp.media_dev, isp.csi_dphy, 0,
                            "fmt:" + mbus + " field:none"));
    // Tolerate failure -- some boards use dphy0, others dphy1
    (void)rc;

    // 3) ISP CSI sub-device
    run_cmd(media_set(isp.media_dev, isp.csi_subdev, 0,
                       "fmt:" + mbus + " field:none"));

    // 4) ISP input pad (pad0) -- set Bayer format + crop to full sensor
    run_cmd(media_set(isp.media_dev, isp.isp_subdev, 0,
                       "fmt:" + mbus +
                       " crop:(0,0)/" + sz + " field:none"));

    // 5) Sensor timing (vertical blanking for FPS control)
    run_cmd("v4l2-ctl -d " + isp.sensor_subdev +
            " --set-ctrl=vertical_blanking=" + std::to_string(sen.vblank));

    return 0;
}

// ---------------------------------------------------------------------------
// Mainpath configuration (pad2 -> /dev/video8)
// ---------------------------------------------------------------------------
static int configure_mainpath(const Config& cfg) {
    const auto& isp = cfg.isp;
    const auto& st  = cfg.stream;

    // ISP mainpath: media pad always uses YUYV8_2X8 (ISP internal bus format).
    // The V4L2 capture node format is set from cfg.stream.src_fmt (NV12 or UYVY).
    // The ISP MI (Memory Interface) hardware converts YUYV→NV12 in the output
    // formatter.  NV12 UV plane is valid from frame #2 onwards (frame #1 is
    // ISP warm-up with UV=0x80).  See doc/isp/NV12_Y_ZERO_BUG.md.
    std::string isp_out_fmt = "YUYV8_2X8";
    std::string v4l2_fmt    = st.src_fmt;  // "NV12" (default) or "UYVY"

    std::string sz = std::to_string(st.width) + "x" + std::to_string(st.height);

    SC_LOG_INFO("Configuring mainpath: %s %s (%s)", v4l2_fmt.c_str(), sz.c_str(),
                isp.mainpath.c_str());

    // ISP output pad2 -- mainpath
    run_cmd(media_set(isp.media_dev, isp.isp_subdev, 2,
                       "fmt:" + isp_out_fmt + "/" + sz +
                       " crop:(0,0)/" + sz));

    // V4L2 device format
    run_cmd(v4l2_set_fmt(isp.mainpath, st.width, st.height, v4l2_fmt));

    // Crop to full sensor (so ISP does the downscale, not top-left crop)
    run_cmd(v4l2_set_crop(isp.mainpath, cfg.sensor.width, cfg.sensor.height));

    return 0;
}

// ---------------------------------------------------------------------------
// Selfpath configuration (/dev/video9)
//
// The selfpath has its own scaler inside the ISP, so we can request a
// completely different resolution and format from mainpath.
//
// For AI inference we want the smallest resolution that the model accepts
// (typically 640x480 or 640x640).  NV12 is the default format; RGA handles
// NV12→RGB conversion for RKNN.  UYVY can also be used (set via config).
// ---------------------------------------------------------------------------
static int configure_selfpath(const Config& cfg) {
    const auto& isp = cfg.isp;
    const auto& ai  = cfg.ai;

    std::string v4l2_fmt = ai.src_fmt;  // "NV12" or "UYVY"
    std::string sz = std::to_string(ai.width) + "x" + std::to_string(ai.height);

    SC_LOG_INFO("Configuring selfpath: %s %s (%s)", v4l2_fmt.c_str(), sz.c_str(),
                isp.selfpath.c_str());

    // Set selfpath V4L2 format.
    // The selfpath does NOT have a media-ctl pad to configure on rkisp v21 --
    // it takes its input from the ISP core and you just set the output format
    // on the V4L2 device directly.
    run_cmd(v4l2_set_fmt(isp.selfpath, ai.width, ai.height, v4l2_fmt));

    // Crop to full sensor bounds (let ISP scale down)
    run_cmd(v4l2_set_crop(isp.selfpath, cfg.sensor.width, cfg.sensor.height));

    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int isp_configure(const Config& cfg) {
    SC_LOG_INFO("=== ISP dual-path configuration ===");

    // Release any process holding the video devices
    run_cmd("fuser -k " + cfg.isp.mainpath + " 2>/dev/null || true");
    if (cfg.enable_ai || cfg.enable_snapshot) {
        run_cmd("fuser -k " + cfg.isp.selfpath + " 2>/dev/null || true");
    }

    if (configure_isp_input(cfg) != 0) {
        SC_LOG_ERROR("Failed to configure ISP input");
        return -1;
    }

    if (configure_mainpath(cfg) != 0) {
        SC_LOG_ERROR("Failed to configure mainpath");
        return -1;
    }

    // Configure selfpath if AI or snapshot needs it
    if (cfg.enable_ai || cfg.enable_snapshot) {
        if (configure_selfpath(cfg) != 0) {
            SC_LOG_ERROR("Failed to configure selfpath");
            return -1;
        }
    }

    SC_LOG_INFO("ISP configuration complete");
    SC_LOG_INFO("  mainpath: %s %dx%d %s",
                cfg.isp.mainpath.c_str(), cfg.stream.width, cfg.stream.height,
                cfg.stream.src_fmt.c_str());
    if (cfg.enable_ai || cfg.enable_snapshot) {
        SC_LOG_INFO("  selfpath: %s %dx%d %s",
                    cfg.isp.selfpath.c_str(), cfg.ai.width, cfg.ai.height,
                    cfg.ai.src_fmt.c_str());
    }

    return 0;
}

int isp_configure_mainpath(const Config& cfg) {
    SC_LOG_INFO("=== ISP mainpath-only configuration ===");
    run_cmd("fuser -k " + cfg.isp.mainpath + " 2>/dev/null || true");

    if (configure_isp_input(cfg) != 0) return -1;
    if (configure_mainpath(cfg) != 0) return -1;

    SC_LOG_INFO("Mainpath ready: %s %dx%d %s",
                cfg.isp.mainpath.c_str(), cfg.stream.width, cfg.stream.height,
                cfg.stream.src_fmt.c_str());
    return 0;
}

int isp_query_mainpath(const Config& cfg) {
    return run_cmd("v4l2-ctl -d " + cfg.isp.mainpath + " --get-fmt-video");
}

}  // namespace sc
