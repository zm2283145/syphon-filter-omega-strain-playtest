#include "launcher.hpp"

#include "card_backup.hpp"
#include "crash_report.hpp"
#include "derived_data.hpp"
#include "disc.hpp"
#ifdef _WIN32
#include "launcher_mod_panel.hpp"
#endif
#include "mod_manager.hpp"
#include "ps2_save_icons.hpp"
#include "windows_hash.hpp"

// Port-only (132vt2): the PS Vita build compiles the portable half (the play argument list, the ISO search and
// identity, the save icons); the launcher itself (run, its dialogs and the install folder) is Windows-only.
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <io.h>
#include <shlobj.h>
#else
#include <strings.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace omega::launcher {
namespace fs = std::filesystem;
namespace {

std::atomic<bool> g_active{false};
std::atomic<bool> g_dialogs{true};
std::atomic<bool> g_failure_shown{false};

#ifdef _WIN32
std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring out(std::size_t(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n);
    return out;
}
std::string narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(std::size_t(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}
#else
std::string narrow(const std::wstring& text) {  // the Vita: libstdc++'s UTF-32 -> UTF-8
    const auto u = fs::path(text).u8string();
    return std::string(u.begin(), u.end());
}
#endif
std::string utf8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

constexpr const wchar_t* title = L"Syphon Filter: The Omega Strain";

void show_error(const std::string& message) {
    std::cerr << "Launcher (port-only): error: " << message << "\n";
    std::cerr.flush();
    std::cout.flush();
#ifdef _WIN32
    if (g_dialogs) MessageBoxW(nullptr, widen(message).c_str(), title, MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
#endif
}

#ifdef _WIN32

// The folder whose config/disc_us.json marks an install (the exe's own, else the working directory: a repository run).
fs::path install_folder() {
    std::wstring buffer(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    if (n > 0 && n < buffer.size()) {
        const fs::path exe_folder = fs::path(buffer.substr(0, n)).parent_path();
        if (fs::is_regular_file(exe_folder / "config" / "disc_us.json")) return exe_folder;
    }
    if (fs::is_regular_file(fs::current_path() / "config" / "disc_us.json")) return fs::current_path();
    throw std::runtime_error("config\\disc_us.json is missing next to OmegaStrain.exe. Reinstall the package folder "
                             "(README-PLAY.md) and run OmegaStrain.exe from it.");
}

// With no console and no redirected output (OmegaStrain.exe started from Explorer), the session's output goes to
// local/omega_strain.log.
// Started without a console or redirection (Explorer, a shortcut): stdout and stderr go to local\omega_strain.log.
// Checkpoint 132pc1: they share ONE open file. Before, stderr was a second _wfreopen_s of the same file; the secure
// _wfreopen_s opens it exclusively for writing, so that second open failed after closing stderr, and the first
// std::cerr write of the session (play_main's "omega_play failed: ..." after a thrown error) was a CRT invalid
// parameter: a fast fail 0xc0000409 in ucrtbase.dll with no message and no crash report (the play-test crashes).
// (Even had it opened, two opens keep two file positions and stdout's writes overwrite stderr's lines.) Now stderr
// is opened on NUL (a valid stream of its own) and its descriptor is replaced by a duplicate of stdout's (_dup2: the
// same file object and position), unbuffered; the Windows standard handles point at the log too, so the crash
// reporter's raw writes land there. std::cerr stays tied to std::cout (flushed first): the lines keep their order.
void redirect_output_if_detached() {
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != nullptr && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN) return;
    FILE* f = nullptr;
    if (_wfreopen_s(&f, L"local\\omega_strain.log", L"w", stdout) != 0 || !f) return;
    FILE* e = nullptr;
    if (_wfreopen_s(&e, L"NUL", L"w", stderr) == 0 && e) {
        if (_dup2(_fileno(stdout), _fileno(stderr)) == 0) std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
    const auto log_handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout)));
    if (log_handle != INVALID_HANDLE_VALUE) {
        SetStdHandle(STD_OUTPUT_HANDLE, log_handle);
        SetStdHandle(STD_ERROR_HANDLE, log_handle);
    }
    std::ios::sync_with_stdio(true);
    std::cout.clear();
    std::cerr.clear();
}

std::vector<fs::path> known_folders() {
    std::vector<fs::path> roots;
    for (const auto& id : {FOLDERID_Downloads, FOLDERID_Documents, FOLDERID_Desktop}) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)) && path) roots.emplace_back(path);
        CoTaskMemFree(path);
    }
    return roots;
}

