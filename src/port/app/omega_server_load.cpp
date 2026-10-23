// PORT-ONLY (not in the original; checkpoint 132on2h, research/ONLINE_DEPLOY.md §12): omega_server's load test and
// anti-abuse check. `omega_server load HOST[:PORT] --clients N` runs N headless clients, one thread each, through what
// the game does online: connect, the PORT-ONLY hello (every `--original-every`th client sends none: an original-kind
// session, which must never see an extension frame), session begin, the usage / privacy policy, the announcements,
// account creation and login, the lobby (player list, chat, game list), create / join (one host per `--group`
// clients), the in-level UDP relay (each joiner sends `--relay-rate` datagrams a second for `--relay-seconds` to its
// host's virtual address, the host echoes them; the round trip is measured), leave, logout and disconnect. It prints
// each request's latency (p50 / p99 / max) and, with --token, the server's `status` (tick work, CPU, memory).
//
// `omega_server load HOST[:PORT] --abuse` instead checks the anti-abuse limits against a server running its default
// limits (ONLINE.md §6.16): a login hammer is refused, a chat flood is dropped then disconnected, and repeated frame
// floods get the address blocked.

#include "online_client.hpp"
#include "online_protocol.hpp"
#include "online_relay.hpp"
#include "online_tcp.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <sys/select.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace omega::online;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// All threads meet here between the phases (each thread arrives even after a failure, so nobody waits forever).
class Barrier {
public:
    explicit Barrier(std::size_t n) : n_(n) {}
    void wait() {
        std::unique_lock<std::mutex> lock(m_);
        const std::size_t gen = gen_;
        if (++count_ == n_) {
            count_ = 0;
            ++gen_;
            cv_.notify_all();
            return;
        }
        cv_.wait(lock, [&] { return gen != gen_; });
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::size_t n_, count_ = 0, gen_ = 0;
};

struct Results {
    std::mutex m;
    std::map<std::string, std::vector<double>> latency;  // ms per operation
    std::map<std::string, int> failures;
    std::atomic<int> ext_on_original{0};
    void add(const std::string& op, double ms) {
        std::lock_guard<std::mutex> lock(m);
        latency[op].push_back(ms);
    }
    void fail(const std::string& what) {
        std::lock_guard<std::mutex> lock(m);
        ++failures[what];
    }
};

struct Options {
    std::string server;
    int clients = 64;
    int group = 4;
    int chat = 3;
    int relay_seconds = 5;
    int relay_rate = 30;
    int original_every = 8;
    std::string prefix = "Load";
    std::string password = "load-pass-1";
    std::string token;
};

bool wait_readable(const UdpSocket& s, int timeout_ms) {
    fd_set set;
    FD_ZERO(&set);
#ifdef _WIN32
    FD_SET(static_cast<SOCKET>(s.native()), &set);
#else
    FD_SET(static_cast<int>(s.native()), &set);
#endif
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    return ::select(static_cast<int>(s.native() + 1), &set, nullptr, nullptr, &tv) > 0;
}

// One request / response, timed.
template <typename F>
std::optional<Frame> timed(Results& r, const std::string& op, OnlineClient& c, std::uint16_t reply, F&& send_it) {
    const auto t = Clock::now();
    send_it();
    auto f = c.wait_for(reply, 10000);
    if (f) r.add(op, ms_since(t));
    else r.fail(op + ": no response");
    return f;
}

struct Shared {
    std::vector<std::uint32_t> worlds;  // by host index
    std::vector<std::uint32_t> host_vips;
};

void run_client(int idx, const Options& o, Barrier& barrier, Results& r, Shared& shared) {
    const bool original = o.original_every > 0 && idx % o.original_every == o.original_every - 1;
    const bool host = idx % o.group == 0;
    const int host_idx = idx - idx % o.group;
    const std::string name = o.prefix + std::to_string(idx);
    OnlineClient c;
    bool ok = true;
    std::string key;
    std::uint32_t world = 0;

    // Phase 1: connect, hello, sign-up and login (the ONLINECN / ACNTNEW / ACNTLOGN sequence).
    {
        const auto t = Clock::now();
        std::string error;
        ok = c.connect(o.server, &error, 10000);
        if (ok) r.add("connect", ms_since(t));
        else r.fail("connect: " + error);
        if (ok && !original) c.hello(Platform::Pc, 1, cap::All);
        if (ok) {
            const auto sb = timed(r, "session_begin", c, mid::SessionBeginResponse,
                                  [&] { c.send_medius(mid::SessionBegin, request::session_begin("sb")); });
            ok = sb.has_value();
            if (ok) key = get_str(sb->body, kSessionKeyOffset, kSessionKeyLength);
        }
        if (ok) {
            const auto t2 = Clock::now();
            c.send_medius(mid::GetPolicy, request::policy(key, false));
            ok = !c.wait_for_list(mid::GetPolicyResponse, 0x11c, 10000).empty();
            c.send_medius(mid::GetPolicy, request::policy(key, true));
            ok = ok && !c.wait_for_list(mid::GetPolicyResponse, 0x11c, 10000).empty();
            c.send_medius(mid::GetAnnouncements, request::announcements(key));
            ok = ok && !c.wait_for_list(mid::GetAnnouncementsResponse, 0x408, 10000).empty();
            if (ok) r.add("policy+announcements", ms_since(t2));
            else r.fail("policy / announcements");
        }
        if (ok) {
            const auto reg = timed(r, "account_create", c, mid::AccountRegistrationResponse,
                                   [&] { c.send_medius(mid::AccountRegistration, request::registration(key, name, o.password)); });
            const std::int32_t st = reg ? get_i32(reg->body, kStatusOffset) : -1;
            if (reg && st != status::Success && st != status::AccountAlreadyExists) r.fail("account_create status " + std::to_string(st));
        }
        if (ok) {
            const auto lr = timed(r, "login", c, mid::AccountLoginResponse,
                                  [&] { c.send_medius(mid::AccountLogin, request::login(key, name, o.password)); });
            const std::int32_t st = lr ? get_i32(lr->body, kStatusOffset) : -1;
            ok = st == status::Success;
            if (lr && !ok) r.fail("login status " + std::to_string(st));
        }
    }
    barrier.wait();

    // Phase 2: the lobby (CMDCENTR / NETMESS): the player list, chat, the game list.
    if (ok) {
        const auto t = Clock::now();
        c.send_medius(mid::PlayerList, request::player_list(0));
        const auto rows = c.wait_for_list(mid::PlayerListResponse, 0xcc, 10000);
        if (rows.empty()) r.fail("player_list: no response");
        else r.add("player_list", ms_since(t));
        for (int i = 0; i < o.chat; ++i) {
            c.send_medius(mid::ChatMessage, request::chat(key, name + " says hello " + std::to_string(i)));
            timed(r, "text_filter", c, mid::TextFilterResponse,
                  [&] { c.send_medius(mid::TextFilter, request::text_filter(key, "hello " + std::to_string(i), 0)); });
        }
        const auto t2 = Clock::now();
        c.send_medius(mid::GameList, request::game_list(key));
        if (c.wait_for_list(mid::GameListResponse, 0x6c, 10000).empty()) r.fail("game_list: no response");
        else r.add("game_list", ms_since(t2));
        c.take_queued();  // the others' chat
    }
    barrier.wait();

    // Phase 3: the hosts create their games (MSNSETUP) ...
    if (ok && host) {
        const auto cr = timed(r, "create_game", c, mid::CreateGameResponse, [&] {
            c.send_medius(mid::CreateGame, request::create_game(key, o.prefix + "Game" + std::to_string(idx) + "\t4\t0\t1\t0"));
        });
        ok = cr && get_i32(cr->body, kStatusOffset) == status::Success;
        if (ok) {
            world = static_cast<std::uint32_t>(get_i32(cr->body, 0x1c));
            if (!original) c.send_ext(ext::HostEndpoint, ExtWriter().u32(world).u16(relay::kVirtualPort).bytes());
            shared.worlds[static_cast<std::size_t>(idx / o.group)] = world;
        } else {
            r.fail("create_game");
        }
    }
    barrier.wait();
    // ... and the others find theirs in the game list and join it (JOINMIS).
    if (ok && !host) {
        const std::uint32_t want = shared.worlds[static_cast<std::size_t>(host_idx / o.group)];
        c.send_medius(mid::GameList, request::game_list(key));
        bool listed = false;
        for (const auto& f : c.wait_for_list(mid::GameListResponse, 0x6c, 10000))
            if (get_i32(f.body, kStatusOffset) == status::Success && static_cast<std::uint32_t>(get_i32(f.body, 0x1c)) == want)
                listed = true;
        if (!listed) r.fail("join: the host's game is not in the game list");
        const auto jr = timed(r, "join_game", c, mid::JoinGameResponse,
                              [&] { c.send_medius(mid::JoinGame, request::join_game(key, want)); });
        ok = want && jr && get_i32(jr->body, kStatusOffset) == status::Success;
        if (ok) world = want;
        else r.fail("join_game");
    }
    c.pump(50);
    barrier.wait();

    // Phase 4: the in-level traffic through the relay (port sessions with a relay ticket).
    UdpSocket udp;
    std::uint32_t own_vip = 0;
    const bool relaying = ok && !original && c.relay_ticket().has_value();
    if (ok && !original && !c.relay_ticket()) r.fail("relay: no RelayTicket");
    if (relaying) {
        std::string error;
        if (!udp.open(0, 0, &error)) {
            r.fail("relay: udp open " + error);
        } else {
            RelayPacket reg;
            reg.kind = relay::Kind::Register;
            reg.token = c.relay_ticket()->token;
            const auto t = Clock::now();
            bool registered = false;
            for (int attempt = 0; attempt < 5 && !registered; ++attempt) {
                udp.send_to(c.server_ip(), c.relay_ticket()->port, encode_relay(reg));
                while (wait_readable(udp, 500)) {
                    const auto d = udp.receive();
                    if (!d) break;
                    const auto p = decode_relay(d->bytes);
                    if (p && p->kind == relay::Kind::Registered) {
                        registered = true;
                        own_vip = p->vip;
                        break;
                    }
                }
            }
            if (registered) r.add("relay_register", ms_since(t));
            else r.fail("relay: no Registered");
        }
    }
    barrier.wait();
    if (relaying && own_vip) {
        const auto end = Clock::now() + std::chrono::seconds(o.relay_seconds) + (host ? std::chrono::seconds(2) : std::chrono::seconds(0));
        std::optional<RelayPeer> peer = host ? std::nullopt : c.relay_peer(world);
        if (!host && !peer) r.fail("relay: no RelayPeer for the joined world");
        const auto step = std::chrono::microseconds(1000000 / std::max(1, o.relay_rate));
        auto next_send = Clock::now();
        std::uint32_t seq = 0, echoed = 0;
        std::map<std::uint32_t, Clock::time_point> sent_at;
        while (Clock::now() < end) {
            if (!host && peer && Clock::now() >= next_send && Clock::now() + std::chrono::milliseconds(500) < end) {
                RelayPacket d;
                d.kind = relay::Kind::Data;
                d.token = c.relay_ticket()->token;
                d.vip = peer->vip;
                d.payload.assign(200, 0);  // a typical LanTransport state datagram's size
                std::memcpy(d.payload.data(), &seq, sizeof seq);
                sent_at[seq++] = Clock::now();
                udp.send_to(c.server_ip(), c.relay_ticket()->port, encode_relay(d));
                next_send += step;
            }
            if (!wait_readable(udp, 2)) continue;
            while (auto dg = udp.receive()) {
                const auto p = decode_relay(dg->bytes);
                if (!p || p->kind != relay::Kind::DataFrom) continue;
                if (host) {  // the host echoes each datagram to its sender
                    RelayPacket back;
                    back.kind = relay::Kind::Data;
                    back.token = c.relay_ticket()->token;
                    back.vip = p->vip;
                    back.payload = p->payload;
                    udp.send_to(c.server_ip(), c.relay_ticket()->port, encode_relay(back));
                } else if (p->payload.size() >= 4) {
                    std::uint32_t s = 0;
                    std::memcpy(&s, p->payload.data(), sizeof s);
                    if (auto it = sent_at.find(s); it != sent_at.end()) {
                        r.add("relay_round_trip", ms_since(it->second));
                        sent_at.erase(it);
                        ++echoed;
                    }
                }
            }
        }
        if (!host) {
            std::lock_guard<std::mutex> lock(r.m);
            r.latency["relay_sent"].push_back(seq);
            r.latency["relay_echoed"].push_back(echoed);
        }
    }
    c.pump(20);
    barrier.wait();

    // Phase 5: leave the game, log out, disconnect.
    if (ok) {
        if (!original && world) c.send_ext(ext::LeaveGame, {});
        const auto lo = timed(r, "logout", c, mid::AccountLogoutResponse,
                              [&] { c.send_medius(mid::AccountLogout, request::logout(key)); });
        if (!lo) ok = false;
    }
    c.pump(20);
    if (original && c.ext_frames_received() > 0) {
        r.ext_on_original += static_cast<int>(c.ext_frames_received());
        r.fail("an original-kind session received " + std::to_string(c.ext_frames_received()) + " extension frame(s)");
    }
    c.disconnect();
}

std::string admin_text(const std::string& server, const std::string& token, const std::string& command) {
    OnlineClient c;
    std::string error;
    if (!c.connect(server, &error)) return "(admin: " + error + ")";
    c.hello(Platform::Pc, 1, cap::Admin | cap::Notices);
    c.send_ext(ext::AdminCommand, ExtWriter().str("token " + token + " " + command).bytes());
    const auto reply = c.wait_admin_reply(5000);
    return reply ? *reply : "(admin: no reply)";
}

void print_table(const Results& r) {
    std::printf("  %-22s %7s %10s %10s %10s\n", "operation", "count", "p50 ms", "p99 ms", "max ms");
    for (const auto& [op, v0] : r.latency) {
        if (op == "relay_sent" || op == "relay_echoed") continue;
        auto v = v0;
        std::sort(v.begin(), v.end());
        auto q = [&](double x) { return v[std::min(v.size() - 1, static_cast<std::size_t>(x * static_cast<double>(v.size())))]; };
        std::printf("  %-22s %7zu %10.2f %10.2f %10.2f\n", op.c_str(), v.size(), q(0.5), q(0.99), v.back());
    }
    double sent = 0, echoed = 0;
    if (auto it = r.latency.find("relay_sent"); it != r.latency.end())
        for (double x : it->second) sent += x;
    if (auto it = r.latency.find("relay_echoed"); it != r.latency.end())
        for (double x : it->second) echoed += x;
    std::printf("  relay datagrams: %.0f sent by the joiners, %.0f echoed back by their hosts (%.1f%%)\n", sent, echoed,
                sent > 0 ? 100.0 * echoed / sent : 0.0);
}

int run_abuse(const Options& o) {
    int failures = 0;
    auto check = [&](bool good, const std::string& what) {
        std::cout << (good ? "  ok   " : "  FAIL ") << what << "\n" << std::flush;
        if (!good) ++failures;
    };
    auto sign_up = [&](OnlineClient& c, const std::string& name, std::string& key) {
        std::string error;
        if (!c.connect(o.server, &error)) return false;
        c.hello(Platform::Pc, 1, cap::All);
        c.send_medius(mid::SessionBegin, request::session_begin("sb"));
        const auto sb = c.wait_for(mid::SessionBeginResponse, 5000);
        if (!sb) return false;
        key = get_str(sb->body, kSessionKeyOffset, kSessionKeyLength);
        c.send_medius(mid::GetPolicy, request::policy(key, false));
        c.wait_for_list(mid::GetPolicyResponse, 0x11c, 5000);
        c.send_medius(mid::GetPolicy, request::policy(key, true));
        c.wait_for_list(mid::GetPolicyResponse, 0x11c, 5000);
        c.send_medius(mid::AccountRegistration, request::registration(key, name, o.password));
        c.wait_for(mid::AccountRegistrationResponse, 5000);
        c.send_medius(mid::AccountLogin, request::login(key, name, o.password));
        const auto lr = c.wait_for(mid::AccountLoginResponse, 5000);
        return lr && get_i32(lr->body, kStatusOffset) == status::Success;
    };
    std::cout << "== abuse 1: a login hammer (wrong passwords, then the right one) is refused by the attempt limits\n";
    {
        OnlineClient c;
        std::string key;
        check(sign_up(c, o.prefix + "Victim", key), o.prefix + "Victim signs up and logs in");
        c.send_medius(mid::AccountLogout, request::logout(key));
        c.wait_for(mid::AccountLogoutResponse, 5000);
        int refused_attempts = 0, wrong = 0;
        for (int i = 0; i < 14; ++i) {
            c.send_medius(mid::AccountLogin, request::login(key, o.prefix + "Victim", "guess" + std::to_string(i)));
            const auto lr = c.wait_for(mid::AccountLoginResponse, 5000);
            const std::int32_t st = lr ? get_i32(lr->body, kStatusOffset) : 0;
            if (st == status::InvalidPassword) ++wrong;
            if (st == status::PlayerNotPrivileged) ++refused_attempts;
        }
        bool told = false;
        for (const auto& n : c.take_notices()) told = told || n.text.find("Too many") != std::string::npos;
        check(refused_attempts > 0 && told, "14 rapid logins: " + std::to_string(wrong) + " answered wrong password, " +
                                                std::to_string(refused_attempts) + " refused (PlayerNotPrivileged + notice)");
    }
    std::cout << "== abuse 2: a chat flood is dropped, then the flooder is disconnected\n";
    {
        OnlineClient a, b;
        std::string ka, kb;
        check(sign_up(a, o.prefix + "Flooder", ka) && sign_up(b, o.prefix + "Reader", kb), "two players in the lobby");
        b.take_queued();
        for (int i = 0; i < 40 && a.connected(); ++i) {
            a.send_medius(mid::ChatMessage, request::chat(ka, "spam " + std::to_string(i)));
            a.pump(5);
        }
        a.pump(500);
        b.pump(500);
        int delivered = 0;
        for (const auto& f : b.take_queued())
            if (f.type == mid::ChatFwdMessage && get_str(f.body, 0x40, 64).rfind("spam", 0) == 0) ++delivered;
        bool kicked = false;
        for (const auto& n : a.take_notices()) kicked = kicked || n.text.find("chat flooding") != std::string::npos;
        a.pump(300);
        check(delivered > 0 && delivered < 40, "the reader got " + std::to_string(delivered) + " of 40 messages");
        check(kicked && !a.connected(), "the flooder was told and disconnected");
    }
    std::cout << "== abuse 3: frame floods; the address is blocked after flood_block_after disconnects\n";
    {
        int closed = 0;
        for (int round = 0; round < 3; ++round) {
            OnlineClient f;
            std::string error;
            if (!f.connect(o.server, &error)) break;
            f.hello(Platform::Pc, 1, cap::All);
            for (int i = 0; i < 3000 && f.connected(); ++i) {
                f.send_medius(mid::SessionBegin, request::session_begin("sb"));
                if (i % 200 == 0) f.pump(1);
            }
            for (int i = 0; i < 40 && f.connected(); ++i) f.pump(50);
            if (!f.connected()) ++closed;
        }
        check(closed == 3, std::to_string(closed) + " of 3 flooding connections closed by message_rate / message_burst");
        OnlineClient late;
        std::string error;
        bool refused = !late.connect(o.server, &error);
        if (!refused) {
            late.hello(Platform::Pc, 1, cap::All);
            for (int i = 0; i < 20 && late.connected(); ++i) late.pump(50);
            refused = !late.connected();
        }
        check(refused, "a new connection from the blocked address is closed at once");
    }
    std::cout << "== abuse " << (failures ? "FAILED" : "passed") << " (" << failures << " failure(s))\n";
    return failures ? 1 : 0;
}

}  // namespace

