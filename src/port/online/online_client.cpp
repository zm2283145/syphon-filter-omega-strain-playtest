// PORT-ONLY (checkpoint 132on1): see online_client.hpp.
#include "online_client.hpp"

#include <algorithm>
#include <chrono>

namespace omega::online {

bool OnlineClient::connect(const std::string& host_port, std::string* error, int timeout_ms) {
    const auto hp = split_host_port(host_port, kDefaultServerPort);
    if (!hp) {
        if (error) *error = "bad server address '" + host_port + "'";
        return false;
    }
    const auto ip = resolve_ipv4(hp->first);
    if (!ip) {
        if (error) *error = "cannot resolve '" + hp->first + "'";
        return false;
    }
    server_ip_ = *ip;
    relay_ticket_.reset();
    relay_peers_.clear();
    return tcp_.connect(*ip, hp->second, timeout_ms, error);
}

void OnlineClient::disconnect() {
    tcp_.disconnect();
    relay_ticket_.reset();
    relay_peers_.clear();
}

std::optional<RelayPeer> OnlineClient::relay_peer(std::uint32_t world) const {
    const auto it = relay_peers_.find(world);
    if (it == relay_peers_.end()) return std::nullopt;
    return it->second;
}

void OnlineClient::hello(Platform platform, std::uint32_t build, std::uint32_t caps) {
    HelloInfo h;
    h.platform = platform;
    h.build = build;
    h.caps = caps;
    send_ext(ext::Hello, encode_hello(h));
}

void OnlineClient::send_medius(std::uint16_t id, std::span<const std::uint8_t> body) {
    tcp_.send(encode_frame(id, body));
}

void OnlineClient::send_ext(std::uint16_t type, std::span<const std::uint8_t> body) {
    tcp_.send(encode_frame(type, body));
}

void OnlineClient::handle(const Frame& f) {
    if (f.type < 0xF000) {
        queued_.push_back(f);
        return;
    }
    ++ext_frames_;
    ExtReader r(f.body);
    switch (f.type) {
    case ext::Welcome: {
        r.u16();
        const std::uint32_t caps = r.u32();
        const bool cheats = r.u8() != 0;
        std::string name = r.str();
        if (r.ok()) {
            welcomed_ = true;
            caps_ = caps;
            cheats_allowed_ = cheats;
            server_name_ = std::move(name);
        }
        break;
    }
    case ext::Roster:
        if (auto roster = decode_roster(f.body)) roster_ = std::move(*roster);
        break;
    case ext::Notice: {
        const std::uint32_t code = r.u32();
        std::string text = r.str();
        if (r.ok()) notices_.push_back({code, std::move(text)});
        break;
    }
    case ext::CheatPolicy: {
        const bool on = r.u8() != 0;
        if (r.ok()) cheats_allowed_ = on;
        break;
    }
    case ext::RelayTicket:
        if (auto t = decode_relay_ticket(f.body)) relay_ticket_ = *t;
        break;
    case ext::RelayPeer:
        if (auto p = decode_relay_peer(f.body)) relay_peers_[p->world] = *p;
        break;
    case ext::AdminReply: {
        std::string text = r.str();
        if (r.ok()) admin_replies_.push_back(std::move(text));
        break;
    }
    case ext::RecentPlayersList: {
        const std::uint16_t count = r.u16();
        std::vector<RecentPlayer> list;
        for (std::uint16_t i = 0; i < count && r.ok(); ++i) {
            RecentPlayer p;
            p.account = r.u32();
            p.name = r.str();
            p.platform = platform_from_byte(r.u8());
            p.online = r.u8() != 0;
            p.seconds_ago = r.u32();
            if (r.ok()) list.push_back(std::move(p));
        }
        if (r.ok()) recent_players_ = std::move(list);
        break;
    }
    default:
        break;
    }
}

void OnlineClient::pump(int timeout_ms) {
    for (const Frame& f : tcp_.poll(timeout_ms)) handle(f);
}

std::optional<Frame> OnlineClient::wait_for(std::uint16_t id, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        for (auto it = queued_.begin(); it != queued_.end(); ++it)
            if (it->type == id) {
                Frame f = std::move(*it);
                queued_.erase(it);
                return f;
            }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0 || !tcp_.connected()) return std::nullopt;
        pump(static_cast<int>(std::min<long long>(left, 50)));
    }
}

std::vector<Frame> OnlineClient::wait_for_list(std::uint16_t id, std::size_t end_offset, int timeout_ms) {
    std::vector<Frame> out;
    for (;;) {
        auto f = wait_for(id, timeout_ms);
        if (!f) return out;
        const bool end = f->body.size() <= end_offset || f->body[end_offset] != 0;
        out.push_back(std::move(*f));
        if (end) return out;
    }
}

