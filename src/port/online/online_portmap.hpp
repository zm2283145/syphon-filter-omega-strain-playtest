#pragma once
// PORT-ONLY (not in the original; checkpoint 132on2f, research/ONLINE.md §6.13, research/ONLINE_DEPLOY.md §2.4): the
// host's port mapping chain. UPnP IGD (online_upnp.hpp, 132on2e) first, then the two fallbacks home routers speak when
// UPnP is off or absent: PCP (RFC 6887, the Port Control Protocol) and NAT-PMP (RFC 6886, its predecessor, Apple's
// AirPort and many open firmwares). The original has neither: SCUS_972.64 links only SCE-RT's rt_upnp (0x004af8b0)
// and holds no NAT-PMP / PCP string. Written for the port with no third-party code.
//
// Both speak UDP to the default gateway's port 5351. PCP first (a MAP request for UDP, a 12-byte nonce, the client's
// address as an IPv4-mapped IPv6 address); a gateway that speaks only NAT-PMP answers it with a version-0
// UNSUPP_VERSION reply, and the chain moves on to NAT-PMP (a UDP map request, then the external address request).
// Mappings carry a lifetime (7200 s asked); the caller renews at half of it (portmap_renew) and deletes them at the
// leave (lifetime 0). Retransmissions: RFC 6886's 250 ms doubling, `tries` sends. Blocking: the caller runs it off
// the game thread.

#include "online_upnp.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace omega::online {

constexpr std::uint16_t kPortMapGatewayPort = 5351;  // NAT-PMP's and PCP's server port

using PcpNonce = std::array<std::uint8_t, 12>;

// ---- The codecs (RFC 6886 §3.2 / §3.3, RFC 6887 §7 / §11), exposed for the tests and the fake gateway. ----------
namespace natpmp {
std::vector<std::uint8_t> external_address_request();  // [0][0]
// [0][1 UDP][u16 0][u16 internal][u16 suggested external][u32 lifetime]; lifetime 0 and external 0 delete.
std::vector<std::uint8_t> map_request(std::uint16_t internal_port, std::uint16_t external_port, std::uint32_t lifetime);
struct Response {
    std::uint8_t version = 0;
    std::uint8_t opcode = 0;   // 128 + the request's
    std::uint16_t result = 0;  // 0 success, 1 unsupported version, 2 refused, 3 network failure, 4 no resources, 5 opcode
    std::uint32_t epoch = 0;
    std::uint32_t external_ip = 0;  // opcode 128
    std::uint16_t internal_port = 0, external_port = 0;  // opcode 129 / 130
    std::uint32_t lifetime = 0;
};
std::optional<Response> parse(std::span<const std::uint8_t> bytes);  // nullopt: too short or not version 0
std::string result_text(std::uint16_t result);
}  // namespace natpmp

namespace pcp {
// A MAP request (60 bytes): version 2, opcode 1, lifetime, the client's IPv4 address (IPv4-mapped), the nonce,
// protocol 17 (UDP), the internal port, the suggested external port and address (0: any).
std::vector<std::uint8_t> map_request(std::uint32_t client_ip, const PcpNonce& nonce, std::uint16_t internal_port,
                                      std::uint16_t external_port, std::uint32_t external_ip, std::uint32_t lifetime);
struct Response {
    std::uint8_t version = 0;  // 2; 0 is a NAT-PMP-only gateway's answer (its result in `result`)
    std::uint8_t opcode = 0;   // 0x81 (MAP response)
    std::uint8_t result = 0;   // 0 success, 1 UNSUPP_VERSION, 2 NOT_AUTHORIZED, 8 NO_RESOURCES ...
    std::uint32_t lifetime = 0;
    std::uint32_t epoch = 0;
    PcpNonce nonce{};
    std::uint8_t protocol = 0;
    std::uint16_t internal_port = 0, external_port = 0;
    std::uint32_t external_ip = 0;  // when the assigned address is IPv4-mapped (else 0)
};
std::optional<Response> parse(std::span<const std::uint8_t> bytes);  // a MAP response, or a version-0 reply
std::string result_text(std::uint8_t result);
}  // namespace pcp

