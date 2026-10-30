#pragma once
// PORT-ONLY (not in the original; checkpoint 132on2, research/ONLINE.md §9): the online lobby the front end's
// GuiOnlineConnect (ONLINECN) drives, on omega_server through online::OnlineLobby. It answers the SFOLobby calls the
// screen makes before the account login (FrontendLobby, the GuiHost lobby_* calls) the way the original's would
// against a PS2 network configuration and Sony's servers, with these PORT-ONLY substitutions, each labelled:
//  * the network configuration list (0044c440, the memory card's eznet configurations) has one row naming the
//    server ("omega_server HOST:PORT", the default); the PC has no PS2 network configuration;
//  * the connect (0044bab0: eznet up, then the DNAS authentication) succeeds at once with no DNAS event 0x2002:
//    the DNAS BYPASS (DNAS needs Sony's retired servers), so 0029daf0 goes straight on to the MUIS step 0029d390;
//  * MUIS (00442f40 .. 00442e90) lists one server named as the original's default ("SYPHONFILTER ONLINE QA",
//    0048c138), so 0029d390 picks it with no MUIS_SKIP setting, and one universe, the server, as the default;
//  * lobby_region_choice answers false: with one universe there is nothing for ONLIPREF to choose on the way in;
//    ONLIPREF itself (132on2c, from ACNTLOGN's Circle and JOINMIS' online_prefs) lists that one universe;
//  * the policy and the announcements are the server's (GetPolicy / GetAnnouncements over the transport);
//  * (132on2b) the registration's text filter 00443100 (Medius TextFilter on the name and the password) is skipped:
//    omega_server has no TextFilter service, so error 3 (vulgarity) / 0x19 (filter failed) never arise;
//  * (132on2b) the mission channel 00442510 ("%s_%d", the mission's name and universe) is not joined: omega_server
//    has one channel; the call answers success while logged in, mission in range;
//  * (132on2b) the login's MAS -> MLS move (0044ce60: a reconnect to the lobby server) is not made: omega_server is
//    both, and the session key stays valid after the login.
// Stages B3 / B4 (GuiMissionSetup / GuiNetJoinMission online, ONLINE.md §9.4 / §9.5):
//  * (132on2b) the game room 004518f0 (Medius "game world" lists, kept by 004515f0) is the lobby state 5 only:
//    omega_server has no rooms; the game list (GameList) is the server's, requested about once a second while the
//    room's list kind is 1 (the games), in place of the room's list updates;
//  * (132on2b) the main channel's leave 004426b0 sends nothing: omega_server's channel membership (chat) stays;
//  * (132on2b) the create's check 004441d0 (the DME / network state) stands as: logged in, not in a game (state 6
//    / 10), and no LAN game open or joining; failing it is error 0x31, as the original's. 0044e960 itself does not
//    test the lobby state: MSNSETUP opened from the channel (state 4) only leaves it (002a18ec, state 9), and the
//    create runs there;
//  * (132on2b) the in-level game is the port's LAN game (lan::LanSession, research/LAN_PLAY.md): the create hosts
//    it (LanSession::host, no beacons) before CreateGame and, when the response succeeds, tells omega_server its
//    UDP port (ext::HostEndpoint, PORT-ONLY, port clients only); the join connects to the address the
//    JoinGameResponse names (LanSession::join_at), in place of the DME connect 0042a960, and the callback is the
//    LAN join's result (JOIN_ACK: done; JOIN_NAK: F_TO_JOINGAME; no answer: F_TO_CONNECT). Leaving sends
//    ext::LeaveGame (PORT-ONLY);
//  * (132on2b) a failed LAN host (the create could not open its game) answers 0 with error 0x32 and queues the
//    callback {0, F_TO_CREATEGAME}, so the waiting screen gets its failure (the original's 0x32 is a send failure).
// Stage C2 (JOINMIS' chat, player list and the game password, ONLINE.md §9.5 / §9.8):
//  * (132on2c) the room's player list 004516e0 (PlayerList of the room's world, about once a second in state 5)
//    asks for world 0: omega_server has no rooms and lists every logged-in player (00447a30's mapping unchanged);
//  * (132on2c) the room's chat list (+23ec, filled by 00447bb0 / 00447c70 only while a room is registered) is the
//    lobby state 5; a private ChatFwd outside it becomes the lobby message of type 9 (as 00447c70's), others drop;
//  * (132on2c) the game password: the original's games carry none (0044e960 leaves CreateGame's password empty
//    and 0044e430's JoinGame sends none). PORT-ONLY: the creator's password (online_game_password) goes to
//    omega_server as ext::GamePassword after the CreateGame response, port clients only; a joiner's goes in
//    JoinGame's own GamePassword field (+0x30, set_join_password) and a JoinGameResponse of -978 (the wrong
//    password) is reported to the screen (join_password_refused), which asks for it on the keyboard.
// Stage D (the agent statistics, ONLINE.md §3.6 / §9.9):
//  * (132on2d) the record's uploads are the original's: the login's tail 0044ce60 (GetAccountStats of the own
//    account; its record, else 00585e90, rebuilt and sent with the lobby tail), the create 0044e960 (reason 1), the
//    join 0044e430 (reason 2: the game row's host, mission and filter from its name), the game's end 0044e340
//    (reason 3; the abandoned join and the network level's end) and 00451b30's refresh every 0x9c40 ms in the
//    states 4 / 5 / 6 / 7 (GetAccountStats, AccountUpdateStats of the answer). +e8 is session +2484, the clan id
//    GetMyClans (00449a00 / 00448b70) stores: -1, as the original's with no clan (132on2i; omega_server has no clans
//    yet). PORT-ONLY: +f4 (00443020, the client's own address) is 0: omega_server
//    sees the address itself; 00451b30's 004499a0 (the login's refresh 00449a00 every 00497800 ms) is not run;
//  * (132on2d) in the level the lobby's frame is level_frame (play_main's): the original's 004536b0 runs 004446e0(1)
//    in state 10 while the DME game runs; the port's level pumps only the lobby's client (the LAN session is the
//    level's). WorldReport: PlayerCount 1 + the LAN clients (002ebec8's DME count), WorldStatus 1 while the game has
//    room for another agent, else 3 (PORT-ONLY approximation of 003a6bc0, not ported), GameStats +10 the seconds
//    since the level's start (005842cc's start, approximated);
//  * (132on2d) JOINMIS' rank column (002ab0f8) reads the record cache through player_rank (00450cf0).
// Stage E (the in-level connection, ONLINE.md §6.11 / §6.12; all PORT-ONLY, port clients only):
//  * (132on2e) the host's game port is the original's DME peer port 3658 (002e9480's default, ONLINE.md §6.12) or the
//    setting online_game_port; when it is taken the host falls back to a system-picked port (logged);
//  * (132on2e) with online_upnp on, the host asks the home router (UPnP IGD, online_upnp.hpp) to forward that UDP
//    port, off the game thread; HostEndpoint then names the external port too, and the mapping is removed when the
//    game is left (and at exit). A socket bound to loopback (--lan-address 127.0.0.1) skips it;
//  * (132on2e) the relay (online_relay_socket.hpp): the login's relay ticket goes to the RelayLink the game socket
//    factory shares; online_relay=auto joins directly first and, when that join has no answer, through the relay
//    (the host's relay address from ext::RelayPeer); force joins through the relay at once; off announces no
//    cap::Relay. The path taken is logged.
//  * (132on2e) the agent edit's upload (0033fc80: CMDCENTR's modify agent, Square on the appearance) is
//    agent_edited(): the record rebuilt and uploaded (AccountUpdateStats) while logged in.
// Stage F (132on2f, ONLINE.md §6.13; all PORT-ONLY, port clients only):
//  * the host's port mapping is a chain (online_portmap.hpp): UPnP IGD, then PCP, then NAT-PMP, each step logged;
//    PCP / NAT-PMP mappings are renewed at half their lifetime and deleted (lifetime 0) at the leave and at exit;
//  * the connection path is shown to the player: take_path_notices() feeds the front end's notice overlay (the
//    lobby screens) and the level's HUD ticker (00242b00, play_main): "direct to the host" / "relay through
//    omega_server" on a join, "<method> mapped ext port N" / "port mapping failed - see the port-forward guide" on a
//    host. Only when the session negotiated cap::HostEndpoint (a port client's hello): an original-kind session
//    (PS2) never shows one, as it never receives the extensions they describe.

