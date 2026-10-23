// PORT-ONLY (checkpoint 132on2): see online_lobby_host.hpp.
#include "online_lobby_host.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <utility>

namespace omega {

namespace {
std::uint64_t now_ms() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

// "a.b.c.d" -> the address (lan_ipv4).
std::optional<std::uint32_t> parse_ipv4(const std::string& text) {
    std::uint32_t parts[4] = {};
    std::size_t at = 0;
    for (int i = 0; i < 4; ++i) {
        if (i != 0 && (at >= text.size() || text[at++] != '.')) return std::nullopt;
        const std::size_t start = at;
        while (at < text.size() && at - start < 3 && text[at] >= '0' && text[at] <= '9')
            parts[i] = parts[i] * 10 + static_cast<std::uint32_t>(text[at++] - '0');
        if (at == start || parts[i] > 255) return std::nullopt;
    }
    if (at != text.size()) return std::nullopt;
    return lan::lan_ipv4(static_cast<std::uint8_t>(parts[0]), static_cast<std::uint8_t>(parts[1]),
                         static_cast<std::uint8_t>(parts[2]), static_cast<std::uint8_t>(parts[3]));
}
}  // namespace

void OnlineLobbyHost::log(std::string line) { log_.push_back(std::move(line)); }

std::string OnlineLobbyHost::shown_name(const std::string& name, std::int32_t account, std::size_t limit) {
    if (!badges_ || name.empty() || !(lobby_.client().caps() & online::cap::Roster)) return name;
    const online::Platform p = account == static_cast<std::int32_t>(lobby_.account_id())
                                   ? lobby_.options().platform
                                   : lobby_.client().platform_of(static_cast<std::uint32_t>(account));
    if (p == online::Platform::Unknown) return name;
    std::string b = online::badged_name(name, p, true);
    return b.size() <= limit ? b : name;
}

std::vector<std::string> OnlineLobbyHost::take_log() {
    auto out = std::move(log_);
    log_.clear();
    return out;
}

bool OnlineLobbyHost::session() { return lobby_.session(); }

std::int32_t OnlineLobbyHost::start() {
    if (lobby_.start()) {
        log("online: connected to " + lobby_.options().server + ", session " + lobby_.session_key() +
            (lobby_.client().welcomed() ? " (server '" + lobby_.client().server_name() + "')" : std::string()));
        return 1;
    }
    log("online: start failed: " + lobby_.last_error());
    return 0;
}

std::int32_t OnlineLobbyHost::configs(std::vector<gui::LobbyConfig>& configs, std::vector<gui::LobbyServer>& servers) {
    // PORT-ONLY: one configuration row (the server) and one MUIS server (see the header).
    gui::LobbyConfig c;
    c.selected = 1;
    c.name = "omega_server " + lobby_.options().server;
    configs.push_back(std::move(c));
    gui::LobbyServer s;
    s.name = muis_server_name;
    s.universe = lobby_.client().server_name().empty() ? std::string("omega_server") : lobby_.client().server_name();
    servers.push_back(std::move(s));
    error_ = 0;
    return 1;
}

// 0044c3c0 ends the eznet configuration's connection; the port's lobby session (the TCP stream) is the DME
// session's and stays up, as the original's DME session does.
void OnlineLobbyHost::disconnect() {}

std::int32_t OnlineLobbyHost::connect(std::int32_t row) {
    if (row != 0 || !lobby_.session()) {
        error_ = 1;
        log("online: connect row " + std::to_string(row) + " refused");
        return 0;
    }
    log("online: DNAS bypassed (port-only: no DNAS authentication)");
    error_ = 0;
    return 1;
}

std::int32_t OnlineLobbyHost::muis_connect(std::int32_t server, std::vector<gui::LobbyUniverse>& universes) {
    if (server != 0) return 0;
    gui::LobbyUniverse u;
    u.id = 1;
    u.name = lobby_.client().server_name().empty() ? std::string("omega_server") : lobby_.client().server_name();
    universes.push_back(std::move(u));
    return 1;
}

std::int32_t OnlineLobbyHost::universe_list(std::vector<gui::LobbyUniverse>& universes) {
    universes.clear();
    return muis_connect(0, universes);
}

std::optional<gui::GuiHost::LobbyMessage> OnlineLobbyHost::message(std::int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= messages_.size()) return std::nullopt;
    return messages_[static_cast<std::size_t>(index)];
}

void OnlineLobbyHost::message_answer(std::int32_t index, std::int32_t accept) {
    if (index < 0 || static_cast<std::size_t>(index) >= messages_.size()) return;
    log("online: message " + std::to_string(index) + " answered " + std::to_string(accept));
    const auto& m = messages_[static_cast<std::size_t>(index)];
    // 00450f30: a type 1 message (a contact invitation) is answered by 00446560: AddToBuddyListFwdConfirmationResponse
    // (260; +18 accept ? 2 : -985, +30 the inviter), no reply (132on2g, ONLINE.md §3.7).
    if (m.type == 1 && lobby_.logged_in()) {
        lobby_.client().send_medius(online::mid::AddToBuddyListFwdConfirmationResponse,
                                    online::request::buddy_answer(lobby_.session_key(), m.id, accept != 0));
        log("online: contact invitation from " + m.from + (accept ? " accepted" : " declined") + " (260)");
    }
    messages_.erase(messages_.begin() + index);
    messages_changed_ = true;
}

// 132on2g (ONLINE.md §3.7): the contact invitations. 0044d780 (the lobby tick 00442700, once per login) asks for the
// pending ones (261 -> 262, 00446740), and 00446640 takes the forwarded ones (259) while online: each is a lobby
// message of type 1 from the inviter (NETMESS), answered by message_answer. PORT-ONLY (cap::Social): the player's
// chat filter choice (online_chat_filter) and the recent players list, asked once per login and logged.
void OnlineLobbyHost::contacts_service() {
    if (!lobby_.logged_in()) {
        invitations_asked_ = social_asked_ = false;
        return;
    }
    auto& client = lobby_.client();
    if (!invitations_asked_ && state_ >= 4) {
        invitations_asked_ = true;
        client.send_medius(online::mid::GetBuddyInvitations, online::request::buddy_invitations());
        log("online: GetBuddyInvitations sent (261, 0044d780)");
    }
    auto invitation = [&](std::int32_t id, std::string name, const char* how) {
        gui::GuiHost::LobbyMessage m;
        m.type = 1;
        m.id = id;
        m.from = std::move(name);
        log("online: contact invitation from " + m.from + " (" + how + "): lobby message 1");
        messages_.push_back(std::move(m));
        messages_changed_ = true;
    };
    while (const auto f = client.wait_for(online::mid::GetBuddyInvitationsResponse, 0)) {
        if (f->body.size() < online::msize::GetBuddyInvitationsResponse) continue;
        if (online::get_i32(f->body, online::kStatusOffset) == online::status::Success)
            invitation(online::get_i32(f->body, 0x1c), online::get_str(f->body, 0x20, 32), "262");
    }
    while (const auto f = client.wait_for(online::mid::AddToBuddyListFwdConfirmation, 0)) {
        if (f->body.size() < online::msize::AddToBuddyListFwdConfirmation) continue;
        invitation(online::get_i32(f->body, 0x18), online::get_str(f->body, 0x1c, 32), "259");
    }
    if (!social_asked_ && client.welcomed() && (client.caps() & online::cap::Social)) {
        social_asked_ = true;
        if (chat_filter_) {
            client.send_ext(online::ext::ChatFilter, online::ExtWriter().u8(*chat_filter_ ? 1 : 0).bytes());
            log(std::string("online: chat filter ") + (*chat_filter_ ? "on" : "off") + " (online_chat_filter, port-only)");
        }
        client.send_ext(online::ext::RecentPlayers, {});
    }
    if (auto recent = client.take_recent_players()) {
        std::string names;
        for (const auto& p : *recent)
            names += (names.empty() ? ": " : ", ") + online::badged_name(p.name, p.platform, badges_) + (p.online ? " (online)" : "");
        log("online: recent players (port-only): " + std::to_string(recent->size()) + names);
    }
}

