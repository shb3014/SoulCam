/*
 * Copyright 2025 Rive
 * DRM/KMS + EGL Context for RK3566 (Mali G52)
 */

#pragma once
#ifdef SOULCAM_HAVE_RIVE

#include <cstdint>
#include <string>
#include <memory>

// Forward declarations to avoid pulling in EGL/DRM headers
struct gbm_device;
struct gbm_surface;
struct gbm_bo;
typedef void* EGLDisplay;
typedef void* EGLSurface;
typedef void* EGLContext;
typedef void* EGLConfig;

namespace rive_rk3566
{

struct DRMFramebuffer
{
    uint32_t fb_id;
    uint32_t handle;
    uint32_t pitch;
    uint32_t offset;
    uint64_t modifier;
    gbm_bo* bo;
};

class DRMEGLContext
{
public:
    DRMEGLContext();
    ~DRMEGLContext();

    // Initialize DRM, GBM, and EGL
    bool initialize(int width = 0, int height = 0);

    // Get display dimensions
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    uint32_t refreshRate() const { return m_refreshRate; }

    // Get EGL function loader
    void* (*getProcAddress())(const char*);

    // Frame management
    void beginFrame();
    void swapBuffers();

    // Error reporting
    const std::string& lastError() const { return m_lastError; }

private:
    // DRM/KMS setup
    bool initDRM();
    bool findDisplay();
    bool setupCRTC();

    // GBM setup
    bool initGBM();

    // EGL setup
    bool initEGL();

    // Framebuffer management
    DRMFramebuffer* getFBFromBO(gbm_bo* bo);
    void destroyFB(gbm_bo* bo, void* data);

    // Page flip handling
    static void pageFlipHandler(int fd,
                                unsigned int frame,
                                unsigned int sec,
                                unsigned int usec,
                                void* data);
    void waitForPageFlip();

    // Error handling
    void setError(const std::string& error);

private:
    // DRM/KMS state
    int m_drmFd = -1;
    uint32_t m_connectorId = 0;
    uint32_t m_crtcId = 0;
    uint32_t m_crtcIndex = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_refreshRate = 60;
    void* m_drmResources = nullptr; // drmModeRes*
    void* m_drmConnector = nullptr; // drmModeConnector*
    void* m_drmMode = nullptr;      // drmModeModeInfo*

    // GBM state
    gbm_device* m_gbmDevice = nullptr;
    gbm_surface* m_gbmSurface = nullptr;
    gbm_bo* m_previousBO = nullptr;
    DRMFramebuffer* m_previousFB = nullptr;

    // EGL state
    EGLDisplay m_eglDisplay = nullptr;
    EGLSurface m_eglSurface = nullptr;
    EGLContext m_eglContext = nullptr;
    EGLConfig m_eglConfig = nullptr;

    // Page flip state
    bool m_pageFlipPending = false;

    // Error state
    std::string m_lastError;
};

} // namespace rive_rk3566

#endif // SOULCAM_HAVE_RIVE
