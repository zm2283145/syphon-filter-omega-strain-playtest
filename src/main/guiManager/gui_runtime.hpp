#pragma once

// Front-end GUI widget runtime (checkpoint 118, stage F5): the widget tree
// that a decoded .GUI file (frontend_formats.hpp) becomes, the frontend canvas
// with its screen stack, the pad translation into widget input events, focus
// navigation, the per-frame update, and the main-menu screen GuiTitleScreen.
//
//   widget        vtable 004e0b40  (GuiWidget)
//   text          vtable 004e0ac0  (GuiTextWidget, ctor 0041c7d0)
//   button        vtable 004e0c00  (GuiButtonWidget, ctor 00420d50)
//   title screen  vtable 004dd0a0  (GuiTitleScreen, TITLESCR)
//   decorator     vtable 004e09f0  (GuiInterfaceDecorator, ctor 00417bc0)
//   canvas        vtable 004e19b0  (GuiFrontendCanvas)
//
// Everything outside the runtime (strings, fonts, IE lookups, the memory
// card, popups, sounds, the pad driver, the text-object renderer, the screen
// factory) is a GuiHost service, called in the original order with the
// original arguments. Paths whose callee is not hosted (an interface element
// that resolves, the keyboard, the TRIANGLE debug path, the string popup,
// widget classes other than GuiWidget / GuiTextWidget / GuiButtonWidget)
// throw naming the address.

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "agent_create.hpp"
#include "agent_modify.hpp"
#include "frontend_formats.hpp"
#include "gui_keyboard_tables.hpp"

namespace omega {
class SaveAgentFlow;
}

