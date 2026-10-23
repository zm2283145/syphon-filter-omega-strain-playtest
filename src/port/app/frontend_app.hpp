#pragma once
// Checkpoint 126: the PC window of the front end (omega_play --frontend). It
// runs FrontendSession (frontend_session.hpp) at a fixed 60 Hz from boot (the
// INTRO movie) through the title, agent load, online / offline select and
// command centre screens to the level load (state 0xe), feeding the pad word
// the game reads (the DualShock 2 held mask of 0055a44c) from the controller
// (gamepad_device.hpp) or the keyboard (keyboard_pad.hpp), drawing the GUI
// canvas (gui_draw + gui_renderer) or the movie picture, and playing the
// movie's PCM or (checkpoint 126c) the gameSound mix of the FRONTEND banks. Host choices are listed in research/FRONTEND.md "Checkpoint
// 126 front-end flow".
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace omega {

class DiscImage;
class GamepadDevice;
class GamepadSimulation;
class HostWindow;
class PadConnection;
class PortSettingsPage;
class PortSettingsStore;
namespace lan {
class LanNetCore;
}
struct InputBindings;
struct PortSettings;
struct AgentStatistics;
struct Texture;

struct FrontendOptions {
    std::filesystem::path iso;            // the movies stream from it (a second handle)
    std::filesystem::path card_folder;    // FolderAgentCard root (the PC memory card)
    std::filesystem::path memcard;        // optional PCSX2 .ps2 image imported into card_folder
    // Scripted input instead of the controller / keyboard: lines "N BUTTONS"
    // hold BUTTONS ("-" none, else names joined by '+': cross circle square
    // triangle start select up down left right l1 r1 l2 r2 l3 r3) for N frames;
    // "#" starts a comment. The run ends (no hand-off) after the last line.
    std::filesystem::path script;
    std::filesystem::path capture_dir;    // BMP of every screen change (and capture_frames)
    std::set<std::int64_t> capture_frames;
    bool hidden = false;                  // no window (needs a script)
    bool audio = true;
    // Port-only test (checkpoint 132win7, play's --test-audio-device): simulated output device events by front-end
    // frame counted from each pass's start (audio_output.hpp; "0:absent" first: no device at startup). Non-empty opens a muted
    // device even when hidden.
    std::string test_audio_device;
    // Prints the process's private bytes and handle count at each view change
    // (tools\check_frontend_memory.ps1 compares them across attract cycles).
    bool memory_log = false;
    std::filesystem::path audio_wav;      // the front end's gameSound mix (48 kHz stereo)
    // Port testing convenience (--unlock-all-levels): Game_ParseCommandLine's
    // developer switch "-r" (0013d270: 004ffbb5 = 0, "Not restricting
    // levels."). Retail's StringPool_Load -> Game_LoadIni (00165744) rewrites
    // 004ffbb5 = 1 right after the argv loop; the port applies the write
    // after that boot value.
    bool unlock_all_levels = false;
    // PORT-ONLY cheat (--unlock-all, port_cheats.hpp): FrontendSession::port_unlock_all
    // at start; the session re-applies it after every refill.
    bool unlock_all = false;
    // PORT-ONLY test (checkpoint 132cc10, --frontend-card-sim): the simulated card state
    // (agent_card.hpp parse_agent_card_simulation; empty: none). A script line "card <spec>" changes it.
    std::string card_simulation;
    // Port-only (checkpoint 132ob6): the port settings file behind the title options' "CPU Partner"
    // row (port_options_row.hpp); empty: the rows keep their values in memory (checkpoint 132win2: the
    // CPU PARTNER and FULLSCREEN rows are always present).
    std::filesystem::path port_settings;
    // Checkpoint 132win (port-only): the session's game window and device (host_window.hpp), kept by the
    // caller across the play; null: the front end makes its own (unless hidden).
    HostWindow* host = nullptr;
    // Checkpoint 132win3 (port-only, frame_pacer.hpp): the live loop sleeps on a high-resolution timer until
    // its next 1/60 s step instead of a 1 ms poll; with vsync on a refresh that is a multiple of 60 Hz each
    // frame is held for that many refreshes. Neither changes the session's steps.
    bool vsync = true;
    bool frame_pacing = true;
    bool frame_cadence = true;  // checkpoint 132win13: --frame-cadence off (frame_pacer.hpp LoopPacing::set_cadence_allowed)
    // Checkpoint 132win4 (port-only): --frame-times: each front-end pass's paced frame intervals (one value per
    // line, ms) to <stem>_frontend<k><ext> (k = 0 for the boot pass, then 1, 2... after each level end). Scripted
    // and hidden runs are paced when frame_pacing is set (--frame-pacing on), one step per loop as before.
    std::filesystem::path frame_times;
    // Port-only diagnostics (checkpoint 132win5), by session frame (FrontendSession::frames() after the step):
    // test_device_lost re-creates the window's device before that frame's draw (a loss at the frame boundary);
    // test_device_lost_sweep starts a sweep there: the loop's k-th iteration from it injects the loss at its
    // k-th checked D3D call (device_lost.hpp), k = 0, 1, 2... until an iteration has fewer calls, so every
    // checked call of those frames (uploads, the movie texture, the draw, the present) loses the device once;
    // window_captures writes the window's back buffer (the fitted frame, BMP) into capture_dir.
    std::set<std::int64_t> test_device_lost;
    std::set<std::int64_t> test_device_lost_sweep;
    std::set<std::int64_t> window_captures;
    // Port-only diagnostic (checkpoint 132win5, --frontend-profile <ms>): each loop iteration's host time split
    // into the session step (its disc / decode services separately), the sound mix, the texture uploads and
    // the draw + present; iterations over <ms> are logged, and a summary at each pass's end. 0: off.
    double profile_ms = 0.0;
    // Port-only diagnostics (checkpoint 132win5): test_minimize reports the window minimised (simulate_minimized)
    // from front-end frame first for second frames (the front end keeps stepping, nothing is presented);
    // test_close posts Alt+F4 to the window at that frame (the loop ends at the close request). -1: off.
    std::int64_t test_minimize_at = -1, test_minimize_frames = 0;
    std::int64_t test_close = -1;
    // Checkpoint 132win12 (port-only test): crash_report.hpp's test_crash(kind) at this front-end frame (-1: off).
    std::string test_crash_kind;
    std::int64_t test_crash_at = -1;
    // Checkpoint 132win12 (port-only tests): the window reported unfocused (HostWindow::simulate_focus) from
    // front-end frame test_focus_loss_at for test_focus_loss_frames frames (-1: off); Alt+Enter sent to the window
    // (HostWindow::simulate_alt_enter) at each frame of test_alt_enter.
    std::int64_t test_focus_loss_at = -1, test_focus_loss_frames = 0;
    std::vector<std::int64_t> test_alt_enter;
    // Checkpoint 132win6 (port-only): the session's controller (one GamepadDevice: one Sony HID scan thread, no
    // spurious disconnect at the hand-offs) and its connection state (pad_connection.hpp: the controller-removed
    // state 0055a4ec = 0 behind the controller check 00465870), kept by the caller across the play; null: the
    // front end keeps its own. menu_bindings: the keyboard / mouse "menu" profile (input_bindings.hpp); null:
    // the defaults. test_pads (--frontend-test-pads): a simulated device list by front-end frame (the frame
    // number after the step) for hidden tests, replacing the real controller's presence; scripted runs use it
    // with their script's buttons.
    GamepadDevice* gamepad = nullptr;
    PadConnection* pads = nullptr;
    const InputBindings* menu_bindings = nullptr;
    std::string test_pads;
    // Checkpoint 132win12 (port-only tests): pad_simulation, the simulated controller devices behind gamepad
    // (gamepad_simulation.hpp), driven by test_pad_devices (--frontend-test-pad-devices, by front-end frame);
    // scripted runs then read the device for the controller's presence (their script keeps the buttons).
    GamepadSimulation* pad_simulation = nullptr;
    std::string test_pad_devices;
    // Port-only (checkpoint 132win11): settings_store, the port settings shared with the play and the Port
    // Settings page (port_settings_page.hpp); null: the front end makes its own from port_settings. settings_page:
    // the page the Options' PORT SETTINGS row opens (null: no row); the front end listens to its changes (menu
    // bindings, vsync, the frame overlay) and feeds it the keyboard while it waits for a key.
    std::shared_ptr<PortSettingsStore> settings_store;
    PortSettingsPage* settings_page = nullptr;
    // Port-only (checkpoint 132win11): the frame-time overlay (frame_overlay.hpp) on the front end's window: on at
    // start, its toggle key (nullopt: none), and test_overlay_key posts the key to the window at those front-end
    // frames (a hidden test of the toggle).
    bool frame_overlay = false;
    std::optional<std::uint8_t> frame_overlay_key = std::uint8_t{0x72};
    std::set<std::int64_t> test_overlay_key;
    // Port-only (checkpoint 132ln1, research/LAN_PLAY.md): LAN Play on the online / offline select (GuiHost::
    // port_lan_play), from --lan-play or the settings key lan_play=1. It enters the LAN-mode ONLINECN (checkpoint
    // 132ln2: HOST LAN GAME / JOIN LAN GAME).
    bool lan_play = false;
    // Port-only (checkpoint 132ln3): the LAN session (LanSession, lan_session.hpp) that MSNSETUP's begin_mission
    // hosts through in LAN mode. lan_loopback (--lan-loopback) puts it on an in-memory network (LanLoopbackNetwork)
    // with no beacons, so an automated run opens no socket on the real network.
    bool lan_loopback = false;
    // Port-only test (checkpoint 132ln4, --lan-loopback-peer=<mission>): with lan_loopback, a second LAN session
    // on the same in-memory network hosts a game of that online mission (0..8) as agent "LOOPBACK PEER", so a hidden
    // run can join a game in JOINMIS without a second process; it answers JOIN_REQ while the front end pumps it.
    std::optional<std::int32_t> lan_loopback_peer;
    // Port-only (checkpoint 132on2, research/ONLINE.md section 9, --online-server=<host>[:<port>]): the omega_server
    // the Online mode's lobby (OnlineLobbyHost, DNAS bypassed) connects to; unset, the settings key online_server
    // (when present) gives it, and with neither the Online mode keeps the original's unhosted lobby start.
    std::optional<std::string> online_server;
    // Port-only (checkpoint 132on2c, --online-game-password <text>): the join password of the online games this agent
    // creates; unset, the settings key online_game_password (empty: no password).
    std::optional<std::string> online_game_password;
    // Port-only (checkpoint 132on2e; --online-relay auto|off|force, --online-game-port <n>, --online-upnp 0|1): the
    // in-level connection of online games; unset, the settings keys online_relay / online_game_port / online_upnp.
    std::optional<std::string> online_relay;
    std::optional<std::uint16_t> online_game_port;
    std::optional<bool> online_upnp;
    // Port-only test (checkpoint 132on2f, --online-portmap-test pcp|natpmp|refuse|silent): the host's port mapping
    // goes to an in-process fake PCP / NAT-PMP gateway on 127.0.0.1 (UPnP off), even on a loopback socket.
    std::optional<std::string> online_portmap_test;
    // Port-only (checkpoint 132ln5f, --lan-address <ip>): the LAN session's real UDP sockets bound to that one local
    // address (host order) instead of INADDR_ANY, with no broadcasts and no beacon listener: the browser asks
    // ip:47524 directly. With 127.0.0.1, two instances on one machine play over loopback UDP and no firewall
    // rule is needed (a socket bound to 127.0.0.1 is unreachable from other machines).
    std::optional<std::uint32_t> lan_address;
    // Port-only (checkpoint 132md5, research/MODDING.md "LAN: the mod report at join", --lan-mod-policy): the hosted
    // LAN games' mod policy (any / cosmetic / none / equal) over the settings key lan_mod_policy.
    std::optional<std::string> lan_mod_policy;
};

struct FrontendResult {
    std::optional<std::string> level;     // set at the hand-off (state 0xe)
    bool closed = false;                  // port-only (132win5): the game window's close request ended it
    std::int64_t frames = 0;
    std::vector<std::string> captures;
};

// Parses one script line's buttons ("-" or names joined by '+').
std::uint16_t frontend_buttons(const std::string& text);

class FrontendSession;

// Checkpoint 132cc2: the front end kept alive across a level. run() stops at
// the hand-off (state 0xe) and releases its renderer and audio device for the
// play (the game window stays: checkpoint 132win, host_window.hpp); the play's level end (00335830 on session().agent) is followed by
// resume_after_level, which sets the state 001690f0 leaves (0x0c, the EOL
// movie, or 1) and continues the same session (renderer remade, script lines
// left at the hand-off driving it) to the next hand-off, close or script end.
class FrontendApp {
public:
    FrontendApp(DiscImage& disc, const FrontendOptions& options);
    ~FrontendApp();
    FrontendApp(const FrontendApp&) = delete;
    FrontendApp& operator=(const FrontendApp&) = delete;
    FrontendResult run();
    FrontendSession& session();
    // Checkpoint 132f11: the ZEUS.HOG / MANTEX pictures by slot (FrontendDisc::zeus_images), which the debrief's
    // GuiEndGameAwards loads through session() (0043c890) and the play's GUI renderer draws.
    const std::map<std::uint32_t, Texture>& zeus_images() const;
    FrontendResult resume_after_level(std::uint8_t state_50, std::uint8_t previous_51);
    // Port-only (checkpoint 132win3): the CPU PARTNER / FULLSCREEN rows' current values (the settings file,
    // or the values kept in memory without one) and where they live, read by the play at each level hand-off.
    PortSettings port_settings() const;
    std::string port_settings_source() const;
    // Port-only (checkpoint 132win4): the presents' vsync now (FrontendOptions::vsync, then the VSYNC row's
    // toggles), read by the play at each level hand-off.
    bool vsync() const;
    // Port-only (checkpoint 132ln5e): a LAN network game's DME.cc game side after the hand-off ran 001693f0's
    // network sequence up to 001350e0 (LanNetCore::level_load_begin); null otherwise. The play hosts the rest.
    lan::LanNetCore* lan_core();
    // Port-only (checkpoint 132md15, research/MODDING.md "Mod changes at the next level"): a LAN or online session was
    // opened this run (a LAN game, its browser, or the online lobby): the peers checked this mod set, so the play does
    // not change it at a level boundary.
    bool network_session() const;
    // Port-only (checkpoint 132on2d, research/ONLINE.md §3.6): an online game's lobby side in the level: the frame
    // (004536b0 state 10 -> 004446e0(1): PlayerReport / WorldReport every 15 s) and the level's end (World_Teardown's
    // network branch 0013493c: 0044e340's record upload, then 0042b1f0 the game left). `live`: the level's
    // statistics (null: the agent's). Both do nothing outside an online game. The frame returns the connection-path
    // notices (132on2f, port-only; none for an original-kind session) for the level's HUD ticker.
    std::vector<std::string> online_level_frame();
    void online_level_end(const AgentStatistics* live, bool completed);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

FrontendResult run_frontend(DiscImage& disc, const FrontendOptions& options);

}  // namespace omega