std::int32_t OnlineLobbyHost::announcements(std::vector<gui::LobbyAnnouncement>& out) {
    std::vector<std::pair<std::int32_t, std::string>> rows;
    const bool ok = lobby_.announcements(rows);
    for (auto& [id, text] : rows) out.push_back({id, std::move(text)});
    log("online: " + std::to_string(rows.size()) + " announcement row(s)" + (ok ? "" : " (" + lobby_.last_error() + ")"));
    return ok ? 1 : 0;
}

std::int32_t OnlineLobbyHost::policy(bool privacy, std::vector<std::string>& out) {
    const std::size_t before = out.size();
    const bool ok = lobby_.policy(privacy, out);
    log(std::string("online: ") + (privacy ? "privacy" : "usage") + " policy, " + std::to_string(out.size() - before) +
        " chunk(s)" + (ok ? "" : " (" + lobby_.last_error() + ")"));
    return ok ? 1 : 0;
}

void OnlineLobbyHost::leave(std::int32_t) {
    if (lobby_.logged_in()) lobby_.logout();  // 002ca640: 00445850 when logged in
    lobby_.end();
    if (relay_ && relay_->active) relay_->clear();  // 132on2e: the ticket died with the login
    contacts_open_ = contact_pending_ = false;
    game_cache_.clear();  // 0044d528: 0044f7e0(session + 118)
    game_cache_updates_.clear();
    log("online: left the server");
}

namespace {
// 004475e0: the login response's status -> the SFOLobby error.
std::int32_t login_error(std::int32_t st) {
    switch (st) {
    case online::status::InvalidPassword: return 0x3a;
    case online::status::AccountNotFound: return 0x3b;
    case online::status::AccountLoggedIn: return 0x3c;
    default: return 1;
    }
}
}  // namespace

// 00445a40: the login (00445420); -997 (logged in elsewhere) logs out and logs in once more; -991 logs out. On
// success the login's processing (0044ca30) and its tail 0044ce60: GetAccountStats of the own account, its record
// (else 00585e90) rebuilt (reason 0) with the lobby tail and uploaded (132on2d).
std::int32_t OnlineLobbyHost::login(const std::string& name, const std::string& password) {
    if (!login_core(name, password)) return 0;
    if (lobby_.get_account_stats(lobby_.account_id(), &response_) != online::status::Success) response_ = record_;
    build_record(0, response_);
    lobby_tail(response_);
    upload_record("login", response_);
    stats_update_ms_ = now_ms();
    return 1;
}

std::int32_t OnlineLobbyHost::login_core(const std::string& name, const std::string& password) {
    auto st = lobby_.login(name, password);
    if (st && *st == online::status::AccountLoggedIn) {
        log("online: '" + name + "' was logged in: logout and login again (00445a40)");
        lobby_.logout();
        st = lobby_.login(name, password);
    }
    if (!st) {
        error_ = 2;
        log("online: login '" + name + "' timed out: " + lobby_.last_error());
        return 0;
    }
    if (*st != online::status::Success) {
        if (*st == online::status::WMError) lobby_.logout();
        error_ = login_error(*st);
        log("online: login '" + name + "' status " + std::to_string(*st) + " -> error " + std::to_string(error_));
        return 0;
    }
    error_ = 0;
    state_ = 9;
    log("online: logged in as '" + name + "', account " + std::to_string(lobby_.account_id()));
    lobby_.client().pump(0);
    apply_relay_ticket();
    return 1;
}

// 00445b90: (the text filter 00443100 is skipped, PORT-ONLY: see the header), then AccountRegistration (00447270).
std::int32_t OnlineLobbyHost::register_account(const std::string& name, const std::string& password) {
    const auto st = lobby_.register_account(name, password);
    if (!st) {
        error_ = 2;
        log("online: registration '" + name + "' timed out");
        return 0;
    }
    if (*st != online::status::Success) {
        error_ = *st == online::status::AccountAlreadyExists ? 0x3d : 0xb;
        log("online: registration '" + name + "' status " + std::to_string(*st) + " -> error " + std::to_string(error_));
        return 0;
    }
    error_ = 0;
    log("online: account '" + name + "' created");
    return 1;
}

// 004459d0: a login and, when it succeeds, the logout (no statistics: it calls 00445420, not 00445a40).
std::int32_t OnlineLobbyHost::check_account(const std::string& name, const std::string& password) {
    const std::int32_t r = login_core(name, password);
    if (r) lobby_.logout();
    return r;
}

// 00442750: with the lobby logged in (state 9), the main channel's join; 1 on success. PORT-ONLY (132on2d): the
// channel join itself sends nothing; omega_server has one channel, joined at the login (132on1 read message 17 as
// the channel join; it is AccountUpdateStats, which the login's tail sends, ONLINE.md §3.6).
std::int32_t OnlineLobbyHost::enter() {
    if (!lobby_.logged_in()) {
        error_ = 1;
        return 0;
    }
    error_ = 0;
    state_ = 4;
    log("online: joined the lobby channel");
    return 1;
}

std::int32_t OnlineLobbyHost::state() {
    if (!lobby_.logged_in()) {
        state_ = 0;
        return 0;
    }
    return state_;
}

// 00442510: the mission's range check (session +58, the lobby's missions: the nine multiplayer missions, app +248
// 0..8 from the command centre's 004acb60), then the mission channel (PORT-ONLY: omega_server's one channel; see
// the header).
std::int32_t OnlineLobbyHost::mission(std::int32_t mission) {
    if (!lobby_.logged_in() || mission < 0 || mission >= 9) {
        error_ = 1;
        log("online: mission " + std::to_string(mission) + " refused");
        return 0;
    }
    error_ = 0;
    log("online: mission " + std::to_string(mission) + " (the lobby's one channel)");
    return 1;
}

// 0033cca0's lobby message +210 == 9 (the server's notice) has no counterpart on omega_server yet: the PORT-ONLY
// notices (ext::Notice) are kept for the later screens, and the poll answers none.
std::optional<std::string> OnlineLobbyHost::notice() {
    if (!lobby_.session()) return std::nullopt;
    lobby_.client().pump(0);
    for (auto& n : lobby_.client().take_notices()) {
        log("online: notice " + std::to_string(n.code) + ": " + n.text);
        notices_.push_back(std::move(n));
    }
    return std::nullopt;
}

// Checkpoint 132on2b stages B3 / B4 (research/ONLINE.md §9.4 / §9.5; the substitutions are the header's).

void OnlineLobbyHost::callback(std::int32_t a3, std::int32_t t0, const std::string& what) {
    callbacks_.push_back({a3, t0});
    log("online: " + what + " (callback a3 " + std::to_string(a3) + ")");
}

// 004426b0: in the main channel (4), the channel is left (state 9, the error cleared).
void OnlineLobbyHost::channel_leave() {
    if (state_ != 4) return;
    state_ = 9;
    error_ = 0;
    log("online: lobby channel left");
}

// 004518f0: from state 9, the game room (state 5); 0 otherwise.
std::int32_t OnlineLobbyHost::room_open() {
    if (!lobby_.logged_in() || state_ != 9) {
        error_ = 1;
        log("online: game room refused (state " + std::to_string(state_) + ")");
        return 0;
    }
    state_ = 5;
    error_ = 0;
    list_kind_ = 0;
    rows_.clear();
    player_sent_ms_ = 0;
    log("online: game room entered");
    return 1;
}

// 00451610: in state 5, 0 or 10 the room is left (9 unless 10), the error cleared and the room's lists freed.
void OnlineLobbyHost::room_leave() {
    if (state_ != 5 && state_ != 0 && state_ != 10) return;
    if (state_ != 10) state_ = 9;
    error_ = 0;
    list_kind_ = 0;
    list_pending_ = false;
    rows_.clear();
    rows_next_.clear();
    player_pending_ = false;
    player_sent_ms_ = 0;
    players_.clear();
    players_next_.clear();
    ++player_lists_;
    chat_.clear();
    log("online: game room left");
}

