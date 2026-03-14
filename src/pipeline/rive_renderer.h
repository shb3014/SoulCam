#pragma once
// ============================================================================
// Rive Renderer — GPU-rendered tracking animations driven by AI detections.
//
// Runs the Rive PLS renderer on a dedicated thread using the Mali-G52 GPU
// via DRM/KMS + EGL.  Detections are passed directly from the AI callback
// (zero-copy, mutex-protected) instead of going through the scene hub socket.
//
// The module auto-detects the best control mode for any .riv file:
//   1. Joystick (continuous x/y — avatar.riv)
//   2. look_dir (discrete directions — dress-up.riv)
//   3. pointerMove (fallback — face-tracking-test.riv, anime-girl.riv)
// ============================================================================

#include "soulcam.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sc {

struct RiveRendererConfig {
    std::string riv_file;
    uint32_t    resolution = 500;
    std::string target_label = "person";
    bool        enabled = false;
};

#ifdef SOULCAM_HAVE_RIVE

class RiveRenderer {
public:
    RiveRenderer();
    ~RiveRenderer();

    void start(const RiveRendererConfig& cfg);
    void stop();
    bool running() const { return m_running.load(); }

    // Thread-safe detection push (call from AI callback thread)
    void updateDetections(const std::vector<Detection>& dets, int w, int h);

    // Runtime configuration (thread-safe, applied on next frame)
    void setEnabled(bool enabled);
    void setRivFile(const std::string& path);
    void setResolution(uint32_t res);
    void setTargetLabel(const std::string& label);

private:
    void renderThread();

    std::thread       m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};

    // Shared detection state (AI thread writes, render thread reads)
    struct SharedState {
        std::mutex             mtx;
        std::vector<Detection> dets;
        int                    src_w = 640;
        int                    src_h = 480;
        bool                   fresh = false;
    };
    SharedState m_shared;

    // Pending config changes (main thread writes, render thread reads)
    struct PendingConfig {
        std::mutex  mtx;
        std::string riv_file;
        uint32_t    resolution = 500;
        std::string target_label = "person";
        bool        enabled = false;
        bool        riv_file_changed = false;
        bool        resolution_changed = false;
        bool        target_changed = false;
        bool        enabled_changed = false;
    };
    PendingConfig m_pending;

    RiveRendererConfig m_cfg;
};

#else // !SOULCAM_HAVE_RIVE

// Stub when Rive is not available
class RiveRenderer {
public:
    void start(const RiveRendererConfig&) {}
    void stop() {}
    bool running() const { return false; }
    void updateDetections(const std::vector<Detection>&, int, int) {}
    void setEnabled(bool) {}
    void setRivFile(const std::string&) {}
    void setResolution(uint32_t) {}
    void setTargetLabel(const std::string&) {}
};

#endif // SOULCAM_HAVE_RIVE

}  // namespace sc
