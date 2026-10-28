#pragma once
// Port-only settings (checkpoint 132ob6, not the original): a small key=value text file for the options
// the port adds (the front end's "CPU Partner" row, port_options_row.hpp). The game's own options stay
// on the memory card (GuiGameOptions' save, 0029ad10); nothing here reaches the game record.
//
// File format: one "key=value" per line; blank lines and lines starting with '#' are kept as written.
// Known keys: coop_partner=0|1 (the CPU co-op partner, coop_partner.hpp; default 0) and fullscreen=0|1
// (checkpoint 132win: the game window's borderless fullscreen, host_window.hpp; default 0) and vsync=0|1
// (checkpoint 132win3: present on the display's vertical blank; default 1; the frame pacer holds the game's
// rate either way, frame_pacer.hpp; checkpoint 132win4: the front end's Options > Video VSYNC row writes it) and
// window=left,top,right,bottom (checkpoint 132win5: the game window's windowed outer rectangle in screen
// pixels, written at the session's end and restored at the next start through fit_saved_window, host_window.hpp;
// absent: the default placement). Checkpoint 132win6 reads two more groups from the kept unknown keys:
// controller_prompt=0|1 (the controller-removed popup, pad_connection.hpp) and bind.<profile>.<input>=<keys>
// (the keyboard / mouse bindings, input_bindings.hpp); checkpoint 132win7 mouse_look=0|1, mouse_sensitivity=<0.05..20>
// and mouse_invert_y=0|1 (mouse look, mouse_look.hpp); checkpoint 132win8 iso=<disc image path> (UTF-8; the
// launcher, launcher.hpp) and checkpoint 132win9 iso_verified=<size>;<write time>;<sha256>;<path> (the launcher's cached
// full-image verification); checkpoint 132win10 aspect=pillarbox|4:3|stretch, render_scale=<0.25..4>, frame_overlay=0|1 and
// frame_overlay_key=<key>|none (the presentation options, display_settings.hpp); checkpoint 132win12 background_audio=0|1
// (the sound while the window is unfocused, background_audio_setting). Unknown keys are kept and written back unchanged, and a write keeps the file's comments
// (update_port_settings_text). A malformed line or value throws.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace omega {

struct PortSettings {
    bool coop_partner = false;
    bool fullscreen = false;  // checkpoint 132win: borderless fullscreen (host_window.hpp)
    bool vsync = true;        // checkpoint 132win3: vertical-blank presents (frame_pacer.hpp)
    std::optional<std::array<int, 4>> window;  // checkpoint 132win5: the windowed outer rectangle (left, top, right, bottom)
    std::vector<std::pair<std::string, std::string>> other;  // unknown keys, in file order
};

// The default file, relative to the repository root the sandbox runs from.
inline constexpr const char* port_settings_default_file = "local/port_settings.txt";

