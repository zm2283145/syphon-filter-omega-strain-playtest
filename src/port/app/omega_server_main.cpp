// PORT-ONLY (not in the original; checkpoint 132on1, research/ONLINE.md §5): omega_server, the server side of the
// port's online play. It stands in for the Medius servers the original game reached (authentication / lobby; the
// in-level traffic is the game's own net core, peer to peer from the joiners to the host, ONLINE.md §3.3).
//
//   omega_server [--config FILE] [--bind IP] [--port N] [--accounts FILE] [--policy FILE] [--log FILE]
//                [--stats FILE] [--log-format text|json] [--no-console] [--run-seconds N]
//                [--mod-policy gameplay|any|cosmetic|none] [--relay-port N|any] [--social FILE] [--data-dir DIR]
//                [--parent-pid N] [--service]
//       Runs the server. Its stdin is the admin console (type `help`; `quit` stops the server; `mods` lists the mod
//       reports). --mod-policy (or the config's mod_policy=; checkpoint 132md4, ONLINE.md §6.9): who may create or
//       join a game by the mods their client reports (ServerConfig::mod_policy, applied by OmegaServer; 132md16:
//       gameplay, the default, needs a joiner's gameplay mods to be the game creator's).
//       Checkpoint 132on2d (ONLINE.md §6.10, research/ONLINE_DEPLOY.md): --stats (stats_file=) the statistics store,
//       --log-format (log_format=) json writes one JSON object per log line; backups= / backup_interval= the
//       rotated copies of the account, statistics and policy files.
//       Checkpoint 132on2e (online_relay.hpp, ONLINE_DEPLOY.md §2.3): --relay-port (relay_port=) opens the in-level
//       UDP relay on that port (0, the default: off); relay_max_endpoints / relay_packet_rate / relay_byte_rate /
//       relay_idle are its limits; the console's `relay` prints its state.
//       Checkpoint 132on2g (ONLINE.md §6.14, ONLINE_DEPLOY.md §3, §5, §8): the stores are written atomically and durably, and a store that
//       does not parse falls back to its newest good backup (online_files.hpp); the log rotates at log_max_bytes=
//       (default 10 MiB) keeping log_keep= files; the console's `shutdown` / `restart` warn the players, close the
//       games keeping their results and save (restart: in-process, the same sockets); Ctrl+C / SIGTERM / a service
//       stop are a graceful shutdown now. --social (social_file=) the contact / invitation / recent-player store;
//       chat_filter= / chat_filter_words= the chat filter. --data-dir DIR runs in DIR (relative paths resolve there).
//       --parent-pid N stops the server when process N ends (the test scripts pass their own). --service (Windows)
//       runs under the Service Control Manager (no console). --port 0 and --relay-port any take system-picked ports,
//       which the log's "listening on" / "relay: UDP port" lines name (the test scripts' free ports).
//   omega_server admin HOST[:PORT] (--token TOKEN | --user NAME --password PASSWORD) COMMAND...
//       Runs one admin command against a running server and prints the reply.
//   omega_server demo HOST[:PORT]
//       A scripted loopback session of test clients (a declined policy; sign-up, policy, announcements, login, host,
//       list, join, chat, the cheat policy, the statistics; 132on2g: contacts, recent players, the chat filter);
//       prints the transcript. Exit code 0 on success.
//   omega_server login HOST[:PORT] NAME PASSWORD [--original]
//       One client's login sequence (session begin, policy, announcements, login); prints the texts, the status and
//       the notices. --original sends no hello, like an original PS2 session: it must receive no extension frame.
//   omega_server load HOST[:PORT] [--clients N] [--group N] [--chat N] [--relay-seconds S] [--relay-rate R]
//                     [--original-every K] [--prefix NAME] [--token TOKEN]   |   load HOST[:PORT] --abuse
//       132on2h (omega_server_load.cpp): N headless clients through login, lobby, create / join, the relay and leave,
//       with each request's latency and (--token) the server's status; --abuse checks the anti-abuse limits.
//       Exit code 0 when the login succeeded.
//
// Portable C++20: Winsock on Windows, BSD sockets elsewhere; the Windows-only parts (the service, the parent-process
// watch) are under _WIN32.

#include "online_accounts.hpp"
#include "online_client.hpp"
#include "online_files.hpp"
#include "online_mod_report.hpp"
#include "online_server.hpp"
#include "online_social.hpp"
#include "online_tcp.hpp"
#include "online_workers.hpp"

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <cerrno>
#include <signal.h>
#include <sys/resource.h>
#include <sys/types.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace omega::online;

namespace {

std::atomic<bool> g_stop{false};

std::uint64_t unix_now() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

// 132on2h (the `status` command's "process:" line): this process's CPU time and memory.
std::string process_info() {
    char text[160];
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    double cpu = 0;
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        auto seconds = [](const FILETIME& f) {
            return static_cast<double>((static_cast<unsigned long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e7;
        };
        cpu = seconds(kernel) + seconds(user);
    }
    PROCESS_MEMORY_COUNTERS m{};
    m.cb = sizeof m;
    K32GetProcessMemoryInfo(GetCurrentProcess(), &m, sizeof m);
    std::snprintf(text, sizeof text, "cpu %.2f s, working set %.1f MB (peak %.1f MB)", cpu,
                  static_cast<double>(m.WorkingSetSize) / 1048576.0, static_cast<double>(m.PeakWorkingSetSize) / 1048576.0);
#else
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    const double cpu = static_cast<double>(u.ru_utime.tv_sec + u.ru_stime.tv_sec) +
                       static_cast<double>(u.ru_utime.tv_usec + u.ru_stime.tv_usec) / 1e6;
    long rss_kb = 0;
    std::ifstream status("/proc/self/status");
    for (std::string line; std::getline(status, line);)
        if (line.rfind("VmRSS:", 0) == 0) rss_kb = std::strtol(line.c_str() + 6, nullptr, 10);
    std::snprintf(text, sizeof text, "cpu %.2f s, resident %.1f MB (peak %.1f MB)", cpu, static_cast<double>(rss_kb) / 1024.0,
                  static_cast<double>(u.ru_maxrss) / 1024.0);
#endif
    return text;
}

std::string timestamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

std::string read_file(const std::string& path, bool* ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (ok) *ok = false;
        return {};
    }
    std::ostringstream s;
    s << in.rdbuf();
    if (ok) *ok = true;
    return s.str();
}

// Checkpoint 132on2d (ONLINE.md §6.10): log_format=json writes each line as one JSON object: the UTC time, the level
// ("warn" for the limits, refusals and denials; else "info"), the event (the line's first word: login, logout, game,
// join, chat, stats, result, limit, admin, policy, mods, conn, ...) and the text.
std::string utc_iso() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string json_text(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += static_cast<char>(c);
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\u%04x", c);
            out += buf;
        } else out += static_cast<char>(c);
    }
    return out + "\"";
}