std::optional<fs::path> ask_for_iso(const std::string& reason) {
    if (!g_dialogs) return std::nullopt;
    const std::string text = reason +
                             "\n\nSelect your own disc image (.iso) of Syphon Filter: The Omega Strain (USA, SCUS-97264). "
                             "It is checked against the supported disc and remembered in local\\port_settings.txt.";
    if (MessageBoxW(nullptr, widen(text).c_str(), title, MB_OKCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND) != IDOK)
        return std::nullopt;
    std::wstring file(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.lpstrFilter = L"Disc images (*.iso)\0*.iso\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = DWORD(file.size());
    ofn.lpstrTitle = L"Select the Syphon Filter: The Omega Strain (USA) disc image";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return fs::path(file.c_str());
}

std::string size_problem(const fs::path& iso) {
    std::error_code ec;
    if (!fs::is_regular_file(iso, ec)) return "The disc image " + utf8(iso) + " does not exist.";
    const auto size = fs::file_size(iso, ec);
    if (ec) return "The disc image " + utf8(iso) + " cannot be read.";
    if (size != OMEGA_ISO_SIZE)
        return "The file " + utf8(iso) + " is not the supported disc image: it has " + std::to_string(size) + " bytes, the USA disc " +
               std::to_string(OMEGA_ISO_SIZE) + ".";
    return {};
}

derived::DiscFingerprint fingerprint() { return {OMEGA_ISO_SHA256, OMEGA_BOOT_EXECUTABLE, OMEGA_EXE_SHA256, OMEGA_EXE_SIZE}; }
#endif  // _WIN32

std::vector<std::string> backslash_path(const std::string& path) {
    std::vector<std::string> parts;
    std::string part;
    for (const char c : path + "\\") {
        if (c != '\\') {
            part += c;
            continue;
        }
        if (!part.empty()) parts.push_back(part);
        part.clear();
    }
    return parts;
}

#ifdef _WIN32
// The SHA-256 check (the full hash unless iso_verified= matches the file as it is now and hash is false; the play
// host verifies the image too, and open_verified_iso remembers a verified file for the process, so it is hashed at most
// once) and the derived files when missing or stale. Returns an error text (empty: success); *verified receives the
// image's identity on success.
std::string verify_and_derive(const fs::path& iso, const fs::path& folder, bool force, const std::optional<VerifiedIso>& cached,
                              bool hash, std::optional<VerifiedIso>* verified = nullptr) {
    if (auto problem = size_problem(iso); !problem.empty()) return problem;
    const auto files = derived::derived_files(folder);
    auto identity = iso_identity(iso);
    if (!identity) return "The disc image " + utf8(iso) + " cannot be read.";
    identity->sha256 = OMEGA_ISO_SHA256;
    std::ifstream stream;
    try {
        if (!hash && verified_iso_matches(cached, *identity, OMEGA_ISO_SHA256)) {
            std::cout << "Launcher (port-only): " << utf8(iso) << " was verified before (the same path, size and write "
                      << "time; iso_verified=); not hashed again (--verify-iso hashes it)\n";
            remember_verified_iso(iso, OMEGA_ISO_SIZE, OMEGA_ISO_SHA256);
            stream.open(iso, std::ios::binary);
            if (!stream) return "The disc image " + utf8(iso) + " cannot be opened.";
        } else {
            std::cout << "Launcher (port-only): verifying " << utf8(iso) << " (SHA-256 of the whole image)...\n";
            std::cout.flush();
            const auto t0 = GetTickCount64();
            stream = open_verified_iso(iso, OMEGA_ISO_SIZE, OMEGA_ISO_SHA256);
            std::cout << "Launcher (port-only): verified (" << (GetTickCount64() - t0) << " ms)\n";
        }
    } catch (const std::exception& e) {
        return "The disc image " + utf8(iso) + " is not the supported Syphon Filter: The Omega Strain (USA) disc (" + e.what() +
               "). Only an unmodified 1:1 image of SCUS-97264 is supported.";
    }
    if (verified) *verified = identity;
    if (!force && derived::derived_data_current(files, fingerprint())) return {};
    DiscImage disc(stream, OMEGA_ISO_SIZE);
    const auto t0 = GetTickCount64();
    derived::derive_game_data(disc, fingerprint(), folder);
    std::cout << "Launcher (port-only): derived the motion graphs and tunings into " << utf8(folder) << " ("
              << (GetTickCount64() - t0) << " ms)\n";
    return {};
}
#endif  // _WIN32

}  // namespace

