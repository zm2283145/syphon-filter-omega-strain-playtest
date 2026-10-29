#include "frontend_session.hpp"

#include "agent_parts.hpp"
#include "appearance_parts.hpp"
#include "movie_subtitle_overlay.hpp"
#include "port_cheats.hpp"
#include "weapon_database.hpp"
#include "weapon_models.hpp"
#include "zone_profile.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace omega {

namespace {

[[noreturn]] void unreachable(std::uint32_t address, const std::string& what) {
    char text[16];
    std::snprintf(text, sizeof text, "%08x", address);
    throw std::runtime_error(std::string("Front end ") + text + ": " + what + " is not hosted");
}

bool iequal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
           });
}

// 0048c148: the front-end music names 002c9af0 sends (entries 3.. are null).
constexpr const char* music_names[3] = {"THEME_MAIN", "THEME_AGENCY", "THEME_ZEUS"};

// The host's EE timer model: T1_COUNT counts the 147.456 MHz bus clock / 256
// (9600 counts per 1/60 s frame) and wraps at 16 bits; the kernel's overflow
// handler is not modelled, so 00533870 stays 0 and the 64-bit tick counter
// 00533878 holds the wrapped part in 10 us units (count / 5.76).
constexpr std::uint64_t t1_per_frame = 9600;

// The GUI's sound descriptors (static init 004d8180: 001ca210(table, 3, name,
// 0, 0), the names at 004c2a18 + 0x10 * k; k 12..14 from 004d8458..004d8508),
// which 00465c50 / 00465b50 send.
struct GuiSoundTable {
    std::uint32_t table;
    const char* name;
};
constexpr GuiSoundTable gui_sounds[] = {
    {0x5862d0, "GS_PAGE_OPEN"},    {0x586470, "GS_PAGE_CLOSE"}, {0x586610, "GS_SELECT"},
    {0x5867b0, "GS_CONFIRM"},      {0x586950, "GS_INVALID"},    {0x586af0, "GS_BACKGROUND"},
    {0x586c90, "GS_TEXT_MOVE"},    {0x586e30, "GS_COMMANDOPEN"}, {0x586fd0, "GS_COMMANDLIGHT"},
    {0x587170, "GS_LEVEL_SELECT"}, {0x587310, "GS_MISSION"},    {0x5874b0, "GS_POPUP"},
    {0x587650, "GS_OBJTIV_REC"},   {0x5877f0, "GS_OBJTIV_DONE"}, {0x587990, "GS_3D_MAP_TURN"},
};

void deliver_sound(GameSoundManager& sound, const GameSoundRecord& descriptor, bool stop) {
    GameSoundMessage m;
    m.kind = stop ? GameSoundMessage::Kind::stop_20ecb0 : GameSoundMessage::Kind::play_20faa0;
    m.flag_20 = 1;
    m.request = sound.message_copy(descriptor, stop);
    sound.deliver(m);
}

}  // namespace

const char* gui_sound_name(std::uint32_t table) {
    for (const auto& s : gui_sounds)
        if (s.table == table) return s.name;
    return nullptr;
}

void gui_sound_deliver(GameSoundManager& sound, const char* name, bool stop) {
    deliver_sound(sound, sound.request(3, name, 0, 0), stop);
}

void gui_sound_send(GameSoundManager& sound, std::int32_t kind, std::int32_t type, const std::string& name) {
    if (kind < 0 || kind > 2) throw std::runtime_error("Front end: GUI sound message kind " + std::to_string(kind));
    const auto descriptor = sound.request(type, name, 0, 0);
    GameSoundMessage m;
    m.kind = kind == 0 ? GameSoundMessage::Kind::stop_20ecb0
                       : (kind == 1 ? GameSoundMessage::Kind::play_20faa0 : GameSoundMessage::Kind::quip_20f2a0);
    m.flag_20 = 1;
    m.request = sound.message_copy(descriptor, kind == 0);
    sound.deliver(m);
}

bool frontend_command_center_hosted(std::uint8_t online, std::uint8_t online_hosted, std::uint8_t lan_mode) {
    return !online || online_hosted || lan_mode;
}

FrontendSession::FrontendSession(FrontendServices services)
    : services_(std::move(services)),
      rt_(*this),
      decor_(services_.decor, 0),
      card_(services_.card_root),
      save_buffer_(std::make_unique<AgentSaveBuffer>()) {
    if (!services_.awards) throw std::runtime_error("Front end: the award registries are required");
    // 004187c0 / 004191d0 / the popup queue of the application object.
    rt_.native_app = true;
    rt_.load_observer = [this](gui::Widget* w, const GuiNode& root) { decor_.attach(rt_, *w, root); };
    decor_.bridge_install(rt_);
    // 0043fbb0("%s\COMMON\sfo_char.map") of 003fdf40; byte 0055d450 its result.
    rt_.charmap.ready = rt_.charmap.load(services_.charmap);
    // 00165754 stores byte 004ffbb1 = 1 (the title's idle attract enable).
    rt_.g.keyboard = 1;
    rt_.g.pad_state = 6;
    rt_.g.card.present = 1;
    // The boot state of the main update (00167e60): state 9 (INTRO), the
    // front end and movies enabled, an empty first movie-list name.
    g_.state_50 = 9;
    g_.front_end_62 = 1;
    g_.movies_61 = 1;
    g_.level_140 = rt_.g.level_name;
    gui_.names_39 = {std::string()};
}

FrontendSession::~FrontendSession() = default;

void FrontendSession::record(const std::string& text) {
    if (logged_.insert(text).second) log_.push_back(text);
}

std::vector<std::string> FrontendSession::take_log() { return std::exchange(log_, {}); }

std::vector<std::int16_t> FrontendSession::take_movie_audio() { return std::exchange(audio_, {}); }

// G and the GUI object alias the runtime's globals: 004ffba0 / 004ffba1
// (G+50 / G+51), 004ffc90 (G+140), the GUI object's +30..+35, +39, +13c and
// +24c, and 00533870.
void FrontendSession::to_runtime() {
    auto& r = rt_.g;
    r.app_state = static_cast<std::int8_t>(g_.state_50);
    r.app_state_prev = static_cast<std::int8_t>(g_.previous_51);
    r.level_name = g_.level_140;
    r.app_mode = gui_.names_39.empty() ? std::string() : gui_.names_39[0];
    r.app_attract = gui_.count_13c;
    r.app_30 = gui_.multiplayer_30;
    r.b_21c8 = g_.mp_5721c8;   // the same byte 005721c8 (132ln4: the state-3 multiplayer path sets it)
    r.app_32 = gui_.mp_32;
    r.app_33 = gui_.mp_33;
    r.app_34 = gui_.level_load_34;
    r.app_35 = gui_.done_35;
    r.app_24c = gui_.mission_24c;
    r.t1_base = g_.timer_base_533870;
}

void FrontendSession::from_runtime() {
    const auto& r = rt_.g;
    g_.state_50 = static_cast<std::uint8_t>(r.app_state);
    g_.previous_51 = static_cast<std::uint8_t>(r.app_state_prev);
    g_.level_140 = r.level_name;
    if (gui_.names_39.empty()) gui_.names_39.emplace_back();
    gui_.names_39[0] = r.app_mode;
    gui_.count_13c = r.app_attract;
    gui_.multiplayer_30 = r.app_30;
    g_.mp_5721c8 = r.b_21c8;
    gui_.mp_32 = r.app_32;
    gui_.mp_33 = r.app_33;
    gui_.level_load_34 = r.app_34;
    gui_.done_35 = r.app_35;
    gui_.mission_24c = r.app_24c;
    g_.timer_base_533870 = r.t1_base;
}

template <class F> void FrontendSession::in_runtime(F&& f) {
    to_runtime();
    f();
    from_runtime();
}

std::uint32_t FrontendSession::timer_t1() {
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(frames_) * t1_per_frame) & 0xffffU);
}

gui::PadInput FrontendSession::pad_input() const {
    gui::PadInput in;
    in.current[0] = held_;
    in.previous[0] = previous_held_;
    const std::uint64_t count = static_cast<std::uint64_t>(frames_) * t1_per_frame;
    in.t1 = static_cast<std::uint32_t>(count & 0xffffU);
    in.ticks = static_cast<std::int64_t>(static_cast<double>(count - in.t1) / 5.76);
    return in;
}

void FrontendSession::set_pad_state(std::int32_t state) { rt_.g.pad_state = state; }

std::int32_t FrontendSession::pad_state() const { return rt_.g.pad_state; }

void FrontendSession::frame(std::uint16_t held) {
    // 00369c50: no read while pad 0 is not ready (checkpoint 132win6); the masks keep their last values.
    if (rt_.g.pad_state == 6) {
        previous_held_ = held_;
        held_ = held;
    }
    held = held_;
    ++frames_;
    ms_ = static_cast<std::int32_t>(frames_ * 1000 / 60);   // 003698b0
    drawn_ = false;
    {
        OMEGA_ZONE("fs.prologue");
        prologue();
    }
    OMEGA_ZONE("fs.game_flow");
    run_game_flow_frame(g_, gui_, app_, *this, held);
    // 00168bcc: the lobby state 5 runs later in the same frame. Only the state-3 multiplayer path with app +32 set
    // and +33 clear reaches it (00168008), and no retail code stores a nonzero +32 (132ln4: the one store 002a544c
    // is in the unreferenced 002a5440), so neither the LAN host nor the LAN joiner gets here.
    if (g_.state_50 == 5)
        unreachable(0x168bcc, "the lobby state 5 (00167630: IALOBBYN.HOG, the create 0042a870(0) / connect 0042a780)");
}

void FrontendSession::schedule(std::int32_t delay, std::function<void()> event) {
    events_.enqueue(delay, std::move(event));
}

void FrontendSession::prologue() {   // 00167c40 .. 00167dd0
    // The ms clock: (*00533878 + cvt.w.s(float(T1 + *00533870) / 5.76)) / 100.
    const std::uint64_t count = static_cast<std::uint64_t>(frames_) * t1_per_frame;
    const std::uint32_t t1 = timer_t1();
    const auto ticks = static_cast<std::int64_t>(static_cast<double>(count - t1) / 5.76);
    const auto part = static_cast<std::int32_t>(static_cast<float>(t1 + g_.timer_base_533870) / 5.76F);
    const auto ms = static_cast<std::int32_t>((ticks + part) / 100);
    if (clock_ms_ == -1) clock_ms_ = ms;
    const std::int32_t delta = std::max(0, ms - clock_ms_);
    // 003e9e30(delta, byte 004ffbb7 ? 0 : delta): the front end never sets
    // 004ffbb7 (game +67), so gameSound's events count the delta either way.
    events_.advance(delta);
    clock_ms_ = ms;
    // 003eb340(003eba50()): fstream.cc's loader (FstreamLoader, 119o) only
    // queues a quip's LPD, which no front-end request makes, so its count
    // is 0 and gameSound gets 0020e860(0), the resume.
    if (services_.sound) {
        GameSoundMessage m;
        m.kind = GameSoundMessage::Kind::pause_20e860;
        m.pause_24 = 0;
        services_.sound->deliver(m);
    }
}

template <class F> void FrontendSession::front_end_sound(F&& f) {
    // 003caa50(004ffb50, gameSound); 004ffc90 saved, strncpy "FRONTEND"
    // (004acf48), the calls, then the saved name copied back.
    if (!services_.sound) return;
    const std::string saved = g_.level_140;
    if (services_.sound_level) services_.sound_level("FRONTEND");
    f(*services_.sound);
    if (services_.sound_level) services_.sound_level(saved);
}

void FrontendSession::fn_3e9c20() {
    // 003e9c20 (state 3): the list's events with +5 set run, the rest are
    // dropped; gameSound's (0036b180(0, 0x10)) have +5 = 0, the only kind here.
    events_.clear();
}

// ---------------------------------------------------------------- GameFlowHost

void FrontendSession::fn_37e780(const VideoModeRequest& m) {
    record("0037e780 video mode " + std::to_string(m.width) + "x" + std::to_string(m.height) + " flags " +
           std::to_string(m.flags));
}

