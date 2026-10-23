#include "frontend_app.hpp"

#include "agent_card.hpp"
#include "card_backup.hpp"
#include "audio_device.hpp"
#include "crash_report.hpp"
#include "audio_output.hpp"
#include "disc.hpp"
#include "frontend_disc.hpp"
#include "frontend_session.hpp"
#include "frame_fit.hpp"
#include "frame_overlay.hpp"
#include "frame_pacer.hpp"
#include "frame_profile.hpp"
#include "host_window.hpp"
#include "game_sound.hpp"
#include "gamepad_device.hpp"
#include "gamepad_reports.hpp"
#include "gamepad_simulation.hpp"
#include "gui_draw.hpp"
#include "gui_renderer.hpp"
#include "ie_draw.hpp"
#include "input_bindings.hpp"
#include "keyboard_pad.hpp"
#include "lan_loopback.hpp"
#include "lan_mod_gate.hpp"
#include "lan_net_core.hpp"
#include "lan_session.hpp"
#include "mod_fingerprint.hpp"
#include "movie_subtitle_overlay.hpp"
#include "online_lobby_host.hpp"
#include "pad_connection.hpp"
#include "port_options_row.hpp"
#include "port_settings_page.hpp"
#include "display_settings.hpp"
#include "ps2_card_image.hpp"
#include "sound_iop_host.hpp"
#include "stats_record.hpp"

#include <windows.h>
#include <psapi.h>
#include <wrl/client.h>

#include "device_lost.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iostream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>

namespace omega {

namespace {

// FrontendOptions::memory_log: the process's private bytes and handles.
std::string process_memory() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof counters;
    DWORD handles = 0;
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof counters) ||
        !GetProcessHandleCount(GetCurrentProcess(), &handles))
        return "memory unavailable";
    return "private " + std::to_string(counters.PrivateUsage / 1024) + " KB, handles " + std::to_string(handles);
}

struct ScriptStep {
    std::int64_t frames = 0;
    std::uint16_t held = 0;
    std::optional<std::string> card;  // PORT-ONLY test (132cc10): a "card <spec>" line before this step
    std::optional<std::string> keys;  // PORT-ONLY test (132on2b): a "keys TEXT" line before this step
    // PORT-ONLY test (132on2c): "touch PATH" makes PATH when this step begins; "waitfile PATH" holds this step
    // (no buttons, its frames not counted) until PATH exists. Two scripted instances sync through them.
    std::optional<std::string> touch_file, wait_file;
    // PORT-ONLY test (132on2c): "waitgames N" holds this step the same way until the online lobby's game list (the
    // JOINMIS rows, refreshed about once a second) holds at least N games: the front end runs unpaced, so a frame
    // count is no measure of the refresh. With both, the step waits for both. 132ln5h14: without an online lobby,
    // the LAN browser's games (LanSession::games, the JOINMIS rows of the LAN browse, refreshed every update).
    std::optional<std::size_t> wait_games;
    // PORT-ONLY test (132on2h): "capture NAME" keeps the last drawn frame as a --frontend-capture BMP when this step
    // begins (a screen whose content changes after it settled, e.g. a list filled by the server).
    std::optional<std::string> capture;
};

// PORT-ONLY test (checkpoint 132on2b): "keys TEXT" as USB keyboard reports (sceUsbKb HID codes, shift = mod 2), then
// Enter (0x28), the way 004153f0 reads them. TEXT holds letters, digits, '-', '_' and (132on2c) spaces (0x2c).
// 132on2c: GuiKeyboard's input (0032ba10) ignores the report's modifiers and shifts by its own state, which
// Caps Lock (0x39) toggles, so each change of case is a Caps Lock press (and one more puts it back before Enter).
std::vector<omega::gui::GuiHost::UsbKeyReport> usb_key_reports(const std::string& text) {
    std::vector<omega::gui::GuiHost::UsbKeyReport> out;
    const auto add = [&](std::uint16_t code, std::uint32_t mod) {
        omega::gui::GuiHost::UsbKeyReport k;
        k.code = code;
        k.mod = mod;
        out.push_back(k);
    };
    bool caps = false;
    const auto shift = [&](bool on) {
        if (caps == on) return;
        add(0x39, 0);
        caps = on;
    };
    for (const char c : text) {
        const bool upper = (c >= 'A' && c <= 'Z') || c == '_';
        shift(upper);
        if (c >= 'a' && c <= 'z') add(static_cast<std::uint16_t>(4 + (c - 'a')), 0);
        else if (c >= 'A' && c <= 'Z') add(static_cast<std::uint16_t>(4 + (c - 'A')), 2);
        else if (c >= '1' && c <= '9') add(static_cast<std::uint16_t>(0x1e + (c - '1')), 0);
        else if (c == '0') add(0x27, 0);
        else if (c == '-') add(0x2d, 0);
        else if (c == '_') add(0x2d, 2);
        else if (c == ' ') add(0x2c, 0);
        else throw std::runtime_error(std::string("Front-end script: \"keys\" cannot type '") + c + "'");
    }
    shift(false);
    add(0x28, 0);
    return out;
}

// Port-only diagnostic (checkpoint 132win5, FrontendOptions::profile_ms): host milliseconds by part, per loop
// iteration and per pass.
struct FrontendProfile {
    using Clock = std::chrono::steady_clock;
    double threshold_ms = 0.0;
    std::map<std::string, double> frame, total;
    std::map<std::string, std::uint64_t> calls;
    Clock::time_point begun{};
    std::uint64_t iterations = 0, slow = 0;
    double worst_ms = 0.0;
    std::string worst;
    const DiscImage* disc = nullptr;  // its reads (DiscImage::read_stats) as the part "disc_read"
    DiscImage::ReadStats disc_at{};
    bool on() const { return threshold_ms > 0.0; }
    void add(const std::string& part, double ms) {
        frame[part] += ms;
        total[part] += ms;
        ++calls[part];
    }
    void begin() {
        if (!on()) return;
        frame.clear();
        if (disc) disc_at = disc->read_stats();
        begun = Clock::now();
    }
    // Logs the iteration when its work (the time outside the present, which waits for the display) took over
    // the threshold.
    void end(std::int64_t session_frame, const std::string& view) {
        if (!on()) return;
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - begun).count();
        if (disc) {
            const auto& now = disc->read_stats();
            if (now.reads != disc_at.reads) {
                add("disc_read", (now.seconds - disc_at.seconds) * 1000.0);
                frame["disc_reads"] += double(now.reads - disc_at.reads);
                frame["disc_KiB"] += double(now.bytes - disc_at.bytes) / 1024.0;
            }
        }
        const double work = ms - frame["present"];
        ++iterations;
        total["(work)"] += work;
        if (work < threshold_ms) return;
        ++slow;
        std::ostringstream line;
        line.setf(std::ios::fixed);
        line.precision(2);
        line << "Front end profile (port-only): frame " << session_frame << " " << view << " work " << work << " ms:";
        for (const auto& [part, v] : frame)
            if (v >= 0.05) line << " " << part << " " << v;
        if (work > worst_ms) {
            worst_ms = work;
            worst = line.str();
        }
        std::cout << line.str() << "\n";
    }
    void summary(unsigned pass) {
        if (!on()) return;
        std::ostringstream line;
        line.setf(std::ios::fixed);
        line.precision(1);
        line << "Front end profile (port-only) pass " << pass << ": " << iterations << " iterations, " << slow
             << " over " << threshold_ms << " ms, worst " << worst_ms << " ms; totals (ms / calls):";
        for (const auto& [part, v] : total) line << " " << part << " " << v << "/" << calls[part];
        std::cout << line.str() << "\n";
        frame.clear();
        total.clear();
        calls.clear();
        iterations = slow = 0;
        worst_ms = 0.0;
        worst.clear();
    }
};

class FrontendProfileScope {
public:
    FrontendProfileScope(FrontendProfile& p, const char* part)
        : p_(p.on() ? &p : nullptr), part_(part), begun_(p_ ? FrontendProfile::Clock::now() : FrontendProfile::Clock::time_point{}) {}
    ~FrontendProfileScope() {
        if (p_) p_->add(part_, std::chrono::duration<double, std::milli>(FrontendProfile::Clock::now() - begun_).count());
    }
    FrontendProfileScope(const FrontendProfileScope&) = delete;
    FrontendProfileScope& operator=(const FrontendProfileScope&) = delete;

private:
    FrontendProfile* p_;
    const char* part_;
    FrontendProfile::Clock::time_point begun_;
};

// Wraps a service so its calls are timed under `part`.
template <class R, class... A> void profile_service(std::function<R(A...)>& f, FrontendProfile& p, const char* part) {
    if (!f) return;
    f = [inner = std::move(f), &p, part](A... a) -> R {
        FrontendProfileScope scope(p, part);
        return inner(std::forward<A>(a)...);
    };
}

std::vector<ScriptStep> read_script(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot read the front-end script " + path.string());
    std::vector<ScriptStep> steps;
    std::string line;
    int number = 0;
    std::optional<std::string> card;
    std::optional<std::string> keys, touch_file, wait_file, capture;
    std::optional<std::size_t> wait_games;
    // The rest of the line, trimmed (a path or text that may hold spaces).
    const auto rest = [](std::istringstream& s) {
        std::string r;
        std::getline(s, r);
        const auto b = r.find_first_not_of(" \t\r"), e = r.find_last_not_of(" \t\r");
        return b == std::string::npos ? std::string() : r.substr(b, e - b + 1);
    };
    while (std::getline(in, line)) {
        ++number;
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream s(line);
        std::string word;
        if (!(s >> word)) continue;
        if (word == "card") {  // PORT-ONLY test (132cc10): the simulated card state from the next step
            std::string spec;
            if (!(s >> spec)) throw std::runtime_error("Front-end script line " + std::to_string(number) + ": expected \"card SPEC\"");
            (void)parse_agent_card_simulation(spec);
            card = spec;
            continue;
        }
        if (word == "touch" || word == "waitfile") {  // PORT-ONLY test (132on2c): see ScriptStep
            const std::string path = rest(s);
            if (path.empty()) throw std::runtime_error("Front-end script line " + std::to_string(number) + ": expected \"" + word + " PATH\"");
            (word == "touch" ? touch_file : wait_file) = path;
            continue;
        }
        if (word == "capture") {  // PORT-ONLY test (132on2h): see ScriptStep
            std::string name;
            if (!(s >> name) || name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
                throw std::runtime_error("Front-end script line " + std::to_string(number) + ": expected \"capture NAME\" (letters, digits, _)");
            capture = name;
            continue;
        }
        if (word == "waitgames") {  // PORT-ONLY test (132on2c): see ScriptStep
            long long n = -1;
            if (!(s >> n) || n < 0) throw std::runtime_error("Front-end script line " + std::to_string(number) + ": expected \"waitgames N\"");
            wait_games = static_cast<std::size_t>(n);
            continue;
        }
        if (word == "keys") {  // PORT-ONLY test (132on2b): typed on the USB keyboard at the next step
            const std::string text = rest(s);  // 132on2c: the rest of the line (it may hold spaces)
            if (text.empty()) throw std::runtime_error("Front-end script line " + std::to_string(number) + ": expected \"keys TEXT\"");
            (void)usb_key_reports(text);
            keys = text;
            continue;
        }
        std::istringstream n_in(word);
        std::int64_t n = 0;
        if (!(n_in >> n) || !n_in.eof()) continue;
        std::string buttons;
        if (!(s >> buttons) || n <= 0)
            throw std::runtime_error("Front-end script line " + std::to_string(number) + ": expected \"N BUTTONS\"");
        steps.push_back({n, frontend_buttons(buttons), std::move(card), std::move(keys), std::move(touch_file), std::move(wait_file),
                         wait_games, std::move(capture)});
        capture.reset();
        card.reset();
        keys.reset();
        touch_file.reset();
        wait_file.reset();
        wait_games.reset();
    }
    if (touch_file || wait_file || wait_games || capture)
        throw std::runtime_error("Front-end script: a \"touch\" / \"waitfile\" / \"waitgames\" / \"capture\" line after the last step");
    if (keys) throw std::runtime_error("Front-end script: a \"keys\" line after the last step");
    if (card) throw std::runtime_error("Front-end script: a \"card\" line after the last step");
    return steps;
}

}  // namespace

