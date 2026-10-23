// PORT-ONLY (not in the original; checkpoint 132ln): LanDiscovery, the UDP broadcast lobby standing in for the
// Medius game list (SFOLobby_Games 0x0044e060 / 0x0044e960, SFOLobby_Lobby 0x00450cf0 / 0x00450590).
// See lan_discovery.hpp for the wire formats and research/LAN_PLAY.md §5.3.
#include "lan_discovery.hpp"

#include "lan_wire.hpp"

#include <algorithm>
#include <utility>

namespace omega::lan {

namespace {

constexpr std::array<std::uint8_t, 8> kAdvertMagic{'S', 'F', 'O', 'S', 'L', 'A', 'N', 0};
constexpr std::array<std::uint8_t, 8> kDiscoverMagic{'S', 'F', 'O', 'S', 'D', 'S', 'C', 0};

void append_crc(std::vector<std::uint8_t>& bytes) {
    const std::uint32_t crc = lan_crc32(bytes);
    LanWriter(bytes).u32(crc);
}

bool check_frame(std::span<const std::uint8_t> bytes, std::size_t size, const std::array<std::uint8_t, 8>& magic) {
    if (bytes.size() != size) return false;
    if (!std::equal(magic.begin(), magic.end(), bytes.begin())) return false;
    LanReader crc_reader(bytes.subspan(size - 4));
    return crc_reader.u32() == lan_crc32(bytes.first(size - 4));
}

}  // namespace

std::vector<std::uint8_t> lan_encode_advert(const LanAdvert& advert) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kLanAdvertSize);
    LanWriter out(bytes);
    out.bytes(kAdvertMagic);
    out.u16(advert.protocol_version);
    out.u32(advert.build_hash);
    out.u64(advert.session_id);
    out.u16(advert.game_port);
    out.fixed_text(advert.host_name, kLanHostNameSize);
    out.u8(advert.mission_index);
    out.u8(advert.mission_type);
    out.u8(advert.agents_now);
    out.u8(advert.agents_max);
    out.u8(advert.join_filter);
    out.u8(static_cast<std::uint8_t>(advert.state));
    append_crc(bytes);
    return bytes;
}

std::optional<LanAdvert> lan_decode_advert(std::span<const std::uint8_t> bytes) {
    if (!check_frame(bytes, kLanAdvertSize, kAdvertMagic)) return std::nullopt;
    LanReader in(bytes.subspan(8));
    LanAdvert advert;
    advert.protocol_version = in.u16();
    advert.build_hash = in.u32();
    advert.session_id = in.u64();
    advert.game_port = in.u16();
    advert.host_name = in.fixed_text(kLanHostNameSize);
    advert.mission_index = in.u8();
    advert.mission_type = in.u8();
    advert.agents_now = in.u8();
    advert.agents_max = in.u8();
    advert.join_filter = in.u8();
    advert.state = static_cast<LanSessionState>(in.u8());
    if (in.failed()) return std::nullopt;
    return advert;
}

std::vector<std::uint8_t> lan_encode_discover(const LanDiscover& discover) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(kLanDiscoverSize);
    LanWriter out(bytes);
    out.bytes(kDiscoverMagic);
    out.u16(discover.protocol_version);
    out.u32(discover.build_hash);
    append_crc(bytes);
    return bytes;
}

std::optional<LanDiscover> lan_decode_discover(std::span<const std::uint8_t> bytes) {
    if (!check_frame(bytes, kLanDiscoverSize, kDiscoverMagic)) return std::nullopt;
    LanReader in(bytes.subspan(8));
    LanDiscover discover;
    discover.protocol_version = in.u16();
    discover.build_hash = in.u32();
    if (in.failed()) return std::nullopt;
    return discover;
}

// ---- LanBrowseCache ------------------------------------------------------------------------------------------

void LanBrowseCache::observe(std::uint32_t sender_ip, const LanAdvert& advert, std::uint64_t now_ms) {
    const bool mismatch = advert.protocol_version != protocol_version_ || advert.build_hash != build_hash_;
    for (LanBrowseEntry& entry : entries_) {
        if (entry.game_address.ip == sender_ip && entry.advert.session_id == advert.session_id) {
            entry.advert = advert;
            entry.game_address.port = advert.game_port;
            entry.last_seen_ms = now_ms;
            entry.version_mismatch = mismatch;
            return;
        }
    }
    entries_.push_back(LanBrowseEntry{LanAddress{sender_ip, advert.game_port}, advert, now_ms, now_ms, mismatch});
}

void LanBrowseCache::expire(std::uint64_t now_ms) {
    std::erase_if(entries_, [&](const LanBrowseEntry& entry) {
        return now_ms >= entry.last_seen_ms && now_ms - entry.last_seen_ms >= expiry_ms_;
    });
}

