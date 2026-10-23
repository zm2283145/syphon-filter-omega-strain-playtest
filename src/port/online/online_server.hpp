#pragma once
// PORT-ONLY (not in the original; checkpoint 132on1, research/ONLINE.md §5): omega_server's core. It stands in for the
// SCEA Medius servers the original client talked to (the authentication / lobby servers behind the MCL API
// 0x00466a28..0x0046e0f0) and answers the requests the SFOLobby code sends with the response layouts its callbacks read
// (ONLINE.md §3 / §4). Transport independent: the caller feeds frames per connection and gets frames to send through a
// callback, so the unit tests drive it in memory and omega_server_main.cpp drives it over TCP.
//
// Port-only additions (ONLINE.md §5 - §7): the platform of each player (hello / roster), the admin console (bans,
// kicks, lists, broadcast, cheat policy), the server log, the host's in-level endpoint for the join hand-off, and the
// mod policy (132md4's mod report, folded in by 132on2c; ONLINE.md §6.9). Checkpoint 132on2d: the statistics the
// original client reports (AccountUpdateStats / GetAccountStats / PlayerReport / WorldReport, ONLINE.md §3.6) kept per
// account with each finished game's result, and the operations limits (connections and frames per address, the
// failed-login throttle, account creations per hour; ONLINE.md §6.10). Checkpoint 132on2g: the contact (buddy) list
// and its invitations on the original wire (ONLINE.md §3.7), the chat filter, the PORT-ONLY recent players
// (cap::Social), and the graceful shutdown / restart (the admin `shutdown` / `restart`, ONLINE_DEPLOY.md).

#include "online_accounts.hpp"
#include "online_mod_report.hpp"
#include "online_protocol.hpp"
#include "online_relay.hpp"
#include "online_social.hpp"
#include "online_stats.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega::online {

struct ServerConfig {
    bool allow_cheats = true;  // PORT-ONLY dev setting: on for now; `cheats off` (or allow_cheats=0) disables
    std::string server_name = "omega_server";  // PORT-ONLY: the Welcome's text (port clients only)
    // The texts the original shows after DNAS and before the account login (ONLINE.md §4.3): the usage policy
    // (PolicyTitle1), the privacy policy (PolicyTitle2; empty: the page is skipped) and the announcements, one page
    // each. Port-written placeholders; editable by an admin and kept in the policy file.
    std::string usage_policy =
        "omega_server usage policy (placeholder written for the PC port).\n"
        "Play fair. No cheating against other players, no harassment, no impersonation.\n"
        "Accounts that break these rules can be suspended or banned by the server's administrators.";
    std::string privacy_policy =
        "omega_server privacy policy (placeholder written for the PC port).\n"
        "The server stores your account name, a salted digest of your password (never the password), "
        "the policy version you accepted, your bans and a log of logins, games and chat.";
    std::vector<std::string> announcements = {"Welcome to omega_server, the community server of the Omega Strain PC port."};
    std::uint32_t policy_version = 1;  // bumped by every policy edit; each login records it on the account
    std::string admin_token;            // empty: only admin accounts can run admin commands
    std::string public_ip = "127.0.0.1";  // the address written into the login response's connect info
    std::uint16_t port = kDefaultServerPort;
    std::uint16_t default_game_port = 47525;  // kLanGamePort: the host's in-level port if it reports none
    int max_players = 4;                       // kLanMaxClients
    // PORT-ONLY (132md4, ONLINE.md §6.9): who may create or join a game by the mods its client reports
    // (ext::ModReport). Original (PS2) sessions are never checked by any / cosmetic / none. 132md16: gameplay (the
    // default) lets a session join only a game whose creator has the same gameplay set (gameplay_refusal).
    ModPolicy mod_policy = ModPolicy::Gameplay;
    // PORT-ONLY operations (checkpoint 132on2d, ONLINE.md §6.10). 0 turns a limit off.
    int max_conns_per_ip = 16;        // connections open at once from one address (more are closed at once)
    int message_rate = 100;           // client frames per second per connection, on average ...
    int message_burst = 1000;         // ... with this many at once; a connection past it is closed
    int login_failures = 5;           // failed logins of one name from one address within login_window ...
    std::uint64_t login_window = 600;  // (seconds) ...
    std::uint64_t login_lockout = 300;  // lock that name on that address this long; 4x login_failures from one
                                        // address (any names) lock the address
    int registrations_per_hour = 20;  // new accounts per address per hour
    // PORT-ONLY anti-abuse (checkpoint 132on2h, ONLINE.md §6.16, ONLINE_DEPLOY.md §4). 0 turns a limit off. The rates
    // are token buckets: the burst at once, then refilled at the rate. Every refusal is logged with "abuse:".
    int login_attempts_per_ip = 30;       // login + account-creation attempts per address per minute (any outcome) ...
    int login_attempts_per_account = 10;  // ... and logins of one account name per minute (from any address)
    int chat_per_minute = 30;             // chat messages (ChatMessage) per player per minute ...
    int chat_burst = 10;                  // ... with this many at once (a message past it is dropped, the sender told)
    int chat_per_minute_ip = 120;         // the chat of every player of one address together (burst: the same)
    int chat_flood_kick = 5;              // chat messages dropped within a minute before the player is disconnected
    int flood_block_after = 3;            // flood disconnects (message_rate, chat_flood_kick) of one address ...
    std::uint64_t flood_window = 600;     // ... within this many seconds ...
    std::uint64_t flood_block = 900;      // ... block new connections from that address this long (seconds)
    std::size_t max_results = 200;    // finished games kept in the statistics file
    // PORT-ONLY (checkpoint 132on2e, online_relay.hpp, ONLINE_DEPLOY.md §2.3): the in-level UDP relay. relay_port 0
    // (the default) turns it off; the other keys are RelayLimits (relay_idle in seconds).
    std::uint16_t relay_port = 0;
    std::size_t relay_max_endpoints = 64;
    int relay_packet_rate = 200;
    int relay_byte_rate = 256 * 1024;
    std::uint64_t relay_idle = 60;
    // Checkpoint 132on2g (online_social.hpp): the chat filter's default (TextFilter / ChatMessage); a port client can
    // turn it off for itself (ext::ChatFilter); an original session always gets this.
    bool chat_filter = true;
};

