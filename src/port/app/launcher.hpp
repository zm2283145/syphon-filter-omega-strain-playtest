#pragma once
// Port-only (checkpoint 132win8, not the original): the player-facing launcher. OmegaStrain.exe (and omega_play with no
// arguments or --launch) finds the user's own disc image, verifies it, derives the play host's disc-derived data
// natively (derived_data.hpp) and then runs the usual one-process front end -> play session (play_main.cpp) with the
// argument list tools/play_toronto1.ps1 -Frontend passes. Errors are shown in dialogs instead of console throws.
//
// The install folder (the folder holding config/disc_us.json: OmegaStrain.exe's own, else the working directory) becomes
// the working directory; everything the session writes stays in its local/ folder (port_settings.txt, the PC memory card
// folder frontend_card, derived/, omega_strain.log). The disc image path is the port setting iso=<path> (port_settings.hpp
// keeps the unknown key); first run order: --iso, the iso= setting, a search for a *.iso of the supported size (the
// install folder two levels deep, then the user's Downloads, Documents and Desktop folders), then a file dialog.
//
// Checkpoint 132win9: the full SHA-256 (about 6 s) runs once. A verified image is cached as the port setting
// iso_verified=<size>;<write time>;<sha256>;<path> (the write time is the file system's 100 ns tick count); a later
// start whose image has the same absolute path, size and write time and whose cached SHA-256 is the supported disc's
// skips the hash (the play host is told through remember_verified_iso). Any change, or --verify-iso, hashes again.
//
// Launcher options (anything else is passed on to the play host after the synthesized list):
//   --launch          omega_play: launcher mode with arguments (no arguments at all also selects it; OmegaStrain.exe
//                     is always in launcher mode)
//   --iso <file>      the disc image (saved to iso= once verified)
//   --no-dialogs      errors go to the log / stderr only (scripted runs; --hidden implies it)
//   --no-search       no automatic search for the image (only --iso, the iso= setting and the file dialog)
//   --verify-iso      hash the whole image again even when iso_verified= says it was verified (checkpoint 132win9)
//   --derive-data <local folder>  verify --iso, write the derived files there, report their SHA-256s and exit
//                     (a check of the extractor ports against the Python tools' outputs)
//   --card-export <file.ps2> | --card-import <file.ps2> [--card-replace] | --card-restore <backup folder>
//                     (checkpoint 132win13) the memory card tools (card_tools.hpp) on local/frontend_card, without a disc
//                     image; the result is shown in a dialog (and the log) and the launcher exits
//   --mod-panel       (checkpoint 132md5) the mod panel (launcher_mod_panel.hpp) before the disc image is looked for:
//                     Play goes on with the launch, Close exits. Holding Shift while the launcher starts (with dialogs)
//                     opens it too. The mods folder is the play extras' --mods, else the mods_folder= setting, else mods.
//   --mod-panel-script <file>  (132md5) the panel driven by a script (implies --mod-panel; evidence, tests)

#include "card_tools.hpp"
#include "derived_data.hpp"
#include "port_settings.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace omega::launcher {

struct Options {
    std::optional<std::filesystem::path> iso;
    std::optional<std::filesystem::path> derive_only;
    bool dialogs = true;
    bool hidden = false;
    bool search = true;  // false: --no-search
    bool verify_iso = false;  // --verify-iso: ignore the cached verification
    std::filesystem::path settings = port_settings_default_file;
    std::vector<std::wstring> extra;  // passed on to the play host
    std::optional<CardCommand> card;  // --card-export / --card-import / --card-restore (the file made absolute)
    bool mod_panel = false;  // --mod-panel (checkpoint 132md5)
    std::optional<std::filesystem::path> mod_panel_script;  // --mod-panel-script (made absolute)
};

// No arguments, or argv[1] is --launch / --derive-data (omega_play: the scripted command line stays the default).
bool is_launch(int argc, const wchar_t* const* argv);
// OmegaStrain.exe (the stem, any case) is always the launcher: its arguments are launcher options plus play extras.
bool is_launcher_executable(const std::filesystem::path& exe);
// This process's executable (GetModuleFileNameW).
std::filesystem::path module_path();
Options parse_options(int argc, const wchar_t* const* argv);

// The play host's arguments (argv[0] excluded): play_toronto1.ps1 -Frontend's list over the derived files, then extra.
// --audio-out is left out of a --hidden run (the play host rejects real-time output there).
// Checkpoint 132md5: the mods folder the play session will use (the extras' --mods <folder>, else mods_folder=, else
// mods), for the mod panel.
std::filesystem::path panel_mods_folder(const Options& o, const PortSettings& s);

std::vector<std::wstring> play_arguments(const std::filesystem::path& iso, const derived::DerivedFiles& files, const Options& o);

// Regular *.iso files (any case) of exactly `size` bytes under each root, at most max_depth folder levels down
// (directory junctions are followed; unreadable folders are skipped), roots in order, each root's files sorted.
std::vector<std::filesystem::path> find_iso_candidates(const std::vector<std::filesystem::path>& roots, std::uint64_t size,
                                                       int max_depth);

// Checkpoint 132win14 (port-only): the PS2 save icon files 0040d740 writes (MEMCARD.ICN, icon.sys; ps2_save_icons.hpp)
// made from the disc image: its boot executable (size and SHA-256 checked) and GAMEDATA\COMMON\MEMCARD.ICN. Throws
// when the image cannot be read or is not the supported disc. The card tools' CardCommand::icons.
std::vector<Ps2CardFile> save_icon_files_from_iso(const std::filesystem::path& iso);

// The iso= port setting (UTF-8 in the file).
std::optional<std::filesystem::path> iso_setting(const PortSettings& s);
void set_iso_setting(PortSettings& s, const std::filesystem::path& iso);

// Checkpoint 132win9: the identity of a verified image (the iso_verified= port setting).
struct VerifiedIso {
    std::string path;  // absolute, UTF-8
    std::uint64_t size = 0;
    std::int64_t write_time = 0;  // std::filesystem::file_time_type ticks
    std::string sha256;
    bool operator==(const VerifiedIso&) const = default;
};
// The file's current path / size / write time (sha256 left empty); nullopt when it cannot be read.
std::optional<VerifiedIso> iso_identity(const std::filesystem::path& iso);
// iso_verified= parsed (nullopt when absent or malformed: a malformed value only means "hash again").
std::optional<VerifiedIso> verified_iso_setting(const PortSettings& s);
void set_verified_iso_setting(PortSettings& s, const VerifiedIso& v);
std::string format_verified_iso(const VerifiedIso& v);
std::optional<VerifiedIso> parse_verified_iso(const std::string& value);
// True when the cache entry names this file as it is now and records the expected SHA-256.
bool verified_iso_matches(const std::optional<VerifiedIso>& cached, const VerifiedIso& now, const std::string& expected_sha256);

// The launcher: returns the process exit code. play is the play host's entry (play_main.cpp).
int run(int argc, wchar_t** argv, int (*play)(int, wchar_t**));

// Set while the launcher runs the play host: its top-level failure (play_main's final catch) is shown in a dialog.
// crash_files (checkpoint 132win12, port-only): the crash reporter's files (crash_report.hpp), named in the dialog.
void report_play_failure(const std::string& message, const std::string& crash_files = {});
bool active();

}  // namespace omega::launcher
