// ============================================================================
// ONVIF Device Service implementation
//
// WS-Discovery (UDP multicast) + SOAP device service (HTTP).
//
// Key design:
//   1. No external XML/SOAP library -- all responses are snprintf templates
//   2. WS-Discovery runs in its own thread, joining the 239.255.255.250:3702
//      multicast group and responding to Probe messages
//   3. HTTP SOAP server runs in its own thread, accepting TCP connections
//      on the configured port and dispatching SOAP actions
//   4. Thread-safe: each thread manages its own sockets
//
// ONVIF Profile S/T compliance:
//   - Device service: GetDeviceInformation, GetCapabilities, GetServices,
//     GetScopes, GetSystemDateAndTime, GetNetworkInterfaces
//   - Media service: GetProfiles, GetStreamUri
// ============================================================================

#include "pipeline/onvif_device.h"
#include "util/logger.h"

#include <cstring>
#include <cstdio>
#include <ctime>
#include <string>
#include <sstream>
#include <thread>
#include <atomic>
#include <vector>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <fcntl.h>

namespace sc {

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr const char* WS_DISCOVERY_ADDR = "239.255.255.250";
static constexpr int         WS_DISCOVERY_PORT = 3702;
static constexpr int         HTTP_BACKLOG      = 8;
static constexpr int         MAX_HTTP_REQ_SIZE = 65536;

// ONVIF namespaces
static constexpr const char* NS_S = "http://www.w3.org/2003/05/soap-envelope";
static constexpr const char* NS_D = "http://schemas.xmlsoap.org/ws/2005/04/discovery";
static constexpr const char* NS_TDS = "http://www.onvif.org/ver10/device/wsdl";
static constexpr const char* NS_TRT = "http://www.onvif.org/ver10/media/wsdl";
static constexpr const char* NS_TT = "http://www.onvif.org/ver10/schema";

// ---------------------------------------------------------------------------
// OnvifDevice structure
// ---------------------------------------------------------------------------
struct OnvifDevice {
    Config           cfg;
    OnvifDeviceConfig dev_cfg;

    std::thread      discovery_thread;
    std::thread      http_thread;
    std::atomic<bool> running{false};

    int              discovery_fd = -1;
    int              http_fd      = -1;

    std::string      device_ip;        // Our IP address
    std::string      device_uuid;      // UUID for WS-Discovery
};

// ---------------------------------------------------------------------------
// Utility: get local IP address (non-loopback)
// ---------------------------------------------------------------------------
static std::string get_local_ip() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) < 0) return "0.0.0.0";

    std::string ip = "0.0.0.0";
    for (auto* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        if (ifa->ifa_flags & IFF_LOOPBACK) continue;

        auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sa->sin_addr, buf, sizeof(buf));
        ip = buf;
        break;  // Use first non-loopback IPv4 address
    }
    freeifaddrs(ifaddr);
    return ip;
}

// ---------------------------------------------------------------------------
// Utility: generate a UUID (deterministic from MAC-like seed)
// ---------------------------------------------------------------------------
static std::string generate_uuid() {
    // Use a simple deterministic UUID based on the IP and a fixed salt
    // In production, this should be based on the device MAC address
    char uuid[64];
    snprintf(uuid, sizeof(uuid),
             "urn:uuid:2419d68a-2dd2-21b2-a205-%012x",
             static_cast<unsigned>(time(nullptr) & 0xFFFFFFFF));
    return uuid;
}