std::uint16_t frontend_buttons(const std::string& text) {
    if (text == "-") return 0;
    static const std::map<std::string, std::uint16_t> names{
        {"l2", ds2::l2},       {"r2", ds2::r2},         {"l1", ds2::l1},       {"r1", ds2::r1},
        {"triangle", ds2::triangle}, {"circle", ds2::circle}, {"cross", ds2::cross}, {"square", ds2::square},
        {"select", ds2::select}, {"l3", ds2::l3},       {"r3", ds2::r3},       {"start", ds2::start},
        {"up", ds2::up},       {"right", ds2::right},   {"down", ds2::down},   {"left", ds2::left}};
    std::uint16_t mask = 0;
    std::size_t at = 0;
    while (at <= text.size()) {
        const std::size_t end = std::min(text.find('+', at), text.size());
        std::string name = text.substr(at, end - at);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        const auto it = names.find(name);
        if (it == names.end()) throw std::runtime_error("Unknown pad button '" + name + "'");
        mask = static_cast<std::uint16_t>(mask | it->second);
        at = end + 1;
    }
    return mask;
}

struct FrontendApp::Impl {
    const FrontendOptions options;
    FrontendResult result;
    std::vector<ScriptStep> script;
    std::unique_ptr<FrontendDisc> data;
    audio::Mixer mixer;
    std::unique_ptr<SoundIopHost> sound_host;
    std::unique_ptr<GameSoundManager> game_sound;
    std::unique_ptr<CardBackups> card_backups;  // port-only (132win13): rolling backups before each card change
    std::unique_ptr<FrontendSession> session;
    // Port-only (132ln3): the LAN session (FrontendOptions::lan_loopback: on lan_net, an in-memory network).
    std::shared_ptr<lan::LanLoopbackNetwork> lan_net;
    std::unique_ptr<lan::LanSession> lan_session;
    std::unique_ptr<lan::LanSession> lan_peer;  // port-only test (132ln4): --lan-loopback-peer's hosted game
    // Port-only (checkpoint 132md5, research/MODDING.md "LAN: the mod report at join"): the hosted LAN games' mod
    // gate (the transport's join filter, lan_mod_gate.hpp), the joiner's report in every JOIN_REQ, and the host's
    // notices about its joiners (name, class, fingerprint; accepted or refused), drawn for a few seconds over the
    // frame's bottom-left corner.
    std::shared_ptr<lan::LanModGate> lan_mod_gate;
    void lan_mod_config(lan::LanSessionConfig& config);
    FrameOverlay lan_notice_overlay;
    std::deque<std::pair<std::string, std::int64_t>> lan_notices;  // the text, the front-end frame it ends at
    void draw_lan_notices();
    std::unique_ptr<lan::LanNetCore> lan_core;  // port-only (132ln5): the network game's DME.cc game side
    // Port-only test (132on2f, --online-portmap-test): the fake PCP / NAT-PMP gateway (outlives online_lobby, whose
    // destructor waits for the mapping's removal).
    std::unique_ptr<online::FakePortMapGateway> portmap_test_gateway;
    std::shared_ptr<OnlineLobbyHost> online_lobby;  // port-only (132on2): the online lobby (omega_server)
    // Port-only (132on2f): the online lobby's connection-path notices into lan_notices (the overlay above).
    void online_path_notices();
    const AgentStatistics* online_live_stats = nullptr;  // 132on2d: the level's statistics at its end (null: the agent's)
    std::function<PortSettings()> port_settings_reader;  // the options rows' values (132win3)
    std::unique_ptr<audio::WavWriter> wav;
    std::vector<std::int16_t> sound_pcm;

    std::unique_ptr<HostWindow> owned_host;  // when the caller passed none
    HostWindow* host = nullptr;               // null: hidden
    bool hidden_fullscreen = false;           // the FULLSCREEN row's value without a window
    bool vsync = true;                        // 132win4: the VSYNC row's value (FrontendOptions::vsync at start)
    unsigned pass = 0;                        // 132win4: loop() calls (the boot pass, then one per level end)
    std::uint64_t screen_generation = 0;
    std::optional<LoopPacing> pacing;  // 132win3 (port-only): the live loop's frame pacing
    std::unique_ptr<GuiRenderer> screen;
    std::unique_ptr<GuiRenderer> offscreen;
    std::set<std::uint32_t> uploaded;
    std::set<std::uint32_t> zeus_uploaded;
    std::unique_ptr<GuiDrawer> drawer;
    IeDrawState ie_state;
    std::unique_ptr<audio::Device> sound;

    // Checkpoint 132win6 (port-only): the controller, its connection state and the keyboard / mouse bindings
    // (FrontendOptions::gamepad / pads / menu_bindings, else the front end's own).
    std::unique_ptr<GamepadDevice> owned_gamepad;
    GamepadDevice* gamepad = nullptr;
    std::unique_ptr<PadConnection> owned_pads;
    PadConnection* pads = nullptr;
    std::optional<SimulatedPads> simulated_pads;
    GamepadSimulation* pad_simulation = nullptr;   // 132win12: simulated devices (tests)
    std::vector<PadDeviceEvent> pad_device_events;
    std::size_t pad_device_next = 0;
    std::string pad_device_seen;
    void read_simulated_devices();
    InputBindings menu_bindings = default_input_bindings(BindProfile::menu);
    bool keys_were_down = false;
    std::int32_t pad_state_shown = 6;
    void track_pad(const std::string& real_source, bool keys_down);
    std::size_t script_index = 0;
    std::int64_t script_left = 0;
    std::size_t card_step = ~std::size_t{0};  // the script step whose "card" line was applied
    bool script_waiting = false;  // PORT-ONLY test (132on2c): the step's "waitfile" is not there yet
    void simulate_card(const std::string& spec, const char* from);
    GuiDrawList shown;
    std::uint64_t movie_serial = 0, screen_movie_serial = 0;
    // Checkpoint 132cp11: *005392d0 as the last GUI batch drawn left it (0 at boot).
    std::uint32_t last_render_mode = 0;
    std::string subtitle_logged;
    // Port-only (132win5): the shown movie picture as RGBA, converted once per picture for both renderers.
    Texture movie_rgba;
    std::uint64_t movie_rgba_serial = 0;
    const Texture& movie_picture() {
        if (movie_rgba_serial != session->movie_serial() + 1) {
            movie_rgba = movie_texture(*session->movie_frame());
            movie_rgba_serial = session->movie_serial() + 1;
        }
        return movie_rgba;
    }
    std::string view, pending_view, focused_name;
    std::int64_t view_since = 0;
    bool view_captured = true;
    bool audio_warned = false;
    std::vector<audio::AudioDeviceEvent> audio_events;  // port-only (132win7): FrontendOptions::test_audio_device
    std::size_t audio_event_at = 0;
    std::int64_t audio_pass_start = 0;  // the session frame the pass's events count from
    std::string figure_note;
    // Port-only diagnostics (checkpoint 132win5): FrontendOptions::test_device_lost / _sweep / window_captures,
    // and the profile (profile_ms).
    std::set<std::int64_t> test_lost, sweep_starts;
    std::optional<std::int64_t> sweep_call;  // the armed sweep's call index
    std::int64_t sweep_first = 0;
    std::vector<std::string> sweep_sites;
    std::uint64_t window_captured = 0;
    FrontendProfile profile;
    void arm_device_hooks();
    // Port-only (checkpoint 132win11): the frame-time overlay (frame_overlay.hpp) in the front end too: on from
    // FrontendOptions::frame_overlay (the settings' frame_overlay / --frame-overlay), toggled by its key
    // (frame_overlay_key, F3 by default) and by the Port Settings page; its rolling averages (frame_profile.hpp,
    // no rows kept) split each loop into the session's steps (sim: the GUI frames, sound mix and texture uploads),
    // the draw (render) and the present.
    bool overlay_on = false;
    std::optional<std::uint8_t> overlay_key;
    FrameOverlay overlay;
    std::optional<omega::FrameProfile> overlay_profile;
    std::uint64_t overlay_logged = ~0ull;
    void overlay_events();
    void draw_overlay(std::int64_t frame);
    // Port-only (checkpoint 132win11): the Port Settings page (FrontendOptions::settings_page) and this front
    // end's listener on it (menu bindings, vsync, the overlay).
    std::shared_ptr<PortSettingsStore> store;
    PortSettingsPage* page = nullptr;
    std::size_t page_listener = 0;
    std::uint16_t page_input(std::uint16_t held, bool keys_down, const KeyboardState& keys);
    ~Impl() {
        if (page) page->unlisten(page_listener);
    }
    // Port-only (checkpoint 132win5): FrontendOptions::test_minimize / test_close, and the minimised window's
    // pacing (the steps go on, timer-paced, nothing presented).
    bool was_minimized = false;
    std::uint64_t minimized_presents_skipped = 0;
    void window_tests();
    void follow_minimized();
    // Port-only (checkpoint 132win12): focus loss. The front end keeps stepping (no focus pause in the PS2 game);
    // its sound is silenced (background_audio=1 keeps it), its input and the motors are off while unfocused.
    bool was_focused = true;
    bool window_focused() const;
    void follow_focus();
    void apply_focus_mute();

    Impl(DiscImage& disc, const FrontendOptions& o);
    void upload(std::uint32_t slot, const Texture& t) {
        if (screen) screen->set_texture(slot, t);
        if (offscreen) offscreen->set_texture(slot, t);
    }
    // Checkpoint 132zf: the ZEUS pictures the slideshow's bitmap holds now
    // (loaded and released as its slides change).
    void sync_zeus() {
        const auto& images = data->zeus_images();
        std::set<std::uint32_t> drawn;
        for (const auto& b : shown.batches) drawn.insert(b.slot);
        for (auto it = zeus_uploaded.begin(); it != zeus_uploaded.end();) {
            if (images.count(*it) || drawn.count(*it)) {
                ++it;
                continue;
            }
            if (screen) screen->drop_texture(*it);
            if (offscreen) offscreen->drop_texture(*it);
            it = zeus_uploaded.erase(it);
        }
        for (const auto& [slot, image] : images)
            if (zeus_uploaded.insert(slot).second) upload(slot, image);
    }
    void open_output();
    void make_screen();
    void release_output();
    void capture(const std::string& name);
    void step(std::uint16_t held);
    void present();
    void nested_frame();
    std::int64_t card_wait_captured = -1;
    FrontendResult loop();
};

