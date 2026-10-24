#include "crash_report.hpp"

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <io.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <thread>
#include <typeinfo>

namespace omega::crash {
namespace fs = std::filesystem;

// --- pure helpers ---

namespace {
bool hex_digit(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }
std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
std::string hex(std::uint64_t value, int digits) {
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%0*llx", digits, static_cast<unsigned long long>(value));
    return buffer;
}
}  // namespace

std::vector<std::string> ee_addresses(std::string_view m) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < m.size()) {
        if (!hex_digit(m[i])) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < m.size() && hex_digit(m[j])) ++j;
        const bool after_0x = i >= 2 && m[i - 1] == 'x' && m[i - 2] == '0';
        const bool word_start = i == 0 || !std::isalnum(static_cast<unsigned char>(m[i - 1])) || after_0x;
        const bool word_end = j == m.size() || !std::isalnum(static_cast<unsigned char>(m[j]));
        if (j - i == 8 && word_start && word_end && !after_0x) {
            const std::string word = lower(m.substr(i, 8));
            const auto value = std::stoull(word, nullptr, 16);
            if (value >= 0x00080000 && value < 0x02000000 && std::find(out.begin(), out.end(), word) == out.end())
                out.push_back(word);
        }
        i = j;
    }
    return out;
}

bool is_unported(std::string_view message) {
    const auto m = lower(message);
    return m.find("not ported") != std::string::npos || m.find("unported") != std::string::npos;
}

LineRing::LineRing(std::size_t capacity) : capacity_(std::max<std::size_t>(capacity, 1)), ring_(capacity_) {}

void LineRing::write(const char* data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        const char c = data[i];
        if (c == '\n') {
            if (!partial_.empty() && partial_.back() == '\r') partial_.pop_back();
            ring_[next_] = std::move(partial_);
            partial_.clear();
            next_ = (next_ + 1) % capacity_;
            count_ = std::min(count_ + 1, capacity_);
        } else if (partial_.size() < 4096) {
            partial_.push_back(c);
        }
    }
}

std::vector<std::string> LineRing::lines() const {
    std::vector<std::string> out;
    out.reserve(count_ + 1);
    for (std::size_t k = 0; k < count_; ++k) out.push_back(ring_[(next_ + capacity_ - count_ + k) % capacity_]);
    if (!partial_.empty()) out.push_back(partial_);
    return out;
}

SymbolMap SymbolMap::parse(std::string_view text) {
    SymbolMap map;
    std::uint64_t preferred = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        constexpr std::string_view load = "Preferred load address is ";
        if (const auto p = line.find(load); p != std::string_view::npos) {
            preferred = std::stoull(std::string(line.substr(p + load.size())), nullptr, 16);
            continue;
        }
        if (line.find("entry point at") != std::string_view::npos) continue;
        // " 0001:00000000       ?name@@YAXXZ          0000000140001000 f   object.obj"
        std::istringstream in{std::string(line)};
        std::string section, name, base;
        if (!(in >> section >> name >> base)) continue;
        if (section.size() != 13 || section[4] != ':' || base.size() != 16) continue;
        if (!std::all_of(base.begin(), base.end(), hex_digit)) continue;
        const auto value = std::stoull(base, nullptr, 16);
        if (preferred == 0 || value <= preferred) continue;
        map.symbols_.push_back({value - preferred, name});
    }
    std::stable_sort(map.symbols_.begin(), map.symbols_.end(), [](const Symbol& a, const Symbol& b) { return a.rva < b.rva; });
    return map;
}

std::string SymbolMap::name_for(std::uint64_t rva) const {
    auto it = std::upper_bound(symbols_.begin(), symbols_.end(), rva, [](std::uint64_t v, const Symbol& s) { return v < s.rva; });
    if (it == symbols_.begin()) return {};
    --it;
    return it->name + "+0x" + hex(rva - it->rva, 1);
}

std::string format_frame(const Frame& f) {
    std::string s = "0x" + hex(f.address, 16);
    if (!f.module.empty()) s += "  " + f.module + "+0x" + hex(f.offset, 1);
    if (!f.symbol.empty()) s += "  " + f.symbol;
    return s;
}

