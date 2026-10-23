#pragma once
// PORT-ONLY (not in the original; LAN phase 2 groundwork, checkpoint 132ln): LanTransport, the host UDP
// transport under the game's net layer in LAN mode. research/LAN_PLAY.md "Decision" (peer to peer, the host
// is a peer, not a server; shared with future online play), §5.1 item 3, §5.2 option M and its LanTransport
// contract, §5.3 (join handshake), §6 (build hash), §9 items 2 and 7.
//
// Option M keeps the game's net API (NetMsg registry, Net_SendTyped 0x2DE398 fragmentation, the recipient and
// flag conversions Net_SendConvertRecipient 0x2DDF58 / Net_TranslateSendFlags 0x2E60C0, Net_DispatchRecv
// 0x2DEF08) and replaces what lies below it: NetGame_Create 0x2EA788, NetSession_CreateSockets 0x2FCA18,
// NetConn_* (0x2FE240 / 0x2FF058 / 0x3012B0 / 0x300BD8) and RtUdp_* (send to peers 0x303710, endpoint send
// 0x305170, batching 0x307a20, header 0x308a58, send pump RtUdp_SendPump 0x3091B8, ACK 0x30ac40 / 0x30a440,
// duplicate check 0x30acd8, ordered read 0x307dd8, reader 0x304800, update 0x3084b8 / 0x3085d8). The RT UDP
// timing is re-implemented, not ported (§5.2 "Cons"). Nothing here is wired into the game yet.
//
// ---- Contract (§5.2) ------------------------------------------------------------------------------------------
// host(port) / connect(addr, port) / close(). Client index 0 = the host; joiners get 1..3 in JOIN_ACK order
// (the lowest free index, which is JOIN_ACK order while nobody leaves; mirrors "Remote client joined the game:
// %d[%d]" 0x4be284 / Net_OnRemoteClientJoined 0x3FAAB0). send(recipient, bytes, flags) with recipient a client
// index, kAll (0xffff, every other client) or kHostRecipient (0xfffe: Net_SendConvertRecipient's -1, which
// NetConn_BufferMessage 0x2FF058 sends as kind 2 without a recipient field; read here as "the session host"
// [I]); flags the RT flags Net_TranslateSendFlags produces: 0x04 reliable + ordered, 0x02 and 0x01
// unreliable. poll(handlers) calls on_recv(origin client, bytes), on_join(client), on_leave(client, reason).
//
// Reliable channel (0x04): a per-link 32-bit sequence (starting at the value exchanged in the handshake; the
// original starts at 1 and skips 0 on wrap, 0x00308a58), a cumulative ACK plus a 32-bit selective-ACK
// bitfield piggybacked on every packet, resends with an exponential backoff capped at 500 ms (the original
// multiplies a retry base capped at 500 ms by the attempt count, RtUdp_SendPump 0x3091B8 / 0x0030ae88),
// in-order delivery and duplicate drop (0x30acd8 / 0x307dd8; a duplicate is still ACKed, as 0x0030a5c8 does).
// Unreliable channel (0x02 / 0x01, no 0x04): a per-origin sequence; a packet not newer than the newest one
// already delivered from that origin is dropped (latest wins). The 0x01 / 0x02 distinction travels in the
// flags byte unchanged; both are latest-wins until phase 3 shows what 0x02 (its own counter at peer +0x78 in
// the original) means to the receiver.
//
// Topology (Decision, §5.2, §9 item 2): a star through the hosting peer. Clients send everything to the host;
// the host delivers what is addressed to it (0, 0xfffe, 0xffff) and relays client->client and 0xffff messages.
// All routing decisions are in LanStarRoute below, so a full mesh (rt_p2p.c, if phase 3 shows the original
// used one) replaces that one struct plus the client links.
//
// Timeouts (§5.2): 5 s without any packet from a link = disconnect -> on_leave(client, Timeout) (the existing
// HOST_DISCONNECT / GAME_DISCONNECT path, DME.cc strings 0x4bf9b8-0x4bfa18). Keepalives every 500 ms keep an
// idle link alive.
//
// ---- Wire format (little-endian) ------------------------------------------------------------------------------
// Every datagram is a 20-byte header plus `length` payload bytes, at most kLanMaxDatagram (1200) in all, so a
// 0x200-byte NetConn aggregate fits.
//   off size field
//    0    1 flags     RT flags: 0x04 reliable+ordered, 0x02, 0x01; plus 0x40 on DATA (the original's data
//                     insertion bit, 0x00308a58) and 0x20 on a pure ACK / keepalive (0x0030ac40's control flag)
//    1    1 context   LanPacketKind (the original's receive-context byte, +0x48)
//    2    2 length    payload bytes after the header
//    4    4 peer      bits 0-15 recipient (client index, 0xffff, 0xfffe), bits 16-23 origin client index
//                     (0xff before the joiner has one), bits 24-31 zero
//    8    4 sequence  reliable: this link's sequence; unreliable DATA: the origin's unreliable sequence;
//                     handshake and ACK packets: 0
//   12    4 ack       cumulative ACK: every reliable sequence up to and including this one was received
//   16    4 ack_bits  bit i set: sequence ack + 1 + i was also received (out of order)
//
// Payloads by kind:
//   JOIN_REQ   (0x01, unsequenced) "SFOSJOIN"[8], u16 protocol_version, u32 build_hash, char[16] agent name,
//                                  u32 first reliable sequence of the joiner                       = 34 bytes
//                                  then, when the joiner has one (checkpoint 132md5, port revision 2), the join
//                                  extension: u16 length + that many opaque bytes (at most 1024)   = 36 + length
//   JOIN_ACK   (0x02, unsequenced) u16 client index, u16 max clients, u64 session id, u32 host clock ms
//                                  (the session clock offset of §6 item 4), u32 first reliable sequence of
//                                  the host, char[16] host name                                     = 36 bytes
//   JOIN_NAK   (0x03, unsequenced) u8 LanJoinError                                                  = 1 byte
//                                  Refused (132md5) adds the host's reason as text (at most 200)   = 1 + length
//   ACK        (0x04, unsequenced) ACK / keepalive, no payload
//   DATA       (0x05)              the game bytes
//   PEER_JOIN  (0x06, reliable)    u16 client index, char[16] name                                  = 18 bytes
//   PEER_LEAVE (0x07, reliable)    u16 client index, u8 LanLeaveReason                              = 3 bytes
//   BYE        (0x08, unsequenced) u8 LanLeaveReason (sent 3 times by close())                      = 1 byte

