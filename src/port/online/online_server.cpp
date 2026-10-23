// PORT-ONLY (checkpoint 132on1): see online_server.hpp. Field offsets: research/ONLINE.md §4 (each with the address
// of the client code that writes or reads it).
#include "online_server.hpp"

#include "online_files.hpp"
#include "online_hash.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace omega::online {
namespace {

std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

std::vector<std::string> words(std::string_view line) {
    std::vector<std::string> out;
    std::istringstream in{std::string(line)};
    std::string w;
    while (in >> w) out.push_back(w);
    return out;
}

// The rest of `line` after `n` words.
std::string rest_after(std::string_view line, std::size_t n) {
    std::size_t pos = 0;
    for (std::size_t i = 0; i < n; ++i) {
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
        while (pos < line.size() && !std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
    }
    return trim(line.substr(std::min(pos, line.size())));
}

// Admin console text: "\n" becomes a new line, "\\" a backslash.
std::string unescape_text(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size() && (s[i + 1] == 'n' || s[i + 1] == '\\')) {
            out += s[i + 1] == 'n' ? '\n' : '\\';
            ++i;
        } else {
            out += s[i];
        }
    }
    return out;
}

bool flag_word(const std::string& v, bool& out) {
    if (v == "1" || v == "on" || v == "true") return out = true, true;
    if (v == "0" || v == "off" || v == "false") return out = false, true;
    return false;
}

// The failed-login throttle's keys (132on2d): one name from one address, and one address.
std::string lower_name(std::string_view name) {
    std::string out(name);
    for (char& ch : out) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return out;
}
std::string name_key(std::string_view name, std::uint32_t ip) { return lower_name(name) + "@" + ipv4_text(ip); }
std::string address_key(std::uint32_t ip) { return "@" + ipv4_text(ip); }

// NetConnectionInfo as the login (+0x28) and join (+0x20) responses carry it: Type, two 24-byte addresses
// {AddressType, Address[16], Port} (the login callback copies the second one, 0x00467d18), WorldID, ServerKey[64],
// SessionKey[17], AccessKey[17].
void put_connect_info(std::span<std::uint8_t> s, std::size_t base, const std::string& ip, std::uint16_t port,
                      std::uint32_t world, const std::string& session_key, const std::string& access_key) {
    put_i32(s, base + 0x00, 1);
    for (std::size_t i = 0; i < 2; ++i) {
        const std::size_t a = base + 0x04 + i * 0x18;
        put_i32(s, a, 1);
        put_str(s, a + 4, 16, ip);
        put_i32(s, a + 0x14, port);
    }
    put_i32(s, base + 0x34, static_cast<std::int32_t>(world));
    put_str(s, base + 0x78, 17, session_key);
    put_str(s, base + 0x89, 17, access_key);
}

}  // namespace

// RFC 1918, loopback, link-local and the shared (carrier-grade NAT) range 100.64.0.0/10.
bool is_private_ipv4(std::uint32_t ip) {
    return (ip >> 24) == 10 || (ip >> 24) == 127 || (ip & 0xFFF00000u) == 0xAC100000u || (ip & 0xFFFF0000u) == 0xC0A80000u ||
           (ip & 0xFFFF0000u) == 0xA9FE0000u || (ip & 0xFFC00000u) == 0x64400000u;
}

std::string ipv4_text(std::uint32_t ip) {
    return std::to_string(ip >> 24) + "." + std::to_string((ip >> 16) & 255) + "." + std::to_string((ip >> 8) & 255) +
           "." + std::to_string(ip & 255);
}

std::optional<std::uint64_t> parse_ban_duration(std::string_view text) {
    if (text == "perm" || text == "permanent" || text == "forever") return kBanForever;
    if (text.empty()) return std::nullopt;
    std::uint64_t unit = 1;
    std::string_view digits = text;
    switch (text.back()) {
    case 's': unit = 1; digits.remove_suffix(1); break;
    case 'm': unit = 60; digits.remove_suffix(1); break;
    case 'h': unit = 3600; digits.remove_suffix(1); break;
    case 'd': unit = 86400; digits.remove_suffix(1); break;
    default: break;
    }
    if (digits.empty()) return std::nullopt;
    std::uint64_t value = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
        if (value > 100000000ull) return std::nullopt;
    }
    if (value == 0) return std::nullopt;
    return value * unit;
}

std::map<std::string, std::string> parse_server_config(std::string_view text, ServerConfig& cfg) {
    std::map<std::string, std::string> extra;
    std::istringstream in{std::string(text)};
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        const std::size_t eq = t.find('=');
        if (eq == std::string::npos) throw std::runtime_error("server config line " + std::to_string(number) + ": no '='");
        const std::string key = trim(t.substr(0, eq));
        const std::string value = trim(t.substr(eq + 1));
        auto bad = [&] { return std::runtime_error("server config line " + std::to_string(number) + ": bad " + key); };
        if (key == "allow_cheats") {
            if (!flag_word(value, cfg.allow_cheats)) throw bad();
        } else if (key == "server_name") {
            cfg.server_name = value;
        } else if (key == "admin_token") {
            cfg.admin_token = value;
        } else if (key == "public_ip") {
            cfg.public_ip = value;
        } else if (key == "port") {
            const int p = std::atoi(value.c_str());
            if (p <= 0 || p > 65535) throw bad();
            cfg.port = static_cast<std::uint16_t>(p);
        } else if (key == "max_players") {
            const int n = std::atoi(value.c_str());
            if (n < 2 || n > 4) throw bad();
            cfg.max_players = n;
        } else if (key == "mod_policy") {
            const auto policy = parse_mod_policy(value);
            if (!policy) throw bad();
            cfg.mod_policy = *policy;
        } else if (key == "max_conns_per_ip" || key == "message_rate" || key == "message_burst" ||
                   key == "login_failures" || key == "registrations_per_hour" || key == "login_attempts_per_ip" ||
                   key == "login_attempts_per_account" || key == "chat_per_minute" || key == "chat_burst" ||
                   key == "chat_per_minute_ip" || key == "chat_flood_kick" || key == "flood_block_after") {
            char* end = nullptr;
            const long n = std::strtol(value.c_str(), &end, 10);
            if (value.empty() || *end || n < 0 || n > 1000000) throw bad();
            (key == "max_conns_per_ip"             ? cfg.max_conns_per_ip
             : key == "message_rate"               ? cfg.message_rate
             : key == "message_burst"              ? cfg.message_burst
             : key == "login_failures"             ? cfg.login_failures
             : key == "login_attempts_per_ip"      ? cfg.login_attempts_per_ip
             : key == "login_attempts_per_account" ? cfg.login_attempts_per_account
             : key == "chat_per_minute"            ? cfg.chat_per_minute
             : key == "chat_burst"                 ? cfg.chat_burst
             : key == "chat_per_minute_ip"         ? cfg.chat_per_minute_ip
             : key == "chat_flood_kick"            ? cfg.chat_flood_kick
             : key == "flood_block_after"          ? cfg.flood_block_after
                                                   : cfg.registrations_per_hour) = static_cast<int>(n);
        } else if (key == "login_window" || key == "login_lockout" || key == "flood_window" || key == "flood_block") {
            const auto seconds = parse_ban_duration(value);  // "600", "10m", "1h" ...
            if (!seconds || *seconds == kBanForever) throw bad();
            (key == "login_window"    ? cfg.login_window
             : key == "login_lockout" ? cfg.login_lockout
             : key == "flood_window"  ? cfg.flood_window
                                      : cfg.flood_block) = *seconds;
        } else if (key == "max_results") {
            char* end = nullptr;
            const unsigned long long n = std::strtoull(value.c_str(), &end, 10);
            if (value.empty() || *end || n == 0 || n > 100000) throw bad();
            cfg.max_results = static_cast<std::size_t>(n);
        } else if (key == "relay_port" || key == "relay_max_endpoints" || key == "relay_packet_rate" ||
                   key == "relay_byte_rate") {
            char* end = nullptr;
            const long n = std::strtol(value.c_str(), &end, 10);
            const long top = key == "relay_port" ? 65535 : key == "relay_max_endpoints" ? 100000 : 100000000;
            if (value.empty() || *end || n < 0 || n > top) throw bad();
            if (key == "relay_port")
                cfg.relay_port = static_cast<std::uint16_t>(n);
            else if (key == "relay_max_endpoints")
                cfg.relay_max_endpoints = static_cast<std::size_t>(n);
            else
                (key == "relay_packet_rate" ? cfg.relay_packet_rate : cfg.relay_byte_rate) = static_cast<int>(n);
        } else if (key == "relay_idle") {
            const auto seconds = parse_ban_duration(value);
            if (!seconds || *seconds == kBanForever) throw bad();
            cfg.relay_idle = *seconds;
        } else if (key == "chat_filter") {
            if (!flag_word(value, cfg.chat_filter)) throw bad();
        } else if (key == "log_max_bytes" || key == "log_keep") {
            char* end = nullptr;
            const unsigned long long n = std::strtoull(value.c_str(), &end, 10);
            if (value.empty() || *end || n > (key == "log_keep" ? 100ull : (1ull << 40))) throw bad();
            extra[key] = value;
        } else if (key == "accounts" || key == "log" || key == "bind" || key == "pbkdf2_iterations" ||
                   key == "policy_file" || key == "stats_file" || key == "backups" || key == "backup_interval" ||
                   key == "log_format" || key == "social_file" || key == "chat_filter_words" || key == "password_threads") {
            extra[key] = value;
        } else {
            throw std::runtime_error("server config line " + std::to_string(number) + ": unknown key " + key);
        }
    }
    return extra;
}

void parse_policy_file(std::string_view text, ServerConfig& cfg) {
    std::istringstream in{std::string(text)};
    std::string line;
    std::string* target = nullptr;
    std::string usage, privacy;
    std::vector<std::string> announcements;
    std::uint32_t version = cfg.policy_version;
    bool any_section = false;
    auto append = [](std::string& s, const std::string& l) {
        if (!s.empty()) s += '\n';
        s += l;
    };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string t = trim(line);
        if (t == "[usage]") {
            target = &usage, any_section = true;
        } else if (t == "[privacy]") {
            target = &privacy, any_section = true;
        } else if (t == "[announcement]") {
            announcements.emplace_back();
            target = &announcements.back(), any_section = true;
        } else if (target) {
            append(*target, line);
        } else if (t.empty() || t[0] == '#') {
            continue;
        } else if (t.rfind("version=", 0) == 0) {
            const long v = std::strtol(t.c_str() + 8, nullptr, 10);
            if (v <= 0) throw std::runtime_error("policy file: bad version");
            version = static_cast<std::uint32_t>(v);
        } else {
            throw std::runtime_error("policy file: text before the first section: " + t);
        }
    }
    if (!any_section) throw std::runtime_error("policy file: no [usage] / [privacy] / [announcement] section");
    auto strip = [](std::string s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
        return s;
    };
    cfg.usage_policy = strip(usage);
    cfg.privacy_policy = strip(privacy);
    cfg.announcements.clear();
    for (auto& a : announcements)
        if (!strip(a).empty()) cfg.announcements.push_back(strip(a));
    cfg.policy_version = version;
}

std::string format_policy_file(const ServerConfig& cfg) {
    std::ostringstream out;
    out << "# omega_server policy and announcements (PORT-ONLY). The game shows them after DNAS, before the account\n"
           "# login: [usage] (PolicyTitle1), [privacy] (PolicyTitle2; empty: skipped), then each [announcement].\n"
           "# version: bumped by every policy edit (each login records it on the account).\n";
    out << "version=" << cfg.policy_version << "\n[usage]\n" << cfg.usage_policy << "\n[privacy]\n" << cfg.privacy_policy << "\n";
    for (const auto& a : cfg.announcements) out << "[announcement]\n" << a << "\n";
    return out.str();
}

OmegaServer::OmegaServer(AccountStore& accounts, ServerConfig config, SendFn send, CloseFn close, LogFn log, ClockFn clock)
    : accounts_(accounts), config_(std::move(config)), send_(std::move(send)), close_(std::move(close)),
      log_(std::move(log)), clock_(std::move(clock)), started_at_(clock_ ? clock_() : 0) {}

void OmegaServer::log(const std::string& line) {
    if (log_) log_(line);
}

void OmegaServer::send(std::uint32_t conn, std::uint16_t type, std::span<const std::uint8_t> body) {
    if (send_) send_(conn, encode_frame(type, body));
}

void OmegaServer::send_ext(const Conn& c, std::uint32_t capability, std::uint16_t type, std::span<const std::uint8_t> body) {
    if (c.has(capability)) send(c.id, type, body);
}

void OmegaServer::send_notice(const Conn& c, std::uint32_t code, std::string_view text) {
    send_ext(c, cap::Notices, ext::Notice, ExtWriter().u32(code).str(text).bytes());
}

void OmegaServer::save_policy() {
    if (policy_file_.empty()) return;
    try {
        write_file_atomic(policy_file_, format_policy_file(config_), policy_backups_, 0);
    } catch (const std::exception& e) {
        log(std::string("policy: ") + e.what());
    }
}

bool OmegaServer::cheats_allowed_for(std::uint32_t conn) const {
    auto it = conns_.find(conn);
    if (it == conns_.end() || !config_.allow_cheats) return false;
    const Conn& c = it->second;
    if (!c.world_id) return true;
    for (const auto& [id, other] : conns_)
        if (other.world_id == c.world_id && other.kind == ClientKind::Original) return false;
    return true;
}

const ModReport* OmegaServer::mod_report(std::uint32_t conn) const {
    const auto it = conns_.find(conn);
    return it != conns_.end() && it->second.mod_report ? &*it->second.mod_report : nullptr;
}

std::string OmegaServer::mods_status() const {
    std::string out = std::string("mod_policy=") + mod_policy_name(config_.mod_policy);
    for (const auto& [id, c] : conns_) {
        out += "\n  conn " + std::to_string(id) + ": ";
        if (c.kind == ClientKind::Original) out += "original session (no report)";
        else if (c.mod_report) out += describe_mod_report(*c.mod_report);
        else out += "no report";
        if (c.account_id) out += " [" + c.name + "]";
    }
    return out;
}