std::string format_report(const ReportData& d) {
    std::ostringstream o;
    o << "Syphon Filter: The Omega Strain (PC port) - crash report (port-only crash reporter, checkpoint 132win12)\n";
    o << "Time:          " << d.time << "\n";
    o << "Executable:    " << d.executable << "\n";
    o << "Command line:  " << d.command_line << "\n";
    o << "Reported by:   " << d.where << "\n";
    o << "Kind:          " << d.kind << "\n";
    if (!d.type.empty()) o << "Type:          " << d.type << "\n";
    o << "Message:       " << d.message << "\n";
    if (!d.fault.empty()) o << "Fault:         " << d.fault << "\n";
    const auto ee = ee_addresses(d.message);
    if (!ee.empty()) {
        o << "EE addresses:  ";
        for (std::size_t i = 0; i < ee.size(); ++i) o << (i ? " " : "") << ee[i];
        o << " (the original's routines / data named in the message: research/ADDRESS_INDEX.md)\n";
    }
    o << "Thrown at:     " << (d.thrown_at ? format_frame(*d.thrown_at) : std::string("unknown (no throw record)")) << "\n";
    o << "Minidump:      " << d.dump_file << "\n";
    o << "\nStack (from the throw / fault; module+offset, names from the linker map when present):\n";
    if (d.stack.empty()) o << "  (none)\n";
    for (std::size_t i = 0; i < d.stack.size(); ++i) o << "  #" << i << "  " << format_frame(d.stack[i]) << "\n";
    o << "\nLast " << d.log.size() << " log lines:\n";
    for (const auto& line : d.log) o << "  " << line << "\n";
    return o.str();
}

std::string file_stem(int year, int month, int day, int hour, int minute, int second, unsigned long pid) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "crash-%04d%02d%02d-%02d%02d%02d-%lu", year, month, day, hour, minute, second, pid);
    return buffer;
}

bool is_runtime_module(std::string_view file_name) {
    const auto n = lower(file_name);
    for (const char* prefix : {"ntdll", "kernelbase", "kernel32", "vcruntime", "msvcp", "ucrtbase"})
        if (n.rfind(prefix, 0) == 0) return true;
    return false;
}

// --- the handlers ---