bool active() { return g_active; }

void report_play_failure(const std::string& message, const std::string& crash_files) {
    if (!g_active) return;
    g_failure_shown = true;
    std::string text = "The game stopped with an error:\n\n" + message + "\n\n";
    if (!crash_files.empty())
        text += "A crash report was written (port-only crash reporter):\n" + crash_files +
                "\n\nPlease attach these files to the bug report. ";
    show_error(text + "The log is local\\omega_strain.log when started without a console (README-PLAY.md, "
                      "\"Troubleshooting\").");
}

bool is_launch(int argc, const wchar_t* const* argv) {
    if (argc <= 1) return true;
    const std::wstring first = argv[1];
    return first == L"--launch" || first == L"--derive-data";
}

bool is_launcher_executable(const fs::path& exe) {
    std::wstring stem = exe.stem().wstring();
    for (auto& c : stem) c = wchar_t(std::towlower(c));
    return stem == L"omegastrain";
}

#ifdef _WIN32
fs::path module_path() {
    std::wstring buffer(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), DWORD(buffer.size()));
    return n > 0 && n < buffer.size() ? fs::path(buffer.substr(0, n)) : fs::path();
}
#endif

Options parse_options(int argc, const wchar_t* const* argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto value = [&]() -> std::wstring {
            if (i + 1 >= argc) throw std::runtime_error(narrow(a) + " needs a value");
            return argv[++i];
        };
        if (a == L"--launch") continue;
        if (a == L"--iso") o.iso = fs::path(value());
        else if (a == L"--derive-data") o.derive_only = fs::path(value());
        else if (a == L"--no-dialogs") o.dialogs = false;
        else if (a == L"--no-search") o.search = false;
        else if (a == L"--verify-iso") o.verify_iso = true;
        else if (a == L"--mod-panel") o.mod_panel = true;
        else if (a == L"--mod-panel-script") {
            o.mod_panel_script = fs::absolute(fs::path(value()));
            o.mod_panel = true;
        }
        else if (a == L"--card-export" || a == L"--card-import" || a == L"--card-restore") {
            if (o.card && !o.card->file.empty()) throw std::runtime_error("give one of --card-export, --card-import and --card-restore");
            CardCommand c;
            c.kind = a == L"--card-export"   ? CardCommand::Kind::export_card
                     : a == L"--card-import" ? CardCommand::Kind::import_card
                                             : CardCommand::Kind::restore;
            c.file = fs::absolute(fs::path(value()));
            c.replace = o.card && o.card->replace;
            o.card = c;
        } else if (a == L"--card-replace") {
            if (!o.card) o.card = CardCommand{CardCommand::Kind::import_card};
            o.card->replace = true;
        }
        else {
            if (a == L"--hidden") {
                o.hidden = true;
                o.dialogs = false;
            }
            o.extra.push_back(a);
            if (a == L"--port-settings" && i + 1 < argc) {
                o.settings = fs::path(argv[i + 1]);
                o.extra.push_back(argv[++i]);
            }
        }
    }
    if (o.card && o.card->file.empty()) throw std::runtime_error("--card-replace needs --card-import <file.ps2>");
    if (o.card && o.card->replace && o.card->kind != CardCommand::Kind::import_card)
        throw std::runtime_error("--card-replace needs --card-import");
    return o;
}

fs::path panel_mods_folder(const Options& o, const PortSettings& s) {
    fs::path flag;
    for (std::size_t i = 0; i + 1 < o.extra.size(); ++i)
        if (o.extra[i] == L"--mods") flag = fs::path(o.extra[i + 1]);
    return mods::session_mods_folder(s, flag);
}