FrontendApp::Impl::Impl(DiscImage& disc, const FrontendOptions& o) : options(o) {
    script = options.script.empty() ? std::vector<ScriptStep>{} : read_script(options.script);
    script_left = script.empty() ? 0 : script[0].frames;
    if (options.hidden && options.script.empty()) throw std::runtime_error("--frontend hidden runs need a script");

    // The PC memory card: a PCSX2 image's BASCUS-97264 copied in once.
    std::filesystem::create_directories(options.card_folder);
    if (!options.memcard.empty()) {
        FolderAgentCard pc(options.card_folder);
        const auto imported = import_ps2_card_directory(Ps2CardImage::read(options.memcard), std::string(agent_card_directory), pc);
        std::cout << "Front end: memory card " << options.memcard.string() << ": "
                  << (imported.found ? std::to_string(imported.copied.size()) + " file(s) imported, " +
                                           std::to_string(imported.kept.size()) + " kept"
                                     : std::string("no BASCUS-97264 save")) << "\n";
    }

    std::cout << "Front end: loading fonts, strings, award tables and screens...\n";
    data = std::make_unique<FrontendDisc>(disc, options.iso);
    const GuiDiscFonts& fonts = data->fonts();
    // Checkpoint 126c: gameSound over the play sound host (the 989snd banks
    // and streams from the disc, mixed at 48 kHz). The GUI application loads
    // the FRONTEND banks into it (002cb3d0) and plays the music and GS_*
    // sounds; the type 3 / 5 requests it makes need no owner, listener or
    // camera, and *004ffc2c (the HUD) is null in the front end.
    SoundIopHostConfig sound_config;
    sound_config.level = "TEST";   // 004ffc90's boot value; the session swaps in "FRONTEND"
    sound_host = std::make_unique<SoundIopHost>(disc, sound_config, mixer);
    game_sound = std::make_unique<GameSoundManager>(*sound_host);
    sound_host->attach(*game_sound);
    FrontendServices services = data->services(options.card_folder);
    services.sound = game_sound.get();
    services.sound_level = [h = sound_host.get()](const std::string& level) { h->set_level(level); };
    services.nested_frame = [this] { nested_frame(); };
    test_lost = options.test_device_lost;
    sweep_starts = options.test_device_lost_sweep;
    if (options.menu_bindings) menu_bindings = *options.menu_bindings;
    pads = options.pads;
    if (!pads) {
        owned_pads = std::make_unique<PadConnection>();
        pads = owned_pads.get();
    }
    if (!options.test_pads.empty()) {
        simulated_pads.emplace(parse_pad_events(options.test_pads));
        std::cout << "Front end: simulated controllers (port-only test, --frontend-test-pads): " << options.test_pads << "\n";
    }
    gamepad = options.gamepad;
    if (options.pad_simulation && !options.test_pad_devices.empty()) {
        if (!gamepad) throw std::runtime_error("Front end: --frontend-test-pad-devices needs the session's controller device");
        pad_simulation = options.pad_simulation;
        pad_device_events = parse_pad_device_events(options.test_pad_devices);
        std::cout << "Front end: simulated pad devices (port-only test, --frontend-test-pad-devices): " << options.test_pad_devices << "\n";
    }
    if (!gamepad && script.empty()) {  // scripted runs read no controller
        owned_gamepad = std::make_unique<GamepadDevice>();
        gamepad = owned_gamepad.get();
    }
    profile.threshold_ms = options.profile_ms;
    profile.disc = &disc;
    if (profile.on()) {  // port-only (132win5): the session's disc / decode services, timed
        profile_service(services.gui_file, profile, "gui_file");
        profile_service(services.decor, profile, "decor");
        profile_service(services.open_movie, profile, "open_movie");
        profile_service(services.zeus_load, profile, "zeus_load");
        profile_service(services.file_load, profile, "mantex_load");
        profile_service(services.frontend_file, profile, "frontend_file");
        profile_service(services.agent_parts, profile, "agent_parts");
        profile_service(services.sound_level, profile, "sound_level");
        std::cout << "Front end profile (port-only diagnostic): loop iterations over " << profile.threshold_ms
                  << " ms are logged by part (ms)\n";
    }
    if (!options.hidden) {
        host = options.host;
        if (!host) {
            owned_host = std::make_unique<HostWindow>();
            host = owned_host.get();
        }
    }
    // Port-only (132win11): the settings store shared with the play and the Port Settings page, else this front
    // end's own (the file, or values kept in memory without one).
    store = options.settings_store ? options.settings_store : std::make_shared<PortSettingsStore>(options.port_settings);
    page = options.settings_page;
    {  // port-only (132ob6, 132win; always present since 132win2, kept in memory without a settings file)
        FullscreenRow row;
        if (host) {
            row.get = [h = host] { return h->fullscreen(); };
            row.set = [h = host](bool on) { h->set_fullscreen(on); };
        } else {  // hidden: the setting alone
            hidden_fullscreen = store->read().fullscreen;
            row.get = [this] { return hidden_fullscreen; };
            row.set = [this](bool on) { hidden_fullscreen = on; };
        }
        vsync = options.vsync;
        VsyncRow vrow;
        vrow.get = [this] { return vsync; };
        vrow.set = [this](bool on) {
            vsync = on;
            if (pacing) pacing->set_vsync(on);
            std::cout << "Front end: vsync (port-only) " << (on ? "on" : "off") << " from the VSYNC row"
                      << (pacing ? ", present interval " + std::to_string(pacing->sync_interval()) : std::string())
                      << "\n";
        };
        port_settings_reader = bind_port_settings_rows(services, store, std::move(row), std::move(vrow), page);
    }
    // Port-only (132ln1): LAN Play on the online / offline select.
    services.lan_play = options.lan_play || lan_play_setting(store->read());
    if (services.lan_play)
        std::cout << "Front end: LAN Play (port-only, 132ln1) shown on the online / offline select"
                  << (options.lan_play ? " (--lan-play)" : " (lan_play=1, the default)") << "\n";
    if (services.lan_play) {  // Port-only (132ln3): the LAN session behind MSNSETUP's begin_mission
        lan::LanSessionConfig lc;
        lc.build_hash = lan::lan_build_hash(OMEGA_ISO_SHA256, OMEGA_EXE_SHA256);
        lan_mod_config(lc);
        if (options.lan_loopback) {
            const auto start = std::chrono::steady_clock::now();
            lan::LanClock clock = [start] {
                return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count());
            };
            lan_net = lan::LanLoopbackNetwork::make(clock, 1);
            lc.clock = clock;
            lc.socket_factory = lan_net->factory(lan::kLanLoopbackIp);
            lc.discovery.clock = clock;
            lc.discovery.socket_factory = lan_net->factory(lan::kLanLoopbackIp);
            lc.discovery.send_beacons = false;
        } else if (options.lan_address) {  // Port-only (132ln5f): real UDP on one local address
            const auto ip = *options.lan_address;
            lc.socket_factory = lan::lan_udp_socket_factory(ip);
            lc.discovery.socket_factory = lan::lan_udp_socket_factory(ip);
            lc.discovery.listen_for_beacons = false;
            lc.discovery.use_broadcast = false;
            lc.discovery.extra_targets = {lan::LanAddress{ip, lc.discovery.discovery_port}};
            std::cout << "Front end: LAN sockets bound to " << lan::lan_address_text({ip, 0}) << " (port-only, 132ln5f, --lan-address): "
                      << "no broadcasts, the browser asks port " << lc.discovery.discovery_port << " there\n";
        }
        lan_session = std::make_unique<lan::LanSession>(lc);
        services.lan_host = [this](const gui::GuiHost::LanHostRequest& r) {
            const bool ok = lan_session->host({r.host_name, r.mission, r.num_agents, r.join_filter});
            if (ok) {
                const auto& a = lan_session->advert();
                char id[32];
                std::snprintf(id, sizeof id, "%016llx", static_cast<unsigned long long>(a.session_id));
                std::cout << "Front end: LAN game hosted (port-only, 132ln3" << (lan_net ? ", loopback network" : "")
                          << "): session " << id << ", game port " << a.game_port << ", build hash "
                          << std::hex << a.build_hash << std::dec << "\n";
            }
            return ok;
        };
        services.lan_hosting = [this] { return lan_session->hosting(); };
        // Port-only (132ln4): GuiNetJoinMission's browser and join.
        services.lan_browse = [this](bool on) {
            const bool ok = lan_session->browse(on);
            std::cout << "Front end: LAN browse (port-only, 132ln4) " << (on ? (ok ? "started" : "failed to start") : "stopped")
                      << "\n";
        };
        services.lan_games = [this] {
            std::vector<gui::GuiHost::LanGame> out;
            for (const auto& e : lan_session->games()) {
                gui::GuiHost::LanGame g;
                g.session_id = e.advert.session_id;
                g.host_name = e.advert.host_name;
                g.mission = e.advert.mission_index;
                g.agents_now = e.advert.agents_now;
                g.agents_max = e.advert.agents_max;
                g.join_filter = e.advert.join_filter;
                g.state = static_cast<std::uint8_t>(e.advert.state);
                g.mismatch = e.version_mismatch;
                out.push_back(std::move(g));
            }
            return out;
        };
        services.lan_join = [this](std::uint64_t id) {
            const std::string name = session->profile_name(session->current_profile()).value_or("");
            const bool ok = lan_session->join(id, name.substr(0, name.find('\0')));
            char text[32];
            std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(id));
            const auto mods = mods::active_online_report();
            std::cout << "Front end: LAN join (port-only, 132ln4) of session " << text << ": "
                      << (ok ? "JOIN_REQ sent" : "not sent") << " (132md5: with " << lan::lan_mod_brief(&mods) << ")\n";
            return ok;
        };
        services.lan_join_state = [this] {
            switch (lan_session->join_state()) {
            case lan::LanJoinState::Pending: return gui::GuiHost::LanJoin::pending;
            case lan::LanJoinState::Accepted: return gui::GuiHost::LanJoin::accepted;
            case lan::LanJoinState::Rejected: return gui::GuiHost::LanJoin::rejected;
            case lan::LanJoinState::Failed: return gui::GuiHost::LanJoin::failed;
            default: return gui::GuiHost::LanJoin::idle;
            }
        };
        services.lan_join_cancel = [this] {
            std::cout << "Front end: LAN join (port-only, 132ln4) closed\n";
            lan_session->cancel_join();
        };
        services.lan_join_notice = [this]() -> std::optional<std::string> {  // 132md5
            if (lan_session->join_error() != lan::LanJoinError::Refused) return std::nullopt;
            std::string why = lan_session->join_detail();
            for (char& c : why)
                if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) >= 0x7f) c = ' ';
            std::cout << "Front end: LAN join refused by the host (port-only, 132md5): " << why << "\n";
            return "Refused by the host: " + why;
        };
        services.lan_pump = [this] {
            lan_session->pump();
            if (lan_peer) lan_peer->pump();
        };
        services.lan_clock_ms = [this] { return static_cast<std::int64_t>(lan_session->clock_ms()); };
        std::cout << "Front end: LAN session (port-only, 132ln3)"
                  << (options.lan_loopback ? " on the loopback network (--lan-loopback, no beacons)" : "") << "\n";
        if (options.lan_loopback_peer) {  // port-only test (132ln4)
            if (!lan_net) throw std::runtime_error("--lan-loopback-peer needs --lan-loopback");
            lan::LanSessionConfig pc = lc;
            pc.socket_factory = lan_net->factory(lan::lan_ipv4(127, 0, 0, 2));
            pc.discovery.socket_factory = lan_net->factory(lan::lan_ipv4(127, 0, 0, 2));
            lan_peer = std::make_unique<lan::LanSession>(pc);
            if (!lan_peer->host({"LOOPBACK PEER", *options.lan_loopback_peer, 4, 0}))
                throw std::runtime_error("--lan-loopback-peer: the peer's LAN game could not be hosted");
            std::cout << "Front end: LAN loopback peer (port-only test, 132ln4) hosts mission "
                      << *options.lan_loopback_peer << " as LOOPBACK PEER\n";
        }
    }
    // Port-only (132on2, research/ONLINE.md section 9): the online lobby GuiOnlineConnect drives, on omega_server at
    // --online-server or the settings key online_server; neither: no lobby (Online keeps the original's unhosted start).
    {
        const PortSettings now = store->read();
        bool keyed = false;
        for (const auto& kv : now.other)
            if (kv.first == "online_server") keyed = true;
        if (options.online_server || keyed) {
            online::OnlineLobby::Options lo;
            if (options.online_server) {
                lo.server = *options.online_server;
            } else {
                const OnlineServerAddress a = online_server_setting(now);
                lo.server = a.host + ":" + std::to_string(a.port);
            }
            lo.platform = online::this_platform();
            lo.build = 1;
            lo.mod_report = [] { return mods::active_online_report(); };  // 132md4 (ONLINE.md §6.9)
            // PORT-ONLY (132on2e, ONLINE.md §6.11 / §6.12): the in-level connection's settings.
            const std::string relay_mode = options.online_relay ? *options.online_relay : online_relay_setting(now);
            const std::uint16_t game_port =
                options.online_game_port ? *options.online_game_port : online_game_port_setting(now);
            const bool upnp = options.online_upnp ? *options.online_upnp : online_upnp_setting(now);
            if (relay_mode == "off") lo.caps &= ~online::cap::Relay;
            online_lobby = std::make_shared<OnlineLobbyHost>(lo);
            services.lobby = online_lobby;
            // PORT-ONLY (132on2c, ONLINE.md §7): the platform badges on the online screens' names (online_platform_badges)
            online_lobby->set_badges(online_platform_badges_setting(now));
            // PORT-ONLY (132on2g, ONLINE.md §6.15): the lobby chat's profanity filter choice (online_chat_filter)
            online_lobby->set_chat_filter(online_chat_filter_setting(now));
            {
                const std::string pw =
                    options.online_game_password ? *options.online_game_password : online_game_password_setting(now);
                if (pw.size() > 31) throw std::runtime_error("--online-game-password: longer than 31 characters");
                if (!pw.empty()) {
                    online_lobby->set_game_password(pw);
                    std::cout << "Front end: the online games this agent creates are password-protected (port-only, "
                              << (options.online_game_password ? "--online-game-password" : "online_game_password")
                              << ")\n";
                }
            }
            // Checkpoint 132on2b (ONLINE.md §9.4 / §9.5): the in-level game of an online create / join is the LAN
            // game (LanSession) at the address omega_server hands over; no discovery beacons (the server lists it).
            if (!lan_session) {
                lan::LanSessionConfig lc;
                lc.build_hash = lan::lan_build_hash(OMEGA_ISO_SHA256, OMEGA_EXE_SHA256);
                lan_mod_config(lc);
                lc.discovery.send_beacons = false;
                // 132on2d: ephemeral ports; omega_server hands the joiner the host's actual port (HostEndpoint).
                // 132on2e: a joiner's socket stays ephemeral; the host binds online_game_port (the original's 3658 by
                // default, so players can forward it), set at each create by OnlineLobbyHost.
                lc.game_port = 0;
                lc.discovery.discovery_port = 0;
                if (options.lan_address) {  // as LAN play's (132ln5f): the game socket on that one address
                    lc.socket_factory = lan::lan_udp_socket_factory(*options.lan_address);
                    lc.discovery.socket_factory = lan::lan_udp_socket_factory(*options.lan_address);
                    lc.discovery.listen_for_beacons = false;
                    lc.discovery.use_broadcast = false;
                    std::cout << "Front end: the online game's LAN socket bound to "
                              << lan::lan_address_text({*options.lan_address, 0}) << " (port-only, --lan-address)\n";
                }
                // 132on2e: the game socket under the omega_server relay (online_relay_socket.hpp); a datagram to a
                // relay virtual address goes through omega_server, everything else directly.
                auto link = std::make_shared<RelayLink>();
                if (relay_mode != "off") lc.socket_factory = relay_socket_factory(lc.socket_factory, link);
                online_lobby->set_relay(relay_mode == "force" ? OnlineLobbyHost::RelayMode::Force
                                        : relay_mode == "off" ? OnlineLobbyHost::RelayMode::Off
                                                              : OnlineLobbyHost::RelayMode::Auto,
                                        link);
                online_lobby->set_game_port(game_port);
                const bool loopback = options.lan_address && (*options.lan_address >> 24) == 127;
                // 132on2f: online_upnp turns on the whole mapping chain (UPnP, then PCP, then NAT-PMP).
                online::PortMapOptions pm;
                if (options.online_portmap_test) {
                    const auto mode = online::FakePortMapGateway::parse_mode(*options.online_portmap_test);
                    if (!mode) throw std::runtime_error("--online-portmap-test: pcp, natpmp, refuse or silent");
                    portmap_test_gateway = std::make_unique<online::FakePortMapGateway>(*mode);
                    if (!portmap_test_gateway->ok())
                        throw std::runtime_error("--online-portmap-test: the fake gateway: " + portmap_test_gateway->error());
                    pm.upnp = false;
                    pm.gateway_ip = 0x7f000001u;
                    pm.gateway_port = portmap_test_gateway->port();
                    pm.tries = 2;  // a silent gateway fails in 0.75 s
                    std::cout << "Front end: port mapping test (port-only, 132on2f): a fake " << *options.online_portmap_test
                              << " gateway at 127.0.0.1:" << pm.gateway_port << " (UPnP off)\n";
                }
                const bool portmap = upnp || options.online_portmap_test.has_value();
                online_lobby->set_portmap(portmap, loopback, pm);
                std::cout << "Front end: online in-level connection (port-only, 132on2e / 132on2f): game port "
                          << (game_port ? std::to_string(game_port) : std::string("system-picked")) << ", port mapping "
                          << (portmap ? (loopback && !options.online_portmap_test ? "on (skipped: loopback socket)"
                                                                                   : (pm.upnp ? "on (UPnP, PCP, NAT-PMP)"
                                                                                              : "on (PCP, NAT-PMP)"))
                                      : "off")
                          << ", relay " << relay_mode << "\n";
                lan_session = std::make_unique<lan::LanSession>(lc);
            } else {
                std::cout << "Front end: the online games use the LAN session already open (no relay, no UPnP)\n";
            }
            online_lobby->set_lan(lan_session.get());
            // Checkpoint 132on2d (ONLINE.md §3.6): the record's builder, the session's callback +2554 (00452f30 ->
            // 002cbd40 on the selected agent): the rank row (+b50; 0 none), the statistics (00333ee0; at the level's
            // end the level's own, which play_main's end_agent syncs into the agent), the rating and award flags and
            // the compact appearance (+9a8).
            online_lobby->set_stats_builder([this](std::span<std::uint8_t, online::kStatsBlobSize> blob) {
                FrontendSession& fs = *session;
                const std::int32_t p = fs.current_profile();
                const AgentStatistics stats = online_live_stats ? *online_live_stats : fs.agent_statistics();
                std::vector<std::uint8_t> ratings, awards;
                for (const auto& r : fs.profile_ratings(p)) ratings.push_back(r.b8);
                for (const auto& r : fs.profile_vec(p, 0)) awards.push_back(r.b8);
                const StatsRecordAgent a{
                    fs.profile_word(p, 0xb50) != 0
                        ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(fs.profile_rank_row(p).index))
                        : std::nullopt,
                    [&stats](std::uint32_t i) { return read_agent_statistic(stats, i); }, ratings, awards,
                    fs.agent().subspan<0x9a8, 0xc0>()};
                build_stats_record(blob, a);
            });
            std::cout << "Front end: online lobby (port-only, 132on2) on omega_server " << lo.server
                      << (options.online_server ? " (--online-server)" : " (online_server)") << ", DNAS bypassed\n";
        }
    }
    overlay_on = options.frame_overlay;
    overlay_key = options.frame_overlay_key;
    if (host) overlay_profile.emplace(false);  // the overlay's rolling averages only (no rows kept)
    if (host && overlay_on) std::cout << "Front end: frame overlay (port-only) on\n";
    if (page) {
        page_listener = page->listen([this](const PortSettings& s) {
            try {
                menu_bindings = parse_input_bindings(s).menu;
                const DisplaySettings d = display_settings(s);
                overlay_on = d.frame_overlay;
                overlay_key = d.frame_overlay_key;
                if (s.vsync != vsync) {
                    vsync = s.vsync;
                    if (pacing) pacing->set_vsync(vsync);
                    std::cout << "Front end: vsync (port-only) " << (vsync ? "on" : "off") << " from the Port Settings page"
                              << (pacing ? ", present interval " + std::to_string(pacing->sync_interval()) : std::string())
                              << "\n";
                }
            } catch (const std::exception& e) {
                std::cerr << "Front end: Port Settings page (port-only): " << e.what() << "\n";
            }
        });
    }
    session = std::make_unique<FrontendSession>(std::move(services));
    {  // Port-only (132win13, card_backup.hpp): the save directory is copied before each change to the card (the
       // front end's saves and the play's, which use this card); a failed copy is logged and the save goes on.
        unsigned keep = card_backups_default;
        try {
            keep = card_backups_setting(store->read());
        } catch (const std::exception& e) {
            std::cerr << "Front end: " << e.what() << "; using " << keep << "\n";
        }
        card_backups = std::make_unique<CardBackups>(session->card().directory(), keep);
        std::cout << "Front end: memory card backups (port-only): " << (keep ? std::to_string(keep) + " kept in " +
                     card_backups->root().string() : std::string("off (card_backups=0)")) << "\n";
        session->card().set_before_change([b = card_backups.get()] {
            try {
                const auto r = b->before_change();
                if (r.made)
                    std::cout << "Memory card backup (port-only): " << r.made->string()
                              << (r.pruned.empty() ? std::string() : ", " + std::to_string(r.pruned.size()) + " old deleted")
                              << "\n";
                else if (r.skipped != "the same save")
                    std::cout << "Memory card backup (port-only): none (" << r.skipped << ")\n";
            } catch (const std::exception& e) {
                std::cerr << "Memory card backup (port-only): failed: " << e.what() << "\n";
            }
        });
    }
    if (options.unlock_all_levels) session->runtime().g.b_bb5 = 0;  // 0013d270 (FrontendOptions)
    if (options.unlock_all) session->port_unlock_all_at_boot();  // PORT-ONLY cheat (port_cheats.hpp)
    if (!options.card_simulation.empty()) simulate_card(options.card_simulation, "--frontend-card-sim");
    sound_host->schedule = [s = session.get()](std::int32_t delay, std::function<void()> event) {
        s->schedule(delay, std::move(event));
    };
    sound_host->hud = [] { return false; };
    if (!options.audio_wav.empty())
        wav = std::make_unique<audio::WavWriter>(options.audio_wav.string(), audio::Mixer::output_rate, 2);

    if (!options.capture_dir.empty()) {
        std::filesystem::create_directories(options.capture_dir);
        offscreen = std::make_unique<GuiRenderer>(640, 448);
    }
    drawer = std::make_unique<GuiDrawer>([&fonts](std::uint32_t h) { return fonts.font(h); });
    drawer->set_decor(&session->decor(), &ie_state);
    // Checkpoint 132cc6: GuiCharacterDisplay's +48 callback for the agent screens, GuiAgentCreate's
    // 00290bc0 (the draw 0028d870) and GuiAgentModify's 00345020 (00341f00). The front end has no other
    // display callback (GuiScoresEquip's 0035b390 figure is the play's).
    drawer->character_display_draw = [this](GuiDrawList& out, IeDrawState*, const gui::Widget& screen,
                                            const gui::Widget&) {
        std::string note;
        if (screen.cls == gui::WidgetClass::agentcreate && screen.ac)
            note = data->agent_figure_draw(out, screen.ac->m.model().packing, screen.ac->m.camera,
                                           screen.ac->pose_time, 0x28d870);
        else if (screen.cls == gui::WidgetClass::agentmod && screen.am)
            note = data->agent_figure_draw(out, screen.am->m.packing, screen.am->m.camera, screen.am->pose_time,
                                           0x341f00);
        else
            throw std::runtime_error("gui draw: unhosted GuiScoresEquip figure draw (0035b390)");
        if (note != figure_note) {
            figure_note = note;
            std::cout << "Front end: frame " << session->frames() << " agent model: " << note << "\n";
        }
    };
    // Checkpoint 132f13: GuiWeaponDisplay's draw 00440580 (the personnel pages' earned items).
    drawer->weapon_display_draw = [this](GuiDrawList& out, IeDrawState* st, const gui::Widget& w) {
        data->weapon_display_draw(out, st, w, static_cast<std::uint32_t>(session->frames()));
    };
    // Checkpoint 132f14: GuiAgentDisplay's draw 00361e70 (the dossier's agent file, AGNTINFO).
    drawer->agent_display_draw = [this](GuiDrawList& out, IeDrawState*, const gui::Widget& w) {
        if (!w.ad) throw std::runtime_error("gui draw: GuiAgentDisplay without its state (00361e70)");
        const std::string note =
            data->agent_figure_draw(out, w.ad->packing, w.ad->camera, w.ad->pose_time, 0x361e70, w.ad->turns);
        if (note != figure_note) {
            figure_note = note;
            std::cout << "Front end: frame " << session->frames() << " agent model: " << note << "\n";
        }
    };
    open_output();
}