// 004515f0: the room's list kind (+1ea0; 1 the games).
void OnlineLobbyHost::list_kind(std::int32_t kind) {
    list_kind_ = kind;
    list_sent_ms_ = 0;  // a fresh list now
    service();
}

// 0044e960: the check (see the header), then the game's name "%s\t%d\t%d\t%d\t%d" (the account name +243d, agents,
// mission, filter, the account id +2480) and the CreateGame request. Its response arrives through service().
std::int32_t OnlineLobbyHost::create(std::int32_t mission, std::int32_t agents, std::int32_t filter) {
    if (!lobby_.logged_in() || state_ == 6 || state_ == 10 || !lan_ || lan_->hosting() || lan_->joining() ||
        pending_ != Pending::None) {
        error_ = 0x31;
        log("online: create refused (state " + std::to_string(state_) + ")");
        return 0;
    }
    error_ = 0;
    // 132on2e: the preferred game port (the original's 3658 by default), else a system-picked one.
    const lan::LanHostSetup setup{lobby_.account_name(), mission, agents, static_cast<std::uint8_t>(filter)};
    const std::uint16_t port_before = lan_->game_port();
    lan_->set_game_port(game_port_);
    bool hosted = lan_->host(setup);
    if (!hosted && game_port_ != 0) {
        log("online: game port " + std::to_string(game_port_) +
            " could not be opened (in use?): a system-picked port instead (online_game_port)");
        lan_->set_game_port(0);
        hosted = lan_->host(setup);
    }
    lan_->set_game_port(port_before);
    if (hosted) portmap_start(lan_->transport()->local_port());
    if (!hosted) {
        error_ = 0x32;
        callback(0, gui::lobby_key_f_to_creategame, "create: the LAN game could not be hosted");
        return 0;
    }
    online::GameNameFields f;
    f.name = lobby_.account_name();
    f.a = agents;
    f.b = mission;
    f.c = filter;
    f.d = static_cast<int>(lobby_.account_id());
    lobby_.send_create_game(online::format_game_name(f));
    pending_ = Pending::Create;
    log("online: CreateGame sent: mission " + std::to_string(mission) + ", agents " + std::to_string(agents) +
        ", filter " + std::to_string(filter) + ", LAN game port " + std::to_string(lan_->transport()->local_port()));
    // 0044e960's tail (132on2d): the record 00585e90 rebuilt (reason 1) with +ec the own account, +f0 *004976d8 (the
    // world: not yet known, the response sets it), +f9 the mission, +f8 the filter, +f4 the address (PORT-ONLY 0);
    // GameStats 00585e70 {account, mission, account, filter, 0, address}; then AccountUpdateStats. The response
    // to CreateGame stays queued (wait_for keeps the other frames).
    build_record(1, record_);
    online::put_i32(record_, 0xec, static_cast<std::int32_t>(lobby_.account_id()));
    online::put_i32(record_, 0xf0, static_cast<std::int32_t>(world_));
    online::put_i32(record_, 0xf4, 0);
    record_[0xf8] = static_cast<std::uint8_t>(filter);
    record_[0xf9] = static_cast<std::uint8_t>(mission);
    game_stats_ = {static_cast<std::int32_t>(lobby_.account_id()), mission,
                   static_cast<std::int32_t>(lobby_.account_id()), filter, 0, 0};
    game_name_ = online::format_game_name(f);
    game_agents_ = agents;
    upload_record("create", record_);
    return 1;
}

std::optional<gui::GuiHost::LobbyCallback> OnlineLobbyHost::pump() {
    service();
    if (callbacks_.empty()) return std::nullopt;
    const auto cb = callbacks_.front();
    callbacks_.pop_front();
    return cb;
}

// 0042b1f0: the DME game left; the port's LAN game closed and the server told.
void OnlineLobbyHost::game_cleanup() {
    if (lan_) {
        if (lan_->hosting()) lan_->leave();
        else lan_->cancel_join();
    }
    portmap_release();
    if (relay_ && (relay_->sent || relay_->received))
        log("online: relay counters: " + std::to_string(relay_->sent) + " datagram(s) sent (" +
            std::to_string(relay_->sent_bytes) + " bytes), " + std::to_string(relay_->received) + " received (" +
            std::to_string(relay_->received_bytes) + " bytes)");
    join_path_.clear();
    path_notice_.clear();
    relay_tried_ = false;
    if (world_ != 0 || joining_ != 0) lobby_.leave_game();
    log("online: game " + std::to_string(world_ ? world_ : joining_) + " left");
    world_ = 0;
    joining_ = 0;
    pending_ = Pending::None;
    level_ = false;
    report_player_ms_ = report_world_ms_ = level_start_ms_ = 0;
}

// ---- Checkpoint 132on2d: the statistics record (ONLINE.md §3.6) ------------------------------------------------

void OnlineLobbyHost::build_record(std::int32_t reason, online::StatsBlob& blob) {
    if (stats_builder_) {
        stats_builder_(std::span<std::uint8_t, online::kStatsBlobSize>(blob));
    } else if (blob[0] != online::kStatsBlobVersion) {  // 002cbd40's own head with no agent (tests)
        blob.fill(0);
        blob[0] = online::kStatsBlobVersion;
    }
    (void)reason;
}

void OnlineLobbyHost::upload_record(const char* why, const online::StatsBlob& blob) {
    const auto st = lobby_.account_update_stats(blob);
    log(std::string("online: stats uploaded (") + why + ", status " +
        (st ? std::to_string(*st) : std::string("none")) + "): " + online::describe_stats(blob));
}

void OnlineLobbyHost::lobby_tail(online::StatsBlob& blob) {
    online::put_i32(blob, 0xe8, -1);  // session +2484, the clan id: -1, no agency cell (see the header)
    online::put_i32(blob, 0xec, 0);
    online::put_i32(blob, 0xf0, 0);
    online::put_i32(blob, 0xf4, 0);  // 00443020's address (PORT-ONLY 0)
    blob[0xf8] = online::kStatsFilterNone;
    blob[0xf9] = 0;
}

void OnlineLobbyHost::entry_end() {
    if (!lobby_.logged_in()) return;
    if (state_ == 10) state_ = 9;
    build_record(3, record_);
    lobby_tail(record_);
    upload_record("game end", record_);
}

std::optional<std::int32_t> OnlineLobbyHost::player_rank(std::int32_t account) {
    const auto* c = lobby_.cached_stats(static_cast<std::uint32_t>(account));
    if (!c || std::chrono::steady_clock::now() - c->when >= std::chrono::milliseconds(0x7530)) return std::nullopt;
    return c->blob[1];
}

void OnlineLobbyHost::level_frame() {
    if (!lobby_.logged_in() || world_ == 0) return;
    lobby_.client().pump(0);
    portmap_poll();
    const std::uint64_t now = now_ms();
    if (!level_) {
        level_ = true;
        level_start_ms_ = now;
        log("online: in game " + std::to_string(world_) + (lan_ && lan_->hosting() ? " (host)" : ""));
        // 132on2f: the connection path again on the level's HUD (the front end's overlay is gone).
        path_notices_.clear();
        if (!path_notice_.empty()) path_notice(path_notice_);
    }
    if (report_player_ms_ == 0 || now >= report_player_ms_ + 15000) {
        report_player_ms_ = now;
        lobby_.send_player_report(static_cast<std::int32_t>(world_), record_);
        log("online: PlayerReport, world " + std::to_string(world_) + ": " + online::describe_stats(record_));
    }
    if (lan_ && lan_->hosting() && (report_world_ms_ == 0 || now >= report_world_ms_ + 15000)) {
        report_world_ms_ = now;
        game_stats_[4] = static_cast<std::int32_t>((now - level_start_ms_) / 1000);
        online::WorldReportFields w;
        w.world = static_cast<std::int32_t>(world_);
        w.player_count = 1 + static_cast<std::int32_t>(lan_->transport()->clients().size());
        w.game_name = game_name_;
        w.game_stats.resize(sizeof(game_stats_));
        for (std::size_t i = 0; i < game_stats_.size(); ++i)
            online::put_i32(w.game_stats, i * 4, game_stats_[i]);
        w.world_status = w.player_count < game_agents_ ? 1 : 3;
        w.generic[0] = w.world_status;
        lobby_.send_world_report(w);
        log("online: WorldReport, world " + std::to_string(world_) + ": " + std::to_string(w.player_count) +
            " player(s), " + std::to_string(game_stats_[4]) + " s, status " + std::to_string(w.world_status));
    }
}

