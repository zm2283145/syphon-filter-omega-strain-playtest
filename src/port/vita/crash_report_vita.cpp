// Port-only (checkpoint 132vt2, research/VITA.md "Errors on screen"): the crash reporter's PS Vita side (the API of
// crash_report.hpp; its pure helpers are shared from src/port/platform/crash_report.cpp). The Windows reporter's
// minidumps, SEH filters and stack walks have no Vita counterpart in a user app: a fault is the system's psp2core dump
// (ux0:data/psp2core-*.psp2dmp, read with vita-parse-core and the ELF), and a C++ exception that reaches play_main's
// top-level catch writes the same text report as on the PC (format_report: the message, the EE addresses it names,
// the last log lines) into the report folder. The stall watchdog (StallWatch) runs as on the PC on a plain thread.

#include "crash_report.hpp"
#include "crash_report_vita.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <streambuf>
#include <thread>
#include <typeinfo>

namespace omega::crash {
namespace fs = std::filesystem;

namespace {

class TeeBuf : public std::streambuf {
public:
    TeeBuf(std::streambuf* out, LineRing* ring, std::mutex* mutex) : out_(out), ring_(ring), mutex_(mutex) {}

protected:
    int overflow(int c) override {
        if (c == traits_type::eof()) return traits_type::not_eof(c);
        const char ch = char(c);
        {
            std::lock_guard lock(*mutex_);
            ring_->write(&ch, 1);
        }
        return out_->sputc(ch) == traits_type::eof() ? traits_type::eof() : c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        {
            std::lock_guard lock(*mutex_);
            ring_->write(s, std::size_t(n));
        }
        return out_->sputn(s, n);
    }
    int sync() override { return out_->pubsync(); }

private:
    std::streambuf* out_;
    LineRing* ring_;
    std::mutex* mutex_;
};

struct State {
    std::mutex mutex;  // settings, last message
    std::mutex log_mutex;
    Settings settings;
    std::unique_ptr<LineRing> ring;
    bool installed = false;
    std::atomic<bool> reporting{false};
    std::string last_message;
};
State& state() {
    static State* s = new State;  // never destroyed, as on the PC
    return *s;
}

void log_line(const std::string& line) {
    std::cerr << line << "\n";
    std::cerr.flush();
}

std::optional<Written> write_report(ReportData data, const fs::path& folder, const std::vector<std::string>& extra) {
    auto& s = state();
    if (s.reporting.exchange(true)) return std::nullopt;
    std::cout.flush();
    std::cerr.flush();
    std::optional<Written> result;
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (fs::is_directory(folder, ec)) {
        const std::time_t now = std::time(nullptr);
        std::tm t{};
        localtime_r(&now, &t);
        const auto stem0 = file_stem(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, 0);
        std::string stem = stem0;
        for (int k = 2; fs::exists(folder / (stem + ".txt"), ec); ++k) stem = stem0 + "-" + std::to_string(k);
        char time_text[32];
        std::strftime(time_text, sizeof time_text, "%Y-%m-%d %H:%M:%S", &t);
        data.time = time_text;
        data.executable = "app0:eboot.bin (PS Vita, SFOS00001)";
        data.command_line = "(the PS Vita app's play arguments: omega_vita.log)";
        data.dump_file = "not written (on the PS Vita a fault is the system's psp2core dump in ux0:data)";
        {
            std::lock_guard lock(s.log_mutex);
            if (s.ring) data.log = s.ring->lines();
        }
        Written w;
        w.text = folder / (stem + ".txt");
        std::ofstream out(w.text, std::ios::binary);
        out << format_report(data);
        for (const auto& line : extra) out << line << "\n";
        out.close();
        if (out) {
            w.summary = data.kind;
            result = w;
            log_line("Crash reporter (port-only): wrote " + w.text.string() + " (" + w.summary + ")");
        }
    }
    s.reporting = false;
    return result;
}

std::atomic<bool> g_watchdog_started{false};

struct Watch {
    std::uint64_t id;
    std::string name;
    double seconds;
    std::function<std::int64_t()> progress;
    std::function<std::string()> describe;
    std::int64_t last = 0;
    std::chrono::steady_clock::time_point since;
    bool reported = false;
};
struct Shutdown {  // port-only (132ws8): arm_shutdown's phase
    bool armed = false;
    std::string phase;
    int exit_code = 0;
    double seconds = 0;
    std::chrono::steady_clock::time_point since;
};
struct Watchdog {
    std::mutex mutex;
    std::vector<Watch> watches;
    std::uint64_t next_id = 1;
    fs::path fallback = "ux0:data/omega_strain/stalls";
    Shutdown shutdown;
};
Watchdog& watchdog() {
    static Watchdog* w = new Watchdog;
    return *w;
}

void watchdog_loop() {
    auto& w = watchdog();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::vector<std::pair<std::string, std::string>> stalls;
        {
            std::lock_guard lock(w.mutex);
            const auto now = std::chrono::steady_clock::now();
            for (auto& watch : w.watches) {
                const auto p = watch.progress();
                if (p != watch.last) {
                    watch.last = p;
                    watch.since = now;
                    continue;
                }
                const double idle = std::chrono::duration<double>(now - watch.since).count();
                if (watch.reported || idle < watch.seconds) continue;
                watch.reported = true;
                char head[160];
                std::snprintf(head, sizeof head, ": no progress for %.1f s (progress %lld); ", idle,
                              static_cast<long long>(p));
                stalls.emplace_back(watch.name, watch.name + head + (watch.describe ? watch.describe() : std::string()));
            }
        }
        for (const auto& [name, message] : stalls) {
            log_line("Stall watchdog (port-only, 132mt3): " + message);
            report_stall("the stall watchdog (crash_report.hpp StallWatch: " + name + ")", message, w.fallback);
        }
        // Port-only (132ws8): a shutdown phase past its bound ends the process (arm_shutdown), as on the PC.
        std::optional<Shutdown> late;
        {
            std::lock_guard lock(w.mutex);
            const auto now = std::chrono::steady_clock::now();
            if (w.shutdown.armed && std::chrono::duration<double>(now - w.shutdown.since).count() >= w.shutdown.seconds) {
                late = w.shutdown;
                w.shutdown.armed = false;
            }
        }
        if (late) {
            const std::string message = "the shutdown phase \"" + late->phase + "\" did not end within " +
                                        std::to_string(int(late->seconds)) + " s; the process ends with exit code " +
                                        std::to_string(late->exit_code) + " (the saves were written before the teardown)";
            log_line("Shutdown watchdog (port-only, 132ws8): " + message);
            report_stall("the shutdown watchdog (crash_report.hpp arm_shutdown)", message, w.fallback);
            std::cout.flush();
            std::cerr.flush();
            std::fflush(stdout);
            std::fflush(stderr);
            std::_Exit(late->exit_code);
        }
    }
}

}  // namespace