// Port-only (132vt2): the PS Vita build has its own handlers (src/port/vita/crash_report_vita.cpp); the pure
// helpers above are shared.
#ifdef _WIN32
namespace {

constexpr DWORD cpp_exception = 0xE06D7363;
constexpr unsigned max_frames = 48;
constexpr unsigned records_per_thread = 4;

struct ThrowRecord {
    EXCEPTION_RECORD record;
    CONTEXT context;
    DWORD64 frames[max_frames];
    unsigned count;
    std::uint64_t serial;
};
struct ThreadThrows {
    ThrowRecord r[records_per_thread];
    unsigned next;
    std::uint64_t serial;
};
// Trivial, so static TLS with no constructor (safe to touch from the vectored handler).
thread_local ThreadThrows t_throws;

// Checkpoint 132pc1: set while a CRT invalid-parameter report runs. The C runtime's own state (a stream, a
// descriptor) may be what it rejected, so the reporter's log lines then bypass std::cout / std::cerr and stdio
// (log_line: the in-memory log ring and WriteFile on the standard error handle).
std::atomic<bool> g_raw_log{false};

// The call stack from a context (RtlVirtualUnwind; a frame without unwind data is a leaf: its return address is at
// the stack pointer). A bad context stops the walk.
unsigned unwind(CONTEXT context, DWORD64* out, unsigned max) {
    unsigned n = 0;
    __try {
        while (n < max && context.Rip != 0) {
            out[n++] = context.Rip;
            DWORD64 image = 0;
            const DWORD64 rsp = context.Rsp;
            if (auto* fn = RtlLookupFunctionEntry(context.Rip, &image, nullptr)) {
                void* handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, context.Rip, fn, &context, &handler_data, &establisher, nullptr);
            } else {
                context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                context.Rsp += 8;
            }
            if (context.Rsp <= rsp) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

LONG CALLBACK on_vectored(EXCEPTION_POINTERS* p) {
    if (p && p->ExceptionRecord && p->ExceptionRecord->ExceptionCode == cpp_exception && p->ContextRecord) {
        auto& t = t_throws;
        auto& r = t.r[t.next % records_per_thread];
        ++t.next;
        r.record = *p->ExceptionRecord;
        r.record.ExceptionRecord = nullptr;
        r.context = *p->ContextRecord;
        r.serial = ++t.serial;
        r.count = unwind(*p->ContextRecord, r.frames, max_frames);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

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
    std::mutex mutex;  // settings
    std::mutex log_mutex;
    Settings settings;
    std::unique_ptr<LineRing> ring;
    bool installed = false;
    std::atomic<bool> reporting{false};
};
// Never destroyed: the tees and handlers may run during static destruction.
State& state() {
    static State* s = new State;
    return *s;
}

std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(std::size_t(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(std::size_t(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n);
    return out;
}
std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

fs::path module_file(HMODULE module) {
    std::wstring buffer(32768, L'\0');
    const DWORD n = GetModuleFileNameW(module, buffer.data(), DWORD(buffer.size()));
    return n > 0 && n < buffer.size() ? fs::path(buffer.substr(0, n)) : fs::path();
}

HMODULE dbghelp() {
    static HMODULE module = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module;
}

std::string undecorate(const std::string& name) {
    if (name.empty() || name[0] != '?') return name;
    using Fn = DWORD(WINAPI*)(PCSTR, PSTR, DWORD, DWORD);
    static const auto fn = dbghelp() ? reinterpret_cast<Fn>(GetProcAddress(dbghelp(), "UnDecorateSymbolName")) : nullptr;
    if (!fn) return name;
    char buffer[1024];
    if (fn(name.c_str(), buffer, sizeof buffer, UNDNAME_NAME_ONLY) == 0) return name;
    return buffer;
}

// The executable's linker map (<exe>.map next to it), parsed at the first report.
const SymbolMap& exe_map() {
    static const SymbolMap map = [] {
        fs::path p = module_file(nullptr);
        p.replace_extension(".map");
        std::ifstream in(p, std::ios::binary);
        if (!in) return SymbolMap{};
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return SymbolMap::parse(text);
    }();
    return map;
}

Frame describe(std::uint64_t address) {
    Frame f;
    f.address = address;
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        module) {
        f.module = utf8(module_file(module).filename());
        f.offset = address - reinterpret_cast<std::uint64_t>(module);
        if (module == GetModuleHandleW(nullptr)) {
            // A return address points after its call: look up the byte before it.
            const auto name = exe_map().name_for(f.offset > 0 ? f.offset - 1 : 0);
            if (!name.empty()) {
                const auto plus = name.rfind("+0x");
                f.symbol = undecorate(name.substr(0, plus)) + (plus == std::string::npos ? "" : name.substr(plus));
            }
        }
    }
    return f;
}

std::string seh_name(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "floating-point divide by zero";
    case EXCEPTION_FLT_INVALID_OPERATION: return "floating-point invalid operation";
    case EXCEPTION_BREAKPOINT: return "breakpoint";
    case cpp_exception: return "C++ exception";
    case 0xC0000409: return "stack buffer overrun / fast fail";
    default: return "exception";
    }
}

std::string now_text(const SYSTEMTIME& t) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buffer;
}

// One report's inputs; the work is done on a fresh thread (a faulting thread may be out of stack, and
// MiniDumpWriteDump is meant to run outside the thread it describes).
struct Job {
    ReportData data;
    EXCEPTION_RECORD record{};
    CONTEXT context{};
    bool has_context = false;
    std::vector<DWORD64> frames;
    bool skip_handler_frames = false;  // the stack starts in the reporter's handler and the C runtime: drop them
    DWORD thread_id = 0;
    std::optional<Written> result;
    fs::path folder;                  // port-only (132mt3): when set, the report's folder (a stall report)
    std::vector<std::string> extra;   // port-only (132mt3): text after the report (the stall report's threads)
    bool inline_run = false;          // port-only (132mt3): run on the calling thread (a stall report)
};

void fill_frames(Job& job) {
    for (const auto address : job.frames) job.data.stack.push_back(describe(address));
    if (job.skip_handler_frames) {
        // The captured context is the handler's own frame, then the C runtime's (_invalid_parameter, ...): the stack
        // the report names starts at the first frame after the runtime frames.
        auto& s = job.data.stack;
        std::size_t k = 0;
        while (k < s.size() && !is_runtime_module(s[k].module)) ++k;
        while (k < s.size() && is_runtime_module(s[k].module)) ++k;
        if (k > 0 && k < s.size()) s.erase(s.begin(), s.begin() + std::ptrdiff_t(k));
    }
    for (const auto& f : job.data.stack)
        if (!f.module.empty() && !is_runtime_module(f.module)) {
            job.data.thrown_at = f;
            break;
        }
}

void run_job(Job& job) {
    auto& s = state();
    Settings settings;
    {
        std::lock_guard lock(s.mutex);
        settings = s.settings;
    }
    if (!job.folder.empty()) settings.folder = job.folder;
    std::error_code ec;
    fs::create_directories(settings.folder, ec);
    if (!fs::is_directory(settings.folder)) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::string stem = file_stem(t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
    for (int k = 2; fs::exists(settings.folder / (stem + ".txt")); ++k)
        stem = file_stem(t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId()) + "-" + std::to_string(k);
    Written w;
    w.text = settings.folder / (stem + ".txt");
    const fs::path dump = settings.folder / (stem + ".dmp");
    job.data.time = now_text(t);
    job.data.executable = utf8(module_file(nullptr));
    job.data.command_line = narrow(GetCommandLineW());
    fill_frames(job);

    std::string dump_status = "not written (dbghelp.dll MiniDumpWriteDump unavailable)";
    using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                    PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    if (const auto write = dbghelp() ? reinterpret_cast<WriteDump>(GetProcAddress(dbghelp(), "MiniDumpWriteDump")) : nullptr) {
        const HANDLE file = CreateFileW(dump.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            EXCEPTION_POINTERS pointers{&job.record, &job.context};
            MINIDUMP_EXCEPTION_INFORMATION info{job.thread_id, &pointers, FALSE};
            const auto type = MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory | MiniDumpWithThreadInfo |
                                            MiniDumpWithUnloadedModules);
            const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, type, job.has_context ? &info : nullptr,
                                  nullptr, nullptr);
            const DWORD error = ok ? 0 : GetLastError();
            CloseHandle(file);
            if (ok) {
                w.dump = dump;
                dump_status = utf8(dump.filename()) + " (" + std::to_string(fs::file_size(dump, ec)) + " bytes" +
                              (job.has_context ? ", exception stream = the " + std::string(job.data.where == "unhandled exception filter" ? "fault" : "throw") + " context"
                                               : ", no exception context") + ")";
            } else {
                fs::remove(dump, ec);
                dump_status = "not written (MiniDumpWriteDump error " + std::to_string(error) + ")";
            }
        } else {
            dump_status = "not written (cannot create " + utf8(dump) + ")";
        }
    }
    job.data.dump_file = dump_status;
    {
        std::lock_guard lock(s.log_mutex);
        if (s.ring) job.data.log = s.ring->lines();
    }
    std::ofstream out(w.text, std::ios::binary);
    out << format_report(job.data);
    for (const auto& line : job.extra) out << line << "\n";
    out.close();
    if (!out) return;
    w.summary = job.data.kind + (job.data.thrown_at ? ", thrown at " + format_frame(*job.data.thrown_at) : std::string());
    job.result = w;
}

std::optional<Written> run_on_worker(Job& job) {
    auto& s = state();
    if (s.reporting.exchange(true)) return std::nullopt;  // a fault while reporting
    if (job.inline_run) {  // port-only (132mt3): a stall report makes no thread (the loader may be what stalls)
        run_job(job);
        s.reporting = false;
        return job.result;
    }
    HANDLE thread = CreateThread(
        nullptr, 1 << 20,
        [](void* p) -> DWORD {
            run_job(*static_cast<Job*>(p));
            return 0;
        },
        &job, 0, nullptr);
    if (thread) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    } else {
        run_job(job);
    }
    s.reporting = false;
    return job.result;
}

// One line of the reporter's own log: std::cerr normally; during an invalid-parameter report (g_raw_log) the log
// ring and a WriteFile on the standard error handle (the launcher points it at local\omega_strain.log).
void log_line(const std::string& line) {
    if (!g_raw_log) {
        std::cerr << line << "\n";
        std::cerr.flush();
        return;
    }
    auto& s = state();
    {
        std::lock_guard lock(s.log_mutex);
        if (s.ring) {
            s.ring->write(line.data(), line.size());
            s.ring->write("\n", 1);
        }
    }
    const HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err == nullptr || err == INVALID_HANDLE_VALUE) return;
    const std::string text = line + "\r\n";
    DWORD written = 0;
    WriteFile(err, text.data(), DWORD(text.size()), &written, nullptr);
}