#include "frontend_session.hpp"
#include "lan_session.hpp"
#include "online_lobby.hpp"
#include "online_portmap.hpp"
#include "online_relay_socket.hpp"
#include "online_upnp.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace omega {

class OnlineLobbyHost final : public FrontendLobby {
public:
    explicit OnlineLobbyHost(online::OnlineLobby::Options options) : lobby_(std::move(options)) {}
    ~OnlineLobbyHost() override;

    bool session() override;
    std::int32_t start() override;
    std::int32_t configs(std::vector<gui::LobbyConfig>& configs, std::vector<gui::LobbyServer>& servers) override;
    void disconnect() override;
    std::int32_t error() override { return error_; }
    std::int32_t connect(std::int32_t row) override;
    std::int32_t dnas_result() override { return 0; }
    std::int32_t universes_begin() override { return 1; }
    std::int32_t muis_connect(std::int32_t server, std::vector<gui::LobbyUniverse>& universes) override;
    std::int32_t universe_select(std::int32_t index) override { return index == 0 ? 1 : 0; }
    std::int32_t universe_default() override { return 1; }
    void universes_end() override {}
    std::string setting(const std::string&) override { return std::string(); }  // no debug settings file
    std::int32_t policy_begin() override { return 1; }
    std::int32_t announcements(std::vector<gui::LobbyAnnouncement>& out) override;
    std::int32_t policy(bool privacy, std::vector<std::string>& out) override;
    void policy_end() override {}
    void leave(std::int32_t a1) override;
    std::optional<std::string> notice() override;
    bool unread() override { return false; }
    bool region_choice() override { return false; }
    std::int32_t login(const std::string& name, const std::string& password) override;
    std::int32_t register_account(const std::string& name, const std::string& password) override;
    std::int32_t check_account(const std::string& name, const std::string& password) override;
    std::int32_t enter() override;
    std::int32_t state() override;
    std::int32_t mission(std::int32_t mission) override;
    std::int32_t records() override { return 0; }  // omega_server keeps no lobby records (offline messages)
    void channel_leave() override;
    std::int32_t room_open() override;
    void room_leave() override;
    void list_kind(std::int32_t kind) override;
    std::int32_t create(std::int32_t mission, std::int32_t agents, std::int32_t filter) override;
    std::optional<gui::GuiHost::LobbyCallback> pump() override;
    void update() override { service(); }
    void game_cleanup() override;
    std::optional<gui::GuiHost::LobbyGameInfo> game_info(std::int32_t world) override;
    // Checkpoint 132on2i (ONLINE.md §3.7.1): the game cache +118 (0044edc0 / 0044f0c0) and the contacts' fetch
    // 0044ebd0 (state 6 with the contacts open, every *00497860 ms; an entry older than *00497878 ms is asked again).
    std::optional<gui::GuiHost::LobbyCachedGame> game_cached(std::int32_t world) override;
    std::vector<gui::GuiHost::LobbyCachedGame> game_cache_take() override;
    std::int32_t join(std::int32_t world) override;
    void join_end() override;
    void set_state(std::int32_t state) override { state_ = state; }
    std::int32_t own_id() override { return static_cast<std::int32_t>(lobby_.account_id()); }
    gui::GuiHost::LobbyGameList game_list() override { return {lists_, rows_}; }
    // Checkpoint 132on2c: ONLIPREF's list is the one universe muis_connect names; the lobby's message list (NETMESS)
    // holds what the server sent the agent for an answer: omega_server sends no invitations or offline messages (its
    // notices are net_poll's, notice()); a private chat line outside the room (00447c70, type 9) is the one kind, and
    // an answer drops the message.
    std::int32_t universe_list(std::vector<gui::LobbyUniverse>& universes) override;
    std::int32_t message_count() override { return static_cast<std::int32_t>(messages_.size()); }
    std::optional<gui::GuiHost::LobbyMessage> message(std::int32_t index) override;
    void message_answer(std::int32_t index, std::int32_t accept) override;
    bool messages_changed() override { return messages_changed_; }
    void messages_set_changed(bool changed) override { messages_changed_ = changed; }
    // Checkpoint 132on2c: the room's chat (00451050 / 00447bb0 / 00447c70), its player list (004516e0 / 00447a30)
    // and the PORT-ONLY game password (see the header).
    std::int32_t chat_send(const std::string& text, std::int32_t target) override;
    std::vector<gui::GuiHost::LobbyChatLine> chat_take() override;
    gui::GuiHost::LobbyPlayerList player_list() override { return {player_lists_, players_}; }
    // Checkpoint 132on2h (CONTACTS, ONLINE.md §3.7): 0044df80 (state 9 / 0 -> 6, the contact list asked at once and
    // every 20 s: GetBuddyList_ExtraInfo 214 -> 215, 00446830), 0044dd20 (6 -> 9), the blocking 0044d9c0 (65 -> 66),
    // 00451370 (23 -> 24) and 0044db60 (a blocking list refresh, GetAccountStats of the invitee, then 258, whose 68 is
    // only logged), with the original's error codes. The room chat runs in state 6 too (0044df80 registers its list).
    std::int32_t contacts_open() override;
    void contacts_close() override;
    gui::GuiHost::LobbyContactList contact_list() override { return {contact_lists_, contacts_}; }
    std::int32_t contact_remove(std::int32_t id) override;
    std::int32_t account_id(const std::string& name) override;
    std::int32_t contact_add(std::int32_t id) override;
    // Checkpoint 132on2i (GuiAgentInfo mode 1, ONLINE.md §3.7.2): 00450c60, the record cache's (00450cf0, younger
    // than 30000 ms) or GetAccountStats'; the contacts test 0044d670 over the last contact list. The agency cell
    // calls answer as the original with no clan (GetMyClans' NoResult, 00448b70: +2484 / the leader -1): omega_server
    // has no clans yet (PORT-ONLY; the creation fails as a timeout, error 2).
    bool agent_stats(std::int32_t id, std::array<std::uint8_t, 0xfc>& out) override;
    bool is_contact(std::int32_t id) override;
    std::int32_t cell_invite(std::int32_t id) override;
    std::int32_t cell_create(const std::string& name) override;
    bool join_password_refused() override { return password_refused_; }
    void set_join_password(const std::string& password) override { join_password_ = password; }
    // PORT-ONLY (132on2c): the password the games this agent creates are protected by (empty: none; the settings key
    // online_game_password or --online-game-password).
    void set_game_password(std::string password) { game_password_ = std::move(password); }
    // PORT-ONLY (132on2c, ONLINE.md §7): the platform badges ("[PC] name") on the names the screens show (the game
    // rows' host name, the player list, the chat senders), from the server's roster (ext::Roster). Only when the
    // setting online_platform_badges is on and the server negotiated cap::Roster; a name the badge would push past
    // the original's field (the game name's %32s, the chat window's 0x10) stays plain.
    void set_badges(bool show) { badges_ = show; }
    // PORT-ONLY (132on2g, cap::Social): the player's chat filter choice (online_chat_filter; nullopt: the server's
    // default), sent once per login as ext::ChatFilter.
    void set_chat_filter(std::optional<bool> on) { chat_filter_ = on; }