std::string json_log_line(const std::string& line) {
    std::size_t end = 0;
    while (end < line.size() && (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_')) ++end;
    const std::string event = end ? line.substr(0, end) : "server";
    const bool warn = event == "limit" || event == "abuse" || line.find("refused") != std::string::npos ||
                      line.find("denied") != std::string::npos || line.find("cannot") != std::string::npos ||
                      line.find("bad ") != std::string::npos;
    return "{\"ts\":" + json_text(utc_iso()) + ",\"level\":\"" + (warn ? "warn" : "info") + "\",\"event\":" +
           json_text(event) + ",\"msg\":" + json_text(line) + "}";
}

int usage() {
    std::cerr << "usage:\n"
                 "  omega_server [--config FILE] [--bind IP] [--port N] [--accounts FILE] [--policy FILE] [--log FILE]\n"
                 "               [--stats FILE] [--log-format text|json] [--no-console] [--run-seconds N]\n"
                 "               [--mod-policy gameplay|any|cosmetic|none] [--relay-port N|any] [--social FILE]\n"
                 "               [--data-dir DIR] [--parent-pid N] [--service]\n"
                 "  omega_server admin HOST[:PORT] (--token TOKEN | --user NAME --password PASSWORD) COMMAND...\n"
                 "  omega_server demo HOST[:PORT]\n"
                 "  omega_server login HOST[:PORT] NAME PASSWORD [--original]\n"
                 "  omega_server load HOST[:PORT] [--clients N] [--group N] [--chat N] [--relay-seconds S]\n"
                 "               [--relay-rate R] [--original-every K] [--prefix NAME] [--token TOKEN] | --abuse\n";
    return 2;
}

// ---- server mode -------------------------------------------------------------------------------------------------

// Everything the config file and the command line set; read at the start and again by an in-process restart.
struct Settings {
    ServerConfig cfg;
    std::string bind_text = "127.0.0.1", accounts_path = "omega_accounts.txt", policy_path = "omega_policy.txt",
                log_path = "omega_server.log", stats_path = "omega_stats.txt", log_format = "text",
                social_path = "omega_social.txt", filter_words_path;
    std::uint32_t iterations = kDefaultPbkdf2Iterations;
    int password_threads = 0;  // 132on2h: the PBKDF2 worker threads (0: half the CPUs, 1..4)
    int backups = 5;
    std::uint64_t backup_interval = 3600;
    std::uint64_t log_max_bytes = 10ull << 20;  // 132on2g: the log's rotation (0: never)
    int log_keep = 5;
};

struct CommandLine {
    std::string config_path;
    int port_override = -1;  // 0: a system-picked port (the log's "listening on" line names it)
    int relay_port_override = -1;
    std::optional<std::string> bind, accounts, policy, log, stats, log_format, mod_policy, social;
};

Settings load_settings(const CommandLine& cl) {
    Settings s;
    ServerConfig& cfg = s.cfg;
    if (!cl.config_path.empty()) {
        bool ok = false;
        const std::string text = read_file(cl.config_path, &ok);
        if (!ok) throw std::runtime_error("cannot read " + cl.config_path);
        const auto extra = parse_server_config(text, cfg);
        auto take = [&](const char* key, std::string& into) {
            if (auto it = extra.find(key); it != extra.end()) into = it->second;
        };
        take("accounts", s.accounts_path);
        take("policy_file", s.policy_path);
        take("log", s.log_path);
        take("bind", s.bind_text);
        take("stats_file", s.stats_path);
        take("log_format", s.log_format);
        take("social_file", s.social_path);
        take("chat_filter_words", s.filter_words_path);
        if (auto it = extra.find("backups"); it != extra.end()) s.backups = std::clamp(std::atoi(it->second.c_str()), 0, 100);
        if (auto it = extra.find("backup_interval"); it != extra.end()) {
            const auto seconds = parse_ban_duration(it->second);
            if (!seconds || *seconds == kBanForever) throw std::runtime_error("bad backup_interval " + it->second);
            s.backup_interval = *seconds;
        }
        if (auto it = extra.find("pbkdf2_iterations"); it != extra.end())
            s.iterations = static_cast<std::uint32_t>(std::max(1000L, std::strtol(it->second.c_str(), nullptr, 10)));
        if (auto it = extra.find("password_threads"); it != extra.end())
            s.password_threads = std::clamp(std::atoi(it->second.c_str()), 0, 64);
        if (auto it = extra.find("log_max_bytes"); it != extra.end()) s.log_max_bytes = std::strtoull(it->second.c_str(), nullptr, 10);
        if (auto it = extra.find("log_keep"); it != extra.end()) s.log_keep = std::atoi(it->second.c_str());
    }
    if (cl.bind) s.bind_text = *cl.bind;
    if (cl.accounts) s.accounts_path = *cl.accounts;
    if (cl.policy) s.policy_path = *cl.policy;
    if (cl.log) s.log_path = *cl.log;
    if (cl.stats) s.stats_path = *cl.stats;
    if (cl.social) s.social_path = *cl.social;
    if (cl.log_format) s.log_format = *cl.log_format;
    if (s.log_format != "text" && s.log_format != "json")
        throw std::runtime_error("bad log format '" + s.log_format + "' (text or json)");
    if (cl.mod_policy) {
        const auto policy = parse_mod_policy(*cl.mod_policy);
        if (!policy) throw std::runtime_error("bad mod policy '" + *cl.mod_policy + "' (gameplay, any, cosmetic or none)");
        cfg.mod_policy = *policy;
    }
    if (cl.port_override >= 0 && cl.port_override < 65536) cfg.port = static_cast<std::uint16_t>(cl.port_override);
    if (cl.relay_port_override > 65535) throw std::runtime_error("bad relay port " + std::to_string(cl.relay_port_override));
    if (cl.relay_port_override >= 0) cfg.relay_port = static_cast<std::uint16_t>(cl.relay_port_override);
    bool have_policy = false;
    const std::string policy_text = read_file(s.policy_path, &have_policy);
    if (have_policy) parse_policy_file(policy_text, cfg);
    else write_file_atomic(s.policy_path, format_policy_file(cfg));  // a first, editable copy
    return s;
}

// 132on2g: --parent-pid. The test scripts pass their own process id, so a server they leave behind (a script killed
// or timed out) stops by itself instead of holding its port.
class ParentWatch {
public:
    explicit ParentWatch(unsigned long pid) : pid_(pid) {
#ifdef _WIN32
        if (pid_) handle_ = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid_));
#endif
    }
    ~ParentWatch() {
#ifdef _WIN32
        if (handle_) CloseHandle(handle_);
#endif
    }
    ParentWatch(const ParentWatch&) = delete;
    ParentWatch& operator=(const ParentWatch&) = delete;
    bool active() const { return pid_ != 0; }
    bool gone() const {
        if (!pid_) return false;
#ifdef _WIN32
        return !handle_ || WaitForSingleObject(handle_, 0) != WAIT_TIMEOUT;
#else
        return kill(static_cast<pid_t>(pid_), 0) != 0 && errno == ESRCH;
#endif
    }

private:
    unsigned long pid_ = 0;
#ifdef _WIN32
    HANDLE handle_ = nullptr;
#endif
};