void OnlineLobbyHost::level_end(bool completed) {
    if (!lobby_.logged_in() || world_ == 0) return;
    log("online: the level ended (" + std::string(completed ? "completed" : "not completed") + ")");
    entry_end();
    game_cleanup();
}

// 0044e060: the GameInfoResponse (status 0), its max players (+24), and GameStats' creator id (+34) and mission
// (+38), and its GameName (+134; 0033e5b0's another-host prompt reads it). 132on2i: 00446b40 caches it (0044edc0).
std::optional<gui::GuiHost::LobbyGameInfo> OnlineLobbyHost::game_info(std::int32_t world) {
    const auto r = lobby_.game_info(static_cast<std::uint32_t>(world));
    if (!r || online::get_i32(r->body, online::kStatusOffset) != online::status::Success) {
        log("online: game info " + std::to_string(world) + ": none");
        return std::nullopt;
    }
    cache_game(*r, "join");
    gui::GuiHost::LobbyGameInfo info;
    info.max_players = online::get_i32(r->body, 0x24);
    info.owner = online::get_i32(r->body, 0x34);
    info.mission = online::get_i32(r->body, 0x38);
    info.name = online::get_str(r->body, 0x134, 64);
    info.game_name = !info.name.empty();
    return info;
}

// ---- Checkpoint 132on2i: the game cache (session +118; ONLINE.md §3.7.1) -------------------------------------------

// 0044edc0(session, world +3c, players +30, max +24, status +184, GameStats +34): world 0 is not cached; an entry with
// the same players, max, status and first 0x18 GameStats bytes is left as it is (its time too); else it is updated
// (or appended, 0044f470) with the time, and 0044eff0 applies it to the lists (here: queued for game_cache_take).
void OnlineLobbyHost::cache_game(const online::Frame& f, const char* how) {
    gui::GuiHost::LobbyCachedGame g;
    g.world = online::get_i32(f.body, 0x3c);
    if (g.world == 0) return;
    g.players = online::get_i32(f.body, 0x30);
    g.max_players = online::get_i32(f.body, 0x24);
    for (std::size_t i = 0; i < g.stats.size(); ++i) g.stats[i] = online::get_i32(f.body, 0x34 + 4 * i);
    g.world_status = online::get_i32(f.body, 0x184);
    auto it = std::find_if(game_cache_.begin(), game_cache_.end(),
                           [&](const CachedGame& e) { return e.game.world == g.world; });
    if (it != game_cache_.end()) {
        const auto& o = it->game;
        if (o.players == g.players && o.max_players == g.max_players && o.world_status == g.world_status &&
            o.stats == g.stats)
            return;
        it->game = g;
        it->ms = now_ms();
    } else {
        game_cache_.push_back({g, now_ms()});
    }
    game_cache_updates_.push_back(g);
    log("online: game cache (" + std::string(how) + "): world " + std::to_string(g.world) + ", " +
        std::to_string(g.players) + "/" + std::to_string(g.max_players) + " player(s), mission " +
        std::to_string(g.stats[1]) + ", creator " + std::to_string(g.stats[0]) + ", status " +
        std::to_string(g.world_status));
}

std::optional<gui::GuiHost::LobbyCachedGame> OnlineLobbyHost::game_cached(std::int32_t world) {  // 0044f0c0
    if (world == 0) return std::nullopt;
    for (const auto& e : game_cache_)
        if (e.game.world == world) return e.game;
    return std::nullopt;
}

std::vector<gui::GuiHost::LobbyCachedGame> OnlineLobbyHost::game_cache_take() {
    service();
    auto out = std::move(game_cache_updates_);
    game_cache_updates_.clear();
    return out;
}

// 0044ebd0(session, list): nothing for no list or an empty one, or within *00497860 (SFOLOBBY_GAMEINFO_INTEL_INTERVAL,
// 5000 ms) of the last pass (*00585e50). Each record in a game (+24 not 3 / 2) whose player cache entry (00450a00)
// holds a world (the record's +f0) gets a GameInfo (MessageID "1", the callback 00446ae0) when that world's cache
// entry is missing or older than *00497878 (SFOLOBBY_GAMEINFO_REFRESH_INTERVAL, 5000 ms). The original's player cache
// holds a host's record with +f0 0 (the create uploads it before the world exists; message 64's room list and the
// stub 00453fa0 are no help), so a contact hosting a game is never fetched; PORT-ONLY: with no cached world the
// contact row's own world (the 215's +48) is asked.
void OnlineLobbyHost::contacts_game_info() {
    constexpr std::uint64_t intel_interval = 5000;    // *00497860
    constexpr std::uint64_t refresh_interval = 5000;  // *00497878
    if (contacts_.empty()) return;
    const std::uint64_t now = now_ms();
    if (game_info_fetch_ms_ != 0 && now - game_info_fetch_ms_ < intel_interval) return;
    game_info_fetch_ms_ = now;
    for (const auto& row : contacts_) {
        if (row.status != 3) continue;  // 00446830's kind 1
        std::int32_t world = 0;
        bool port_only = false;
        if (const auto* c = lobby_.cached_stats(static_cast<std::uint32_t>(row.id))) world = online::get_i32(c->blob, 0xf0);
        if (world == 0) {
            world = row.world;
            port_only = true;
        }
        if (world == 0) continue;
        const auto it = std::find_if(game_cache_.begin(), game_cache_.end(),
                                     [&](const CachedGame& e) { return e.game.world == world; });
        if (it != game_cache_.end() && now - it->ms <= refresh_interval) continue;
        lobby_.send_game_info_fetch(static_cast<std::uint32_t>(world));
        log("online: contact " + row.name + "'s game " + std::to_string(world) + ": GameInfo sent (0044ebd0" +
            (port_only ? ", PORT-ONLY world from the contact row)" : ")"));
    }
}

// 0044e430: the JoinGame request (state 6); its response, then the LAN join, arrive through service().
std::int32_t OnlineLobbyHost::join(std::int32_t world) {
    if (!lobby_.logged_in() || !lan_ || lan_->hosting() || lan_->joining() || pending_ != Pending::None) {
        error_ = 1;
        log("online: join " + std::to_string(world) + " refused");
        return 0;
    }
    error_ = 0;
    password_refused_ = false;
    joining_ = static_cast<std::uint32_t>(world);
    // 0044e430 (132on2d): the record 00585e90 rebuilt (reason 2) with the game row's host account (+ec), world (+f0),
    // filter (+f8) and mission (+f9), uploaded before the JoinGame request.
    build_record(2, record_);
    online::put_i32(record_, 0xf0, world);
    for (const auto& row : rows_) {
        if (row.world != world) continue;
        if (const auto g = online::parse_game_name(row.name)) {
            online::put_i32(record_, 0xec, g->d);
            record_[0xf8] = static_cast<std::uint8_t>(g->c);
            record_[0xf9] = static_cast<std::uint8_t>(g->b);
        }
        break;
    }
    upload_record("join", record_);
    lobby_.send_join_game(joining_, join_password_);
    pending_ = Pending::JoinRequest;
    state_ = 6;
    log("online: JoinGame sent: world " + std::to_string(world) +
        (join_password_.empty() ? std::string() : " (with a game password, port-only)"));
    return 1;
}

// 0044e340: the join abandoned (the record's game end, then the join cancelled).
void OnlineLobbyHost::join_end() {
    entry_end();
    if (lan_ && lan_->joining()) lan_->cancel_join();
    if (joining_ != 0) lobby_.leave_game();
    log("online: join " + std::to_string(joining_) + " abandoned");
    joining_ = 0;
    pending_ = Pending::None;
}