void OmegaServer::set_relay(RelayCore* relay, std::uint16_t udp_port) {
    relay_ = relay;
    relay_port_ = udp_port;
}

std::uint32_t OmegaServer::world_of(std::uint32_t conn) const {
    const auto it = conns_.find(conn);
    return it == conns_.end() || !it->second.account_id ? 0 : it->second.world_id;
}

RelayLimits OmegaServer::relay_limits() const {
    RelayLimits l;
    l.max_endpoints = config_.relay_max_endpoints;
    l.packet_rate = config_.relay_packet_rate;
    l.byte_rate = config_.relay_byte_rate;
    l.idle_ms = config_.relay_idle * 1000;
    return l;
}

std::uint64_t OmegaServer::relay_now_ms() const {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool OmegaServer::refused_by_mod_policy(Conn& c, std::span<const std::uint8_t> req, bool create) {
    const auto* report = c.mod_report ? &*c.mod_report : nullptr;
    auto why = mod_refusal(config_.mod_policy, report, c.kind == ClientKind::Original);
    if (!why && !create && config_.mod_policy == ModPolicy::Gameplay && req.size() >= 0x2c) {
        // Checkpoint 132md16: a join needs the game creator's gameplay set (its host connection's last report).
        const auto game = games_.find(static_cast<std::uint32_t>(get_i32(req, 0x28)));
        const auto host = game == games_.end() ? conns_.end() : conns_.find(game->second.host_conn);
        if (host != conns_.end() && host->second.id != c.id)
            why = gameplay_refusal(report, c.kind == ClientKind::Original,
                                   host->second.mod_report ? &*host->second.mod_report : nullptr,
                                   host->second.kind == ClientKind::Original);
    }
    if (!why) return false;
    auto r = response(req, create ? msize::CreateGameResponse : msize::JoinGameResponse);
    put_i32(r, kStatusOffset, status::RequestDenied);
    send(c.id, create ? mid::CreateGameResponse : mid::JoinGameResponse, r);
    send_notice(c, notice::Info, "Refused by the server's mod policy: " + *why);
    log(std::string("mods: ") + (create ? "create" : "join") + " refused for conn " + std::to_string(c.id) +
        " (mod_policy=" + mod_policy_name(config_.mod_policy) + "): " + *why);
    return true;
}

void OmegaServer::push_cheat_policy() {
    for (auto& [id, c] : conns_) {
        if (!c.has(cap::CheatPolicy)) continue;
        const int now = cheats_allowed_for(id) ? 1 : 0;
        if (now == c.cheats_sent) continue;
        c.cheats_sent = now;
        send(id, ext::CheatPolicy, ExtWriter().u8(static_cast<std::uint8_t>(now)).bytes());
    }
}

std::vector<std::uint8_t> OmegaServer::response(std::span<const std::uint8_t> request, std::size_t size) const {
    std::vector<std::uint8_t> out(size, 0);
    // Every response echoes the request's MessageID (the callbacks match it, e.g. 0x0044fa70's "%d:%d:%d").
    for (std::size_t i = 0; i < kMessageIdLength && i < request.size(); ++i) out[i] = request[i];
    out[kMessageIdLength - 1] = 0;
    return out;
}

void OmegaServer::on_connect(std::uint32_t conn, std::uint32_t peer_ip, ClientKind kind) {
    Conn c;
    c.id = conn;
    c.peer_ip = peer_ip;
    c.kind = kind;
    int open = 0;
    for (const auto& [id, other] : conns_)
        if (other.peer_ip == peer_ip && !other.closing) ++open;
    conns_[conn] = c;
    log("conn " + std::to_string(conn) + " connected from " + ipv4_text(peer_ip) +
        (kind == ClientKind::Original ? " (original client)" : ""));
    const std::uint64_t now = clock_ ? clock_() : 0;
    if (const std::uint64_t blocked = blocked_for(peer_ip, now)) {
        // PORT-ONLY anti-abuse (132on2h): an address blocked after repeated flood disconnects.
        ++abuse_.blocked_connects;
        log("abuse: conn " + std::to_string(conn) + " closed, " + ipv4_text(peer_ip) + " is blocked for " +
            std::to_string(blocked) + " more s (flood disconnects)");
        conns_[conn].closing = true;
        if (close_) close_(conn);
        return;
    }
    if (config_.max_conns_per_ip > 0 && open >= config_.max_conns_per_ip) {
        // PORT-ONLY operations (132on2d): too many connections from one address.
        log("limit: conn " + std::to_string(conn) + " closed, " + std::to_string(open) + " connections already open from " +
            ipv4_text(peer_ip) + " (max_conns_per_ip=" + std::to_string(config_.max_conns_per_ip) + ")");
        conns_[conn].closing = true;
        if (close_) close_(conn);
    }
}

bool OmegaServer::within_rate(Conn& c) {
    if (config_.message_rate <= 0 || config_.message_burst <= 0) return true;
    const std::uint64_t now = clock_ ? clock_() : 0;
    if (c.tokens < 0) {
        c.tokens = config_.message_burst;
        c.tokens_at = now;
    } else if (now > c.tokens_at) {
        c.tokens = std::min<double>(config_.message_burst,
                                    c.tokens + static_cast<double>(now - c.tokens_at) * config_.message_rate);
        c.tokens_at = now;
    }
    if (c.tokens >= 1) {
        c.tokens -= 1;
        return true;
    }
    log("limit: conn " + std::to_string(c.id) + (c.account_id ? " (" + c.name + ")" : std::string()) +
        " closed, over message_rate=" + std::to_string(config_.message_rate) + "/s (burst " +
        std::to_string(config_.message_burst) + ")");
    flood_disconnect(c, "message_rate", now);
    return false;
}

// ---- PORT-ONLY anti-abuse (checkpoint 132on2h, ONLINE.md §6.16) --------------------------------------------------

bool OmegaServer::take_token(Bucket& b, int per_minute, int burst, std::uint64_t now) {
    if (per_minute <= 0 || burst <= 0) return true;
    if (b.tokens < 0) {
        b.tokens = burst;
        b.at = now;
    } else if (now > b.at) {
        b.tokens = std::min<double>(burst, b.tokens + static_cast<double>(now - b.at) * per_minute / 60.0);
        b.at = now;
    }
    if (b.tokens < 1) return false;
    b.tokens -= 1;
    return true;
}

bool OmegaServer::bucket_full(const Bucket& b, int per_minute, int burst, std::uint64_t now) {
    if (per_minute <= 0 || burst <= 0 || b.tokens < 0) return true;
    return b.tokens + static_cast<double>(now > b.at ? now - b.at : 0) * per_minute / 60.0 >= burst;
}

bool OmegaServer::login_attempt_allowed(Conn& c, const std::string& name, bool registration, std::uint64_t now) {
    std::string why;
    if (!take_token(login_by_ip_[c.peer_ip], config_.login_attempts_per_ip, config_.login_attempts_per_ip, now))
        why = "login_attempts_per_ip=" + std::to_string(config_.login_attempts_per_ip);
    else if (!registration && !take_token(login_by_name_[lower_name(name)], config_.login_attempts_per_account,
                                          config_.login_attempts_per_account, now))
        why = "login_attempts_per_account=" + std::to_string(config_.login_attempts_per_account);
    if (why.empty()) return true;
    ++abuse_.login_refused;
    log("abuse: " + std::string(registration ? "account create" : "login") + " '" + name + "' from " +
        ipv4_text(c.peer_ip) + " refused, over " + why + "/min");
    send_notice(c, notice::Info, "Too many login attempts. Wait a minute and try again.");
    return false;
}

// A chat message within the sender's budget and its address's? Past either it is dropped and the sender told (a
// server line: ext::Notice for a port session, else a private ChatFwd from "omega_server", the original wire);
// chat_flood_kick drops within a minute disconnect the player (a flood disconnect).
bool OmegaServer::chat_allowed(Conn& c, std::uint64_t now) {
    Bucket own{c.chat_tokens, c.chat_at};
    std::string why;
    if (!take_token(own, config_.chat_per_minute, config_.chat_burst, now))
        why = "chat_per_minute=" + std::to_string(config_.chat_per_minute) + " (burst " +
              std::to_string(config_.chat_burst) + ")";
    else if (!take_token(chat_by_ip_[c.peer_ip], config_.chat_per_minute_ip, config_.chat_per_minute_ip, now))
        why = "chat_per_minute_ip=" + std::to_string(config_.chat_per_minute_ip);
    c.chat_tokens = own.tokens;
    c.chat_at = own.at;
    if (why.empty()) return true;
    ++abuse_.chat_dropped;
    c.chat_drops.erase(std::remove_if(c.chat_drops.begin(), c.chat_drops.end(), [&](std::uint64_t t) { return t + 60 <= now; }),
                       c.chat_drops.end());
    c.chat_drops.push_back(now);
    log("abuse: chat from " + c.name + " (" + ipv4_text(c.peer_ip) + ") dropped, over " + why + ", " +
        std::to_string(c.chat_drops.size()) + " dropped this minute");
    if (config_.chat_flood_kick > 0 && static_cast<int>(c.chat_drops.size()) >= config_.chat_flood_kick) {
        ++abuse_.chat_kicks;
        log("abuse: " + c.name + " (" + ipv4_text(c.peer_ip) + ") disconnected for chat flooding (chat_flood_kick=" +
            std::to_string(config_.chat_flood_kick) + ")");
        server_message(c, notice::Kicked, "Disconnected by the server: chat flooding.");
        log_out(c, "chat flood");
        flood_disconnect(c, "chat_flood_kick", now);
    } else {
        server_message(c, notice::Info, "You are sending messages too fast; that message was not delivered.");
    }
    return false;
}

void OmegaServer::flood_disconnect(Conn& c, const std::string& why, std::uint64_t now) {
    ++abuse_.flood_disconnects;
    c.closing = true;
    if (close_) close_(c.id);
    if (config_.flood_block_after <= 0 || config_.flood_block == 0) return;
    Floods& f = floods_[c.peer_ip];
    f.times.erase(std::remove_if(f.times.begin(), f.times.end(),
                                 [&](std::uint64_t t) { return t + config_.flood_window <= now; }),
                  f.times.end());
    f.times.push_back(now);
    if (static_cast<int>(f.times.size()) >= config_.flood_block_after && f.blocked_until <= now) {
        f.blocked_until = now + config_.flood_block;
        ++abuse_.blocks;
        log("abuse: " + ipv4_text(c.peer_ip) + " blocked for " + std::to_string(config_.flood_block) + " s after " +
            std::to_string(f.times.size()) + " flood disconnects within " + std::to_string(config_.flood_window) +
            " s (the last: " + why + ")");
    }
}

std::uint64_t OmegaServer::blocked_for(std::uint32_t ip, std::uint64_t now) const {
    auto it = floods_.find(ip);
    return it != floods_.end() && it->second.blocked_until > now ? it->second.blocked_until - now : 0;
}

// Forgets the buckets that refilled and the flood records that expired (once a minute, from tick()).
void OmegaServer::maintain(std::uint64_t now) {
    if (now < maintained_at_ + 60) return;
    maintained_at_ = now;
    std::erase_if(login_by_ip_, [&](const auto& kv) {
        return bucket_full(kv.second, config_.login_attempts_per_ip, config_.login_attempts_per_ip, now);
    });
    std::erase_if(login_by_name_, [&](const auto& kv) {
        return bucket_full(kv.second, config_.login_attempts_per_account, config_.login_attempts_per_account, now);
    });
    std::erase_if(chat_by_ip_, [&](const auto& kv) {
        return bucket_full(kv.second, config_.chat_per_minute_ip, config_.chat_per_minute_ip, now);
    });
    std::erase_if(floods_, [&](const auto& kv) {
        return kv.second.blocked_until <= now &&
               std::all_of(kv.second.times.begin(), kv.second.times.end(),
                           [&](std::uint64_t t) { return t + config_.flood_window <= now; });
    });
}

void OmegaServer::on_disconnect(std::uint32_t conn) {
    auto it = conns_.find(conn);
    if (it == conns_.end()) return;
    log_out(it->second, "disconnected");
    log("conn " + std::to_string(conn) + " closed");
    conns_.erase(it);
}

void OmegaServer::on_frame(std::uint32_t conn, const Frame& frame) {
    auto it = conns_.find(conn);
    if (it == conns_.end()) return;
    Conn& c = it->second;
    if (c.closing || !within_rate(c)) return;
    const std::span<const std::uint8_t> b(frame.body);
    if (frame.type >= 0xF000) {
        if (c.kind == ClientKind::Port) on_ext(c, frame);  // an original session never speaks the extensions
        return;
    }
    auto sized = [&](std::size_t need) {
        if (b.size() >= need) return true;
        log("conn " + std::to_string(conn) + ": Medius id " + std::to_string(frame.type) + " too short (" +
            std::to_string(b.size()) + " < " + std::to_string(need) + ")");
        return false;
    };
    switch (frame.type) {
    case mid::SessionBegin: if (sized(msize::SessionBegin)) on_session_begin(c, b); break;
    case mid::AccountRegistration: if (sized(msize::AccountRegistration)) on_registration(c, b); break;
    case mid::AccountLogin: if (sized(msize::AccountLogin)) on_login(c, b); break;
    case mid::AccountLogout: if (sized(msize::AccountLogout)) on_logout(c, b); break;
    case mid::GetPolicy: if (sized(msize::GetPolicy)) on_policy(c, b); break;
    case mid::GetAnnouncements:
    case mid::GetAllAnnouncements: if (sized(msize::GetAnnouncements)) on_announcements(c, b); break;
    case mid::AccountUpdateStats: if (sized(msize::AccountUpdateStats)) on_update_stats(c, b); break;
    case mid::GetAccountStats: if (sized(msize::GetAccountStats)) on_get_stats(c, b); break;
    case mid::PlayerReport: if (sized(msize::PlayerReport)) on_player_report(c, b); break;
    case mid::WorldReport: if (sized(msize::WorldReport)) on_world_report(c, b); break;
    case mid::CreateGame: if (sized(msize::CreateGame)) on_create_game(c, b); break;
    case mid::JoinGame: if (sized(msize::JoinGame)) on_join_game(c, b); break;
    case mid::GameList: if (sized(msize::GameList)) on_game_list(c, b); break;
    case mid::GameInfo: if (sized(msize::GameInfo)) on_game_info(c, b); break;
    case mid::ChatMessage: if (sized(msize::ChatMessage)) on_chat(c, b); break;
    case mid::TextFilter: if (sized(msize::TextFilter)) on_text_filter(c, b); break;
    case mid::PlayerList: if (sized(msize::PlayerList)) on_player_list(c, b); break;
    case mid::GetBuddyListExtraInfo: if (sized(msize::GetBuddyListExtraInfo)) on_buddy_list(c, b); break;
    case mid::GetBuddyInvitations: if (sized(msize::GetBuddyInvitations)) on_buddy_invitations(c, b); break;
    case mid::AddToBuddyListConfirmation: if (sized(msize::AddToBuddyListConfirmation)) on_buddy_add(c, b); break;
    case mid::AddToBuddyListFwdConfirmationResponse:
        if (sized(msize::AddToBuddyListFwdConfirmationResponse)) on_buddy_answer(c, b);
        break;
    case mid::RemoveFromBuddyList: if (sized(msize::RemoveFromBuddyList)) on_buddy_remove(c, b); break;
    case mid::AccountGetID: if (sized(msize::AccountGetID)) on_account_get_id(c, b); break;
    default:
        log("conn " + std::to_string(conn) + ": unhandled Medius id " + std::to_string(frame.type) + " (" +
            std::to_string(b.size()) + " bytes)");
        break;
    }
}

void OmegaServer::on_session_begin(Conn& c, std::span<const std::uint8_t> req) {
    c.session_key = to_hex(random_bytes(8));  // 16 characters + NUL = SessionKey[17]
    auto r = response(req, msize::SessionBeginResponse);
    put_str(r, kSessionKeyOffset, kSessionKeyLength, c.session_key);  // 0x00446d00 copies resp+0x15 to lobby+0x2429
    put_i32(r, 0x2c, status::Success);                               // 0x00446d00 tests +0x2c
    send(c.id, mid::SessionBeginResponse, r);
}

void OmegaServer::on_registration(Conn& c, std::span<const std::uint8_t> req) {
    const std::string name = get_str(req, 0x2c, 32);      // 0x00445b90: AccountName at +0x2c
    const std::string password = get_str(req, 0x4c, 32);  // Password at +0x4c
    const std::uint64_t now = clock_ ? clock_() : 0;
    if (!login_attempt_allowed(c, name, true, now)) {
        auto r = response(req, msize::AccountRegistrationResponse);
        put_i32(r, kStatusOffset, status::WMError);
        send(c.id, mid::AccountRegistrationResponse, r);
        return;
    }
    if (config_.registrations_per_hour > 0) {
        // PORT-ONLY operations (132on2d): new accounts per address per hour.
        auto& times = registrations_[c.peer_ip];
        times.erase(std::remove_if(times.begin(), times.end(), [&](std::uint64_t t) { return t + 3600 <= now; }),
                    times.end());
        if (static_cast<int>(times.size()) >= config_.registrations_per_hour) {
            log("limit: account create refused for " + ipv4_text(c.peer_ip) + ", " + std::to_string(times.size()) +
                " this hour (registrations_per_hour=" + std::to_string(config_.registrations_per_hour) + ")");
            send_notice(c, notice::Info, "Too many new accounts from your address; try again later.");
            auto r = response(req, msize::AccountRegistrationResponse);
            put_i32(r, kStatusOffset, status::WMError);
            send(c.id, mid::AccountRegistrationResponse, r);
            return;
        }
    }
    std::uint32_t id = 0;
    auto r = response(req, msize::AccountRegistrationResponse);
    // The quick refusals first (no password work); then the digest, on a worker when the server has a runner.
    CreateResult quick = CreateResult::Created;
    if (!AccountStore::valid_name(name)) quick = CreateResult::BadName;
    else if (!AccountStore::valid_password(password)) quick = CreateResult::BadPassword;
    else if (accounts_.find(name)) quick = CreateResult::NameTaken;
    if (quick != CreateResult::Created || c.hashing) {
        const std::int32_t code = quick == CreateResult::NameTaken ? status::AccountAlreadyExists : status::WMError;
        log("account create refused: '" + name + "' status " + std::to_string(code) +
            (c.hashing ? " (a login / account creation of this connection is still in progress)" : ""));
        put_i32(r, kStatusOffset, code);
        put_i32(r, 0x1c, static_cast<std::int32_t>(id));
        send(c.id, mid::AccountRegistrationResponse, r);
        return;
    }
    const AccountStore::HashParams params = accounts_.new_hash_params();
    run_password_work(
        c, [password, params] { return pbkdf2_sha256(password, params.salt, params.iterations, 32); },
        [this, name, params, r](Conn& conn, std::vector<std::uint8_t> digest) {
            finish_registration(conn, name, params, digest, r);
        });
}

void OmegaServer::finish_registration(Conn& c, const std::string& name, const AccountStore::HashParams& params,
                                      std::span<const std::uint8_t> digest, std::vector<std::uint8_t> r) {
    const std::uint64_t now = clock_ ? clock_() : 0;
    std::uint32_t id = 0;
    const CreateResult result = accounts_.create_with_digest(name, params, digest, now, &id);
    std::int32_t code = status::Success;
    switch (result) {
    case CreateResult::Created:
        registrations_[c.peer_ip].push_back(now);
        accounts_dirty_ = true;  // written by tick() (132on2h)
        log("account created: " + name + " (id " + std::to_string(id) + ")");
        break;
    case CreateResult::NameTaken: code = status::AccountAlreadyExists; break;  // 0x00447270: error 0x3d
    default: code = status::WMError; break;
    }
    if (code != status::Success) log("account create refused: '" + name + "' status " + std::to_string(code));
    put_i32(r, kStatusOffset, code);  // 0x00447270 copies the struct to lobby+0x314 and tests +0x18
    put_i32(r, 0x1c, static_cast<std::int32_t>(id));
    send(c.id, mid::AccountRegistrationResponse, r);
}

void OmegaServer::run_password_work(Conn& c, PasswordWork work, std::function<void(Conn&, std::vector<std::uint8_t>)> then) {
    c.hashing = true;
    ++hash_pending_;
    ++hash_jobs_;
    const std::uint32_t conn = c.id;
    PasswordDone done = [this, conn, then = std::move(then)](std::vector<std::uint8_t> result) {
        --hash_pending_;
        auto it = conns_.find(conn);
        if (it == conns_.end() || it->second.closing) return;  // gone meanwhile: nothing to answer
        it->second.hashing = false;
        then(it->second, std::move(result));
    };
    if (hash_runner_) hash_runner_(std::move(work), std::move(done));
    else done(work());
}

void OmegaServer::on_login(Conn& c, std::span<const std::uint8_t> req) {
    const std::string name = get_str(req, 0x26, 32);      // 0x00445420: AccountName from lobby+0x243d
    const std::string password = get_str(req, 0x46, 32);  // Password from lobby+0x245d
    auto r = response(req, msize::AccountLoginResponse);
    const std::uint64_t now = clock_ ? clock_() : 0;
    if (c.hashing) {
        log("login refused: '" + name + "' on conn " + std::to_string(c.id) +
            ", a login / account creation of this connection is still in progress");
        put_i32(r, kStatusOffset, status::PlayerNotPrivileged);
        send(c.id, mid::AccountLoginResponse, r);
        return;
    }
    if (!login_attempt_allowed(c, name, false, now)) {
        // PORT-ONLY anti-abuse (132on2h): the attempts themselves are rate limited, before the password is hashed.
        put_i32(r, kStatusOffset, status::PlayerNotPrivileged);
        send(c.id, mid::AccountLoginResponse, r);
        return;
    }
    if (const std::uint64_t locked = login_locked(name, c.peer_ip, now)) {
        // PORT-ONLY operations (132on2d): too many failed logins; refused before the password is even hashed.
        send_notice(c, notice::Info, "Too many failed logins. Try again in " + std::to_string((locked + 59) / 60) +
                                         " minute(s).");
        log("login refused: '" + name + "' from " + ipv4_text(c.peer_ip) + " locked for " + std::to_string(locked) +
            " s (failed logins)");
        put_i32(r, kStatusOffset, status::PlayerNotPrivileged);
        send(c.id, mid::AccountLoginResponse, r);
        return;
    }
    const AccountStore::HashParams params = accounts_.hash_params(name);
    run_password_work(
        c, [password, params] { return pbkdf2_sha256(password, params.salt, params.iterations, 32); },
        [this, name, password, r](Conn& conn, std::vector<std::uint8_t> digest) {
            finish_login(conn, name, password, digest, r);
        });
}

void OmegaServer::finish_login(Conn& c, const std::string& name, const std::string& password,
                               std::span<const std::uint8_t> digest, std::vector<std::uint8_t> r) {
    const std::uint64_t now = clock_ ? clock_() : 0;
    const Account* account = nullptr;
    const CheckResult check = accounts_.check_digest(name, digest, now, &account);
    std::int32_t code = status::Success;
    if (check == CheckResult::NotFound) {
        code = status::AccountNotFound;
        login_failed(name, c.peer_ip, now);
    } else if (check == CheckResult::WrongPassword) {
        code = status::InvalidPassword;
        login_failed(name, c.peer_ip, now);
    } else if (check == CheckResult::Banned) {
        // PORT-ONLY reason text first, then the refusal through a status the client's own login callback handles.
        std::string why = "You are banned";
        if (account->ban_until != kBanForever) why += " for " + std::to_string((account->ban_until - now + 59) / 60) + " more minute(s)";
        if (!account->ban_reason.empty()) why += ": " + account->ban_reason;
        send_notice(c, notice::Banned, why);
        code = status::PlayerNotPrivileged;
    } else if (c.policy_seen != config_.policy_version) {
        // PORT-ONLY rule: the original fetches and shows the policy after DNAS and before this login (0x0029d390 ->
        // 0x004456d0 / 0x004456c0, Accept -> 0x0029dfc0 -> ACNTLOGN), so reaching the login means it was accepted;
        // a session that did not fetch the current version (the policy changed meanwhile) must reconnect and accept.
        send_notice(c, notice::Info, "The usage policy changed. Reconnect to read and accept the new version.");
        code = status::PlayerNotPrivileged;
    } else if (Conn* other = conn_of_account(account->id); other && other->id != c.id) {
        // 0x00445a40 answers -997 with a logout and a second login; the stale session is dropped so it succeeds.
        log("login: " + account->name + " already logged in on conn " + std::to_string(other->id) + ", dropping it");
        kick_conn(*other, "logged in from another session");
        code = status::AccountLoggedIn;
    }
    if (code != status::Success) {
        log("login refused: '" + name + "' status " + std::to_string(code));
        put_i32(r, kStatusOffset, code);
        send(c.id, mid::AccountLoginResponse, r);
        return;
    }
    c.account_id = account->id;
    c.name = account->name;
    c.admin = account->admin;
    // Checkpoint 132on2d: the login puts the session in the lobby (omega_server has one chat channel; 132on1 waited
    // for message 17, which is the statistics upload, ONLINE.md §3.6).
    c.in_channel = true;
    login_failures_.erase(name_key(name, c.peer_ip));
    const std::uint32_t id_logged_in = account->id;
    const std::uint32_t old_iterations = account->iterations;
    if (accounts_.upgrade_hash(id_logged_in, password)) {
        log("accounts: " + c.name + "'s password digest upgraded to " + std::to_string(accounts_.iterations()) +
            " PBKDF2 iterations (was " + std::to_string(old_iterations) + ")");
        accounts_.save();
    }
    account = accounts_.find(id_logged_in);
    if (account->accepted_policy != config_.policy_version) {
        log("policy: " + c.name + " accepted version " + std::to_string(config_.policy_version) + " (was " +
            std::to_string(account->accepted_policy) + ")");
        accounts_.set_accepted_policy(account->id, config_.policy_version);
        accounts_dirty_ = true;
    }
    put_i32(r, kStatusOffset, status::Success);
    put_i32(r, 0x1c, static_cast<std::int32_t>(account->id));  // 0x004475e0: account id -> lobby+0x2480
    put_i32(r, 0x20, 1);
    put_connect_info(r, 0x28, config_.public_ip, config_.port, 0, c.session_key, to_hex(random_bytes(8)));
    send(c.id, mid::AccountLoginResponse, r);
    log("login: " + c.name + " (id " + std::to_string(c.account_id) + ", " + std::string(platform_name(c.platform())) +
        ") on conn " + std::to_string(c.id));
    // PORT-ONLY (132on2e): the relay endpoint, for a port session that announced cap::Relay (send_ext gates it; the
    // endpoint is only opened for such a session, so an original session never has one).
    if (relay_ && c.has(cap::Relay)) {
        if (const auto ticket = relay_->open(c.id, relay_port_, c.name))
            send_ext(c, cap::Relay, ext::RelayTicket, encode_relay_ticket(*ticket));
    }
    broadcast_roster();
}

void OmegaServer::on_logout(Conn& c, std::span<const std::uint8_t> req) {
    log_out(c, "logout");
    auto r = response(req, msize::AccountLogoutResponse);
    put_i32(r, kStatusOffset, status::Success);  // 0x004474c0 tests +0x18
    send(c.id, mid::AccountLogoutResponse, r);
}

void OmegaServer::on_policy(Conn& c, std::span<const std::uint8_t> req) {
    // 0x004456e0: MessageID "USAGE" (a1 = 0, 0x004456d0) or "PRIVACY" (a1 = 1, 0x004456c0), Policy type at +0x28.
    // The callback 0x004473c0 appends +0x1c (text[256]) to the usage list (lobby +0x23f8) when the echoed MessageID
    // is "USAGE", else to the privacy list (+0x23fc); a nonzero status ends the text with nothing appended; the
    // request loop waits for byte +0x11c (EndOfText) of the copy at lobby +0x484.
    const bool privacy = get_i32(req, 0x28) == 1;
    const std::string& text = privacy ? config_.privacy_policy : config_.usage_policy;
    if (!privacy) c.policy_seen = config_.policy_version;
    if (text.empty()) {
        auto r = response(req, msize::GetPolicyResponse);
        put_i32(r, kStatusOffset, status::NoResult);
        r[0x11c] = 1;
        send(c.id, mid::GetPolicyResponse, r);
        return;
    }
    std::size_t pos = 0;
    while (pos < text.size()) {
        auto r = response(req, msize::GetPolicyResponse);
        const std::string chunk = text.substr(pos, 255);
        pos += chunk.size();
        put_i32(r, kStatusOffset, status::Success);
        put_str(r, 0x1c, 256, chunk);
        r[0x11c] = pos >= text.size() ? 1 : 0;  // EndOfText
        send(c.id, mid::GetPolicyResponse, r);
    }
}

void OmegaServer::on_announcements(Conn& c, std::span<const std::uint8_t> req) {
    // 0x00445570 (MessageID "", +0x28 = 0x00476998(), the MCL's application id). The callback 0x004472c0 appends
    // {+0x1c id, +0x20 text[1000]} to the announcement list (lobby +0x2400) unless the text starts with
    // "{SFOLOBBY_SETTINGS}" (handed to 0x00443bf0 instead); a nonzero status ends the list; the loop waits for byte
    // +0x408 (EndOfList). 0x0029cbb0 shows the rows of the first id as one page, and each OK shows the next id.
    std::vector<std::pair<std::int32_t, std::string>> rows;
    for (std::size_t i = 0; i < config_.announcements.size(); ++i)
        for (std::size_t pos = 0; pos < config_.announcements[i].size(); pos += 999)
            rows.emplace_back(static_cast<std::int32_t>(i + 1), config_.announcements[i].substr(pos, 999));
    if (rows.empty()) {
        auto r = response(req, msize::GetAnnouncementsResponse);
        put_i32(r, kStatusOffset, status::NoResult);
        r[0x408] = 1;
        send(c.id, mid::GetAnnouncementsResponse, r);
        return;
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        auto r = response(req, msize::GetAnnouncementsResponse);
        put_i32(r, kStatusOffset, status::Success);
        put_i32(r, 0x1c, rows[i].first);
        put_str(r, 0x20, 1000, rows[i].second);
        r[0x408] = i + 1 == rows.size() ? 1 : 0;  // EndOfList
        send(c.id, mid::GetAnnouncementsResponse, r);
    }
}

// ---- Checkpoint 132on2d: the agent statistics (ONLINE.md §3.6) ----------------------------------------------------

// 004505e0's AccountUpdateStats: MessageID "1", SessionKey +15, Stats[256] +26 (the record 002cbd40 builds). The
// callback 00446cc0 tests the status (+18).
void OmegaServer::on_update_stats(Conn& c, std::span<const std::uint8_t> req) {
    auto r = response(req, msize::AccountUpdateStatsResponse);
    if (!c.account_id) {
        put_i32(r, kStatusOffset, status::WMError);
    } else {
        c.in_channel = true;
        keep_stats(c, req.subspan(0x26, kStatsBlobSize), "AccountUpdateStats");
        put_i32(r, kStatusOffset, status::Success);
    }
    send(c.id, mid::AccountUpdateStatsResponse, r);
}

// 00451200 / 0044df20's GetAccountStats: the account id at +28. The response (0x148) as 00446db0 / 004467d0 read it:
// +18 status, +1c the account's name [32], +48 its Stats[256]. An account without a record answers zero bytes (the
// client's 002cbd40 then starts a new record: it rewrites a record whose byte 0 is not 0xb).
void OmegaServer::on_get_stats(Conn& c, std::span<const std::uint8_t> req) {
    const auto id = static_cast<std::uint32_t>(get_i32(req, 0x28));
    auto r = response(req, msize::GetAccountStatsResponse);
    const Account* a = c.account_id ? accounts_.find(id) : nullptr;
    if (!c.account_id) {
        put_i32(r, kStatusOffset, status::WMError);
    } else if (!a) {
        put_i32(r, kStatusOffset, status::AccountNotFound);
    } else {
        put_i32(r, kStatusOffset, status::Success);
        put_str(r, 0x1c, 32, a->name);
        if (const AccountStatsRecord* s = stats_.find(id)) std::copy(s->blob.begin(), s->blob.end(), r.begin() + 0x48);
    }
    send(c.id, mid::GetAccountStatsResponse, r);
}

// 004446e0's PlayerReport (every 15 seconds in a network game; no response): SessionKey +0, WorldID +14 (0x4976d8),
// Stats[256] +18 (the record at 0x585e90).
void OmegaServer::on_player_report(Conn& c, std::span<const std::uint8_t> req) {
    if (!c.account_id) return;
    const auto world = static_cast<std::uint32_t>(get_i32(req, 0x14));
    if (auto it = games_.find(c.world_id); it != games_.end() && (world == c.world_id || world == 0))
        it->second.reported = true;
    keep_stats(c, req.subspan(0x18, kStatsBlobSize), "PlayerReport");
}

// 00444944's WorldReport (the host, every 15 seconds or when forced; no response): +14 WorldID, +18 PlayerCount,
// +1c GameName [64], +5c GameStats (0x18 bytes from 0x585e70), +15c MinPlayers .. +17c WorldStatus (00444800: 1 while
// 0042b770 says the game takes players, else 3; GenericField1 +170 the same). As Medius did, the status is the game
// list's: a game its host reports closed (3) refuses joins.
void OmegaServer::on_world_report(Conn& c, std::span<const std::uint8_t> req) {
    if (!c.account_id) return;
    const auto world = static_cast<std::uint32_t>(get_i32(req, 0x14));
    auto it = games_.find(world);
    if (it == games_.end() || it->second.host_conn != c.id) return;
    ServerGame& g = it->second;
    WorldReportFields w;
    w.world = static_cast<std::int32_t>(world);
    w.player_count = get_i32(req, 0x18);
    w.game_name = get_str(req, 0x1c, 64);
    w.game_stats.assign(req.begin() + 0x5c, req.begin() + 0x5c + kStatsFieldSize);
    w.min_players = get_i32(req, 0x15c);
    w.max_players = get_i32(req, 0x160);
    w.game_level = get_i32(req, 0x164);
    w.skill_level = get_i32(req, 0x168);
    w.rules_set = get_i32(req, 0x16c);
    for (int i = 0; i < 3; ++i) w.generic[i] = get_i32(req, 0x170 + 4 * static_cast<std::size_t>(i));
    w.world_status = get_i32(req, 0x17c);
    const bool first = !g.world_report;
    const bool status_changed = first || g.world_report->world_status != w.world_status;
    const bool count_changed = first || g.world_report->player_count != w.player_count;
    g.world_report = w;
    g.reported = true;
    if (w.world_status >= kWorldStaging && w.world_status <= kWorldClosed) g.world_status = w.world_status;
    if (status_changed || count_changed)
        log("game " + std::to_string(world) + " world report: " + std::to_string(w.player_count) + " player(s), status " +
            std::to_string(w.world_status) + (w.world_status == kWorldClosed ? " (closed to joins)" : ""));
}

void OmegaServer::keep_stats(Conn& c, std::span<const std::uint8_t> blob, const char* source) {
    if (!valid_stats(blob)) {
        log("stats: " + c.name + " " + source + " ignored (byte 0 is not version 0x0b)");
        return;
    }
    const AccountStatsRecord* before = stats_.find(c.account_id);
    const bool changed = !before || !std::equal(before->blob.begin(), before->blob.end(), blob.begin());
    const std::string change = before && changed ? describe_stats_change(before->blob, blob) : std::string();
    stats_.update(c.account_id, blob, clock_ ? clock_() : 0);
    if (auto it = games_.find(c.world_id); it != games_.end()) {
        GameResultPlayer& p = it->second.results[c.account_id];
        p.account = c.account_id;
        p.name = c.name;
        std::copy_n(blob.begin(), kStatsBlobSize, p.final.begin());
    }
    if (changed) {
        log("stats: " + c.name + " " + source + (before ? ": " + change : ": first record") + " -> " + describe_stats(blob));
        mark_stats_dirty();
    }
}

// A player enters a game (create / join): its record so far is the result's starting point.
void OmegaServer::enter_world_stats(ServerGame& g, const Conn& c) {
    GameResultPlayer& p = g.results[c.account_id];
    p.account = c.account_id;
    p.name = c.name;
    if (const AccountStatsRecord* s = stats_.find(c.account_id)) p.entry = p.final = s->blob;
}

void OmegaServer::mark_stats_dirty() {
    stats_dirty_ = true;
    const std::uint64_t now = clock_ ? clock_() : 0;
    if (now >= stats_saved_at_ + 30) flush_stats();
}

void OmegaServer::flush_stats() {
    if (!stats_dirty_) return;
    try {
        stats_.save();
        stats_dirty_ = false;
        stats_saved_at_ = clock_ ? clock_() : 0;
    } catch (const std::exception& e) {
        log(std::string("stats: ") + e.what());
    }
}

std::string OmegaServer::stats_command(const std::vector<std::string>& w) {
    std::ostringstream out;
    auto name_of = [&](std::uint32_t id) {
        const Account* a = accounts_.find(id);
        return a ? a->name : "#" + std::to_string(id);
    };
    if (w[0] == "stats") {
        if (w.size() >= 2) {
            const Account* a = accounts_.find(w[1]);
            if (!a) return "stats: no account '" + w[1] + "'";
            const AccountStatsRecord* s = stats_.find(a->id);
            if (!s) return a->name + ": no statistics record yet";
            out << a->name << " (id " << a->id << "): " << describe_stats(s->blob) << "\n  " << s->reports
                << " report(s), " << s->games << " finished game(s), updated " << s->updated;
            return out.str();
        }
        out << "statistics records: " << stats_.records().size();
        for (const auto& [id, s] : stats_.records())
            out << "\n  " << name_of(id) << ": " << describe_stats(s.blob) << " (" << s.games << " game(s))";
        return out.str();
    }
    std::size_t n = 10;
    if (w.size() >= 2) n = std::max<std::size_t>(1, std::strtoul(w[1].c_str(), nullptr, 10));
    const auto& all = stats_.results();
    out << "finished games: " << all.size();
    for (std::size_t i = all.size() > n ? all.size() - n : 0; i < all.size(); ++i) {
        const GameResult& g = all[i];
        out << "\n  world " << g.world << " '" << g.game_name << "' host " << name_of(g.host) << ", " << g.seconds
            << " s, ended " << g.ended << " (" << g.why << ")";
        for (const auto& p : g.players) out << "\n    " << p.name << ": " << describe_stats_change(p.entry, p.final);
    }
    return out.str();
}

std::uint64_t OmegaServer::login_locked(const std::string& name, std::uint32_t ip, std::uint64_t now) {
    if (config_.login_failures <= 0) return 0;
    std::uint64_t left = 0;
    for (const std::string& key : {name_key(name, ip), address_key(ip)}) {
        auto it = login_failures_.find(key);
        if (it == login_failures_.end()) continue;
        if (it->second.locked_until > now) left = std::max(left, it->second.locked_until - now);
        else if (it->second.locked_until || it->second.first + config_.login_window <= now) login_failures_.erase(it);
    }
    return left;
}

void OmegaServer::login_failed(const std::string& name, std::uint32_t ip, std::uint64_t now) {
    if (config_.login_failures <= 0) return;
    const std::pair<std::string, int> keys[] = {{name_key(name, ip), config_.login_failures},
                                                {address_key(ip), config_.login_failures * 4}};
    for (const auto& [key, limit] : keys) {
        Failures& f = login_failures_[key];
        if (f.count == 0 || f.first + config_.login_window <= now) f = Failures{0, now, 0};
        if (++f.count >= limit) {
            f.locked_until = now + config_.login_lockout;
            log("limit: " + std::to_string(f.count) + " failed logins for " + key + ", locked for " +
                std::to_string(config_.login_lockout) + " s");
        }
    }
}

void OmegaServer::on_create_game(Conn& c, std::span<const std::uint8_t> req) {
    if (refused_by_mod_policy(c, req, true)) return;
    auto r = response(req, msize::CreateGameResponse);
    if (!c.account_id) {
        put_i32(r, kStatusOffset, status::WMError);
        send(c.id, mid::CreateGameResponse, r);
        return;
    }
    leave_world(c);
    ServerGame g;
    g.world_id = next_world_++;
    g.game_name = get_str(req, 0x30, 64);  // 0x004524c0: the name argument at +0x30
    g.create_tail.assign(req.begin() + 0x70, req.begin() + 0x94);
    g.host_conn = c.id;
    g.host_account = c.account_id;
    g.host_ip = c.peer_ip;
    g.host_port = config_.default_game_port;
    g.players.push_back(c.account_id);
    g.max_players = config_.max_players;
    g.created = clock_ ? clock_() : 0;
    if (const auto fields = parse_game_name(g.game_name)) g.max_players = std::clamp(fields->a, 2, config_.max_players);
    c.world_id = g.world_id;
    games_[g.world_id] = g;
    enter_world_stats(games_[g.world_id], c);
    put_i32(r, kStatusOffset, status::Success);
    put_i32(r, 0x1c, static_cast<std::int32_t>(g.world_id));
    send(c.id, mid::CreateGameResponse, r);
    log("game " + std::to_string(g.world_id) + " created by " + c.name + ": '" + g.game_name + "'");
    broadcast_roster();
}

void OmegaServer::on_join_game(Conn& c, std::span<const std::uint8_t> req) {
    if (refused_by_mod_policy(c, req, false)) return;
    const auto world = static_cast<std::uint32_t>(get_i32(req, 0x28));  // 0x0044e430: world id at +0x28
    auto r = response(req, msize::JoinGameResponse);
    auto it = games_.find(world);
    std::int32_t code = status::Success;
    if (!c.account_id) code = status::WMError;
    else if (it == games_.end()) code = status::GameNotFound;
    else if (it->second.world_status == kWorldClosed ||
             static_cast<int>(it->second.players.size()) >= it->second.max_players)
        code = status::RequestDenied;
    else if (!it->second.password.empty() && get_str(req, 0x30, 32) != it->second.password)
        code = status::InvalidPassword;  // PORT-ONLY (132on2c): the GamePassword +0x30 the original leaves zero
    if (code != status::Success) {
        put_i32(r, kStatusOffset, code);
        send(c.id, mid::JoinGameResponse, r);
        log("join refused: " + c.name + " -> world " + std::to_string(world) + " status " + std::to_string(code) +
            (code == status::InvalidPassword ? " (wrong game password)" : ""));
        return;
    }
    ServerGame& g = it->second;
    if (c.world_id != world) {
        leave_world(c);
        // 132on2g (PORT-ONLY store): each pair now in the game has played together (the recent players).
        const std::uint64_t now = clock_ ? clock_() : 0;
        for (const std::uint32_t other : g.players) {
            social_.played_with(c.account_id, other, world, now);
            social_.played_with(other, c.account_id, world, now);
        }
        social_dirty_ = true;  // written by tick() (132on2h)
        g.players.push_back(c.account_id);
        c.world_id = world;
        enter_world_stats(g, c);
    }
    // The host-address hand-off: 0x0044e280 copies +0x20.. (0x27 words) to lobby+0x1c50, which 0x0044e430 passes to
    // the game's net connect 0x0042a960.
    put_i32(r, kStatusOffset, status::Success);
    put_i32(r, 0x1c, kGameHostPeerToPeer);
    put_connect_info(r, 0x20, ipv4_text(g.host_ip), g.host_port, g.world_id, c.session_key, to_hex(random_bytes(8)));
    // PORT-ONLY (132on2e): the host's relay address, before the response the joiner's hand-off acts on.
    if (relay_ && c.has(cap::Relay) && relay_->virtual_ip(c.id)) {
        if (const auto host_vip = relay_->virtual_ip(g.host_conn)) {
            send_ext(c, cap::Relay, ext::RelayPeer, encode_relay_peer(RelayPeer{world, *host_vip, relay::kVirtualPort}));
            log("relay: " + c.name + " may reach world " + std::to_string(world) + "'s host at " +
                relay_vip_text(*host_vip) + ":" + std::to_string(relay::kVirtualPort));
        }
    }
    send(c.id, mid::JoinGameResponse, r);
    log("join: " + c.name + " -> world " + std::to_string(world) + " host " + ipv4_text(g.host_ip) + ":" +
        std::to_string(g.host_port));
    broadcast_roster();
}

void OmegaServer::on_game_list(Conn& c, std::span<const std::uint8_t> req) {
    std::vector<const ServerGame*> open;
    for (const auto& [id, g] : games_)
        if (g.world_status != kWorldClosed) open.push_back(&g);
    if (open.empty()) {
        auto r = response(req, msize::GameListResponse);
        put_i32(r, kStatusOffset, status::NoResult);  // 0x00447790 skips the row, then sees EndOfList
        r[0x6c] = 1;
        send(c.id, mid::GameListResponse, r);
        return;
    }
    for (std::size_t i = 0; i < open.size(); ++i) {
        const ServerGame& g = *open[i];
        auto r = response(req, msize::GameListResponse);
        put_i32(r, kStatusOffset, status::Success);
        put_i32(r, 0x1c, static_cast<std::int32_t>(g.world_id));  // 0x00447790 copies +0x1c to the entry
        put_str(r, 0x20, 64, g.game_name);                       // parsed with "%32s\t%d\t%d\t%d\t%d"
        // Checkpoint 132on2b: 00447790 copies +60 (WorldStatus) to the row's +30 and +68 (PlayerCount) to +34.
        put_i32(r, 0x60, g.world_status);
        put_i32(r, 0x64, kGameHostPeerToPeer);
        put_i32(r, 0x68, static_cast<std::int32_t>(g.players.size()));
        r[0x6c] = i + 1 == open.size() ? 1 : 0;  // EndOfList (lb +0x6c)
        send(c.id, mid::GameListResponse, r);
    }
}

void OmegaServer::on_game_info(Conn& c, std::span<const std::uint8_t> req) {
    const auto world = static_cast<std::uint32_t>(get_i32(req, 0x28));
    auto r = response(req, msize::GameInfoResponse);
    auto it = games_.find(world);
    if (it == games_.end()) {
        put_i32(r, kStatusOffset, status::GameNotFound);
    } else {
        const ServerGame& g = it->second;
        put_i32(r, kStatusOffset, status::Success);
        put_i32(r, 0x20, 1);
        put_i32(r, 0x24, g.max_players);                                  // 0x00446b40 passes +0x24
        put_i32(r, 0x30, static_cast<std::int32_t>(g.players.size()));    // and +0x30
        // +0x34.. is GameStats (256 bytes). Checkpoint 132on2b: JOINMIS's join 0033e5b0 reads +0x34 (the creator's id,
        // the name's 5th field) and +0x38 (the mission, its 3rd); PORT-ONLY: omega_server derives these GameStats from
        // the game name 0044e960 sent (the original's server echoed the creator's own GameStats).
        if (const auto fields = parse_game_name(g.game_name)) {
            put_i32(r, 0x34, fields->d);
            put_i32(r, 0x38, fields->b);
            put_i32(r, 0x40, fields->c);  // 132on2i: GameStats +c, the filter (0044edc0 caches it; 0044eff0's +44)
        }
        // 132on2i: GameStats +10, the game's seconds (0044eff0's +3c), from the host's last WorldReport's GameStats
        // (00585e70 +10, 004446e0). Its other words stay derived: the port's GameStats +8 is not the world.
        if (g.world_report && g.world_report->game_stats.size() >= 0x14)
            put_i32(r, 0x44, get_i32(g.world_report->game_stats, 0x10));
        put_i32(r, 0x3c, static_cast<std::int32_t>(g.world_id));          // +0x3c: matched against the list's world ids
        put_str(r, 0x134, 64, g.game_name);
        put_i32(r, 0x184, g.world_status);                                // +0x184: the world status
        put_i32(r, 0x188, kGameHostPeerToPeer);
    }
    send(c.id, mid::GameInfoResponse, r);
}

// 0046cef8 / 00447c70 read ChatFwdMessage as +18 the sender's id, +1c its name [32], +3c the type (1 private) and
// +40 the text [64] (132on2c: the layout was +1c type / +20 name before). Nobody gets their own message back: the
// sender's echo is its TextFilter response (00447bb0).
void OmegaServer::on_chat(Conn& c, std::span<const std::uint8_t> req) {
    if (!c.account_id) return;
    if (!chat_allowed(c, clock_ ? clock_() : 0)) return;  // 132on2h anti-abuse
    const std::int32_t type = get_i32(req, 0x28);  // 0x00451050: 1 when a target is given (whisper), else 0
    const std::int32_t target = get_i32(req, 0x2c);
    const std::string text = get_str(req, 0x30, 64);
    std::vector<std::uint8_t> fwd(msize::ChatFwdMessage, 0);
    put_str(fwd, 0, kMessageIdLength, get_str(req, 0, kMessageIdLength));
    put_i32(fwd, 0x18, static_cast<std::int32_t>(c.account_id));
    put_str(fwd, 0x1c, 32, c.name);
    put_i32(fwd, 0x3c, type);
    put_str(fwd, 0x40, 64, text);
    // 132on2g: the chat filter, per receiver (its own choice, ext::ChatFilter, else the server's chat_filter).
    std::vector<std::uint8_t> filtered;
    log("chat " + std::string(type ? "whisper" : "lobby") + " " + c.name +
        (type ? " -> " + std::to_string(target) : std::string()) + ": " + text);
    for (auto& [id, other] : conns_) {
        if (!other.account_id || id == c.id) continue;
        if (type != 0 ? static_cast<std::int32_t>(other.account_id) == target
                      : other.in_channel || other.world_id == c.world_id) {
            if (filter_for(other) && chat_filter_.changes(text)) {
                if (filtered.empty()) {
                    filtered = fwd;
                    put_str(filtered, 0x40, 64, chat_filter_.apply(text));
                }
                send(id, mid::ChatFwdMessage, filtered);
            } else {
                send(id, mid::ChatFwdMessage, fwd);
            }
        }
    }
}

// 00451050's TextFilter: the text comes back (132on2g: through the chat filter when the sender has it on, the
// original's server-side TextFilter) with status +58 0, the MessageID (the target's id as text) echoed: 00447bb0 adds
// it to the sender's own room chat.
void OmegaServer::on_text_filter(Conn& c, std::span<const std::uint8_t> req) {
    auto r = response(req, msize::TextFilterResponse);
    const std::string text = get_str(req, 0x2c, 64);
    put_str(r, 0x15, 64, filter_for(c) ? chat_filter_.apply(text) : text);
    put_i32(r, 0x58, c.account_id ? status::Success : status::WMError);
    send(c.id, mid::TextFilterResponse, r);
}

// 004516e0's PlayerList (the room's world +18; omega_server has no rooms): one response per logged-in player, as
// 00447a30 reads them: +18 status, +1c the account id, +20 its name [32], +40 its Medius player status (2 in the
// chat world, 3 in a game world: 00447a30 lists 2 as kind 2, the kind 002ac470 lets the Chat button pick, and 3 as
// kind 1) and +cc EndOfList on the last. No badge here: the platforms reach port clients only
// through ext::Roster.
void OmegaServer::on_player_list(Conn& c, std::span<const std::uint8_t> req) {
    std::vector<const Conn*> players;
    if (c.account_id)
        for (const auto& [id, other] : conns_)
            if (other.account_id) players.push_back(&other);
    if (players.empty()) {
        auto r = response(req, msize::PlayerListResponse);
        put_i32(r, kStatusOffset, status::NoResult);
        r[0xcc] = 1;
        send(c.id, mid::PlayerListResponse, r);
        return;
    }
    for (std::size_t i = 0; i < players.size(); ++i) {
        auto r = response(req, msize::PlayerListResponse);
        put_i32(r, kStatusOffset, status::Success);
        put_i32(r, 0x1c, static_cast<std::int32_t>(players[i]->account_id));
        put_str(r, 0x20, 32, players[i]->name);
        put_i32(r, 0x40, players[i]->world_id ? 3 : 2);
        r[0xcc] = i + 1 == players.size() ? 1 : 0;
        send(c.id, mid::PlayerListResponse, r);
    }
}

// ---- Checkpoint 132on2g: the contact list (SFOLobby_Contact.cc, ONLINE.md §3.7) ----------------------------------

void OmegaServer::save_social() {
    social_dirty_ = false;
    try {
        social_.save();
    } catch (const std::exception& e) {
        log(std::string("social: ") + e.what());
    }
}

void OmegaServer::flush_stores(bool now_anyway) {
    const std::uint64_t now = clock_ ? clock_() : 0;
    if (!now_anyway && now == stores_flushed_at_) return;
    if (accounts_dirty_) {
        accounts_dirty_ = false;
        stores_flushed_at_ = now;
        try {
            accounts_.save();
        } catch (const std::exception& e) {
            log(std::string("accounts: ") + e.what());
        }
    }
    if (social_dirty_) {
        stores_flushed_at_ = now;
        save_social();
    }
}

bool OmegaServer::filter_for(const Conn& c) const {
    if (c.kind == ClientKind::Original || !c.chat_filter) return config_.chat_filter;
    return *c.chat_filter;
}

void OmegaServer::server_message(const Conn& c, std::uint32_t code, const std::string& text) {
    if (c.has(cap::Notices)) {
        send_notice(c, code, text);
        return;
    }
    if (!c.account_id) return;
    std::vector<std::uint8_t> fwd(msize::ChatFwdMessage, 0);
    put_i32(fwd, 0x18, 0);
    put_str(fwd, 0x1c, 32, "omega_server");
    put_i32(fwd, 0x3c, 1);  // private: outside the room, 00447c70 makes it the lobby message of type 9 (NETMESS)
    put_str(fwd, 0x40, 64, text.substr(0, 63));
    send(c.id, mid::ChatFwdMessage, fwd);
}

// 0044d860's GetBuddyList_ExtraInfo (0x15: MessageID, SessionKey +15; the requester is the session). One 0xd0 response
// per contact, as 00446830 reads it: +18 status, +1c AccountID, +20 AccountName [32], +40 the player status (0 offline,
// 2 in the lobby, 3 in a game: the PlayerList's values), +44 LobbyWorldID, +48 GameWorldID, +4c LobbyName [64],
// +8c GameName [64], +cc EndOfList. No contacts: NoResult with EndOfList.
void OmegaServer::on_buddy_list(Conn& c, std::span<const std::uint8_t> req) {
    std::vector<const Account*> list;
    if (c.account_id)
        for (const std::uint32_t id : social_.contacts(c.account_id))
            if (const Account* a = accounts_.find(id)) list.push_back(a);
    if (list.empty()) {
        auto r = response(req, msize::GetBuddyListExtraInfoResponse);
        put_i32(r, kStatusOffset, c.account_id ? status::NoResult : status::WMError);
        r[0xcc] = 1;
        send(c.id, mid::GetBuddyListExtraInfoResponse, r);
        return;
    }
    for (std::size_t i = 0; i < list.size(); ++i) {
        auto r = response(req, msize::GetBuddyListExtraInfoResponse);
        put_i32(r, kStatusOffset, status::Success);
        put_i32(r, 0x1c, static_cast<std::int32_t>(list[i]->id));
        put_str(r, 0x20, 32, list[i]->name);
        if (const Conn* o = conn_of_account(list[i]->id)) {
            put_i32(r, 0x40, o->world_id ? 3 : 2);
            put_i32(r, 0x44, 1);  // omega_server's one lobby channel
            put_str(r, 0x4c, 64, config_.server_name);
            if (o->world_id) {
                put_i32(r, 0x48, static_cast<std::int32_t>(o->world_id));
                if (const auto g = games_.find(o->world_id); g != games_.end()) put_str(r, 0x8c, 64, g->second.game_name);
            }
        }
        r[0xcc] = i + 1 == list.size() ? 1 : 0;
        send(c.id, mid::GetBuddyListExtraInfoResponse, r);
    }
}

// 0044d780's GetBuddyInvitations (0x15): one 0x48 response per pending invitation not yet delivered to this login, as
// 00446740 reads it (each becomes a lobby message of type 1 "from" the inviter): +18 status, +1c the inviter's
// AccountID, +20 its AccountName [32], +40 AddType, +44 EndOfList. None: NoResult with EndOfList.
void OmegaServer::on_buddy_invitations(Conn& c, std::span<const std::uint8_t> req) {
    std::vector<std::pair<std::uint32_t, const Account*>> list;
    if (c.account_id)
        for (const auto& inv : social_.invitations(c.account_id)) {
            const auto& done = c.invitations_delivered;
            if (std::find(done.begin(), done.end(), inv.from) != done.end()) continue;
            if (const Account* a = accounts_.find(inv.from)) list.emplace_back(inv.from, a);
        }
    if (list.empty()) {
        auto r = response(req, msize::GetBuddyInvitationsResponse);
        put_i32(r, kStatusOffset, c.account_id ? status::NoResult : status::WMError);
        r[0x44] = 1;
        send(c.id, mid::GetBuddyInvitationsResponse, r);
        return;
    }
    for (std::size_t i = 0; i < list.size(); ++i) {
        auto r = response(req, msize::GetBuddyInvitationsResponse);
        put_i32(r, kStatusOffset, status::Success);
        put_i32(r, 0x1c, static_cast<std::int32_t>(list[i].first));
        put_str(r, 0x20, 32, list[i].second->name);
        put_i32(r, 0x40, 1);
        r[0x44] = i + 1 == list.size() ? 1 : 0;
        send(c.id, mid::GetBuddyInvitationsResponse, r);
        c.invitations_delivered.push_back(list[i].first);
    }
    log("contacts: " + c.name + " got " + std::to_string(list.size()) + " pending invitation(s)");
}

void OmegaServer::forward_invitation(Conn& to, std::uint32_t from) {
    const Account* a = accounts_.find(from);
    if (!a) return;
    // 259, unsolicited (0x40, 00446640): +18 the inviter's AccountID, +1c its AccountName [32], +3c AddType.
    std::vector<std::uint8_t> m(msize::AddToBuddyListFwdConfirmation, 0);
    put_i32(m, 0x18, static_cast<std::int32_t>(from));
    put_str(m, 0x1c, 32, a->name);
    put_i32(m, 0x3c, 1);
    send(to.id, mid::AddToBuddyListFwdConfirmation, m);
    to.invitations_delivered.push_back(from);
}

// 0044db60's AddToBuddyListConfirmation (0x30: SessionKey +15, the invitee's AccountID +28, AddType +2c = 1). The
// invitation is kept until the invitee answers (260), and forwarded at once (259) when it is online, else listed by
// its next GetBuddyInvitations. The 0x40 response 68 (00446b60 only notes it) has the status +18: Success, or
// AccountNotFound for an unknown or own account. Already contacts: Success, nothing sent.
void OmegaServer::on_buddy_add(Conn& c, std::span<const std::uint8_t> req) {
    const auto target = static_cast<std::uint32_t>(get_i32(req, 0x28));
    auto r = response(req, msize::AddToBuddyListConfirmationResponse);
    const Account* a = c.account_id ? accounts_.find(target) : nullptr;
    std::int32_t code = status::Success;
    if (!c.account_id) code = status::WMError;
    else if (!a || target == c.account_id) code = status::AccountNotFound;
    else if (social_.is_contact(c.account_id, target) && social_.is_contact(target, c.account_id)) code = status::Success;
    else if (!social_.invite(c.account_id, target, clock_ ? clock_() : 0)) {
        if (social_.invitations(target).size() >= SocialStore::kMaxInvitations) code = status::RequestDenied;
    } else {
        save_social();
        log("contacts: " + c.name + " invited " + a->name);
        if (Conn* o = conn_of_account(target)) forward_invitation(*o, c.account_id);
    }
    if (code != status::Success && c.account_id)
        log("contacts: " + c.name + "'s invitation of account " + std::to_string(target) + " refused, status " +
            std::to_string(code));
    put_i32(r, kStatusOffset, code);
    send(c.id, mid::AddToBuddyListConfirmationResponse, r);
}

// 00446560's AddToBuddyListFwdConfirmationResponse (0x38, no reply; NETMESS's answer of a type 1 message): +18 the
// answer (2 accepted, RequestDenied -985 declined), +1c SessionKey, +30 the inviter's AccountID, +34 AddType. An
// acceptance makes both agents contacts of each other.
void OmegaServer::on_buddy_answer(Conn& c, std::span<const std::uint8_t> req) {
    if (!c.account_id) return;
    const std::int32_t answer = get_i32(req, 0x18);
    const auto from = static_cast<std::uint32_t>(get_i32(req, 0x30));
    if (!social_.take_invitation(from, c.account_id)) {
        log("contacts: " + c.name + " answered a missing invitation from account " + std::to_string(from));
        return;
    }
    const Account* a = accounts_.find(from);
    const std::string from_name = a ? a->name : "#" + std::to_string(from);
    const bool accepted = answer >= 0;
    if (accepted && !social_.add_contacts(from, c.account_id)) {
        log("contacts: " + c.name + " accepted " + from_name + ", but a contact list is full");
    } else {
        log("contacts: " + c.name + (accepted ? " accepted " : " declined ") + from_name + "'s invitation");
    }
    save_social();
    // PORT-ONLY: the inviter hears the outcome as a notice (an original inviter sees it in its next contact list).
    if (Conn* o = conn_of_account(from); o && o->has(cap::Notices))
        send_notice(*o, notice::Info, c.name + (accepted ? " accepted" : " declined") + " your contact invitation");
}

// 0044d9c0's RemoveFromBuddyList (0x2c: SessionKey +15, the AccountID +28). The 0x1c response 66 has the status +18
// (004466f0: done, removed when 0): Success, or NoResult when it was not a contact. One-sided.
void OmegaServer::on_buddy_remove(Conn& c, std::span<const std::uint8_t> req) {
    const auto other = static_cast<std::uint32_t>(get_i32(req, 0x28));
    auto r = response(req, msize::RemoveFromBuddyListResponse);
    std::int32_t code = status::WMError;
    if (c.account_id) {
        code = social_.remove_contact(c.account_id, other) ? status::Success : status::NoResult;
        if (code == status::Success) {
            save_social();
            log("contacts: " + c.name + " removed account " + std::to_string(other));
        }
    }
    put_i32(r, kStatusOffset, code);
    send(c.id, mid::RemoveFromBuddyListResponse, r);
}

// 00451370's AccountGetID (0x46: SessionKey +15, AccountName [32] +26), CONTACTS' invitation by name. The 0x20
// response 24 (00446e00 copies it to lobby +a68): +18 the AccountID, +1c the status (00451370: not 0 -> error 0x1d
// "no such agent", 0033c7c0's popup 0x17). Original wire, for every client kind; it needs a login.
void OmegaServer::on_account_get_id(Conn& c, std::span<const std::uint8_t> req) {
    const std::string name = get_str(req, 0x26, 32);
    auto r = response(req, msize::AccountGetIDResponse);
    std::int32_t code = status::WMError;
    if (c.account_id) {
        const Account* a = name.empty() ? nullptr : accounts_.find(name);
        code = a ? status::Success : status::AccountNotFound;
        if (a) put_i32(r, 0x18, static_cast<std::int32_t>(a->id));
    }
    put_i32(r, 0x1c, code);
    send(c.id, mid::AccountGetIDResponse, r);
}

// PORT-ONLY (132on2g, cap::Social): ext::RecentPlayersList: u16 count, then per player u32 account, str name,
// u8 platform (its session's, when online; else Unknown), u8 online, u32 seconds since the game.
void OmegaServer::send_recent_players(Conn& c) {
    const std::uint64_t now = clock_ ? clock_() : 0;
    std::vector<std::pair<const SocialStore::Recent*, const Account*>> list;
    for (const auto& r : social_.recent(c.account_id))
        if (const Account* a = accounts_.find(r.account)) list.emplace_back(&r, a);
    ExtWriter w;
    w.u16(static_cast<std::uint16_t>(list.size()));
    for (const auto& [r, a] : list) {
        const Conn* o = conn_of_account(r->account);
        w.u32(r->account).str(a->name).u8(static_cast<std::uint8_t>(o ? o->platform() : Platform::Unknown));
        w.u8(o ? 1 : 0).u32(static_cast<std::uint32_t>(std::min<std::uint64_t>(now > r->when ? now - r->when : 0, 0xffffffffu)));
    }
    send_ext(c, cap::Social, ext::RecentPlayersList, w.bytes());
}

void OmegaServer::on_ext(Conn& c, const Frame& frame) {
    const std::span<const std::uint8_t> b(frame.body);
    switch (frame.type) {
    case ext::ModReport: {  // PORT-ONLY (132md4): kept per session, replacing an earlier one; a bad body keeps it
        if (!c.has(cap::Mods)) return;
        if (auto r = decode_mod_report(b)) {
            log("conn " + std::to_string(c.id) + " " + describe_mod_report(*r));
            c.mod_report = std::move(*r);
        } else {
            log("conn " + std::to_string(c.id) + ": bad mod report, ignored");
        }
        return;
    }
    case ext::Hello: {
        const auto h = decode_hello(b);
        if (!h || h->protocol != kProtocolVersion) {
            log("conn " + std::to_string(c.id) + ": bad hello, closing");
            if (close_) close_(c.id);
            return;
        }
        c.helloed = true;
        c.hello = *h;
        c.hello.caps &= cap::All;  // the negotiated set: what both sides know
        c.cheats_sent = c.has(cap::CheatPolicy) ? (cheats_allowed_for(c.id) ? 1 : 0) : -1;
        send(c.id, ext::Welcome,
             ExtWriter().u16(kProtocolVersion).u32(c.hello.caps).u8(c.cheats_sent == 1 ? 1 : 0).str(config_.server_name).bytes());
        log("conn " + std::to_string(c.id) + " hello: platform " + std::string(platform_name(h->platform)) + ", build " +
            std::to_string(h->build) + ", caps " + std::to_string(c.hello.caps));
        return;
    }
    case ext::AdminCommand: {
        if (!c.has(cap::Admin)) return;
        ExtReader r(b);
        std::string line = r.str();
        bool allowed = c.admin;
        const auto w = words(line);
        const bool token_try = !config_.admin_token.empty() && w.size() >= 2 && w[0] == "token";
        const std::uint64_t now = clock_ ? clock_() : 0;
        // 132on2d: admin tokens go through the failed-login throttle (the name "<admin token>", per address).
        const bool token_locked = token_try && login_locked("<admin token>", c.peer_ip, now) > 0;
        if (token_try && !token_locked && equal_digest(sha256(w[1]), sha256(config_.admin_token))) {
            allowed = true;
            line = rest_after(line, 2);
        } else if (token_try && !token_locked) {
            login_failed("<admin token>", c.peer_ip, now);
        }
        const std::string who = c.account_id ? c.name : "conn " + std::to_string(c.id);
        std::string out;
        if (!allowed) {
            out = "denied: not an admin";
            // 132on2d: a refused token is not logged (it may be a near miss of the real one).
            log("admin denied for " + who + ": " +
                (w.size() >= 2 && w[0] == "token" ? "token <hidden> " + rest_after(line, 2) : line));
        } else {
            out = admin_command(line, who);
        }
        send(c.id, ext::AdminReply, ExtWriter().str(out).bytes());
        return;
    }
    case ext::HostEndpoint: {
        if (!c.has(cap::HostEndpoint)) return;
        ExtReader r(b);
        const std::uint32_t world = r.u32();
        const std::uint16_t port = r.u16();
        auto it = games_.find(world);
        if (!r.ok() || it == games_.end() || it->second.host_conn != c.id || port == 0) return;
        // 132on2e: optional trailing [u16 external port][str external ip], the host router's UPnP forward. Joiners get
        // the external port when this server sees the host at a public address (else the host shares this server's
        // network and the game port itself is reachable).
        std::uint16_t external_port = 0;
        std::string external_ip;
        if (b.size() > 6) {
            external_port = r.u16();
            external_ip = r.str();
            if (!r.ok()) external_port = 0;
        }
        const std::uint32_t host_ip = it->second.host_ip;
        const bool use_external = external_port != 0 && !is_private_ipv4(host_ip);
        it->second.host_port = use_external ? external_port : port;
        std::string line = "game " + std::to_string(world) + " host endpoint " + ipv4_text(host_ip) + ":" +
                           std::to_string(it->second.host_port);
        if (external_port != 0) {
            line += use_external ? " (the router's port forward of local port " + std::to_string(port) +
                                       ": UPnP / PCP / NAT-PMP or manual)"
                                 : " (a private address: its local port, not the router's forward " +
                                       std::to_string(external_port) + ")";
            if (!external_ip.empty() && external_ip != ipv4_text(host_ip) && !is_private_ipv4(host_ip))
                line += "; warning: its router reports the external address " + external_ip +
                        " (a second NAT, e.g. carrier-grade: joiners may need the relay)";
        }
        log(line);
        return;
    }
    case ext::LeaveGame:
        if (!c.has(cap::HostEndpoint)) return;
        leave_world(c);
        broadcast_roster();
        return;
    case ext::GamePassword: {  // PORT-ONLY (132on2c): the host's join password for its own world
        if (!c.has(cap::GamePassword)) return;
        ExtReader r(b);
        const std::uint32_t world = r.u32();
        std::string password = r.str();
        auto it = games_.find(world);
        if (r.ok() && it != games_.end() && it->second.host_conn == c.id) {
            if (password.size() > 31) password.resize(31);
            it->second.password = password;
            log("game " + std::to_string(world) + (password.empty() ? " has no password" : " is password-protected"));
        }
        return;
    }
    case ext::RecentPlayers:  // PORT-ONLY (132on2g): the recent players list
        if (!c.has(cap::Social) || !c.account_id) return;
        send_recent_players(c);
        return;
    case ext::ChatFilter: {  // PORT-ONLY (132on2g): this player's chat filter choice
        if (!c.has(cap::Social)) return;
        ExtReader r(b);
        const std::uint8_t on = r.u8();
        if (!r.ok()) return;
        c.chat_filter = on != 0;
        log("conn " + std::to_string(c.id) + (c.account_id ? " (" + c.name + ")" : std::string()) + ": chat filter " +
            (on ? "on" : "off") + " (its own choice)");
        return;
    }
    default:
        log("conn " + std::to_string(c.id) + ": unknown extension 0x" + to_hex(std::vector<std::uint8_t>{
                                                                         static_cast<std::uint8_t>(frame.type >> 8),
                                                                         static_cast<std::uint8_t>(frame.type)}));
        return;
    }
}

void OmegaServer::log_out(Conn& c, const char* why) {
    if (!c.account_id) return;
    leave_world(c);
    if (relay_) relay_->close(c.id);
    log("logout: " + c.name + " (" + why + ")");
    c.account_id = 0;
    c.name.clear();
    c.admin = false;
    c.in_channel = false;
    c.invitations_delivered.clear();
    broadcast_roster();
}

void OmegaServer::leave_world(Conn& c) {
    if (!c.world_id) return;
    const std::uint32_t world = c.world_id;
    c.world_id = 0;
    auto it = games_.find(world);
    if (it == games_.end()) return;
    if (it->second.host_conn == c.id) {
        close_world(world, "host left");
        return;
    }
    auto& p = it->second.players;
    p.erase(std::remove(p.begin(), p.end(), c.account_id), p.end());
}

void OmegaServer::close_world(std::uint32_t world_id, const char* why) {
    auto it = games_.find(world_id);
    if (it == games_.end()) return;
    for (auto& [id, other] : conns_)
        if (other.world_id == world_id) {
            other.world_id = 0;
            send_notice(other, notice::GameClosed, std::string("The game was closed: ") + why);
        }
    log("game " + std::to_string(world_id) + " closed (" + why + ")");
    const ServerGame& g = it->second;
    if (g.reported && !g.results.empty()) {
        // Checkpoint 132on2d (PORT-ONLY store): the played game's result, each player's record and its change.
        GameResult result;
        result.ended = clock_ ? clock_() : 0;
        result.world = world_id;
        result.seconds = result.ended >= g.created ? result.ended - g.created : 0;
        result.host = g.host_account;
        result.game_name = g.game_name;
        result.why = why;
        for (const auto& [account, p] : g.results) result.players.push_back(p);
        log("result: game " + std::to_string(world_id) + " '" + g.game_name + "', " + std::to_string(result.seconds) +
            " s, " + std::to_string(result.players.size()) + " player(s)");
        for (const auto& p : result.players)
            log("result:   " + p.name + (p.account == g.host_account ? " (host)" : "") + ": " +
                describe_stats_change(p.entry, p.final) + " -> " + describe_stats(p.final));
        stats_.add_result(std::move(result));
        stats_dirty_ = true;
        flush_stats();
    }
    games_.erase(it);
}

void OmegaServer::broadcast_roster() {
    // 132on2h (the load test's hot spot): coalesced; tick() sends one roster per loop iteration however many logins,
    // joins and logouts it covers (each was a full roster to every session: n^2 frames of n entries).
    roster_dirty_ = true;
    push_cheat_policy();
}

void OmegaServer::flush_roster() {
    if (!roster_dirty_) return;
    roster_dirty_ = false;
    std::vector<RosterEntry> roster;
    for (const auto& [id, c] : conns_)
        if (c.account_id) roster.push_back({c.account_id, c.platform(), c.world_id, c.name});
    const auto body = encode_roster(roster);
    for (const auto& [id, c] : conns_)
        if (c.account_id) send_ext(c, cap::Roster, ext::Roster, body);
}

OmegaServer::Conn* OmegaServer::conn_of_account(std::uint32_t account_id) {
    for (auto& [id, c] : conns_)
        if (c.account_id == account_id) return &c;
    return nullptr;
}

OmegaServer::Conn* OmegaServer::conn_of_name(std::string_view name) {
    const Account* a = accounts_.find(name);
    return a ? conn_of_account(a->id) : nullptr;
}

void OmegaServer::kick_conn(Conn& c, std::string_view reason) {
    send_notice(c, notice::Kicked, std::string("Disconnected by the server: ") + std::string(reason));
    log_out(c, "kicked");
    if (close_) close_(c.id);
}

std::vector<ServerPlayer> OmegaServer::players() const {
    std::vector<ServerPlayer> out;
    for (const auto& [id, c] : conns_)
        if (c.account_id) out.push_back({id, c.account_id, c.name, c.platform(), c.world_id, c.admin, c.kind});
    return out;
}

// ---- Checkpoint 132on2g: the graceful shutdown / restart (ONLINE_DEPLOY.md "Stopping and restarting") ------------

void OmegaServer::request_stop(Lifecycle kind, std::uint64_t seconds, std::string reason) {
    const std::uint64_t now = clock_ ? clock_() : 0;
    if (kind == Lifecycle::None) {
        if (stop_.kind == Lifecycle::None) return;
        log("lifecycle: the " + std::string(stop_.kind == Lifecycle::Restart ? "restart" : "shutdown") + " was cancelled");
        for (const auto& [id, c] : conns_)
            if (c.account_id) server_message(c, notice::Shutdown, "The server " + std::string(stop_.kind == Lifecycle::Restart ? "restart" : "shutdown") + " was cancelled.");
        stop_ = Stop{};
        return;
    }
    stop_.kind = kind;
    stop_.due = now + seconds;
    stop_.reason = std::move(reason);
    stop_.last_warned = ~0ull;
    log("lifecycle: " + std::string(kind == Lifecycle::Restart ? "restart" : "shutdown") + " in " +
        std::to_string(seconds) + " s" + (stop_.reason.empty() ? std::string() : " (" + stop_.reason + ")"));
    tick();
}

void OmegaServer::tick() {
    maintain(clock_ ? clock_() : 0);
    flush_roster();
    flush_stores(false);
    if (stop_.kind == Lifecycle::None) return;
    const std::uint64_t now = clock_ ? clock_() : 0;
    const std::uint64_t left = stop_.due > now ? stop_.due - now : 0;
    if (left == 0) return;
    // The first warning at once, then at each mark passed (60, 30, 10, 5 seconds left).
    std::uint64_t mark = left > 60 ? 1000000 : left;
    for (const std::uint64_t m : {60ull, 30ull, 10ull, 5ull})
        if (left <= m) mark = m;
    if (stop_.last_warned != ~0ull && mark >= stop_.last_warned) return;
    stop_.last_warned = mark;
    const std::string what = stop_.kind == Lifecycle::Restart ? "restarts" : "shuts down";
    const std::string text = "The server " + what + " in " + std::to_string(left) + " s" +
                             (stop_.reason.empty() ? std::string(".") : ": " + stop_.reason);
    for (const auto& [id, c] : conns_)
        if (c.account_id) server_message(c, notice::Shutdown, text);
    log("lifecycle: warned " + std::to_string(players().size()) + " player(s): " + text);
}

OmegaServer::Lifecycle OmegaServer::lifecycle_due() const {
    if (stop_.kind == Lifecycle::None) return Lifecycle::None;
    const std::uint64_t now = clock_ ? clock_() : 0;
    return now >= stop_.due ? stop_.kind : Lifecycle::None;
}

void OmegaServer::shutdown_sessions(const std::string& reason) {
    std::vector<std::uint32_t> worlds;
    for (const auto& [id, g] : games_) worlds.push_back(id);
    for (const std::uint32_t w : worlds) close_world(w, "server shutdown");  // each played game's result is kept
    for (auto& [id, c] : conns_) {
        if (c.account_id) server_message(c, notice::Shutdown, reason);
        log_out(c, "server shutdown");
        c.closing = true;
        if (close_) close_(id);
    }
    flush_stats();
    save_social();
    flush_stores(true);
    log("lifecycle: " + std::to_string(conns_.size()) + " connection(s) closed (" + reason + ")");
    stop_ = Stop{};
}

std::string OmegaServer::stop_command(const std::vector<std::string>& w, std::string_view line, bool restart) {
    const Lifecycle kind = restart ? Lifecycle::Restart : Lifecycle::Shutdown;
    if (w.size() >= 2 && w[1] == "cancel") {
        if (stop_.kind == Lifecycle::None) return "nothing to cancel";
        request_stop(Lifecycle::None, 0, {});
        return "cancelled";
    }
    std::uint64_t seconds = 60;
    std::size_t reason_from = 1;
    if (w.size() >= 2) {
        if (w[1] == "now") {
            seconds = 0, reason_from = 2;
        } else if (const auto d = parse_ban_duration(w[1]); d && *d != kBanForever) {
            seconds = *d, reason_from = 2;
        } else if (w[1] == "0") {
            seconds = 0, reason_from = 2;
        }
    }
    request_stop(kind, seconds, rest_after(line, reason_from));
    return std::string(restart ? "restart" : "shutdown") + " in " + std::to_string(seconds) + " s" +
           (restart ? " (in-process: the stores, the config file's keys and the policy are read again; bind / port / "
                      "relay_port changes need a process restart)"
                    : "");
}

// The console's `contacts <name>` / `recent <name>` / `filter [on|off|test <text>]`.
std::string OmegaServer::social_command(const std::vector<std::string>& w) {
    std::ostringstream out;
    if (w[0] == "filter") {
        if (w.size() >= 2 && w[1] == "test") {
            std::string text;
            for (std::size_t i = 2; i < w.size(); ++i) text += (i > 2 ? " " : "") + w[i];
            return chat_filter_.apply(text);
        }
        if (w.size() >= 2) {
            bool on = false;
            if (!flag_word(w[1], on)) return "filter: on | off | test <text>";
            config_.chat_filter = on;
            log("admin: chat_filter=" + std::string(on ? "1" : "0"));
        }
        std::size_t own = 0;
        for (const auto& [id, c] : conns_)
            if (c.chat_filter) ++own;
        out << "chat_filter=" << (config_.chat_filter ? 1 : 0) << " (" << chat_filter_.size() << " words; " << own
            << " session(s) chose their own)";
        return out.str();
    }
    if (w.size() < 2) return w[0] + " <name>";
    const Account* a = accounts_.find(w[1]);
    if (!a) return w[0] + ": no account '" + w[1] + "'";
    auto name_of = [&](std::uint32_t id) {
        const Account* o = accounts_.find(id);
        return o ? o->name : "#" + std::to_string(id);
    };
    if (w[0] == "contacts") {
        out << a->name << ": " << social_.contacts(a->id).size() << " contact(s)";
        for (const auto id : social_.contacts(a->id)) out << "\n  " << name_of(id) << (conn_of_account(id) ? " (online)" : "");
        out << "\n  pending invitations to " << a->name << ": " << social_.invitations(a->id).size();
        for (const auto& i : social_.invitations(a->id)) out << "\n    from " << name_of(i.from) << " at " << i.when;
    } else {
        out << a->name << ": " << social_.recent(a->id).size() << " recent player(s)";
        for (const auto& r : social_.recent(a->id))
            out << "\n  " << name_of(r.account) << " (world " << r.world << ", at " << r.when << ")";
    }
    return out.str();
}

// PORT-ONLY (132on2f): the console's `status` (ONLINE_DEPLOY.md §6): one health summary for operators and monitors.
std::string OmegaServer::limits_text(std::uint64_t now) const {
    std::ostringstream out;
    out << "max_conns_per_ip=" << config_.max_conns_per_ip << " message_rate=" << config_.message_rate
        << " message_burst=" << config_.message_burst << " login_failures=" << config_.login_failures
        << " login_window=" << config_.login_window << " login_lockout=" << config_.login_lockout
        << " registrations_per_hour=" << config_.registrations_per_hour
        << " login_attempts_per_ip=" << config_.login_attempts_per_ip
        << " login_attempts_per_account=" << config_.login_attempts_per_account
        << " chat_per_minute=" << config_.chat_per_minute << " chat_burst=" << config_.chat_burst
        << " chat_per_minute_ip=" << config_.chat_per_minute_ip << " chat_flood_kick=" << config_.chat_flood_kick
        << " flood_block_after=" << config_.flood_block_after << " flood_window=" << config_.flood_window
        << " flood_block=" << config_.flood_block;
    for (const auto& [key, f] : login_failures_)
        if (f.locked_until > now)
            out << "\n  locked: " << key << " for " << (f.locked_until - now) << " s (" << f.count << " failures)";
        else
            out << "\n  failures: " << key << " " << f.count;
    for (const auto& [ip, f] : floods_) {
        out << "\n  " << (f.blocked_until > now ? "blocked: " : "floods: ") << ipv4_text(ip);
        if (f.blocked_until > now) out << " for " << (f.blocked_until - now) << " s";
        out << " (" << f.times.size() << " flood disconnect(s))";
    }
    const AbuseCounters& a = abuse_;
    out << "\n  since the start: " << a.login_refused << " login attempt(s) refused, " << a.chat_dropped
        << " chat message(s) dropped, " << a.chat_kicks << " chat flood kick(s), " << a.flood_disconnects
        << " flood disconnect(s), " << a.blocks << " address block(s), " << a.blocked_connects
        << " connection(s) from blocked addresses";
    return out.str();
}

void OmegaServer::note_tick(std::uint64_t busy_us, std::size_t frames) {
    if (tick_busy_.size() < kTickSamples) tick_busy_.resize(kTickSamples, 0);
    tick_busy_[tick_next_] = static_cast<std::uint32_t>(std::min<std::uint64_t>(busy_us, 0xFFFFFFFFu));
    tick_next_ = (tick_next_ + 1) % kTickSamples;
    ++ticks_;
    tick_frames_ += frames;
}

OmegaServer::TickSummary OmegaServer::tick_summary() const {
    TickSummary s;
    s.ticks = ticks_;
    s.frames = tick_frames_;
    s.samples = static_cast<std::size_t>(std::min<std::uint64_t>(ticks_, kTickSamples));
    if (!s.samples) return s;
    std::vector<std::uint32_t> v(tick_busy_.begin(), tick_busy_.begin() + static_cast<std::ptrdiff_t>(s.samples));
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())))] / 1000.0; };
    s.p50_ms = at(0.50);
    s.p99_ms = at(0.99);
    s.max_ms = v.back() / 1000.0;
    return s;
}