int run_server(int argc, char** argv, bool service) {
    CommandLine cl;
    bool console = !service;
    int run_seconds = 0;
    unsigned long parent_pid = 0;
    std::string data_dir;
    bool relay_any = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--config") cl.config_path = next();
        else if (a == "--bind") cl.bind = next();
        else if (a == "--port") cl.port_override = std::atoi(next().c_str());
        else if (a == "--accounts") cl.accounts = next();
        else if (a == "--policy") cl.policy = next();
        else if (a == "--log") cl.log = next();
        else if (a == "--stats") cl.stats = next();
        else if (a == "--social") cl.social = next();
        else if (a == "--log-format") cl.log_format = next();
        else if (a == "--no-console") console = false;
        else if (a == "--run-seconds") run_seconds = std::atoi(next().c_str());
        else if (a == "--mod-policy") cl.mod_policy = next();
        else if (a == "--relay-port") {
            const std::string v = next();
            // 132on2g: "any" opens the relay on a system-picked UDP port (the log's "relay: UDP port" line names it)
            if (v == "any") relay_any = true;
            else cl.relay_port_override = std::atoi(v.c_str());
        }
        else if (a == "--parent-pid") parent_pid = std::strtoul(next().c_str(), nullptr, 10);
        else if (a == "--data-dir") data_dir = next();
        else if (a == "--service") continue;
        else return usage();
    }
    Settings s;
    try {
        if (!data_dir.empty()) std::filesystem::current_path(data_dir);
        s = load_settings(cl);
    } catch (const std::exception& e) {
        std::cerr << "omega_server: " << e.what() << "\n";
        return 1;
    }

    RotatingLog log_file;
    if (!log_file.open(s.log_path, s.log_max_bytes, s.log_keep))
        std::cerr << "omega_server: cannot open the log " << s.log_path << " (logging to stdout only)\n";
    std::mutex log_mutex;
    bool json_log = s.log_format == "json";
    auto log = [&](const std::string& line) {
        std::lock_guard<std::mutex> lock(log_mutex);
        const std::string stamped = json_log ? json_log_line(line) : timestamp() + " " + line;
        if (!service) std::cout << stamped << "\n" << std::flush;
        log_file.write_line(stamped);
    };

    const auto bind_ip = s.bind_text == "0.0.0.0" ? std::optional<std::uint32_t>(0) : resolve_ipv4(s.bind_text);
    if (!bind_ip) {
        std::cerr << "omega_server: bad bind address " << s.bind_text << "\n";
        log("omega_server: bad bind address " + s.bind_text);
        return 1;
    }
    TcpServer tcp;
    std::string error;
    if (!tcp.listen(*bind_ip, s.cfg.port, &error)) {
        std::cerr << "omega_server: listen on " << s.bind_text << ":" << s.cfg.port << " failed: " << error << "\n";
        log("omega_server: listen on " + s.bind_text + ":" + std::to_string(s.cfg.port) + " failed: " + error);
        return 1;
    }
    const std::string first_bind = s.bind_text;
    const std::uint16_t first_port = tcp.port(), first_relay_port = s.cfg.relay_port;
    UdpSocket udp;
    if ((s.cfg.relay_port || relay_any) && !udp.open(*bind_ip, relay_any ? 0 : s.cfg.relay_port, &error)) {
        std::cerr << "omega_server: relay UDP port " << s.bind_text << ":" << s.cfg.relay_port << " failed: " << error << "\n";
        log("omega_server: relay UDP port " + std::to_string(s.cfg.relay_port) + " failed: " + error);
        return 1;
    }

    std::mutex console_mutex;
    std::deque<std::string> console_lines;
    if (console) {
        std::thread([&] {
            std::string line;
            while (std::getline(std::cin, line)) {
                std::lock_guard<std::mutex> lock(console_mutex);
                console_lines.push_back(line);
            }
        }).detach();
    }
    std::signal(SIGINT, [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });  // 132on2d: a service manager's stop (systemd, ONLINE_DEPLOY.md)
    const ParentWatch parent(parent_pid);
    if (parent.active() && parent.gone()) {
        log("omega_server: parent process " + std::to_string(parent_pid) + " is not running; not starting");
        return 1;
    }
    auto steady_ms = [] {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    };
    const auto start = std::chrono::steady_clock::now();

    // One pass per server life: an admin `restart` closes every session, reads the settings and the stores again and
    // begins the next pass on the same sockets.
    for (int life = 1;; ++life) {
        ServerConfig cfg = s.cfg;
        cfg.port = tcp.port();
        cfg.relay_port = udp.is_open() ? udp.port() : 0;
        AccountStore accounts(s.accounts_path, s.iterations);
        accounts.set_backups(s.backups, s.backup_interval);
        StatsStore stats(s.stats_path, s.backups, cfg.max_results, s.backup_interval);
        SocialStore social(s.social_path, s.backups, s.backup_interval);
        ChatFilter filter;
        try {
            // 132on2g: a store that does not parse falls back to its newest backup that does (online_files.hpp).
            load_or_recover(s.accounts_path, s.backups, [&] { accounts.load(); }, log, unix_now());
            load_or_recover(s.stats_path, s.backups, [&] { stats.load(); }, log, unix_now());
            load_or_recover(s.social_path, s.backups, [&] { social.load(); }, log, unix_now());
            if (!s.filter_words_path.empty()) {
                bool ok = false;
                const std::string words = read_file(s.filter_words_path, &ok);
                if (!ok) throw std::runtime_error("cannot read chat_filter_words " + s.filter_words_path);
                filter.load_words(words);
            }
        } catch (const std::exception& e) {
            std::cerr << "omega_server: " << e.what() << "\n";
            log("omega_server: " + std::string(e.what()) + "; stopping");
            return 1;
        }

        WorkerPool passwords(s.password_threads > 0
                                 ? static_cast<unsigned>(s.password_threads)
                                 : std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u));
        OmegaServer server(
            accounts, cfg, [&](std::uint32_t conn, std::vector<std::uint8_t> frame) { tcp.send(conn, std::move(frame)); },
            [&](std::uint32_t conn) { tcp.close(conn); }, log, unix_now);
        // 132on2h: the password work (PBKDF2) on the pool; its answers are drained on this thread after each poll.
        server.set_hash_runner([&](OmegaServer::PasswordWork work, OmegaServer::PasswordDone done) {
            passwords.submit([work = std::move(work), done = std::move(done)]() mutable -> std::function<void()> {
                auto result = std::make_shared<std::vector<std::uint8_t>>(work());
                return [done = std::move(done), result]() { done(std::move(*result)); };
            });
        });
        server.set_policy_file(s.policy_path, s.backups);
        server.stats() = std::move(stats);
        server.social() = std::move(social);
        server.chat_filter() = std::move(filter);
        std::optional<RelayCore> relay;
        if (udp.is_open()) {
            relay.emplace(
                server.relay_limits(),
                [&](std::uint32_t ip, std::uint16_t port, std::vector<std::uint8_t> bytes) { udp.send_to(ip, port, bytes); },
                [&](std::uint32_t conn) { return server.world_of(conn); }, RelayCore::TokenFn{}, log);
            server.set_relay(&*relay, udp.port());
        }
        log("omega_server (PORT-ONLY) listening on " + first_bind + ":" + std::to_string(tcp.port()) + "; accounts " +
            s.accounts_path + " (" + std::to_string(accounts.accounts().size()) + "), policy " + s.policy_path +
            " (version " + std::to_string(server.config().policy_version) + "), allow_cheats=" +
            (server.config().allow_cheats ? "1" : "0") + (life > 1 ? " (restart " + std::to_string(life - 1) + ")" : ""));
        log(std::string("mod_policy=") + mod_policy_name(server.config().mod_policy));
        const std::string limits = server.admin_command("limits", "startup");
        log("stats " + s.stats_path + " (" + std::to_string(server.stats().records().size()) + " record(s), " +
            std::to_string(server.stats().results().size()) + " finished game(s)); backups=" + std::to_string(s.backups) +
            " every " + std::to_string(s.backup_interval) + " s; pbkdf2_iterations=" + std::to_string(s.iterations) +
            " (password_threads=" + std::to_string(passwords.threads()) + ")" +
            "; limits: " + limits.substr(0, limits.find('\n')));
        log("social " + s.social_path + "; chat_filter=" + (server.config().chat_filter ? "1" : "0") + " (" +
            std::to_string(server.chat_filter().size()) + " words" +
            (s.filter_words_path.empty() ? ", built-in" : ", " + s.filter_words_path) + "); log " + s.log_path +
            " (log_max_bytes=" + std::to_string(s.log_max_bytes) + " log_keep=" + std::to_string(s.log_keep) + ")" +
            (parent.active() ? "; watching parent process " + std::to_string(parent_pid) : ""));
        if (relay)
            log("relay: UDP port " + first_bind + ":" + std::to_string(udp.port()) + " (in-level relay for port clients; " +
                "relay_max_endpoints=" + std::to_string(cfg.relay_max_endpoints) + " relay_packet_rate=" +
                std::to_string(cfg.relay_packet_rate) + " relay_byte_rate=" + std::to_string(cfg.relay_byte_rate) +
                " relay_idle=" + std::to_string(cfg.relay_idle) + ")");
        else
            log("relay: off (relay_port=0)");

        auto last_flush = std::chrono::steady_clock::now();
        auto last_relay_log = last_flush, last_parent_check = last_flush;
        std::uint64_t relay_logged = 0;
        bool run_out = false;
        std::size_t frames_this_tick = 0;
        server.set_process_info(process_info);
        TcpServer::Events events;
        events.connected = [&](std::uint32_t conn, std::uint32_t ip) { server.on_connect(conn, ip, ClientKind::Port); };
        events.frame = [&](std::uint32_t conn, const Frame& f) {
            ++frames_this_tick;
            server.on_frame(conn, f);
        };
        events.disconnected = [&](std::uint32_t conn) { server.on_disconnect(conn); };
        OmegaServer::Lifecycle ending = OmegaServer::Lifecycle::None;
        while (ending == OmegaServer::Lifecycle::None) {
            const auto tick_start = std::chrono::steady_clock::now();
            frames_this_tick = 0;
            // A signal, a service stop, the console's quit, --run-seconds or the parent's end: a shutdown now (the
            // sessions are still closed and the stores saved).
            const bool pending_now = server.lifecycle_pending() == OmegaServer::Lifecycle::Shutdown;
            if (g_stop && !pending_now) server.request_stop(OmegaServer::Lifecycle::Shutdown, 0, "the server is stopping");
            tcp.poll(passwords.outstanding() ? 2 : 50, events, relay ? &udp : nullptr);
            passwords.drain();
            if (relay) {
                for (int n = 0; n < 4096; ++n) {
                    auto d = udp.receive();
                    if (!d) break;
                    relay->on_datagram(d->ip, d->port, d->bytes, steady_ms());
                }
                // The counters in the log every 10 seconds while they change.
                const std::uint64_t seen = relay->stats().received;
                if (seen != relay_logged && std::chrono::steady_clock::now() - last_relay_log > std::chrono::seconds(10)) {
                    const std::string st = relay->status(steady_ms());
                    log(st.substr(0, st.find('\n')));
                    relay_logged = seen;
                    last_relay_log = std::chrono::steady_clock::now();
                }
            }
            std::deque<std::string> lines;
            {
                std::lock_guard<std::mutex> lock(console_mutex);
                lines.swap(console_lines);
            }
            for (const auto& line : lines) {
                if (line == "quit" || line == "exit") {
                    server.request_stop(OmegaServer::Lifecycle::Shutdown, 0, "the server is stopping (console)");
                    break;
                }
                const std::string out = server.admin_command(line, "console");
                if (!out.empty()) std::cout << out << "\n" << std::flush;
            }
            const auto now = std::chrono::steady_clock::now();
            if (!run_out && run_seconds > 0 && now - start > std::chrono::seconds(run_seconds)) {
                run_out = true;
                log("omega_server: --run-seconds " + std::to_string(run_seconds) + " reached");
                server.request_stop(OmegaServer::Lifecycle::Shutdown, 0, "the server is stopping");
            }
            if (parent.active() && now - last_parent_check > std::chrono::milliseconds(500)) {
                last_parent_check = now;
                if (parent.gone() && server.lifecycle_pending() != OmegaServer::Lifecycle::Shutdown) {
                    log("omega_server: parent process " + std::to_string(parent_pid) + " gone, stopping");
                    server.request_stop(OmegaServer::Lifecycle::Shutdown, 0, "the server is stopping");
                }
            }
            server.tick();
            ending = server.lifecycle_due();
            if (now - last_flush > std::chrono::seconds(30)) {
                server.flush_stats();
                last_flush = now;
            }
            const auto spent = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - tick_start).count());
            server.note_tick(spent > tcp.last_wait_us() ? spent - tcp.last_wait_us() : 0, frames_this_tick);
        }
        const bool restart = ending == OmegaServer::Lifecycle::Restart;
        const std::string reason = server.stop_reason();
        server.shutdown_sessions(std::string(restart ? "The server is restarting now" : "The server is shutting down now") +
                                 (reason.empty() ? "." : ": " + reason));
        // The goodbyes are written before the sockets close (TcpServer::close sends what is queued first); a new
        // connection meanwhile is closed at once.
        TcpServer::Events drain;
        drain.connected = [&](std::uint32_t conn, std::uint32_t) { tcp.close(conn); };
        drain.frame = [](std::uint32_t, const Frame&) {};
        drain.disconnected = [&](std::uint32_t conn) { server.on_disconnect(conn); };
        const auto drain_until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (tcp.connection_count() > 0 && std::chrono::steady_clock::now() < drain_until) tcp.poll(20, drain);
        if (relay) log(relay->status(steady_ms()));
        if (!restart) {
            log("omega_server stopping");
            return 0;
        }
        log("omega_server restarting (in-process)");
        try {
            Settings next = load_settings(cl);
            if (next.bind_text != first_bind || (next.cfg.port != s.cfg.port && next.cfg.port != first_port) ||
                next.cfg.relay_port != first_relay_port)
                log("omega_server: bind / port / relay_port changes need a process restart; keeping " + first_bind + ":" +
                    std::to_string(first_port) + (udp.is_open() ? ", relay " + std::to_string(udp.port()) : ""));
            if (next.log_path != s.log_path || next.log_max_bytes != s.log_max_bytes || next.log_keep != s.log_keep) {
                std::lock_guard<std::mutex> lock(log_mutex);
                if (!log_file.open(next.log_path, next.log_max_bytes, next.log_keep))
                    std::cerr << "omega_server: cannot open the log " << next.log_path << "\n";
            }
            json_log = next.log_format == "json";
            s = std::move(next);
        } catch (const std::exception& e) {
            log(std::string("omega_server: the settings did not load again (") + e.what() + "); keeping the previous ones");
        }
        g_stop = false;
    }
}