namespace omega::gui {

// Checkpoint 118b adds GuiTextEditWidget (vtable 004e0a40, factory 0041bd30)
// and GuiMessageBox (vtable 004dd9d0, ctor 002a8780), the application popup
// queue that drives it (002cae30 / 002cac80 / 002caba0) and the GUI manager's
// screen factory, cache and unloading (00418a80 / 00418840 / 00418e00 /
// 00419270). They are native when GuiRuntime::native_app is set; otherwise
// popups, the factory and release stay GuiHost services as in checkpoint 118.
// Checkpoint 118d adds the on-screen keyboard GuiKeyboard (vtable 004de790,
// ctor 0032d2a0, KEYBOARD.GUI) and the character code map at 0055ca90 that
// its key labels and typed characters go through (load 0043fbb0, lookup
// 003fd6b0).
// Checkpoint 118e adds GuiTextListWidget (vtable 004e0c80, ctor 00421bf0)
// with the GuiArrayWidget (vtable 004e0d20, ctor 00424c50) layer it derives
// from: the grid selection, scrolling, cursor and item list.
// Checkpoint 118e also adds GuiAgentLoad (vtable 004dd400, create 002940b0,
// AGENTOPN.GUI) over the GuiSaveAgentWidget layer whose event 00441ca0 it
// falls back to.
// Checkpoint 118f adds GuiOnlineOfflineSelect (vtable 004dd6b0, create
// 002a0c30, ONOFFSEL.GUI): the screen after an offline agent load.
// Checkpoint 132f adds the in-game start (pause) menu: GuiObjectives
// (vtable 004df5c0, ctor 00368e10, OBJECTIV.GUI), its subclass GuiStartMenu
// (vtable 004e1430, ctor 00456d90, INGAMENU.GUI) and GuiStartMenuBackdrop
// (vtable 004e1490, GAMEBACK.GUI), hosted on the game canvas (event 004653b0).
// Checkpoint 132f2 adds GuiSliderWidget (vtable 004e10c0, ctor 0043f910) and
// the options screen GuiGameOptions (vtable 004dd580, ctor 0029ba10,
// GAMEOPT2.GUI from the start menu, GAMEOPT.GUI from the title).
// Checkpoint 132f3 adds the start menu's briefing pane GuiBriefingPane
// (vtable 004e13d0, BRIEFING.GUI) with GuiMLTextWidget (vtable 004e1040,
// ctor 0043e5d0: the word-wrapped multi-line text) and GuiScrollBarWidget
// (vtable 004e0f40, ctor 0043bef0).
// Checkpoint 132f3 also adds the start menu's radio log GuiRadioLog (vtable
// 004e1290, RADIOLOG.GUI) with GuiSubTitleDisplay (vtable 004e1560, ctor
// 00458590: the timed "typewriter" subtitle of the replayed quip).
// Checkpoint 132zf adds the command centre's ZEUS file screen GuiSlideShow
// (vtable 004de710, ctor 00328660, ZEUSHELL.GUI) with GuiBitmapWidget
// (vtable 004e0fc0, ctor 0043cb20: one texture of a HOG archive).
// Checkpoint 132f3b adds the start menu's equipment pane GuiEquipmentPane
// (vtable 004e12f0, EQUIPNT.GUI) with GuiWeaponDisplay (vtable 004e11a0,
// ctor 004412f0: the turning weapon model), GuiTextArrayWidget (vtable
// 004e0dc0, ctor 00426cc0) and its subclass GuiWeaponStats (vtable 004e1350).
// It also adds the start menu's stats screen GuiScoresEquip (vtable 004df140,
// ctor 0035d430, SCOREONE.GUI / SCORES.GUI) with GuiCharacterDisplay (vtable
// 004de670, ctor 00326860: the draw-callback holder of its figure).
// port_cheats is port-only (checkpoint 132f5, not in the original): the
// pause menu's Cheats screen, built from GAMEOPT2's widgets (PortGuiCheats); port_settings likewise
// (checkpoint 132win11): the Port Settings screen (PortGuiSettings over a PortSettingsScreen model).
// Checkpoint 132f4 adds the level-start loadout screen GuiEquipmentModify
// (ctor 002c1100, init 002c0790, input 002bcb50, event 002bd560, EQMODIFY.GUI).
// Checkpoint 132cc4b adds the personnel screen's appearance editor
// GuiAgentModify (vtable 004de920, ctor 00343e10, AGENTMOD.GUI), whose
// GuiCharacterDisplay "playercharacter" shows the PORT-ONLY model preview.
// Checkpoint 132cc5 adds the new-agent screens: GuiAgentCreate (vtable 004dd1b0,
// ctor 0028f760, AGENTNEW.GUI) and GuiAgentSave (vtable 004dd3a0, ctor 00293fc0,
// AGENTOPN.GUI over the GuiSaveAgentWidget).
// Checkpoint 132f11 adds the debrief 00134110's screens (gui_end_game.cpp):
// GuiEndGameObjectives (vtable 004e17c0, SUMMARY.GUI, over GuiObjectives),
// GuiEndGameStatistics (vtable 004e1840, SCOREONE / SCORES.GUI, over
// GuiScoresEquip), GuiEndGameAwards (vtable 004e1730, AWARDS.GUI) and
// GuiAutoSaver (vtable 004e16d0, over the GuiSaveAgentWidget).
// Checkpoint 132f12 adds the command centre's personnel "statistics" file
// (gui_personnel.cpp): GuiMissionStatistics (vtable 004dee70, ctor 0034fcb0,
// STATS.GUI) over the GuiPersonnelScreen base (vtable 004df0b0, ctor
// 00356ff0), with its table GuiMissionStatList (vtable 004deee0, ctor
// 00350460 over GuiTextArrayWidget: own cell draw 0034fec0).
// Checkpoint 132f13 adds the other personnel files over the same base:
// GuiDossiers, GuiRanks, GuiRatings, GuiOmegaStrain, GuiAchievementScreen
// (SARATING / COMENDAT / MEDALS) and the sub-menu GuiPersonnelMenu (CSUBMENU).
// Checkpoint 132ln2 adds the online connection screen GuiOnlineConnect (vtable
// 004dd620, ctor 0029f2a0 over the net screen 0033d580, ONLINECN.GUI;
// gui_online_connect.cpp), the lobby strings 0033ce30 and the online screens'
// navigator 0033c320, with its PORT-ONLY LAN mode (research/LAN_PLAY.md).
// Checkpoint 132ln3 adds the mission setup screen GuiMissionSetup (vtable
// 004dd730, ctor 002a19c0 over the net screen 0033d580, MSNSETUP.GUI;
// gui_mission_setup.cpp) with its PORT-ONLY LAN host (begin_mission 002a0f60).
enum class WidgetClass : std::uint8_t { widget, text, button, title, canvas, edit, msgbox, keyboard, textlist, agentload,
                                        onoffsel, cmdcentr, objectives, startmenu, startback, slider, gameopt,
                                        scrollbar, mltext, briefing, radiolog, subtitle, slideshow, bitmap,
                                        textarray, weaponstats, weapondisplay, equippane, chardisplay,
                                        scoresequip, port_cheats, eqmodify, agentmod, agentcreate, agentsave,
                                        mapscreen, endobjectives, endstats, endawards, autosaver, missionstats,
                                        missionstatlist, port_settings, dossiers, ranks, ratings, omega, achieve, pmenu,
                                        agentinfo, agentdisplay, onlinecn, msnsetup,
                                        netjoin, missionlist, chatwin, acntlogn, onlinepref, netmess,
                                        netmsgarray, netcontacts, netagencycell };

// PORT-ONLY (checkpoint 132ln1): the label ONOFFSEL's "lan" button shows with GuiHost::port_lan_play (the
// disc's text there is the developer placeholder "TEST"; no LAN string exists on the disc). Worded like the disc's
// $PlayOnline / $PlayOffline buttons at the user's request.
inline constexpr const char* port_lan_play_label = "Play LAN";
// PORT-ONLY (checkpoint 132ln2): the LAN-mode ONLINECN's configuration group (no LAN strings exist on the disc):
// config_title, and the two config_list rows in place of the PS2 network configurations 0029d120 lists.
inline constexpr const char* port_lan_game_title = "LAN GAME";
inline constexpr const char* port_lan_host_label = "HOST LAN GAME";
inline constexpr const char* port_lan_join_label = "JOIN LAN GAME";
// PORT-ONLY (checkpoint 132ln4): GuiNetJoinMission in LAN mode. region_name's text (the agent's online region in
// the original), and the mission column's text for a LAN game of another build (listed greyed, not joinable).
inline constexpr const char* port_lan_region = "LAN";
inline constexpr const char* port_lan_version_mismatch = "VERSION MISMATCH";

// The string keys the lobby's callback (event 0x1e) carries in t0, by address, and their names (an unknown key
// is unhosted, 0033ec18).
inline constexpr std::int32_t lobby_key_f_to_connect = 0x4bf9b8;     // "F_TO_CONNECT" (0042a870: a failed connect)
inline constexpr std::int32_t lobby_key_f_to_joingame = 0x4bfa18;    // "F_TO_JOINGAME"
inline constexpr std::int32_t lobby_key_f_to_creategame = 0x4bfa30;  // "F_TO_CREATEGAME" (0042a870: a failed create)
std::string lobby_callback_key(std::int32_t t0);

// Widget +14 flag bits.
inline constexpr std::uint16_t flag_init = 0x1;
inline constexpr std::uint16_t flag_visible = 0x2;
inline constexpr std::uint16_t flag_enabled = 0x4;
inline constexpr std::uint16_t flag_awake = 0x8;
inline constexpr std::uint16_t flag_sleep_pending = 0x10;
inline constexpr std::uint16_t flag_releasable = 0x20;
inline constexpr std::uint16_t flag_unloading = 0x40;  // 00418e00
inline constexpr std::uint16_t flag_focusable = 0x80;

// Pad masks of the 16 translated buttons (table 004940b0, in scan order).
inline constexpr std::array<std::uint32_t, 16> pad_masks = {0x1000, 0x4000, 0x8000, 0x2000, 0x10, 0x40, 0x80, 0x20,
                                                            0x200,  0x400,  0x4,    0x1,    0x8,  0x2,  0x800, 0x100};
inline constexpr std::uint32_t pad_up = 0x1000, pad_down = 0x4000, pad_cross = 0x40, pad_triangle = 0x10;

// The text object 003d0940 creates for an awake text widget (the "draw
// list" entry of a text run): creation arguments and the bytes the widget
// writes into it (+10 halign, +11 / +12 from widget +74, +58 / +5b).
struct TextObject {
    std::array<std::int32_t, 4> rect{};  // cvt.w.s of widget +34..+40
    std::uint32_t font = 0;              // widget +54
    std::array<float, 4> rgba{};         // widget +60..+6c
    std::string text;                    // last 003d08b0
    std::uint8_t b10 = 0, b11 = 0, b12 = 0, b58 = 0, b59 = 0, b5b = 0;
    // +54 as GuiKeyboard's wake 0032ca00 writes it into its edit box's
    // object (min(max(0.2, 0), 1)); the value the 003d0940 constructor
    // leaves there is not modelled (0, as the interpreter's hooked
    // constructor leaves fresh heap).
    std::uint32_t w54 = 0;
};

struct Decorator {
    std::string ie;    // d+8: the file's IE name, or the interface name when empty
    std::string node;  // d+14
    std::int32_t ie_id = -1;  // d+20 (00412f80)
    std::uint32_t d80 = 0;    // d+80
    // d+30..+6c: the file's rows 0..2 (w 0) and translation row (w 1.0);
    // event 0x12 (004176c0) moves the translation x / z and calls the
    // IE's 003e8040 when one is attached (d+24, never in the native model).
    std::array<float, 16> matrix{};
    // d+70: the file's event records (00416e40 searches them by name).
    std::vector<GuiEvent> events;
};

// The options record (*004ffd2c, 0x24 bytes; defaults 0045a7a0). The screen
// keeps a copy at +b8 (fields +bc..+d8).
struct GameOptionsRecord {
    std::int32_t w0 = 1;            // +0
    std::uint8_t manual_aim = 0;    // +4
    std::uint8_t rumble = 1;        // +5
    std::uint8_t freelook_x = 1;    // +6
    std::uint8_t freelook_y = 0;    // +7
    float sfx = 0.8f;               // +8
    float voices = 1.0f;            // +c
    float music = 0.8f;             // +10
    std::int32_t sound_mode = 0;    // +14
    std::uint8_t b18 = 1, b19 = 1;  // +18 / +19
    float gamma = 0.0f;             // +1c
    std::int32_t video_mode = 0;    // +20
};
// 0045a600: the bits of the fields that differ (1 +4, 2 +5, 4 +6, 8 +7,
// 0x10 +8, 0x20 +c, 0x40 +10, 0x80 +14, 0x400 +1c, 0x800 +20).
std::uint32_t options_diff(const GameOptionsRecord& a, const GameOptionsRecord& b);
// vt +54 (the class id string) of a ported class.
const char* widget_class_name(WidgetClass c);
// SFO.CFG (the name at *004979a0), the options file: 0x24 bytes in the record's layout.
inline constexpr const char* options_file_name = "SFO.CFG";
// PORT-ONLY (not in the original): the names of the options screen's cheat
// entries (FrontendSession::add_port_cheat_entries) and their popup tags.
inline constexpr const char* port_cheat_all_widget = "port_cheat_all";
inline constexpr const char* port_cheat_weapons_widget = "port_cheat_weapons";
inline constexpr std::int32_t port_cheat_all_popup = 0x43485441;      // 'CHTA'; the original uses 0xa..0xd
inline constexpr std::int32_t port_cheat_weapons_popup = 0x43485457;  // 'CHTW'

// Port-only (checkpoint 132win11, not in the original): the model of the Port Settings screen PortGuiSettings
// (PORTSETTINGS, synthesised from GAMEOPT2's widgets; omega::PortSettingsPage implements it). The screen has
// `rows` fixed slots ("ps_label%d" / "ps_value%d" / "ps_background%d"); a slot whose label is empty is hidden.
// act() answers whether anything changed; the screen redraws its texts whenever revision() moves.
struct PortSettingsScreen {
    static constexpr std::int32_t rows = 9;
    enum class Action { previous, next, press, page_previous, page_next };
    virtual ~PortSettingsScreen() = default;
    virtual std::string title() const = 0;
    virtual std::string label(std::int32_t row) const = 0;
    virtual std::string value(std::int32_t row) const = 0;
    virtual bool act(std::int32_t row, Action a) = 0;
    virtual std::uint64_t revision() const = 0;
    virtual void opened() {}  // the screen woke (reread the settings)
    virtual void closed() {}  // the screen left (a key capture is cancelled)
};
inline constexpr std::size_t options_file_size = 0x24;
// 0045a2a0's buffer: the record copied field by field to the stack (+0 word, +4..+7 bytes, +8 / +c / +10
// floats, +14 word, +18 / +19 bytes, +1c float), +20 written as 0 (stored 0 before the 0040e090 write and
// restored after). The padding +1a / +1b is the EE's uninitialised stack; here 0.
std::array<std::uint8_t, options_file_size> options_file_bytes(const GameOptionsRecord& r);
// 0045a150's copy of a read buffer into the record (every field, +20 included).
GameOptionsRecord options_file_record(std::span<const std::uint8_t, options_file_size> bytes);

// The GuiSaveAgentWidget base (start 00441a10, popup handler 00441ca0) of a
// screen built over it: GuiGameOptions (ctor 0029ba10) and GuiCommandCenter
// (over the net screen 0033d580). Both ctors store -2 at +48.
struct SaveWidgetFields {
    std::int32_t popup = -2;     // +48 the flow's popup id
    std::uint8_t mode = 0;       // +4c (set by the caller before 00441a10)
    std::int32_t slot = 0;       // +50
    std::uint8_t no_remove = 0;  // +54 the MC_NOREMOVE popup shown
    std::string old_name;        // +55 (the rename's previous name)
};

// Checkpoint 132ln4: one record of an SFOLobby list (the 0x50-byte records 0044fe18 / 0044eff0 write; 0044f290
// returns the i-th). +24 is the record's kind (0 a game; 1 / 2 / 3 players), +30 the game's status (1: open to
// join), +34 / +38 the agents and the maximum, +40 the online mission (0..8, table 004acb30), +44 the join filter
// (1 friends, 2 the agency cell), +48 the game's world id (0044e060's key).
struct LobbyRecord {
    std::int32_t id = 0;                       // +0 (002ac070 numbers an id-0 record from 0048bcb0)
    std::string name;                          // +4 char[0x20]
    std::array<std::int32_t, 11> w{};          // +24..+4c
    std::int32_t& at(std::uint32_t off) { return w[(off - 0x24) / 4]; }
    std::int32_t at(std::uint32_t off) const { return w[(off - 0x24) / 4]; }
    // PORT-ONLY (132ln4, LAN mode): the LanBrowseEntry the record stands for, and whether its build differs.
    std::uint64_t lan_session = 0;
    bool lan_mismatch = false;
    // Checkpoint 132on2c: a room chat record (the 0x68-byte list +ac, 00447bb0 / 00447c70): +0 id, +4 name,
    // +24 text char[0x40], +64 private (a whisper).
    std::string text;
    std::uint8_t b64 = 0;
};
// 0044fa00: an SFOLobby list (+4 count, +10 record size, +14 data, +18 changed: the ctor sets 1).
struct LobbyList {
    std::vector<LobbyRecord> items;
    std::uint8_t changed = 1;                  // +18 (0044f280 stores it)
};
// Checkpoint 132on2: GuiOnlineConnect's SFOLobby list rows (GuiHost::lobby_configs .. lobby_announcements).
struct LobbyConfig {        // the configuration list +98 (0x13c0 elements)
    std::uint8_t selected = 0;  // +0 (the default configuration)
    std::string name;           // +34 (the list's text)
};
struct LobbyServer {        // the MUIS server list +d0 (0x210 elements)
    std::string name;           // +0 (compared with app +2ac / 0048c138)
    std::string universe;       // +104 (002ca100 copies it to app +26c)
};
struct LobbyUniverse {      // 00442f40's universe list (0x44 elements)
    std::int32_t id = 0;        // +0 (compared with the agent's +9a0)
    std::string name;           // +4 (compared with the agent's +95c)
};
struct LobbyAnnouncement {  // app +2f8 (0x3ec elements)
    std::int32_t id = 0;        // +0
    std::string text;           // +4 (0x3e8 bytes)
};

struct Widget {
    WidgetClass cls = WidgetClass::widget;
    std::string name;                 // +4
    std::uint16_t flags = 0x26;       // +14
    std::int16_t layer = 0;           // +16
    Widget* parent = nullptr;         // +1c
    Widget* focus = nullptr;          // +20
    Widget* loader = nullptr;         // +24 (the loading root's widget; the root is its own)
    std::vector<Widget*> children;    // +28 list
    std::array<float, 4> rect{};      // +34..+40
    std::unique_ptr<Decorator> deco;  // +44 owner when set
    // GuiTextWidget / GuiButtonWidget
    std::string text;                 // +48
    std::uint32_t font = 0;           // +54
    std::array<float, 3> rgb{};       // +60..+68
    float alpha = 1.0f;               // +6c
    std::uint32_t halign = 0;         // +70
    std::uint8_t b74 = 0, b75 = 0, b76 = 0;
    std::optional<TextObject> obj;    // +78
    // GuiTitleScreen
    std::uint8_t demo = 0;            // +48
    float last_input = 0.0f;          // +4c (seconds)
    std::int32_t popup = -2;          // +50
    // GuiAgentLoad (create 002940b0)
    std::int32_t al_popup = -2;       // +48 (the GuiSaveAgentWidget popup id)
    float al_timer = 10.0f;           // +78 seconds since the slot list was rebuilt
    std::uint8_t al_7c = 0;           // +7c empty slots stay selectable
    // GuiOnlineOfflineSelect (create 002a0c30)
    std::uint8_t oo_online = 0;       // +48 byte 00506488 at init: online play only
    // GuiMapScreen (vtable 004e18c0, ctor 00464bf0; checkpoint 132hud)
    Widget* map_backdark = nullptr;   // +48 0041db80(w, "backdark") at init 00464b70
    bool map_6a0 = false;             // +4c the HUD's map (hud + 6a0) from wake 00464b30; 0 at sleep 00464b00
    // Manager bookkeeping
    std::int32_t id10 = -1;           // +10 (GuiMessageBox: the shown popup id)
    std::uint16_t refs = 0;           // +18 (00418a80 / 00418840)
    bool deleted = false;             // destroyed by 00419270 / 00418e00 (memory kept natively)
    // GuiTextEditWidget (ctor 0041bd30)
    std::uint8_t password = 0;        // +80
    std::uint32_t max_len = 100;      // +84
    std::uint8_t e88 = 0;             // +88
    std::int32_t cursor = 0;          // +8c
    // GuiMessageBox
    std::int32_t mb_type = 0;         // +48
    Widget* mb_target = nullptr;      // +4c
    std::int32_t mb_arg = 0;          // +50
    std::int32_t mb_layout = 0;       // +54
    struct Layout {
        Widget* group = nullptr;      // messagebox%d (0041dc70)
        Widget* title = nullptr;      // title%d
        Widget* message = nullptr;    // message%d
        Widget* button1 = nullptr;    // button1%d
        Widget* button2 = nullptr;    // button2%d
    };
    std::array<Layout, 3> layouts{};  // +58 + 0x14 i
    // GuiKeyboard (vtable 004de790); id10 (+10) of its key buttons is the
    // key index 0032cb40 assigns.
    struct Keyboard {
        Widget* label = nullptr;           // +48 "label_title" (GuiTextWidget)
        Widget* edit = nullptr;            // +4c "editbox2" (GuiTextEditWidget)
        std::int32_t max = 0;              // +50 maximum text length
        Widget* target = nullptr;          // +54 receives 0x1a / 0x1b on close
        std::int32_t arg = 0;              // +58 sent as a3
        std::int32_t key = 0;              // +5c current key index
        std::vector<Widget*> keys;         // +60
        std::vector<std::int32_t> ids;     // +70 key codes (row_ids)
        std::vector<std::int32_t> lay;     // +80 layout entry per key (-1 none)
        std::vector<std::int32_t> up, dn, lf, rt;  // +a0 / +b0 / +c0 / +d0
        std::int32_t layout = -1;          // +90 as an index of 0052af60 (-1 = null)
        std::int32_t lang = 0;             // +94
        const kbd::Dead* dead = nullptr;   // +98 pending dead key
        std::uint8_t altgr = 0;            // +9c the layout has AltGr characters
        std::uint8_t e0 = 0;               // +e0 (0032b4f0's last argument)
        std::uint8_t e1 = 0;               // +e1 one-shot shift
        std::uint32_t mods = 0;            // +e4 1 shift, 2 caps lock, 4 AltGr
        std::uint32_t usb_mods = 0;        // +e8 the USB keyboard modifiers last seen
        // The string 0032b2d0 sends with 0x1a (trimmed edit text); nullopt
        // after a cancel (0x1b, t0 0).
        std::optional<std::string> result;
    };
    std::unique_ptr<Keyboard> kb;
    // GuiArrayWidget (ctor 00424c50) + GuiTextListWidget (ctor 00421bf0).
    // The EE keeps the grid words unsigned (sltu compares) except where noted.
    struct List {
        std::uint32_t cols = 1, rows = 0;  // +48 / +4c (00421bf0 calls vt +5c (1, 0))
        std::uint32_t col = 0, row = 0;    // +50 / +54 selection
        std::uint8_t mode = 2;             // +58: 1 horizontal only, 2 vertical only (list ctor)
        std::uint32_t row_h = 0x18;        // +5c
        std::vector<std::int32_t> colw{0x3c};  // +60 vector (+64 count, +68 data; fill 00494158)
        std::uint32_t vis_cols = 1, vis_rows = 1;  // +70 / +74
        std::uint32_t left = 0, top = 0;   // +78 / +7c scroll origin
        std::uint8_t paging = 0;           // +80 (pad 0x8 / 0x4 page up / down)
        std::uint8_t wrap = 0;             // +81
        std::uint8_t no_scroll = 0;        // +82 (also makes 004245d0 report every cell visible)
        std::int32_t a84 = 0;              // +84 (00423550 / 00423540)
        Widget* cursor = nullptr;          // +88 "cursor" (0041dc70, any class)
        // +8c the scroll bar (GuiScrollBarWidget): 00424bc0's "vscrollbar"
        // child, or the one a screen stores (GuiRadioLog's "vscroll").
        // 00424430 / 00424750 end with 0043bc70(+8c, 0, row, 0, rows - 1).
        Widget* sb = nullptr;
        struct Item {
            std::string text;              // +0 string object
            std::int32_t id = 0;           // +c
            std::int8_t style = 0;         // +10 (sb / lb)
        };
        std::vector<Item> items;           // +90 vector of 0x14-byte items
        std::optional<TextObject> obj;     // +a0 (003d0940 at 0, 0, w, h)
        std::uint8_t a4 = 0;               // +a4 -> obj +10
        std::uint32_t font = 0;            // +a8
        std::array<float, 4> rgba{1.0f, 1.0f, 1.0f, 1.0f};  // +b0..+bc
        std::array<float, 4> rgba_c0{0.0f, 1.0f, 0.0f, 1.0f};  // +c0..+cc (00421bf0)
        std::array<float, 4> rgba_d0{0.6f, 0.6f, 0.6f, 1.0f};  // +d0..+dc (0x3f19999a)
        std::array<float, 4> rgba_e0{1.0f, 1.0f, 1.0f, 1.0f};  // +e0..+ec (style 3 / the highlighted row)
        std::uint8_t f0 = 0;               // +f0 highlight the selected row in +e0 (00420e50)
        std::uint8_t f1 = 0;               // +f1 -> obj +5b
    };
    std::unique_ptr<List> list;
    // GuiCommandCenter (vtable 004dd4f0; ctor 002984d0 over the net screen
    // 0033d580). The 24 "slots" are the 17 levels, the three extras, unlock,
    // online, personnel and training (+118); +178 is their "open" byte.
    struct CmdCenter {
        SaveWidgetFields save;                 // +48..+77 (cc_exit 002955b0 sets mode 2, the autosave)
        std::int8_t net78 = 0;                 // +78 (the net object; 0 offline)
        Widget* start_button = nullptr;        // +7c
        std::uint8_t active = 0;               // +80
        std::int32_t w84 = 1;                  // +84
        Widget* circle = nullptr;              // +88 "backgroundcircle"
        Widget* arrows = nullptr;              // +8c
        Widget* navigate = nullptr;            // +90
        Widget* navigatea = nullptr;           // +94
        Widget* levelname1 = nullptr;          // +98
        Widget* levelname2 = nullptr;          // +9c
        Widget* pou = nullptr;                 // +a0 "personnel_online_unlocked"
        Widget* personnel = nullptr;           // +a4
        Widget* online = nullptr;              // +a8
        Widget* unlock = nullptr;              // +ac
        Widget* training = nullptr;            // +b0
        std::array<Widget*, 17> levels{};      // +b4 "level%d"
        std::array<std::uint8_t, 18> bonus{};  // +f8 (00332860(level + 1, -1); +109 the training's)
        std::array<Widget*, 3> extras{};       // +10c "extra%d"
        std::array<Widget*, 24> slots{};       // +118
        std::array<std::uint8_t, 24> open{};   // +178 (ctor: all 1)
        Widget* bonus_grp = nullptr;           // +190 "bonusmissions"
        Widget* list = nullptr;                // +194 "textlist"
        std::array<Widget*, 6> zeus{};         // +198
        float pulse = 0.0f;                    // +1b0
        std::array<std::uint8_t, 24> panel_of{};  // +1b4
        std::int32_t sel = -1;                 // +1cc
        Widget* sel_w = nullptr;               // +1d0
        std::int8_t panel = 0;                 // +1d4
        std::array<Widget*, 6> groups{};       // +1d8
        std::array<std::uint8_t, 17> cached{};  // +1f0 one byte per level: cache looked up
        std::array<std::string, 17> cache;     // +204 + 0xc i (empty: 003fdb50 returned NULL or "")
        std::array<Widget*, 6> saved{};        // +2d0 the focus kept per panel
        std::uint8_t keep = 0;                 // +2e8
        Widget* zeus_files = nullptr;          // +2ec
        Widget* modify_agent = nullptr;        // +2f0
        std::uint8_t exiting = 0;              // +2f4
        std::array<Widget*, 10> movies{};      // +2fc "%2.2d"
        std::int32_t bonus_movie = 0;          // +324 the bonus movie's number (19_%d; set by 002943f8 only)
        // +2f8: the online quit question's popup (00295818); the ctor leaves it unset, only its reply reads it
        std::int32_t quit_popup = -1;
    };
    std::unique_ptr<CmdCenter> cc;
    // GuiObjectives (ctor 00368e10) and GuiStartMenu (ctor 00456d90) over it.
    struct Objectives {
        std::array<Widget*, 4> lists{};        // +48 "objective_list%d" (GuiTextListWidget)
        std::int32_t tab = 0;                  // +58
        std::int32_t sel_id = -1;              // +5c the selected objective's id
        std::array<Widget*, 2> groups{};       // +60 "current_parameters_grp" / +64 "complete_failed_grp"
        std::array<Widget*, 2> tabs{};         // +68 "01" / +6c "02" (GuiButtonWidget)
        Widget* value_time = nullptr;          // +70
        Widget* label_time = nullptr;          // +74
        std::int32_t y78 = 0, h7c = 0;         // +78 / +7c objective_list3's top and height
        Widget* failed_label = nullptr;        // +80 "label_failed_obj"
        std::int32_t off84 = 0;                // +84 its offset from +78
        std::uint8_t b88 = 1, b89 = 1;         // +88 / +89 (00368f00 passes 1, 1)
        // GuiStartMenu
        Widget* paused = nullptr;              // +8c "label_paused"
        // +90 the children of "ingame_button_grp" (7 in the original; the 8th is the
        // port-only Cheats button, checkpoint 132f5; [8] the port-only Port Settings button "09", checkpoint 132win11).
        std::array<Widget*, 9> buttons{};
        Widget* button_grp = nullptr;          // +ac
        std::int32_t option = 0;               // +b0 the button whose StartMenuOpt%d is shown
    };
    std::unique_ptr<Objectives> ob;
    // GuiSliderWidget (ctor 0043f910 over 0041f690).
    struct Slider {
        std::int32_t state = 1;                // +48 (0 focused, 1 not)
        std::array<std::uint8_t, 2> has{};     // +4c / +4d the "onActivate" / "onDeactivate" record exists
        std::array<std::int32_t, 2> a{};       // +50 / +54 its first frame
        std::array<std::int32_t, 2> b{};       // +58 / +5c its last frame
        std::uint8_t orient = 0;               // +60 (1: up / down)
        std::int32_t v = 0, min = 0, max = 100, step = 10;  // +64 / +68 / +6c / +70
    };
    std::unique_ptr<Slider> sl;
    // GuiGameOptions (ctor 0029ba10; +48 the GuiSaveAgentWidget popup id).
    struct GameOpt {
        SaveWidgetFields save;                 // +48..+77 (go_close 0029ad10 sets mode 4, the options save)
        std::uint8_t b78 = 0;                  // +78
        std::int32_t t7c = 0;                  // +7c
        Widget* square = nullptr;              // +80 "buttonsquare"
        Widget* buttonx = nullptr;             // +84 "buttonx"
        Widget* to_map = nullptr;              // +88 "label_to_map" (GuiTextWidget)
        std::array<Widget*, 4> groups{};       // +8c control_grp / volume_grp / video_grp / kick_grp
        std::array<Widget*, 4> buttons{};      // +9c 0control / 1volume / 2video / 3kick (GuiButtonWidget)
        Widget* main_grp = nullptr;            // +ac "optionmain_grp"
        Widget* header = nullptr;              // +b0 "label_controlsettings" (GuiTextWidget)
        std::int32_t sel = 0;                  // +b4
        GameOptionsRecord rec;                 // +b8
        std::uint8_t in_game = 0;              // +400 (the game canvas)
        std::uint8_t b401 = 0;                 // +401
        Widget* manual_aim = nullptr;          // +408 "value_manualaim"
        Widget* rumble = nullptr;              // +40c "value_rumble"
        Widget* freelook_x = nullptr;          // +410 "value_freelookx"
        Widget* freelook_y = nullptr;          // +414 "value_freelooky"
        Widget* sfx = nullptr;                 // +418 "slider_sfx" (GuiSliderWidget)
        Widget* voices = nullptr;              // +41c "slider_voices"
        Widget* music = nullptr;               // +420 "slider_music"
        Widget* mode_btn = nullptr;            // +424 "value_mode_btn"
        Widget* gamma = nullptr;               // +428 "slider_gamma"
        Widget* video_mode = nullptr;          // +42c "value_videomode"
        Widget* players = nullptr;             // +430 "player_list" (GuiTextListWidget)
        Widget* config_select = nullptr;       // +434 "select_config_select"
        std::array<std::optional<std::string>, 2> audio_mode;  // +438 AudioMode0 / 1
        std::array<std::optional<std::string>, 2> video_text;  // +440 VideoMode0 / 1
        std::array<std::optional<std::string>, 4> headers;     // +448 OptionsHeader0..3
        std::optional<std::string> invert, normal, mb_on, mb_off;  // +458 / +45c / +460 / +464
        // PORT-ONLY (not in the original): the cheat entries a cheat host adds to optionmain_grp.
        Widget* port_all = nullptr;
        Widget* port_weapons = nullptr;
    };
    std::unique_ptr<GameOpt> go;
    // GuiScrollBarWidget (ctor 0043bef0 over 0041f690). The position is
    // shown by broadcasting a frame of the "onWake" record to the children.
    struct ScrollBar {
        std::uint8_t has = 0;                  // +48 the "onWake" record exists
        std::int32_t a = 0, b = 0;             // +4c / +50 its first / last frame
        std::uint8_t b54 = 0;                  // +54 (0043bc70's byte)
        std::int32_t v = 0, min = 0, max = 10;  // +58 / +5c / +60
    };
    std::unique_ptr<ScrollBar> sb;
    // GuiMLTextWidget (ctor 0043e5d0 over 0041f690).
    struct MLText {
        std::optional<TextObject> obj;         // +48 (003d0940 at init 0043e330)
        std::uint32_t font = 0;                // +4c
        std::array<float, 4> colour{1.0f, 1.0f, 1.0f, 1.0f};  // +50..+5c (004c0100)
        std::int32_t max_lines = 0;            // +60 (0: no trim)
        std::int32_t per_page = 0;             // +64
        std::int32_t top = 0;                  // +68 first drawn line
        std::int32_t code = 0;                 // +6c colour code carried to the next line
        // +70 list (count +70): each node's +8 is a 0036b180 copy of the line
        // (no terminator here; the bytes include the 0x10 + code prefix).
        std::vector<std::string> lines;
        Widget* scroll = nullptr;              // +7c GuiScrollBarWidget
    };
    std::unique_ptr<MLText> ml;
    Widget* briefing_text = nullptr;           // GuiBriefingPane +48 "value_briefing" (GuiMLTextWidget)
    // GuiRadioLog (ctor 00457988 over 0041f690).
    struct RadioLog {
        Widget* list = nullptr;                // +48 "radio_log_list" (GuiTextListWidget)
        Widget* sub = nullptr;                 // +4c "sub_titles" (GuiSubTitleDisplay)
        // +50 the sound record (001ca010's fields that the messages copy):
        // the ctor's type -1 and empty name until the first replay.
        std::int32_t type = -1;
        std::string name;
    };
    std::unique_ptr<RadioLog> rl;
    // GuiSubTitleDisplay (ctor 00458590 over 0041f690).
    struct SubTitle {
        // One run of the parsed text (a 0x40-byte list node, fields +10..+3c).
        struct Segment {
            float delay = 0.0f;                // +10 the pause before it types
            std::array<float, 4> rgba{};       // +20..+2c
            std::int32_t x = 0, y = 0;         // +30 / +34 offset from the widget's corner
            std::int32_t off = 0, count = 0;   // +38 / +3c the bytes of +50 it shows
        };
        std::optional<TextObject> obj;         // +48 (003d0940 at wake)
        std::uint32_t font = 0;                // +4c "callout"
        std::optional<std::string> str;        // +50 (0036b180 copy; nullopt = NULL)
        float time = 0.0f;                     // +54 seconds since the text was set
        std::vector<std::vector<Segment>> lines;  // +58 vector of segment lists
        std::vector<float> times;              // +68 each line's end time (00458900)
        std::int32_t vis = 0;                  // +78 lines that fit
    };
    std::unique_ptr<SubTitle> st;
    // GuiSlideShow (ctor 00328660 over 0041f690).
    struct SlideShow {
        // One 0x2c-byte slide (the vector at category +c, count +10).
        struct Slide {
            std::string image;                 // +0 the TDX member
            std::uint32_t archive = 0;         // +c the HOG (*(*004ffc04 + 25c); 0 none)
            std::string voice;                 // +10 the quip ("" none)
            std::uint8_t unlocked = 0;         // +1c
            std::int32_t level = 0, cat = 0, idx = 0;  // +20 / +24 / +28 the agent's ZEUS entry
        };
        // One 0x20-byte category (the vector at +48, count +4c).
        struct Category {
            std::string name;                  // +0
            std::vector<Slide> slides;         // +c (count +10, data +14)
            std::int32_t current = 0;          // +1c
        };
        std::vector<Category> cats;
        std::int32_t cat = -1;                 // +58
        Widget* bitmap = nullptr;              // +5c "bitmap_frame" (GuiBitmapWidget)
        Widget* title = nullptr;               // +60 "menutitle" (GuiTextWidget)
        Widget* value = nullptr;               // +64 "value_slot"
        Widget* label_new = nullptr;           // +68 "label_new"
        std::string ret_class;                 // +6c the screen Triangle returns to (00418a80 class)
        std::string ret_file;                  // +78 its file
        std::int32_t ret_arg = 0;              // +84 the 0x3000 event's argument
        // +90 the voice record (001ca010's fields the messages copy): the
        // ctor's type -1 and empty name until the first voiced slide.
        std::int32_t type = -1;
        std::string name;
        std::uint8_t bd4 = 0;                  // +d4
    };
    std::unique_ptr<SlideShow> ss;
    // GuiBitmapWidget (ctor 0043cb20 over 0041f690).
    struct Bitmap {
        std::int32_t texture = -1;             // +48 the texture manager slot (-1 none)
        std::uint32_t archive = 0;             // +4c the HOG the name is read from
        std::string name;                      // +50 the upper-cased member name
        std::int32_t width = 0, height = 0;    // the loaded texture's size (its slot record)
        std::array<float, 4> rgba{1.0f, 1.0f, 1.0f, 1.0f};  // +60..+6c
        float z = 1.0f;                        // +70
    };
    std::unique_ptr<Bitmap> bm;
    // GuiTextArrayWidget (ctor 00426cc0 over the GuiArrayWidget ctor 00424c50,
    // whose grid is `list`: mode 0, one column and one row).
    struct TextArray {
        std::optional<TextObject> obj;         // +90 (003d0940 at wake)
        std::uint32_t font = 0;                // +94
        std::uint8_t halign = 2;               // +98
        std::array<float, 4> rgba{1.0f, 1.0f, 1.0f, 1.0f};  // +a0..+ac
        // One cell (0x30 bytes, default 00426600): the flags pick which of
        // halign (1), font (2) and colour (4) override the widget's.
        struct Cell {
            std::string text;                  // +0 string object
            std::uint32_t flags = 0;           // +c
            std::uint8_t halign = 0;           // +10
            std::uint32_t font = 0;            // +14
            std::array<float, 4> rgba{1.0f, 1.0f, 1.0f, 1.0f};  // +20..+2c
        };
        std::vector<std::vector<Cell>> rows;   // +b0 vector (count +b4, data +b8) of rows of cells
    };
    std::unique_ptr<TextArray> ta;
    // GuiWeaponDisplay (ctor 004412f0 over 0041f690).
    struct WeaponDisplay {
        std::int32_t item = -1;                // +48
        std::int32_t model = 0;                // +4c (record +13c, or +11c for hold class 7)
        float t = 0.0f;                        // +50 the turn clock (mod 120)
        float fade = 0.0f;                     // +54 (1.0: not drawn; 0045d960 writes the weapon select's)
        std::array<float, 16> rot{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // +60..+9c
        Widget* name = nullptr;                // +a0 *004976c8 "value_weaponname" (GuiTextWidget)
        std::uint8_t a4 = 1;                   // +a4
    };
    std::unique_ptr<WeaponDisplay> wd;
    // GuiEquipmentPane (vtable 004e12f0, over 0041f690).
    struct EquipPane {
        Widget* display = nullptr;             // +48 *00497998 "weapon_placement" (GuiWeaponDisplay)
        Widget* ml = nullptr;                  // +4c "weapon_desc" (GuiMLTextWidget)
        Widget* label = nullptr;               // +50 "label_weaponname" (GuiTextWidget)
        Widget* stats = nullptr;               // +54 "value_weaponstats" (GuiWeaponStats)
    };
    std::unique_ptr<EquipPane> ep;
    // GuiCharacterDisplay (ctor 00326860 over 0041f690): +48 the draw callback
    // its draw 00326810 calls after 0041e470. The writers are GuiScoresEquip's
    // init (0035df30 -> 0035b390(*00533760)) and GuiEquipmentModify's init
    // (002c2340 the figure, 002c2290 / 002c21c0 the weapon boxes): the screen
    // it draws for. GuiAgentModify's init (00345020 through D_005331d0, 132cc4b)
    // is the third: the port draws its PORT-ONLY preview texture (`bm`) instead.
    Widget* cd_screen = nullptr;
    std::uint32_t cd_callback = 0;             // the +48 callback's address (which of the four it is)
    // GuiScoresEquip (vtable 004df140, ctor 0035d430 over 0041f690).
    struct ScoresEquip {
        std::array<Widget*, 4> net{};          // +48..+54 "networkconnection%d" (init, when +60)
        std::int32_t wake_58 = 0, wake_5c = 0;  // +58 / +5c the "onWake" record's +10 / +14
        std::uint8_t b60 = 1;                  // +60 (ctor: 1)
        // +64 + 0x50 slot + 4 i: the text fields of 00492d10's 20 names
        // ('1'..'4' + "value_agentname" ...), isA GuiTextWidget.
        std::array<std::array<Widget*, 20>, 4> fields{};
        // The four display actors +1a4 (00393ff0 each): the state 0035bfb0
        // writes into them. 003927a0 copies the player's render model (p+60,
        // with its attach table +2410) to +60; here the table's weapon items
        // (the models' +40a, -1 for an empty point).
        struct Figure {
            std::array<std::int32_t, 10> attach{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};  // +2470
            std::uint8_t b2c71 = 0;            // +2c71 (1 after the copy)
            std::int8_t b2c3a = 0;             // +2c3a (-1: no player for this index)
        };
        std::array<Figure, 4> figures{};
        std::array<float, 4> rot_1c0{0.0f, 0.0f, 3.14159274f, 0.0f};  // +1c0: x, y, z (+1c8 the turn)
        std::array<float, 4> cam_1d0{0.0f, 0.0f, 3.14159274f, 0.0f};  // +1d0 (w not written)
        // +1e0 + 0x10 s: the figures' translations (slot 0: (-3.2, 8, -2) when
        // 005721c8 is set).
        std::array<std::array<float, 4>, 4> pos_1e0{{{3.2f, 8.0f, -1.7f, 1.0f},
                                                    {-0.4f, 8.0f, -2.0f, 1.0f},
                                                    {2.4f, 8.0f, -2.0f, 1.0f},
                                                    {5.2f, 8.0f, -2.0f, 1.0f}}};
        std::array<std::uint8_t, 4> shown_220{};  // +220 + slot (1 once a player was copied)
        std::int32_t scores_224 = 0;           // +224: 0 the scores group, 1 the equipment group
        float turn_228 = 0.0f;                 // +228: L2 / R2 (+1 / -1 while held)
        std::uint8_t b22c = 1;                 // +22c
        std::array<Widget*, 4> grp_230{};      // +230 + 8 i: 'i'"player_stats_grp"
        std::array<Widget*, 4> grp_234{};      // +234 + 8 i: 'i'"player_stats_grpe"
    };
    std::unique_ptr<ScoresEquip> se;
    // Port-only (checkpoint 132f5): the Cheats screen's value buttons (one per
    // cheat, "pc_value%d") and the MB_On / MB_Off strings they show.
    struct PortCheats {
        std::vector<Widget*> values;
        std::optional<std::string> on, off;
    };
    std::unique_ptr<PortCheats> pc;
    // Port-only (checkpoint 132win11): the Port Settings screen's row slots ("ps_label%d" / "ps_value%d" /
    // "ps_background%d", PortSettingsScreen::rows of each), its title and the model revision last shown.
    struct PortSettingsRows {
        std::vector<Widget*> labels, values, backgrounds;
        Widget* title = nullptr;
        std::uint64_t revision = 0;
        bool shown = false;
        std::string return_focus;  // the front-end options row that opened the screen
    };
    std::unique_ptr<PortSettingsRows> port_settings_rows;
    // GuiEquipmentModify (ctor 002c1100 over 0041f690; *0051ee10 the screen).
    // The preview +4c is a copy of the player's render model; the port keeps
    // its attach table +2470 and the player's +48 +60 +2410 as the weapon
    // records of the attached models (+40a; -1 for an empty point). A model is
    // the record's +13c, so a record whose +13c is 0 (not preloaded) stores
    // an empty point, as the original's 0 handle.
    struct EquipModify {
        bool built = false;                    // the ctor's player copy ran (first init)
        std::int32_t cat_84 = -1;              // +84: 1 back .. 5 melee, 6 equip, 0 reset
        std::int32_t point_88 = 2;             // +88: the attach point of the category
        std::uint8_t list_8c = 0;              // +8c: the list has the focus
        std::uint8_t b90 = 0;                  // +90
        Widget* list = nullptr;                // +94 "weaponlist" (textlist)
        std::int32_t x_98 = 0, y_9c = 0;       // +98 / +9c the list's rect x / y
        std::int32_t a0 = 0;                   // +a0 the category button's rect y
        std::int32_t row_a4 = 0;               // +a4 the list's row height
        std::int32_t rows_a8 = 0;              // +a8 the rows the list shows
        Widget* highlight = nullptr;           // +cc "select_weapon_highlight"
        Widget* box1 = nullptr;                // +d0 "1weaponbox"
        Widget* box2 = nullptr;                // +d4 "2weaponbox"
        std::vector<std::int32_t> initial;     // +dc / +f0: the player's items at the start
        std::int32_t fc = 0;                   // +fc: the point the held weapon goes back to
        std::int32_t fc_100 = 0;               // +100: +fc at the start
        std::vector<std::int32_t> items;       // +104 count, +10c the list's items
        std::int32_t sel_108 = 0;              // +108 the list's selection
        std::array<std::int16_t, 10> sel_90c{};  // +90c the selection per attach point
        std::vector<std::int32_t> ring;        // +920 count, +924: the preloaded items (13)
        std::array<std::int32_t, 10> preview{};  // +4c +2470 (records, -1 empty)
        std::array<std::int32_t, 10> player{};   // +48 +60 +2410 (records, -1 empty)
        Widget* waiting = nullptr;             // +958 "waitingforhost"
        std::string countdown;                 // +95c "EQS_COUNTDOWN"
        // Checkpoint 132f6: the figure's turn. +60 = (0, 0, pi, 0) from the ctor (+68 the yaw 002bfa90
        // eases) and the per-category targets +ac + 4 * +84 (002c1d8c..002c1dcc): 0 reset pi, 1 back 2 pi,
        // 2 sidearm 3 pi / 2, 3..6 pi. +84 = -1 reads +a8 (rows_a8's bits as a float).
        float yaw_68 = 3.14159274F;
        std::array<float, 7> turn_ac{3.14159274F, 6.28318548F, 4.71238899F, 3.14159274F, 3.14159274F, 3.14159274F,
                                     3.14159274F};
    };
    std::unique_ptr<EquipModify> em;
    // GuiAgentModify (ctor 00343e10 over the GuiSaveAgentWidget 00441d00).
    struct AgentMod {
        SaveWidgetFields save;                 // +48..+77 (Square: mode 1, the agent save)
        Widget* indicator = nullptr;           // +78 "selectindicator"
        Widget* value_slot = nullptr;          // +7c "value_slot" (GuiTextWidget)
        Widget* preview = nullptr;             // +80 "bitmap_preview" (GuiBitmapWidget)
        Widget* value_name = nullptr;          // +84 "value_name" (GuiTextWidget)
        Widget* beard = nullptr;               // +88 "beard" (GuiButtonWidget)
        std::int32_t group = 0;                // +ec the open group: 0 tattoos, 1 patches, 2 body, 3 head
        std::int32_t in_group = 0;             // +f0 a group button (head / body / tattoos / patches) has focus
        std::array<std::int32_t, 6> pairs{};   // +114..+128 TATT1..3, PATC1..3 (004068b0)
        // +12c..+150: leftarm, rightarm, back, leftarm2, rightarm2, hat2, body, head, tattoos, patches
        std::array<Widget*, 10> buttons{};
        Widget* group_w = nullptr;             // +154 "createagent_grp"
        omega::AgentModify m;                  // the editing state (+e4..+110, +158; agent_modify.hpp)
        // The GuiCharacterDisplay "playercharacter" (D_005331d0's widget) whose callback draws the model.
        Widget* display = nullptr;
        float anim_time = 0.0f;                // the ANIMIDLE clock (seconds; 003ab510's dt steps)
        float pose_time = 0.0f;                // PORT-ONLY: that clock at the 30 Hz the draw poses at
    };
    std::unique_ptr<AgentMod> am;
    // GuiAgentCreate (ctor 0028f760 over 0041f690; agent_create.hpp the editing state).
    struct AgentCreateW {
        Widget* group_w = nullptr;             // +48 "createagent_grp"
        Widget* body = nullptr;                // +4c "body" (GuiButtonWidget)
        Widget* head = nullptr;                // +50 "head" (GuiButtonWidget)
        Widget* beard = nullptr;               // +54 "beard" (GuiButtonWidget)
        Widget* indicator = nullptr;           // +58 "selectindicator"
        Widget* value_slot = nullptr;          // +5c "value_slot" (GuiTextWidget)
        omega::AgentCreate m;                  // +60..+f0
        std::int32_t group = -1;               // +cc 0 gender, 1 randomize, 2 body, 3 head, 4 agentname
        std::int32_t in_group = 1;             // +d0 a group button has focus
        // The GuiCharacterDisplay "playercharacter" (D_00506390's widget) whose callback draws the
        // model; the ANIMIDLE clock and (PORT-ONLY) its 30 Hz pose time.
        Widget* display = nullptr;
        float anim_time = 0.0f;
        float pose_time = 0.0f;
    };
    std::unique_ptr<AgentCreateW> ac;
    // GuiAgentSave (ctor 00293fc0 over the GuiSaveAgentWidget; +78 / +7c are al_timer / al_7c).
    struct AgentSave {
        SaveWidgetFields save;                 // +48..+77 (a slot's activation: mode 0, the create)
    };
    std::unique_ptr<AgentSave> as;
    // The debrief screens' voice queue (checkpoint 132f11): GuiEndGameObjectives +90,
    // GuiEndGameStatistics +260 and GuiEndGameAwards +350 (ctor 00460d40 / 00462fe0,
    // push 00460ac0, fire 004610f0, the update head of 00462270 / 004637d0 / 00461020).
    struct VoiceQueue {
        struct Entry {
            std::string name;                  // +0 char[8] (00129a70(entry, name, 8))
            std::int32_t delay = 0;            // +8 cvt.w(30 * f)
            // 004624e0 pushes its stack buffer uninitialised when statistic 0x19 is 0;
            // the port plays nothing for such an entry (port deviation, see LEVEL_END.md).
            bool garbage = false;
        };
        std::vector<Entry> entries;            // +4 count, +8 the 0xc-byte entries
        float timer = 0.0f;                    // +10
        std::int32_t type = -1;                // +20 the sound record (001ca010's fields; ctor type -1)
        std::string sound;
        std::int32_t cursor = 0;               // +1b0 the next entry
    };
    std::unique_ptr<VoiceQueue> vq;
    // GuiEndGameStatistics (vtable 004e1840 over GuiScoresEquip, se->b60 = 0).
    struct EndStats {
        Widget* back = nullptr;                // +250 the "ENDBACK" backdrop (00418a80 at wake)
        std::array<std::uint8_t, 3> b254{};    // +254..+256 (zeroed at wake)
    };
    std::unique_ptr<EndStats> es;
    // GuiEndGameAwards (vtable 004e1730, built at 00460d04 over 0041f690).
    struct Awards {
        struct Entry {                         // +48 + 0x18 i
            Widget* bitmap = nullptr;          // +0 "award_grp"'s child i (isA GuiBitmapWidget)
            std::optional<std::string> text;   // +4 the award's caption (nullopt = NULL)
            std::string sound;                 // +8 (always "" here: 0045f5c4 throws otherwise)
        };
        std::array<Entry, 15> entries{};
        Widget* border = nullptr;              // +1b0 "border"
        std::int32_t count = 0;                // +1b4 the awards built (0045fab0)
        std::int32_t cur = -1;                 // +1b8 the award shown
        std::uint8_t b204 = 0;                 // +204 (cleared at wake)
    };
    std::unique_ptr<Awards> aw;
    // GuiAutoSaver (vtable 004e16d0 over the GuiSaveAgentWidget; popup -2).
    struct AutoSaver {
        SaveWidgetFields save;                 // +48..+77 (wake: mode 2, the agent save)
    };
    std::unique_ptr<AutoSaver> asv;
    // One 0x5c-byte row of GuiMissionStatistics' level table (the vector at +70; 0034f670 builds 17
    // zeroed rows). Each field is a 0xc-byte {byte, word, word} (0034f920).
    struct MissionStatRow {
        struct Field {
            std::uint8_t ok = 0;               // +0 (cell colour: cc_list_rgba when set)
            std::int32_t a = 0;                // +4
            std::int32_t b = 0;                // +8
        };
        std::string name;                      // +0 string object (SHORT_LEVEL%d, or "Classified")
        std::uint8_t played = 0;               // +c 00332fe0
        std::array<Field, 5> f{};              // +10 / +1c / +28 / +34 / +40
    };
    // One 0x14-byte row of 003334c0 (the achievement files) / 00333600 (GuiRatings): +0 the text
    // 003fdb80(key), +4 the key, +8 the flag, +c the unlock, +10 the rating's percentage.
    struct AchieveRow {
        std::optional<std::string> text;       // +0
        std::int32_t key = -1;                 // +4
        std::uint8_t flag = 0;                 // +8
        std::int32_t unlock = -1;              // +c
        std::int32_t percent = 0;              // +10
    };
    // GuiPersonnelScreen (vtable 004df0b0, ctor 00356ff0) and its subclasses GuiMissionStatistics
    // (vtable 004dee70, ctor 0034fcb0), GuiDossiers (004dec30), GuiRatings (004decc0, ctor 0034df40),
    // GuiRanks (004ded50), GuiOmegaStrain (004dede0), GuiAchievementScreen (004def90) and
    // GuiPersonnelMenu (004df020, ctor 00356730). The subclasses' fields from +6c overlap; each
    // class uses its own group below.
    struct Personnel {
        Widget* item_list = nullptr;           // +48 "item_list" isA GuiTextListWidget
        Widget* item_cursor = nullptr;         // +4c "item_list_cursor"
        Widget* thumbnail = nullptr;           // +50 "thumbnail" isA GuiBitmapWidget
        Widget* earned_bitmap = nullptr;       // +54 "earned_item_bitmap" isA GuiBitmapWidget
        Widget* unlocked_name = nullptr;       // +58 "value_unlocked_name" isA GuiTextWidget
        Widget* earned_item = nullptr;         // +5c "earned_item" isA GuiWeaponDisplay
        std::int32_t sel = 0;                  // +60 (event 0x3000)
        std::int32_t kind_img = 0;             // +64 the thumbnails' "0_%d_%ds.tdx" first number
        std::int32_t offset = 1;               // +68 added to the item id for the thumbnail
        // GuiMissionStatistics
        Widget* list = nullptr;                // +6c "config_list" isA GuiMissionStatList
        std::vector<MissionStatRow> rows;      // +70 (count +74, data +78)
        std::uint8_t b7c = 1;                  // +7c
        std::int32_t row = 0;                  // +80 (event 0x3000; the row selected at wake)
        // GuiAchievementScreen: rows +6c (count +70, data +74), mode +7c, the sub-menu file +80.
        std::vector<AchieveRow> ach;
        std::int8_t mode = 0;
        std::string sub_file;
        // GuiRatings: the 24 grid bitmaps +6c..+c8, rows +d0 (data +d4), the percentage text +dc.
        std::array<Widget*, 24> grid{};
        std::optional<TextObject> pct_text;
        // GuiPersonnelMenu: return class +6c / file +78, kind +84, row +88, image numbers +8c / +90,
        // the selected row has a detail +94, "bitmap_frame" +98, "group1_submenu" +9c,
        // "group2_bitmap" +a0, "buttonx" +a4, the voice record +b0 (001ca010's type / name), +f4,
        // and the detail shown +240.
        std::string ret_class, ret_file;
        std::int8_t pm_kind = 0;
        std::int32_t pm_row = -1;
        std::int32_t pm_a = 0, pm_b = 0;
        Widget* frame = nullptr;
        Widget* group1 = nullptr;
        Widget* group2 = nullptr;
        Widget* buttonx = nullptr;
        std::uint8_t detail = 0;
        std::uint8_t has_detail = 0;
        std::uint8_t f4 = 0;
        std::int32_t voice_type = -1;
        std::string voice_name;
    };
    std::unique_ptr<Personnel> ps;
    // GuiMissionStatList (ctor 00350460): the rows it shows and its column widths.
    struct MissionStatList {
        const std::vector<MissionStatRow>* rows = nullptr;  // +c0
        std::array<std::int32_t, 11> colw{0x6e, 0x2d, 0x2d, 0xf, 0x32, 0x32, 0x14, 0x50, 0x1e, 0x50, 0};  // +c4..+ec
    };
    std::unique_ptr<MissionStatList> msl;
    // GuiAgentDisplay (vtable 004df530, ctor 00364590 over 0041f690): the agent-info screen's turning
    // figure. Its actor (+48: init 003643a0's 00393ff0, rebuilt by wake 00362dc0 from +a0) is the
    // host's agent preview (frontend_disc agent_figure_draw).
    struct AgentDisplay {
        omega::AgentCamera camera;             // +50 name, +60 value, +64 half fov, +68 loc, +74 rot (+7c the turn)
        std::array<float, 4> turns{};          // +80..+8c: (0, 0, pi, 0) from the ctor
        omega::AgentPacking packing{};         // +a0 (0x440 bytes; 003609f0 copies the agent's appearance)
        float rate = 0.0f;                     // +4e0: the turn rate (L2 / R2, 003617c0)
        float anim_time = 0.0f;                // the ANIMIDLE clock (seconds; the draw's 0040f3e0 dt steps)
        float pose_time = 0.0f;                // PORT-ONLY: that clock at the 30 Hz the draw poses at
    };
    std::unique_ptr<AgentDisplay> ad;
    // GuiAgentInfo (vtable 004df350, ctor 00361d90 over the net screen 0033d580): the dossier's own
    // agent page (AGNTINFO.GUI), filled by 003609f0 and shown by 00361000.
    struct AgentInfo {
        Widget* start_button = nullptr;        // the net screen's "start_button"
        std::int32_t net78 = 0;                // +78 the network manager (null offline)
        std::int32_t b84 = 8;                  // +84
        std::array<std::int32_t, 12> words{};  // +88..+b4 the statistics column
        std::string name;                      // +b8 string object (00336170)
        std::string cell;                      // +c4 string object (the agent's +8bc)
        std::int32_t rank = 0;                 // +d0 the rank row's index
        std::array<std::uint8_t, 24> ratings{};  // +d4 the 24 ratings' flags
        std::array<std::uint8_t, 8> specials{};  // +ec the special ratings' flags
        std::uint8_t leader = 0;               // +f4 mode 1: the own agent leads an agency cell (0044a1d0)
        std::int32_t target = 0;               // +f8 mode 1: the agent's account id (-1 the own agent's)
        Widget* lobby = nullptr;               // +10 mode 1: the lobby screen that opened it (0033e410)
        std::int32_t mode = 0;                 // +fc 0 the dossier's, 1 online (another player's)
        Widget* value_agent = nullptr;         // +100 GuiTextWidget
        Widget* value_rank = nullptr;          // +104 GuiTextWidget
        Widget* value_cell = nullptr;          // +108 GuiTextWidget
        Widget* ratings_list = nullptr;        // +10c GuiTextArrayWidget
        Widget* statistics = nullptr;          // +110 GuiTextArrayWidget
        Widget* button01 = nullptr;            // +114 "01" GuiButtonWidget
        Widget* button02 = nullptr;            // +118 "02" GuiButtonWidget
        Widget* thumbnail = nullptr;           // +11c GuiBitmapWidget
        Widget* display = nullptr;             // +120 "playercharacter" GuiAgentDisplay
    };
    std::unique_ptr<AgentInfo> ai;
    // GuiOnlineConnect (vtable 004dd620, ctor 0029f2a0 over the net screen 0033d580; checkpoint 132ln2). The
    // lists at +98 / +d0 (0044fa00) are filled by the configuration list 0044c440 (checkpoint 132on2, a session
    // only); +b4 is never read by the screen and is not modelled. The sleep clears them (0044f6b0).
    struct OnlineConnect {
        std::vector<LobbyConfig> configs;    // +98 (count +9c)
        std::vector<LobbyServer> servers;    // +d0 (count +d4)
        SaveWidgetFields save;                 // +48..+77 (the net screen's GuiSaveAgentWidget; +48 the wait popup)
        std::uint8_t b78 = 0;                  // +78 (0033d580; read by the net screen input 0033cef0)
        Widget* start_button = nullptr;        // +7c "start_button" (0033d3f0)
        std::int32_t w84 = 0;                  // +84 (the navigator's op 1 skips its text when 9)
        std::int32_t state = 0;                // +88
        float t8c = 0.0f;                      // +8c (15.0 from wake)
        std::int32_t config = 0;               // +90 the chosen configuration row
        std::uint8_t connected = 0;            // +94 (0044c440 succeeded)
        std::uint8_t dnas_done = 0;            // +95 (event 0x2002)
        Widget* config_list = nullptr;         // +ec "config_list" (GuiTextListWidget)
        Widget* policy_title = nullptr;        // +f0 (GuiTextWidget)
        Widget* policy_text = nullptr;         // +f4 (GuiMLTextWidget)
        Widget* policy_accept = nullptr;       // +f8
        Widget* policy_decline = nullptr;      // +fc
        Widget* announce_text = nullptr;       // +100 (GuiMLTextWidget)
        Widget* announce_ok = nullptr;         // +104
        Widget* dnas = nullptr;                // +108 the DNASLOGO screen (factory "GuiWidget" / "DNASLOGO")
        std::array<Widget*, 3> groups{};       // +10c config_grp, +110 policy_grp, +114 announce_grp
        std::int32_t shown = 4;                // +118 the group 0029cbb0 showed (4: none)
        std::int32_t popup_11c = -2;           // +11c
        std::int32_t popup_120 = -2;           // +120
    };
    std::unique_ptr<OnlineConnect> oc;
    // GuiMissionSetup (vtable 004dd730, ctor 002a19c0 over the net screen 0033d580; checkpoint 132ln3). The
    // lists +88 / +a4 / +c0 (0044fa00) are filled only by the lobby's room join 004518f0 (a session call), so
    // they stay empty and are not modelled. +f0 is the app and +f4 the app's +140 block, whose +40 (app +180,
    // the agent count) and +45 (app +185, the join filter) the screen edits (GuiRuntime::Globals app_180 /
    // app_185).
    struct MissionSetup {
        SaveWidgetFields save;                 // +48..+77 (the net screen's GuiSaveAgentWidget)
        std::uint8_t b78 = 0;                  // +78 (0033d580; read by the net screen input 0033cef0)
        Widget* start_button = nullptr;        // +7c "start_button" (0033d3f0)
        std::uint8_t active = 0;               // +80 (activate 002a1780 / deactivate 002a1700)
        std::int32_t w84 = 6;                  // +84 (ctor: 6)
        Widget* num_agents = nullptr;          // +dc "num_agents"
        Widget* join_filter = nullptr;         // +e0 "join_filter"
        Widget* begin = nullptr;               // +e4 "begin_mission"
        Widget* num_agents_txt = nullptr;      // +e8 "num_agents_txt" (GuiTextWidget)
        Widget* join_filter_txt = nullptr;     // +ec "join_filter_txt" (GuiTextWidget)
        std::uint8_t room = 0;                 // +f8 (004518f0 joined the lobby's room; session only)
    };
    std::unique_ptr<MissionSetup> ms;
    // Checkpoint 132on2b: GuiAccountLogin (vtable 004dd120, ctor 0028b2c0 over the net screen 0033d580;
    // ACNTLOGN.GUI, the account login and, with +ad set, the sign-up: ACNTNEW.GUI is never loaded).
    struct AccountLogin {
        SaveWidgetFields save;                 // +48..+77 (the net screen's GuiSaveAgentWidget; +4c the save mode)
        std::uint8_t b78 = 0;                  // +78 (0033d580; read by the net screen input 0033cef0)
        Widget* start_button = nullptr;        // +7c "start_button" (0033d3f0)
        std::uint8_t active = 0;               // +80 (activate 0028a470 / deactivate 0028a430)
        std::int32_t w84 = 7;                  // +84 (0028b2c0: 7)
        Widget* username = nullptr;            // +88 "username" (0048b400)
        Widget* username_txt = nullptr;        // +8c "username_txt" isA GuiTextEditWidget (+84 max 15)
        Widget* password = nullptr;            // +90 "password"
        Widget* password_txt = nullptr;        // +94 "password_txt" isA GuiTextEditWidget (+84 max 15)
        Widget* login = nullptr;               // +98 "login" isA GuiButtonWidget
        std::int32_t record = -1;              // +9c 001314f0(004ffb50, -1): the current profile
        Widget* menutitle = nullptr;           // +a0 "menutitle" isA GuiTextWidget
        std::uint8_t kb_ok = 0;                // +a4 (the username keyboard opens; else the popup LOBBY_STR 0x2a)
        std::int32_t wait = -1;                // +a8 the wait popup
        std::uint8_t saved = 0;                // +ac (the save that ends the sign-up: back to the login mode)
        std::uint8_t mode = 0;                 // +ad (0 login, 1 create account)
    };
    std::unique_ptr<AccountLogin> acl;
    // Checkpoint 132ln4: GuiLobbyScreen (vtable 004de8a0, ctor 0033f480 over the net screen 0033d580) with
    // GuiNetJoinMission (vtable 004dda50, ctor 002a9340; JOINMIS.GUI) over it.
    struct Lobby {
        SaveWidgetFields save;                 // +48..+77 (the net screen's GuiSaveAgentWidget)
        std::uint8_t b78 = 0;                  // +78 (wake 0033f110 sets it: START opens NETMESS, 0033cef0)
        Widget* start_button = nullptr;        // +7c "start_button" (0033d3f0)
        std::uint8_t active = 0;               // +80 (GuiNetJoinMission's activate 002a9030 / deactivate 002a8fd0)
        std::int32_t w84 = 2;                  // +84 (002a9340)
        Widget* list = nullptr;                // +88 "mission_list" (GuiMissionList)
        Widget* chat = nullptr;                // +8c "chat_display" (GuiChatWindow)
        std::int32_t mode = 0;                 // +90 (0033ef10: 0 chat, 1 info, 2 join, 3 remove)
        std::uint8_t b94 = 1;                  // +94
        Widget* join_game = nullptr;           // +98 (GuiButtonWidget)
        Widget* agent_chat = nullptr;          // +9c
        Widget* agent_info = nullptr;          // +a0
        Widget* agent_remove = nullptr;        // +a4
        LobbyList* source = nullptr;           // +a8: &+c8 or &+e4 (0033d8e0), or none
        LobbyList chat_ac, players_c8, games_e4;  // +ac / +c8 / +e4 (0044fa00; filled by the lobby, 004518f0)
        std::int32_t w100 = -1;                // +100
        std::string s104;                      // +104 string object
        LobbyRecord pending;                   // +110..+15c (0033e5b0: the entry a password prompt waits on)
        Widget* show_toggle = nullptr;         // +160 (GuiNetJoinMission, GuiButtonWidget)
        Widget* online_prefs = nullptr;        // +164
        Widget* host_game = nullptr;           // +168
        // Checkpoint 132on2h: GuiNetContacts (vtable 004ddc20, ctor 002adfc0; CONTACTS.GUI) keeps its two buttons at
        // +160 / +164 where GuiNetJoinMission keeps show_toggle / online_prefs (init 002adf00).
        Widget* invite_contact = nullptr;      // +160 "invite_contact" (GuiButtonWidget)
        Widget* remove_contact = nullptr;      // +164 "remove_contact" (not on the disc's CONTACTS.GUI: null)
        // Checkpoint 132on2i: GuiNetAgencyCell (vtable 004ddd40, ctor 002aff20, +84 3; AGENTCEL.GUI), init 002afdc0
        Widget* agency_message = nullptr;      // +160 "agency_message" (GuiTextWidget, 0041c0d0(t, 1))
        Widget* resign_cell = nullptr;         // +164 "resign_cell" (GuiButtonWidget)
        Widget* invite_agent = nullptr;        // +168 "invite_agent"
        Widget* set_message = nullptr;         // +16c "set_message"
        // PORT-ONLY (132on2b): the last game list (GuiHost::LobbyGameList::serial) 00447790 copied into +e4
        std::uint32_t port_list_serial = 0;
        // PORT-ONLY (132on2c): the last player list (GuiHost::LobbyPlayerList::serial) 00447a30 copied into +c8
        std::uint32_t port_player_serial = 0;
    };
    std::unique_ptr<Lobby> lb;
    // Checkpoint 132ln4: GuiMissionList (vtable 004ddb80, ctor 002acea0 over GuiArrayWidget 00424c50).
    struct MissionList {
        std::array<std::optional<std::string>, 6> header{};  // +90 MisListHeader%d (the column types' titles)
        std::optional<TextObject> obj;         // +a8 (wake 002abd50's 003d0940)
        std::uint8_t kind = 1;                 // +ac the column layout (table 0048bc60)
        std::uint8_t action = 1;               // +ad (0033ef10's mode: which rows 002ac470 selects)
        Widget* header_w = nullptr;            // +b0 "column_header" (GuiTextArrayWidget)
        std::uint8_t b4 = 0, b5 = 0;           // +b4 (the mission column) / +b5 (players' rows join)
        std::int32_t b8 = -1;                  // +b8 (the lobby screen's +84)
        std::uint8_t bc = 0;                   // +bc (session && 0044fa40: the lobby pages the game list)
        std::int32_t sel_id = -1, sel_index = -1, sel_flags = 0, sel_row = -1;  // +c0 / +c4 / +c8 / +cc
        std::array<std::string, 4> status{};   // +d0 AgentStatus%d ("" when missing)
        std::array<std::string, 5> ping{};     // +e0 PingResult%d
        std::array<std::string, 5> filter{};   // +f4 FilterType%d
        struct Row {
            std::int32_t id = 0;               // +0 the record's +0
            std::int32_t index = 0;            // +4 the record's index in the source
            std::int32_t flags = 0;            // +8 002ac470 (1 selectable, 2 listed, 4 no name)
        };
        std::vector<Row> rows;                 // +10c vector (+110 count, +114 data)
        LobbyList* source = nullptr;           // +11c (002acc80)
        std::int32_t own_id = 0;               // +120 (wake: session +2480 or -1; the ctor 002acea0 leaves it 0)
    };
    std::unique_ptr<MissionList> mls;
    // Checkpoint 132on2c: GuiOnlineOptions (vtable 004ddaf0, ctor 002aa110 over the net screen 0033d580;
    // ONLIPREF.GUI, the agent's universe / region choice).
    struct OnlineOptions {
        SaveWidgetFields save;                 // +48..+77 (the net screen's GuiSaveAgentWidget; +4c the save mode)
        std::uint8_t b78 = 0;                  // +78 (read by the net screen input 0033cef0)
        Widget* start_button = nullptr;        // +7c "start_button" (0033d3f0)
        std::uint8_t active = 0;               // +80 (activate 002a9ba0 / deactivate 002a9b50)
        std::int32_t w84 = 5;                  // +84 (002aa110: 5)
        Widget* label = nullptr;               // +88 "label_muis_server" isA GuiTextWidget
        Widget* value = nullptr;               // +8c "value_region" isA GuiTextWidget
        Widget* list = nullptr;                // +90 "config_list" isA GuiTextListWidget
        Widget* select = nullptr;              // +94 "select_config_select" (any class)
        std::int32_t current = -1;             // +98 the agent's universe's row (-1 none)
        std::int32_t picked = -1;              // +9c the list's universe row
        std::vector<LobbyUniverse> universes;  // +a0 / +a4 the SFOLobby universe list (0x44-byte rows)
    };
    std::unique_ptr<OnlineOptions> opt;
    // Checkpoint 132on2c: GuiNetMessages (vtable 004deb10, ctor 00347a40 over the net screen 0033d580; NETMESS.GUI,
    // the lobby's message box) and its list GuiNetMessageArray (vtable 004deb80, ctor 00348fc0 over GuiArrayWidget).
    struct NetMessages {
        SaveWidgetFields save;                 // +48..+77
        std::uint8_t b78 = 0;                  // +78 (the activate sets it: START is the net screen's, 0033cef0)
        Widget* start_button = nullptr;        // +7c "start_button" (0033d3f0)
        std::uint8_t active = 0;               // +80 (activate 003476b0 / deactivate 00347690)
        std::int32_t w84 = 9;                  // +84 (00347a40: 9)
        Widget* box = nullptr;                 // +88 "messages_box" isA GuiNetMessageArray
        Widget* button1 = nullptr;             // +8c "button1" isA GuiButtonWidget
        Widget* button2 = nullptr;             // +90 "button2"
        Widget* button3 = nullptr;             // +94 "button3"
        std::int32_t shown = 0;                // +98 the message the event 0x14 picked
        std::int32_t popup = -1;               // +9c (00347a40: -1; closed by the leave)
        Widget* info = nullptr;                // +a0 "messageinfo" isA GuiTextWidget
        Widget* nomessage = nullptr;           // +a4 "nomessage" (any class)
    };
    std::unique_ptr<NetMessages> nm;
    struct NetMsgArray {
        Widget* header = nullptr;              // +90 "column_header" isA GuiTextArrayWidget
        std::array<Widget*, 3> icons{};        // +94 / +98 / +9c "icon_notification" / "icon_chat" / "icon_question"
        std::optional<TextObject> obj;         // +a0 (wake 00348b90's 003d0940, font "callout", 00587c20)
        bool source = false;                   // +a4 the session's message list (session +1c) while awake
        std::string system;                    // +a8 GetString("TOSSystem") or 004ba338 (the ctor)
    };
    std::unique_ptr<NetMsgArray> nma;
};
using TextArrayCell = Widget::TextArray::Cell;

// The character code map at 0055ca90 (SFO_CHAR.MAP, loaded by 003fdf40
// through 0043fbb0): code points to font character indices.
struct CharMap {
    std::int32_t count = 0;                    // +0
    std::array<std::uint16_t, 0xde> codes{};   // +4
    std::array<std::uint8_t, 0x400> pages{};   // +1c0: four 256-byte pages
    std::array<std::int8_t, 256> page{};       // +5c0 page table as a page index (-1 = null)
    std::uint8_t ready = 0;                    // byte 0055d450
    CharMap() { page.fill(-1); }
    // 0043fbb0(this, path) on the file's bytes (nullopt: the file does not
    // open). Returns its result; the caller stores it in `ready`. Parsing
    // that would read past the end of the file throws (the bytes after it
    // are the read buffer's slack, not modelled).
    std::uint8_t load(const std::optional<std::vector<std::uint8_t>>& file);
    std::uint8_t lookup(std::int32_t code) const;  // 003fd6b0
};

// One entry of the application popup queue (app +2fc, 0x28-byte payload).
struct PopupEntry {
    std::int32_t id = 0;              // +0
    std::int32_t type = 0;            // +4
    std::string title;                // +8
    std::string message;              // +14
    Widget* target = nullptr;         // +20
    std::int32_t arg = 0;             // +24
};

// The services the runtime calls. Every method names its EE callee.
class GuiHost {
public:
    virtual ~GuiHost() = default;
    virtual std::optional<std::string> get_string(const std::string& key) = 0;  // 003fdb50 (nullopt = NULL)
    virtual std::uint32_t font(const std::string& name) = 0;                      // 00418420
    virtual std::int32_t ie_lookup(const std::string& name) = 0;                  // 00412f80
    virtual std::int32_t demo_mode() = 0;                                         // 001302f0
    virtual std::int32_t time_ms() = 0;                                           // 003698b0
    virtual std::uint8_t save_present() = 0;                                      // 00288510
    virtual void card_refresh() = 0;                                              // 0040d3c0
    virtual std::int32_t card_space_needed(std::int32_t size) = 0;                // 0040d580
    virtual std::int32_t profile_size(std::uint32_t profile) = 0;                 // 00334100
    virtual std::int32_t current_profile() = 0;                                   // 001314f0
    virtual void profile_select(std::uint32_t profile) = 0;                       // 003361b0
    virtual void app_2c9af0(std::int32_t value) = 0;                              // 002c9af0
    virtual void app_131470(std::int32_t value) = 0;                              // 00131470
    virtual void app_2c82f0() = 0;                                                // 002c82f0
    virtual void app_2c81d0() = 0;                                                // 002c81d0
    virtual std::int32_t popup_open(std::int32_t kind, const std::optional<std::string>& title,
                                    const std::optional<std::string>& message, const Widget* owner,
                                    std::int32_t arg) = 0;                        // 002cae30
    virtual void popup_close(std::int32_t id) = 0;                                // 002cac80
    virtual std::int32_t popup_query(std::int32_t id) = 0;                        // 002caba0
    virtual std::uint8_t sound(std::uint32_t table, const Widget* sender, bool alternate) = 0;  // 00465c50 / 00465b50
    virtual void pad_reset() = 0;                                                 // 0036a240
    virtual void text_object(const TextObject& created) = 0;                      // 003d0940
    virtual void set_text(const std::string& text) = 0;                           // 003d08b0
    virtual Widget* factory(const std::string& class_name, const std::string& file) = 0;  // 00418a80
    virtual void release(Widget* widget) = 0;                                     // 00418840
    // Native manager only (GuiRuntime::native_app): the resource lookup
    // 004133b0(00571dc0, &res, name) of 004191d0 (nullopt = not found) and the
    // stream of 00419110 (0036e1b0 open "NAME.GUI" + 0036dc60 data); nullptr =
    // the data does not start with "GUI".
    virtual std::optional<std::int32_t> resource(const std::string& name);
    virtual const GuiNode* open_gui(const std::string& file, std::int32_t res);
    // 003d1660(0055a700, 1): the sceUsbKb poll of 004153f0 (IOP RPCs
    // 00222ee0 / 00223360 / 003d1770). nullopt = no report pending (returns
    // 0); otherwise the report the buffer holds (+98 key count, +94 modifier
    // word, +9c first key code) and the call returns count != 0.
    struct UsbKeyReport {
        std::int32_t count = 1;   // +98
        std::uint32_t mod = 0;    // +94
        std::uint16_t code = 0;   // +9c
    };
    virtual std::optional<UsbKeyReport> usbkb_poll();
    // 00418420(manager, NULL / ""): the manager's default font (+10), as
    // GuiTextListWidget's init 004218f0 asks for it.
    virtual std::uint32_t default_font();
    // 0036a2f0(font): the font's line height (byte +99).
    virtual std::uint8_t font_height(std::uint32_t font);
    // 0036aa50(font, s, n): the width of the first n characters of the
    // NUL-terminated bytes s (GuiMLTextWidget's word wrap 0043dce0).
    virtual std::int32_t font_width(std::uint32_t font, const std::string& s, std::int32_t n);
    // GuiAgentLoad 00292b90: 00334790(agent, name, slot) & 0xff on the card file
    // (0 loaded, 2 corrupt, else a bad load), gent being 001314f0(004ffb50, -1).
    virtual std::uint8_t agent_load(std::int32_t agent, const std::string& name, std::int32_t slot);
    // 00292c54 after a load: byte 00493a60 = 0 and the 0x440 bytes at
    // *(agent + 9a4) copied to 0055d510.
    virtual void agent_activate(std::int32_t agent);
    // Test scripts only: 0041f960 declines to construct id (returns 0), so a
    // factory of a screen that is not ported yields null. Default: never.
    virtual bool class_stopped(const std::string& id);

