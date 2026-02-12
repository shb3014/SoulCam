#pragma once
// ============================================================================
// ONVIF Device Service — WS-Discovery + SOAP endpoints
//
// Implements ONVIF Profile S/T device service for NVR/VMS auto-discovery:
//   1. WS-Discovery responder (UDP multicast 239.255.255.250:3702)
//   2. ONVIF SOAP device service (HTTP on configurable port, default 8080)
//
// Supported ONVIF operations:
//   - GetDeviceInformation
//   - GetCapabilities
//   - GetProfiles
//   - GetStreamUri
//   - GetServices
//   - GetScopes
//   - GetSystemDateAndTime
//   - GetNetworkInterfaces
//
// Zero external dependencies -- uses raw sockets and snprintf XML templates.
// ============================================================================

#include "soulcam.h"
#include <string>

namespace sc {

// Opaque handle for the ONVIF device service
struct OnvifDevice;

// ONVIF device service configuration
struct OnvifDeviceConfig {
    int    http_port        = 8080;    // HTTP SOAP service port
    bool   enable_discovery = true;    // WS-Discovery (UDP multicast)
    // Device metadata
    std::string manufacturer  = "SoulCam";
    std::string model         = "SC-RK3566";
    std::string firmware_ver  = "0.2.0";
    std::string serial_number = "SOULCAM-001";
    std::string hardware_id   = "RK3566-AXON";
    // Network
    std::string device_ip;             // Filled at runtime from interface query
};

// Start the ONVIF device service.
// Creates WS-Discovery responder + HTTP SOAP service threads.
OnvifDevice* onvif_device_start(const Config& cfg, const OnvifDeviceConfig& dev_cfg);

// Stop and destroy the ONVIF device service.
void onvif_device_stop(OnvifDevice* dev);

}  // namespace sc