// The renderer and the audio device. The game window (host) stays across the play, which makes its own
// renderers on the same device at the hand-off; they are released there and made again on a resume.
void FrontendApp::Impl::open_output() {
    if (host) {
        host->show(true);
        host->clear_events();
        host->want_mouse_capture(false);  // port-only (132win7): the front end keeps the cursor
        screen = std::make_unique<GuiRenderer>(host->gfx());
        screen->set_frame_fit(true);  // port-only 132win2: the 4:3 PS2 frame with black bars (frame_fit.hpp)
        screen_generation = host->device_generation();
        screen_movie_serial = 0;
    }
    uploaded.clear();
    for (const auto& [slot, image] : data->fonts().images) {
        upload(slot, image);
        uploaded.insert(slot);
    }
    for (const auto& [slot, image] : data->assets().images())
        if (uploaded.insert(slot).second) upload(slot, image);
    zeus_uploaded.clear();
    sync_zeus();
    // Checkpoint 132win7: with no output device the Device starts silently and opens one when it appears.
    const bool audio_test = !options.test_audio_device.empty();
    if ((options.audio && !options.hidden) || audio_test) {
        audio_events = audio_test ? audio::parse_audio_device_events(options.test_audio_device)
                                  : std::vector<audio::AudioDeviceEvent>{};
        audio_event_at = 0;
        audio_pass_start = session->frames();
        const bool absent = audio::starts_absent(audio_events);
        try {
            sound = std::make_unique<audio::Device>(0.15, audio_test, absent);
            if (audio_test)
                std::cout << "Front end: audio output muted for --test-audio-device (port-only test)"
                          << (absent ? ", no device at startup (simulated)" : "") << "\n";
            for (const auto& line : sound->take_log()) std::cout << "Front end: " << line << " at startup\n";
            if (!was_focused) apply_focus_mute();  // 132win12: opened while the window is unfocused
        } catch (const std::exception& e) {
            std::cout << "Front end: no audio output (" << e.what() << ")\n";
        }
    }
}