void log_written(const Written& w) {
    std::string line = "Crash reporter (port-only): wrote " + utf8(w.text);
    if (!w.dump.empty()) line += " and " + utf8(w.dump);
    log_line(line + " (" + w.summary + ")");
}

void flush_streams() {
    if (g_raw_log) return;
    std::cout.flush();
    std::cerr.flush();
    std::fflush(stdout);
    std::fflush(stderr);
}

LONG WINAPI on_unhandled(EXCEPTION_POINTERS* p) {
    Job job;
    job.data.where = "unhandled exception filter";
    const DWORD code = p && p->ExceptionRecord ? p->ExceptionRecord->ExceptionCode : 0;
    job.data.kind = "SEH 0x" + hex(code, 8) + " " + seh_name(code);
    job.data.type = seh_name(code);
    job.data.message = code == cpp_exception ? "a C++ exception no handler caught (the minidump has its throw context)"
                                             : "the process faulted (" + seh_name(code) + ")";
    if (p && p->ExceptionRecord) {
        job.record = *p->ExceptionRecord;
        job.record.ExceptionRecord = nullptr;
        if (code == EXCEPTION_ACCESS_VIOLATION && job.record.NumberParameters >= 2) {
            const auto op = job.record.ExceptionInformation[0];
            job.data.fault = std::string(op == 0 ? "read" : op == 1 ? "write" : op == 8 ? "execute" : "access") + " at 0x" +
                             hex(job.record.ExceptionInformation[1], 16);
        }
    }
    if (p && p->ContextRecord) {
        job.context = *p->ContextRecord;
        job.has_context = true;
        DWORD64 frames[max_frames];
        const unsigned n = unwind(*p->ContextRecord, frames, max_frames);
        job.frames.assign(frames, frames + n);
    }
    job.thread_id = GetCurrentThreadId();
    log_line("Crash reporter (port-only): " + job.data.kind + (job.data.fault.empty() ? "" : " (" + job.data.fault + ")"));
    if (const auto w = run_on_worker(job)) {
        log_written(*w);
        show_message(*w, job.data.message + (job.data.fault.empty() ? "" : " - " + job.data.fault));
    }
    flush_streams();
    return EXCEPTION_EXECUTE_HANDLER;
}

