#pragma once
// PORT-ONLY (not in the original; checkpoint 132on2, research/ONLINE.md §9): the lobby session the game's online
// screens drive through the transport (OnlineClient), standing in for the DME session and the SFOLobby requests
// GuiOnlineConnect makes before the account login. Each call names the original it replaces:
//  * start: 002ca710 (the online start: modules, DNS, the DME session 0042a870) -> TCP connect to omega_server, the
//    PORT-ONLY hello (this build's platform and capabilities) and the Medius SessionBegin (0044c620), whose
//    SessionKey every later request carries.
//  * policy / announcements: the downloads 004456d0 / 004456c0 (GetPolicy "USAGE" / "PRIVACY") and 00445570
//    (GetAnnouncements), each a list of response frames ended by EndOfText / EndOfList.
//  * end: 00429500(0) (the DME session's end) -> disconnect.
// There is no DNAS here: the original's DNAS authentication (0044bab0's eznet / DNAS overlay) needs Sony's
// servers; the port's GuiHost adapter skips it (the labelled DNAS bypass, ONLINE.md §9.2).

#include "online_client.hpp"
#include "online_mod_report.hpp"
#include "online_stats.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace omega::online {

class OnlineLobby {
public:
    struct Options {
        std::string server = "127.0.0.1";  // "host[:port]" (default port kDefaultServerPort)
        Platform platform = Platform::Pc;
        std::uint32_t build = 0;
        int timeout_ms = 5000;
        // PORT-ONLY (checkpoint 132md4, ONLINE.md §6.9): the mod report start() sends after the hello (ext ModReport,
        // to omega_server only); unset: none sent.
        std::function<ModReport()> mod_report;
        // The capabilities the hello announces (cap::*). 132on2e: online_relay=off leaves out cap::Relay.
        std::uint32_t caps = cap::All;
        // Tests (132on2f): false sends no hello and no mod report, so the server keeps an original-kind session (the
        // PS2's message set, no extensions).
        bool hello = true;
    };
    explicit OnlineLobby(Options options) : options_(std::move(options)) {}

    const Options& options() const { return options_; }
    // 002ca710's stand-in: connect, hello and SessionBegin. False (and last_error) on any failure.
    bool start();
    void end();  // 00429500(0)
    bool session() const { return started_ && client_.connected(); }
    const std::string& session_key() const { return session_key_; }
    const std::string& last_error() const { return error_; }

    // 004456d0 (privacy false) / 004456c0 (privacy true): the policy text's chunks (GetPolicyResponse +1c, 256
    // bytes each) until EndOfText (+11c). A nonzero status ends the text with nothing appended (004473c0). Returns
    // whether the request completed (EndOfText arrived).
    bool policy(bool privacy, std::vector<std::string>& chunks);
    // 00445570: {id +1c, text +20} rows until EndOfList (+408); rows whose text starts with "{SFOLOBBY_SETTINGS}"
    // go to 00443bf0 in the original and are not listed (kept in settings_rows()).
    bool announcements(std::vector<std::pair<std::int32_t, std::string>>& rows);
    const std::vector<std::string>& settings_rows() const { return settings_rows_; }

    // Checkpoint 132on2b. Each answers the response's StatusCode (+18), or nullopt when none arrived in time.
    // 00445420 (AccountLogin 7 -> 8; the account id at +1c is kept).
    std::optional<std::int32_t> login(const std::string& name, const std::string& password);
    // 00445b90's AccountRegistration (9 -> 10).
    std::optional<std::int32_t> register_account(const std::string& name, const std::string& password);
    // 004452f0 (AccountLogout 21 -> 22).
    std::optional<std::int32_t> logout();
    // Checkpoint 132on2d (ONLINE.md §3.6): the agent statistics record.
    // 004505e0 (AccountUpdateStats 17 -> 18): the record uploaded (the login's tail 0044ce60, each game entry).
    std::optional<std::int32_t> account_update_stats(std::span<const std::uint8_t> blob);
    // 00451200 (GetAccountStats 49 -> 50): an account's record; on status 0 it is cached (00450880) and copied to out.
    std::optional<std::int32_t> get_account_stats(std::uint32_t account, StatsBlob* out = nullptr);
    // 004446e0 (PlayerReport 1; no response): the in-game record, with the world id.
    void send_player_report(std::int32_t world, std::span<const std::uint8_t> blob);
    // 00444944 (WorldReport 0; no response): the host's world.
    void send_world_report(const WorldReportFields& w);
    // The record cache (session +fc, 00450880 / 00450cf0): JOINMIS' rank column (002ab0f8) and the agent info.
    struct CachedStats {
        std::string name;
        StatsBlob blob{};
        std::chrono::steady_clock::time_point when{};
    };
    const CachedStats* cached_stats(std::uint32_t account) const;
    bool logged_in() const { return session() && account_id_ != 0; }
    std::uint32_t account_id() const { return account_id_; }
    const std::string& account_name() const { return account_name_; }  // the login's name (lobby +243d)