void OnlineLobbyHost::service() {
    if (!lobby_.session()) return;
    if (lan_) lan_->pump();
    auto& client = lobby_.client();
    client.pump(0);  // wait_for(id, 0) reads only the frames already queued
    apply_relay_ticket();
    portmap_poll();
    contacts_service();
    // 00451b30 (states 4 / 5 / 6 / 7, 132on2d): every *00497830 (0x9c40) ms the own record fetched (GetAccountStats;
    // else 00585e90), its +e8 set and uploaded again, and cached (00450880). The clock (+2590) is restarted here and
    // at the login (its store was not found; PORT-ONLY placement).
    if (state_ >= 4 && state_ <= 7 && lobby_.logged_in() && now_ms() - stats_update_ms_ > 0x9c40) {
        stats_update_ms_ = now_ms();
        online::StatsBlob& blob =
            lobby_.get_account_stats(lobby_.account_id(), &response_) == online::status::Success ? response_ : record_;
        online::put_i32(blob, 0xe8, 0);
        upload_record("lobby refresh", blob);
    }
    if (pending_ == Pending::Create) {
        if (const auto r = client.wait_for(online::mid::CreateGameResponse, 0)) {
            pending_ = Pending::None;
            const std::int32_t st = r->body.size() >= online::msize::CreateGameResponse
                                        ? online::get_i32(r->body, online::kStatusOffset)
                                        : -1;
            if (st == online::status::Success && lan_ && lan_->hosting()) {
                world_ = static_cast<std::uint32_t>(online::get_i32(r->body, 0x1c));
                send_host_endpoint();
                if (!game_password_.empty()) lobby_.game_password(world_, game_password_);
                state_ = 10;
                callback(1, 0, "game " + std::to_string(world_) + " created, LAN game port " +
                                   std::to_string(lan_->transport()->local_port()) + " sent (HostEndpoint)" +
                                   (game_password_.empty() ? std::string() : ", password-protected (port-only)"));
            } else {
                if (lan_) lan_->leave();
                portmap_release();
                callback(0, gui::lobby_key_f_to_creategame, "CreateGame status " + std::to_string(st));
            }
        }
    }
    if (pending_ == Pending::JoinRequest) {
        if (const auto r = client.wait_for(online::mid::JoinGameResponse, 0)) {
            const std::int32_t st = r->body.size() >= online::msize::JoinGameResponse
                                        ? online::get_i32(r->body, online::kStatusOffset)
                                        : -1;
            if (st != online::status::Success) {
                pending_ = Pending::None;
                password_refused_ = st == online::status::InvalidPassword;
                callback(0, gui::lobby_key_f_to_joingame,
                         "JoinGame status " + std::to_string(st) + (password_refused_ ? " (the game password)" : ""));
            } else {
                const online::ConnectInfo ci = online::read_connect_info(r->body, 0x20);
                const auto ip = parse_ipv4(ci.ip);
                relay_tried_ = false;
                join_path_.clear();
                // 132on2e: online_relay=force goes through the relay at once; otherwise the direct join first.
                if (relay_mode_ == RelayMode::Force && join_through_relay("online_relay=force")) {
                } else if (ip && ci.port != 0 && lan_ && lan_->join_at({*ip, ci.port}, lobby_.account_name())) {
                    pending_ = Pending::JoinConnect;
                    join_path_ = "direct";
                    log("online: JoinGame accepted: LAN JOIN_REQ to " + ci.ip + ":" + std::to_string(ci.port) +
                        " (direct" + (relay_mode_ == RelayMode::Auto && relay_ && relay_->active
                                          ? std::string(", the relay if it has no answer)")
                                          : std::string(")")));
                } else if (relay_mode_ == RelayMode::Auto && join_through_relay("the direct join could not start")) {
                } else {
                    pending_ = Pending::None;
                    callback(0, gui::lobby_key_f_to_connect, "the LAN join to " + ci.ip + ":" +
                                                                 std::to_string(ci.port) + " could not start");
                }
            }
        }
    }
    if (pending_ == Pending::JoinConnect && lan_) {
        switch (lan_->join_state()) {
        case lan::LanJoinState::Accepted:
            pending_ = Pending::None;
            world_ = joining_;
            log("online: in-level path: " +
                (join_path_ == "relay" ? std::string("through omega_server's relay (port-only)")
                                       : std::string("direct to the host")));
            path_notice(join_path_ == "relay" ? "Connection: relay through omega_server" : "Connection: direct to the host");
            callback(1, 0, "joined game " + std::to_string(world_) + " (LAN JOIN_ACK, client index " +
                               std::to_string(lan_->transport()->local_index()) + ", " + join_path_ + ")");
            break;
        case lan::LanJoinState::Rejected:
            pending_ = Pending::None;
            callback(0, gui::lobby_key_f_to_joingame, "the LAN join was refused (JOIN_NAK)");
            break;
        case lan::LanJoinState::Failed:
        case lan::LanJoinState::Idle:
            if (relay_mode_ == RelayMode::Auto && !relay_tried_ && join_through_relay("the direct join had no answer"))
                break;
            pending_ = Pending::None;
            callback(0, gui::lobby_key_f_to_connect, "the LAN join had no answer");
            break;
        default: break;
        }
    }
    // The room's game list (list kind 1).
    if (state_ == 5 && list_kind_ == 1) {
        const std::uint64_t now = now_ms();
        if (!list_pending_ && (list_sent_ms_ == 0 || now - list_sent_ms_ >= 1000)) {
            lobby_.send_game_list();
            list_pending_ = true;
            list_sent_ms_ = now;
            rows_next_.clear();
        }
        while (list_pending_) {
            const auto f = client.wait_for(online::mid::GameListResponse, 0);
            if (!f) break;
            if (f->body.size() < online::msize::GameListResponse) continue;
            if (online::get_i32(f->body, online::kStatusOffset) == online::status::Success) {
                gui::GuiHost::LobbyGameRow row;
                row.world = online::get_i32(f->body, 0x1c);
                row.name = online::get_str(f->body, 0x20, 64);
                if (auto g = online::parse_game_name(row.name)) {  // PORT-ONLY (132on2c): the host's badge
                    const std::string shown = shown_name(g->name, g->d, 32);
                    if (shown != g->name) {
                        g->name = shown;
                        row.name = online::format_game_name(*g);
                    }
                }
                row.status = online::get_i32(f->body, 0x60);
                row.players = online::get_i32(f->body, 0x68);
                rows_next_.push_back(std::move(row));
            }
            if (f->body[0x6c] != 0) {
                list_pending_ = false;
                if (rows_next_.size() != rows_.size())
                    log("online: game list: " + std::to_string(rows_next_.size()) + " game(s)");
                rows_ = std::move(rows_next_);
                ++lists_;
                rows_next_.clear();
            }
        }
    }
    // 004516e0: the room's player list, about once a second in state 5 (00447a30 receives the rows).
    if (state_ == 5) {
        const std::uint64_t now = now_ms();
        if (!player_pending_ && (player_sent_ms_ == 0 || now - player_sent_ms_ >= 1000)) {
            lobby_.send_player_list(0);
            player_pending_ = true;
            player_sent_ms_ = now;
            players_next_.clear();
        }
    }
    while (const auto f = client.wait_for(online::mid::PlayerListResponse, 0)) {
        if (f->body.size() < online::msize::PlayerListResponse || !player_pending_) continue;
        if (online::get_i32(f->body, online::kStatusOffset) == online::status::Success) {
            gui::GuiHost::LobbyPlayerRow row;
            row.id = online::get_i32(f->body, 0x1c);
            row.name = shown_name(online::get_str(f->body, 0x20, 32), row.id, 0x1f);  // PORT-ONLY badge (132on2c)
            row.status = online::get_i32(f->body, 0x40);
            players_next_.push_back(std::move(row));
        }
        if (f->body[0xcc] != 0) {
            player_pending_ = false;
            if (players_next_.size() != players_.size()) {
                std::string names;
                for (const auto& p : players_next_) names += (names.empty() ? ": " : ", ") + p.name;
                log("online: player list: " + std::to_string(players_next_.size()) + " player(s)" + names);
            }
            players_ = std::move(players_next_);
            ++player_lists_;
            players_next_.clear();
        }
    }
    // 132on2h: 0044d700 in the contacts' state 6, every *004977b8 (0x4e20) ms (00446830 receives the rows).
    if (state_ == 6 && contacts_open_ && !contact_pending_ &&
        (contact_sent_ms_ == 0 || now_ms() - contact_sent_ms_ >= 0x4e20)) {
        lobby_.send_buddy_list();
        contact_pending_ = true;
        contact_sent_ms_ = now_ms();
        contacts_next_.clear();
    }
    while (const auto f = client.wait_for(online::mid::GetBuddyListExtraInfoResponse, 0))
        if (contact_pending_) contact_frame(*f);
    // 132on2i: 0044de60 (the contacts' tick) runs 0044ebd0 over +e0; 00446ae0 caches a response with status +18 0.
    if (state_ == 6 && contacts_open_ && lobby_.logged_in()) contacts_game_info();
    for (const auto& f : lobby_.take_game_info_fetches())
        if (online::get_i32(f.body, online::kStatusOffset) == online::status::Success) cache_game(f, "contacts");
    while (const auto f = client.wait_for(online::mid::AddToBuddyListConfirmationResponse, 0))
        if (f->body.size() >= online::msize::AddToBuddyListConfirmationResponse)
            log("online: contact invitation answered, status " +
                std::to_string(online::get_i32(f->body, online::kStatusOffset)) + " (68, 004466b0)");
    // 00447bb0: the sender's TextFilter echo (status +58 0 or 4) is its own line: the MessageID (the target's id as
    // text), its name, the filtered text +15, private when the target is not 0.
    while (const auto f = client.wait_for(online::mid::TextFilterResponse, 0)) {
        if (f->body.size() < online::msize::TextFilterResponse || !room_chat()) continue;
        const std::int32_t st = online::get_i32(f->body, 0x58);
        if (st != 0 && st != 4) continue;
        gui::GuiHost::LobbyChatLine line;
        line.id = std::atoi(online::get_str(f->body, 0, online::kMessageIdLength).c_str());
        line.text = online::get_str(f->body, 0x15, 64);
        line.whisper = line.id != 0;
        log("online: chat (own) " + lobby_.account_name() + ": " + line.text);
        line.name = shown_name(lobby_.account_name(), own_id(), 0xf);  // PORT-ONLY badge (132on2c)
        chat_.push_back(std::move(line));
    }
    // 00447c70: another agent's line (+18 id, +1c name, +3c type 1 private, +40 text): to the room's chat in state 5;
    // outside it a private line is the lobby message of type 9 (NETMESS), any other is dropped.
    while (const auto f = client.wait_for(online::mid::ChatFwdMessage, 0)) {
        if (f->body.size() < online::msize::ChatFwdMessage) continue;
        gui::GuiHost::LobbyChatLine line;
        line.id = online::get_i32(f->body, 0x18);
        line.name = online::get_str(f->body, 0x1c, 32);
        line.whisper = online::get_i32(f->body, 0x3c) == 1;
        line.text = online::get_str(f->body, 0x40, 64);
        if (room_chat()) {
            log("online: chat from " + line.name + (line.whisper ? " (private)" : "") + ": " + line.text);
            line.name = shown_name(line.name, line.id, 0xf);  // PORT-ONLY badge (132on2c)
            chat_.push_back(std::move(line));
        } else if (line.whisper) {
            gui::GuiHost::LobbyMessage m;
            m.type = 9;
            m.id = line.id;
            m.from = line.name;
            m.text = line.text;
            log("online: private chat from " + line.name + " outside the room: lobby message 9");
            messages_.push_back(std::move(m));
            messages_changed_ = true;
        }
    }
}