// ---- LanDiscoveryHost ----------------------------------------------------------------------------------------

LanDiscoveryHost::LanDiscoveryHost(LanDiscoveryConfig config) : config_(std::move(config)) {}
LanDiscoveryHost::~LanDiscoveryHost() { stop(); }

bool LanDiscoveryHost::start(const LanAdvert& advert) {
    stop();
    auto socket = config_.socket_factory();
    if (!socket || !socket->open(config_.discovery_port, LanSocketOptions{true, true})) return false;
    socket_ = std::move(socket);
    update(advert);
    return true;
}

void LanDiscoveryHost::update(const LanAdvert& advert) {
    advert_ = advert;
    advert_.protocol_version = config_.protocol_version;
    advert_.build_hash = config_.build_hash;
    if (socket_) beacon(config_.clock());
}

void LanDiscoveryHost::stop() {
    if (socket_) socket_->close();
    socket_.reset();
}

std::uint16_t LanDiscoveryHost::local_port() const { return socket_ ? socket_->local_port() : 0; }

void LanDiscoveryHost::beacon(std::uint64_t now) {
    last_beacon_ms_ = now;
    if (!config_.send_beacons) return;
    const auto bytes = lan_encode_advert(advert_);
    // The beacon goes to the configured discovery port (a host bound to an ephemeral test port still beacons
    // to the port browsers listen on).
    const std::uint16_t port = config_.discovery_port != 0 ? config_.discovery_port : kLanDiscoveryPort;
    for (const LanAddress& target : socket_->broadcast_targets(port)) socket_->send_to(target, bytes);
    ++beacons_;
}

void LanDiscoveryHost::pump() {
    if (!socket_) return;
    const std::uint64_t now = config_.clock();
    std::array<std::uint8_t, 1536> buffer{};
    LanAddress from;
    while (const auto size = socket_->receive_from(from, buffer)) {
        // Only DISCOVER is answered; adverts (our own looped-back beacon, other hosts) are ignored.
        if (!lan_decode_discover(std::span<const std::uint8_t>(buffer.data(), *size))) continue;
        socket_->send_to(from, lan_encode_advert(advert_));
        ++replies_;
    }
    if (now - last_beacon_ms_ >= config_.beacon_interval_ms) beacon(now);
}

// ---- LanDiscoveryBrowser -------------------------------------------------------------------------------------

LanDiscoveryBrowser::LanDiscoveryBrowser(LanDiscoveryConfig config)
    : config_(std::move(config)), cache_(config_.protocol_version, config_.build_hash, config_.expiry_ms) {}
LanDiscoveryBrowser::~LanDiscoveryBrowser() { stop(); }

bool LanDiscoveryBrowser::start() {
    stop();
    auto query = config_.socket_factory();
    if (!query || !query->open(0, LanSocketOptions{true, false})) return false;
    query_ = std::move(query);
    if (config_.listen_for_beacons && config_.discovery_port != 0) {
        auto listen = config_.socket_factory();
        if (listen && listen->open(config_.discovery_port, LanSocketOptions{true, true})) listen_ = std::move(listen);
    }
    last_discover_ms_.reset();
    return true;
}

void LanDiscoveryBrowser::stop() {
    if (query_) query_->close();
    if (listen_) listen_->close();
    query_.reset();
    listen_.reset();
    cache_.clear();
}

void LanDiscoveryBrowser::drain(LanSocket& socket, std::uint64_t now) {
    std::array<std::uint8_t, 1536> buffer{};
    LanAddress from;
    while (const auto size = socket.receive_from(from, buffer)) {
        const std::span<const std::uint8_t> bytes(buffer.data(), *size);
        if (const auto advert = lan_decode_advert(bytes)) {
            cache_.observe(from.ip, *advert, now);
        } else if (!lan_decode_discover(bytes)) {
            ++rejected_;  // neither format: bad size, magic or CRC
        }
    }
}

void LanDiscoveryBrowser::pump() {
    if (!query_) return;
    const std::uint64_t now = config_.clock();
    if (!last_discover_ms_ || now - *last_discover_ms_ >= config_.discover_interval_ms) {
        const auto bytes = lan_encode_discover(LanDiscover{config_.protocol_version, config_.build_hash});
        if (config_.use_broadcast && config_.discovery_port != 0)
            for (const LanAddress& target : query_->broadcast_targets(config_.discovery_port))
                query_->send_to(target, bytes);
        for (const LanAddress& target : config_.extra_targets) query_->send_to(target, bytes);
        last_discover_ms_ = now;
        ++discovers_;
    }
    drain(*query_, now);
    if (listen_) drain(*listen_, now);
    cache_.expire(now);
}

}  // namespace omega::lan