    // GuiCommandCenter's profile services (profile = 001314f0(004ffb50, -1)).
    virtual std::optional<std::string> profile_name(std::int32_t profile);   // 00336170 (nullopt = NULL)
    virtual std::optional<std::string> profile_rank(std::int32_t profile);   // 00336110
    virtual std::int32_t profile_clp(std::int32_t profile);                  // 003305d0
    // 00332fe0(profile, key): key = 003fda40's 16-byte lower-case level name.
    virtual std::int32_t level_done(std::int32_t profile, const std::string& key);
    // 00332860(profile, level + 1, k): k -1 the level's bonus flag, else ZEUS file k.
    virtual std::int32_t level_flag(std::int32_t profile, std::int32_t level, std::int32_t k);
    // The agent's level records (the vector at agent + c4: count +c8, data +cc, 0x20 bytes each) as
    // eight words, read by GuiMissionStatistics' refresh 0034f3b0.
    virtual std::vector<std::array<std::int32_t, 8>> profile_level_records(std::int32_t profile);
    virtual std::optional<std::string> profile_6060(std::int32_t profile, const std::string& key);  // 00336060
    virtual std::int32_t profile_3b10(std::int32_t profile, const std::string& key);                // 00333b10
    struct BonusRow {          // 003334c0's 0x14-byte rows
        std::int32_t key = 0;  // +4 string table index (003fdae0)
        std::uint8_t b8 = 0;   // +8
        std::int32_t unlock = -1;  // +c the registry row's unlock
        std::int32_t percent = 0;  // +10 (00333600's rating percentage; 0 for 003334c0)
    };
    virtual std::vector<BonusRow> profile_vec(std::int32_t profile, std::int32_t kind);  // 003334c0
    struct BonusItem {                    // 00333260's 0x1c-byte rows
        std::optional<std::string> name;  // +4
        std::uint8_t b8 = 0;              // +8
        std::optional<std::string> text;  // +0 the localized text (003fdb50)
        std::int32_t a = 0, b = 0, c = 0;  // +c / +10 / +14 (kind 4: the omega requirement's image)
        std::int32_t unlock = -1;          // +18
    };
    virtual std::vector<BonusItem> profile_items(std::int32_t profile, std::int32_t kind, std::int32_t index);  // 00333260
    // 003353f0(agent, short, list): the level's objectives (the objective rows "<short>_*" but not
    // "*_COMPLETED"), in BonusItem's layout (+8: 2 done, 1 not).
    virtual std::vector<BonusItem> profile_level_objectives(std::int32_t profile, const std::string& short_name);
    // The rank (00532b50) / dossier (00532b70) registry rows the personnel files read.
    struct RankEntry {
        std::optional<std::string> name;  // +0
        std::int32_t index = 0;           // +4
        std::int32_t threshold = 0;       // +8
        std::int32_t unlock = -1;         // +c
        std::optional<std::array<char, 16>> text;  // +10 (16 bytes, no NUL at 16 characters)
    };
    virtual std::vector<RankEntry> profile_ranks();     // 00532b50
    virtual std::vector<RankEntry> profile_dossiers();  // 00532b70
    virtual std::string string_key(std::int32_t index);  // 003fdae0
    virtual void level_start(std::int32_t profile, std::int32_t level, std::int32_t a, std::int32_t b,
                             std::int32_t c);  // 003328d0
    // lbu profile + offset (+b68 "modify agent" lit, +b72 / +b73 unsaved changes).
    virtual std::uint8_t profile_byte(std::int32_t profile, std::uint32_t offset);
    // The agent save flow 00441a10 / 00441ca0 (checkpoint 132cc3): the agent's
    // word +4 (its card slot) and C string +8ac (its name), the card wrapper's
    // services on W = *0055a830 and the profile save 00334110.
    virtual std::int32_t profile_word(std::int32_t profile, std::uint32_t offset);
    virtual std::string profile_text(std::int32_t profile, std::uint32_t offset);
    virtual std::uint32_t card_make_directory();                 // 0040d740(W)
    virtual std::uint32_t card_format();                         // 0040db10(W)
    virtual void card_remove(const std::string& name);           // 0040dc80(W, name)
    // 0040ddc0(W, old, new): the AGENTOPN format update's rename; on success the card thread's command 9
    // (0040ec40) copies the new name into W's entry table (g.card.dir). Its result is ignored.
    virtual void card_rename(const std::string& from, const std::string& to);
    virtual std::uint32_t card_exists(const std::string& name);  // 0040d690(W, name)
    virtual std::uint32_t agent_save(std::int32_t profile);      // 00334110(agent)
    // 00333f60(agent, slot, nosave): reset, refill (0, 1), +4 = slot, +b70 = 1, then the save 00334110
    // unless nosave or the card-busy byte 0055a834; its result (else 0).
    virtual std::uint32_t agent_create(std::int32_t profile, std::int32_t slot, std::uint32_t nosave);
    virtual void agent_set_name(std::int32_t profile, const std::string& name);  // 003340b0(agent, name)
    // 0045a2a0(*004ffd2c): 0040e090(W, "SFO.CFG", options_file_bytes, 0x24, 1); its result != 0.
    virtual std::uint32_t options_save(const GameOptionsRecord& record);
    // *0055a838 = 00441c10 while a save flow polls: run once per pass of a card call's wait loop
    // (0040e270 & co.): 002cc260(0, 0), the nested GUI update 002cbb30 and draw 002cbad0.
    virtual void card_poll();
    // PORT-ONLY (not in the original): the options screen's added cheat
    // entries (port_cheats.hpp). port_cheats() false (the default) adds no
    // entries; the unlocks throw unless a host provides them.
    virtual bool port_cheats();
    virtual void port_unlock_all();
    virtual void port_unlock_all_weapons();