// The calling thread's record of the throw that made `object` (the most-derived exception object), else the newest.
std::optional<ThrowRecord> find_record(const void* object) {
    const auto& t = t_throws;
    const ThrowRecord* newest = nullptr;
    for (const auto& r : t.r) {
        if (r.serial == 0) continue;
        if (object && r.record.NumberParameters >= 2 && reinterpret_cast<const void*>(r.record.ExceptionInformation[1]) == object)
            return r;
        if (!newest || r.serial > newest->serial) newest = &r;
    }
    if (newest) return *newest;
    return std::nullopt;
}

Job job_for(const std::string& message, const std::string& type, const std::optional<ThrowRecord>& r, std::string_view where) {
    Job job;
    job.data.where = std::string(where);
    job.data.message = message;
    job.data.type = type;
    job.data.kind = is_unported(message) ? "unported branch (C++ exception: the port has not ported an original routine)"
                                         : "C++ exception";
    if (r) {
        job.record = r->record;
        job.context = r->context;
        job.has_context = true;
        job.frames.assign(r->frames, r->frames + r->count);
    }
    job.thread_id = GetCurrentThreadId();
    return job;
}

[[noreturn]] void on_terminate() {
    auto r = find_record(nullptr);  // before a rethrow below records another throw
    std::string message = "std::terminate with no active exception", type;
    if (const auto e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& x) {
            message = x.what();
            type = typeid(x).name();
        } catch (...) {
            message = "an exception not derived from std::exception";
        }
    }
    log_line("Crash reporter (port-only): std::terminate: " + message);
    auto job = job_for(message, type, r, "std::terminate (an exception escaped a thread or a noexcept function)");
    if (const auto w = run_on_worker(job)) {
        log_written(*w);
        show_message(*w, message);
    }
    flush_streams();
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) Sleep(INFINITE);
}

// abort() (std::terminate's default in a thread whose terminate handler is not ours, an assert): the same report.
void on_abort(int) { on_terminate(); }

// Checkpoint 132pc1: a failure the C runtime ends with a fast fail (__fastfail, exception 0xc0000409, which no
// exception filter sees): a CRT function given an invalid argument (_invalid_parameter: FAST_FAIL_INVALID_ARG, the
// play-test crashes in ucrtbase.dll at +0x11858, parameter 5), an STL check, a pure virtual call. The report is
// written from here (the current context and stack), then the process ends with the code the fast fail would give.
[[noreturn]] void fatal_report(const char* where, const std::string& kind, const std::string& message, DWORD code,
                               std::uint64_t parameter) {
    static std::atomic<bool> busy{false};
    if (busy.exchange(true)) TerminateProcess(GetCurrentProcess(), code);  // a second failure while reporting
    g_raw_log = true;
    Job job;
    job.data.where = where;
    job.data.kind = kind;
    job.data.type = kind;
    job.data.message = message;
    CONTEXT context{};
    RtlCaptureContext(&context);
    job.context = context;
    job.has_context = true;
    job.record.ExceptionCode = code;
    job.record.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
    job.record.NumberParameters = 1;
    job.record.ExceptionInformation[0] = ULONG_PTR(parameter);
    DWORD64 frames[max_frames];
    const unsigned n = unwind(context, frames, max_frames);
    job.frames.assign(frames, frames + n);
    job.skip_handler_frames = true;
    job.thread_id = GetCurrentThreadId();
    log_line("Crash reporter (port-only): " + kind + ": " + message);
    if (const auto w = run_on_worker(job)) {
        log_written(*w);
        show_message(*w, message);
    }
    TerminateProcess(GetCurrentProcess(), code);
    for (;;) Sleep(INFINITE);
}

