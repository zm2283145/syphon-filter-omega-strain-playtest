#pragma once
// PORT-ONLY (not in the original; checkpoint 132on1, research/ONLINE.md §5.4): the client end of the omega_server
// transport. OnlineClient is the plug-in point the MCL API call sites send through (each API function's request
// struct goes out as a frame of its Medius id; the responses come back as frames and go to the API's callback slot),
// and it keeps the PORT-ONLY extension state: the negotiated capabilities, the server's cheat permission, the roster
// (each player's platform, for the badges) and the notices (ban reason, kick, broadcast).
//
// The `request` namespace builds requests in the decoded layouts for the omega_server demo / admin tool and the
// tests. It is a TEST CLIENT, not ported game code: the game fills these structs itself (the addresses are named
// next to each builder).

#include "online_protocol.hpp"
#include "online_relay.hpp"
#include "online_tcp.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace omega::online {

class OnlineClient {
public:
    // "host[:port]" (default port kDefaultServerPort). False with *error on failure.
    bool connect(const std::string& host_port, std::string* error, int timeout_ms = 3000);
    void disconnect();
    bool connected() const { return tcp_.connected(); }

    // PORT-ONLY hello: this client's platform, build number and the capabilities it understands (cap::*).
    void hello(Platform platform, std::uint32_t build, std::uint32_t caps = cap::All);
    void send_medius(std::uint16_t id, std::span<const std::uint8_t> body);
    void send_ext(std::uint16_t type, std::span<const std::uint8_t> body);

    // Reads what arrived (waiting up to timeout_ms): extension frames update the state below, Medius frames queue.
    void pump(int timeout_ms);
    // The next queued Medius frame of `id`, pumping until it arrives or timeout_ms passes.
    std::optional<Frame> wait_for(std::uint16_t id, int timeout_ms);
    // Every frame of `id` up to and including the one whose byte `end_offset` is nonzero (EndOfList / EndOfText).
    std::vector<Frame> wait_for_list(std::uint16_t id, std::size_t end_offset, int timeout_ms);
    // Removes and returns every queued Medius frame (any id).
    std::vector<Frame> take_queued();

    bool welcomed() const { return welcomed_; }
    std::uint32_t caps() const { return caps_; }  // negotiated (the Welcome's)
    bool cheats_allowed() const { return cheats_allowed_; }
    const std::string& server_name() const { return server_name_; }
    const std::vector<RosterEntry>& roster() const { return roster_; }
    Platform platform_of(std::uint32_t account_id) const;  // Unknown when not in the roster
    struct Notice {
        std::uint32_t code = 0;
        std::string text;
    };
    std::vector<Notice> take_notices();
    std::optional<std::string> wait_admin_reply(int timeout_ms);
    std::size_t ext_frames_received() const { return ext_frames_; }
    // PORT-ONLY (132on2e, online_relay.hpp): the server's address (host order, from connect), the relay ticket of
    // this login (ext::RelayTicket) and the relay address of each joined world's host (ext::RelayPeer, by world).
    std::uint32_t server_ip() const { return server_ip_; }
    const std::optional<RelayTicket>& relay_ticket() const { return relay_ticket_; }
    std::optional<RelayPeer> relay_peer(std::uint32_t world) const;
    // PORT-ONLY (132on2g, cap::Social): the recent players list (ext::RecentPlayersList, the answer to
    // ext::RecentPlayers), newest first; nullopt until one arrives (each taken once).
    struct RecentPlayer {
        std::uint32_t account = 0;
        std::string name;
        Platform platform = Platform::Unknown;
        bool online = false;
        std::uint32_t seconds_ago = 0;
    };
    std::optional<std::vector<RecentPlayer>> take_recent_players() { return std::exchange(recent_players_, std::nullopt); }

private:
    void handle(const Frame& f);