// Loads key=value lines (allow_cheats, server_name, admin_token, public_ip, port, max_players, mod_policy (132md4:
// any | cosmetic | none; 132md16: gameplay, the default), the 132on2d operations keys max_conns_per_ip, message_rate, message_burst, login_failures,
// login_window, login_lockout, registrations_per_hour, max_results, the 132on2h anti-abuse keys login_attempts_per_ip,
// login_attempts_per_account, chat_per_minute, chat_burst, chat_per_minute_ip, chat_flood_kick, flood_block_after,
// flood_window, flood_block, the 132on2e relay keys relay_port,
// relay_max_endpoints, relay_packet_rate, relay_byte_rate, relay_idle, the 132on2g chat_filter, and the extra keys
// accounts, log, bind, pbkdf2_iterations, policy_file, stats_file, backups, backup_interval, log_format and the
// 132on2g social_file, chat_filter_words, log_max_bytes, log_keep); '#' comments. Unknown keys and bad values throw.
// Returns the extra keys.
std::map<std::string, std::string> parse_server_config(std::string_view text, ServerConfig& cfg);

// The policy file (PORT-ONLY): "version=N", then sections "[usage]", "[privacy]", "[announcement]" (one section per
// announcement, in order), each followed by its text lines. '#' lines before the first section are comments.
void parse_policy_file(std::string_view text, ServerConfig& cfg);
std::string format_policy_file(const ServerConfig& cfg);

struct ServerGame {
    std::uint32_t world_id = 0;
    std::string game_name;  // the GameName text (the create request's +0x30, "%32s\t%d\t%d\t%d\t%d")
    std::vector<std::uint8_t> create_tail;  // the create request's +0x70..+0x94 (opaque, echoed in GameInfo)
    std::uint32_t host_conn = 0;
    std::uint32_t host_account = 0;
    std::uint32_t host_ip = 0;  // host byte order
    std::uint16_t host_port = 0;
    std::vector<std::uint32_t> players;  // account ids, the host first
    int max_players = 4;
    std::int32_t world_status = kWorldStaging;
    // PORT-ONLY (132on2c, cap::GamePassword): the join password the host set (ext::GamePassword); empty: none. A
    // JoinGame whose GamePassword (+0x30) differs is refused with InvalidPassword (-978).
    std::string password;
    // Checkpoint 132on2d (ONLINE.md §3.6): the game's statistics. Each player who entered it, with the record at the
    // entry and the last one received in the game (AccountUpdateStats / PlayerReport), becomes the game's result
    // when it closes; the host's last WorldReport.
    std::uint64_t created = 0;
    bool reported = false;  // a PlayerReport or WorldReport arrived (the game was played; a result is kept)
    std::map<std::uint32_t, GameResultPlayer> results;
    std::optional<WorldReportFields> world_report;
};