std::string narrow_c(const wchar_t* text) { return text ? narrow(text) : std::string(); }

double filetime_seconds(const FILETIME& t) {
    return double((std::uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) / 1e7;
}

void __cdecl on_invalid_parameter(const wchar_t* expression, const wchar_t* function, const wchar_t* file, unsigned line,
                                  uintptr_t) {
    // A release CRT passes no expression, function or file.
    std::string detail;
    if (expression) detail += " expression " + narrow_c(expression);
    if (function) detail += " in " + narrow_c(function);
    if (file) detail += " (" + narrow_c(file) + ":" + std::to_string(line) + ")";
    fatal_report("CRT invalid-parameter handler (_set_invalid_parameter_handler)",
                 "CRT invalid parameter (without the reporter: fast fail 0xc0000409, FAST_FAIL_INVALID_ARG)",
                 "the C runtime rejected an argument (an invalid parameter" + (detail.empty() ? std::string(", no detail in a release CRT") : ":" + detail) +
                     "); the stack below names the caller",
                 0xC0000409, 5);
}

void __cdecl on_purecall() {
    fatal_report("CRT pure-call handler (_set_purecall_handler)", "pure virtual function call",
                 "a pure virtual function was called (an object used during its construction or destruction)", 0xC0000409, 0);
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
    ULONG guarantee = 64 * 1024;  // room for the unhandled-exception filter after a stack overflow
    SetThreadStackGuarantee(&guarantee);
    AddVectoredExceptionHandler(1, on_vectored);
    SetUnhandledExceptionFilter(on_unhandled);
    std::set_terminate(on_terminate);
    std::signal(SIGABRT, on_abort);
    // Checkpoint 132pc1: the C runtime's fast fails (an invalid parameter, a pure call) report too.
    _set_invalid_parameter_handler(on_invalid_parameter);
    _set_purecall_handler(on_purecall);
}

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
    if (kind == "invalid-parameter") {
        // Checkpoint 132pc1: a CRT call with an invalid descriptor (the play-test crashes' kind: _invalid_parameter,
        // without the reporter a fast fail 0xc0000409 in ucrtbase.dll).
        volatile int descriptor = -7;
        (void)_close(descriptor);
    }
    if (kind == "terminate") {
        std::thread([where = std::string(where)] {
            throw std::runtime_error("Simulated exception escaping a thread in " + where + " (--test-crash terminate, port-only test)");
        }).join();
    }
    throw std::runtime_error("--test-crash: unknown kind " + std::string(kind));
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
    flush_streams();
    auto job = job_for(e.what(), typeid(e).name(), find_record(dynamic_cast<const void*>(&e)), where);
    auto w = run_on_worker(job);
    if (w) log_written(*w);
    return w;
}

