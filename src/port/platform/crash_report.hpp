#pragma once
// Port-only (checkpoint 132win12, not the original): the crash reporter. The PS2 game has no equivalent (an EE
// exception there ends in the kernel's exception screen); this exists so a play-test crash of the PC port comes back
// with evidence.
//
// install() (the launcher, OmegaStrain.exe / omega_play --launch, before the play host runs; omega_play with
// --crash-dir <folder>) sets up:
//   * a vectored exception handler that records each thread's last C++ throws (the MSVC exception 0xE06D7363's
//     EXCEPTION_RECORD, the CPU context at the throw and the call stack unwound from it; a few microseconds per
//     throw, so throws caught in normal flow cost nothing noticeable) - the throw address of an exception that is
//     later caught at the top level;
//   * an unhandled-exception filter (access violations and other SEH faults) and a std::terminate handler
//     (an exception escaping a thread or a noexcept function);
//   * (checkpoint 132pc1) the C runtime's invalid-parameter and pure-call handlers: those failures otherwise end in
//     a fast fail (0xc0000409 in ucrtbase.dll, FAST_FAIL_INVALID_ARG) that no exception filter sees;
//   * a tee on std::cout / std::cerr keeping the last `log_lines` lines in memory.
// report_exception() (play_main's top-level catch: the thrown "omega_play failed" error, e.g. a "... is not ported"
// branch naming its EE address) and the two handlers write, into the crash folder:
//   crash-YYYYMMDD-HHMMSS-<pid>.txt  the message, the exception type, the EE addresses named in the message, the
//                                    throw address (module+offset, with the function name when <exe>.map lies next
//                                    to the executable), the unwound stack, the command line and the last log lines;
//   crash-YYYYMMDD-HHMMSS-<pid>.dmp  a minidump (MiniDumpWriteDump, dbghelp.dll loaded at report time) whose
//                                    exception stream is the throw's (or the fault's) context.
// A message box names the files; with dialogs off (--hidden, --no-dialogs) its text goes to stderr instead.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omega::crash {

struct Settings {
    std::filesystem::path folder;  // made on the first report
    bool dialogs = true;
    std::size_t log_lines = 200;
};

// Idempotent: a later call only replaces the folder and the dialog setting.
void install(const Settings& settings);
bool installed();
void set_dialogs(bool dialogs);

struct Written {
    std::filesystem::path text;
    std::filesystem::path dump;  // empty when the minidump could not be written
    std::string summary;         // one line: the kind and the throw address
};

// The report for an exception caught at the top level (the calling thread's matching throw record supplies the
// throw context). nullopt when not installed or the folder cannot be written.
std::optional<Written> report_exception(const std::exception& e, std::string_view where);
// The message box naming the files (stderr when dialogs are off).
void show_message(const Written& written, std::string_view message);

// Port-only (checkpoint 132mt3): a stall report, for a watchdog that found a thread making no progress. Works without
// install() (then into `fallback_folder`). The .txt names `message` and, for every other thread of the process, its id,
// description, priority, CPU times and call stack (each thread suspended in turn, its context unwound, the frames named
// from <exe>.map); the minidump (MiniDumpWithThreadInfo) holds every thread's stack and context. The process carries
// on (nothing is changed). nullopt when the folder cannot be written.
std::optional<Written> report_stall(std::string_view where, std::string_view message,
                                    const std::filesystem::path& fallback_folder);
// The threads section of a stall report: every thread but the caller's, one header line each (its scheduler state and
// wait reason, priority, CPU times), then its frames.
std::vector<std::string> thread_report();