void FrontendSession::fn_2c7c40(std::string_view name, std::uint32_t) {
    unreachable(0x2c7c40, "the splash '" + std::string(name) + "' (G+62 is set)");
}

void FrontendSession::fn_136710() {
    // 00136710: once (byte 004e9ff0): 00147400, 0040e4c0, the AWARDS script
    // 003e1b60 and 00336700 for the agent.
    if (constructed_) return;
    constructed_ = true;
    record("00147400 / 0040e4c0 / 003e1b60 boot init (the award registries are loaded by the host)");
    construct_agent(AgentRecord(agent_), agent_state_, services_.awards->registries);
}

void FrontendSession::fn_2cb8c0(std::uint32_t a1) {
    // 002cb8c0(app, a1): once (app +29).
    if (app_29_) return;
    record("00165950 / 00414900 canvas init, 004185a0 fonts and 00413c70.. NTSC.HOG mounts (host-loaded)");
    rt_.canvas_create();   // 00414900: a new canvas after 002cb160's 004147b0
    if (a1 == 0) record("002cb8c0 colour table 00539170 -> video +1f0");
    app_29_ = 1;
}

void FrontendSession::fn_2cc3e0() {
    // 002cc3e0: the first entry (byte 0048c248) clears app +2c, loads the options file 0045a150(*004ffd2c)
    // and records the rest.
    if (first_entry_) {
        app_2c_ = 0;
        options_load();
        record("002cc980 / 003cba90 first front-end entry");
        first_entry_ = false;
    }
    gui_start();
    g_.video_5383d4 = app_.flags_250 == 2 ? 0x32 : 0x3c;
}

void FrontendSession::options_load() {   // 0045a150(*004ffd2c)
    gui::GameOptionsRecord local;   // 0045a7a0's defaults
    // !byte 0055a834 (the folder card is never busy) && W +59d; 0040dbb0 == 0x24; 0040df00 read; +0 == 1.
    const AgentCardState s = card_.state(rt_.charmap.ready != 0, rt_.charmap.count);
    const char* failed = nullptr;
    std::array<std::uint8_t, gui::options_file_size> bytes = gui::options_file_bytes(local);
    if (!s.card_present) failed = "no card";
    else if (card_.size(gui::options_file_name) != gui::options_file_size) failed = "no 0x24-byte SFO.CFG";
    else if (!card_.read(gui::options_file_name, bytes)) failed = "the read failed";
    if (!failed) {
        local = gui::options_file_record(bytes);
        if (local.w0 != 1) failed = "+0 is not 1";
    }
    if (failed) {
        record(std::string("0045a150 options file: ") + failed + " (the record keeps its values)");
        return;
    }
    const std::uint32_t mask = gui::options_diff(rt_.g.options, local);   // 0045a600
    rt_.g.options = local;
    rt_.options_apply(local, mask);   // 0045a3a0
    char text[96];
    std::snprintf(text, sizeof text, "0045a150 options file SFO.CFG loaded: 0045a3a0 bits %x", mask);
    record(text);
}

std::uint32_t FrontendSession::options_save(const gui::GameOptionsRecord& r) {   // 0045a2a0
    const auto bytes = gui::options_file_bytes(r);
    const bool ok = card_.write(gui::options_file_name, bytes);   // 0040e090(W, name, buf, 0x24, 1)
    record(std::string("0045a2a0 options file SFO.CFG: ") + (ok ? "1" : "0"));
    return ok ? 1 : 0;
}


// *0055a838 = 00441c10 in a card wait: 002cc260(0, 0). Byte 0051eeb0 guards it; while the "please wait" thread
// runs (app +310, 002c82f0) the a1 != 0x3039 form skips the frame. 002cbb30 with 0051eeb0 set: the dt clock,
// then 00415b60 (message 7) instead of the flush / pads / +60 update, then the gameSound frame; 002cbad0: the
// draw and present. The caller is inside the GUI runtime already (an event of the frame's update), so no state
// is copied.
void FrontendSession::card_poll() {
    auto& guard = rt_.g.debug_triangle;   // byte 0051eeb0
    if (guard) return;
    guard = 1;
    if (!app_310_) nested_service();
    guard = 0;
}

void FrontendSession::nested_service() {
    if (app_28_) {
        const std::int32_t now = ms_;
        if (now - last_update_ms_ > 100) last_update_ms_ = now - 100;
        const float dt = static_cast<float>(now - last_update_ms_) / 1000.0F;
        rt_.nested_update(dt);
        last_update_ms_ = now;
        if (!loading_51ee90_ && services_.sound) services_.sound->frame();
        drawn_ = true;
        ++nested_frames_;
        if (services_.nested_frame) services_.nested_frame();
        else if (nested_wait_thread_) record("002cbad0 in the wait thread: the canvas draw and present (no window)");
        else record("002cbad0 in a card wait: the canvas draw and present (no window)");
    }
}

void FrontendSession::gui_start() {   // 002cb3d0(app, 0)
    if (app_28_) return;
    if (app_37_) {
        gui_resume();
        return;
    }
    fn_2cb8c0(0);
    in_runtime([&] {
        auto& canvas = rt_.canvas();
        rt_.keyboard_path = 1;   // canvas +51 = (a1 == 0)
        // 004158c0(canvas, 0) then (canvas, 1): the enable flag cycles; with
        // +52 and +51 set the enable calls 003d1770 / 003d1730 on the USB
        // keyboard 0055a700 (not hosted).
        canvas.flags &= ~gui::flag_enabled;
        canvas.flags |= gui::flag_enabled;
        if (rt_.ascii_keys && rt_.keyboard_path) record("003d1770 / 003d1730 USB keyboard 0055a700");
        rt_.repeat_mask = 0xf000;   // +68 = 0, then |= 0xf000 (a1 == 0)
        rt_.msgbox = rt_.create("GuiMessageBox", "POPUP");
        record("002cb4d8 colour table; 004294c0 network (none); 002c7d90(1, 0, 0) loading screen");
        app_30c_ = -1;
        g_.level_140 = rt_.g.level_name;
        if (!services_.sound) record("003caa50 gameSound init 001c9bf0 / level load 001c98d0 of FRONTEND (no sound)");
        // The init (class *004f5370) and level load (*004f5378) messages, 003c9420 transport 1.
        front_end_sound([](GameSoundManager& s) {
            s.init();
            if (!s.level_load()) throw std::runtime_error("Front end 002cb3d0: the gameSound level load 001c98d0 failed");
        });
        rt_.send(&canvas, &canvas, 0x10, 5, 0);
        if (app_2c_) unreachable(0x2cb6f4, "the app +2c preload");
        gui::Widget* screen = nullptr;
        if (rt_.g.app_35) {
            rt_.g.app_35 = 0;
            screen = rt_.factory("GuiCommandCenter", "CMDCENTR");
            if (!frontend_command_center_hosted(rt_.g.app_30, rt_.g.app_32, rt_.g.lan_mode))
                unreachable(0x2ca6f0, "the online command centre");
        } else {
            screen = rt_.factory("GuiTitleScreen", "TITLESCR");
        }
        if (!screen) throw std::runtime_error("Front end 002cb3d0: the first screen did not load");
        record("002c9ee0 / 00145160 / 002c7d90(0, 0, 0)");
        rt_.push(screen, 0);
        record("003d1730 USB keyboard 0055a700 (app +278 = 0)");
    });
    app_28_ = 1;
    app_36_ = 0;
}

void FrontendSession::gui_resume(bool nested) {   // 002c9510
    record("00412ed0(00571dc0); 003d1730 USB keyboard 0055a700");
    auto body = [&] {
        if (app_38_ == 1) {
            record("00165950");
        } else if (app_38_ == 0) {
            auto& canvas = rt_.canvas();
            rt_.send(&canvas, &canvas, 0x10, 5, 0);
            g_.level_140 = rt_.g.level_name;
            if (!services_.sound) record("003caa50 gameSound init 001c9bf0 / level load 001c98d0 of FRONTEND (no sound)");
            front_end_sound([](GameSoundManager& s) {
                s.init();
                if (!s.level_load()) throw std::runtime_error("Front end 002c9510: the gameSound level load 001c98d0 failed");
            });
            const auto old = static_cast<std::int32_t>(static_cast<std::uint32_t>(app_30c_) & 0xffU);
            app_30c_ = 0xff;
            music(old);
        }
        record("002c9ee0 / 00145160");
        rt_.app_broadcast(0x2003, 0, 0);
    };
    if (nested) body();
    else in_runtime(body);
    app_37_ = 0;
    app_28_ = 1;
    record("0037a9f0(005381f0)");
}

void FrontendSession::app_suspend(std::uint32_t mode) { fn_2c9860(mode); }

void FrontendSession::app_resume() { gui_resume(true); }

// 003cba90("BOOT_VIDEOMODE"): the disc's SFO_GAME.INI holds "NTSC" (app_'s boot 640x448 from 00165630).
bool FrontendSession::boot_video_pal() { return false; }

void FrontendSession::video_mode_set(std::uint8_t mode) {   // 00165420(001698c0(), mode)
    switch (mode) {
    case 0: app_.width_254 = 640; app_.height_258 = 448; app_.value_260 = 0x1e; break;
    case 2: app_.width_254 = 640; app_.height_258 = 512; app_.value_260 = 0x32; break;
    case 4: app_.width_254 = 640; app_.height_258 = 480; app_.value_260 = 0x1e; break;
    default: break;   // other modes keep the sizes
    }
    app_.flags_250 = mode;
    g_.video_5383d4 = app_.value_260;
    char text[128];
    std::snprintf(text, sizeof text, "00165420 video mode %u: 0037e320 / 0037e780 GS reset %ux%u (the window keeps its size)",
                  static_cast<unsigned>(mode), static_cast<unsigned>(app_.width_254), static_cast<unsigned>(app_.height_258));
    record(text);
}

void FrontendSession::fn_2c9860(std::uint32_t a1) {   // 002c9860: the suspend
    if (app_37_) return;
    record("001698c0; 003d1700 USB keyboard 0055a700 (app +278 = 1)");
    if (a1 == 0) {
        record("byte 0051ef68 = 0");
        if (!services_.sound) record("003caa50 gameSound level unload 001c9710 (no sound)");
        front_end_sound([](GameSoundManager& s) { s.level_unload(); });
        record("004294c0 network (none)");
    }
    record("002c9e20 / 001450f0 / 002c9c80(1)");
    app_37_ = 1;
    app_28_ = 0;
    app_38_ = static_cast<std::uint8_t>(a1);
}

void FrontendSession::fn_2cb160() {   // 002cb160: the GUI teardown
    if (!app_28_) return;
    // 004147b0: the canvas's screens are popped and the canvas freed; the
    // next 002cb8c0 constructs a new one (00414900).
    in_runtime([&] { rt_.canvas_destroy(); });
    record("004139c0 resource release (the GUI files stay decoded on the host)");
    if (!app_36_) {
        record("0037a9f0 x3; 001698c0, 003d1700, 002c9e20, 001450f0, 002c9c80");
        if (!services_.sound) record("003caa50 gameSound level unload 001c9710 (no sound)");
        front_end_sound([](GameSoundManager& s) { s.level_unload(); });
        record("004294c0 x2, 004538d0, 0037e320");
    }
    app_29_ = 0;
    app_28_ = 0;
    rt_.msgbox = nullptr;
    record("002cdef0");
}

void FrontendSession::music(std::int32_t value) {   // 002c9af0
    const auto v = static_cast<std::uint32_t>(value) & 0xffU;
    if (static_cast<std::uint32_t>(app_30c_) == v) return;
    // 001ca210(rec, 5, 0048c148[v], 0, 0) then 00465c50 (entries 3.. are null).
    if (v >= 3) unreachable(0x2c9af0, "music " + std::to_string(v) + " (0048c148 has no name)");
    if (services_.sound) deliver_sound(*services_.sound, services_.sound->request(5, music_names[v], 0, 0), false);
    else record(std::string("002c9af0 music ") + music_names[v] + " (no sound)");
    app_30c_ = static_cast<std::int32_t>(v);
}