#ifdef _WIN32
// 132on2g (ONLINE_DEPLOY.md "Windows service"): `omega_server --service --data-dir DIR [server options]` run by the
// Service Control Manager (sc.exe create ... binPath= "...omega_server.exe --service --data-dir ..."). STOP and
// the system's shutdown set g_stop: the same graceful stop as Ctrl+C.
int g_service_argc = 0;
char** g_service_argv = nullptr;
SERVICE_STATUS_HANDLE g_service_handle = nullptr;
SERVICE_STATUS g_service_status{};

void set_service_state(DWORD state, DWORD exit_code = NO_ERROR, DWORD wait_hint = 0) {
    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwCurrentState = state;
    g_service_status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    g_service_status.dwWin32ExitCode = exit_code == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR;
    g_service_status.dwServiceSpecificExitCode = exit_code;
    g_service_status.dwWaitHint = wait_hint;
    if (g_service_handle) SetServiceStatus(g_service_handle, &g_service_status);
}

DWORD WINAPI service_control(DWORD control, DWORD, LPVOID, LPVOID) {
    if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN) {
        g_stop = true;
        set_service_state(SERVICE_STOP_PENDING, 0, 15000);
        return NO_ERROR;
    }
    return control == SERVICE_CONTROL_INTERROGATE ? NO_ERROR : ERROR_CALL_NOT_IMPLEMENTED;
}

