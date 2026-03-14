/*
 * Copyright 2025 Rive
 * DRM/KMS + EGL Context Implementation for RK3566
 */
#ifdef SOULCAM_HAVE_RIVE

#include "drm_egl_context.h"
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <cstdlib>

namespace rive_rk3566
{

DRMEGLContext::DRMEGLContext() = default;

DRMEGLContext::~DRMEGLContext()
{
    // Cleanup EGL
    if (m_eglDisplay != EGL_NO_DISPLAY)
    {
        eglMakeCurrent(m_eglDisplay,
                       EGL_NO_SURFACE,
                       EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
        if (m_eglContext != EGL_NO_CONTEXT)
            eglDestroyContext(m_eglDisplay, m_eglContext);
        if (m_eglSurface != EGL_NO_SURFACE)
            eglDestroySurface(m_eglDisplay, m_eglSurface);
        eglTerminate(m_eglDisplay);
    }

    // Cleanup GBM
    if (m_gbmSurface)
        gbm_surface_destroy(m_gbmSurface);
    if (m_gbmDevice)
        gbm_device_destroy(m_gbmDevice);

    // Cleanup DRM
    if (m_drmConnector)
        drmModeFreeConnector(static_cast<drmModeConnector*>(m_drmConnector));
    if (m_drmResources)
        drmModeFreeResources(static_cast<drmModeRes*>(m_drmResources));
    if (m_drmFd >= 0)
        close(m_drmFd);
}

bool DRMEGLContext::initialize(int width, int height)
{
    if (!initDRM())
        return false;

    if (!findDisplay())
        return false;

    // Use requested dimensions or display native resolution
    if (width > 0 && height > 0)
    {
        m_width = width;
        m_height = height;
    }

    if (!setupCRTC())
        return false;

    if (!initGBM())
        return false;

    if (!initEGL())
        return false;

    std::cout << "DRM/EGL initialized successfully:\n";
    std::cout << "  Display: " << m_width << "x" << m_height << "@"
              << m_refreshRate << "Hz\n";
    std::cout << "  EGL Version: " << eglQueryString(m_eglDisplay, EGL_VERSION)
              << "\n";
    std::cout << "  EGL Vendor: " << eglQueryString(m_eglDisplay, EGL_VENDOR)
              << "\n";
    std::cout << "  GL Version: " << glGetString(GL_VERSION) << "\n";
    std::cout << "  GL Renderer: " << glGetString(GL_RENDERER) << "\n";

    return true;
}

bool DRMEGLContext::initDRM()
{
    // Try to open DRM device
    const char* drmDevices[] = {"/dev/dri/card0", "/dev/dri/card1"};

    for (const char* device : drmDevices)
    {
        m_drmFd = open(device, O_RDWR | O_CLOEXEC);
        if (m_drmFd >= 0)
        {
            std::cout << "Opened DRM device: " << device << "\n";
            break;
        }
    }

    if (m_drmFd < 0)
    {
        setError("Failed to open DRM device. Are you running as root or in "
                 "video group?");
        return false;
    }

    // Check if we have DRM master
    if (drmIsMaster(m_drmFd) == 0)
    {
        std::cout << "Warning: Not DRM master, trying to set...\n";
        if (drmSetMaster(m_drmFd) != 0)
        {
            std::cout << "Warning: Could not become DRM master. Display may be "
                         "in use.\n";
        }
    }

    return true;
}

bool DRMEGLContext::findDisplay()
{
    drmModeRes* resources = drmModeGetResources(m_drmFd);
    if (!resources)
    {
        setError("Failed to get DRM resources");
        return false;
    }
    m_drmResources = resources;

    // Find a connected connector
    drmModeConnector* connector = nullptr;
    for (int i = 0; i < resources->count_connectors; i++)
    {
        connector = drmModeGetConnector(m_drmFd, resources->connectors[i]);
        if (connector && connector->connection == DRM_MODE_CONNECTED &&
            connector->count_modes > 0)
        {
            m_drmConnector = connector;
            m_connectorId = connector->connector_id;
            break;
        }
        if (connector)
            drmModeFreeConnector(connector);
        connector = nullptr;
    }

    if (!connector)
    {
        setError("No connected display found");
        return false;
    }

    // Use the preferred mode (usually the first one)
    drmModeModeInfo* mode = &connector->modes[0];
    m_drmMode = mode;
    m_width = mode->hdisplay;
    m_height = mode->vdisplay;
    m_refreshRate = mode->vrefresh;

    std::cout << "Found display: " << m_width << "x" << m_height << "@"
              << m_refreshRate << "Hz\n";

    return true;
}

bool DRMEGLContext::setupCRTC()
{
    drmModeRes* resources = static_cast<drmModeRes*>(m_drmResources);
    drmModeConnector* connector =
        static_cast<drmModeConnector*>(m_drmConnector);

    // Find CRTC for this connector
    if (connector->encoder_id)
    {
        drmModeEncoder* encoder =
            drmModeGetEncoder(m_drmFd, connector->encoder_id);
        if (encoder)
        {
            m_crtcId = encoder->crtc_id;

            // Find CRTC index
            for (int i = 0; i < resources->count_crtcs; i++)
            {
                if (resources->crtcs[i] == m_crtcId)
                {
                    m_crtcIndex = i;
                    break;
                }
            }

            drmModeFreeEncoder(encoder);
            return true;
        }
    }

    // No encoder, find a compatible CRTC
    // Simplified: just use the first available CRTC
    if (resources->count_crtcs > 0)
    {
        m_crtcId = resources->crtcs[0];
        m_crtcIndex = 0;
        return true;
    }

    setError("Failed to find CRTC for connector");
    return false;
}

bool DRMEGLContext::initGBM()
{
    m_gbmDevice = gbm_create_device(m_drmFd);
    if (!m_gbmDevice)
    {
        setError("Failed to create GBM device");
        return false;
    }

    m_gbmSurface = gbm_surface_create(m_gbmDevice,
                                      m_width,
                                      m_height,
                                      GBM_FORMAT_ARGB8888,
                                      GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if (!m_gbmSurface)
    {
        setError("Failed to create GBM surface");
        return false;
    }

    return true;
}

bool DRMEGLContext::initEGL()
{
    // Get EGL display from GBM device
    m_eglDisplay = eglGetDisplay((EGLNativeDisplayType)m_gbmDevice);
    if (m_eglDisplay == EGL_NO_DISPLAY)
    {
        setError("Failed to get EGL display");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(m_eglDisplay, &major, &minor))
    {
        setError("Failed to initialize EGL");
        return false;
    }

    std::cout << "EGL " << major << "." << minor << " initialized\n";

    // Choose EGL config for OpenGL ES 3.2
    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE};

    EGLint numConfigs;
    EGLConfig config;
    if (!eglChooseConfig(m_eglDisplay, configAttribs, &config, 1, &numConfigs) ||
        numConfigs == 0)
    {
        setError("Failed to choose EGL config");
        return false;
    }
    m_eglConfig = config;

    // Bind OpenGL ES API
    if (!eglBindAPI(EGL_OPENGL_ES_API))
    {
        setError("Failed to bind OpenGL ES API");
        return false;
    }

    // Create EGL context with ES 3.0 (llvmpipe may not fully support 3.2)
    const EGLint contextAttribs[] = {EGL_CONTEXT_MAJOR_VERSION,
                                     3,
                                     EGL_CONTEXT_MINOR_VERSION,
                                     0,
                                     EGL_NONE};

    m_eglContext =
        eglCreateContext(m_eglDisplay, config, EGL_NO_CONTEXT, contextAttribs);
    if (m_eglContext == EGL_NO_CONTEXT)
    {
        setError("Failed to create EGL context");
        return false;
    }
    std::cout << "Created OpenGL ES 3.0 context\n";

    // Create EGL surface from GBM surface
    m_eglSurface = eglCreateWindowSurface(
        m_eglDisplay,
        config,
        (EGLNativeWindowType)m_gbmSurface,
        nullptr);
    if (m_eglSurface == EGL_NO_SURFACE)
    {
        EGLint eglError = eglGetError();
        char errorMsg[256];
        snprintf(errorMsg, sizeof(errorMsg), "Failed to create EGL surface (EGL error: 0x%x)", eglError);
        setError(errorMsg);
        return false;
    }

    // Make context current
    if (!eglMakeCurrent(m_eglDisplay, m_eglSurface, m_eglSurface, m_eglContext))
    {
        setError("Failed to make EGL context current");
        return false;
    }

    // VSync control (default ON). Set RIVE_VSYNC=0 to disable and measure
    // uncapped throughput.
    int swapInterval = 1;
    if (const char* v = std::getenv("RIVE_VSYNC"))
    {
        // Treat empty as enabled; treat "0"/"false"/"no"/"off" as disabled.
        if (*v != '\0' && (!strcasecmp(v, "0") || !strcasecmp(v, "false") ||
                           !strcasecmp(v, "no") || !strcasecmp(v, "off")))
        {
            swapInterval = 0;
        }
    }
    eglSwapInterval(m_eglDisplay, swapInterval);

    return true;
}

void* (*DRMEGLContext::getProcAddress())(const char*)
{
    return (void* (*)(const char*))&eglGetProcAddress;
}

void DRMEGLContext::beginFrame()
{
    // Clear any previous errors
    glGetError();
}

DRMFramebuffer* DRMEGLContext::getFBFromBO(gbm_bo* bo)
{
    DRMFramebuffer* fb = static_cast<DRMFramebuffer*>(gbm_bo_get_user_data(bo));
    if (fb)
        return fb;

    // Create new framebuffer
    fb = new DRMFramebuffer();
    fb->bo = bo;
    fb->handle = gbm_bo_get_handle(bo).u32;
    fb->pitch = gbm_bo_get_stride(bo);
    fb->offset = 0;

    int ret = drmModeAddFB(m_drmFd,
                           m_width,
                           m_height,
                           24,
                           32,
                           fb->pitch,
                           fb->handle,
                           &fb->fb_id);

    if (ret)
    {
        std::cerr << "Failed to create framebuffer: " << strerror(errno)
                  << "\n";
        delete fb;
        return nullptr;
    }

    gbm_bo_set_user_data(bo, fb, [](gbm_bo* bo, void* data) {
        DRMFramebuffer* fb = static_cast<DRMFramebuffer*>(data);
        if (fb && fb->fb_id)
        {
            // Get DRM fd from somewhere - we'll need to pass it
            // For now, this is called during destruction, so it's okay
        }
        delete fb;
    });

    return fb;
}

void DRMEGLContext::pageFlipHandler(int fd,
                                    unsigned int frame,
                                    unsigned int sec,
                                    unsigned int usec,
                                    void* data)
{
    DRMEGLContext* ctx = static_cast<DRMEGLContext*>(data);
    ctx->m_pageFlipPending = false;
}

void DRMEGLContext::waitForPageFlip()
{
    if (!m_pageFlipPending)
        return;

    drmEventContext evctx = {};
    evctx.version = DRM_EVENT_CONTEXT_VERSION;
    evctx.page_flip_handler = pageFlipHandler;

    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(m_drmFd, &fds);

    // Wait for page flip with timeout
    timeval timeout = {1, 0}; // 1 second timeout
    int ret = select(m_drmFd + 1, &fds, nullptr, nullptr, &timeout);
    if (ret < 0)
    {
        std::cerr << "select() failed: " << strerror(errno) << "\n";
        m_pageFlipPending = false;
    }
    else if (ret == 0)
    {
        std::cerr << "Page flip timeout\n";
        m_pageFlipPending = false;
    }
    else
    {
        drmHandleEvent(m_drmFd, &evctx);
    }
}

void DRMEGLContext::swapBuffers()
{
    // Swap EGL buffers
    eglSwapBuffers(m_eglDisplay, m_eglSurface);

    // Get the next buffer object
    gbm_bo* bo = gbm_surface_lock_front_buffer(m_gbmSurface);
    if (!bo)
    {
        std::cerr << "Failed to lock front buffer\n";
        return;
    }

    DRMFramebuffer* fb = getFBFromBO(bo);
    if (!fb)
    {
        gbm_surface_release_buffer(m_gbmSurface, bo);
        return;
    }

    // Wait for previous page flip to complete
    waitForPageFlip();

    // Queue page flip
    int ret = drmModePageFlip(m_drmFd,
                              m_crtcId,
                              fb->fb_id,
                              DRM_MODE_PAGE_FLIP_EVENT,
                              this);

    if (ret)
    {
        std::cerr << "Page flip failed: " << strerror(errno) << "\n";
        // Fallback to setcrtc
        drmModeSetCrtc(m_drmFd,
                       m_crtcId,
                       fb->fb_id,
                       0,
                       0,
                       &m_connectorId,
                       1,
                       static_cast<drmModeModeInfo*>(m_drmMode));
    }
    else
    {
        m_pageFlipPending = true;
    }

    // Release previous buffer
    if (m_previousBO)
    {
        gbm_surface_release_buffer(m_gbmSurface, m_previousBO);
    }

    m_previousBO = bo;
    m_previousFB = fb;
}

void DRMEGLContext::setError(const std::string& error)
{
    m_lastError = error;
    std::cerr << "DRM/EGL Error: " << error << "\n";
}

} // namespace rive_rk3566

#endif // SOULCAM_HAVE_RIVE
