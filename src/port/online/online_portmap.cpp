// PORT-ONLY (checkpoint 132on2f): see online_portmap.hpp.
#include "online_portmap.hpp"

#include "online_tcp.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <random>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <arpa/inet.h>
#include <fstream>
#include <sstream>
#endif

namespace omega::online {
namespace {

std::string ip_text(std::uint32_t ip) {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 0xff) + "." + std::to_string((ip >> 8) & 0xff) + "." +
           std::to_string(ip & 0xff);
}

void put16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v >> 8));
    b.push_back(static_cast<std::uint8_t>(v));
}
void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<std::uint8_t>(v >> s));
}
// An IPv4 address as an IPv4-mapped IPv6 address (::ffff:a.b.c.d), RFC 6887 §5.
void put_mapped(std::vector<std::uint8_t>& b, std::uint32_t ip) {
    b.insert(b.end(), 10, 0);
    b.push_back(0xff);
    b.push_back(0xff);
    put32(b, ip);
}
std::uint16_t get16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint16_t>((b[at] << 8) | b[at + 1]);
}
std::uint32_t get32(std::span<const std::uint8_t> b, std::size_t at) {
    return (std::uint32_t(b[at]) << 24) | (std::uint32_t(b[at + 1]) << 16) | (std::uint32_t(b[at + 2]) << 8) | b[at + 3];
}
// The IPv4 address of an IPv4-mapped IPv6 address at `at` (0 when it is not one).
std::uint32_t get_mapped(std::span<const std::uint8_t> b, std::size_t at) {
    for (std::size_t i = 0; i < 10; ++i)
        if (b[at + i] != 0) return 0;
    if (b[at + 10] != 0xff || b[at + 11] != 0xff) return 0;
    return get32(b, at + 12);
}

PcpNonce random_nonce() {
    std::random_device rd;
    PcpNonce n{};
    for (auto& x : n) x = static_cast<std::uint8_t>(rd());
    return n;
}

