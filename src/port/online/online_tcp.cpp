// PORT-ONLY (checkpoint 132on1): see online_tcp.hpp.
#include "online_tcp.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_len = int;
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_len = socklen_t;
#endif

namespace omega::online {
namespace {

#ifdef _WIN32
using native_socket = SOCKET;
constexpr std::intptr_t kNone = -1;
native_socket as_native(std::intptr_t s) { return static_cast<native_socket>(s); }
bool would_block() {
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
}
void close_native(std::intptr_t s) { closesocket(as_native(s)); }
void set_nonblocking(std::intptr_t s) {
    u_long on = 1;
    ioctlsocket(as_native(s), FIONBIO, &on);
}
struct WinsockInit {
    WinsockInit() {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
};
void ensure_sockets() { static WinsockInit init; }
std::string socket_error_text() { return "socket error " + std::to_string(WSAGetLastError()); }
#else
using native_socket = int;
constexpr std::intptr_t kNone = -1;
native_socket as_native(std::intptr_t s) { return static_cast<native_socket>(s); }
bool would_block() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS; }
void close_native(std::intptr_t s) { ::close(as_native(s)); }
void set_nonblocking(std::intptr_t s) { fcntl(as_native(s), F_SETFL, fcntl(as_native(s), F_GETFL, 0) | O_NONBLOCK); }
void ensure_sockets() {}
std::string socket_error_text() { return std::string("socket error: ") + std::strerror(errno); }
#endif

void set_nodelay(std::intptr_t s) {
    int on = 1;
    setsockopt(as_native(s), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
}

sockaddr_in make_addr(std::uint32_t ip, std::uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(ip);
    a.sin_port = htons(port);
    return a;
}

// Writes as much of `out` as the socket takes. False when the connection failed.
bool flush(std::intptr_t s, std::vector<std::uint8_t>& out) {
    while (!out.empty()) {
        const int n = ::send(as_native(s), reinterpret_cast<const char*>(out.data()),
                             static_cast<int>(std::min<std::size_t>(out.size(), 1 << 20)), 0);
        if (n > 0) {
            out.erase(out.begin(), out.begin() + n);
            continue;
        }
        return n < 0 && would_block();
    }
    return true;
}

// Reads everything available into `reader`. False when the peer closed or the connection failed.
bool drain(std::intptr_t s, FrameReader& reader) {
    std::uint8_t buf[8192];
    for (;;) {
        const int n = ::recv(as_native(s), reinterpret_cast<char*>(buf), sizeof buf, 0);
        if (n > 0) {
            reader.feed(std::span<const std::uint8_t>(buf, static_cast<std::size_t>(n)));
            continue;
        }
        if (n == 0) return false;
        return would_block();
    }
}

}  // namespace

std::optional<std::uint32_t> resolve_ipv4(const std::string& host) {
    ensure_sockets();
    in_addr a{};
    if (inet_pton(AF_INET, host.c_str(), &a) == 1) return ntohl(a.s_addr);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) return std::nullopt;
    const auto ip = ntohl(reinterpret_cast<const sockaddr_in*>(result->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(result);
    return ip;
}

UdpSocket::UdpSocket() = default;
UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::open(std::uint32_t bind_ip, std::uint16_t port, std::string* error) {
    close();
    ensure_sockets();
    const auto s = static_cast<std::intptr_t>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
#ifdef _WIN32
    if (as_native(s) == INVALID_SOCKET) {
#else
    if (s < 0) {
#endif
        if (error) *error = socket_error_text();
        return false;
    }
#ifdef _WIN32
    // A datagram to a gone peer must not make the next recvfrom fail with WSAECONNRESET.
    BOOL off = FALSE;
    DWORD returned = 0;
    WSAIoctl(as_native(s), _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, nullptr, 0, &returned, nullptr, nullptr);
#endif
    const sockaddr_in a = make_addr(bind_ip, port);
    if (::bind(as_native(s), reinterpret_cast<const sockaddr*>(&a), sizeof a) != 0) {
        if (error) *error = "UDP bind to port " + std::to_string(port) + ": " + socket_error_text();
        close_native(s);
        return false;
    }
    sockaddr_in bound{};
    sock_len len = sizeof bound;
    getsockname(as_native(s), reinterpret_cast<sockaddr*>(&bound), &len);
    set_nonblocking(s);
    sock_ = s;
    port_ = ntohs(bound.sin_port);
    return true;
}

void UdpSocket::close() {
    if (sock_ != kNone) close_native(sock_);
    sock_ = kNone;
    port_ = 0;
}

bool UdpSocket::send_to(std::uint32_t ip, std::uint16_t port, std::span<const std::uint8_t> bytes) {
    if (sock_ == kNone) return false;
    const sockaddr_in a = make_addr(ip, port);
    const int n = ::sendto(as_native(sock_), reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0,
                           reinterpret_cast<const sockaddr*>(&a), sizeof a);
    return n == static_cast<int>(bytes.size());
}

std::optional<UdpSocket::Datagram> UdpSocket::receive() {
    if (sock_ == kNone) return std::nullopt;
    std::uint8_t buf[2048];
    for (;;) {
        sockaddr_in from{};
        sock_len len = sizeof from;
        const int n = ::recvfrom(as_native(sock_), reinterpret_cast<char*>(buf), sizeof buf, 0,
                                 reinterpret_cast<sockaddr*>(&from), &len);
        if (n >= 0) {
            Datagram d;
            d.ip = ntohl(from.sin_addr.s_addr);
            d.port = ntohs(from.sin_port);
            d.bytes.assign(buf, buf + n);
            return d;
        }
#ifdef _WIN32
        const int e = WSAGetLastError();
        if (e == WSAECONNRESET || e == WSAEMSGSIZE) continue;  // a gone peer's ICMP; an oversized datagram (dropped)
#endif
        return std::nullopt;
    }
}

std::optional<std::string> tcp_exchange(std::uint32_t ip, std::uint16_t port, const std::string& request, int timeout_ms) {
    ensure_sockets();
    const auto s = static_cast<std::intptr_t>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
#ifdef _WIN32
    if (as_native(s) == INVALID_SOCKET) return std::nullopt;
#else
    if (s < 0) return std::nullopt;
#endif
    set_nonblocking(s);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    auto wait = [&](bool write) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return false;
        fd_set set;
        fd_set failed;
        FD_ZERO(&set);
        FD_ZERO(&failed);
        FD_SET(as_native(s), &set);
        FD_SET(as_native(s), &failed);  // Winsock reports a refused connect here
        timeval tv{static_cast<long>(left / 1000), static_cast<long>((left % 1000) * 1000)};
        const int n = ::select(static_cast<int>(as_native(s) + 1), write ? nullptr : &set, write ? &set : nullptr, &failed, &tv);
        return n > 0 && !FD_ISSET(as_native(s), &failed);
    };
    // A complete response: the headers and Content-Length bytes (a router may keep the connection open).
    auto complete = [](const std::string& in) {
        const auto split = in.find("\r\n\r\n");
        if (split == std::string::npos) return false;
        std::string head = in.substr(0, split);
        for (char& c : head) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const auto cl = head.find("\ncontent-length:");
        return cl != std::string::npos && in.size() - split - 4 >= std::strtoul(head.c_str() + cl + 16, nullptr, 10);
    };
    const sockaddr_in a = make_addr(ip, port);
    std::optional<std::string> result;
    if (::connect(as_native(s), reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0 || would_block()) {
        bool ok = wait(true);
        int err = 0;
        sock_len len = sizeof err;
        if (ok) getsockopt(as_native(s), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
        std::vector<std::uint8_t> out(request.begin(), request.end());
        while (ok && err == 0 && !out.empty()) {
            if (!flush(s, out)) ok = false;
            else if (!out.empty()) ok = wait(true);
        }
        if (ok && err == 0) {
            std::string in;
            char buf[4096];
            while (wait(false)) {
                const int n = ::recv(as_native(s), buf, sizeof buf, 0);
                if (n > 0) {
                    in.append(buf, static_cast<std::size_t>(n));
                    if (complete(in)) break;
                } else if (n == 0 || !would_block()) {
                    break;
                }
            }
            if (!in.empty()) result = std::move(in);
        }
    }
    close_native(s);
    return result;
}

std::optional<std::uint32_t> local_ip_toward(std::uint32_t remote_ip) {
    ensure_sockets();
    const auto s = static_cast<std::intptr_t>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
#ifdef _WIN32
    if (as_native(s) == INVALID_SOCKET) return std::nullopt;
#else
    if (s < 0) return std::nullopt;
#endif
    std::optional<std::uint32_t> ip;
    const sockaddr_in a = make_addr(remote_ip, 1900);
    if (::connect(as_native(s), reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0) {
        sockaddr_in local{};
        sock_len len = sizeof local;
        if (getsockname(as_native(s), reinterpret_cast<sockaddr*>(&local), &len) == 0 && local.sin_addr.s_addr != 0)
            ip = ntohl(local.sin_addr.s_addr);
    }
    close_native(s);
    return ip;
}

std::optional<std::pair<std::string, std::uint16_t>> split_host_port(std::string_view text, std::uint16_t default_port) {
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos) {
        if (text.empty()) return std::nullopt;
        return std::make_pair(std::string(text), default_port);
    }
    const std::string_view host = text.substr(0, colon);
    const std::string_view port_text = text.substr(colon + 1);
    if (host.empty() || port_text.empty() || port_text.size() > 5) return std::nullopt;
    unsigned port = 0;
    for (char c : port_text) {
        if (c < '0' || c > '9') return std::nullopt;
        port = port * 10 + static_cast<unsigned>(c - '0');
    }
    if (port == 0 || port > 65535) return std::nullopt;
    return std::make_pair(std::string(host), static_cast<std::uint16_t>(port));
}

struct TcpServer::Conn {
    std::intptr_t sock = kNone;
    std::vector<std::uint8_t> out;
    FrameReader reader;
    bool closing = false;
    bool dead = false;
};

TcpServer::TcpServer() { ensure_sockets(); }

TcpServer::~TcpServer() {
    for (auto& [id, c] : conns_)
        if (c->sock != kNone) close_native(c->sock);
    if (listener_ != kNone) close_native(listener_);
}

bool TcpServer::listen(std::uint32_t bind_ip, std::uint16_t port, std::string* error) {
    const auto s = static_cast<std::intptr_t>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
#ifdef _WIN32
    if (as_native(s) == INVALID_SOCKET) {
#else
    if (s < 0) {
#endif
        if (error) *error = socket_error_text();
        return false;
    }
#ifndef _WIN32
    int reuse = 1;
    setsockopt(as_native(s), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
#endif
    const sockaddr_in addr = make_addr(bind_ip, port);
    if (::bind(as_native(s), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0 ||
        ::listen(as_native(s), SOMAXCONN) != 0) {  // 132on2h: was 16 (a burst of connects was refused)
        if (error) *error = socket_error_text();
        close_native(s);
        return false;
    }
    sockaddr_in bound{};
    sock_len len = sizeof bound;
    getsockname(as_native(s), reinterpret_cast<sockaddr*>(&bound), &len);
    port_ = ntohs(bound.sin_port);
    set_nonblocking(s);
    listener_ = s;
    return true;
}

void TcpServer::send(std::uint32_t conn, std::vector<std::uint8_t> bytes) {
    auto it = conns_.find(conn);
    if (it == conns_.end() || it->second->dead) return;
    auto& out = it->second->out;
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void TcpServer::close(std::uint32_t conn) {
    auto it = conns_.find(conn);
    if (it != conns_.end()) it->second->closing = true;
}

void TcpServer::poll(int timeout_ms, const Events& events, const UdpSocket* also_wait) {
    // 132on2h: poll() / WSAPoll() instead of select(): Winsock's fd_set holds at most FD_SETSIZE (64) sockets and
    // FD_SET silently skips the rest, so past ~62 clients some connections were never read (the load test's 128).
#ifdef _WIN32
    using pollfd_t = WSAPOLLFD;
#else
    using pollfd_t = pollfd;
#endif
    std::vector<pollfd_t> fds;
    std::vector<std::uint32_t> fd_ids;  // per fds entry: the connection (0: the listener / the relay socket)
    fds.reserve(conns_.size() + 2);
    fd_ids.reserve(conns_.size() + 2);
    auto add = [&](std::intptr_t s, short wanted, std::uint32_t id) {
        pollfd_t p{};
        p.fd = as_native(s);
        p.events = wanted;
        fds.push_back(p);
        fd_ids.push_back(id);
    };
    if (listener_ != kNone) add(listener_, POLLIN, 0);
    if (also_wait && also_wait->is_open()) add(also_wait->native(), POLLIN, 0);
    for (auto& [id, c] : conns_) add(c->sock, static_cast<short>(POLLIN | (c->out.empty() ? 0 : POLLOUT)), id);
    const auto wait_start = std::chrono::steady_clock::now();
#ifdef _WIN32
    WSAPoll(fds.data(), static_cast<ULONG>(fds.size()), timeout_ms);
#else
    ::poll(fds.data(), static_cast<nfds_t>(fds.size()), timeout_ms);
#endif
    last_wait_us_ = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - wait_start).count());
    const bool listener_ready = listener_ != kNone && (fds[0].revents & POLLIN);
    std::map<std::uint32_t, bool> readable;
    for (std::size_t i = 0; i < fds.size(); ++i)
        if (fd_ids[i] && (fds[i].revents & (POLLIN | POLLHUP | POLLERR))) readable[fd_ids[i]] = true;

    if (listener_ready) {
        for (;;) {
            sockaddr_in peer{};
            sock_len len = sizeof peer;
            const auto s = static_cast<std::intptr_t>(::accept(as_native(listener_), reinterpret_cast<sockaddr*>(&peer), &len));
#ifdef _WIN32
            if (as_native(s) == INVALID_SOCKET) break;
#else
            if (s < 0) break;
#endif
            set_nonblocking(s);
            set_nodelay(s);
            auto c = std::make_unique<Conn>();
            c->sock = s;
            const std::uint32_t id = next_id_++;
            conns_[id] = std::move(c);
            if (events.connected) events.connected(id, ntohl(peer.sin_addr.s_addr));
        }
    }

    std::vector<std::uint32_t> ids;
    for (auto& [id, c] : conns_) ids.push_back(id);
    for (std::uint32_t id : ids) {
        auto it = conns_.find(id);
        if (it == conns_.end()) continue;
        Conn& c = *it->second;
        if (!c.dead && readable.count(id)) {
            if (!drain(c.sock, c.reader)) c.dead = true;
            while (auto f = c.reader.next()) {
                if (events.frame) events.frame(id, *f);
                if (conns_.find(id) == conns_.end()) break;
            }
            if (c.reader.bad()) c.dead = true;
        }
    }
    for (std::uint32_t id : ids) {
        auto it = conns_.find(id);
        if (it == conns_.end()) continue;
        Conn& c = *it->second;
        if (!c.dead && !flush(c.sock, c.out)) c.dead = true;
        if (c.dead || (c.closing && c.out.empty())) {
            close_native(c.sock);
            conns_.erase(it);
            if (events.disconnected) events.disconnected(id);
        }
    }
}

TcpClient::TcpClient() { ensure_sockets(); }
TcpClient::~TcpClient() { disconnect(); }

bool TcpClient::connect(std::uint32_t ip, std::uint16_t port, int timeout_ms, std::string* error) {
    disconnect();
    const auto s = static_cast<std::intptr_t>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
#ifdef _WIN32
    if (as_native(s) == INVALID_SOCKET) {
#else
    if (s < 0) {
#endif
        error_ = socket_error_text();
        if (error) *error = error_;
        return false;
    }
    set_nonblocking(s);
    set_nodelay(s);
    const sockaddr_in addr = make_addr(ip, port);
    const int r = ::connect(as_native(s), reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
    if (r != 0 && !would_block()) {
        error_ = socket_error_text();
        if (error) *error = error_;
        close_native(s);
        return false;
    }
    fd_set writable, failed;
    FD_ZERO(&writable);
    FD_ZERO(&failed);
    FD_SET(as_native(s), &writable);
    FD_SET(as_native(s), &failed);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    const int n = ::select(static_cast<int>(as_native(s) + 1), nullptr, &writable, &failed, &tv);
    int so_error = 0;
    sock_len len = sizeof so_error;
    getsockopt(as_native(s), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_error), &len);
    if (n <= 0 || FD_ISSET(as_native(s), &failed) || so_error != 0) {
        error_ = n <= 0 ? "connect timed out" : "connect failed (error " + std::to_string(so_error) + ")";
        if (error) *error = error_;
        close_native(s);
        return false;
    }
    sock_ = s;
    reader_ = FrameReader{};
    out_.clear();
    return true;
}

void TcpClient::send(std::vector<std::uint8_t> bytes) {
    if (sock_ == kNone) return;
    out_.insert(out_.end(), bytes.begin(), bytes.end());
    if (!flush(sock_, out_)) {
        error_ = "connection lost";
        disconnect();
    }
}

std::vector<Frame> TcpClient::poll(int timeout_ms) {
    std::vector<Frame> frames;
    if (sock_ == kNone) return frames;
    fd_set readable, writable;
    FD_ZERO(&readable);
    FD_ZERO(&writable);
    FD_SET(as_native(sock_), &readable);
    if (!out_.empty()) FD_SET(as_native(sock_), &writable);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    ::select(static_cast<int>(as_native(sock_) + 1), &readable, &writable, nullptr, &tv);
    bool alive = flush(sock_, out_);
    if (alive && FD_ISSET(as_native(sock_), &readable)) alive = drain(sock_, reader_);
    while (auto f = reader_.next()) frames.push_back(std::move(*f));
    if (reader_.bad()) alive = false;
    if (!alive) {
        error_ = "connection closed by the server";
        disconnect();
    }
    return frames;
}

void TcpClient::disconnect() {
    if (sock_ != kNone) close_native(sock_);
    sock_ = kNone;
}

}  // namespace omega::online