void install(const Settings& settings) {
    auto& s = state();
    {
        std::lock_guard lock(s.mutex);
        s.settings = settings;
        if (s.installed) return;
        s.installed = true;
    }
    {
        std::lock_guard lock(s.log_mutex);
        s.ring = std::make_unique<LineRing>(settings.log_lines);
    }
    static TeeBuf* out = new TeeBuf(std::cout.rdbuf(), s.ring.get(), &s.log_mutex);
    static TeeBuf* err = new TeeBuf(std::cerr.rdbuf(), s.ring.get(), &s.log_mutex);
    std::cout.rdbuf(out);
    std::cerr.rdbuf(err);
}

bool installed() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.installed;
}

void set_dialogs(bool dialogs) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.settings.dialogs = dialogs;
}

std::optional<Written> report_exception(const std::exception& e, std::string_view where) {
    if (!installed()) return std::nullopt;
    ReportData data;
    data.where = std::string(where);
    data.message = e.what();
    data.type = typeid(e).name();
    data.kind = is_unported(data.message) ? "unported branch (C++ exception)" : "C++ exception";
    fs::path folder;
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        folder = s.settings.folder;
    }
    return write_report(std::move(data), folder, {});
}

void show_message(const Written& w, std::string_view message) {
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        s.last_message = std::string(message) + "\n\nA crash report was written: " + w.text.string();
    }
    log_line("Crash reporter (port-only): " + std::string(message) + " (report " + w.text.string() + ")");
}

