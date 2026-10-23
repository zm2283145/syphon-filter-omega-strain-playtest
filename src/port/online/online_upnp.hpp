#pragma once
// PORT-ONLY (not in the original as such; checkpoint 132on2e, research/ONLINE.md §6.12, research/ONLINE_DEPLOY.md
// §2.4): a small UPnP Internet Gateway Device client that maps the online host's in-level UDP port on the player's
// home router, so joiners on the Internet can reach the host directly. The original links SCE-RT's own UPnP IGD
// client (rt_upnp 1.01.0007, string 0x004af8b0; AddPortMapping body 0x004b01b0 sent by 0x0030e7c8, pumped from the
// rt_udp update 0x003085d8 via 0x0030ef88); whether retail turns it on is not decoded. This is the port's own
// implementation of the same standard (UPnP Device Architecture 1.0 + WANIPConnection / WANPPPConnection), written
// for the port with no third-party code.
//
// The steps: SSDP M-SEARCH (UDP multicast 239.255.255.250:1900) for an InternetGatewayDevice; the device
// description (HTTP GET of the LOCATION) for the WANIPConnection / WANPPPConnection control URL; SOAP AddPortMapping
// (UDP, external port = the internal port, else the next few), GetExternalIPAddress; and DeletePortMapping at the
// leave. Blocking (a few seconds at most): the caller runs it off the game thread.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace omega::online {

struct UpnpOptions {
    std::uint32_t ssdp_ip = 0xEFFFFFFAu;  // 239.255.255.250 (tests: 127.0.0.1)
    std::uint16_t ssdp_port = 1900;
    int discover_ms = 2000;               // how long to wait for the router's SSDP answer
    int http_timeout_ms = 3000;
    int port_tries = 4;                   // external ports tried: the internal port, then +1 ..
    std::string description = "Omega Strain (PC port) in-level";
    // HTTP exchange (tests replace it): ip, port, request -> the whole response.
    std::function<std::optional<std::string>(std::uint32_t, std::uint16_t, const std::string&, int)> http;
};

struct UpnpMapping {
    bool mapped = false;
    std::string error;            // why not (no router answered, no WAN service, the router refused ...)
    std::string gateway;          // the device description's URL
    std::uint32_t control_ip = 0;  // the control URL's address / port / path and the service type
    std::uint16_t control_port = 0;
    std::string control_path;
    std::string service_type;
    std::uint32_t internal_ip = 0;
    std::uint16_t internal_port = 0;
    std::uint16_t external_port = 0;
    std::string external_ip;      // GetExternalIPAddress (empty: the router did not say)
};

// Maps UDP internal_port on the router. `log` gets one line per step.
UpnpMapping upnp_map_udp(std::uint16_t internal_port, const UpnpOptions& options,
                         const std::function<void(const std::string&)>& log = {});
// Removes a mapping upnp_map_udp made. False (with *error) when the router refused or did not answer.
bool upnp_unmap(const UpnpMapping& mapping, const UpnpOptions& options, std::string* error = nullptr);

// The parsing steps, exposed for the tests.
std::optional<std::string> upnp_header(const std::string& response, const std::string& name);  // case-insensitive
struct UpnpUrl {
    std::uint32_t ip = 0;
    std::uint16_t port = 80;
    std::string path = "/";
};
std::optional<UpnpUrl> upnp_parse_url(const std::string& url);  // http://a.b.c.d[:port][/path] (IPv4 only)
// The description's WAN connection service: {service type, control URL (as written)}.
std::optional<std::pair<std::string, std::string>> upnp_find_wan_service(const std::string& description_xml);
std::string upnp_http_body(const std::string& response);  // after the headers; chunked transfer decoded

}  // namespace omega::online
