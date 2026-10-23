// PORT-ONLY (not in the original; checkpoint 132ln): LanTransport, the LAN-mode stand-in for the RT UDP / DME
// layer below the game's net API (option M): NetGame_Create 0x2EA788, NetSession_CreateSockets 0x2FCA18,
// NetConn_* and RtUdp_* (RtUdp_SendPump 0x3091B8, ACK 0x30ac40 / 0x30a440, duplicate check 0x30acd8, ordered
// read 0x307dd8). See lan_transport.hpp for the contract and wire format, research/LAN_PLAY.md §5.2 / §5.3.
#include "lan_transport.hpp"

#include "lan_wire.hpp"

#include <algorithm>
#include <utility>

namespace omega::lan {

namespace {

constexpr std::array<std::uint8_t, 8> kJoinMagic{'S', 'F', 'O', 'S', 'J', 'O', 'I', 'N'};
constexpr std::size_t kJoinRequestSize = 34;
constexpr std::size_t kJoinAcceptSize = 36;
constexpr std::size_t kNameSize = 16;
constexpr std::uint8_t kNoOrigin = 0xff;
constexpr std::uint32_t kReceiveWindow = 1024;  // reliable sequences buffered ahead of the next in-order one
constexpr int kByeRepeats = 3;

bool valid_kind(std::uint8_t kind) { return kind >= 0x01 && kind <= 0x08; }

}  // namespace

// ---- Header ----------------------------------------------------------------------------------------------------

void lan_encode_header(const LanPacketHeader& header, std::vector<std::uint8_t>& out) {
    LanWriter writer(out);
    writer.u8(header.flags);
    writer.u8(static_cast<std::uint8_t>(header.kind));
    writer.u16(header.length);
    writer.u32(static_cast<std::uint32_t>(header.recipient) | (static_cast<std::uint32_t>(header.origin) << 16));
    writer.u32(header.sequence);
    writer.u32(header.ack);
    writer.u32(header.ack_bits);
}

std::optional<LanPacketHeader> lan_decode_header(std::span<const std::uint8_t> datagram) {
    if (datagram.size() < kLanHeaderSize || datagram.size() > kLanMaxDatagram) return std::nullopt;
    LanReader in(datagram);
    LanPacketHeader header;
    header.flags = in.u8();
    const std::uint8_t kind = in.u8();
    header.length = in.u16();
    const std::uint32_t peer = in.u32();
    header.sequence = in.u32();
    header.ack = in.u32();
    header.ack_bits = in.u32();
    if (in.failed() || !valid_kind(kind) || (peer >> 24) != 0) return std::nullopt;
    if (header.length != datagram.size() - kLanHeaderSize) return std::nullopt;
    header.kind = static_cast<LanPacketKind>(kind);
    header.recipient = static_cast<std::uint16_t>(peer & 0xffffu);
    header.origin = static_cast<std::uint8_t>((peer >> 16) & 0xffu);
    return header;
}

// ---- Routing (the one place the star lives) --------------------------------------------------------------------

bool LanStarRoute::delivers_locally(std::uint16_t local, std::uint16_t recipient) {
    return recipient == local || recipient == kLanAll || (recipient == kLanHostRecipient && local == 0);
}

std::vector<std::uint16_t> LanStarRoute::next_hops(bool local_is_host, std::uint16_t local, std::uint16_t origin,
                                                   std::uint16_t recipient, std::span<const std::uint16_t> present) {
    std::vector<std::uint16_t> hops;
    if (!local_is_host) {
        // A client sends everything through the host and never forwards.
        if (origin == local) hops.push_back(0);
        return hops;
    }
    for (const std::uint16_t client : present) {
        if (client == local || client == origin) continue;
        if (recipient == kLanAll || recipient == client) hops.push_back(client);
    }
    return hops;
}

// ---- LanTransport ----------------------------------------------------------------------------------------------

LanTransport::LanTransport(LanTransportConfig config) : config_(std::move(config)) {
    config_.max_clients = std::clamp<std::uint16_t>(config_.max_clients, 1, kLanMaxClients);
}

LanTransport::~LanTransport() { close(); }

void LanTransport::reset() {
    if (socket_) socket_->close();
    socket_.reset();
    for (auto& link : links_) link.reset();
    present_.fill(false);
    for (auto& name : names_) name.clear();
    for (auto& extension : extensions_) extension.clear();
    unreliable_latest_.fill(std::nullopt);
    unreliable_next_ = 1;
    local_index_ = 0;
    host_clock_offset_ms_ = 0;
    session_id_ = 0;
}

bool LanTransport::host(std::uint16_t port) {
    close();
    events_.clear();
    join_error_ = LanJoinError::None;
    join_detail_.clear();
    auto socket = config_.socket_factory();
    if (!socket || !socket->open(port, LanSocketOptions{})) return false;
    socket_ = std::move(socket);
    state_ = LanTransportState::Hosting;
    local_index_ = 0;
    present_[0] = true;
    names_[0] = config_.agent_name;
    session_id_ = config_.session_id;
    return true;
}

bool LanTransport::connect(const LanAddress& host_address) {
    close();
    events_.clear();
    join_error_ = LanJoinError::None;
    join_detail_.clear();
    auto socket = config_.socket_factory();
    if (!socket || !socket->open(0, LanSocketOptions{})) return false;
    socket_ = std::move(socket);
    state_ = LanTransportState::Connecting;
    const std::uint64_t now = config_.clock();
    auto link = std::make_unique<Link>();
    link->address = host_address;
    link->index = 0;
    link->join_started_ms = now;
    link->last_recv_ms = now;
    link->send_next = config_.first_sequence;
    links_[0] = std::move(link);
    send_unsequenced(*links_[0], LanPacketKind::JoinRequest, 0, join_request_payload(), now);
    return true;
}

void LanTransport::close() {
    if (socket_) {
        const std::uint64_t now = config_.clock();
        const std::uint8_t reason = static_cast<std::uint8_t>(LanLeaveReason::Closed);
        for (auto& link : links_) {
            if (!link) continue;
            for (int i = 0; i < kByeRepeats; ++i)
                send_unsequenced(*link, LanPacketKind::Bye, 0, std::span<const std::uint8_t>(&reason, 1), now);
        }
    }
    reset();
    state_ = LanTransportState::Idle;
}

std::uint16_t LanTransport::local_port() const { return socket_ ? socket_->local_port() : 0; }

std::vector<std::uint16_t> LanTransport::clients() const {
    std::vector<std::uint16_t> out;
    for (std::uint16_t i = 0; i < kLanMaxClients; ++i)
        if (present_[i] && i != local_index_) out.push_back(i);
    return out;
}

std::string LanTransport::client_name(std::uint16_t client) const {
    return client < kLanMaxClients && present_[client] ? names_[client] : std::string{};
}

std::vector<std::uint8_t> LanTransport::client_extension(std::uint16_t client) const {
    return client < kLanMaxClients && present_[client] ? extensions_[client] : std::vector<std::uint8_t>{};
}

LanTransport::Link* LanTransport::link_for(const LanAddress& address) {
    for (auto& link : links_)
        if (link && link->address == address) return link.get();
    return nullptr;
}

LanTransport::Link* LanTransport::link_at(std::uint16_t index) {
    return index < kLanMaxClients ? links_[index].get() : nullptr;
}

std::uint64_t LanTransport::resend_delay(std::uint32_t attempts) const {
    // attempts transmissions so far (>= 1): initial, 2x, 4x, ... capped (RtUdp_SendPump 0x3091B8's 500 ms cap).
    const std::uint32_t shift = std::min<std::uint32_t>(attempts > 0 ? attempts - 1 : 0, 20);
    return std::min<std::uint64_t>(config_.initial_resend_ms << shift, config_.max_resend_ms);
}

std::vector<std::uint8_t> LanTransport::join_request_payload() const {
    std::vector<std::uint8_t> payload;
    LanWriter out(payload);
    out.bytes(kJoinMagic);
    out.u16(config_.protocol_version);
    out.u32(config_.build_hash);
    out.fixed_text(config_.agent_name, kNameSize);
    out.u32(config_.first_sequence);
    if (!config_.join_extension.empty()) {  // 132md5: only a joiner with an extension sends more than 34 bytes
        const auto size = std::min(config_.join_extension.size(), kLanMaxJoinExtension);
        out.u16(static_cast<std::uint16_t>(size));
        out.bytes(std::span<const std::uint8_t>(config_.join_extension.data(), size));
    }
    return payload;
}

std::vector<std::uint8_t> LanTransport::join_accept_payload(const Link& link) const {
    std::vector<std::uint8_t> payload;
    LanWriter out(payload);
    out.u16(link.index);
    out.u16(config_.max_clients);
    out.u64(session_id_);
    out.u32(static_cast<std::uint32_t>(config_.clock()));
    out.u32(config_.first_sequence);
    out.fixed_text(config_.agent_name, kNameSize);
    return payload;
}

void LanTransport::transmit(const LanAddress& to, const LanPacketHeader& header,
                            std::span<const std::uint8_t> payload) {
    if (!socket_) return;
    std::vector<std::uint8_t> datagram;
    datagram.reserve(kLanHeaderSize + payload.size());
    lan_encode_header(header, datagram);
    datagram.insert(datagram.end(), payload.begin(), payload.end());
    socket_->send_to(to, datagram);
    ++stats_.datagrams_sent;
}

void LanTransport::send_packet(Link& link, LanPacketHeader header, std::span<const std::uint8_t> payload,
                               std::uint64_t now) {
    // Every packet carries this link's receive state: the cumulative ACK and the selective bits after it.
    header.length = static_cast<std::uint16_t>(payload.size());
    header.ack = link.recv_next - 1;
    header.ack_bits = 0;
    for (const auto& [sequence, unused] : link.recv_buffer) {
        const std::uint32_t distance = sequence - link.recv_next;  // bit i = ack + 1 + i = recv_next + i
        if (distance < 32) header.ack_bits |= 1u << distance;
    }
    transmit(link.address, header, payload);
    link.last_send_ms = now;
    link.ack_pending = false;
}

void LanTransport::send_unsequenced(Link& link, LanPacketKind kind, std::uint8_t flags,
                                    std::span<const std::uint8_t> payload, std::uint64_t now) {
    LanPacketHeader header;
    header.flags = flags;
    header.kind = kind;
    header.recipient = (link.established || is_host()) ? link.index : kLanHostRecipient;
    header.origin = state_ == LanTransportState::Connecting ? kNoOrigin : static_cast<std::uint8_t>(local_index_);
    send_packet(link, header, payload, now);
}

void LanTransport::send_outgoing(Link& link, Outgoing& outgoing, std::uint64_t now) {
    LanPacketHeader header;
    header.flags = static_cast<std::uint8_t>((outgoing.flags & 0x07) |
                                             (outgoing.kind == LanPacketKind::Data ? kLanFlagData : 0));
    header.kind = outgoing.kind;
    header.recipient = outgoing.recipient;
    header.origin = outgoing.origin;
    header.sequence = outgoing.sequence;
    send_packet(link, header, outgoing.payload, now);
    ++outgoing.attempts;
    outgoing.next_send_ms = now + resend_delay(outgoing.attempts);
}

void LanTransport::send_reliable(Link& link, LanPacketKind kind, std::uint8_t flags, std::uint16_t recipient,
                                 std::uint8_t origin, std::vector<std::uint8_t> payload, std::uint64_t now) {
    Outgoing outgoing;
    outgoing.sequence = link.send_next++;
    outgoing.kind = kind;
    outgoing.flags = static_cast<std::uint8_t>(flags | kLanReliableOrdered);
    outgoing.recipient = recipient;
    outgoing.origin = origin;
    outgoing.payload = std::move(payload);
    link.unacked.push_back(std::move(outgoing));
    send_outgoing(link, link.unacked.back(), now);
}

void LanTransport::send_unreliable(Link& link, std::uint8_t flags, std::uint16_t recipient, std::uint8_t origin,
                                   std::uint32_t sequence, std::span<const std::uint8_t> payload,
                                   std::uint64_t now) {
    LanPacketHeader header;
    header.flags = static_cast<std::uint8_t>((flags & 0x03) | kLanFlagData);
    header.kind = LanPacketKind::Data;
    header.recipient = recipient;
    header.origin = origin;
    header.sequence = sequence;
    send_packet(link, header, payload, now);
}

void LanTransport::route(std::uint16_t origin, std::uint16_t recipient, std::uint8_t flags,
                         std::uint32_t unreliable_sequence, std::span<const std::uint8_t> payload,
                         std::uint64_t now) {
    const std::vector<std::uint16_t> present = clients();
    for (const std::uint16_t hop : LanStarRoute::next_hops(is_host(), local_index_, origin, recipient, present)) {
        Link* link = link_at(hop);
        if (link == nullptr || !link->established) continue;
        if (origin != local_index_) ++stats_.relayed;
        if (flags & kLanReliableOrdered)
            send_reliable(*link, LanPacketKind::Data, flags, recipient, static_cast<std::uint8_t>(origin),
                          std::vector<std::uint8_t>(payload.begin(), payload.end()), now);
        else
            send_unreliable(*link, flags, recipient, static_cast<std::uint8_t>(origin), unreliable_sequence, payload,
                            now);
    }
}

LanSendResult LanTransport::send(std::uint16_t recipient, std::span<const std::uint8_t> bytes, std::uint8_t flags) {
    if (state_ != LanTransportState::Hosting && state_ != LanTransportState::Connected)
        return LanSendResult::NotConnected;
    if (bytes.size() > kLanMaxPayload) return LanSendResult::TooLarge;
    if (recipient == kLanHostRecipient) {
        if (is_host()) return LanSendResult::BadRecipient;
    } else if (recipient != kLanAll) {
        if (recipient >= kLanMaxClients || recipient == local_index_ || !present_[recipient])
            return LanSendResult::BadRecipient;
    }
    flags = static_cast<std::uint8_t>(flags & 0x07);
    if (flags == 0) flags = kLanUnreliable;
    const std::uint32_t sequence = (flags & kLanReliableOrdered) ? 0 : unreliable_next_++;
    route(local_index_, recipient, flags, sequence, bytes, config_.clock());
    return LanSendResult::Sent;
}

void LanTransport::handle_ack(Link& link, std::uint32_t ack, std::uint32_t ack_bits) {
    std::erase_if(link.unacked, [&](const Outgoing& outgoing) {
        if (lan_seq_less_equal(outgoing.sequence, ack)) return true;
        const std::uint32_t distance = outgoing.sequence - ack - 1;
        return distance < 32 && (ack_bits & (1u << distance)) != 0;
    });
}

void LanTransport::handle_join_request(const LanAddress& from, std::span<const std::uint8_t> payload,
                                       std::uint64_t now) {
    // 34 bytes, or 34 + u16 length + the join extension (132md5).
    std::size_t extension_size = 0;
    if (payload.size() >= kJoinRequestSize + 2)
        extension_size = static_cast<std::size_t>(payload[kJoinRequestSize] | (payload[kJoinRequestSize + 1] << 8));
    const bool sized = payload.size() == kJoinRequestSize ||
                       (payload.size() == kJoinRequestSize + 2 + extension_size && extension_size <= kLanMaxJoinExtension);
    if (!sized || payload.size() < kJoinRequestSize ||
        !std::equal(kJoinMagic.begin(), kJoinMagic.end(), payload.begin())) {
        ++stats_.rejected_datagrams;
        return;
    }
    LanReader in(payload.subspan(kJoinMagic.size()));
    const std::uint16_t version = in.u16();
    const std::uint32_t build_hash = in.u32();
    std::string name = in.fixed_text(kNameSize);
    const std::uint32_t first_sequence = in.u32();
    const auto extension = payload.size() > kJoinRequestSize ? payload.subspan(kJoinRequestSize + 2)
                                                              : std::span<const std::uint8_t>{};

    LanJoinError error = LanJoinError::None;
    std::uint16_t index = 0;
    if (version != config_.protocol_version || build_hash != config_.build_hash) {
        error = LanJoinError::VersionMismatch;
    } else if (!accepting_joins_) {
        error = LanJoinError::InProgress;
    } else {
        for (std::uint16_t i = 1; i < config_.max_clients && index == 0; ++i)
            if (!present_[i] && !links_[i]) index = i;
        if (index == 0) error = LanJoinError::Full;
    }
    std::string detail;
    if (error == LanJoinError::None && config_.join_filter) {  // 132md5: the host's filter (its mod policy)
        if (auto why = config_.join_filter(from, name, extension)) {
            error = LanJoinError::Refused;
            detail = why->substr(0, kLanMaxJoinDetail);
        }
    }
    if (error != LanJoinError::None) {
        // Stateless: every retried JOIN_REQ gets the NAK again.
        std::vector<std::uint8_t> nak{static_cast<std::uint8_t>(error)};
        nak.insert(nak.end(), detail.begin(), detail.end());
        LanPacketHeader header;
        header.kind = LanPacketKind::JoinReject;
        header.length = static_cast<std::uint16_t>(nak.size());
        header.recipient = kLanHostRecipient;
        transmit(from, header, nak);
        return;
    }

    auto link = std::make_unique<Link>();
    link->address = from;
    link->index = index;
    link->name = name;
    link->established = true;
    link->last_recv_ms = now;
    link->send_next = config_.first_sequence;
    link->recv_next = first_sequence;
    links_[index] = std::move(link);
    Link& joined = *links_[index];
    send_unsequenced(joined, LanPacketKind::JoinAccept, 0, join_accept_payload(joined), now);

    // Tell the joiner about everyone already here, and everyone else about the joiner (reliable, ordered after
    // the handshake on each link).
    auto peer_join = [](std::uint16_t client, const std::string& client_name) {
        std::vector<std::uint8_t> bytes;
        LanWriter out(bytes);
        out.u16(client);
        out.fixed_text(client_name, kNameSize);
        return bytes;
    };
    for (std::uint16_t other = 1; other < kLanMaxClients; ++other) {
        if (other == index || !links_[other]) continue;
        send_reliable(joined, LanPacketKind::PeerJoin, 0, index, 0, peer_join(other, names_[other]), now);
        send_reliable(*links_[other], LanPacketKind::PeerJoin, 0, other, 0, peer_join(index, name), now);
    }
    present_[index] = true;
    names_[index] = name;
    extensions_[index].assign(extension.begin(), extension.end());
    unreliable_latest_[index].reset();
    events_.push_back(Event{Event::Type::Join, index, LanLeaveReason::Closed, {}});
}

void LanTransport::deliver_data(std::uint16_t origin, std::uint16_t recipient, std::uint8_t flags,
                                std::uint32_t unreliable_sequence, std::span<const std::uint8_t> payload,
                                std::uint64_t now) {
    if (origin >= kLanMaxClients) return;
    if (!(flags & kLanReliableOrdered)) {
        // Latest wins, per origin: drop anything not newer than the newest already taken from that client.
        auto& latest = unreliable_latest_[origin];
        if (latest && lan_seq_less_equal(unreliable_sequence, *latest)) {
            ++stats_.stale_unreliable_dropped;
            return;
        }
        latest = unreliable_sequence;
    }
    if (LanStarRoute::delivers_locally(local_index_, recipient))
        events_.push_back(Event{Event::Type::Recv, origin, LanLeaveReason::Closed,
                                std::vector<std::uint8_t>(payload.begin(), payload.end())});
    if (is_host()) route(origin, recipient, flags, unreliable_sequence, payload, now);
}

void LanTransport::handle_message(Link& link, LanPacketKind kind, std::uint8_t flags, std::uint16_t recipient,
                                  std::uint8_t origin, std::uint32_t unreliable_sequence,
                                  std::span<const std::uint8_t> payload, std::uint64_t now) {
    switch (kind) {
    case LanPacketKind::Data:
        // The host trusts the link, not the header, for who sent it.
        deliver_data(is_host() ? link.index : origin, recipient, flags, unreliable_sequence, payload, now);
        break;
    case LanPacketKind::PeerJoin: {
        if (is_host()) break;
        LanReader in(payload);
        const std::uint16_t client = in.u16();
        std::string name = in.fixed_text(kNameSize);
        if (in.failed() || client == 0 || client >= kLanMaxClients || client == local_index_) break;
        present_[client] = true;
        names_[client] = std::move(name);
        unreliable_latest_[client].reset();
        events_.push_back(Event{Event::Type::Join, client, LanLeaveReason::Closed, {}});
        break;
    }
    case LanPacketKind::PeerLeave: {
        if (is_host()) break;
        LanReader in(payload);
        const std::uint16_t client = in.u16();
        const auto reason = static_cast<LanLeaveReason>(in.u8());
        if (in.failed() || client == 0 || client >= kLanMaxClients || !present_[client]) break;
        present_[client] = false;
        names_[client].clear();
        events_.push_back(Event{Event::Type::Leave, client, reason, {}});
        break;
    }
    default:
        break;
    }
}

void LanTransport::handle_reliable(Link& link, const LanPacketHeader& header, std::span<const std::uint8_t> payload,
                                   std::uint64_t now) {
    link.ack_pending = true;  // ACK even a duplicate: its first ACK may have been lost (as 0x0030a5c8 does)
    const std::uint32_t sequence = header.sequence;
    if (lan_seq_less(sequence, link.recv_next) || link.recv_buffer.count(sequence) != 0) {
        ++stats_.duplicates_dropped;
        return;
    }
    if (sequence - link.recv_next >= kReceiveWindow) return;  // too far ahead: the sender resends it later
    link.recv_buffer.emplace(sequence, Incoming{header.kind, header.flags, header.recipient, header.origin,
                                                std::vector<std::uint8_t>(payload.begin(), payload.end())});
    // In-order delivery (the ordered read 0x307dd8: only last-consumed + 1 is released).
    for (auto it = link.recv_buffer.find(link.recv_next); it != link.recv_buffer.end();
         it = link.recv_buffer.find(link.recv_next)) {
        Incoming incoming = std::move(it->second);
        link.recv_buffer.erase(it);
        ++link.recv_next;
        handle_message(link, incoming.kind, incoming.flags, incoming.recipient, incoming.origin, 0, incoming.payload,
                       now);
    }
}

void LanTransport::handle_datagram(const LanAddress& from, std::span<const std::uint8_t> datagram,
                                   std::uint64_t now) {
    const auto header = lan_decode_header(datagram);
    if (!header) {
        ++stats_.rejected_datagrams;
        return;
    }
    const auto payload = datagram.subspan(kLanHeaderSize, header->length);
    Link* link = link_for(from);
    if (link == nullptr) {
        if (is_host() && header->kind == LanPacketKind::JoinRequest)
            handle_join_request(from, payload, now);
        else
            ++stats_.rejected_datagrams;
        return;
    }
    ++stats_.datagrams_received;
    link->last_recv_ms = now;

    if (!link->established) {
        // A client waiting for the handshake; anything else from the host is dropped unACKed (resent later).
        if (header->kind == LanPacketKind::JoinAccept && payload.size() == kJoinAcceptSize) {
            LanReader in(payload);
            const std::uint16_t index = in.u16();
            in.u16();  // max clients
            const std::uint64_t session = in.u64();
            const std::uint32_t host_clock = in.u32();
            const std::uint32_t host_first_sequence = in.u32();
            std::string host_name = in.fixed_text(kNameSize);
            if (index == 0 || index >= kLanMaxClients) return;
            link->established = true;
            link->name = host_name;
            link->recv_next = host_first_sequence;
            local_index_ = index;
            session_id_ = session;
            host_clock_offset_ms_ = static_cast<std::int64_t>(host_clock) - static_cast<std::int64_t>(now & 0xffffffffu);
            present_[0] = true;
            names_[0] = std::move(host_name);
            present_[index] = true;
            names_[index] = config_.agent_name;
            unreliable_latest_.fill(std::nullopt);
            state_ = LanTransportState::Connected;
            events_.push_back(Event{Event::Type::Join, 0, LanLeaveReason::Closed, {}});
        } else if (header->kind == LanPacketKind::JoinReject && !payload.empty() &&
                   payload.size() <= 1 + kLanMaxJoinDetail) {
            join_error_ = static_cast<LanJoinError>(payload[0]);
            join_detail_.assign(payload.begin() + 1, payload.end());  // Refused's reason (132md5); else empty
            reset();
            state_ = LanTransportState::Rejected;
        }
        return;
    }

    if (header->kind != LanPacketKind::JoinRequest) handle_ack(*link, header->ack, header->ack_bits);
    switch (header->kind) {
    case LanPacketKind::JoinRequest:
        if (is_host()) send_unsequenced(*link, LanPacketKind::JoinAccept, 0, join_accept_payload(*link), now);
        break;
    case LanPacketKind::Bye:
        if (is_host())
            drop_link(link->index, LanLeaveReason::Closed, now);
        else
            lose_host(LanLeaveReason::Closed);
        break;
    case LanPacketKind::Data:
    case LanPacketKind::PeerJoin:
    case LanPacketKind::PeerLeave:
        if (header->flags & kLanReliableOrdered)
            handle_reliable(*link, *header, payload, now);
        else if (header->kind == LanPacketKind::Data)
            handle_message(*link, header->kind, header->flags, header->recipient, header->origin, header->sequence,
                           payload, now);
        break;
    default:
        break;  // ACK / keepalive, late JOIN_ACK / JOIN_NAK
    }
}

void LanTransport::drop_link(std::uint16_t index, LanLeaveReason reason, std::uint64_t now) {
    links_[index].reset();
    present_[index] = false;
    names_[index].clear();
    extensions_[index].clear();
    unreliable_latest_[index].reset();
    events_.push_back(Event{Event::Type::Leave, index, reason, {}});
    std::vector<std::uint8_t> payload;
    LanWriter out(payload);
    out.u16(index);
    out.u8(static_cast<std::uint8_t>(reason));
    for (auto& link : links_)
        if (link && link->established) send_reliable(*link, LanPacketKind::PeerLeave, 0, link->index, 0, payload, now);
}

void LanTransport::lose_host(LanLeaveReason reason) {
    events_.push_back(Event{Event::Type::Leave, 0, reason, {}});
    for (std::uint16_t client = 1; client < kLanMaxClients; ++client)
        if (present_[client] && client != local_index_)
            events_.push_back(Event{Event::Type::Leave, client, LanLeaveReason::HostLeft, {}});
    reset();
    state_ = LanTransportState::Disconnected;
}

void LanTransport::service_link(Link& link, std::uint64_t now) {
    if (!link.established) {
        if (now - link.join_started_ms >= config_.timeout_ms) {
            join_error_ = LanJoinError::Timeout;
            reset();
            state_ = LanTransportState::Rejected;
        } else if (now - link.last_send_ms >= config_.join_retry_ms) {
            send_unsequenced(link, LanPacketKind::JoinRequest, 0, join_request_payload(), now);
        }
        return;
    }
    if (now - link.last_recv_ms >= config_.timeout_ms) {
        if (is_host())
            drop_link(link.index, LanLeaveReason::Timeout, now);
        else
            lose_host(LanLeaveReason::Timeout);
        return;
    }
    for (Outgoing& outgoing : link.unacked) {
        if (now < outgoing.next_send_ms) continue;
        send_outgoing(link, outgoing, now);
        ++stats_.resends;
    }
    if (link.ack_pending || now - link.last_send_ms >= config_.keepalive_ms)
        send_unsequenced(link, LanPacketKind::Ack, kLanFlagAck, {}, now);
}

void LanTransport::poll(const LanTransportHandlers& handlers) {
    if (socket_) {
        const std::uint64_t now = config_.clock();
        std::array<std::uint8_t, 2048> buffer{};
        LanAddress from;
        while (socket_) {
            const auto size = socket_->receive_from(from, buffer);
            if (!size) break;
            handle_datagram(from, std::span<const std::uint8_t>(buffer.data(), *size), now);
        }
        for (std::uint16_t index = 0; index < kLanMaxClients && socket_; ++index)
            if (Link* link = link_at(index)) service_link(*link, now);
    }
    // Callbacks run after the network work, so a handler may send() or close() freely.
    std::vector<Event> events;
    events.swap(events_);
    for (const Event& event : events) {
        switch (event.type) {
        case Event::Type::Recv:
            if (handlers.on_recv) handlers.on_recv(event.client, event.bytes);
            break;
        case Event::Type::Join:
            if (handlers.on_join) handlers.on_join(event.client);
            break;
        case Event::Type::Leave:
            if (handlers.on_leave) handlers.on_leave(event.client, event.reason);
            break;
        }
    }
}

}  // namespace omega::lan