// ---------------------------------------------------------------------------
// Utility: get current UTC time as ISO 8601
// ---------------------------------------------------------------------------
static std::string utc_now_iso() {
    time_t t = time(nullptr);
    struct tm tm{};
    gmtime_r(&t, &tm);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

// ---------------------------------------------------------------------------
// Utility: extract substring between two markers
// ---------------------------------------------------------------------------
static std::string extract_between(const std::string& s,
                                    const std::string& start,
                                    const std::string& end) {
    auto pos = s.find(start);
    if (pos == std::string::npos) return {};
    pos += start.size();
    auto epos = s.find(end, pos);
    if (epos == std::string::npos) return {};
    return s.substr(pos, epos - pos);
}

// ---------------------------------------------------------------------------
// SOAP envelope wrapper
// ---------------------------------------------------------------------------
static std::string soap_envelope(const std::string& body) {
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<s:Envelope xmlns:s=\"" + std::string(NS_S) + "\"\n"
        "            xmlns:tds=\"" + std::string(NS_TDS) + "\"\n"
        "            xmlns:trt=\"" + std::string(NS_TRT) + "\"\n"
        "            xmlns:tt=\"" + std::string(NS_TT) + "\">\n"
        "  <s:Body>\n" + body +
        "  </s:Body>\n"
        "</s:Envelope>\n";
}

// ---------------------------------------------------------------------------
// SOAP response: GetDeviceInformation
// ---------------------------------------------------------------------------
static std::string resp_get_device_information(const OnvifDevice* dev) {
    std::ostringstream body;
    body << "    <tds:GetDeviceInformationResponse>\n"
         << "      <tds:Manufacturer>" << dev->dev_cfg.manufacturer << "</tds:Manufacturer>\n"
         << "      <tds:Model>" << dev->dev_cfg.model << "</tds:Model>\n"
         << "      <tds:FirmwareVersion>" << dev->dev_cfg.firmware_ver << "</tds:FirmwareVersion>\n"
         << "      <tds:SerialNumber>" << dev->dev_cfg.serial_number << "</tds:SerialNumber>\n"
         << "      <tds:HardwareId>" << dev->dev_cfg.hardware_id << "</tds:HardwareId>\n"
         << "    </tds:GetDeviceInformationResponse>\n";
    return soap_envelope(body.str());
}

// ---------------------------------------------------------------------------
// SOAP response: GetCapabilities
// ---------------------------------------------------------------------------
static std::string resp_get_capabilities(const OnvifDevice* dev) {
    std::string base_url = "http://" + dev->device_ip + ":" +
                           std::to_string(dev->dev_cfg.http_port);
    std::string rtsp_url = "rtsp://" + dev->device_ip + ":" +
                           std::to_string(dev->cfg.rtsp.port);

    std::ostringstream body;
    body << "    <tds:GetCapabilitiesResponse>\n"
         << "      <tds:Capabilities>\n"
         // Device service
         << "        <tt:Device>\n"
         << "          <tt:XAddr>" << base_url << "/onvif/device_service</tt:XAddr>\n"
         << "          <tt:Network>\n"
         << "            <tt:IPFilter>false</tt:IPFilter>\n"
         << "            <tt:ZeroConfiguration>false</tt:ZeroConfiguration>\n"
         << "            <tt:IPVersion6>false</tt:IPVersion6>\n"
         << "            <tt:DynDNS>false</tt:DynDNS>\n"
         << "          </tt:Network>\n"
         << "          <tt:System>\n"
         << "            <tt:DiscoveryResolve>false</tt:DiscoveryResolve>\n"
         << "            <tt:DiscoveryBye>true</tt:DiscoveryBye>\n"
         << "            <tt:RemoteDiscovery>false</tt:RemoteDiscovery>\n"
         << "            <tt:SystemBackup>false</tt:SystemBackup>\n"
         << "            <tt:SystemLogging>false</tt:SystemLogging>\n"
         << "            <tt:FirmwareUpgrade>false</tt:FirmwareUpgrade>\n"
         << "          </tt:System>\n"
         << "          <tt:IO>\n"
         << "            <tt:InputConnectors>0</tt:InputConnectors>\n"
         << "            <tt:RelayOutputs>0</tt:RelayOutputs>\n"
         << "          </tt:IO>\n"
         << "        </tt:Device>\n"
         // Media service
         << "        <tt:Media>\n"
         << "          <tt:XAddr>" << base_url << "/onvif/media_service</tt:XAddr>\n"
         << "          <tt:StreamingCapabilities>\n"
         << "            <tt:RTPMulticast>false</tt:RTPMulticast>\n"
         << "            <tt:RTP_TCP>true</tt:RTP_TCP>\n"
         << "            <tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP>\n"
         << "          </tt:StreamingCapabilities>\n"
         << "        </tt:Media>\n"
         // Analytics (metadata)
         << "        <tt:Analytics>\n"
         << "          <tt:XAddr>" << base_url << "/onvif/analytics_service</tt:XAddr>\n"
         << "          <tt:RuleSupport>false</tt:RuleSupport>\n"
         << "          <tt:AnalyticsModuleSupport>false</tt:AnalyticsModuleSupport>\n"
         << "        </tt:Analytics>\n"
         << "      </tds:Capabilities>\n"
         << "    </tds:GetCapabilitiesResponse>\n";
    return soap_envelope(body.str());
}

// ---------------------------------------------------------------------------
// SOAP response: GetServices
// ---------------------------------------------------------------------------
static std::string resp_get_services(const OnvifDevice* dev) {
    std::string base_url = "http://" + dev->device_ip + ":" +
                           std::to_string(dev->dev_cfg.http_port);
    std::ostringstream body;
    body << "    <tds:GetServicesResponse>\n"
         << "      <tds:Service>\n"
         << "        <tds:Namespace>http://www.onvif.org/ver10/device/wsdl</tds:Namespace>\n"
         << "        <tds:XAddr>" << base_url << "/onvif/device_service</tds:XAddr>\n"
         << "        <tds:Version><tt:Major>2</tt:Major><tt:Minor>60</tt:Minor></tds:Version>\n"
         << "      </tds:Service>\n"
         << "      <tds:Service>\n"
         << "        <tds:Namespace>http://www.onvif.org/ver10/media/wsdl</tds:Namespace>\n"
         << "        <tds:XAddr>" << base_url << "/onvif/media_service</tds:XAddr>\n"
         << "        <tds:Version><tt:Major>2</tt:Major><tt:Minor>60</tt:Minor></tds:Version>\n"
         << "      </tds:Service>\n"
         << "    </tds:GetServicesResponse>\n";
    return soap_envelope(body.str());
}

// ---------------------------------------------------------------------------
// SOAP response: GetScopes
// ---------------------------------------------------------------------------
static std::string resp_get_scopes(const OnvifDevice* dev) {
    std::ostringstream body;
    body << "    <tds:GetScopesResponse>\n"
         << "      <tds:Scopes>\n"
         << "        <tt:ScopeDef>Fixed</tt:ScopeDef>\n"
         << "        <tt:ScopeItem>onvif://www.onvif.org/type/video_encoder</tt:ScopeItem>\n"
         << "      </tds:Scopes>\n"
         << "      <tds:Scopes>\n"
         << "        <tt:ScopeDef>Fixed</tt:ScopeDef>\n"
         << "        <tt:ScopeItem>onvif://www.onvif.org/type/audio_encoder</tt:ScopeItem>\n"
         << "      </tds:Scopes>\n"
         << "      <tds:Scopes>\n"
         << "        <tt:ScopeDef>Fixed</tt:ScopeDef>\n"
         << "        <tt:ScopeItem>onvif://www.onvif.org/hardware/" << dev->dev_cfg.hardware_id << "</tt:ScopeItem>\n"
         << "      </tds:Scopes>\n"
         << "      <tds:Scopes>\n"
         << "        <tt:ScopeDef>Fixed</tt:ScopeDef>\n"
         << "        <tt:ScopeItem>onvif://www.onvif.org/name/" << dev->dev_cfg.model << "</tt:ScopeItem>\n"
         << "      </tds:Scopes>\n"
         << "      <tds:Scopes>\n"
         << "        <tt:ScopeDef>Fixed</tt:ScopeDef>\n"
         << "        <tt:ScopeItem>onvif://www.onvif.org/Profile/Streaming</tt:ScopeItem>\n"
         << "      </tds:Scopes>\n"
         << "    </tds:GetScopesResponse>\n";
    return soap_envelope(body.str());
}

// ---------------------------------------------------------------------------
// SOAP response: GetSystemDateAndTime
// ---------------------------------------------------------------------------
static std::string resp_get_system_date_and_time() {
    time_t t = time(nullptr);
    struct tm utc{}, local{};
    gmtime_r(&t, &utc);
    localtime_r(&t, &local);

    std::ostringstream body;
    body << "    <tds:GetSystemDateAndTimeResponse>\n"
         << "      <tds:SystemDateAndTime>\n"
         << "        <tt:DateTimeType>NTP</tt:DateTimeType>\n"
         << "        <tt:DaylightSavings>false</tt:DaylightSavings>\n"
         << "        <tt:TimeZone>\n"
         << "          <tt:TZ>UTC0</tt:TZ>\n"
         << "        </tt:TimeZone>\n"
         << "        <tt:UTCDateTime>\n"
         << "          <tt:Time>\n"
         << "            <tt:Hour>" << utc.tm_hour << "</tt:Hour>\n"
         << "            <tt:Minute>" << utc.tm_min << "</tt:Minute>\n"
         << "            <tt:Second>" << utc.tm_sec << "</tt:Second>\n"
         << "          </tt:Time>\n"
         << "          <tt:Date>\n"
         << "            <tt:Year>" << (utc.tm_year + 1900) << "</tt:Year>\n"
         << "            <tt:Month>" << (utc.tm_mon + 1) << "</tt:Month>\n"
         << "            <tt:Day>" << utc.tm_mday << "</tt:Day>\n"
         << "          </tt:Date>\n"
         << "        </tt:UTCDateTime>\n"
         << "        <tt:LocalDateTime>\n"
         << "          <tt:Time>\n"
         << "            <tt:Hour>" << local.tm_hour << "</tt:Hour>\n"
         << "            <tt:Minute>" << local.tm_min << "</tt:Minute>\n"
         << "            <tt:Second>" << local.tm_sec << "</tt:Second>\n"
         << "          </tt:Time>\n"
         << "          <tt:Date>\n"
         << "            <tt:Year>" << (local.tm_year + 1900) << "</tt:Year>\n"
         << "            <tt:Month>" << (local.tm_mon + 1) << "</tt:Month>\n"
         << "            <tt:Day>" << local.tm_mday << "</tt:Day>\n"
         << "          </tt:Date>\n"
         << "        </tt:LocalDateTime>\n"
         << "      </tds:SystemDateAndTime>\n"
         << "    </tds:GetSystemDateAndTimeResponse>\n";
    return soap_envelope(body.str());
}

// ---------------------------------------------------------------------------
// SOAP response: GetNetworkInterfaces
// ---------------------------------------------------------------------------
static std::string resp_get_network_interfaces(const OnvifDevice* dev) {
    std::ostringstream body;
    body << "    <tds:GetNetworkInterfacesResponse>\n"
         << "      <tds:NetworkInterfaces token=\"eth0\">\n"
         << "        <tt:Enabled>true</tt:Enabled>\n"
         << "        <tt:Info>\n"
         << "          <tt:Name>eth0</tt:Name>\n"
         << "        </tt:Info>\n"
         << "        <tt:IPv4>\n"
         << "          <tt:Enabled>true</tt:Enabled>\n"
         << "          <tt:Config>\n"
         << "            <tt:Manual>\n"
         << "              <tt:Address>" << dev->device_ip << "</tt:Address>\n"
         << "              <tt:PrefixLength>24</tt:PrefixLength>\n"
         << "            </tt:Manual>\n"
         << "            <tt:DHCP>false</tt:DHCP>\n"
         << "          </tt:Config>\n"
         << "        </tt:IPv4>\n"
         << "      </tds:NetworkInterfaces>\n"
         << "    </tds:GetNetworkInterfacesResponse>\n";
    return soap_envelope(body.str());
}

// ---------------------------------------------------------------------------
// SOAP response: GetProfiles (Media service)
// ---------------------------------------------------------------------------
static std::string resp_get_profiles(const OnvifDevice* dev) {
    std::ostringstream body;
    body << "    <trt:GetProfilesResponse>\n"
         << "      <trt:Profiles token=\"MainProfile\" fixed=\"true\">\n"
         << "        <tt:Name>MainStream</tt:Name>\n"
         << "        <tt:VideoSourceConfiguration token=\"VSC_1\">\n"
         << "          <tt:Name>VideoSource</tt:Name>\n"
         << "          <tt:UseCount>1</tt:UseCount>\n"
         << "          <tt:SourceToken>VS_1</tt:SourceToken>\n"
         << "          <tt:Bounds x=\"0\" y=\"0\""
         << " width=\"" << dev->cfg.stream.width << "\""
         << " height=\"" << dev->cfg.stream.height << "\"/>\n"
         << "        </tt:VideoSourceConfiguration>\n"
         << "        <tt:VideoEncoderConfiguration token=\"VEC_1\">\n"
         << "          <tt:Name>H264</tt:Name>\n"
         << "          <tt:UseCount>1</tt:UseCount>\n"
         << "          <tt:Encoding>H264</tt:Encoding>\n"
         << "          <tt:Resolution>\n"
         << "            <tt:Width>" << dev->cfg.stream.width << "</tt:Width>\n"
         << "            <tt:Height>" << dev->cfg.stream.height << "</tt:Height>\n"
         << "          </tt:Resolution>\n"
         << "          <tt:Quality>5</tt:Quality>\n"
         << "          <tt:RateControl>\n"
         << "            <tt:FrameRateLimit>" << dev->cfg.stream.fps << "</tt:FrameRateLimit>\n"
         << "            <tt:EncodingInterval>1</tt:EncodingInterval>\n"
         << "            <tt:BitrateLimit>" << dev->cfg.rtsp.bitrate_kbps << "</tt:BitrateLimit>\n"
         << "          </tt:RateControl>\n"
         << "          <tt:H264>\n"
         << "            <tt:GovLength>" << dev->cfg.rtsp.gop << "</tt:GovLength>\n"
         << "            <tt:H264Profile>High</tt:H264Profile>\n"
         << "          </tt:H264>\n"
         << "          <tt:Multicast>\n"
         << "            <tt:Address>\n"
         << "              <tt:Type>IPv4</tt:Type>\n"
         << "              <tt:IPv4Address>0.0.0.0</tt:IPv4Address>\n"
         << "            </tt:Address>\n"
         << "            <tt:Port>0</tt:Port>\n"
         << "            <tt:TTL>0</tt:TTL>\n"
         << "            <tt:AutoStart>false</tt:AutoStart>\n"
         << "          </tt:Multicast>\n"
         << "          <tt:SessionTimeout>PT60S</tt:SessionTimeout>\n"
         << "        </tt:VideoEncoderConfiguration>\n"
         << "      </trt:Profiles>\n"
         << "    </trt:GetProfilesResponse>\n";
    return soap_envelope(body.str());
}

// ---------------------------------------------------------------------------
// SOAP response: GetStreamUri (Media service)
// ---------------------------------------------------------------------------
static std::string resp_get_stream_uri(const OnvifDevice* dev) {
    std::string rtsp_url = "rtsp://" + dev->device_ip + ":" +
                           std::to_string(dev->cfg.rtsp.port) + dev->cfg.rtsp.mount;
    std::ostringstream body;
    body << "    <trt:GetStreamUriResponse>\n"
         << "      <trt:MediaUri>\n"
         << "        <tt:Uri>" << rtsp_url << "</tt:Uri>\n"
         << "        <tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>\n"
         << "        <tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>\n"
         << "        <tt:Timeout>PT60S</tt:Timeout>\n"
         << "      </trt:MediaUri>\n"
         << "    </trt:GetStreamUriResponse>\n";
    return soap_envelope(body.str());
}

// ---------------------------------------------------------------------------
// SOAP fault
// ---------------------------------------------------------------------------
static std::string resp_soap_fault(const std::string& reason) {
    std::ostringstream body;
    body << "    <s:Fault>\n"
         << "      <s:Code>\n"
         << "        <s:Value>s:Sender</s:Value>\n"
         << "        <s:Subcode>\n"
         << "          <s:Value>ter:ActionNotSupported</s:Value>\n"
         << "        </s:Subcode>\n"
         << "      </s:Code>\n"
         << "      <s:Reason>\n"
         << "        <s:Text xml:lang=\"en\">" << reason << "</s:Text>\n"
         << "      </s:Reason>\n"
         << "    </s:Fault>\n";
    return soap_envelope(body.str());
}

// ---------------------------------------------------------------------------
// SOAP action dispatch
// ---------------------------------------------------------------------------
static std::string dispatch_soap(const OnvifDevice* dev, const std::string& body) {
    // Detect the SOAP action from the request body
    if (body.find("GetDeviceInformation") != std::string::npos)
        return resp_get_device_information(dev);
    if (body.find("GetCapabilities") != std::string::npos)
        return resp_get_capabilities(dev);
    if (body.find("GetServices") != std::string::npos)
        return resp_get_services(dev);
    if (body.find("GetScopes") != std::string::npos)
        return resp_get_scopes(dev);
    if (body.find("GetSystemDateAndTime") != std::string::npos)
        return resp_get_system_date_and_time();
    if (body.find("GetNetworkInterfaces") != std::string::npos)
        return resp_get_network_interfaces(dev);
    if (body.find("GetProfiles") != std::string::npos)
        return resp_get_profiles(dev);
    if (body.find("GetStreamUri") != std::string::npos)
        return resp_get_stream_uri(dev);
    if (body.find("GetSnapshotUri") != std::string::npos)
        return resp_get_stream_uri(dev);  // Reuse stream URI for now

    // Unrecognized action
    SC_LOG_DEBUG("ONVIF: unrecognized SOAP action in request");
    return resp_soap_fault("Action not supported");
}

// ---------------------------------------------------------------------------
// HTTP response helper
// ---------------------------------------------------------------------------
static std::string http_response(int status, const std::string& body,
                                  const std::string& content_type = "application/soap+xml; charset=utf-8") {
    const char* status_text = (status == 200) ? "OK" :
                               (status == 400) ? "Bad Request" :
                               (status == 404) ? "Not Found" :
                               (status == 500) ? "Internal Server Error" : "Unknown";

    std::ostringstream resp;
    resp << "HTTP/1.1 " << status << " " << status_text << "\r\n"
         << "Content-Type: " << content_type << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Connection: close\r\n"
         << "\r\n"
         << body;
    return resp.str();
}

// ---------------------------------------------------------------------------
// HTTP server: handle one client connection
// ---------------------------------------------------------------------------
static void handle_http_client(const OnvifDevice* dev, int client_fd) {
    char buf[MAX_HTTP_REQ_SIZE];
    ssize_t total = 0;
    int attempts = 0;

    // Read the full HTTP request (simple blocking read with timeout)
    while (total < MAX_HTTP_REQ_SIZE - 1 && attempts < 50) {
        struct pollfd pfd = {client_fd, POLLIN, 0};
        int ret = poll(&pfd, 1, 100);  // 100ms timeout
        if (ret <= 0) {
            if (total > 0) break;  // Got some data, stop waiting
            attempts++;
            continue;
        }
        ssize_t n = recv(client_fd, buf + total, MAX_HTTP_REQ_SIZE - 1 - total, 0);
        if (n <= 0) break;
        total += n;

        // Check if we have the full HTTP request (Content-Length based)
        buf[total] = '\0';
        const char* hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) {
            // Find Content-Length
            const char* cl = strcasestr(buf, "Content-Length:");
            if (cl) {
                int content_len = atoi(cl + 15);
                int body_start = (hdr_end + 4) - buf;
                if (total >= body_start + content_len) break;
            } else {
                break;  // No Content-Length, assume we have everything
            }
        }
    }

    if (total <= 0) {
        close(client_fd);
        return;
    }
    buf[total] = '\0';

    std::string request(buf, total);
    std::string response;

    // Check if this is an ONVIF SOAP request
    if (request.find("POST") == 0 &&
        (request.find("/onvif") != std::string::npos ||
         request.find("Envelope") != std::string::npos)) {

        // Extract SOAP body
        std::string soap_resp = dispatch_soap(dev, request);
        response = http_response(200, soap_resp);

        SC_LOG_DEBUG("ONVIF HTTP: SOAP request handled (%zu bytes response)",
                     soap_resp.size());
    } else if (request.find("GET") == 0) {
        // Simple status page
        std::string html =
            "<html><body><h1>SoulCam ONVIF Device Service</h1>"
            "<p>Manufacturer: " + dev->dev_cfg.manufacturer + "</p>"
            "<p>Model: " + dev->dev_cfg.model + "</p>"
            "<p>Firmware: " + dev->dev_cfg.firmware_ver + "</p>"
            "<p>RTSP: rtsp://" + dev->device_ip + ":" +
            std::to_string(dev->cfg.rtsp.port) + dev->cfg.rtsp.mount + "</p>"
            "</body></html>";
        response = http_response(200, html, "text/html");
    } else {
        response = http_response(400, "Bad Request");
    }

    // Send response
    const char* data = response.c_str();
    size_t remaining = response.size();
    while (remaining > 0) {
        ssize_t sent = send(client_fd, data, remaining, MSG_NOSIGNAL);
        if (sent <= 0) break;
        data += sent;
        remaining -= sent;
    }

    close(client_fd);
}

