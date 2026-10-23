// PORT-ONLY (not in the original; checkpoint 132ln): the platform UDP socket behind LanSocket. Winsock2 on
// Windows, POSIX sockets elsewhere. Stands in for the original's socket layer (Socket_Construct 0x2F6CC8,
// RT UDP socket init 0x0030af68, Socket_WriteAddressed 0x2F5F20); see lan_socket.hpp and
// research/LAN_PLAY.md §5.2.
#include "lan_socket.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET  // mstcpip.h guards it by the target Windows version
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#ifndef __vita__
#include <ifaddrs.h>
#include <net/if.h>
#endif
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace omega::lan {

LanClock lan_steady_clock() {
    return [] {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    };
}

std::string lan_address_text(const LanAddress& address) {
    char text[32];
    std::snprintf(text, sizeof text, "%u.%u.%u.%u:%u", (address.ip >> 24) & 0xFFu, (address.ip >> 16) & 0xFFu,
                  (address.ip >> 8) & 0xFFu, address.ip & 0xFFu, static_cast<unsigned>(address.port));
    return text;
}

std::optional<std::uint32_t> lan_parse_ipv4(const std::string& text) {
    std::uint32_t ip = 0;
    std::size_t at = 0;
    for (int part = 0; part < 4; ++part) {
        if (part > 0) {
            if (at >= text.size() || text[at] != '.') return std::nullopt;
            ++at;
        }
        std::uint32_t value = 0;
        std::size_t digits = 0;
        while (at < text.size() && text[at] >= '0' && text[at] <= '9' && digits < 3) {
            value = value * 10 + static_cast<std::uint32_t>(text[at] - '0');
            ++at;
            ++digits;
        }
        if (digits == 0 || value > 255) return std::nullopt;
        ip = (ip << 8) | value;
    }
    if (at != text.size()) return std::nullopt;
    return ip;
}

namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;

// WSAStartup once per process; the matching WSACleanup is left to process exit.
bool winsock_ready() {
    static std::once_flag once;
    static bool ready = false;
    std::call_once(once, [] {
        WSADATA data{};
        ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return ready;
}

void close_native(NativeSocket socket) { closesocket(socket); }
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
bool winsock_ready() { return true; }
void close_native(NativeSocket socket) { ::close(socket); }
#endif

sockaddr_in to_sockaddr(const LanAddress& address) {
    sockaddr_in out{};
    out.sin_family = AF_INET;
    out.sin_port = htons(address.port);
    out.sin_addr.s_addr = htonl(address.ip);
    return out;
}

class UdpSocket final : public LanSocket {
public:
    explicit UdpSocket(std::uint32_t bind_ip) : bind_ip_(bind_ip) {}
    ~UdpSocket() override { close(); }

    bool open(std::uint16_t port, const LanSocketOptions& options) override {
        close();
        if (!winsock_ready()) return false;
        socket_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket_ == kInvalidSocket) return false;
        int on = 1;
        if (options.reuse_address)
            setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
        if (options.broadcast)
            setsockopt(socket_, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&on), sizeof on);
#ifdef _WIN32
        // A UDP send that draws an ICMP port-unreachable would otherwise fail the next recvfrom with
        // WSAECONNRESET; the LAN transport treats that peer as silent instead (its 5 s timeout).
        BOOL report = FALSE;
        DWORD returned = 0;
        WSAIoctl(socket_, SIO_UDP_CONNRESET, &report, sizeof report, nullptr, 0, &returned, nullptr, nullptr);
        u_long nonblocking = 1;
        if (ioctlsocket(socket_, FIONBIO, &nonblocking) != 0) {
            close();
            return false;
        }
#else
        const int flags = fcntl(socket_, F_GETFL, 0);
        if (flags < 0 || fcntl(socket_, F_SETFL, flags | O_NONBLOCK) != 0) {
            close();
            return false;
        }
#endif
        sockaddr_in local = to_sockaddr(LanAddress{bind_ip_, port});  // INADDR_ANY unless a bind address is given
        if (::bind(socket_, reinterpret_cast<const sockaddr*>(&local), sizeof local) != 0) {
            close();
            return false;
        }
        sockaddr_in bound{};
#ifdef _WIN32
        int length = sizeof bound;
#else
        socklen_t length = sizeof bound;
#endif
        if (getsockname(socket_, reinterpret_cast<sockaddr*>(&bound), &length) == 0) port_ = ntohs(bound.sin_port);
        return true;
    }

    void close() override {
        if (socket_ != kInvalidSocket) close_native(socket_);
        socket_ = kInvalidSocket;
        port_ = 0;
    }

    bool is_open() const override { return socket_ != kInvalidSocket; }
    std::uint16_t local_port() const override { return port_; }

    bool send_to(const LanAddress& to, std::span<const std::uint8_t> bytes) override {
        if (!is_open()) return false;
        const sockaddr_in address = to_sockaddr(to);
#ifdef _WIN32
        const int sent = ::sendto(socket_, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()),
                                  0, reinterpret_cast<const sockaddr*>(&address), sizeof address);
#else
        const ssize_t sent = ::sendto(socket_, bytes.data(), bytes.size(), 0,
                                      reinterpret_cast<const sockaddr*>(&address), sizeof address);
#endif
        return sent >= 0 && static_cast<std::size_t>(sent) == bytes.size();
    }

    std::optional<std::size_t> receive_from(LanAddress& from, std::span<std::uint8_t> buffer) override {
        if (!is_open()) return std::nullopt;
        for (int attempt = 0; attempt < 64; ++attempt) {
            sockaddr_in address{};
#ifdef _WIN32
            int length = sizeof address;
            const int received = ::recvfrom(socket_, reinterpret_cast<char*>(buffer.data()),
                                            static_cast<int>(buffer.size()), 0,
                                            reinterpret_cast<sockaddr*>(&address), &length);
            if (received < 0) {
                const int error = WSAGetLastError();
                // A truncated datagram (WSAEMSGSIZE) still filled the buffer; report it at the buffer size.
                if (error == WSAEMSGSIZE) {
                    from = LanAddress{ntohl(address.sin_addr.s_addr), ntohs(address.sin_port)};
                    return buffer.size();
                }
                if (error == WSAECONNRESET) continue;
                return std::nullopt;
            }
#else
            socklen_t length = sizeof address;
            const ssize_t received = ::recvfrom(socket_, buffer.data(), buffer.size(), 0,
                                                reinterpret_cast<sockaddr*>(&address), &length);
            if (received < 0) {
                // EAGAIN / EWOULDBLOCK: nothing pending. ECONNREFUSED: an earlier send drew an ICMP
                // port-unreachable; skip it as Windows does with SIO_UDP_CONNRESET off.
                if (errno == EINTR || errno == ECONNREFUSED) continue;
                return std::nullopt;
            }
#endif
            from = LanAddress{ntohl(address.sin_addr.s_addr), ntohs(address.sin_port)};
            return static_cast<std::size_t>(received);
        }
        return std::nullopt;
    }

    std::vector<LanAddress> broadcast_targets(std::uint16_t port) const override {
        // A socket bound to one address (127.0.0.1) reaches no broadcast domain.
        if (bind_ip_ != 0) return {};
        std::vector<LanAddress> targets{LanAddress{kLanBroadcastIp, port}};
        auto add = [&](std::uint32_t ip) {
            if (ip == 0 || ip == kLanBroadcastIp) return;
            for (const LanAddress& existing : targets)
                if (existing.ip == ip) return;
            targets.push_back(LanAddress{ip, port});
        };
#ifdef _WIN32
        if (!is_open()) return targets;
        INTERFACE_INFO interfaces[32]{};
        DWORD returned = 0;
        if (WSAIoctl(socket_, SIO_GET_INTERFACE_LIST, nullptr, 0, interfaces, sizeof interfaces, &returned, nullptr,
                     nullptr) == 0) {
            const std::size_t count = returned / sizeof(INTERFACE_INFO);
            for (std::size_t i = 0; i < count; ++i) {
                const INTERFACE_INFO& info = interfaces[i];
                if (!(info.iiFlags & IFF_UP) || (info.iiFlags & IFF_LOOPBACK) || !(info.iiFlags & IFF_BROADCAST))
                    continue;
                const std::uint32_t ip = ntohl(info.iiAddress.AddressIn.sin_addr.s_addr);
                const std::uint32_t mask = ntohl(info.iiNetmask.AddressIn.sin_addr.s_addr);
                add(ip | ~mask);
            }
        }
#elif defined(__vita__)
        // Port-only: the Vita's newlib has no getifaddrs; the limited broadcast above reaches the Wi-Fi subnet.
        (void)add;
#else
        ifaddrs* list = nullptr;
        if (getifaddrs(&list) == 0) {
            for (const ifaddrs* entry = list; entry != nullptr; entry = entry->ifa_next) {
                if (entry->ifa_addr == nullptr || entry->ifa_addr->sa_family != AF_INET) continue;
                if (!(entry->ifa_flags & IFF_UP) || (entry->ifa_flags & IFF_LOOPBACK) ||
                    !(entry->ifa_flags & IFF_BROADCAST) || entry->ifa_broadaddr == nullptr)
                    continue;
                sockaddr_in broadcast{};
                std::memcpy(&broadcast, entry->ifa_broadaddr, sizeof broadcast);
                add(ntohl(broadcast.sin_addr.s_addr));
            }
            freeifaddrs(list);
        }
#endif
        return targets;
    }

private:
    std::uint32_t bind_ip_ = 0;
    NativeSocket socket_ = kInvalidSocket;
    std::uint16_t port_ = 0;
};

}  // namespace

std::unique_ptr<LanSocket> make_udp_socket(std::uint32_t bind_ip) { return std::make_unique<UdpSocket>(bind_ip); }

LanSocketFactory lan_udp_socket_factory(std::uint32_t bind_ip) {
    return [bind_ip] { return make_udp_socket(bind_ip); };
}

}  // namespace omega::lan