    // Checkpoint 132on2d (ONLINE.md §3.6): the agent statistics record. The builder is the session's callback
    // +2554 (00452f30's), which runs 002cbd40 on the selected agent (the front end's build_stats_record).
    using StatsBuilder = std::function<void(std::span<std::uint8_t, online::kStatsBlobSize>)>;
    void set_stats_builder(StatsBuilder builder) { stats_builder_ = std::move(builder); }
    // 00450cf0 for JOINMIS' rank column (002ab0f8): the cached record's rank byte (+1) when the cache holds a record
    // for the account younger than 00497848 (30000 ms).
    std::optional<std::int32_t> player_rank(std::int32_t account) override;
    // The network level's lobby frame (004536b0 in state 10 runs 0044e330 = 004446e0(1)): every 15 s (0x3a98) the
    // PlayerReport of the record 00585e90 and, from the host (00584240), the WorldReport; the first of each at the
    // level's first frame (the clocks 005842d4 / 005842d8 start at 0). Only the lobby's client is pumped: the LAN
    // session is the level's (play_main's net frame).
    void level_frame();
    // The level's end (World_Teardown's network branch 0013493c: 0044e340, then 0042b1f0): the record rebuilt and
    // uploaded (reason 3), then the game left. `completed` is logged only.
    void level_end(bool completed);
    // 0044e340's reporting phase only. Keeps the game transport alive for native
    // teardown; game_cleanup is called after its borrowers have been released.
    // Repeated calls for one world do not upload the final record twice.
    void level_exit(bool completed);
    bool in_game() const { return world_ != 0; }