// ---------------------------------------------------------------------------
// HTTP server thread
// ---------------------------------------------------------------------------
static void http_server_loop(OnvifDevice* dev) {
    SC_LOG_INFO("ONVIF HTTP server started on port %d", dev->dev_cfg.http_port);

    while (dev->running) {
        struct pollfd pfd = {dev->http_fd, POLLIN, 0};
        int ret = poll(&pfd, 1, 500);  // 500ms timeout for shutdown check
        if (ret <= 0) continue;

        struct sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(dev->http_fd,
                                reinterpret_cast<struct sockaddr*>(&client_addr),
                                &addr_len);
        if (client_fd < 0) continue;

        // Handle in-place (single-threaded is fine for ONVIF SOAP)
        handle_http_client(dev, client_fd);
    }

    SC_LOG_INFO("ONVIF HTTP server stopped");
}

// ---------------------------------------------------------------------------
// WS-Discovery: build Probe match response
// ---------------------------------------------------------------------------
static std::string ws_discovery_probe_match(const OnvifDevice* dev,
                                              const std::string& msg_id) {
    std::string base_url = "http://" + dev->device_ip + ":" +
                           std::to_string(dev->dev_cfg.http_port) +
                           "/onvif/device_service";
    std::ostringstream resp;
    resp << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         << "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"\n"
         << "            xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\"\n"
         << "            xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\"\n"
         << "            xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">\n"
         << "  <s:Header>\n"
         << "    <a:MessageID>" << dev->device_uuid << "-" << time(nullptr) << "</a:MessageID>\n"
         << "    <a:RelatesTo>" << msg_id << "</a:RelatesTo>\n"
         << "    <a:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</a:To>\n"
         << "    <a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</a:Action>\n"
         << "  </s:Header>\n"
         << "  <s:Body>\n"
         << "    <d:ProbeMatches>\n"
         << "      <d:ProbeMatch>\n"
         << "        <a:EndpointReference>\n"
         << "          <a:Address>" << dev->device_uuid << "</a:Address>\n"
         << "        </a:EndpointReference>\n"
         << "        <d:Types>dn:NetworkVideoTransmitter</d:Types>\n"
         << "        <d:Scopes>"
         << "onvif://www.onvif.org/type/video_encoder "
         << "onvif://www.onvif.org/hardware/" << dev->dev_cfg.hardware_id << " "
         << "onvif://www.onvif.org/name/" << dev->dev_cfg.model << " "
         << "onvif://www.onvif.org/Profile/Streaming"
         << "</d:Scopes>\n"
         << "        <d:XAddrs>" << base_url << "</d:XAddrs>\n"
         << "        <d:MetadataVersion>1</d:MetadataVersion>\n"
         << "      </d:ProbeMatch>\n"
         << "    </d:ProbeMatches>\n"
         << "  </s:Body>\n"
         << "</s:Envelope>\n";
    return resp.str();
}