std::string OmegaServer::status_text(std::uint64_t now) const {
    std::ostringstream out;
    const std::uint64_t up = now > started_at_ ? now - started_at_ : 0;
    char clock_text[32];
    std::snprintf(clock_text, sizeof clock_text, "%llud %02llu:%02llu:%02llu", static_cast<unsigned long long>(up / 86400),
                  static_cast<unsigned long long>(up / 3600 % 24), static_cast<unsigned long long>(up / 60 % 60),
                  static_cast<unsigned long long>(up % 60));
    std::size_t port_conns = 0, original_conns = 0, logged_in = 0, in_game = 0, admins = 0;
    for (const auto& [id, c] : conns_) {
        (c.kind == ClientKind::Original ? original_conns : port_conns)++;
        if (c.account_id) {
            ++logged_in;
            if (c.world_id) ++in_game;
            if (c.admin) ++admins;
        }
    }
    std::size_t game_players = 0;
    for (const auto& [id, g] : games_) game_players += g.players.size();
    std::size_t banned = 0;
    for (const auto& a : accounts_.accounts())
        if (a.banned_at(now)) ++banned;
    std::size_t locks = 0;
    for (const auto& [key, f] : login_failures_)
        if (f.locked_until > now) ++locks;
    out << "status ok: " << config_.server_name << ", up " << clock_text << " (" << up << " s, since unix " << started_at_
        << ")\n  connections " << conns_.size() << " (port " << port_conns << ", original " << original_conns
        << "), players logged in " << logged_in << " (in a game " << in_game << ", admins " << admins << ")\n  games "
        << games_.size() << " (" << game_players << " players in them, max_players " << config_.max_players
        << ")\n  accounts " << accounts_.accounts().size() << " (banned " << banned << "), failed-login locks " << locks
        << "\n  statistics: " << stats_.records().size() << " records, " << stats_.results().size() << " results"
        << (stats_dirty_ ? " (unsaved changes)" : "") << "\n  ";
    if (relay_) {
        const auto& s = relay_->stats();
        out << "relay: UDP port " << relay_port_ << ", endpoints " << relay_->endpoint_count() << "/"
            << relay_->limits().max_endpoints << ", datagrams received " << s.received << ", forwarded " << s.forwarded
            << " (" << s.forwarded_bytes << " bytes), dropped "
            << (s.dropped_bad + s.dropped_token + s.dropped_world + s.dropped_unregistered + s.dropped_rate);
    } else {
        out << "relay: off (relay_port=0)";
    }
    std::size_t contacts = 0, invitations = 0;
    for (const auto& a : accounts_.accounts()) {
        contacts += social_.contacts(a.id).size();
        invitations += social_.invitations(a.id).size();
    }
    out << "\n  social: " << contacts << " contact entries, " << invitations << " pending invitation(s); chat_filter="
        << (config_.chat_filter ? 1 : 0);
    std::size_t blocked = 0;
    for (const auto& [ip, f] : floods_)
        if (f.blocked_until > now) ++blocked;
    out << "\n  abuse: " << blocked << " blocked address(es); since the start " << abuse_.login_refused
        << " login attempt(s) refused, " << abuse_.chat_dropped << " chat message(s) dropped, "
        << abuse_.flood_disconnects << " flood disconnect(s)";
    if (const TickSummary t = tick_summary(); t.ticks) {
        char text[256];
        std::snprintf(text, sizeof text, "\n  tick: %llu loop iterations, %llu frames; work per iteration (last %zu) p50 %.3f ms, p99 %.3f ms, max %.3f ms",
                      static_cast<unsigned long long>(t.ticks), static_cast<unsigned long long>(t.frames), t.samples,
                      t.p50_ms, t.p99_ms, t.max_ms);
        out << text;
    }
    if (process_info_) out << "\n  process: " << process_info_();
    out << "\n  password work (PBKDF2): " << hash_jobs_ << " job(s) since the start, " << hash_pending_ << " pending"
        << (hash_runner_ ? " (worker threads)" : " (inline)");
    if (stop_.kind != Lifecycle::None)
        out << "\n  " << (stop_.kind == Lifecycle::Restart ? "restart" : "shutdown") << " pending in "
            << (stop_.due > now ? stop_.due - now : 0) << " s" << (stop_.reason.empty() ? "" : ": " + stop_.reason);
    return out.str();
}