#include "lan_socket.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace omega::lan {

constexpr std::size_t kLanMaxDatagram = 1200;
constexpr std::size_t kLanHeaderSize = 20;
constexpr std::size_t kLanMaxPayload = kLanMaxDatagram - kLanHeaderSize;  // 1180
constexpr std::uint16_t kLanMaxClients = 4;                                // the host + 3 joiners

constexpr std::uint16_t kLanAll = 0xffff;            // every other client
constexpr std::uint16_t kLanHostRecipient = 0xfffe;  // Net_SendConvertRecipient's -1: the session host [I]

constexpr std::uint8_t kLanReliableOrdered = 0x04;  // game flag 0x40 -> RT 0x04
constexpr std::uint8_t kLanSequenced = 0x02;        // game flag 0x10 -> RT 0x02
constexpr std::uint8_t kLanUnreliable = 0x01;       // game flag 0x80 -> RT 0x01
constexpr std::uint8_t kLanFlagData = 0x40;
constexpr std::uint8_t kLanFlagAck = 0x20;

enum class LanPacketKind : std::uint8_t {
    JoinRequest = 0x01,
    JoinAccept = 0x02,
    JoinReject = 0x03,
    Ack = 0x04,
    Data = 0x05,
    PeerJoin = 0x06,
    PeerLeave = 0x07,
    Bye = 0x08,
};