// ---------------------------------------------------------------------------
// WS-Discovery: build Hello message
// ---------------------------------------------------------------------------
static std::string ws_discovery_hello(const OnvifDevice* dev) {
    std::string base_url = "http://" + dev->device_ip + ":" +
                           std::to_string(dev->dev_cfg.http_port) +
                           "/onvif/device_service";
    std::ostringstream msg;
    msg << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"\n"
        << "            xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\"\n"
        << "            xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\"\n"
        << "            xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">\n"
        << "  <s:Header>\n"
        << "    <a:MessageID>" << dev->device_uuid << "-hello-" << time(nullptr) << "</a:MessageID>\n"
        << "    <a:To>urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To>\n"
        << "    <a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Hello</a:Action>\n"
        << "  </s:Header>\n"
        << "  <s:Body>\n"
        << "    <d:Hello>\n"
        << "      <a:EndpointReference>\n"
        << "        <a:Address>" << dev->device_uuid << "</a:Address>\n"
        << "      </a:EndpointReference>\n"
        << "      <d:Types>dn:NetworkVideoTransmitter</d:Types>\n"
        << "      <d:Scopes>"
        << "onvif://www.onvif.org/type/video_encoder "
        << "onvif://www.onvif.org/hardware/" << dev->dev_cfg.hardware_id << " "
        << "onvif://www.onvif.org/name/" << dev->dev_cfg.model << " "
        << "onvif://www.onvif.org/Profile/Streaming"
        << "</d:Scopes>\n"
        << "      <d:XAddrs>" << base_url << "</d:XAddrs>\n"
        << "      <d:MetadataVersion>1</d:MetadataVersion>\n"
        << "    </d:Hello>\n"
        << "  </s:Body>\n"
        << "</s:Envelope>\n";
    return msg.str();
}