    // The in-game start menu's game services (checkpoint 132f).
    virtual float mission_time();                 // 00245890(*004ffc2c): the mission clock in seconds
    virtual std::int32_t par_time();              // 00330c30(004ffc90, mp): the par in seconds (< 0: none)
    virtual bool objective_manager();             // 00225cd0: 003caa50(004ffb50, "ObjectiveManager") != 0
    struct ObjectiveRow {                         // the record pointers 00227170 returns
        std::optional<std::string> text;          // record +60 (nullopt = NULL)
        std::int32_t id = 0;                      // record +20
    };
    // kind 0 00227100 (open objectives), 1 002270e0 (open parameters),
    // 2 002270c0(byte) (completed), 3 002270a0 (failed); at most `max` rows.
    virtual std::vector<ObjectiveRow> objective_rows(std::int32_t kind, std::int32_t max, std::uint8_t byte);
    virtual std::int32_t objective_total();       // 00227120
    // 003680a0 event 0x13 (00368150..003682b4): Cross on an objective row. 002275a0(00225cd0(), id)
    // (NULL: nothing), then each record of the HUD marker manager's +24 list whose live object
    // (+14 when its +c is the record's +18) is that objective: 00272ad0, 0026c570 and the ticker
    // 002749b0 / 003fa3f0. The event returns 1 either way.
    virtual void objective_select(std::int32_t id);
    // Checkpoint 132hud: the HUD map (hud + 6a0) the GuiMapScreen (MAPSCRN) drives. Defaults throw naming
    // the address. map_open is 0026dbc0 (wake 00464b30), map_close 0026d720 (sleep 00464b00), map_reset
    // 0026c130 (Select), map_hold 0026d710 then map_prev 0026c180 / map_next 0026c3b0 (L1 / R1, true: the
    // waypoint changed), map_release 0026d6d0 (L1 / R1 let go).
    virtual void map_open();
    virtual void map_close();
    virtual void map_reset();
    virtual void map_hold();
    virtual bool map_prev();
    virtual bool map_next();
    virtual void map_release();
    // The pad driver's +3c0 mask (*0055a6c0 = 0055a300 + 3c0): the next pad_reset 0036a240 clears only the
    // bits it holds (00369ee0 ands with ~mask), then stores -1.
    virtual void pad_mask(std::int32_t mask);
    virtual void sound_pause(std::int32_t v);     // 001c96d0(003caa50(004ffb50, "GameSoundManager"), v)
    virtual void ticker_show(bool v);             // 003f9e30(*004ffc2c + 100, v)
    virtual void app_12fa60(std::int32_t v);      // 0012fa60(004ffb50, v)

    // GuiGameOptions' game services (checkpoint 132f2).
    virtual void rumble_stop();                   // 0026bd60
    virtual void rumble_start(std::int32_t a, std::int32_t b, std::int32_t c, std::int32_t d, float e);  // 0026bde0
    virtual void rumble_update(float t);          // 0026bc20
    virtual void rumble_enable(std::uint8_t on);  // 0026be70
    // *004ffc0c (the player) +174 / +175 / +176 from the record's +4 / +6 / +7
    // (offset 0x174..0x176); not called when there is no player.
    virtual bool options_player();                // *004ffc0c != 0
    virtual void options_player_byte(std::uint32_t offset, std::uint8_t v);
    virtual bool sound_manager();                 // 003caa50(004ffb50, "GameSoundManager") != 0
    virtual void sound_volume(std::int32_t channel, std::int32_t volume);  // 001c9540(mgr, channel, volume)
    virtual void sound_mode(std::int32_t mode);   // 001c9670(mgr, mode) -> 003a80f0
    // The screen's sound descriptors (001ca210(desc, type, name, 0, 0)) sent
    // with 003c9420: kind 0 STOP (0020ecb0), 1 PLAY (0020faa0), 2 QUIP (0020f2a0).
    virtual void options_sound(std::int32_t kind, std::int32_t type, const std::string& name);
    // GuiGameOptions sleep 0029b070 / wake 0029b1e0 outside the game canvas:
    // 003eb750(003eba50()) and 0036b250(1) / 0036b250(1) and 0036b3b0(1, 0x100000, 0x800).
    virtual void options_frontend_sleep();
    virtual void options_frontend_wake();
    // The video-mode (progressive scan) change, checkpoint 132f14: GuiGameOptions 0029a470 / 0029a568 /
    // 002996d0 / 0029ade8 wrap 0045a3a0 bit 0x800 in the app's suspend 002c9860(*004ffc04, 1) and
    // resume 002c9510(*004ffc04). Bit 0x800 (0045a590) reads 003cba90("BOOT_VIDEOMODE") against "PAL"
    // (00128e80) and calls 00165420(001698c0(), mode) with mode 4 (video_mode set), 2 (PAL) or 0: the
    // frame buffer 640x448 / 640x512 / 640x480 (+254 / +258), +260 (0x1e / 0x32 / 0x1e) into *005383d4,
    // +250 = mode and the GS reset 0037e320 / 0037e780(005381f0, w, h, 0x20, 0x10, mode, 0xfa0, 0x1f4).
    virtual void app_suspend(std::uint32_t mode);       // 002c9860
    virtual void app_resume();                          // 002c9510
    virtual bool boot_video_pal();                      // 00128e80(003cba90("BOOT_VIDEOMODE"), "PAL") == 0
    virtual void video_mode_set(std::uint8_t mode);     // 00165420(001698c0(), mode)
    // Port-only (checkpoint 132ob6, not the original): the rows a port host adds to the title screen's
    // GAMEOPT (the "CPU Partner" row, port_options_row.hpp). Called after options_frontend_wake and for a
    // Cross on a button GuiGameOptions does not know; the defaults add nothing and handle nothing.
    virtual void port_options_wake(Widget* screen);
    virtual bool port_options_pressed(Widget* screen, Widget* sender);
    // Port-only (checkpoint 132ln1, not the original; research/LAN_PLAY.md): LAN Play, a third play mode beside
    // Offline and Online. True reveals ONOFFSEL's hidden "lan" button (00506490 is never written in retail) with
    // the label port_lan_play_label and makes it enter ONLINECN in LAN mode (g.lan_mode). The default (false)
    // leaves the original screen unchanged.
    virtual bool port_lan_play();
    // Checkpoint 132ln2 (GuiOnlineConnect, the navigator 0033c320).
    // 004294c0: the DME session (*00572480 when byte 00572488 is set). Only the network start (002ca710 /
    // 0042a870, not ported) sets it, so the default answers false (null), as in the boot state; every call the
    // original makes on a non-null session stays an unhosted throw at its call site.
    virtual bool net_session();
    virtual std::int32_t net_start();  // 002ca710(app): the online start (modules, MUIS / DNS, DME); throws
    virtual void app_2c9aa0();         // 002c9aa0(app): 001c7d80 on the GameSoundManager (the music stop), +30c = -1
    virtual void card_sync();          // 0040d5d0(0055a830): wait until the card thread is idle (002b5f48)
    // Checkpoint 132on2 (research/ONLINE.md §9): the SFOLobby calls GuiOnlineConnect makes on a non-null session.
    // Each stands in for one call; the lists are the elements the call leaves in the caller's SFOLobby list
    // (0044f880 / 0044f290; the row types are LobbyConfig .. LobbyAnnouncement). The defaults throw naming the
    // call (the default host has no session). A port host (src/port/online/online_lobby) implements them on
    // omega_server.
    using LobbyConfig = gui::LobbyConfig;
    using LobbyServer = gui::LobbyServer;
    using LobbyUniverse = gui::LobbyUniverse;
    using LobbyAnnouncement = gui::LobbyAnnouncement;
    // 0044c440(session, 002cc260, +98, +b4, +d0): the network configurations and MUIS servers (eznet); 0 fails.
    virtual std::int32_t lobby_configs(std::vector<LobbyConfig>& configs, std::vector<LobbyServer>& servers);
    virtual void lobby_disconnect();                       // 0044c3c0
    virtual std::int32_t lobby_error();                    // 00452ff0(session, ..) & 0xff
    virtual std::int32_t lobby_connect(std::int32_t row);  // 0044bab0(session, row, 1): the network and DNAS
    virtual std::int32_t lobby_dnas_result();              // 00452fe0
    // 00442f40(session, 002cc260, &list): the universe request's start; 1 when the lobby was idle.
    virtual std::int32_t lobby_universes_begin();
    // 00442b50(session, server): connect to MUIS server `server` and fill the universe list.
    virtual std::int32_t lobby_muis_connect(std::int32_t server, std::vector<LobbyUniverse>& universes);
    virtual std::int32_t lobby_universe_select(std::int32_t index);  // 00442840
    virtual std::int32_t lobby_universe_default();                   // 00442970
    virtual void lobby_universes_end();                              // 00442e90
    // 003cba90(key): the debug setting string (MUIS_SKIP); the default has no settings and answers "".
    virtual std::string setting(const std::string& key);
    // 00445e80(session, 002cc260, +2f0, +2f4, +2f8) from 002ca350: the policy / announcement request's start.
    virtual std::int32_t lobby_policy_begin();
    // 00445570: the announcements ({id, text} rows appended to app +2f8).
    virtual std::int32_t lobby_announcements(std::vector<LobbyAnnouncement>& out);
    // 004456d0 (usage, app +2f0) / 004456c0 (privacy, app +2f4): the policy's text chunks (0x100 elements).
    virtual std::int32_t lobby_policy(bool privacy, std::vector<std::string>& out);
    virtual void lobby_policy_end();  // 00445dc0 (002ca160 with a session)
    // 002ca640's session tail with a session: 00445850 (the logout, when app +264 and the lobby state is not 10),
    // 002c8090, 00453b00(session, a1) and 00429500(0) (the DME session's end; net_session answers false after).
    virtual void lobby_leave(std::int32_t a1);
    // 0033cca0 (the net screen's lobby poll): the first lobby message whose +210 is 9 (the server's notice; its
    // +c text), removed; nullopt when none.
    virtual std::optional<std::string> lobby_notice();
    virtual bool lobby_unread();  // session +20 (0033cca0: the start button's "onActivate" / "onWake")
    // PORT-ONLY (checkpoint 132on2, research/ONLINE.md §9): whether 0029d390's ONLIPREF branch (app +265, the agent
    // has no universe, or 00442970 failed) is taken. true is the original. A host whose server has one universe
    // (omega_server) answers false: there is nothing to choose, and the branch would make every new agent pick the
    // one universe and save before signing in. GuiOnlineOptions (ONLIPREF) is ported (132on2c) and stays reachable
    // from ACNTLOGN (Circle) and JOINMIS (online_prefs).
    virtual bool lobby_region_choice();
    // Checkpoint 132on2c (GuiOnlineOptions, research/ONLINE.md §9.7): 00442d80(session), the universe list request
    // into the list 00442f40 registered (its rows refilled); 0 fails.
    virtual std::int32_t lobby_universe_list(std::vector<LobbyUniverse>& universes);
    // Checkpoint 132on2c (GuiNetMessages, research/ONLINE.md §9.8): the lobby's message list (session +1c, the
    // SFOLobby list 0044fa00: count +4, the changed flag +18). The defaults throw.
    struct LobbyMessage {
        std::int32_t type = 0;  // +0 (00346fb0: 9 "[from] - text", 10 the text, others a LOBBY_STR text)
        std::int32_t id = 0;    // +8 (00450f30's answer key)
        std::string from;       // +c (0x20 bytes)
        std::string text;       // +2c
    };
    virtual std::int32_t lobby_message_count();                                  // list +4
    virtual std::optional<LobbyMessage> lobby_message(std::int32_t index);       // 00451010(session, i)
    virtual void lobby_message_answer(std::int32_t index, std::int32_t accept);  // 00450f30(session, i, accept)
    virtual bool lobby_messages_changed();                                       // list +18
    virtual void lobby_messages_set_changed(bool changed);                       // 0044f280(list, changed)
    // 00418420(manager, "default") +10..+1c / +20..+2c, as GuiNetMessages' init 00347820 sets them (the two colours
    // 00587b50 / 00587b60, (1, 1, 1, 1) and (0.8, 1, 0.8, 1) after the static set-up). The default does nothing: the
    // port's text drawing keeps no per-font colours.
    virtual void font_colors(std::uint32_t font, const std::array<float, 4>& c10, const std::array<float, 4>& c20);
    // Checkpoint 132on2b (GuiAccountLogin, research/ONLINE.md §9): the account calls. Each answers 1 on success; on
    // failure lobby_error() names the SFOLobby error (0x3a bad password, 0x3b no such account, 0x3c logged in,
    // 0x3d exists, 3 vulgar, 0xb create failed, 2 timeout, 1 general). The defaults throw.
    // 00445a40(session, name, password): the login (00445420, the MAS -> MLS move 0044ce60).
    virtual std::int32_t lobby_login(const std::string& name, const std::string& password);
    // 00445b90(session, name, password): the text filter 00443100 of both, then AccountRegistration.
    virtual std::int32_t lobby_register(const std::string& name, const std::string& password);
    // 004459d0(session, name, password): a login (00445420) and, when it succeeds, the logout.
    virtual std::int32_t lobby_check_account(const std::string& name, const std::string& password);
    // 00442750(session, 002cc260): the main lobby's channel join (lobby state 9 -> 4); 0 fails.
    virtual std::int32_t lobby_enter();
    // Checkpoint 132on2b (GuiCommandCenter online, research/ONLINE.md §9.3). The lobby's state (session +255c: 9
    // logged in, 4 in the main channel; 002973b0 joins the channel when it is not 4).
    virtual std::int32_t lobby_state();
    // 00442510(session, mission): the mission's channel (the navigator's ops 2 / 6, JOINMIS / MSNSETUP); 0 fails.
    virtual std::int32_t lobby_mission(std::int32_t mission);
    // 002c8090(app): with a session and a current agent, the lobby's records (session +1c) to the agent: profile
    // +b73 = 1 when there are any or *0052fe80 (the agent's message count) is nonzero, which is then cleared; more
    // than 0x40 answers 0. The records' dispatch (0044f290, 00336ec0) is not ported. 1 when handled.
    virtual std::int32_t lobby_messages();
    // Checkpoint 132on2b stages B3 / B4 (GuiMissionSetup / GuiNetJoinMission online, research/ONLINE.md §9.4 /
    // §9.5). The defaults are unhosted at their call addresses.
    virtual void lobby_channel_leave();  // 004426b0: the main channel left (state 4 -> 9, the error cleared)
    // 004518f0(session, 002cc260, ..): the game room (state 9 -> 5 and its player / game list requests); 0 fails.
    virtual std::int32_t lobby_room_open();
    virtual void lobby_room_leave();                  // 00451610: state 5 / 0 -> 9, the room's lists freed
    virtual void lobby_list_kind(std::int32_t kind);  // 004515f0: the room list kept up to date (0 players, 1 games)
    virtual void lobby_net_start();                   // 001698c0 / 00165bc0: the PS2 network modules' start
    // 0044e960(session, mission, agents, filter): the game creation request (the host role 005721c0 = 1); 0 fails
    // (lobby_error 0x31: the check 004441d0 failed; 0x32: the request was not sent).
    virtual std::int32_t lobby_create(std::int32_t mission, std::int32_t agents, std::int32_t filter);
    // 004536b0: the lobby's update. A request's completion is its callback, the event 0x1e (a3 1 done, 0 failed; t0
    // the failure's text key, lobby_key_*) to the screen.
    struct LobbyCallback {
        std::int32_t a3 = 0;
        std::int32_t t0 = 0;
    };
    virtual std::optional<LobbyCallback> lobby_pump();
    virtual void lobby_game_cleanup();  // 0042b1f0: the DME game left
    // 0044e060(session, world): the game's GameInfoResponse (null unless its status is 0). The fields the join reads.
    struct LobbyGameInfo {
        std::int32_t max_players = 0;  // +24
        std::int32_t owner = 0;        // +34 (GameStats: the creator's account id)
        std::int32_t mission = 0;      // +38 (GameStats: the mission)
        bool game_name = false;        // +134 (GameName) nonempty: 0033e5b0's another-host prompt (list +b5 set)
        std::string name;              // +134 (132on2i): the prompt's $1 is its first tab field
    };
    virtual std::optional<LobbyGameInfo> lobby_game_info(std::int32_t world);
    // Checkpoint 132on2i (research/ONLINE.md §3.7.1): the session's game cache +118 (0x2c-byte entries, 0044f880 at
    // 00453a88; emptied at the logout 0044d528). 0044edc0 stores a GameInfoResponse (00446b40 the join's, 00446ae0 the
    // contacts' fetch 0044ebd0): +0 world, +4 players (+30), +8 max players (+24), +c..+23 GameStats' first 0x18 bytes
    // (+34: creator, mission, +8, filter, game seconds, +14), +24 world status (+184), +28 the time.
    struct LobbyCachedGame {
        std::int32_t world = 0;
        std::int32_t players = 0;
        std::int32_t max_players = 0;
        std::array<std::int32_t, 6> stats{};
        std::int32_t world_status = 0;
    };
    // 0044f0c0(session, world): the cached game (world 0: none). The default caches nothing.
    virtual std::optional<LobbyCachedGame> lobby_game_cached(std::int32_t world);
    // PORT-ONLY (132on2i): the entries 0044edc0 added or changed since the last call, in order; the runtime applies
    // each as 0044eff0 does to the screen's lists (the original's runs inside the response's callback).
    virtual std::vector<LobbyCachedGame> lobby_game_cache_take();
    virtual std::int32_t lobby_join(std::int32_t world);  // 0044e430(session, world): the join request; 0 fails
    virtual void lobby_join_end();                        // 0044e340: the join abandoned
    virtual void lobby_set_state(std::int32_t state);     // session +255c (the join restores +2560 into it)
    virtual void net_update();                            // 00429d30: the DME update (the join's wait loop)
    virtual std::int64_t lobby_clock_ms();                // 003698b0 (the join wait's deadline, ms)
    virtual std::int32_t lobby_own_id();                  // session +2480: the account id
    // PORT-ONLY (132on2b): the room's last complete game list, its rows as 00447790 receives them (GameListResponse:
    // +1c world, +20 name, +60 world status, +68 player count; status +18 0 only), and a serial that counts the
    // completed lists (the EndOfList +6c on which 00447790 copies the list to the screen). The runtime parses them as
    // 00447790 does.
    struct LobbyGameRow {
        std::int32_t world = 0;
        std::string name;
        std::int32_t status = 0;
        std::int32_t players = 0;
    };
    struct LobbyGameList {
        std::uint32_t serial = 0;
        std::vector<LobbyGameRow> rows;
    };
    virtual LobbyGameList lobby_game_list();
    // Checkpoint 132on2c (research/ONLINE.md §9.5): the game room's chat and player list.
    // 00451050(session, text, target): the chat send (TextFilter, then ChatMessage when the target is not the agent
    // itself; 0 the room's chat). 0 fails (error 0x24: no room chat list registered).
    virtual std::int32_t lobby_chat_send(const std::string& text, std::int32_t target);
    // PORT-ONLY (132on2c): the room chat's records (the 0x68 records the TextFilter callback 00447bb0, the agent's own
    // line, and the ChatFwd callback 00447c70 append to the registered list +23ec, the screen's +ac) since the last
    // call: +0 id, +4 name, +24 text, +64 private.
    struct LobbyChatLine {
        std::int32_t id = 0;
        std::string name;
        std::string text;
        bool whisper = false;
    };
    virtual std::vector<LobbyChatLine> lobby_chat_take();
    // Checkpoint 132on2c: the session's +1ea4, the room chat list's capacity 004518f0 sets up (0044f880 makes it at
    // least 5): 8 from the session's constructor (00453eb8), else the universe setting MAX_CHAT (00443260, >= 2).
    virtual std::int32_t lobby_max_chat() { return 8; }
    // PORT-ONLY (132on2c): the room's last complete player list, its rows as 00447a30 receives them
    // (PlayerListResponse, status +18 0: +1c the account id, +20 the name, +40 the player status) and a serial that
    // counts the completed lists (the EndOfList +cc on which 00447a30 copies the list to the screen's +c8).
    struct LobbyPlayerRow {
        std::int32_t id = 0;
        std::string name;
        std::int32_t status = 0;
    };
    struct LobbyPlayerList {
        std::uint32_t serial = 0;
        std::vector<LobbyPlayerRow> rows;
    };
    virtual LobbyPlayerList lobby_player_list();
    // Checkpoint 132on2h (GuiNetContacts, research/ONLINE.md §3.7). The defaults are unhosted at their addresses.
    // 0044df80(session, 002cc260, +ac, +c8): the contacts (state 9 / 0 -> 6, the chat and contact lists registered,
    // the first GetBuddyListExtraInfo sent; refreshed every 20 s); 0 fails (error 0x21).
    virtual std::int32_t lobby_contacts_open();
    virtual void lobby_contacts_close();  // 0044dd20: state 6 / 10 -> 9, the lists freed
    // PORT-ONLY (132on2h): the last complete contact list, its rows as the callback 00446830 receives them
    // (GetBuddyListExtraInfoResponse, status +18 0: +1c the account id, +20 the name, +40 the player status, +48 the
    // game world) and a serial that counts the completed lists (the EndOfList +cc on which 00446830 copies the list
    // to the screen's +c8).
    struct LobbyContactRow {
        std::int32_t id = 0;
        std::string name;
        std::int32_t status = 0;
        std::int32_t world = 0;
    };
    struct LobbyContactList {
        std::uint32_t serial = 0;
        std::vector<LobbyContactRow> rows;
    };
    virtual LobbyContactList lobby_contact_list();
    // 0044d9c0(session, id): RemoveBuddy, blocking; 1 removed, 0 refused (error 0x29) or failed (1 / 2).
    virtual std::int32_t lobby_contact_remove(std::int32_t id);
    // 00451370(session, name): AccountGetID (blocking unless cached); -1 fails (error 0x23 state, 0x22 name, 0x1d
    // no such account).
    virtual std::int32_t lobby_account_id(const std::string& name);
    // 0044db60(session, id): AddToBuddyList after a blocking list refresh; 0 fails (error 0x1d bad id, 0x25 self,
    // 0x1e already a contact, 1 not sent).
    virtual std::int32_t lobby_contact_add(std::int32_t id);
    // Checkpoint 132on2d: 00450cf0, the record cache's rank byte (+1) for an account whose record is younger than
    // 00497848 (30000 ms); nullopt draws JOINMIS' rank column as "---" (002ab0f8). The default answers nullopt.
    virtual std::optional<std::int32_t> lobby_player_rank(std::int32_t account);
    // 0033fc80 (132on2e): the agent's appearance was stored (CMDCENTR's modify agent); with the network manager
    // (004294c0) up, the record is rebuilt (002cbd40) and uploaded (004505e0). Default: no network, nothing.
    virtual void lobby_agent_edited();
    // Checkpoint 132on2i (GuiAgentInfo mode 1, research/ONLINE.md §3.7.2). 00450c60(session, id, buf): another
    // agent's record (0xfc bytes): the record cache's (00450cf0, younger than 00497848 30000 ms) or GetAccountStats'
    // (00451200, copied whatever its version byte); false fails. Default: unhosted.
    virtual bool lobby_agent_stats(std::int32_t id, std::array<std::uint8_t, 0xfc>& out);
    // The agency cell (clan) calls: 0044a1d0 the agent leads its cell (+265c after the refresh 00449a00), 0044a230
    // the agent is in a cell (+265d, GetMyClans when stale), 0044a070 its cell's name (+e8c), 00448ff0 a clan's name
    // by id (GetClanByID; nullopt none), 0044a0d0(id, 1) the agent is in the own cell's member lists, 0044d670(id, 1)
    // the agent is a contact (the buddy lists), 0044ac10 the cell invitation (1 sent, else lobby_error()) and 0044af60
    // the cell's creation (1 created, else lobby_error()). Defaults: unhosted.
    virtual bool lobby_cell_leader();
    virtual bool lobby_cell_member();
    virtual std::string lobby_cell_name();
    virtual std::optional<std::string> lobby_cell_name_of(std::int32_t clan);
    virtual bool lobby_cell_has(std::int32_t id);
    virtual bool lobby_is_contact(std::int32_t id);
    virtual std::int32_t lobby_cell_invite(std::int32_t id);
    virtual std::int32_t lobby_cell_create(const std::string& name);
    // PORT-ONLY (132on2c, research/ONLINE.md §5.3 GamePassword): the last join was refused for its game password
    // (JoinGameResponse -978); the default (no such service) answers false. lobby_set_join_password stores the
    // password the next lobby_join sends (JoinGame +0x30, which the original leaves zero).
    virtual bool lobby_join_password_refused();
    virtual void lobby_set_join_password(const std::string& password);
    // A string stored into the agent record at `offset` (00129360 strcpy; GuiAccountLogin's password, +8cc).
    virtual void profile_set_text(std::int32_t profile, std::uint32_t offset, const std::string& s);
    // Checkpoint 132ln3 (GuiMissionSetup's begin_mission 002a0f60 and its success 002a0dc0).
    // 0012ee30(004ffb50): +b70 = 0 on the profile of every player slot 0..3 other than the local one.
    virtual void profiles_clear_b70();
    // 00332a10(profile, level, a, b): the agent's level-state list (agent_level_progress).
    virtual void level_progress(std::int32_t profile, std::int32_t level, std::int32_t a, std::int32_t b);
    // PORT-ONLY (checkpoint 132ln3, research/LAN_PLAY.md §4 rows 5 and 6b): the LAN session (LanSession). The
    // default has none: lan_hosting answers false and lan_host throws.
    struct LanHostRequest {
        std::string host_name;         // the agent's name (00336170)
        std::int32_t mission = -1;     // app +248 (the command centre's online mission, table 004acb30)
        std::int32_t num_agents = 4;   // app +180 (MSNSETUP num_agents, 2..4)
        std::uint8_t join_filter = 0;  // app +185 (MSNSETUP join_filter, 0..2)
    };
    virtual bool lan_hosting();
    // Open the LAN game (the host socket and its discovery advertisement); false when it cannot be opened.
    virtual bool lan_host(const LanHostRequest& request);
    // PORT-ONLY (checkpoint 132ln4, LAN_PLAY.md §4 rows 7 / 7b): GuiNetJoinMission in LAN mode. The lobby lists the
    // discovered LAN games (LanDiscoveryBrowser) where the original lists Medius games; the join connects through
    // the LAN session (LanTransport::connect, JOIN_REQ / JOIN_ACK) in place of 0044e430. The defaults have no LAN
    // session: browsing lists nothing, lan_join throws.
    struct LanGame {
        std::uint64_t session_id = 0;
        std::string host_name;         // the advert's host agent name
        std::int32_t mission = -1;     // the advert's mission index (app +248 on the host, 0..8)
        std::int32_t agents_now = 1, agents_max = 4;
        std::uint8_t join_filter = 0;  // the advert's MSNSETUP join_filter (0..2)
        std::uint8_t state = 0;        // 0 open, 1 in progress, 2 full
        bool mismatch = false;         // another protocol version or build hash (§5.3: listed greyed)
    };
    virtual void lan_browse(bool on);              // start / stop the discovery browser
    virtual std::vector<LanGame> lan_games();      // the browse list, in first-seen order
    enum class LanJoin { idle, pending, accepted, rejected, failed };
    // Connect to the game and send JOIN_REQ; false when it cannot be sent (no such game, a game hosted here).
    virtual bool lan_join(std::uint64_t session_id);
    virtual LanJoin lan_join_state();              // rejected: a JOIN_NAK; failed: no answer / the link lost
    virtual void lan_join_cancel();                // close the join (0042b1f0 / 0044e340's stand-in)
    // PORT-ONLY (checkpoint 132md5, research/MODDING.md "LAN: the mod report at join"): after a rejected join, the
    // host's reason when its mod policy refused the joiner; the rejected join's modal shows it in place of
    // F_TO_JOINGAME's text. nullopt (the default, and every other JOIN_NAK) keeps the original's text.
    virtual std::optional<std::string> lan_join_notice();
    virtual void lan_pump();                       // the LAN session's discovery and transport (004536b0 / 00429d30)
    virtual std::int64_t lan_clock_ms();           // the LAN session's clock (the join wait's deadline)