namespace {

// Port-only (132mt3): each thread's scheduler state from NtQuerySystemInformation(SystemProcessInformation), resolved
// when the watchdog starts (no DLL is loaded or procedure looked up while a stall is reported).
struct ThreadState {
    std::uint32_t state = 0, wait_reason = 0;
    std::int32_t priority = 0, base_priority = 0;
    std::uint32_t context_switches = 0;
    std::uint64_t start_address = 0;
};
using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
std::atomic<NtQuerySystemInformationFn> g_query_system{nullptr};

void resolve_query_system() {
    if (g_query_system) return;
    if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
        g_query_system = reinterpret_cast<NtQuerySystemInformationFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"));
}

std::map<DWORD, ThreadState> thread_states() {
    std::map<DWORD, ThreadState> out;
    const auto query = g_query_system.load();
    if (!query) return out;
    // The documented layouts (winternl.h's SYSTEM_PROCESS_INFORMATION; SYSTEM_THREAD_INFORMATION, x64).
    struct SystemThread {
        LARGE_INTEGER kernel, user, create;
        ULONG wait_time;
        PVOID start_address;
        struct { HANDLE process, thread; } client;
        LONG priority, base_priority;
        ULONG context_switches, state, wait_reason;
    };
    struct SystemProcess {
        ULONG next, threads;
        BYTE reserved1[48];
        struct { USHORT length, maximum; PWSTR buffer; } name;
        LONG base_priority;
        HANDLE pid;
        HANDLE parent;
        ULONG handles, session;
        ULONG_PTR reserved2;
        SIZE_T peak_virtual, virtual_size;
        ULONG page_faults;
        SIZE_T peak_working, working, quota_peak_paged, quota_paged, quota_peak_nonpaged, quota_nonpaged, pagefile,
            peak_pagefile, private_pages;
        LARGE_INTEGER reserved3[6];
    };
    std::vector<std::uint8_t> buffer(1 << 20);
    for (int tries = 0; tries < 6; ++tries) {
        ULONG needed = 0;
        const LONG status = query(5, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);  // SystemProcessInformation
        if (status == 0) break;
        if (status != static_cast<LONG>(0xC0000004L)) return out;  // STATUS_INFO_LENGTH_MISMATCH: grow, else give up
        buffer.assign(std::max<std::size_t>(needed + (1 << 16), buffer.size() * 2), 0);
        if (tries == 5) return out;
    }
    const auto pid = static_cast<ULONG_PTR>(GetCurrentProcessId());
    for (std::size_t at = 0;;) {
        const auto* p = reinterpret_cast<const SystemProcess*>(buffer.data() + at);
        if (reinterpret_cast<ULONG_PTR>(p->pid) == pid) {
            const auto* t = reinterpret_cast<const SystemThread*>(p + 1);
            for (ULONG k = 0; k < p->threads; ++k) {
                ThreadState s;
                s.state = t[k].state;
                s.wait_reason = t[k].wait_reason;
                s.priority = t[k].priority;
                s.base_priority = t[k].base_priority;
                s.context_switches = t[k].context_switches;
                s.start_address = reinterpret_cast<std::uint64_t>(t[k].start_address);
                out[static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(t[k].client.thread))] = s;
            }
            break;
        }
        if (!p->next) break;
        at += p->next;
    }
    return out;
}

std::string thread_state_name(std::uint32_t state, std::uint32_t wait_reason) {
    static const char* states[] = {"initialized", "ready", "running", "standby", "terminated", "waiting", "transition",
                                   "deferred ready", "gate waiting"};
    static const char* reasons[] = {"Executive", "FreePage", "PageIn", "PoolAllocation", "DelayExecution", "Suspended",
                                    "UserRequest", "WrExecutive", "WrFreePage", "WrPageIn", "WrPoolAllocation",
                                    "WrDelayExecution", "WrSuspended", "WrUserRequest", "WrEventPair", "WrQueue",
                                    "WrLpcReceive", "WrLpcReply", "WrVirtualMemory", "WrPageOut", "WrRendezvous",
                                    "WrKeyedEvent", "WrTerminated", "WrProcessInSwap", "WrCpuRateControl",
                                    "WrCalloutStack", "WrKernel", "WrResource", "WrPushLock", "WrMutex",
                                    "WrQuantumEnd", "WrDispatchInt", "WrPreempted", "WrYieldExecution", "WrFastMutex",
                                    "WrGuardedMutex", "WrRundown", "WrAlertByThreadId", "WrDeferredPreempt"};
    std::string s = state < std::size(states) ? states[state] : "state " + std::to_string(state);
    if (state == 5) s += std::string(" (") + (wait_reason < std::size(reasons) ? reasons[wait_reason] : "reason " + std::to_string(wait_reason)) + ")";
    return s;
}

std::atomic<bool> g_watchdog_started{false};

}  // namespace

