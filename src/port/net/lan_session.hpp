#pragma once
// PORT-ONLY (not in the original; LAN phase 1 step 3, checkpoint 132ln3): LanSession, the LAN game session
// behind the stand-in calls of research/LAN_PLAY.md §4 (§5.1 item 1). It owns the LanTransport (the game socket)
// and the LanDiscoveryHost (the advertisement) of a hosted game.
//
// GuiMissionSetup's begin_mission (002a0f60) in LAN mode hosts through host(), in place of the PS2 network start
// (001698c0 / 00165bc0) and the Medius create game (SFOLobby_Games 0x0044e960, §4 row 6b); the GUI then completes
// the lobby callback itself (the event 0x1e with "created"). The navigator's LAN lobby check (§4 row 5) reads
// hosting(). Checkpoint 132ln4: GuiNetJoinMission (JOINMIS, §4 rows 7 / 7b) lists browse() entries (a
// LanDiscoveryBrowser in place of the Medius game list) and joins through join() (LanTransport::connect, JOIN_REQ
// / JOIN_ACK, in place of SFOLobby_Games 0x0044e430 and the DME connect). The game's netcode is phase 3; the
// session interface is kept free of LAN specifics where it can be, so a later online mode can put a server-backed
// session behind the same calls (Decision, "Shared code with future online play").
//
// The build hash (§6 item 1) and the session id (§4 row 3) are defined here: lan_build_hash and
// lan_random_session_id.
#include "lan_discovery.hpp"
#include "lan_transport.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace omega::lan {

// PORT-ONLY: bumped whenever the port changes what peers exchange (the LAN wire formats, the replicated game
// state) without a change to the executable or the disc data. Part of the build hash.
// 2 (checkpoint 132md5): JOIN_REQ's join extension (the mod report) and JOIN_NAK Refused with its reason.
constexpr std::uint32_t kLanPortRevision = 2;

// PORT-ONLY (§6 item 1): the discovery / join build hash. Both sides must run the same executable and the same
// disc data (message type numbers come from registration order, receiver ids from creation order), and the same
// port revision. The port verifies the disc image and its boot executable against fixed SHA-256 values (the
// build's OMEGA_ISO_SHA256 / OMEGA_EXE_SHA256), so those two digests stand for "executable version + disc data
// hashes": the hash is lan_crc32 over "SFOSLAN-BUILD\0", the u16 protocol version and the u32 port revision
// (little-endian), then the two digests as lower-case hex text, each followed by a NUL.
std::uint32_t lan_build_hash(std::string_view iso_sha256, std::string_view executable_sha256,
                             std::uint16_t protocol_version = kLanProtocolVersion,
                             std::uint32_t port_revision = kLanPortRevision);

// PORT-ONLY (§4 row 3): a random 64-bit session id (std::random_device), never 0.
std::uint64_t lan_random_session_id();

struct LanSessionConfig {
    std::uint32_t build_hash = 0;
    std::uint16_t game_port = kLanGamePort;  // 0: an ephemeral port (tests)
    LanDiscoveryConfig discovery;            // its build_hash / protocol_version are set from this config
    LanClock clock = lan_steady_clock();
    LanSocketFactory socket_factory = lan_udp_socket_factory();  // the game socket's
    std::function<std::uint64_t()> session_id = lan_random_session_id;
    // Checkpoint 132md5 (port-only, research/MODDING.md "LAN: the mod report at join"): what a joiner appends to
    // its JOIN_REQ (asked at each join; unset or empty: nothing), and the host's say on each joiner
    // (LanTransportConfig::join_filter; unset: every joiner that fits). lan_mod_gate.hpp makes both from the mods.
    std::function<std::vector<std::uint8_t>()> join_extension;
    LanJoinFilter join_filter;
};

// Checkpoint 132ln4: where a join stands (join_state()).
enum class LanJoinState { Idle, Pending, Accepted, Rejected, Failed };