    // GuiRadioLog's game services (checkpoint 132f3): the HUD's PDA quip
    // records (*004ffc2c +0 vector, count +4).
    virtual std::int32_t pda_count();             // -1: no HUD (*004ffc2c == 0)
    struct PdaRecord {
        std::uint8_t flag = 0;                    // +0 (replayed)
        std::string text;                         // +1 the list's string key
        std::string name;                         // +11 the quip (and subtitle key)
    };
    virtual PdaRecord pda_record(std::int32_t i);
    virtual void pda_mark(std::int32_t i);        // record +0 = 1 (00454530)
    // The +50 record sent with 003c9420 (deliver): kind 0 0020ecb0 STOP of the
    // record as it stands, kind 2 0020f2a0 QUIP after 001ca010(+50, type,
    // name, 0, priority).
    virtual void radio_sound(std::int32_t kind, std::int32_t type, const std::string& name, std::int32_t priority);
    // Checkpoint 132hud4: 0045ce10's a3 != 0 tail (0x0045ced8..0x0045cf5c, the PlayXA voice): a stack
    // descriptor 001ca210(desc, 4, name, 0, 100), the quip message 0020f2a0(msg, desc), msg +20 = 1,
    // 003c9420(msg, 004ffb50, 1) (delivered at once), then the destructors 0036e220 x 4 and 003c9d60.
    virtual void subtitle_voice(const std::string& name);

    // GuiSlideShow's / GuiBitmapWidget's services (checkpoint 132zf).
    virtual std::uint32_t zeus_archive();         // *(*004ffc04 + 25c): the mounted ZEUS.HOG (0: none)
    // 0036d2a0(archive, name, 1): the member's index, -1 when the HOG has none.
    virtual std::int32_t zeus_lookup(std::uint32_t archive, const std::string& name);
    virtual std::int32_t zeus_has(std::int32_t profile, std::int32_t a1, std::int32_t a2, std::int32_t a3);     // 003329b0
    virtual std::int32_t zeus_viewed(std::int32_t profile, std::int32_t a1, std::int32_t a2, std::int32_t a3);  // 00332950
    // 0043c730: 00380a40 of the archive member (opened by 0036e1b0): the
    // texture slot (-1: none) and the slot record's width / height (+44 +4 / +6).
    struct BitmapTexture {
        std::int32_t slot = -1;
        std::int32_t width = 0, height = 0;
    };
    virtual BitmapTexture bitmap_load(std::uint32_t archive, const std::string& name);
    virtual void bitmap_release(std::int32_t slot);  // 003813a0
    // GuiEquipmentPane / GuiWeaponDisplay / GuiWeaponStats' game services
    // (checkpoint 132f3b) on the player *004ffc0c and its actor +34.
    virtual bool equip_player();                  // *004ffc0c != 0 and its actor != 0
    virtual std::int32_t equip_held();            // 00176110(player): -1, or slots[current_84].item
    virtual std::array<std::int32_t, 8> equip_slots();  // the actor inventory's eight slots' item (+8)
    struct ItemStack { std::int32_t index = 0; std::int32_t count = 0; };
    virtual std::vector<ItemStack> equip_stacks();  // the actor's item list +3528 (00470560 counts)
    virtual std::int32_t weapon_class(std::int32_t item);  // 00143d20 (byte table 00489d60[record +30])
    virtual bool weapon_is_held(std::int32_t item);  // 00147840(item) == 001437e0(actor inventory)
    virtual void equip_item(std::int32_t item);   // 00175ff0(player, item) -> 001439b0
    virtual std::uint8_t goggles_33e7();          // actor +33e7
    virtual void goggles_toggle();                // 00199860(actor)
    virtual std::int32_t clips_for(std::int32_t clips_74);  // 001861f0(player actor, record +74)
    // GuiGameScreen's weapon select (checkpoint 132hud) on the player actor's inventory +3524.
    virtual void holster_current();                       // 00143b60(+3524)
    virtual std::int32_t weapon_spare(std::int32_t slot);  // 00143870(+3524, slot)
    virtual std::int32_t weapon_clip(std::int32_t slot);   // 00143940(+3524, slot)
    virtual bool weapon_has_clip(std::int32_t item);       // 00143710(+3524, item)
    // 0045cc90's 00147150(*004ffd30, *00497ac0 "icon_hand.skl", gs + d8, -1, 0, 0, 0): the hand icon model
    // handle (0: not loaded); nullopt when the host has no weapon model loader (the panel then throws).
    virtual std::optional<std::int32_t> hand_model();
    // 00147840(item): the weapon table record (nullopt: null).
    struct WeaponRecord {
        std::string name_80;                      // +80
        std::uint8_t hold_f0 = 0;                 // +f0
        std::int32_t icon_11c = 0;                // +11c
        std::int32_t model_13c = 0;               // +13c
        std::int32_t w60 = 0, w64 = 0;            // +60 / +64
        std::int32_t clip_70 = 0, clips_74 = 0;   // +70 / +74
        std::uint8_t class_30 = 0;                // +30 (raw: the attach point 0035bfb0 indexes)
        std::uint8_t byte_bc = 0;                 // +bc (7: the non-lethal damage type 002be910 tests)
    };
    virtual std::optional<WeaponRecord> weapon_record(std::int32_t item);
    virtual void weapon_preload(std::int32_t item);  // 00145710(db, item, 0)
    virtual std::optional<std::string> item_name(std::int32_t index);  // 00470270 (item table +0)
    virtual std::optional<std::string> item_desc(std::int32_t index);  // 00470240 (item table +4)
    virtual std::int32_t item_model(std::int32_t index);  // 004702a0: the item's resource model +28

    // GuiScoresEquip's game services (checkpoint 132f3b).
    // 003fb8d0(i): the game's player i's +c id (nullopt: no player i).
    virtual std::optional<std::int32_t> scores_player(std::int32_t i);
    // 00185c70(player i): its agent record's +b70 byte (a loaded or created agent).
    virtual bool scores_agent_b70(std::int32_t i);
    virtual std::string scores_agent_name(std::int32_t i);  // 00336170(agent)
    virtual std::int32_t scores_statistic(std::int32_t i, std::uint32_t index);  // 00333ee0(agent, index)
    // Player i's render model's attach table (+60 + 2410, ten points): each
    // point's model's weapon record (lh +40a), -1 for an empty point.
    virtual std::array<std::int32_t, 10> scores_attach(std::int32_t i);

    // GuiEquipmentModify's game services (checkpoint 132f4), for the screen's
    // player +48 (solo: player 0, whose attach table is scores_attach(0)).
    // 00332ac0(00185c70(001314f0(G, -1)), list): the weapons the agent's
    // unlock bits (+b08) give, in AWARDS unlock-row order (duplicates kept).
    virtual std::vector<std::int32_t> eqm_unlocked_items();
    // 002bf0b4: the BSS byte 0051ee38, which no code of the original sets (0 here); nonzero makes 002bf090
    // list every database record of the category's class (0x002bf230) instead of the agent's weapons.
    // Checkpoint 132wp1: omega_play's PORT-ONLY --loadout-all-records sets it for the weapons sweep.
    virtual bool eqm_byte_51ee38();
    virtual std::int32_t weapon_record_count();  // 002bf234: *(*(*004ffd30 + 4) + 4)
    // PORT-ONLY observation (Checkpoint 132wp1, the weapons sweep): the records 002bf090 just listed.
    virtual void eqm_listed(const std::vector<std::int32_t>& items);
    virtual bool eqm_dual_wield();          // 00332f90(001314f0(G, -1), 1)
    virtual std::string eqm_agent_rank();   // 00336110(00185c70(+48))
    // 003fdb80(003fdb40(key) + offset): the string `offset` ids after key's.
    virtual std::optional<std::string> string_after(const std::string& key, std::int32_t offset);
    virtual void weapon_unload(std::int32_t item);  // 00145630(db, item, 1)
    // 002bcb50's Square on the player (steps 00143b60 .. 00142c50): `preview`
    // the edited attach table (records, -1 empty) after the hand
    // normalisation, `fc` the hand's point.
    virtual void eqm_accept(const std::array<std::int32_t, 10>& preview, std::int32_t fc);
    virtual void eqm_start();   // byte 004ffc29 = 0; 0036a0f0(0055a300)
    virtual void eqm_resume();  // 0012eee0(G, 0, 0)
    // Port-only (checkpoint 132f5): the pause menu's Cheats entry. With
    // port_cheat_menu() false (the default) the start menu is the original's.
    // port_cheat_toggle returns true when the pause menu is to close (End
    // Level: the level end runs unpaused, as the start menu's Quit).
    // port_cheat_action rows (End Level) are one-shot: no On / Off value is
    // shown and Cross always runs them.
    virtual bool port_cheat_menu();
    virtual std::int32_t port_cheat_count();
    virtual bool port_cheat_action(std::int32_t i);
    virtual bool port_cheat_on(std::int32_t i);
    virtual bool port_cheat_toggle(std::int32_t i);
    // Port-only (checkpoint 132win11): the Port Settings screen's model (nullptr, the default: no screen, no pause
    // menu entry).
    virtual PortSettingsScreen* port_settings_screen();

    // GuiAgentModify's services (checkpoint 132cc4b).
    // 0055dda0: the front end's character part cache (frontCharacter 0022a220).
    virtual omega::AgentPartCache* agent_parts();
    // 00332500(agent, list) (AgentData_FilterAwards): the names of the parts the agent has not unlocked.
    virtual std::vector<std::string> agent_locked_parts(std::int32_t profile);
    // 00334000(agent, packing): the appearance block to *(agent + 9a4), and agent +9a8 = the
    // compact form 004073c0 (zeroed 0x100 bytes, then min(n, 0x100)).
    virtual void agent_store_appearance(std::int32_t profile, const omega::AgentPacking& packing,
                                        const omega::AgentPartCache& cache);
    virtual void profile_set_byte(std::int32_t profile, std::uint32_t offset, std::uint8_t v);  // sb to the agent
    virtual std::int32_t loc_find(const std::string& key);       // Loc_FindKeyThunk (003fe650)
    virtual std::optional<std::string> loc_text(std::int32_t id);  // Loc_GetTextById
    virtual void app_40f540();                                   // 0040f540: if (*00571cd0) World_ClearPending()
    // GuiAgentCreate's services (checkpoint 132cc5).
    // Skl_Load 00402840's file reads (GAMEDATA\FRONTEND\MALE.SKL / FEMALE.SKL): the file's bytes.
    virtual std::vector<std::uint8_t> skl_file(const std::string& path);
    virtual void profile_set_word(std::int32_t profile, std::uint32_t offset, std::int32_t v);  // sw to the agent
    // The debrief's services (checkpoint 132f11, gui_end_game.cpp).
    virtual bool quip_playing(const std::string& name);  // 001c95d0(003caa50(G, "GameSoundManager"), name)
    // 00333600(agent, list): the agent's rating rows (0x14 each: +4 the string key, +8 the flag).
    virtual std::vector<BonusRow> profile_ratings(std::int32_t profile);
    // The rank row *(agent + b50) points at (AwardRankEntry): +0 its name (a string key, 003fdb50's
    // argument) and +4 its index. Only read when +b54 != 0 and +b50 != +b54 (the rank changed).
    struct RankRow {
        std::optional<std::string> name;
        std::int32_t index = 0;
    };
    virtual RankRow profile_rank_row(std::int32_t profile);
    // GuiAgentInfo's services (checkpoint 132f14).
    // 00333ee0(agent, index): the agent's statistic word `index` (agent_statistics.hpp).
    virtual std::int32_t profile_statistic(std::int32_t profile, std::int32_t index);
    // *(agent + 9a4): the agent's appearance block (0x440 bytes), or nullopt when the pointer is null.
    virtual std::optional<omega::AgentPacking> profile_appearance(std::int32_t profile);
};

// 0x497b20: the debrief award layout's bitmaps per line, for 1..15 awards (gui_end_game.cpp).
extern const std::array<std::array<std::int32_t, 3>, 15> end_award_rows;

// 0048c160: the 18 level names (17 missions, then TRAINING).
extern const std::array<const char*, 18> cc_level_names;

// One translated input event (the 0x18-byte record 00415510 / 004153f0 build).
struct PadEvent {
    std::uint16_t kind = 1;      // +0 (1 pad, 2 USB keyboard)
    std::uint16_t port = 0;      // +2
    std::uint16_t zero = 0;      // +4 (1 in keyboard events)
    std::uint16_t released = 0;  // +6
    std::uint16_t mask = 0;      // +8 (pad button mask; keyboard: the key code)
    std::uint16_t repeat = 0;    // +c (auto-repeat count of pad events)
    std::uint32_t mod = 0;       // +c (keyboard events: the modifier word)
    std::uint8_t ch = 0;         // +10 (keyboard events: the character, 003d1580)
};

// Per-frame inputs of the pad translation.
struct PadInput {
    std::array<std::uint32_t, 2> current{};   // pads +14c
    std::array<std::uint32_t, 2> previous{};  // pads +158
    std::uint32_t t1 = 0;                     // 10000800 (T1_COUNT)
    std::int64_t ticks = 0;                   // 00533878
};

struct CardState {           // *0055a830
    std::uint8_t present = 1;     // +59d
    std::uint8_t check_space = 0; // +59e (formatted)
    std::uint8_t directory = 0;   // +59f (the game's save directory exists)
    std::int32_t free = 0;        // +5a4
    std::int32_t slot = 0;        // +5a8
    // The entry table 0040d600 lists: count +560, names at +1c + 0x38 i.
    std::vector<std::string> dir;
};

class GuiRuntime {
public:
    explicit GuiRuntime(GuiHost& host);
    GuiRuntime(const GuiRuntime&) = delete;
    GuiRuntime& operator=(const GuiRuntime&) = delete;

    // 0041f960 + 00418fd0 + 00419530: builds the tree of a decoded .GUI file
    // under a new root of `root_class` ("GuiWidget", "GuiTitleScreen" or
    // "GuiMessageBox"), adds it to the manager's roots and initialises it.
    // Throws if the file root is not a GuiWidget.
    Widget* load(const GuiNode& file_root, WidgetClass root_class);

    // 0041f960: the registered class `name` (case-insensitive); nullptr when
    // no class has that name, throws 0041f960 for registered classes that
    // are not ported.
    Widget* new_widget(const std::string& name);
    // 00418fd0 (00418b10 + 00419530) into an existing root.
    // Returns nullptr (nothing read) when the file root is not a GuiWidget.
    Widget* load_into(const GuiNode& file_root, Widget* root);

    // Manager (*00571fb8) services; native when native_app is set,
    // otherwise factory / release go to the GuiHost.
    Widget* factory(const std::string& class_name, const std::optional<std::string>& file);  // 00418a80
    std::uint8_t release(Widget* w);                                 // 00418840
    Widget* lookup(const std::string& class_name) const;             // 00418980
    Widget* create(const std::string& class_name, const std::optional<std::string>& file);   // 004187c0
    bool load_file(const std::string& name, Widget* w);              // 004191d0
    Widget* load_root(const std::string& name);                      // 004191d0(mgr, name, 0): a new root
    void unload(Widget* w, bool now);                                // 00418e00
    void remove(Widget* w);                                          // 004193d0
    void flush();                                                    // 00419270
    bool in_roots(const Widget* w) const;