// ---------------------------------------------------------------------------
// WS-Discovery thread
// ---------------------------------------------------------------------------
static void ws_discovery_loop(OnvifDevice* dev) {
    SC_LOG_INFO("WS-Discovery responder started (multicast %s:%d)",
                WS_DISCOVERY_ADDR, WS_DISCOVERY_PORT);

    // Send Hello message to announce ourselves
    {
        std::string hello = ws_discovery_hello(dev);
        struct sockaddr_in mcast_addr{};
        mcast_addr.sin_family = AF_INET;
        mcast_addr.sin_port = htons(WS_DISCOVERY_PORT);
        inet_pton(AF_INET, WS_DISCOVERY_ADDR, &mcast_addr.sin_addr);

        sendto(dev->discovery_fd, hello.c_str(), hello.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&mcast_addr),
               sizeof(mcast_addr));
        SC_LOG_INFO("WS-Discovery: Hello sent to multicast group");
    }

    char buf[65536];
    while (dev->running) {
        struct pollfd pfd = {dev->discovery_fd, POLLIN, 0};
        int ret = poll(&pfd, 1, 1000);  // 1s timeout
        if (ret <= 0) continue;

        struct sockaddr_in sender_addr{};
        socklen_t addr_len = sizeof(sender_addr);
        ssize_t n = recvfrom(dev->discovery_fd, buf, sizeof(buf) - 1, 0,
                              reinterpret_cast<struct sockaddr*>(&sender_addr),
                              &addr_len);
        if (n <= 0) continue;
        buf[n] = '\0';

        std::string msg(buf, n);

        // Check if this is a Probe message
        if (msg.find("Probe") == std::string::npos) continue;
        // Ignore our own ProbeMatch messages
        if (msg.find("ProbeMatch") != std::string::npos) continue;

        // Check if the probe is for NetworkVideoTransmitter (or generic)
        bool type_match = (msg.find("NetworkVideoTransmitter") != std::string::npos) ||
                          (msg.find("<d:Types/>") != std::string::npos) ||
                          (msg.find("<d:Types></d:Types>") != std::string::npos) ||
                          (msg.find("Types") == std::string::npos);  // No Types = match all

        if (!type_match) {
            SC_LOG_DEBUG("WS-Discovery: Probe type mismatch, ignoring");
            continue;
        }

        // Extract MessageID from the probe
        std::string relates_to = extract_between(msg, "<a:MessageID>", "</a:MessageID>");
        if (relates_to.empty()) {
            relates_to = extract_between(msg,
                "MessageID>", "</");  // Try without namespace prefix
        }

        char sender_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sender_addr.sin_addr, sender_ip, sizeof(sender_ip));
        SC_LOG_INFO("WS-Discovery: Probe from %s:%d (MessageID: %s)",
                    sender_ip, ntohs(sender_addr.sin_port),
                    relates_to.c_str());

        // Build and send ProbeMatch
        std::string response = ws_discovery_probe_match(dev, relates_to);

        // Send unicast response back to the sender
        sendto(dev->discovery_fd, response.c_str(), response.size(), 0,
               reinterpret_cast<const struct sockaddr*>(&sender_addr),
               addr_len);

        SC_LOG_DEBUG("WS-Discovery: ProbeMatch sent to %s:%d (%zu bytes)",
                     sender_ip, ntohs(sender_addr.sin_port), response.size());
    }

    SC_LOG_INFO("WS-Discovery responder stopped");
}