// Sends `request` to the gateway and waits for a datagram from it that `accept` takes: RFC 6886 §3.1's schedule,
// first_wait_ms then doubling, `tries` sends.
std::optional<std::vector<std::uint8_t>> udp_exchange(std::uint32_t ip, std::uint16_t port,
                                                      std::span<const std::uint8_t> request, int tries, int first_wait_ms,
                                                      const std::function<bool(std::span<const std::uint8_t>)>& accept,
                                                      std::string* error) {
    UdpSocket s;
    if (!s.open(0, 0, error)) return std::nullopt;
    int wait = std::max(1, first_wait_ms);
    for (int t = 0; t < std::max(1, tries); ++t, wait *= 2) {
        if (!s.send_to(ip, port, request)) {
            if (error) *error = "cannot send to " + ip_text(ip) + ":" + std::to_string(port);
            return std::nullopt;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait);
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto d = s.receive()) {
                if (d->ip == ip && d->port == port && accept(d->bytes)) return std::move(d->bytes);
                continue;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    if (error) *error = "no answer from " + ip_text(ip) + ":" + std::to_string(port) + " (" + std::to_string(tries) + " tries)";
    return std::nullopt;
}

std::optional<std::uint32_t> resolve_gateway(const PortMapOptions& o) {
    if (o.gateway_ip) return o.gateway_ip;
    return default_gateway_ipv4();
}

// One PCP MAP exchange. `nonce_seen_natpmp`: set when the gateway answered with NAT-PMP's version-0 reply.
std::optional<pcp::Response> pcp_exchange(std::uint32_t gw, const PortMapOptions& o, std::uint32_t client_ip,
                                          const PcpNonce& nonce, std::uint16_t internal_port, std::uint16_t external_port,
                                          std::uint32_t lifetime, std::string* error) {
    const auto req = pcp::map_request(client_ip, nonce, internal_port, external_port, 0, lifetime);
    const auto bytes = udp_exchange(gw, o.gateway_port, req, o.tries, o.first_wait_ms,
                                    [&](std::span<const std::uint8_t> b) {
                                        const auto r = pcp::parse(b);
                                        return r && (r->version == 0 || (r->opcode == 0x81 && r->nonce == nonce));
                                    },
                                    error);
    if (!bytes) return std::nullopt;
    return pcp::parse(*bytes);
}

std::optional<natpmp::Response> natpmp_map_exchange(std::uint32_t gw, const PortMapOptions& o, std::uint16_t internal_port,
                                                    std::uint16_t external_port, std::uint32_t lifetime, std::string* error) {
    const auto req = natpmp::map_request(internal_port, external_port, lifetime);
    const auto bytes = udp_exchange(gw, o.gateway_port, req, o.tries, o.first_wait_ms,
                                    [&](std::span<const std::uint8_t> b) {
                                        const auto r = natpmp::parse(b);
                                        return r && r->opcode == 129 && (r->result != 0 || r->internal_port == internal_port);
                                    },
                                    error);
    if (!bytes) return std::nullopt;
    return natpmp::parse(*bytes);
}

bool try_pcp(PortMapping& m, std::uint32_t gw, const PortMapOptions& o, std::string& why,
             const std::function<void(const std::string&)>& log) {
    const auto client = local_ip_toward(gw);
    if (!client) {
        why = "PCP: no local address toward " + ip_text(gw);
        return false;
    }
    const PcpNonce nonce = o.nonce ? o.nonce() : random_nonce();
    if (log) log("PCP: asking the gateway " + ip_text(gw) + ":" + std::to_string(o.gateway_port) + " to map UDP " +
                 std::to_string(m.internal_port));
    std::string error;
    const auto r = pcp_exchange(gw, o, *client, nonce, m.internal_port, m.internal_port, o.lifetime, &error);
    if (!r) {
        why = "PCP: " + error;
        return false;
    }
    if (r->version == 0) {
        why = "PCP: the gateway speaks only NAT-PMP (a version-0 " + natpmp::result_text(r->result) + " reply)";
        return false;
    }
    if (r->result != 0) {
        why = "PCP: refused (" + pcp::result_text(r->result) + ")";
        return false;
    }
    m.mapped = true;
    m.method = PortMapMethod::Pcp;
    m.internal_ip = *client;
    m.external_port = r->external_port;
    m.external_ip = r->external_ip ? ip_text(r->external_ip) : std::string();
    m.lifetime = r->lifetime;
    m.nonce = nonce;
    return true;
}

bool try_natpmp(PortMapping& m, std::uint32_t gw, const PortMapOptions& o, std::string& why,
                const std::function<void(const std::string&)>& log) {
    if (log) log("NAT-PMP: asking the gateway " + ip_text(gw) + ":" + std::to_string(o.gateway_port) + " to map UDP " +
                 std::to_string(m.internal_port));
    std::string error;
    const auto r = natpmp_map_exchange(gw, o, m.internal_port, m.internal_port, o.lifetime, &error);
    if (!r) {
        why = "NAT-PMP: " + error;
        return false;
    }
    if (r->result != 0) {
        why = "NAT-PMP: refused (" + natpmp::result_text(r->result) + ")";
        return false;
    }
    m.mapped = true;
    m.method = PortMapMethod::NatPmp;
    m.internal_ip = local_ip_toward(gw).value_or(0);
    m.external_port = r->external_port;
    m.lifetime = r->lifetime;
    // The external address (opcode 0); the mapping stands without it.
    const auto req = natpmp::external_address_request();
    const auto a = udp_exchange(gw, o.gateway_port, req, o.tries, o.first_wait_ms,
                                [](std::span<const std::uint8_t> b) {
                                    const auto x = natpmp::parse(b);
                                    return x && x->opcode == 128;
                                },
                                &error);
    if (a)
        if (const auto x = natpmp::parse(*a); x && x->result == 0 && x->external_ip) m.external_ip = ip_text(x->external_ip);
    return true;
}

}  // namespace

// ---- NAT-PMP ------------------------------------------------------------------------------------------------------

std::vector<std::uint8_t> natpmp::external_address_request() { return {0, 0}; }

std::vector<std::uint8_t> natpmp::map_request(std::uint16_t internal_port, std::uint16_t external_port,
                                              std::uint32_t lifetime) {
    std::vector<std::uint8_t> b{0, 1, 0, 0};
    put16(b, internal_port);
    put16(b, external_port);
    put32(b, lifetime);
    return b;
}

std::optional<natpmp::Response> natpmp::parse(std::span<const std::uint8_t> b) {
    if (b.size() < 8 || b[0] != 0 || b[1] < 128) return std::nullopt;
    Response r;
    r.version = b[0];
    r.opcode = b[1];
    r.result = get16(b, 2);
    r.epoch = get32(b, 4);
    if (r.opcode == 128 && b.size() >= 12) r.external_ip = get32(b, 8);
    if ((r.opcode == 129 || r.opcode == 130) && b.size() >= 16) {
        r.internal_port = get16(b, 8);
        r.external_port = get16(b, 10);
        r.lifetime = get32(b, 12);
    }
    return r;
}