    // Application popup queue (app +2fc) shown through the message box at
    // app +258; native when native_app is set, otherwise GuiHost services.
    std::int32_t popup_open(std::int32_t type, const std::optional<std::string>& title,
                            const std::optional<std::string>& message, Widget* target, std::int32_t arg);  // 002cae30
    std::int32_t popup_close(std::int32_t id);   // 002cac80
    // 002caba0 returns the matching payload pointer (callers only test it
    // for nonzero); natively 1 when an entry with `id` is queued, else 0.
    std::int32_t popup_query(std::int32_t id);   // 002caba0
    void msgbox_setup(Widget* mb, std::int32_t type, const std::string& title, const std::string& message,
                      Widget* target, std::int32_t arg);  // 002a7e90

    bool native_app = false;
    Widget* msgbox = nullptr;                     // app +258
    std::int32_t next_popup = 0;                  // app +308
    std::vector<PopupEntry> popups;               // app +2fc
    std::vector<Widget*> roots;                   // manager +14
    std::vector<Widget*> pending_unload;          // manager +20
    float blink = 0.0f;                           // 00572100 (0041a8f0)
    std::array<float, 4> blink_rgba{};            // 005720e0

    // Canvas vtable entries.
    void push(Widget* w, std::int32_t layer);        // +64 00465570
    void pop(Widget* w);                             // +68 00414be0
    void switch_screens(Widget* from, Widget* to);   // 00414ac0
    // Port-only (checkpoint 132win11): from the front end's options screen (GuiGameOptions, not in game) to the
    // Port Settings screen PORTSETTINGS: the options leave as on Triangle (0029ad10 without a save: unsaved
    // changes are undone), then the screens switch. False when the host has no settings screen.
    bool port_open_settings(Widget* options);
    void set_focus(Widget* f);                       // 00414b30
    void update(float dt);                           // +60 00465500
    // 00415b60(canvas, dt): 002cbb30's update while 0051eeb0 is set (a card wait's nested poll): if the
    // canvas is enabled (+14 & 4), broadcast 0041df00 message 7 with a pointer to dt * 30.
    void nested_update(float dt);
    // Message 7's argument: a3 is the stack float's address (00415b60 sp + 1c); here a3 = tick_arg and the
    // float is tick_value.
    static constexpr std::int32_t tick_arg = 0x7fff0000;
    float tick_value = 0.0f;
    // Decorator event 9's output pointer (004174c8: *a3 = the element's +4 clock), as 0045d960 / 0045ca20
    // pass a stack float: a3 = out_arg and the answer lands in out_value (unchanged without an element).
    static constexpr std::int32_t out_arg = 0x7ffe0000;
    float out_value = 0.0f;
    // The decorator's element clock (the GUI decor's bridge); nullopt: no element.
    std::function<std::optional<float>(Widget* w)> deco_time;
    // The t0 of the send / broadcast whose observer is running (the observers' signature has no t0).
    std::int32_t observed_t0 = 0;
    // 0045a3a0(*004ffd2c, bits): the changed fields' effects (the options screen and the load 0045a150).
    void options_apply(const GameOptionsRecord& r, std::uint32_t bits);
    void translate_pads(const PadInput& in);         // 00415870 / 00415510
    void defer(Widget* pop_w, Widget* push_w);       // canvas +48 / +4c / +50