std::string OmegaServer::admin_command(std::string_view line, std::string_view who) {
    const auto w = words(line);
    if (w.empty()) return "";
    const std::string& cmd = w[0];
    std::ostringstream out;
    auto audit = [&](const std::string& what) { log("admin " + std::string(who) + ": " + what); };
    const std::uint64_t now = clock_ ? clock_() : 0;
    if (cmd == "help") {
        out << "commands: players | games | accounts | ban <name> <30m|2h|7d|seconds|perm> [reason] | unban <name>\n"
               "  kick <name> [reason] | close <world id> | broadcast <text> | cheats [on|off]\n"
               "  admin <name> on|off | password <name> <new password> | create <name> <password>\n"
               "  policy | policy usage|privacy <text> | policy privacy clear   (\\n = new line; bumps the version)\n"
               "  announce list | announce add <text> | announce del <n> | announce clear | motd <text>\n"
               "  mods   (the mod policy and each connection's mod report)\n"
               "  stats [name]   (each account's agent statistics record, or one account's)\n"
               "  results [n]    (the last n finished games, default 10)\n"
               "  limits [clear] (the rate limits, anti-abuse counters, failed-login locks and flood blocks; clear unlocks)\n"
               "  relay          (the in-level UDP relay: its limits, counters and endpoints)\n"
               "  status         (health: uptime, connections, players, games, accounts, relay endpoints / bytes)\n"
               "  shutdown [seconds|30s|5m|now|cancel] [reason] | restart [seconds|now|cancel] [reason]\n"
               "                 (warns the players, closes the games keeping their results, saves; default 60 s)\n"
               "  contacts <name> | recent <name>   (an account's contact list / invitations, its recent players)\n"
               "  filter [on|off] | filter test <text>   (the chat filter's default; a port player may choose its own)";
    } else if (cmd == "shutdown" || cmd == "restart") {
        out << stop_command(w, line, cmd == "restart");
        audit(std::string(line));
    } else if (cmd == "contacts" || cmd == "recent" || cmd == "filter") {
        out << social_command(w);
    } else if (cmd == "status") {
        out << status_text(now);
    } else if (cmd == "players") {
        out << "players online: " << players().size();
        for (const auto& p : players())
            out << "\n  " << p.name << " (id " << p.account_id << ", " << platform_name(p.platform) << ", "
                << (p.world_id ? "world " + std::to_string(p.world_id) : std::string("lobby")) << (p.admin ? ", admin" : "")
                << ")";
    } else if (cmd == "games") {
        out << "games: " << games_.size();
        for (const auto& [id, g] : games_) {
            out << "\n  world " << id << " '" << g.game_name << "' " << g.players.size() << "/" << g.max_players
                << " host " << ipv4_text(g.host_ip) << ":" << g.host_port << " players:";
            for (std::uint32_t a : g.players)
                if (const Account* acc = accounts_.find(a)) out << ' ' << acc->name;
        }
    } else if (cmd == "accounts") {
        out << "accounts: " << accounts_.accounts().size();
        for (const auto& a : accounts_.accounts()) {
            out << "\n  " << a.id << ' ' << a.name << (a.admin ? " admin" : "") << " policy v" << a.accepted_policy;
            if (a.banned_at(now))
                out << " BANNED " << (a.ban_until == kBanForever ? std::string("permanently") : "until " + std::to_string(a.ban_until))
                    << (a.ban_reason.empty() ? "" : ": " + a.ban_reason);
        }
    } else if (cmd == "ban" && w.size() >= 3) {
        const auto duration = parse_ban_duration(w[2]);
        if (!duration) return "ban: bad duration '" + w[2] + "'";
        const std::uint64_t until = *duration == kBanForever ? kBanForever : now + *duration;
        const std::string reason = rest_after(line, 3);
        if (!accounts_.ban(w[1], until, reason)) return "ban: no account '" + w[1] + "'";
        accounts_.save();
        if (Conn* c = conn_of_name(w[1])) kick_conn(*c, reason.empty() ? "banned" : "banned: " + reason);
        audit(std::string(line));
        out << "banned " << w[1] << (until == kBanForever ? " permanently" : " for " + w[2]);
    } else if (cmd == "unban" && w.size() >= 2) {
        if (!accounts_.unban(w[1])) return "unban: no account '" + w[1] + "'";
        accounts_.save();
        audit(std::string(line));
        out << "unbanned " << w[1];
    } else if (cmd == "kick" && w.size() >= 2) {
        Conn* c = conn_of_name(w[1]);
        if (!c) return "kick: '" + w[1] + "' is not online";
        const std::string reason = rest_after(line, 2);
        kick_conn(*c, reason.empty() ? "kicked by an admin" : reason);
        audit(std::string(line));
        out << "kicked " << w[1];
    } else if (cmd == "close" && w.size() >= 2) {
        const auto world = static_cast<std::uint32_t>(std::strtoul(w[1].c_str(), nullptr, 10));
        if (!games_.count(world)) return "close: no world " + w[1];
        close_world(world, "closed by an admin");
        broadcast_roster();
        audit(std::string(line));
        out << "closed world " << world;
    } else if (cmd == "broadcast" && w.size() >= 2) {
        const std::string text = rest_after(line, 1);
        for (const auto& [id, c] : conns_)
            if (c.account_id) server_message(c, notice::Broadcast, text);  // 132on2g: a private ChatFwd to the others
        audit(std::string(line));
        out << "broadcast sent";
    } else if (cmd == "cheats") {
        if (w.size() >= 2) {
            bool on = false;
            if (!flag_word(w[1], on)) return "cheats: on or off";
            config_.allow_cheats = on;
            push_cheat_policy();
            audit(std::string(line));
        }
        out << "allow_cheats=" << (config_.allow_cheats ? 1 : 0);
    } else if (cmd == "mods") {
        out << mods_status();
    } else if (cmd == "relay") {
        if (relay_)
            out << relay_->status(relay_now_ms()) << "\n  UDP port " << relay_port_;
        else
            out << "relay off (relay_port=0)";
    } else if (cmd == "stats" || cmd == "results") {
        out << stats_command(w);
    } else if (cmd == "limits") {
        out << limits_text(now);
        if (w.size() >= 2 && w[1] == "clear") {
            login_failures_.clear();
            registrations_.clear();
            login_by_ip_.clear();
            login_by_name_.clear();
            chat_by_ip_.clear();
            floods_.clear();
            audit("limits clear");
            out << "\ncleared the failed-login locks, the account-creation counts, the login / chat rate buckets and "
                   "the flood blocks";
        }
    } else if (cmd == "admin" && w.size() >= 3) {
        bool on = false;
        if (!flag_word(w[2], on)) return "admin: on or off";
        if (!accounts_.set_admin(w[1], on)) return "admin: no account '" + w[1] + "'";
        accounts_.save();
        if (Conn* c = conn_of_name(w[1])) c->admin = on;
        audit(std::string(line));
        out << w[1] << (on ? " is an admin" : " is not an admin");
    } else if (cmd == "password" && w.size() >= 3) {
        if (!accounts_.set_password(w[1], w[2])) return "password: no account or bad password";
        accounts_.save();
        audit("password " + w[1] + " <hidden>");
        out << "password set for " << w[1];
    } else if (cmd == "create" && w.size() >= 3) {
        const CreateResult r = accounts_.create(w[1], w[2], now);
        if (r != CreateResult::Created) return "create: refused (name taken or invalid)";
        accounts_.save();
        audit("create " + w[1] + " <hidden>");
        out << "created " << w[1];
    } else if (cmd == "policy" && w.size() == 1) {
        out << "policy version " << config_.policy_version << "\n[usage]\n" << config_.usage_policy << "\n[privacy]\n"
            << (config_.privacy_policy.empty() ? "(empty: the page is skipped)" : config_.privacy_policy);
    } else if (cmd == "policy" && w.size() >= 3 && (w[1] == "usage" || w[1] == "privacy")) {
        const std::string text = (w[1] == "privacy" && w.size() == 3 && w[2] == "clear") ? "" : unescape_text(rest_after(line, 2));
        if (w[1] == "usage" && text.empty()) return "policy: the usage policy cannot be empty";
        (w[1] == "usage" ? config_.usage_policy : config_.privacy_policy) = text;
        ++config_.policy_version;
        save_policy();
        audit(w[1] + " policy edited, version " + std::to_string(config_.policy_version));
        out << w[1] << " policy set; version " << config_.policy_version
            << " (every account accepts it again at its next connect)";
    } else if (cmd == "announce" && w.size() >= 2) {
        if (w[1] == "list") {
            out << "announcements: " << config_.announcements.size();
            for (std::size_t i = 0; i < config_.announcements.size(); ++i)
                out << "\n  " << (i + 1) << ": " << config_.announcements[i];
            return out.str();
        }
        if (w[1] == "add" && w.size() >= 3) {
            config_.announcements.push_back(unescape_text(rest_after(line, 2)));
        } else if (w[1] == "del" && w.size() >= 3) {
            const auto n = std::strtoul(w[2].c_str(), nullptr, 10);
            if (n < 1 || n > config_.announcements.size()) return "announce: no announcement " + w[2];
            config_.announcements.erase(config_.announcements.begin() + static_cast<std::ptrdiff_t>(n - 1));
        } else if (w[1] == "clear") {
            config_.announcements.clear();
        } else {
            return "announce: list | add <text> | del <n> | clear";
        }
        save_policy();
        audit(std::string(line));
        out << "announcements: " << config_.announcements.size();
    } else if (cmd == "motd" && w.size() >= 2) {
        const std::string text = unescape_text(rest_after(line, 1));
        if (config_.announcements.empty())
            config_.announcements.push_back(text);
        else
            config_.announcements[0] = text;
        save_policy();
        audit(std::string(line));
        out << "motd set (announcement 1)";
    } else {
        return "unknown or incomplete command '" + cmd + "' (try help)";
    }
    return out.str();
}

}  // namespace omega::online