std::vector<std::wstring> play_arguments(const fs::path& iso, const derived::DerivedFiles& files, const Options& o) {
    static constexpr const wchar_t* parts[][3]{
        {L"--part", L"1BOOT_COM_B01-0", L"_BOOT_COM_ZIPP0"},       {L"--part", L"1LEGS_PAN_L01-0", L"_LEGS_PAN_GCAM0"},
        {L"--part", L"1TORS_BAR_U02-0", L"1TORS_BAR3PC1-0"},       {L"--morph-part", L"1HAND1BAR_NONE0", L"1HAND_BAR3----0"},
        {L"--morph-part", L"1HAND2BAR_NONE0", L"1HAND_BAR3----0"}, {L"--morph-part", L"1NECK_PLA_DEFA0", L"1NECK_PLA3----0"},
        {L"--morph-part", L"1MOUT_PLA_DEFA0", L"1MOUT_PLA_----0"}, {L"--morph-part", L"1EYE-_PLA_DEFA0", L"_EYE-_PLA_PURP0"},
        {L"--morph-part", L"1HEAD_PLA_DEFA0", L"1HEAD_PLA3PC030"}, {L"--morph-part", L"1BEAR_PLA_DEFA0", L"_BEAR_PMA_BROW0"},
        {L"--part", L"1HAIR_A03_HAIR0", L"1HAIR_A03_BLAC0"},       {L"--part", L"1SHIR_TIG_U02-0", L"_SHIR_TIG_WHIT0"},
        {L"--part", L"1BELT1HOL_T01-0", L"_BELT_HOL_01--0"},       {L"--part", L"1BELT3HOL_T01-0", L"_BELT_HOL_01--0"},
        {L"--part", L"1BELT2HOL_T01-0", L"_BELT_HOL_01--0"},       {L"--part", L"1VEST_V02_V02-0", L"_VEST_V02_GREE0"},
        {L"--part", L"1TATT2ARM_U02-0", L"_TATT_ARM_01--0"},       {L"--part", L"1TATT1ARM_U02-0", L"_TATT_ARM_02--0"},
        {L"--part", L"1STRA_NYL_V02-0", L"_STRA_NYL_BLAC0"},
    };
    std::vector<std::wstring> a{L"--iso",   iso.wstring(), L"--mission", L"TORONTO1", L"--objects", L"--graph",
                                files.graph.wstring(), L"--tuning", files.actor_tuning.wstring()};
    for (const auto& p : parts) {
        a.insert(a.end(), {p[0], std::wstring(p[1]) + L".SKM", L"--tex", std::wstring(p[2]) + L".TDX"});
    }
    a.insert(a.end(), {L"--textured", L"--gui-overlay"});
    if (!o.hidden) a.push_back(L"--audio-out");
    a.insert(a.end(), {L"--mission-scripts", L"--combat", L"--npc-perception", L"--hard-mesh", L"1HARD_DEF_V02-0.SKM",
                       L"--navigation-tuning", files.navigation_tuning.wstring(), L"--frontend", L"--female-graph",
                       files.female_graph.wstring()});
    a.insert(a.end(), o.extra.begin(), o.extra.end());
    return a;
}

std::vector<fs::path> find_iso_candidates(const std::vector<fs::path>& roots, std::uint64_t size, int max_depth) {
    std::vector<fs::path> found;
    auto lower_ext = [](const fs::path& p) {
        std::wstring e = p.extension().wstring();
        for (auto& c : e) c = wchar_t(std::towlower(c));
        return e;
    };
    auto walk = [&](auto&& self, const fs::path& folder, int depth, std::vector<fs::path>& out) -> void {
        std::error_code ec;
        fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec);
        if (ec) return;
        std::vector<fs::path> files, folders;
        for (; it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) break;
            const auto& entry = *it;
            std::error_code e2;
            if (entry.is_regular_file(e2)) {
                if (lower_ext(entry.path()) == L".iso" && entry.file_size(e2) == size && !e2) files.push_back(entry.path());
            } else if (entry.is_directory(e2) && depth < max_depth) {
                folders.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        std::sort(folders.begin(), folders.end());
        out.insert(out.end(), files.begin(), files.end());
        for (const auto& f : folders) self(self, f, depth + 1, out);
    };
    for (const auto& root : roots) walk(walk, root, 0, found);
    // A file reached through two roots (a junction or a nested root) is listed once, at its first place.
    std::vector<fs::path> unique;
    for (const auto& p : found) {
        std::error_code ec;
        const auto canonical = fs::weakly_canonical(p, ec);
        const bool seen = std::any_of(unique.begin(), unique.end(), [&](const fs::path& q) {
            std::error_code e2;
            return fs::equivalent(q, p, e2) || (!ec && fs::weakly_canonical(q, e2) == canonical);
        });
        if (!seen) unique.push_back(p);
    }
    return unique;
}