enum class LanJoinError : std::uint8_t {
    None = 0,
    Full = 1,             // all kLanMaxClients slots taken (or the host's max_clients)
    InProgress = 2,       // the host no longer accepts joins (§6 item 7: lobby-time joins only)
    VersionMismatch = 3,  // protocol version or build hash differ (§6 item 1)
    Timeout = 4,          // no JOIN_ACK / JOIN_NAK within the timeout
    Refused = 5,          // the host's join filter refused the joiner (132md5: its mods); join_detail() says why
};

constexpr std::size_t kLanMaxJoinExtension = 1024;  // JOIN_REQ's extension bytes (132md5)
constexpr std::size_t kLanMaxJoinDetail = 200;      // JOIN_NAK Refused's reason text (132md5)

// Checkpoint 132md5 (port-only): the host's say on a joiner that passed the version, in-progress and slot checks:
// nullopt accepts it, a text refuses it (JOIN_NAK Refused with the text, truncated to kLanMaxJoinDetail). Called
// for every JOIN_REQ retry of a refused joiner (the refusal is stateless, as the other NAKs).
using LanJoinFilter = std::function<std::optional<std::string>(const LanAddress& from, const std::string& name,
                                                                std::span<const std::uint8_t> extension)>;

enum class LanLeaveReason : std::uint8_t {
    Closed = 1,    // the peer closed (BYE)
    Timeout = 2,   // 5 s of silence
    HostLeft = 3,  // on a client: the session ended because the host left; reported for the other clients
};

struct LanPacketHeader {
    std::uint8_t flags = 0;
    LanPacketKind kind = LanPacketKind::Ack;
    std::uint16_t length = 0;
    std::uint16_t recipient = 0;
    std::uint8_t origin = 0;
    std::uint32_t sequence = 0;
    std::uint32_t ack = 0;
    std::uint32_t ack_bits = 0;
    friend bool operator==(const LanPacketHeader&, const LanPacketHeader&) = default;
};

void lan_encode_header(const LanPacketHeader& header, std::vector<std::uint8_t>& out);
// Rejects a short datagram, a length that does not match the datagram, an unknown kind or a nonzero bits
// 24-31 of the peer field.
std::optional<LanPacketHeader> lan_decode_header(std::span<const std::uint8_t> datagram);

// PORT-ONLY: the star topology's routing, the one place a mesh would change (Decision; §5.2 "Topology").
struct LanStarRoute {
    // Whether the local client consumes a message addressed to `recipient`.
    static bool delivers_locally(std::uint16_t local, std::uint16_t recipient);
    // The links the local client sends a message from `origin` to `recipient` on. `present` lists the
    // connected client indices other than the local one. On a client every outgoing message goes to the host
    // (index 0) and nothing received is forwarded; on the host, 0xffff fans out to every client but the
    // origin and a client index goes to that client.
    static std::vector<std::uint16_t> next_hops(bool local_is_host, std::uint16_t local, std::uint16_t origin,
                                                std::uint16_t recipient, std::span<const std::uint16_t> present);
};

struct LanTransportConfig {
    std::uint16_t protocol_version = 1;
    std::uint32_t build_hash = 0;
    std::string agent_name;
    std::uint64_t session_id = 0;           // host: sent in JOIN_ACK
    std::uint16_t max_clients = kLanMaxClients;
    std::uint64_t timeout_ms = 5000;        // silence = disconnect; also the join timeout
    std::uint64_t keepalive_ms = 500;
    std::uint64_t initial_resend_ms = 100;  // first retry; doubles per attempt
    std::uint64_t max_resend_ms = 500;      // RtUdp_SendPump's 500 ms cap
    std::uint64_t join_retry_ms = 250;
    std::uint32_t first_sequence = 1;       // tests set it near 0xffffffff to cover the wrap
    std::vector<std::uint8_t> join_extension;  // joiner: sent after JOIN_REQ's fields when not empty (132md5)
    LanJoinFilter join_filter;                 // host: unset accepts every joiner that fits (132md5)
    LanClock clock = lan_steady_clock();
    LanSocketFactory socket_factory = lan_udp_socket_factory();
};