    // Checkpoint 132on2e (see the header's stage E). The relay: the mode (online_relay) and the link the game socket
    // factory shares (relay_socket_factory); null: no relay.
    enum class RelayMode { Off, Auto, Force };
    void set_relay(RelayMode mode, std::shared_ptr<RelayLink> link);
    RelayMode relay_mode() const { return relay_mode_; }
    // The host's preferred game port (online_game_port; 0: system-picked).
    void set_game_port(std::uint16_t port) { game_port_ = port; }
    // The host's port mapping (online_upnp: UPnP, then PCP, then NAT-PMP; online_portmap.hpp). `loopback`: the game
    // socket is bound to loopback, so nothing is mapped, unless the options name a loopback gateway (the fake one of
    // --online-portmap-test).
    void set_portmap(bool on, bool loopback, online::PortMapOptions options = {});
    // The joined game's path: "direct" / "relay" (empty: none yet).
    const std::string& join_path() const { return join_path_; }
    // PORT-ONLY (132on2f): the connection-path notices queued since the last call ("Connection: direct to the host",
    // "Connection: UPnP mapped ext port 3658", ...), for the front end's notice overlay and the level's HUD ticker.
    // Only for a port session that negotiated cap::HostEndpoint; an original-kind session (no hello) gets none.
    std::vector<std::string> take_path_notices() { return std::exchange(path_notices_, {}); }
    // The current one (empty: none).
    const std::string& path_notice_text() const { return path_notice_; }
    // 0033fc80 (CMDCENTR's modify agent, the appearance stored): the record rebuilt and uploaded while logged in.
    void agent_edited() override;