std::vector<Ps2CardFile> save_icon_files_from_iso(const fs::path& iso) {
    std::error_code ec;
    const auto size = fs::file_size(iso, ec);
    std::ifstream in(iso, std::ios::binary);
    if (ec || !in) throw std::runtime_error("cannot read the disc image " + utf8(iso));
    DiscImage disc(in, size);
    const auto executable = disc.read(disc.file({OMEGA_BOOT_EXECUTABLE}));
    std::istringstream bytes(std::string(executable.begin(), executable.end()));
    if (executable.size() != OMEGA_EXE_SIZE || sha256_stream(bytes) != OMEGA_EXE_SHA256)
        throw std::runtime_error(utf8(iso) + " is not the supported disc (its boot executable differs)");
    const derived::ElfImage elf(executable);
    const auto sources = ps2_save_icon_sources([&](std::uint32_t address, std::uint32_t length) {
        const auto s = elf.read(address, length);
        return std::vector<std::uint8_t>(s.begin(), s.end());
    });
    // 0036de50's size is the file's whole 2048-byte sectors: the PS2 game's own save (PCSX2's Mcd001.ps2) holds
    // MEMCARD.ICN as 40960 bytes for the 39224-byte disc file, the sector's zero padding included.
    return ps2_save_icon_files(sources, [&](const std::string& path) {
        auto extent = disc.file(backslash_path(path));
        extent.size = (extent.size + 2047) / 2048 * 2048;
        return disc.read(extent);
    });
}

std::optional<fs::path> iso_setting(const PortSettings& s) {
    for (const auto& [k, v] : s.other)
        if (k == "iso" && !v.empty()) return fs::path(std::u8string(v.begin(), v.end()));
    return std::nullopt;
}

namespace {
void set_other(PortSettings& s, const char* key, const std::string& value) {
    for (auto& [k, v] : s.other)
        if (k == key) {
            v = value;
            return;
        }
    s.other.emplace_back(key, value);
}
}  // namespace

void set_iso_setting(PortSettings& s, const fs::path& iso) { set_other(s, "iso", utf8(iso)); }

std::optional<VerifiedIso> iso_identity(const fs::path& iso) {
    std::error_code ec;
    const auto absolute = fs::absolute(iso, ec).lexically_normal();
    if (ec) return std::nullopt;
    const auto size = fs::file_size(absolute, ec);
    if (ec) return std::nullopt;
    const auto time = fs::last_write_time(absolute, ec);
    if (ec) return std::nullopt;
    return VerifiedIso{utf8(absolute), size, std::int64_t(time.time_since_epoch().count()), {}};
}

std::string format_verified_iso(const VerifiedIso& v) {
    return std::to_string(v.size) + ";" + std::to_string(v.write_time) + ";" + v.sha256 + ";" + v.path;
}