// 00451050: with no room registered error 0x24 and 0; an empty text 0; else TextFilter, then (to another agent or
// the room) ChatMessage, and 1.
std::int32_t OnlineLobbyHost::chat_send(const std::string& text, std::int32_t target) {
    if (!room_chat() || !lobby_.logged_in()) {
        error_ = 0x24;
        log("online: chat refused (state " + std::to_string(state_) + ")");
        return 0;
    }
    if (text.empty()) return 0;
    lobby_.send_chat(text, target);
    log("online: chat sent" + (target ? " to " + std::to_string(target) : std::string()) + ": " + text);
    return 1;
}

std::vector<gui::GuiHost::LobbyChatLine> OnlineLobbyHost::chat_take() {
    service();
    auto out = std::move(chat_);
    chat_.clear();
    return out;
}

// ---- Checkpoint 132on2h: CONTACTS (GuiNetContacts; ONLINE.md §3.7) ------------------------------------------------

// 00446830: one GetBuddyList_ExtraInfoResponse. Status 0 rows join the list being received (+1c id, +20 name, +40 the
// player status, +48 the game world); the EndOfList (+cc) publishes it (00446830 copies it to the registered +c8).
bool OnlineLobbyHost::contact_frame(const online::Frame& f) {
    if (f.body.size() < online::msize::GetBuddyListExtraInfoResponse) return false;
    if (online::get_i32(f.body, online::kStatusOffset) == online::status::Success) {
        gui::GuiHost::LobbyContactRow row;
        row.id = online::get_i32(f.body, 0x1c);
        row.name = online::get_str(f.body, 0x20, 32);
        row.status = online::get_i32(f.body, 0x40);
        row.world = online::get_i32(f.body, 0x48);
        contacts_next_.push_back(std::move(row));
    }
    if (f.body[0xcc] == 0) return false;
    contact_pending_ = false;
    if (contacts_next_.size() != contacts_.size()) {
        std::string names;
        for (const auto& c : contacts_next_)
            names += (names.empty() ? ": " : ", ") + c.name + " (status " + std::to_string(c.status) + ")";
        log("online: contact list: " + std::to_string(contacts_next_.size()) + " contact(s)" + names);
    }
    contacts_ = std::move(contacts_next_);
    contacts_next_.clear();
    ++contact_lists_;
    return true;
}

// 0044df80: in state 9 or 0 the lists are registered, the state 6 and the first list asked; else error 0x21.
std::int32_t OnlineLobbyHost::contacts_open() {
    if (!lobby_.logged_in() || (state_ != 9 && state_ != 0)) {
        error_ = 0x21;
        log("online: contacts refused (state " + std::to_string(state_) + ")");
        return 0;
    }
    state_ = 6;
    error_ = 0;
    contacts_open_ = true;
    chat_.clear();
    contacts_.clear();
    contacts_next_.clear();
    ++contact_lists_;
    lobby_.send_buddy_list();
    contact_pending_ = true;
    contact_sent_ms_ = now_ms();
    log("online: contacts opened: GetBuddyList_ExtraInfo sent (214, 0044df80)");
    return 1;
}

// 0044dd20: in state 6 / 10 the lists are freed (6 -> 9).
void OnlineLobbyHost::contacts_close() {
    if (state_ != 6 && state_ != 10) return;
    if (!contacts_open_) return;  // a join's state 6 registered no contact lists
    if (state_ == 6) state_ = 9;
    contacts_open_ = false;
    contact_pending_ = false;
    contacts_.clear();
    contacts_next_.clear();
    ++contact_lists_;
    chat_.clear();
    log("online: contacts closed");
}

// 0044d9c0: id < 0 -> error 0x29; RemoveFromBuddyList, waited for (none: error 2); status 0 -> 1 and the list refreshed
// at the next update (+257c = 0), else error 0x29.
std::int32_t OnlineLobbyHost::contact_remove(std::int32_t id) {
    if (id < 0) {
        error_ = 0x29;
        return 0;
    }
    if (!lobby_.logged_in()) {
        error_ = 1;
        return 0;
    }
    const auto st = lobby_.remove_contact(id);
    if (!st) {
        error_ = 2;
        log("online: contact " + std::to_string(id) + " not removed (" + lobby_.last_error() + ")");
        return 0;
    }
    contact_sent_ms_ = 0;
    if (*st != online::status::Success) {
        error_ = 0x29;
        log("online: contact " + std::to_string(id) + " not removed (status " + std::to_string(*st) + ", 66)");
        return 0;
    }
    error_ = 0;
    log("online: contact " + std::to_string(id) + " removed (65 -> 66)");
    return 1;
}