void FrontendSession::fn_2cbb30() {   // 002cbb30: the GUI frame
    if (!app_28_) return;
    std::int32_t now = ms_;
    if (now - last_update_ms_ > 100) last_update_ms_ = now - 100;
    const float dt = static_cast<float>(now - last_update_ms_) / 1000.0F;
    in_runtime([&] {
        {
            OMEGA_ZONE("gui.flush+pads");
            rt_.flush();
            rt_.translate_pads(pad_input());
        }
        {
            OMEGA_ZONE("decor.begin_update");
            decor_.bridge_begin_update(rt_);
        }
        {
            OMEGA_ZONE("gui.update");
            rt_.update(dt);
        }
        OMEGA_ZONE("decor.step");
        decor_.bridge_step(rt_, dt);
    });
    last_update_ms_ = now;
    // The frame update (class 005436b0) to gameSound, 003c9420 transport 1,
    // unless the loading screen's thread runs (byte 0051ee90).
    OMEGA_ZONE("sound.frame");
    if (!loading_51ee90_ && services_.sound) services_.sound->frame();
}

std::int32_t FrontendSession::demo_count_1302f0() {
    const bool loaded = !services_.movie_members.empty();
    return attract_demo_count(demo_cache_, loaded, [&](const std::string& name) {
        return std::any_of(services_.movie_members.begin(), services_.movie_members.end(),
                           [&](const std::string& m) { return iequal(m, name); });
    });
}

void FrontendSession::start_movie(GameFlow& g, std::string_view name) {   // 00130520
    if (!g.movies_61) return;
    record("001c94d0 / 001c9540 channel-2 volume 0x400");
    if (g.player_b0) return;
    fn_36b250(1);
    record("0036b3b0(1, 0x700000, 0x800)");
    // 003fe0f0 language 0 (English): 003f7fb0(-1) / 003f7fa0(0); the feed is
    // 00130610's jump table 0049a980 (language 0: ENGLISH), the movie's +22c.
    movie_ = services_.open_movie ? services_.open_movie(std::string(name)) : nullptr;
    if (!movie_) {
        record("00130520 '" + std::string(name) + "': not in ZMOVIES.HOG (no player)");
        return;
    }
    player_ = std::make_unique<MoviePlayer>();   // 003f9380 clears +230..+235
    flags_ = {};
    movie_ended_ = false;
    field_ = 0;
    frame_valid_ = false;
    movie_name_ = std::string(name);
    // 003f8450: 00412600(0x571cf0, the name with its last three characters
    // replaced by "SUB" (003f84b8), +22c) parses the feed (00410c20); the
    // movie thread's buffer setup then starts the object (00412220).
    subtitle_track_.reset();
    subtitle_file_.reset();
    if (services_.movie_member && movie_name_.size() >= 3) {
        const auto sub = services_.movie_member(movie_name_.substr(0, movie_name_.size() - 3) + "SUB");
        if (sub) {
            subtitle_file_ = std::make_unique<movie::SubtitleFile>(movie::parse_subtitles(
                std::string_view(reinterpret_cast<const char*>(sub->data()), sub->size())));
            subtitle_colour_ = movie::subtitle_colour(*subtitle_file_);
            const movie::SubtitleFeed* feed = subtitle_file_->feed(movie::subtitle_feed_for_language(0));
            if (feed) subtitle_track_ = std::make_unique<movie::SubtitleTrack>(feed);
        }
    }
    g.player_b0 = player_.get();
}

std::int32_t FrontendSession::fn_3f8220(MoviePlayer& player, float) {
    // 00411db0 first, on the presented-field counter 0x55c5a8 (zeroed by the
    // buffer setup 003f5964, one per field: field_ here).
    if (subtitle_track_) subtitle_track_->update(static_cast<std::uint32_t>(field_));
    flags_.stop_235 = flags_.stop_235 || player.skip_235 != 0;
    const auto state = movie_ended_ ? movie::MovieDecoderState::finished : movie::MovieDecoderState::running;
    const movie::MoviePlayerStep step = movie::update_movie_player(flags_, state);
    if (step.pumped && movie_ && !movie_ended_) {
        // 003f7060: every picture spans two fields (one 60 Hz frame each here).
        if (field_++ % 2 == 0) {
            if (movie_->next_frame(frame_)) {
                frame_valid_ = true;
                // Port-only (132vt2a2): a picture held by an audio-paced player is not a new one.
                if (!movie_->repeated_picture()) ++serial_;
            } else {
                movie_ended_ = true;
            }
            int rate = 0, channels = 0;
            if (movie_->audio_format(rate, channels)) {
                audio_rate_ = rate;
                audio_channels_ = channels;
            }
            movie_->take_audio(audio_);
        }
    }
    return step.keep_playing ? 1 : 0;
}

void FrontendSession::destroy_player(MoviePlayer&) {
    player_.reset();
    movie_.reset();
    frame_valid_ = false;
    movie_name_.clear();
    subtitle_track_.reset();   // 003f8434: the object's end 00412320
    subtitle_file_.reset();
}

void FrontendSession::movie_end_message(std::uint32_t) {
    record("003caa50 / 001c9540 movie end: channel-2 volume restored");
}

void FrontendSession::agent_332a10(std::int32_t a1, std::int32_t a2, std::int32_t a3) {
    agent_level_progress(AgentRecord(agent_), a1, a2, a3);
}

void FrontendSession::agent_3328d0(std::int32_t a1, std::int32_t a2, std::int32_t a3, std::int32_t t0) {
    agent_level_start(AgentRecord(agent_), a1, a2, a3, t0);
}

// ---------------------------------------------------------------- port cheats (PORT-ONLY)

AgentRefillHost FrontendSession::refill_host() {
    AgentRefillHost host = profile_refill_host(*services_.awards, rank_value);
    if (port_unlock_all_ || port_unlock_weapons_) {
        auto script = host.set_awards_and_bonuses;
        host.set_awards_and_bonuses = [this, script](std::span<std::uint8_t, agent_record_size> agent,
                                                     AgentLoadState& state) {
            script(agent, state);
            if (port_unlock_all_) omega::port_unlock_all(*services_.awards, agent, state, rt_.g.b_bb5);
            else omega::port_unlock_all_weapons(*services_.awards, agent, state);
        };
    }
    return host;
}

void FrontendSession::port_unlock_all() {
    port_unlock_all_ = true;
    omega::port_unlock_all(*services_.awards, AgentRecord(agent_), agent_state_, rt_.g.b_bb5);
    record("PORT-ONLY cheat: unlock all (port_cheats.hpp)");
}

void FrontendSession::port_unlock_all_at_boot() {
    port_unlock_all_ = true;
    record("PORT-ONLY cheat: unlock all from boot (applied by the agent's refill, port_cheats.hpp)");
}

void FrontendSession::port_unlock_all_weapons() {
    port_unlock_weapons_ = true;
    omega::port_unlock_all_weapons(*services_.awards, AgentRecord(agent_), agent_state_);
    record("PORT-ONLY cheat: unlock all weapons in the loadout (port_cheats.hpp)");
}

// 00335d50(agent, 1) from the level load (level_load.cpp's 0x335d50 call): the level statistics +50..+83 zeroed,
// the refill 00335e50(agent, 1, 1), +a4..+c0 zeroed, the unlock words +b08 copied to +b2c (00335d40: 9 words),
// +b70 = 1, +b71 = 0, +b54 = +b50 (the rank before the level) and +d4 = -1.0f (the level's time, unset).
void FrontendSession::start_agent_level() {
    const ProfileAwards& awards = *services_.awards;
    std::fill(agent_.begin() + 0x50, agent_.begin() + 0x84, std::uint8_t{0});
    refill_agent(AgentRecord(agent_), agent_state_, awards.tables, 1, 1, refill_host());
    std::fill(agent_.begin() + 0xa4, agent_.begin() + 0xc4, std::uint8_t{0});
    std::copy(agent_.begin() + 0xb08, agent_.begin() + 0xb08 + 9 * 4, agent_.begin() + 0xb2c);
    agent_[0xb70] = 1;
    agent_[0xb71] = 0;
    std::copy(agent_.begin() + 0xb50, agent_.begin() + 0xb54, agent_.begin() + 0xb54);
    const std::uint32_t minus_one = 0xbf800000u;
    std::memcpy(agent_.data() + 0xd4, &minus_one, 4);
    record("00335d50 agent level start " + g_.level_140);
}

AgentStatistics FrontendSession::agent_statistics() const {
    AgentStatistics s;
    std::memcpy(s.statistics.data(), agent_.data() + 0x8, sizeof s.statistics);
    std::memcpy(s.words_84.data(), agent_.data() + 0x84, sizeof s.words_84);
    std::memcpy(s.words_a4.data(), agent_.data() + 0xa4, sizeof s.words_a4);
    return s;
}

void FrontendSession::sync_play_statistics(const AgentStatistics& play) {
    std::memcpy(agent_.data() + 0x8, play.statistics.data(), sizeof play.statistics);
    auto or_words = [&](std::size_t at, const std::array<std::int32_t, 8>& words) {
        for (std::size_t i = 0; i < words.size(); ++i) {
            std::int32_t v;
            std::memcpy(&v, agent_.data() + at + 4 * i, 4);
            v |= words[i];
            std::memcpy(agent_.data() + at + 4 * i, &v, 4);
        }
    };
    or_words(0x84, play.words_84);
    or_words(0xa4, play.words_a4);
}

void FrontendSession::end_agent_level(std::uint32_t completed, const std::function<float()>& mission_time,
                                      std::span<const std::string> mantex_members) {
    const ProfileAwards& awards = *services_.awards;
    AgentEndLevelInput in;
    in.level = g_.level_140;                 // 004ffc90
    in.completed = completed;
    in.multiplayer = g_.mp_5721c8 != 0;      // 005721c8
    in.solo = awards.registries.solo;        // 00532a10
    in.multi = awards.registries.multi;      // 00532a40
    const AgentRefillHost refill = refill_host();
    AgentEndLevelHost host;
    host.mission_time = mission_time;
    host.refill = [&] { refill_agent(AgentRecord(agent_), agent_state_, awards.tables, 0, 1, refill); };
    std::size_t locked = 0;
    host.parts = [&] {
        const auto rows = locked_award_parts(AgentRecord(agent_), agent_state_, awards.tables, awards.parts);
        locked = rows.size();
        std::vector<std::string> names;
        names.reserve(rows.size());
        for (const auto row : rows) names.push_back(awards.parts.at(row).name);
        patch_appearance_parts(agent_state_.appearance, names, mantex_members);
    };
    omega::end_agent_level(AgentRecord(agent_), agent_state_, awards.tables, in, host);
    std::int32_t b6c = 0;
    std::memcpy(&b6c, agent_.data() + 0xb6c, 4);
    record("00335830 agent end level " + g_.level_140 + " completed " + std::to_string(completed) + " (+b71 " +
           std::to_string(agent_[0xb71]) + ", 00332500 locked parts " + std::to_string(locked) + ", +b6c " +
           std::to_string(b6c) + ", +b68 " + std::to_string(agent_[0xb68]) + ")");
}

AgentRefillHost FrontendSession::refill_host() const {
    const ProfileAwards& awards = *services_.awards;
    AgentRefillHost host = profile_refill_host(awards, rank_value);
    if (port_agent_patch_) {
        host.set_awards_and_bonuses = [script = host.set_awards_and_bonuses, patch = port_agent_patch_, &awards](
                                          std::span<std::uint8_t, agent_record_size> agent, AgentLoadState& state) {
            script(agent, state);
            patch(agent, state, awards);
        };
    }
    return host;
}

void FrontendSession::port_refill_agent() {
    const ProfileAwards& awards = *services_.awards;
    refill_agent(AgentRecord(agent_), agent_state_, awards.tables, 0, 1, refill_host());
    record("port-only: 00335e50 agent refill (0, 1)" + std::string(port_agent_patch_ ? " with the port agent patch" : ""));
}