    // The in-game canvas (GuiGameCanvas, checkpoint 132f): push 00465570 without
    // the tempbackground, update without the phase, event 004653b0.
    bool game_canvas = false;
    // The HUD's start menu (*004ffc2c): open 002429f0, close 00242900, the
    // busy test 00241ac0 and the Start edge of 0024cd30.
    void hud_open();
    void hud_close();
    // Checkpoint 132hud: the HUD map screen. 002427a0 (the D-pad Left toggle; returns +6a9 after it),
    // 00242870 (open: push GuiMapScreen "MAPSCRN" at layer 0) and 002427f0 (close: pop it).
    bool map_toggle();
    void map_screen_open();
    void map_screen_close();
    bool hud_busy() const;
    void start_press();
    // Checkpoint 132hud: GuiGameScreen's weapon select (the HUD's +6a4 screen, GAMEGUI "weaponselect"):
    // the hold-Select menu 0045ca20 (open / close), its list 0045c760, panels 0045c5e0, step 0045c540,
    // choice 0045c480, input 0045d420, update 0045d960 (the panel fades) and the counts 0045c0f0 draws.
    // The port loads GAMEGUI's root as a plain widget; `game_screen` is that root (the GuiGameScreen's place)
    // and v_input / v_update run 0045d420 / 0045d960 on it. Offsets are GuiGameScreen's.
    struct GameScreen {
        Widget* ws = nullptr;                    // +6c "weaponselect"
        Widget* brackets = nullptr;              // +a0 "brackets"
        Widget* lines = nullptr;                 // +a4 "lines_arrows"
        std::array<Widget*, 5> panels{};         // +a8 the GuiWeaponDisplay panels 0045cc90 adds
        std::array<float, 5> bc{};               // +bc the panels' fade values (0045d960)
        std::vector<std::int32_t> list;          // +70 (+74 count, +78 data): items, -2 the hand (holster)
        std::int32_t sel_90 = -1;                // +90 the selected list index
        std::int32_t sel_94 = -1;                // +94 +90 at the open
        std::int32_t t98 = 0;                    // +98 003698b0 at the open (ms)
        std::uint8_t b9c = 0;                    // +9c stepped since the open
        std::int32_t d0 = 0;                     // +d0 the settled state (0 closed, 1 open, 2 moving)
        std::int32_t d4 = 0;                     // +d4 1: open
        std::int32_t d8 = 0;                     // +d8 00147150 "icon_hand.skl" (the hand icon model)
        bool d8_unhosted = false;                // the host has no model loader for +d8
        std::int32_t dc = 0;                     // +dc the panels shown (min(count, 5))
        std::optional<TextObject> text;          // +48 003d0940's text object (0045dfd0)
        // 0045e140..0045e194: +68 "subtitles" (004c22d0) when it isA "GuiSubTitleDisplay" (004c1d10), else
        // null (checkpoint 132hud3).
        Widget* sub_68 = nullptr;
        // 0045e18c..0045e2e0: the choose menu (cPlayer_LaunchChooseMenu 0045c030; the elevator's
        // ChooseFloor, research/ELEVATORS.md). +e0 is cleared unless all three are found.
        Widget* choose_e0 = nullptr;             // +e0 "choosemenu"
        Widget* title_e4 = nullptr;              // +e4 its "title" (isA GuiTextWidget)
        Widget* choices_e8 = nullptr;            // +e8 its "choices" (isA GuiTextListWidget)
        std::uint32_t receiver_ec = 0;           // +ec the lift the reply goes to (0045beb0; the host's lift id)
        std::uint64_t object_f0 = 0;             // +f0 the launching agent (0045c030's a1; the host's agent key, 0 none)
        std::array<float, 4> pos_100{};          // +100 the agent position at the first 0045bd30
        std::uint8_t b110 = 0;                   // +110 +100 is set
        // 0045e2e8..0045e4e8 (checkpoint 132hud5): +114 "value_codename" (004c2300) when it isA GuiTextWidget
        // (004a7520); +120 "headset_playing" (004c2340, isA GuiTextWidget) and +124 "label_talking" (004c2350,
        // no test), the voice chat's texts 0045d960 updates.
        Widget* codename_114 = nullptr;
        Widget* headset_120 = nullptr;
        Widget* talking_124 = nullptr;
    } gs;
    Widget* game_screen = nullptr;
    // Port-only co-op HUD (checkpoint 132hud5; set by play_main with --coop-partner unless --no-coop-hud): gs_init
    // takes 0045e348's multiplayer branch (byte 005721c8, 0 natively) for +114 with the local agent slot 0 as
    // *00494178, this returning 0012f5d0(004ffb50, 0) (the name slot "PlayerName0"; nullopt: NULL, "" 004c2268).
    // Empty: the single-player branch (+114 hidden).
    std::function<std::optional<std::string>()> gs_coop_codename;
    // The reply 0045bdf0 / 0045bf80 queue (003c9420 transport 0) to +ec: a MSGGAME_MENU_CHOICE (*004f7e48,
    // vtable 004dc330) with +24 = +f0 (the agent) and +28 = the row, or -0xff when closed without a choice.
    std::function<void(std::uint32_t lift, std::uint64_t actor, std::int32_t row)> gs_choice_reply;
    bool gs_choose_shown_bd00() const;         // 0045bd00: +e0 && (+e0 flags & 2)
    void gs_choose_track_bd30(const std::array<float, 4>& pos);  // 0045bd30 (the pad producer at 0x0024c674)
    void gs_choose_confirm_bdf0();             // 0045bdf0
    void gs_choose_lift_beb0(std::uint32_t lift);   // 0045beb0
    void gs_choose_select_bec0(std::int32_t row);   // 0045bec0
    void gs_choose_title_bf10(const std::optional<std::string>& text);  // 0045bf10
    void gs_choose_add_bf40(const std::optional<std::string>& name);    // 0045bf40
    void gs_choose_cancel_bf80();              // 0045bf80 (also Triangle on the shown choose menu)
    void gs_choose_open_c030(std::uint64_t actor);  // 0045c030
    std::uint8_t gs_event_d3d0(Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 0045d3d0
    void gs_init(Widget* screen);              // 0045dfd0's text object and finds (+6c / +a4 / +a0 / +e0..), 0045cc90
    void gs_open_ca20(bool open);              // 0045ca20(gs, open)
    // 0045ce10(gs, name, a2, a3): the HUD subtitle (00245e00 from gameSound's quip start 0x001c9214, a3 0;
    // the PlayXA natives 0045ed80 / 0045ed40 -> 0045edb0, a3 1). A null or empty name hides +68 (clears flag 2);
    // otherwise 003fdb50(name) goes to 00459500 and, when it parsed, +68 gets onWake (when hidden), its
    // line count +78 = 4 and flag 2. a3 != 0 then sends the voice (host subtitle_voice, 0045ceec: 001ca210 /
    // 0020f2a0 / 003c9420 / 003c9d60; checkpoint 132hud4). a2 is not read.
    void gs_subtitle_ce10(const std::optional<std::string>& name, std::int32_t a2, std::int32_t a3);
    void gs_gather_c760();                     // 0045c760
    void gs_fill_c5e0();                       // 0045c5e0
    void gs_step_c540(bool forward);           // 0045c540(gs, dir)
    void gs_choose_c480();                     // 0045c480
    std::uint8_t gs_input_d420(const PadEvent& ev);  // 0045d420
    void gs_update_d960(float dt);             // 0045d960 (after the base update 0041e3b0)
    // 0045c0f0's texts for the drawer: per shown panel with a count, the text and the panel's rect and fade.
    struct GsCount {
        const Widget* panel = nullptr;
        std::string text;
        std::array<float, 4> rect{};
        float bc = 0.0f;
    };
    std::vector<GsCount> gs_counts_c0f0() const;
    // GuiEquipmentModify's weapon box callbacks (the screen *0051ee10): 002c2290 (box 1, +d0) the preview
    // actor +4c's +2470 model at +88 (point 0 when +88 == +fc); 002c21c0 (box 2, +d4) the list's selected
    // item's record +13c (+10c[+108] while +8c and +108 < +104). Both need +84 != 0 and their box, and
    // take record +11c instead when the model's record (+40a) holds class 7 (+f0). 0: nothing drawn.
    std::int32_t em_box_model(const Widget& screen, std::uint32_t callback);
    // Checkpoint 132f6: 002bfa90 at 0x002bfe18 (the figure callback 002c2340's draw): +68 eases to
    // +ac[+84] by an eighth of the difference when they differ; returns the new +68.
    float em_figure_turn(const Widget& screen);

    // 0041e0f0(sender, target, type, a3, t0).
    std::uint8_t send(Widget* sender, Widget* target, std::uint32_t type, std::int32_t a3, std::int32_t t0 = 0);
    // 002cca40(app, type, a1, a2): 0041e0f0(canvas, w, type & 0xffff, a1, a2)
    // for each screen of a copy of the canvas's stack that is still on it.
    void app_broadcast(std::uint32_t type, std::int32_t a1, std::int32_t a2);
    // Called at the entry of every send (before the null-target test).
    std::function<void(Widget* sender, Widget* target, std::uint32_t type, std::int32_t a3)> send_observer;
    // Called at the entry of every send_up 0041e040.
    std::function<void(Widget* w, std::uint32_t type, std::int32_t a3, std::int32_t t0)> up_observer;
    // Called for each child 0041df00 (broadcast) sends to (it calls the
    // child's event slot directly, not through 0041e0f0).
    std::function<void(Widget* sender, Widget* target, std::uint32_t type, std::int32_t a3)> broadcast_observer;
    // Host hook (no EE counterpart): update 00465500 calls it after controller_check 00465870 and before
    // update_stack 00415950 (the GUI decor's screens' update starts there: the no-controller popup's open /
    // close and focus change precede every element step, checkpoint 132win6).
    std::function<void()> stack_observer;
    // Port bookkeeping (no EE counterpart): bumped on every Decorator::matrix write (creation 00417bc0 and
    // event 0x12 004176c0) so the GUI decor can skip its matrix copy pass when nothing moved.
    std::uint64_t deco_matrix_serial = 0;
    // True while update_stack 00415950 runs the screens' updates.
    bool updating_screens() const { return iterating_; }
    // Host hook (no EE counterpart): called after 004191d0 read a file into
    // `w` (the GUI decor attaches 004178e0's elements to the loaded tree).
    std::function<void(Widget* w, const GuiNode& file_root)> load_observer;
    // The interface-element half of GuiInterfaceDecorator (vtable 004e09f0),
    // installed by the GUI decor's bridge (null: no element, as before):
    //   deco_wake    00416b30: the "onWake" record after the widget's wake
    //                (vt +1c, which wakes its children and, for a screen,
    //                runs the screen's own wake first);
    //   deco_sleep   00416b80: the "onSleep" record after the widget's sleep;
    //   deco_settled 00417400: event 0xf's answer (the last record is not a
    //                fade, or the element's +4 reached cvt(b));
    //   deco_step    00417790: 003f43d0(el, dt) before the sleep wait and
    //                the widget's own update.
    std::function<void(Widget* w)> deco_wake;
    std::function<void(Widget* w)> deco_sleep;
    std::function<std::uint8_t(Widget* w)> deco_settled;
    std::function<void(Widget* w, float dt)> deco_step;
    //   deco_named   the named record of GuiWidget_OnActivate 00416c70
    //                ("onActivate") / GuiWidget_OnDeactivate 00416cc0
    //                ("onDeactivate"), after the widget's 0041f080 / 0041f070.
    std::function<void(Widget* w, const char* record)> deco_named;

    // GuiKeyboard (vtable 004de790): 0032b4f0(kb, title, text, target, max,
    // +58, +e0), GuiAgentCreate's call shape, and 0032b2d0(kb, accept).
    void kb_open(Widget* kb, const std::optional<std::string>& title, const std::optional<std::string>& text,
                 Widget* target, std::int32_t max, std::int32_t arg, std::uint8_t e0);
    void kb_close(Widget* kb, bool accept);

    CharMap charmap;               // 0055ca90 (+9c0 = byte 0055d450 ready)
    std::int8_t lang = 0;          // byte 0055d470 (003fe0f0)
    std::uint8_t kb_a701 = 0;      // byte 0055a701 (the USB keyboard is attached)
    std::uint32_t kb_mod = 0;      // 0055a700 +94: the USB keyboard modifier word

    Widget& canvas() { return canvas_; }
    const Widget& canvas() const { return canvas_; }
    Widget* top() const { return canvas_.children.empty() ? nullptr : canvas_.children.back(); }
    Widget* find(Widget* w, const std::string& name) const;  // 0041db80
    // 004147b0 (guard byte 00571fc0): the canvas *00571fb0 destructor
    // 00465e10 -> 00415f00 pops (vt +68 = 00414be0) the stack's last screen
    // until it is empty, then the GuiWidget destructor 0041f370 frees the
    // canvas. 00414900 (from 002cb8c0) constructs a new one (0041f960 +
    // 004158c0), which canvas_create models as the constructor's canvas.
    void canvas_destroy();
    void canvas_create();
    bool canvas_alive() const { return canvas_alive_; }

    // Globals the runtime reads and writes.
    struct Globals {
        std::array<std::array<std::int32_t, 16>, 2> repeat_count{};  // 00571fe0
        std::array<std::array<std::int32_t, 16>, 2> press_time{};    // 00572060
        Widget* dispatch_top = nullptr;   // 00571fd8
        Widget* next_from = nullptr;      // 00572138
        Widget* prev_from = nullptr;      // 00572140
        std::uint8_t card_ok = 0;         // 004ffbb8
        std::uint32_t card_poll = 0;      // *0055a838 (00441c10 while a save flow polls the card, 00293650 in the format update)
        std::int8_t app_state = 0;        // 004ffba0
        std::int8_t app_state_prev = 0;   // 004ffba1
        std::uint8_t keyboard = 0;        // 004ffbb1 (idle attract enable)
        std::int32_t card_slot = -1;      // 004ffbbc
        std::int32_t no_pad_frames = 0;   // 00587b20
        std::int32_t no_pad_popup = -1;   // 00497be8
        std::uint8_t card_checked = 0;    // 00506330
        std::uint8_t sound_gate = 0;      // 00587b38
        std::uint8_t debug_triangle = 0;  // 0051eeb0
        std::int32_t pad_state = 6;       // 0055a4ec
        std::uint32_t t1_base = 0;        // 00533870
        std::int32_t app_attract = 0;     // app +13c
        std::string app_mode;             // app +39
        std::int32_t app_loaded = 1;      // app +260
        bool controller = false;          // *004ffc2c != 0
        std::uint8_t controller_6a8 = 0;  // *004ffc2c +6a8
        CardState card;
        std::uint8_t app_online = 0;      // app +2e (GuiAgentLoad: command centre, else online / offline select)
        std::uint8_t app_2f = 0;          // app +2f (cleared by GuiAgentLoad's wake)
        // GuiOnlineOfflineSelect: read by init 002a0970 / wake 002a06c0 / update 002a05b0
        std::uint8_t b_6488 = 0;          // byte 00506488 (copied to the screen's +48)
        std::uint8_t b_6490 = 0;          // byte 00506490 ("lan" visible when +48 is 0)
        std::uint8_t b_eea0 = 0;          // byte 0051eea0 (set by 00165bf4: "online" disabled)
        std::uint8_t b_eea8 = 0;          // byte 0051eea8 (set by 00165bd0 / 002ca760: "lan" disabled)
        std::uint8_t app_31 = 0;          // app +31 ("online" disabled)
        // ... and written by its event 002a01c0 (002cca20(app, v): byte 005723d0 = 0, app +30 = v)
        std::uint8_t app_30 = 0;          // app +30
        std::uint8_t app_32 = 0;          // app +32
        std::uint8_t b_23d0 = 0;          // byte 005723d0
        std::uint8_t b_2fe8 = 0;          // byte 00532fe8
        // PORT-ONLY (checkpoint 132ln1, not the original; research/LAN_PLAY.md): set by the LAN Play entry
        // ("lan" with GuiHost::port_lan_play), cleared where 002cca20 runs (the online / offline entries).
        // The LAN-mode screens read it; it never reaches a game record.
        std::uint8_t lan_mode = 0;
        // PORT-ONLY (checkpoint 132ln4, the coordinator's decision on the Join route): ONLINECN's JOIN LAN GAME row
        // goes to the command centre in online mode like HOST LAN GAME (the navigator's op 1); this byte makes the
        // command centre's activate seed its mission panel's focus with join_mission (cc_lan_activate), so the
        // player picks a mission and join_mission opens JOINMIS through op 2 with its LAN check, as the original.
        std::uint8_t lan_join_focus = 0;
        // GuiOnlineConnect (checkpoint 132ln2)
        std::uint8_t app_2a = 0;          // app +2a (wake 0029ed40 clears it, sleep 0029ecd0 sets it)
        std::uint8_t app_265 = 0;         // app +265 (wake: the agent's +9a0 is -1, no online account)
        // Checkpoint 132on2: the policy download 002ca350's lists (app +2ec set: allocated) and app +26c / +2ac
        std::uint8_t app_2ec = 0;                                  // app +2ec
        std::vector<std::string> policy_usage, policy_privacy;     // app +2f0 / +2f4 (0x100-byte chunks)
        std::vector<LobbyAnnouncement> announcements;              // app +2f8
        std::string app_26c;                                       // app +26c (002ca100: the universe, 0x40)
        std::string app_2ac;                                       // app +2ac (002ca0e0: "" uses 0048c138)
        // GuiMissionSetup (checkpoint 132ln3): the app ctor 002cc5a0 stores +180 = 4, +185 = 0, +254 = 4
        std::int32_t app_180 = 4;         // app +180 (the agent count, num_agents)
        std::uint8_t app_185 = 0;         // app +185 (the join filter, join_filter)
        std::int32_t app_254 = 4;         // app +254 (begin_mission copies +180)
        // The lobby's create-game result (event 0x1e to GuiMissionSetup, 002a126c; cleared by its activate,
        // begin_mission and the result paths 002a0dc0 / 002a0e80)
        std::uint8_t r_64a8 = 0;          // byte 005064a8 (the result arrived)
        std::int32_t r_64b0 = 0;          // 005064b0 (the event's a3: nonzero created)
        std::int32_t r_64b8 = 0;          // 005064b8 (the event's t0: the error text's key)
        // GuiLobbyScreen (checkpoint 132ln4): the lobby's join callback (event 0x1e to a visible lobby screen
        // with app +32 clear, 0033e208; cleared by wake 0033f110 and the join 0033e5b0's end)
        std::uint8_t r_2ff0 = 0;          // byte 00532ff0 (the result arrived)
        std::int32_t r_2ff8 = 0;          // 00532ff8 (the event's a3: nonzero joined)
        std::int32_t r_3000 = 0;          // 00533000 (the event's t0: the error text's key, a string address)
        std::int32_t b_2fd0 = 0;          // 00532fd0 (0033cef0: the net screen's +84 before NETMESS)
        std::int32_t app_268 = 0;         // app +268 (002a8d60: 2 before the navigator's op 5)
        // Checkpoint 132on2b: GuiAccountLogin
        std::uint8_t app_264 = 0;         // app +264 (logged in: 002ca640 logs out)
        std::string acl_pass, acl_user;     // 00506340 / 00506350 (0x10 each: kept over the region choice)
        std::uint8_t acl_kb_ok = 0;        // byte 00506360 (+a4 kept over the region choice)
        std::uint8_t acl_busy = 0;         // byte 00506378 (the login button's reentrancy guard)
        std::uint8_t reg_cached = 0;      // byte 0051ef60 (002ca070: the setting was read)
        std::uint8_t reg_enabled = 0;     // byte 0051ef58 (LOBBY_DISABLE_REGISTER is not "1")
        std::int32_t ml_next_id = -1;     // 0048bcb0 (002ac070 numbers id-0 records downwards from -1)
        std::string iface = "frontend";   // manager +2c
        // GuiCommandCenter: its saved state (00298420 / 00298360 / 002982c0) and what it writes
        std::uint8_t cc_panel = 0;                     // byte 0048b7a0
        std::int32_t cc_sel = -1;                      // 0048b7a4
        std::array<std::string, 6> cc_names;           // 0048b7a8 + 0x20 k (0x1f characters)
        std::uint8_t cc_want_bonus = 1;                // byte 0048b868 (cleared by levels 00297020)
        std::uint8_t b_bb5 = 1;                        // byte 004ffbb5 (0: every gated level is open)
        std::array<std::int32_t, 18> bonus_row{};      // 005063d0 (the level's bonus vector row, -1 none)
        std::string level_name = "TEST";               // 004ffc90 (0x80; "TEST" in the boot state)
        std::string app_1c8;                           // app +1c8 (the shown level text)
        std::uint8_t app_33 = 0, app_34 = 0, app_35 = 0;  // app +33..+35
        std::int32_t level_494198 = -1;                // 00494198
        std::string app_188;                           // app +188 (002cc820)
        std::int32_t app_248 = -1, app_24c = -1;       // app +248 / +24c
        std::uint8_t app_250 = 0, app_251 = 0;         // app +250 / +251
        // In play (checkpoint 132f)
        std::uint8_t b_bb7 = 0;                        // byte 004ffbb7 = app +67: the world is paused (00131470)
        std::uint8_t b_21c8 = 0;                       // byte 005721c8: a network game
        std::uint8_t hud_6a9 = 0;                      // *004ffc2c +6a9
        // GuiGameOptions (checkpoint 132f2)
        GameOptionsRecord options;                     // *004ffd2c (the live options record)
        std::int32_t gamma_word = 0;                   // 005383c0 (0045a3a0 bit 0x400: cvt_w(191.25 * gamma) & 0xff)
        // byte 005721c0 (a hosted network game: the host role). Net_StartSession 0042a870's create (0042a980 ->
        // NetGame_Create 002ea788) stores 1 at 0042aac8 on success; the connect (0042a960 / 0042ab00) leaves it.
        // In LAN mode (checkpoint 132ln4) the LAN host of MSNSETUP's begin_mission stands in for 0044e960's
        // 0042a870(0).
        std::uint8_t b_21c0 = 0;
        // GuiAgentModify (checkpoint 132cc4b)
        std::array<omega::AgentPacking, 2> packings{};  // 0055d510 (0x440 each; the agents' appearances)
        std::int8_t packing_slot = -1;                 // byte 00493a60 (the edited packing, -1 none)
        std::array<omega::AgentModifyCategory, 16> am_table{};  // 00492a00 (the categories' kind / pair / position)
        omega::AgentCreateTable ac_table = omega::agent_create_table_initial();  // 0048b500 (GuiAgentCreate, 132cc5)
        std::uint32_t pad_held = 0;                    // 0055a44c (port 0's held buttons; 00342cd0 reads bits 0 / 1)
    } g;

    // Canvas state.
    float phase = 0.0f;            // +6c
    float last_dt = 0.0f;          // +64
    Widget* pending_pop = nullptr; // +48
    Widget* pending_push = nullptr;// +4c
    std::uint8_t pending = 0;      // +50
    std::uint8_t keyboard_path = 0;// +51 (USB keyboard input 004153f0)
    std::uint8_t ascii_keys = 1;   // +52 (keyboard codes through 003d1580)
    std::uint32_t repeat_mask = 0xf000;  // +68

    // GuiTextEditWidget edit paths (the keyboard screens' callbacks).
    std::int32_t edit_cursor(Widget* w, std::int32_t a);           // 0041a100
    std::uint8_t edit_insert(Widget* w, const std::string& s);     // 0041a2c0
    std::uint8_t edit_delete(Widget* w, bool forward);             // 0041a180
    // 003d1580: a USB HID key code + modifier word to the character
    // (0 = none) through the tables 004937fc / 0049383c.
    static std::uint8_t usbkb_char(std::uint8_t code, std::uint32_t mod);

    // GuiArrayWidget / GuiTextListWidget services the screens call.
    void array_set_dims(Widget* w, std::uint32_t cols, std::uint32_t rows);  // vt +5c 00424750
    void array_select(Widget* w, std::uint32_t col, std::uint32_t row);     // 00424430
    void array_set_row_height(Widget* w, std::uint32_t h);                  // 00424820
    void array_set_col_width(Widget* w, std::uint32_t col, std::uint32_t width);  // 004248c0
    std::uint8_t array_cell_visible(const Widget* w, std::uint32_t col, std::uint32_t row) const;  // 004245d0
    void array_set_visible(Widget* w, std::uint32_t cols, std::uint32_t rows);  // 004235b0 (+70 / +74)
    void array_set_origin(Widget* w, std::uint32_t left, std::uint32_t top);    // 00423580 (+78 / +7c)
    void array_set_a84(Widget* w, std::int32_t v);                             // 00423550
    std::uint8_t list_add(Widget* w, const std::optional<std::string>& text, std::int32_t id, std::int8_t style);  // 00421290
    void list_clear(Widget* w);                                             // 00421070
    std::uint8_t list_remove(Widget* w, std::int32_t index);                // 004210c0
    void list_refresh(Widget* w);                                           // 004213e0
    std::optional<std::string> list_text(const Widget* w, std::int32_t index) const;  // 00421430
    std::int32_t list_id(const Widget* w, std::int32_t index) const;        // 00421480
    void list_set_style(Widget* w, std::int32_t index, std::int8_t style);  // 004214c0
    std::int32_t list_style(const Widget* w, std::int32_t index) const;     // 00421500
    std::int32_t list_find(const Widget* w, const std::string& text) const; // 00421540
    std::int32_t list_find_id(const Widget* w, std::int32_t id) const;      // 004215e0
    void list_set_f1(Widget* w, std::uint8_t v);                            // 00421650
    void list_set_a4(Widget* w, std::uint8_t v);                            // 00421670
    void list_set_font(Widget* w, const std::optional<std::string>& name);  // 00421690
    void set_text(Widget* w, const std::string& text, bool translate);      // 0041beb0
    void set_rgba(Widget* w, const std::array<float, 4>& c);  // 0041c070

    const std::vector<std::unique_ptr<Widget>>& widgets() const { return widgets_; }

    // GuiScrollBarWidget (init 0043be70, wake 0043bde0)
    void sb_init(Widget* w);
    void sb_frame(Widget* w, std::uint32_t type);  // the frame broadcast of 0043bbc0 / 0043bc70 / 0043bd50 / 0043bde0
    void sb_set_value(Widget* w, std::int32_t v);  // 0043bbc0
    void sb_set_range(Widget* w, std::uint8_t b54, std::int32_t v, std::int32_t x, std::int32_t y);  // 0043bc70
    // GuiMLTextWidget (init 0043e330, deinit 0043e0e0, wake 0043e2e0, draw 0043d8b0)
    void ml_init(Widget* w);
    void ml_sync(Widget* w);                       // 0043e290 (the scroll bar range)
    void ml_down(Widget* w, bool page);            // 0043d760
    void ml_up(Widget* w, bool page);              // 0043d820
    void ml_clear(Widget* w);                      // 0043db80
    void ml_set_text(Widget* w, const std::string& s);  // 0043dce0 (appends the word-wrapped lines)
    void ml_set_font(Widget* w, const std::string& name);  // 0043e200 (the font, the line count by its height)
    // GuiBriefingPane (init 00456020, wake 00455cb0, input 00455e10, draw 00455db0)
    void br_init(Widget* w);
    void br_wake(Widget* w);
    std::uint8_t br_input(Widget* w, const PadEvent& ev);
    // 00455db0 before its base draw: 0043e1a0(+48, flags & 4 ? 00587d20 : 00587d30).
    // The drawer applies it to the text's draw (the runtime is const there).
    static std::array<float, 4> br_text_colour(std::uint16_t flags);
    // GuiRadioLog (init 00454910, sleep 00454a80, input 00454150, update
    // 00454380, event 00454440)
    void rl_init(Widget* w);
    void rl_sleep(Widget* w);
    std::uint8_t rl_input(Widget* w, const PadEvent& ev);
    void rl_update(Widget* w, float dt);
    std::uint8_t rl_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    void rl_refill(Widget* w);                     // 00454280
    void rl_stop(Widget* w);                       // 00454630
    void rl_play(Widget* w, const std::string& name);  // 004546c0
    // GuiSubTitleDisplay (wake 00458140, sleep 00458080, update 00457fe0,
    // draw 00457ae0)
    void st_wake(Widget* w);
    void st_sleep(Widget* w);
    void st_update(Widget* w, float dt);
    void st_clear(Widget* w);                      // 00459450
    std::uint8_t st_set_text(Widget* w, const std::optional<std::string>& s);  // 00459500
    std::uint8_t st_parse(Widget* w);              // 00458a30
    void st_timings(Widget* w);                    // 00458900
    // GuiSlideShow (vtable 004de710: init 00327fd0, wake 00327d90, sleep
    // 00327cf0, input 00327960, draw 00327910; the rest GuiWidget's).
    void ss_init(Widget* w);
    void ss_wake(Widget* w);
    void ss_sleep(Widget* w);
    std::uint8_t ss_input(Widget* w, const PadEvent& ev);
    void ss_return(Widget* w, const std::string& cls, const std::string& file, std::int32_t arg);  // 00327c70
    void ss_clear(Widget* w);                       // 00328630
    std::int32_t ss_add_category(Widget* w, const std::optional<std::string>& name);  // 00328310
    std::int32_t ss_add_slide(Widget* w, std::int32_t cat, const std::string& image, std::uint32_t archive,
                              const std::string& voice);  // 00328140
    // 00326de0: the level's five categories of ZEUS files.
    void ss_load(Widget* w, std::int32_t level, std::int32_t cat, std::int32_t slide, std::uint8_t only_unlocked);
    std::uint8_t ss_step(Widget* w, bool forward);  // 003272b0
    void ss_refresh(Widget* w);                    // 00327890
    void ss_show(Widget* w, std::int32_t cat, std::int32_t slide);  // 00327530
    std::int32_t ss_total(const Widget* w) const;
    // GuiBitmapWidget (vtable 004e0fc0: deinit 0043ca20, wake 0043caf0,
    // sleep 0043caa0, draw 0043c0f0; input 0 0041df70).
    std::uint8_t bm_set_image(Widget* w, const std::string& name, std::uint32_t archive);  // 0043c890
    std::uint8_t bm_load(Widget* w);               // 0043c730
    void bm_release(Widget* w);                    // the 003813a0 of +48, +48 = -1
    // GuiTextArrayWidget (init 004262d0, wake 004261d0, sleep 00426100)
    void ta_init(Widget* w);
    void ta_wake(Widget* w);
    void ta_sleep(Widget* w);
    void ta_set_dims(Widget* w, std::uint32_t cols, std::uint32_t rows);   // vt +5c 00426350
    void ta_set_cell(Widget* w, std::uint32_t col, std::uint32_t row, const std::optional<std::string>& s);  // 00426510
    void ta_set_cell_rgba(Widget* w, std::uint32_t col, std::uint32_t row, const std::array<float, 4>& rgba);  // 00425c30
    void ta_set_cell_halign(Widget* w, std::uint32_t col, std::uint32_t row, std::uint8_t v);  // 00425cd0
    void ta_set_font(Widget* w, const std::string& name);                    // 00425e20
    void ta_set_halign(Widget* w, std::uint8_t v);                           // 00425e00
    const TextArrayCell* ta_cell(const Widget* w, std::uint32_t col, std::uint32_t row) const;  // 00426590
    // GuiWeaponStats (init 00455b60, input 004557c0, fill 00455840, cell
    // colour 00455ae0)
    void ws_init(Widget* w);
    std::uint8_t ws_input(Widget* w, const PadEvent& ev);
    void ws_fill(Widget* w, std::int32_t item);
    static std::array<float, 4> ws_cell_colour(std::uint32_t col);
    // GuiWeaponDisplay (init 00441180, set 00441060 / 00441000, update 00440ee0)
    void wd_init(Widget* w);
    void wd_set(Widget* w, std::int32_t item);
    void wd_set_item(Widget* w, std::int32_t item, std::int32_t model, const std::optional<std::string>& name);
    void wd_update(Widget* w, float dt);
    // GuiEquipmentPane (init 00455610, wake 00455550, input 00454b10, cycle
    // 00454f80, set 00455260)
    void ep_init(Widget* w);
    void ep_wake(Widget* w);
    std::uint8_t ep_input(Widget* w, const PadEvent& ev);
    void ep_cycle(Widget* w, bool up);
    void ep_set(Widget* w, std::int32_t item);
    // GuiScoresEquip (init 0035cd00, wake 0035cbf0, input 0035aec0, event
    // 0035b210, update 0035bfb0)
    void se_init(Widget* w);
    void se_wake(Widget* w);
    std::uint8_t se_input(Widget* w, const PadEvent& ev);
    std::uint8_t se_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    void se_update(Widget* w, float dt);
    void se_view(Widget* w, std::int32_t v);  // the groups and the button text for +224 = v
    // Port-only (checkpoint 132f5): the Cheats screen PortGuiCheats.
    void pc_init(Widget* w);
    void pc_wake(Widget* w);
    std::uint8_t pc_input(Widget* w, const PadEvent& ev);
    std::uint8_t pc_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    void pc_refresh(Widget* w);
    // Port-only (checkpoint 132win11): PortGuiSettings.
    void ps_init(Widget* w);
    void ps_wake(Widget* w);
    void ps_update(Widget* w, float dt);
    std::uint8_t ps_input(Widget* w, const PadEvent& ev);
    std::uint8_t ps_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    void ps_refresh(Widget* w);
    void ps_leave(Widget* w);
    // GuiEquipmentModify (ctor 002c1100, init 002c0790, input 002bcb50,
    // event 002bd560, dtor 002c0f00).
    void em_build(Widget* w);              // the ctor 002c1100's player copy
    void em_init(Widget* w);
    std::uint8_t em_input(Widget* w, const PadEvent& ev);
    std::uint8_t em_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    Widget* em_button(Widget* w, std::int32_t cat);  // the +84 category's button (isA GuiButtonWidget)
    void em_focus_list(Widget* w, Widget* button);   // the list focus of 002bcb50's Right / 002bd560's 2
    void em_place(Widget* w, std::int32_t model);    // 002bd560's 0x13 attach-table edit
    void em_build_category(Widget* w);     // 002bf090
    void em_build_equip(Widget* w);        // 002bf370
    void em_list(Widget* w);               // 002beee0
    void em_preload_window(Widget* w, std::int32_t n);  // 002bded0
    void em_ring_touch(Widget* w, std::int32_t item);   // 002bdcf0
    void em_ring_release(Widget* w);       // 002bdc10
    void em_clear(Widget* w);              // 002bdf90
    void em_column1(Widget* w);            // 002be910
    void em_column2(Widget* w);            // 002be380
    // The five value texts of one column ('1' / '2'); nullopt clears them.
    void em_fill(Widget* w, char column, const std::optional<GuiHost::WeaponRecord>& rec, std::uint32_t at);
    // Owner (decorator or own) dispatch of the virtual slots.
    void o_init(Widget* w);
    void o_wake(Widget* w);
    void o_sleep(Widget* w);
    void o_gain(Widget* w);
    void o_lose(Widget* w);
    void o_activate(Widget* w);
    void o_update(Widget* w, float dt);
    std::uint8_t o_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    // The widget's own vtable.
    void v_init(Widget* w);
    void v_wake(Widget* w);
    void v_sleep(Widget* w);
    std::uint8_t v_input(Widget* w, const PadEvent& ev);
    void v_update(Widget* w, float dt);
    std::uint8_t v_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    Widget* v_first(Widget* w);
    Widget* v_last(Widget* w);
    Widget* v_next(Widget* w, Widget* from, bool reset);
    Widget* v_prev(Widget* w, Widget* from, bool reset);

    void base_init(Widget* w);      // 0041f150
    void base_wake(Widget* w);      // 0041efa0
    void base_sleep(Widget* w);     // 0041ebf0
    void base_update(Widget* w, float dt);  // 0041e3b0
    std::uint8_t base_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 0041ef10
    void check_sleep(Widget* w);    // 0041ed20
    void clear_awake(Widget* w);    // 0041ee50
    std::uint8_t send_up(Widget* w, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 0041e040
    // The same parent walk inlined into 0041e330 / 0041e2b0 / 0041e230 (type 0 / 1 / 2, no arguments).
    std::uint8_t bubble(Widget* w, std::uint32_t type);
    std::uint8_t broadcast(Widget* w, std::uint32_t type, std::int32_t a3, std::int32_t t0); // 0041df00
    void text_wake(Widget* w);      // 0041c3e0
    void text_sleep(Widget* w);     // 0041c310
    void v_set_text(Widget* w);     // vt +5c (0041be50 / 0041aa10)
    void text_changed(Widget* w);   // 0041be50
    void edit_mask(Widget* w, std::size_t n);         // 0041ae90
    std::uint8_t edit_input(Widget* w, const PadEvent& ev);  // 0041aa80
    void keyboard_input();                            // 004153f0
    void edit_update(Widget* w, float dt);            // 0041a8f0
    void set_b74(Widget* w, std::uint8_t v);          // 0041c110
    Widget* find_sub(Widget* w, const std::string& part) const;  // 0041dc70
    void deinit(Widget* w);         // 0041f090

    void msgbox_init(Widget* w);    // 002a8410
    void msgbox_wake(Widget* w);    // 002a86f0
    void msgbox_sleep(Widget* w);   // 002a86a0
    std::uint8_t msgbox_input(Widget* w, const PadEvent& ev);  // 002a7b00
    std::uint8_t msgbox_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 002a7c20
    Widget* msgbox_target(Widget* t) const;  // 002a8800
    void destroy(Widget* w);        // the destructor (vt +8) + free of 00418e00 / 00419270

    std::uint8_t title_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 00287e00
    void title_update(Widget* w, float dt);    // 00288060
    void title_wake(Widget* w);                // 002882b0
    void title_card_check(Widget* w, bool force);  // 00288310
    float now_seconds();

    std::uint8_t deco_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 004173b0

    void kb_init(Widget* w);                      // 0032cb40
    void kb_layout(Widget* w, std::int32_t lang); // 0032d0d0
    void kb_relabel(Widget* w, std::uint32_t mods);  // 0032b5f0
    static std::int32_t kb_mod_index(const Widget* w, const kbd::Entry& e);  // 0032c2d0
    // The character of a layout entry under the current modifiers, shared
    // by 0032ba10 / 0032c390: a dead key arms +98 (0xff), any other code
    // goes through the pending dead key's pairs and the char map, disarming it.
    std::uint8_t kb_resolve(Widget* w, const kbd::Entry& e);
    std::uint8_t kb_label_char(const Widget* w, const kbd::Entry& e) const;  // 0032b5f0's lookup
    std::uint8_t kb_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 0032c390
    std::uint8_t kb_input(Widget* w, const PadEvent& ev);  // 0032ba10
    void kb_update(Widget* w, float dt);          // 0032c860
    void kb_sync(Widget* w);                      // 0032afe0
    void kb_sleep(Widget* w);                     // 0032c9a0
    void kb_wake(Widget* w);                      // 0032ca00
    bool kb_char_ok(const Widget* w, std::uint8_t ch) const;  // 0032af10
    Widget* kb_find_id(const Widget* w, std::int32_t id) const;  // 0032b260
    bool kb_shift_key(const Widget* w, const Widget* key) const;  // 0032af70
    void kb_focus(Widget* w, Widget* key);        // the canvas focus change of 0032ba10 / 0032ca00

    void v_gain(Widget* w);                       // vt +24
    void v_lose(Widget* w);                       // vt +28
    void array_init(Widget* w);                   // 00424bc0
    void array_vt6c(Widget* w, std::uint32_t col, std::uint32_t row);  // vt +6c (004235e0 / 002ac300)
    void array_wake(Widget* w);                   // 00424a90
    void array_scroll(Widget* w, std::uint32_t col, std::uint32_t row);  // 00424670
    std::uint8_t array_input(Widget* w, const PadEvent& ev);  // 00423cc0
    void list_init(Widget* w);                    // 004218f0
    void list_wake(Widget* w);                    // 004217d0
    void list_sleep(Widget* w);                   // 00421700
    void set_pos(Widget* w, float x, float y);    // 0041eba0

    void al_wake(Widget* w);                      // 00292dd0
    void al_slots(Widget* w);                     // 00292e80
    void al_update(Widget* w, float dt);          // 00293660
    std::uint8_t al_input(Widget* w, const PadEvent& ev);  // 00292ad0
    std::uint8_t al_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 00292b90
    std::uint8_t save_widget_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 00441ca0

    void oo_init(Widget* w);                      // 002a0970
    void oo_wake(Widget* w);                      // 002a06c0
    void oo_update(Widget* w, float dt);          // 002a05b0
    void oo_online_button(Widget* online);        // the block 002a05e4 / 002a0774 (and 002a0860)
    void oo_lan_button(Widget* lan);              // PORT-ONLY (132ln1): the LAN Play button's enable gate
    std::uint8_t oo_input(Widget* w, const PadEvent& ev);  // 002a0380
    std::uint8_t oo_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 002a01c0

    // GuiCommandCenter (vtable 004dd4f0)
    void cc_init(Widget* w);                      // 00297c10
    void cc_wake(Widget* w);                      // 00297b10
    void cc_sleep(Widget* w);                     // 0033d360
    void cc_activate(Widget* w);                  // 002973b0
    void cc_deactivate(Widget* w);                // 00297350
    void cc_levels(Widget* w);                    // 00297020
    void cc_bonus_flags(Widget* w);               // 002972e0
    void cc_save(Widget* w);                      // 00298420
    void cc_restore(Widget* w);                   // 00298250
    void cc_clear(Widget* w);                     // 002982c0
    void cc_reset(Widget* w);                     // 00294ab0
    std::int32_t cc_default(Widget* w);           // 002952c0
    Widget* cc_first(Widget* w);                  // 00294250
    void cc_panel(Widget* w, std::int8_t p);      // 00294300
    bool cc_movies(Widget* w);                    // 002943f8 (false: a null movie button ends 00294300)
    void cc_play(Widget* w, Widget* sender);      // 002963e4
    void cc_split(Widget* w, const std::optional<std::string>& s);  // 00294b20
    void cc_texts(Widget* w);                     // 00294c60
    void cc_move(Widget* w, std::uint32_t mask);  // 00294f50
    void cc_select(Widget* w, std::int32_t idx, bool reset_panel);  // 00295090
    std::uint8_t cc_input(Widget* w, const PadEvent& ev);  // 00295640
    void cc_exit(Widget* w);                      // 002955b0
    void cc_leave(Widget* w);                     // 00295430
    void cc_update(Widget* w, float dt);          // 00296660
    void cc_circle(Widget* w, float dt);          // 00296a80
    void cc_show_bonus(Widget* w, std::int32_t row);  // 00296b70
    void cc_level_block(std::int32_t idx);        // 002cc820
    std::uint8_t cc_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 002958b0
    std::uint8_t net_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 0033cf90
    Widget* v_first_base(Widget* w);              // 0041d8c0
    // GuiObjectives (vtable: init 003688c0, wake 00368760, input 00368300, update 00368620, event 003680a0)
    void ob_init(Widget* w);
    void ob_wake(Widget* w);
    void ob_update(Widget* w, float dt);
    void ob_clock(Widget* w);
    void ob_fill(Widget* w);                                   // 00367d60
    void ob_tab(Widget* w, std::int32_t tab, bool a2);         // 00367b20
    std::uint8_t ob_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    std::uint8_t ob_input(Widget* w, const PadEvent& ev);
    // GuiStartMenu (init 00456bf0, wake 00456ab0, sleep 00456a30, input 004566d0, event 00456830, first 00456a60)
    void sm_init(Widget* w);
    void sm_wake(Widget* w);
    void sm_sleep(Widget* w);
    Widget* sm_first(Widget* w);
    std::uint8_t sm_input(Widget* w, const PadEvent& ev);
    std::uint8_t sm_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    void sm_label(Widget* w, std::int32_t idx);
    void sm_select(Widget* w, std::int32_t idx);   // 004562e0
    void sm_back(Widget* screen);                  // 00456160
    void sm_exit(Widget* screen);                  // 00456100
    // GuiStartMenuBackdrop (init 00456f90, wake 00456ed0, sleep 00456e60, input 00293950, event 00456f40)
    void bd_init(Widget* w);
    void bd_wake(Widget* w);
    void bd_sleep(Widget* w);
    std::uint8_t game_canvas_event(Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 004653b0
    // GuiMapScreen (vtable 004e18c0: init 00464b70, wake 00464b30, sleep 00464b00, input 004648c0, draw 00464a90;
    // update 0041e3b0 and event 0041ef10 are the GuiWidget entries). Checkpoint 132hud.
    void mp_init(Widget* w);
    void mp_wake(Widget* w);
    void mp_sleep(Widget* w);
    std::uint8_t mp_input(Widget* w, const PadEvent& ev);
    // GuiSliderWidget (init 0043f850, gain 0043f7a0, lose 0043f6f0, input 0043f200)
    void sl_init(Widget* w);
    void sl_gain(Widget* w);
    void sl_lose(Widget* w);
    std::uint8_t sl_input(Widget* w, const PadEvent& ev);
    void sl_frame(Widget* w);                      // the frame broadcast of 0043f150 / 0043f6f0 / 0043f7a0
    void sl_set(Widget* w, std::uint8_t orient, std::int32_t v, std::int32_t min, std::int32_t max,
                std::int32_t step);                // 0043f150
    // GuiGameOptions (init 0029b360, wake 0029b1e0, sleep 0029b070, input 00299b90,
    // update 0029b920, event 0029a190)
    void go_init(Widget* w);
    void go_wake(Widget* w);
    void go_sleep(Widget* w);
    void go_update(Widget* w, float dt);
    std::uint8_t go_input(Widget* w, const PadEvent& ev);
    std::uint8_t go_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    void go_leave(Widget* w);                      // 00299780
    void go_menu(Widget* w);                       // 00299800 (back to the option buttons)
    void go_enter(Widget* w);                      // 002998b0 (into the selected group)
    void go_hints(Widget* w);                      // 002999b0
    void go_show(Widget* w, std::int32_t idx);     // 00299aa0
    void go_refresh(Widget* w, std::uint32_t mask);  // 0029aa50
    void go_close(Widget* w, bool save);           // 0029ad10
    void go_video(Widget* w);                      // 002c9860(app, 1); 0045a3a0(+b8, 0x800); 002c9510(app)
    void go_video_revert(Widget* w, bool close_popup);  // 002996d0
    void save_start(Widget* w);                    // 00441a10 (options mode 4, command centre mode 2)
    void save_done(Widget* w, std::uint8_t result);  // vtable +5c (0029ad00 / 00295430 / 00293890)
    SaveWidgetFields& save_fields(Widget* w);
    // Runs body on a SaveAgentFlow over w's +48..+77 fields, the card globals
    // 004ffbb8 / 004ffbbc / *0055a838 and this runtime's host.
    std::int32_t save_flow(Widget* w, Widget* sender, const std::function<std::int32_t(SaveAgentFlow&)>& body);
    std::int32_t go_players();                     // 0029bdb0
    // GuiAgentModify (init 00342fa0, deinit 00342e80, wake 00342f00, sleep 00342ec0,
    // input 0033fa80, draw 00342890, update 00342cd0, event 00340c80; gui_agent_modify.cpp)
    void am_construct(Widget* w);                  // 00343e10 (the editor over the edited packing)
    void am_init(Widget* w);
    void am_deinit(Widget* w);
    void am_wake(Widget* w);
    void am_sleep(Widget* w);
    std::uint8_t am_input(Widget* w, const PadEvent& ev);
    void am_update(Widget* w, float dt);
    std::uint8_t am_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    void am_leave(Widget* w);                      // 00332870 + 0040f540 (+ the switch to CMDCENTR)
    void am_select(Widget* w, const std::string& name);  // 00341c50
    void am_step(Widget* w, bool back, bool forward);    // 00341330
    void am_avail(Widget* w);                      // 00340850 (the tattoo / patch buttons' availability)
    void am_patch(Widget* w);                      // 00340320 (the patch's picture and name)
    void am_group_lock(Widget* g);                 // 00341060
    void am_group_unlock(Widget* g);               // 00340f60
    void am_group_hide(Widget* g);                 // 00341170
    void am_group_show(Widget* g);                 // 00341240
    void am_store_table(Widget* w);                // the editor's category table back to 00492a00
    void am_preview(Widget* w);                    // PORT-ONLY: the GuiCharacterDisplay's texture
    // GuiAgentCreate (ctor 0028f760, init 0028ed60, deinit 0028ec00, wake 0028ec80, sleep 0028ec40,
    // input 0028b3c0, update 0028ea40, event 0028bbe0; checkpoint 132cc5).
    void ac_construct(Widget* w);
    void ac_init(Widget* w);
    void ac_deinit(Widget* w);
    void ac_wake(Widget* w);
    void ac_sleep(Widget* w);
    std::uint8_t ac_input(Widget* w, const PadEvent& ev);
    void ac_update(Widget* w, float dt);
    std::uint8_t ac_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    void ac_save(Widget* w);                       // 0028ba90 (the appearance to the agent, then AGENTOPN)
    void ac_select(Widget* w, const std::string& name);  // 0028d610
    void ac_step(Widget* w, bool back, bool forward);    // 0028c390
    void ac_show(Widget* w);                       // the indicator / value_slot writes of 0028d610 / 0028c390
    void ac_preview(Widget* w);                    // PORT-ONLY: the GuiCharacterDisplay's texture
    // GuiAgentSave (ctor 00293fc0, wake 00292a20, input 00292820, event 00292980, save_done 002928e0).
    void as_wake(Widget* w);
    std::uint8_t as_input(Widget* w, const PadEvent& ev);
    std::uint8_t as_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    void as_done(Widget* w);
    // The debrief 00134110's screens (checkpoint 132f11, gui_end_game.cpp).
    void vq_push(Widget* w, const std::string& name, float f);  // 00460ac0
    void vq_fire(Widget* w);                       // 004610f0
    void vq_update(Widget* w, float dt);           // the queue head of 00462270 / 004637d0 / 00461020
    // GuiEndGameObjectives (vtable 004e17c0: wake 004624e0, input 00462380, update 00462270; the rest GuiObjectives')
    void eo_wake(Widget* w);
    std::uint8_t eo_input(Widget* w, const PadEvent& ev);
    // GuiEndGameStatistics (vtable 004e1840: wake 00463a20, input 00463160, update 004637d0, event 00463980,
    // first 00463940; the rest GuiScoresEquip's)
    void es_wake(Widget* w);
    void es_voices(Widget* w);                     // 00463390
    std::uint8_t es_input(Widget* w, const PadEvent& ev);
    std::uint8_t es_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    void es_awards(Widget* w);                     // the Cross path: pop +250, switch to "GuiEndGameAwards" AWARDS
    // GuiEndGameAwards (vtable 004e1730: init 00460bb0, deinit 00460b80, wake 004603a0, input 0045f3f0,
    // update 00461020)
    void aw_init(Widget* w);
    void aw_wake(Widget* w);
    std::uint8_t aw_input(Widget* w, const PadEvent& ev);
    void aw_build(Widget* w);                      // 0045fab0
    void aw_add(Widget* w, const std::string& file, const std::optional<std::string>& text);  // the build's add block
    std::uint8_t aw_next(Widget* w);               // 0045f4d0
    void aw_layout(Widget* w);                     // 0045f860
    void aw_voices(Widget* w);                     // 00460450
    void aw_autosave(Widget* w);                   // 0041f960("GuiAutoSaver") + 00419530 + 00414ac0
    // GuiAutoSaver (vtable 004e16d0: wake 0045f340, event 00441ca0, save_done 0045f300)
    void asv_wake(Widget* w);
    // The personnel screens (checkpoint 132f12, gui_personnel.cpp). GuiPersonnelScreen (vtable
    // 004df0b0: init 00356cd0, wake 00356c70, input 00356be0, event 00356b30, back 00356ad0).
    void pe_init(Widget* w);
    void pe_wake(Widget* w);
    std::uint8_t pe_input(Widget* w, const PadEvent& ev);
    std::uint8_t pe_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    void pe_back(Widget* w);                       // 00356ad0
    // GuiMissionStatistics (vtable 004dee70: init 0034fba0, wake 0034fb50, input 0034fa80, event
    // 0034fa40, select 0034f940, refresh 0034f3b0)
    void ms_init(Widget* w);
    void ms_wake(Widget* w);
    std::uint8_t ms_input(Widget* w, const PadEvent& ev);
    std::uint8_t ms_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    void ms_select(Widget* w, std::int32_t row);  // 0034f940
    void ms_refresh(Widget* w);                    // 0034f3b0
    // GuiMissionStatList (vtable 004deee0: the GuiTextArrayWidget's, with cell draw 0034fec0)
    void msl_attach(Widget* w, const std::vector<Widget::MissionStatRow>* rows);  // 003503d0
    void msl_fill(Widget* w);                      // 00350050
    // 0034fec0's colour choice for one cell (nullopt: the cell is not drawn).
    static std::optional<std::array<float, 4>> msl_cell_colour(const Widget& w, std::uint32_t col, std::uint32_t row);
    // The personnel files' base virtuals (vtable 004df0b0 +5c fill 0034d2b0, +60 show 00349ed0, +64
    // select 0034d2c0, +68 back 00356ad0: the base's are empty) dispatched on the class.
    void pe_fill(Widget* w);
    void pe_show(Widget* w, std::int32_t row);
    void pe_select(Widget* w, std::int32_t row);
    void pe_show_item(Widget* w, std::int32_t id, std::int32_t unlock);  // 00356850
    // 003569f0(this, file, kind, row, a, b): GuiPersonnelMenu over `file`, set up and switched to.
    void pe_open_menu(Widget* w, const std::string& file, std::int32_t kind, std::int32_t row, std::int32_t a,
                      std::int32_t b);
    Widget* pe_achievement(std::int32_t mode);     // 003534a0
    // GuiDossiers (vtable 004dec30: fill 00349ee0, select 00349fd0)
    void ds_fill(Widget* w);
    void ds_select(Widget* w, std::int32_t row);
    // GuiRanks (vtable 004ded50: fill 0034e4d0, show 0034e770, select 0034e5a0)
    void rk_fill(Widget* w);
    void rk_show(Widget* w, std::int32_t row);
    void rk_select(Widget* w, std::int32_t row);
    // GuiRatings (vtable 004decc0: init 0034dd20, wake 0034dc20, sleep 0034db10, input 0034d2d0,
    // event 0034d600, first 0034dbe0, draw 0034d8c0, the grid move 0034d770)
    void rt_init(Widget* w);
    void rt_wake(Widget* w);
    void rt_sleep(Widget* w);
    std::uint8_t rt_input(Widget* w, const PadEvent& ev);
    std::uint8_t rt_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    Widget* rt_first(Widget* w);
    void rt_move(Widget* w, std::int32_t dir);     // 0034d770 (0 left, 1 right, 2 up, 3 down)
    // 0034d8c0's percentage cells: the grid bitmap and the text, for the first unearned rating of
    // each grid row.
    struct RatingCell {
        const Widget* bitmap = nullptr;
        std::string text;
    };
    std::vector<RatingCell> rt_cells(const Widget* w) const;
    // GuiOmegaStrain (vtable 004dede0: wake 0034ef10, fill 0034edf0, show 0034ea00, select 0034ec00)
    void om_wake(Widget* w);
    void om_fill(Widget* w);
    void om_show(Widget* w, std::int32_t row);
    void om_select(Widget* w, std::int32_t row);
    // GuiAchievementScreen (vtable 004def90: init 00353850, wake 00353800, fill 00353700, show
    // 003536a0, select 003535d0)
    void ah_init(Widget* w);
    void ah_wake(Widget* w);
    void ah_fill(Widget* w);
    void ah_show(Widget* w, std::int32_t row);
    void ah_select(Widget* w, std::int32_t row);
    // GuiPersonnelMenu (vtable 004df020: init 003557d0, wake 00355790, sleep 003556f0, input
    // 00354a70, fill 00354b50, back 003549c0; setup 00354e40, toggle 00355490)
    void pm_init(Widget* w);
    void pm_wake(Widget* w);
    void pm_sleep(Widget* w);
    std::uint8_t pm_input(Widget* w, const PadEvent& ev);
    void pm_fill(Widget* w);
    void pm_back(Widget* w);
    void pm_setup(Widget* w, const std::string& cls, const std::string& file, std::int32_t kind, std::int32_t row,
                  std::int32_t a, std::int32_t b);
    void pm_toggle(Widget* w, std::uint8_t v);
    // GuiAgentInfo (vtable 004df350: init 00361aa0, deinit 00361a70, wake 0033d3a0, sleep 0033d360,
    // input 003617c0, update 003616e0, event 00361730; fill 003609f0, refresh 00361000) and its
    // GuiAgentDisplay (vtable 004df530: init 003643a0, deinit 003641c0, wake 00362dc0, update 00362d00,
    // draw 00361e70) (checkpoint 132f14, gui_personnel.cpp).
    void ai_init(Widget* w);
    void ai_deinit(Widget* w);
    void ai_wake(Widget* w);
    void ai_sleep(Widget* w);
    std::uint8_t ai_input(Widget* w, const PadEvent& ev);
    void ai_update(Widget* w, float dt);
    std::uint8_t ai_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    void ai_fill(Widget* w);                       // 003609f0
    void ai_fill_net(Widget* w, const LobbyRecord& r, const std::array<std::uint8_t, 0xfc>& rec);  // 00360610
    void ai_cell_invite(Widget* w, std::int32_t id, const std::string& name);                     // 0033c9e0
    void ai_refresh(Widget* w);                    // 00361000
    void ad_deinit(Widget* w);
    void ad_wake(Widget* w);
    void ad_update(Widget* w, float dt);
    // GuiOnlineConnect (vtable 004dd620: init 0029ef90, deinit 0029ef30, wake 0029ed40, sleep 0029ecd0, input
    // 0029e8c0, update 0029e1a0, event 0029e440; checkpoint 132ln2, gui_online_connect.cpp) over the net screen
    // 0033d580, the lobby strings 0033ce30 and the navigator 0033c320.
    void oc_init(Widget* w);
    void oc_deinit(Widget* w);
    void oc_wake(Widget* w);
    void oc_sleep(Widget* w);
    std::uint8_t oc_input(Widget* w, const PadEvent& ev);
    void oc_update(Widget* w, float dt);
    std::uint8_t oc_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    void oc_show(Widget* w, std::int32_t group);   // 0029cbb0
    void oc_back(Widget* w, bool error);           // 0029de70
    void oc_next(Widget* w, int depth = 0);        // 0029dfc0
    std::uint8_t oc_retry(Widget* w);              // 0029d020 (no caller found)
    void oc_list(Widget* w);                       // 0029d120
    void oc_dnas_retry(Widget* w);                 // 0029d390 (the MUIS universe step after the connect)
    void oc_dnas_popup(Widget* w, std::int32_t code);  // 0029d980
    void oc_connect(Widget* w, std::int32_t row);  // 0029daf0
    bool oc_policy(Widget* w);                     // checkpoint 132on2: the policy fetch tail of 0029d390 / 0029e1a0
    void net_poll(Widget* w, Widget* start);       // 0033cca0 (the net screen's vt +60; start: its +7c)
    void policy_fetch();                           // 002ca350(app)
    void policy_free();                            // 002ca160(app)
    bool font_text(std::string& s);                // 003fd740: UTF-8 to the character map's font codes
    struct DnasText {
        std::string title, message, footer;        // 0029f340's three buffers (0x80 / 0x100 / 0x80)
    };
    DnasText oc_dnas_text(std::int32_t code);      // 0029f340 (table 0048b990)
    void oc_lan_wake(Widget* w);                   // PORT-ONLY (132ln2): the LAN-mode content
    void oc_lan_select(Widget* w, std::int32_t row);  // PORT-ONLY (132ln2): Host / Join
    std::string lobby_string(std::uint32_t id);    // 0033ce30(screen, id, NULL, NULL, NULL, NULL)
    bool net_navigate(Widget* screen, std::int32_t op);  // 0033c320
    void net_leave(std::int32_t a1);               // 002ca640(app, a1)
    bool lan_lobby_check();                        // PORT-ONLY (132ln3): the navigator's LAN 00442510
    void cc_lan_activate(Widget* w);               // PORT-ONLY (132ln3): CMDCENTR's LAN online group
    // GuiMissionSetup (vtable 004dd730: activate 002a1780, deactivate 002a1700, input 002a12f0, event 002a1140,
    // begin_mission 002a0f60; checkpoint 132ln3, gui_mission_setup.cpp) over the net screen 0033d580.
    void mset_init(Widget* w);
    void mset_wake(Widget* w);
    void mset_sleep(Widget* w);
    void mset_update(Widget* w, float dt);
    void mset_activate(Widget* w);                   // 002a1780 (vt +64)
    void mset_deactivate(Widget* w);                 // 002a1700 (vt +68)
    std::uint8_t mset_input(Widget* w, const PadEvent& ev);
    std::uint8_t mset_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);
    void mset_agents(Widget* w, bool up);            // 002a1590
    void mset_filter(Widget* w, bool up);            // 002a1600
    void mset_texts(Widget* w);                      // 002a1670
    void mset_begin(Widget* w);                      // 002a0f60
    void mset_created(Widget* w);                    // 002a0dc0
    void mset_failed(Widget* w, bool lobby_text);    // 002a0e80
    void mset_create(Widget* w);                     // 002a0fc8 (begin_mission with a session)
    // Checkpoint 132on2b (gui_account_login.cpp): GuiAccountLogin (vtable 004dd120) over the net screen 0033d580.
    void acl_init(Widget* w);                         // 0028a5c0
    void acl_wake(Widget* w);                         // 0028a4b0
    void acl_sleep(Widget* w);                        // 0033d360 (vt +68 0028a430)
    void acl_net_update(Widget* w, float dt);         // 0033d310
    void acl_activate(Widget* w);                     // 0028a470 (vt +64)
    void acl_deactivate(Widget* w);                   // 0028a430 (vt +68)
    std::uint8_t acl_input(Widget* w, const PadEvent& ev);  // 00289990
    std::uint8_t acl_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 00289c80
    void acl_setmode(Widget* w, std::uint8_t create);  // 00288de0
    void acl_texts(Widget* w, std::uint8_t create);    // the two set_text calls of 00288de0, inlined elsewhere
    void acl_register(Widget* w);                     // 00288e90
    void acl_login(Widget* w);                        // 00289440
    void acl_done(Widget* w);                         // 002891d0 (vt +5c, the save flow's end)
    void acl_keyboard_user(Widget* w);                // 0028a230
    void acl_keyboard_pass(Widget* w);                // 0028a180
    void acl_signed_in(Widget* w, bool registered);   // the shared tail of 00289440 / 00289c80's 0x23 / 0x24 reply
    bool register_enabled();                         // 002ca070
    void save_rename(Widget* w, const std::string& name);  // 00441c20
    // Checkpoint 132on2c (gui_online_options.cpp): GuiOnlineOptions (vtable 004ddaf0) over the net screen 0033d580.
    void opt_init(Widget* w);                         // 002a9fe0
    void opt_wake(Widget* w);                         // 0033d3a0 (vt +64 002a9ba0)
    void opt_sleep(Widget* w);                        // 0033d360 (vt +68 002a9b50)
    void opt_net_update(Widget* w, float dt);         // 0033d310
    void opt_activate(Widget* w);                     // 002a9ba0 (vt +64)
    void opt_deactivate(Widget* w);                   // 002a9b50 (vt +68)
    std::uint8_t opt_input(Widget* w, const PadEvent& ev);  // 002a98d0
    std::uint8_t opt_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 002a97f0
    void opt_end(Widget* w, std::uint8_t a1);         // 002a96a0 (vt +5c: a1 0)
    void opt_apply(Widget* w);                        // 002a9a20
    // Checkpoint 132on2c (gui_net_messages.cpp): GuiNetMessages (vtable 004deb10) over the net screen 0033d580 and
    // GuiNetMessageArray (vtable 004deb80) over GuiArrayWidget.
    void nm_init(Widget* w);                          // 00347820
    void nm_wake(Widget* w);                          // 003477d0
    void nm_sleep(Widget* w);                         // 0033d360 (vt +68 00347690)
    void nm_net_update(Widget* w, float dt);          // 0033d310
    void nm_activate(Widget* w);                      // 003476b0 (vt +64)
    void nm_deactivate(Widget* w);                    // 00347690 (vt +68)
    std::uint8_t nm_input(Widget* w, PadEvent& ev);  // 003473e0 (Right / Left rewrite the caller's mask)
    std::uint8_t nm_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 00346d60
    void nm_show(Widget* w, const std::optional<GuiHost::LobbyMessage>& msg);  // 00346fb0
    void nma_init(Widget* w);                         // 00348d90
    void nma_wake(Widget* w);                         // 00348b90
    void nma_sleep(Widget* w);                        // 00348ad0
    void nma_update(Widget* w, float dt);             // 00348a00
    void nma_reselect(Widget* w);                     // the dimension / selection tail of 00348b90 and 00348a00
    // 0033ce30 with $1 / $2 (GuiNetMessages' texts: the message's +c sender, +2c text).
    std::string lobby_string(std::uint32_t id, const std::optional<std::string>& a1,
                             const std::optional<std::string>& a2);
    // Checkpoint 132ln4 (gui_net_join_mission.cpp): GuiLobbyScreen (vtable 004de8a0) and GuiNetJoinMission
    // (vtable 004dda50) over the net screen 0033d580, GuiMissionList (vtable 004ddb80) over GuiArrayWidget.
    std::string lobby_string(std::uint32_t id, const std::optional<std::string>& a1);  // 0033ce30 with $1
    void lb_init(Widget* w);                         // 002a9240 -> 0033f250
    void lb_wake(Widget* w);                         // 0033f110
    void lb_sleep(Widget* w);                        // 0033f050
    void lb_update(Widget* w, float dt);             // 0033de00
    std::uint8_t lb_input(Widget* w, const PadEvent& ev);  // 0033da00
    std::uint8_t lb_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 0033df00
    std::uint8_t lb_pick(Widget* w, Widget* sender, bool now);    // 0033e2a0
    std::uint8_t lb_mode(Widget* w, std::uint8_t mode, bool focus);  // 0033ef10
    void lb_show(Widget* w, std::uint8_t kind);      // 0033d8e0
    std::uint8_t lb_kind(const Widget* w) const;     // 0033d8c0
    void lb_host_game(Widget* w);                    // 0033d800
    void lb_off(Widget* w);                          // 0033f0c0
    void lb_chat(Widget* w, LobbyRecord* r);         // vt +6c 0033ee40
    void lb_info(Widget* w, LobbyRecord* r);         // vt +70 0033e410
    // vt +74 0033e5b0; `prompt` its a2 (1 from the list's join, 0 from the password prompt's OK)
    void lb_join(Widget* w, LobbyRecord* r, std::int32_t prompt);
    void lb_action(Widget* w, LobbyRecord* r);       // vt +6c / +70 / +74 / +78 by +90
    void lb_lan_refresh(Widget* w);                  // PORT-ONLY (132ln4): the LAN games into +c8 / +e4
    // 0033e5b0's session branch (132on2b)
    void lb_join_session(Widget* w, LobbyRecord* r, std::int32_t prompt);
    // 00447790 / 00447a30 / 00447bb0 / 00447c70 over the GuiHost lobby lists into +e4 / +c8 / +ac (132on2b / 132on2c)
    void lb_session_refresh(Widget* w);
    // 0044eff0's record update from a game cache entry (132on2i): +30..+44.
    static void lb_cache_fill(LobbyRecord& rec, const GuiHost::LobbyCachedGame& e);
    void chat_line(Widget* chat, const std::string& name, const std::string& text, bool whisper);  // 002aec90
    std::uint8_t jm_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 002a8d60
    void jm_activate(Widget* w);                     // 002a9030 (vt +64)
    void jm_deactivate(Widget* w);                   // 002a8fd0 (vt +68)
    void jm_toggle_text(Widget* w, const char* key); // 002a8b60 / 002a8be0's 0041beb0(+160, GetString(key))
    // Checkpoint 132on2h (gui_net_join_mission.cpp): GuiNetContacts (vtable 004ddc20) over GuiLobbyScreen.
    std::uint8_t ct_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 002adab0
    void ct_activate(Widget* w);                     // 002ade60 (vt +64)
    void ct_deactivate(Widget* w);                   // 002ade00 (vt +68)
    void ct_remove(Widget* w, LobbyRecord* r);       // vt +78 002ada10
    void ct_add(Widget* w, std::int32_t id, const std::string& name);  // 0033c7c0
    void mlist_init(Widget* w);                      // 002accf0
    void mlist_wake(Widget* w);                      // 002abd50
    void mlist_sleep(Widget* w);                     // 002abc90
    void mlist_update(Widget* w, float dt);          // 002acca0
    void mlist_action(Widget* w, std::uint8_t action);  // 002abf00
    void mlist_kind(Widget* w, std::uint8_t kind);   // 002abf40
    void mlist_rebuild(Widget* w);                   // 002ac070
    void mlist_sort(Widget* w);                      // 002ac6f0
    void mlist_refresh(Widget* w);                   // 002acb70
    void mlist_attach(Widget* w, LobbyList* source); // 002acc80
    void mlist_entered(Widget* w, std::uint32_t col, std::uint32_t row);  // vt +6c 002ac300
    LobbyRecord* mlist_entry(Widget* w, std::int32_t row);  // 002ac400
    std::int32_t mlist_flags(Widget* w, const LobbyRecord* r);  // 002ac470
    bool lobby_data() { return host_.net_session() || g.lan_mode; }  // 004294c0 for the list's data (132ln4)
    // 002aad40 (GuiMissionList's cell draw +68) up to its 003cfab0: the cell's text, the text object's colour
    // (003d0810) and +5b; nullopt where it draws nothing (no text object, no source, a row past the rows, no record).
    struct MissionListCell {
        std::string text;
        std::array<float, 4> rgba{};
        std::uint8_t b5b = 0;
    };
    std::optional<MissionListCell> mlist_cell(const Widget& w, std::uint32_t col, std::uint32_t row) const;
    // Checkpoint 132on2h: 00348530 (GuiNetMessageArray's cell draw +68) up to its draws: the row's lobby message
    // (00348554..003485a4: nullopt with no text object +a0, no message list +a4, a row past its count, column 2 or no
    // message), the column's icon (column 0: +94 / +98 / +9c by type), else the text, halign (+10), +5b and colour.
    struct NetMessageCell {
        Widget* icon = nullptr;
        std::string text;
        std::uint8_t halign = 0;
        std::uint8_t b5b = 0;
        std::array<float, 4> rgba{};
    };
    std::optional<NetMessageCell> nma_cell(const Widget& w, std::uint32_t col, std::uint32_t row) const;
    // Decorator event 4 (00417520 -> 00416e40): the record found by name.
    const GuiEvent* deco_record_ = nullptr;

    void canvas_push(Widget* w, std::int32_t layer);  // 00414f00
    std::uint8_t canvas_navigate(const PadEvent& ev); // 00415170
    std::uint8_t canvas_event(Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 00465610
    std::uint8_t canvas_base_event(Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0);  // 00414a80
    void dispatch(const PadEvent& ev);  // 004657e0 / 00415310
    void controller_check();            // 00465870
    void update_stack(float dt);        // 00415950

    Widget* make_widget(WidgetClass cls);              // 0041f960 + the class constructor
    void load_node(const GuiNode& n, Widget* w);       // 00418b10
    void add_root(Widget* w);                          // 00419530
    bool selectable(const Widget* w) const;
    void o_gain_focus(Widget* w) { o_gain(w); }

    GuiHost& host_;
    Widget canvas_;
    bool canvas_alive_ = true;            // byte 00571fc0
    std::vector<std::unique_ptr<Widget>> widgets_;
    std::vector<Widget*> pending_list_;   // canvas +54
    bool iterating_ = false;              // canvas +60
};

// ---------------------------------------------------------------- scripted run
// The text script of tools/ee/gen_gui_runtime.py (its cases are the goldens)
// and a host that logs every service call the way the generator's hooks do.

struct GuiScript {
    std::string root = "GuiTitleScreen";
    bool native = false;          // "native 1": native popups and manager (checkpoint 118b)
    bool msgbox = false;          // "msgbox 1": create GuiMessageBox / POPUP first
    bool cdvd = false;            // "cdvd 1": the generator's disc model (char-map files)
    int kbd = -1;                 // "kbd IDX": load GuiKeyboard from file IDX after the screens
    std::vector<std::pair<std::string, int>> res;  // "res NAME IDX": resource -> file (-2: not a GUI)
    int main = 0;
    int layer = 0;
    std::string iface = "frontend";
    std::vector<int> screens;
    std::vector<std::pair<std::string, std::optional<std::string>>> strings;
    std::vector<std::pair<std::string, int>> factory;
    std::vector<std::pair<std::string, std::int64_t>> cfg;
    std::vector<std::string> dir;   // "dir HEX..": the card entry names (CardState::dir)
    std::vector<std::string> stop;  // "stop CLASS": classes 0041f960 declines (GuiHost::class_stopped)
    // Checkpoint 118g ("cc 1"): the command centre's profile services.
    bool cc = false;
    std::vector<std::pair<std::string, std::optional<std::string>>> prof;  // "prof K HEX" / "profnull K"
    std::vector<std::pair<std::int32_t, std::uint8_t>> bvec;                // "bvec i:b .."
    std::vector<std::pair<std::int32_t, std::vector<std::pair<std::optional<std::string>, std::uint8_t>>>> bitems;
    std::vector<std::pair<std::int32_t, std::string>> skey;                 // "skey i HEX"
    // PORT-ONLY (checkpoint 132ln4, no golden): the LAN browse list, "lgame SIDHEX MISSION NOW MAX FILTER STATE
    // MISMATCH NAME" (GuiHost::lan_games); cfg lan_reply (0 pending, 1 JOIN_ACK, 2 JOIN_NAK, 3 no answer) and
    // lan_join_ok (lan_join's result) drive the join.
    std::vector<GuiHost::LanGame> lgame;
    // Checkpoint 132on2 (cfg sess=1, the generator's SFOLobby hooks): the rows the hooks append, "lcfg SEL HEXNAME"
    // (0044c440 +98), "lsrv HEXNAME HEXUNIVERSE" (+d0), "luni ID HEXNAME" (00442b50), "lann ID HEXTEXT" (00445570),
    // "lpol 0|1 HEXTEXT" (004456d0 / 004456c0), and "sett KEY HEXVALUE" (003cba90's settings).
    std::vector<GuiHost::LobbyConfig> lcfg;
    std::vector<GuiHost::LobbyServer> lsrv;
    std::vector<GuiHost::LobbyUniverse> luni;
    std::vector<GuiHost::LobbyAnnouncement> lann;
    std::vector<std::pair<int, std::string>> lpol;
    std::vector<std::pair<std::string, std::string>> sett;
    // Checkpoint 132on2c: the session's message rows (+1c), "lmsg TYPE ID HEXFROM HEXTEXT" (0044f470)
    std::vector<GuiHost::LobbyMessage> lmsg;
    struct Frame {
        std::uint32_t pad0 = 0, pad1 = 0, t1 = 0;
        std::int64_t ticks = 0;
        std::uint32_t dt = 0;
        std::int32_t ms = 0;
        std::uint8_t save = 0;
        std::vector<std::string> ops;
    };
    std::vector<Frame> frames;
    std::int64_t config(const std::string& key) const;
};

GuiScript parse_gui_script(const std::string& text);

// Runs a script over .GUI files (indexed by `main` / `screen`) and returns
// the trace: service log lines, "L"/"P"/"@i" markers and the changed
// S (canvas / globals) and W (widget) state lines.
std::vector<std::string> run_gui_script(const GuiScript& script, const std::vector<GuiFile>& files);

}  // namespace omega::gui