std::optional<VerifiedIso> parse_verified_iso(const std::string& value) {
    const auto a = value.find(';');
    const auto b = a == std::string::npos ? a : value.find(';', a + 1);
    const auto c = b == std::string::npos ? b : value.find(';', b + 1);
    if (c == std::string::npos) return std::nullopt;
    const std::string size = value.substr(0, a), time = value.substr(a + 1, b - a - 1), sha = value.substr(b + 1, c - b - 1);
    auto digits = [](const std::string& t, bool sign) {
        std::size_t i = sign && !t.empty() && t[0] == '-' ? 1 : 0;
        if (t.size() <= i || t.size() > 19 + i) return false;
        return std::all_of(t.begin() + std::ptrdiff_t(i), t.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
    };
    if (!digits(size, false) || !digits(time, true) || sha.size() != 64 ||
        !std::all_of(sha.begin(), sha.end(), [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }) ||
        c + 1 >= value.size())
        return std::nullopt;
    return VerifiedIso{value.substr(c + 1), std::stoull(size), std::stoll(time), sha};
}

std::optional<VerifiedIso> verified_iso_setting(const PortSettings& s) {
    for (const auto& [k, v] : s.other)
        if (k == "iso_verified") return parse_verified_iso(v);
    return std::nullopt;
}

void set_verified_iso_setting(PortSettings& s, const VerifiedIso& v) { set_other(s, "iso_verified", format_verified_iso(v)); }

bool verified_iso_matches(const std::optional<VerifiedIso>& cached, const VerifiedIso& now, const std::string& expected_sha256) {
    if (!cached || cached->sha256 != expected_sha256 || cached->size != now.size || cached->write_time != now.write_time)
        return false;
    // Windows paths compare without case.
#ifdef _WIN32
    return CompareStringOrdinal(widen(cached->path).c_str(), -1, widen(now.path).c_str(), -1, TRUE) == CSTR_EQUAL;
#else
    return strcasecmp(cached->path.c_str(), now.path.c_str()) == 0;  // the Vita's FAT / exFAT names: the same
#endif
}

#ifdef _WIN32
int run(int argc, wchar_t** argv, int (*play)(int, wchar_t**)) {
    Options o;
    try {
        o = parse_options(argc, argv);
    } catch (const std::exception& e) {
        show_error(std::string("Launcher arguments: ") + e.what());
        return 2;
    }
    g_dialogs = o.dialogs;
    try {
        const fs::path base = install_folder();
        fs::current_path(base);
        // A writable install: everything the session writes is in local/.
        {
            std::error_code ec;
            fs::create_directories("local", ec);
            std::ofstream probe("local/.write_test", std::ios::trunc);
            if (ec || !probe) {
                show_error("The game folder " + utf8(base) + " is not writable (the settings, the memory card and the "
                           "derived data are kept in its local\\ folder). Move the folder somewhere you own, e.g. Documents.");
                return 3;
            }
            probe.close();
            fs::remove("local/.write_test", ec);
        }
        redirect_output_if_detached();
        std::cout << "Launcher (port-only, checkpoint 132win8): install folder " << utf8(base) << "\n";
        if (o.card) {
            // Checkpoint 132win13 (port-only): the memory card tools need no disc image.
            CardCommand c = *o.card;
            c.card_root = fs::path("local") / "frontend_card";
            const auto card_settings = read_port_settings(o.settings);
            c.backups = card_backups_setting(card_settings);
            // Checkpoint 132win14: the save icons from --iso or the iso= image (no search, no dialog).
            if (const auto image = o.iso ? o.iso : iso_setting(card_settings)) {
                c.icons = [image = *image] { return save_icon_files_from_iso(image); };
            } else {
                c.icons_missing = "no disc image: give --iso, or start the game once so iso= is set";
            }
            std::string summary;
            const int code = run_card_command(c, std::cout, summary);
            std::cout.flush();
            if (code != 0) {
                show_error(summary);
                return code;
            }
            if (g_dialogs) MessageBoxW(nullptr, widen(summary).c_str(), title, MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            return 0;
        }
        if (o.derive_only) {
            if (!o.iso) throw std::runtime_error("--derive-data needs --iso");
            const fs::path folder = *o.derive_only;
            const auto local = fs::weakly_canonical(fs::current_path() / "local");
            const auto target = fs::weakly_canonical(fs::current_path() / folder);
            const auto rel = target.lexically_relative(local);
            if (rel.empty() || *rel.begin() == "..") throw std::runtime_error("--derive-data must name a folder under local/");
            if (auto problem = verify_and_derive(*o.iso, folder, true, std::nullopt, true); !problem.empty())
                throw std::runtime_error(problem);
            const auto files = derived::derived_files(folder);
            for (const auto& f : {files.graph, files.female_graph, files.actor_tuning, files.navigation_tuning}) {
                std::ifstream in(f, std::ios::binary);
                std::cout << "Derived (port-only): " << utf8(f) << " " << fs::file_size(f) << " bytes sha256 " << sha256_stream(in)
                          << "\n";
            }
            return 0;
        }

        // Checkpoint 132md5 (port-only): the mod panel, before any disc work (its changes are in the settings file,
        // read below). Shift held at the start opens it as --mod-panel does.
        if (o.mod_panel || (g_dialogs && !o.hidden && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0)) {
            PortSettings for_folder;
            try {
                for_folder = read_port_settings(o.settings);
            } catch (const std::exception&) {
            }
            ModPanelOptions panel{panel_mods_folder(o, for_folder), o.settings, o.mod_panel_script, o.hidden, {}};
            std::cout << "Launcher (port-only): mod panel (" << utf8(panel.mods_folder) << ")\n";
            if (run_mod_panel(panel) == ModPanelResult::close) {
                std::cout << "Launcher (port-only): closed from the mod panel\n";
                return 0;
            }
        }

        PortSettings settings;
        try {
            settings = read_port_settings(o.settings);
        } catch (const std::exception& e) {
            show_error(std::string("The settings file ") + utf8(o.settings) + " is malformed: " + e.what() +
                       "\n\nFix or delete that line (README-PLAY.md lists the keys).");
            return 4;
        }
        const fs::path derived_folder = fs::path("local") / "derived";
        std::vector<std::pair<fs::path, std::string>> tried;
        std::optional<fs::path> chosen;
        const auto cached = verified_iso_setting(settings);
        std::optional<VerifiedIso> verified;
        auto attempt = [&](const fs::path& iso, const char* how) {
            std::cout << "Launcher (port-only): disc image " << how << ": " << utf8(iso) << "\n";
            const auto problem = verify_and_derive(iso, derived_folder, false, cached, o.verify_iso, &verified);
            if (problem.empty()) {
                chosen = fs::absolute(iso);
                return true;
            }
            std::cout << "Launcher (port-only): rejected: " << problem << "\n";
            tried.emplace_back(iso, problem);
            return false;
        };
        if (o.iso) {
            if (!attempt(*o.iso, "from --iso")) {
                show_error(tried.back().second);
                return 5;
            }
        }
        if (!chosen) {
            if (const auto saved = iso_setting(settings)) attempt(*saved, "from the iso= setting");
        }
        if (!chosen && o.search) {
            std::vector<fs::path> roots{fs::current_path()};
            std::cout << "Launcher (port-only): searching for the disc image (" << OMEGA_ISO_SIZE << " bytes, *.iso)\n";
            for (const auto& c : find_iso_candidates(roots, OMEGA_ISO_SIZE, 2))
                if (attempt(c, "found in the install folder")) break;
            if (!chosen)
                for (const auto& c : find_iso_candidates(known_folders(), OMEGA_ISO_SIZE, 2))
                    if (attempt(c, "found in the user's folders")) break;
        }
        while (!chosen) {
            const std::string reason = tried.empty() ? std::string("No disc image was found.") : tried.back().second;
            const auto picked = ask_for_iso(reason);
            if (!picked) {
                show_error(tried.empty() ? "No disc image of Syphon Filter: The Omega Strain (USA) was found. Put your own .iso "
                                           "in the game folder, or start once with --iso <file>, or set iso=<file> in "
                                           "local\\port_settings.txt (README-PLAY.md, \"First run\")."
                                         : tried.back().second);
                return 5;
            }
            attempt(*picked, "selected");
        }
        const bool iso_changed = iso_setting(settings) != chosen;
        const bool verified_changed = verified && cached != verified;
        if (iso_changed || verified_changed) {
            if (iso_changed) set_iso_setting(settings, *chosen);
            if (verified_changed) set_verified_iso_setting(settings, *verified);
            write_port_settings(o.settings, settings);
            if (iso_changed)
                std::cout << "Launcher (port-only): iso=" << utf8(*chosen) << " written to " << utf8(o.settings) << "\n";
            if (verified_changed)
                std::cout << "Launcher (port-only): iso_verified=" << format_verified_iso(*verified) << " written to "
                          << utf8(o.settings) << "\n";
        }
        const auto arguments = play_arguments(*chosen, derived::derived_files(derived_folder), o);
        std::vector<std::wstring> storage{argc > 0 ? std::wstring(argv[0]) : std::wstring(L"OmegaStrain")};
        storage.insert(storage.end(), arguments.begin(), arguments.end());
        std::vector<wchar_t*> pointers;
        for (auto& s : storage) pointers.push_back(s.data());
        pointers.push_back(nullptr);
        std::cout << "Launcher (port-only): starting the front end and play session (" << arguments.size() << " arguments)\n";
        std::cout.flush();
        // Checkpoint 132win12 (port-only): crashes in the session leave a report in local\crashes (crash_report.hpp).
        crash::install({fs::absolute("local") / "crashes", g_dialogs});
        g_active = true;
        const int code = play(int(storage.size()), pointers.data());
        g_active = false;
        std::cout << "Launcher (port-only): session ended (exit " << code << ")\n";
        if (code != 0 && !g_failure_shown)
            show_error("The game stopped with exit code " + std::to_string(code) + ". The log is local\\omega_strain.log when "
                       "started without a console (README-PLAY.md, \"Troubleshooting\").");
        return code;
    } catch (const std::exception& e) {
        show_error(e.what());
        return 1;
    }
}

#endif  // _WIN32

}  // namespace omega::launcher