std::string natpmp::result_text(std::uint16_t result) {
    switch (result) {
    case 0: return "0 success";
    case 1: return "1 unsupported version";
    case 2: return "2 not authorized / refused";
    case 3: return "3 network failure";
    case 4: return "4 out of resources";
    case 5: return "5 unsupported opcode";
    default: return std::to_string(result) + " unknown";
    }
}

// ---- PCP ----------------------------------------------------------------------------------------------------------

std::vector<std::uint8_t> pcp::map_request(std::uint32_t client_ip, const PcpNonce& nonce, std::uint16_t internal_port,
                                           std::uint16_t external_port, std::uint32_t external_ip, std::uint32_t lifetime) {
    std::vector<std::uint8_t> b{2, 1, 0, 0};
    put32(b, lifetime);
    put_mapped(b, client_ip);
    b.insert(b.end(), nonce.begin(), nonce.end());
    b.push_back(17);  // UDP
    b.insert(b.end(), 3, 0);
    put16(b, internal_port);
    put16(b, external_port);
    put_mapped(b, external_ip);
    return b;
}

std::optional<pcp::Response> pcp::parse(std::span<const std::uint8_t> b) {
    if (b.size() >= 4 && b[0] == 0) {  // a NAT-PMP gateway's reply
        Response r;
        r.version = 0;
        r.opcode = b[1];
        r.result = static_cast<std::uint8_t>(std::min<std::uint16_t>(get16(b, 2), 255));
        return r;
    }
    if (b.size() < 24 || b[0] != 2 || !(b[1] & 0x80)) return std::nullopt;
    Response r;
    r.version = b[0];
    r.opcode = b[1];
    r.result = b[3];
    r.lifetime = get32(b, 4);
    r.epoch = get32(b, 8);
    if (r.opcode == 0x81 && b.size() >= 60) {
        std::copy(b.begin() + 24, b.begin() + 36, r.nonce.begin());
        r.protocol = b[36];
        r.internal_port = get16(b, 40);
        r.external_port = get16(b, 42);
        r.external_ip = get_mapped(b, 44);
    }
    return r;
}

std::string pcp::result_text(std::uint8_t result) {
    static const char* const names[] = {"SUCCESS",          "UNSUPP_VERSION",  "NOT_AUTHORIZED",          "MALFORMED_REQUEST",
                                        "UNSUPP_OPCODE",    "UNSUPP_OPTION",   "MALFORMED_OPTION",        "NETWORK_FAILURE",
                                        "NO_RESOURCES",     "UNSUPP_PROTOCOL", "USER_EX_QUOTA",           "CANNOT_PROVIDE_EXTERNAL",
                                        "ADDRESS_MISMATCH", "EXCESSIVE_REMOTE_PEERS"};
    return std::to_string(result) + " " + (result < std::size(names) ? names[result] : "unknown");
}

// ---- The chain ----------------------------------------------------------------------------------------------------

const char* portmap_method_name(PortMapMethod m) {
    switch (m) {
    case PortMapMethod::Upnp: return "UPnP";
    case PortMapMethod::Pcp: return "PCP";
    case PortMapMethod::NatPmp: return "NAT-PMP";
    default: return "none";
    }
}

PortMapping portmap_udp(std::uint16_t internal_port, const PortMapOptions& o,
                        const std::function<void(const std::string&)>& log) {
    PortMapping m;
    m.internal_port = internal_port;
    std::vector<std::string> failures;
    if (o.upnp) {
        if (log) log("UPnP IGD: searching for the router");
        UpnpMapping u = upnp_map_udp(internal_port, o.upnp_options, log);
        if (u.mapped) {
            m.mapped = true;
            m.method = PortMapMethod::Upnp;
            m.gateway = u.gateway;
            m.internal_ip = u.internal_ip;
            m.external_port = u.external_port;
            m.external_ip = u.external_ip;
            m.upnp = std::move(u);
            return m;
        }
        failures.push_back("UPnP: " + u.error);
        if (log) log("UPnP: " + u.error);
    }
    if (o.pcp || o.natpmp) {
        const auto gw = resolve_gateway(o);
        if (!gw) {
            failures.push_back("PCP / NAT-PMP: no default gateway");
            if (log) log("PCP / NAT-PMP: no IPv4 default gateway");
        } else {
            m.gateway_ip = *gw;
            m.gateway_port = o.gateway_port;
            m.gateway = ip_text(*gw) + ":" + std::to_string(o.gateway_port);
            std::string why;
            if (o.pcp) {
                if (try_pcp(m, *gw, o, why, log)) return m;
                failures.push_back(why);
                if (log) log(why);
            }
            if (o.natpmp) {
                if (try_natpmp(m, *gw, o, why, log)) return m;
                failures.push_back(why);
                if (log) log(why);
            }
        }
    }
    if (failures.empty()) failures.push_back("every method is off");
    for (std::size_t i = 0; i < failures.size(); ++i) m.error += (i ? "; " : "") + failures[i];
    return m;
}