struct ServerPlayer {
    std::uint32_t conn = 0;
    std::uint32_t account_id = 0;
    std::string name;
    Platform platform = Platform::Unknown;
    std::uint32_t world_id = 0;
    bool admin = false;
    ClientKind kind = ClientKind::Port;
};

class OmegaServer {
public:
    using SendFn = std::function<void(std::uint32_t conn, std::vector<std::uint8_t> frame)>;
    using CloseFn = std::function<void(std::uint32_t conn)>;
    using LogFn = std::function<void(const std::string& line)>;
    using ClockFn = std::function<std::uint64_t()>;  // Unix seconds

    OmegaServer(AccountStore& accounts, ServerConfig config, SendFn send, CloseFn close, LogFn log, ClockFn clock);

    void on_connect(std::uint32_t conn, std::uint32_t peer_ip, ClientKind kind = ClientKind::Port);
    void on_frame(std::uint32_t conn, const Frame& frame);
    void on_disconnect(std::uint32_t conn);

    // One admin console line (the server's stdin, or ext::AdminCommand from an admin). Returns the output text.
    std::string admin_command(std::string_view line, std::string_view who);
    // Where admin edits of the policy / announcements are saved (empty: not saved).
    void set_policy_file(std::string path, int backups = 0) { policy_file_ = std::move(path), policy_backups_ = backups; }
    // Checkpoint 132on2d: the statistics store (each account's record, the finished games). Memory only until
    // replaced; flush_stats() writes pending changes (the server saves at most every 30 seconds, and at the end).
    StatsStore& stats() { return stats_; }
    void flush_stats();

    std::vector<ServerPlayer> players() const;
    const std::map<std::uint32_t, ServerGame>& games() const { return games_; }
    const ServerConfig& config() const { return config_; }
    // PORT-ONLY: the cheat permission a player gets: allow_cheats, but never in a world with an original (PS2)
    // player (ONLINE.md §8.3).
    bool cheats_allowed_for(std::uint32_t conn) const;
    // PORT-ONLY (132md4): the last mod report a port session sent (null: none yet, or an original session), and the
    // console's `mods` text (the policy, then one line per connection).
    const ModReport* mod_report(std::uint32_t conn) const;
    std::string mods_status() const;
    // PORT-ONLY (132on2e, online_relay.hpp): the in-level UDP relay (null: off). A port session with cap::Relay gets
    // an endpoint and its ext::RelayTicket at login, and a joiner gets the host's ext::RelayPeer before its
    // JoinGameResponse; the endpoint closes with the login. The relay forwards only within one world (world_of).
    void set_relay(RelayCore* relay, std::uint16_t udp_port);
    std::uint32_t world_of(std::uint32_t conn) const;
    RelayLimits relay_limits() const;

    // Checkpoint 132on2g: the contacts, invitations and recent players (saved at each change when it has a file), and
    // the chat filter's word list.
    SocialStore& social() { return social_; }
    ChatFilter& chat_filter() { return chat_filter_; }

    // Checkpoint 132on2g: the graceful stop. The admin `shutdown [seconds|now|cancel] [reason]` / `restart [seconds]
    // [reason]` (or request_stop, for a signal or a service stop) schedules it; tick() warns the players (at 60, 30,
    // 10 and 5 seconds left: ext::Notice Shutdown to a port session with cap::Notices, else a private ChatFwd from
    // "omega_server", the original wire, which NETMESS shows); lifecycle_due() says when it is time, and
    // shutdown_sessions() then closes every game (keeping its result), logs everybody out, closes the connections
    // and saves the stores. The caller (omega_server_main) stops, or rebuilds the server for a restart.
    enum class Lifecycle { None, Shutdown, Restart };
    void request_stop(Lifecycle kind, std::uint64_t seconds, std::string reason);
    void tick();
    Lifecycle lifecycle_due() const;
    Lifecycle lifecycle_pending() const { return stop_.kind; }
    const std::string& stop_reason() const { return stop_.reason; }
    void shutdown_sessions(const std::string& reason);
    std::size_t connection_count() const { return conns_.size(); }

