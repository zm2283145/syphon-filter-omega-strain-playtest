#pragma once
// Port-only (checkpoint 132md5, research/MODDING.md "Managing mods"): the launcher's mod panel. OmegaStrain.exe started
// with --mod-panel, or with Shift held, opens it before the disc image is looked for: every folder of the mods folder
// (mods::list_mods: the resolution order, each mod's state, priority, name, version, author), the selected mod's
// details, and the conflicts of the enabled set (omega_mod_tool conflicts' lines). Its buttons:
//   Enable / Disable  mod.<folder>=1|0 in the port settings (the in-game MODS rows and omega_mod_tool do the same);
//   Move up / down    one place in the resolution order (the manifests' priority=, mods::move_mod);
//   All mods on / off mods=;
//   Refresh, Open folder (Explorer), Play (the launch goes on), Close (the launcher exits).
// Everything is written at once and applies when the game starts (nothing is loaded here). A Win32 window of standard
// controls (no game code; the play host's own window comes later).
//
// --mod-panel-script <file> drives it (evidence, tests): one command per line ('#' comments): select <folder>,
// toggle, up, down, all, refresh, capture <file.png> (the panel's window as PNG), play, close. The script ends with
// close unless it says play. With --hidden the window is never shown (captures still work).

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace omega::launcher {

struct ModPanelOptions {
    std::filesystem::path mods_folder;   // the session's (mods::session_mods_folder)
    std::filesystem::path settings;      // the port settings file
    std::optional<std::filesystem::path> script;
    bool hidden = false;
    std::function<void(const std::string&)> log;   // one line per change (default std::cout)
};

enum class ModPanelResult { play, close };

// Throws on a script error (an unknown command or folder, a failed capture).
ModPanelResult run_mod_panel(const ModPanelOptions& options);

}  // namespace omega::launcher