PortMapping portmap_renew(const PortMapping& mapping, const PortMapOptions& o) {
    PortMapping m = mapping;
    if (!mapping.mapped || mapping.method == PortMapMethod::Upnp) return m;
    std::string error;
    if (mapping.method == PortMapMethod::Pcp) {
        const auto r = pcp_exchange(mapping.gateway_ip, o, mapping.internal_ip, mapping.nonce, mapping.internal_port,
                                    mapping.external_port, o.lifetime, &error);
        if (r && r->version == 2 && r->result == 0) {
            m.external_port = r->external_port;
            if (r->external_ip) m.external_ip = ip_text(r->external_ip);
            m.lifetime = r->lifetime;
            m.error.clear();
            return m;
        }
        m.error = r ? "PCP renewal refused (" + (r->version == 0 ? natpmp::result_text(r->result) : pcp::result_text(r->result)) + ")"
                    : "PCP renewal: " + error;
    } else {
        const auto r = natpmp_map_exchange(mapping.gateway_ip, o, mapping.internal_port, mapping.external_port, o.lifetime, &error);
        if (r && r->result == 0) {
            m.external_port = r->external_port;
            m.lifetime = r->lifetime;
            m.error.clear();
            return m;
        }
        m.error = r ? "NAT-PMP renewal refused (" + natpmp::result_text(r->result) + ")" : "NAT-PMP renewal: " + error;
    }
    m.mapped = false;
    return m;
}

bool portmap_unmap(const PortMapping& m, const PortMapOptions& o, std::string* error) {
    if (!m.mapped) return true;
    std::string e;
    bool ok = false;
    switch (m.method) {
    case PortMapMethod::Upnp: ok = upnp_unmap(m.upnp, o.upnp_options, &e); break;
    case PortMapMethod::Pcp: {
        const auto r = pcp_exchange(m.gateway_ip, o, m.internal_ip, m.nonce, m.internal_port, m.external_port, 0, &e);
        ok = r && r->version == 2 && r->result == 0;
        if (r && !ok) e = "refused (" + pcp::result_text(r->result) + ")";
        break;
    }
    case PortMapMethod::NatPmp: {
        const auto r = natpmp_map_exchange(m.gateway_ip, o, m.internal_port, 0, 0, &e);
        ok = r && r->result == 0;
        if (r && !ok) e = "refused (" + natpmp::result_text(r->result) + ")";
        break;
    }
    default: return true;
    }
    if (!ok && error) *error = e;
    return ok;
}

std::optional<std::uint32_t> default_gateway_ipv4() {
#ifdef _WIN32
    MIB_IPFORWARDROW row{};
    if (GetBestRoute(htonl(0x08080808u), 0, &row) != NO_ERROR) return std::nullopt;
    const std::uint32_t hop = ntohl(row.dwForwardNextHop);
    if (hop == 0 || row.dwForwardType == MIB_IPROUTE_TYPE_DIRECT) return std::nullopt;
    return hop;
#else
    std::ifstream in("/proc/net/route");
    std::string line;
    std::getline(in, line);  // the header
    while (std::getline(in, line)) {
        std::istringstream s(line);
        std::string iface, dest, gw;
        unsigned flags = 0;
        if (!(s >> iface >> dest >> gw >> std::hex >> flags)) continue;
        if (dest != "00000000" || !(flags & 0x2)) continue;  // the default route, RTF_GATEWAY
        const auto raw = static_cast<std::uint32_t>(std::strtoul(gw.c_str(), nullptr, 16));
        const std::uint32_t hop = ntohl(raw);  // printed in the kernel's (network-order) memory layout
        if (hop) return hop;
    }
    return std::nullopt;
#endif
}

// ---- The fake gateway ---------------------------------------------------------------------------------------------