std::vector<std::string> thread_report() {
    std::vector<std::string> out;
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    const auto states = thread_states();  // before any thread is suspended
    std::vector<DWORD> ids;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return {"(no thread snapshot: error " + std::to_string(GetLastError()) + ")"};
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) ids.push_back(te.th32ThreadID);
    CloseHandle(snap);
    out.push_back("Threads (port-only stall report, 132mt3): " + std::to_string(ids.size()) + " besides the reporter's");
    for (const DWORD id : ids) {
        const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION |
                                        THREAD_QUERY_LIMITED_INFORMATION, FALSE, id);
        if (!h) {
            out.push_back("thread " + std::to_string(id) + ": cannot open (error " + std::to_string(GetLastError()) + ")");
            continue;
        }
        std::string head = "thread " + std::to_string(id);
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(h, &description)) && description) {
            if (description[0]) head += " \"" + narrow(description) + "\"";
            LocalFree(description);
        }
        head += " priority " + std::to_string(GetThreadPriority(h));
        if (const auto it = states.find(id); it != states.end()) {
            const auto& s = it->second;
            char t[160];
            std::snprintf(t, sizeof t, ", %s, scheduler priority %d (base %d), %u context switches, start 0x%llx",
                          thread_state_name(s.state, s.wait_reason).c_str(), s.priority, s.base_priority,
                          s.context_switches, static_cast<unsigned long long>(s.start_address));
            head += t;
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        if (GetThreadTimes(h, &created, &exited, &kernel, &user)) {
            char t[96];
            std::snprintf(t, sizeof t, ", CPU user %.3f s kernel %.3f s", filetime_seconds(user), filetime_seconds(kernel));
            head += t;
        }
        // Nothing allocates while the thread is suspended (it may hold the heap lock).
        DWORD64 frames[max_frames];
        unsigned n = 0;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        const DWORD suspended = SuspendThread(h);
        bool got = false;
        if (suspended != DWORD(-1)) {
            got = GetThreadContext(h, &context) != 0;
            if (got) n = unwind(context, frames, max_frames);
            ResumeThread(h);
        }
        CloseHandle(h);
        if (suspended == DWORD(-1)) head += ", not suspended (error " + std::to_string(GetLastError()) + ")";
        else head += ", suspend count before " + std::to_string(suspended);
        if (!got) head += ", no context";
        out.push_back(head + ":");
        for (unsigned k = 0; k < n; ++k) out.push_back("    " + format_frame(describe(frames[k])));
    }
    return out;
}

std::optional<Written> report_stall(std::string_view where, std::string_view message, const fs::path& fallback_folder) {
    flush_streams();
    Job job;
    job.data.where = std::string(where);
    job.data.kind = "stall (port-only watchdog, 132mt3)";
    job.data.type = "stall";
    job.data.message = std::string(message);
    job.thread_id = GetCurrentThreadId();
    if (!installed()) job.folder = fallback_folder;
    job.inline_run = true;
    job.extra = thread_report();
    auto w = run_on_worker(job);
    if (w) log_written(*w);
    return w;
}

namespace {

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
    fs::path fallback = fs::absolute("local") / "stalls";
    Shutdown shutdown;
};
Watchdog& watchdog() {
    static Watchdog* w = new Watchdog;  // never destroyed: its thread runs to the process end
    return *w;
}

void watchdog_loop() {
    auto& w = watchdog();
    (void)exe_map();  // parsed now, not during a stall
    for (;;) {
        Sleep(500);
        std::vector<std::pair<std::string, std::string>> stalls;  // name, message
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
        // Port-only (132ws8): a shutdown phase past its bound ends the process (arm_shutdown).
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
            flush_streams();
            // TerminateProcess, not ExitProcess: no DLL detach (a driver's detach could stall the same way).
            TerminateProcess(GetCurrentProcess(), UINT(late->exit_code));
            for (;;) Sleep(INFINITE);
        }
    }
}

}  // namespace

void start_stall_watchdog() {
    if (g_watchdog_started.exchange(true)) return;
    resolve_query_system();
    (void)dbghelp();  // loaded now: a stall report loads no DLL
    (void)watchdog();
    // A plain Win32 thread (std::thread's start is the same CreateThread underneath); detached, it sleeps between checks.
    if (const HANDLE t = CreateThread(nullptr, 1 << 20, [](void*) -> DWORD { watchdog_loop(); return 0; }, nullptr, 0, nullptr)) {
        SetThreadDescription(t, L"stall watchdog (port-only)");
        CloseHandle(t);
    }
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

int raise_idle_threads() {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    const DWORD pid = GetCurrentProcessId();
    int raised = 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        const HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SET_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        const int priority = GetThreadPriority(h);
        if (priority != THREAD_PRIORITY_ERROR_RETURN && priority < THREAD_PRIORITY_NORMAL &&
            SetThreadPriority(h, THREAD_PRIORITY_NORMAL))
            ++raised;
        CloseHandle(h);
    }
    CloseHandle(snap);
    return raised;
}

void show_message(const Written& w, std::string_view message) {
    bool dialogs = false;
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        dialogs = s.settings.dialogs;
    }
    std::string text = "The game stopped with an error:\n\n" + std::string(message) +
                       "\n\nA crash report was written (port-only crash reporter):\n" + utf8(w.text);
    if (!w.dump.empty()) text += "\n" + utf8(w.dump);
    text += "\n\nPlease attach both files to the bug report.";
    if (!dialogs) {
        std::string one = text;
        std::replace(one.begin(), one.end(), '\n', ' ');
        log_line("Crash reporter (port-only): message box suppressed (dialogs off): " + one);
        return;
    }
    MessageBoxW(nullptr, widen(text).c_str(), L"Syphon Filter: The Omega Strain", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

#endif  // _WIN32

}  // namespace omega::crash