void WINAPI service_main(DWORD, LPWSTR*) {
    g_service_handle = RegisterServiceCtrlHandlerExW(L"omega_server", service_control, nullptr);
    if (!g_service_handle) return;
    set_service_state(SERVICE_RUNNING);
    const int code = run_server(g_service_argc, g_service_argv, true);
    set_service_state(SERVICE_STOPPED, static_cast<DWORD>(code));
}

int run_service(int argc, char** argv) {
    g_service_argc = argc;
    g_service_argv = argv;
    wchar_t name[] = L"omega_server";
    const SERVICE_TABLE_ENTRYW table[] = {{name, service_main}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) {
        std::cerr << "omega_server --service: not started by the Service Control Manager (error " << GetLastError()
                  << "); run it without --service from a console\n";
        return 1;
    }
    return 0;
}
#endif

// ---- admin mode --------------------------------------------------------------------------------------------------

// The login sequence the game runs (ONLINE.md §3.2): session begin, the usage / privacy policy, then the login.
bool sign_in(OnlineClient& c, const std::string& name, const std::string& password, std::string* key_out,
             std::int32_t* status_out, std::uint32_t* account_out = nullptr) {
    c.send_medius(mid::SessionBegin, request::session_begin("sb"));
    const auto sb = c.wait_for(mid::SessionBeginResponse, 3000);
    if (!sb) return false;
    const std::string key = get_str(sb->body, kSessionKeyOffset, kSessionKeyLength);
    c.send_medius(mid::GetPolicy, request::policy(key, false));
    if (c.wait_for_list(mid::GetPolicyResponse, 0x11c, 3000).empty()) return false;
    c.send_medius(mid::GetPolicy, request::policy(key, true));
    if (c.wait_for_list(mid::GetPolicyResponse, 0x11c, 3000).empty()) return false;
    c.send_medius(mid::GetAnnouncements, request::announcements(key));
    if (c.wait_for_list(mid::GetAnnouncementsResponse, 0x408, 3000).empty()) return false;
    c.send_medius(mid::AccountLogin, request::login(key, name, password));
    const auto lr = c.wait_for(mid::AccountLoginResponse, 3000);
    if (!lr) return false;
    if (key_out) *key_out = key;
    if (status_out) *status_out = get_i32(lr->body, kStatusOffset);
    if (account_out) *account_out = static_cast<std::uint32_t>(get_i32(lr->body, 0x1c));
    return true;
}

int run_admin(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string server = argv[2];
    std::string token, user, password, command;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--token" && i + 1 < argc) token = argv[++i];
        else if (a == "--user" && i + 1 < argc) user = argv[++i];
        else if (a == "--password" && i + 1 < argc) password = argv[++i];
        else command += (command.empty() ? "" : " ") + a;
    }
    if (command.empty() || (token.empty() && user.empty())) return usage();
    OnlineClient c;
    std::string error;
    if (!c.connect(server, &error)) {
        std::cerr << "omega_server admin: " << error << "\n";
        return 1;
    }
    c.hello(this_platform(), 1, cap::Admin | cap::Notices);
    if (!user.empty()) {
        std::int32_t st = 0;
        if (!sign_in(c, user, password, nullptr, &st) || st != status::Success) {
            std::cerr << "omega_server admin: login failed (status " << st << ")\n";
            return 1;
        }
    }
    c.send_ext(ext::AdminCommand, ExtWriter().str(token.empty() ? command : "token " + token + " " + command).bytes());
    const auto reply = c.wait_admin_reply(5000);
    if (!reply) {
        std::cerr << "omega_server admin: no reply\n";
        return 1;
    }
    std::cout << *reply << "\n";
    return reply->rfind("denied", 0) == 0 ? 1 : 0;
}

// ---- demo mode (PORT-ONLY test clients, the loopback evidence) ---------------------------------------------------

struct Demo {
    std::string server;
    int failures = 0;
    void check(bool ok, const std::string& what) {
        std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n" << std::flush;
        if (!ok) ++failures;
    }
};