    // PORT-ONLY (checkpoint 132on2h, ONLINE_DEPLOY.md §12): the main loop's work per iteration (its time outside the
    // poll's wait) and the frames it handled, kept for `status` (the last kTickSamples iterations: p50 / p99 / max),
    // and a text about the process (CPU time, memory) the caller supplies for `status`.
    static constexpr std::size_t kTickSamples = 4096;
    void note_tick(std::uint64_t busy_us, std::size_t frames);
    struct TickSummary {
        std::uint64_t ticks = 0, frames = 0;  // since the start
        std::size_t samples = 0;
        double p50_ms = 0, p99_ms = 0, max_ms = 0;
    };
    TickSummary tick_summary() const;
    void set_process_info(std::function<std::string()> f) { process_info_ = std::move(f); }
    // 132on2h: the anti-abuse counters since the start (the `status` / `limits` commands).
    struct AbuseCounters {
        std::uint64_t login_refused = 0, chat_dropped = 0, chat_kicks = 0, flood_disconnects = 0, blocked_connects = 0,
                      blocks = 0;
    };
    const AbuseCounters& abuse_counters() const { return abuse_; }

    // PORT-ONLY (checkpoint 132on2h, ONLINE_DEPLOY.md §12): where the password work (PBKDF2, AccountStore::HashParams)
    // runs. The runner calls work() anywhere (a worker thread: it touches nothing of the server) and later done(result)
    // on the server's own thread (omega_server_main's loop drains them). None set (the tests): both at once, inline.
    // One password operation per connection at a time; the request's answer waits for its done().
    using PasswordWork = std::function<std::vector<std::uint8_t>()>;
    using PasswordDone = std::function<void(std::vector<std::uint8_t>)>;
    using HashRunner = std::function<void(PasswordWork, PasswordDone)>;
    void set_hash_runner(HashRunner r) { hash_runner_ = std::move(r); }
    std::size_t password_jobs_pending() const { return hash_pending_; }

private:
    struct Conn {
        std::uint32_t id = 0;
        std::uint32_t peer_ip = 0;
        ClientKind kind = ClientKind::Port;
        bool helloed = false;
        HelloInfo hello;
        std::string session_key;
        std::uint32_t account_id = 0;
        std::string name;
        bool admin = false;
        bool in_channel = false;
        std::uint32_t world_id = 0;
        std::uint32_t policy_seen = 0;  // the policy version this session fetched (0x004456d0) before its login
        int cheats_sent = -1;           // the last allow_cheats sent (ext::Welcome / CheatPolicy)
        std::optional<ModReport> mod_report;  // PORT-ONLY (132md4): the last ext::ModReport (cap::Mods)
        // PORT-ONLY operations (132on2d): the frame budget (message_rate / message_burst) and a pending close.
        double tokens = -1;
        std::uint64_t tokens_at = 0;
        bool closing = false;
        // 132on2g: the contact invitations already delivered to this login (each goes once, by 259 or 262), and the
        // player's own chat filter choice (ext::ChatFilter; none: the server's chat_filter).
        std::vector<std::uint32_t> invitations_delivered;
        std::optional<bool> chat_filter;
        // 132on2h: the player's chat budget (chat_per_minute / chat_burst) and the times of its dropped messages.
        double chat_tokens = -1;
        std::uint64_t chat_at = 0;
        std::vector<std::uint64_t> chat_drops;
        bool hashing = false;  // 132on2h: a registration / login waits for its password work
        Platform platform() const { return kind == ClientKind::Original ? Platform::Ps2 : hello.platform; }
        bool has(std::uint32_t c) const { return kind == ClientKind::Port && helloed && (hello.caps & c) == c; }
    };

    void send(std::uint32_t conn, std::uint16_t type, std::span<const std::uint8_t> body);
    // PORT-ONLY extensions go only to a port session that announced `capability` (never to an original session).
    void send_ext(const Conn& c, std::uint32_t capability, std::uint16_t type, std::span<const std::uint8_t> body);
    void send_notice(const Conn& c, std::uint32_t code, std::string_view text);
    void log(const std::string& line);
    std::vector<std::uint8_t> response(std::span<const std::uint8_t> request, std::size_t size) const;
    void save_policy();
    void push_cheat_policy();