int run_load(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: omega_server load HOST[:PORT] [--clients N] [--group N] [--chat N] [--relay-seconds S]\n"
                     "                         [--relay-rate R] [--original-every K] [--prefix NAME] [--token TOKEN]\n"
                     "       omega_server load HOST[:PORT] --abuse [--prefix NAME]\n";
        return 2;
    }
    Options o;
    o.server = argv[2];
    bool abuse = false;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        auto next_int = [&](int& into) {
            if (i + 1 < argc) into = std::max(0, std::atoi(argv[++i]));
        };
        if (a == "--clients") next_int(o.clients);
        else if (a == "--group") next_int(o.group);
        else if (a == "--chat") next_int(o.chat);
        else if (a == "--relay-seconds") next_int(o.relay_seconds);
        else if (a == "--relay-rate") next_int(o.relay_rate);
        else if (a == "--original-every") next_int(o.original_every);
        else if (a == "--prefix" && i + 1 < argc) o.prefix = argv[++i];
        else if (a == "--token" && i + 1 < argc) o.token = argv[++i];
        else if (a == "--abuse") abuse = true;
    }
    if (abuse) return run_abuse(o);
    o.clients = std::max(1, o.clients);
    o.group = std::clamp(o.group, 1, 4);
    std::cout << "== load: " << o.clients << " clients against " << o.server << " (games of " << o.group << ", "
              << o.chat << " chat message(s) each, relay " << o.relay_rate << "/s for " << o.relay_seconds
              << " s per joiner, every " << o.original_every << "th client original-kind)\n" << std::flush;
    Results r;
    Shared shared;
    shared.worlds.assign(static_cast<std::size_t>((o.clients + o.group - 1) / o.group), 0);
    Barrier barrier(static_cast<std::size_t>(o.clients));
    const auto start = Clock::now();
    std::vector<std::thread> threads;
    for (int i = 0; i < o.clients; ++i) threads.emplace_back(run_client, i, std::cref(o), std::ref(barrier), std::ref(r), std::ref(shared));
    for (auto& t : threads) t.join();
    const double seconds = ms_since(start) / 1000.0;
    std::printf("== load: %d clients done in %.1f s\n", o.clients, seconds);
    print_table(r);
    int failures = 0;
    for (const auto& [what, n] : r.failures) {
        std::printf("  FAIL %s (x%d)\n", what.c_str(), n);
        failures += n;
    }
    std::printf("  original-kind sessions: extension frames received %d (must be 0)\n", r.ext_on_original.load());
    if (!o.token.empty()) std::cout << "== server status after the run\n" << admin_text(o.server, o.token, "status") << "\n";
    std::cout << "== load " << (failures ? "FAILED" : "passed") << " (" << failures << " failure(s))\n" << std::flush;
    return failures ? 1 : 0;
}