// A device re-created after a loss (HostWindow::present): a new renderer with the images uploaded so far.
void FrontendApp::Impl::make_screen() {
    screen = std::make_unique<GuiRenderer>(host->gfx());
    overlay.release();  // port-only (132win11): the overlay's texture on the new device
    lan_notice_overlay.release();  // port-only (132md5)
    screen->set_frame_fit(true);
    screen->keep_image_copies(false);  // port-only (132win5): re-created and re-uploaded here after a loss
    screen_generation = host->device_generation();
    screen_movie_serial = 0;
    std::set<std::uint32_t> done;
    for (const auto& [slot, image] : data->fonts().images)
        if (done.insert(slot).second) screen->set_texture(slot, image);
    for (const auto& [slot, image] : data->assets().images())
        if (uploaded.count(slot) && done.insert(slot).second) screen->set_texture(slot, image);
    for (const auto& [slot, image] : data->zeus_images())
        if (zeus_uploaded.count(slot) && done.insert(slot).second) screen->set_texture(slot, image);
    std::cout << "Front end: renderer re-created on the new display device\n";
}

// Checkpoint 132win6 (port-only): pad 0's state for this step's controller check (00465870), from the controller
// in use (or the simulated device list) and a newly pressed bound key (the keyboard taking over).
void FrontendApp::Impl::track_pad(const std::string& real_source, bool keys_down) {
    std::string source = real_source;
    bool key = keys_down && !keys_were_down;
    keys_were_down = keys_down;
    const std::int64_t frame = session->frames() + 1;
    if (simulated_pads) {
        auto f = simulated_pads->at(static_cast<std::uint64_t>(frame));
        for (const auto& line : f.log)
            std::cout << "Front end: simulated controllers (port-only test) at front-end frame " << frame << ": " << line << "\n";
        source = f.source;
        key = key || f.key;
    }
    if (const auto line = pads->update(source, key)) std::cout << *line << " at front-end frame " << frame << "\n";
    session->set_pad_state(pads->pad_state());
    if (pads->pad_state() != pad_state_shown) {
        pad_state_shown = pads->pad_state();
        std::cout << "Front end: pad 0 state (0055a4ec) " << pad_state_shown << " from front-end frame " << frame << "\n";
    }
}

// Checkpoint 132win12 (port-only test): the simulated devices' events up to this front-end frame, then the
// controller device's reading as the pad's presence (the script keeps the buttons).
void FrontendApp::Impl::read_simulated_devices() {
    const std::int64_t frame = session->frames() + 1;
    for (; pad_device_next < pad_device_events.size() && pad_device_events[pad_device_next].frame <= frame; ++pad_device_next)
        std::cout << "Front end: simulated pad devices (port-only test) at front-end frame " << frame << ": "
                  << pad_simulation->apply(pad_device_events[pad_device_next]) << "\n";
    const auto pad = gamepad->read();
    std::ostringstream seen;
    if (pad) seen << pad->source << " held 0x" << std::hex << pad->pad.held << (was_focused ? "" : " (the window unfocused: not passed on)");
    else seen << "no controller";
    if (seen.str() != pad_device_seen) {
        pad_device_seen = seen.str();
        std::cout << "Front end: controller device (port-only test) at front-end frame " << frame << ": reads " << pad_device_seen << "\n";
    }
    track_pad(pad ? pad->source : std::string(), false);
}

void FrontendApp::Impl::release_output() {
    sound.reset();
    screen.reset();
    if (host) host->drop_target();
}

void FrontendApp::Impl::capture(const std::string& name) {
    if (!offscreen) return;
    char file[96];
    std::snprintf(file, sizeof file, "%02zu_%s_f%05lld.bmp", result.captures.size(), name.c_str(),
                  static_cast<long long>(session->frames()));
    const auto path = options.capture_dir / file;
    offscreen->clear({0.0F, 0.0F, 0.0F, 1.0F});
    offscreen->draw(shown);
    offscreen->capture(path);
    result.captures.push_back(path.string());
    std::cout << "Front end: capture " << path.string() << "\n";
}

void FrontendApp::Impl::step(std::uint16_t held) {
    FrontendSession& s = *session;
    {
        FrontendProfileScope scope(profile, "session");  // port-only (132win5): includes the services timed apart
        s.frame(held);
    }
    if (lan_session) lan_session->pump();  // port-only (132ln3): the hosted LAN game's discovery and transport
    if (lan_peer) lan_peer->pump();        // port-only test (132ln4): --lan-loopback-peer
    // Port-only (132on2b): the online lobby's service (the room's game list; a request's callback waits for the
    // screen's 004536b0).
    if (online_lobby) online_lobby->service();
    for (const auto& line : s.take_log()) std::cout << "Front end: recorded " << line << "\n";
    if (online_lobby)  // port-only (132on2)
        for (const auto& line : online_lobby->take_log()) std::cout << "Front end: " << line << " at front-end frame " << s.frames() << "\n";
    online_path_notices();
    if (page)
        for (const auto& line : page->take_log()) std::cout << "Front end: " << line << " at front-end frame " << s.frames() << "\n";
    // This frame's sound (800 frames of 48 kHz at 60 Hz).
    sound_pcm.clear();
    {
        FrontendProfileScope scope(profile, "sound_mix");
        sound_host->mix(audio::Mixer::output_rate / 60, sound_pcm);
    }
    if (wav) wav->write(sound_pcm);
    for (const auto& line : sound_host->take_log())
        std::cout << "Front end sound at frame " << s.frames() << ": " << line << "\n";
    // New decorator textures (the IE manager loads them with each screen).
    {
        FrontendProfileScope scope(profile, "uploads");
        for (const auto& [slot, image] : data->assets().images())
            if (uploaded.insert(slot).second) upload(slot, image);
        sync_zeus();
    }
    if (s.movie_playing() && s.movie_frame()) {
        // The window's copy is made when it is presented (present_movie):
        // up to 4 frames run per present, and none while minimized.
        if (offscreen && s.movie_serial() != movie_serial) {
            FrontendProfileScope scope(profile, "capture");  // port-only diagnostic renderer (--frontend-capture)
            movie_serial = s.movie_serial();
            offscreen->set_texture(movie_slot, movie_picture());
        }
        shown = movie_list(s.movie_aspect());
        if (const auto* track = s.movie_subtitles()) {
            append_subtitles(shown, data->fonts(), *track, s.movie_subtitle_colour(), last_render_mode);
            // Port-only log (checkpoint 132cp11): the cue's reveal start, its complete text and its hide.
            const auto drawn = track->visible_lines();
            std::string text = std::to_string(track->cue_index()) + ": ";
            if (!track->visible()) text += "(hidden)";
            else if (drawn != track->lines()) text += "(revealing)";
            else for (const auto& line : drawn) text += "[" + line + "]";
            if (text != subtitle_logged) {
                subtitle_logged = text;
                std::cout << "Front end subtitles at frame " << s.frames() << " (" << s.movie_name() << ", t " << track->time()
                          << " s), cue " << text << "\n";
            }
        }
    } else if (s.gui_drawn()) {
        FrontendProfileScope scope(profile, "draw_list");
        shown.batches.clear();
        drawer->draw_canvas(shown, s.runtime(), s.now_ms());
        if (!shown.batches.empty()) last_render_mode = shown.batches.back().render_mode;
        sync_zeus();  // the part textures the agent model draw took this frame
    } else if (s.movie_playing() || !s.gui_active()) {
        shown.batches.clear();
    }
    // A movie's PCM goes to the device instead of the sound mix: gameSound
    // is not loaded while a movie plays (the boot movie precedes 002cb3d0,
    // the attract movies follow the suspend 002c9860's level unload).
    auto pcm = s.take_movie_audio();
    while (sound && audio_event_at < audio_events.size() &&
           audio_events[audio_event_at].frame <= static_cast<std::uint64_t>(s.frames() - audio_pass_start)) {
        using K = audio::AudioDeviceEvent::Kind;
        const auto kind = audio_events[audio_event_at++].kind;
        if (kind == K::absent) continue;  // applied when the device was made
        if (kind == K::lost) sound->simulate_loss(false);
        else if (kind == K::gone) sound->simulate_loss(true);
        else if (kind == K::back) sound->simulate_device_back();
        else sound->simulate_default_change();
        std::cout << "Front end: audio device test (port-only) at frame " << s.frames() << ": "
                  << (kind == K::lost ? "device lost" : kind == K::gone ? "device lost, no device until back"
                      : kind == K::back ? "device back" : "default output device switched") << "\n";
    }
    if (sound && !s.movie_playing() && pcm.empty()) sound->submit(sound_pcm);
    if (sound && !pcm.empty()) {
        if (s.movie_rate() == 48000 && s.movie_channels() == 2) sound->submit(pcm);
        else if (!audio_warned) {
            audio_warned = true;
            std::cout << "Front end: movie audio " << s.movie_rate() << " Hz x " << s.movie_channels()
                      << " not played (the host output is 48 kHz stereo)\n";
        }
    }
    // Port-only (132win6): the device's loss / re-open notes (audio_device.hpp).
    if (sound)
        for (const auto& line : sound->take_log()) std::cout << "Front end: " << line << " at frame " << session->frames() << "\n";
    // The view: the movie, else the top screen.
    std::string now;
    if (s.movie_playing()) now = "MOVIE_" + s.movie_name();
    else if (const gui::Widget* top = s.runtime().top(); top && s.gui_active()) {
        now = frontend_class_name(top->cls);
        if (top->cc && top->cc->panel != 0) now += "_PANEL" + std::to_string(top->cc->panel);
        // Port: the slide shown (category, slide), so a capture follows each.
        if (top->ss && top->ss->cat >= 0 && top->ss->cat < static_cast<int>(top->ss->cats.size()))
            now += "_C" + std::to_string(top->ss->cat) + "_S" +
                   std::to_string(top->ss->cats[static_cast<std::size_t>(top->ss->cat)].current);
    } else now = "STATE" + std::to_string(s.state());
    if (now != pending_view) {
        pending_view = now;
        view_since = s.frames();
        view_captured = false;
    }
    if (now != view) {
        view = now;
        std::cout << "Front end: frame " << s.frames() << " state 0x" << std::hex << int(s.state()) << std::dec << " " << now;
        if (options.memory_log) std::cout << " (" << process_memory() << ")";
        std::cout << "\n";
    }
    const gui::Widget* focus = s.gui_active() ? s.runtime().canvas().focus : nullptr;
    std::string focus_name = focus ? (focus->name.empty() ? std::string("(unnamed)") : focus->name) : "";
    if (const gui::Widget* top = s.gui_active() ? s.runtime().top() : nullptr; top && top->cc) {
        const auto& cc = *top->cc;
        focus_name += " selection " + (cc.sel_w ? cc.sel_w->name : std::to_string(cc.sel)) + " panel " +
                      std::to_string(cc.panel);
    }
    if (focus_name != focused_name) {
        focused_name = focus_name;
        if (!focus_name.empty()) std::cout << "Front end: frame " << s.frames() << " focus " << focus_name << "\n";
    }
    // A screen is captured once it has settled (its fade-in done).
    const std::int64_t settle = now.rfind("MOVIE_", 0) == 0 ? 180 : 45;
    if (!view_captured && s.frames() - view_since >= settle) {
        view_captured = true;
        if (now.rfind("STATE", 0) != 0) capture(now);
    }
    if (options.capture_frames.count(s.frames())) capture("FRAME");
}

