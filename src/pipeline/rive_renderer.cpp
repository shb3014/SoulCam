#ifdef SOULCAM_HAVE_RIVE
// ============================================================================
// Rive Renderer implementation
//
// Runs on a dedicated thread with exclusive EGL context ownership.
// Detection data flows in via updateDetections() (mutex-protected).
// Configuration changes are queued and applied at the start of each frame.
// ============================================================================

#include "pipeline/rive_renderer.h"
#include "pipeline/drm_egl_context.h"
#include "util/logger.h"

#include "rive/artboard.hpp"
#include "rive/file.hpp"
#include "rive/animation/state_machine_instance.hpp"
#include "rive/animation/state_machine_input_instance.hpp"
#include "rive/animation/nested_input.hpp"
#include "rive/renderer/rive_renderer.hpp"
#include "rive/renderer/gl/render_context_gl_impl.hpp"
#include "rive/renderer/gl/render_target_gl.hpp"
#include "rive/math/aabb.hpp"
#include "rive/math/vec2d.hpp"
#include "rive/node.hpp"
#include "rive/joystick.hpp"
#include "rive/nested_artboard.hpp"
#include <GLES3/gl3.h>

extern "C" {
    using GLADapiproc = void (*)();
    using GLADloadfunc = GLADapiproc (*)(const char* name);
    int gladLoadCustomLoader(GLADloadfunc);
}

#include <fstream>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <pthread.h>
#include <pwd.h>
#include <unistd.h>

using namespace rive;
using namespace rive::gpu;