// What the mission setup screen hosts (the advert's fields, §5.3).
struct LanHostSetup {
    std::string host_name;          // the host agent's name
    std::int32_t mission = -1;      // app +248 (0..8, table 0x4acb30)
    std::int32_t num_agents = 4;    // app +180, MSNSETUP num_agents (2..4)
    std::uint8_t join_filter = 0;   // app +185, MSNSETUP join_filter (0..2)
};

class LanSession {
public:
    explicit LanSession(LanSessionConfig config);
    ~LanSession();
    LanSession(const LanSession&) = delete;
    LanSession& operator=(const LanSession&) = delete;

    // Open the game socket (LanTransport::host) and start the advertisement (LanDiscoveryHost). False, with
    // nothing left open, when either cannot be opened, the setup is out of range or a game is hosted already.
    bool host(const LanHostSetup& setup);
    bool hosting() const { return transport_ != nullptr && hosting_; }
    // Checkpoint 132on2e (online create): the port the next host() binds (0: an ephemeral port).
    void set_game_port(std::uint16_t port) { config_.game_port = port; }
    std::uint16_t game_port() const { return config_.game_port; }
    // LAN lifecycle mapping for 0044e340, before 0042b1f0 leaves the DME game: withdraw a hosted game from
    // discovery and refuse further joins while preserving the game transport for the rest of teardown. A joined
    // client has no LAN advertisement to withdraw. This is not the retail idle publication: the caller handles
    // the original reason-3 stats upload and lobby client-state transitions separately.
    void exit_game();
    // Close the game and stop the advertisement.
    void leave();
    // Serve the discovery sockets (host and browser) and the transport (call once per frame).
    void pump();
    // Checkpoint 132ln5f: the discovery sockets only (the advertisement and the browser), for the level, where
    // LanDme polls the transport itself. Stands in for SFOLobby's update 004536b0 at the lobby keep-alive
    // 002ca580 (the original keeps its Medius lobby connection, and so the game's listing, served every frame).
    void pump_lobby();

    // Checkpoint 132ln4: the browser of JOINMIS. browse(true) starts a LanDiscoveryBrowser (false when its socket
    // cannot be opened), browse(false) stops it; games() lists the entries in first-seen order.
    bool browse(bool on);
    bool browsing() const { return browser_ != nullptr; }
    std::vector<LanBrowseEntry> games() const;
    // Connect to the browsed game of `session_id` and send JOIN_REQ with `agent_name` (truncated to 15). False
    // (nothing opened) while hosting or joining, without a browser, for an unknown session or for a game of
    // another build.
    bool join(std::uint64_t session_id, const std::string& agent_name);
    // Checkpoint 132on2b (online JOINMIS): connect to the game at `host_address` (the address omega_server's
    // JoinGameResponse hands over) and send JOIN_REQ, with no browser; any session id the host accepts with counts.
    // False (nothing opened) while hosting or joining.
    bool join_at(const LanAddress& host_address, const std::string& agent_name);
    // Pending until JOIN_ACK (Accepted), JOIN_NAK (Rejected: Full / InProgress / VersionMismatch / Refused), or no answer
    // within the transport's timeout or the link lost (Failed).
    LanJoinState join_state() const;
    LanJoinError join_error() const { return transport_ ? transport_->join_error() : LanJoinError::None; }
    // A JOIN_NAK Refused's reason text (132md5: the host's mod policy), else empty.
    std::string join_detail() const { return transport_ ? transport_->join_detail() : std::string{}; }
    void cancel_join();
    bool joining() const { return transport_ != nullptr && !hosting_; }
    std::uint64_t clock_ms() const { return config_.clock(); }

    const LanAdvert& advert() const { return advert_; }
    LanTransport* transport() { return transport_.get(); }
    LanDiscoveryHost* discovery() { return discovery_.get(); }
    std::uint64_t session_id() const { return advert_.session_id; }

private:
    LanSessionConfig config_;
    std::unique_ptr<LanTransport> transport_;
    std::unique_ptr<LanDiscoveryHost> discovery_;
    std::unique_ptr<LanDiscoveryBrowser> browser_;
    LanAdvert advert_;
    bool hosting_ = false;
    std::uint64_t joined_session_ = 0;
};

}  // namespace omega::lan