void FrontendApp::Impl::present() {
    if (host && host->minimized()) ++minimized_presents_skipped;
    if (!host || !screen || host->minimized()) return;
    const std::int64_t frame = session->frames();
    if (!test_lost.empty() && *test_lost.begin() <= frame) {  // port-only diagnostic (132win5): a loss at the frame boundary
        test_lost.erase(test_lost.begin());
        host->recover_device("--frontend-test-device-lost (port-only diagnostic) at front-end frame " + std::to_string(frame));
    }
    // Port-only (132win3): a device lost in any D3D call of the frame (device_lost.hpp) drops the frame; the
    // window re-creates the device (HostWindow::present) and the next frame makes the renderer again.
    // 132win5: the renderer's re-creation and the movie frame's upload are inside the same catch.
    bool drawn = false;
    const auto draw_begun = std::chrono::steady_clock::now();
    try {
        FrontendProfileScope scope(profile, "draw");
        if (host->device_generation() != screen_generation) make_screen();
        if (session->movie_playing() && session->movie_frame() && session->movie_serial() != screen_movie_serial) {
            screen_movie_serial = session->movie_serial();
            screen->set_texture(movie_slot, movie_picture());
        }
        host->sync_buffers();
        const float black[4] = {0, 0, 0, 1};
        gfx::clear_target(*host->gfx(), host->back_buffer(), black);
        // Port-only (checkpoint 132ws): under the wide aspect a mod's wide background for the screen (132md12,
        // "frontend.<screen>") goes under the 4:3 menu and replaces the side fill.
        bool wide_background = false;
        if (!session->movie_playing() && frame_wide(host->width(), host->height()))
            if (const gui::Widget* top = session->runtime().top(); top && session->gui_active())
                wide_background = screen->draw_wide_background(std::string("frontend.") + frontend_class_name(top->cls),
                                                               host->back_buffer(), host->width(), host->height());
        screen->draw(shown, host->back_buffer(), host->width(), host->height());
        // Port-only (checkpoint 132ws): under the wide aspect a menu's 4:3 picture gets the blurred side fill.
        if (!wide_background && !session->movie_playing() && !shown.batches.empty())
            screen->draw_wide_fill(host->back_buffer(), host->width(), host->height());
        draw_overlay(frame);
        draw_lan_notices();
        drawn = !device_lost_pending();
    } catch (const DeviceLost&) {
    }
    if (options.window_captures.count(frame) && window_captured != static_cast<std::uint64_t>(frame) + 1 &&
        !options.capture_dir.empty()) {
        char file[64];
        std::snprintf(file, sizeof file, "window_f%05lld.bmp", static_cast<long long>(frame));
        if (!drawn) {
            std::cout << "Front end: window capture at frame " << frame << " skipped: the frame was dropped (device lost)\n";
        } else if (host->capture(options.capture_dir / file)) {
            window_captured = static_cast<std::uint64_t>(frame) + 1;
            std::cout << "Front end: window capture (port-only diagnostic) " << (options.capture_dir / file).string() << " ("
                      << host->width() << " x " << host->height() << ")\n";
        }
    }
    const auto present_begun = std::chrono::steady_clock::now();
    {
        FrontendProfileScope scope(profile, "present");
        host->present(pacing ? pacing->sync_interval() : 1u);
    }
    if (overlay_profile) {
        const auto now = std::chrono::steady_clock::now();
        overlay_profile->add(omega::FrameProfile::render, std::chrono::duration<double>(present_begun - draw_begun).count());
        overlay_profile->add(omega::FrameProfile::present, std::chrono::duration<double>(now - present_begun).count());
    }
}

// Port-only (checkpoint 132win11): the frame-time overlay over the drawn frame (inside the fitted 4:3 frame's
// top-left corner, as the play draws it), before the window capture and the present.
void FrontendApp::Impl::draw_overlay(std::int64_t frame) {
    if (!overlay_on || !overlay_profile) return;
    const auto& a = overlay_profile->averages();
    const auto ms = [](double v) {
        std::ostringstream o;
        o << std::fixed << std::setprecision(2) << v;
        return o.str();
    };
    std::vector<std::string> lines;
    if (a.frames == 0) lines = {"FRONT END FRAME TIMES (MEASURING)"};
    else
        lines = {"FRAME " + ms(a.total) + " MS  " + ms(a.fps) + " FPS  MAX " + ms(a.max_total),
                 "FRONT END STEP " + ms(a.ms[omega::FrameProfile::sim]),
                 "DRAW " + ms(a.ms[omega::FrameProfile::render]) + "  PRESENT " + ms(a.ms[omega::FrameProfile::present])};
    const auto r = frame_rect(host->width(), host->height());
    const unsigned scale = FrameOverlay::scale_for(host->height());
    overlay.draw(*host->gfx(), host->back_buffer(), host->width(), host->height(), lines, r.x + 4 * scale,
                 r.y + 4 * scale, scale);
    if (overlay_logged != a.frames && options.window_captures.count(frame)) {
        overlay_logged = a.frames;
        std::cout << "Front end: frame overlay (port-only) at front-end frame " << frame << ":";
        for (const auto& t : lines) std::cout << " [" << t << "]";
        std::cout << "\n";
    }
}

// Port-only (checkpoint 132md5, research/MODDING.md "LAN: the mod report at join"): the LAN mod gate on a session
// config: the host's policy (--lan-mod-policy, else the lan_mod_policy setting), the joiner's report.
void FrontendApp::Impl::lan_mod_config(lan::LanSessionConfig& config) {
    if (!lan_mod_gate) {
        const std::string name = options.lan_mod_policy ? *options.lan_mod_policy : lan_mod_policy_setting(store->read());
        const auto policy = lan::parse_lan_mod_policy(name);
        if (!policy) throw std::runtime_error("LAN mod policy: gameplay, any, cosmetic, none or equal, not " + name);
        lan_mod_gate = std::make_shared<lan::LanModGate>(
            *policy, [] { return mods::active_online_report(); },
            [this](const std::string& line) {
                std::cout << "Front end: " << line << " (port-only, 132md5)\n";
                lan_notices.emplace_back(line, (session ? session->frames() : 0) + 480);  // about 8 s
                while (lan_notices.size() > 3) lan_notices.pop_front();
            });
        std::cout << "Front end: LAN mod policy (port-only, 132md5) " << lan::lan_mod_policy_name(*policy)
                  << (options.lan_mod_policy ? " (--lan-mod-policy)" : " (lan_mod_policy)") << "\n";
    }
    config.join_extension = [] { return lan::lan_mod_extension(mods::active_online_report()); };
    config.join_filter = [gate = lan_mod_gate](const lan::LanAddress& from, const std::string& name,
                                               std::span<const std::uint8_t> extension) {
        return gate->check(from, name, extension);
    };
}

// Port-only (checkpoint 132on2f): the connection path ("direct", "relay", the port mapping) as a front-end notice.
void FrontendApp::Impl::online_path_notices() {
    if (!online_lobby) return;
    for (auto& text : online_lobby->take_path_notices()) {
        std::cout << "Front end: connection notice shown (port-only, 132on2f) at front-end frame "
                  << (session ? session->frames() : 0) << ": " << text << "\n";
        lan_notices.emplace_back(std::move(text), (session ? session->frames() : 0) + 480);  // about 8 s
        while (lan_notices.size() > 3) lan_notices.pop_front();
    }
}

// Port-only (checkpoint 132md5): the host's joiner notices, over the fitted frame's bottom-left corner.
void FrontendApp::Impl::draw_lan_notices() {
    const std::int64_t now = session->frames();
    while (!lan_notices.empty() && lan_notices.front().second <= now) lan_notices.pop_front();
    if (lan_notices.empty()) return;
    const auto r = frame_rect(host->width(), host->height());
    const unsigned scale = FrameOverlay::scale_for(host->height());
    // Wrapped at spaces to 96 columns, or fewer to fit the fitted frame's width.
    const unsigned advance = std::max(1u, raster_overlay_text({"MM"}, scale).width - raster_overlay_text({"M"}, scale).width);
    const std::size_t columns =
        std::clamp<std::size_t>((r.width > 8 * scale ? r.width - 8 * scale : 0) / advance, 20, 96);
    std::vector<std::string> lines;
    for (const auto& n : lan_notices) {
        std::string rest = n.first;
        while (rest.size() > columns) {
            std::size_t cut = rest.rfind(' ', columns);
            if (cut == std::string::npos || cut == 0) cut = columns;
            lines.push_back(rest.substr(0, cut));
            rest.erase(0, rest[cut] == ' ' ? cut + 1 : cut);
        }
        lines.push_back(rest);
    }
    const unsigned height = raster_overlay_text(lines, scale).height;
    const unsigned bottom = r.y + r.height;
    lan_notice_overlay.draw(*host->gfx(), host->back_buffer(), host->width(), host->height(), lines, r.x + 4 * scale,
                            bottom > height + 4 * scale ? bottom - height - 4 * scale : 0, scale);
}