    void on_session_begin(Conn& c, std::span<const std::uint8_t> req);
    void on_registration(Conn& c, std::span<const std::uint8_t> req);
    void finish_registration(Conn& c, const std::string& name, const AccountStore::HashParams& params,
                             std::span<const std::uint8_t> digest, std::vector<std::uint8_t> r);
    void on_login(Conn& c, std::span<const std::uint8_t> req);
    void finish_login(Conn& c, const std::string& name, const std::string& password, std::span<const std::uint8_t> digest,
                      std::vector<std::uint8_t> r);
    // Runs the password work through hash_runner_ and `then` on the connection when it is still there.
    void run_password_work(Conn& c, PasswordWork work, std::function<void(Conn&, std::vector<std::uint8_t>)> then);
    void on_logout(Conn& c, std::span<const std::uint8_t> req);
    void on_policy(Conn& c, std::span<const std::uint8_t> req);
    void on_announcements(Conn& c, std::span<const std::uint8_t> req);
    void on_update_stats(Conn& c, std::span<const std::uint8_t> req);
    void on_get_stats(Conn& c, std::span<const std::uint8_t> req);
    void on_player_report(Conn& c, std::span<const std::uint8_t> req);
    void on_world_report(Conn& c, std::span<const std::uint8_t> req);
    void keep_stats(Conn& c, std::span<const std::uint8_t> blob, const char* source);
    void enter_world_stats(ServerGame& g, const Conn& c);
    void mark_stats_dirty();
    std::string stats_command(const std::vector<std::string>& w);
    // PORT-ONLY operations (132on2d): false (and the connection closing) past message_rate / message_burst.
    bool within_rate(Conn& c);
    // The failed-login throttle: seconds left on a lock of this name / address (0: none).
    std::uint64_t login_locked(const std::string& name, std::uint32_t ip, std::uint64_t now);
    void login_failed(const std::string& name, std::uint32_t ip, std::uint64_t now);
    void on_create_game(Conn& c, std::span<const std::uint8_t> req);
    void on_join_game(Conn& c, std::span<const std::uint8_t> req);
    // PORT-ONLY (132md4): true when config_.mod_policy refuses this create / join; the refusal is already answered.
    bool refused_by_mod_policy(Conn& c, std::span<const std::uint8_t> req, bool create);
    void on_game_list(Conn& c, std::span<const std::uint8_t> req);
    void on_game_info(Conn& c, std::span<const std::uint8_t> req);
    void on_chat(Conn& c, std::span<const std::uint8_t> req);
    void on_text_filter(Conn& c, std::span<const std::uint8_t> req);
    void on_player_list(Conn& c, std::span<const std::uint8_t> req);
    void on_ext(Conn& c, const Frame& frame);
    // 132on2g: the contact list (ONLINE.md §3.7).
    void on_buddy_list(Conn& c, std::span<const std::uint8_t> req);
    void on_buddy_invitations(Conn& c, std::span<const std::uint8_t> req);
    void on_buddy_add(Conn& c, std::span<const std::uint8_t> req);
    void on_buddy_answer(Conn& c, std::span<const std::uint8_t> req);
    void on_buddy_remove(Conn& c, std::span<const std::uint8_t> req);
    void on_account_get_id(Conn& c, std::span<const std::uint8_t> req);
    void forward_invitation(Conn& to, std::uint32_t from);
    void save_social();
    // 132on2h (the load test's hot spot): the busy paths (account creation, a policy acceptance at login, a game
    // join's recent players) mark the store dirty; tick() writes it at most once per clock second, and the
    // shutdown at once (the atomic writes keep the files whole; a crash loses at most the last second's changes).
    void flush_stores(bool now_anyway);
    bool accounts_dirty_ = false;
    bool social_dirty_ = false;
    std::uint64_t stores_flushed_at_ = 0;
    void send_recent_players(Conn& c);
    bool filter_for(const Conn& c) const;
    // A server line to one session: ext::Notice `code` when it has cap::Notices, else a private ChatFwd from
    // "omega_server" (account id 0; the original wire, NETMESS type 9 outside the room).
    void server_message(const Conn& c, std::uint32_t code, const std::string& text);