struct FakePortMapGateway::State {
    Mode mode = Mode::Pcp;
    std::uint32_t external_ip = 0;
    std::atomic<bool> stop{false};
    UdpSocket socket;
    mutable std::mutex mutex;
    std::vector<std::string> requests;
    std::vector<std::uint16_t> taken;
    std::map<std::uint16_t, std::uint16_t> mappings;  // internal -> external
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    std::uint32_t epoch() const {
        return static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count());
    }
    // The external port for `internal`, suggesting `suggested` (lifetime 0: the mapping removed, 0 answered).
    std::uint16_t assign(std::uint16_t internal, std::uint16_t suggested, std::uint32_t lifetime) {
        if (lifetime == 0) {
            mappings.erase(internal);
            return 0;
        }
        if (auto it = mappings.find(internal); it != mappings.end()) return it->second;
        std::uint16_t p = suggested ? suggested : internal;
        auto busy = [&](std::uint16_t x) {
            if (std::find(taken.begin(), taken.end(), x) != taken.end()) return true;
            for (const auto& [i, e] : mappings)
                if (e == x) return true;
            return false;
        };
        while (busy(p)) ++p;
        mappings[internal] = p;
        return p;
    }
    void answer(const UdpSocket::Datagram& d) {
        const std::span<const std::uint8_t> b(d.bytes);
        std::vector<std::uint8_t> out;
        std::lock_guard lock(mutex);
        if (b.size() >= 2 && b[0] == 2) {
            const std::uint32_t lifetime = b.size() >= 8 ? get32(b, 4) : 0;
            const std::uint16_t internal = b.size() >= 60 ? get16(b, 40) : 0;
            requests.push_back("pcp map " + std::to_string(internal) + " lifetime " + std::to_string(lifetime));
            if (mode == Mode::Silent) return;
            if (mode == Mode::NatPmp) {  // RFC 6886 §3.5: version 0, the opcode + 128, result 1, the epoch
                out = {0, static_cast<std::uint8_t>((b[1] & 0x7f) + 128), 0, 1};
                put32(out, epoch());
            } else {
                if (b.size() < 60 || b[1] != 1) return;
                const std::uint8_t result = mode == Mode::Refuse ? 2 : 0;
                const std::uint16_t ext = result ? 0 : assign(internal, get16(b, 42), lifetime);
                out = {2, 0x81, 0, result};
                put32(out, result ? 0 : std::min<std::uint32_t>(lifetime, 7200));
                put32(out, epoch());
                out.insert(out.end(), 12, 0);
                out.insert(out.end(), b.begin() + 24, b.begin() + 36);  // the nonce
                out.push_back(17);
                out.insert(out.end(), 3, 0);
                put16(out, internal);
                put16(out, ext);
                put_mapped(out, result ? 0 : external_ip);
            }
        } else if (b.size() >= 2 && b[0] == 0) {
            if (b[1] == 0) {
                requests.push_back("natpmp external address");
                if (mode == Mode::Silent) return;
                out = {0, 128};
                put16(out, mode == Mode::Refuse ? 2 : 0);
                put32(out, epoch());
                put32(out, mode == Mode::Refuse ? 0 : external_ip);
            } else if (b[1] == 1 && b.size() >= 12) {
                const std::uint16_t internal = get16(b, 4);
                const std::uint32_t lifetime = get32(b, 8);
                requests.push_back("natpmp map " + std::to_string(internal) + " lifetime " + std::to_string(lifetime));
                if (mode == Mode::Silent) return;
                const std::uint16_t result = mode == Mode::Refuse ? 2 : 0;
                const std::uint16_t ext = result ? 0 : assign(internal, get16(b, 6), lifetime);
                out = {0, 129};
                put16(out, result);
                put32(out, epoch());
                put16(out, internal);
                put16(out, ext);
                put32(out, result ? 0 : std::min<std::uint32_t>(lifetime, 7200));
            } else {
                return;
            }
        } else {
            return;
        }
        socket.send_to(d.ip, d.port, out);
    }
};

FakePortMapGateway::FakePortMapGateway(Mode mode, std::uint32_t external_ip) : state_(std::make_shared<State>()) {
    state_->mode = mode;
    state_->external_ip = external_ip;
    if (!state_->socket.open(0x7f000001u, 0, &error_)) return;
    port_ = state_->socket.port();
    thread_ = std::thread([s = state_] {
        while (!s->stop) {
            if (auto d = s->socket.receive()) s->answer(*d);
            else std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
}

FakePortMapGateway::~FakePortMapGateway() {
    state_->stop = true;
    if (thread_.joinable()) thread_.join();
    state_->socket.close();
}

std::vector<std::string> FakePortMapGateway::requests() const {
    std::lock_guard lock(state_->mutex);
    return state_->requests;
}

void FakePortMapGateway::set_taken(std::vector<std::uint16_t> ports) {
    std::lock_guard lock(state_->mutex);
    state_->taken = std::move(ports);
}

std::optional<FakePortMapGateway::Mode> FakePortMapGateway::parse_mode(const std::string& text) {
    if (text == "pcp") return Mode::Pcp;
    if (text == "natpmp") return Mode::NatPmp;
    if (text == "refuse") return Mode::Refuse;
    if (text == "silent") return Mode::Silent;
    return std::nullopt;
}

}  // namespace omega::online