void FrontendSession::resume_after_level(std::uint8_t state_50, std::uint8_t previous_51) {
    if (g_.state_50 != 0xe) throw std::runtime_error("Front end: a level-end resume without the level load hand-off (state 0xe)");
    if (state_50 != 0x0c && state_50 != 1)
        throw std::runtime_error("Front end: a level-end resume to state " + std::to_string(state_50) + " (001690f0 leaves 0x0c or 1)");
    g_.state_50 = state_50;
    g_.previous_51 = previous_51;
}

// ---------------------------------------------------------------- GuiHost

std::optional<std::string> FrontendSession::get_string(const std::string& key) { return services_.strings(key); }

std::uint32_t FrontendSession::font(const std::string& name) { return services_.font(name); }

std::uint8_t FrontendSession::font_height(std::uint32_t handle) { return services_.font_height(handle); }
std::int32_t FrontendSession::font_width(std::uint32_t handle, const std::string& s, std::int32_t n) {
    if (!services_.font_width) return gui::GuiHost::font_width(handle, s, n);
    return services_.font_width(handle, s, n);
}

void FrontendSession::card_refresh() {   // 0040d3c0 over the card folder
    auto& c = rt_.g.card;
    const AgentCardState s = card_.state(rt_.charmap.ready != 0, rt_.charmap.count);
    c.present = s.card_present ? 1 : 0;
    c.check_space = s.card_formatted ? 1 : 0;
    c.free = static_cast<std::int32_t>(std::min<std::uint64_t>(card_.free_bytes() / 1024, 0x7fffffff));
    c.slot = static_cast<std::int32_t>(card_.card_id());
    c.directory = card_.save_directory_present() ? 1 : 0;
    c.dir = card_.list();
}

std::uint8_t FrontendSession::save_present() {   // 00288510
    const auto& c = rt_.g.card;
    return card_save_present(c.present != 0, c.check_space != 0, card_.save_directory_present(), c.dir,
                             rt_.charmap.ready != 0, rt_.charmap.count);
}

std::int32_t FrontendSession::card_space_needed(std::int32_t size) {
    return omega::card_space_needed(size, card_.save_directory_present());
}

void FrontendSession::check_profile(std::int32_t profile) const {
    if (profile != profile_handle) throw std::runtime_error("Front end: profile " + std::to_string(profile) + " is not the agent");
}

void FrontendSession::profile_select(std::uint32_t profile) {   // 003361b0
    check_profile(static_cast<std::int32_t>(profile));
    record("003361b0 profile select");
}

void FrontendSession::app_2c9af0(std::int32_t value) { music(value); }

// 002c9aa0: 003caa50(004ffb50, gameSound) (no FRONTEND swap), 001c7d80 on it when found, then app +30c = -1 (the
// next 002c9af0 starts its music again).
void FrontendSession::app_2c9aa0() {
    if (services_.sound) services_.sound->stop_music_streams();
    else record("002c9aa0 music stop (no sound)");
    app_30c_ = -1;
}

void FrontendSession::app_131470(std::int32_t value) {
    record("00131470(" + std::to_string(value) + ") no-controller pause");
}

// 002c82f0(app): the "please wait" thread around a blocking call (the navigator 0033c320's factory, the command
// centre's agent load). With app +310 clear it raises the main thread's priority value to 10 (ChangeThreadPriority),
// creates the thread 002c84b0 (priority 5, stack *0048c340 + 0x10 from 0036b180) and starts it with the app
// (StartThread), then installs the VBLANK handler 002c8460 (AddIntcHandler(2) / EnableIntc(2)) and sets +310.
// The thread outranks the main thread, so StartThread runs its first pass at once: with at least 10 ms on the
// EE clock (00533878, 10 us units, / 100) since its last frame (none yet: 0) and the allocator's lock count
// 00533888 back at 0, it suspends the main thread, copies the scratchpad out, sets 00587b38 (the GUI sound gate)
// and 0051ee90, runs 002cc260(0, 0x3039) (one nested GUI frame: 0x3039 passes the +310 test), polls the lobby
// 004536b0 when a DME session exists, clears both bytes, restores the scratchpad, resumes the main thread and
// sleeps. Later passes need the VBLANK handler's wake-up (skipped while 0051ee90 is set) while the main thread is
// still between 002c82f0 and 002c81d0: the port's factories read the disc inside the same frame, so no frame
// time passes there and only the first pass runs. Static inference from the disassembly (no interpreter model of
// the EE kernel's threads); research/FRONTEND.md "Checkpoint 132ln3".
void FrontendSession::app_2c82f0() {
    if (app_310_) return;
    record("002c82f0 the wait thread 002c84b0: its first pass (002cc260(0, 0x3039))");
    rt_.g.sound_gate = 1;   // byte 00587b38
    loading_51ee90_ = 1;    // byte 0051ee90
    auto& guard = rt_.g.debug_triangle;   // 002cc260: byte 0051eeb0
    if (!guard) {
        guard = 1;
        nested_wait_thread_ = 1;
        nested_service();
        nested_wait_thread_ = 0;
        ++wait_frames_;
        guard = 0;
    }
    // bytes 00572528 / 005723c8, then 004294c0: the lobby's update 004536b0 runs only with a DME session
    if (net_session()) {
        if (!services_.lobby) unreachable(0x2c86f0, "the wait thread's lobby update 004536b0");
        services_.lobby->update();
        record("004536b0 lobby update (the wait thread)");
    }
    rt_.g.sound_gate = 0;
    loading_51ee90_ = 0;
    app_310_ = 1;
}
// 002c81d0(app): with +310 set, DisableIntc(2) / RemoveIntcHandler(2), TerminateThread / DeleteThread of the
// wait thread, its stack freed (0036b130), 0051ef70 = 0 and +310 = 0. Nothing it does is drawn.
void FrontendSession::app_2c81d0() {
    if (!app_310_) return;
    record("002c81d0 the wait thread ends");
    app_310_ = 0;
}
// 0040d5d0(0055a830): loops on 002b5f48 until the card thread has no command pending. The PC card (the
// FolderAgentCard) runs every command to completion inside its call, so none is pending here.
void FrontendSession::card_sync() { record("0040d5d0 card sync (no card command pending)"); }

// 0012ee30(004ffb50): for player slots 0..3 other than the local one (0 unless byte 005721c8, a network game,
// is set), 001314f0(G, i) and, for a profile, +b70 = 0. The front end has one agent, in slot 0 (static inference:
// the slots 1..3 are filled only by a network game's players), so no byte changes.
void FrontendSession::profiles_clear_b70() {
    if (rt_.g.b_21c8) unreachable(0x12ee5c, "0012ee30 in a network game (the local slot 00494178)");
    record("0012ee30 other players' +b70: no other profile");
}

void FrontendSession::level_progress(std::int32_t profile, std::int32_t level, std::int32_t a, std::int32_t b) {
    check_profile(profile);
    agent_332a10(level, a, b);
    char text[64];
    std::snprintf(text, sizeof text, "00332a10 level state %d %d %d", level, a, b);
    record(text);
}

bool FrontendSession::lan_hosting() { return services_.lan_hosting ? services_.lan_hosting() : false; }

bool FrontendSession::lan_host(const LanHostRequest& request) {
    if (!services_.lan_host) unreachable(0x2a1044, "the PORT-ONLY LAN host without a LAN session");
    const bool ok = services_.lan_host(request);
    char text[160];
    std::snprintf(text, sizeof text, "PORT-ONLY LAN host '%s' mission %d agents %d filter %d: %s",
                  request.host_name.c_str(), request.mission, request.num_agents, request.join_filter,
                  ok ? "created" : "failed");
    record(text);
    return ok;
}

// Port-only (checkpoint 132ln4): GuiNetJoinMission's LAN services, forwarded to the LAN session.
void FrontendSession::lan_browse(bool on) {
    if (!services_.lan_browse) return;
    services_.lan_browse(on);
    record(on ? "PORT-ONLY LAN browse started" : "PORT-ONLY LAN browse stopped");
}
std::vector<gui::GuiHost::LanGame> FrontendSession::lan_games() {
    return services_.lan_games ? services_.lan_games() : std::vector<LanGame>{};
}
bool FrontendSession::lan_join(std::uint64_t session) {
    if (!services_.lan_join) unreachable(0x33eafc, "the PORT-ONLY LAN join without a LAN session");
    const bool ok = services_.lan_join(session);
    char text[96];
    std::snprintf(text, sizeof text, "PORT-ONLY LAN join %016llx: %s", static_cast<unsigned long long>(session),
                  ok ? "JOIN_REQ sent" : "not sent");
    record(text);
    return ok;
}
gui::GuiHost::LanJoin FrontendSession::lan_join_state() {
    return services_.lan_join_state ? services_.lan_join_state() : LanJoin::idle;
}
void FrontendSession::lan_join_cancel() {
    if (services_.lan_join_cancel) services_.lan_join_cancel();
}
std::optional<std::string> FrontendSession::lan_join_notice() {
    return services_.lan_join_notice ? services_.lan_join_notice() : std::nullopt;
}
void FrontendSession::lan_pump() {
    if (services_.lan_pump) services_.lan_pump();
}
std::int64_t FrontendSession::lan_clock_ms() { return services_.lan_clock_ms ? services_.lan_clock_ms() : ms_; }