// Port-only (checkpoint 132win11): the window's key events (the front end reads the keyboard's state; only the
// overlay key is taken from them), then the --frontend-test-overlay-key posts.
void FrontendApp::Impl::overlay_events() {
    for (const auto& e : host->take_events()) {
        if (e.type != HostEvent::Type::key || !overlay_key || e.value != *overlay_key) continue;
        if (page && page->capturing()) continue;  // a key being bound on the Port Settings page
        overlay_on = !overlay_on;
        std::cout << "Front end: frame overlay (port-only) " << (overlay_on ? "on" : "off") << " (" << key_name(*overlay_key)
                  << ") at front-end frame " << session->frames() << "\n";
    }
    if (overlay_key && options.test_overlay_key.count(session->frames() + 1)) {
        std::cout << "Front end: --frontend-test-overlay-key (port-only test) posts " << key_name(*overlay_key)
                  << " to the game window at front-end frame " << session->frames() + 1 << "\n";
        PostMessageW(host->handle(), WM_KEYDOWN, *overlay_key, LPARAM(1));
    }
}

// Port-only (checkpoint 132win11): the Port Settings page's key capture. While it waits for a key it gets the
// keyboard (keys: the focused window's state, or none in a scripted run, whose keys come from
// --port-settings-test-keys) and the GUI gets no buttons, until every input is up after the capture.
std::uint16_t FrontendApp::Impl::page_input(std::uint16_t held, bool keys_down, const KeyboardState& keys) {
    if (!page) return held;
    if (page->capturing()) page->feed_keys(keys);
    for (const auto& line : page->take_log())
        std::cout << "Front end: " << line << " at front-end frame " << session->frames() << "\n";
    return page->holds_input(held != 0 || keys_down) ? std::uint16_t(0) : held;
}

// Port-only diagnostic (checkpoint 132win5): FrontendOptions::test_device_lost_sweep, armed after each loop
// iteration for the next. An armed loss that fired was recovered by the iteration's present (the window
// re-created the device; the next draw makes the renderer again); one that did not fire means the iteration
// had fewer checked calls than the index, so every one of them has lost the device once: the sweep ends.
void FrontendApp::Impl::arm_device_hooks() {
    if (sweep_call) {
        if (device_lost_injection.load() >= 0) {
            device_lost_inject(-1);
            std::cout << "Device-lost sweep (port-only diagnostic) complete at front-end frame " << session->frames()
                      << ": the device was lost at each of the " << *sweep_call
                      << " checked D3D calls of the iterations from frame " << sweep_first
                      << " in turn and recovered every time; calls:";
            std::map<std::string, unsigned> sites;
            for (const auto& site : sweep_sites) ++sites[site];
            for (const auto& [site, n] : sites) std::cout << " [" << site << "] x" << n;
            std::cout << "\n";
            sweep_call.reset();
            sweep_sites.clear();
        } else {
            sweep_sites.push_back(device_lost_by());
            ++*sweep_call;
        }
    }
    if (!sweep_call && host && !sweep_starts.empty() && *sweep_starts.begin() <= session->frames() + 1) {
        sweep_starts.erase(sweep_starts.begin());
        sweep_call = 0;
        sweep_first = session->frames() + 1;
        std::cout << "Device-lost sweep (port-only diagnostic) from front-end frame " << sweep_first << "\n";
    }
    if (sweep_call) device_lost_inject(*sweep_call, "--frontend-test-device-lost-sweep");
}

// Port-only diagnostics (checkpoint 132win5): --frontend-test-minimize / --frontend-test-close on the window,
// before the iteration whose step makes that front-end frame.
void FrontendApp::Impl::window_tests() {
    if (!host) return;
    const std::int64_t next = session->frames() + 1;
    if (options.test_minimize_at >= 0 && next == options.test_minimize_at) host->simulate_minimized(true);
    if (options.test_minimize_at >= 0 && next == options.test_minimize_at + options.test_minimize_frames)
        host->simulate_minimized(false);
    if (options.test_focus_loss_at >= 0 && next == options.test_focus_loss_at) host->simulate_focus(false);
    if (options.test_focus_loss_at >= 0 && next == options.test_focus_loss_at + options.test_focus_loss_frames)
        host->simulate_focus(true);
    for (const auto at : options.test_alt_enter)
        if (next == at) host->simulate_alt_enter();
    if (options.test_close >= 0 && next == options.test_close) {
        std::cout << "Front end: --frontend-test-close (port-only diagnostic) posts Alt+F4 to the game window at front-end frame "
                  << next << "\n";
        PostMessageW(host->handle(), WM_SYSKEYDOWN, VK_F4, LPARAM(1) | (LPARAM(1) << 29));
    }
}

// Port-only (checkpoint 132win5): a minimised window keeps the front end running (the PS2 game has no pause on
// focus loss or minimise, and the port adds none): its steps go on at 1/60 s, so its sound keeps being mixed
// (no underrun gap) and a restore brings no catch-up burst. Nothing is presented while minimised, so the pacing
// is timer-paced (no vertical-blank lock to wait on) until the restore reads the display's refresh again.
void FrontendApp::Impl::follow_minimized() {
    const bool minimized = host && host->minimized();
    if (minimized == was_minimized) return;
    was_minimized = minimized;
    if (pacing) pacing->set_refresh(minimized || host->kept_hidden() ? 0.0 : display_refresh_hz(host->handle()));
    std::cout << "Front end: window " << (minimized ? "minimised" : "restored") << " at front-end frame " << session->frames()
              << " (port-only: the steps go on" << (minimized ? ", nothing presented" : "")
              << (pacing ? ", " + pacing->mode_text() : std::string())
              << ")\n";
}

// Checkpoint 132win12: a hidden window (headless runs) counts as focused unless a test reports otherwise.
bool FrontendApp::Impl::window_focused() const {
    if (!host) return false;
    return host->focus_simulated() || (host->visible() && !host->kept_hidden()) ? host->focused() : true;
}

void FrontendApp::Impl::apply_focus_mute() {
    if (!sound) return;
    bool background = false;
    try {
        background = background_audio_setting(store->read());
    } catch (const std::exception& e) {
        std::cerr << "Front end: port settings: " << e.what() << "; background_audio=0\n";
    }
    sound->set_background_muted(!was_focused && !background);
    for (const auto& line : sound->take_log()) std::cout << "Front end: " << line << " at frame " << session->frames() << "\n";
}

void FrontendApp::Impl::follow_focus() {
    const bool focused = window_focused();
    if (focused == was_focused) return;
    was_focused = focused;
    if (!focused && gamepad) gamepad->set_vibration(false, 0);
    std::cout << "Front end: window focus " << (focused ? "back" : "lost") << " at front-end frame " << session->frames()
              << (focused ? " (port-only: the sound and the input are back)"
                          : " (port-only: the input ignored, the steps go on)")
              << "\n";
    apply_focus_mute();
}

// 002cbad0 inside a card wait's poll (00441c10 -> 002cc260(0, 0)): the canvas is drawn and presented
// (0037a9f0(005381f0)) within the GUI event that made the card call, so a popup opened before the call
// (MC_NOREMOVE) is on screen while the card is written. The first such frame with the message box on top
// is captured once per front-end frame as CARDWAIT.
void FrontendApp::Impl::nested_frame() {
    FrontendSession& s = *session;
    shown.batches.clear();
    drawer->draw_canvas(shown, s.runtime(), s.now_ms());
    sync_zeus();
    const gui::Widget* top = s.runtime().top();
    const std::string name = top ? frontend_class_name(top->cls) : std::string("(none)");
    const bool wait_thread = s.nested_in_wait_thread();  // 132ln4: the wait thread's frame is not a card wait
    std::cout << "Front end: frame " << s.frames() << (wait_thread ? " wait thread" : " card wait")
              << ": nested GUI frame " << s.nested_frames() << " (top " << name << ")\n";
    if (top && top == s.runtime().msgbox && card_wait_captured != s.frames()) {
        card_wait_captured = s.frames();
        capture(wait_thread ? "WAITTHREAD" : "CARDWAIT");
    }
    present();
}

// PORT-ONLY test (checkpoint 132cc10): the simulated PS2 card state (AgentCardSimulation) over the folder card.
void FrontendApp::Impl::simulate_card(const std::string& spec, const char* from) {
    session->card().simulate(parse_agent_card_simulation(spec));
    std::cout << "Front end: simulated memory card (PORT-ONLY test, " << from << ") at front-end frame " << session->frames()
              << ": " << describe_agent_card_simulation(session->card().simulation()) << "\n";
}