enum class LanTransportState { Idle, Hosting, Connecting, Connected, Rejected, Disconnected };

enum class LanSendResult { Sent, NotConnected, TooLarge, BadRecipient };

struct LanTransportHandlers {
    std::function<void(std::uint16_t origin, std::span<const std::uint8_t> bytes)> on_recv;
    std::function<void(std::uint16_t client)> on_join;
    std::function<void(std::uint16_t client, LanLeaveReason reason)> on_leave;
};

struct LanTransportStats {
    std::uint64_t datagrams_sent = 0, datagrams_received = 0, resends = 0, duplicates_dropped = 0,
                  stale_unreliable_dropped = 0, relayed = 0, rejected_datagrams = 0;
};

// PORT-ONLY: see the file comment.
class LanTransport {
public:
    explicit LanTransport(LanTransportConfig config);
    ~LanTransport();
    LanTransport(const LanTransport&) = delete;
    LanTransport& operator=(const LanTransport&) = delete;

    bool host(std::uint16_t port);  // 0 = an ephemeral port (tests); see local_port()
    bool connect(const LanAddress& host_address);
    bool connect(std::uint32_t ip, std::uint16_t port) { return connect(LanAddress{ip, port}); }
    void close();  // BYE to every link; no on_leave for the local side

    LanSendResult send(std::uint16_t recipient, std::span<const std::uint8_t> bytes, std::uint8_t flags);
    void poll(const LanTransportHandlers& handlers);

    // Host: whether new joins are accepted (false -> JOIN_NAK InProgress).
    void set_accepting_joins(bool accepting) { accepting_joins_ = accepting; }

    LanTransportState state() const { return state_; }
    bool is_host() const { return state_ == LanTransportState::Hosting; }
    std::uint16_t local_index() const { return local_index_; }
    LanJoinError join_error() const { return join_error_; }
    const std::string& join_detail() const { return join_detail_; }  // a JOIN_NAK Refused's text (132md5)
    std::uint16_t local_port() const;
    std::vector<std::uint16_t> clients() const;  // connected client indices other than the local one
    std::string client_name(std::uint16_t client) const;
    // Host (132md5): the join extension a connected client sent in its JOIN_REQ (empty: none).
    std::vector<std::uint8_t> client_extension(std::uint16_t client) const;
    std::uint64_t session_id() const { return session_id_; }
    // Client: the host clock minus the local clock at JOIN_ACK (the shared session clock of §6 item 4).
    std::int64_t host_clock_offset_ms() const { return host_clock_offset_ms_; }
    const LanTransportStats& stats() const { return stats_; }

private:
    struct Outgoing {
        std::uint32_t sequence = 0;
        LanPacketKind kind = LanPacketKind::Data;
        std::uint8_t flags = 0;
        std::uint16_t recipient = 0;
        std::uint8_t origin = 0;
        std::vector<std::uint8_t> payload;
        std::uint64_t next_send_ms = 0;
        std::uint32_t attempts = 0;
    };
    struct Incoming {
        LanPacketKind kind = LanPacketKind::Data;
        std::uint8_t flags = 0;
        std::uint16_t recipient = 0;
        std::uint8_t origin = 0;
        std::vector<std::uint8_t> payload;
    };
    struct Link {
        LanAddress address;
        std::uint16_t index = 0;
        std::string name;
        bool established = false;  // a client's link to the host before JOIN_ACK is not
        std::uint64_t last_recv_ms = 0;
        std::uint64_t last_send_ms = 0;
        std::uint64_t join_started_ms = 0;
        bool ack_pending = false;
        std::uint32_t send_next = 1;  // next reliable sequence to assign
        std::deque<Outgoing> unacked;
        std::uint32_t recv_next = 1;  // next reliable sequence to deliver
        std::map<std::uint32_t, Incoming> recv_buffer;  // received out of order, keyed by sequence (looked up only)
    };
    struct Event {
        enum class Type { Recv, Join, Leave } type = Type::Recv;
        std::uint16_t client = 0;
        LanLeaveReason reason = LanLeaveReason::Closed;
        std::vector<std::uint8_t> bytes;
    };