    // The LAN session the in-level game runs on (the front end's; null: no create / join).
    void set_lan(lan::LanSession* lan) { lan_ = lan; }
    // The lobby's per-frame service (the original's 004536b0 runs every frame): the pending create / join, the
    // game list, the notices. pump() runs it too.
    void service();
    std::uint32_t world() const { return world_; }

    online::OnlineLobby& lobby() { return lobby_; }
    // What the host did, one line each (the hidden runs print them).
    std::vector<std::string> take_log();
    // The server's PORT-ONLY notices (ext::Notice) received so far, in order.
    std::deque<online::OnlineClient::Notice>& notices() { return notices_; }

    static constexpr const char* muis_server_name = "SYPHONFILTER ONLINE QA";  // 0048c138 (0029d390's default)

private:
    void log(std::string line);

    void callback(std::int32_t a3, std::int32_t t0, const std::string& what);
    // The name with its platform badge (set_badges), when it fits in `limit` characters; else the name.
    std::string shown_name(const std::string& name, std::int32_t account, std::size_t limit);

    online::OnlineLobby lobby_;
    lan::LanSession* lan_ = nullptr;
    std::int32_t error_ = 0;
    std::int32_t state_ = 0;  // session +255c: 9 logged in, 4 main channel, 5 room, 6 joining, 10 created
    std::int32_t list_kind_ = 0;
    enum class Pending { None, Create, JoinRequest, JoinConnect };
    Pending pending_ = Pending::None;
    std::uint32_t world_ = 0;     // the world created or joined
    std::uint32_t joining_ = 0;   // the world being joined
    std::deque<gui::GuiHost::LobbyCallback> callbacks_;
    std::vector<gui::GuiHost::LobbyGameRow> rows_, rows_next_;
    std::uint32_t lists_ = 0;  // the completed game lists (LobbyGameList::serial)
    bool list_pending_ = false;
    std::uint64_t list_sent_ms_ = 0;
    std::vector<std::string> log_;
    std::deque<online::OnlineClient::Notice> notices_;
    std::vector<gui::GuiHost::LobbyMessage> messages_;
    bool messages_changed_ = false;
    std::vector<gui::GuiHost::LobbyChatLine> chat_;
    std::vector<gui::GuiHost::LobbyPlayerRow> players_, players_next_;
    std::uint32_t player_lists_ = 0;  // the completed player lists (LobbyPlayerList::serial)
    bool player_pending_ = false;
    std::uint64_t player_sent_ms_ = 0;
    std::string join_password_;  // the joiner's (JoinGame +0x30)
    bool password_refused_ = false;
    std::string game_password_;  // the creator's (ext::GamePassword)
    bool badges_ = false;
    void contacts_service();
    // Checkpoint 132on2h: the contact list (session +e0; 00446830's rows, published on the EndOfList).
    bool contact_frame(const online::Frame& f);  // one 215; true on its EndOfList
    std::vector<gui::GuiHost::LobbyContactRow> contacts_, contacts_next_;
    std::uint32_t contact_lists_ = 0;
    bool contact_pending_ = false;
    std::uint64_t contact_sent_ms_ = 0;  // session +257c (0: refresh at the next service)
    bool contacts_open_ = false;         // 0044df80 registered the chat and contact lists (until 0044dd20)
    bool room_chat() const { return state_ == 5 || (state_ == 6 && contacts_open_); }
    // Checkpoint 132on2i: the game cache (session +118) and 0044ebd0's fetch.
    struct CachedGame {
        gui::GuiHost::LobbyCachedGame game;
        std::uint64_t ms = 0;  // +28 (003698b0)
    };
    void cache_game(const online::Frame& f, const char* how);  // 0044edc0 (00446b40 / 00446ae0)
    void contacts_game_info();                                 // 0044ebd0 over the contact list (+e0)
    std::vector<CachedGame> game_cache_;
    std::vector<gui::GuiHost::LobbyCachedGame> game_cache_updates_;
    std::uint64_t game_info_fetch_ms_ = 0;  // *00585e50
    bool invitations_asked_ = false, social_asked_ = false;
    std::optional<bool> chat_filter_;