// Port-only (checkpoint 132mt3): the stall watchdog, one process-wide thread. Start it early (play_main does, before
// the front end): a stall in the Windows loader keeps any thread made later from starting, so a watchdog made at the
// moment of the risk could not run. A StallWatch registers a progress probe for its lifetime (progress() must be a
// cheap atomic read); when the value stays the same for `seconds`, the watchdog logs the name and describe() and writes
// one stall report (report_stall: every thread's state and stack, the minidump), made on the watchdog's own thread
// with dbghelp and the linker map loaded beforehand, so it neither makes a thread nor loads a DLL. Reports only.
void start_stall_watchdog();
class StallWatch {
public:
    StallWatch(std::string name, double seconds, std::function<std::int64_t()> progress,
               std::function<std::string()> describe);
    ~StallWatch();
    StallWatch(const StallWatch&) = delete;
    StallWatch& operator=(const StallWatch&) = delete;

private:
    std::uint64_t id_ = 0;
};

// Port-only tests (--test-crash / --frontend-test-crash <kind> <frame>): "throw" (a std::runtime_error), "unported"
// (a "... (00214920) is not ported" error), "access-violation" (a write through a null pointer: the SEH filter),
// "terminate" (an exception escaping a std::thread) or "invalid-parameter" (checkpoint 132pc1: _close of an invalid
// descriptor, the CRT invalid-parameter handler). Does not return.
bool test_crash_kind(std::string_view kind);
[[noreturn]] void test_crash(std::string_view kind, std::string_view where);

// --- pure helpers (tests/crash_report_tests.cpp) ---

// EE addresses named in a message: 8-digit hex words (not after "0x", not part of a longer hex run) inside the PS2's
// 32 MiB of EE RAM above the kernel (0x00080000..0x01ffffff), in order, without repeats.
std::vector<std::string> ee_addresses(std::string_view message);
// An error the port throws for an original branch it has not ported ("... is not ported", "unported").
bool is_unported(std::string_view message);

// The last `capacity` complete lines written, plus the line being written.
class LineRing {
public:
    explicit LineRing(std::size_t capacity);
    void write(const char* data, std::size_t size);
    std::vector<std::string> lines() const;

private:
    std::size_t capacity_;
    std::vector<std::string> ring_;
    std::size_t next_ = 0;
    std::size_t count_ = 0;
    std::string partial_;
};

// A linker map (/MAP): the public symbols' RVAs, sorted. name_for(rva) is the nearest symbol at or below the RVA
// with the offset into it ("name+0x1c"), or empty.
class SymbolMap {
public:
    static SymbolMap parse(std::string_view map_text);
    std::string name_for(std::uint64_t rva) const;
    std::size_t size() const { return symbols_.size(); }

private:
    struct Symbol {
        std::uint64_t rva;
        std::string name;
    };
    std::vector<Symbol> symbols_;
};

struct Frame {
    std::uint64_t address = 0;
    std::string module;      // file name, empty when unknown
    std::uint64_t offset = 0;  // from the module base
    std::string symbol;      // from the map, may be empty
};

struct ReportData {
    std::string time;          // "2025-01-31 12:34:56"
    std::string executable;
    std::string command_line;
    std::string where;         // "play_main top-level catch", "unhandled exception filter", "std::terminate"
    std::string kind;          // "C++ exception", "unported branch (C++ exception)", "SEH 0xc0000005 access violation"
    std::string type;          // the C++ type, or the SEH code's name
    std::string message;
    std::string fault;         // SEH detail ("write at 0x0000000000000000")
    std::optional<Frame> thrown_at;  // the first frame outside the Windows / C++ runtime DLLs
    std::vector<Frame> stack;
    std::string dump_file;     // name, or "not written (<why>)"
    std::vector<std::string> log;
};

std::string format_frame(const Frame& f);
std::string format_report(const ReportData& data);
// "crash-20250131-123456-4242"
std::string file_stem(int year, int month, int day, int hour, int minute, int second, unsigned long pid);
// True for the system / C++ runtime modules a throw passes through (ntdll, KERNELBASE, kernel32, vcruntime, msvcp,
// ucrtbase): the throw address is the first frame in any other module.
bool is_runtime_module(std::string_view file_name);

}  // namespace omega::crash