    TcpClient tcp_;
    std::deque<Frame> queued_;
    bool welcomed_ = false;
    std::uint32_t caps_ = 0;
    bool cheats_allowed_ = false;
    std::string server_name_;
    std::vector<RosterEntry> roster_;
    std::vector<Notice> notices_;
    std::deque<std::string> admin_replies_;
    std::size_t ext_frames_ = 0;
    std::uint32_t server_ip_ = 0;
    std::optional<RelayTicket> relay_ticket_;
    std::map<std::uint32_t, RelayPeer> relay_peers_;
    std::optional<std::vector<RecentPlayer>> recent_players_;
};

// PORT-ONLY test client: requests in the decoded layouts (ONLINE.md §4). message_id is echoed by the server.
namespace request {
std::vector<std::uint8_t> session_begin(std::string_view message_id);                       // 0x0044c620
std::vector<std::uint8_t> registration(std::string_view session_key, std::string_view name,
                                       std::string_view password);                          // 0x00445b90
std::vector<std::uint8_t> login(std::string_view session_key, std::string_view name, std::string_view password);  // 0x00445420
std::vector<std::uint8_t> logout(std::string_view session_key);                             // 0x004452f0
std::vector<std::uint8_t> policy(std::string_view session_key, bool privacy);               // 0x004456e0 ("USAGE"/"PRIVACY")
std::vector<std::uint8_t> announcements(std::string_view session_key);                      // 0x00445570
std::vector<std::uint8_t> account_update_stats(std::string_view session_key,
                                               std::span<const std::uint8_t> stats);         // 0x004505e0
std::vector<std::uint8_t> get_account_stats(std::string_view session_key, std::int32_t account,
                                            std::string_view message_id = "1");             // 0x00451200
std::vector<std::uint8_t> player_report(std::string_view session_key, std::int32_t world,
                                        std::span<const std::uint8_t> stats);               // 0x004446e0
std::vector<std::uint8_t> world_report(std::string_view session_key, const WorldReportFields& w);  // 0x00444944
std::vector<std::uint8_t> create_game(std::string_view session_key, std::string_view game_name);  // 0x004524c0
std::vector<std::uint8_t> game_list(std::string_view session_key);                          // 0x00450510
std::vector<std::uint8_t> game_info(std::string_view session_key, std::uint32_t world);     // 0x0044e0b4
std::vector<std::uint8_t> join_game(std::string_view session_key, std::uint32_t world, std::string_view password = {});  // 0x0044e430
std::vector<std::uint8_t> chat(std::string_view session_key, std::string_view text, std::int32_t whisper_to = 0);  // 0x00451050
std::vector<std::uint8_t> text_filter(std::string_view session_key, std::string_view text, std::int32_t target);  // 0x00451050
std::vector<std::uint8_t> player_list(std::int32_t world);                                    // 0x004516e0
// 132on2g (ONLINE.md §3.7): the contact ("buddy") messages of SFOLobby_Contact.cc.
std::vector<std::uint8_t> buddy_list(std::string_view message_id = "1");                      // 0x0044d860 (214)
std::vector<std::uint8_t> buddy_invitations(std::string_view message_id = "1");               // 0x0044d780 (261)
std::vector<std::uint8_t> buddy_add(std::string_view session_key, std::int32_t account);      // 0x0044db60 (258)
std::vector<std::uint8_t> buddy_answer(std::string_view session_key, std::int32_t inviter, bool accept);  // 0x00446560 (260)
std::vector<std::uint8_t> buddy_remove(std::string_view session_key, std::int32_t account);   // 0x0044d9c0 (65)
std::vector<std::uint8_t> account_get_id(std::string_view session_key, std::string_view name);  // 0x00451370 (23)
}  // namespace request

// The decoded fields of a NetConnectionInfo (login +0x28, join +0x20).
struct ConnectInfo {
    std::string ip;
    std::uint16_t port = 0;
    std::uint32_t world = 0;
    std::string session_key;
};
ConnectInfo read_connect_info(std::span<const std::uint8_t> s, std::size_t base);

}  // namespace omega::online