// 00451370: states 5 / 6 / 7 only (else error 0x23), a name of 1..31 characters (else 0x22); AccountGetID waited for
// (not sent: 1; none: 2); its status +1c not 0 -> 0x1d; else the AccountID +18. -1 fails. (The original first searches
// its record cache +fc by name; the port's cache is keyed by account, so it always asks.)
std::int32_t OnlineLobbyHost::account_id(const std::string& name) {
    if (state_ != 5 && state_ != 6 && state_ != 7) {
        error_ = 0x23;
        return -1;
    }
    if (name.empty() || name.size() >= 0x20) {
        error_ = 0x22;
        return -1;
    }
    if (!lobby_.logged_in()) {
        error_ = 1;
        return -1;
    }
    const auto r = lobby_.account_get_id(name);
    if (!r) {
        error_ = 2;
        log("online: AccountGetID '" + name + "': " + lobby_.last_error());
        return -1;
    }
    if (online::get_i32(r->body, 0x1c) != 0) {
        error_ = 0x1d;
        log("online: AccountGetID '" + name + "': no such agent (23 -> 24)");
        return -1;
    }
    error_ = 0;
    const std::int32_t id = online::get_i32(r->body, 0x18);
    log("online: AccountGetID '" + name + "' -> " + std::to_string(id) + " (23 -> 24)");
    return id;
}

// 0044db60: id < 0 -> error 0x1d, the agent's own -> 0x25; the contact list refreshed, waited for (0044d860(1)); a
// contact already -> 0x1e; the invitee's record asked (00451200, GetAccountStats; failed: 1); then
// AddToBuddyListConfirmation (258) is sent and 1 answered (its response 68 only notes completion, 004466b0).
std::int32_t OnlineLobbyHost::contact_add(std::int32_t id) {
    if (id < 0) {
        error_ = 0x1d;
        return 0;
    }
    if (id == own_id()) {
        error_ = 0x25;
        return 0;
    }
    if (!lobby_.logged_in()) {
        error_ = 1;
        return 0;
    }
    auto& client = lobby_.client();
    lobby_.send_buddy_list();
    contact_pending_ = true;
    contacts_next_.clear();
    while (const auto f = client.wait_for(online::mid::GetBuddyListExtraInfoResponse, lobby_.timeout_ms()))
        if (contact_frame(*f)) break;
    contact_sent_ms_ = now_ms();
    for (const auto& c : contacts_)
        if (c.id == id) {
            error_ = 0x1e;
            log("online: contact " + std::to_string(id) + " not invited: a contact already");
            return 0;
        }
    if (!lobby_.get_account_stats(static_cast<std::uint32_t>(id))) {
        error_ = 1;
        log("online: contact " + std::to_string(id) + " not invited (" + lobby_.last_error() + ")");
        return 0;
    }
    lobby_.send_buddy_add(id);
    error_ = 0;
    log("online: contact invitation sent to " + std::to_string(id) + " (258, 0044db60)");
    return 1;
}

// 00450c60: the record cache first (00450cf0: a record younger than 00497848, 30000 ms), else GetAccountStats
// (00451200), whose record is handed back whatever its version byte (00360610 tests it: 0xb, else no statistics).
bool OnlineLobbyHost::agent_stats(std::int32_t id, std::array<std::uint8_t, 0xfc>& out) {
    if (!lobby_.logged_in()) {
        error_ = 1;
        return false;
    }
    const auto* c = lobby_.cached_stats(static_cast<std::uint32_t>(id));
    if (c && std::chrono::steady_clock::now() - c->when < std::chrono::milliseconds(0x7530)) {
        std::copy(c->blob.begin(), c->blob.end(), out.begin());
        log("online: agent " + std::to_string(id) + "'s record from the cache: " + online::describe_stats(out));
        return true;
    }
    online::StatsBlob blob{};
    const auto st = lobby_.get_account_stats(static_cast<std::uint32_t>(id), &blob);
    if (!st || *st != online::status::Success) {
        error_ = 1;
        log("online: agent " + std::to_string(id) + "'s record not received (" +
            (st ? "status " + std::to_string(*st) : lobby_.last_error()) + ")");
        return false;
    }
    std::copy(blob.begin(), blob.end(), out.begin());
    log("online: agent " + std::to_string(id) + "'s record (GetAccountStats): " + online::describe_stats(out));
    return true;
}

// 0044d670(id, 1): the agent is in the contact lists (the session's +150 / +e0, which 0044d860 refreshes).
bool OnlineLobbyHost::is_contact(std::int32_t id) {
    for (const auto& c : contacts_)
        if (c.id == id) return true;
    return false;
}

// 0044ac10: only an agency cell's leader invites (AGNTINFO enables "02" only then); PORT-ONLY no cells: not sent.
std::int32_t OnlineLobbyHost::cell_invite(std::int32_t id) {
    error_ = 2;
    log("online: agency cell invitation to " + std::to_string(id) + " not sent (omega_server has no agency cells)");
    return 0;
}

// 0044af60: a name longer than 0x20 -> error 0x1b (before any request); PORT-ONLY no cells: as CreateClan's timeout
// (error 2, CMDCENTR's LOBBY_STR 0x63).
std::int32_t OnlineLobbyHost::cell_create(const std::string& name) {
    if (name.size() > 0x20) {
        error_ = 0x1b;
        return 0;
    }
    error_ = 2;
    log("online: agency cell '" + name + "' not created (omega_server has no agency cells)");
    return 0;
}

// ---- Checkpoint 132on2e: the relay, the game port, UPnP and the agent edit's upload (see the header) ----------

OnlineLobbyHost::~OnlineLobbyHost() {
    if (relay_) relay_->log = nullptr;  // the link may outlive this host (the LanSession's sockets hold it)
    portmap_release();
    if (portmap_renewing_.valid()) portmap_renewing_.wait();
    for (auto& f : portmap_unmaps_)
        if (f.valid()) f.wait();
}

void OnlineLobbyHost::set_relay(RelayMode mode, std::shared_ptr<RelayLink> link) {
    relay_mode_ = mode;
    relay_ = std::move(link);
    if (relay_ && !relay_->log) relay_->log = [this](const std::string& line) { log("online: " + line); };
}

void OnlineLobbyHost::apply_relay_ticket() {
    if (!relay_ || relay_mode_ == RelayMode::Off) return;
    const auto& t = lobby_.client().relay_ticket();
    if (!t) {
        if (relay_->active && !lobby_.session()) relay_->clear();
        return;
    }
    if (relay_->active && relay_->vip == t->vip && relay_->token == t->token) return;
    relay_->set_ticket(lobby_.client().server_ip(), *t);
    log("online: relay ticket: omega_server's relay at " + lan::lan_address_text(relay_->server) + ", virtual address " +
        online::relay_vip_text(t->vip) + " (port-only, online_relay=" +
        (relay_mode_ == RelayMode::Force ? "force" : "auto") + ")");
}

bool OnlineLobbyHost::join_through_relay(const char* why) {
    if (!lan_ || !relay_ || !relay_->active || joining_ == 0) return false;
    const auto peer = lobby_.client().relay_peer(joining_);
    if (!peer) {
        log(std::string("online: no relay address for game ") + std::to_string(joining_) + " (" + why + ")");
        return false;
    }
    if (lan_->joining()) lan_->cancel_join();
    if (!lan_->join_at({peer->vip, peer->port}, lobby_.account_name())) return false;
    relay_tried_ = true;
    join_path_ = "relay";
    pending_ = Pending::JoinConnect;
    log("online: JoinGame accepted: LAN JOIN_REQ via omega_server's relay to " + online::relay_vip_text(peer->vip) + ":" +
        std::to_string(peer->port) + " (" + why + ")");
    return true;
}

void OnlineLobbyHost::set_portmap(bool on, bool loopback, online::PortMapOptions options) {
    portmap_ = on;
    portmap_loopback_ = loopback;
    portmap_options_ = std::move(options);
}

