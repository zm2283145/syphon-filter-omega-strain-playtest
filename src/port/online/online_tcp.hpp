#pragma once
// PORT-ONLY (not in the original; checkpoint 132on1, research/ONLINE.md §5.1): the TCP transport between the game
// and omega_server. The original reached the Medius servers through the SCE-RT socket layer (Net_SendLocked
// 0x002ea0a0 and its receive side), which the port does not run; this carries the same Medius request / response
// structs inside the frames of online_protocol.hpp. Portable: Winsock on Windows, BSD sockets elsewhere (so
// omega_server can be built for a hosted Linux server). Non-blocking sockets; the server waits with poll() / WSAPoll()
// (132on2h: select() capped Winsock at 64 sockets), a client with select().

#include "online_protocol.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omega::online {

// "a.b.c.d" or a host name (resolved to IPv4 with getaddrinfo); host byte order.
std::optional<std::uint32_t> resolve_ipv4(const std::string& host);
// "host[:port]" -> host, port (default_port when absent). nullopt on a bad port.
std::optional<std::pair<std::string, std::uint16_t>> split_host_port(std::string_view text, std::uint16_t default_port);

// PORT-ONLY (checkpoint 132on2e): omega_server's UDP socket for the in-level relay (online_relay.hpp). Non-blocking.
class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool open(std::uint32_t bind_ip, std::uint16_t port, std::string* error);  // port 0 picks a free port
    void close();
    bool is_open() const { return sock_ != -1; }
    std::uint16_t port() const { return port_; }
    std::intptr_t native() const { return sock_; }
    bool send_to(std::uint32_t ip, std::uint16_t port, std::span<const std::uint8_t> bytes);
    struct Datagram {
        std::uint32_t ip = 0;
        std::uint16_t port = 0;
        std::vector<std::uint8_t> bytes;
    };
    std::optional<Datagram> receive();  // nullopt: nothing waiting

private:
    std::intptr_t sock_ = -1;
    std::uint16_t port_ = 0;
};

// PORT-ONLY (checkpoint 132on2e, online_upnp.hpp): one blocking HTTP/1.1 exchange (send `request`, read until the
// peer closes or timeout_ms passes); nullopt when the connection fails or nothing arrived.
std::optional<std::string> tcp_exchange(std::uint32_t ip, std::uint16_t port, const std::string& request, int timeout_ms);
// The local IPv4 address the system routes toward `remote_ip` (a connected UDP socket's name; nothing is sent).
std::optional<std::uint32_t> local_ip_toward(std::uint32_t remote_ip);

class TcpServer {
public:
    TcpServer();
    ~TcpServer();
    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // bind_ip in host order (0x7f000001 = loopback, 0 = all interfaces); port 0 picks a free port.
    bool listen(std::uint32_t bind_ip, std::uint16_t port, std::string* error);
    std::uint16_t port() const { return port_; }

    struct Events {
        std::function<void(std::uint32_t conn, std::uint32_t peer_ip)> connected;
        std::function<void(std::uint32_t conn, const Frame& frame)> frame;
        std::function<void(std::uint32_t conn)> disconnected;
    };
    // Accepts, reads, writes; waits up to timeout_ms for activity (or a datagram on `also_wait`, the relay's UDP
    // socket, which the caller then drains). Callbacks run inside.
    void poll(int timeout_ms, const Events& events, const UdpSocket* also_wait = nullptr);
    void send(std::uint32_t conn, std::vector<std::uint8_t> bytes);
    // Closes after the queued bytes are written (or at once if the peer is gone); `disconnected` follows.
    void close(std::uint32_t conn);
    std::size_t connection_count() const { return conns_.size(); }
    // 132on2h: how long the last poll() waited for activity (microseconds); the rest of a loop iteration is work.
    std::uint64_t last_wait_us() const { return last_wait_us_; }

private:
    struct Conn;
    std::intptr_t listener_ = -1;
    std::uint16_t port_ = 0;
    std::uint32_t next_id_ = 1;
    std::uint64_t last_wait_us_ = 0;
    std::map<std::uint32_t, std::unique_ptr<Conn>> conns_;
};

class TcpClient {
public:
    TcpClient();
    ~TcpClient();
    TcpClient(const TcpClient&) = delete;
    TcpClient& operator=(const TcpClient&) = delete;

    bool connect(std::uint32_t ip, std::uint16_t port, int timeout_ms, std::string* error);
    bool connected() const { return sock_ != -1; }
    void send(std::vector<std::uint8_t> bytes);
    // Writes queued bytes and reads what arrived (waiting up to timeout_ms); returns the whole frames.
    std::vector<Frame> poll(int timeout_ms);
    void disconnect();
    std::string last_error() const { return error_; }

private:
    std::intptr_t sock_ = -1;
    std::vector<std::uint8_t> out_;
    FrameReader reader_;
    std::string error_;
};

}  // namespace omega::online