std::string policy_text(const std::vector<Frame>& rows) {
    std::string s;
    for (const auto& f : rows)
        if (get_i32(f.body, kStatusOffset) == status::Success) s += get_str(f.body, 0x1c, 256);
    return s;
}

// A statistics record in 002cbd40's layout (ONLINE.md §3.6) for the demo's scripted clients.
std::vector<std::uint8_t> demo_record(std::uint8_t rank, std::uint32_t shots, std::uint32_t hits, std::uint32_t kills,
                                      std::uint32_t deaths) {
    std::vector<std::uint8_t> r(kStatsBlobSize, 0);
    r[0] = kStatsBlobVersion;
    r[1] = rank;
    r[3] = static_cast<std::uint8_t>(shots ? 100 * hits / shots : 0);
    put_i32(r, 0x10, static_cast<std::int32_t>(shots));
    put_i32(r, 0x14, static_cast<std::int32_t>(hits));
    put_i32(r, 0x1c, static_cast<std::int32_t>(kills));
    put_i32(r, 0x20, static_cast<std::int32_t>(deaths));
    return r;
}

int run_demo(int argc, char** argv) {
    if (argc < 3) return usage();
    Demo d;
    d.server = argv[2];
    const std::string stamp = std::to_string(unix_now() % 100000);
    const std::string host_name = "Gabe" + stamp, join_name = "Lian" + stamp;
    auto connect = [&](OnlineClient& c, const char* who) {
        std::string error;
        const bool ok = c.connect(d.server, &error);
        d.check(ok, std::string(who) + " connects to " + d.server + (ok ? "" : ": " + error));
        if (ok) c.hello(Platform::Pc, 1, cap::All);
        return ok;
    };

    std::cout << "== 1. ONLINECN after DNAS: the policy pages, Decline (the client leaves for ONOFFSEL)\n";
    {
        OnlineClient c;
        if (!connect(c, "client C")) return 1;
        c.send_medius(mid::SessionBegin, request::session_begin("sb"));
        const auto sb = c.wait_for(mid::SessionBeginResponse, 3000);
        d.check(sb && get_i32(sb->body, 0x2c) == 0, "SessionBegin (27 -> 28) status 0, session key issued");
        const std::string key = sb ? get_str(sb->body, kSessionKeyOffset, kSessionKeyLength) : "";
        c.send_medius(mid::GetPolicy, request::policy(key, false));
        const auto usage_rows = c.wait_for_list(mid::GetPolicyResponse, 0x11c, 3000);
        std::cout << "  usage policy page (PolicyTitle1):\n    " << policy_text(usage_rows) << "\n";
        d.check(!usage_rows.empty() && get_str(usage_rows[0].body, 0, 21) == "USAGE", "GetPolicy USAGE echoed");
        std::cout << "  [Decline] -> LOBBY_STR 0xe quit popup -> yes -> 0x0029de70 -> ONOFFSEL; the client disconnects\n";
        c.disconnect();
    }

    std::cout << "== 2. Host: sign-up, policy -> Accept, announcements, login, lobby, create game\n";
    OnlineClient a;
    if (!connect(a, "client A (host)")) return 1;
    a.pump(200);
    d.check(a.welcomed(), "ext Welcome: server '" + a.server_name() + "', caps " + std::to_string(a.caps()) +
                              ", allow_cheats " + (a.cheats_allowed() ? "1" : "0"));
    a.send_medius(mid::SessionBegin, request::session_begin("sb"));
    const auto sba = a.wait_for(mid::SessionBeginResponse, 3000);
    const std::string key_a = sba ? get_str(sba->body, kSessionKeyOffset, kSessionKeyLength) : "";
    a.send_medius(mid::GetPolicy, request::policy(key_a, false));
    const auto pa = a.wait_for_list(mid::GetPolicyResponse, 0x11c, 3000);
    std::cout << "  usage policy page:\n    " << policy_text(pa) << "\n  [Accept]\n";
    a.send_medius(mid::GetPolicy, request::policy(key_a, true));
    const auto pp = a.wait_for_list(mid::GetPolicyResponse, 0x11c, 3000);
    std::cout << "  privacy policy page (PolicyTitle2):\n    " << policy_text(pp) << "\n  [Accept]\n";
    a.send_medius(mid::GetAnnouncements, request::announcements(key_a));
    const auto an = a.wait_for_list(mid::GetAnnouncementsResponse, 0x408, 3000);
    for (const auto& f : an)
        if (get_i32(f.body, kStatusOffset) == 0)
            std::cout << "  announcement page " << get_i32(f.body, 0x1c) << ": " << get_str(f.body, 0x20, 1000) << "\n  [OK]\n";
    d.check(!an.empty(), "GetAnnouncements (75 -> 77) answered, EndOfList set");
    a.send_medius(mid::AccountRegistration, request::registration(key_a, host_name, "omega-pass-1"));
    const auto ra = a.wait_for(mid::AccountRegistrationResponse, 5000);
    d.check(ra && get_i32(ra->body, kStatusOffset) == 0, "ACNTLOGN create account '" + host_name + "' (9 -> 10) status 0");
    a.send_medius(mid::AccountRegistration, request::registration(key_a, host_name, "other"));
    const auto ra2 = a.wait_for(mid::AccountRegistrationResponse, 5000);
    d.check(ra2 && get_i32(ra2->body, kStatusOffset) == status::AccountAlreadyExists, "the same name again: status -999 (exists)");
    a.send_medius(mid::AccountLogin, request::login(key_a, host_name, "wrong"));
    const auto la0 = a.wait_for(mid::AccountLoginResponse, 5000);
    d.check(la0 && get_i32(la0->body, kStatusOffset) == status::InvalidPassword, "login with a wrong password: status -978");
    a.send_medius(mid::AccountLogin, request::login(key_a, host_name, "omega-pass-1"));
    const auto la = a.wait_for(mid::AccountLoginResponse, 5000);
    const auto id_a = la ? static_cast<std::uint32_t>(get_i32(la->body, 0x1c)) : 0u;
    d.check(la && get_i32(la->body, kStatusOffset) == 0, "login (7 -> 8) status 0, account id " + std::to_string(id_a));
    // 0044ce60's tail: GetAccountStats (49 -> 50) for the own record, 002cbd40 builds it, AccountUpdateStats uploads.
    a.send_medius(mid::GetAccountStats, request::get_account_stats(key_a, static_cast<std::int32_t>(id_a)));
    const auto ga = a.wait_for(mid::GetAccountStatsResponse, 3000);
    d.check(ga && get_i32(ga->body, kStatusOffset) == 0 && get_str(ga->body, 0x1c, 32) == host_name,
            "GetAccountStats (49 -> 50) status 0, name '" + (ga ? get_str(ga->body, 0x1c, 32) : std::string()) +
                "', no record yet");
    const auto rec_a = demo_record(2, 120, 49, 12, 1);
    a.send_medius(mid::AccountUpdateStats, request::account_update_stats(key_a, rec_a));
    const auto ca = a.wait_for(mid::AccountUpdateStatsResponse, 3000);
    d.check(ca && get_i32(ca->body, kStatusOffset) == 0,
            "AccountUpdateStats (17 -> 18) status 0: " + describe_stats(rec_a));
    a.send_medius(mid::CreateGame, request::create_game(key_a, "Gabe's game\t4\t0\t1\t0"));
    const auto cg = a.wait_for(mid::CreateGameResponse, 3000);
    const auto world = cg ? static_cast<std::uint32_t>(get_i32(cg->body, 0x1c)) : 0u;
    d.check(cg && get_i32(cg->body, kStatusOffset) == 0 && world != 0, "MSNSETUP CreateGame (253 -> 34) world " + std::to_string(world));
    a.send_ext(ext::HostEndpoint, ExtWriter().u32(world).u16(47525).bytes());

    std::cout << "== 3. Joiner: sign-up, login, game list, join (the host address hand-off), chat\n";
    OnlineClient b;
    if (!connect(b, "client B (joiner)")) return 1;
    std::string key_b;
    {
        b.send_medius(mid::SessionBegin, request::session_begin("sb"));
        const auto sbb = b.wait_for(mid::SessionBeginResponse, 3000);
        key_b = sbb ? get_str(sbb->body, kSessionKeyOffset, kSessionKeyLength) : "";
        b.send_medius(mid::AccountRegistration, request::registration(key_b, join_name, "omega-pass-2"));
        const auto rb = b.wait_for(mid::AccountRegistrationResponse, 5000);
        d.check(rb && get_i32(rb->body, kStatusOffset) == 0, "create account '" + join_name + "'");
    }
    std::int32_t st = -1;
    std::uint32_t id_b = 0;
    const bool signed_in = sign_in(b, join_name, "omega-pass-2", &key_b, &st, &id_b);
    d.check(signed_in && st == 0,
            "policy -> Accept -> announcements -> login status 0, account id " + std::to_string(id_b));
    b.send_medius(mid::AccountUpdateStats, request::account_update_stats(key_b, demo_record(1, 30, 9, 2, 3)));
    b.wait_for(mid::AccountUpdateStatsResponse, 3000);
    b.send_medius(mid::GetAccountStats, request::get_account_stats(key_b, static_cast<std::int32_t>(id_a)));
    const auto gb = b.wait_for(mid::GetAccountStatsResponse, 3000);
    d.check(gb && get_i32(gb->body, kStatusOffset) == 0 && gb->body[0x48] == kStatsBlobVersion &&
                gb->body[0x49] == 2,
            "JOINMIS rank column: the host's record via GetAccountStats, rank row " +
                std::to_string(gb ? gb->body[0x49] : 0));
    b.send_medius(mid::GameList, request::game_list(key_b));
    const auto rows = b.wait_for_list(mid::GameListResponse, 0x6c, 3000);
    bool seen = false;
    for (const auto& f : rows) {
        if (get_i32(f.body, kStatusOffset) != 0) continue;
        const auto fields = parse_game_name(get_str(f.body, 0x20, 64));
        std::cout << "  JOINMIS row: world " << get_i32(f.body, 0x1c) << " '" << (fields ? fields->name : "?") << "' "
                  << "status " << get_i32(f.body, 0x60) << ", " << get_i32(f.body, 0x68) << " player(s), host badge ["
                  << platform_badge(b.platform_of(id_a)) << "]\n";
        if (static_cast<std::uint32_t>(get_i32(f.body, 0x1c)) == world) seen = true;
    }
    d.check(seen, "GameList (41 -> 42) shows the host's world");
    b.send_medius(mid::JoinGame, request::join_game(key_b, world));
    const auto jg = b.wait_for(mid::JoinGameResponse, 3000);
    const ConnectInfo ci = jg ? read_connect_info(jg->body, 0x20) : ConnectInfo{};
    d.check(jg && get_i32(jg->body, kStatusOffset) == 0 && ci.port == 47525 && ci.world == world,
            "JoinGame (35 -> 36): host " + ci.ip + ":" + std::to_string(ci.port) + " world " + std::to_string(ci.world) +
                " -> 0x0042a960 (the game's net connect; the in-level net core takes over)");
    b.pump(200);
    a.pump(200);
    d.check(b.platform_of(id_a) == Platform::Pc && a.platform_of(id_b) == Platform::Pc,
            "ext Roster: both players carry the [" + std::string(platform_badge(Platform::Pc)) + "] badge");
    a.send_medius(mid::ChatMessage, request::chat(key_a, "ready when you are"));
    const auto chat = b.wait_for(mid::ChatFwdMessage, 3000);
    d.check(chat && get_str(chat->body, 0x40, 64) == "ready when you are",
            "chat (59 -> 60): " +
                badged_name(chat ? get_str(chat->body, 0x1c, 32) : "", b.platform_of(chat ? get_i32(chat->body, 0x18) : 0), true) +
                ": " + (chat ? get_str(chat->body, 0x40, 64) : ""));
    d.check(a.cheats_allowed() && b.cheats_allowed(), "allow_cheats reaches both port clients (each applies its own)");
    std::cout << "== 4. In the level: PlayerReport (1) from both, the host's WorldReport (0); the host leaves\n";
    b.send_medius(mid::PlayerReport,
                  request::player_report(key_b, static_cast<std::int32_t>(world), demo_record(1, 42, 15, 4, 3)));
    a.send_medius(mid::PlayerReport,
                  request::player_report(key_a, static_cast<std::int32_t>(world), demo_record(2, 150, 63, 15, 1)));
    WorldReportFields wr;
    wr.world = static_cast<std::int32_t>(world);
    wr.player_count = 2;
    wr.game_name = "Gabe's game\t4\t0\t1\t0";
    wr.world_status = kWorldClosed;
    a.send_medius(mid::WorldReport, request::world_report(key_a, wr));
    a.send_ext(ext::LeaveGame, {});
    a.pump(300);
    b.pump(300);
    a.send_medius(mid::GetAccountStats, request::get_account_stats(key_a, static_cast<std::int32_t>(id_b)));
    const auto gr = a.wait_for(mid::GetAccountStatsResponse, 3000);
    d.check(gr && get_i32(gr->body, kStatusOffset) == 0 && get_i32(gr->body, 0x48 + 0x10) == 42,
            "the joiner's in-game record is the server's: " +
                (gr ? describe_stats(std::span<const std::uint8_t>(gr->body).subspan(0x48, kStatsBlobSize)) : std::string()));
    std::cout << "  (the server log's 'result:' lines list each player's change in the finished game)\n";
    std::cout << "== 5. Contacts (the original's buddy flows, ONLINE.md §3.7); PORT-ONLY: recent players, chat filter\n";
    a.send_medius(mid::AddToBuddyListConfirmation, request::buddy_add(key_a, static_cast<std::int32_t>(id_b)));
    const auto ab = a.wait_for(mid::AddToBuddyListConfirmationResponse, 3000);
    d.check(ab && get_i32(ab->body, kStatusOffset) == 0, "AddToBuddyListConfirmation (258 -> 68): " + host_name + " invites " +
                                                             join_name + ", status 0");
    const auto fwd = b.wait_for(mid::AddToBuddyListFwdConfirmation, 3000);
    d.check(fwd && static_cast<std::uint32_t>(get_i32(fwd->body, 0x18)) == id_a && get_str(fwd->body, 0x1c, 32) == host_name,
            "AddToBuddyListFwdConfirmation (259) to the online invitee: from '" +
                (fwd ? get_str(fwd->body, 0x1c, 32) : std::string()) + "' (NETMESS: a type 1 message)");
    b.send_medius(mid::GetBuddyInvitations, request::buddy_invitations());
    const auto inv = b.wait_for_list(mid::GetBuddyInvitationsResponse, 0x44, 3000);
    bool listed = false;
    for (const auto& f : inv)
        if (get_i32(f.body, kStatusOffset) == 0 && get_str(f.body, 0x20, 32) == host_name) listed = true;
    d.check(!inv.empty() && !listed && get_i32(inv.back().body, kStatusOffset) == status::NoResult,
            "GetBuddyInvitations (261 -> 262): NoResult, the forwarded invitation is not listed twice");
    b.send_medius(mid::AddToBuddyListFwdConfirmationResponse, request::buddy_answer(key_b, static_cast<std::int32_t>(id_a), true));
    b.pump(200);
    a.pump(300);
    bool told = false;
    for (const auto& n : a.take_notices())
        if (n.text.find("accepted your contact invitation") != std::string::npos) told = true;
    d.check(told, "AddToBuddyListFwdConfirmationResponse (260, accept): the inviter told (PORT-ONLY notice)");
    a.send_medius(mid::GetBuddyListExtraInfo, request::buddy_list());
    const auto bl = a.wait_for_list(mid::GetBuddyListExtraInfoResponse, 0xcc, 3000);
    std::string row;
    for (const auto& f : bl)
        if (get_i32(f.body, kStatusOffset) == 0 && get_str(f.body, 0x20, 32) == join_name)
            row = join_name + " state " + std::to_string(get_i32(f.body, 0x40)) + " (2 lobby, 3 in a game)";
    d.check(!row.empty(), "GetBuddyListExtraInfo (214 -> 215): " + (row.empty() ? std::string("no row") : row));
    a.send_ext(ext::RecentPlayers, {});
    a.pump(300);
    const auto recent = a.take_recent_players();
    bool met = false;
    if (recent)
        for (const auto& r : *recent)
            if (r.name == join_name) met = true;
    d.check(met, "ext RecentPlayers -> RecentPlayersList (PORT-ONLY): " + host_name + " played with " + join_name);
    a.send_ext(ext::ChatFilter, ExtWriter().u8(1).bytes());
    a.pump(100);
    b.send_medius(mid::ChatMessage, request::chat(key_b, "that was a shit landing"));
    std::string seen_text;  // the joiner's line (the host's own earlier chat may still be queued)
    for (int n = 0; n < 4 && seen_text.find("landing") == std::string::npos; ++n) {
        const auto f = a.wait_for(mid::ChatFwdMessage, 3000);
        if (!f) break;
        seen_text = get_str(f->body, 0x40, 64);
    }
    d.check(seen_text.find('*') != std::string::npos && seen_text.find("shit") == std::string::npos,
            "ext ChatFilter on (PORT-ONLY): " + host_name + " reads '" + seen_text + "'");
    std::cout << "== demo " << (d.failures ? "FAILED" : "passed") << " (" << d.failures << " failure(s))\n";
    return d.failures ? 1 : 0;
}

