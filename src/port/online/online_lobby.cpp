// PORT-ONLY (checkpoint 132on2): see online_lobby.hpp.
#include "online_lobby.hpp"

#include <algorithm>

namespace omega::online {

bool OnlineLobby::start() {
    end();
    error_.clear();
    if (!client_.connect(options_.server, &error_, options_.timeout_ms)) {
        if (error_.empty()) error_ = "cannot connect to " + options_.server;
        return false;
    }
    if (options_.hello) {
        client_.hello(options_.platform, options_.build, options_.caps);
        if (options_.mod_report) client_.send_ext(ext::ModReport, encode_mod_report(options_.mod_report()));
    }
    // 0044c620: SessionBegin's MessageID is the lobby's serial; 00446d00 copies the response's SessionKey (+15) to
    // lobby +2429 and tests its status (+2c).
    client_.send_medius(mid::SessionBegin, request::session_begin("sb" + std::to_string(++message_serial_)));
    const auto r = client_.wait_for(mid::SessionBeginResponse, options_.timeout_ms);
    if (!r || r->body.size() < msize::SessionBeginResponse) {
        error_ = "no SessionBegin response from " + options_.server;
        client_.disconnect();
        return false;
    }
    if (get_i32(r->body, 0x2c) != status::Success) {
        error_ = "SessionBegin refused (status " + std::to_string(get_i32(r->body, 0x2c)) + ")";
        client_.disconnect();
        return false;
    }
    session_key_ = get_str(r->body, kSessionKeyOffset, kSessionKeyLength);
    started_ = true;
    return true;
}

void OnlineLobby::end() {
    if (client_.connected()) client_.disconnect();
    started_ = false;
    session_key_.clear();
    account_id_ = 0;
    account_name_.clear();
}

namespace {
std::optional<std::int32_t> status_of(const std::optional<Frame>& r, std::size_t size) {
    if (!r || r->body.size() < size) return std::nullopt;
    return get_i32(r->body, kStatusOffset);
}
}  // namespace

std::optional<std::int32_t> OnlineLobby::login(const std::string& name, const std::string& password) {
    if (!session()) return std::nullopt;
    client_.send_medius(mid::AccountLogin, request::login(session_key_, name, password));
    const auto r = client_.wait_for(mid::AccountLoginResponse, options_.timeout_ms);
    const auto st = status_of(r, msize::AccountLoginResponse);
    if (st && *st == status::Success) {
        account_id_ = static_cast<std::uint32_t>(get_i32(r->body, 0x1c));
        account_name_ = name;
    }
    if (!st) error_ = "no AccountLogin response";
    return st;
}

std::optional<std::int32_t> OnlineLobby::register_account(const std::string& name, const std::string& password) {
    if (!session()) return std::nullopt;
    client_.send_medius(mid::AccountRegistration, request::registration(session_key_, name, password));
    const auto st = status_of(client_.wait_for(mid::AccountRegistrationResponse, options_.timeout_ms),
                              msize::AccountRegistrationResponse);
    if (!st) error_ = "no AccountRegistration response";
    return st;
}

std::optional<std::int32_t> OnlineLobby::logout() {
    if (!session()) return std::nullopt;
    client_.send_medius(mid::AccountLogout, request::logout(session_key_));
    const auto st = status_of(client_.wait_for(mid::AccountLogoutResponse, options_.timeout_ms), msize::AccountLogoutResponse);
    account_id_ = 0;
    account_name_.clear();
    fetched_game_infos_.clear();
    if (!st) error_ = "no AccountLogout response";
    return st;
}

std::optional<std::int32_t> OnlineLobby::account_update_stats(std::span<const std::uint8_t> blob) {
    if (!session()) return std::nullopt;
    client_.send_medius(mid::AccountUpdateStats, request::account_update_stats(session_key_, blob));
    const auto st = status_of(client_.wait_for(mid::AccountUpdateStatsResponse, options_.timeout_ms),
                              msize::AccountUpdateStatsResponse);
    if (!st) error_ = "no AccountUpdateStats response";
    if (st && *st == status::Success && account_id_ && blob.size() >= kStatsBlobSize)
        cache_stats(account_id_, account_name_, blob);
    return st;
}

std::optional<std::int32_t> OnlineLobby::get_account_stats(std::uint32_t account, StatsBlob* out) {
    if (!session()) return std::nullopt;
    client_.send_medius(mid::GetAccountStats,
                        request::get_account_stats(session_key_, static_cast<std::int32_t>(account)));
    const auto r = client_.wait_for(mid::GetAccountStatsResponse, options_.timeout_ms);
    const auto st = status_of(r, msize::GetAccountStatsResponse);
    if (!st) {
        error_ = "no GetAccountStats response";
        return st;
    }
    // 00446db0 keeps the whole response (session +a88: the name at +1c, the record at +48); 004467d0 hands the name and
    // the record to the cache (00450880), which keeps only version-0xb records.
    if (*st == status::Success) {
        const std::span<const std::uint8_t> rec(r->body.data() + 0x48, kStatsBlobSize);
        cache_stats(account, get_str(r->body, 0x1c, 32), rec);
        if (out) std::copy(rec.begin(), rec.end(), out->begin());
    }
    return st;
}

void OnlineLobby::cache_stats(std::uint32_t account, const std::string& name, std::span<const std::uint8_t> blob) {
    if (!valid_stats(blob)) return;  // 00450880: byte 0 must be 0xb
    CachedStats& c = stats_cache_[account];
    c.name = name;
    std::copy_n(blob.begin(), kStatsBlobSize, c.blob.begin());
    c.when = std::chrono::steady_clock::now();
}

const OnlineLobby::CachedStats* OnlineLobby::cached_stats(std::uint32_t account) const {
    const auto it = stats_cache_.find(account);
    return it == stats_cache_.end() ? nullptr : &it->second;
}

void OnlineLobby::send_player_report(std::int32_t world, std::span<const std::uint8_t> blob) {
    if (!logged_in()) return;
    client_.send_medius(mid::PlayerReport, request::player_report(session_key_, world, blob));
    if (blob.size() >= kStatsBlobSize) cache_stats(account_id_, account_name_, blob);
}

void OnlineLobby::send_world_report(const WorldReportFields& w) {
    if (!logged_in()) return;
    client_.send_medius(mid::WorldReport, request::world_report(session_key_, w));
}

bool OnlineLobby::policy(bool privacy, std::vector<std::string>& chunks) {
    if (!session()) return false;
    client_.send_medius(mid::GetPolicy, request::policy(session_key_, privacy));
    const auto frames = client_.wait_for_list(mid::GetPolicyResponse, 0x11c, options_.timeout_ms);
    bool ended = false;
    for (const Frame& f : frames) {
        if (f.body.size() < msize::GetPolicyResponse) continue;
        ended = f.body[0x11c] != 0;
        if (get_i32(f.body, kStatusOffset) != status::Success) break;  // 004473c0: nothing appended
        chunks.push_back(get_str(f.body, 0x1c, 256));
    }
    if (!ended) error_ = "policy download incomplete";
    return ended;
}

bool OnlineLobby::announcements(std::vector<std::pair<std::int32_t, std::string>>& rows) {
    if (!session()) return false;
    client_.send_medius(mid::GetAnnouncements, request::announcements(session_key_));
    const auto frames = client_.wait_for_list(mid::GetAnnouncementsResponse, 0x408, options_.timeout_ms);
    bool ended = false;
    static constexpr std::string_view settings_tag = "{SFOLOBBY_SETTINGS}";
    for (const Frame& f : frames) {
        if (f.body.size() < msize::GetAnnouncementsResponse) continue;
        ended = f.body[0x408] != 0;
        if (get_i32(f.body, kStatusOffset) != status::Success) break;
        std::string text = get_str(f.body, 0x20, 1000);
        if (text.compare(0, settings_tag.size(), settings_tag) == 0) {
            settings_rows_.push_back(std::move(text));
            continue;
        }
        rows.emplace_back(get_i32(f.body, 0x1c), std::move(text));
    }
    if (!ended) error_ = "announcement download incomplete";
    return ended;
}

void OnlineLobby::send_create_game(const std::string& game_name) {
    if (session()) client_.send_medius(mid::CreateGame, request::create_game(session_key_, game_name));
}

void OnlineLobby::send_game_list() {
    if (session()) client_.send_medius(mid::GameList, request::game_list(session_key_));
}

void OnlineLobby::send_join_game(std::uint32_t world, const std::string& password) {
    if (session()) client_.send_medius(mid::JoinGame, request::join_game(session_key_, world, password));
}

// 00451050's sends: TextFilter (its response is the sender's own echo, 00447bb0), then ChatMessage unless the
// target is the account itself.
void OnlineLobby::send_chat(const std::string& text, std::int32_t target) {
    if (!session()) return;
    client_.send_medius(mid::TextFilter, request::text_filter(session_key_, text, target));
    if (target != static_cast<std::int32_t>(account_id_))
        client_.send_medius(mid::ChatMessage, request::chat(session_key_, text, target));
}

void OnlineLobby::send_player_list(std::int32_t world) {
    if (session()) client_.send_medius(mid::PlayerList, request::player_list(world));
}

void OnlineLobby::send_buddy_list() {
    if (logged_in()) client_.send_medius(mid::GetBuddyListExtraInfo, request::buddy_list());
}

void OnlineLobby::send_buddy_add(std::int32_t account) {
    if (logged_in()) client_.send_medius(mid::AddToBuddyListConfirmation, request::buddy_add(session_key_, account));
}

std::optional<std::int32_t> OnlineLobby::remove_contact(std::int32_t account) {
    if (!logged_in()) return std::nullopt;
    client_.send_medius(mid::RemoveFromBuddyList, request::buddy_remove(session_key_, account));
    const auto st = status_of(client_.wait_for(mid::RemoveFromBuddyListResponse, options_.timeout_ms),
                              msize::RemoveFromBuddyListResponse);
    if (!st) error_ = "no RemoveFromBuddyList response";
    return st;
}

std::optional<Frame> OnlineLobby::account_get_id(const std::string& name) {
    if (!logged_in()) return std::nullopt;
    client_.send_medius(mid::AccountGetID, request::account_get_id(session_key_, name));
    auto r = client_.wait_for(mid::AccountGetIDResponse, options_.timeout_ms);
    if (!r || r->body.size() < msize::AccountGetIDResponse) {
        error_ = "no AccountGetID response";
        return std::nullopt;
    }
    return r;
}

void OnlineLobby::game_password(std::uint32_t world, const std::string& password) {
    if (session() && (client_.caps() & cap::GamePassword))
        client_.send_ext(ext::GamePassword, ExtWriter().u32(world).str(password).bytes());
}

std::optional<Frame> OnlineLobby::game_info(std::uint32_t world) {
    if (!session()) return std::nullopt;
    client_.send_medius(mid::GameInfo, request::game_info(session_key_, world));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(options_.timeout_ms);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        auto r = client_.wait_for(mid::GameInfoResponse, static_cast<int>(std::max<std::int64_t>(0, left.count())));
        if (r && r->body.size() >= msize::GameInfoResponse && get_str(r->body, kMessageIdOffset, kMessageIdLength) == "1") {
            fetched_game_infos_.push_back(std::move(*r));  // 0044ebd0's (132on2i): not this request's
            continue;
        }
        if (!r || r->body.size() < msize::GameInfoResponse) {
            error_ = "no GameInfo response";
            return std::nullopt;
        }
        return r;
    }
}