    Link* link_for(const LanAddress& address);
    Link* link_at(std::uint16_t index);
    void transmit(const LanAddress& to, const LanPacketHeader& header, std::span<const std::uint8_t> payload);
    void send_packet(Link& link, LanPacketHeader header, std::span<const std::uint8_t> payload, std::uint64_t now);
    void send_unsequenced(Link& link, LanPacketKind kind, std::uint8_t flags, std::span<const std::uint8_t> payload,
                          std::uint64_t now);
    void send_outgoing(Link& link, Outgoing& outgoing, std::uint64_t now);
    std::vector<std::uint8_t> join_accept_payload(const Link& link) const;
    void send_reliable(Link& link, LanPacketKind kind, std::uint8_t flags, std::uint16_t recipient,
                       std::uint8_t origin, std::vector<std::uint8_t> payload, std::uint64_t now);
    void send_unreliable(Link& link, std::uint8_t flags, std::uint16_t recipient, std::uint8_t origin,
                         std::uint32_t sequence, std::span<const std::uint8_t> payload, std::uint64_t now);
    void route(std::uint16_t origin, std::uint16_t recipient, std::uint8_t flags, std::uint32_t unreliable_sequence,
               std::span<const std::uint8_t> payload, std::uint64_t now);
    void handle_datagram(const LanAddress& from, std::span<const std::uint8_t> datagram, std::uint64_t now);
    void handle_join_request(const LanAddress& from, std::span<const std::uint8_t> payload, std::uint64_t now);
    void handle_ack(Link& link, std::uint32_t ack, std::uint32_t ack_bits);
    void handle_reliable(Link& link, const LanPacketHeader& header, std::span<const std::uint8_t> payload,
                         std::uint64_t now);
    void handle_message(Link& link, LanPacketKind kind, std::uint8_t flags, std::uint16_t recipient,
                        std::uint8_t origin, std::uint32_t unreliable_sequence, std::span<const std::uint8_t> payload,
                        std::uint64_t now);
    void deliver_data(std::uint16_t origin, std::uint16_t recipient, std::uint8_t flags,
                      std::uint32_t unreliable_sequence, std::span<const std::uint8_t> payload, std::uint64_t now);
    void drop_link(std::uint16_t index, LanLeaveReason reason, std::uint64_t now);
    void lose_host(LanLeaveReason reason);
    void service_link(Link& link, std::uint64_t now);
    std::uint64_t resend_delay(std::uint32_t attempts) const;
    std::vector<std::uint8_t> join_request_payload() const;
    void reset();

    LanTransportConfig config_;
    std::unique_ptr<LanSocket> socket_;
    LanTransportState state_ = LanTransportState::Idle;
    std::uint16_t local_index_ = 0;
    LanJoinError join_error_ = LanJoinError::None;
    std::string join_detail_;
    bool accepting_joins_ = true;
    std::uint64_t session_id_ = 0;
    std::int64_t host_clock_offset_ms_ = 0;
    std::array<std::unique_ptr<Link>, kLanMaxClients> links_{};  // by client index (a client: [0] = host)
    std::array<bool, kLanMaxClients> present_{};                 // every client in the session, local included
    std::array<std::string, kLanMaxClients> names_{};
    std::array<std::vector<std::uint8_t>, kLanMaxClients> extensions_{};  // host: each joiner's (132md5)
    std::uint32_t unreliable_next_ = 1;
    std::array<std::optional<std::uint32_t>, kLanMaxClients> unreliable_latest_{};  // per origin
    std::vector<Event> events_;
    LanTransportStats stats_{};
};

}  // namespace omega::lan