int run_login(int argc, char** argv) {
    if (argc < 5) return usage();
    const bool original = argc >= 6 && std::string(argv[5]) == "--original";
    OnlineClient c;
    std::string error;
    if (!c.connect(argv[2], &error)) {
        std::cerr << "omega_server login: " << error << "\n";
        return 1;
    }
    if (!original) c.hello(this_platform(), 1, cap::All);
    std::cout << "client kind: " << (original ? "original (no hello)" : "port (hello, caps " + std::to_string(cap::All) + ")")
              << "\n";
    c.send_medius(mid::SessionBegin, request::session_begin("sb"));
    const auto sb = c.wait_for(mid::SessionBeginResponse, 3000);
    if (!sb) {
        std::cerr << "omega_server login: no SessionBegin response\n";
        return 1;
    }
    const std::string key = get_str(sb->body, kSessionKeyOffset, kSessionKeyLength);
    c.send_medius(mid::GetPolicy, request::policy(key, false));
    std::cout << "usage policy page:\n  " << policy_text(c.wait_for_list(mid::GetPolicyResponse, 0x11c, 3000)) << "\n  [Accept]\n";
    c.send_medius(mid::GetPolicy, request::policy(key, true));
    std::cout << "privacy policy page:\n  " << policy_text(c.wait_for_list(mid::GetPolicyResponse, 0x11c, 3000))
              << "\n  [Accept]\n";
    c.send_medius(mid::GetAnnouncements, request::announcements(key));
    for (const auto& f : c.wait_for_list(mid::GetAnnouncementsResponse, 0x408, 3000))
        if (get_i32(f.body, kStatusOffset) == status::Success)
            std::cout << "announcement " << get_i32(f.body, 0x1c) << ": " << get_str(f.body, 0x20, 1000) << "\n";
    c.send_medius(mid::AccountLogin, request::login(key, argv[3], argv[4]));
    const auto lr = c.wait_for(mid::AccountLoginResponse, 3000);
    c.pump(200);
    const std::int32_t st = lr ? get_i32(lr->body, kStatusOffset) : -1;
    std::cout << "login (7 -> 8) '" << argv[3] << "': status " << st;
    if (st == status::Success) std::cout << ", account id " << get_i32(lr->body, 0x1c);
    std::cout << "\n";
    for (const auto& n : c.take_notices()) std::cout << "notice " << n.code << ": " << n.text << "\n";
    std::cout << "extension frames received: " << c.ext_frames_received() << "\n";
    return st == status::Success ? 0 : 1;
}

}  // namespace

int run_load(int argc, char** argv);  // omega_server_load.cpp

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "load") return run_load(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "admin") return run_admin(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "demo") return run_demo(argc, argv);
    if (argc >= 2 && std::string(argv[1]) == "login") return run_login(argc, argv);
    if (argc >= 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) return usage();
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--service") {
#ifdef _WIN32
            return run_service(argc, argv);
#else
            std::cerr << "omega_server: --service is for Windows; use the systemd unit (tools/deploy/omega_server.service)\n";
            return 2;
#endif
        }
    return run_server(argc, argv, false);
}
