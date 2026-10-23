#pragma once
// PORT-ONLY (not in the original; LAN phase 2 groundwork, checkpoint 132ln): LanDiscovery, the UDP broadcast
// lobby that replaces the Medius game list in LAN mode. research/LAN_PLAY.md "Decision" (LAN needs no server),
// §5.1 item 2, §5.3, §6 item 1 (build hash), §9 item 7 (blocked broadcast: manual targets).
//
// It stands in for the Medius game-list calls the JOINMIS / GuiMissionList code makes (SFOLobby_Games list
// 0x0044e060, SFOLobby_Lobby list/refresh 0x00450cf0 / 0x00450590 / 0x00451050) and, on the host, for the
// Medius create-game advertisement (SFOLobby_Games 0x0044e960). Nothing here is wired into the game yet; the
// LanSession of the next phase owns a LanDiscoveryHost (MSNSETUP begin_mission, 002a0f60) or a
// LanDiscoveryBrowser (JOINMIS, 002a9240).
//
// Timing (§5.3): the browser sends DISCOVER every 1 s to the broadcast targets; the host replies unicast and
// also beacons every 2 s; a browse entry expires 6 s after it was last heard. Time comes only from the
// injected LanClock, and work happens only in pump(), so tests step it deterministically.
//
// ---- Wire formats (all little-endian, no padding) ----------------------------------------------------------
//
// LAN advertisement (host -> browser, unicast reply or broadcast beacon), 50 bytes:
//   off size field
//    0    8 magic            "SFOSLAN\0"
//    8    2 protocol_version kLanProtocolVersion; a different value is listed as "version mismatch"
//   10    4 build_hash       must equal the browser's exactly (§6 item 1), else "version mismatch"
//   14    8 session_id       random per hosted session (§4 row 3)
//   22    2 game_port        the LanTransport port (default 47525)
//   24   16 host_name        char[16], the host agent's name, NUL padded (encoder truncates to 15)
//   40    1 mission_index    app+0x248, the command-centre cursor (table 0x4acb30)
//   41    1 mission_type     MSNSETUP mission_type
//   42    1 agents_now
//   43    1 agents_max       MSNSETUP num_agents (2-4)
//   44    1 join_filter      MSNSETUP join_filter
//   45    1 state            0 open, 1 in progress, 2 full
//   46    4 crc32            lan_crc32 of bytes 0..45
//
// DISCOVER (browser -> broadcast / manual host), 18 bytes:
//    0    8 magic            "SFOSDSC\0"
//    8    2 protocol_version
//   10    4 build_hash
//   14    4 crc32            lan_crc32 of bytes 0..13
//
// Decoders reject a wrong size, a wrong magic and a wrong CRC. They do not reject other protocol versions or
// build hashes: the browser lists those greyed out ("version mismatch", §5.3) and the host still answers them.

#include "lan_socket.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace omega::lan {

constexpr std::uint16_t kLanProtocolVersion = 1;
constexpr std::uint16_t kLanDiscoveryPort = 47524;
constexpr std::uint16_t kLanGamePort = 47525;
constexpr std::size_t kLanAdvertSize = 50;
constexpr std::size_t kLanDiscoverSize = 18;
constexpr std::size_t kLanHostNameSize = 16;

enum class LanSessionState : std::uint8_t { Open = 0, InProgress = 1, Full = 2 };

// PORT-ONLY: the advertisement fields (one JOINMIS mission_list row).
struct LanAdvert {
    std::uint16_t protocol_version = kLanProtocolVersion;
    std::uint32_t build_hash = 0;
    std::uint64_t session_id = 0;
    std::uint16_t game_port = kLanGamePort;
    std::string host_name;
    std::uint8_t mission_index = 0;
    std::uint8_t mission_type = 0;
    std::uint8_t agents_now = 1;
    std::uint8_t agents_max = 4;
    std::uint8_t join_filter = 0;
    LanSessionState state = LanSessionState::Open;
    friend bool operator==(const LanAdvert&, const LanAdvert&) = default;
};

struct LanDiscover {
    std::uint16_t protocol_version = kLanProtocolVersion;
    std::uint32_t build_hash = 0;
    friend bool operator==(const LanDiscover&, const LanDiscover&) = default;
};