std::vector<Frame> OnlineClient::take_queued() {
    std::vector<Frame> out(queued_.begin(), queued_.end());
    queued_.clear();
    return out;
}

Platform OnlineClient::platform_of(std::uint32_t account_id) const {
    for (const auto& e : roster_)
        if (e.account_id == account_id) return e.platform;
    return Platform::Unknown;
}

std::vector<OnlineClient::Notice> OnlineClient::take_notices() {
    auto out = std::move(notices_);
    notices_.clear();
    return out;
}

std::optional<std::string> OnlineClient::wait_admin_reply(int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (admin_replies_.empty()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0 || !tcp_.connected()) return std::nullopt;
        pump(static_cast<int>(std::min<long long>(left, 50)));
    }
    std::string s = std::move(admin_replies_.front());
    admin_replies_.pop_front();
    return s;
}

namespace request {
namespace {
std::vector<std::uint8_t> base(std::size_t size, std::string_view message_id, std::string_view session_key) {
    std::vector<std::uint8_t> r(size, 0);
    put_str(r, kMessageIdOffset, kMessageIdLength, message_id);
    put_str(r, kSessionKeyOffset, kSessionKeyLength, session_key);
    return r;
}
}  // namespace

std::vector<std::uint8_t> session_begin(std::string_view message_id) {
    return base(msize::SessionBegin, message_id, "");
}
std::vector<std::uint8_t> registration(std::string_view key, std::string_view name, std::string_view password) {
    auto r = base(msize::AccountRegistration, "reg", key);
    put_str(r, 0x2c, 32, name);
    put_str(r, 0x4c, 32, password);
    return r;
}
std::vector<std::uint8_t> login(std::string_view key, std::string_view name, std::string_view password) {
    auto r = base(msize::AccountLogin, "login", key);
    put_str(r, 0x26, 32, name);
    put_str(r, 0x46, 32, password);
    return r;
}
std::vector<std::uint8_t> logout(std::string_view key) {
    return base(msize::AccountLogout, "logout", key);
}
std::vector<std::uint8_t> policy(std::string_view key, bool privacy) {
    auto r = base(msize::GetPolicy, privacy ? "PRIVACY" : "USAGE", key);
    put_i32(r, 0x28, privacy ? 1 : 0);
    return r;
}
std::vector<std::uint8_t> announcements(std::string_view key) {
    return base(msize::GetAnnouncements, "", key);
}
std::vector<std::uint8_t> account_update_stats(std::string_view key, std::span<const std::uint8_t> stats) {
    auto r = base(msize::AccountUpdateStats, "1", key);  // 004505e0: MessageID 004c0f68 "1", Stats[256] at +0x26
    std::copy_n(stats.begin(), std::min<std::size_t>(stats.size(), kStatsFieldSize), r.begin() + 0x26);
    return r;
}
std::vector<std::uint8_t> get_account_stats(std::string_view key, std::int32_t account, std::string_view message_id) {
    auto r = base(msize::GetAccountStats, message_id, key);  // 00451200: "1"; 0044df20: sprintf("%d", id)
    put_i32(r, 0x28, account);
    return r;
}
std::vector<std::uint8_t> player_report(std::string_view key, std::int32_t world, std::span<const std::uint8_t> stats) {
    std::vector<std::uint8_t> r(msize::PlayerReport, 0);  // 004446e0: SessionKey +0, WorldID +14, Stats[256] +18
    put_str(r, 0, kSessionKeyLength, key);
    put_i32(r, 0x14, world);
    std::copy_n(stats.begin(), std::min<std::size_t>(stats.size(), kStatsFieldSize), r.begin() + 0x18);
    return r;
}
std::vector<std::uint8_t> world_report(std::string_view key, const WorldReportFields& w) {
    std::vector<std::uint8_t> r(msize::WorldReport, 0);  // 00444944: Medius' WorldReport layout
    put_str(r, 0, kSessionKeyLength, key);
    put_i32(r, 0x14, w.world);
    put_i32(r, 0x18, w.player_count);
    put_str(r, 0x1c, 64, w.game_name);
    std::copy_n(w.game_stats.begin(), std::min<std::size_t>(w.game_stats.size(), kStatsFieldSize), r.begin() + 0x5c);
    const std::int32_t tail[9] = {w.min_players, w.max_players, w.game_level, w.skill_level, w.rules_set,
                                  w.generic[0], w.generic[1], w.generic[2], w.world_status};
    for (int i = 0; i < 9; ++i) put_i32(r, 0x15c + 4 * static_cast<std::size_t>(i), tail[i]);
    return r;
}
std::vector<std::uint8_t> create_game(std::string_view key, std::string_view game_name) {
    auto r = base(msize::CreateGame, "create", key);
    put_str(r, 0x30, 64, game_name);
    return r;
}
std::vector<std::uint8_t> game_list(std::string_view key) {
    return base(msize::GameList, "list", key);
}
std::vector<std::uint8_t> game_info(std::string_view key, std::uint32_t world) {
    auto r = base(msize::GameInfo, "info", key);
    put_i32(r, 0x28, static_cast<std::int32_t>(world));
    return r;
}
std::vector<std::uint8_t> join_game(std::string_view key, std::uint32_t world, std::string_view password) {
    auto r = base(msize::JoinGame, "join", key);
    put_i32(r, 0x28, static_cast<std::int32_t>(world));
    put_str(r, 0x30, 32, password);  // Medius GamePassword[32]: 0044e430 leaves it zero (PORT-ONLY use, 132on2c)
    put_i32(r, 0x50, kGameHostPeerToPeer);
    return r;
}
std::vector<std::uint8_t> chat(std::string_view key, std::string_view text, std::int32_t whisper_to) {
    auto r = base(msize::ChatMessage, "1", key);  // MessageID 004c0f68 "1"
    put_i32(r, 0x28, whisper_to ? 1 : 0);
    put_i32(r, 0x2c, whisper_to);
    put_str(r, 0x30, 64, text);
    return r;
}
std::vector<std::uint8_t> text_filter(std::string_view key, std::string_view text, std::int32_t target) {
    auto r = base(msize::TextFilter, std::to_string(target), key);  // MessageID sprintf("%d", target)
    put_i32(r, 0x28, 1);
    put_str(r, 0x2c, 64, text);
    return r;
}
std::vector<std::uint8_t> player_list(std::int32_t world) {
    std::vector<std::uint8_t> r(msize::PlayerList, 0);  // 004516e0: memset 0x1c, MessageID "1", +18 the world (+1eac)
    put_str(r, kMessageIdOffset, kMessageIdLength, "1");
    put_i32(r, 0x18, world);
    return r;
}
std::vector<std::uint8_t> buddy_list(std::string_view message_id) {
    std::vector<std::uint8_t> r(msize::GetBuddyListExtraInfo, 0);  // 0044d860: MessageID only (the session's account)
    put_str(r, kMessageIdOffset, kMessageIdLength, message_id);
    return r;
}
std::vector<std::uint8_t> buddy_invitations(std::string_view message_id) {
    std::vector<std::uint8_t> r(msize::GetBuddyInvitations, 0);  // 0044d780: MessageID only
    put_str(r, kMessageIdOffset, kMessageIdLength, message_id);
    return r;
}
std::vector<std::uint8_t> buddy_add(std::string_view key, std::int32_t account) {
    auto r = base(msize::AddToBuddyListConfirmation, "1", key);  // 0044db60: +28 AccountID, +2c AddType 1
    put_i32(r, 0x28, account);
    put_i32(r, 0x2c, 1);
    return r;
}
std::vector<std::uint8_t> buddy_answer(std::string_view key, std::int32_t inviter, bool accept) {
    std::vector<std::uint8_t> r(msize::AddToBuddyListFwdConfirmationResponse, 0);  // 00446560
    put_i32(r, 0x18, accept ? 2 : status::RequestDenied);
    put_str(r, 0x1c, kSessionKeyLength, key);
    put_i32(r, 0x30, inviter);
    put_i32(r, 0x34, 1);
    return r;
}
std::vector<std::uint8_t> buddy_remove(std::string_view key, std::int32_t account) {
    auto r = base(msize::RemoveFromBuddyList, "1", key);  // 0044d9c0: +28 AccountID
    put_i32(r, 0x28, account);
    return r;
}
std::vector<std::uint8_t> account_get_id(std::string_view key, std::string_view name) {
    auto r = base(msize::AccountGetID, "1", key);  // 00451370: MessageID "1" (0x004c0f68), +26 AccountName [32]
    put_str(r, 0x26, 32, name.substr(0, 31));
    return r;
}
}  // namespace request

ConnectInfo read_connect_info(std::span<const std::uint8_t> s, std::size_t base) {
    ConnectInfo c;
    // The second address (base +0x1c) is the one the login callback copies (0x00467d18).
    c.ip = get_str(s, base + 0x1c + 4, 16);
    c.port = static_cast<std::uint16_t>(get_i32(s, base + 0x1c + 0x14));
    c.world = static_cast<std::uint32_t>(get_i32(s, base + 0x34));
    c.session_key = get_str(s, base + 0x78, 17);
    return c;
}

}  // namespace omega::online