namespace sc {

// ---------------------------------------------------------------------------
// Control mode (same detection logic as universal_tracker_demo.cpp)
// ---------------------------------------------------------------------------

enum class RiveControlMode { NONE, JOYSTICK, LOOK_DIR, POINTER_MOVE };

static const char* controlModeName(RiveControlMode m) {
    switch (m) {
        case RiveControlMode::JOYSTICK:     return "Joystick";
        case RiveControlMode::LOOK_DIR:     return "look_dir";
        case RiveControlMode::POINTER_MOVE: return "pointerMove";
        case RiveControlMode::NONE:         return "NONE";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------
// Internal tracker state (lives on the render thread only)
// ---------------------------------------------------------------------------

struct TrackerState {
    // Rive objects
    std::unique_ptr<rive_rk3566::DRMEGLContext> drm;
    std::unique_ptr<RenderContext>    renderCtx;
    std::unique_ptr<rive::RiveRenderer> renderer;
    rcp<RenderTarget>                renderTarget;
    rcp<File>                        file;
    std::unique_ptr<ArtboardInstance> artboard;
    std::unique_ptr<Scene>           scene;
    std::unique_ptr<StateMachineInstance> charScene;

    // Display / render
    uint32_t displayW = 0, displayH = 0;
    uint32_t renderW = 500, renderH = 500;
    GLuint fbo = 0, fboTex = 0;
    int clearFrames = 6;

    // Control mode
    RiveControlMode controlMode = RiveControlMode::NONE;

    // Joystick
    Joystick* joystick = nullptr;
    Artboard* nestedArtboard = nullptr;

    // look_dir
    SMINumber* lookDirInput = nullptr;
    float currentLookDir = 0.0f;

    // Shared gaze state
    float targetX = 0, targetY = 0;
    float currentX = 0, currentY = 0;
    float abWidth = 500, abHeight = 500;
    bool personVisible = false;
    float timeSinceLastPerson = 10.0f;

    // pointerMove extras
    bool pointerIsDown = false;
    float clickTimer = 0.0f;
    std::vector<SMIBool*> presenceBools;
    SMITrigger* triggerLeft = nullptr;
    SMITrigger* triggerRight = nullptr;
    std::vector<SMITrigger*> triggerCenter;
    int lastTriggerZone = 999;

    // Config
    std::string targetLabel = "person";
    std::string rivPath;
    bool enabled = false;

    // Detections (copied from shared state each frame)
    std::vector<Detection> dets;
    int detW = 640, detH = 480;
    bool hasFresh = false;

    // Sticky target: persist lock-on across frames via IOU overlap.
    Detection stickyTarget{};
    bool hasStickyTarget = false;
    int stickyMissFrames = 0;
    static constexpr int kStickyMaxMiss = 8;
};

// ---------------------------------------------------------------------------
// Rive file loading and control mode detection (mirror of demo logic)
// ---------------------------------------------------------------------------

static std::string expandTilde(const std::string& path) {
    if (path.empty() || path[0] != '~') return path;
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home) return path;
    return std::string(home) + path.substr(1);
}

static bool loadRiveFile(TrackerState& ts, const std::string& path) {
    std::string resolved = expandTilde(path);
    std::ifstream f(resolved, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        SC_LOG_ERROR("Rive: cannot open %s (resolved: %s)", path.c_str(), resolved.c_str());
        return false;
    }
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(size);
    if (!f.read(reinterpret_cast<char*>(buf.data()), size)) {
        SC_LOG_ERROR("Rive: read error %s", path.c_str());
        return false;
    }

    ts.file = File::import(buf, ts.renderCtx.get());
    if (!ts.file) { SC_LOG_ERROR("Rive: import failed"); return false; }

    ts.artboard = ts.file->artboardDefault();
    if (!ts.artboard) { SC_LOG_ERROR("Rive: no artboard"); return false; }

    ts.scene = ts.artboard->defaultStateMachine();
    if (!ts.scene) ts.scene = ts.artboard->defaultScene();
    if (!ts.scene) { SC_LOG_ERROR("Rive: no scene"); return false; }

    SC_LOG_INFO("Rive: loaded \"%s\" artboard=\"%s\" inputs=%zu",
                path.c_str(), ts.artboard->name().c_str(), ts.scene->inputCount());
    return true;
}

static bool tryFindJoystick(TrackerState& ts) {
    auto nested = ts.artboard->nestedArtboards();
    for (auto* na : nested) {
        auto* inst = na->artboardInstance();
        if (!inst) continue;
        auto joysticks = inst->find<Joystick>();
        if (!joysticks.empty()) {
            ts.joystick = joysticks[0];
            ts.nestedArtboard = inst;
            return true;
        }
    }
    auto joysticks = ts.artboard->find<Joystick>();
    if (!joysticks.empty()) {
        ts.joystick = joysticks[0];
        ts.nestedArtboard = ts.artboard.get();
        return true;
    }
    return false;
}

static bool tryFindLookDir(TrackerState& ts) {
    auto nested = ts.artboard->nestedArtboards();
    for (auto* na : nested) {
        auto* inst = na->artboardInstance();
        if (!inst) continue;
        auto* ni = na->input("look_dir");
        if (ni) {
            auto* smi = ni->input();
            if (smi) {
                ts.lookDirInput = static_cast<SMINumber*>(smi);
                return true;
            }
        }
        for (size_t s = 0; s < inst->stateMachineCount(); s++) {
            auto sm = inst->stateMachineAt(s);
            if (!sm) continue;
            for (size_t i = 0; i < sm->inputCount(); i++) {
                auto* inp = sm->input(i);
                if (inp && inp->name() == "look_dir") {
                    ts.lookDirInput = static_cast<SMINumber*>(inp);
                    ts.charScene = std::move(sm);
                    return true;
                }
            }
        }
    }
    for (size_t i = 0; i < ts.scene->inputCount(); i++) {
        auto* inp = ts.scene->input(i);
        if (inp && inp->name() == "look_dir") {
            ts.lookDirInput = static_cast<SMINumber*>(inp);
            return true;
        }
    }
    return false;
}

static void bindDirectInputs(TrackerState& ts) {
    for (size_t i = 0; i < ts.scene->inputCount(); i++) {
        auto* inp = ts.scene->input(i);
        if (!inp) continue;
        const auto& name = inp->name();
        uint16_t ct = inp->inputCoreType();

        if (ct == 59) { // Bool
            bool en = (name == "Parent-isTracking" || name == "isTracking" ||
                       name == "tracking" || name == "isOnGlasses");
            if (en) static_cast<SMIBool*>(inp)->value(true);
            if (name.find("blush") != std::string::npos ||
                name.find("Blush") != std::string::npos ||
                name.find("blushing") != std::string::npos) {
                ts.presenceBools.push_back(static_cast<SMIBool*>(inp));
            }
        }
        if (ct == 58) { // Trigger
            bool isL = (name.find("left") != std::string::npos ||
                        name.find("Left") != std::string::npos);
            bool isR = (name.find("right") != std::string::npos ||
                        name.find("Right") != std::string::npos);
            if (isL) ts.triggerLeft = static_cast<SMITrigger*>(inp);
            else if (isR) ts.triggerRight = static_cast<SMITrigger*>(inp);
            else ts.triggerCenter.push_back(static_cast<SMITrigger*>(inp));
        }
    }
}

static void detectControlMode(TrackerState& ts) {
    ts.scene->advanceAndApply(0.0f);

    if (tryFindJoystick(ts)) {
        ts.controlMode = RiveControlMode::JOYSTICK;
    } else if (tryFindLookDir(ts)) {
        ts.controlMode = RiveControlMode::LOOK_DIR;
    } else {
        bindDirectInputs(ts);
        ts.abWidth = ts.artboard->width();
        ts.abHeight = ts.artboard->height();
        ts.currentX = ts.abWidth / 2.0f;
        ts.currentY = ts.abHeight / 2.0f;
        ts.targetX = ts.currentX;
        ts.targetY = ts.currentY;
        ts.controlMode = RiveControlMode::POINTER_MOVE;
    }
    SC_LOG_INFO("Rive: control mode = %s", controlModeName(ts.controlMode));
}

// ---------------------------------------------------------------------------
// Tracking point estimation (mirrors demo logic)
// ---------------------------------------------------------------------------

static void estimateTrackingPoint(const Detection& det, const std::string& targetLabel,
                                  int detW, int detH, float& cx, float& cy) {
    const float frameW = static_cast<float>(std::max(1, detW));
    const float frameH = static_cast<float>(std::max(1, detH));
    cx = frameW - (det.left + det.right) / 2.0f;
    if (targetLabel == "person") {
        float h = static_cast<float>(det.bottom - det.top);
        cy = det.top + h * 0.12f;
    } else {
        cy = (det.top + det.bottom) / 2.0f;
    }
    cx = std::clamp(cx, 0.0f, frameW);
    cy = std::clamp(cy, 0.0f, frameH);
}

static float det_iou(const Detection& a, const Detection& b) {
    const float x1 = static_cast<float>(std::max(a.left, b.left));
    const float y1 = static_cast<float>(std::max(a.top, b.top));
    const float x2 = static_cast<float>(std::min(a.right, b.right));
    const float y2 = static_cast<float>(std::min(a.bottom, b.bottom));
    if (x2 <= x1 || y2 <= y1) return 0.0f;
    const float inter = (x2 - x1) * (y2 - y1);
    const float areaA = static_cast<float>((a.right - a.left) * (a.bottom - a.top));
    const float areaB = static_cast<float>((b.right - b.left) * (b.bottom - b.top));
    const float uni = areaA + areaB - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

static const Detection* findStickyTarget(TrackerState& ts,
                                         const std::vector<Detection>& dets,
                                         const std::string& label) {
    auto collect = [&](const std::string& wanted) {
        std::vector<const Detection*> out;
        for (const auto& d : dets) {
            if (d.label && std::string(d.label) == wanted && d.confidence > 0.3f)
                out.push_back(&d);
        }
        return out;
    };

    auto candidates = collect(label);
    if (candidates.empty() && label == "person")
        candidates = collect("hand");
    if (candidates.empty()) {
        for (const auto& d : dets)
            if (d.confidence > 0.3f) candidates.push_back(&d);
    }
    if (candidates.empty()) {
        ts.stickyMissFrames++;
        if (ts.stickyMissFrames > TrackerState::kStickyMaxMiss)
            ts.hasStickyTarget = false;
        return nullptr;
    }

    if (ts.hasStickyTarget) {
        const Detection* best_match = nullptr;
        float best_iou = 0.15f;
        for (const auto* c : candidates) {
            float iou = det_iou(ts.stickyTarget, *c);
            if (iou > best_iou) { best_iou = iou; best_match = c; }
        }
        if (best_match) {
            float cur_area = static_cast<float>(
                (best_match->right - best_match->left) *
                (best_match->bottom - best_match->top));

            // Switch to a challenger if it is much closer (larger area).
            constexpr float kSwitchAreaRatio = 1.8f;
            const Detection* challenger = nullptr;
            float challenger_area = 0.0f;
            for (const auto* c : candidates) {
                if (c == best_match) continue;
                float a = static_cast<float>(
                    (c->right - c->left) * (c->bottom - c->top));
                if (a > challenger_area) { challenger_area = a; challenger = c; }
            }
            if (challenger && cur_area > 0.0f &&
                challenger_area / cur_area >= kSwitchAreaRatio) {
                ts.stickyTarget = *challenger;
                ts.stickyMissFrames = 0;
                return challenger;
            }

            ts.stickyTarget = *best_match;
            ts.stickyMissFrames = 0;
            return best_match;
        }
        ts.stickyMissFrames++;
        if (ts.stickyMissFrames <= TrackerState::kStickyMaxMiss)
            return nullptr;
    }

    const Detection* best = nullptr;
    float bestScore = -1.0f;
    for (const auto* c : candidates) {
        float area = static_cast<float>((c->right - c->left) * (c->bottom - c->top));
        float score = area + 0.15f * c->confidence * area;
        if (score > bestScore) { bestScore = score; best = c; }
    }
    if (best) {
        ts.stickyTarget = *best;
        ts.hasStickyTarget = true;
        ts.stickyMissFrames = 0;
    }
    return best;
}

// ---------------------------------------------------------------------------
// Per-mode update functions
// ---------------------------------------------------------------------------

static void updateJoystick(TrackerState& ts, float dt) {
    if (!ts.joystick) return;
    const float frameW = static_cast<float>(std::max(1, ts.detW));
    const float frameH = static_cast<float>(std::max(1, ts.detH));

    if (ts.hasFresh) {
        ts.hasFresh = false;
        auto* p = findStickyTarget(ts, ts.dets, ts.targetLabel);
        if (p) {
            float cx, cy;
            estimateTrackingPoint(*p, ts.targetLabel, ts.detW, ts.detH, cx, cy);
            ts.targetX = std::clamp((cx / frameW) * 2.0f - 1.0f, -1.0f, 1.0f);
            ts.targetY = std::clamp((cy / frameH) * 2.0f - 1.0f, -1.0f, 1.0f);
            ts.personVisible = true;
            ts.timeSinceLastPerson = 0.0f;
        } else {
            ts.timeSinceLastPerson += dt;
            if (ts.timeSinceLastPerson > 1.0f) {
                ts.targetX = 0; ts.targetY = 0;
                ts.personVisible = false;
            }
        }
    } else {
        ts.timeSinceLastPerson += dt;
        if (ts.timeSinceLastPerson > 1.0f) {
            ts.targetX = 0; ts.targetY = 0;
            ts.personVisible = false;
        }
    }

    float speed = ts.personVisible ? 8.0f : 3.0f;
    float t = 1.0f - std::exp(-speed * dt);
    ts.currentX += (ts.targetX - ts.currentX) * t;
    ts.currentY += (ts.targetY - ts.currentY) * t;

    ts.joystick->x(ts.currentX);
    ts.joystick->y(ts.currentY);
    ts.joystick->apply(ts.nestedArtboard);
}

static void updateLookDir(TrackerState& ts) {
    if (!ts.lookDirInput || !ts.hasFresh) return;
    const float frameW = static_cast<float>(std::max(1, ts.detW));
    ts.hasFresh = false;

    auto* p = findStickyTarget(ts, ts.dets, ts.targetLabel);
    float newDir = 0.0f;
    if (p) {
        float cx, cy;
        estimateTrackingPoint(*p, ts.targetLabel, ts.detW, ts.detH, cx, cy);
        float normX = cx / frameW;
        if (normX < 0.33f) newDir = 1.0f;
        else if (normX > 0.66f) newDir = 2.0f;
        else newDir = 3.0f;
    }
    if (newDir != ts.currentLookDir) {
        ts.currentLookDir = newDir;
        ts.lookDirInput->value(newDir);
    }
}

static void updatePointerMove(TrackerState& ts, float dt) {
    bool wasVisible = ts.personVisible;
    const float frameW = static_cast<float>(std::max(1, ts.detW));
    const float frameH = static_cast<float>(std::max(1, ts.detH));

    if (ts.hasFresh) {
        ts.hasFresh = false;
        auto* p = findStickyTarget(ts, ts.dets, ts.targetLabel);
        if (p) {
            float cx, cy;
            estimateTrackingPoint(*p, ts.targetLabel, ts.detW, ts.detH, cx, cy);
            ts.targetX = (cx / frameW) * ts.abWidth;
            ts.targetY = (cy / frameH) * ts.abHeight;
            ts.personVisible = true;
            ts.timeSinceLastPerson = 0.0f;
        } else {
            ts.timeSinceLastPerson += dt;
            if (ts.timeSinceLastPerson > 1.0f) {
                ts.targetX = ts.abWidth / 2.0f;
                ts.targetY = ts.abHeight / 2.0f;
                ts.personVisible = false;
            }
        }
    } else {
        ts.timeSinceLastPerson += dt;
        if (ts.timeSinceLastPerson > 1.0f) {
            ts.targetX = ts.abWidth / 2.0f;
            ts.targetY = ts.abHeight / 2.0f;
            ts.personVisible = false;
        }
    }

    float speed = ts.personVisible ? 8.0f : 3.0f;
    float t = 1.0f - std::exp(-speed * dt);
    ts.currentX += (ts.targetX - ts.currentX) * t;
    ts.currentY += (ts.targetY - ts.currentY) * t;

    Vec2D pos(ts.currentX, ts.currentY);
    ts.scene->pointerMove(pos);

    if (ts.personVisible && !wasVisible) {
        ts.scene->pointerDown(pos);
        ts.pointerIsDown = true;
        ts.clickTimer = 0.0f;
    }
    if (!ts.personVisible && wasVisible && ts.pointerIsDown) {
        ts.scene->pointerUp(pos);
        ts.pointerIsDown = false;
    }
    if (ts.personVisible && ts.pointerIsDown) {
        ts.clickTimer += dt;
        if (ts.clickTimer >= 2.0f) {
            ts.scene->pointerUp(pos);
            ts.scene->pointerDown(pos);
            ts.clickTimer = 0.0f;
        }
    }

    for (auto* b : ts.presenceBools) b->value(ts.personVisible);

    if (ts.personVisible && (ts.triggerLeft || ts.triggerRight)) {
        float normX = ts.currentX / ts.abWidth;
        int zone = (normX < 0.4f) ? -1 : (normX > 0.6f) ? 1 : 0;
        if (zone != ts.lastTriggerZone) {
            ts.lastTriggerZone = zone;
            if (zone < 0 && ts.triggerLeft) ts.triggerLeft->fire();
            else if (zone > 0 && ts.triggerRight) ts.triggerRight->fire();
            else for (auto* t : ts.triggerCenter) t->fire();
        }
    } else if (!ts.personVisible) {
        ts.lastTriggerZone = 999;
    }
}

// ---------------------------------------------------------------------------
// FBO creation / teardown
// ---------------------------------------------------------------------------

static void createFBO(TrackerState& ts) {
    glGenFramebuffers(1, &ts.fbo);
    glGenTextures(1, &ts.fboTex);
    glBindTexture(GL_TEXTURE_2D, ts.fboTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ts.renderW, ts.renderH,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, ts.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, ts.fboTex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void destroyFBO(TrackerState& ts) {
    if (ts.fbo) { glDeleteFramebuffers(1, &ts.fbo); ts.fbo = 0; }
    if (ts.fboTex) { glDeleteTextures(1, &ts.fboTex); ts.fboTex = 0; }
}

// ---------------------------------------------------------------------------
// RiveRenderer public API
// ---------------------------------------------------------------------------

RiveRenderer::RiveRenderer() = default;

RiveRenderer::~RiveRenderer() {
    stop();
}

void RiveRenderer::start(const RiveRendererConfig& cfg) {
    if (m_running.load()) return;
    m_cfg = cfg;
    m_cfg.riv_file = expandTilde(cfg.riv_file);
    m_stopRequested = false;
    {
        std::lock_guard<std::mutex> lk(m_pending.mtx);
        m_pending.riv_file = m_cfg.riv_file;
        m_pending.resolution = cfg.resolution;
        m_pending.target_label = cfg.target_label;
        m_pending.enabled = cfg.enabled;
    }
    m_thread = std::thread(&RiveRenderer::renderThread, this);
    m_running = true;
}

void RiveRenderer::stop() {
    if (!m_running.load()) return;
    m_stopRequested = true;
    if (m_thread.joinable()) m_thread.join();
    m_running = false;
}

void RiveRenderer::updateDetections(const std::vector<Detection>& dets, int w, int h) {
    std::lock_guard<std::mutex> lk(m_shared.mtx);
    m_shared.dets = dets;
    m_shared.src_w = w;
    m_shared.src_h = h;
    m_shared.fresh = true;
}

void RiveRenderer::setEnabled(bool v) {
    std::lock_guard<std::mutex> lk(m_pending.mtx);
    m_pending.enabled = v;
    m_pending.enabled_changed = true;
}

void RiveRenderer::setRivFile(const std::string& path) {
    std::lock_guard<std::mutex> lk(m_pending.mtx);
    m_pending.riv_file = expandTilde(path);
    m_pending.riv_file_changed = true;
}

void RiveRenderer::setResolution(uint32_t res) {
    std::lock_guard<std::mutex> lk(m_pending.mtx);
    m_pending.resolution = res;
    m_pending.resolution_changed = true;
}

void RiveRenderer::setTargetLabel(const std::string& label) {
    std::lock_guard<std::mutex> lk(m_pending.mtx);
    m_pending.target_label = label;
    m_pending.target_changed = true;
}

// ---------------------------------------------------------------------------
// Render thread
// ---------------------------------------------------------------------------

void RiveRenderer::renderThread() {
    pthread_setname_np(pthread_self(), "rive_render");

    TrackerState ts;
    ts.renderW = m_cfg.resolution;
    ts.renderH = m_cfg.resolution;
    ts.targetLabel = m_cfg.target_label;
    ts.rivPath = m_cfg.riv_file;
    ts.enabled = m_cfg.enabled;

    // --- Mesa PLS driver path (required for Panfrost PLS rendering) ---
    const char* mesa_pls = "/opt/mesa-pls/lib/aarch64-linux-gnu";
    if (access(mesa_pls, F_OK) == 0) {
        std::string dri_path = std::string(mesa_pls) + "/dri";
        setenv("LIBGL_DRIVERS_PATH", dri_path.c_str(), 0);

        const char* existing = getenv("LD_LIBRARY_PATH");
        if (!existing || std::string(existing).find(mesa_pls) == std::string::npos) {
            std::string ld_path = mesa_pls;
            if (existing && existing[0]) ld_path += std::string(":") + existing;
            setenv("LD_LIBRARY_PATH", ld_path.c_str(), 1);
        }
    }

    // --- DRM/EGL initialization ---
    SC_LOG_INFO("Rive: initializing DRM/EGL...");
    ts.drm = std::make_unique<rive_rk3566::DRMEGLContext>();
    if (!ts.drm->initialize()) {
        SC_LOG_ERROR("Rive: DRM/EGL init failed: %s", ts.drm->lastError().c_str());
        return;
    }
    ts.displayW = ts.drm->width();
    ts.displayH = ts.drm->height();

    auto procAddr = ts.drm->getProcAddress();
    if (!procAddr || gladLoadCustomLoader((GLADloadfunc)procAddr) == 0) {
        SC_LOG_ERROR("Rive: GLAD init failed");
        return;
    }

    RenderContextGLImpl::ContextOptions ctxOpts = {};
    ts.renderCtx = RenderContextGLImpl::MakeContext(ctxOpts);
    if (!ts.renderCtx) {
        SC_LOG_ERROR("Rive: render context creation failed");
        return;
    }

    createFBO(ts);
    ts.renderTarget = make_rcp<FramebufferRenderTargetGL>(ts.renderW, ts.renderH, ts.fbo, 0);

    // --- Load initial .riv if specified ---
    bool riveLoaded = false;
    if (!ts.rivPath.empty()) {
        if (loadRiveFile(ts, ts.rivPath)) {
            ts.renderer = std::make_unique<rive::RiveRenderer>(ts.renderCtx.get());
            detectControlMode(ts);
            riveLoaded = true;
        }
    }

    SC_LOG_INFO("Rive: thread started (display=%ux%u, render=%ux%u, loaded=%s)",
                ts.displayW, ts.displayH, ts.renderW, ts.renderH,
                riveLoaded ? "yes" : "no");

    // --- Render loop ---
    auto lastTime = std::chrono::high_resolution_clock::now();
    int frameCount = 0;
    auto lastFpsReport = std::chrono::steady_clock::now();

    while (!m_stopRequested.load()) {
        // Apply pending config changes
        {
            std::lock_guard<std::mutex> lk(m_pending.mtx);
            if (m_pending.enabled_changed) {
                ts.enabled = m_pending.enabled;
                m_pending.enabled_changed = false;
                SC_LOG_INFO("Rive: %s", ts.enabled ? "enabled" : "disabled");
            }
            if (m_pending.target_changed) {
                ts.targetLabel = m_pending.target_label;
                m_pending.target_changed = false;
                ts.hasStickyTarget = false;
            }
            if (m_pending.resolution_changed) {
                uint32_t newRes = m_pending.resolution;
                m_pending.resolution_changed = false;
                if (newRes != ts.renderW && newRes > 0) {
                    ts.renderW = newRes;
                    ts.renderH = newRes;
                    destroyFBO(ts);
                    createFBO(ts);
                    ts.renderTarget = make_rcp<FramebufferRenderTargetGL>(
                        ts.renderW, ts.renderH, ts.fbo, 0);
                    ts.clearFrames = 6;
                    SC_LOG_INFO("Rive: resolution changed to %ux%u", ts.renderW, ts.renderH);
                }
            }
            if (m_pending.riv_file_changed) {
                std::string newPath = m_pending.riv_file;
                m_pending.riv_file_changed = false;
                if (newPath != ts.rivPath) {
                    ts.rivPath = newPath;
                    // Reset Rive state
                    ts.file.reset();
                    ts.artboard.reset();
                    ts.scene.reset();
                    ts.charScene.reset();
                    ts.renderer.reset();
                    ts.joystick = nullptr;
                    ts.nestedArtboard = nullptr;
                    ts.lookDirInput = nullptr;
                    ts.presenceBools.clear();
                    ts.triggerLeft = nullptr;
                    ts.triggerRight = nullptr;
                    ts.triggerCenter.clear();
                    ts.controlMode = RiveControlMode::NONE;
                    riveLoaded = false;

                    if (!newPath.empty()) {
                        if (loadRiveFile(ts, newPath)) {
                            ts.renderer = std::make_unique<rive::RiveRenderer>(ts.renderCtx.get());
                            detectControlMode(ts);
                            riveLoaded = true;
                        }
                    }
                }
            }
        }

        // Compute dt
        auto now = std::chrono::high_resolution_clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        // If disabled or no riv loaded, render black and sleep
        if (!ts.enabled || !riveLoaded) {
            ts.drm->beginFrame();
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, ts.displayW, ts.displayH);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ts.drm->swapBuffers();
            continue;
        }

        // Read detections from shared state
        {
            std::lock_guard<std::mutex> lk(m_shared.mtx);
            if (m_shared.fresh) {
                ts.dets = m_shared.dets;
                ts.detW = m_shared.src_w;
                ts.detH = m_shared.src_h;
                ts.hasFresh = true;
                m_shared.fresh = false;
            }
        }

        // Periodic diagnostic: confirm detections reach the Rive thread.
        static int diagCounter = 0;
        if (++diagCounter >= 300) {
            diagCounter = 0;
            const Detection* tgt = findStickyTarget(ts, ts.dets, ts.targetLabel);
            SC_LOG_INFO("Rive diag: enabled=%d loaded=%d dets=%zu target=\"%s\" found=%s visible=%d",
                        ts.enabled ? 1 : 0, riveLoaded ? 1 : 0,
                        ts.dets.size(), ts.targetLabel.c_str(),
                        tgt ? (tgt->label ? tgt->label : "?") : "none",
                        ts.personVisible ? 1 : 0);
        }

        // Update tracking
        switch (ts.controlMode) {
            case RiveControlMode::JOYSTICK:     updateJoystick(ts, dt); break;
            case RiveControlMode::LOOK_DIR:     updateLookDir(ts); break;
            case RiveControlMode::POINTER_MOVE:  updatePointerMove(ts, dt); break;
            case RiveControlMode::NONE:          break;
        }

        // Advance Rive animation
        if (ts.charScene) ts.charScene->advanceAndApply(dt);
        if (ts.scene) ts.scene->advanceAndApply(dt);

        // Render frame
        ts.drm->beginFrame();

        RenderContext::FrameDescriptor frameDesc = {
            .renderTargetWidth = ts.renderW,
            .renderTargetHeight = ts.renderH,
            .clearColor = 0xff000000,
        };

        auto renderCtxGL = ts.renderCtx->static_impl_cast<RenderContextGLImpl>();
        renderCtxGL->invalidateGLState();
        ts.renderCtx->beginFrame(frameDesc);

        float abW = ts.artboard->width();
        float abH = ts.artboard->height();
        if (abW <= 0 || abH <= 0) { abW = ts.renderW; abH = ts.renderH; }

        AABB displayBounds(0, 0, ts.renderW, ts.renderH);
        AABB artboardBounds(0, 0, abW, abH);

        ts.renderer->save();
        ts.renderer->align(Fit::contain, Alignment::center, displayBounds, artboardBounds);
        ts.artboard->draw(ts.renderer.get());
        ts.renderer->restore();

        ts.renderCtx->flush({.renderTarget = ts.renderTarget.get()});

        // Blit FBO to center of display
        int dstX = (ts.displayW - ts.renderW) / 2;
        int dstY = (ts.displayH - ts.renderH) / 2;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

        if (ts.clearFrames > 0) {
            glViewport(0, 0, ts.displayW, ts.displayH);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ts.clearFrames--;
        }

        glBindFramebuffer(GL_READ_FRAMEBUFFER, ts.fbo);
        glBlitFramebuffer(0, 0, ts.renderW, ts.renderH,
                          dstX, dstY, dstX + ts.renderW, dstY + ts.renderH,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        ts.drm->swapBuffers();

        // FPS reporting
        frameCount++;
        auto fpsNow = std::chrono::steady_clock::now();
        auto fpsElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            fpsNow - lastFpsReport).count();
        if (fpsElapsed >= 10000) {
            double fps = (frameCount * 1000.0) / fpsElapsed;
            SC_LOG_INFO("Rive: %.1f FPS (%ux%u)", fps, ts.renderW, ts.renderH);
            frameCount = 0;
            lastFpsReport = fpsNow;
        }
    }

    // Cleanup
    destroyFBO(ts);
    ts.renderer.reset();
    ts.renderTarget.reset();
    ts.scene.reset();
    ts.charScene.reset();
    ts.artboard.reset();
    ts.file.reset();
    ts.renderCtx.reset();
    ts.drm.reset();

    SC_LOG_INFO("Rive: thread stopped");
}

}  // namespace sc

#endif // SOULCAM_HAVE_RIVE