namespace {
std::string portmap_methods(const online::PortMapOptions& o) {
    std::string s;
    for (const auto& [on, name] : {std::pair{o.upnp, "UPnP"}, std::pair{o.pcp, "PCP"}, std::pair{o.natpmp, "NAT-PMP"}})
        if (on) s += (s.empty() ? "" : ", then ") + std::string(name);
    return s.empty() ? std::string("none") : s;
}
}  // namespace

void OnlineLobbyHost::portmap_start(std::uint16_t port) {
    if (!portmap_) {
        path_notice("Connection: hosting on UDP " + std::to_string(port) + " (port mapping off - see the port-forward guide)");
        return;
    }
    // A loopback gateway (--online-portmap-test's fake one) is reachable from a loopback socket; a router is not.
    const bool loopback_gateway = (portmap_options_.gateway_ip >> 24) == 127;
    if (portmap_loopback_ && !loopback_gateway) {
        log("online: port mapping (UPnP / PCP / NAT-PMP) skipped: the game socket is bound to loopback (--lan-address)");
        path_notice("Connection: hosting on UDP " + std::to_string(port) + " (loopback - no port mapping)");
        return;
    }
    if (portmap_pending_.valid() || (portmap_mapping_ && portmap_mapping_->mapped)) portmap_release();
    log("online: port mapping: asking the router to forward UDP port " + std::to_string(port) + " (" +
        portmap_methods(portmap_options_) + "; online_upnp=1, port-only)");
    const online::PortMapOptions o = portmap_options_;
    portmap_pending_ = std::async(std::launch::async, [port, o] {
                           PortMapResult r;
                           r.mapping = online::portmap_udp(port, o, [&r](const std::string& line) { r.steps.push_back(line); });
                           return r;
                       }).share();
}

void OnlineLobbyHost::portmap_poll() {
    const auto ready = [](const auto& f) {
        return f.valid() && f.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
    };
    if (ready(portmap_pending_)) {
        const PortMapResult r = portmap_pending_.get();
        portmap_pending_ = {};
        for (const auto& line : r.steps) log("online: port mapping: " + line);
        portmap_mapping_ = r.mapping;
        portmap_granted_ms_ = now_ms();
        const auto& m = *portmap_mapping_;
        if (!m.mapped) {
            log("online: port mapping failed (" + m.error + "); joiners outside this network need a manual port " +
                "forward of UDP " + std::to_string(m.internal_port) + " (the port-forward guide) or the relay");
            path_notice("Connection: port mapping failed - see the port-forward guide (UDP " +
                        std::to_string(m.internal_port) + ")");
            return;
        }
        log("online: port mapping: " + std::string(online::portmap_method_name(m.method)) + ": the gateway " + m.gateway +
            " forwards UDP " + (m.external_ip.empty() ? "" : m.external_ip + ":") + std::to_string(m.external_port) +
            " to " + lan::lan_address_text({m.internal_ip, m.internal_port}) +
            (m.lifetime ? " (lifetime " + std::to_string(m.lifetime) + " s, renewed at half)" : std::string()));
        path_notice("Connection: " + std::string(online::portmap_method_name(m.method)) + " mapped ext port " +
                    std::to_string(m.external_port));
        if (world_ != 0 && lan_ && lan_->hosting()) send_host_endpoint();
        return;
    }
    if (ready(portmap_renewing_)) {
        const online::PortMapping r = portmap_renewing_.get();
        portmap_renewing_ = {};
        portmap_granted_ms_ = now_ms();
        const bool moved = portmap_mapping_ && r.external_port != portmap_mapping_->external_port;
        portmap_mapping_ = r;
        if (!r.mapped) {
            log("online: port mapping: " + r.error + "; joiners outside this network need a manual port forward of UDP " +
                std::to_string(r.internal_port) + " or the relay");
            path_notice("Connection: port mapping failed - see the port-forward guide (UDP " +
                        std::to_string(r.internal_port) + ")");
            if (world_ != 0 && lan_ && lan_->hosting()) send_host_endpoint();
            return;
        }
        log("online: port mapping: " + std::string(online::portmap_method_name(r.method)) + " renewed: UDP " +
            std::to_string(r.external_port) + ", lifetime " + std::to_string(r.lifetime) + " s");
        if (moved && world_ != 0 && lan_ && lan_->hosting()) send_host_endpoint();
        return;
    }
    // PCP / NAT-PMP: the same request again at half the granted lifetime (RFC 6886 §3.3, RFC 6887 §11.2.1).
    if (portmap_mapping_ && portmap_mapping_->mapped && portmap_mapping_->lifetime != 0 && !portmap_renewing_.valid() &&
        portmap_mapping_->method != online::PortMapMethod::Upnp &&
        now_ms() >= portmap_granted_ms_ + std::uint64_t(portmap_mapping_->lifetime) * 500) {
        const online::PortMapping m = *portmap_mapping_;
        const online::PortMapOptions o = portmap_options_;
        log("online: port mapping: renewing the " + std::string(online::portmap_method_name(m.method)) + " mapping of UDP " +
            std::to_string(m.external_port));
        portmap_renewing_ = std::async(std::launch::async, [m, o] { return online::portmap_renew(m, o); }).share();
    }
}

void OnlineLobbyHost::portmap_release() {
    const online::PortMapOptions o = portmap_options_;
    if (portmap_pending_.valid()) {
        auto pending = portmap_pending_;
        portmap_pending_ = {};
        portmap_unmaps_.push_back(
            std::async(std::launch::async, [pending, o] { online::portmap_unmap(pending.get().mapping, o); }));
        log("online: port mapping: the mapping still in progress is removed when it completes");
    }
    if (portmap_renewing_.valid()) {
        auto renewing = portmap_renewing_;
        portmap_renewing_ = {};
        const online::PortMapping old = portmap_mapping_.value_or(online::PortMapping{});
        portmap_unmaps_.push_back(std::async(std::launch::async, [renewing, old, o] {
            const online::PortMapping r = renewing.get();
            online::portmap_unmap(r.mapped ? r : old, o);
        }));
        log("online: port mapping: the renewal still in progress is followed by the removal");
    } else if (portmap_mapping_ && portmap_mapping_->mapped) {
        const online::PortMapping m = *portmap_mapping_;
        portmap_unmaps_.push_back(std::async(std::launch::async, [m, o] { online::portmap_unmap(m, o); }));
        log("online: port mapping: removing the " + std::string(online::portmap_method_name(m.method)) +
            " forward of UDP " + std::to_string(m.external_port));
    }
    portmap_mapping_.reset();
}

void OnlineLobbyHost::send_host_endpoint() {
    if (!lan_ || !lan_->transport()) return;
    const std::uint16_t port = lan_->transport()->local_port();
    const bool mapped = portmap_mapping_ && portmap_mapping_->mapped;
    lobby_.host_endpoint(world_, port, mapped ? portmap_mapping_->external_port : 0,
                         mapped ? portmap_mapping_->external_ip : std::string());
    log("online: HostEndpoint: world " + std::to_string(world_) + ", UDP port " + std::to_string(port) +
        (mapped ? ", external " + portmap_mapping_->external_ip + ":" + std::to_string(portmap_mapping_->external_port) +
                      " (" + online::portmap_method_name(portmap_mapping_->method) + ")"
                : std::string()));
}

// 132on2f: a port session that negotiated cap::HostEndpoint (the hello's); an original-kind session never.
bool OnlineLobbyHost::path_notices_allowed() const {
    const auto& c = lobby_.client();
    return c.welcomed() && (c.caps() & online::cap::HostEndpoint) != 0;
}

void OnlineLobbyHost::path_notice(std::string text) {
    if (!path_notices_allowed()) return;
    path_notice_ = text;
    log("online: connection notice (port-only): " + text);
    path_notices_.push_back(std::move(text));
}

void OnlineLobbyHost::agent_edited() {
    if (!lobby_.logged_in()) return;
    online::StatsBlob blob = record_;  // 0033fc80: 00585e90 copied (0xfc bytes), 002cbd40 into the copy
    build_record(0, blob);
    upload_record("agent edit", blob);
}

}  // namespace omega