std::vector<std::uint8_t> lan_encode_advert(const LanAdvert& advert);
std::optional<LanAdvert> lan_decode_advert(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> lan_encode_discover(const LanDiscover& discover);
std::optional<LanDiscover> lan_decode_discover(std::span<const std::uint8_t> bytes);

struct LanDiscoveryConfig {
    std::uint16_t discovery_port = kLanDiscoveryPort;  // ONLIPREF LAN option (§4 row 4c); 0 = ephemeral (tests)
    std::uint16_t protocol_version = kLanProtocolVersion;
    std::uint32_t build_hash = 0;
    std::uint64_t discover_interval_ms = 1000;
    std::uint64_t beacon_interval_ms = 2000;
    std::uint64_t expiry_ms = 6000;
    // Host: broadcast beacons (off: answer DISCOVER only, e.g. a test that must not broadcast on the LAN).
    bool send_beacons = true;
    // Browser: also bind the discovery port to hear host beacons. Off, only replies to DISCOVER arrive.
    bool listen_for_beacons = true;
    // Browser: extra DISCOVER targets (the manual "Join by IP" fallback, §5.3), besides the broadcasts.
    std::vector<LanAddress> extra_targets;
    // Browser: send to the socket's broadcast targets (255.255.255.255 and each directed broadcast).
    bool use_broadcast = true;
    LanClock clock = lan_steady_clock();
    LanSocketFactory socket_factory = lan_udp_socket_factory();
};

// PORT-ONLY: one discovered host.
struct LanBrowseEntry {
    LanAddress game_address;  // sender ip, advertised game_port
    LanAdvert advert;
    std::uint64_t first_seen_ms = 0;
    std::uint64_t last_seen_ms = 0;
    bool version_mismatch = false;  // protocol version or build hash differ: listed greyed out (§5.3)
};

// PORT-ONLY: the browse list with expiry, keyed by (sender ip, session id). No sockets: the browser feeds it.
class LanBrowseCache {
public:
    LanBrowseCache(std::uint16_t protocol_version, std::uint32_t build_hash, std::uint64_t expiry_ms)
        : protocol_version_(protocol_version), build_hash_(build_hash), expiry_ms_(expiry_ms) {}
    void observe(std::uint32_t sender_ip, const LanAdvert& advert, std::uint64_t now_ms);
    // Drop entries not heard for expiry_ms or longer.
    void expire(std::uint64_t now_ms);
    // In first-seen order (a stable JOINMIS row order).
    const std::vector<LanBrowseEntry>& entries() const { return entries_; }
    void clear() { entries_.clear(); }

private:
    std::uint16_t protocol_version_;
    std::uint32_t build_hash_;
    std::uint64_t expiry_ms_;
    std::vector<LanBrowseEntry> entries_;
};

// PORT-ONLY: host side. Answers DISCOVER with a unicast advertisement and beacons every beacon_interval_ms.
// Stands in for the Medius game creation's listing (SFOLobby_Games 0x0044e960, §4 row 6b).
class LanDiscoveryHost {
public:
    explicit LanDiscoveryHost(LanDiscoveryConfig config);
    ~LanDiscoveryHost();
    // Open the discovery socket and beacon at once. The advert's protocol_version / build_hash are taken
    // from the config. False when the socket cannot be bound.
    bool start(const LanAdvert& advert);
    void update(const LanAdvert& advert);  // e.g. agents_now / state changes; beacons at once
    void stop();
    bool running() const { return socket_ != nullptr; }
    std::uint16_t local_port() const;
    void pump();
    std::uint64_t replies_sent() const { return replies_; }
    std::uint64_t beacons_sent() const { return beacons_; }

private:
    void beacon(std::uint64_t now);
    LanDiscoveryConfig config_;
    std::unique_ptr<LanSocket> socket_;
    LanAdvert advert_;
    std::uint64_t last_beacon_ms_ = 0;
    std::uint64_t replies_ = 0;
    std::uint64_t beacons_ = 0;
};

// PORT-ONLY: client side (JOINMIS open). Sends DISCOVER every discover_interval_ms from its own ephemeral
// socket (so a host on the same machine answers to a unique port) and, with listen_for_beacons, also hears
// beacons on the shared discovery port.
class LanDiscoveryBrowser {
public:
    explicit LanDiscoveryBrowser(LanDiscoveryConfig config);
    ~LanDiscoveryBrowser();
    // False when not even the query socket can be opened. A failed beacon-port bind is not an error.
    bool start();
    void stop();
    bool running() const { return query_ != nullptr; }
    bool hears_beacons() const { return listen_ != nullptr; }
    void pump();
    const std::vector<LanBrowseEntry>& entries() const { return cache_.entries(); }
    std::uint64_t discovers_sent() const { return discovers_; }
    std::uint64_t rejected_packets() const { return rejected_; }

private:
    void drain(LanSocket& socket, std::uint64_t now);
    LanDiscoveryConfig config_;
    LanBrowseCache cache_;
    std::unique_ptr<LanSocket> query_;
    std::unique_ptr<LanSocket> listen_;
    std::optional<std::uint64_t> last_discover_ms_;
    std::uint64_t discovers_ = 0;
    std::uint64_t rejected_ = 0;
};

}  // namespace omega::lan