    // Checkpoint 132on2d: the statistics record (see the header's 132on2d paragraph).
    // 00452f30(session, reason, blob): the callback +2554 builds the record (reasons 0 login, 1 create, 2 join,
    // 3 the game's end).
    void build_record(std::int32_t reason, online::StatsBlob& blob);
    std::int32_t login_core(const std::string& name, const std::string& password);  // 00445420 with 00445a40's retry
    // 004505e0: AccountUpdateStats, logged.
    void upload_record(const char* why, const online::StatsBlob& blob);
    // The login's tail 0044ce60 and 0044e340: +e8 the lobby world, +f4 the client's address, +ec / +f0 / +f9 0,
    // +f8 5.
    static void lobby_tail(online::StatsBlob& blob);
    // 0044e340's head: the game's end (state 10 -> 9), the record rebuilt and uploaded.
    void entry_end();
    StatsBuilder stats_builder_;
    online::StatsBlob record_{};    // 00585e90 (the create / join / game end build into it; PlayerReport sends it)
    online::StatsBlob response_{};  // session +ad0 (the GetAccountStats response's record)
    std::uint64_t stats_update_ms_ = 0;  // session +2590 (00451b30's clock)
    // 00585e70 (0x18 bytes, the WorldReport's GameStats): the creator's account, mission, world, filter, the game's
    // seconds (+10, 004446e0) and the client's address (+14).
    std::array<std::int32_t, 6> game_stats_{};
    std::string game_name_;              // the created game's name (the WorldReport's GameName)
    std::int32_t game_agents_ = 0;       // the created game's agents (its players' limit)
    std::uint64_t report_player_ms_ = 0, report_world_ms_ = 0, level_start_ms_ = 0;
    bool level_ = false;
    bool level_exit_reported_ = false;

    // Checkpoint 132on2e: the relay, the game port and UPnP; 132on2f: the PCP / NAT-PMP fallbacks and the renewal.
    void apply_relay_ticket();
    bool join_through_relay(const char* why);
    void portmap_start(std::uint16_t port);
    void portmap_poll();
    void portmap_release();
    void send_host_endpoint();
    RelayMode relay_mode_ = RelayMode::Off;
    std::shared_ptr<RelayLink> relay_;
    std::uint32_t relay_generation_ = 0;  // the ticket applied (RelayTicket objects compared by vip / token)
    bool relay_tried_ = false;            // this join went through the relay already
    std::string join_path_;
    std::uint16_t game_port_ = 0;
    // The mapping chain's outcome and its step lines (written on the worker thread, logged on the game's).
    struct PortMapResult {
        online::PortMapping mapping;
        std::vector<std::string> steps;
    };
    bool portmap_ = false, portmap_loopback_ = false;
    online::PortMapOptions portmap_options_;
    std::shared_future<PortMapResult> portmap_pending_;
    std::shared_future<online::PortMapping> portmap_renewing_;
    std::optional<online::PortMapping> portmap_mapping_;
    std::uint64_t portmap_granted_ms_ = 0;  // when the gateway granted (or renewed) the PCP / NAT-PMP lifetime
    std::vector<std::future<void>> portmap_unmaps_;

    // Checkpoint 132on2f: the connection-path notice (see the header's stage F).
    bool path_notices_allowed() const;
    void path_notice(std::string text);
    std::string path_notice_;
    std::vector<std::string> path_notices_;
};

}  // namespace omega