PortSettings parse_port_settings(std::string_view text);
std::string format_port_settings(const PortSettings& s);
// Checkpoint 132win8: the file text after writing s over an existing file: comment, blank and other non-setting lines
// stay where they are, each key's first line takes its new value (repeats and dropped keys are removed), and keys the
// file did not have are appended. An empty file gives format_port_settings(s).
std::string update_port_settings_text(std::string_view existing, const PortSettings& s);
// A missing file reads as the defaults.
// Checkpoint 132win12 (port-only): background_audio=0|1 (kept in `other`; default 0): 0 silences the game's sound
// while its window is unfocused (audio_device.hpp set_background_muted), 1 keeps it playing. Throws on another value.
bool background_audio_setting(const PortSettings& s);
// Checkpoint 132ln1 (port-only, research/LAN_PLAY.md): lan_play=0|1 (kept in `other`; default 1 since 132ln1b, the user's
// request): 1 shows Play LAN on the online / offline select (GuiHost::port_lan_play), 0 hides it. Throws on another value.
bool lan_play_setting(const PortSettings& s);
// Checkpoint 132md1 (port-only, research/MODDING.md): mods=0|1 (kept in `other`; default 1): 0 turns every mod off
// (the original game's assets only, as with --no-mods); mods_folder=<folder> (UTF-8; default "mods" next to the game):
// where interactive runs look for mods. Throws on another mods value.
bool mods_setting(const PortSettings& s);
std::string mods_folder_setting(const PortSettings& s);
// Checkpoint 132md9 (port-only, research/MODDING.md "Texture memory"): mods_texture_cap=<MB> (kept in `other`; 0..65536,
// 0 = no cap): the memory cap on a level's mod replacement images (texture_replacement.hpp set_texture_memory_cap).
// Absent: nullopt (the platform's default: none on PC). Throws on another value.
std::optional<std::uint32_t> mods_texture_cap_setting(const PortSettings& s);
// Checkpoint 132md4: mod.<folder>=0|1 (kept in `other`): the player's choice for one mod folder over its manifest's
// enabled= (the Port Settings page's MODS rows, omega_mod_tool enable / disable). Keyed by the folder name upper-
// cased; the last line of a folder wins. Throws on another value.
std::vector<std::pair<std::string, bool>> mod_choices_setting(const PortSettings& s);
// Checkpoint 132md13: mod_volume.<folder>=<percent> (kept in `other`; 0..400): the player's audio volume for one mod
// over its manifest's audio_volume= (omega_mod_tool volume). Keyed by the folder name upper-cased; the last line of a
// folder wins. Throws on another value.
std::vector<std::pair<std::string, unsigned>> mod_volumes_setting(const PortSettings& s);
// Checkpoint 132pc3 (port-only, research/COOP.md): partner_friendly_fire=0|1 (kept in `other`; default 0): 0 shields the
// CPU co-op partner from the local player's and its allies' damage (port_cheats.hpp PortPartnerShield), 1 keeps the
// original damage paths. Throws on another value.
bool partner_friendly_fire_setting(const PortSettings& s);
// Checkpoint 132pw1 (port-only, research/COOP.md "The partner's weapons"): partner_loadout=auto|copy|record (kept in
// `other`; default auto): the CPU partner's level-start kit from the player's unlocked weapons (auto), the player's
// accepted loadout (copy) or its population record's item words (record, the kit before 132pw1). Throws on another
// value. partner_pickups=0|1 (default 1): the partner takes better weapons and rounds from backpacks and crates.
std::string partner_loadout_setting(const PortSettings& s);
bool partner_pickups_setting(const PortSettings& s);
// Checkpoint 132pw3 (port-only, research/COOP.md "Up to 3 CPU partners"): coop_partners=1|2|3 (kept in `other`; default
// 1): how many CPU partners join an offline level when the CPU partner is on. Throws on another value.
std::size_t coop_partners_setting(const PortSettings& s);
// Checkpoint 132pt1 (port-only): writes coop_partners=count (1..3; throws otherwise), the front end's CPU PARTNER row's count.
void set_coop_partners_setting(PortSettings& s, std::size_t count);
// Checkpoint 132win14 (port-only HiDPI): window_units=pixels (kept in `other`): the window= rectangle is in physical
// pixels (a per-monitor DPI aware build wrote it). Absent: it was written by an earlier, DPI-unaware build (96-DPI
// coordinates; host_window.hpp saved_window_pixels migrates it). Throws on another value.
bool window_units_pixels(const PortSettings& s);
void set_window_units_pixels(PortSettings& s);
// Checkpoint 132on1 (port-only, research/ONLINE.md §6.8): online_server=<host>[:<port>] (kept in `other`; default
// 127.0.0.1:10075): the omega_server the Online mode connects to. The host is an IPv4 address or a name; the port is
// 1..65535. Throws on an empty host or a bad port.
struct OnlineServerAddress {
    std::string host = "127.0.0.1";
    unsigned short port = 10075;
};
OnlineServerAddress online_server_setting(const PortSettings& s);
// Checkpoint 132on1 (port-only, research/ONLINE.md §7): online_platform_badges=0|1 (kept in `other`; default 1): 1 draws
// the players' platform badges ("PC", "PS2", ...) next to their names online, 0 hides them. Throws on another value.
bool online_platform_badges_setting(const PortSettings& s);
// Checkpoint 132on2g (port-only, research/ONLINE.md §6.15): online_chat_filter=0|1 (kept in `other`; default unset:
// omega_server's chat_filter default): the player's profanity filter choice for the lobby chat delivered to this
// client (sent as ext::ChatFilter). Throws on another value.
std::optional<bool> online_chat_filter_setting(const PortSettings& s);
// Checkpoint 132on2c (port-only, research/ONLINE.md §9.8): online_game_password=<text> (kept in `other`; default
// empty): the join password of the online games this agent creates (sent to omega_server as ext::GamePassword).
// Up to 31 characters (JoinGame's GamePassword field holds 32 with its terminator); throws on a longer one.
std::string online_game_password_setting(const PortSettings& s);
// Checkpoint 132on2e (port-only, research/ONLINE.md §6.11 / §6.12, research/ONLINE_DEPLOY.md §2): the in-level
// connection of online games (all kept in `other`).
//  * online_relay=auto|off|force (default auto, lower case): auto joins directly and, when that has no answer, through
//    omega_server's UDP relay; force always through the relay; off never (the relay capability is not announced).
//  * online_game_port=<0..65535> (default 3658, the original's DME peer port): the UDP port a hosted online game
//    opens; when it is taken, a system-picked one (logged). 0: always system-picked.
//  * online_upnp=0|1 (default 1): the host asks its router (UPnP IGD, then PCP, then NAT-PMP; 132on2f) to forward that
//    UDP port while the game is open.
// Each throws on another value.
std::string online_relay_setting(const PortSettings& s);
unsigned short online_game_port_setting(const PortSettings& s);
bool online_upnp_setting(const PortSettings& s);
// Checkpoint 132md5 (port-only, research/MODDING.md "LAN: the mod report at join"): lan_mod_policy=gameplay|any|
// cosmetic|none|equal (kept in `other`; default gameplay since 132md16, lower case): whom a LAN game this agent hosts
// lets join by their mods (the host's gameplay mods, cosmetic ones free; any; no gameplay-changing mods; no mods; the
// host's own mod files). Throws on another value.
std::string lan_mod_policy_setting(const PortSettings& s);

PortSettings read_port_settings(const std::filesystem::path& file);
void write_port_settings(const std::filesystem::path& file, const PortSettings& s);

}  // namespace omega