FrontendResult FrontendApp::Impl::loop() {
    FrontendSession& s = *session;
    result.level.reset();
    result.closed = false;
    const auto period = std::chrono::duration<double>(1.0 / 60.0);
    auto last = std::chrono::steady_clock::now();
    double accumulator = 0;
    // Checkpoint 132win3 (port-only, frame_pacer.hpp): the live loop waits on a high-resolution timer (or the
    // vertical blank, when the display's refresh is a multiple of 60 Hz) for its next 1/60 s step.
    // Checkpoint 132win4: a scripted or hidden front end is paced too when asked (--frame-pacing on), so its
    // pacing can be measured headless; it still steps once per loop (the script's frames are unchanged). A
    // window kept hidden or no window counts as an unknown refresh (timer-paced), as the play's hidden runs do.
    pacing.reset();
    const unsigned this_pass = pass++;
    if (options.frame_pacing && (host || !script.empty())) {
        pacing.emplace(period.count(), vsync);
        pacing->set_cadence_allowed(options.frame_cadence);
        pacing->start(last);
        pacing->set_refresh(host && !host->kept_hidden() ? display_refresh_hz(host->handle()) : 0.0);
        std::cout << "Front end: frame pacing (port-only) vsync " << (vsync ? "on" : "off") << ", display "
                  << pacing->refresh_hz() << " Hz, " << pacing->mode_text() << "\n";
    }
    auto refresh_at = last + std::chrono::seconds(2);
    std::uint64_t size_serial = host ? host->size_serial() : 0;
    arm_device_hooks();  // port-only diagnostic (132win5): a sweep from the pass's first frame
    while (!s.handoff()) {
        // Port-only (checkpoint 132win12): --frontend-test-crash, with or without the game window.
        if (options.test_crash_at >= 0 && s.frames() + 1 == options.test_crash_at)
            crash::test_crash(options.test_crash_kind, "the front end");
        if (host) {
            overlay_events();  // the front end reads the keyboard's state (keyboard_pad.hpp); 132win11: the overlay key
            if (host->close_requested()) {
                std::cout << "Front end: the game window's close (Alt+F4 / the close button) at front-end frame " << s.frames()
                          << ": the front end ends (port-only)\n";
                result.closed = true;
                break;
            }
            window_tests();
            follow_minimized();
            follow_focus();
        }
        if (!script.empty()) {
            if (script_index >= script.size()) break;
            if (card_step != script_index) {
                card_step = script_index;
                if (script[script_index].card) simulate_card(*script[script_index].card, "script");
                if (script[script_index].keys) {
                    for (const auto& k : usb_key_reports(*script[script_index].keys)) s.usbkb_queue(k);
                    std::cout << "Front-end script: keys \"" << *script[script_index].keys << "\" queued (port-only)\n";
                }
                if (script[script_index].touch_file) {  // PORT-ONLY test (132on2c)
                    std::ofstream(std::filesystem::u8path(*script[script_index].touch_file)) << s.frames() << "\n";
                    std::cout << "Front-end script: touch \"" << *script[script_index].touch_file << "\" at front-end frame "
                              << s.frames() << " (port-only)\n";
                }
                if (script[script_index].capture) capture(*script[script_index].capture);  // PORT-ONLY test (132on2h)
                script_waiting = script[script_index].wait_file.has_value() || script[script_index].wait_games.has_value();
            }
            std::uint16_t held = script[script_index].held;
            if (script_waiting) {  // PORT-ONLY test (132on2c): the step waits, buttons up, for its file / games
                const auto& st = script[script_index];
                std::error_code ec;
                const bool file = !st.wait_file || std::filesystem::exists(std::filesystem::u8path(*st.wait_file), ec);
                const bool games = !st.wait_games ||
                                   (online_lobby ? online_lobby->game_list().rows.size() >= *st.wait_games
                                                 : lan_session && lan_session->games().size() >= *st.wait_games);
                if (file && games) {
                    script_waiting = false;
                    if (st.wait_file)
                        std::cout << "Front-end script: waitfile \"" << *st.wait_file << "\" found at front-end frame "
                                  << s.frames() << " (port-only)\n";
                    if (st.wait_games)
                        std::cout << "Front-end script: waitgames " << *st.wait_games << " met at front-end frame "
                                  << s.frames() << " (port-only)\n";
                } else {
                    held = 0;
                }
            }
            if (!script_waiting && --script_left <= 0 && ++script_index < script.size()) script_left = script[script_index].frames;
            if (pacing) pacing->next_frame();  // the wait only: one scripted step per loop
            profile.begin();
            if (overlay_profile) overlay_profile->begin_frame();
            if (pad_simulation) read_simulated_devices();  // 132win12: the simulated controller devices
            else if (simulated_pads) track_pad(std::string(), false);  // 132win6: the simulated device list only
            held = page_input(held, false, KeyboardState{});
            const auto step_begun = std::chrono::steady_clock::now();
            step(held);
            if (overlay_profile)
                overlay_profile->add(omega::FrameProfile::sim,
                                     std::chrono::duration<double>(std::chrono::steady_clock::now() - step_begun).count());
            present();
            profile.end(s.frames(), view);
            if (overlay_profile) overlay_profile->end_frame(static_cast<std::uint64_t>(s.frames()));
            arm_device_hooks();
            continue;
        }
        int steps = 0;
        if (pacing) {
            if (!host->kept_hidden() && !host->minimized() &&
                (host->size_serial() != size_serial || std::chrono::steady_clock::now() >= refresh_at)) {
                size_serial = host->size_serial();
                refresh_at = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                pacing->set_refresh(display_refresh_hz(host->handle()));
            }
            steps = pacing->next_frame();
        } else {
            const auto now = std::chrono::steady_clock::now();
            accumulator += std::chrono::duration<double>(now - last).count();
            last = now;
            steps = std::min(4, static_cast<int>(accumulator / period.count()));
            accumulator -= steps * period.count();
            if (accumulator > 0.25) accumulator = 0;
        }
        profile.begin();
        if (overlay_profile) overlay_profile->begin_frame();
        const auto steps_begun = std::chrono::steady_clock::now();
        for (int k = 0; k < steps && !s.handoff(); ++k) {
            // The controller as the DualShock 2 report (the 0055a44c held
            // mask) or-ed with the keyboard's buttons, while focused.
            // Checkpoint 132win6: the keys through the menu bindings (input_bindings.hpp); the controller's
            // presence through PadConnection (the controller-removed state of 00369c50 / 00465870).
            std::uint16_t held = 0;
            std::optional<GamepadDevice::Reading> pad;
            if (gamepad) pad = gamepad->read();
            const bool focused = host && host->focused();
            const KeyboardState keys = focused ? host_keyboard_state() : KeyboardState{};
            track_pad(pad ? pad->source : std::string(), any_bound_key(menu_bindings, keys));
            if (focused) {
                if (pad) held = pad->pad.held;
                held = static_cast<std::uint16_t>(held | bound_buttons(menu_bindings, keys));
            }
            held = page_input(held, std::any_of(keys.begin(), keys.end(), [](bool d) { return d; }), keys);
            step(held);
        }
        if (overlay_profile)
            overlay_profile->add(omega::FrameProfile::sim,
                                 std::chrono::duration<double>(std::chrono::steady_clock::now() - steps_begun).count());
        // Checkpoint 132cp3: the options screen's pad vibration (00369920 bytes) to the controller.
        if (gamepad && was_focused) gamepad->set_vibration(s.vibration().last[0].small_on(), s.vibration().last[0].large());
        present();
        if (steps > 0) profile.end(s.frames(), view);
        if (overlay_profile && steps > 0) overlay_profile->end_frame(static_cast<std::uint64_t>(s.frames()));
        arm_device_hooks();
        if (steps == 0 && !pacing) Sleep(1);
    }
    if (sweep_call) {  // the pass ended inside a sweep: disarmed so that the loss cannot fire in the level
        device_lost_inject(-1);
        std::cout << "Device-lost sweep (port-only diagnostic) cut short at front-end frame " << s.frames()
                  << " (the pass ended) after " << *sweep_call << " calls\n";
        sweep_call.reset();
        sweep_sites.clear();
    }
    profile.summary(this_pass);
    if (minimized_presents_skipped) {
        std::cout << "Front end: " << minimized_presents_skipped
                  << " presents skipped while the window was minimised (port-only; the steps went on)\n";
        minimized_presents_skipped = 0;
    }
    if (pacing) {
        std::cout << "Front end: frame pacing (port-only) " << pacing->summary() << "\n";
        if (!options.frame_times.empty()) {
            std::filesystem::path file = options.frame_times;
            file.replace_filename(file.stem().string() + "_frontend" + std::to_string(this_pass) + file.extension().string());
            pacing->write_times(file);
            std::cout << "Front end: frame pacing (port-only) frame intervals -> " << file.string() << "\n";
        }
    }
    result.frames = s.frames();
    if (wav && !s.handoff()) wav->close();
    if (s.handoff() && s.multiplayer()) {
        // Checkpoint 132ln4: the state-3 multiplayer path (005721c8 = 1) handed off to the level load.
        std::cout << "Front end: level load (state 0xe) of " << s.level() << " in a network game (005721c8 = 1, host role "
                  << "005721c0 = " << (s.host_role() ? 1 : 0) << ") after " << s.frames() << " frames\n";
        // Checkpoint 132ln5: the level-load wrapper 001693f0's network sequence runs on the native DME.cc game side
        // (LanNetCore, PORT-ONLY glue: the DME API on the LAN session's transport, the DME thread model). The DME
        // update thread start 0042a770 and the team setup 0016a970 (checkpoint 132ln5c) run here; checkpoint
        // 132ln5e: the play hosts the level load 001350e0 and the rest (LanNetCore::level_load_finish).
        if (!lan_session || !lan_session->transport())
            throw std::runtime_error("Level load 001693f0 (state 0xe) in a network game without a LAN session: Online play "
                                     "(the DME connect 0042ab00) is not ported");
        lan_core = std::make_unique<lan::LanNetCore>(*lan_session->transport(), [this] { return lan_session->clock_ms(); },
                                                     s.host_role());
        // Checkpoint 132ln5f: the lobby keep-alive 002ca580 in the level serves LanSession's discovery (the host's
        // advertisement goes on, as the Medius listing does), not its transport (LanDme polls that).
        lan_core->set_lobby([this] { lan_session->pump_lobby(); });
        try {
            lan_core->level_load_begin(nullptr);
        } catch (const std::exception& e) {
            auto& m = lan_core->memory();
            std::cout << "Front end: network level load (port-only LanNetCore, 132ln5): 0042a770 started the DME update thread "
                      << "(00429380: 00572528 = " << m.read(0x572528, 1) << "; the thread ran " << lan_core->thread_runs()
                      << " time(s), 00429d30 with 005723d0 = " << m.read(0x5723d0, 1) << " (00165bc0: 0042af00 and the 72 "
                      << "registrations on the native DME library, 132ln5b)), 0016a970 native (team data 004ee620 = 0x"
                      << std::hex << m.read(0x4ee620) << std::dec << ", 132ln5c), client index "
                      << lan_session->transport()->local_index() << ", state 005724b0 = 0x"
                      << std::hex << m.read(0x5724b0, 1) << std::dec << "; stopped at: " << e.what() << "\n";
            throw;
        }
        auto& m = lan_core->memory();
        std::cout << "Front end: network level load (port-only LanNetCore, 132ln5e): 0042a770 started the DME update thread "
                  << "(the thread ran " << lan_core->thread_runs() << " time(s)), 0016a970 native (team data 004ee620 = 0x"
                  << std::hex << m.read(0x4ee620) << std::dec << "), client index " << lan_session->transport()->local_index()
                  << "; 001350e0 and the rest of 001693f0 are the play's\n";
    }
    if (s.handoff()) {
        result.level = s.level();
        std::cout << "Front end: level load (state 0xe) of " << s.level() << " after " << s.frames() << " frames\n";
        release_output();
    }
    return result;
}

FrontendApp::FrontendApp(DiscImage& disc, const FrontendOptions& options)
    : impl_(std::make_unique<Impl>(disc, options)) {}

FrontendApp::~FrontendApp() = default;

FrontendResult FrontendApp::run() { return impl_->loop(); }

FrontendSession& FrontendApp::session() { return *impl_->session; }
const std::map<std::uint32_t, Texture>& FrontendApp::zeus_images() const { return impl_->data->zeus_images(); }
PortSettings FrontendApp::port_settings() const { return impl_->port_settings_reader(); }
bool FrontendApp::vsync() const { return impl_->vsync; }
lan::LanNetCore* FrontendApp::lan_core() { return impl_->lan_core.get(); }

bool FrontendApp::network_session() const {
    const auto& m = *impl_;
    return m.lan_core || m.lan_session || m.lan_peer || m.online_lobby;
}

std::vector<std::string> FrontendApp::online_level_frame() {
    auto& m = *impl_;
    if (!m.online_lobby || !m.online_lobby->in_game()) return {};
    m.online_lobby->level_frame();
    for (const auto& line : m.online_lobby->take_log()) std::cout << "Front end: " << line << "\n";
    return m.online_lobby->take_path_notices();
}

void FrontendApp::online_level_end(const AgentStatistics* live, bool completed) {
    auto& m = *impl_;
    if (!m.online_lobby || !m.online_lobby->in_game()) return;
    m.online_live_stats = live;
    m.online_lobby->level_end(completed);
    m.online_live_stats = nullptr;
    for (const auto& line : m.online_lobby->take_log()) std::cout << "Front end: " << line << "\n";
}
std::string FrontendApp::port_settings_source() const {
    return impl_->store->where();
}

FrontendResult FrontendApp::resume_after_level(std::uint8_t state_50, std::uint8_t previous_51) {
    impl_->session->resume_after_level(state_50, previous_51);
    std::cout << "Front end: resumed after the level end (state 0x" << std::hex << int(state_50) << ", previous 0x"
              << int(previous_51) << std::dec << ")";
    if (!impl_->script.empty())
        std::cout << "; script line " << (impl_->script_index + 1) << " of " << impl_->script.size();
    std::cout << "\n";
    impl_->open_output();
    return impl_->loop();
}

FrontendResult run_frontend(DiscImage& disc, const FrontendOptions& options) {
    FrontendApp app(disc, options);
    return app.run();
}

}  // namespace omega
