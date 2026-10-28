#pragma once
// Port-only (checkpoint 132ob6, not the original): the "CPU Partner" on/off row the port adds to the
// title screen's options (GAMEOPT.GUI, GuiGameOptions 0029b360) for the CPU co-op partner
// (coop_partner.hpp). The original's offline options have no such row: it is a copy of the VIBRATION
// row (label_rumble, textbackground6, value_rumble: the same fonts, colours and decorator IE nodes)
// placed one row step below it, the step being the file's value_rumble - value_freelooky offsets (rect y
// and decorator translation z). The value reads MB_On / MB_Off (the VIBRATION strings); Cross toggles it (132pt1: OFF / 1 / 2 / 3)
// and writes the port settings file (port_settings.hpp) at once, outside the memory-card save. The
// in-game options (GAMEOPT2) get no row: the partner is chosen before the level loads.

#include "frontend_session.hpp"
#include "port_settings.hpp"
#include "port_settings_page.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace omega {

inline constexpr const char* cpu_partner_label = "label_cpupartner";
inline constexpr const char* cpu_partner_background = "textbackground_cpupartner";
inline constexpr const char* cpu_partner_button = "value_cpupartner";
inline constexpr const char* cpu_partner_text = "CPU PARTNER";
inline constexpr float cpu_partner_label_width = 199.9F;   // label_manualaim's box width (label_rumble's 110.3 wraps the text)

// Port-only (checkpoint 132win): the "FULLSCREEN" on/off row of the Video page (video_grp), a copy of the
// VIDEO MODE row (label_videomode, textbackground107, value_videomode) one row step (the control page's,
// as above) below it. Cross toggles the game window's borderless fullscreen (host_window.hpp) and writes
// fullscreen=0|1 to the port settings file; Alt+Enter / F11 do the same at any time.
inline constexpr const char* fullscreen_label = "label_fullscreen";
inline constexpr const char* fullscreen_background = "textbackground_fullscreen";
inline constexpr const char* fullscreen_button = "value_fullscreen";
inline constexpr const char* fullscreen_text = "FULLSCREEN";
// Port-only (checkpoint 132win4): the "VSYNC" on/off row below FULLSCREEN (another VIDEO MODE copy). Cross toggles
// whether the game window presents on the display's vertical blank (frame_pacer.hpp: only the presents change,
// the game's fixed step does not) and writes vsync=0|1 to the port settings file.
inline constexpr const char* vsync_label = "label_vsync";
inline constexpr const char* vsync_background = "textbackground_vsync";
inline constexpr const char* vsync_button = "value_vsync";
inline constexpr const char* vsync_text = "VSYNC";
// The "PRESS [] TO SAVE" group (optionmain_grp, shown on every page), moved down with the port rows.
// Port-only (checkpoint 132win11): the "PORT SETTINGS" row below CPU PARTNER (a copy of the CPU PARTNER row, one
// control-page row step down). Cross opens the Port Settings page (port_settings_page.hpp, PortGuiSettings); its
// value reads port_settings_open_text.
inline constexpr const char* port_settings_label = "label_portsettings";
inline constexpr const char* port_settings_background = "textbackground_portsettings";
inline constexpr const char* port_settings_button = "value_portsettings";
inline constexpr const char* port_settings_text = "PORT SETTINGS";
inline constexpr const char* port_settings_open_text = "OPEN";
inline constexpr const char* save_message = "squarebutton_message";
// Checkpoint 132win4: the Video page's port rows (FULLSCREEN, VSYNC) and the save message below VIDEO MODE are
// spaced at this fraction of the control page's row step (39.1 units: 28.2), so VIDEO MODE, FULLSCREEN, VSYNC and
// "PRESS [] TO SAVE" keep one even rhythm above the footer buttons (z -200) instead of overlapping it.
inline constexpr float video_row_step_scale = 0.72F;

// Checkpoint 132pt1 (port-only, the user's request): the CPU PARTNER row cycles OFF, 1, 2, 3 (the number of CPU partners,
// research/COOP.md "Up to 3 CPU partners"): Cross writes coop_partner=0|1 and coop_partners=1..3 (OFF keeps the count).
// The value reads the game's MB_Off string for OFF (the VIBRATION row's) and the count's digit otherwise.
// The next value after `count` (0: OFF).
std::size_t cpu_partner_row_next(std::size_t count);
// The value's text for the settings (strings: GetString 003fdb50; a missing MB_Off shows nothing, as go_refresh).
std::string cpu_partner_row_text(const PortSettings& s, const std::function<std::optional<std::string>(const std::string&)>& strings);

struct PortToggleRow {
    std::function<bool()> get;       // the state now (the window's fullscreen, the presents' vsync)
    std::function<void(bool)> set;   // applies it
};
using FullscreenRow = PortToggleRow;
using VsyncRow = PortToggleRow;

// Adds the row to GAMEOPT's control_grp (each copy right after its source node, so the Down order is
// MANUAL AIM, FREELOOK X, FREELOOK Y, VIBRATION, CPU PARTNER). Throws when the file lacks the rows it
// copies; a second call is a no-op.
void inject_cpu_partner_row(GuiNode& root);
// Adds the FULLSCREEN and VSYNC rows to GAMEOPT's video_grp (Down order GAMMA, VIDEO MODE, FULLSCREEN, VSYNC),
// video_row_step_scale row steps apart below VIDEO MODE, and moves "PRESS [] TO SAVE" the same distance below
// VSYNC (132win4; 132win/132win2 had FULLSCREEN alone, a full step down). As above.
void inject_video_rows(GuiNode& root);
// Adds the PORT SETTINGS row below CPU PARTNER (inject_cpu_partner_row first). As above.
void inject_port_settings_row(GuiNode& root);

// Binds the rows to a front-end session's services: the GAMEOPT patch, the values' text on wake and the
// Cross toggles, reading and writing settings_file (empty: values kept in memory, nothing read or written, as
// for scripted runs without --port-settings). No FULLSCREEN / VSYNC rows without `fullscreen`'s functions; the
// VSYNC row shows and sets `vsync` (when given; else the settings' vsync alone).
// Returns the rows' settings reader (checkpoint 132win3): the file, or the in-memory values without one, so
// the play hand-off sees a CPU PARTNER toggled in a front end that has no settings file.
std::function<PortSettings()> bind_cpu_partner_row(FrontendServices& services, std::filesystem::path settings_file,
                                                   FullscreenRow fullscreen = {}, VsyncRow vsync = {});
// Checkpoint 132win11: the same over a settings store shared with the Port Settings page (and the play); with a
// page, the PORT SETTINGS row is added and opens it (services.port_rows.settings).
std::function<PortSettings()> bind_port_settings_rows(FrontendServices& services, std::shared_ptr<PortSettingsStore> store,
                                                   FullscreenRow fullscreen = {}, VsyncRow vsync = {},
                                                   PortSettingsPage* page = nullptr);

}  // namespace omega