// ---- The chain ----------------------------------------------------------------------------------------------------
enum class PortMapMethod : std::uint8_t { None, Upnp, Pcp, NatPmp };
const char* portmap_method_name(PortMapMethod m);  // "UPnP", "PCP", "NAT-PMP", "none"

struct PortMapOptions {
    bool upnp = true, pcp = true, natpmp = true;  // the methods tried, in this order
    UpnpOptions upnp_options;
    std::uint32_t gateway_ip = 0;  // 0: the default route's gateway (default_gateway_ipv4)
    std::uint16_t gateway_port = kPortMapGatewayPort;
    int tries = 4;                 // sends per request (250, 500, 1000, 2000 ms waits)
    int first_wait_ms = 250;
    std::uint32_t lifetime = 7200;  // seconds asked of PCP / NAT-PMP
    std::function<PcpNonce()> nonce;  // tests; default: random
};

struct PortMapping {
    bool mapped = false;
    PortMapMethod method = PortMapMethod::None;
    std::string error;       // each method's failure, "; " between them
    std::string gateway;     // UPnP: the device description's URL; PCP / NAT-PMP: "a.b.c.d:5351"
    std::uint32_t gateway_ip = 0;
    std::uint16_t gateway_port = 0;
    std::uint32_t internal_ip = 0;
    std::uint16_t internal_port = 0;
    std::uint16_t external_port = 0;
    std::string external_ip;  // empty: the gateway did not say
    std::uint32_t lifetime = 0;  // PCP / NAT-PMP: the granted seconds (renew at half); UPnP: 0, kept until removed
    PcpNonce nonce{};            // PCP: the mapping's nonce (renewals and the delete reuse it)
    UpnpMapping upnp;            // UPnP: what upnp_unmap needs
};

// Maps UDP internal_port: UPnP, then PCP, then NAT-PMP (each when enabled). `log` gets one line per step.
PortMapping portmap_udp(std::uint16_t internal_port, const PortMapOptions& options,
                        const std::function<void(const std::string&)>& log = {});
// PCP / NAT-PMP: the same request again before the lifetime runs out; the result replaces the mapping (mapped false
// with the error when the gateway refused or did not answer). UPnP mappings are returned unchanged.
PortMapping portmap_renew(const PortMapping& mapping, const PortMapOptions& options);
// Removes a mapping (UPnP DeletePortMapping, PCP / NAT-PMP lifetime 0). False (with *error) when refused / no answer.
bool portmap_unmap(const PortMapping& mapping, const PortMapOptions& options, std::string* error = nullptr);
// The IPv4 default route's next hop (Windows GetBestRoute, Linux /proc/net/route); nullopt: none.
std::optional<std::uint32_t> default_gateway_ipv4();

// ---- A fake gateway (tests, and the play's --online-portmap-test): answers on 127.0.0.1 from a thread. ---------
class FakePortMapGateway {
public:
    enum class Mode {
        Pcp,      // a PCP server (NAT-PMP requests answered too)
        NatPmp,   // NAT-PMP only: PCP requests get the version-0 UNSUPP_VERSION reply
        Refuse,   // PCP NOT_AUTHORIZED, NAT-PMP "refused" (2)
        Silent,   // never answers
    };
    explicit FakePortMapGateway(Mode mode, std::uint32_t external_ip = 0xCB007107u /* 203.0.113.7 */);
    ~FakePortMapGateway();
    FakePortMapGateway(const FakePortMapGateway&) = delete;
    FakePortMapGateway& operator=(const FakePortMapGateway&) = delete;
    bool ok() const { return port_ != 0; }
    std::uint16_t port() const { return port_; }
    const std::string& error() const { return error_; }
    // What it saw: one line per request ("pcp map 3658 lifetime 7200", "natpmp map 3658 lifetime 0", ...).
    std::vector<std::string> requests() const;
    // External ports it treats as taken (the next free one is assigned).
    void set_taken(std::vector<std::uint16_t> ports);
    static std::optional<Mode> parse_mode(const std::string& text);  // pcp | natpmp | refuse | silent

private:
    struct State;
    std::shared_ptr<State> state_;
    std::thread thread_;
    std::uint16_t port_ = 0;
    std::string error_;
};

}  // namespace omega::online