    void log_out(Conn& c, const char* why);
    void leave_world(Conn& c);
    void close_world(std::uint32_t world_id, const char* why);
    void broadcast_roster();
    void flush_roster();
    bool roster_dirty_ = false;
    Conn* conn_of_account(std::uint32_t account_id);
    Conn* conn_of_name(std::string_view name);
    void kick_conn(Conn& c, std::string_view reason);

    AccountStore& accounts_;
    ServerConfig config_;
    SendFn send_;
    CloseFn close_;
    LogFn log_;
    ClockFn clock_;
    std::map<std::uint32_t, Conn> conns_;
    std::map<std::uint32_t, ServerGame> games_;
    std::uint32_t next_world_ = 1;
    std::string policy_file_;
    int policy_backups_ = 0;
    StatsStore stats_;
    bool stats_dirty_ = false;
    std::uint64_t stats_saved_at_ = 0;
    struct Failures {
        int count = 0;
        std::uint64_t first = 0;
        std::uint64_t locked_until = 0;
    };
    std::map<std::string, Failures> login_failures_;   // "name@ip" and "@ip"
    std::map<std::uint32_t, std::vector<std::uint64_t>> registrations_;  // address -> the last hour's
    // 132on2h anti-abuse: token buckets (per minute) of login attempts by address and by account name, and of chat by
    // address; each address's flood disconnects and its block.
    struct Bucket {
        double tokens = -1;
        std::uint64_t at = 0;
    };
    static bool take_token(Bucket& b, int per_minute, int burst, std::uint64_t now);
    static bool bucket_full(const Bucket& b, int per_minute, int burst, std::uint64_t now);
    std::map<std::uint32_t, Bucket> login_by_ip_;
    std::map<std::string, Bucket> login_by_name_;
    std::map<std::uint32_t, Bucket> chat_by_ip_;
    struct Floods {
        std::vector<std::uint64_t> times;
        std::uint64_t blocked_until = 0;
    };
    std::map<std::uint32_t, Floods> floods_;
    AbuseCounters abuse_;
    std::uint64_t maintained_at_ = 0;
    // True when the login / registration may go on; else refused (logged; a port session gets a notice).
    bool login_attempt_allowed(Conn& c, const std::string& name, bool registration, std::uint64_t now);
    bool chat_allowed(Conn& c, std::uint64_t now);
    void flood_disconnect(Conn& c, const std::string& why, std::uint64_t now);
    std::uint64_t blocked_for(std::uint32_t ip, std::uint64_t now) const;
    void maintain(std::uint64_t now);
    std::string limits_text(std::uint64_t now) const;
    std::vector<std::uint32_t> tick_busy_;  // ring of kTickSamples (microseconds)
    std::size_t tick_next_ = 0;
    std::uint64_t ticks_ = 0, tick_frames_ = 0;
    std::function<std::string()> process_info_;
    HashRunner hash_runner_;
    std::size_t hash_pending_ = 0;
    std::uint64_t hash_jobs_ = 0;
    RelayCore* relay_ = nullptr;
    std::uint16_t relay_port_ = 0;
    std::uint64_t relay_now_ms() const;
    std::uint64_t started_at_ = 0;  // 132on2f: clock_() at construction (the `status` command's uptime)
    std::string status_text(std::uint64_t now) const;
    SocialStore social_;
    ChatFilter chat_filter_;
    struct Stop {
        Lifecycle kind = Lifecycle::None;
        std::uint64_t due = 0;  // clock_() seconds
        std::string reason;
        std::uint64_t last_warned = ~0ull;  // the seconds-left mark last warned at
    } stop_;
    std::string stop_command(const std::vector<std::string>& w, std::string_view line, bool restart);
    std::string social_command(const std::vector<std::string>& w);
};

// "30m" / "2h" / "7d" / "3600" (seconds) / "perm"; nullopt when malformed.
std::optional<std::uint64_t> parse_ban_duration(std::string_view text);
std::string ipv4_text(std::uint32_t ip);
// 132on2e: a private / loopback / link-local / carrier-grade NAT (100.64.0.0/10) address (no UPnP forward applies).
bool is_private_ipv4(std::uint32_t ip);

}  // namespace omega::online