// Port-only (checkpoint 132on2, research/ONLINE.md §9): with FrontendServices::lobby set, the DME session and the
// SFOLobby calls go to the port's online lobby (omega_server); without it, the GuiHost defaults (no session).
bool FrontendSession::net_session() { return services_.lobby ? services_.lobby->session() : GuiHost::net_session(); }
std::int32_t FrontendSession::net_start() {
    if (!services_.lobby) return GuiHost::net_start();
    const std::int32_t r = services_.lobby->start();
    record(r ? "002ca710 online start (port lobby: omega_server)" : "002ca710 online start failed (port lobby)");
    return r;
}
std::int32_t FrontendSession::lobby_configs(std::vector<LobbyConfig>& configs, std::vector<LobbyServer>& servers) {
    return services_.lobby ? services_.lobby->configs(configs, servers) : GuiHost::lobby_configs(configs, servers);
}
void FrontendSession::lobby_disconnect() {
    if (services_.lobby) services_.lobby->disconnect();
    else GuiHost::lobby_disconnect();
}
std::int32_t FrontendSession::lobby_error() { return services_.lobby ? services_.lobby->error() : GuiHost::lobby_error(); }
std::int32_t FrontendSession::lobby_connect(std::int32_t row) {
    if (!services_.lobby) return GuiHost::lobby_connect(row);
    const std::int32_t r = services_.lobby->connect(row);
    record("0044bab0 connect row " + std::to_string(row) + " -> " + std::to_string(r) + " (port: DNAS bypassed)");
    return r;
}
std::int32_t FrontendSession::lobby_dnas_result() {
    return services_.lobby ? services_.lobby->dnas_result() : GuiHost::lobby_dnas_result();
}
std::int32_t FrontendSession::lobby_universes_begin() {
    return services_.lobby ? services_.lobby->universes_begin() : GuiHost::lobby_universes_begin();
}
std::int32_t FrontendSession::lobby_muis_connect(std::int32_t server, std::vector<LobbyUniverse>& universes) {
    return services_.lobby ? services_.lobby->muis_connect(server, universes) : GuiHost::lobby_muis_connect(server, universes);
}
std::int32_t FrontendSession::lobby_universe_select(std::int32_t index) {
    return services_.lobby ? services_.lobby->universe_select(index) : GuiHost::lobby_universe_select(index);
}
std::int32_t FrontendSession::lobby_universe_default() {
    return services_.lobby ? services_.lobby->universe_default() : GuiHost::lobby_universe_default();
}
void FrontendSession::lobby_universes_end() {
    if (services_.lobby) services_.lobby->universes_end();
    else GuiHost::lobby_universes_end();
}
std::string FrontendSession::setting(const std::string& key) {
    return services_.lobby ? services_.lobby->setting(key) : GuiHost::setting(key);
}
std::int32_t FrontendSession::lobby_policy_begin() {
    return services_.lobby ? services_.lobby->policy_begin() : GuiHost::lobby_policy_begin();
}
std::int32_t FrontendSession::lobby_announcements(std::vector<LobbyAnnouncement>& out) {
    if (!services_.lobby) return GuiHost::lobby_announcements(out);
    const std::int32_t r = services_.lobby->announcements(out);
    record("00445570 announcements: " + std::to_string(out.size()) + " row(s)");
    return r;
}
std::int32_t FrontendSession::lobby_policy(bool privacy, std::vector<std::string>& out) {
    if (!services_.lobby) return GuiHost::lobby_policy(privacy, out);
    const std::int32_t r = services_.lobby->policy(privacy, out);
    record(std::string(privacy ? "004456c0 privacy" : "004456d0 usage") + " policy: " + std::to_string(out.size()) +
           " chunk(s) -> " + std::to_string(r));
    return r;
}
void FrontendSession::lobby_policy_end() {
    if (services_.lobby) services_.lobby->policy_end();
    else GuiHost::lobby_policy_end();
}
void FrontendSession::lobby_leave(std::int32_t a1) {
    if (!services_.lobby) return GuiHost::lobby_leave(a1);
    lobby_messages();  // 002ca640's 002c8090 (before the logout 00445850 here: the server sees no difference)
    services_.lobby->leave(a1);
    record("002ca640 online leave (port lobby)");
}
std::optional<std::string> FrontendSession::lobby_notice() {
    return services_.lobby ? services_.lobby->notice() : GuiHost::lobby_notice();
}
bool FrontendSession::lobby_unread() { return services_.lobby ? services_.lobby->unread() : GuiHost::lobby_unread(); }
bool FrontendSession::lobby_region_choice() {
    return services_.lobby ? services_.lobby->region_choice() : GuiHost::lobby_region_choice();
}
std::int32_t FrontendSession::lobby_universe_list(std::vector<LobbyUniverse>& universes) {
    if (!services_.lobby) return GuiHost::lobby_universe_list(universes);
    const std::int32_t r = services_.lobby->universe_list(universes);
    record("00442d80 universe list: " + std::to_string(universes.size()) + " row(s) -> " + std::to_string(r));
    return r;
}
std::int32_t FrontendSession::lobby_message_count() {
    return services_.lobby ? services_.lobby->message_count() : GuiHost::lobby_message_count();
}
std::optional<gui::GuiHost::LobbyMessage> FrontendSession::lobby_message(std::int32_t index) {
    return services_.lobby ? services_.lobby->message(index) : GuiHost::lobby_message(index);
}
void FrontendSession::lobby_message_answer(std::int32_t index, std::int32_t accept) {
    if (!services_.lobby) return GuiHost::lobby_message_answer(index, accept);
    services_.lobby->message_answer(index, accept);
    record("00450f30 message " + std::to_string(index) + " answered " + std::to_string(accept));
}
bool FrontendSession::lobby_messages_changed() {
    return services_.lobby ? services_.lobby->messages_changed() : GuiHost::lobby_messages_changed();
}
void FrontendSession::lobby_messages_set_changed(bool changed) {
    if (services_.lobby) services_.lobby->messages_set_changed(changed);
    else GuiHost::lobby_messages_set_changed(changed);
}
std::optional<gui::GuiHost::UsbKeyReport> FrontendSession::usbkb_poll() {
    if (usb_keys_.empty()) return std::nullopt;
    const UsbKeyReport k = usb_keys_.front();
    usb_keys_.pop_front();
    return k;
}
std::int32_t FrontendSession::lobby_login(const std::string& name, const std::string& password) {
    if (!services_.lobby) return GuiHost::lobby_login(name, password);
    const std::int32_t r = services_.lobby->login(name, password);
    record("00445a40 login " + name + " -> " + std::to_string(r) + (r ? "" : " error " + std::to_string(lobby_error())));
    return r;
}
std::int32_t FrontendSession::lobby_register(const std::string& name, const std::string& password) {
    if (!services_.lobby) return GuiHost::lobby_register(name, password);
    const std::int32_t r = services_.lobby->register_account(name, password);
    record("00445b90 register " + name + " -> " + std::to_string(r) + (r ? "" : " error " + std::to_string(lobby_error())));
    return r;
}
std::int32_t FrontendSession::lobby_check_account(const std::string& name, const std::string& password) {
    if (!services_.lobby) return GuiHost::lobby_check_account(name, password);
    const std::int32_t r = services_.lobby->check_account(name, password);
    record("004459d0 account check " + name + " -> " + std::to_string(r));
    return r;
}
std::int32_t FrontendSession::lobby_enter() {
    if (!services_.lobby) return GuiHost::lobby_enter();
    const std::int32_t r = services_.lobby->enter();
    record("00442750 lobby channel join -> " + std::to_string(r));
    return r;
}
std::int32_t FrontendSession::lobby_state() {
    return services_.lobby ? services_.lobby->state() : GuiHost::lobby_state();
}
std::int32_t FrontendSession::lobby_mission(std::int32_t mission) {
    if (!services_.lobby) return GuiHost::lobby_mission(mission);
    const std::int32_t r = services_.lobby->mission(mission);
    record("00442510 lobby mission " + std::to_string(mission) + " -> " + std::to_string(r));
    return r;
}
// 002c8090(app): the lobby's records (+1c) to the current agent. Any record, or a nonzero *0052fe80 (the agent's
// message count), marks the agent for saving (+b73); the count is cleared (the records are the agent's online
// messages, dispatched again below by 00336ec0); more than 0x40 records answers 0.
std::int32_t FrontendSession::lobby_messages() {
    if (!services_.lobby) return GuiHost::lobby_messages();
    if (!services_.lobby->session()) return 0;
    const std::int32_t p = current_profile();
    if (!p) return 0;
    const std::int32_t n = services_.lobby->records();
    if (n != 0 || agent_state_.message_count != 0) profile_set_byte(p, 0xb73, 1);
    agent_state_.message_count = 0;
    if (n > 0x40) return 0;
    if (n > 0) unreachable(0x2c8140, "the lobby records' dispatch (0044f290, 00336ec0)");
    return 1;
}
// Checkpoint 132on2b stages B3 / B4: GuiMissionSetup / GuiNetJoinMission online (research/ONLINE.md §9.4 / §9.5).
void FrontendSession::lobby_channel_leave() {
    if (!services_.lobby) return GuiHost::lobby_channel_leave();
    services_.lobby->channel_leave();
    record("004426b0 lobby channel left");
}
std::int32_t FrontendSession::lobby_room_open() {
    if (!services_.lobby) return GuiHost::lobby_room_open();
    const std::int32_t r = services_.lobby->room_open();
    record("004518f0 game room -> " + std::to_string(r));
    return r;
}
void FrontendSession::lobby_room_leave() {
    if (!services_.lobby) return GuiHost::lobby_room_leave();
    services_.lobby->room_leave();
    record("00451610 game room left");
}
void FrontendSession::lobby_list_kind(std::int32_t kind) {
    if (!services_.lobby) return GuiHost::lobby_list_kind(kind);
    services_.lobby->list_kind(kind);
    record("004515f0 room list " + std::to_string(kind));
}
// 001698c0 / 00165bc0 load and start the PS2 network modules (the IOP's); PORT-ONLY: the PC has none to start.
void FrontendSession::lobby_net_start() {
    if (!services_.lobby) return GuiHost::lobby_net_start();
    record("001698c0 / 00165bc0 network start (port: no IOP modules)");
}
std::int32_t FrontendSession::lobby_create(std::int32_t mission, std::int32_t agents, std::int32_t filter) {
    if (!services_.lobby) return GuiHost::lobby_create(mission, agents, filter);
    const std::int32_t r = services_.lobby->create(mission, agents, filter);
    record("0044e960 create game: mission " + std::to_string(mission) + ", agents " + std::to_string(agents) +
           ", filter " + std::to_string(filter) + " -> " + std::to_string(r) +
           (r ? "" : " error " + std::to_string(lobby_error())));
    return r;
}
std::optional<gui::GuiHost::LobbyCallback> FrontendSession::lobby_pump() {
    if (!services_.lobby) return GuiHost::lobby_pump();
    const auto cb = services_.lobby->pump();
    if (cb) record("004536b0 lobby callback: a3 " + std::to_string(cb->a3) + ", t0 " + std::to_string(cb->t0));
    return cb;
}
void FrontendSession::lobby_game_cleanup() {
    if (!services_.lobby) return GuiHost::lobby_game_cleanup();
    services_.lobby->game_cleanup();
    record("0042b1f0 game left");
}
std::optional<gui::GuiHost::LobbyGameInfo> FrontendSession::lobby_game_info(std::int32_t world) {
    if (!services_.lobby) return GuiHost::lobby_game_info(world);
    const auto info = services_.lobby->game_info(world);
    record("0044e060 game info " + std::to_string(world) +
           (info ? ": max " + std::to_string(info->max_players) + ", owner " + std::to_string(info->owner) +
                       ", mission " + std::to_string(info->mission)
                 : std::string(": none")));
    return info;
}
std::optional<gui::GuiHost::LobbyCachedGame> FrontendSession::lobby_game_cached(std::int32_t world) {
    return services_.lobby ? services_.lobby->game_cached(world) : GuiHost::lobby_game_cached(world);
}
std::vector<gui::GuiHost::LobbyCachedGame> FrontendSession::lobby_game_cache_take() {
    if (!services_.lobby) return GuiHost::lobby_game_cache_take();
    auto updates = services_.lobby->game_cache_take();
    for (const auto& e : updates)
        record("0044edc0 game cache " + std::to_string(e.world) + ": players " + std::to_string(e.players) + "/" +
               std::to_string(e.max_players) + ", mission " + std::to_string(e.stats[1]) + ", status " +
               std::to_string(e.world_status));
    return updates;
}
std::int32_t FrontendSession::lobby_join(std::int32_t world) {
    if (!services_.lobby) return GuiHost::lobby_join(world);
    const std::int32_t r = services_.lobby->join(world);
    record("0044e430 join game " + std::to_string(world) + " -> " + std::to_string(r) +
           (r ? "" : " error " + std::to_string(lobby_error())));
    return r;
}
void FrontendSession::lobby_join_end() {
    if (!services_.lobby) return GuiHost::lobby_join_end();
    services_.lobby->join_end();
    record("0044e340 join abandoned");
}
void FrontendSession::lobby_set_state(std::int32_t state) {
    if (!services_.lobby) return GuiHost::lobby_set_state(state);
    services_.lobby->set_state(state);
}
// 00429d30: the DME update. PORT-ONLY: the lobby's pump (004536b0's stand-in) serves the transport and the LAN game
// the join connects to, so there is nothing more to update here.
void FrontendSession::net_update() {
    if (!services_.lobby) return GuiHost::net_update();
}
// 003698b0: the PS2 clock in ms; the PC's steady clock.
std::int64_t FrontendSession::lobby_clock_ms() {
    if (!services_.lobby) return GuiHost::lobby_clock_ms();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
std::int32_t FrontendSession::lobby_own_id() {
    return services_.lobby ? services_.lobby->own_id() : GuiHost::lobby_own_id();
}
gui::GuiHost::LobbyGameList FrontendSession::lobby_game_list() {
    return services_.lobby ? services_.lobby->game_list() : GuiHost::lobby_game_list();
}
std::int32_t FrontendSession::lobby_chat_send(const std::string& text, std::int32_t target) {
    return services_.lobby ? services_.lobby->chat_send(text, target) : GuiHost::lobby_chat_send(text, target);
}
std::vector<gui::GuiHost::LobbyChatLine> FrontendSession::lobby_chat_take() {
    return services_.lobby ? services_.lobby->chat_take() : GuiHost::lobby_chat_take();
}
gui::GuiHost::LobbyPlayerList FrontendSession::lobby_player_list() {
    return services_.lobby ? services_.lobby->player_list() : GuiHost::lobby_player_list();
}
std::int32_t FrontendSession::lobby_contacts_open() {
    return services_.lobby ? services_.lobby->contacts_open() : GuiHost::lobby_contacts_open();
}
void FrontendSession::lobby_contacts_close() {
    if (!services_.lobby) return GuiHost::lobby_contacts_close();
    services_.lobby->contacts_close();
}
gui::GuiHost::LobbyContactList FrontendSession::lobby_contact_list() {
    return services_.lobby ? services_.lobby->contact_list() : GuiHost::lobby_contact_list();
}
std::int32_t FrontendSession::lobby_contact_remove(std::int32_t id) {
    return services_.lobby ? services_.lobby->contact_remove(id) : GuiHost::lobby_contact_remove(id);
}
std::int32_t FrontendSession::lobby_account_id(const std::string& name) {
    return services_.lobby ? services_.lobby->account_id(name) : GuiHost::lobby_account_id(name);
}
std::int32_t FrontendSession::lobby_contact_add(std::int32_t id) {
    return services_.lobby ? services_.lobby->contact_add(id) : GuiHost::lobby_contact_add(id);
}
void FrontendSession::lobby_agent_edited() {
    if (services_.lobby) services_.lobby->agent_edited();
}
bool FrontendSession::lobby_agent_stats(std::int32_t id, std::array<std::uint8_t, 0xfc>& out) {
    return services_.lobby ? services_.lobby->agent_stats(id, out) : GuiHost::lobby_agent_stats(id, out);
}
bool FrontendSession::lobby_cell_leader() {
    return services_.lobby ? services_.lobby->cell_leader() : GuiHost::lobby_cell_leader();
}
bool FrontendSession::lobby_cell_member() {
    return services_.lobby ? services_.lobby->cell_member() : GuiHost::lobby_cell_member();
}
std::string FrontendSession::lobby_cell_name() {
    return services_.lobby ? services_.lobby->cell_name() : GuiHost::lobby_cell_name();
}
std::optional<std::string> FrontendSession::lobby_cell_name_of(std::int32_t clan) {
    return services_.lobby ? services_.lobby->cell_name_of(clan) : GuiHost::lobby_cell_name_of(clan);
}
bool FrontendSession::lobby_cell_has(std::int32_t id) {
    return services_.lobby ? services_.lobby->cell_has(id) : GuiHost::lobby_cell_has(id);
}
bool FrontendSession::lobby_is_contact(std::int32_t id) {
    return services_.lobby ? services_.lobby->is_contact(id) : GuiHost::lobby_is_contact(id);
}
std::int32_t FrontendSession::lobby_cell_invite(std::int32_t id) {
    return services_.lobby ? services_.lobby->cell_invite(id) : GuiHost::lobby_cell_invite(id);
}
std::int32_t FrontendSession::lobby_cell_create(const std::string& name) {
    return services_.lobby ? services_.lobby->cell_create(name) : GuiHost::lobby_cell_create(name);
}

std::optional<std::int32_t> FrontendSession::lobby_player_rank(std::int32_t account) {
    return services_.lobby ? services_.lobby->player_rank(account) : GuiHost::lobby_player_rank(account);
}
bool FrontendSession::lobby_join_password_refused() {
    return services_.lobby ? services_.lobby->join_password_refused() : GuiHost::lobby_join_password_refused();
}
void FrontendSession::lobby_set_join_password(const std::string& password) {
    if (!services_.lobby) return GuiHost::lobby_set_join_password(password);
    services_.lobby->set_join_password(password);
}

std::int32_t FrontendSession::popup_open(std::int32_t, const std::optional<std::string>&,
                                         const std::optional<std::string>&, const gui::Widget*, std::int32_t) {
    unreachable(0x2cae30, "a host popup (the runtime's queue is native)");
}
void FrontendSession::popup_close(std::int32_t) { unreachable(0x2cac80, "a host popup close"); }
std::int32_t FrontendSession::popup_query(std::int32_t) { unreachable(0x2caba0, "a host popup query"); }

std::uint8_t FrontendSession::sound(std::uint32_t table, const gui::Widget*, bool alternate) {
    const char* name = gui_sound_name(table);
    char text[16];
    std::snprintf(text, sizeof text, "%08x", table);
    if (!name) unreachable(alternate ? 0x465b50 : 0x465c50, std::string("the sound table ") + text);
    if (services_.sound) gui_sound_deliver(*services_.sound, name, alternate);
    else record(std::string(alternate ? "00465b50 stop " : "00465c50 play ") + name + " (no sound)");
    return 1;
}

// Checkpoint 132cp3: the title screen's GAMEOPT (GuiGameOptions over GuiTitle, not in game).
void FrontendSession::vibration_log(std::uint32_t address) {
    const auto& w = vibration_.last[0];
    if (w.bytes == vibration_logged_) return;
    vibration_logged_ = w.bytes;
    char text[160];
    std::snprintf(text, sizeof text, "frame %lld %08x: pad vibration small motor %s, large motor %d (00369920)",
                  static_cast<long long>(frames_), address, w.small_on() ? "on" : "off", int(w.large()));
    record(text);
}
void FrontendSession::rumble_stop() {   // 0026bd60 (0029b084)
    stop_pad_vibration(vibration_);
    record("frame " + std::to_string(frames_) + " 0026bd60 pad vibration stop");
    vibration_log(0x26bd60);
}
void FrontendSession::rumble_start(std::int32_t port, std::int32_t small, std::int32_t large, std::int32_t period,
                                   float duration) {   // 0026bde0 (0029a360)
    const std::int32_t serial =
        start_pad_vibration(vibration_, port, static_cast<std::uint8_t>(small), large, period, duration);   // sb a1
    char text[160];
    std::snprintf(text, sizeof text, "frame %lld 0026bde0(%d, %d, %d, %d, %g): serial %d",
                  static_cast<long long>(frames_), port, small, large, period, double(duration), serial);
    record(text);
}
void FrontendSession::rumble_update(float dt) {   // 0026bc20 (0029b920, dt / 30)
    update_pad_vibration(vibration_, dt);
    vibration_log(0x26bc20);
}
void FrontendSession::rumble_enable(std::uint8_t enabled) {   // 0026be70 (0045a3a0)
    enable_pad_vibration(vibration_, enabled);
    record("frame " + std::to_string(frames_) + " 0026be70(" + std::to_string(int(enabled)) + ")");
}
void FrontendSession::sound_volume(std::int32_t channel, std::int32_t volume) {   // 001c9540
    services_.sound->set_volume(static_cast<std::uint8_t>(channel), volume);
    record("001c9540(" + std::to_string(channel) + ", " + std::to_string(volume) + ")");
}
void FrontendSession::sound_mode(std::int32_t mode) {   // 001c9670
    services_.sound->set_mode(mode);
    record("001c9670(" + std::to_string(mode) + ")");
}
void FrontendSession::options_sound(std::int32_t kind, std::int32_t type, const std::string& name) {
    if (services_.sound) gui_sound_send(*services_.sound, kind, type, name);
    else {
        static constexpr const char* kinds[3] = {"0020ecb0 stop ", "0020faa0 play ", "0020f2a0 quip "};
        record(std::string(kind >= 0 && kind < 3 ? kinds[kind] : "? ") + std::to_string(type) + " '" + name +
               "' (no sound)");
    }
}
gui::Widget* FrontendSession::factory(const std::string& cls, const std::string&) {
    unreachable(0x418a80, "a host factory of " + cls + " (the manager is native)");
}
void FrontendSession::release(gui::Widget*) { unreachable(0x418840, "a host release (the manager is native)"); }

std::optional<std::int32_t> FrontendSession::resource(const std::string& name) {   // 004133b0
    for (std::size_t i = 0; i < services_.gui_resources.size(); ++i)
        if (iequal(services_.gui_resources[i], name + ".HOG")) return static_cast<std::int32_t>(i);
    // Port-only (132win11): the Port Settings screen, one past the disc's resources (open_gui).
    if (services_.port_rows.settings && name == "PORTSETTINGS") return static_cast<std::int32_t>(services_.gui_resources.size());
    return std::nullopt;
}

const GuiNode* FrontendSession::open_gui(const std::string& file, std::int32_t res) {   // 0036e1b0 + 00419020
    if (res >= 0 && static_cast<std::size_t>(res) == services_.gui_resources.size() && services_.port_rows.settings) {
        // Port-only (132win11): PORTSETTINGS, GAMEOPT2's widgets rearranged (port_settings_screen), made once.
        const std::string key = "PORTSETTINGS/" + file;
        auto it = gui_files_.find(key);
        if (it == gui_files_.end()) {
            const auto bytes = services_.gui_file("GAMEOPT2");
            if (bytes.size() < 3 || bytes[0] != 'G' || bytes[1] != 'U' || bytes[2] != 'I') return nullptr;
            auto f = std::make_unique<GuiFile>(decode_gui(bytes));
            f->root = omega::port_settings_screen(f->root, gui::PortSettingsScreen::rows);
            it = gui_files_.emplace(key, std::move(f)).first;
        }
        return &it->second->root;
    }
    if (res < 0 || static_cast<std::size_t>(res) >= services_.gui_resources.size()) return nullptr;
    std::string screen = services_.gui_resources[static_cast<std::size_t>(res)];
    screen = screen.substr(0, screen.size() - 4);
    const std::string key = screen + "/" + file;
    auto it = gui_files_.find(key);
    if (it == gui_files_.end()) {
        const auto bytes = services_.gui_file(screen);
        if (bytes.size() < 3 || bytes[0] != 'G' || bytes[1] != 'U' || bytes[2] != 'I') return nullptr;
        it = gui_files_.emplace(key, std::make_unique<GuiFile>(decode_gui(bytes))).first;
        if (screen == "GAMEOPT" && port_cheats()) add_port_cheat_entries(it->second->root);
        if (services_.port_rows.patch) services_.port_rows.patch(screen, *it->second);  // port-only (132ob6)
    }
    return &it->second->root;
}

// PORT-ONLY (not in the original): the options screen's cheat entries. Two
// copies of its own tab button "2video" (GuiButtonWidget, ie game, with its
// textbackground child) are added to optionmain_grp below it, 40 units apart
// as the tabs are (8 units right for the longer labels), with the arrow-less "bracketbutton" node of the value
// buttons (value_manualaim's scale) and literal labels. They are not tabs
// (id10 stays -1); GuiRuntime::go_event opens their Yes/No popup.
void FrontendSession::add_port_cheat_entries(GuiNode& root) {
    std::function<GuiNode*(GuiNode&, const std::string&)> find = [&](GuiNode& n, const std::string& name) -> GuiNode* {
        if (n.name == name) return &n;
        for (auto& c : n.children)
            if (GuiNode* f = find(c, name)) return f;
        return nullptr;
    };
    GuiNode* grp = find(root, "optionmain_grp");
    GuiNode* video = grp ? find(*grp, "2video") : nullptr;
    if (!video || !video->text || !video->decorator)
        throw std::runtime_error("frontend: GAMEOPT has no optionmain_grp / 2video to copy the PORT-ONLY cheat entries from");
    const GuiNode source = *video;
    std::function<void(GuiNode&, float, float, const std::string&)> shift = [&](GuiNode& n, float dx, float dy,
                                                                         const std::string& tag) {
        n.rect[0] += dx;
        n.rect[1] += dy;
        if (n.decorator) {
            n.decorator->matrix[9] += dx;
            n.decorator->matrix[11] += dy;
        }
        n.name += tag;
        for (auto& c : n.children) shift(c, dx, dy, tag);
    };
    struct Entry {
        const char* name;
        const char* label;
        float dy;
        float bracket_x;   // the bracket's x scale: value_manualaim's 0.9235, widened for the longer label
    };
    static constexpr Entry entries[] = {{gui::port_cheat_all_widget, "CHEAT: ALL", -40.0f, 0.9235f},
                                        {gui::port_cheat_weapons_widget, "CHEAT: WEAPONS", -80.0f, 1.25f}};
    for (const Entry& e : entries) {
        GuiNode n = source;
        shift(n, 8.0f, e.dy, "_port");   // 8 units right: "CHEAT: WEAPONS" is wider than the tabs' column
        n.name = e.name;
        n.text->text = e.label;
        n.decorator->node = "bracketbutton";
        n.decorator->matrix[0] = e.bracket_x;   // value_manualaim's bracketbutton scale (0.9235, 0.8915)
        n.decorator->matrix[5] = 0.8915f;
        grp->children.push_back(std::move(n));
    }
    record("PORT-ONLY: GAMEOPT cheat entries added (port_cheats.hpp)");
}

std::uint8_t FrontendSession::agent_load(std::int32_t agent, const std::string& name, std::int32_t slot) {
    check_profile(agent);   // 00334790
    std::vector<std::uint8_t> bytes(name.begin(), name.end());   // the C string with its NUL
    bytes.push_back(0);
    agent_state_.last_card_id = static_cast<std::uint32_t>(rt_.g.card_slot);   // 004ffbbc: one global
    const AgentLoadResult r =
        load_profile(*services_.awards, refill_host(), *save_buffer_,
                     AgentRecord(agent_), std::span<const std::uint8_t>(bytes), slot, agent_state_,
                     card_.state(rt_.charmap.ready != 0, rt_.charmap.count), card_.reader());
    rt_.g.card_slot = static_cast<std::int32_t>(agent_state_.last_card_id);
    record("00334790 agent load '" + name + "' slot " + std::to_string(slot) + ": " + std::to_string(r.result));
    return static_cast<std::uint8_t>(r.result & 0xff);
}

void FrontendSession::agent_activate(std::int32_t agent) {   // 00292c54
    check_profile(agent);
    // byte 00493a60 = 0, then *(agent + 9a4)'s 0x44 quadwords to 0055d510 (GuiAgentModify's packing).
    rt_.g.packing_slot = 0;
    std::copy(agent_state_.appearance.begin(), agent_state_.appearance.end(), rt_.g.packings[0].begin());
    record("00292c54 byte 00493a60 = 0, the appearance block to 0055d510");
}

std::optional<std::string> FrontendSession::profile_name(std::int32_t profile) {
    check_profile(profile);
    return agent_name(view(), {rt_.g.app_35 != 0, false, rt_.g.app_24c}, services_.strings);
}

std::optional<std::string> FrontendSession::profile_rank(std::int32_t profile) {
    check_profile(profile);
    return agent_rank(view(), {rt_.g.app_35 != 0, false, rt_.g.app_24c}, services_.strings,
                      services_.awards->registries, rank_value);
}

std::int32_t FrontendSession::profile_clp(std::int32_t profile) {
    check_profile(profile);
    return agent_clp(view(), agent_state_, services_.awards->registries);
}

std::int32_t FrontendSession::level_done(std::int32_t profile, const std::string& key) {
    check_profile(profile);
    return agent_level_done(view(), services_.awards->registries, services_.awards->pool, key);
}

std::int32_t FrontendSession::level_flag(std::int32_t profile, std::int32_t level, std::int32_t k) {
    check_profile(profile);
    return agent_level_flag(view(), level, k);
}

std::vector<std::array<std::int32_t, 8>> FrontendSession::profile_level_records(std::int32_t profile) {
    check_profile(profile);
    std::vector<std::array<std::int32_t, 8>> out;  // agent + c4: count +c8, data +cc, 0x20 bytes each
    out.reserve(agent_state_.level_records.size());
    for (const auto& rec : agent_state_.level_records) {
        std::array<std::int32_t, 8> words{};
        for (std::size_t i = 0; i < words.size(); ++i)
            words[i] = static_cast<std::int32_t>(static_cast<std::uint32_t>(rec[i * 4]) |
                                                 static_cast<std::uint32_t>(rec[i * 4 + 1]) << 8 |
                                                 static_cast<std::uint32_t>(rec[i * 4 + 2]) << 16 |
                                                 static_cast<std::uint32_t>(rec[i * 4 + 3]) << 24);
        out.push_back(words);
    }
    return out;
}

std::optional<std::string> FrontendSession::profile_6060(std::int32_t profile, const std::string& key) {
    check_profile(profile);
    return agent_short_name(key);
}

std::int32_t FrontendSession::profile_3b10(std::int32_t profile, const std::string& key) {
    check_profile(profile);
    return agent_objective_done(view(), services_.awards->registries, services_.awards->pool, key);
}

std::vector<gui::GuiHost::BonusRow> FrontendSession::profile_vec(std::int32_t profile, std::int32_t kind) {
    check_profile(profile);
    std::vector<BonusRow> out;
    for (const auto& r : agent_award_rows(agent_state_, services_.awards->registries, kind))
        out.push_back({r.key, r.flag, r.unlock, 0});
    return out;
}

std::vector<gui::GuiHost::BonusRow> FrontendSession::profile_ratings(std::int32_t profile) {
    check_profile(profile);
    std::vector<BonusRow> out;
    for (const auto& r : agent_rating_rows(view(), agent_state_, services_.awards->registries))
        out.push_back({r.key, static_cast<std::uint8_t>(r.flag), r.unlock, r.percent});
    return out;
}

gui::GuiHost::RankRow FrontendSession::profile_rank_row(std::int32_t profile) {
    check_profile(profile);
    const auto b50 = static_cast<std::uint32_t>(profile_word(profile, 0xb50));
    const auto& ranks = services_.awards->registries.ranks;
    for (std::size_t k = 0; k < ranks.size(); ++k)
        if (rank_value(k) == b50) return {ranks[k].name, ranks[k].index};
    unreachable(0x45fb40, "a +b50 that is no rank row");
}

std::int32_t FrontendSession::profile_statistic(std::int32_t profile, std::int32_t index) {
    check_profile(profile);
    return read_agent_statistic(agent_statistics(), static_cast<std::uint32_t>(index));  // 00333ee0
}

// *(agent + 9a4): the agent record's appearance block (agent_state_, always allocated with the record).
std::optional<AgentPacking> FrontendSession::profile_appearance(std::int32_t profile) {
    check_profile(profile);
    AgentPacking p{};
    std::copy(agent_state_.appearance.begin(), agent_state_.appearance.end(), p.begin());
    return p;
}

bool FrontendSession::quip_playing(const std::string& name) {
    if (!services_.sound) return false;  // no sound manager: nothing is playing
    return services_.sound->quip_playing(name);
}

std::vector<gui::GuiHost::BonusItem> FrontendSession::profile_items(std::int32_t profile, std::int32_t kind,
                                                                    std::int32_t index) {
    check_profile(profile);
    std::vector<BonusItem> out;
    const auto text = [&](const std::optional<std::string>& k) -> std::optional<std::string> {
        if (!k) return std::nullopt;  // 003fdb50(NULL)
        return services_.strings ? services_.strings(*k) : std::nullopt;
    };
    if (kind == 4) {  // 00333260's omega requirements
        const ProfileAwards& awards = *services_.awards;
        for (const auto& r : agent_omega_items(agent_state_, awards.registries, index))
            out.push_back({r.name, r.bit, text(r.name), r.a, r.b, r.c, r.unlock});
        auto& list = agent_state_.awards[4];  // agent + af0: element 0
        if (list.empty()) unreachable(0x333418, "00335350 on an empty omega list");
        if (awards.registries.awards[4].empty()) unreachable(0x333408, "00335350 with no omega registry row");
        award_element_step(AgentRecord(agent_), list[0], awards.registries.awards[4][0].unlock, 0, awards.tables);
        if (list[0].flag) {
            const auto r = agent_omega_last(awards.registries);
            out.push_back({r.name, r.bit, text(r.name), r.a, r.b, r.c, r.unlock});
        }
        return out;
    }
    for (const auto& r : agent_award_items(agent_state_, services_.awards->registries, services_.awards->pool, kind, index))
        out.push_back({r.name, r.bit, text(r.name), 0, 0, 0, -1});
    return out;
}

std::vector<gui::GuiHost::BonusItem> FrontendSession::profile_level_objectives(std::int32_t profile,
                                                                               const std::string& short_name) {
    check_profile(profile);
    const ProfileAwards& awards = *services_.awards;
    std::vector<BonusItem> out;
    for (const auto& k : agent_level_objective_keys(awards.registries, awards.pool, short_name)) {
        const std::uint8_t flag = agent_objective_done(view(), awards.registries, awards.pool, k) ? 2 : 1;
        out.push_back({k, flag, services_.strings ? services_.strings(k) : std::nullopt, 0, 0, 0, -1});
    }
    return out;
}

namespace {
std::vector<gui::GuiHost::RankEntry> rank_entries(const std::vector<AwardRankEntry>& rows) {
    std::vector<gui::GuiHost::RankEntry> out;
    for (const auto& r : rows) out.push_back({r.name, r.index, r.threshold, r.unlock, r.text});
    return out;
}
}  // namespace

std::vector<gui::GuiHost::RankEntry> FrontendSession::profile_ranks() {
    return rank_entries(services_.awards->registries.ranks);
}

std::vector<gui::GuiHost::RankEntry> FrontendSession::profile_dossiers() {
    return rank_entries(services_.awards->registries.dossiers);
}

std::string FrontendSession::string_key(std::int32_t index) { return pool_key_text(services_.awards->pool, index); }

void FrontendSession::level_start(std::int32_t profile, std::int32_t level, std::int32_t a, std::int32_t b,
                                  std::int32_t c) {
    check_profile(profile);
    agent_level_start(AgentRecord(agent_), level, a, b, c);
}

// The heap 1 GuiGameOptions / GuiSlideShow open and close in the front end
// (0036b250(1) frees it, 0036b3b0(1, 0x100000, 0x800) creates it; 003eb750
// frees the 0055a870 collection's blocks): native memory, recorded.
void FrontendSession::options_frontend_sleep() { record("003eb750 + 0036b250(1) heap 1 closed"); }
void FrontendSession::options_frontend_wake() { record("0036b250(1) + 0036b3b0(1, 0x100000, 0x800) heap 1 opened"); }
void FrontendSession::port_options_wake(gui::Widget* screen) {
    if (services_.port_rows.wake) services_.port_rows.wake(rt_, screen);
}
bool FrontendSession::port_options_pressed(gui::Widget* screen, gui::Widget* sender) {
    return services_.port_rows.pressed && services_.port_rows.pressed(rt_, screen, sender);
}

void FrontendSession::radio_sound(std::int32_t kind, std::int32_t type, const std::string& name, std::int32_t priority) {
    // The +90 record (GuiSlideShow) sent as 0020ecb0 (kind 0) / 0020f2a0
    // (kind 2) with +20 = 1 and delivered (003c9420).
    if (kind != 0 && kind != 2) unreachable(0x3c9420, "a slideshow sound message of kind " + std::to_string(kind));
    if (services_.sound) {
        GameSoundManager& s = *services_.sound;
        const auto rec = s.request(type, name, 0, priority);
        GameSoundMessage m;
        m.kind = kind == 0 ? GameSoundMessage::Kind::stop_20ecb0 : GameSoundMessage::Kind::quip_20f2a0;
        m.flag_20 = 1;
        m.request = s.message_copy(rec, kind == 0);
        s.deliver(m);
    } else {
        record(std::string(kind == 0 ? "0020ecb0 stop " : "0020f2a0 quip ") + std::to_string(type) + " '" + name +
               "' (no sound)");
    }
}

std::uint32_t FrontendSession::zeus_archive() { return services_.zeus_lookup ? 1u : 0u; }

std::int32_t FrontendSession::zeus_lookup(std::uint32_t archive, const std::string& name) {
    if (archive != 1 || !services_.zeus_lookup) unreachable(0x36d2a0, "a member lookup outside ZEUS.HOG");
    return services_.zeus_lookup(name);
}

std::int32_t FrontendSession::zeus_has(std::int32_t profile, std::int32_t a1, std::int32_t a2, std::int32_t a3) {
    check_profile(profile);
    return agent_zeus_has(view(), a1, a2, a3);
}

std::int32_t FrontendSession::zeus_viewed(std::int32_t profile, std::int32_t a1, std::int32_t a2, std::int32_t a3) {
    check_profile(profile);
    return agent_zeus_viewed(view(), a1, a2, a3);
}

gui::GuiHost::BitmapTexture FrontendSession::bitmap_load(std::uint32_t archive, const std::string& name) {
    if (archive == 0) {   // 0036e4b0: GuiAgentModify's patch pictures (00340320)
        if (!services_.file_load) unreachable(0x36e4b0, "a bitmap texture file '" + name + "' without the front end's files");
        return services_.file_load(name);
    }
    if (archive != 1 || !services_.zeus_load) unreachable(0x380a40, "a bitmap texture outside ZEUS.HOG ('" + name + "')");
    return services_.zeus_load(name);
}

void FrontendSession::bitmap_release(std::int32_t slot) {
    if (!services_.zeus_release) unreachable(0x3813a0, "a bitmap texture release without a texture table");
    services_.zeus_release(slot);
}

std::uint8_t FrontendSession::profile_byte(std::int32_t profile, std::uint32_t offset) {
    check_profile(profile);
    if (offset >= agent_record_size) throw std::runtime_error("Front end: profile byte past the agent record");
    return agent_[offset];
}

std::int32_t FrontendSession::profile_word(std::int32_t profile, std::uint32_t offset) {
    check_profile(profile);
    if (offset % 4 != 0 || offset + 4 > agent_record_size) throw std::runtime_error("Front end: bad profile word offset");
    std::uint32_t v = 0;
    for (std::uint32_t i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(agent_[offset + i]) << (8 * i);
    return static_cast<std::int32_t>(v);
}

std::string FrontendSession::profile_text(std::int32_t profile, std::uint32_t offset) {
    check_profile(profile);
    std::string s;
    for (std::uint32_t i = offset;; ++i) {
        if (i >= agent_record_size) throw std::runtime_error("Front end: a profile string runs past the agent record");
        if (agent_[i] == 0) return s;
        s.push_back(static_cast<char>(agent_[i]));
    }
}

std::uint32_t FrontendSession::card_make_directory() {   // 0040d740: the save directory (no icon files on PC)
    const bool ok = card_.create_save_directory();
    record(std::string("0040d740 card directory: ") + (ok ? "1" : "0"));
    return ok ? 1 : 0;
}

// 0040db10: only a PORT-ONLY simulated card (AgentCardSimulation, 132cc10) reads unformatted.
std::uint32_t FrontendSession::card_format() {
    if (!card_.simulation()) unreachable(0x40db10, "a card format (the folder card always reads formatted)");
    const bool ok = card_.format();
    // 0040db10 waits for W +5a0 (set by a completed info cycle 0040efa0) and returns +59e: W reads the
    // formatted card.
    card_refresh();
    record(std::string("0040db10 card format (PORT-ONLY simulated card): ") + (ok ? "1" : "0"));
    return ok ? 1 : 0;
}

void FrontendSession::card_remove(const std::string& name) {   // 0040dc80 (its result is ignored)
    const bool ok = card_.remove(name);
    record("0040dc80 card remove '" + name + "': " + (ok ? "1" : "0"));
}

// 0040ddc0: the folder card renames at once; command 9's success copies the new name into the entry
// table (0040ec40), which is the runtime's g.card.dir here.
void FrontendSession::card_rename(const std::string& from, const std::string& to) {
    const bool ok = card_.rename(from, to);
    if (ok) {
        auto& dir = rt_.g.card.dir;
        const auto it = std::find(dir.begin(), dir.end(), from);
        if (it != dir.end()) *it = to;
    }
    record("0040ddc0 card rename '" + from + "' -> '" + to + "': " + (ok ? "1" : "0"));
}

std::uint32_t FrontendSession::card_exists(const std::string& name) {   // 0040d690
    const auto entries = card_.list();
    return std::find(entries.begin(), entries.end(), name) != entries.end() ? 1 : 0;
}

// 00334110 on the front end's agent. Its two inputs the EE leaves to the hardware are port-only here:
// - the key word reads T1_COUNT (10000800) + *00533870; the session's deterministic timer model gives T1
//   (frames x 9600, 16-bit wrap, timer_t1) and 00533870 (0: no timer interrupt is modelled), so the file +4
//   depends on the front-end frame of the save. The cipher stores the key, so the load reads any key back.
// - the 0xac-byte message filler for slots past *0052fe80 is its uninitialised stack (sp + 0x60); here it is
//   zero bytes. The load skips those slots.
// The message flush 002c8090(*004ffc04) returns at once offline (004294c0, the network manager, is null).
std::uint32_t FrontendSession::agent_save(std::int32_t profile) {
    check_profile(profile);
    static const std::array<std::uint8_t, agent_message_entry_size> stack_filler{};
    std::vector<std::uint8_t> records;
    records.reserve(agent_state_.level_records.size() * agent_level_record_size);
    for (const auto& rec : agent_state_.level_records) records.insert(records.end(), rec.begin(), rec.end());
    const std::uint32_t key = agent_save_key_word(timer_t1(), g_.timer_base_533870);
    AgentSaveSources sources{agent_state_.appearance, records, agent_state_.message_count, agent_state_.messages,
                             stack_filler, key};
    const AgentSaveResult r = save_agent(*save_buffer_, std::span<std::uint8_t, agent_record_size>(agent_), sources,
                                         card_.state(rt_.charmap.ready != 0, rt_.charmap.count), card_.writer());
    // The key word and frame keep each save's line distinct (record() drops repeated lines).
    char tail[48];
    std::snprintf(tail, sizeof tail, " key 0x%08x frame %lld", static_cast<unsigned>(key), static_cast<long long>(frames()));
    record("00334110 agent save: written " + std::to_string(r.written ? 1 : 0) + " result " +
           std::to_string(r.result ? 1 : 0) + " size " + std::to_string(r.size) + tail);
    return r.result ? 1 : 0;
}

// 00333f60 on the front end's agent. The card-busy byte 0055a834 is never set by the folder card.
std::uint32_t FrontendSession::agent_create(std::int32_t profile, std::int32_t slot, std::uint32_t nosave) {
    check_profile(profile);
    const AgentRefillHost refill = refill_host();
    const std::uint32_t r = create_agent(AgentRecord(agent_), agent_state_, services_.awards->tables, slot, nosave,
                                         false, refill, [&] { return agent_save(profile); });
    record("00333f60 agent create slot " + std::to_string(slot) + " nosave " + std::to_string(nosave) + ": " +
           std::to_string(r));
    return r;
}

void FrontendSession::agent_set_name(std::int32_t profile, const std::string& name) {  // 003340b0
    check_profile(profile);
    set_agent_name(AgentRecord(agent_), name, agent_state_.message_count);
    record("003340b0 agent name '" + name + "'");
}

// ---------------------------------------------------------------- GuiAgentModify (Checkpoint 132cc4b)

AgentPartCache* FrontendSession::agent_parts() {   // 0055dda0
    if (!services_.agent_parts) unreachable(0x22a220, "the front end's character part cache (0055dda0) without the disc");
    return services_.agent_parts();
}

// Checkpoint 132f13: the personnel pages' GuiWeaponDisplay (wd_set 00441060) on the front end's
// database *004ffd30 and its WPNHOG.HOG library (the fields play_main's PlayGuiHost reads).
std::optional<gui::GuiHost::WeaponRecord> FrontendSession::weapon_record(std::int32_t item) {   // 00147840
    if (!services_.weapons) unreachable(0x147840, "the weapon record of item " + std::to_string(item) + " without the disc");
    const auto& rec = services_.weapons()->database().record(item);
    WeaponRecord r;
    r.name_80 = rec.display_name_80();
    r.hold_f0 = rec.hold_class_f0();
    r.icon_11c = rec.hud_icon_11c();
    r.model_13c = rec.model_13c();
    r.w60 = rec.word(0x60);
    r.w64 = rec.damage_mid_64();
    r.clip_70 = rec.clip_70();
    r.clips_74 = rec.clips_74();
    r.class_30 = rec.weapon_class_30();
    r.byte_bc = rec.byte(0xbc);
    return r;
}

void FrontendSession::weapon_preload(std::int32_t item) {   // 00145710(db, item, 0)
    if (!services_.weapons) unreachable(0x145710, "the weapon model preload of item " + std::to_string(item) + " without the disc");
    services_.weapons()->preload(item, 0);
    record("00145710(db, " + std::to_string(item) + ", 0)");
}

std::vector<std::string> FrontendSession::agent_locked_parts(std::int32_t profile) {   // 00332500
    check_profile(profile);
    const ProfileAwards& awards = *services_.awards;
    std::vector<std::string> names;
    for (const auto row : locked_award_parts(AgentRecord(agent_), agent_state_, awards.tables, awards.parts))
        names.push_back(awards.parts.at(row).name);
    return names;
}

void FrontendSession::agent_store_appearance(std::int32_t profile, const AgentPacking& packing,
                                             const AgentPartCache& cache) {   // 00334000
    check_profile(profile);
    std::copy(packing.begin(), packing.end(), agent_state_.appearance.begin());   // 0x44 quadwords to *(agent + 9a4)
    AgentPacking stored;
    std::copy(agent_state_.appearance.begin(), agent_state_.appearance.end(), stored.begin());
    const auto compact = agent_packing_compact(cache, stored);   // 004073c0(*(agent + 9a4), &n)
    const std::size_t n = std::min<std::size_t>(compact.size(), 0x100);
    std::fill(agent_.begin() + 0x9a8, agent_.begin() + 0xaa8, std::uint8_t{0});
    std::copy(compact.begin(), compact.begin() + static_cast<std::ptrdiff_t>(n), agent_.begin() + 0x9a8);
    record("00334000 agent appearance stored (" + std::to_string(compact.size()) + " compact bytes)");
}

void FrontendSession::profile_set_byte(std::int32_t profile, std::uint32_t offset, std::uint8_t v) {
    check_profile(profile);
    if (offset >= agent_record_size) throw std::runtime_error("Front end: profile byte store past the agent record");
    agent_[offset] = v;
}

std::int32_t FrontendSession::loc_find(const std::string& key) {   // 003fe650
    if (!services_.loc_find) unreachable(0x3fe650, "Loc_FindKey '" + key + "' without the string table");
    return services_.loc_find(key);
}

std::optional<std::string> FrontendSession::loc_text(std::int32_t id) {   // 003fdb80
    if (!services_.loc_text) unreachable(0x3fdb80, "Loc_GetTextById without the string table");
    return services_.loc_text(id);
}

// 0040f540: World_ClearPending when *00571cd0 is set; the front end has no world (*00571cd0 = 0).
void FrontendSession::app_40f540() { record("0040f540 (no world: *00571cd0 = 0)"); }


// ---------------------------------------------------------------- GuiAgentCreate (Checkpoint 132cc5)

// Skl_Load 00402840's reads of GAMEDATA\FRONTEND\MALE.SKL / FEMALE.SKL (its fopen / fread).
std::vector<std::uint8_t> FrontendSession::skl_file(const std::string& path) {
    if (!services_.frontend_file) unreachable(0x402840, "Skl_Load's " + path + " without the disc");
    auto bytes = services_.frontend_file(path);
    record("00402840 Skl_Load " + path + " (" + std::to_string(bytes.size()) + " bytes)");
    return bytes;
}

void FrontendSession::profile_set_word(std::int32_t profile, std::uint32_t offset, std::int32_t v) {
    check_profile(profile);
    if (offset + 4 > agent_record_size) throw std::runtime_error("Front end: profile word store past the agent record");
    const auto u = static_cast<std::uint32_t>(v);
    for (std::uint32_t i = 0; i < 4; ++i) agent_[offset + i] = static_cast<std::uint8_t>(u >> (8 * i));
}

void FrontendSession::profile_set_text(std::int32_t profile, std::uint32_t offset, const std::string& s) {  // 00129360
    check_profile(profile);
    if (offset + s.size() + 1 > agent_record_size)
        throw std::runtime_error("Front end: profile string store past the agent record");
    for (std::size_t i = 0; i < s.size(); ++i) agent_[offset + i] = static_cast<std::uint8_t>(s[i]);
    agent_[offset + s.size()] = 0;
}

}  // namespace omega