std::optional<Written> report_stall(std::string_view where, std::string_view message, const fs::path& fallback_folder) {
    ReportData data;
    data.where = std::string(where);
    data.kind = "stall (port-only watchdog, 132mt3)";
    data.type = "stall";
    data.message = std::string(message);
    fs::path folder = fallback_folder;
    if (installed()) {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        folder = s.settings.folder;
    }
    return write_report(std::move(data), folder, thread_report());
}

std::vector<std::string> thread_report() {
    return {"Threads: the per-thread scheduler states are the Windows reporter's (the Vita's busy times are in "
            "omega_vita.log)"};
}

void start_stall_watchdog() {
    if (g_watchdog_started.exchange(true)) return;
    (void)watchdog();
    std::thread(watchdog_loop).detach();
}

StallWatch::StallWatch(std::string name, double seconds, std::function<std::int64_t()> progress,
                       std::function<std::string()> describe) {
    start_stall_watchdog();
    auto& w = watchdog();
    std::lock_guard lock(w.mutex);
    id_ = w.next_id++;
    Watch watch{id_, std::move(name), seconds, std::move(progress), std::move(describe)};
    watch.last = watch.progress();
    watch.since = std::chrono::steady_clock::now();
    w.watches.push_back(std::move(watch));
}

StallWatch::~StallWatch() {
    auto& w = watchdog();
    std::lock_guard lock(w.mutex);
    std::erase_if(w.watches, [&](const Watch& watch) { return watch.id == id_; });
}

void arm_shutdown(std::string phase, int exit_code, double seconds) {
    start_stall_watchdog();
    auto& w = watchdog();
    std::lock_guard lock(w.mutex);
    w.shutdown = {true, std::move(phase), exit_code, seconds, std::chrono::steady_clock::now()};
}

void disarm_shutdown() {
    auto& w = watchdog();
    std::lock_guard lock(w.mutex);
    w.shutdown.armed = false;
}

int raise_idle_threads() { return 0; }  // the PC's driver threads only

bool test_crash_kind(std::string_view kind) {
    return kind == "throw" || kind == "unported" || kind == "access-violation" || kind == "terminate" ||
           kind == "invalid-parameter";
}

void test_crash(std::string_view kind, std::string_view where) {
    std::cout << "Crash reporter (port-only test): forcing a " << kind << " in " << where << "\n";
    std::cout.flush();
    if (kind == "throw")
        throw std::runtime_error("Simulated failure in " + std::string(where) + " (--test-crash throw, port-only test)");
    if (kind == "unported")
        throw std::runtime_error("Lift trigger action 10 (00214920) is not ported (simulated by --test-crash in " +
                                 std::string(where) + ", port-only test)");
    if (kind == "access-violation") {
        volatile int* p = nullptr;
        *p = 1;
    }
    if (kind == "invalid-parameter")
        throw std::runtime_error("--test-crash invalid-parameter is the Windows C runtime's handler (not on the PS Vita)");
    if (kind == "terminate") {
        std::thread([where = std::string(where)] {
            throw std::runtime_error("Simulated exception escaping a thread in " + where +
                                     " (--test-crash terminate, port-only test)");
        }).join();
    }
    throw std::runtime_error("--test-crash: unknown kind " + std::string(kind));
}

std::string vita_last_message() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.last_message;
}

}  // namespace omega::crash