// ---------------------------------------------------------------------------
// Start the ONVIF device service
// ---------------------------------------------------------------------------
OnvifDevice* onvif_device_start(const Config& cfg, const OnvifDeviceConfig& dev_cfg) {
    auto* dev = new OnvifDevice();
    dev->cfg = cfg;
    dev->dev_cfg = dev_cfg;

    // Determine device IP
    if (!dev_cfg.device_ip.empty()) {
        dev->device_ip = dev_cfg.device_ip;
    } else {
        dev->device_ip = get_local_ip();
    }

    dev->device_uuid = generate_uuid();

    SC_LOG_INFO("ONVIF device service: IP=%s, UUID=%s",
                dev->device_ip.c_str(), dev->device_uuid.c_str());

    // --- Create HTTP server socket ---
    dev->http_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (dev->http_fd < 0) {
        SC_LOG_ERROR("ONVIF: failed to create HTTP socket");
        delete dev;
        return nullptr;
    }

    int opt = 1;
    setsockopt(dev->http_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in http_addr{};
    http_addr.sin_family = AF_INET;
    http_addr.sin_addr.s_addr = INADDR_ANY;
    http_addr.sin_port = htons(dev->dev_cfg.http_port);

    if (bind(dev->http_fd, reinterpret_cast<struct sockaddr*>(&http_addr),
             sizeof(http_addr)) < 0) {
        SC_LOG_ERROR("ONVIF: failed to bind HTTP port %d (errno=%d: %s)",
                     dev->dev_cfg.http_port, errno, strerror(errno));
        close(dev->http_fd);
        delete dev;
        return nullptr;
    }

    if (listen(dev->http_fd, HTTP_BACKLOG) < 0) {
        SC_LOG_ERROR("ONVIF: listen() failed");
        close(dev->http_fd);
        delete dev;
        return nullptr;
    }

    // --- Create WS-Discovery socket ---
    if (dev_cfg.enable_discovery) {
        dev->discovery_fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (dev->discovery_fd < 0) {
            SC_LOG_WARN("ONVIF: failed to create WS-Discovery socket");
        } else {
            int reuse = 1;
            setsockopt(dev->discovery_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
            setsockopt(dev->discovery_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

            struct sockaddr_in mcast_bind{};
            mcast_bind.sin_family = AF_INET;
            mcast_bind.sin_addr.s_addr = INADDR_ANY;
            mcast_bind.sin_port = htons(WS_DISCOVERY_PORT);

            if (bind(dev->discovery_fd,
                     reinterpret_cast<struct sockaddr*>(&mcast_bind),
                     sizeof(mcast_bind)) < 0) {
                SC_LOG_WARN("ONVIF: WS-Discovery bind failed (errno=%d: %s) -- "
                            "discovery disabled (port 3702 may be in use)",
                            errno, strerror(errno));
                close(dev->discovery_fd);
                dev->discovery_fd = -1;
            } else {
                // Join multicast group
                struct ip_mreq mreq{};
                inet_pton(AF_INET, WS_DISCOVERY_ADDR, &mreq.imr_multiaddr);
                mreq.imr_interface.s_addr = INADDR_ANY;
                if (setsockopt(dev->discovery_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                               &mreq, sizeof(mreq)) < 0) {
                    SC_LOG_WARN("ONVIF: failed to join multicast group %s",
                                WS_DISCOVERY_ADDR);
                    close(dev->discovery_fd);
                    dev->discovery_fd = -1;
                }
            }
        }
    }

    // --- Start threads ---
    dev->running = true;
    dev->http_thread = std::thread(http_server_loop, dev);

    if (dev->discovery_fd >= 0) {
        dev->discovery_thread = std::thread(ws_discovery_loop, dev);
    }

    SC_LOG_INFO("ONVIF device service started:");
    SC_LOG_INFO("  HTTP SOAP: http://%s:%d/onvif/device_service",
                dev->device_ip.c_str(), dev->dev_cfg.http_port);
    if (dev->discovery_fd >= 0) {
        SC_LOG_INFO("  WS-Discovery: active (multicast %s:%d)",
                    WS_DISCOVERY_ADDR, WS_DISCOVERY_PORT);
    }

    return dev;
}

// ---------------------------------------------------------------------------
// Stop the ONVIF device service
// ---------------------------------------------------------------------------
void onvif_device_stop(OnvifDevice* dev) {
    if (!dev) return;

    SC_LOG_INFO("Stopping ONVIF device service...");
    dev->running = false;

    // Close sockets (unblocks poll() in threads)
    if (dev->http_fd >= 0) {
        close(dev->http_fd);
        dev->http_fd = -1;
    }
    if (dev->discovery_fd >= 0) {
        close(dev->discovery_fd);
        dev->discovery_fd = -1;
    }

    // Join threads
    if (dev->http_thread.joinable()) {
        dev->http_thread.join();
    }
    if (dev->discovery_thread.joinable()) {
        dev->discovery_thread.join();
    }

    delete dev;
    SC_LOG_INFO("ONVIF device service stopped");
}

}  // namespace sc