    // Checkpoint 132on2b stages B3 / B4: the game requests. send_* sends; its response is the caller's to poll
    // (client().wait_for(id, 0)), as the original's arrive through the lobby's update 004536b0.
    void send_create_game(const std::string& game_name);  // 0044e960 -> 004524c0 (CreateGame 253 -> 34)
    void send_game_list();                                 // 00450510 (GameList 41 -> 42)
    // 0044e430 (JoinGame 35 -> 36). PORT-ONLY (132on2c): `password` in the request's GamePassword (+0x30), which the
    // original leaves zero; omega_server checks it against a password-protected game's.
    void send_join_game(std::uint32_t world, const std::string& password = {});
    // Checkpoint 132on2c: 00451050's TextFilter 152 (and ChatMessage 59 when `target` is not the account) and
    // 004516e0's PlayerList 220 (the room's world id; omega_server has none and answers every logged-in player).
    void send_chat(const std::string& text, std::int32_t target);
    void send_player_list(std::int32_t world);
    // Checkpoint 132on2h (CONTACTS, ONLINE.md §3.7): 0044d860's GetBuddyList_ExtraInfo 214 (its 215 rows are the
    // caller's to poll), 0044db60's AddToBuddyListConfirmation 258 (its 68 too), 0044d9c0's RemoveFromBuddyList
    // 65 -> 66 (the status, waiting up to the timeout) and 00451370's AccountGetID 23 -> 24 (the response).
    void send_buddy_list();
    void send_buddy_add(std::int32_t account);
    std::optional<std::int32_t> remove_contact(std::int32_t account);
    std::optional<Frame> account_get_id(const std::string& name);
    int timeout_ms() const { return options_.timeout_ms; }
    // PORT-ONLY (cap::GamePassword, 132on2c): the created game's join password (empty: none).
    void game_password(std::uint32_t world, const std::string& password);
    // 0044e060 -> 0044e0b4 (GameInfo 51 -> 52): the response, waiting up to the timeout. A response to
    // send_game_info_fetch (MessageID "1") received meanwhile is kept for take_game_info_fetches.
    std::optional<Frame> game_info(std::uint32_t world);
    // Checkpoint 132on2i: 0044ebd0's GameInfo (MessageID "1" 004c0ef0, the callback 00446ae0), not waited for; its
    // responses (whatever their status) are the caller's from take_game_info_fetches.
    void send_game_info_fetch(std::uint32_t world);
    std::vector<Frame> take_game_info_fetches();
    // PORT-ONLY (cap::HostEndpoint; never sent by, nor to, an original PS2 session): the hosted game's in-level UDP
    // port, and the game left. 132on2e: when the host's router forwards it (UPnP), the external port and address
    // follow (optional trailing fields; omega_server hands joiners the external port when it sees the host's public
    // address).
    void host_endpoint(std::uint32_t world, std::uint16_t port, std::uint16_t external_port = 0,
                       const std::string& external_ip = {});
    void leave_game();

    OnlineClient& client() { return client_; }
    const OnlineClient& client() const { return client_; }

private:
    void cache_stats(std::uint32_t account, const std::string& name, std::span<const std::uint8_t> blob);

    Options options_;
    OnlineClient client_;
    bool started_ = false;
    std::string session_key_;
    std::string error_;
    std::vector<std::string> settings_rows_;
    std::uint32_t message_serial_ = 0;
    std::uint32_t account_id_ = 0;
    std::string account_name_;
    std::map<std::uint32_t, CachedStats> stats_cache_;
    std::vector<Frame> fetched_game_infos_;  // 132on2i: send_game_info_fetch's responses
};

}  // namespace omega::online