void OnlineLobby::send_game_info_fetch(std::uint32_t world) {
    if (!session()) return;
    auto r = request::game_info(session_key_, world);
    put_str(r, kMessageIdOffset, kMessageIdLength, "1");  // 004c0ef0
    client_.send_medius(mid::GameInfo, r);
}

std::vector<Frame> OnlineLobby::take_game_info_fetches() {
    while (auto f = client_.wait_for(mid::GameInfoResponse, 0))
        if (f->body.size() >= msize::GameInfoResponse) fetched_game_infos_.push_back(std::move(*f));
    auto out = std::move(fetched_game_infos_);
    fetched_game_infos_.clear();
    return out;
}

void OnlineLobby::host_endpoint(std::uint32_t world, std::uint16_t port, std::uint16_t external_port,
                                const std::string& external_ip) {
    if (!session() || !(client_.caps() & cap::HostEndpoint)) return;
    ExtWriter w;
    w.u32(world).u16(port);
    if (external_port != 0) w.u16(external_port).str(external_ip);
    client_.send_ext(ext::HostEndpoint, w.bytes());
}

void OnlineLobby::leave_game() {
    if (session() && (client_.caps() & cap::HostEndpoint)) client_.send_ext(ext::LeaveGame, {});
}

}  // namespace omega::online
