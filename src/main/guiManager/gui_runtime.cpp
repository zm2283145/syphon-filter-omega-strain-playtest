#include "gui_runtime.hpp"
#include "agent_card.hpp"
#include "agent_save_flow.hpp"
#include "ni_event_object.hpp"
#include "zone_profile.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <stdexcept>
#include <unordered_map>

namespace omega::gui {
namespace {

// EE COP1 semantics (tools/ee/ee.py): denormal operands and results flush to
// a signed zero, overflow is IEEE infinity, division by zero gives the signed
// maximum, cvt.w.s saturates.
float ftz(float x) {
    if (x != 0.0f && std::fabs(x) < FLT_MIN) return std::copysign(0.0f, x);
    return x;
}
float fadd(float a, float b) { return ftz(ftz(a) + ftz(b)); }
float fsub(float a, float b) { return ftz(ftz(a) - ftz(b)); }
float fmul(float a, float b) { return ftz(ftz(a) * ftz(b)); }
float fdiv(float a, float b) {
    a = ftz(a);
    b = ftz(b);
    if (b == 0.0f) {
        const float sign = a != 0.0f ? a * b : 1.0f;
        return std::copysign(FLT_MAX, std::signbit(sign) ? -1.0f : 1.0f);
    }
    return ftz(a / b);
}
std::int32_t cvt_w(float x, std::uint32_t address) {
    x = ftz(x);
    if (std::isnan(x)) throw std::runtime_error("cvt.w.s of NaN at " + std::to_string(address));
    if (x >= 2147483647.0f) return INT32_MAX;
    if (x <= -2147483648.0f) return INT32_MIN;
    return static_cast<std::int32_t>(x);
}

[[noreturn]] void unhosted(std::uint32_t address, const std::string& what) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", address);
    throw std::runtime_error("gui runtime: unhosted " + what + " (" + buf + ")");
}

// The compiler's unsigned -> float idiom (bltz; srl / andi / or; cvt.s.w;
// add.s). Only exact conversions are hosted: the EE's cvt.s.w truncates
// where a host conversion would round.
float u2f(std::uint32_t v, std::uint32_t address) {
    const bool high = static_cast<std::int32_t>(v) < 0;
    const auto c = static_cast<std::int32_t>(high ? (v >> 1) | (v & 1) : v);
    const float f = static_cast<float>(c);
    if (static_cast<std::int64_t>(f) != c) unhosted(address, "inexact cvt.s.w");
    return high ? fadd(f, f) : f;
}

// The float -> unsigned idiom (c.ole.s 2^31; cvt.w.s; or 0x80000000).
std::uint32_t f2u(float x, std::uint32_t address) {
    const float two31 = 2147483648.0f;
    if (two31 <= x) return static_cast<std::uint32_t>(cvt_w(fsub(x, two31), address)) | 0x80000000u;
    return static_cast<std::uint32_t>(cvt_w(x, address));
}

// 0011dbd8 (fmodf, reached through 00121ae8): integer remainder on the
// bit patterns; |y| denormal or zero gives NaN through mul.s/div.s.
float fmodf_ee(float xf, float yf) {
    std::int32_t hx = static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(xf));
    const std::int32_t hy = static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(yf) & 0x7fffffffu);
    const std::uint32_t sx = static_cast<std::uint32_t>(hx) & 0x80000000u;
    hx = static_cast<std::int32_t>(static_cast<std::uint32_t>(hx) ^ sx);
    const float zero = std::bit_cast<float>(sx);  // Zero[sign] at 004a9528
    if (hy <= 0x7fffff) unhosted(0x11dc1c, "fmodf by a zero or denormal");
    if (hx < hy) return xf;
    if (hx == hy) return zero;
    std::int32_t ix = (hx >> 23) - 127;
    std::int32_t iy = (hy >> 23) - 127;
    std::int32_t mx = ix < -126 ? static_cast<std::int32_t>(static_cast<std::uint32_t>(hx) << (-126 - ix))
                                : ((hx & 0x7fffff) | 0x800000);
    const std::int32_t my = iy < -126 ? static_cast<std::int32_t>(static_cast<std::uint32_t>(hy) << (-126 - iy))
                                      : ((hy & 0x7fffff) | 0x800000);
    std::int32_t n = ix - iy;
    std::int32_t hz = 0;
    for (;;) {
        hz = mx - my;
        if (n-- == 0) break;
        if (hz < 0) mx = static_cast<std::int32_t>(static_cast<std::uint32_t>(mx) << 1);
        else {
            if (hz == 0) return zero;
            mx = static_cast<std::int32_t>(static_cast<std::uint32_t>(hz) << 1);
        }
    }
    if (hz >= 0) mx = hz;
    if (mx == 0) return zero;
    while (mx <= 0x7fffff) {
        mx = static_cast<std::int32_t>(static_cast<std::uint32_t>(mx) << 1);
        --iy;
    }
    std::uint32_t r;
    if (iy >= -126) r = (static_cast<std::uint32_t>(mx) - 0x800000u) | (static_cast<std::uint32_t>(iy + 127) << 23);
    else r = static_cast<std::uint32_t>(mx >> (-126 - iy));
    return std::bit_cast<float>(r | sx);
}

// The class registry *00572118 that 0041f960 searches (00128e80, case-insensitive).
constexpr std::array<const char*, 69> registered_classes = {
    "GuiTitleScreen", "GuiAccountLogin", "GuiAgentCreate", "GuiAgentSave", "GuiAgentLoad", "GuiCommandCenter",
    "GuiGameOptions", "GuiOnlineConnect", "GuiOnlineOfflineSelect", "GuiMissionSetup", "GuiEquipmentSetup",
    "GuiMessageBox", "GuiNetJoinMission", "GuiOnlineOptions", "GuiMissionList", "GuiNetContacts", "GuiChatWindow",
    "GuiNetAgencyCell", "GuiZeusShell", "GuiQuickChat", "GuiEquipmentModify", "GuiCharacterDisplay", "GuiSlideShow",
    "GuiKeyboard", "GuiAgentModify", "GuiNetMessages", "GuiNetMessageArray", "GuiDossiers", "GuiRatings", "GuiRanks",
    "GuiOmegaStrain", "GuiMissionStatistics", "GuiMissionStatList", "GuiAchievementScreen", "GuiPersonnelMenu",
    "GuiPersonnelScreen", "GuiScoresEquip", "GuiAgentInfo", "GuiAgentDisplay", "GuiObjectives", "GuiCanvas",
    "GuiTextEditWidget", "GuiTextWidget", "GuiWidget", "GuiButtonWidget", "GuiTextListWidget", "GuiArrayWidget",
    "GuiTextArrayWidget", "GuiScrollBarWidget", "GuiBitmapWidget", "GuiMLTextWidget", "GuiSliderWidget",
    "GuiWeaponDisplay", "GuiSaveAgentWidget", "GuiRadioLog", "GuiEquipmentPane", "GuiWeaponStats",
    "GuiBriefingPane", "GuiStartMenu", "GuiStartMenuBackdrop", "GuiSubTitleDisplay", "GuiGameScreen",
    "GuiAutoSaver", "GuiEndGameAwards", "GuiEndGameObjectives", "GuiEndGameStatistics", "GuiMapScreen",
    "GuiGameCanvas", "GuiFrontendCanvas"};

bool iequal(const std::string& a, const char* b) {
    std::size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return i == a.size() && !b[i];
}

// vt +54 of the ported classes.
const char* class_id(WidgetClass c) {
    switch (c) {
    case WidgetClass::widget: return "GuiWidget";               // 00418a70
    case WidgetClass::text: return "GuiTextWidget";             // 0041be10
    case WidgetClass::button: return "GuiButtonWidget";         // 00420ba0
    case WidgetClass::title: return "GuiTitleScreen";           // 00287dc0
    case WidgetClass::edit: return "GuiTextEditWidget";         // 0041a0c0
    case WidgetClass::msgbox: return "GuiMessageBox";           // 002a7ac0
    case WidgetClass::keyboard: return "GuiKeyboard";          // 0032aed0
    case WidgetClass::textlist: return "GuiTextListWidget";     // 00420e10
    case WidgetClass::agentload: return "GuiAgentLoad";         // 00293850
    case WidgetClass::onoffsel: return "GuiOnlineOfflineSelect";  // 002a0180
    case WidgetClass::cmdcentr: return "GuiCommandCenter";      // 00294210
    case WidgetClass::objectives: return "GuiObjectives";        // 00367ae0
    case WidgetClass::startmenu: return "GuiStartMenu";          // 00457430
    case WidgetClass::startback: return "GuiStartMenuBackdrop";  // 004574d0
    case WidgetClass::slider: return "GuiSliderWidget";          // 0043eec0
    case WidgetClass::gameopt: return "GuiGameOptions";          // 00299690
    case WidgetClass::scrollbar: return "GuiScrollBarWidget";    // 0043bb80
    case WidgetClass::mltext: return "GuiMLTextWidget";
    case WidgetClass::briefing: return "GuiBriefingPane";
    case WidgetClass::radiolog: return "GuiRadioLog";            // 00454110
    case WidgetClass::subtitle: return "GuiSubTitleDisplay";     // 00457aa0
    case WidgetClass::slideshow: return "GuiSlideShow";          // 00326da0
    case WidgetClass::bitmap: return "GuiBitmapWidget";          // 0043c0b0
    case WidgetClass::textarray: return "GuiTextArrayWidget";
    case WidgetClass::weaponstats: return "GuiWeaponStats";
    case WidgetClass::weapondisplay: return "GuiWeaponDisplay";
    case WidgetClass::equippane: return "GuiEquipmentPane";
    case WidgetClass::eqmodify: return "GuiEquipmentModify";  // 002c1100
    case WidgetClass::agentmod: return "GuiAgentModify";         // 0033f9c0
    case WidgetClass::agentcreate: return "GuiAgentCreate";      // 0028b380
    case WidgetClass::agentsave: return "GuiAgentSave";          // 002927e0
    case WidgetClass::mapscreen: return "GuiMapScreen";         // 00464880
    case WidgetClass::endobjectives: return "GuiEndGameObjectives";  // 00462340
    case WidgetClass::endstats: return "GuiEndGameStatistics";   // 00463120
    case WidgetClass::endawards: return "GuiEndGameAwards";      // 00461260
    case WidgetClass::autosaver: return "GuiAutoSaver";          // 0045f2c0
    case WidgetClass::missionstats: return "GuiMissionStatistics";  // 0034f370
    case WidgetClass::missionstatlist: return "GuiMissionStatList";  // 00350560
    case WidgetClass::port_settings: return "PortGuiSettings";  // port-only (132win11)
    case WidgetClass::dossiers: return "GuiDossiers";
    case WidgetClass::ranks: return "GuiRanks";
    case WidgetClass::ratings: return "GuiRatings";
    case WidgetClass::omega: return "GuiOmegaStrain";
    case WidgetClass::achieve: return "GuiAchievementScreen";
    case WidgetClass::pmenu: return "GuiPersonnelMenu";
    case WidgetClass::agentinfo: return "GuiAgentInfo";          // 00360430
    case WidgetClass::agentdisplay: return "GuiAgentDisplay";
    case WidgetClass::onlinecn: return "GuiOnlineConnect";      // 0029cb50
    case WidgetClass::msnsetup: return "GuiMissionSetup";       // 002a0d80
    case WidgetClass::netjoin: return "GuiNetJoinMission";      // 002a8b10 (checkpoint 132ln4)
    case WidgetClass::missionlist: return "GuiMissionList";     // 002aad00
    case WidgetClass::chatwin: return "GuiChatWindow";          // 002aef00
    case WidgetClass::acntlogn: return "GuiAccountLogin";       // 00288da0 (checkpoint 132on2b)
    case WidgetClass::onlinepref: return "GuiOnlineOptions";    // 002a9660 (checkpoint 132on2c)
    case WidgetClass::netmess: return "GuiNetMessages";         // 00346d20 (checkpoint 132on2c)
    case WidgetClass::netmsgarray: return "GuiNetMessageArray";  // 00349180 (checkpoint 132on2c)
    case WidgetClass::netcontacts: return "GuiNetContacts";      // checkpoint 132on2h
    case WidgetClass::netagencycell: return "GuiNetAgencyCell";  // checkpoint 132on2i
    case WidgetClass::canvas: return "GuiFrontendCanvas";
    }
    return "";
}

bool is_text(const Widget* w) {
    return w->cls == WidgetClass::text || w->cls == WidgetClass::button || w->cls == WidgetClass::edit;
}

}  // namespace

const char* widget_class_name(WidgetClass c) { return class_id(c); }

std::optional<std::int32_t> GuiHost::resource(const std::string& name) {
    unhosted(0x4133b0, "resource lookup '" + name + "'");
}
const GuiNode* GuiHost::open_gui(const std::string& file, std::int32_t) {
    unhosted(0x36e1b0, "stream open '" + file + "'");
}
std::optional<GuiHost::UsbKeyReport> GuiHost::usbkb_poll() {
    unhosted(0x3d1660, "USB keyboard poll");
}
std::uint32_t GuiHost::default_font() { unhosted(0x418420, "default font (manager +10)"); }
std::uint8_t GuiHost::font_height(std::uint32_t) { unhosted(0x36a2f0, "font height (font +99)"); }
std::int32_t GuiHost::font_width(std::uint32_t, const std::string&, std::int32_t) {
    unhosted(0x36aa50, "font string width");
}
std::uint8_t GuiHost::agent_load(std::int32_t, const std::string&, std::int32_t) {
    unhosted(0x334790, "agent load from the card");
}
void GuiHost::agent_activate(std::int32_t) { unhosted(0x292c54, "loaded agent copy to 0055d510"); }
bool GuiHost::class_stopped(const std::string&) { return false; }
std::optional<std::string> GuiHost::profile_name(std::int32_t) { unhosted(0x336170, "profile agent name"); }
std::int32_t GuiHost::debrief_profile_slot(std::int32_t) { unhosted(0x1314f0, "the network debrief's agent slot"); }
std::int32_t GuiHost::debrief_local_slot() { unhosted(0x4ffd28, "the network debrief's local player index"); }
std::int32_t GuiHost::debrief_statistic(std::int32_t, std::uint32_t) {
    unhosted(0x333ee0, "the network debrief's agent statistic");
}
std::optional<std::string> GuiHost::profile_rank(std::int32_t) { unhosted(0x336110, "profile rank string"); }
std::int32_t GuiHost::profile_clp(std::int32_t) { unhosted(0x3305d0, "profile CLP"); }
std::int32_t GuiHost::level_done(std::int32_t, const std::string& key) {
    unhosted(0x332fe0, "profile level completed '" + key + "'");
}
std::int32_t GuiHost::level_flag(std::int32_t, std::int32_t, std::int32_t) {
    unhosted(0x332860, "profile level bonus / ZEUS flag");
}
std::vector<std::array<std::int32_t, 8>> GuiHost::profile_level_records(std::int32_t) {
    unhosted(0x34f3b0, "the agent's level records (agent + c4)");
}
std::optional<std::string> GuiHost::profile_6060(std::int32_t, const std::string& key) {
    unhosted(0x336060, "profile lookup '" + key + "'");
}
std::int32_t GuiHost::profile_3b10(std::int32_t, const std::string& key) {
    unhosted(0x333b10, "profile flag '" + key + "'");
}
std::vector<GuiHost::BonusRow> GuiHost::profile_vec(std::int32_t, std::int32_t) {
    unhosted(0x3334c0, "profile bonus vector");
}
std::vector<GuiHost::BonusItem> GuiHost::profile_items(std::int32_t, std::int32_t, std::int32_t) {
    unhosted(0x333260, "profile bonus items");
}
std::vector<GuiHost::BonusItem> GuiHost::profile_level_objectives(std::int32_t, const std::string& s) {
    unhosted(0x3353f0, "the level's objective rows '" + s + "'");
}
std::vector<GuiHost::RankEntry> GuiHost::profile_ranks() { unhosted(0x532b50, "the rank registry"); }
std::vector<GuiHost::RankEntry> GuiHost::profile_dossiers() { unhosted(0x532b70, "the dossier registry"); }
std::string GuiHost::string_key(std::int32_t) { unhosted(0x3fdae0, "string table key"); }
void GuiHost::level_start(std::int32_t, std::int32_t, std::int32_t, std::int32_t, std::int32_t) {
    unhosted(0x3328d0, "mission start");
}
std::uint8_t GuiHost::profile_byte(std::int32_t, std::uint32_t) { unhosted(0x1314f0, "profile byte"); }
std::int32_t GuiHost::profile_word(std::int32_t, std::uint32_t) { unhosted(0x1314f0, "profile word"); }
std::string GuiHost::profile_text(std::int32_t, std::uint32_t) { unhosted(0x1314f0, "profile string"); }
std::uint32_t GuiHost::card_make_directory() { unhosted(0x40d740, "card save directory create"); }
std::uint32_t GuiHost::card_format() { unhosted(0x40db10, "card format"); }
void GuiHost::card_remove(const std::string&) { unhosted(0x40dc80, "card entry remove"); }
void GuiHost::card_rename(const std::string&, const std::string&) { unhosted(0x40ddc0, "card entry rename (agent file format update)"); }
std::uint32_t GuiHost::card_exists(const std::string&) { unhosted(0x40d690, "card entry lookup"); }
std::uint32_t GuiHost::agent_save(std::int32_t) { unhosted(0x334110, "agent profile save"); }
std::uint32_t GuiHost::agent_create(std::int32_t, std::int32_t, std::uint32_t) { unhosted(0x333f60, "the agent create"); }
void GuiHost::agent_set_name(std::int32_t, const std::string&) { unhosted(0x3340b0, "the agent rename"); }
std::uint32_t GuiHost::options_save(const GameOptionsRecord&) { unhosted(0x45a2a0, "the options file SFO.CFG"); }
void GuiHost::card_poll() { unhosted(0x441c10, "the card wait's GUI poll 002cc260(0, 0)"); }
bool GuiHost::quip_playing(const std::string& name) { unhosted(0x1c95d0, "the quip-playing test '" + name + "'"); }
std::vector<GuiHost::BonusRow> GuiHost::profile_ratings(std::int32_t) { unhosted(0x333600, "the agent's rating rows"); }
GuiHost::RankRow GuiHost::profile_rank_row(std::int32_t) { unhosted(0x45fb40, "the agent's rank row (*(agent + b50))"); }
std::int32_t GuiHost::profile_statistic(std::int32_t, std::int32_t) { unhosted(0x333ee0, "the agent's statistic word"); }
std::optional<omega::AgentPacking> GuiHost::profile_appearance(std::int32_t) {
    unhosted(0x3609f0, "the agent's appearance (*(agent + 9a4))");
}
bool GuiHost::port_lan_play() { return false; }
bool GuiHost::net_session() { return false; }  // 004294c0: byte 00572488 is never set (no network start)
std::int32_t GuiHost::net_start() {
    unhosted(0x2ca710, "the online start (003a6710 modules, MUIS / DNS, 002ea4f0, 00429510 DME)");
}
void GuiHost::app_2c9aa0() { unhosted(0x2c9aa0, "the music stop (001c7d80 on the GameSoundManager)"); }
void GuiHost::card_sync() { unhosted(0x40d5d0, "the card wait (002b5f48 until the card thread is idle)"); }
// Checkpoint 132on2: the SFOLobby calls (no session in the default host)
std::int32_t GuiHost::lobby_configs(std::vector<LobbyConfig>&, std::vector<LobbyServer>&) {
    unhosted(0x44c440, "the network configuration list");
}
void GuiHost::lobby_disconnect() { unhosted(0x44c3c0, "the lobby disconnect"); }
std::int32_t GuiHost::lobby_error() { unhosted(0x452ff0, "the lobby error code"); }
std::int32_t GuiHost::lobby_connect(std::int32_t) { unhosted(0x44bab0, "the network connect and DNAS"); }
std::int32_t GuiHost::lobby_dnas_result() { unhosted(0x452fe0, "the DNAS result"); }
std::int32_t GuiHost::lobby_universes_begin() { unhosted(0x442f40, "the MUIS universe request"); }
std::int32_t GuiHost::lobby_muis_connect(std::int32_t, std::vector<LobbyUniverse>&) {
    unhosted(0x442b50, "the MUIS server connect");
}
std::int32_t GuiHost::lobby_universe_select(std::int32_t) { unhosted(0x442840, "the universe select"); }
std::int32_t GuiHost::lobby_universe_default() { unhosted(0x442970, "the default universe"); }
void GuiHost::lobby_universes_end() { unhosted(0x442e90, "the MUIS request's end"); }
std::string GuiHost::setting(const std::string&) { return std::string(); }
std::int32_t GuiHost::lobby_policy_begin() { unhosted(0x445e80, "the policy / announcement request"); }
std::int32_t GuiHost::lobby_announcements(std::vector<LobbyAnnouncement>&) {
    unhosted(0x445570, "the announcement download");
}
std::int32_t GuiHost::lobby_policy(bool privacy, std::vector<std::string>&) {
    unhosted(privacy ? 0x4456c0 : 0x4456d0, "the policy download");
}
void GuiHost::lobby_policy_end() { unhosted(0x445dc0, "the policy request's end"); }
void GuiHost::lobby_leave(std::int32_t) { unhosted(0x2ca66c, "the online leave (00445850, 002c8090, 00453b00, 00429500)"); }
std::optional<std::string> GuiHost::lobby_notice() { unhosted(0x33ccd0, "the net screen's lobby poll 0033cca0"); }
bool GuiHost::lobby_unread() { unhosted(0x33cd9c, "the session's +20 (0033cca0)"); }
bool GuiHost::lobby_region_choice() { return true; }
std::int32_t GuiHost::lobby_universe_list(std::vector<LobbyUniverse>&) { unhosted(0x442d80, "the universe list request"); }
std::int32_t GuiHost::lobby_message_count() { unhosted(0x348a3c, "the lobby message list's count (session +1c, +4)"); }
std::optional<GuiHost::LobbyMessage> GuiHost::lobby_message(std::int32_t) { unhosted(0x451010, "the lobby message"); }
void GuiHost::lobby_message_answer(std::int32_t, std::int32_t) { unhosted(0x450f30, "the lobby message's answer"); }
bool GuiHost::lobby_messages_changed() { unhosted(0x348a20, "the lobby message list's changed flag (+18)"); }
void GuiHost::lobby_messages_set_changed(bool) { unhosted(0x44f280, "the lobby message list's changed flag store"); }
void GuiHost::font_colors(std::uint32_t, const std::array<float, 4>&, const std::array<float, 4>&) {}
std::int32_t GuiHost::lobby_login(const std::string&, const std::string&) { unhosted(0x445a40, "the account login"); }
std::int32_t GuiHost::lobby_register(const std::string&, const std::string&) {
    unhosted(0x445b90, "the account registration");
}
std::int32_t GuiHost::lobby_check_account(const std::string&, const std::string&) {
    unhosted(0x4459d0, "the account check");
}
std::int32_t GuiHost::lobby_enter() { unhosted(0x442750, "the lobby channel join"); }
std::int32_t GuiHost::lobby_state() { unhosted(0x297410, "the lobby state (session +255c)"); }
std::int32_t GuiHost::lobby_mission(std::int32_t) { unhosted(0x442510, "the lobby's mission channel (SFOLobby_Mission)"); }
std::int32_t GuiHost::lobby_messages() { unhosted(0x2c80d8, "the lobby records to the agent 002c8090"); }
void GuiHost::lobby_channel_leave() { unhosted(0x4426b0, "the lobby channel leave"); }
std::int32_t GuiHost::lobby_room_open() { unhosted(0x4518f0, "the lobby's game room"); }
void GuiHost::lobby_room_leave() { unhosted(0x451610, "the lobby's room leave"); }
void GuiHost::lobby_list_kind(std::int32_t) { unhosted(0x4515f0, "the lobby's room list kind"); }
void GuiHost::lobby_net_start() { unhosted(0x1698c0, "the network modules' start (001698c0 / 00165bc0)"); }
std::int32_t GuiHost::lobby_create(std::int32_t, std::int32_t, std::int32_t) {
    unhosted(0x44e960, "the Medius game creation");
}
std::optional<GuiHost::LobbyCallback> GuiHost::lobby_pump() { unhosted(0x4536b0, "the lobby update"); }
void GuiHost::lobby_game_cleanup() { unhosted(0x42b1f0, "the DME game cleanup"); }
std::optional<GuiHost::LobbyGameInfo> GuiHost::lobby_game_info(std::int32_t) {
    unhosted(0x44e060, "the Medius game info");
}
std::optional<GuiHost::LobbyCachedGame> GuiHost::lobby_game_cached(std::int32_t) { return std::nullopt; }
std::vector<GuiHost::LobbyCachedGame> GuiHost::lobby_game_cache_take() { return {}; }
std::int32_t GuiHost::lobby_join(std::int32_t) { unhosted(0x44e430, "the Medius game join"); }
void GuiHost::lobby_join_end() { unhosted(0x44e340, "the Medius join's end"); }
void GuiHost::lobby_set_state(std::int32_t) { unhosted(0x33e9d0, "the lobby state store (session +255c)"); }
void GuiHost::net_update() { unhosted(0x429d30, "the DME update"); }
std::int64_t GuiHost::lobby_clock_ms() { unhosted(0x3698b0, "the lobby join's clock"); }
std::int32_t GuiHost::lobby_own_id() { unhosted(0x2abe9c, "the account id (session +2480)"); }
GuiHost::LobbyGameList GuiHost::lobby_game_list() { unhosted(0x447790, "the Medius game list"); }
std::int32_t GuiHost::lobby_chat_send(const std::string&, std::int32_t) { unhosted(0x451050, "the lobby chat send"); }
std::vector<GuiHost::LobbyChatLine> GuiHost::lobby_chat_take() { unhosted(0x447c70, "the room chat's records"); }
GuiHost::LobbyPlayerList GuiHost::lobby_player_list() { unhosted(0x447a30, "the Medius player list"); }
std::int32_t GuiHost::lobby_contacts_open() { unhosted(0x44df80, "the lobby's contacts"); }
void GuiHost::lobby_contacts_close() { unhosted(0x44dd20, "the lobby's contacts' end"); }
GuiHost::LobbyContactList GuiHost::lobby_contact_list() { unhosted(0x446830, "the Medius buddy list"); }
std::int32_t GuiHost::lobby_contact_remove(std::int32_t) { unhosted(0x44d9c0, "the Medius buddy remove"); }
std::int32_t GuiHost::lobby_account_id(const std::string&) { unhosted(0x451370, "the Medius account id"); }
std::int32_t GuiHost::lobby_contact_add(std::int32_t) { unhosted(0x44db60, "the Medius buddy add"); }
std::optional<std::int32_t> GuiHost::lobby_player_rank(std::int32_t) { return std::nullopt; }
void GuiHost::lobby_agent_edited() {}
bool GuiHost::lobby_agent_stats(std::int32_t, std::array<std::uint8_t, 0xfc>&) {
    unhosted(0x450c60, "the agent's record (GetAccountStats)");
}
bool GuiHost::lobby_cell_leader() { unhosted(0x44a1d0, "the agency cell's leader test"); }
bool GuiHost::lobby_cell_member() { unhosted(0x44a230, "the agency cell (GetMyClans)"); }
std::string GuiHost::lobby_cell_name() { unhosted(0x44a070, "the agency cell's name"); }
std::optional<std::string> GuiHost::lobby_cell_name_of(std::int32_t) { unhosted(0x448ff0, "GetClanByID"); }
bool GuiHost::lobby_cell_has(std::int32_t) { unhosted(0x44a0d0, "the agency cell's member test"); }
bool GuiHost::lobby_is_contact(std::int32_t) { unhosted(0x44d670, "the contacts test"); }
std::int32_t GuiHost::lobby_cell_invite(std::int32_t) { unhosted(0x44ac10, "the agency cell's invitation"); }
std::int32_t GuiHost::lobby_cell_create(const std::string&) { unhosted(0x44af60, "CreateClan"); }
bool GuiHost::lobby_join_password_refused() { return false; }
void GuiHost::lobby_set_join_password(const std::string&) {
    throw std::runtime_error("gui runtime: PORT-ONLY game password with no lobby (132on2c)");
}
void GuiHost::profile_set_text(std::int32_t, std::uint32_t, const std::string&) {
    unhosted(0x129360, "profile string store");
}
void GuiHost::profiles_clear_b70() { unhosted(0x12ee30, "the other players' profile byte +b70"); }
void GuiHost::level_progress(std::int32_t, std::int32_t, std::int32_t, std::int32_t) {
    unhosted(0x332a10, "the agent's level-state list");
}
bool GuiHost::lan_hosting() { return false; }  // PORT-ONLY (132ln3): no LanSession
bool GuiHost::lan_host(const LanHostRequest&) {
    throw std::runtime_error("gui runtime: the PORT-ONLY LAN session (LanSession) is not hosted");
}
// PORT-ONLY (132ln4): no LanSession: the browser lists nothing, a join cannot start.
void GuiHost::lan_browse(bool) {}
std::vector<GuiHost::LanGame> GuiHost::lan_games() { return {}; }
bool GuiHost::lan_join(std::uint64_t) {
    throw std::runtime_error("gui runtime: the PORT-ONLY LAN session (LanSession) join is not hosted");
}
GuiHost::LanJoin GuiHost::lan_join_state() { return LanJoin::idle; }
void GuiHost::lan_join_cancel() {}
std::optional<std::string> GuiHost::lan_join_notice() { return std::nullopt; }
void GuiHost::lan_pump() {}
std::int64_t GuiHost::lan_clock_ms() { return 0; }
float GuiHost::mission_time() { unhosted(0x245890, "mission clock"); }
std::int32_t GuiHost::par_time() { unhosted(0x330c30, "mission par time"); }
bool GuiHost::objective_manager() { unhosted(0x225cd0, "ObjectiveManager lookup"); }
std::vector<GuiHost::ObjectiveRow> GuiHost::objective_rows(std::int32_t kind, std::int32_t, std::uint8_t) {
    static constexpr std::uint32_t at[4] = {0x227100, 0x2270e0, 0x2270c0, 0x2270a0};
    unhosted(kind >= 0 && kind < 4 ? at[kind] : 0x227170, "objective records");
}
std::int32_t GuiHost::objective_total() { unhosted(0x227120, "objective total"); }
void GuiHost::objective_select(std::int32_t) {
    unhosted(0x368150, "objective selection (00225cd0 / 002275a0 and the HUD map marker records)");
}
void GuiHost::sound_pause(std::int32_t) { unhosted(0x1c96d0, "gameSound pause"); }
void GuiHost::map_open() { unhosted(0x26dbc0, "the HUD map open (hud + 6a0)"); }
void GuiHost::map_close() { unhosted(0x26d720, "the HUD map close (hud + 6a0)"); }
void GuiHost::map_reset() { unhosted(0x26c130, "the HUD map reset (hud + 6a0)"); }
void GuiHost::map_hold() { unhosted(0x26d710, "the HUD map hold (hud + 6a0)"); }
bool GuiHost::map_prev() { unhosted(0x26c180, "the HUD map previous waypoint (hud + 6a0)"); }
bool GuiHost::map_next() { unhosted(0x26c3b0, "the HUD map next waypoint (hud + 6a0)"); }
void GuiHost::map_release() { unhosted(0x26d6d0, "the HUD map release (hud + 6a0)"); }
void GuiHost::pad_mask(std::int32_t) { unhosted(0x2427b0, "the pad driver's reset mask *0055a6c0"); }
void GuiHost::ticker_show(bool) { unhosted(0x3f9e30, "HUD ticker visibility"); }
void GuiHost::app_12fa60(std::int32_t) { unhosted(0x12fa60, "mission end request"); }
bool GuiHost::network_play_game_572400() { unhosted(0x4653e4, "00572400 for the network quit popup answer"); }
void GuiHost::rumble_stop() { unhosted(0x26bd60, "pad vibration stop"); }
void GuiHost::rumble_start(std::int32_t, std::int32_t, std::int32_t, std::int32_t, float) {
    unhosted(0x26bde0, "pad vibration start");
}
void GuiHost::rumble_update(float) { unhosted(0x26bc20, "pad vibration update"); }
void GuiHost::rumble_enable(std::uint8_t) { unhosted(0x26be70, "pad vibration enable"); }
bool GuiHost::options_player() { unhosted(0x45a3c0, "the player (*004ffc0c) of the options"); }
void GuiHost::options_player_byte(std::uint32_t, std::uint8_t) { unhosted(0x45a3d4, "the player's option bytes"); }
bool GuiHost::sound_manager() { unhosted(0x3caa50, "GameSoundManager lookup"); }
void GuiHost::sound_volume(std::int32_t, std::int32_t) { unhosted(0x1c9540, "gameSound channel volume"); }
void GuiHost::sound_mode(std::int32_t) { unhosted(0x1c9670, "gameSound output mode"); }
void GuiHost::options_sound(std::int32_t kind, std::int32_t, const std::string& name) {
    static constexpr std::uint32_t at[3] = {0x20ecb0, 0x20faa0, 0x20f2a0};
    unhosted(kind >= 0 && kind < 3 ? at[kind] : 0x3c9420, "options screen sound '" + name + "'");
}
void GuiHost::options_frontend_sleep() { unhosted(0x3eb750, "front-end options sleep (003eb750 / 0036b250)"); }
void GuiHost::app_suspend(std::uint32_t) { unhosted(0x2c9860, "the app suspend"); }
void GuiHost::app_resume() { unhosted(0x2c9510, "the app resume"); }
bool GuiHost::boot_video_pal() { unhosted(0x3cba90, "BOOT_VIDEOMODE"); }
void GuiHost::video_mode_set(std::uint8_t) { unhosted(0x165420, "the video mode set (frame buffer and GS reset)"); }
void GuiHost::options_frontend_wake() { unhosted(0x36b3b0, "front-end options wake (0036b250 / 0036b3b0)"); }
void GuiHost::port_options_wake(Widget*) {}
bool GuiHost::port_options_pressed(Widget*, Widget*) { return false; }
std::int32_t GuiHost::pda_count() { unhosted(0x454280, "the HUD's PDA quip records (*004ffc2c +4)"); }
GuiHost::PdaRecord GuiHost::pda_record(std::int32_t) { unhosted(0x4542f4, "a HUD PDA quip record"); }
void GuiHost::pda_mark(std::int32_t) { unhosted(0x454530, "the HUD PDA record's replayed byte"); }
void GuiHost::radio_sound(std::int32_t kind, std::int32_t, const std::string& name, std::int32_t) {
    unhosted(kind == 0 ? 0x20ecb0 : 0x20f2a0, "radio log sound '" + name + "'");
}
void GuiHost::subtitle_voice(const std::string& name) {
    unhosted(0x45ceec, "the PlayXA voice of the subtitle " + name + " (001ca210 / 0020f2a0 / 003c9420 / 003c9d60)");
}
std::uint32_t GuiHost::zeus_archive() { unhosted(0x326e48, "the ZEUS archive (*(*004ffc04 + 25c))"); }
std::int32_t GuiHost::zeus_lookup(std::uint32_t, const std::string& name) {
    unhosted(0x36d2a0, "ZEUS archive member lookup '" + name + "'");
}
std::int32_t GuiHost::zeus_has(std::int32_t, std::int32_t, std::int32_t, std::int32_t) {
    unhosted(0x3329b0, "the agent's ZEUS unlock");
}
std::int32_t GuiHost::zeus_viewed(std::int32_t, std::int32_t, std::int32_t, std::int32_t) {
    unhosted(0x332950, "the agent's ZEUS viewed flag");
}
GuiHost::BitmapTexture GuiHost::bitmap_load(std::uint32_t, const std::string& name) {
    unhosted(0x380a40, "bitmap texture load '" + name + "'");
}
void GuiHost::bitmap_release(std::int32_t) { unhosted(0x3813a0, "bitmap texture release"); }
bool GuiHost::equip_player() { unhosted(0x454fa0, "the player (*004ffc0c) of the equipment pane"); }
std::int32_t GuiHost::equip_held() { unhosted(0x176110, "the player's held weapon"); }
std::array<std::int32_t, 8> GuiHost::equip_slots() { unhosted(0x454fd0, "the player actor's inventory slots"); }
std::vector<GuiHost::ItemStack> GuiHost::equip_stacks() { unhosted(0x470560, "the player actor's item list (+3528)"); }
std::int32_t GuiHost::weapon_class(std::int32_t) { unhosted(0x143d20, "weapon slot class"); }
bool GuiHost::weapon_is_held(std::int32_t) { unhosted(0x1437e0, "the held weapon record"); }
void GuiHost::equip_item(std::int32_t) { unhosted(0x175ff0, "equip the player's weapon"); }
std::uint8_t GuiHost::goggles_33e7() { unhosted(0x454c94, "the player actor's goggles byte +33e7"); }
void GuiHost::goggles_toggle() { unhosted(0x199860, "goggles toggle"); }
std::int32_t GuiHost::clips_for(std::int32_t) { unhosted(0x1861f0, "the player actor's clips"); }
void GuiHost::holster_current() { unhosted(0x143b60, "holster the player's weapon"); }
std::int32_t GuiHost::weapon_spare(std::int32_t) { unhosted(0x143870, "the player's spare rounds"); }
std::int32_t GuiHost::weapon_clip(std::int32_t) { unhosted(0x143940, "the player's clip rounds"); }
bool GuiHost::weapon_has_clip(std::int32_t) { unhosted(0x143710, "the weapon's clip test"); }
std::optional<std::int32_t> GuiHost::hand_model() { return std::nullopt; }
std::optional<GuiHost::WeaponRecord> GuiHost::weapon_record(std::int32_t) { unhosted(0x147840, "weapon record"); }
void GuiHost::weapon_preload(std::int32_t) { unhosted(0x145710, "weapon model preload"); }
std::optional<std::string> GuiHost::item_name(std::int32_t) { unhosted(0x470270, "item name"); }
std::optional<std::string> GuiHost::item_desc(std::int32_t) { unhosted(0x470240, "item description"); }
std::int32_t GuiHost::item_model(std::int32_t) { unhosted(0x4702a0, "item resource model"); }
std::optional<std::int32_t> GuiHost::scores_player(std::int32_t) { unhosted(0x3fb8d0, "the game's player list"); }
bool GuiHost::scores_agent_b70(std::int32_t) { unhosted(0x185c70, "the player's agent record"); }
std::string GuiHost::scores_agent_name(std::int32_t) { unhosted(0x336170, "the agent name"); }
std::int32_t GuiHost::scores_statistic(std::int32_t, std::uint32_t) { unhosted(0x333ee0, "the agent statistics"); }
std::array<std::int32_t, 10> GuiHost::scores_attach(std::int32_t) {
    unhosted(0x3927a0, "the player's render model attach table");
}
std::optional<float> GuiHost::scores_connection_status(std::int32_t) {
    unhosted(0x42b320, "the player's measured network connection status");
}
// Port-only (checkpoint 132f5): no Cheats entry unless a host offers one.
bool GuiHost::port_cheat_menu() { return false; }
// Port-only (checkpoint 132win11): no Port Settings screen unless a host offers one.
PortSettingsScreen* GuiHost::port_settings_screen() { return nullptr; }
std::vector<std::int32_t> GuiHost::eqm_unlocked_items() { unhosted(0x332ac0, "the agent's unlocked weapons"); }
bool GuiHost::eqm_byte_51ee38() { return false; }  // BSS, never written by the original
std::int32_t GuiHost::weapon_record_count() { unhosted(0x2bf234, "the weapon database's record count"); }
void GuiHost::eqm_listed(const std::vector<std::int32_t>&) {}
bool GuiHost::eqm_dual_wield() { unhosted(0x332f90, "the agent's dual wield perk"); }
std::string GuiHost::eqm_agent_rank() { unhosted(0x336110, "the agent rank"); }
std::optional<std::string> GuiHost::string_after(const std::string&, std::int32_t) {
    unhosted(0x3fdb80, "the string table by id");
}
void GuiHost::weapon_unload(std::int32_t) { unhosted(0x145630, "weapon model unload"); }
void GuiHost::eqm_accept(const std::array<std::int32_t, 10>&, std::int32_t) {
    unhosted(0x143b60, "the player's inventory from the loadout");
}
void GuiHost::eqm_start() { unhosted(0x36a0f0, "the level start after the loadout screen"); }
void GuiHost::eqm_resume() { unhosted(0x12eee0, "the game resume after the loadout screen"); }
std::int32_t GuiHost::port_cheat_count() { return 0; }
bool GuiHost::port_cheat_action(std::int32_t) { return false; }
bool GuiHost::port_cheat_on(std::int32_t) { return false; }
bool GuiHost::port_cheat_toggle(std::int32_t) { throw std::runtime_error("port cheats: no cheat host"); }
omega::AgentPartCache* GuiHost::agent_parts() { unhosted(0x22a220, "the front end's character part cache (0055dda0)"); }
std::vector<std::string> GuiHost::agent_locked_parts(std::int32_t) { unhosted(0x332500, "the agent's locked parts"); }
void GuiHost::agent_store_appearance(std::int32_t, const omega::AgentPacking&, const omega::AgentPartCache&) {
    unhosted(0x334000, "the agent's appearance store");
}
void GuiHost::profile_set_byte(std::int32_t, std::uint32_t, std::uint8_t) { unhosted(0x1314f0, "profile byte store"); }
std::int32_t GuiHost::loc_find(const std::string& key) { unhosted(0x3fe650, "Loc_FindKey '" + key + "'"); }
std::optional<std::string> GuiHost::loc_text(std::int32_t) { unhosted(0x3fdb80, "Loc_GetTextById"); }
void GuiHost::app_40f540() { unhosted(0x40f540, "the world's pending clear"); }
std::vector<std::uint8_t> GuiHost::skl_file(const std::string& path) {
    unhosted(0x402840, "Skl_Load's " + path);
}
void GuiHost::profile_set_word(std::int32_t, std::uint32_t, std::int32_t) { unhosted(0x1314f0, "profile word store"); }

std::uint32_t options_diff(const GameOptionsRecord& a, const GameOptionsRecord& b) {  // 0045a600
    std::uint32_t d = 0;
    if (a.manual_aim != b.manual_aim) d |= 1;
    if (a.rumble != b.rumble) d |= 2;
    if (a.freelook_x != b.freelook_x) d |= 4;
    if (a.freelook_y != b.freelook_y) d |= 8;
    if (ftz(a.sfx) != ftz(b.sfx)) d |= 0x10;  // c.eq.s
    if (ftz(a.voices) != ftz(b.voices)) d |= 0x20;
    if (ftz(a.music) != ftz(b.music)) d |= 0x40;
    if (a.sound_mode != b.sound_mode) d |= 0x80;
    if (ftz(a.gamma) != ftz(b.gamma)) d |= 0x400;
    if (a.video_mode != b.video_mode) d |= 0x800;
    return d;
}

namespace {
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
std::uint32_t get32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
}  // namespace

std::array<std::uint8_t, options_file_size> options_file_bytes(const GameOptionsRecord& r) {  // 0045a2a0
    std::array<std::uint8_t, options_file_size> b{};
    put32(&b[0], static_cast<std::uint32_t>(r.w0));
    b[4] = r.manual_aim;
    b[5] = r.rumble;
    b[6] = r.freelook_x;
    b[7] = r.freelook_y;
    put32(&b[8], std::bit_cast<std::uint32_t>(r.sfx));
    put32(&b[0xc], std::bit_cast<std::uint32_t>(r.voices));
    put32(&b[0x10], std::bit_cast<std::uint32_t>(r.music));
    put32(&b[0x14], static_cast<std::uint32_t>(r.sound_mode));
    b[0x18] = r.b18;
    b[0x19] = r.b19;
    put32(&b[0x1c], std::bit_cast<std::uint32_t>(r.gamma));
    put32(&b[0x20], 0);  // 0045a35c: +20 is 0 in the file
    return b;
}

GameOptionsRecord options_file_record(std::span<const std::uint8_t, options_file_size> b) {  // 0045a20c..0045a274
    GameOptionsRecord r;
    r.w0 = static_cast<std::int32_t>(get32(&b[0]));
    r.manual_aim = b[4];
    r.rumble = b[5];
    r.freelook_x = b[6];
    r.freelook_y = b[7];
    r.sfx = std::bit_cast<float>(get32(&b[8]));
    r.voices = std::bit_cast<float>(get32(&b[0xc]));
    r.music = std::bit_cast<float>(get32(&b[0x10]));
    r.sound_mode = static_cast<std::int32_t>(get32(&b[0x14]));
    r.b18 = b[0x18];
    r.b19 = b[0x19];
    r.gamma = std::bit_cast<float>(get32(&b[0x1c]));
    r.video_mode = static_cast<std::int32_t>(get32(&b[0x20]));
    return r;
}

const std::array<const char*, 18> cc_level_names = {
    "TORONTO1", "TORONTO2", "TORONTO3", "ITALY",    "BELARUS1", "BELARUS2", "KYRGSTAN", "YEMEN1",  "YEMEN2",
    "MINSK",    "CHECHNYA", "LORELEI",  "TOKYO",    "MYANMAR",  "ZURICH",   "MNTNEGR1", "UKRAINE", "TRAINING"};

GuiRuntime::GuiRuntime(GuiHost& host) : host_(host) {
    // 00414900 / 004158c0: the frontend canvas, enabled, keyboard path off.
    canvas_.cls = WidgetClass::canvas;
    canvas_.name = "canvas";
    canvas_.flags = 0x26;
    canvas_.loader = &canvas_;
}

void GuiRuntime::canvas_destroy() {  // 004147b0
    if (!canvas_alive_) return;
    // 00465e10 -> 00415f00: while the stack (+28) is not empty, vt +68
    // (00414be0) of its last node's screen (*(+2c) + 8).
    while (!canvas_.children.empty()) pop(canvas_.children.back());
    destroy(&canvas_);   // 0041f370 + 0036b130
    canvas_alive_ = false;
}

void GuiRuntime::canvas_create() {  // 00414900
    if (canvas_alive_) return;
    // 00466960 (GuiFrontendCanvas: +6c = 0) over the GuiCanvas constructor
    // 004160a0: +48 / +4c / +50 / +60 / +64 = 0, +51 = +52 = 1, +68 = 0xf000.
    // The widget part is the first canvas's (the constructor above).
    canvas_ = Widget{};
    canvas_.cls = WidgetClass::canvas;
    canvas_.name = "canvas";
    canvas_.flags = 0x26;
    canvas_.loader = &canvas_;
    phase = 0.0f;
    last_dt = 0.0f;
    pending_pop = pending_push = nullptr;
    pending = 0;
    iterating_ = false;
    keyboard_path = 1;
    ascii_keys = 1;
    repeat_mask = 0xf000;
    pending_list_.clear();
    canvas_alive_ = true;
}

// ---------------------------------------------------------------- loading

Widget* GuiRuntime::make_widget(WidgetClass cls) {
    auto w = std::make_unique<Widget>();
    w->cls = cls;
    w->flags = 0x26;                                   // 0041f9e0 GuiWidget ctor
    if (cls == WidgetClass::button || cls == WidgetClass::edit) w->flags |= flag_focusable;  // 00420d50 / 0041bd30
    if (cls == WidgetClass::keyboard) {  // 0032d2a0
        w->flags &= ~flag_releasable;
        w->kb = std::make_unique<Widget::Keyboard>();
    }
    if (cls == WidgetClass::textlist) {  // 00421bf0 over the GuiArrayWidget ctor 00424c50
        w->flags |= flag_focusable;
        w->list = std::make_unique<Widget::List>();
    }
    if (cls == WidgetClass::cmdcentr) {  // 002984d0 (net screen ctor 0033d580)
        w->flags &= ~flag_releasable;
        w->cc = std::make_unique<Widget::CmdCenter>();
        w->cc->open.fill(1);
    }
    if (cls == WidgetClass::objectives || cls == WidgetClass::startmenu) {  // 00368e10(1, 1)
        w->ob = std::make_unique<Widget::Objectives>();
        if (cls == WidgetClass::startmenu) w->flags &= ~flag_releasable;  // 00456d90
    }
    if (cls == WidgetClass::startback) w->flags &= ~flag_releasable;
    if (cls == WidgetClass::slider) {  // 0043f910
        w->flags |= flag_focusable;
        w->sl = std::make_unique<Widget::Slider>();
    }
    if (cls == WidgetClass::gameopt) w->go = std::make_unique<Widget::GameOpt>();  // 0029ba10 (+48 = -2)
    if (cls == WidgetClass::scrollbar) w->sb = std::make_unique<Widget::ScrollBar>();  // 0043bef0
    if (cls == WidgetClass::mltext) w->ml = std::make_unique<Widget::MLText>();       // 0043e5d0
    // Checkpoint 132ln4: GuiChatWindow is GuiMLTextWidget (0043e5d0) with its own name / IsType / destroy only
    // (vtable 004ddce0: every other entry is 004e1040's).
    if (cls == WidgetClass::chatwin) w->ml = std::make_unique<Widget::MLText>();
    if (cls == WidgetClass::radiolog) w->rl = std::make_unique<Widget::RadioLog>();   // 00457988
    if (cls == WidgetClass::subtitle) {  // 00458590
        w->flags &= ~flag_focusable;
        w->st = std::make_unique<Widget::SubTitle>();
    }
    if (cls == WidgetClass::slideshow) w->ss = std::make_unique<Widget::SlideShow>();  // 00328660
    if (cls == WidgetClass::bitmap) w->bm = std::make_unique<Widget::Bitmap>();        // 0043cb20
    if (cls == WidgetClass::textarray || cls == WidgetClass::weaponstats) {  // 00426cc0 / 00455bf0 over 00424c50
        w->list = std::make_unique<Widget::List>();
        w->list->mode = 0;
        w->list->rows = 1;
        w->ta = std::make_unique<Widget::TextArray>();
    }
    if (cls == WidgetClass::weapondisplay) w->wd = std::make_unique<Widget::WeaponDisplay>();  // 004412f0
    if (cls == WidgetClass::equippane) w->ep = std::make_unique<Widget::EquipPane>();
    if (cls == WidgetClass::scoresequip) {  // 0035d430
        w->se = std::make_unique<Widget::ScoresEquip>();
        if (g.b_21c8) w->se->pos_1e0[0] = {-3.2f, 8.0f, -2.0f, 1.0f};
    }
    if (cls == WidgetClass::port_cheats) w->pc = std::make_unique<Widget::PortCheats>();
    if (cls == WidgetClass::port_settings) w->port_settings_rows = std::make_unique<Widget::PortSettingsRows>();
    if (cls == WidgetClass::eqmodify) w->em = std::make_unique<Widget::EquipModify>();
    if (cls == WidgetClass::chardisplay) w->bm = std::make_unique<Widget::Bitmap>();   // PORT-ONLY: the 132cc4b preview texture
    if (cls == WidgetClass::agentsave) {  // 00293fc0: the GuiSaveAgentWidget (+48 -2), +78 10.0f, byte +7c 1
        w->as = std::make_unique<Widget::AgentSave>();
        w->al_7c = 1;
    }
    // Checkpoint 132f11: the debrief's screens.
    if (cls == WidgetClass::endobjectives) {  // 00462de0: 00368e10(1, 0), the voice queue at +90 (00462fa0)
        w->ob = std::make_unique<Widget::Objectives>();
        w->ob->b89 = 0;
        w->vq = std::make_unique<Widget::VoiceQueue>();
    }
    if (cls == WidgetClass::endstats) {  // the factory at 00464758: 0035d430, the voice queue at +260 (00462fa0), +60 = 0
        w->se = std::make_unique<Widget::ScoresEquip>();
        w->se->b60 = 0;
        if (g.b_21c8) w->se->pos_1e0[0] = {-3.2f, 8.0f, -2.0f, 1.0f};
        w->es = std::make_unique<Widget::EndStats>();
        w->vq = std::make_unique<Widget::VoiceQueue>();
    }
    if (cls == WidgetClass::endawards) {  // the factory at 00460d04: 0041f690, the entries (+1c0) cleared, the queue at +350
        w->aw = std::make_unique<Widget::Awards>();
        w->vq = std::make_unique<Widget::VoiceQueue>();
    }
    if (cls == WidgetClass::autosaver) w->asv = std::make_unique<Widget::AutoSaver>();  // the factory at 004620f8: 0041f690, +48 = -2
    // Checkpoint 132f12: the personnel statistics file.
    if (cls == WidgetClass::missionstats) w->ps = std::make_unique<Widget::Personnel>();  // 0034fcb0 over 00356ff0
    // Checkpoint 132f13: the other personnel files (each ctor over 00356ff0: +68 = 1).
    if (cls == WidgetClass::dossiers || cls == WidgetClass::ranks || cls == WidgetClass::ratings ||
        cls == WidgetClass::omega || cls == WidgetClass::achieve || cls == WidgetClass::pmenu) {
        w->ps = std::make_unique<Widget::Personnel>();
        if (cls == WidgetClass::ratings) w->ps->kind_img = 3;  // 0034df40
        if (cls == WidgetClass::ranks) w->ps->kind_img = 6;    // the factory 0034e890's ctor
        if (cls == WidgetClass::omega) w->ps->kind_img = 8;    // the factory 0034f210's ctor
    }
    if (cls == WidgetClass::missionstatlist) {  // 00350460 over 00426cc0: list mode 2, focusable
        w->list = std::make_unique<Widget::List>();
        w->list->mode = 2;
        w->list->rows = 1;
        w->ta = std::make_unique<Widget::TextArray>();
        w->msl = std::make_unique<Widget::MissionStatList>();
        w->flags |= flag_focusable;
    }
    // Checkpoint 132f14: the dossier's agent page and its figure.
    if (cls == WidgetClass::agentinfo) w->ai = std::make_unique<Widget::AgentInfo>();  // 00361d90: +84 8, +fc 0
    // Checkpoint 132ln2: 0029f2a0 over the net screen 0033d580 (+48 -2, +78 / +80 0; the lists 0044fa00).
    if (cls == WidgetClass::onlinecn) w->oc = std::make_unique<Widget::OnlineConnect>();
    // Checkpoint 132ln3: 002a19c0 over the net screen 0033d580 (+84 6, +dc..+f4 0, +f8 0; +f0 / +f4 the app).
    if (cls == WidgetClass::msnsetup) w->ms = std::make_unique<Widget::MissionSetup>();
    if (cls == WidgetClass::acntlogn) w->acl = std::make_unique<Widget::AccountLogin>();
    // Checkpoint 132on2c: 002aa110 / 00347a40 over the net screen 0033d580 (+84 5 / 9), and 00348fc0 over
    // GuiArrayWidget 00424c50 (+58 2, +90..+a4 0, +a8 GetString("TOSSystem") or "").
    if (cls == WidgetClass::onlinepref) w->opt = std::make_unique<Widget::OnlineOptions>();
    if (cls == WidgetClass::netmess) w->nm = std::make_unique<Widget::NetMessages>();
    if (cls == WidgetClass::netmsgarray) {
        w->list = std::make_unique<Widget::List>();
        w->list->rows = 1;
        w->list->mode = 2;
        w->nma = std::make_unique<Widget::NetMsgArray>();
        if (const auto s = host_.get_string("TOSSystem")) w->nma->system = s->substr(0, s->find('\0'));
    }
    // Checkpoint 132ln4: 002a9340 over GuiLobbyScreen 0033f480 over the net screen 0033d580 (+84 2, +88..+a8 0,
    // +94 1, +100 -1, the lists 0044fa00; +160 / +164 0), and 002acea0 over GuiArrayWidget 00424c50 (+58 2, +ac /
    // +ad 1, +b8 / +c0 / +c4 / +cc -1, the row vector 002ad080).
    if (cls == WidgetClass::netjoin) w->lb = std::make_unique<Widget::Lobby>();
    if (cls == WidgetClass::netcontacts) {  // checkpoint 132on2h: 002adfc0 over 0033f480 (+84 4, +160 / +164 0)
        w->lb = std::make_unique<Widget::Lobby>();
        w->lb->w84 = 4;
    }
    if (cls == WidgetClass::netagencycell) {  // checkpoint 132on2i: 002aff20 over 0033f480 (+84 3)
        w->lb = std::make_unique<Widget::Lobby>();
        w->lb->w84 = 3;
    }
    if (cls == WidgetClass::missionlist) {
        w->list = std::make_unique<Widget::List>();
        w->list->rows = 1;  // 00424c50: +48 / +4c 1, +58 0, +5c 0x18
        w->list->mode = 2;  // 002acf00
        w->mls = std::make_unique<Widget::MissionList>();
    }
    if (cls == WidgetClass::agentdisplay) {  // 00364590 over 0041f690: +80 = (0, 0, pi, 0), +48 0, +4e0 0
        w->ad = std::make_unique<Widget::AgentDisplay>();
        w->ad->turns = {0.0f, 0.0f, std::bit_cast<float>(0x40490fdbu), 0.0f};
    }
    widgets_.push_back(std::move(w));
    Widget* made = widgets_.back().get();
    if (cls == WidgetClass::agentmod) am_construct(made);  // 00343e10
    if (cls == WidgetClass::agentcreate) ac_construct(made);  // 0028f760
    if (cls == WidgetClass::eqmodify) em_build(made);  // 002c1100
    return made;
}

Widget* GuiRuntime::new_widget(const std::string& name) {  // 0041f960
    if (host_.class_stopped(name)) return nullptr;
    // Port-only (checkpoint 132f5): the Cheats screen is not a registered class.
    if (name == "PortGuiCheats") return host_.port_cheat_menu() ? make_widget(WidgetClass::port_cheats) : nullptr;
    if (name == "PortGuiSettings") return host_.port_settings_screen() ? make_widget(WidgetClass::port_settings) : nullptr;
    const char* hit = nullptr;
    for (const char* c : registered_classes)
        if (iequal(name, c)) {
            hit = c;
            break;
        }
    if (!hit) return nullptr;
    const std::string id = hit;
    if (id == "GuiWidget") return make_widget(WidgetClass::widget);
    if (id == "GuiTextWidget") return make_widget(WidgetClass::text);
    if (id == "GuiButtonWidget") return make_widget(WidgetClass::button);
    if (id == "GuiTitleScreen") return make_widget(WidgetClass::title);
    if (id == "GuiTextEditWidget") return make_widget(WidgetClass::edit);
    if (id == "GuiMessageBox") return make_widget(WidgetClass::msgbox);
    if (id == "GuiKeyboard") return make_widget(WidgetClass::keyboard);
    if (id == "GuiTextListWidget") return make_widget(WidgetClass::textlist);
    if (id == "GuiAgentLoad") return make_widget(WidgetClass::agentload);  // 002940b0
    if (id == "GuiOnlineOfflineSelect") return make_widget(WidgetClass::onoffsel);  // 002a0c30
    if (id == "GuiCommandCenter") return make_widget(WidgetClass::cmdcentr);  // 002984d0
    if (id == "GuiObjectives") return make_widget(WidgetClass::objectives);  // 00368f00
    if (id == "GuiStartMenu") return make_widget(WidgetClass::startmenu);  // 00456d90
    if (id == "GuiStartMenuBackdrop") return make_widget(WidgetClass::startback);
    if (id == "GuiSliderWidget") return make_widget(WidgetClass::slider);  // 0043f910
    if (id == "GuiGameOptions") return make_widget(WidgetClass::gameopt);  // 0029ba10
    if (id == "GuiScrollBarWidget") return make_widget(WidgetClass::scrollbar);  // 0043bef0
    if (id == "GuiMLTextWidget") return make_widget(WidgetClass::mltext);  // 0043e5d0
    if (id == "GuiBriefingPane") return make_widget(WidgetClass::briefing);  // 004576c0 (0041f690)
    if (id == "GuiRadioLog") return make_widget(WidgetClass::radiolog);    // 00457930 (0041f690)
    if (id == "GuiSubTitleDisplay") return make_widget(WidgetClass::subtitle);  // 00458590 (0041f690)
    if (id == "GuiSlideShow") return make_widget(WidgetClass::slideshow);      // 00328660 (0041f690)
    if (id == "GuiBitmapWidget") return make_widget(WidgetClass::bitmap);      // 0043cb20 (0041f690)
    if (id == "GuiTextArrayWidget") return make_widget(WidgetClass::textarray);  // 00426cc0
    if (id == "GuiWeaponStats") return make_widget(WidgetClass::weaponstats);    // 00455bf0
    if (id == "GuiWeaponDisplay") return make_widget(WidgetClass::weapondisplay);  // 004412f0
    if (id == "GuiEquipmentPane") return make_widget(WidgetClass::equippane);    // vtable 004e12f0 (0041f690)
    if (id == "GuiCharacterDisplay") return make_widget(WidgetClass::chardisplay);  // 00326860
    if (id == "GuiEquipmentModify") return make_widget(WidgetClass::eqmodify);  // 002c1100 (vtable 004ddee0)
    if (id == "GuiScoresEquip") return make_widget(WidgetClass::scoresequip);    // 0035d430
    if (id == "GuiAgentModify") return make_widget(WidgetClass::agentmod);     // 00343e10 (00441d00)
    if (id == "GuiAgentCreate") return make_widget(WidgetClass::agentcreate);  // 0028f760 (0041f690)
    if (id == "GuiAgentSave") return make_widget(WidgetClass::agentsave);      // 00293fc0 (00441d00)
    if (id == "GuiMapScreen") return make_widget(WidgetClass::mapscreen);      // 00464bf0 (0041f690)
    if (id == "GuiEndGameObjectives") return make_widget(WidgetClass::endobjectives);  // 00462de0 (00368e10)
    if (id == "GuiEndGameStatistics") return make_widget(WidgetClass::endstats);   // 00464758 (0035d430)
    if (id == "GuiEndGameAwards") return make_widget(WidgetClass::endawards);      // 00460d04 (0041f690)
    if (id == "GuiAutoSaver") return make_widget(WidgetClass::autosaver);          // 004620f8 (vtable 004e16d0)
    if (id == "GuiMissionStatistics") return make_widget(WidgetClass::missionstats);  // 0034fcb0 (vtable 004dee70)
    if (id == "GuiMissionStatList") return make_widget(WidgetClass::missionstatlist);  // 00350460 (vtable 004deee0)
    if (id == "GuiDossiers") return make_widget(WidgetClass::dossiers);            // 0034a240 (vtable 004dec30)
    if (id == "GuiRatings") return make_widget(WidgetClass::ratings);              // 0034e370 (ctor 0034df40)
    if (id == "GuiRanks") return make_widget(WidgetClass::ranks);                  // 0034e890 (vtable 004ded50)
    if (id == "GuiOmegaStrain") return make_widget(WidgetClass::omega);            // 0034f210 (vtable 004dede0)
    if (id == "GuiAchievementScreen") return make_widget(WidgetClass::achieve);    // 00354490 (vtable 004def90)
    if (id == "GuiPersonnelMenu") return make_widget(WidgetClass::pmenu);          // 00356600 (ctor 00356730)
    if (id == "GuiAgentInfo") return make_widget(WidgetClass::agentinfo);          // ctor 00361d90 (vtable 004df350)
    if (id == "GuiAgentDisplay") return make_widget(WidgetClass::agentdisplay);    // ctor 00364590 (vtable 004df530)
    if (id == "GuiOnlineConnect") return make_widget(WidgetClass::onlinecn);       // ctor 0029f2a0 (vtable 004dd620)
    if (id == "GuiAccountLogin") return make_widget(WidgetClass::acntlogn);        // ctor 0028b2c0 (vtable 004dd120)
    if (id == "GuiMissionSetup") return make_widget(WidgetClass::msnsetup);        // ctor 002a19c0 (vtable 004dd730)
    if (id == "GuiNetJoinMission") return make_widget(WidgetClass::netjoin);       // ctor 002a9340 (vtable 004dda50)
    if (id == "GuiMissionList") return make_widget(WidgetClass::missionlist);      // ctor 002acea0 (vtable 004ddb80)
    if (id == "GuiChatWindow") return make_widget(WidgetClass::chatwin);           // 0043e5d0 (vtable 004ddce0)
    if (id == "GuiOnlineOptions") return make_widget(WidgetClass::onlinepref);     // ctor 002aa110 (vtable 004ddaf0)
    if (id == "GuiNetMessages") return make_widget(WidgetClass::netmess);          // ctor 00347a40 (vtable 004deb10)
    if (id == "GuiNetMessageArray") return make_widget(WidgetClass::netmsgarray);  // ctor 00348fc0 (vtable 004deb80)
    if (id == "GuiNetContacts") return make_widget(WidgetClass::netcontacts);      // ctor 002adfc0 (vtable 004ddc20)
    if (id == "GuiNetAgencyCell") return make_widget(WidgetClass::netagencycell);  // ctor 002aff20 (vtable 004ddd40)
    unhosted(0x41f960, "widget class '" + id + "'");
}

void GuiRuntime::load_node(const GuiNode& n, Widget* w) {
    w->loader = w;
    // 0041f210 base Load
    w->name = n.name;
    w->rect = n.rect;
    if (n.flag2) w->flags |= flag_visible; else w->flags &= ~flag_visible;
    if (n.flag4) w->flags |= flag_enabled; else w->flags &= ~flag_enabled;
    if (is_text(w)) {
        // 0041c560 text Load
        if (!n.text) unhosted(0x41c560, "text widget without text record");
        set_text(w, n.text->text, true);
        w->font = host_.font(n.text->font);
        w->rgb = n.text->rgb;
        w->alpha = 1.0f;
        w->halign = n.text->halign;
    }
    if (w->cls == WidgetClass::edit) {  // 0041ace0
        if (!n.edit) unhosted(0x41ace0, "text edit widget without its record");
        w->max_len = n.edit->max_len;
        w->password = n.edit->password != 0;
    }
    if (!n.decorator_class.empty()) {
        if (n.decorator_class != "GuiInterfaceDecorator" || !n.decorator)
            unhosted(0x418b10, "decorator class '" + n.decorator_class + "'");
        auto d = std::make_unique<Decorator>();  // 00417bc0
        d->ie = n.decorator->ie.empty() ? g.iface : n.decorator->ie;
        d->node = n.decorator->node;
        const auto& m = n.decorator->matrix;  // 00416f70: 00131e90 rows 0..2 (w 0), 00131e70 the translation (w 1)
        d->matrix = {m[0], m[1], m[2], 0.0f, m[3], m[4], m[5], 0.0f, m[6], m[7], m[8], 0.0f, m[9], m[10], m[11], 1.0f};
        d->events = n.decorator->events;
        w->deco = std::move(d);
        ++deco_matrix_serial;
    }
    for (const GuiNode& child : n.children) {
        // 0041f960 returns 0 for an unregistered name; 00418b10 then stops
        // reading the node, which leaves the stream inside it.
        Widget* c = new_widget(child.class_name);
        if (!c) unhosted(0x41f960, "unregistered widget class '" + child.class_name + "'");
        load_node(child, c);
        c->loader = w;
        c->parent = w;
        w->children.push_back(c);
    }
}

void GuiRuntime::add_root(Widget* w) {  // 00419530
    if (w->flags & flag_init) return;
    roots.push_back(w);
    o_init(w);
}

Widget* GuiRuntime::load(const GuiNode& file_root, WidgetClass root_class) {
    if (root_class == WidgetClass::canvas) unhosted(0x41f960, "root widget class");
    if (file_root.class_name != "GuiWidget") unhosted(0x418b10, "file root class '" + file_root.class_name + "'");
    Widget* root = make_widget(root_class);
    load_node(file_root, root);
    add_root(root);
    return root;
}

Widget* GuiRuntime::load_into(const GuiNode& file_root, Widget* root) {  // 00418fd0
    // 00418b10 with a root: the file's root class must be "GuiWidget" (00129218), else nothing is read.
    if (file_root.class_name != "GuiWidget") return nullptr;
    load_node(file_root, root);
    add_root(root);
    return root;
}

Widget* GuiRuntime::find(Widget* w, const std::string& name) const {
    if (!w) return nullptr;
    if (w->name == name) return w;
    for (Widget* c : w->children)
        if (Widget* r = find(c, name)) return r;
    return nullptr;
}

// ---------------------------------------------------------------- owner dispatch
// Widget +44 is the "owner": the GuiInterfaceDecorator (vtable 004e09f0) when
// one is attached, otherwise the widget itself.

void GuiRuntime::o_init(Widget* w) {
    v_init(w);
    if (w->deco) {  // 00416c30: interface element of the decorator
        const std::int32_t id = host_.ie_lookup(w->deco->ie);
        w->deco->ie_id = id;
        if (id != -1) unhosted(0x413370, "interface element '" + w->deco->ie + "'");
    }
}
void GuiRuntime::o_wake(Widget* w) {  // the decorator 00416b30: vt +1c, then 00416d10(d, "onWake")
    v_wake(w);
    if (w->deco && deco_wake) deco_wake(w);
}
void GuiRuntime::o_sleep(Widget* w) {  // the decorator 00416b80: vt +20, then 00416d10(d, "onSleep")
    v_sleep(w);
    if (w->deco && deco_sleep) deco_sleep(w);
}
void GuiRuntime::o_gain(Widget* w) { v_gain(w); }  // the decorator 00416f00 forwards to the widget's vt +24
void GuiRuntime::o_lose(Widget* w) { v_lose(w); }
void GuiRuntime::o_activate(Widget* w) { bubble(w, 2); }  // 0041e230
void GuiRuntime::o_update(Widget* w, float dt) {
    if (w->deco) {  // 00417790: the element step, then the sleep wait (0x10 and 0x8: clear 0x8 once settled)
        if (deco_step) deco_step(w, dt);
        if ((w->flags & flag_sleep_pending) && (w->flags & flag_awake) && (!deco_settled || deco_settled(w)))
            w->flags &= ~flag_awake;
    }
    v_update(w, dt);
}
std::uint8_t GuiRuntime::o_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {
    if (w == &canvas_) return canvas_event(sender, type, a3, t0);
    if (w->deco) return deco_event(w, sender, type, a3, t0);
    return v_event(w, sender, type, a3, t0);
}

// ---------------------------------------------------------------- own vtables

void GuiRuntime::v_init(Widget* w) {
    if (w->cls == WidgetClass::msgbox) {
        msgbox_init(w);
        return;
    }
    if (w->cls == WidgetClass::keyboard) {
        kb_init(w);
        return;
    }
    if (w->cls == WidgetClass::textlist) {
        list_init(w);
        return;
    }
    if (w->cls == WidgetClass::onoffsel) {
        oo_init(w);
        return;
    }
    if (w->cls == WidgetClass::cmdcentr) {
        cc_init(w);
        return;
    }
    if (w->cls == WidgetClass::objectives || w->cls == WidgetClass::endobjectives) {  // 003688c0
        ob_init(w);
        return;
    }
    if (w->cls == WidgetClass::startmenu) {
        sm_init(w);
        return;
    }
    if (w->cls == WidgetClass::startback) {
        bd_init(w);
        return;
    }
    if (w->cls == WidgetClass::mapscreen) {
        mp_init(w);
        return;
    }
    if (w->cls == WidgetClass::slider) {
        sl_init(w);
        return;
    }
    if (w->cls == WidgetClass::gameopt) {
        go_init(w);
        return;
    }
    if (w->cls == WidgetClass::scrollbar) {
        sb_init(w);
        return;
    }
    if (w->cls == WidgetClass::mltext || w->cls == WidgetClass::chatwin) {
        ml_init(w);
        return;
    }
    if (w->cls == WidgetClass::briefing) {
        br_init(w);
        return;
    }
    if (w->cls == WidgetClass::radiolog) {
        rl_init(w);
        return;
    }
    if (w->cls == WidgetClass::slideshow) {
        ss_init(w);
        return;
    }
    if (w->cls == WidgetClass::textarray || w->cls == WidgetClass::missionstatlist) {  // 004262d0 (both vtables)
        ta_init(w);
        return;
    }
    if (w->cls == WidgetClass::missionstats) {
        ms_init(w);
        return;
    }
    if (w->cls == WidgetClass::dossiers || w->cls == WidgetClass::ranks || w->cls == WidgetClass::omega) {
        pe_init(w);  // their vtables keep 00356cd0
        return;
    }
    if (w->cls == WidgetClass::ratings) {
        rt_init(w);
        return;
    }
    if (w->cls == WidgetClass::achieve) {
        ah_init(w);
        return;
    }
    if (w->cls == WidgetClass::pmenu) {
        pm_init(w);
        return;
    }
    if (w->cls == WidgetClass::agentinfo) {
        ai_init(w);
        return;
    }
    if (w->cls == WidgetClass::onlinecn) {
        oc_init(w);
        return;
    }
    if (w->cls == WidgetClass::msnsetup) {
        mset_init(w);
        return;
    }
    if (w->cls == WidgetClass::acntlogn) {  // 0028a5c0
        acl_init(w);
        return;
    }
    if (w->cls == WidgetClass::onlinepref) {  // 002a9fe0
        opt_init(w);
        return;
    }
    if (w->cls == WidgetClass::netmess) {  // 00347820
        nm_init(w);
        return;
    }
    if (w->cls == WidgetClass::netmsgarray) {  // 00348d90
        nma_init(w);
        return;
    }
    if (w->cls == WidgetClass::netjoin || w->cls == WidgetClass::netcontacts ||
        w->cls == WidgetClass::netagencycell) {  // 002a9240 / 002adf00 / 002afdc0
        lb_init(w);
        return;
    }
    if (w->cls == WidgetClass::missionlist) {  // 002accf0
        mlist_init(w);
        return;
    }
    if (w->cls == WidgetClass::agentdisplay) {  // 003643a0: 0041f150, then the light object and the actor
        base_init(w);  // (the host's agent preview)
        return;
    }
    if (w->cls == WidgetClass::weaponstats) {
        ws_init(w);
        return;
    }
    if (w->cls == WidgetClass::weapondisplay) {
        wd_init(w);
        return;
    }
    if (w->cls == WidgetClass::equippane) {
        ep_init(w);
        return;
    }
    if (w->cls == WidgetClass::scoresequip || w->cls == WidgetClass::endstats) {  // 0035cd00
        se_init(w);
        return;
    }
    if (w->cls == WidgetClass::endawards) {
        aw_init(w);
        return;
    }
    if (w->cls == WidgetClass::port_cheats) {
        pc_init(w);
        return;
    }
    if (w->cls == WidgetClass::port_settings) {
        ps_init(w);
        return;
    }
    if (w->cls == WidgetClass::eqmodify) {
        em_init(w);
        return;
    }
    if (w->cls == WidgetClass::agentmod) {
        am_init(w);
        return;
    }
    if (w->cls == WidgetClass::agentcreate) {
        ac_init(w);
        return;
    }
    base_init(w);
    if (w->cls == WidgetClass::title) w->demo = host_.demo_mode() != 0;  // 002888e0
}
void GuiRuntime::v_wake(Widget* w) {
    switch (w->cls) {
    case WidgetClass::text: text_wake(w); break;
    case WidgetClass::button:  // 00420be0
        text_wake(w);
        if (w->obj) w->obj->b58 = 1;
        break;
    case WidgetClass::edit:  // 0041ad60
        text_wake(w);
        if (w->password) edit_mask(w, w->text.size());
        break;
    case WidgetClass::title: title_wake(w); break;
    case WidgetClass::msgbox: msgbox_wake(w); break;
    case WidgetClass::keyboard: kb_wake(w); break;
    case WidgetClass::textlist: list_wake(w); break;
    case WidgetClass::agentload: al_wake(w); break;
    case WidgetClass::onoffsel: oo_wake(w); break;
    case WidgetClass::cmdcentr: cc_wake(w); break;
    case WidgetClass::objectives: ob_wake(w); break;
    case WidgetClass::startmenu: sm_wake(w); break;
    case WidgetClass::startback: bd_wake(w); break;
    case WidgetClass::mapscreen: mp_wake(w); break;
    case WidgetClass::gameopt: go_wake(w); break;
    case WidgetClass::scrollbar:  // 0043bde0
        base_wake(w);
        sb_frame(w, 0xa);
        break;
    case WidgetClass::mltext:  // 0043e2e0
    case WidgetClass::chatwin:
        base_wake(w);
        ml_sync(w);
        break;
    case WidgetClass::briefing: br_wake(w); break;
    case WidgetClass::subtitle: st_wake(w); break;
    case WidgetClass::slideshow: ss_wake(w); break;
    case WidgetClass::agentmod: am_wake(w); break;
    case WidgetClass::agentcreate: ac_wake(w); break;
    case WidgetClass::agentsave: as_wake(w); break;
    case WidgetClass::bitmap:  // 0043caf0
        base_wake(w);
        bm_load(w);
        break;
    case WidgetClass::textarray:
    case WidgetClass::missionstatlist:
    case WidgetClass::weaponstats: ta_wake(w); break;
    case WidgetClass::missionstats: ms_wake(w); break;
    case WidgetClass::dossiers:
    case WidgetClass::ranks: pe_wake(w); break;
    case WidgetClass::ratings: rt_wake(w); break;
    case WidgetClass::omega: om_wake(w); break;
    case WidgetClass::achieve: ah_wake(w); break;
    case WidgetClass::pmenu: pm_wake(w); break;
    case WidgetClass::agentinfo: ai_wake(w); break;
    case WidgetClass::onlinecn: oc_wake(w); break;
    case WidgetClass::msnsetup: mset_wake(w); break;
    case WidgetClass::acntlogn: acl_wake(w); break;         // 0028a4b0
    case WidgetClass::onlinepref: opt_wake(w); break;       // 0033d3a0
    case WidgetClass::netmess: nm_wake(w); break;           // 003477d0
    case WidgetClass::netmsgarray: nma_wake(w); break;      // 00348b90
    case WidgetClass::netjoin:
    case WidgetClass::netcontacts: lb_wake(w); break;       // 0033f110
    // Checkpoint 132on2i: omega_server keeps no agency cells, so the navigator releases AGENTCEL before it wakes
    // (0044a230 false); the screen's own handlers (wake 002afa80, sleep 002afa20, update 002af2a0, event 002af310)
    // are a later delivery.
    case WidgetClass::netagencycell: unhosted(0x2afa80, "GuiNetAgencyCell's wake (an agency cell member)");
    case WidgetClass::missionlist: mlist_wake(w); break;    // 002abd50
    case WidgetClass::agentdisplay: ad_wake(w); break;
    case WidgetClass::equippane: ep_wake(w); break;
    case WidgetClass::scoresequip: se_wake(w); break;
    case WidgetClass::port_cheats: pc_wake(w); break;
    case WidgetClass::port_settings: ps_wake(w); break;
    case WidgetClass::endobjectives: eo_wake(w); break;
    case WidgetClass::endstats: es_wake(w); break;
    case WidgetClass::endawards: aw_wake(w); break;
    case WidgetClass::autosaver: asv_wake(w); break;
    default: base_wake(w); break;
    }
}
void GuiRuntime::v_sleep(Widget* w) {
    if (is_text(w)) text_sleep(w);
    else if (w->cls == WidgetClass::msgbox) msgbox_sleep(w);
    else if (w->cls == WidgetClass::keyboard) kb_sleep(w);
    else if (w->cls == WidgetClass::textlist) list_sleep(w);
    else if (w->cls == WidgetClass::cmdcentr) cc_sleep(w);
    else if (w->cls == WidgetClass::startmenu) sm_sleep(w);
    else if (w->cls == WidgetClass::startback) bd_sleep(w);
    else if (w->cls == WidgetClass::mapscreen) mp_sleep(w);
    else if (w->cls == WidgetClass::gameopt) go_sleep(w);
    else if (w->cls == WidgetClass::radiolog) rl_sleep(w);
    else if (w->cls == WidgetClass::subtitle) st_sleep(w);
    else if (w->cls == WidgetClass::textarray || w->cls == WidgetClass::weaponstats ||
             w->cls == WidgetClass::missionstatlist) ta_sleep(w);
    else if (w->cls == WidgetClass::slideshow) ss_sleep(w);
    else if (w->cls == WidgetClass::agentmod) am_sleep(w);
    else if (w->cls == WidgetClass::agentcreate) ac_sleep(w);
    else if (w->cls == WidgetClass::ratings) rt_sleep(w);
    else if (w->cls == WidgetClass::pmenu) pm_sleep(w);
    else if (w->cls == WidgetClass::agentinfo) ai_sleep(w);
    else if (w->cls == WidgetClass::onlinecn) oc_sleep(w);
    else if (w->cls == WidgetClass::msnsetup) mset_sleep(w);
    else if (w->cls == WidgetClass::acntlogn) acl_sleep(w);       // 0033d360
    else if (w->cls == WidgetClass::onlinepref) opt_sleep(w);     // 0033d360
    else if (w->cls == WidgetClass::netmess) nm_sleep(w);         // 0033d360
    else if (w->cls == WidgetClass::netmsgarray) nma_sleep(w);    // 00348ad0
    else if (w->cls == WidgetClass::netjoin || w->cls == WidgetClass::netcontacts) lb_sleep(w);  // 0033f050
    else if (w->cls == WidgetClass::netagencycell) unhosted(0x2afa20, "GuiNetAgencyCell's sleep");
    else if (w->cls == WidgetClass::missionlist) mlist_sleep(w); // 002abc90
    else if (w->cls == WidgetClass::bitmap) {  // 0043caa0
        base_sleep(w);
        bm_release(w);
    } else base_sleep(w);
}
void GuiRuntime::v_gain(Widget* w) {
    if (w->cls == WidgetClass::slider) {
        sl_gain(w);
        return;
    }
    bubble(w, 0);  // 0041e330
    if (w->cls == WidgetClass::textlist || w->cls == WidgetClass::textarray ||
        w->cls == WidgetClass::weaponstats || w->cls == WidgetClass::missionstatlist ||
        w->cls == WidgetClass::missionlist || w->cls == WidgetClass::netmsgarray)  // 00424a10: then vt +6c
        array_vt6c(w, w->list->col, w->list->row);
}
void GuiRuntime::v_lose(Widget* w) {
    if (w->cls == WidgetClass::slider) {
        sl_lose(w);
        return;
    }
    bubble(w, 1);  // 0041e2b0
    if (w->cls == WidgetClass::textlist || w->cls == WidgetClass::textarray ||
        w->cls == WidgetClass::weaponstats || w->cls == WidgetClass::missionstatlist ||
        w->cls == WidgetClass::missionlist || w->cls == WidgetClass::netmsgarray)  // 004249d0: then vt +70 004235c0
        send_up(w, 0x15, static_cast<std::int32_t>(w->list->col), static_cast<std::int32_t>(w->list->row));
}
std::uint8_t GuiRuntime::v_input(Widget* w, const PadEvent& ev) {
    if (w == game_screen) return gs_input_d420(ev);  // GuiGameScreen's input 0045d420
    switch (w->cls) {
    case WidgetClass::canvas: return canvas_navigate(ev);
    case WidgetClass::title:  // 00288260
        w->last_input = now_seconds();
        return 0;
    case WidgetClass::button:  // 00420c90
        if (ev.kind == 1 && ev.port == 0 && ev.mask == pad_cross && ev.released == 0) {
            send(w, w, 0x16, 0, 0);
            return 1;
        }
        return 0;
    case WidgetClass::edit: return edit_input(w, ev);
    case WidgetClass::msgbox: return msgbox_input(w, ev);
    case WidgetClass::keyboard: return kb_input(w, ev);
    case WidgetClass::textlist: return array_input(w, ev);
    case WidgetClass::agentload: return al_input(w, ev);
    case WidgetClass::onoffsel: return oo_input(w, ev);
    case WidgetClass::cmdcentr: return cc_input(w, ev);
    case WidgetClass::objectives: return ob_input(w, ev);
    case WidgetClass::startmenu: return sm_input(w, ev);
    case WidgetClass::slider: return sl_input(w, ev);
    case WidgetClass::gameopt: return go_input(w, ev);
    case WidgetClass::briefing: return br_input(w, ev);
    case WidgetClass::radiolog: return rl_input(w, ev);
    case WidgetClass::slideshow: return ss_input(w, ev);
    case WidgetClass::textarray: return array_input(w, ev);
    case WidgetClass::missionstatlist: return array_input(w, ev);  // 00423cc0
    case WidgetClass::missionstats: return ms_input(w, ev);
    case WidgetClass::dossiers:
    case WidgetClass::ranks:
    case WidgetClass::omega:
    case WidgetClass::achieve: return pe_input(w, ev);  // 00356be0
    case WidgetClass::ratings: return rt_input(w, ev);
    case WidgetClass::pmenu: return pm_input(w, ev);
    case WidgetClass::agentinfo: return ai_input(w, ev);
    case WidgetClass::onlinecn: return oc_input(w, ev);
    case WidgetClass::msnsetup: return mset_input(w, ev);
    case WidgetClass::acntlogn: return acl_input(w, ev);              // 00289990
    case WidgetClass::onlinepref: return opt_input(w, ev);            // 002a98d0
    // 003473e0 rewrites Right / Left in the caller's event; dispatch_input hands every input call its own copy.
    case WidgetClass::netmess: return nm_input(w, const_cast<PadEvent&>(ev));
    case WidgetClass::netmsgarray: return array_input(w, ev);         // 00423cc0
    case WidgetClass::netjoin:
    case WidgetClass::netcontacts:
    case WidgetClass::netagencycell: return lb_input(w, ev);          // 0033da00
    case WidgetClass::missionlist: return array_input(w, ev);         // 00423cc0
    case WidgetClass::weaponstats: return ws_input(w, ev);
    case WidgetClass::equippane: return ep_input(w, ev);
    case WidgetClass::scoresequip: return se_input(w, ev);
    case WidgetClass::port_cheats: return pc_input(w, ev);
    case WidgetClass::port_settings: return ps_input(w, ev);
    case WidgetClass::eqmodify: return em_input(w, ev);
    case WidgetClass::agentmod: return am_input(w, ev);
    case WidgetClass::agentcreate: return ac_input(w, ev);
    case WidgetClass::agentsave: return as_input(w, ev);
    case WidgetClass::mapscreen: return mp_input(w, ev);
    case WidgetClass::endobjectives: return eo_input(w, ev);
    case WidgetClass::endstats: return es_input(w, ev);
    case WidgetClass::endawards: return aw_input(w, ev);
    default: return 0;  // 0041df70 (GuiStartMenuBackdrop / GuiSubTitleDisplay / GuiBitmapWidget / GuiCharacterDisplay: 00293950)
    }
}
void GuiRuntime::v_update(Widget* w, float dt) {
    if (w == game_screen) {  // GuiGameScreen 0045d960: 0041e3b0, then its own update
        base_update(w, dt);
        gs_update_d960(dt);
        return;
    }
    if (w->cls == WidgetClass::title) title_update(w, dt);
    else if (w->cls == WidgetClass::edit) edit_update(w, dt);
    else if (w->cls == WidgetClass::keyboard) kb_update(w, dt);
    else if (w->cls == WidgetClass::agentload) al_update(w, dt);
    else if (w->cls == WidgetClass::onoffsel) oo_update(w, dt);
    else if (w->cls == WidgetClass::cmdcentr) cc_update(w, dt);
    else if (w->cls == WidgetClass::objectives || w->cls == WidgetClass::startmenu) ob_update(w, dt);
    else if (w->cls == WidgetClass::gameopt) go_update(w, dt);
    else if (w->cls == WidgetClass::radiolog) rl_update(w, dt);
    else if (w->cls == WidgetClass::subtitle) st_update(w, dt);
    else if (w->cls == WidgetClass::weapondisplay) wd_update(w, dt);
    else if (w->cls == WidgetClass::scoresequip) se_update(w, dt);
    else if (w->cls == WidgetClass::port_settings) ps_update(w, dt);
    else if (w->cls == WidgetClass::eqmodify) {  // 002c03c0: 0041e3b0, then the network game's host wait
        base_update(w, dt);
        if (g.b_21c8) unhosted(0x2c0400, "EQMODIFY's network countdown (003fb8d0 / *00506518)");
    }
    else if (w->cls == WidgetClass::agentmod) am_update(w, dt);
    else if (w->cls == WidgetClass::agentcreate) ac_update(w, dt);
    else if (w->cls == WidgetClass::agentinfo) ai_update(w, dt);
    else if (w->cls == WidgetClass::onlinecn) oc_update(w, dt);
    else if (w->cls == WidgetClass::msnsetup) mset_update(w, dt);
    else if (w->cls == WidgetClass::acntlogn) acl_net_update(w, dt);  // 0033d310
    else if (w->cls == WidgetClass::onlinepref) opt_net_update(w, dt);  // 0033d310
    else if (w->cls == WidgetClass::netmess) nm_net_update(w, dt);      // 0033d310
    else if (w->cls == WidgetClass::netmsgarray) nma_update(w, dt);     // 00348a00
    else if (w->cls == WidgetClass::netjoin || w->cls == WidgetClass::netcontacts) lb_update(w, dt);  // 0033de00
    else if (w->cls == WidgetClass::netagencycell) unhosted(0x2af2a0, "GuiNetAgencyCell's update");
    else if (w->cls == WidgetClass::missionlist) mlist_update(w, dt); // 002acca0
    else if (w->cls == WidgetClass::agentdisplay) ad_update(w, dt);
    else if (w->cls == WidgetClass::agentsave) al_update(w, dt);  // 00293660 (GuiAgentLoad's)
    else if (w->cls == WidgetClass::endobjectives) {  // 00462270
        vq_update(w, dt);
        ob_update(w, dt);
    } else if (w->cls == WidgetClass::endstats) {  // 004637d0
        vq_update(w, dt);
        se_update(w, dt);
    } else if (w->cls == WidgetClass::endawards) {  // 00461020
        vq_update(w, dt);
        base_update(w, dt);
    }
    else base_update(w, dt);
}
std::uint8_t GuiRuntime::v_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {
    if (w == game_screen) return gs_event_d3d0(sender, type, a3, t0);  // GuiGameScreen's event 0045d3d0
    switch (w->cls) {
    case WidgetClass::canvas: return canvas_event(sender, type, a3, t0);
    case WidgetClass::title: return title_event(w, sender, type, a3, t0);
    case WidgetClass::msgbox: return msgbox_event(w, sender, type, a3, t0);
    case WidgetClass::keyboard: return kb_event(w, sender, type, a3, t0);
    case WidgetClass::agentload: return al_event(w, sender, type, a3, t0);
    case WidgetClass::onoffsel: return oo_event(w, sender, type, a3, t0);
    case WidgetClass::cmdcentr: return cc_event(w, sender, type, a3, t0);
    case WidgetClass::objectives: return ob_event(w, sender, type, a3, t0);
    case WidgetClass::startmenu: return sm_event(w, sender, type, a3, t0);
    case WidgetClass::gameopt: return go_event(w, sender, type, a3, t0);
    case WidgetClass::agentmod: return am_event(w, sender, type, a3, t0);
    case WidgetClass::agentcreate: return ac_event(w, sender, type, a3, t0);
    case WidgetClass::agentsave: return as_event(w, sender, type, a3, t0);
    case WidgetClass::radiolog: return rl_event(w, sender, type, a3, t0);
    case WidgetClass::scoresequip: return se_event(w, sender, type, a3, t0);
    case WidgetClass::endobjectives: return ob_event(w, sender, type, a3, t0);  // 003680a0
    case WidgetClass::endstats: return es_event(w, sender, type, a3, t0);
    case WidgetClass::autosaver: return save_widget_event(w, sender, type, a3, t0);  // 00441ca0
    case WidgetClass::missionstats: return ms_event(w, sender, type, a3, t0);       // 0034fa40
    case WidgetClass::dossiers:
    case WidgetClass::ranks:
    case WidgetClass::omega:  // 0034ef00: a jump to 00356b30
    case WidgetClass::achieve:
    case WidgetClass::pmenu: return pe_event(w, sender, type, a3, t0);  // 00356b30
    case WidgetClass::ratings: return rt_event(w, sender, type, a3, t0);  // 0034d600
    case WidgetClass::agentinfo: return ai_event(w, sender, type, a3, t0);  // 00361730
    case WidgetClass::onlinecn: return oc_event(w, sender, type, a3, t0);   // 0029e440
    case WidgetClass::msnsetup: return mset_event(w, sender, type, a3, t0);   // 002a1140
    case WidgetClass::acntlogn: return acl_event(w, sender, type, a3, t0);    // 00289c80
    case WidgetClass::onlinepref: return opt_event(w, sender, type, a3, t0);  // 002a97f0
    case WidgetClass::netmess: return nm_event(w, sender, type, a3, t0);      // 00346d60
    case WidgetClass::netjoin: return jm_event(w, sender, type, a3, t0);      // 002a8d60
    case WidgetClass::netcontacts: return ct_event(w, sender, type, a3, t0);  // 002adab0 (132on2h)
    case WidgetClass::netagencycell: unhosted(0x2af310, "GuiNetAgencyCell's event");
    case WidgetClass::port_cheats: return pc_event(w, sender, type, a3, t0);
    case WidgetClass::port_settings: return ps_event(w, sender, type, a3, t0);
    case WidgetClass::eqmodify: return em_event(w, sender, type, a3, t0);
    case WidgetClass::startback:  // 00456f40
        if ((type & 0xffff) == 0x10 && (a3 == 2 || a3 == 3)) return 1;
        return base_event(w, sender, type, a3, t0);
    case WidgetClass::button:  // 00420c20
        if ((type & 0xffff) == 0x16 && sender == w) {
            send_up(w, 0x10, 1, 0);
            o_activate(w);
            return 1;
        }
        return base_event(w, sender, type, a3, t0);
    default: return base_event(w, sender, type, a3, t0);
    }
}

bool GuiRuntime::selectable(const Widget* w) const {
    return w->loader == w ||
           ((w->flags & flag_focusable) && (w->flags & flag_enabled) && (w->flags & flag_visible));
}

Widget* GuiRuntime::v_first(Widget* w) {
    if (w->cls == WidgetClass::cmdcentr) return cc_first(w);
    if (w->cls == WidgetClass::startmenu) return sm_first(w);
    if (w->cls == WidgetClass::ratings) return rt_first(w);  // 0034dbe0
    if (w->cls == WidgetClass::endstats) {  // 00463940
        Widget* r = v_first_base(w);
        if (!r) unhosted(0x46395c, "GuiEndGameStatistics first with no focusable widget");
        if (r != w) r->flags &= ~flag_visible;
        return w;
    }
    return v_first_base(w);
}

Widget* GuiRuntime::v_first_base(Widget* w) {  // 0041d8c0
    for (Widget* c : w->children) {
        if ((c->flags & flag_enabled) && (c->flags & flag_visible)) {
            if (Widget* r = v_first(c)) {
                w->focus = r;
                return r;
            }
        }
    }
    return selectable(w) ? w : nullptr;
}

Widget* GuiRuntime::v_last(Widget* w) {  // 0041d780
    for (auto it = w->children.rbegin(); it != w->children.rend(); ++it) {
        Widget* c = *it;
        if ((c->flags & flag_enabled) && (c->flags & flag_visible)) {
            if (Widget* r = v_last(c)) {
                w->focus = r;
                return r;
            }
        }
    }
    return selectable(w) ? w : nullptr;
}

Widget* GuiRuntime::v_next(Widget* w, Widget* from, bool reset) {  // 0041d5c0
    if (reset) g.next_from = nullptr;
    if (g.next_from && selectable(w)) return w;
    if (from == w) g.next_from = w;
    Widget* r = nullptr;
    for (Widget* c : w->children) {
        if ((c->flags & flag_enabled) && (c->flags & flag_visible)) {
            r = v_next(c, from, false);
            if (r) break;
        }
    }
    w->focus = r;
    if (reset && !w->focus) w->focus = v_first(w);
    return w->focus;
}

Widget* GuiRuntime::v_prev(Widget* w, Widget* from, bool reset) {  // 0041d400
    if (reset) g.prev_from = nullptr;
    if (g.prev_from && selectable(w)) return w;
    if (from == w) g.prev_from = w;
    Widget* r = nullptr;
    for (auto it = w->children.rbegin(); it != w->children.rend(); ++it) {
        Widget* c = *it;
        if ((c->flags & flag_enabled) && (c->flags & flag_visible)) {
            r = v_prev(c, from, false);
            if (r) break;
        }
    }
    w->focus = r;
    if (reset && !w->focus) w->focus = v_last(w);
    return w->focus;
}

// ---------------------------------------------------------------- GuiWidget base

void GuiRuntime::base_init(Widget* w) {
    w->flags |= flag_init;
    for (std::size_t i = 0; i < w->children.size(); ++i) o_init(w->children[i]);
}

void GuiRuntime::base_wake(Widget* w) {
    if (w->flags & flag_awake) return;
    w->flags |= flag_awake;
    for (std::size_t i = 0; i < w->children.size(); ++i) o_wake(w->children[i]);
}

void GuiRuntime::base_sleep(Widget* w) {
    if (!(w->flags & flag_awake)) return;
    w->flags |= flag_sleep_pending;
    const std::uint8_t r = o_event(w, w, 0xf, 0, 0);
    if (r) w->flags &= ~flag_awake;
    else w->flags |= flag_awake;
    for (std::size_t i = 0; i < w->children.size(); ++i) o_sleep(w->children[i]);
    check_sleep(w);
}

void GuiRuntime::check_sleep(Widget* w) {  // 0041ed20
    if (!(w->flags & flag_sleep_pending) || (w->flags & flag_awake)) return;
    for (std::size_t i = 0; i < w->children.size(); ++i) {
        Widget* c = w->children[i];
        if (!c || !o_event(c, w, 0xf, 0, 0)) return;
    }
    w->flags &= ~flag_sleep_pending;
    if (w->parent) o_event(w->parent, w, 3, 0, 0);
}

void GuiRuntime::clear_awake(Widget* w) {  // 0041ee50
    w->flags &= ~(flag_awake | flag_sleep_pending);
    for (Widget* c : w->children) clear_awake(c);
}

void GuiRuntime::base_update(Widget* w, float dt) {  // 0041e3b0
    for (std::size_t i = 0; i < w->children.size(); ++i) o_update(w->children[i], dt);
    check_sleep(w);
}

std::uint8_t GuiRuntime::base_event(Widget* w, Widget*, std::uint32_t type, std::int32_t, std::int32_t) {
    switch (type & 0xffff) {
    case 0x19:
        if (w->flags & flag_releasable) {
            release(w);

            return 1;
        }
        return 0;
    case 0xf:
        w->flags &= ~flag_awake;
        return 1;
    default: return 0;
    }
}

std::uint8_t GuiRuntime::send(Widget* sender, Widget* target, std::uint32_t type, std::int32_t a3, std::int32_t t0) {
    observed_t0 = t0;
    if (send_observer) send_observer(sender, target, type, a3);
    return target ? o_event(target, sender, type, a3, t0) : 0;  // 0041e0f0
}

std::uint8_t GuiRuntime::send_up(Widget* w, std::uint32_t type, std::int32_t a3, std::int32_t t0) {  // 0041e040
    if (up_observer) up_observer(w, type, a3, t0);
    for (Widget* s = w->parent; s; s = s->parent)
        if (o_event(s, w, type, a3, t0)) return 1;
    return 0;
}

std::uint8_t GuiRuntime::bubble(Widget* w, std::uint32_t type) {
    for (Widget* s = w->parent; s; s = s->parent)
        if (o_event(s, w, type, 0, 0)) return 1;
    return 0;
}

std::uint8_t GuiRuntime::broadcast(Widget* w, std::uint32_t type, std::int32_t a3, std::int32_t t0) {  // 0041df00
    for (std::size_t i = 0; i < w->children.size(); ++i) {
        Widget* c = w->children[i];
        observed_t0 = t0;
        if (broadcast_observer) broadcast_observer(w, c, type, a3);
        if (o_event(c, w, type, a3, t0)) return 1;
        if (broadcast(c, type, a3, t0)) return 1;
    }
    return 0;
}

void GuiRuntime::app_broadcast(std::uint32_t type, std::int32_t a1, std::int32_t a2) {  // 002cca40
    const std::vector<Widget*> copy = canvas_.children;  // 00139070 / 002cce50: a copy of canvas +28
    for (Widget* w : copy)
        if (std::find(canvas_.children.begin(), canvas_.children.end(), w) != canvas_.children.end())
            send(&canvas_, w, type & 0xffff, a1, a2);
}

// ---------------------------------------------------------------- GuiTextWidget

void GuiRuntime::set_text(Widget* w, const std::string& s, bool translate) {  // 0041beb0
    if (translate && s.size() >= 2 && s[0] == '$') {
        if (s[1] != '$') {
            const auto v = host_.get_string(s.substr(1));
            w->text = v ? *v : std::string();
            return;
        }
        w->text = s.substr(1);
    } else {
        w->text = s;
    }
    v_set_text(w);
}

void GuiRuntime::v_set_text(Widget* w) {
    text_changed(w);
    if (w->cls != WidgetClass::edit) return;
    if (w->password) edit_mask(w, w->text.size());  // 0041aa10
    edit_cursor(w, w->cursor);
}

void GuiRuntime::text_changed(Widget* w) {  // 0041be50
    if (w->obj) {
        host_.set_text(w->text);
        w->obj->text = w->text;
    }
    send_up(w, 0x1c, 0, 0);  // a3 is the text pointer; no hosted handler reads it
}

// ---------------------------------------------------------------- GuiTextEditWidget

void GuiRuntime::edit_mask(Widget* w, std::size_t n) {  // 0041ae90
    if (!w->obj) return;
    const std::string s(n, '#');
    host_.set_text(s);
    w->obj->text = s;
}

std::int32_t GuiRuntime::edit_cursor(Widget* w, std::int32_t a) {  // 0041a100
    const auto len = static_cast<std::int32_t>(w->text.size());
    if (a == -1) a = len;
    if (len < a) {
        w->cursor = len;
        return 0;
    }
    if (a < 0) {
        w->cursor = 0;
        return 0;
    }
    w->cursor = a;
    return 1;
}

std::uint8_t GuiRuntime::edit_input(Widget* w, const PadEvent& ev) {  // 0041aa80
    if (ev.kind == 2 && ev.port == 0 && ev.released == 0) {
        const bool ascii = ascii_keys != 0;  // 004147a0()->+52
        const std::uint16_t code = ev.mask;
        if ((ascii && code == 0x2a) || code == 0x4c || (!ascii && code == 8) || code == 0x804c) {
            edit_delete(w, code == 0x4c || code == 0x804c);
            return 1;
        }
        if ((ascii && code == 0x28) || (!ascii && code == 0xa)) {
            o_activate(w);  // +44 vt +34
            return 1;
        }
        const auto max = static_cast<std::int32_t>(w->max_len);
        if (ev.ch != 0 && (max == -1 || static_cast<std::int32_t>(w->text.size()) < max)) {
            edit_insert(w, std::string(1, static_cast<char>(ev.ch)));
            return 1;
        }
    }
    if (ev.kind == 1 && ev.port == 0 && ev.mask == pad_cross && ev.released == 0) {
        o_activate(w);
        return 1;
    }
    return 0;
}

std::uint8_t GuiRuntime::edit_delete(Widget* w, bool forward) {  // 0041a180
    edit_cursor(w, w->cursor);
    const auto len = static_cast<std::int32_t>(w->text.size());
    if (len == 0) return 0;
    if (forward) {
        if (w->cursor == len) return 0;
        w->text.erase(static_cast<std::size_t>(w->cursor), 1);  // 0013b500(+48, cursor, 1)
    } else {
        if (w->cursor == 0) return 0;
        w->text.erase(static_cast<std::size_t>(w->cursor - 1), 1);
        --w->cursor;
    }
    v_set_text(w);
    return 1;
}

std::uint8_t GuiRuntime::edit_insert(Widget* w, const std::string& s) {  // 0041a2c0
    const auto n = static_cast<std::int32_t>(s.size());
    const auto max = static_cast<std::int32_t>(w->max_len);
    if (max > 0 && !(n + static_cast<std::int32_t>(w->text.size()) < max)) return 0;
    if (w->cursor < 0 || static_cast<std::size_t>(w->cursor) > w->text.size())
        unhosted(0x3b52b0, "substr out of range");
    const auto c = static_cast<std::size_t>(w->cursor);
    w->text = w->text.substr(0, c) + s + w->text.substr(c);  // 003b52b0 / 002072b0 / 0013d680
    edit_cursor(w, w->cursor + n);
    v_set_text(w);
    return 1;
}

void GuiRuntime::edit_update(Widget* w, float dt) {  // 0041a8f0
    base_update(w, dt);
    blink = fmodf_ee(fadd(blink, fmul(std::bit_cast<float>(0x42055555u), dt)), 500.0f);  // 004bf1c0
    const float f5 = fdiv(fmul(2.0f, blink), 500.0f);
    const float f4 = std::fabs(fsub(1.0f, f5));
    static constexpr std::array<std::uint32_t, 4> lo = {0x3ec0c0c1u, 0x3efcfcfdu, 0x3f048485u, 0x3f800000u};  // 004bf1a0
    for (int i = 0; i < 4; ++i) {
        const float a = std::bit_cast<float>(lo[i]);
        blink_rgba[i] = fadd(a, fmul(f4, fsub(1.0f, a)));  // 004bf1b0: all 1.0
    }
}

void GuiRuntime::set_b74(Widget* w, std::uint8_t v) {  // 0041c110
    w->b74 = v;
    if (w->obj) {
        w->obj->b12 = v == 0;
        w->obj->b11 = v == 2;
    }
}

Widget* GuiRuntime::find_sub(Widget* w, const std::string& part) const {  // 0041dc70
    if (!w) return nullptr;
    if (w->name.find(part) != std::string::npos) return w;
    for (Widget* c : w->children)
        if (Widget* r = find_sub(c, part)) return r;
    return nullptr;
}

void GuiRuntime::deinit(Widget* w) {  // 0041f090 (decorator 00417870: no IE to release)
    w->flags &= ~flag_init;
    for (std::size_t i = 0; i < w->children.size(); ++i) deinit(w->children[i]);
    if (w->cls == WidgetClass::mltext || w->cls == WidgetClass::chatwin) w->ml->obj.reset();  // 0043e0e0: then 0036b130 of +48, +48 = 0
    if (w->cls == WidgetClass::bitmap) {  // 0043ca20
        bm_release(w);
        w->bm->archive = 0;
        w->bm->name.clear();
    }
    if (w->cls == WidgetClass::agentmod) am_deinit(w);  // 00342e80: then 0041f090
    if (w->cls == WidgetClass::agentcreate) ac_deinit(w);  // 0028ec00: then 0041f090
    if (w->cls == WidgetClass::agentinfo) ai_deinit(w);    // 00361a70: 0041f090, then 0040f540
    if (w->cls == WidgetClass::agentdisplay) ad_deinit(w);  // 003641c0: 0041f090, 002c9c80(fe, 0), the actor
    if (w->cls == WidgetClass::onlinecn) oc_deinit(w);      // 0029ef30: 0041f090, then DNASLOGO's unload
    if (w->cls == WidgetClass::chardisplay && w->bm) w->bm->texture = -1;  // PORT-ONLY: the host's preview slot
    // 00460b80: 0041f090, then 002c9e20(*004ffc04) unmounts the ZEUS archive 00460bb0 mounted (the host's
    // archive stays loaded; see aw_init).
}

void GuiRuntime::text_wake(Widget* w) {  // 0041c3e0
    base_wake(w);
    if (!w->obj) {
        TextObject o;
        for (int i = 0; i < 4; ++i) o.rect[i] = cvt_w(w->rect[i], 0x41c3e0);
        o.font = w->font;
        o.rgba = {w->rgb[0], w->rgb[1], w->rgb[2], w->alpha};
        host_.text_object(o);
        w->obj = o;
    }
    if (w->obj) {
        w->obj->b12 = w->b74 == 0;
        w->obj->b11 = w->b74 == 2;
        w->obj->b58 = w->b75;
        w->obj->b5b = w->b76;
        host_.set_text(w->text);
        w->obj->text = w->text;
        w->obj->b10 = static_cast<std::uint8_t>(w->halign);
    }
}

void GuiRuntime::text_sleep(Widget* w) {  // 0041c310
    base_sleep(w);
    w->obj.reset();
}

// ---------------------------------------------------------------- GuiArrayWidget (vtable 004e0d20)

// vt +6c of an array widget: 004235e0 (the row entered: 0x14 to the parents), GuiMissionList's 002ac300.
void GuiRuntime::array_vt6c(Widget* w, std::uint32_t col, std::uint32_t row) {
    if (w->cls == WidgetClass::missionlist) {
        mlist_entered(w, col, row);
        return;
    }
    send_up(w, 0x14, static_cast<std::int32_t>(col), static_cast<std::int32_t>(row));  // 004235e0
}

void GuiRuntime::array_init(Widget* w) {  // 00424bc0
    base_init(w);
    // +8c = 0041dc70(w, "vscrollbar") when vt +58 says GuiScrollBarWidget.
    Widget* sb = find_sub(w, "vscrollbar");
    w->list->sb = sb && sb->cls == WidgetClass::scrollbar ? sb : nullptr;
    w->list->cursor = find_sub(w, "cursor");  // +88, any class
}

void GuiRuntime::array_wake(Widget* w) {  // 00424a90
    base_wake(w);
    auto& l = *w->list;
    l.col = 0;
    l.row = 0;
    l.vis_rows = f2u(fdiv(w->rect[3], u2f(l.row_h, 0x424ab0)), 0x424b04);
    if (l.cols == 1) array_set_col_width(w, 0, f2u(w->rect[2], 0x424b5c));
    array_vt6c(w, l.col, l.row);  // vt +6c
}

void GuiRuntime::array_set_dims(Widget* w, std::uint32_t cols, std::uint32_t rows) {  // 00424750
    auto& l = *w->list;
    l.cols = cols;
    l.rows = rows;
    l.vis_cols = cols;
    if (l.colw.size() != cols) {
        l.colw.clear();
        for (std::uint32_t i = 0; i < cols; ++i) l.colw.push_back(0x3c);  // 001c1080(+60, 00494158)
    }
    if (l.sb) sb_set_range(l.sb, 0, static_cast<std::int32_t>(l.row), 0, static_cast<std::int32_t>(l.rows) - 1);
}

void GuiRuntime::array_set_row_height(Widget* w, std::uint32_t h) {  // 00424820
    auto& l = *w->list;
    l.row_h = h;
    l.vis_rows = f2u(fdiv(w->rect[3], u2f(l.row_h, 0x424828)), 0x424880);
}

void GuiRuntime::array_set_col_width(Widget* w, std::uint32_t col, std::uint32_t width) {  // 004248c0
    auto& l = *w->list;
    if (col < l.cols - 1) {
        if (col >= l.colw.size()) unhosted(0x424904, "column width store past the width vector");
        l.colw[col] = static_cast<std::int32_t>(width);
    }
    if (col != l.cols - 1) return;
    std::int32_t s = cvt_w(w->rect[2], 0x424920);
    for (std::uint32_t i = 0; i < l.cols - 1; ++i) {  // 004249a0(i)
        if (i >= l.colw.size()) unhosted(0x4249b8, "column width read past the width vector");
        s = static_cast<std::int32_t>(static_cast<std::uint32_t>(s) - static_cast<std::uint32_t>(l.colw[i]));
    }
    if (s <= 0) s = static_cast<std::int32_t>(width);
    if (col >= l.colw.size()) unhosted(0x424978, "column width store past the width vector");
    l.colw[col] = s;
}

void GuiRuntime::array_scroll(Widget* w, std::uint32_t col, std::uint32_t row) {  // 00424670
    auto& l = *w->list;
    if (l.no_scroll) return;
    if (col < l.left) l.left = col;
    else if (!(col < l.left + l.vis_cols)) l.left = col - l.vis_cols + 1;
    if (row < l.top) l.top = row;
    else if (!(row < l.top + l.vis_rows)) l.top = row - l.vis_rows + 1;
    if (l.rows < l.top + l.vis_rows) {
        const auto t = static_cast<std::int32_t>(l.rows - l.vis_rows);
        l.top = t < 0 ? 0 : static_cast<std::uint32_t>(t);
    }
}

std::uint8_t GuiRuntime::array_cell_visible(const Widget* w, std::uint32_t col, std::uint32_t row) const {  // 004245d0
    const auto& l = *w->list;
    if (l.no_scroll) return 1;
    return l.left <= col && col < l.left + l.vis_cols && l.top <= row && row < l.top + l.vis_rows;
}

void GuiRuntime::array_set_visible(Widget* w, std::uint32_t cols, std::uint32_t rows) {  // 004235b0
    w->list->vis_cols = cols;
    w->list->vis_rows = rows;
}

void GuiRuntime::array_set_origin(Widget* w, std::uint32_t left, std::uint32_t top) {  // 00423580
    w->list->left = left;
    w->list->top = top;
}

void GuiRuntime::array_set_a84(Widget* w, std::int32_t v) { w->list->a84 = v; }  // 00423550

void GuiRuntime::set_pos(Widget* w, float x, float y) {  // 0041eba0
    w->rect[0] = x;
    w->rect[1] = y;
    o_event(w, w, 0x12, cvt_w(x, 0x41ebbc), cvt_w(y, 0x41ebc8));
}

void GuiRuntime::array_select(Widget* w, std::uint32_t col, std::uint32_t row) {  // 00424430
    auto& l = *w->list;
    if (l.cols == 0 && l.rows == 0) {
        l.col = col;
        l.row = row;
        return;
    }
    if (col != l.col || row != l.row) {
        send_up(w, 0x15, static_cast<std::int32_t>(l.col), static_cast<std::int32_t>(l.row));  // vt +70 004235c0
        l.col = col;
        l.row = row;
        array_vt6c(w, l.col, l.row);  // vt +6c
        if (!l.no_scroll) array_scroll(w, l.col, l.row);
    }
    if (l.cursor && l.mode == 2) {
        Widget* c = l.cursor;
        const std::uint32_t p = l.row_h * (row - l.top);  // mult: low word
        const float y = fsub(w->rect[1], u2f(p, 0x42450c));
        const float off = fdiv(fsub(c->rect[3], u2f(l.row_h, 0x424540)), 2.0f);
        set_pos(c, w->rect[0], fadd(y, off));
    }
    if (l.sb) sb_set_range(l.sb, 0, static_cast<std::int32_t>(l.row), 0, static_cast<std::int32_t>(l.rows) - 1);
}

std::uint8_t GuiRuntime::array_input(Widget* w, const PadEvent& ev) {  // 00423cc0
    auto& l = *w->list;
    auto pressed = [&](std::uint16_t m) { return ev.kind == 1 && ev.port == 0 && ev.mask == m && ev.released == 0; };
    const std::uint32_t col = l.col, row = l.row;
    auto moved = [&]() {
        if (col != l.col || row != l.row) send_up(w, 0x10, 0, 0);
        return std::uint8_t{1};
    };
    if (ev.kind == 1 && ev.port == 0) {
        if (pressed(pad_cross)) {
            send_up(w, 0x10, 1, 0);
            send_up(w, 0x13, static_cast<std::int32_t>(l.col), static_cast<std::int32_t>(l.row));
            return 1;
        }
        if (l.mode != 1 && l.rows != 0) {
            const std::uint32_t rows = l.rows;
            if (pressed(0x1000)) {
                if (row) array_select(w, col, row - 1);
                else if (l.wrap && rows >= 2) array_select(w, col, rows - 1);
                return moved();
            }
            if (pressed(0x4000)) {
                if (row != rows - 1) array_select(w, col, row + 1);
                else if (l.wrap && rows >= 2) array_select(w, col, 0);
                return moved();
            }
            if (l.paging) {
                if (pressed(0x8)) {
                    if (row && l.vis_rows >= 2) {
                        auto a = static_cast<std::int32_t>(row - (l.vis_rows - 1));
                        if (a < 0) a = l.wrap ? static_cast<std::int32_t>(rows - 1) : 0;
                        array_select(w, col, static_cast<std::uint32_t>(a));
                        moved();
                    }
                    return 1;
                }
                if (pressed(0x4)) {
                    if (row != rows - 1 && l.vis_rows >= 2) {
                        std::uint32_t a = row + l.vis_rows - 1;
                        if (!(a < rows)) a = l.wrap ? 0 : rows - 1;
                        array_select(w, col, a);
                        moved();
                    }
                    return 1;
                }
            }
        }
        if (l.mode != 2 && l.cols != 0) {
            const std::uint32_t cols = l.cols;
            if (pressed(0x2000)) {
                if (col != cols - 1) array_select(w, col + 1, row);
                else if (l.wrap && cols >= 2) array_select(w, cols - 1, row);  // the same column (00424178)
                return moved();
            }
            if (pressed(0x8000)) {
                if (col) array_select(w, col - 1, row);
                else if (l.wrap && cols >= 2) array_select(w, 0, row);
                return moved();
            }
        }
    }
    if (l.mode != 1 && (pressed(0x1000) || pressed(0x4000))) return 1;
    if (l.mode != 2 && (pressed(0x8000) || pressed(0x2000))) return 1;
    return 0;
}

// ---------------------------------------------------------------- GuiTextListWidget (vtable 004e0c80)

void GuiRuntime::list_init(Widget* w) {  // 004218f0
    array_init(w);
    auto& l = *w->list;
    l.font = host_.default_font();  // 00418420(00414790(), NULL)
    if (l.font) array_set_row_height(w, static_cast<std::uint32_t>(host_.font_height(l.font)) + 4);
}

void GuiRuntime::list_wake(Widget* w) {  // 004217d0
    array_wake(w);
    auto& l = *w->list;
    if (!l.obj) {
        TextObject o;  // 003d0940(o, 0, 0, w, h, +a8, +b0)
        o.rect = {0, 0, cvt_w(w->rect[2], 0x421880), cvt_w(w->rect[3], 0x421884)};
        o.font = l.font;
        o.rgba = l.rgba;
        host_.text_object(o);
        l.obj = o;
    }
    l.obj->b12 = 1;
    l.obj->b58 = 1;
    l.obj->b5b = l.f1;
    l.obj->b10 = l.a4;
}

void GuiRuntime::list_sleep(Widget* w) {  // 00421700
    base_sleep(w);  // 00424a50: 0041ebf0 then vt +70
    send_up(w, 0x15, static_cast<std::int32_t>(w->list->col), static_cast<std::int32_t>(w->list->row));
    w->list->obj.reset();
}

void GuiRuntime::list_refresh(Widget* w) {  // 004213e0
    array_set_dims(w, 1, static_cast<std::uint32_t>(w->list->items.size()));  // vt +5c
    array_select(w, 0, 0);
}

std::uint8_t GuiRuntime::list_add(Widget* w, const std::optional<std::string>& text, std::int32_t id,
                                  std::int8_t style) {  // 00421290
    if (!text) return 0;
    Widget::List::Item it;
    it.text = text->substr(0, text->find('\0'));
    it.id = id;
    it.style = style;
    w->list->items.push_back(std::move(it));  // 00422a50 at the end
    list_refresh(w);
    return 1;
}

void GuiRuntime::list_clear(Widget* w) {  // 00421070
    w->list->items.clear();  // 00421f90
    list_refresh(w);
}

std::uint8_t GuiRuntime::list_remove(Widget* w, std::int32_t index) {  // 004210c0
    auto& items = w->list->items;
    if (index < 0 || index >= static_cast<std::int32_t>(items.size())) return 0;
    items.erase(items.begin() + index);
    list_refresh(w);
    return 1;
}

std::optional<std::string> GuiRuntime::list_text(const Widget* w, std::int32_t index) const {  // 00421430
    const auto& items = w->list->items;
    if (index < 0 || index >= static_cast<std::int32_t>(items.size())) return std::nullopt;
    return items[static_cast<std::size_t>(index)].text;
}

std::int32_t GuiRuntime::list_id(const Widget* w, std::int32_t index) const {  // 00421480
    const auto& items = w->list->items;
    if (index < 0 || index >= static_cast<std::int32_t>(items.size())) return -1;
    return items[static_cast<std::size_t>(index)].id;
}

void GuiRuntime::list_set_style(Widget* w, std::int32_t index, std::int8_t style) {  // 004214c0
    auto& items = w->list->items;
    if (index < 0 || index >= static_cast<std::int32_t>(items.size())) return;
    items[static_cast<std::size_t>(index)].style = style;
}

std::int32_t GuiRuntime::list_style(const Widget* w, std::int32_t index) const {  // 00421500
    const auto& items = w->list->items;
    if (index < 0 || index >= static_cast<std::int32_t>(items.size())) return 0xff;
    return items[static_cast<std::size_t>(index)].style;
}

std::int32_t GuiRuntime::list_find(const Widget* w, const std::string& text) const {  // 00421540 (strcmp 00129218)
    const auto& items = w->list->items;
    const std::string key = text.substr(0, text.find('\0'));
    for (std::size_t i = 0; i < items.size(); ++i)
        if (items[i].text == key) return static_cast<std::int32_t>(i);
    return -1;
}

std::int32_t GuiRuntime::list_find_id(const Widget* w, std::int32_t id) const {  // 004215e0
    const auto& items = w->list->items;
    for (std::size_t i = 0; i < items.size(); ++i)
        if (items[i].id == id) return static_cast<std::int32_t>(i);
    return -1;
}

void GuiRuntime::list_set_f1(Widget* w, std::uint8_t v) {  // 00421650
    w->list->f1 = v;
    if (w->list->obj) w->list->obj->b5b = v;
}

void GuiRuntime::list_set_a4(Widget* w, std::uint8_t v) {  // 00421670
    w->list->a4 = v;
    if (w->list->obj) w->list->obj->b10 = v;
}

void GuiRuntime::list_set_font(Widget* w, const std::optional<std::string>& name) {  // 00421690
    auto& l = *w->list;
    // 00418420: NULL or "" -> the manager's default font (+10)
    l.font = !name || name->empty() || (*name)[0] == '\0' ? host_.default_font() : host_.font(*name);
    if (l.font) array_set_row_height(w, static_cast<std::uint32_t>(host_.font_height(l.font)) + 4);
}

// ---------------------------------------------------------------- decorator 004173b0

std::uint8_t GuiRuntime::deco_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {
    const std::uint32_t t = type & 0xffff;
    switch (t) {
    case 4: {  // 00417520: 00416e40(d, name); found and t0 -> *t0 = the record, 1
        if (a3 == 0) {
            w->deco->d80 = 0;
            return 0;
        }
        const char* name = nullptr;  // the names GuiSliderWidget's init 0043f850 passes (table 004976c0)
        if (a3 == 0x4c01d0) name = "onActivate";
        else if (a3 == 0x4c01e0) name = "onDeactivate";
        else if (a3 == 0x4bffa8) name = "onWake";  // GuiScrollBarWidget's init 0043be70 (*004976a0)
        else unhosted(0x417520, "decorator event 4 with the name at " + std::to_string(a3));
        deco_record_ = nullptr;
        for (const GuiEvent& e : w->deco->events)
            if (e.name == name) {
                deco_record_ = &e;
                break;
            }
        return deco_record_ ? 1 : 0;
    }
    case 5: return 1;  // 00417468: *a3 = d+24, the IE instance (none natively)
    case 6: return 0;
    case 7:  // 00417480
        if (a3 == 0) return 1;
        if (a3 != tick_arg) unhosted(0x417480, "decorator event 7 with an argument other than 00415b60's");
        if (ftz(tick_value) == 0.0f) return 1;  // c.eq.s 0
        return 0;  // d+24 (the IE instance, 003f43d0) is never attached natively
    case 8:
        if (a3 != 0) unhosted(0x4174f0, "decorator event 8 output pointer");
        return 1;
    case 9:  // 004174c8: *a3 = the element's clock when it has one
        if (a3 == out_arg) {
            if (deco_time)
                if (const auto clock = deco_time(w)) out_value = *clock;
            return 1;
        }
        if (a3 != 0) unhosted(0x4174c8, "decorator event 9 output pointer");
        return 1;
    case 15: return deco_settled ? deco_settled(w) : 1;  // 00417400 (no element: settled)
    case 18:            // 004176c0
        w->deco->matrix[12] = fadd(static_cast<float>(a3), fdiv(w->rect[2], 2.0f));
        w->deco->matrix[14] = fsub(static_cast<float>(t0), fdiv(w->rect[3], 2.0f));
        ++deco_matrix_serial;
        return 1;
    default: return v_event(w, sender, type, a3, t0);
    }
}

// ---------------------------------------------------------------- GuiMessageBox (vtable 004dd9d0)

void GuiRuntime::msgbox_init(Widget* w) {  // 002a8410
    for (int i = 0; i < 3; ++i) {
        auto& l = w->layouts[i];
        const std::string n = std::to_string(i);
        l.group = find_sub(w, "messagebox" + n);
        l.title = find(w, "title" + n);
        if (l.title && l.title->cls != WidgetClass::text) l.title = nullptr;  // isA "GuiTextWidget" (exact)
        l.message = find(w, "message" + n);
        const WidgetClass mc = i == 2 ? WidgetClass::edit : WidgetClass::text;
        if (l.message && l.message->cls != mc) l.message = nullptr;
        if (i == 1 && l.message) set_b74(l.message, 0);
        l.button1 = find(w, "button1" + n);
        if (l.button1 && l.button1->cls != WidgetClass::button) l.button1 = nullptr;
        l.button2 = find(w, "button2" + n);
        if (l.button2 && l.button2->cls != WidgetClass::button) l.button2 = nullptr;
    }
    base_init(w);
}

void GuiRuntime::msgbox_setup(Widget* mb, std::int32_t type, const std::string& title, const std::string& message,
                              Widget* target, std::int32_t arg) {  // 002a7e90
    mb->mb_type = type;
    mb->mb_target = target;
    mb->mb_arg = arg;
    const std::int32_t L = type == 6 ? 2 : (!title.empty() ? 1 : 0);
    for (int i = 0; i < 3; ++i) {
        Widget* gw = mb->layouts[i].group;
        if (!gw) continue;
        if (i == L) gw->flags |= flag_visible | flag_enabled;
        else gw->flags &= ~(flag_visible | flag_enabled);
    }
    mb->mb_layout = L;
    const auto& l = mb->layouts[L];
    auto str_of = [&](const char* key) {
        const auto v = host_.get_string(key);
        return v ? *v : std::string();
    };
    if (l.button1 && l.button2) {
        switch (static_cast<std::uint32_t>(type)) {  // jump table 004ab590
        case 0:
        case 1:
            l.button1->flags &= ~flag_visible;
            l.button2->flags &= ~flag_visible;
            break;
        case 2:
            l.button1->flags |= flag_visible;
            set_text(l.button1, str_of("MB_OK"), false);
            l.button2->flags &= ~flag_visible;
            break;
        case 4:
        case 5:
            l.button1->flags |= flag_visible;
            set_text(l.button1, str_of("MB_Yes"), false);
            l.button2->flags |= flag_visible;
            set_text(l.button2, str_of("MB_No"), false);
            break;
        default:  // 3, 6 and anything else
            l.button1->flags |= flag_visible;
            set_text(l.button1, str_of("MB_OK"), false);
            l.button2->flags |= flag_visible;
            set_text(l.button2, str_of("MB_Cancel"), false);
            break;
        }
    }
    if (l.title) set_text(l.title, title, false);
    if (l.message) set_text(l.message, message, false);
    if (mb->flags & flag_awake) {
        mb->focus = type == 5 ? l.button2 : v_first(mb);
        set_focus(mb->focus);
        send(mb, &canvas_, 0x10, 2, 0);
    }
}

void GuiRuntime::msgbox_wake(Widget* w) {  // 002a86f0
    base_wake(w);
    if (w->mb_type == 5) w->focus = w->layouts[w->mb_layout].button2;
}

void GuiRuntime::msgbox_sleep(Widget* w) {  // 002a86a0
    base_sleep(w);
    send(w, &canvas_, 0x10, 3, 0);
}

Widget* GuiRuntime::msgbox_target(Widget* t) const {  // 002a8800
    if (!t) return nullptr;
    if (t == &canvas_) return t;
    return in_roots(t) ? t : nullptr;
}

std::uint8_t GuiRuntime::msgbox_input(Widget* w, const PadEvent& ev) {  // 002a7b00
    if (!(ev.kind == 1 && ev.port == 0 && ev.mask == pad_triangle && ev.released == 0)) return 0;
    if (w->mb_type == 0 || w->mb_type == 1) return 1;
    w->mb_target = msgbox_target(w->mb_target);
    const std::int32_t id = w->id10;
    send(w, w->mb_target, w->mb_type == 2 ? 0x1a : 0x1b, w->mb_arg, 0);
    if (popup_close(id) == 0 && w->id10 == id) {
        w->id10 = -1;
        pop(w);
    }
    return 1;
}

std::uint8_t GuiRuntime::msgbox_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {
    if ((type & 0xffff) != 2) return base_event(w, sender, type, a3, t0);  // 002a7c20
    const std::int32_t id = w->id10;
    const auto& l = w->layouts[w->mb_layout];
    w->mb_target = msgbox_target(w->mb_target);
    if (w->mb_type == 6) {
        // t0 is the edit widget's text pointer (00169 2d0); natively 0, no ported handler reads it.
        if (sender == l.button1 || sender == l.message) send(w, w->mb_target, 0x1a, w->mb_arg, 0);
        else if (sender == l.button2) send(w, w->mb_target, 0x1b, w->mb_arg, 0);
    } else {
        if (sender == l.button1) send(w, w->mb_target, 0x1a, w->mb_arg, 0);
        else if (sender == l.button2) send(w, w->mb_target, 0x1b, w->mb_arg, 0);
    }
    if (popup_close(id) == 0 && w->id10 == id) {
        w->id10 = -1;
        pop(w);
    }
    return 1;
}

// ---------------------------------------------------------------- application popup queue (app *004ffc04)

std::int32_t GuiRuntime::popup_open(std::int32_t type, const std::optional<std::string>& title,
                                    const std::optional<std::string>& message, Widget* target, std::int32_t arg) {
    if (!native_app) return host_.popup_open(type, title, message, target, arg);
    if (!msgbox) return -1;  // 002cae30
    PopupEntry e;
    e.id = next_popup++;
    e.type = type;
    e.title = title.value_or(std::string());
    e.message = message.value_or(std::string());
    e.target = target;
    e.arg = arg;
    bool show = true;
    auto it = popups.begin();
    for (; it != popups.end(); ++it) {
        if (it->type != 1) break;
        show = false;
    }
    popups.insert(it, e);
    if (show) {
        msgbox_setup(msgbox, type, e.title, e.message, target, arg);
        msgbox->id10 = e.id;
        if (!(msgbox->flags & flag_awake)) {
            push(msgbox, 1000);
            send(&canvas_, &canvas_, 0x10, 2, 0);
        }
    }
    return e.id;
}

std::int32_t GuiRuntime::popup_close(std::int32_t id) {
    if (!native_app) {
        host_.popup_close(id);
        return 0;
    }
    if (!msgbox) return 0;  // 002cac80
    if (popups.empty()) {
        pop(msgbox);
        return 0;
    }
    bool head = true;
    std::int32_t result = 0;
    for (auto it = popups.begin(); it != popups.end(); ++it) {
        if (id == -1 || it->id == id) {
            popups.erase(it);
            result = 1;
            break;
        }
        head = false;
    }
    if (popups.empty()) {
        msgbox->id10 = -1;
        pop(msgbox);
    } else if (head) {
        const PopupEntry e = popups.front();
        msgbox_setup(msgbox, e.type, e.title, e.message, e.target, e.arg);
        msgbox->id10 = e.id;
    }
    return result;
}

std::int32_t GuiRuntime::popup_query(std::int32_t id) {
    if (!native_app) return host_.popup_query(id);
    for (const PopupEntry& e : popups)  // 002caba0
        if (e.id == id) return 1;
    return 0;
}

// ---------------------------------------------------------------- GUI manager (*00571fb8)

bool GuiRuntime::in_roots(const Widget* w) const {
    return std::find(roots.begin(), roots.end(), w) != roots.end();
}

Widget* GuiRuntime::lookup(const std::string& class_name) const {  // 00418980
    if (class_name.empty()) return nullptr;
    for (Widget* r : roots)
        if (iequal(class_name, class_id(r->cls))) return r;
    return nullptr;
}

Widget* GuiRuntime::factory(const std::string& class_name, const std::optional<std::string>& file) {
    if (!native_app) return host_.factory(class_name, file.value_or(std::string()));
    Widget* w = lookup(class_name);  // 00418a80
    if (w) {
        if (w->refs != 0) ++w->refs;
        return w;
    }
    w = create(class_name, file);
    if (w) ++w->refs;
    return w;
}

Widget* GuiRuntime::create(const std::string& class_name, const std::optional<std::string>& file) {  // 004187c0
    Widget* w = new_widget(class_name);
    if (!w) return nullptr;
    return load_file(file ? *file : class_name, w) ? w : nullptr;  // a failed load leaks the widget
}

bool GuiRuntime::load_file(const std::string& name, Widget* w) {  // 004191d0
    if (name.empty()) return false;
    const auto res = host_.resource(name);  // 004133b0(00571dc0, &res, name)
    if (!res) return false;
    g.iface = name;                                       // 00419110: manager +2c
    const GuiNode* n = host_.open_gui(name + ".GUI", *res);  // 0036e1b0 + 00419020 (0036dc60, "GUI" magic)
    if (!n) return false;
    if (!load_into(*n, w)) return false;
    if (load_observer) load_observer(w, *n);
    return true;
}

Widget* GuiRuntime::load_root(const std::string& name) {  // 004191d0 with a null widget
    if (name.empty()) return nullptr;
    const auto res = host_.resource(name);
    if (!res) return nullptr;
    g.iface = name;  // 00419110
    const GuiNode* n = host_.open_gui(name + ".GUI", *res);
    if (!n) return nullptr;
    // 00418b10 with a2 == 0: 0041f960(the file root's class name) makes the root, then the
    // children load into it and 00419530 adds it to the roots.
    Widget* w = new_widget(n->class_name);
    if (!w) unhosted(0x41f960, "unregistered root widget class '" + n->class_name + "'");
    load_node(*n, w);
    add_root(w);
    if (load_observer) load_observer(w, *n);
    return w;
}

std::uint8_t GuiRuntime::release(Widget* w) {
    if (!native_app) {
        host_.release(w);
        return 1;
    }
    if (!in_roots(w)) return 0;  // 00418840
    if (w->refs == 0) return 0;
    --w->refs;
    if (w->refs == 0) unload(w, false);
    return 1;
}

void GuiRuntime::unload(Widget* w, bool now) {  // 00418e00
    if (!in_roots(w)) return;
    if ((w->flags & flag_unloading) && !now) return;
    w->flags |= flag_unloading;
    if (now) {
        remove(w);
        destroy(w);
    } else {
        pending_unload.push_back(w);
    }
}

void GuiRuntime::remove(Widget* w) {  // 004193d0
    if (w->deleted) unhosted(0x4193d0, "removal of a destroyed (freed) widget");
    if (!(w->flags & flag_init)) return;
    const auto it = std::find(roots.begin(), roots.end(), w);
    if (it == roots.end()) unhosted(0x419488, "removal of a widget that is not a root");
    if (w->flags & flag_awake) pop(w);
    deinit(w);
    roots.erase(std::find(roots.begin(), roots.end(), w));
}

void GuiRuntime::flush() {  // 00419270
    for (std::size_t i = 0; i < pending_unload.size(); ++i) {
        Widget* w = pending_unload[i];
        remove(w);
        destroy(w);
    }
    pending_unload.clear();
}

void GuiRuntime::destroy(Widget* w) {  // vt +8 (0041f370 / 0041c6a0 / 0041a060 / 002a7a60) + 0036b130
    if (w->deleted) unhosted(0x36b130, "double free of a widget");
    w->deleted = true;
    if (w->cls == WidgetClass::eqmodify && w->em) em_ring_release(w);  // 002c0f00
    for (Widget* c : w->children) destroy(c);
}

// ---------------------------------------------------------------- GuiTitleScreen


float GuiRuntime::now_seconds() {  // 003698b0 + 001002d0
    const std::int64_t ms = host_.time_ms();
    float f;
    if (ms > 0) {
        f = static_cast<float>(ms);
    } else {
        const std::uint64_t u = static_cast<std::uint64_t>(ms);
        const std::uint64_t v = (u >> 1) | (u & 1);
        f = static_cast<float>(static_cast<std::int64_t>(v));
        f = fadd(f, f);
    }
    return fdiv(f, 1000.0f);
}

void GuiRuntime::title_wake(Widget* w) {  // 002882b0
    base_wake(w);
    const std::int32_t profile = host_.current_profile();
    host_.profile_select(static_cast<std::uint32_t>(profile));
    host_.app_2c9af0(0);
    title_card_check(w, false);
}

void GuiRuntime::title_card_check(Widget* w, bool force) {  // 00288310
    if (g.card_checked && !force) return;
    g.card_checked = 1;
    host_.card_refresh();
    if (!host_.save_present()) {
        if (!g.card.present) {
            const auto title = host_.get_string("MC_ALERT");
            const auto msg = host_.get_string("MC_INITNOCARD");
            w->popup = popup_open(4, title, msg, w, 1);
        } else if (g.card.check_space) {
            const std::int32_t free = g.card.free;
            const std::int32_t profile = host_.current_profile();
            const std::int32_t size = host_.profile_size(static_cast<std::uint32_t>(profile));
            const std::int32_t need = host_.card_space_needed(size);
            if (free < need) {
                char num[16];
                std::snprintf(num, sizeof num, "%d", need);
                const auto fmt = host_.get_string("MC_INITNOSPACE");
                if (!fmt) unhosted(0x288430, "NULL MC_INITNOSPACE string");
                std::string msg = *fmt;
                const auto at = msg.find("1000");
                if (at != std::string::npos) msg = msg.substr(0, at) + num + msg.substr(at + 4);
                const auto title = host_.get_string("MC_ALERT");
                w->popup = popup_open(4, title, msg, w, 1);
            }
        }
    }
    g.card_slot = g.card.slot;
}

std::uint8_t GuiRuntime::title_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {
    w->last_input = now_seconds();
    const std::uint32_t t = type & 0xffff;
    if (t == 0x1a && a3 == 1) {
        g.card_ok = 0;
        return 1;
    }
    if (t == 0x1b && a3 == 1) {
        popup_close(w->popup);
        title_card_check(w, true);
        return 1;
    }
    if (t == 2) {
        if (!sender) unhosted(0x287e00, "activation without a sender");
        Widget* next = nullptr;
        if (sender->name == "newagent") {
            const bool noapp = g.app_loaded == 0;
            std::int32_t id = 0;
            if (noapp) {
                host_.app_2c82f0();
                const auto msg = host_.get_string("LoadCharData");
                id = popup_open(0, std::string(), msg, nullptr, 0);
            }
            next = factory("GuiAgentCreate", "AGENTNEW");
            if (noapp) {
                popup_close(id);
                host_.app_2c81d0();
            }
        } else if (sender->name == "loadagent") {
            next = factory("GuiAgentLoad", "AGENTOPN");
        } else if (sender->name == "options") {
            next = factory("GuiGameOptions", "GAMEOPT");
        }
        if (next) switch_screens(w, next);
        return 1;
    }
    return base_event(w, sender, type, a3, t0);
}

void GuiRuntime::title_update(Widget* w, float dt) {  // 00288060
    Widget* la = find(w, "loadagent");
    const std::uint8_t present = g.card.present != 0;
    if (la) {
        const std::uint8_t sv = host_.save_present();
        if (!sv && canvas_.focus == la) set_focus(v_next(la->loader, la, true));
        if (sv) la->flags |= flag_enabled;
        else la->flags &= ~flag_enabled;
    }
    g.card_ok = present;
    if (w->demo) {
        if (top() != w) {
            w->last_input = now_seconds();
        } else {
            const float now = now_seconds();
            if (g.keyboard && !(fsub(now, w->last_input) <= 30.0f)) {
                g.app_attract = 1;
                g.app_mode = "IDLE";
                g.app_state_prev = g.app_state;
                g.app_state = 4;
            }
        }
    }
    base_update(w, dt);
}

// ---------------------------------------------------------------- GuiAgentLoad (vtable 004dd400)
// Create 002940b0 (base ctor 0041f690, then GuiSaveAgentWidget 004dd460):
// +48 popup -2, +78 timer 10.0f, byte +7c 0. init / sleep / gain / lose /
// first are the GuiWidget entries.

void GuiRuntime::al_wake(Widget* w) {  // 00292dd0
    g.app_2f = 0;
    Widget* t = find(w, "menutitle");
    if (t && t->cls == WidgetClass::text) {  // IsA (vt +58) "GuiTextWidget" (exact; buttons fail)
        const auto s = host_.get_string("MC_LOADTITLE");
        set_text(t, s ? *s : std::string(), false);
    }
    al_slots(w);
    base_wake(w);
}

void GuiRuntime::al_slots(Widget* w) {  // 00292e80
    AgentSlotListScreen screen;
    screen.empty_selectable = w->al_7c != 0;
    std::array<Widget*, agent_card_slots> buttons{};
    for (std::size_t i = 0; i < agent_card_slots; ++i) {
        Widget* b = find(w, std::string(1, static_cast<char>('0' + i)));
        AgentSlotButton& s = screen.buttons[i];
        s.found = b != nullptr;
        s.is_button = b && b->cls == WidgetClass::button;
        s.flags = b ? static_cast<std::uint16_t>(b->flags) : 0;
        if (s.found && s.is_button) buttons[i] = b;
    }
    // 004147a0() + 20: the canvas focus.
    screen.focus = agent_slot_focus_other;
    if (canvas_.focus == w) screen.focus = agent_slot_focus_screen;
    for (std::size_t i = 0; i < agent_card_slots; ++i)
        if (buttons[i] && canvas_.focus == buttons[i]) screen.focus = static_cast<std::int32_t>(i);
    // Checkpoint 132cc9: the agent format update. Before the first legacy "<name>.agt?" entry's encode
    // (002930b4), *0055a838 = 00293650 (j 002cc260(0, 0)) and 002cae30(*004ffc04, 0, "" 004a9d80,
    // "Updating agents to new format" 004a9d90, screen, 0); each rename 0040ddc0(W, old, new) waits
    // like 0040dc80 (one pass of its loop runs the poll), and its command 9 copies the new name into
    // the entry table (0040ec40). After the scan, 002cac80(*004ffc04, id) and *0055a838 = 0 (002931c0).
    // 0040d600 hands the builder pointers into that table; here a copy, so the renames update g.card.dir.
    std::int32_t update_popup = -1;
    bool update_open = false;
    const auto open_update = [&] {
        g.card_poll = 0x293650;
        update_popup = popup_open(0, std::string(), std::string("Updating agents to new format"), w, 0);
        update_open = true;
    };
    const std::vector<std::string> dir = g.card.dir;
    const AgentSlotListResult r = build_agent_slot_list(
        dir, screen, charmap.ready != 0 && charmap.count != 0, charmap.count,
        [&](std::string_view from, std::string_view to) {
            if (!update_open) open_update();
            host_.card_rename(std::string(from), std::string(to));
            host_.card_poll();   // the wait's pass: *0055a838 = 00293650
        });
    if (r.update_popup) {
        if (!update_open) open_update();   // every encode failed: the popup opened, no rename ran
        popup_close(update_popup);
        g.card_poll = 0;
    }
    for (std::size_t i = 0; i < agent_card_slots; ++i) {
        Widget* b = buttons[i];
        if (!b) continue;
        if (r.text_kind[i] == AgentSlotText::name) {
            set_text(b, r.text[i], false);
        } else if (r.text_kind[i] == AgentSlotText::empty) {
            const auto s = host_.get_string("MC_EMPTY");
            set_text(b, s ? *s : std::string(), false);
        }
        b->flags = screen.buttons[i].flags;
    }
    if (r.refocus) set_focus(v_first(w));  // vt +44 0041d8c0
}

void GuiRuntime::al_update(Widget* w, float dt) {  // 00293660
    w->al_timer = fadd(w->al_timer, dt);
    if (!(w->al_timer <= 1.0f)) {
        al_slots(w);
        w->al_timer = fsub(w->al_timer, 1.0f);
    }
    base_update(w, dt);
}

std::uint8_t GuiRuntime::al_input(Widget* w, const PadEvent& ev) {  // 00292ad0
    if (ev.kind == 1 && ev.port == 0 && ev.mask == pad_triangle && ev.released == 0) {
        Widget* r = factory("GuiTitleScreen", std::string("titlescr"));
        switch_screens(w, r);
        return 1;
    }
    return 0;
}

namespace {

// 00121bc8 strtol(s, 0, 10) through newlib's 0012af90 (64-bit long), the
// result truncated to the 32-bit int the caller keeps.
std::int32_t strtol10(const std::string& s) {
    std::size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || (s[i] >= '\t' && s[i] <= '\r'))) ++i;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    std::uint64_t v = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
        v = v * 10 + static_cast<std::uint64_t>(s[i] - '0');
        if (v > 0x7fffffffffffffffull) unhosted(0x12af90, "strtol overflow (ERANGE)");
    }
    const std::uint64_t r = neg ? ~v + 1 : v;
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(r));
}

}  // namespace

std::uint8_t GuiRuntime::al_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {  // 00292b90
    if ((type & 0xffff) != 2) return save_widget_event(w, sender, type, a3, t0);
    if (!sender || sender->cls != WidgetClass::button) return 1;  // IsA "GuiButtonWidget"
    const std::int32_t slot = strtol10(sender->name);
    const std::int32_t profile = host_.current_profile();
    const std::uint8_t r = host_.agent_load(profile, sender->text, slot);
    if (r == 0) {
        host_.agent_activate(profile);
        Widget* next = g.app_online ? factory("GuiCommandCenter", std::string("CMDCENTR"))
                                    : factory("GuiOnlineOfflineSelect", std::string("onoffsel"));
        switch_screens(w, next);
    } else {
        const auto title = host_.get_string("MC_ERROR");
        const auto msg = host_.get_string(r == 2 ? "MC_CORRUPT" : "MC_BADLOAD");
        popup_open(2, title, msg, w, -1);
    }
    return 1;
}

// ---------------------------------------------------------------- GuiAgentSave (vtable 004dd3a0)
// 00292980: a slot button's activation creates the agent there (the flow's mode 0: 00333f60).
std::uint8_t GuiRuntime::as_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {
    if ((type & 0xffff) != 2) return save_widget_event(w, sender, type, a3, t0);  // 00441ca0
    if (!sender || sender->cls != WidgetClass::button) return 1;  // IsA "GuiButtonWidget" (004a9870)
    auto& s = w->as->save;
    s.slot = strtol10(sender->name);  // +50
    s.mode = 0;                       // +4c
    save_start(w);                    // 00441a10
    return 1;
}

std::uint8_t GuiRuntime::save_widget_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                           std::int32_t t0) {  // 00441ca0
    host_.get_string("MC_ERROR");
    host_.get_string("MC_ALERT");
    if (w->cls == WidgetClass::gameopt || w->cls == WidgetClass::cmdcentr || w->cls == WidgetClass::agentmod ||
        w->cls == WidgetClass::agentsave || w->cls == WidgetClass::autosaver || w->cls == WidgetClass::onlinecn ||
        w->cls == WidgetClass::msnsetup || w->cls == WidgetClass::netjoin || w->cls == WidgetClass::acntlogn ||
        w->cls == WidgetClass::onlinepref || w->cls == WidgetClass::netmess || w->cls == WidgetClass::netcontacts)
        return static_cast<std::uint8_t>(save_flow(w, sender, [&](SaveAgentFlow& f) {
            return f.on_event(0, type, a3, static_cast<std::uint32_t>(t0));
        }));
    // GuiAgentLoad starts no save flow (its only popup is the format update's, type 0, closed by 002cac80
    // with no reply), so these replies cannot reach it; GuiAgentSave runs the create.
    const std::uint32_t t = type & 0xffff;
    const std::uint32_t u = static_cast<std::uint32_t>(a3);
    if (t == 0x1b) {
        if (u <= 9 && ((0x22fu >> u) & 1)) unhosted(0x441d04, "save-agent popup reply (event 0x1b)");
        return base_event(w, sender, type, a3, t0);
    }
    if (t == 0x1a) {
        if (u == 0xa) {  // 0044219c: the "no card" OK of the options save: vtable +5c(w, 0), no popup close
            save_done(w, 0);
            return 1;
        }
        if (u <= 10 && ((0x6afu >> u) & 1)) unhosted(0x441df0, "save-agent popup reply (event 0x1a)");
        return 1;
    }
    return base_event(w, sender, type, a3, t0);
}

// ---------------------------------------------------------------- GuiOnlineOfflineSelect (vtable 004dd6b0)
// Create 002a0c30 (0x4c bytes; base ctor 0041f690, byte +48 = 0). The
// button names are the pointers 0048ba28 "online", 0048ba30 "offline" and
// 0048ba38 "lan"; sleep / gain / lose / first are the GuiWidget entries.

void GuiRuntime::oo_init(Widget* w) {  // 002a0970
    base_init(w);
    w->oo_online = g.b_6488;
    if (Widget* lan = find(w, "lan")) {
        // PORT-ONLY (132ln1): port_lan_play shows "lan" as LAN Play where the retail gate (00506490, never
        // written) would have; with it false the original gate alone decides.
        const bool lan_play = host_.port_lan_play();
        if (w->oo_online == 0 && (g.b_6490 != 0 || lan_play)) lan->flags |= flag_visible;
        else lan->flags &= ~flag_visible;
        // The label replaces the disc text before the button wakes (as if the file had it): no 0x1c notice.
        if (lan_play) lan->text = port_lan_play_label;
    }
    if (Widget* off = find(w, "offline")) {
        if (w->oo_online == 0) off->flags |= flag_enabled;
        else off->flags &= ~flag_enabled;
    }
}

void GuiRuntime::oo_online_button(Widget* online) {  // 002a05e4 (update) / 002a0774 (wake) / 002a0860
    if (!online) return;
    if (g.app_31) {
        online->flags &= ~flag_enabled;
        return;
    }
    // card +59d (0055a830) and byte 0051eea0
    const bool on = g.card.present != 0 && g.b_eea0 == 0;
    if (!on && canvas_.focus == online) set_focus(v_next(online->loader, online, true));  // vt +4c
    if (on) online->flags |= flag_enabled;
    else online->flags &= ~flag_enabled;
}

void GuiRuntime::oo_wake(Widget* w) {  // 002a06c0
    base_wake(w);
    host_.app_2c9af0(0);
    w->focus = find(w, w->oo_online ? "online" : "offline");
    if (Widget* lan = find(w, "lan")) {
        if (g.b_eea8 == 0) lan->flags |= flag_enabled;
        else lan->flags &= ~flag_enabled;
    }
    oo_online_button(find(w, "online"));
    if (host_.port_lan_play()) oo_lan_button(find(w, "lan"));  // PORT-ONLY (132ln1)
}

// PORT-ONLY (checkpoint 132ln1, not the original): LAN Play takes the "online" button's gates (the memory card
// for the agent, 0051eea0 and app +31, oo_online_button) on top of the retail "lan" gate 0051eea8, and moves the
// focus off it when it is disabled, as 002a05e4 does for "online". Run at wake and every update.
void GuiRuntime::oo_lan_button(Widget* lan) {
    if (!lan) return;
    const bool on = g.b_eea8 == 0 && g.app_31 == 0 && g.card.present != 0 && g.b_eea0 == 0;
    if (!on && canvas_.focus == lan) set_focus(v_next(lan->loader, lan, true));
    if (on) lan->flags |= flag_enabled;
    else lan->flags &= ~flag_enabled;
}

void GuiRuntime::oo_update(Widget* w, float dt) {  // 002a05b0
    base_update(w, dt);
    oo_online_button(find(w, "online"));
    if (host_.port_lan_play()) oo_lan_button(find(w, "lan"));  // PORT-ONLY (132ln1)
}

std::uint8_t GuiRuntime::oo_input(Widget* w, const PadEvent& ev) {  // 002a0380
    if (ev.kind == 1 && ev.port == 0 && ev.mask == pad_triangle && ev.released == 0) {
        Widget* r = factory("GuiTitleScreen", std::string("titlescr"));
        switch_screens(w, r);
        return 1;
    }
    return 0;
}

std::uint8_t GuiRuntime::oo_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {  // 002a01c0
    if ((type & 0xffff) != 2) return base_event(w, sender, type, a3, t0);
    if (!sender) unhosted(0x2a0204, "activation without a sender (reads sender +4)");
    // 001c43a0: the exact, case-sensitive compare of the sender's name
    Widget* next = nullptr;
    const bool lan_play = sender->name == "lan" && host_.port_lan_play();  // PORT-ONLY (132ln1)
    if (sender->name == "online" || lan_play) {
        g.b_2fe8 = 0;
        g.b_23d0 = 0;  // 002cca20(app, 1)
        g.app_30 = 1;
        g.app_32 = 0;
        // PORT-ONLY (132ln1): LAN Play is the online branch with g.lan_mode set; the LAN-mode ONLINECN offers
        // Host / Join instead of the PS2 network configurations (research/LAN_PLAY.md section 4).
        g.lan_mode = lan_play ? 1 : 0;
        next = factory("GuiOnlineConnect", std::string("ONLINECN"));
    } else if (sender->name == "offline" || sender->name == "lan") {
        if (sender->name == "offline") {
            g.b_23d0 = 0;  // 002cca20(app, 0)
            g.app_30 = 0;
            g.app_32 = 0;
            g.lan_mode = 0;  // PORT-ONLY (132ln1)
        }
        next = factory("GuiCommandCenter", std::string("CMDCENTR"));
        // 002a02c8 / 002a032c: sw 1 to the new screen's +10 without a null
        // check (CMDCENTR always loads on the disc)
        if (next) next->id10 = 1;
    } else {
        return base_event(w, sender, type, a3, t0);
    }
    switch_screens(w, next);
    return 1;
}

// ---------------------------------------------------------------- GuiCommandCenter (vtable 004dd4f0)
// Ctor 002984d0 over the net screen 0033d580 (init 0033d3f0, wake 0033d3a0,
// sleep 0033d360, update 0033d310, input 0033cef0, event 0033cf90). gain /
// lose / last / next are the GuiWidget entries; vt +44 is 00294250.
// The net screen's object +78 is the network manager 004294c0, which is
// null in the boot state and in the native front end (offline play).

namespace {

// The type-6 state names the screen sends its slot buttons (a3 = the string's address).
constexpr std::int32_t cc_a3_state_on = 0x4aa2c8;     // "onStateOn"
constexpr std::int32_t cc_a3_lose_focus = 0x4aa2d8;   // "onLoseFocus"
constexpr std::int32_t cc_a3_state_off = 0x4aa2e8;    // "onStateOff"
constexpr std::int32_t cc_a3_gain_focus = 0x4aa2f8;   // "onGainFocus"
constexpr std::int32_t cc_a3_deactivate = 0x4aa308;   // "onDeactivate"

constexpr std::array<const char*, 6> cc_group_names = {"buttons_grp",   "mission_grp", "online_grp",
                                                       "personnel_grp", "zeus_grp",    "movie_grp"};  // 0048b6e0
constexpr std::array<const char*, 6> cc_zeus_names = {"news_reports",   "mission_data",     "agency_files",
                                                      "political_data", "conspiracy_files", "media_files"};  // 0048b770
constexpr std::array<std::uint8_t, 18> cc_online_levels = {1, 1, 1, 0, 1, 1, 0, 0, 1,
                                                           0, 0, 0, 1, 1, 0, 0, 1, 0};  // 004acc70
constexpr std::array<std::uint8_t, 18> cc_gate = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};  // 004acd30
constexpr std::array<std::int32_t, 18> cc_app_248 = {0, 1, 2, -1, 3, 4, -1, -1, 5, -1, -1, -1, 6, 7, -1, -1, 8, -1};  // 004acb60
constexpr std::array<std::uint8_t, 18> cc_app_250 = {0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0};  // 004acbb0 (= 004acbd0)
// 0048b870: the background circle's animation marks (seconds)
constexpr std::array<std::uint32_t, 11> cc_circle_marks = {0x42080000, 0x431f0000, 0x438c0000, 0x43c68000,
                                                           0x43fe8000, 0x441b4000, 0x4434c000, 0x444e0000,
                                                           0x44680000, 0x447c4000, 0x448a8000};

using Rgba = std::array<float, 4>;
Rgba rgba_bits(std::uint32_t r, std::uint32_t g, std::uint32_t b, std::uint32_t a) {
    return {std::bit_cast<float>(r), std::bit_cast<float>(g), std::bit_cast<float>(b), std::bit_cast<float>(a)};
}
const Rgba cc_white = {1.0f, 1.0f, 1.0f, 1.0f};                                       // 00587b80
const Rgba cc_pulse_lo = rgba_bits(0x3ec0c0c1, 0x3efcfcfd, 0x3f048485, 0x3f800000);   // 00587b90
const Rgba cc_list_rgba = rgba_bits(0x3eaeaeaf, 0x3f26a6a7, 0x3f800000, 0x3f800000);  // 00587ba0
const Rgba cc_list_c0 = rgba_bits(0x3f50d0d1, 0x3f6eeeef, 0x3f800000, 0x3f800000);    // 00587bb0
const Rgba cc_list_d0 = rgba_bits(0x3ecacacb, 0x3f149495, 0x3f25a5a6, 0x3f800000);    // 00587bc0

// 003fda40: the 16-byte lower-case key 00332fe0 takes.
std::string level_key(const char* name) {
    std::string k = name;
    for (auto& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return k.substr(0, 16);
}

// A char* result as the EE sees it (up to the first NUL).
std::string cstr_of(const std::string& s) { return s.substr(0, s.find('\0')); }

bool is_button_w(const Widget* w) { return w && w->cls == WidgetClass::button; }  // vt +58 "GuiButtonWidget"
bool is_text_w(const Widget* w) { return w && w->cls == WidgetClass::text; }      // vt +58 "GuiTextWidget"

void set_visible(Widget* w, bool on) {
    if (on) w->flags |= flag_visible;
    else w->flags &= ~flag_visible;
}
void set_enabled(Widget* w, bool on) {
    if (on) w->flags |= flag_enabled;
    else w->flags &= ~flag_enabled;
}

}  // namespace

void GuiRuntime::set_rgba(Widget* w, const std::array<float, 4>& c) {  // 0041c070
    w->rgb = {c[0], c[1], c[2]};
    w->alpha = c[3];
    if (w->obj && w->obj->b59 == 0) w->obj->rgba = c;  // 003d0810
}

void GuiRuntime::cc_init(Widget* w) {  // 00297c10
    auto& c = *w->cc;
    c.circle = find(w, "backgroundcircle");
    c.arrows = find(w, "arrows");
    c.navigate = find(w, "navigate");
    c.navigatea = find(w, "navigatea");
    char buf[32];
    for (int i = 0; i < 17; ++i) {
        c.bonus[i] = 0;
        std::snprintf(buf, sizeof buf, "level%d", i);
        c.levels[i] = find(w, buf);
        if (c.levels[i]) {
            c.levels[i]->id10 = i;
            c.slots[i] = c.levels[i];
        }
    }
    c.bonus[17] = 0;
    for (int k = 0; k < 3; ++k) {
        std::snprintf(buf, sizeof buf, "extra%d", k + 1);
        c.extras[k] = find(w, buf);
        if (c.extras[k]) {
            c.extras[k]->id10 = 17 + k;
            c.slots[17 + k] = c.extras[k];
        }
    }
    const auto slot = [&](Widget*& field, const char* name, int index) {
        field = find(w, name);
        if (field) {
            field->id10 = index;
            c.slots[index] = field;
        }
    };
    slot(c.unlock, "unlock", 0x14);
    slot(c.online, "online", 0x15);
    slot(c.personnel, "personnel", 0x16);
    slot(c.training, "training", 0x17);
    for (int k = 0; k < 6; ++k) {
        Widget* z = find(w, cc_zeus_names[k]);
        c.zeus[k] = is_button_w(z) ? z : nullptr;
        if (c.zeus[k]) c.zeus[k]->id10 = k;
    }
    for (int i = 0; i < 24; ++i) c.panel_of[i] = i == 0x15 ? 2 : i == 0x16 ? 3 : i == 0x14 ? 4 : 1;
    for (int k = 0; k < 6; ++k) c.groups[k] = find(w, cc_group_names[k]);
    for (Widget* s : c.slots)
        if (s) s->flags &= ~flag_focusable;
    const auto text_w = [&](const char* name) {
        Widget* t = find(w, name);
        return is_text_w(t) ? t : nullptr;
    };
    c.levelname1 = text_w("levelname1");
    c.levelname2 = text_w("levelname2");
    c.pou = text_w("personnel_online_unlocked");
    for (Widget* e : c.extras)
        if (e) e->flags &= ~flag_visible;
    const auto button_w = [&](const char* name) {
        Widget* b = find(w, name);
        return is_button_w(b) ? b : nullptr;
    };
    c.zeus_files = button_w("zeus_files");
    c.modify_agent = button_w("modify_agent");
    for (int k = 0; k < 10; ++k) {
        std::snprintf(buf, sizeof buf, "%2.2d", k + 1);
        c.movies[k] = button_w(buf);
    }
    if (Widget* h = find(w, "hardrive")) h->flags &= ~flag_visible;
    base_init(w);  // 0033d3f0
    c.start_button = find(w, "start_button");
    c.bonus_grp = find(w, "bonusmissions");
    if (c.bonus_grp) c.bonus_grp->flags &= ~flag_visible;
    Widget* l = find(w, "textlist");
    c.list = l && l->cls == WidgetClass::textlist ? l : nullptr;  // vt +58 "GuiTextListWidget"
    if (c.list) {  // 002981f0
        list_set_a4(c.list, 2);
        list_set_font(c.list, std::string("callout"));
        c.list->flags &= ~flag_focusable;
    }
}

void GuiRuntime::cc_wake(Widget* w) {  // 00297b10
    auto& c = *w->cc;
    if (w->id10 == 1) {  // set by the online / offline select (002a02c8): a fresh visit
        cc_clear(w);
        w->id10 = -1;
    }
    if (c.list) {
        c.list->list->rgba = cc_list_rgba;
        c.list->list->rgba_c0 = cc_list_c0;
        c.list->list->rgba_d0 = cc_list_d0;
    }
    base_wake(w);  // 0033d3a0
    cc_activate(w);  // vt +64
    if (c.start_button) c.start_button->id10 = -1;
}

void GuiRuntime::cc_sleep(Widget* w) {  // 0033d360
    base_sleep(w);
    cc_deactivate(w);  // vt +68
}

void GuiRuntime::cc_activate(Widget* w) {  // 002973b0
    auto& c = *w->cc;
    if (c.active) return;
    repeat_mask |= 0xc;
    const bool online = g.app_30 != 0, lan = g.app_32 != 0;
    // 00297400: with a DME session the network manager (+78) is set and the lobby's main channel joined unless the
    // lobby is in it already (state 4; 00442750(session, 002cc260)); a failed join opens the popup at the end.
    bool enter_failed = false;
    if (host_.net_session()) {
        if (host_.lobby_state() != 4 && !host_.lobby_enter()) enter_failed = true;
        c.net78 = 1;
    } else {
        c.net78 = 0;
    }
    if (Widget* pm = find(w, "play_mission"); is_button_w(pm))
        set_text(pm, cstr_of(host_.get_string(online ? "CreateGame" : "PlayMission").value_or("")), false);
    if (Widget* jm = find(w, "join_mission")) set_visible(jm, online);
    if (c.online) {
        set_enabled(c.online, online && !lan);
        if (!online || lan) send(w, c.online, 6, cc_a3_deactivate, 0);
    }
    if (Widget* b = find(w, "onlinebutton_grp")) set_visible(b, online && !lan);
    if (Widget* b = find(w, "offlinebutton_grp")) set_visible(b, !(online && !lan));
    if (Widget* t = find(w, "value_agent"); is_text_w(t)) {
        const std::int32_t p = host_.current_profile();
        set_text(t, cstr_of(host_.profile_name(p).value_or("")), false);
    }
    if (Widget* t = find(w, "value_rank"); is_text_w(t)) {
        const std::int32_t p = host_.current_profile();
        set_text(t, cstr_of(host_.profile_rank(p).value_or("")), false);
    }
    if (Widget* t = find(w, "value_clp"); is_text_w(t)) {
        const std::int32_t p = host_.current_profile();
        set_text(t, std::to_string(host_.profile_clp(p)), false);
    }
    if (lan || !online) {
        if (Widget* t = find(w, "label_region")) t->flags &= ~flag_visible;
        if (Widget* t = find(w, "value_region")) t->flags &= ~flag_visible;
    } else {
        if (Widget* t = find(w, "value_region"); is_text_w(t)) {  // 0041beb0(t, profile + 95c, 0), then visible
            const std::int32_t p = host_.current_profile();
            set_text(t, host_.profile_text(p, 0x95c), false);
            t->flags |= flag_visible;
        }
        if (Widget* t = find(w, "label_region")) t->flags |= flag_visible;
    }
    cc_levels(w);
    c.open[17] = c.open[18] = c.open[19] = 0;
    c.open[21] = g.app_30;
    c.open[20] = 0;
    {
        const std::int32_t p = host_.current_profile();
        const auto v = host_.profile_6060(p, "UKRAINE");
        if (v) {
            const std::string key = cstr_of(*v) + "_COMPLETED";
            if (key.size() + 1 > 0x50) unhosted(0x297938, "sprintf overflow of the completion key");
            const std::int32_t p2 = host_.current_profile();
            if (host_.profile_3b10(p2, key)) c.open[20] = 1;
        }
    }
    cc_bonus_flags(w);
    for (int s = 0; s < 24; ++s) {
        Widget* slot = c.slots[s];
        if (!slot) continue;
        if (!c.open[s]) send(w, slot, 6, cc_a3_state_off, 0);
        else if ((s < 17 && c.bonus[s]) || (s == 20 && c.bonus[17])) send(w, slot, 6, cc_a3_state_on, 0);
    }
    host_.app_2c9af0(0);
    cc_restore(w);
    if (enter_failed) {  // 00297a40: (2, LOBBY_STR20, LOBBY_STR134, this, 0x19) into +2f8; its OK leaves (cc_event)
        const std::string title = lobby_string(0x14);
        const std::string m = lobby_string(0x86);
        c.quit_popup = popup_open(2, title, m, w, 0x19);
    }
    c.active = 1;
    if (g.lan_mode) cc_lan_activate(w);  // PORT-ONLY (132ln3)
}

// PORT-ONLY (checkpoint 132ln3, research/LAN_PLAY.md §4 row 4b): in LAN mode the command centre's online-group
// screens that need the lobby (the agency cell AGENTCEL, the contacts CONTACTS and the messages NETMESS) are
// hidden and disabled; online_prefs stays (§4 row 4c: its LAN content is later work). Online keeps them.
// (Checkpoint 132ln4's join_mission focus is applied when the mission panel opens, cc_panel.)
void GuiRuntime::cc_lan_activate(Widget* w) {
    for (const char* name : {"agency_cell", "contacts", "messages"})
        if (Widget* b = find(w, name)) b->flags &= static_cast<std::uint16_t>(~(flag_visible | flag_enabled));
}

void GuiRuntime::cc_deactivate(Widget* w) {  // 00297350
    auto& c = *w->cc;
    if (!c.active) return;
    repeat_mask &= ~0xcu;
    cc_save(w);
    c.sel = -1;
    c.sel_w = nullptr;
    c.active = 0;
}

void GuiRuntime::cc_levels(Widget* w) {  // 00297020
    auto& c = *w->cc;
    for (int i = 0; i < 17; ++i) c.open[i] = 0;
    c.open[23] = 0;
    const std::int32_t p = host_.current_profile();
    std::vector<GuiHost::BonusRow> rows;  // 00296fa0
    if (g.cc_want_bonus) rows = host_.profile_vec(p, 3);
    for (int s = 0; s < 18; ++s) {
        const char* name = cc_level_names[s];
        std::uint8_t open = 0;
        if (cc_gate[s]) {
            if (!g.b_bb5 || s == 17) open = 1;
            else {
                const std::int32_t p2 = host_.current_profile();
                open = host_.level_done(p2, level_key(name)) != 0;
            }
        }
        if (g.cc_want_bonus) {
            g.bonus_row[s] = -1;
            for (std::size_t i = 0; i < rows.size(); ++i) {
                host_.string_key(rows[i].key);  // the first lookup's result is discarded
                if (iequal(cstr_of(host_.string_key(rows[i].key)), name)) {  // 00128e80
                    g.bonus_row[s] = static_cast<std::int32_t>(i);
                    break;
                }
            }
        }
        if (s == 17) {
            c.open[23] = open ? g.app_30 == 0 : 0;
        } else {
            if (open && g.app_30) open = cc_online_levels[s];
            c.open[s] = open;
        }
    }
    g.cc_want_bonus = 0;
}

bool command_centre_online_level(const std::string& level) {
    // 00297020's online rule: with app +30 (an online game) a command-centre level is open only when the online level table
    // 004acc70 says so; TRAINING (slot 0x17, level 17) is open only offline. Any other name is no command-centre level.
    for (std::size_t s = 0; s < cc_level_names.size(); ++s)
        if (level == cc_level_names[s]) return s != 17 && cc_online_levels[s] != 0;
    return false;
}
void GuiRuntime::cc_bonus_flags(Widget* w) {  // 002972e0
    const std::int32_t p = host_.current_profile();
    for (int i = 0; i < 18; ++i) w->cc->bonus[i] = static_cast<std::uint8_t>(host_.level_flag(p, i + 1, -1));
}

void GuiRuntime::cc_save(Widget* w) {  // 00298420
    auto& c = *w->cc;
    g.cc_panel = static_cast<std::uint8_t>(c.panel);
    g.cc_sel = c.sel;
    for (int k = 0; k < 6; ++k) {
        if (c.saved[k]) {
            g.cc_names[k] = cstr_of(c.saved[k]->name).substr(0, 0x1f);  // 00129a70(.., 0x1f)
            // 00298488: the terminator lands on the next entry's first byte;
            // after the last entry that is 0048b868
            if (k == 5) g.cc_want_bonus = 0;
        } else {
            g.cc_names[k].clear();
        }
    }
}

void GuiRuntime::cc_restore(Widget* w) {  // 00298250
    auto& c = *w->cc;
    c.panel = static_cast<std::int8_t>(g.cc_panel);  // 00298360
    c.sel = g.cc_sel;
    for (int k = 0; k < 6; ++k) {
        if (g.cc_names[k].empty()) {
            c.saved[k] = nullptr;
            continue;
        }
        c.saved[k] = find(w, g.cc_names[k]);
        if (!c.saved[k]) c.saved[k] = find_sub(w, g.cc_names[k]);
    }
    if (c.sel == -1) {
        cc_reset(w);
        c.sel = cc_default(w);
    }
    cc_select(w, c.sel, false);
    cc_panel(w, c.panel);
}

void GuiRuntime::cc_clear(Widget* w) {  // 002982c0
    auto& c = *w->cc;
    g.cc_panel = 0;  // 00298310
    g.cc_sel = -1;
    for (auto& n : g.cc_names) n.clear();
    c.sel = -1;
    c.saved.fill(nullptr);
}

void GuiRuntime::cc_reset(Widget* w) {  // 00294ab0
    auto& c = *w->cc;
    for (Widget* grp : c.groups) {
        if (!grp) continue;
        grp->flags &= ~flag_enabled;
        grp->flags &= ~flag_visible;
    }
    if (c.groups[0]) c.groups[0]->flags |= flag_enabled | flag_visible;
    c.panel = 0;
}

std::int32_t GuiRuntime::cc_default(Widget* w) {  // 002952c0
    auto& c = *w->cc;
    std::int32_t last = -1;
    for (int i = 0; i < 17; ++i) {
        const std::int32_t p = host_.current_profile();
        if (host_.level_done(p, level_key(cc_level_names[i]))) last = i;
    }
    if (last == 0 || c.sel == -1) return 0x16;
    if (last != -1) return last;
    if (c.open[23]) return 0x17;
    for (int i = 0; i < 24; ++i)
        if (c.open[i]) return i;
    for (int i = 0; i < 24; ++i) {
        const Widget* s = c.slots[i];
        if (s && (s->flags & flag_enabled) && (s->flags & flag_visible)) return i;
    }
    return -1;
}

Widget* GuiRuntime::cc_first(Widget* w) {  // 00294250
    auto& c = *w->cc;
    const auto pi = static_cast<std::uint8_t>(c.panel);
    Widget* s = c.saved[pi];
    if (s && s->parent == c.groups[pi] &&
        (c.keep || (canvas_.focus && canvas_.focus->parent != c.groups[pi]))) {
        c.keep = 0;
        return s;
    }
    return v_first_base(w);
}

void GuiRuntime::cc_panel(Widget* w, std::int8_t p) {  // 00294300
    auto& c = *w->cc;
    const auto old = static_cast<std::uint8_t>(c.panel);
    if (old != 0 && c.groups[old]) {
        c.groups[old]->flags &= ~flag_visible;
        c.groups[old]->flags &= ~flag_enabled;
    }
    if (old != static_cast<std::uint8_t>(p)) send(w, &canvas_, 0x10, 7, 0);
    c.panel = p;
    if (p == 5 && !cc_movies(w)) return;
    if (p == 0) {
        c.saved.fill(nullptr);
        w->focus = w;
    }
    if (c.arrows) set_visible(c.arrows, c.panel == 0);
    if (c.navigate) set_visible(c.navigate, c.panel != 0);
    if (c.navigatea) set_visible(c.navigatea, c.panel != 0);
    Widget* grp = c.groups[static_cast<std::uint8_t>(p)];
    if (!grp) return;
    grp->flags |= flag_visible;
    grp->flags |= flag_enabled;
    if (c.zeus_files) set_visible(c.zeus_files, p != 1 || c.sel_w != c.training);
    // PORT-ONLY (132ln4): after ONLINECN's JOIN LAN GAME (g.lan_join_focus) the mission panel opens on
    // join_mission (once); panel 0 clears the saved focus, so it is applied here rather than at activate.
    if (g.lan_join_focus && p == 1) {
        g.lan_join_focus = 0;
        if (Widget* jm = find(w, "join_mission"); jm && jm->parent == grp) c.saved[1] = jm;
    }
    Widget* s = c.saved[static_cast<std::uint8_t>(p)];
    if (s && s->parent == grp) set_focus(s);
    else set_focus(v_first(w));
}

bool GuiRuntime::cc_movies(Widget* w) {  // 002943f8: the movie panel's buttons
    auto& c = *w->cc;
    for (Widget* m : c.movies) {
        if (!m) return false;  // 00294440: straight to the epilogue
        m->flags &= ~flag_visible;
        m->flags &= ~flag_enabled;
        m->id10 = -1;
    }
    const auto classified = host_.get_string("Classified");  // 004aa200
    const std::string s4 = cstr_of(classified.value_or(""));
    const std::int32_t p = host_.current_profile();  // 001314f0(004ffb50, -1)
    if (c.sel == 0x14) {  // the bonus missions: the final segments 17_1 .. 17_10
        for (int n = 0; n < 10; ++n) {
            Widget* m = c.movies[n];
            m->flags |= flag_visible;
            bool lock = true;
            if (host_.zeus_has(p, 0x12, 5, n)) {
                if (const auto t = host_.get_string("FINAL_SEGMENT_" + std::to_string(n))) {  // 004aa210
                    m->flags |= flag_enabled;
                    m->id10 = 0x2710 + n;
                    set_text(m, cstr_of(*t), false);
                    lock = false;
                }
            }
            if (lock) set_text(m, s4, false);
        }
        return true;
    }
    const std::string level = cstr_of(g.level_name);  // 004ffc90
    int i = 0;
    if (iequal(level, "TORONTO1")) {  // 00128e80 == 0
        Widget* m = c.movies[i];
        m->flags |= flag_visible;
        m->flags |= flag_enabled;
        m->id10 = 0;
        set_text(m, cstr_of(host_.get_string("INTRO_MOVIE").value_or("")), false);
        ++i;
    }
    {
        Widget* m = c.movies[i];
        m->flags |= flag_visible;
        if (host_.zeus_has(p, c.sel + 1, 5, 0)) {
            m->flags |= flag_enabled;
            m->id10 = 2;
            set_text(m, cstr_of(host_.get_string(level + "_SOL").value_or("")), false);  // 004aa248
        } else {
            set_text(m, s4, false);
        }
        ++i;
    }
    const auto bonus = host_.get_string(level + "_BONUS");  // 004aa250
    const auto btag = host_.get_string(level + "_BTAG");    // 004aa260, "<n>:<objective>"
    const std::string s7 = bonus ? cstr_of(*bonus) : std::string();
    const std::string s1 = btag ? cstr_of(*btag) : std::string();
    if (!s7.empty() && !s1.empty()) {
        Widget* m = c.movies[i];
        m->flags |= flag_visible;
        bool lock = true;
        if (s1.size() > 1 && s1[1] == ':' && s1.size() > 2) {
            const std::int32_t v = s1[0] >= '0' && s1[0] <= '9' ? s1[0] - '0' : 0;  // 00121bc8 (strtol base 10)
            const std::string key = s1.substr(2);  // 00129360 into the 0x40-byte buffer at sp + 0xc0
            if (key.size() + 1 > 0x40) unhosted(0x29472c, "strcpy overflow of the bonus movie's objective");
            const std::int32_t p2 = host_.current_profile();
            if (host_.profile_3b10(p2, key)) {
                lock = false;
                c.bonus_movie = v;
                m->id10 = 3;
            }
        }
        if (lock) {
            set_text(m, s4, false);
        } else {
            set_text(m, s7, false);
            m->flags |= flag_enabled;
        }
        ++i;
    }
    {
        Widget* m = c.movies[i];
        m->flags |= flag_visible;
        bool open = false;
        const auto eol = host_.get_string(level + "_EOL");  // 004aa268
        const std::int32_t p2 = host_.current_profile();
        const auto v = host_.profile_6060(p2, level);  // 00336060: its static buffer, never NULL
        if (eol && v) {
            const std::int32_t p3 = host_.current_profile();
            if (host_.profile_3b10(p3, cstr_of(*v) + "_COMPLETED")) {  // 004aa270
                open = true;
                m->id10 = iequal(level, "UKRAINE") ? 1 : 4;  // 004aa280
            }
        }
        if (open) {
            set_text(m, cstr_of(*eol), false);
            m->flags |= flag_enabled;
        } else {
            set_text(m, cstr_of(host_.get_string("Classified").value_or("")), false);
        }
    }
    return true;
}

void GuiRuntime::cc_split(Widget* w, const std::optional<std::string>& text) {  // 00294b20
    if (!text) return;
    const std::string s = cstr_of(*text);
    if (s.size() >= 0x70) unhosted(0x294b40, "strcpy overflow of the level title");
    auto& c = *w->cc;
    const auto colon = s.find(':');
    if (colon == std::string::npos) {
        set_text(c.levelname1, s, false);
        return;
    }
    set_text(c.levelname1, s.substr(0, colon), false);
    std::size_t i = colon + 1;
    while (i < s.size() && s[i] == ' ') ++i;
    set_text(c.levelname2, s.substr(i), false);
}

void GuiRuntime::cc_texts(Widget* w) {  // 00294c60
    auto& c = *w->cc;
    if (!c.levelname1 || !c.levelname2 || !c.pou) return;
    set_text(c.levelname1, "", false);
    set_text(c.levelname2, "", false);
    set_text(c.pou, "", false);
    const std::int32_t sel = c.sel;
    if (sel == -1) return;
    if (sel < 17) {
        if (!c.open[sel]) {
            set_text(c.pou, cstr_of(host_.get_string("Classified").value_or("")), false);
            return;
        }
        const char* name = cc_level_names[sel];
        g.level_name = name;  // 00129a70(004ffc90, name, 0x80)
        if (!c.cached[sel]) {
            if (const auto v = host_.get_string(name)) c.cache[sel] = cstr_of(*v);
            c.cached[sel] = 1;
        }
        const bool have = !c.cache[sel].empty();
        if (have) cc_split(w, c.cache[sel]);
        else set_text(c.levelname1, name, false);
        const std::string shown = have ? c.cache[sel] : std::string(name);
        if (shown.size() > 0x3f) unhosted(0x294e18, "strcpy overflow of app +1c8");
        g.app_1c8 = shown;
        return;
    }
    if (c.sel_w == c.online) set_text(c.pou, cstr_of(host_.get_string("Online").value_or("")), false);
    else if (c.sel_w == c.personnel) set_text(c.pou, cstr_of(host_.get_string("Personnel").value_or("")), false);
    else if (c.sel_w == c.unlock) set_text(c.pou, cstr_of(host_.get_string("UnlockedMedia").value_or("")), false);
    else if (c.sel_w == c.training) cc_split(w, host_.get_string("Training"));
}

void GuiRuntime::cc_move(Widget* w, std::uint32_t mask) {  // 00294f50
    auto& c = *w->cc;
    if (c.sel == -1) {
        cc_select(w, cc_default(w), true);
        return;
    }
    if (mask != 4 && mask != 8) return;
    const std::int32_t d = mask == 4 ? 1 : -1;
    std::int32_t s = c.sel + d;
    do {
        if (s >= 24) s = 0;
        else if (s < 0) s = 23;
        const Widget* slot = c.slots[s];
        if (slot && (slot->flags & flag_enabled) && (slot->flags & flag_visible)) {
            send(w, &canvas_, 0x10, 9, 0);
            cc_select(w, s, true);
            return;
        }
        s += d;
    } while (s != c.sel);
}

void GuiRuntime::cc_select(Widget* w, std::int32_t idx, bool reset_panel) {  // 00295090
    auto& c = *w->cc;
    if (c.sel_w) {
        const std::int32_t old = c.sel;
        const bool lit = (old < 17 && c.bonus[old]) || (old == 20 && c.bonus[17]);
        send(w, c.sel_w, 6, c.open[old] ? (lit ? cc_a3_state_on : cc_a3_lose_focus) : cc_a3_state_off, 0);
        c.sel = -1;
        c.sel_w = nullptr;
    }
    if (idx < 0 || idx >= 24) return;
    Widget* slot = c.slots[idx];
    if (!slot || !(slot->flags & flag_enabled) || !(slot->flags & flag_visible)) return;
    if (reset_panel) cc_panel(w, 0);
    c.sel_w = slot;
    c.sel = idx;
    send(w, slot, 6, c.open[idx] ? cc_a3_gain_focus : cc_a3_deactivate, 0);
    cc_texts(w);
    if (idx < 17 && !c.open[c.sel] && !g.app_30) cc_show_bonus(w, g.bonus_row[idx]);
    else cc_show_bonus(w, -1);
}

std::uint8_t GuiRuntime::cc_input(Widget* w, const PadEvent& ev) {  // 00295640
    auto& c = *w->cc;
    if (ev.kind == 1 && ev.port == 0 && ev.released == 0) {
        if (ev.mask == 8 || ev.mask == 4) {
            if (c.panel) cc_panel(w, 0);
            cc_move(w, ev.mask);
            return 1;
        }
        if (ev.mask == pad_cross && c.panel == 0 && c.sel != -1) {
            if (c.open[c.sel]) cc_panel(w, static_cast<std::int8_t>(c.panel_of[c.sel]));
            else send(c.slots[c.sel], &canvas_, 0x10, 4, 0);
            return 1;
        }
        if (ev.mask == pad_triangle) {
            if (c.panel == 4) {
                c.saved[4] = nullptr;
                cc_panel(w, c.sel == 0x14 ? 0 : 1);
            } else if (c.panel == 5) {
                c.saved[5] = nullptr;
                cc_panel(w, 4);
            } else if (c.panel == 0) {
                if (g.app_30 && !g.app_32) {  // 00295818: the quit question (5, "", LOBBY_STR14, this, 0x19)
                    const std::string m = lobby_string(0xe);
                    c.quit_popup = popup_open(5, std::string(), m, w, 0x19);  // +2f8
                    return 1;
                }
                cc_exit(w);
            } else {
                cc_panel(w, 0);
            }
            return 1;
        }
    }
    // 0033cef0: with the network manager (+78), START on port 0 opens the messages (00532fd0 = +84, the
    // navigator's op 9, NETMESS); it is null offline and in LAN mode
    if (c.net78 && ev.kind == 1 && ev.port == 0 && ev.mask == 0x800 && ev.released == 0) {
        g.b_2fd0 = c.w84;
        net_navigate(w, 9);
        return 1;
    }
    return 0;
}

void GuiRuntime::cc_exit(Widget* w) {  // 002955b0
    auto& c = *w->cc;
    c.exiting = 1;
    const std::int32_t p = host_.current_profile();
    if (host_.profile_byte(p, 0xb73) || host_.profile_byte(p, 0xb72)) {
        c.save.mode = 2;  // the autosave; its end (vtable +5c, 00295430) leaves whatever the result
        save_start(w);
        return;
    }
    cc_leave(w);  // vt +5c
}

void GuiRuntime::cc_leave(Widget* w) {  // 00295430
    auto& c = *w->cc;
    if (!c.exiting) return;
    Widget* next = nullptr;
    if (g.app_30 && !g.app_32) {  // 00295470: ONOFFSEL, the popup (0, "", LOBBY_STR119) around 002ca640(app, 1)
        next = factory("GuiOnlineOfflineSelect", std::string("ONOFFSEL"));
        const std::string m = lobby_string(0x77);
        const std::int32_t popup = popup_open(0, std::string(), m, nullptr, 0);
        net_leave(1);
        popup_close(popup);
    } else {
        next = g.app_online ? factory("GuiTitleScreen", std::string("TITLESCR"))
                            : factory("GuiOnlineOfflineSelect", std::string("ONOFFSEL"));
    }
    if (next) {
        switch_screens(w, next);
        cc_clear(w);
    }
    c.exiting = 0;
}

void GuiRuntime::cc_update(Widget* w, float dt) {  // 00296660
    OMEGA_ZONE("gui.cc_update");
    auto& c = *w->cc;
    {
        OMEGA_ZONE("gui.cc_circle");
        cc_circle(w, dt);
    }
    {
        OMEGA_ZONE("gui.cc_base_update");
        base_update(w, dt);  // 0033d310
    }
    net_poll(w, c.start_button);  // its vt +60, 0033cca0 (checkpoint 132on2b: with a session)
    if (g.b_2fe8) unhosted(0x33d0c0, "net screen update (byte 00532fe8)");
    const auto panel = static_cast<std::uint8_t>(c.panel);
    if (panel != 1 && panel != 3 && panel != 4 && panel != 5) return;
    c.pulse = fmodf_ee(fadd(c.pulse, fmul(std::bit_cast<float>(0x42055555u), dt)), 500.0f);  // 00121ae8
    const float t = std::fabs(fsub(1.0f, fdiv(fmul(2.0f, c.pulse), 500.0f)));
    Rgba col;  // 002986b0
    for (int i = 0; i < 4; ++i) col[i] = fadd(cc_pulse_lo[i], fmul(t, fsub(cc_white[i], cc_pulse_lo[i])));
    const std::int32_t p = host_.current_profile();
    if (panel == 4) {
        const std::int32_t s4 = c.sel < 17 ? c.sel + 1 : c.sel == 0x14 ? 0x12 : -1;
        for (int k = 0; k < 6; ++k) {
            if (!c.zeus[k]) continue;
            if (s4 == -1) set_rgba(c.zeus[k], cc_white);
            else set_rgba(c.zeus[k], host_.level_flag(p, s4, k) ? col : cc_white);
        }
    }
    if (panel == 3 && c.modify_agent) {
        const std::int32_t p2 = host_.current_profile();
        set_rgba(c.modify_agent, host_.profile_byte(p2, 0xb68) ? col : cc_white);
    }
    if (panel == 1 && c.zeus_files) {
        if (c.sel < 17) set_rgba(c.zeus_files, host_.level_flag(p, c.sel + 1, -1) ? col : cc_white);
        else set_rgba(c.zeus_files, cc_white);
    }
    if (panel == 5) {  // 00296908: a movie not yet seen pulses
        for (int n = 0; n < 10; ++n) {
            Widget* m = c.movies[n];
            if (!m || !(m->flags & flag_visible)) continue;
            bool lit = false;
            if (m->flags & flag_enabled) {
                if (c.sel == 0x14) lit = !host_.zeus_viewed(p, 0x12, 5, n);
                else if (m->id10 == 1 || m->id10 == 4) lit = !host_.zeus_viewed(p, c.sel + 1, 5, 1);
                else if (m->id10 == 3) lit = !host_.zeus_viewed(p, c.sel + 1, 5, 2);
                else if (m->id10 == 2) lit = !host_.zeus_viewed(p, c.sel + 1, 5, 0);
            }
            set_rgba(m, lit ? col : cc_white);
        }
    }
}

void GuiRuntime::cc_circle(Widget* w, float dt) {  // 00296a80
    auto& c = *w->cc;
    if (!c.circle) return;
    // The EE passes a zeroed stack float for event 9 to fill with the circle's
    // IE time; with no interface element (decorator +24 null) it stays 0.
    send(w, c.circle, 9, 0, 0);
    const float now = 0.0f;
    const float next = fadd(now, dt);
    for (std::uint32_t bits : cc_circle_marks) {
        const float mark = std::bit_cast<float>(bits);
        if (now < mark && !(next < mark)) {
            send(w, &canvas_, 0x10, 8, 0);
            return;
        }
    }
}

void GuiRuntime::cc_show_bonus(Widget* w, std::int32_t row) {  // 00296b70
    auto& c = *w->cc;
    if (!c.bonus_grp || !c.list) return;
    if (row == -1) {
        c.bonus_grp->flags &= ~flag_visible;
        return;
    }
    const std::int32_t p = host_.current_profile();
    const auto rows = host_.profile_vec(p, 3);
    if (row < 0 || static_cast<std::size_t>(row) >= rows.size())
        throw std::runtime_error("gui runtime: bonus row " + std::to_string(row) + " out of range (00296b70)");
    if (rows[static_cast<std::size_t>(row)].b8 == 1) {
        c.bonus_grp->flags &= ~flag_visible;
        return;
    }
    c.bonus_grp->flags |= flag_visible;
    list_clear(c.list);
    const auto items = host_.profile_items(p, 3, row);
    for (std::size_t i = 0; i < items.size(); ++i) {
        const auto& it = items[i];
        const std::int8_t style = it.b8 == 2 ? 0 : it.b8 == 1 ? 1 : 2;
        std::string key = "Classified";
        if (it.b8) {
            if (!it.name) unhosted(0x296d00, "bonus item without a name");
            key = cstr_of(*it.name);
        }
        const auto text = host_.get_string(key);
        if (!text) unhosted(0x296d0c, "bonus item string '" + key + "' missing");
        if (cstr_of(*text).size() >= 0x80) unhosted(0x296d0c, "bonus item string overflow");
        list_add(c.list, cstr_of(*text), static_cast<std::int32_t>(i), style);
    }
}

void GuiRuntime::cc_level_block(std::int32_t idx) {  // 002cc820
    if (idx >= 0 && idx < 18) {
        g.app_188 = cc_level_names[idx];
        g.app_248 = cc_app_248[idx];
        g.app_24c = idx;
        g.app_250 = cc_app_250[idx];
        g.app_251 = cc_app_250[idx];
        g.level_494198 = idx;
    } else {
        g.app_188.clear();
        g.app_1c8.clear();
        g.app_248 = -1;
        g.app_24c = -1;
        g.app_250 = 0;
    }
}

std::uint8_t GuiRuntime::cc_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {  // 002958b0
    auto& c = *w->cc;
    const std::uint32_t t = type & 0xffff;
    if (t == 0x18) {
        c.keep = 1;
        return 1;
    }
    if (t == 0x10 && a3 == 1) return 1;
    if (t == 0x1a) {
        if (a3 == 0x19) {  // 0029593c: the online quit question's yes
            // 002c8090(app): 004294c0, 001314f0; with no DME session it returns at once
            // (checkpoint 132on2b: with a session the host's 002c8090 reads the agent itself)
            if (host_.net_session()) host_.lobby_messages();
            else host_.current_profile();
            cc_exit(w);
            popup_close(c.quit_popup);
            return 1;
        }
        // Checkpoint 132on2i: the agency cell question's yes (0029597c) and the create failure's reply 0x1c
        // (00295bd4) open the cell-name keyboard: (LOBBY_STR 0x5d, "", this, 0xf, reply 0x1b, 0).
        if (a3 == 0x1a || a3 == 0x1c) {
            Widget* kb = factory("GuiKeyboard", std::string("KEYBOARD"));  // 004aa368 / 004aa378
            if (kb) kb_open(kb, lobby_string(0x5d), std::string(), w, 0xf, 0x1b, 0);  // 0032b4f0
            return 1;
        }
        if (a3 == 0x1b) {  // 002959f0: the keyboard's cell name
            const bool s = host_.net_session();
            const std::optional<std::string> name =
                sender && sender->kb ? sender->kb->result : std::optional<std::string>();  // t0 the keyboard's text
            if (!s || !name || name->empty() || (*name)[0] == '\0') return 1;
            const std::string nm = cstr_of(*name);
            const std::int32_t wait = popup_open(0, std::string(), lobby_string(0x7e, std::nullopt, nm), nullptr, 0);
            if (host_.lobby_cell_create(nm)) {  // 0044af60(session, name)
                net_navigate(w, 3);  // 0033c320
            } else {
                std::uint32_t text = 0x63;  // 00452ff0(session, 0) & 0xff
                switch (host_.lobby_error() & 0xff) {
                case 0x1c: text = 0x62; break;
                case 0x03: text = 0x61; break;
                case 0x1a: text = 0x60; break;
                case 0x1b: text = 0x5f; break;
                default: break;
                }
                const std::string title = lobby_string(0x5e);
                const std::string m = lobby_string(text, std::nullopt, nm);
                popup_open(5, title, m, w, 0x1c);  // 002cae30
            }
            popup_close(wait);  // 002cac80
            return 1;
        }
    }
    if (t != 2) {
        if (t == 0) c.saved[static_cast<std::uint8_t>(c.panel)] = sender;
        return net_event(w, sender, type, a3, t0);
    }
    if (!sender) unhosted(0x2958b0, "activation without a sender (reads sender +1c)");
    Widget* grp = sender->parent;
    if (grp == c.groups[1]) {
        if (sender->name == "zeus_files") {
            cc_panel(w, 4);
            return 1;
        }
        const std::int32_t s2 = c.sel >= 0 && c.sel < 17 ? c.sel : c.sel == 0x17 ? 0x11 : -1;
        cc_level_block(s2);
        if (s2 == -1) return 1;
        g.level_name = cc_level_names[s2];
        if (sender->name == "play_mission") {
            if (g.app_30) {
                if (g.app_32) unhosted(0x295d44, "the command centre's mission setup from a lobby room");
                // 00295d70: the navigator's op 6 (MSNSETUP); when it fails, (2, "", LOBBY_STR91, this, 0x18)
                if (!net_navigate(w, 6)) {
                    const std::string m = lobby_string(0x5b);
                    popup_open(2, std::string(), m, w, 0x18);
                }
                return 1;
            }
            g.app_35 = 1;
            g.app_33 = 0;
            g.app_34 = c.sel == 0x17;
            g.app_state_prev = g.app_state;
            g.app_state = 3;
            const std::int32_t p = host_.current_profile();
            host_.level_start(p, c.sel + 1, 5, 0, 1);
            return 1;
        }
        if (sender->name == "join_mission") {
            if (g.app_32) return 1;
            if (c.sel >= 0 && c.sel < 17 && g.app_248 >= 0) {
                // 00295ea0: the navigator's op 2 (JOINMIS); when it fails, (2, "", LOBBY_STR91, this, 0x18)
                if (!net_navigate(w, 2)) {
                    const std::string m = lobby_string(0x5b);
                    popup_open(2, std::string(), m, w, 0x18);
                }
            }
            return 1;
        }
        return 1;
    }
    if (grp == c.groups[3]) {
        if (sender->name == "modify_agent") {  // 00295f20 (0048b728): the title's newagent shape
            const bool noapp = g.app_loaded == 0;
            std::int32_t id = -2;
            if (noapp) {
                host_.app_2c82f0();
                const auto msg = host_.get_string("LoadCharData");  // 004aa3b0
                id = popup_open(0, std::string(), msg, nullptr, 0);
            }
            Widget* next = factory("GuiAgentModify", std::string("AGENTMOD"));  // 004aa3c0 / 004aa3d0
            if (noapp) {
                popup_close(id);
                host_.app_2c81d0();
            }
            if (next) switch_screens(w, next);
            return 1;
        }
        if (sender->name == "statistics") {  // 002960d0: factory("GuiMissionStatistics", "STATS") (004aa430 / 004aa448)
            if (Widget* next = factory("GuiMissionStatistics", std::string("STATS"))) switch_screens(w, next);  // 002961a8
            return 1;
        }
        // 00295f20's other personnel files, each switched to when the factory yields one.
        Widget* next = nullptr;
        if (sender->name == "dossier") next = factory("GuiDossiers", std::string("DOSSIERS"));
        else if (sender->name == "rank") next = factory("GuiRanks", std::string("RANKS"));
        else if (sender->name == "ratings") next = factory("GuiRatings", std::string("RATINGS"));
        else if (sender->name == "special_ratings") next = pe_achievement(0);  // 003534a0
        else if (sender->name == "commendations") next = pe_achievement(1);
        else if (sender->name == "medals") next = pe_achievement(2);
        else if (sender->name == "omega_strain") next = factory("GuiOmegaStrain", std::string("OMEGA"));
        if (next) switch_screens(w, next);
        return 1;
    }
    if (grp == c.groups[2]) {
        cc_level_block(-1);
        if (sender->name == "agency_cell") {  // 00296204: the navigator's op 3 (AGENTCEL), checkpoint 132on2i
            if (!net_navigate(w, 3)) {  // not a cell member: (5, "", LOBBY_STR 0x5c, this, 0x1a) "create one?"
                const std::string m = lobby_string(0x5c);
                popup_open(5, std::string(), m, w, 0x1a);
            }
            return 1;
        }
        if (sender->name == "contacts") {  // 00296278: the navigator's op 4, CONTACTS (checkpoint 132on2h)
            net_navigate(w, 4);  // 0033c320
            return 1;
        }
        if (sender->name == "online_prefs") {  // 002962a0: app +268 = 1 (CMDCENTR on the way back), op 5 ONLIPREF
            g.app_268 = 1;
            net_navigate(w, 5);  // 0033c320
            return 1;
        }
        cc_panel(w, 0);
        return 1;
    }
    if (grp == c.groups[4]) {
        if (sender->id10 == 5) {
            cc_panel(w, 5);
            return 1;
        }
        if (sender->id10 == -1) return 1;
        // 00296314: the slideshow of the level's ZEUS files (ZEUSHELL.GUI);
        // Triangle there returns to this screen's class and file.
        Widget* ss = factory("GuiSlideShow", std::string("ZEUSHELL"));  // 004aa468 / 004aa478
        if (!ss) return 1;
        ss_return(ss, class_id(w->cls), w->name, 0);
        if (c.sel == 0x14) ss_load(ss, 0x12, sender->id10, -1, 1);
        else ss_load(ss, c.sel + 1, sender->id10, -1, 0);
        switch_screens(w, ss);
        return 1;
    }
    if (grp == c.groups[5]) cc_play(w, sender);
    return 1;
}

void GuiRuntime::cc_play(Widget* w, Widget* sender) {  // 002963e4: the movie player
    auto& c = *w->cc;
    const std::int32_t id = sender->id10;
    const std::string level = cstr_of(g.level_name);
    std::string name;  // the 0x60-byte buffer at sp + 0x60
    if (id == 1) {
        name = "FINAL";  // 004aa498
        const std::int32_t p = host_.current_profile();
        host_.level_start(p, 0x11, 5, 1, 1);  // 003328d0
    } else if (id == 0) {
        name = "INTRO";  // 004aa490
    } else if (id == 4) {
        name = level + "_EOL";
        const std::int32_t p = host_.current_profile();
        host_.level_start(p, c.sel + 1, 5, 1, 1);
    } else if (id == 3) {
        name = "19_" + std::to_string(c.bonus_movie);  // 004aa488
        const std::int32_t p = host_.current_profile();
        host_.level_start(p, c.sel + 1, 5, 2, 1);
    } else if (id == 2) {  // $a0 = 2 from 00295c48
        name = level + "_SOL";
        const std::int32_t p = host_.current_profile();
        host_.level_start(p, c.sel + 1, 5, 0, 1);
    } else if (id >= 0x2710) {
        name = "17_" + std::to_string(id - 0x270f);  // "%d_%d" with 0x11
        const std::int32_t p = host_.current_profile();
        host_.level_start(p, 0x12, 5, id - 0x2710, 1);
    } else {
        return;
    }
    cc_bonus_flags(w);  // 002972e0
    // The app's movie list (*004ffc04): count +13c = 1, name +39 (0x20 bytes), then G+50 = 4.
    if (name.size() + 1 > 0x20) unhosted(0x2965dc, "strcpy overflow of the app's movie name");
    g.app_attract = 1;
    g.app_mode = name;
    g.app_state_prev = g.app_state;
    g.app_state = 4;
}

std::uint8_t GuiRuntime::net_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {  // 0033cf90
    if ((type & 0xffff) == 0x2000) unhosted(0x33d0c0, "net screen event 0x2000");
    if (a3 == 0x13) return 1;  // the network manager (004294c0) is null
    return save_widget_event(w, sender, type, a3, t0);
}

// ---------------------------------------------------------------- character code map (0055ca90)

namespace {

// 0043fbb0 parses the file object's read buffer: the file's bytes, then
// zeros up to the next 0x800-byte sector boundary. Bytes past that are not
// modelled.
struct MapBuffer {
    const std::vector<std::uint8_t>& file;
    std::size_t padded = 0;
    std::uint8_t at(std::size_t i) const {
        if (i >= padded) unhosted(0x43fe20, "char map parse past the read buffer");
        return i < file.size() ? file[i] : 0;
    }
};

bool ee_isspace(std::uint8_t c) { return c == ' ' || (c >= 9 && c <= 13); }
int hex_digit(std::uint8_t c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// The sscanf of 0043fe20 (only whitespace, literals, %d and %x occur):
// returns the number of conversions. Inputs whose result the model does
// not pin down (overlong numbers, "0x" without a digit) throw.
int map_sscanf(const MapBuffer& b, std::size_t pos, const char* fmt, std::int32_t* out) {
    std::size_t end = pos;
    while (b.at(end)) ++end;  // strlen of the input first
    int n = 0;
    std::size_t p = pos;
    for (const char* f = fmt; *f; ++f) {
        const auto fc = static_cast<std::uint8_t>(*f);
        if (ee_isspace(fc)) {
            while (p < end && ee_isspace(b.at(p))) ++p;
            continue;
        }
        if (fc != '%') {
            if (p >= end) return n ? n : -1;
            if (b.at(p) != fc) return n;
            ++p;
            continue;
        }
        const char conv = *++f;
        while (p < end && ee_isspace(b.at(p))) ++p;
        if (p >= end) return n ? n : -1;
        bool neg = false;
        if (b.at(p) == '+' || b.at(p) == '-') {
            neg = b.at(p) == '-';
            ++p;
        }
        std::uint32_t v = 0;
        int digits = 0;
        if (conv == 'x') {
            if (b.at(p) == '0' && (b.at(p + 1) == 'x' || b.at(p + 1) == 'X')) {
                if (hex_digit(b.at(p + 2)) < 0) unhosted(0x43fe20, "char map \"0x\" without a hex digit");
                p += 2;
            }
            for (int d; (d = hex_digit(b.at(p))) >= 0; ++p, ++digits) v = v * 16 + static_cast<std::uint32_t>(d);
            if (digits > 8) unhosted(0x43fe20, "char map hex number over 8 digits");
        } else {
            for (std::uint8_t c; (c = b.at(p)) >= '0' && c <= '9'; ++p, ++digits) v = v * 10 + (c - '0');
            if (digits > 9) unhosted(0x43fe20, "char map decimal number over 9 digits");
        }
        if (!digits) return n;
        out[n++] = static_cast<std::int32_t>(neg ? 0u - v : v);
    }
    return n;
}

std::uint8_t map_parse(CharMap& m, const MapBuffer& b) {  // 0043fe20
    m.count = 0;
    std::int32_t hv[2] = {};
    if (map_sscanf(b, 0, "[CHARACTER_CODE_MAP VERSION(%d) CHAR_COUNT(%d)]\n", hv) != 2) return 0;
    if (hv[0] != 1 || !(hv[1] < 0xdf) || hv[1] == 0) return 0;
    std::size_t p = 0;
    for (std::int32_t i = 0; i < hv[1]; ++i) {
        std::size_t q = p + 1;  // strchr(p + 1, '\n')
        for (;; ++q) {
            const std::uint8_t c = b.at(q);
            if (c == '\n') break;
            if (c == 0) return 0;
        }
        p = q;
        std::int32_t v = 0;
        if (map_sscanf(b, p + 1, "%x", &v) != 1) return 0;
        if (static_cast<std::uint32_t>(v) & 0xffff0000u) return 0;
        m.codes[static_cast<std::size_t>(i)] = static_cast<std::uint16_t>(v);
        ++m.count;
    }
    return m.codes[0] == 0x20;
}

}  // namespace

std::uint8_t CharMap::load(const std::optional<std::vector<std::uint8_t>>& file) {  // 0043fbb0
    if (!file) unhosted(0x3696b0, "char map file that does not open");
    const std::size_t size = file->size();  // 003694d0
    if (!(48 + 2 < size)) return 0;         // strlen of the header format + 2
    const MapBuffer b{*file, (size + 0x7ff) & ~std::size_t{0x7ff}};
    if (!map_parse(*this, b)) return 0;
    page.fill(-1);
    page[0] = 0;
    for (std::size_t i = 0; i < 0x20; ++i) pages[i] = static_cast<std::uint8_t>(i);
    std::int8_t used = 1;
    for (std::int32_t i = 0; i < count; ++i) {
        const std::uint16_t code = codes[static_cast<std::size_t>(i)];
        const std::size_t hi = code >> 8;
        if (page[hi] < 0) {
            if (used == 4) return 0;
            page[hi] = used++;
        }
        pages[static_cast<std::size_t>(page[hi]) * 256 + (code & 0xff)] = static_cast<std::uint8_t>(i);
    }
    return 1;
}

std::uint8_t CharMap::lookup(std::int32_t code) const {  // 003fd6b0
    if (!ready) return 0xff;
    const auto c = static_cast<std::uint32_t>(code) & 0xffff;
    const std::int8_t pg = page[(c >> 8) & 0xff];
    if (pg < 0) return 0xff;
    const std::uint8_t v = pages[static_cast<std::size_t>(pg) * 256 + (c & 0xff)];
    if (v == 0xff) return 0xff;
    return c < 0x20 ? v : static_cast<std::uint8_t>(v + 0x20);
}

// ---------------------------------------------------------------- GuiKeyboard (vtable 004de790)
// KEYBOARD.GUI: "keys" holds five rows of GuiButtonWidget keys (14, 14, 13,
// 13, 3); the layouts (0052af60 + 16 * lang) map key codes to the four
// characters of a key (plain, shift, AltGr, shift + AltGr) and dead keys.

void GuiRuntime::kb_init(Widget* w) {  // 0032cb40
    base_init(w);
    auto& k = *w->kb;
    k.target = nullptr;
    k.arg = 0;
    Widget* keys = find(w, kbd::keys_name);
    if (!keys || keys->children.size() != kbd::row_counts.size()) return;
    for (std::size_t r = 0; r < kbd::row_counts.size(); ++r) {
        Widget* row = keys->children[r];
        if (static_cast<std::int32_t>(row->children.size()) != kbd::row_counts[r]) return;
        for (Widget* c : row->children) {
            if (c->cls != WidgetClass::button) return;  // isA "GuiButtonWidget"
            k.keys.push_back(c);
        }
    }
    static constexpr std::array<std::int32_t, 5> start = {0, 14, 28, 41, 54};
    std::size_t i = 0;
    for (std::size_t r = 0; r < kbd::row_counts.size(); ++r) {
        const std::int32_t n = kbd::row_counts[r];
        for (std::int32_t c = 0; c < n; ++c, ++i) {
            const auto cu = static_cast<std::size_t>(c);
            k.up.push_back(kbd::up_cols[r][cu] + start[r == 0 ? 4 : r - 1]);
            k.dn.push_back(kbd::down_cols[r][cu] + start[r == 4 ? 0 : r + 1]);
            k.lf.push_back((c == 0 ? n - 1 : c - 1) + start[r]);
            k.rt.push_back((c == n - 1 ? 0 : c + 1) + start[r]);
            k.keys[i]->id10 = c + start[r];
            k.ids.push_back(kbd::row_ids[r][cu]);
        }
    }
    kb_layout(w, static_cast<std::uint8_t>(lang));
    Widget* e = find(w, kbd::edit_name);
    k.edit = e && e->cls == WidgetClass::edit ? e : nullptr;
    if (k.edit) {
        k.edit->flags &= ~flag_focusable;
        k.edit->e88 = 1;
    }
    Widget* l = find(w, kbd::label_name);
    k.label = l && l->cls == WidgetClass::text ? l : nullptr;
}

void GuiRuntime::kb_layout(Widget* w, std::int32_t lang_) {  // 0032d0d0
    if (lang_ < 0 || lang_ >= static_cast<std::int32_t>(kbd::layouts.size()))
        unhosted(0x32d0d0, "keyboard layout past the six of 0052af60");
    auto& k = *w->kb;
    k.dead = nullptr;
    k.lang = lang_;
    k.layout = lang_;
    k.lay.assign(k.keys.size(), -1);
    if (k.keys.size() != k.ids.size()) return;
    const auto& L = kbd::layouts[static_cast<std::size_t>(lang_)];
    k.altgr = 0;
    for (std::size_t i = 0; i < k.keys.size(); ++i) {
        for (std::size_t j = 0; j < L.entries.size(); ++j) {
            if (L.entries[j].id == k.ids[i]) {
                k.lay[i] = static_cast<std::int32_t>(j);
                set_text(k.keys[i], kbd::relabel_empty, false);
                break;
            }
        }
    }
    for (const auto& e : L.entries)
        if (e.code[2] != 0xffff || e.code[3] != 0xffff) k.altgr = 1;
    kb_relabel(w, 0);
}

std::int32_t GuiRuntime::kb_mod_index(const Widget* w, const kbd::Entry& e) {  // 0032c2d0
    const std::uint32_t m = w->kb->mods;
    std::int32_t v;
    if (m & 4) {
        if (m & 2) {
            v = (e.flags & 2) ? 3 : 2;
            if (m & 1) v = v == 2 ? 3 : 2;
        } else {
            v = (m & 1) ? 3 : 2;
        }
    } else if (m & 2) {
        v = e.flags & 1;
        if (m & 1) v = !v;
    } else {
        v = static_cast<std::int32_t>(m & 1);
    }
    return v;
}

std::uint8_t GuiRuntime::kb_label_char(const Widget* w, const kbd::Entry& e) const {
    const auto& k = *w->kb;
    const std::uint16_t code = e.code[static_cast<std::size_t>(kb_mod_index(w, e))];
    if (k.dead)
        for (const auto& p : k.dead->pairs)
            if (p.from == code) return charmap.lookup(p.to);
    return charmap.lookup(code);
}

std::uint8_t GuiRuntime::kb_resolve(Widget* w, const kbd::Entry& e) {
    auto& k = *w->kb;
    const std::int32_t m = kb_mod_index(w, e);
    const std::uint16_t code = e.code[static_cast<std::size_t>(m)];
    if (e.flags & (4u << m)) {
        k.dead = nullptr;
        for (const auto& d : kbd::layouts[static_cast<std::size_t>(k.layout)].dead)
            if (d.key == code) {
                k.dead = &d;
                break;
            }
        return 0xff;
    }
    std::uint8_t ch;
    if (k.dead) {
        ch = 0xff;
        for (const auto& p : k.dead->pairs)
            if (p.from == code) {
                ch = charmap.lookup(p.to);
                break;
            }
    } else {
        ch = charmap.lookup(code);
    }
    k.dead = nullptr;
    return ch;
}

void GuiRuntime::kb_relabel(Widget* w, std::uint32_t mods) {  // 0032b5f0
    auto& k = *w->kb;
    k.mods = mods;
    for (std::size_t i = 0; i < k.keys.size(); ++i) {
        if (i >= k.lay.size()) unhosted(0x32b648, "keyboard relabel past the layout index list");
        if (k.lay[i] != -1) {
            const auto& e = kbd::layouts[static_cast<std::size_t>(k.layout)].entries[static_cast<std::size_t>(k.lay[i])];
            const std::uint8_t ch = kb_label_char(w, e);
            const char s[2] = {static_cast<char>(ch == 0xff ? 0 : ch), 0};
            set_text(k.keys[i], s, false);
            // a3 receives the decorator's IE instance (d+24); with none the
            // 003e7ee0 key colour calls are skipped.
            send(w, k.keys[i], 5, 1, 0);
        }
        if (i >= k.ids.size()) unhosted(0x32b900, "keyboard relabel past the key id list");
        if (k.ids[i] == 0x3ec) send(w, k.keys[i], 5, 1, 0);
    }
}

Widget* GuiRuntime::kb_find_id(const Widget* w, std::int32_t id) const {  // 0032b260
    const auto& k = *w->kb;
    for (std::size_t i = 0; i < k.ids.size() && i < k.keys.size(); ++i)
        if (k.ids[i] == id) return k.keys[i];
    return nullptr;
}

bool GuiRuntime::kb_shift_key(const Widget* w, const Widget* key) const {  // 0032af70
    return key == kb_find_id(w, 0x3ea) || key == kb_find_id(w, 0x408) || key == kb_find_id(w, 0x39);
}

bool GuiRuntime::kb_char_ok(const Widget* w, std::uint8_t ch) const {  // 0032af10
    if (ch != 0x20) return true;
    if (!w->kb->edit) unhosted(0x32af20, "keyboard space without an edit box");
    return !w->kb->edit->text.empty();
}

void GuiRuntime::kb_focus(Widget* w, Widget* key) {
    if (key != canvas_.focus) {
        if (canvas_.focus) o_lose(canvas_.focus);
        canvas_.focus = key;
        if (key && !kb_shift_key(w, key)) o_gain(key);
    }
    w->focus = key;
}

std::uint8_t GuiRuntime::kb_input(Widget* w, const PadEvent& ev) {  // 0032ba10
    auto& k = *w->kb;
    if (ev.kind == 1 && ev.port == 0 && ev.mask == pad_triangle && ev.released == 0) {
        kb_close(w, false);
        return 1;
    }
    if (ev.kind == 1 && ev.port == 0 && ev.mask == 0x80 && ev.released == 0) {  // SQUARE
        kb_close(w, true);
        return 1;
    }
    const std::vector<std::int32_t>* nav = nullptr;
    switch (ev.mask) {
    case 0x4000: nav = &k.dn; break;
    case 0x1000: nav = &k.up; break;
    case 0x2000: nav = &k.rt; break;
    case 0x8000: nav = &k.lf; break;
    default: break;
    }
    std::int32_t n = -1;
    if (nav) {
        if (k.key < 0 || static_cast<std::size_t>(k.key) >= nav->size())
            unhosted(0x32bb50, "keyboard navigation from a key past the tables");
        n = (*nav)[static_cast<std::size_t>(k.key)];
    }
    Widget* s1 = nullptr;
    if (n != -1) {
        if (ev.released == 0) {
            k.key = n;
            if (static_cast<std::size_t>(n) >= k.keys.size()) unhosted(0x32bbcc, "keyboard key index past the keys");
            s1 = k.keys[static_cast<std::size_t>(n)];
            send(s1, &canvas_, 0x10, 6, 0);
            repeat_mask &= ~0x40u;
        } else if (ev.released == 1) {
            repeat_mask |= 0x40u;
        }
    }
    if (s1) {
        if (ev.zero == 1) repeat_mask |= 0x40u;
        else repeat_mask &= ~0x40u;
    }
    if (ev.kind == 2 && ev.port == 0 && ev.released == 0) {
        if (!k.edit) return 1;
        const kbd::Dead* s4 = k.dead;
        bool s3 = true;
        switch (ev.mask) {
        case 0x4c:
            edit_delete(k.edit, true);
            k.dead = nullptr;
            break;
        case 0x2a:
            edit_delete(k.edit, false);
            k.dead = nullptr;
            break;
        case 0x50:
            if (k.edit->cursor) edit_cursor(k.edit, k.edit->cursor - 1);
            k.dead = nullptr;
            break;
        case 0x4f:
            edit_cursor(k.edit, k.edit->cursor + 1);
            k.dead = nullptr;
            break;
        case 0x39: kb_relabel(w, k.mods ^ 2u); break;
        case 0x29: kb_close(w, false); break;
        case 0x28: kb_close(w, true); break;
        default:
            s3 = false;
            if (static_cast<std::int32_t>(k.edit->text.size()) < k.max) {
                for (std::size_t i = 0; i < k.ids.size(); ++i) {
                    if (k.ids[i] != ev.mask) continue;
                    if (i >= k.lay.size()) unhosted(0x32be44, "keyboard key past the layout index list");
                    if (k.lay[i] == -1) break;
                    const auto& e = kbd::layouts[static_cast<std::size_t>(k.layout)].entries[static_cast<std::size_t>(k.lay[i])];
                    const std::uint8_t ch = kb_resolve(w, e);
                    if (ch != 0xff && kb_char_ok(w, ch)) {
                        const char s[2] = {static_cast<char>(ch), 0};
                        edit_insert(k.edit, s);
                        s3 = true;
                    }
                    break;
                }
            }
            if (!s3) send(w, &canvas_, 0x10, 4, 0);
            break;
        }
        if (k.dead != s4) kb_relabel(w, k.mods);
        return 1;
    }
    if (!s1) return 0;
    kb_focus(w, s1);
    return 1;
}

std::uint8_t GuiRuntime::kb_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {  // 0032c390
    auto& k = *w->kb;
    const std::uint32_t t = type & 0xffff;
    if (t == 0x2000) {
        kb_close(w, false);
        return 1;
    }
    if (t == 2 && sender->id10 != -1) {
        const auto idx = static_cast<std::size_t>(sender->id10);
        if (idx >= k.ids.size()) unhosted(0x32c408, "keyboard event from a key past the key id list");
        const kbd::Dead* s2 = k.dead;
        std::uint8_t ch = 0xff;
        bool s0 = false;
        bool s3 = true;
        switch (k.ids[idx]) {
        case 0x50:
            if (!k.edit) unhosted(0x32c560, "keyboard cursor key without an edit box");
            if (k.edit->cursor) edit_cursor(k.edit, k.edit->cursor - 1);
            k.dead = nullptr;
            break;
        case 0x4f:
            if (!k.edit) unhosted(0x32c548, "keyboard cursor key without an edit box");
            edit_cursor(k.edit, k.edit->cursor + 1);
            k.dead = nullptr;
            break;
        case 0x28: kb_close(w, true); break;
        case 0x2a:
            if (k.edit) edit_delete(k.edit, false);
            k.dead = nullptr;
            break;
        case 0x408:
        case 0x3ea:
            if (k.mods & 1) {
                kb_relabel(w, k.mods & ~1u);
                k.e1 = 0;
            } else {
                kb_relabel(w, k.mods | 1);
                k.e1 = 1;
            }
            break;
        case 0x39: kb_relabel(w, k.mods ^ 2u); break;
        case 0x428:
        case 0x3ec:
            if (k.altgr) kb_relabel(w, k.mods ^ 4u);
            else s3 = false;
            break;
        default:
            s0 = true;
            s3 = false;
            if (idx >= k.lay.size()) unhosted(0x32c580, "keyboard key past the layout index list");
            if (k.lay[idx] != -1)
                ch = kb_resolve(w, kbd::layouts[static_cast<std::size_t>(k.layout)].entries[static_cast<std::size_t>(k.lay[idx])]);
            break;
        }
        if (k.dead != s2) kb_relabel(w, k.mods);
        if (k.edit && s0 && ch != 0xff) {
            if (kb_char_ok(w, ch) && static_cast<std::int32_t>(k.edit->text.size()) < k.max) {
                const char s[2] = {static_cast<char>(ch), 0};
                edit_insert(k.edit, s);
                s3 = true;
            } else {
                s3 = false;
            }
            if (k.e1) kb_relabel(w, k.mods ^ 1u);
            k.e1 = 0;
        }
        send_up(w, 0x10, s3 ? 1 : 4, 0);
        return 1;
    }
    if (t == 0x10 && a3 == 1) return 1;
    return base_event(w, sender, type, a3, t0);
}

void GuiRuntime::kb_update(Widget* w, float dt) {  // 0032c860
    base_update(w, dt);
    auto& k = *w->kb;
    if (kb_a701) {
        std::uint32_t v = 0;
        if (k.altgr && (kb_mod & 0x44)) v |= 4;
        if (kb_mod & 0x22) v |= 1;
        bool a1 = false;
        if ((v & k.mods) != v) {
            if (v & 1) k.e1 = 0;
            k.mods |= v;
            a1 = true;
        }
        if (k.usb_mods != v) {
            if (!(v & 1) && (k.usb_mods & 1)) {
                k.e1 = 0;
                k.mods &= ~1u;
                a1 = true;
            }
            if (!(v & 4) && (k.usb_mods & 4)) {
                k.mods &= ~4u;
                a1 = true;
            }
        }
        k.usb_mods = v;
        if (a1) kb_relabel(w, k.mods);
    }
    kb_sync(w);
}

void GuiRuntime::kb_sync(Widget* w) {  // 0032afe0
    static constexpr std::array<std::int32_t, 4> ids = {0x3ea, 0x408, 0x39, 0x3ec};
    auto& k = *w->kb;
    for (std::size_t s = 0; s < ids.size(); ++s) {
        Widget* key = kb_find_id(w, ids[s]);
        if (!key) continue;
        // t0 points at a word the decorator's IE query (00416d10) fills;
        // without an IE it stays 0 and the IE-name compares are skipped.
        send(w, key, 4, 0, 0);
        const std::uint32_t flag = s <= 1 ? (k.mods & 1) : s == 2 ? (k.mods & 2) : (k.mods & 4);
        if (canvas_.focus == key) send(w, key, 6, 0x4b8cb8, 0);  // "onGainFocus"
        else if (flag) send(w, key, 0xc, 0xd, 0);
        else send(w, key, 6, 0x4b8cc8, 0);  // "onLoseFocus"
    }
}

void GuiRuntime::kb_wake(Widget* w) {  // 0032ca00
    base_wake(w);
    auto& k = *w->kb;
    k.key = 0;
    k.e1 = 0;
    k.usb_mods = 0;
    kb_relabel(w, 0);
    if (k.keys.empty()) unhosted(0x32ca40, "keyboard wake without keys");
    kb_focus(w, k.keys[static_cast<std::size_t>(k.key)]);
    repeat_mask |= 0x40u;
    if (!k.edit) unhosted(0x32cae8, "keyboard wake without an edit box");
    if (k.edit->obj) k.edit->obj->w54 = 0x3e4ccccdu;  // min(max(0.2, 0), 1)
}

void GuiRuntime::kb_sleep(Widget* w) {  // 0032c9a0
    base_sleep(w);
    repeat_mask &= ~0x40u;
    send(w, &canvas_, 0x10, 3, 0);
}

void GuiRuntime::kb_open(Widget* kb, const std::optional<std::string>& title, const std::optional<std::string>& text,
                         Widget* target, std::int32_t max, std::int32_t arg, std::uint8_t e0) {  // 0032b4f0
    auto& k = *kb->kb;
    if (k.label) set_text(k.label, title.value_or(kbd::relabel_empty), false);
    if (k.edit) {
        set_text(k.edit, text.value_or(kbd::relabel_empty), false);
        edit_cursor(k.edit, -1);
    }
    k.target = target;
    k.arg = arg;
    k.max = max;
    k.e0 = e0;
    push(kb, 0x64);
}

void GuiRuntime::kb_close(Widget* kb, bool accept) {  // 0032b2d0
    auto& k = *kb->kb;
    k.target = msgbox_target(k.target);  // 0032d750: the same root / canvas check as 002a8800
    switch_screens(kb, nullptr);
    if (accept) {
        std::string s = k.edit ? k.edit->text.substr(0, k.edit->text.find('\0')) : std::string();
        std::size_t b = 0;
        while (b < s.size() && static_cast<std::uint8_t>(s[b]) < 0x21) ++b;
        s.erase(0, b);
        while (!s.empty() && static_cast<std::uint8_t>(s.back()) < 0x21) s.pop_back();
        k.result = s;
        send(kb, k.target, 0x1a, k.arg, 0);  // t0: the trimmed copy (k.result), freed afterwards
    } else {
        k.result.reset();
        send(kb, k.target, 0x1b, k.arg, 0);
    }
}

// ---------------------------------------------------------------- in-game start menu (checkpoint 132f)

namespace {

std::string clock_text(std::int32_t v) {  // "%.2d:%.2d" of (v / 60, v % 60) (the 0x88888889 divide)
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2d:%.2d", v / 60, v % 60);
    return buf;
}

}  // namespace

// The mission clock block shared by wake 00368760 and update 00368620.
void GuiRuntime::ob_clock(Widget* w) {
    auto& o = *w->ob;
    if (!o.value_time) return;
    if (!o.b89) {
        // 003687a0 (checkpoint 132f11, the debrief's GuiEndGameObjectives): the agent's level time +d4
        // (001314f0(G, -1)), coloured 00587d00 when the level's +b71 is set, else 00587d10 (0041c070 on
        // +70 and +74), then the text.
        const std::int32_t cp = host_.current_profile();
        const float t = std::bit_cast<float>(host_.profile_word(cp, 0xd4));
        const std::string text = clock_text(cvt_w(t, 0x3687bc));
        static constexpr std::array<std::uint32_t, 4> c1 = {0x3e28a8a9u, 0x3f60e0e1u, 0u, 0x3f800000u};  // 00587d00
        static constexpr std::array<std::uint32_t, 4> c2 = {0x3f60e0e1u, 0x3e28a8a9u, 0x3e28a8a9u, 0x3f800000u};  // 00587d10
        const auto& c = host_.profile_byte(cp, 0xb71) ? c1 : c2;
        const std::array<float, 4> rgba = {std::bit_cast<float>(c[0]), std::bit_cast<float>(c[1]),
                                           std::bit_cast<float>(c[2]), std::bit_cast<float>(c[3])};
        set_rgba(o.value_time, rgba);
        if (!o.label_time) unhosted(0x368844, "GuiEndGameObjectives without \"label_time\" (0041c070 on a null +74)");
        set_rgba(o.label_time, rgba);
        set_text(o.value_time, text, false);
        return;
    }
    const float t = host_.mission_time();  // 00245890(*004ffc2c)
    set_text(o.value_time, clock_text(cvt_w(t, 0x3687bc)), false);
}

void GuiRuntime::ob_init(Widget* w) {  // 003688c0
    auto& o = *w->ob;
    base_init(w);
    o.groups[0] = find(w, "current_parameters_grp");
    o.groups[1] = find(w, "complete_failed_grp");
    if (o.groups[1]) {
        for (int i = 0; i < 2; ++i) {
            Widget* b = find(w, i == 0 ? "01" : "02");
            o.tabs[i] = b && b->cls == WidgetClass::button ? b : nullptr;  // isA "GuiButtonWidget"
        }
        for (Widget* b : o.tabs)
            if (b) b->flags &= ~flag_focusable;
    } else {
        o.tabs = {};
    }
    o.failed_label = find(w, "label_failed_obj");
    for (int i = 0; i < 4; ++i) {
        Widget* l = find(w, "objective_list" + std::to_string(i));
        if (l && l->cls != WidgetClass::textlist) l = nullptr;  // isA "GuiTextListWidget"
        o.lists[static_cast<std::size_t>(i)] = l;
        if (!l) continue;
        list_set_font(l, std::string("callout"));
        if (i == 0) {
            Widget* s = find(w, "select_config_select");
            if (s) s->flags &= ~flag_visible;
            // 00587ce0 (static init): the selected objective list's colour.
            l->list->rgba = {std::bit_cast<float>(0x3ec0c0c1u), std::bit_cast<float>(0x3efcfcfdu),
                             std::bit_cast<float>(0x3f048485u), 1.0f};
            l->list->cursor = s;
        } else {
            l->list->rgba = {0.8f, 0.8f, 0.8f, 1.0f};  // 00587cc0
        }
        l->flags &= ~flag_focusable;
        array_set_row_height(l, l->list->row_h - 3u);  // - *00492f60
    }
    if (Widget* l3 = o.lists[3]) {
        o.y78 = cvt_w(l3->rect[1], 0x368b88);
        o.h7c = cvt_w(l3->rect[3], 0x368ba0);
        if (o.failed_label)
            o.off84 = cvt_w(fsub(o.failed_label->rect[1], static_cast<float>(o.y78)), 0x368bcc);
    }
    auto text_w = [&](const char* name) {
        Widget* t = find(w, name);
        return t && t->cls == WidgetClass::text ? t : nullptr;  // isA "GuiTextWidget" (exact)
    };
    o.value_time = text_w("value_time");
    o.label_time = text_w("label_time");
    if (Widget* par = text_w("value_par")) {
        const std::int32_t v = host_.par_time();  // 00330c30(004ffc90, byte 005721c8)
        set_text(par, v < 0 ? std::string("--:--") : clock_text(v), false);
    }
    if (Widget* lp = text_w("label_par")) {
        if (const auto s = host_.get_string(g.b_21c8 ? "ParTeam" : "ParSolo")) {
            set_text(lp, *s, false);
            lp->b75 = 1;  // 0041c0f0
            if (lp->obj) lp->obj->b58 = 1;
        }
    }
}

void GuiRuntime::ob_wake(Widget* w) {  // 00368760
    base_wake(w);
    ob_clock(w);
    ob_fill(w);
    ob_tab(w, 0, w->ob->groups[1] == nullptr);
}

void GuiRuntime::ob_update(Widget* w, float dt) {  // 00368620
    base_update(w, dt);
    ob_fill(w);
    ob_clock(w);
}

void GuiRuntime::ob_fill(Widget* w) {  // 00367d60
    auto& o = *w->ob;
    if (!host_.objective_manager()) return;  // 00225cd0
    if (Widget* vc = find(w, "value_completed"); vc && vc->cls == WidgetClass::text) {  // isA "GuiTextWidget"
        const auto done = host_.objective_rows(2, 0x14, 1);
        char buf[48];
        std::snprintf(buf, sizeof buf, "%d / %d", static_cast<int>(done.size()), host_.objective_total());
        set_text(vc, buf, false);
    }
    const std::int32_t keep = o.sel_id;
    for (int i = 0; i < 4; ++i) {
        Widget* l = o.lists[static_cast<std::size_t>(i)];
        if (!l) continue;
        const auto rows = host_.objective_rows(i, 0x20, i == 2 ? o.b88 : 0);
        list_clear(l);
        for (const auto& r : rows)
            if (r.text) list_add(l, r.text, r.id, 0);
    }
    if (o.lists[0] && keep != -1) {
        const std::int32_t idx = list_find_id(o.lists[0], keep);
        if (idx != -1) array_select(o.lists[0], 0, static_cast<std::uint32_t>(idx));
    }
    if (Widget* l3 = o.lists[3]) {
        auto& l = *l3->list;
        const auto n = static_cast<std::uint32_t>(l.items.size());
        const std::int32_t rows = cvt_w(fdiv(l3->rect[3], u2f(l.row_h, 0x367f9c)), 0x367fa8);
        if (static_cast<std::uint32_t>(rows) != n) {
            std::int32_t h = static_cast<std::int32_t>(n * l.row_h);
            std::int32_t y = o.y78 + (h - o.h7c);
            if (h < o.h7c) {
                h = o.h7c;
                y = o.y78;
            }
            set_pos(l3, l3->rect[0], static_cast<float>(y));
            l3->rect[3] = static_cast<float>(h);
            if (o.failed_label)
                set_pos(o.failed_label, o.failed_label->rect[0], static_cast<float>(y + o.off84));
        }
        array_set_visible(l3, 1, n);
    }
}

void GuiRuntime::ob_tab(Widget* w, std::int32_t tab, bool a2) {  // 00367b20
    auto& o = *w->ob;
    if (!o.groups[static_cast<std::size_t>(tab)]) return;
    for (std::int32_t i = 0; i < 2; ++i) {
        const bool on = i == tab;
        if (Widget* gr = o.groups[static_cast<std::size_t>(i)]) {
            gr->flags = on ? (gr->flags | flag_enabled) : (gr->flags & ~flag_enabled);
            gr->flags = on ? (gr->flags | flag_visible) : (gr->flags & ~flag_visible);
        }
        if (Widget* b = o.tabs[static_cast<std::size_t>(i)])
            send(w, b, on ? 6u : 0xbu, on ? 0x4bbc58 : 0, 0);  // 004bbc58 "onGainFocus"
    }
    Widget* l0 = o.lists[0];
    if (l0 && l0->list->cursor) {
        if (a2) {
            array_select(l0, 0, 0);
            l0->flags &= ~flag_focusable;
        } else {
            l0->flags |= flag_focusable;
            set_focus(l0);
        }
        Widget* c = l0->list->cursor;
        if (!a2 && !l0->list->items.empty()) c->flags |= flag_visible;
        else c->flags &= ~flag_visible;
    }
    if (!o.b88 && o.tabs[1]) {
        if (tab == 1) {
            o.tabs[1]->flags |= flag_focusable;
            set_focus(o.tabs[1]);
        } else {
            o.tabs[1]->flags &= ~flag_focusable;
        }
    }
    o.tab = tab;
}

std::uint8_t GuiRuntime::ob_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                  std::int32_t t0) {  // 003680a0
    auto& o = *w->ob;
    const std::uint32_t t = type & 0xffff;
    if (t == 0x10 && a3 == 1 && sender != o.lists[0]) return 1;
    if (sender && sender == o.lists[0]) {
        const std::int32_t id = list_id(o.lists[0], t0);
        if (t == 0x14) {
            o.sel_id = id;
            return 1;
        }
        if (t == 0x13) {
            if (id == -1) return 1;
            host_.objective_select(id);
            return 1;
        }
    }
    return base_event(w, sender, type, a3, t0);
}

std::uint8_t GuiRuntime::ob_input(Widget* w, const PadEvent& ev) {  // 00368300
    auto& o = *w->ob;
    auto pressed = [&](std::uint32_t m) { return ev.kind == 1 && ev.port == 0 && ev.mask == m && ev.released == 0; };
    if (pressed(0x2000)) {
        if (o.tab == 0 && o.groups[1]) {
            ob_tab(w, 1, false);
            send(o.tabs[1], &canvas_, 0x10, 0, 0);
        }
        return 1;
    }
    if (pressed(0x8000)) {
        if (o.tab == 1 && o.groups[0]) {
            ob_tab(w, 0, false);
            send(o.tabs[0], &canvas_, 0x10, 0, 0);
        }
        return 1;
    }
    if (pressed(pad_triangle)) {
        sm_back(w);
        return 1;
    }
    if (pressed(0x800)) {
        if (!g.controller) unhosted(0x368518, "start press without the HUD (*004ffc2c)");
        if (g.controller_6a8) {
            sm_exit(w);
            return 1;
        }
    }
    if (pressed(pad_down) || pressed(pad_up))
        return canvas_.focus == o.tabs[0] || canvas_.focus == o.tabs[1] ? 1 : 0;
    return 0;
}

void GuiRuntime::sm_label(Widget* w, std::int32_t idx) {  // label_to_map ("StartMenuOpt%d" or "")
    Widget* l = find(w, "label_to_map");
    if (!l || l->cls != WidgetClass::text) return;  // isA "GuiTextWidget"
    const auto s = host_.get_string("StartMenuOpt" + std::to_string(idx));
    set_text(l, s ? *s : std::string(), false);
}

void GuiRuntime::sm_init(Widget* w) {  // 00456bf0
    auto& o = *w->ob;
    o.button_grp = find(w, "ingame_button_grp");
    if (o.button_grp) {
        std::size_t i = 0;
        for (Widget* c : o.button_grp->children) {
            // Port-only (checkpoint 132win11): the Port Settings button "09" is entry 8 wherever it sits (the
            // Cheats button "08" may be absent).
            if (c->name == "09") {
                o.buttons[8] = c;
                c->id10 = 8;
                continue;
            }
            if (i >= o.buttons.size() - 1) break;
            o.buttons[i] = c;
            c->id10 = static_cast<std::int32_t>(i);
            ++i;
        }
    }
    o.paused = find(w, "label_paused");
    ob_init(w);
    o.option = 0;
    sm_label(w, 0);
}

void GuiRuntime::sm_wake(Widget* w) {  // 00456ab0
    auto& o = *w->ob;
    ob_wake(w);
    o.option = 0;
    sm_label(w, 0);
    if (w->id10 != -1) set_focus(o.buttons[static_cast<std::size_t>(w->id10)]);
    if (o.paused) {
        if (g.b_bb7) o.paused->flags |= flag_visible;
        else o.paused->flags &= ~flag_visible;
    }
}

void GuiRuntime::sm_sleep(Widget* w) {  // 00456a30
    base_sleep(w);
    w->id10 = -1;
}

Widget* GuiRuntime::sm_first(Widget* w) {  // 00456a60
    if (w->id10 != -1) {
        Widget* b = w->ob->buttons[static_cast<std::size_t>(w->id10)];
        w->id10 = -1;
        return b;
    }
    return v_first_base(w);
}

std::uint8_t GuiRuntime::sm_input(Widget* w, const PadEvent& ev) {  // 004566d0
    auto pressed = [&](std::uint32_t m) { return ev.kind == 1 && ev.port == 0 && ev.mask == m && ev.released == 0; };
    if (pressed(pad_triangle)) {
        if (!canvas_.focus) unhosted(0x456744, "triangle with no canvas focus (null +1c read)");
        if (canvas_.focus->parent == w->ob->button_grp) {
            hud_close();
            if (g.b_bb7) host_.app_131470(0);
            return 1;
        }
    }
    if (pressed(0x800)) {
        hud_close();
        if (g.b_bb7) host_.app_131470(0);
        return 1;
    }
    return ob_input(w, ev);
}

std::uint8_t GuiRuntime::sm_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                  std::int32_t t0) {  // 00456830
    const std::uint32_t t = type & 0xffff;
    if (t == 0 && sender->id10 != -1) {
        w->ob->option = sender->id10;
        sm_label(w, sender->id10);
        return 1;
    }
    if (t == 2) {
        const std::int32_t idx = canvas_.focus ? canvas_.focus->id10 : -1;
        if (idx != -1) {
            sm_select(w, idx);
            return 1;
        }
    }
    if (t == 0x1a && a3 == 0) {
        if (g.app_state != 0x13 && g.app_state != 0x14) {
            hud_close();
            host_.app_12fa60(2);
            host_.app_131470(0);
        }
        return 1;
    }
    return ob_event(w, sender, type, a3, t0);
}

void GuiRuntime::sm_select(Widget* w, std::int32_t idx) {  // 004562e0
    Widget* s = nullptr;
    auto backdrop_hide = [&](const char* a, const char* b) {
        Widget* bd = lookup("GuiStartMenuBackdrop");
        if (!bd) return;
        Widget* x = find(bd, a);
        Widget* y = find(bd, b);
        if (x) x->flags &= ~flag_visible;
        if (y) y->flags &= ~flag_visible;
    };
    switch (idx) {
    case 0: s = factory("GuiObjectives", std::string("OBJECTIV")); break;
    case 1: s = factory("GuiRadioLog", std::string("RADIOLOG")); break;
    case 2:
        s = factory("GuiBriefingPane", std::string("BRIEFING"));
        backdrop_hide("buttonx", "select");
        break;
    case 3: s = factory("GuiEquipmentPane", std::string("EQUIPNT")); break;
    case 4:
        s = factory("GuiScoresEquip", std::string(g.b_21c8 ? "SCORES" : "SCOREONE"));
        backdrop_hide("buttondpad", "navigate");
        break;
    case 5: s = factory("GuiGameOptions", std::string("GAMEOPT2")); break;
    case 7:  // port-only (checkpoint 132f5): the Cheats screen
        if (host_.port_cheat_menu()) s = factory("PortGuiCheats", std::string("PORTCHEATS"));
        break;
    case 8:  // port-only (checkpoint 132win11): the Port Settings screen
        if (host_.port_settings_screen()) s = factory("PortGuiSettings", std::string("PORTSETTINGS"));
        break;
    case 6:
        if (g.app_state != 0x13 && g.app_state != 0x14) {
            const auto title = host_.get_string("QuitTitle");
            const auto msg = host_.get_string("QuitMessage");
            if (title && msg) popup_open(5, title, msg, w, 0);
        }
        break;
    default: break;
    }
    if (!s) return;
    s->id10 = idx;
    s->flags |= flag_focusable;
    const std::int16_t layer = w->layer;
    pop(w);
    push(s, layer);
}

void GuiRuntime::sm_back(Widget* screen) {  // 00456160
    Widget* s = factory("GuiStartMenu", std::string("INGAMENU"));
    if (!s) return;
    s->id10 = screen->id10;
    const std::int16_t layer = screen->layer;
    pop(screen);
    push(s, layer);
    if (Widget* bd = lookup("GuiStartMenuBackdrop")) {
        for (const char* n : {"buttonx", "select"})
            if (Widget* x = find(bd, n)) x->flags |= flag_visible;
    }
    if (Widget* bd = lookup("GuiStartMenuBackdrop")) {
        for (const char* n : {"buttondpad", "navigate"})
            if (Widget* x = find(bd, n)) x->flags |= flag_visible;
    }
}

void GuiRuntime::sm_exit(Widget* screen) {  // 00456100
    pop(screen);
    hud_close();
    host_.app_131470(0);
}

void GuiRuntime::bd_init(Widget* w) {  // 00456f90
    base_init(w);
    Widget* n = find(w, "value_agentname");
    if (n && n->cls == WidgetClass::text) {
        const auto s = host_.profile_name(host_.current_profile());  // 00336170(001314f0(004ffb50, -1))
        set_text(n, s ? *s : std::string(), false);
    }
}

void GuiRuntime::bd_wake(Widget* w) {  // 00456ed0
    if (!g.b_21c8) host_.sound_pause(1);
    host_.ticker_show(false);
    base_wake(w);
}

void GuiRuntime::bd_sleep(Widget* w) {  // 00456e60
    if (!g.b_21c8) host_.sound_pause(0);
    host_.ticker_show(true);
    base_sleep(w);
}

// ---------------------------------------------------------------- GuiMapScreen (vtable 004e18c0, checkpoint 132hud)
// MAPSCRN.GUI over the HUD map (hud + 6a0, 0026xxxx). The world keeps running (no 00131470).

void GuiRuntime::mp_init(Widget* w) {  // 00464b70
    base_init(w);  // 0041f150
    w->map_backdark = find(w, "backdark");
}

void GuiRuntime::mp_wake(Widget* w) {  // 00464b30
    base_wake(w);  // 0041efa0
    w->map_6a0 = true;  // +4c = *(*004ffc2c + 6a0)
    host_.map_open();  // 0026dbc0(+4c)
}

void GuiRuntime::mp_sleep(Widget* w) {  // 00464b00
    base_sleep(w);  // 0041ebf0
    host_.map_close();  // 0026d720(+4c)
    w->map_6a0 = false;
}

std::uint8_t GuiRuntime::mp_input(Widget* w, const PadEvent& ev) {  // 004648c0
    if (ev.kind == 1 && ev.port == 0 && ev.released == 0) {
        switch (ev.mask) {
        case 4:  // L1: 0026d710, 0026c180; a change sends 0x10 (a3 0) to the canvas
            host_.map_hold();
            if (host_.map_prev()) send(w, &canvas_, 0x10, 0, 0);
            return 1;
        case 8:  // R1: 0026d710, 0026c3b0
            host_.map_hold();
            if (host_.map_next()) send(w, &canvas_, 0x10, 0, 0);
            return 1;
        case 0x100:  // Select: 0026c130
            host_.map_reset();
            return 1;
        case pad_triangle:  // 002427a0(*004ffc2c)
            map_toggle();
            return 1;
        default: return 0;
        }
    }
    if (ev.kind == 1 && ev.port == 0 && ev.released == 1 && (ev.mask == 4 || ev.mask == 8)) {
        host_.map_release();  // 0026d6d0
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- GuiSliderWidget (vtable 004e10c0)
// Wake / sleep / update / event are the GuiWidget entries; the value is
// shown by broadcasting event 0xa (the decorators' frame) to the children.

void GuiRuntime::sl_init(Widget* w) {  // 0043f850
    base_init(w);
    static constexpr std::int32_t names[2] = {0x4c01d0, 0x4c01e0};  // 004976c0: "onActivate", "onDeactivate"
    auto& s = *w->sl;
    for (std::size_t i = 0; i < 2; ++i) {
        deco_record_ = nullptr;
        if (send(w, w, 4, names[i], 0) && deco_record_) {  // 0041e0f0(w, w, 4, name, &out)
            s.has[i] = 1;
            s.a[i] = deco_record_->a;  // record +10
            s.b[i] = deco_record_->b;  // record +14
        }
    }
    deco_record_ = nullptr;
}

void GuiRuntime::sl_frame(Widget* w) {
    auto& s = *w->sl;
    const auto k = static_cast<std::size_t>(s.state);
    if (!s.has[k]) return;
    const float f = fdiv(static_cast<float>(s.v - s.min), static_cast<float>(s.max - s.min));
    const std::int32_t frame = s.a[k] + cvt_w(fmul(f, static_cast<float>(s.b[k] - s.a[k])), 0x43f1d8);
    broadcast(w, 0xa, frame, 0);  // 0041df00
}

void GuiRuntime::sl_set(Widget* w, std::uint8_t orient, std::int32_t v, std::int32_t min, std::int32_t max,
                        std::int32_t step) {  // 0043f150
    auto& s = *w->sl;
    s.orient = orient;
    s.v = v;
    s.min = min;
    s.max = max;
    s.step = step;
    sl_frame(w);
}

void GuiRuntime::sl_gain(Widget* w) {  // 0043f7a0
    bubble(w, 0);  // 0041e330
    w->sl->state = 0;
    sl_frame(w);
}

void GuiRuntime::sl_lose(Widget* w) {  // 0043f6f0
    bubble(w, 1);  // 0041e2b0
    w->sl->state = 1;
    sl_frame(w);
}

std::uint8_t GuiRuntime::sl_input(Widget* w, const PadEvent& ev) {  // 0043f200
    auto& s = *w->sl;
    auto pressed = [&](std::uint32_t m) { return ev.kind == 1 && ev.port == 0 && ev.mask == m && ev.released == 0; };
    const std::uint32_t dec = s.orient == 1 ? pad_up : 0x8000u;    // Up / Left
    const std::uint32_t inc = s.orient == 1 ? pad_down : 0x2000u;  // Down / Right
    std::int32_t d = 0;
    if (pressed(dec)) d = -s.step;
    else if (pressed(inc)) d = s.step;
    else return 0;
    const std::int32_t n = s.v + d;
    s.v = n < s.min ? s.min : (s.max < n ? s.max : n);
    if (s.has[static_cast<std::size_t>(s.state)]) {
        sl_frame(w);
        send_up(w, 0x1d, s.v, 0);  // 0041e040
    }
    return 1;
}

// ---------------------------------------------------------------- GuiScrollBarWidget (vtable 004e0f40)
// Draw / sleep / input / update / event are the GuiWidget entries. The
// position is shown by broadcasting a frame of the widget's "onWake" record
// (0xa on wake and a range change, 0xb on a value change).

void GuiRuntime::sb_init(Widget* w) {  // 0043be70
    base_init(w);
    auto& b = *w->sb;
    deco_record_ = nullptr;
    if (send(w, w, 4, 0x4bffa8, 0) && deco_record_) {  // 0041e0f0(w, w, 4, *004976a0 "onWake", &out)
        b.has = 1;
        b.a = deco_record_->a;  // record +10
        b.b = deco_record_->b;  // record +14
    }
    deco_record_ = nullptr;
}

void GuiRuntime::sb_frame(Widget* w, std::uint32_t type) {
    const auto& b = *w->sb;
    if (!b.has) return;
    const float f = fdiv(static_cast<float>(b.v - b.min), static_cast<float>(b.max - b.min));
    const std::int32_t d = cvt_w(fmul(f, static_cast<float>(b.b - b.a)), 0x43bd24);
    broadcast(w, type, static_cast<std::int32_t>(static_cast<std::uint32_t>(b.a) + static_cast<std::uint32_t>(d)),
              0);  // 0041df00
}

void GuiRuntime::sb_set_value(Widget* w, std::int32_t v) {  // 0043bbc0
    auto& b = *w->sb;
    b.v = v < b.min ? b.min : (b.max < v ? b.max : v);
    sb_frame(w, 0xb);
}

void GuiRuntime::sb_set_range(Widget* w, std::uint8_t b54, std::int32_t v, std::int32_t x,
                              std::int32_t y) {  // 0043bc70
    auto& b = *w->sb;
    b.b54 = b54;
    b.min = y < x ? y : x;
    b.max = y < x ? x : y;
    b.v = v < b.min ? b.min : (b.max < v ? b.max : v);
    sb_frame(w, 0xa);
}

// ---------------------------------------------------------------- GuiMLTextWidget (vtable 004e1040)
// Gain / lose / input / update / event are the GuiWidget entries; the draw
// 0043d8b0 is GuiDrawer::draw_ml_text.

void GuiRuntime::ml_init(Widget* w) {  // 0043e330
    base_init(w);
    auto& m = *w->ml;
    m.font = host_.default_font();  // 00418420(00414790(), NULL)
    TextObject o;  // 003d0940(o, 0, 0, w, h, +4c, 004c0100 = (1, 1, 1, 1))
    o.rect = {0, 0, cvt_w(w->rect[2], 0x43e3f4), cvt_w(w->rect[3], 0x43e3f8)};
    o.font = m.font;
    o.rgba = {1.0f, 1.0f, 1.0f, 1.0f};
    host_.text_object(o);
    o.b10 = 0;
    o.b58 = 1;
    m.obj = o;
    const std::uint8_t fh = host_.font_height(m.font);  // 0036a2f0
    if (fh == 0) unhosted(0x43e444, "line count of a font with no height (div by zero)");
    m.max_lines = cvt_w(w->rect[3], 0x43e438) / fh;
    m.per_page = m.max_lines;
    Widget* sb = find_sub(w, "vscrollbar");  // *004976b8
    m.scroll = sb && sb->cls == WidgetClass::scrollbar ? sb : nullptr;  // isA "GuiScrollBarWidget"
}

void GuiRuntime::ml_set_font(Widget* w, const std::string& name) {  // 0043e200
    auto& m = *w->ml;
    m.font = host_.font(name);  // 00418420(00414790(), name)
    if (m.obj) m.obj->font = m.font;  // the text object +20
    const std::uint8_t fh = host_.font_height(m.font);  // 0036a2f0
    if (fh == 0) unhosted(0x43e258, "line count of a font with no height (div by zero)");
    m.max_lines = cvt_w(w->rect[3], 0x43e24c) / fh;
    m.per_page = m.max_lines;
}

void GuiRuntime::ml_sync(Widget* w) {  // 0043e290
    const auto& m = *w->ml;
    if (m.scroll)
        sb_set_range(m.scroll, 0, m.top, 0,
                     static_cast<std::int32_t>(m.lines.size()) - m.per_page);
}

void GuiRuntime::ml_down(Widget* w, bool page) {  // 0043d760
    auto& m = *w->ml;
    const auto count = static_cast<std::int32_t>(m.lines.size());
    if (!(m.top < count - m.per_page)) return;
    m.top += page ? m.per_page : 1;
    if (!(m.top < count - m.per_page)) {
        m.top = count - m.per_page;
        if (m.top < 0) m.top = 0;
    }
    ml_sync(w);
}

void GuiRuntime::ml_up(Widget* w, bool page) {  // 0043d820
    auto& m = *w->ml;
    if (m.top == 0) return;
    m.top -= page ? m.per_page : 1;
    if (m.top < 0) m.top = 0;
    ml_sync(w);
}

void GuiRuntime::ml_clear(Widget* w) {  // 0043db80
    auto& m = *w->ml;
    m.lines.clear();  // 0036b130 of each node's +8, then the list erase 00138c60
    m.top = 0;
    ml_sync(w);
}

void GuiRuntime::ml_set_text(Widget* w, const std::string& text) {  // 0043dce0
    auto& m = *w->ml;
    const std::string s = text.substr(0, text.find('\0'));
    if (s.empty()) return;
    const std::int32_t limit = cvt_w(w->rect[2], 0x43dd2c);
    std::ptrdiff_t start = 0, cur = 0, sp = 0;
    bool has_sp = false;  // sp != NULL
    const auto n = static_cast<std::ptrdiff_t>(s.size());
    while (cur < n) {
        const char c = s[static_cast<std::size_t>(cur)];
        if (c != '\n' &&
            host_.font_width(m.font, s.substr(static_cast<std::size_t>(start)),
                             static_cast<std::int32_t>(cur - start + 1)) < limit) {  // 0036aa50
            if (c == ' ') {
                sp = cur;
                has_sp = true;
            }
            ++cur;
            continue;
        }
        if (c == '\n') {
            sp = cur;
            ++cur;
        } else {
            // No space on the line: it ends before the previous character,
            // which is dropped.
            if (!has_sp) sp = cur - 1;
            if (!(start < sp)) return;  // no trim, no scroll bar update
        }
        const std::ptrdiff_t len = sp - start;
        std::string line(static_cast<std::size_t>(len + 1), '\0');  // 0036b180(len + 2): prefix, bytes, NUL
        line[0] = static_cast<char>(m.code + 0x10);
        s.copy(line.data() + 1, static_cast<std::size_t>(len), static_cast<std::size_t>(start));  // 00129a70
        for (std::size_t i = 0; i < line.size(); ++i) {
            const auto b = static_cast<std::int8_t>(line[i]);
            if (b >= 0x10 && b < 0x18) m.code = b - 0x10;
        }
        m.lines.push_back(std::move(line));  // 0043e0b0
        start = sp + 1;
        has_sp = false;
    }
    if (start < cur)  // the tail: no colour prefix, no code scan
        m.lines.push_back(s.substr(static_cast<std::size_t>(start), static_cast<std::size_t>(cur - start)));
    while (m.max_lines != 0 && m.max_lines < static_cast<std::int32_t>(m.lines.size()))
        m.lines.erase(m.lines.begin());  // 0036b130 of the first node's +8, 00138da0
    ml_sync(w);
}

// ---------------------------------------------------------------- GuiBriefingPane (vtable 004e13d0)
// Sleep / update / event are the GuiWidget entries.

void GuiRuntime::br_init(Widget* w) {  // 00456020
    base_init(w);
    Widget* ml = find(w, "value_briefing");  // *00497980
    ml = ml && ml->cls == WidgetClass::mltext ? ml : nullptr;  // isA "GuiMLTextWidget"
    w->briefing_text = ml;
    if (!ml) return;
    ml->ml->max_lines = 0;
    Widget* sb = find(w, "vscroll");  // *00497988
    ml->ml->scroll = sb && sb->cls == WidgetClass::scrollbar ? sb : nullptr;  // isA "GuiScrollBarWidget"
    ml->flags |= flag_focusable;
}

void GuiRuntime::br_wake(Widget* w) {  // 00455cb0
    base_wake(w);
    if (Widget* ml = w->briefing_text) {
        const auto t = host_.get_string("level_briefing");  // 004c1aa8
        ml_clear(ml);
        ml_set_text(ml, t ? *t : std::string());  // NULL: "" (004c1a38)
    }
    Widget* h = find(w, "label_briefingheader");  // 004c1ac0
    if (h && h->cls == WidgetClass::text) {      // isA "GuiTextWidget" (004a7520)
        const auto t = host_.get_string(g.level_name);  // 004ffc90
        set_text(h, t ? *t : g.level_name, false);
    }
}

std::uint8_t GuiRuntime::br_input(Widget* w, const PadEvent& ev) {  // 00455e10
    auto pressed = [&](std::uint32_t m) { return ev.kind == 1 && ev.port == 0 && ev.mask == m && ev.released == 0; };
    if (pressed(pad_up) && w->briefing_text) {
        ml_up(w->briefing_text, false);
        return 1;
    }
    if (pressed(pad_down) && w->briefing_text) {
        ml_down(w->briefing_text, false);
        return 1;
    }
    if (pressed(pad_triangle)) {
        sm_back(w);
        return 1;
    }
    if (pressed(0x800)) {
        sm_exit(w);  // the manager's +68 pop, 00242900(*004ffc2c), 00131470(G, 0)
        return 1;
    }
    return 0;
}

std::array<float, 4> GuiRuntime::br_text_colour(std::uint16_t flags) {
    // 00587d20 (0.8, 0.8, 0.8, 1) when enabled, else 00587d30 (the boot state's words).
    if (flags & flag_enabled) return {0.8f, 0.8f, 0.8f, 1.0f};
    return {std::bit_cast<float>(0x3ebebebfu), std::bit_cast<float>(0x3efcfcfdu), std::bit_cast<float>(0x3f048485u),
            1.0f};
}

// ---------------------------------------------------------------- GuiRadioLog (vtable 004e1290)
// Wake / gain / lose / draw are the GuiWidget entries. vt +18 (00454880:
// 0041f070 then the stop below) has no caller in the ported runtime.

void GuiRuntime::rl_init(Widget* w) {  // 00454910
    base_init(w);
    auto& r = *w->rl;
    Widget* l = find(w, "radio_log_list");  // 004c19f8
    r.list = l && l->cls == WidgetClass::textlist ? l : nullptr;  // isA "GuiTextListWidget"
    if (r.list) {
        auto& ld = *r.list->list;
        ld.wrap = 1;  // +81
        // +c0..+cc = the quad at 00587d40 (the boot state's words)
        ld.rgba_c0 = {std::bit_cast<float>(0x3ec0c0c1u), std::bit_cast<float>(0x3efcfcfdu),
                      std::bit_cast<float>(0x3f048485u), 1.0f};
        Widget* sb = find(w, "vscroll");  // 004c19b0
        ld.sb = sb && sb->cls == WidgetClass::scrollbar ? sb : nullptr;  // isA "GuiScrollBarWidget"
        ld.cursor = find(w, "select_config_select");  // 004c1a10, any class
        if (ld.cursor) ld.cursor->flags &= ~flag_focusable;
    }
    Widget* s = find(w, "sub_titles");  // 004c1a28
    r.sub = s && s->cls == WidgetClass::subtitle ? s : nullptr;  // isA "GuiSubTitleDisplay"
}

void GuiRuntime::rl_stop(Widget* w) {
    // 0020ecb0(msg, +50, 0); msg +20 = 1; 003c9420(msg, 004ffb50); then
    // 00459450(+4c) when the subtitle display is there.
    auto& r = *w->rl;
    host_.radio_sound(0, r.type, r.name, 0);
    if (r.sub) st_clear(r.sub);
}

void GuiRuntime::rl_sleep(Widget* w) {  // 00454a80
    base_sleep(w);
    rl_stop(w);
}

std::uint8_t GuiRuntime::rl_input(Widget* w, const PadEvent& ev) {  // 00454150
    auto pressed = [&](std::uint32_t m) { return ev.kind == 1 && ev.port == 0 && ev.mask == m && ev.released == 0; };
    if (pressed(pad_triangle)) {
        sm_back(w);
        return 1;
    }
    if (pressed(0x800)) {
        sm_exit(w);
        return 1;
    }
    return 0;
}

void GuiRuntime::rl_refill(Widget* w) {  // 00454280
    auto& r = *w->rl;
    if (!r.list) return;
    const std::uint32_t row = r.list->list->row;  // 00424410(+48, &col, &row)
    const std::int32_t n = host_.pda_count();  // *004ffc2c
    if (n < 0) return;
    list_clear(r.list);
    for (std::int32_t i = 0; i < n; ++i) {
        const auto rec = host_.pda_record(i);  // stride 0x21
        const auto s = host_.get_string(rec.text);  // 003fdb50(rec + 1)
        list_add(r.list, s ? *s : rec.text, 0, rec.flag ? 1 : 0);  // 00421290
    }
    if (row < r.list->list->items.size()) array_select(r.list, 0, row);
}

void GuiRuntime::rl_update(Widget* w, float dt) {  // 00454380
    base_update(w, dt);
    auto& r = *w->rl;
    if (r.list) {
        const std::int32_t n = host_.pda_count();
        if (n >= 0 && static_cast<std::size_t>(n) != r.list->list->items.size()) rl_refill(w);
    }
    if (!r.list || !r.list->list->cursor) return;
    Widget* c = r.list->list->cursor;
    if (!r.list->list->items.empty() && (w->flags & flag_enabled)) c->flags |= flag_visible;
    else c->flags &= ~flag_visible;
}

void GuiRuntime::rl_play(Widget* w, const std::string& name) {  // 004546c0
    auto& r = *w->rl;
    rl_stop(w);
    const auto s = host_.get_string(name);  // 003fdb50
    if (s && r.sub) st_set_text(r.sub, s);  // 00459500
    // 001ca010(+50, 4, name, 0, 1000): the in-place record (strcpy into the 16 bytes at +10).
    if (name.size() >= 16) unhosted(0x1ca0c4, "a radio log quip name of 16 or more characters ('" + name + "')");
    r.type = 4;
    r.name = name;
    host_.radio_sound(2, r.type, r.name, 1000);  // 0020f2a0; +20 = 1; 003c9420
}

std::uint8_t GuiRuntime::rl_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {
    // 00454440
    auto& r = *w->rl;
    switch (type & 0xffff) {
    case 0x14:  // the list's selection moved
        rl_stop(w);
        return 1;
    case 0x13: {  // Cross on row t0
        std::string buf;
        const std::int32_t n = host_.pda_count();
        if (n >= 0 && t0 < n) {
            buf = host_.pda_record(t0).name;  // 00129360(buf, rec + 11)
            host_.pda_mark(t0);               // rec +0 = 1
        }
        rl_play(w, buf);
        if (r.list && t0 < static_cast<std::int32_t>(r.list->list->items.size()))
            list_set_style(r.list, t0, 1);  // 004214c0
        return 1;
    }
    default: return base_event(w, sender, type, a3, t0);  // 0041ef10
    }
}

// ---------------------------------------------------------------- GuiSlideShow (vtable 004de710)
// The command centre's ZEUS files (ZEUSHELL.GUI, opened by 00296314): five
// categories of TDX pictures of ZEUS.HOG with their voice-overs. Deinit /
// gain / lose / update / event are the GuiWidget entries; the draw 00327910
// is GuiDrawer's.

void GuiRuntime::ss_init(Widget* w) {  // 00327fd0
    base_init(w);
    auto& s = *w->ss;
    Widget* b = find(w, "bitmap_frame");
    s.bitmap = b && b->cls == WidgetClass::bitmap ? b : nullptr;  // isA "GuiBitmapWidget"
    if (s.bitmap) s.bitmap->flags &= ~flag_visible;  // drawn by the slideshow's own draw
    Widget* t = find(w, "menutitle");
    s.title = is_text_w(t) ? t : nullptr;
    if (s.title) {
        s.title->b75 = 1;  // 0041c0f0
        if (s.title->obj) s.title->obj->b58 = 1;
    }
    Widget* v = find(w, "value_slot");
    s.value = is_text_w(v) ? v : nullptr;
    Widget* n = find(w, "label_new");
    s.label_new = is_text_w(n) ? n : nullptr;
}

std::int32_t GuiRuntime::ss_total(const Widget* w) const {
    std::int32_t n = 0;
    for (const auto& c : w->ss->cats) n += static_cast<std::int32_t>(c.slides.size());
    return n;
}

void GuiRuntime::ss_wake(Widget* w) {  // 00327d90
    base_wake(w);
    host_.options_frontend_wake();  // 0036b250(1); 0036b3b0(1, 0x100000, 0x800)
    host_.app_2c9af0(2);            // THEME_ZEUS
    w->ss->bd4 = 0;
    ss_refresh(w);
    const bool many = ss_total(w) >= 2;
    auto show = [&](Widget* x) {
        if (!x) return;
        if (many) x->flags |= flag_visible;
        else x->flags &= ~flag_visible;
    };
    show(w->ss->value);
    show(find(w, "navigate"));    // *00491560
    show(find(w, "buttondpad"));  // *00491568
}

void GuiRuntime::ss_sleep(Widget* w) {  // 00327cf0
    base_sleep(w);
    auto& s = *w->ss;
    s.bd4 = 0;
    host_.radio_sound(0, s.type, s.name, 0);  // 0020ecb0(msg, +90, 0); +20 = 1; 003c9420
    host_.options_frontend_sleep();           // 003eb750(003eba50()); 0036b250(1)
    s.cat = -1;
}

void GuiRuntime::ss_return(Widget* w, const std::string& cls, const std::string& file, std::int32_t arg) {
    auto& s = *w->ss;
    s.ret_class = cls;
    s.ret_file = file;
    s.ret_arg = arg;
}

void GuiRuntime::ss_clear(Widget* w) {  // 00328630
    w->ss->cats.clear();  // 0032a280
    w->ss->cat = -1;
}

std::int32_t GuiRuntime::ss_add_category(Widget* w, const std::optional<std::string>& name) {  // 00328310
    auto& s = *w->ss;
    s.cats.push_back({name ? cstr_of(*name) : std::string(), {}, 0});  // a null name: "" (004b8790)
    return static_cast<std::int32_t>(s.cats.size()) - 1;
}

std::int32_t GuiRuntime::ss_add_slide(Widget* w, std::int32_t cat, const std::string& image, std::uint32_t archive,
                                      const std::string& voice) {  // 00328140
    auto& s = *w->ss;
    if (cat < 0 || cat >= static_cast<std::int32_t>(s.cats.size())) return -1;
    Widget::SlideShow::Slide sl;
    sl.image = image;
    sl.archive = archive;
    sl.voice = voice;
    sl.unlocked = 0;  // +1c
    auto& slides = s.cats[static_cast<std::size_t>(cat)].slides;
    slides.push_back(sl);
    return static_cast<std::int32_t>(slides.size()) - 1;
}

void GuiRuntime::ss_load(Widget* w, std::int32_t level, std::int32_t cat, std::int32_t slide,
                         std::uint8_t only_unlocked) {  // 00326de0
    auto& s = *w->ss;
    s.cats.clear();  // 0032a280
    s.cat = -1;
    if (level <= 0 || level >= 0x13) return;
    const std::uint32_t archive = host_.zeus_archive();
    if (!archive) return;
    const std::int32_t agent = host_.current_profile();  // 001314f0(004ffb50, -1)
    for (std::int32_t c = 0; c < 5; ++c) {
        const auto name = host_.get_string("ZeusCategory" + std::to_string(c));
        s.cats.push_back({name ? cstr_of(*name) : std::string(), {}, 0});  // 00328270
        for (std::int32_t n = 1;; ++n) {
            const std::string stem = std::to_string(level) + "_" + std::to_string(c + 1) + "_" + std::to_string(n);
            const std::string file = stem + ".tdx";
            if (host_.zeus_lookup(archive, file) == -1) break;  // 0036d2a0
            auto& slides = s.cats[c].slides;  // 00328140
            if (host_.zeus_has(agent, level, c, n - 1)) {
                Widget::SlideShow::Slide sl;
                sl.image = file;
                sl.archive = archive;
                sl.voice = stem;
                sl.unlocked = 1;
                sl.level = level;
                sl.cat = c;
                sl.idx = n - 1;
                slides.push_back(sl);
            } else if (!only_unlocked) {
                Widget::SlideShow::Slide sl;
                sl.image = "0_0_0.tdx";
                sl.archive = archive;
                slides.push_back(sl);
            }
        }
    }
    if (cat < 0 || cat >= static_cast<std::int32_t>(s.cats.size()))
        unhosted(0x326fd4, "a ZEUS category index out of range (" + std::to_string(cat) + ")");
    s.cat = cat;
    s.cats[cat].current = slide;
}

std::uint8_t GuiRuntime::ss_step(Widget* w, bool forward) {  // 003272b0
    auto& s = *w->ss;
    const std::int32_t nc = static_cast<std::int32_t>(s.cats.size());
    bool any = false;
    for (std::int32_t i = 0; !any && i < nc; ++i) any = !s.cats[i].slides.empty();
    if (!any) return 0;
    if (s.cat < 0 || s.cat >= nc) s.cat = 0;
    auto count = [&](std::int32_t c) { return static_cast<std::int32_t>(s.cats[c].slides.size()); };
    std::int32_t cur = s.cats[s.cat].current;
    if (cur < 0 || cur >= count(s.cat)) cur = 0;
    const std::int32_t c0 = s.cat, s0 = cur;
    if (forward) {
        if (++cur >= count(s.cat)) {
            do s.cat = (s.cat + 1) % nc;
            while (count(s.cat) == 0);
            cur = 0;
        }
    } else {
        if (--cur < 0) {
            do s.cat = s.cat > 0 ? s.cat - 1 : nc - 1;
            while (count(s.cat) == 0);
            cur = count(s.cat) - 1;
        }
    }
    s.cats[s.cat].current = cur;
    ss_refresh(w);
    return !(s.cat == c0 && cur == s0);
}

void GuiRuntime::ss_refresh(Widget* w) {  // 00327890
    auto& s = *w->ss;
    if (s.cat >= 0 && s.cat < static_cast<std::int32_t>(s.cats.size()) && !s.cats[s.cat].slides.empty())
        ss_show(w, s.cat, s.cats[s.cat].current);
    else
        ss_step(w, true);
}

void GuiRuntime::ss_show(Widget* w, std::int32_t cat, std::int32_t slide) {  // 00327530
    auto& s = *w->ss;
    if (cat < 0 || cat >= static_cast<std::int32_t>(s.cats.size())) cat = 0;
    if (s.cats.empty() && slide >= 0) unhosted(0x327578, "a ZEUS slide shown without categories (reads a null vector)");
    const std::int32_t count =
        cat < static_cast<std::int32_t>(s.cats.size()) ? static_cast<std::int32_t>(s.cats[cat].slides.size()) : 0;
    if (slide < 0 || slide >= count) slide = 0;
    s.cat = cat;
    if (s.label_new) s.label_new->flags &= ~flag_visible;
    if (count == 0) {
        if (s.bitmap) bm_set_image(s.bitmap, "", 0);
        if (s.title) set_text(s.title, "", false);
        if (s.value) set_text(s.value, "", false);
        if (s.label_new) s.label_new->flags &= ~flag_visible;
        return;
    }
    auto& category = s.cats[cat];
    category.current = slide;
    const Widget::SlideShow::Slide sl = category.slides[slide];
    if (s.bitmap) bm_set_image(s.bitmap, sl.image, sl.archive);
    if (!sl.voice.empty()) {
        s.bd4 = 0;
        host_.radio_sound(0, s.type, s.name, 0);  // 0020ecb0(msg, +90, 0); +20 = 1; 003c9420
        // 001ca010(+90, 4, voice, 0, 0): the in-place record (strcpy into its 16 bytes).
        if (sl.voice.size() >= 16) unhosted(0x1ca0c4, "a ZEUS voice name of 16 or more characters ('" + sl.voice + "')");
        s.type = 4;
        s.name = sl.voice;
        s.bd4 = 0;
        host_.radio_sound(2, s.type, s.name, 0);  // 0020f2a0; +20 = 1; 003c9420
    }
    if (s.title) set_text(s.title, category.name, false);
    if (s.value) set_text(s.value, std::to_string(slide + 1) + "/" + std::to_string(count), false);  // "%d/%d"
    if (sl.unlocked) {
        const std::int32_t agent = host_.current_profile();
        if (!host_.zeus_viewed(agent, sl.level, sl.cat, sl.idx)) {
            if (s.label_new) s.label_new->flags |= flag_visible;
            host_.level_start(agent, sl.level, sl.cat, sl.idx, 1);  // 003328d0: marks it viewed
        }
    }
}

std::uint8_t GuiRuntime::ss_input(Widget* w, const PadEvent& ev) {  // 00327960
    auto pressed = [&](std::uint32_t m) { return ev.kind == 1 && ev.port == 0 && ev.mask == m && ev.released == 0; };
    if (pressed(0x2000) || pressed(0x8000)) {  // right / left
        if (ss_total(w) >= 2 && ss_step(w, ev.mask == 0x2000)) send(w, &canvas_, 0x10, 2, 0);
        return 1;
    }
    if (pressed(pad_triangle)) {
        auto& s = *w->ss;
        Widget* n = factory(s.ret_class, s.ret_file);  // 00418a80
        if (n) {
            send(w, n, 0x3000, s.ret_arg, 0);
            switch_screens(w, n);
        }
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- GuiBitmapWidget (vtable 004e0fc0)

void GuiRuntime::bm_release(Widget* w) {
    auto& b = *w->bm;
    if (b.texture == -1) return;
    host_.bitmap_release(b.texture);  // 003813a0(*00539248, +48, 0)
    b.texture = -1;
    b.width = b.height = 0;
}

std::uint8_t GuiRuntime::bm_load(Widget* w) {  // 0043c730
    auto& b = *w->bm;
    if (b.name.empty()) return 0;
    // Archive 0: 0036e4b0's loose file, which GuiAgentModify's patches (00340320) read from MANTEX.HOG.
    const auto t = host_.bitmap_load(b.archive, b.name);  // 0036e5d0 / 0036d000 / 00380a40 / 0036e220
    b.texture = t.slot;
    b.width = t.width;
    b.height = t.height;
    return b.texture != -1;
}

std::uint8_t GuiRuntime::bm_set_image(Widget* w, const std::string& name, std::uint32_t archive) {  // 0043c890
    auto& b = *w->bm;
    bm_release(w);
    b.archive = 0;
    b.name.clear();
    if (name.empty()) return 1;
    b.archive = archive;
    b.name = name;
    for (char& ch : b.name)
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 0x20);
    if (w->flags & flag_awake) bm_load(w);
    return 1;
}

// ---------------------------------------------------------------- GuiSubTitleDisplay (vtable 004e1560)
// Init / gain / lose / event are the GuiWidget entries, input 00293950
// (returns 0); the draw 00457ae0 is GuiDrawer::draw_subtitle.

void GuiRuntime::st_wake(Widget* w) {  // 00458140
    base_wake(w);
    auto& s = *w->st;
    s.font = host_.font("callout");  // 00418420(00414790(), 004c1ec8)
    TextObject o;  // 003d0940(o, 0, 0, w, h, +4c, 00587d70 = (1, 1, 1, 1))
    o.rect = {0, 0, cvt_w(w->rect[2], 0x45820c), cvt_w(w->rect[3], 0x458210)};
    o.font = s.font;
    o.rgba = {1.0f, 1.0f, 1.0f, 1.0f};
    host_.text_object(o);
    o.b10 = 0;
    o.b58 = 1;
    s.obj = o;
    const std::uint8_t fh = host_.font_height(s.font);  // 0036a2f0
    if (fh == 0) unhosted(0x458254, "subtitle line count of a font with no height (div by zero)");
    s.vis = cvt_w(fdiv(w->rect[3], static_cast<float>(fh)), 0x458258);
}

void GuiRuntime::st_sleep(Widget* w) {  // 00458080
    base_sleep(w);
    w->st->obj.reset();
}

void GuiRuntime::st_update(Widget* w, float dt) {  // 00457fe0
    base_update(w, dt);
    w->st->time = fadd(w->st->time, fdiv(dt, 30.0f));
}

void GuiRuntime::st_clear(Widget* w) {  // 00459450
    w->st->str.reset();
    w->st->time = 0.0f;
}

std::uint8_t GuiRuntime::st_set_text(Widget* w, const std::optional<std::string>& s) {  // 00459500
    if (!s || s->empty() || (*s)[0] == '\0') return 0;
    auto& st = *w->st;
    st.str = s->substr(0, s->find('\0'));  // 0036b130 the old one, 0036b180 + 00129360
    st.time = 0.0f;
    return st_parse(w) ? 1 : 0;
}

std::uint8_t GuiRuntime::st_parse(Widget* w) {  // 00458a30
    auto& st = *w->st;
    st.lines.clear();  // 004593a0
    st.times.clear();
    if (!st.str) return 0;
    const std::string& str = *st.str;
    auto at = [&](std::size_t i) -> char { return i < str.size() ? str[i] : '\0'; };
    // The state at sp+e0: the pause, the colour, x, y, the run's offset and length.
    Widget::SubTitle::Segment state;
    state.rgba = {1.0f, 1.0f, 1.0f, 1.0f};
    const std::int32_t width = cvt_w(w->rect[2], 0x458aac);  // sp+b0
    std::int32_t linew = 0;  // s2
    std::vector<Widget::SubTitle::Segment> cur;  // 0036b180(0xc) + 00459bb0
    auto fw = [&](std::int32_t off, std::int32_t count) {  // 0036aa50(+4c, +50 + off, count)
        return host_.font_width(st.font, str.substr(static_cast<std::size_t>(off)), count);
    };
    auto new_line = [&] {  // 00459380 push, a new list, x = 0, line width 0, y += the font height
        st.lines.push_back(std::move(cur));
        cur.clear();
        state.x = 0;
        linew = 0;
        state.y += host_.font_height(st.font);
    };
    std::size_t p = 0;  // s4
    while (at(p) != '\0') {
        if (at(p) == '<' && at(p + 1) != '<') {
            if (at(p + 1) == '\0') return 0;
            const char tag = at(p + 1);
            p += 2;
            if (at(p) != '>' && at(p) != ':') return 0;
            if (at(p) == ':') ++p;
            std::string buf;  // sp+c0, 32 bytes before the state at sp+e0
            while (at(p) != '>') {
                if (at(p) == '\0') return 0;
                if (buf.size() >= 31) unhosted(0x458c10, "a subtitle tag argument past its 32-byte buffer");
                buf.push_back(at(p++));
            }
            ++p;
            if (tag == 'n') {
                float t = 0.0f;
                sscanf_s(buf.c_str(), "%f", &t);  // 00128c68(buf, "%f"): the result is not checked
                cur.push_back(state);  // 004598c0
                new_line();
                state.delay = t;
                state.off = static_cast<std::int32_t>(p);
                state.count = 0;
            } else if (tag == 'c') {
                int cr = 0, cg = 0, cb = 0;
                std::array<float, 4> c{1.0f, 1.0f, 1.0f, 1.0f};  // 00587d70
                if (sscanf_s(buf.c_str(), "%d,%d,%d", &cr, &cg, &cb) == 3)
                    c = {fdiv(static_cast<float>(cr), 255.0f), fdiv(static_cast<float>(cg), 255.0f),
                         fdiv(static_cast<float>(cb), 255.0f), 1.0f};
                if (state.count) {
                    cur.push_back(state);
                    linew += fw(state.off, state.count);
                    state.x = linew;
                    state.count = 0;
                    state.delay = 0.0f;
                }
                state.off = static_cast<std::int32_t>(p);
                state.rgba = c;
            } else if (tag == 'p') {
                float t = 0.0f;
                if (sscanf_s(buf.c_str(), "%f", &t) != 1) return 0;
                if (state.count) {
                    cur.push_back(state);
                    linew += fw(state.off, state.count);
                    state.x = linew;
                    state.count = 0;
                }
                state.off = static_cast<std::int32_t>(p);
                state.delay = t;
            }
            continue;
        }
        ++state.count;  // a character ("<<" steps one byte, so both stay in the run)
        const std::int32_t wc = fw(state.off, state.count);
        if (width < linew + wc) {
            const std::int32_t whole = state.count;  // s6
            bool found = false;
            for (std::int32_t i = whole - 1; i >= 0; --i) {
                if (at(static_cast<std::size_t>(state.off + i)) != ' ') continue;
                state.count = i;
                cur.push_back(state);
                state.off += i + 1;
                state.count = whole - i - 1;
                if (state.count < 0) state.count = 0;
                state.delay = 0.0f;
                found = true;
                break;
            }
            // Without a space the over-wide run is kept whole and carries on
            // (its offset and length unchanged) on the next line.
            if (!found) cur.push_back(state);
            new_line();
            state.delay = 0.0f;
        }
        ++p;
    }
    if (state.count != 0 || ftz(state.delay) != 0.0f) cur.push_back(state);
    if (!cur.empty()) st.lines.push_back(std::move(cur));  // else 00459320 + 0036b130
    st_timings(w);
    return 1;
}

void GuiRuntime::st_timings(Widget* w) {  // 00458900
    auto& st = *w->st;
    st.times.clear();
    float t = 0.0f;  // one clock over all the lines
    for (const auto& line : st.lines) {
        for (const auto& seg : line) {
            t = fadd(t, seg.delay);
            t = fadd(t, fdiv(static_cast<float>(seg.count), 20.0f));
        }
        st.times.push_back(t);  // 00458a10
    }
}
// A GuiSaveAgentWidget (vtable 004e1220): its event falls back to 00441ca0.
// Gain / lose / first / last / next / prev are the GuiWidget entries.

void GuiRuntime::go_init(Widget* w) {  // 0029b360
    base_init(w);
    auto& o = *w->go;
    static constexpr const char* groups[4] = {"control_grp", "volume_grp", "video_grp", "kick_grp"};  // 00506420
    static constexpr const char* buttons[4] = {"0control", "1volume", "2video", "3kick"};              // 00506430
    auto of = [&](const char* name, WidgetClass c) {
        Widget* x = find(w, name);
        return x && x->cls == c ? x : nullptr;  // isA (exact class)
    };
    for (std::size_t i = 0; i < 4; ++i) {
        o.groups[i] = find(w, groups[i]);
        if (o.groups[i]) o.groups[i]->flags &= ~flag_enabled;
        o.buttons[i] = of(buttons[i], WidgetClass::button);
        if (o.buttons[i]) o.buttons[i]->id10 = static_cast<std::int32_t>(i);
    }
    o.header = of("label_controlsettings", WidgetClass::text);
    o.main_grp = find(w, "optionmain_grp");
    o.square = find(w, "buttonsquare");
    o.buttonx = find(w, "buttonx");
    o.to_map = of("label_to_map", WidgetClass::text);
    o.manual_aim = of("value_manualaim", WidgetClass::button);
    o.rumble = of("value_rumble", WidgetClass::button);
    o.freelook_x = of("value_freelookx", WidgetClass::button);
    o.freelook_y = of("value_freelooky", WidgetClass::button);
    o.sfx = of("slider_sfx", WidgetClass::slider);
    o.voices = of("slider_voices", WidgetClass::slider);
    o.music = of("slider_music", WidgetClass::slider);
    o.mode_btn = of("value_mode_btn", WidgetClass::button);
    o.gamma = of("slider_gamma", WidgetClass::slider);
    o.video_mode = of("value_videomode", WidgetClass::button);
    o.players = of("player_list", WidgetClass::textlist);
    o.config_select = find(w, "select_config_select");
    // players +88 = config_select: the list's companion widget, which no hosted list path reads.
    for (std::size_t i = 0; i < 2; ++i) {
        o.audio_mode[i] = host_.get_string("AudioMode" + std::to_string(i));
        o.video_text[i] = host_.get_string("VideoMode" + std::to_string(i));
    }
    for (std::size_t i = 0; i < 4; ++i) o.headers[i] = host_.get_string("OptionsHeader" + std::to_string(i));
    o.invert = host_.get_string("ControlInvert");
    o.normal = host_.get_string("ControlNormal");
    o.mb_on = host_.get_string("MB_On");
    o.mb_off = host_.get_string("MB_Off");
    o.b401 = 1;
}

void GuiRuntime::go_wake(Widget* w) {  // 0029b1e0
    auto& o = *w->go;
    base_wake(w);
    o.rec = g.options;
    go_refresh(w, 0xffffffffu);
    o.in_game = game_canvas ? 1 : 0;  // the canvas class (vt +54) is "GuiGameCanvas"
    if (o.in_game) {
        if (o.video_mode) o.video_mode->flags &= ~flag_enabled;
        const bool kick = g.b_21c8 && g.b_21c0 && go_players() >= 2;
        if (!kick && o.buttons[3]) o.buttons[3]->flags &= ~flag_enabled;
    } else {
        host_.options_frontend_wake();
        host_.port_options_wake(w);  // port-only (checkpoint 132ob6): the host's extra rows
    }
}

void GuiRuntime::go_sleep(Widget* w) {  // 0029b070
    base_sleep(w);
    host_.rumble_stop();
    host_.options_sound(0, 4, "30_30_10");  // 0048b908
    if (!w->go->in_game) {
        host_.options_sound(0, 4, "");  // 004aa780
        host_.options_frontend_sleep();
    }
}

void GuiRuntime::go_update(Widget* w, float dt) {  // 0029b920
    auto& o = *w->go;
    base_update(w, dt);
    host_.rumble_update(fdiv(dt, 30.0f));
    if (o.b78 == 1) {  // 0029b95c: not confirmed within 8 s, the video mode reverts
        const auto elapsed = static_cast<std::uint64_t>(static_cast<std::int64_t>(host_.time_ms()) - o.t7c);
        if (elapsed >= 0x1f41) go_video_revert(w, true);
    }
    if (o.sel == 3) unhosted(0x29ae60, "the kick list update");
    if (o.buttons[3] && (o.buttons[3]->flags & flag_enabled) && go_players() < 2)
        o.buttons[3]->flags &= ~flag_enabled;
}

std::int32_t GuiRuntime::go_players() {  // 0029bdb0
    if (!g.b_21c8) return 1;
    unhosted(0x3fb8d0, "the network player count");
}

std::uint8_t GuiRuntime::go_input(Widget* w, const PadEvent& ev) {  // 00299b90
    auto& o = *w->go;
    auto pressed = [&](std::uint32_t m) { return ev.kind == 1 && ev.port == 0 && ev.mask == m && ev.released == 0; };
    auto released = [&](std::uint32_t m) { return ev.kind == 1 && ev.port == 0 && ev.mask == m && ev.released == 1; };
    if (pressed(pad_triangle)) {
        if (canvas_.focus && canvas_.focus->id10 == -1) {
            go_menu(w);
            return 1;
        }
        go_close(w, false);
        if (w->parent == &canvas_) {
            go_leave(w);
            return 1;
        }
    }
    if (pressed(0x2000)) {
        if (!canvas_.focus) unhosted(0x299cc4, "Right with no canvas focus (null +10 read)");
        if (canvas_.focus->id10 != -1) {
            go_enter(w);
            return 1;
        }
    }
    if (pressed(0x80)) {
        if (o.in_game && o.buttonx && (o.buttonx->flags & flag_visible)) return 1;
        go_close(w, true);
        return 1;
    }
    if ((released(0x8000) || released(0x2000)) && canvas_.focus == o.voices) {
        o.rec.voices = fdiv(static_cast<float>(o.voices->sl->v), 100.0f);
        options_apply(o.rec, 0x20);
        host_.options_sound(0, 4, "30_30_10");  // 001ca210 + 0020ecb0
        host_.options_sound(2, 4, "30_30_10");  // +270: 001ca010 + 0020f2a0
        return 1;
    }
    if (pressed(0x8000)) return 1;
    if (pressed(0x800) && o.in_game) {
        sm_exit(w);
        return 1;
    }
    return 0;
}

std::uint8_t GuiRuntime::go_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                  std::int32_t t0) {  // 0029a190
    auto& o = *w->go;
    const std::uint32_t t = type & 0xffff;
    if (t == 0 && sender->id10 != -1) {
        go_show(w, sender->id10);
        return 1;
    }
    if (t == 0x10 && a3 == 1 && sender->id10 != -1) return 1;
    if (t == 0x13 && sender == o.players) unhosted(0x29a220, "the kick list selection");
    if (t == 2) {
        if (sender->id10 != -1) {
            go_enter(w);
            return 1;
        }
        if (sender == o.manual_aim) {
            o.rec.manual_aim = o.rec.manual_aim ? 0 : 1;
            options_apply(o.rec, 1);
            go_refresh(w, 1);
        } else if (sender == o.rumble) {
            o.rec.rumble = o.rec.rumble ? 0 : 1;
            options_apply(o.rec, 2);
            go_refresh(w, 2);
            if (o.rec.rumble) host_.rumble_start(0, 1, 0xff, 1, 1.0f);
        } else if (sender == o.freelook_x) {
            o.rec.freelook_x = o.rec.freelook_x ? 0 : 1;
            options_apply(o.rec, 4);
            go_refresh(w, 4);
        } else if (sender == o.freelook_y) {
            o.rec.freelook_y = o.rec.freelook_y ? 0 : 1;
            options_apply(o.rec, 8);
            go_refresh(w, 8);
        } else if (sender == o.mode_btn) {
            ++o.rec.sound_mode;
            if (o.rec.sound_mode >= 2) o.rec.sound_mode = 0;
            options_apply(o.rec, 0x80);
            go_refresh(w, 0x80);
        } else if (sender == o.video_mode) {  // 0029a430
            ++o.rec.video_mode;
            if (o.rec.video_mode >= 2) o.rec.video_mode = 0;
            if (o.rec.video_mode == 1) {  // 0029a470: ask first (popup tag 0xb)
                popup_open(5, host_.get_string("ProscanSwitchT"), host_.get_string("ProscanSwitchM"), w, 0xb);
            } else {
                go_video(w);  // 0029a500
            }
            go_refresh(w, 0x800);
        } else if (!o.in_game) {
            host_.port_options_pressed(w, sender);  // port-only (checkpoint 132ob6): the host's extra rows
        }
        return 1;
    }
    if (t == 0x1a || t == 0x1b) {
        if (a3 == 0xb) {
            if (t == 0x1a) {  // 0029a568: switched; keep it only if confirmed within 8 s (popup tag 0xc)
                o.save.popup = popup_open(5, host_.get_string("ProscanValidT"), host_.get_string("ProscanValidM"), w, 0xc);
                o.b78 = 1;
                o.t7c = host_.time_ms();
                go_video(w);
                return 1;
            }
            o.rec.video_mode = 0;
            go_refresh(w, 0x800);
            return 1;
        }
        if (a3 == 0xc) {
            if (t == 0x1b) {
                go_video_revert(w, false);
                return 1;
            }
            o.b78 = 0;
            return 1;
        }
        if (a3 == 0xd) {
            if (t == 0x1a) unhosted(0x29a698, "the kick popup accepted");
            return 1;
        }
        return save_widget_event(w, sender, type, a3, t0);
    }
    if (t == 0x1d) {
        if (sender == o.sfx) {
            o.rec.sfx = fdiv(static_cast<float>(a3), 100.0f);
            options_apply(o.rec, 0x10);
            host_.options_sound(1, 1, "G_DESERTEAGLE44");  // 004aa730: 001ca210 + 0020faa0
        } else if (sender == o.music) {
            o.rec.music = fdiv(static_cast<float>(a3), 100.0f);
            options_apply(o.rec, 0x40);
        } else if (sender == o.gamma) {
            o.rec.gamma = fdiv(static_cast<float>(a3), 100.0f);
            options_apply(o.rec, 0x400);
        }
        return 1;  // slider_voices: applied when the key is released (00299b90)
    }
    return save_widget_event(w, sender, type, a3, t0);
}

void GuiRuntime::go_leave(Widget* w) {  // 00299780
    if (w->go->in_game) {
        sm_back(w);
        return;
    }
    switch_screens(w, factory("GuiTitleScreen", std::string("TITLESCR")));
}

void GuiRuntime::go_menu(Widget* w) {  // 00299800
    auto& o = *w->go;
    const auto k = static_cast<std::size_t>(o.sel);
    if (o.buttons[k]) {
        if (o.groups[k]) o.groups[k]->flags &= ~flag_enabled;
        set_focus(o.buttons[k]);
        send(o.buttons[k], &canvas_, 0x10, 0, 0);
    }
    if (o.main_grp) o.main_grp->flags |= flag_enabled;
}

void GuiRuntime::go_enter(Widget* w) {  // 002998b0
    auto& o = *w->go;
    const auto k = static_cast<std::size_t>(o.sel);
    if (o.groups[k]) {
        o.groups[k]->flags |= flag_enabled;
        Widget* f = v_first(o.groups[k]);
        set_focus(f);
        w->focus = f;
    }
    if (o.main_grp) o.main_grp->flags &= ~flag_enabled;
    if (o.buttons[k]) {
        send(w, o.buttons[k], 0xa, 0xa, 0);
        send(o.buttons[k], &canvas_, 0x10, 0, 0);
    }
    go_hints(w);
}

void GuiRuntime::go_hints(Widget* w) {  // 002999b0
    auto& o = *w->go;
    if (!o.in_game) return;
    if (o.square) {
        if (o.sel != 3) o.square->flags |= flag_visible;
        else o.square->flags &= ~flag_visible;
    }
    if (o.buttonx) {
        if (o.sel == 3) o.buttonx->flags |= flag_visible;
        else o.buttonx->flags &= ~flag_visible;
    }
    if (o.to_map) {
        const auto s = host_.get_string(o.sel == 3 ? "ToKick" : "ToSave");  // 004aa770 / 004aa778
        set_text(o.to_map, s ? *s : std::string(), false);
    }
}

void GuiRuntime::go_show(Widget* w, std::int32_t idx) {  // 00299aa0
    auto& o = *w->go;
    for (Widget* gw : o.groups)
        if (gw) gw->flags &= ~flag_visible;
    const auto k = static_cast<std::size_t>(idx);
    if (o.header && o.buttons[k]) set_text(o.header, o.headers[k] ? *o.headers[k] : std::string(), false);
    if (!o.groups[k]) unhosted(0x299b28, "an options group missing from the file (null +14 write)");
    o.groups[k]->flags |= flag_visible;
    if (idx == 3 && o.players) array_select(o.players, 0, 0);
    o.sel = idx;
    go_hints(w);
}

void GuiRuntime::go_refresh(Widget* w, std::uint32_t mask) {  // 0029aa50
    auto& o = *w->go;
    auto text = [&](Widget* x, const std::optional<std::string>& s) { set_text(x, s ? *s : std::string(), false); };
    auto pct = [](float v) { return cvt_w(fmul(100.0f, v), 0x29aa50); };
    if ((mask & 1) && o.manual_aim) text(o.manual_aim, o.rec.manual_aim ? o.normal : o.invert);
    if ((mask & 4) && o.freelook_x) text(o.freelook_x, o.rec.freelook_x ? o.normal : o.invert);
    if ((mask & 8) && o.freelook_y) text(o.freelook_y, o.rec.freelook_y ? o.invert : o.normal);
    if ((mask & 2) && o.rumble) text(o.rumble, o.rec.rumble ? o.mb_on : o.mb_off);
    if ((mask & 0x10) && o.sfx) sl_set(o.sfx, 0, pct(o.rec.sfx), 0, 100, 10);
    if ((mask & 0x20) && o.voices) sl_set(o.voices, 0, pct(o.rec.voices), 0, 100, 10);
    if ((mask & 0x40) && o.music) sl_set(o.music, 0, pct(o.rec.music), 0, 100, 10);
    if ((mask & 0x80) && o.mode_btn) {
        const auto m = static_cast<std::size_t>(o.rec.sound_mode);
        if (m >= o.audio_mode.size()) unhosted(0x29aa50, "a sound mode outside AudioMode0..1");
        text(o.mode_btn, o.audio_mode[m]);
    }
    if ((mask & 0x400) && o.gamma) sl_set(o.gamma, 0, pct(o.rec.gamma), 0, 100, 5);
    if ((mask & 0x800) && o.video_mode) {
        const auto m = static_cast<std::size_t>(o.rec.video_mode);
        if (m >= o.video_text.size()) unhosted(0x29aa50, "a video mode outside VideoMode0..1");
        text(o.video_mode, o.video_text[m]);
    }
}

void GuiRuntime::go_close(Widget* w, bool save) {  // 0029ad10
    auto& o = *w->go;
    const std::uint32_t d = options_diff(o.rec, g.options);
    if (d == 0 && o.b401) {
        send(w, &canvas_, 0x10, 4, 0);
        return;
    }
    if (save) {
        g.options = o.rec;
        o.b401 = 0;
        o.save.mode = 4;
        save_start(w);
        return;  // 0029ade0: the saved options are already live (each change applied them)
    }
    if (d & 0x800) host_.app_suspend(1);  // 0029ade8
    options_apply(g.options, d);
    if (d & 0x800) host_.app_resume();
}

void GuiRuntime::go_video(Widget* w) {
    host_.app_suspend(1);
    options_apply(w->go->rec, 0x800);
    host_.app_resume();
}

void GuiRuntime::go_video_revert(Widget* w, bool close_popup) {  // 002996d0
    auto& o = *w->go;
    o.b78 = 0;
    o.rec.video_mode = 0;
    go_refresh(w, 0x800);
    go_video(w);
    g.gamma_word = cvt_w(fmul(191.25f, o.rec.gamma), 0x2996d0) & 0xff;
    if (close_popup) popup_close(o.save.popup);
}

// The GuiSaveAgentWidget's vtable +5c, the end of a save flow.
void GuiRuntime::save_done(Widget* w, std::uint8_t result) {
    if (w->cls == WidgetClass::gameopt) {
        w->go->b401 = result;  // 0029ad00
        return;
    }
    if (w->cls == WidgetClass::acntlogn) {  // 002891d0 ignores the result
        acl_done(w);
        return;
    }
    if (w->cls == WidgetClass::onlinepref) {  // 002a97e0 ignores the result: 002a96a0(this, 0)
        opt_end(w, 0);
        return;
    }
    if (w->cls == WidgetClass::cmdcentr) {
        cc_leave(w);  // 00295430 ignores the result: the command centre is left whether the save succeeded or not
        return;
    }
    if (w->cls == WidgetClass::agentload || w->cls == WidgetClass::onlinecn || w->cls == WidgetClass::msnsetup ||
        w->cls == WidgetClass::netjoin || w->cls == WidgetClass::netcontacts || w->cls == WidgetClass::netmess)
        return;  // 00293890: the base's nothing
    if (w->cls == WidgetClass::agentmod) {  // 0033fa00 ignores the result: back to the command centre
        am_leave(w);
        return;
    }
    if (w->cls == WidgetClass::agentsave) {  // 002928e0 ignores the result
        as_done(w);
        return;
    }
    if (w->cls == WidgetClass::autosaver) {  // 0045f300 ignores the result: 00414ac0(canvas, this, 0) ends the debrief
        switch_screens(w, nullptr);
        return;
    }
    unhosted(0x441a10, "vtable +5c of this save widget");
}

SaveWidgetFields& GuiRuntime::save_fields(Widget* w) {
    if (w->cls == WidgetClass::gameopt) return w->go->save;
    if (w->cls == WidgetClass::cmdcentr) return w->cc->save;
    if (w->cls == WidgetClass::agentmod) return w->am->save;
    if (w->cls == WidgetClass::agentsave) return w->as->save;
    if (w->cls == WidgetClass::autosaver) return w->asv->save;
    if (w->cls == WidgetClass::onlinecn) return w->oc->save;  // the net screen 0033d580 (+48 the wait popup)
    if (w->cls == WidgetClass::msnsetup) return w->ms->save;  // the net screen 0033d580
    if (w->cls == WidgetClass::netjoin || w->cls == WidgetClass::netcontacts || w->cls == WidgetClass::netagencycell)
        return w->lb->save;  // the net screen 0033d580 (132ln4 / 132on2h / 132on2i)
    if (w->cls == WidgetClass::acntlogn) return w->acl->save;  // the net screen 0033d580 (132on2b)
    if (w->cls == WidgetClass::onlinepref) return w->opt->save;  // the net screen 0033d580 (132on2c)
    if (w->cls == WidgetClass::netmess) return w->nm->save;      // the net screen 0033d580 (132on2c)
    unhosted(0x441a10, "the agent save flow of this widget");
}

// The native SaveAgentFlow (agent_save_flow.cpp, EE goldens) over a widget built on GuiSaveAgentWidget. The
// card W = *0055a830 is the host's (the front end's FolderAgentCard, which the play's start menu shares
// since Checkpoint 132f7; a play without a front end mounts none, so its +59d reads 0 and the options take
// the "no card" popup). Every card call returns at once; the EE's waiting card
// calls (0040e090 & co.) run the poll *0055a838 = 00441c10 (002cc260(0, 0): one nested GUI update 002cbb30
// and draw 002cbad0) in a do-while (0040e270) until the card is idle, so at least once per operation: here
// host card_poll once per wait, so the progress popup MC_NOREMOVE, opened before the save's write, is drawn.
std::int32_t GuiRuntime::save_flow(Widget* w, Widget* sender, const std::function<std::int32_t(SaveAgentFlow&)>& body) {
    SaveWidgetFields& fields = save_fields(w);
    SaveAgentGlobals globals;
    globals.card_seen = g.card_ok;
    globals.last_card_id = static_cast<std::uint32_t>(g.card_slot);
    globals.polling = g.card_poll == 0x441c10;
    const auto sync = [&] {
        g.card_ok = globals.card_seen;
        g.card_slot = static_cast<std::int32_t>(globals.last_card_id);
        g.card_poll = globals.polling ? 0x441c10u : 0u;
    };
    SaveAgentHost h;
    SaveAgentFlow* flow = nullptr;
    const auto store = [&] {
        fields.popup = flow->popup;
        fields.mode = flow->mode;
        fields.slot = flow->slot;
        fields.no_remove = flow->no_remove_shown;
        fields.old_name = flow->old_name;
    };
    // A card call's wait loops (0040e270 & co.): each pass calls *0055a838 when set; the folder card is
    // never busy, so each wait makes one pass. The flow's popup records are stored first (they are the
    // widget's fields the nested draw sees).
    const auto card_wait = [&](int waits) {
        for (int i = 0; i < waits; ++i) {
            if (!globals.polling) continue;
            sync();
            if (flow) store();
            host_.card_poll();
        }
    };
    h.refresh = [&] {  // 0040d3c0: four card operations, each with its wait loop
        host_.card_refresh();
        card_wait(4);
    };
    h.status = [&] {
        SaveCardStatus s;
        s.present = g.card.present;
        s.formatted = g.card.check_space;
        s.directory = g.card.directory;
        s.free_kb = g.card.free;
        s.id = static_cast<std::uint32_t>(g.card.slot);
        return s;
    };
    h.make_directory = [&] {  // 0040d740: the directory's wait (the PC card writes no icon files, 0040e090 x 4)
        const auto r = host_.card_make_directory();
        card_wait(1);
        return r;
    };
    h.format = [&] { return host_.card_format(); };
    h.list = [&] { return g.card.dir; };  // the W entry table 0040d600 copies (+560 / +1c)
    h.remove = [&](const std::string& name) {  // 0040dc80
        host_.card_remove(name);
        card_wait(1);
    };
    h.kilobytes = [&](std::uint32_t bytes) { return host_.card_space_needed(static_cast<std::int32_t>(bytes)); };
    h.exists = [&](const std::string& name) { return host_.card_exists(name); };
    h.text = [&](const std::string& key) {
        const auto s = host_.get_string(key);
        if (!s) unhosted(0x3fdb50, "a NULL string " + key + " in the save flow");
        return *s;
    };
    h.open = [&](std::int32_t kind, const std::string& title, const std::string& text, std::int32_t tag) {
        return popup_open(kind, title, text, w, tag);  // 002cae30(*004ffc04, kind, title, text, this, tag)
    };
    h.close = [&](std::int32_t id) { popup_close(id); };
    h.done = [&](std::int32_t result) {
        sync();
        store();
        save_done(w, static_cast<std::uint8_t>(result));
    };
    h.default_event = [&](std::uint32_t, std::uint32_t a2, std::int32_t tag, std::uint32_t t0) {
        return static_cast<std::int32_t>(base_event(w, sender, a2, tag, static_cast<std::int32_t>(t0)));  // 0041ef10
    };
    h.agent_name = [&] { return host_.profile_text(host_.current_profile(), 0x8ac); };
    h.agent_slot = [&] { return host_.profile_word(host_.current_profile(), 4); };
    h.set_agent_name = [&](const std::string& name) { host_.agent_set_name(host_.current_profile(), name); };
    h.save_agent = [&] {  // 00334110: its 0040e090 write
        const auto r = host_.agent_save(host_.current_profile());
        card_wait(1);
        return r;
    };
    h.create_agent = [&](std::int32_t slot, std::uint32_t nosave) {  // 00333f60(profile, slot, nosave)
        return host_.agent_create(host_.current_profile(), slot, nosave);
    };
    h.save_options = [&] {  // 0045a2a0(*004ffd2c): its 0040e090 write
        const auto r = host_.options_save(g.options);
        card_wait(1);
        return r;
    };
    h.decode = [&](const std::string& file) {
        return decode_agent_file_name(file, 0x10, charmap.ready != 0 && charmap.count != 0, charmap.count);
    };
    SaveAgentFlow f(h, globals);
    flow = &f;
    f.popup = fields.popup;
    f.mode = fields.mode;
    f.slot = fields.slot;
    f.no_remove_shown = fields.no_remove;
    f.old_name = fields.old_name;
    const std::int32_t r = body(f);
    sync();
    store();
    return r;
}

// 00441a10: +4c is 4 for the options save (go_close 0029ad10) and 2 for the command centre's exit autosave
// (cc_exit 002955b0).
void GuiRuntime::save_start(Widget* w) {
    save_flow(w, nullptr, [](SaveAgentFlow& f) {
        f.start();
        return 0;
    });
}

void GuiRuntime::options_apply(const GameOptionsRecord& r, std::uint32_t bits) {  // 0045a3a0
    if ((bits & 1) && host_.options_player()) host_.options_player_byte(0x174, r.manual_aim);
    if (bits & 2) host_.rumble_enable(r.rumble);
    if ((bits & 4) && host_.options_player()) host_.options_player_byte(0x175, r.freelook_x);
    if ((bits & 8) && host_.options_player()) host_.options_player_byte(0x176, r.freelook_y);
    if ((bits & 0x10) && host_.sound_manager()) host_.sound_volume(0, cvt_w(fmul(1024.0f, r.sfx), 0x45a3a0));
    if ((bits & 0x20) && host_.sound_manager()) host_.sound_volume(2, cvt_w(fmul(1024.0f, r.voices), 0x45a3a0));
    if ((bits & 0x40) && host_.sound_manager()) host_.sound_volume(1, cvt_w(fmul(1024.0f, r.music), 0x45a3a0));
    if ((bits & 0x80) && host_.sound_manager()) host_.sound_mode(r.sound_mode);
    if (bits & 0x400) g.gamma_word = cvt_w(fmul(191.25f, r.gamma), 0x45a3a0) & 0xff;
    if (bits & 0x800) {  // 0045a590
        const std::uint8_t mode = r.video_mode ? 4 : (host_.boot_video_pal() ? 2 : 0);
        host_.video_mode_set(mode);  // 00165420(001698c0(), mode)
        g.gamma_word = cvt_w(fmul(191.25f, g.options.gamma), 0x165420) & 0xff;  // 00165504: *004ffd2c +1c
    }
}

bool GuiRuntime::hud_busy() const {  // 00241ac0
    if (msgbox && (msgbox->flags & flag_awake)) return true;  // 002cab70(*004ffc04)
    return canvas_.focus && (canvas_.focus->flags & flag_focusable);
}

void GuiRuntime::hud_open() {  // 002429f0(*004ffc2c)
    if (g.controller_6a8) return;
    host_.app_131470(1);
    if (g.hud_6a9) map_screen_close();  // 002427f0 at 0x00242a28
    if (!hud_busy()) host_.pad_reset();  // 0036a240(0055a300)
    push(factory("GuiStartMenuBackdrop", std::string("GAMEBACK")), -1);
    push(factory("GuiStartMenu", std::string("INGAMENU")), 0);
    // 0045ca20(*(hud + 6a4), 0): the weapon select closes (after the pushes, so its focus returns to the
    // game screen). 00437950(0) (the USB headset voice chat) does nothing in a solo game.
    if (game_screen) gs_open_ca20(false);
    g.controller_6a8 = 1;
}

void GuiRuntime::hud_close() {  // 00242900(*004ffc2c)
    if (!g.controller_6a8) return;
    if (Widget* s = lookup("GuiStartMenu")) pop(s);
    if (Widget* s = lookup("GuiStartMenuBackdrop")) pop(s);
    host_.pad_reset();  // 0036a240(0055a300)
    if (g.b_bb7) host_.app_131470(0);
    g.controller_6a8 = 0;
}

bool GuiRuntime::map_toggle() {  // 002427a0(*004ffc2c)
    host_.pad_mask(-0x41);       // *0055a6c0 = -0x41: the next pad reset keeps Cross (0x002427b0)
    if (g.hud_6a9) map_screen_close();
    else map_screen_open();
    return g.hud_6a9 != 0;
}

void GuiRuntime::map_screen_open() {  // 00242870(*004ffc2c)
    if (g.hud_6a9 || g.controller_6a8) return;
    push(factory("GuiMapScreen", std::string("MAPSCRN")), 0);  // 00418a80, then the manager's +64 at layer 0
    g.hud_6a9 = 1;
}

void GuiRuntime::map_screen_close() {  // 002427f0(*004ffc2c)
    if (!g.hud_6a9) return;
    if (Widget* s = lookup("GuiMapScreen")) pop(s);  // 00418980, then the manager's +68
    g.hud_6a9 = 0;
}

void GuiRuntime::start_press() {  // 0024cd30's Start edge
    if (g.controller_6a8) {
        hud_close();
        host_.app_131470(0);
    } else {
        hud_open();
    }
}

std::uint8_t GuiRuntime::game_canvas_event(Widget* sender, std::uint32_t type, std::int32_t a3,
                                           std::int32_t t0) {  // 004653b0
    const std::uint32_t t = type & 0xffff;
    if (t == 0x1a && a3 == 0x667) {
        host_.app_12fa60(host_.network_play_game_572400() ? 2 : 3);
        return 1;
    }
    if (t == 0x1a && a3 == 0x668) {
        host_.app_12fa60(3);
        return 1;
    }
    if ((t == 0x10 || t == 0x11) && static_cast<std::uint32_t>(a3) < 10) {
        std::uint32_t table = 0;
        switch (a3) {
        case 2: case 3: case 6: table = 0x5874b0; break;  // GS_POPUP
        case 5: table = 0x587310; break;                  // GS_MISSION
        case 7: table = 0x587650; break;
        case 8: table = 0x5877f0; break;
        case 9: table = 0x587990; break;
        case 0: table = 0x586610; break;  // 00465a50
        case 1: table = 0x5867b0; break;
        case 4: table = 0x586950; break;
        default: break;
        }
        host_.sound(table, sender, t == 0x11);
        return 1;
    }
    return canvas_base_event(sender, type, a3, t0);
}

// ---------------------------------------------------------------- canvas

void GuiRuntime::canvas_push(Widget* w, std::int32_t layer) {  // 00414f00
    if (!w || (w->flags & flag_awake)) return;
    const std::int16_t l = static_cast<std::int16_t>(layer);
    auto& stack = canvas_.children;
    auto it = stack.begin();
    while (it != stack.end() && !((*it)->layer > l)) ++it;
    if (w->flags & flag_visible) {
        if (!send(&canvas_, w, 0x10, 2, 0)) send(w, &canvas_, 0x10, 2, 0);
    }
    w->parent = &canvas_;
    w->layer = l;
    it = stack.insert(it, w);
    clear_awake(w);
    o_wake(w);
    send(&canvas_, w, 0x18, 0, 0);
    if (!stack.empty() && stack.back() == w) {
        host_.pad_reset();
        if (canvas_.focus) o_lose(canvas_.focus);
        canvas_.focus = w->focus;
        if (!canvas_.focus) canvas_.focus = v_first(w);
        if (canvas_.focus) o_gain(canvas_.focus);
    }
}

void GuiRuntime::push(Widget* w, std::int32_t layer) {  // 00465570
    canvas_push(w, layer);
    if (game_canvas) return;  // GuiGameCanvas: push is 00414f00 alone
    if (!w) return;
    if (Widget* f = find(w, "tempbackground")) {
        send(&canvas_, f, 0xa, cvt_w(phase, 0x4655bc), 0);
        send(&canvas_, f, 0xd, 0, 300);
    }
}

void GuiRuntime::pop(Widget* w) {  // 00414be0
    if (!w) return;
    if (iterating_) {
        for (auto it = pending_list_.begin(); it != pending_list_.end(); ++it) {
            if (*it == w) {
                pending_list_.erase(it);
                break;
            }
        }
    }
    auto& stack = canvas_.children;
    auto it = stack.begin();
    while (it != stack.end() && *it != w) ++it;
    if (it == stack.end()) return;
    w->parent = nullptr;
    if (stack.back() == w) host_.pad_reset();
    if (w->flags & flag_awake) o_sleep(w);
    stack.erase(std::find(stack.begin(), stack.end(), w));
    send(&canvas_, w, 0x19, 0, 0);
    if (canvas_.focus && w->focus == canvas_.focus) o_lose(canvas_.focus);
    w->focus = nullptr;
    if (!stack.empty()) {
        Widget* t = stack.back();
        canvas_.focus = t->focus;
        if (!canvas_.focus) canvas_.focus = v_first(t);
    } else {
        canvas_.focus = nullptr;
    }
    if (canvas_.focus) o_gain(canvas_.focus);
}

void GuiRuntime::switch_screens(Widget* from, Widget* to) {  // 00414ac0
    if (from) pop(from);
    if (to) push(to, 0);
}

void GuiRuntime::set_focus(Widget* f) {  // 00414b30
    if (f == canvas_.focus) return;
    if (canvas_.focus) o_lose(canvas_.focus);
    canvas_.focus = f;
    if (canvas_.focus) {
        o_gain(canvas_.focus);
        if (canvas_.focus == f && f && f != &canvas_) {
            for (Widget* s = f; s && s != &canvas_; s = s->parent) s->focus = canvas_.focus;
        }
    }
}

void GuiRuntime::defer(Widget* pop_w, Widget* push_w) {
    pending_pop = pop_w;
    pending_push = push_w;
    pending = 1;
}

std::uint8_t GuiRuntime::canvas_navigate(const PadEvent& ev) {  // 00415170
    if (!(ev.kind == 1 && ev.port == 0 && ev.released == 0)) return 0;
    if (ev.mask != pad_up && ev.mask != pad_down) return 0;
    if (canvas_.children.empty() || !canvas_.focus) return 0;
    Widget* t = top();
    Widget* s1 = ev.mask == pad_up ? v_prev(t, canvas_.focus, true) : v_next(t, canvas_.focus, true);
    if (s1 && s1 != canvas_.focus) send_up(s1, 0x10, 0, 0);
    if (s1 != canvas_.focus) {
        if (canvas_.focus) o_lose(canvas_.focus);
        canvas_.focus = s1;
        if (canvas_.focus) {
            o_gain(canvas_.focus);
            if (canvas_.focus == s1 && s1) {
                for (Widget* s = s1; s && s != &canvas_; s = s->parent) s->focus = canvas_.focus;
            }
        }
    }
    return 1;
}

std::uint8_t GuiRuntime::canvas_base_event(Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {
    if ((type & 0xffff) == 3 && sender == pending_pop) {  // 00414a80
        pending = 1;
        return 1;
    }
    return base_event(&canvas_, sender, type, a3, t0);
}

std::uint8_t GuiRuntime::canvas_event(Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {
    if (game_canvas) return game_canvas_event(sender, type, a3, t0);
    const std::uint32_t t = type & 0xffff;  // 00465610
    if (t == 0x1e) {
        if (broadcast(&canvas_, type, a3, t0)) return 1;
        if (a3 != 0) {
            g.app_state_prev = g.app_state;
            g.app_state = 3;
            return 1;
        }
        unhosted(0x465678, "string popup of canvas event 0x1e");
    }
    if (t == 0x10 || t == 0x11) {
        if (g.sound_gate) return 1;
        std::uint32_t table = 0;
        switch (a3) {
        case 5: table = 0x586af0; break;
        case 6: table = 0x586c90; break;
        case 7: table = 0x586e30; break;
        case 8: table = 0x586fd0; break;
        case 9: table = 0x587170; break;
        default: break;
        }
        if (!table) {
            switch (a3) {  // 00465a50
            case 0: table = 0x586610; break;
            case 1: table = 0x5867b0; break;
            case 2: table = 0x5862d0; break;
            case 3: table = 0x586470; break;
            case 4: table = 0x586950; break;
            default: return canvas_base_event(sender, type, a3, t0);
            }
        }
        host_.sound(table, sender, t == 0x11);
        return 1;
    }
    return canvas_base_event(sender, type, a3, t0);
}

void GuiRuntime::dispatch(const PadEvent& ev) {
    if (g.debug_triangle && !game_canvas) {  // 004657e0
        if (ev.kind == 1 && ev.port == 0 && ev.mask == pad_triangle && ev.released == 0)
            unhosted(0x3a6650, "TRIANGLE debug path");
        return;
    }
    std::uint8_t handled = 0;  // 00415310
    PadEvent e = ev;  // one event walks up the parents (GuiNetMessages' 003473e0 rewrites its mask on the way)
    if (!canvas_.children.empty()) {
        Widget* t = top();
        Widget* s = t->focus ? t->focus : t;
        handled = v_input(s, e);
        while (!handled) {
            s = s->parent;
            if (!s) break;
            handled = v_input(s, e);
        }
    }
    if (!handled) v_input(&canvas_, e);
}

void GuiRuntime::translate_pads(const PadInput& in) {  // 00415870
    g.pad_held = in.current[0];  // 0055a44c: the pad reader's port-0 held word (the same frame's buttons)
    if (!(canvas_.flags & flag_enabled)) return;
    // 00415510: pad clock in 1/100 s from T1 and the tick counter.
    const std::uint32_t u = in.t1 + g.t1_base;
    float f;
    if (static_cast<std::int32_t>(u) >= 0) {
        f = static_cast<float>(static_cast<std::int32_t>(u));
    } else {
        const std::int32_t h = static_cast<std::int32_t>((u >> 1) | (u & 1));
        f = static_cast<float>(h);
        f = fadd(f, f);
    }
    const std::int32_t q = cvt_w(fdiv(f, std::bit_cast<float>(0x40b851ecu)), 0x4155b0);
    std::uint64_t a = static_cast<std::uint64_t>(in.ticks) + static_cast<std::uint64_t>(static_cast<std::int64_t>(q));
    std::uint32_t clock = 0;
    std::int64_t step = static_cast<std::int64_t>(100) << 30;
    std::int32_t bit = 0x40000000;
    for (int i = 0; i < 31; ++i) {
        const std::uint64_t d = a - static_cast<std::uint64_t>(step);
        if (static_cast<std::int64_t>(d) >= 0) {
            a = d;
            clock += static_cast<std::uint32_t>(bit);
        }
        step >>= 1;
        bit >>= 1;
    }
    for (std::uint16_t port = 0; port < 2; ++port) {
        const std::uint32_t cur = in.current[port];
        const std::uint32_t prev = in.previous[port];
        for (int k = 0; k < 16; ++k) {
            const std::uint32_t m = pad_masks[k];
            std::int32_t& cnt = g.repeat_count[port][k];
            std::int32_t& tm = g.press_time[port][k];
            if ((cur & m) != (prev & m)) {
                PadEvent ev;
                ev.kind = 1;
                ev.port = port;
                ev.released = (cur & m) ? 0 : 1;
                ev.mask = static_cast<std::uint16_t>(m);
                ev.repeat = 0;
                g.dispatch_top = top();
                cnt = 0;
                tm = static_cast<std::int32_t>(clock);
                dispatch(ev);
            } else {
                if (!(cur & m) || !(repeat_mask & m) || canvas_.children.empty() || g.dispatch_top != top()) continue;
                const std::int32_t held = static_cast<std::int32_t>(clock - static_cast<std::uint32_t>(tm) - 300u);
                if (held < 0) continue;
                const std::int32_t n = cnt >= 4 ? (held - 600) / 75 + 5 : held / 150 + 1;
                if (!(cnt < n)) continue;
                PadEvent ev;
                ev.kind = 1;
                ev.port = port;
                ev.released = 0;
                ev.mask = static_cast<std::uint16_t>(m);
                ev.repeat = static_cast<std::uint16_t>(cnt);
                ++cnt;
                dispatch(ev);
            }
        }
    }
    if (keyboard_path) keyboard_input();
}

void GuiRuntime::keyboard_input() {  // 004153f0(canvas, 0055a700)
    for (;;) {
        const auto r = host_.usbkb_poll();  // 003d1660(kbd, 1)
        if (!r || r->count == 0) return;
        // 003d1610: the first key unless the buffer is empty or it carries 0x8000
        // (0x4000 is masked off, which the & 0xff already does).
        std::uint8_t code = 0;
        if (r->count != 0 && !(r->code & 0x8000)) code = static_cast<std::uint8_t>(r->code & 0xff);
        if (code == 0) continue;
        PadEvent ev;
        ev.kind = 2;
        ev.port = 0;
        ev.zero = 1;
        ev.released = 0;
        ev.mask = code;
        ev.mod = r->mod;
        ev.ch = ascii_keys ? usbkb_char(code, r->mod) : code;
        dispatch(ev);  // canvas vt +6c
    }
}

std::uint8_t GuiRuntime::usbkb_char(std::uint8_t code, std::uint32_t mod) {  // 003d1580
    // 004937fc / 0049383c, indices 0..0x39 (0x3a..0x41 map to code + 0x46).
    static constexpr char lower[0x3a + 1] =
        "\0\0\0\0abcdefghijklmnopqrstuvwxyz1234567890\n\x45\x08\x09 -=[]?\\;'?,./";
    static constexpr char upper[0x3a + 1] =
        "\0\0\0\0ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\n\x45\x08\x09 _+{}?|:\"?<>?";
    if (code >= 0x42 || code < 4) return 0;
    if (code >= 0x3a) return static_cast<std::uint8_t>(code + 0x46);
    const char* t = (mod & 2) || (mod & 0x20) ? upper : lower;
    return static_cast<std::uint8_t>(t[code]);
}

void GuiRuntime::controller_check() {  // 00465870 (without the 00415950 tail)
    if (!(canvas_.flags & flag_enabled)) return;
    if (g.pad_state == 0 || g.pad_state == 3) ++g.no_pad_frames;
    else g.no_pad_frames = 0;
    if (g.no_pad_frames >= 3) {
        if (g.no_pad_popup != -1 && popup_query(g.no_pad_popup)) return;
        const auto title = host_.get_string("NoCtrlTitle");
        if (!title) unhosted(0x465930, "NULL NoCtrlTitle string");
        const auto msg = host_.get_string("NoCtrlMessage");
        if (!msg) unhosted(0x465948, "NULL NoCtrlMessage string");
        g.no_pad_popup = popup_open(1, title, msg, nullptr, 0);
        host_.app_131470(1);
    } else if (g.no_pad_popup != -1) {
        popup_close(g.no_pad_popup);
        g.no_pad_popup = -1;
        if (!g.controller || g.controller_6a8 == 0) host_.app_131470(0);
    }
}

void GuiRuntime::update_stack(float dt) {  // 00415950
    if (!(canvas_.flags & flag_enabled)) return;
    last_dt = dt;
    const float f = fmul(dt, 30.0f);
    if (pending) {
        pop(pending_pop);
        pending_pop = nullptr;
        if (pending_push) {
            push(pending_push, 0);
            pending_push = nullptr;
        }
        pending = 0;
    }
    pending_list_ = canvas_.children;
    iterating_ = true;
    while (!pending_list_.empty()) {
        Widget* s = pending_list_.front();
        if (s->flags & flag_enabled) {
            OMEGA_ZONE("gui.v_update(stack)");
            v_update(s, f);
            if (!pending_list_.empty() && pending_list_.front() == s) pending_list_.erase(pending_list_.begin());
        } else {
            pending_list_.erase(pending_list_.begin());
        }
    }
    iterating_ = false;
}

void GuiRuntime::update(float dt) {  // 00465500
    controller_check();
    if (stack_observer) {
        OMEGA_ZONE("gui.stack_observer");
        stack_observer();
    }
    update_stack(dt);
    if (game_canvas) return;  // the game canvas keeps no tempbackground phase
    phase = fadd(phase, fmul(30.0f, dt));
    if (!(phase <= 300.0f)) phase = fsub(phase, 300.0f);
}

void GuiRuntime::nested_update(float dt) {  // 00415b60
    if (!(canvas_.flags & flag_enabled)) return;
    tick_value = fmul(dt, 30.0f);
    broadcast(&canvas_, 7, tick_arg, 0);
}

// ---------------------------------------------------------------- scripted run

std::int64_t GuiScript::config(const std::string& key) const {
    for (auto it = cfg.rbegin(); it != cfg.rend(); ++it)
        if (it->first == key) return it->second;
    static const std::map<std::string, std::int64_t> defaults = {
        {"demo", 0}, {"mc59d", 1}, {"mc59e", 0}, {"mc5a4", 0},  {"mc5a8", 0}, {"need", 0},   {"p334100", 0},
        {"profile", 0}, {"app260", 1}, {"pad", 6}, {"caba0", 0}, {"bb1", 0}, {"t1base", 0}, {"ctl", -1},
        {"popup", 100}, {"save0", 0}, {"b38", 0}, {"eeb0", 0}, {"s330", 0}, {"kb51", 0}, {"kb52", 1},
        {"lang", 0}, {"a701", 0}, {"kbmod", 0}, {"font0", 0}, {"fh", 0x10}, {"a2e", 0}, {"load", 0},
        {"b506488", 0}, {"b506490", 0}, {"b51eea0", 0}, {"b51eea8", 0}, {"b5723d0", 0}, {"b532fe8", 0},
        {"a30", 0}, {"port_lan", 0}, {"a31", 0}, {"a32", 0}, {"clp", 0}, {"done", 0}, {"f8", 0}, {"zeus", 0}, {"ukc", 0},
        {"pb68", 0}, {"pb72", 0}, {"pb73", 0}, {"b868", 1}, {"bb5", 1}, {"svp", 0}, {"svi", -1},
        {"oc10", 0}, {"p9a0", 0}, {"lan_mode", 0}, {"lan_hosting", 0}, {"lan_host_ok", 1},
        {"lan_reply", 1}, {"lan_join_ok", 1},
        // checkpoint 132on2: the DME session and the SFOLobby hooks' results
        {"sess", 0}, {"cfgok", 1}, {"neterr", 0}, {"connok", 1}, {"conn_dnas", 0}, {"dnasres", 0}, {"muisbegin", 1},
        {"muisok", 1}, {"unisel", 1}, {"unidef", 1}, {"polok", 1}, {"privok", 1}, {"unread", 0}, {"unilist", 1},
        // checkpoint 132on2b: GuiAccountLogin's SFOLobby account calls
        {"acnt", 0}, {"loginok", 1}, {"regok", 1}, {"checkok", 1}, {"enterok", 1}, {"regerr", -1},
        // checkpoint 132on2b: GuiCommandCenter online (the lobby state +255c, 00442510)
        {"lstate", 9}, {"missionok", 1},
        // checkpoint 132on2b stages B3 / B4: the room, the create and the update's callback
        {"roomok", 1}, {"createok", 1}, {"cb", 1}, {"cba3", 1}, {"cbt0", 0},
        // checkpoint 132on2b stage B4: GuiNetJoinMission's game info, join, account id and the join wait's clock
        {"gi", 1}, {"gimax", 4}, {"giowner", 0}, {"gimission", 0}, {"gipw", 0}, {"joinok", 1}, {"ownid", 0},
        {"clkstep", 1000},
        // checkpoint 132on2c: the chat send 00451050's result and the session's +1ea4 (MAX_CHAT)
        {"chatok", 1}, {"maxchat", 0},
        // checkpoint 132on2h: GuiNetContacts' AccountGetID, add and remove
        {"acctid", 7}, {"acctiderr", 0}, {"addok", 1}, {"adderr", 0}, {"rmok", 1},
        // checkpoint 132on2i: the game info's GameName (0: "A" with gipw); the agency cell (0044a230 / 0044af60)
        {"giname", 0}, {"cellmember", 0}, {"cellok", 0}, {"cellerr", 2}};
    const auto d = defaults.find(key);
    if (d == defaults.end()) throw std::runtime_error("gui script: unknown cfg key " + key);
    return d->second;
}

namespace {

std::vector<std::string> split_space(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const auto p = line.find(' ', start);
        if (p == std::string::npos) {
            out.push_back(line.substr(start));
            return out;
        }
        out.push_back(line.substr(start, p - start));
        start = p + 1;
    }
}

std::int64_t to_int(const std::string& s, int base = 10) {
    std::size_t used = 0;
    const long long v = std::stoll(s, &used, base);
    if (used != s.size()) throw std::runtime_error("gui script: bad number " + s);
    return v;
}

// "-" is the empty string.
std::string from_hex(const std::string& h) {
    std::string s;
    if (h == "-") return s;
    if (h.size() % 2) throw std::runtime_error("gui script: odd hex " + h);
    for (std::size_t k = 0; k < h.size(); k += 2) s.push_back(static_cast<char>(to_int(h.substr(k, 2), 16)));
    return s;
}

}  // namespace

GuiScript parse_gui_script(const std::string& text) {
    GuiScript c;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        auto end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        const auto p = split_space(line);
        const std::string& k = p[0];
        auto arg = [&](std::size_t i) -> const std::string& {
            if (i >= p.size()) throw std::runtime_error("gui script: short line " + line);
            return p[i];
        };
        if (k == "root") c.root = arg(1);
        else if (k == "native") c.native = to_int(arg(1)) != 0;
        else if (k == "msgbox") c.msgbox = to_int(arg(1)) != 0;
        else if (k == "res") c.res.emplace_back(arg(1), static_cast<int>(to_int(arg(2))));
        else if (k == "main") c.main = static_cast<int>(to_int(arg(1)));
        else if (k == "layer") c.layer = static_cast<int>(to_int(arg(1)));
        else if (k == "iface") c.iface = line.size() > 6 ? line.substr(6) : std::string();
        else if (k == "screen") c.screens.push_back(static_cast<int>(to_int(arg(1))));
        else if (k == "cdvd") c.cdvd = to_int(arg(1)) != 0;
        else if (k == "kbd") c.kbd = static_cast<int>(to_int(arg(1)));
        else if (k == "str") {
            const std::string rest = line.substr(4);
            const auto eq = rest.find('=');
            if (eq == std::string::npos) throw std::runtime_error("gui script: bad str line " + line);
            c.strings.emplace_back(rest.substr(0, eq), rest.substr(eq + 1));
        } else if (k == "strnull") c.strings.emplace_back(arg(1), std::nullopt);
        else if (k == "fact") c.factory.emplace_back(arg(1), static_cast<int>(to_int(arg(2))));
        else if (k == "dir") {
            for (std::size_t i = 1; i < p.size(); ++i) c.dir.push_back(from_hex(p[i]));
        } else if (k == "stop") c.stop.push_back(arg(1));
        else if (k == "cc") c.cc = to_int(arg(1)) != 0;
        else if (k == "prof") c.prof.emplace_back(arg(1), from_hex(arg(2)));
        else if (k == "profnull") c.prof.emplace_back(arg(1), std::nullopt);
        else if (k == "bvec") {
            c.bvec.clear();
            for (std::size_t i = 1; i < p.size(); ++i) {
                const auto col = p[i].find(':');
                if (col == std::string::npos) throw std::runtime_error("gui script: bad bvec " + p[i]);
                c.bvec.emplace_back(static_cast<std::int32_t>(to_int(p[i].substr(0, col))),
                                    static_cast<std::uint8_t>(to_int(p[i].substr(col + 1))));
            }
        } else if (k == "bitem") {
            std::vector<std::pair<std::optional<std::string>, std::uint8_t>> rows;
            for (std::size_t i = 2; i < p.size(); ++i) {
                const auto col = p[i].find(':');
                if (col == std::string::npos) throw std::runtime_error("gui script: bad bitem " + p[i]);
                const std::string n = p[i].substr(0, col);
                rows.emplace_back(n == "-" ? std::optional<std::string>() : std::optional<std::string>(from_hex(n)),
                                  static_cast<std::uint8_t>(to_int(p[i].substr(col + 1))));
            }
            c.bitems.emplace_back(static_cast<std::int32_t>(to_int(arg(1))), std::move(rows));
        } else if (k == "skey") c.skey.emplace_back(static_cast<std::int32_t>(to_int(arg(1))), from_hex(arg(2)));
        else if (k == "lgame") {  // PORT-ONLY (132ln4)
            GuiHost::LanGame gm;
            gm.session_id = static_cast<std::uint64_t>(std::stoull(arg(1), nullptr, 16));
            gm.mission = static_cast<std::int32_t>(to_int(arg(2)));
            gm.agents_now = static_cast<std::int32_t>(to_int(arg(3)));
            gm.agents_max = static_cast<std::int32_t>(to_int(arg(4)));
            gm.join_filter = static_cast<std::uint8_t>(to_int(arg(5)));
            gm.state = static_cast<std::uint8_t>(to_int(arg(6)));
            gm.mismatch = to_int(arg(7)) != 0;
            gm.host_name = arg(8);
            c.lgame.push_back(std::move(gm));
        }
        else if (k == "lcfg") c.lcfg.push_back({static_cast<std::uint8_t>(to_int(arg(1))), from_hex(arg(2))});
        else if (k == "lsrv") c.lsrv.push_back({from_hex(arg(1)), from_hex(arg(2))});
        else if (k == "luni") c.luni.push_back({static_cast<std::int32_t>(to_int(arg(1))), from_hex(arg(2))});
        else if (k == "lann") c.lann.push_back({static_cast<std::int32_t>(to_int(arg(1))), from_hex(arg(2))});
        else if (k == "lmsg")
            c.lmsg.push_back({static_cast<std::int32_t>(to_int(arg(1))), static_cast<std::int32_t>(to_int(arg(2))),
                              from_hex(arg(3)), from_hex(arg(4))});
        else if (k == "lpol") c.lpol.emplace_back(static_cast<int>(to_int(arg(1))), from_hex(arg(2)));
        else if (k == "sett") c.sett.emplace_back(arg(1), from_hex(arg(2)));
        else if (k == "cfg") {
            for (std::size_t i = 1; i < p.size(); ++i) {
                const auto eq = p[i].find('=');
                if (eq == std::string::npos) throw std::runtime_error("gui script: bad cfg " + p[i]);
                c.cfg.emplace_back(p[i].substr(0, eq), to_int(p[i].substr(eq + 1)));
            }
        } else if (k == "frame") {
            GuiScript::Frame f;
            f.pad0 = static_cast<std::uint32_t>(to_int(arg(1), 16));
            f.pad1 = static_cast<std::uint32_t>(to_int(arg(2), 16));
            f.t1 = static_cast<std::uint32_t>(to_int(arg(3), 16));
            f.ticks = to_int(arg(4));
            f.dt = static_cast<std::uint32_t>(to_int(arg(5), 16));
            f.ms = static_cast<std::int32_t>(static_cast<std::uint32_t>(to_int(arg(6))));
            f.save = static_cast<std::uint8_t>(to_int(arg(7)));
            for (std::size_t i = 8; i < p.size(); ++i) f.ops.push_back(p[i]);
            c.frames.push_back(std::move(f));
        } else {
            throw std::runtime_error("gui script: bad line " + line);
        }
    }
    return c;
}

namespace {

// The generator's hooks (tools/ee/gen_gui_runtime.py, Runner.install).
class ScriptHost final : public GuiHost {
public:
    ScriptHost(const GuiScript& s, std::vector<std::string>& log) : script_(s), log_(log) {
        popup_id_ = static_cast<std::int32_t>(s.config("popup"));
        save = static_cast<std::uint8_t>(s.config("save0"));
    }

    const GuiRuntime* runtime = nullptr;
    std::vector<Widget*> screens;
    std::int32_t ms = 0;
    std::uint8_t save = 0;
    bool lan_play = false;  // PORT-ONLY (132ln1): cfg port_lan
    bool port_lan_play() override { return lan_play; }

    std::string name(const Widget* w) const {
        if (!w) return "-";
        if (runtime && w == &runtime->canvas()) return "canvas";
        return w->name;
    }

    std::optional<std::string> get_string(const std::string& key) override {
        log_.push_back("gs " + key);
        for (auto it = script_.strings.rbegin(); it != script_.strings.rend(); ++it)
            if (it->first == key) return it->second;
        return "<" + key + ">";
    }
    std::uint32_t font(const std::string& n) override {
        // checkpoint 118e scripts (an explicit "font0"): 00418420's NULL / "" path, the manager's default
        if (n.empty() && has_cfg("font0")) return default_font();
        log_.push_back("font " + n);
        std::uint32_t sum = 0x10000;
        for (unsigned char ch : n) sum += ch;
        return sum;
    }
    std::uint32_t default_font() override {
        log_.push_back("font -");
        return static_cast<std::uint32_t>(script_.config("font0"));
    }
    std::uint8_t font_height(std::uint32_t f) override {
        log_.push_back("fh " + hex(f));
        return static_cast<std::uint8_t>(static_cast<std::uint32_t>(script_.config("fh")) + f);
    }
    // 0036aa50 in the 132on2 (sess) scripts: the stand-in fonts have no glyph table, so every character counts 8.
    std::int32_t font_width(std::uint32_t font, const std::string& s, std::int32_t n) override {
        if (!script_.config("sess")) return GuiHost::font_width(font, s, n);
        std::int32_t k = 0;
        while (k < n && static_cast<std::size_t>(k) < s.size() && s[static_cast<std::size_t>(k)] != '\0') ++k;
        return 8 * k;
    }
    bool has_cfg(const std::string& key) const {
        for (const auto& kv : script_.cfg)
            if (kv.first == key) return true;
        return false;
    }
    std::int32_t ie_lookup(const std::string& n) override {
        log_.push_back("ie " + n);
        return -1;
    }
    std::int32_t demo_mode() override {
        log_.push_back("demo");
        return static_cast<std::int32_t>(script_.config("demo"));
    }
    std::int32_t time_ms() override { return ms; }
    std::uint8_t save_present() override {
        log_.push_back("save");
        return save;
    }
    void card_refresh() override { log_.push_back("mcrefresh"); }
    std::int32_t card_space_needed(std::int32_t size) override {
        log_.push_back("need " + std::to_string(size));
        return static_cast<std::int32_t>(script_.config("need"));
    }
    std::int32_t profile_size(std::uint32_t profile) override {
        log_.push_back("p334100 " + hex(profile));
        return static_cast<std::int32_t>(script_.config("p334100"));
    }
    std::int32_t current_profile() override {
        log_.push_back("profile");
        return static_cast<std::int32_t>(script_.config("profile"));
    }
    void profile_select(std::uint32_t profile) override { log_.push_back("p3361b0 " + hex(profile)); }
    void app_2c9af0(std::int32_t v) override { log_.push_back("p2c9af0 " + std::to_string(v)); }
    void app_131470(std::int32_t v) override { log_.push_back("p131470 " + std::to_string(v)); }
    void app_2c82f0() override { log_.push_back("p2c82f0"); }
    void app_2c81d0() override { log_.push_back("p2c81d0"); }
    // checkpoint 132ln2: GuiOnlineConnect's card wait 0040d5d0 and music stop 002c9aa0 (the generator's hooks), the
    // agent's +9a0 word (cfg p9a0; read from memory, not logged)
    void card_sync() override { log_.push_back("mcsync"); }
    void app_2c9aa0() override { log_.push_back("p2c9aa0"); }
    // checkpoint 132ln3: the agent's online region (+95c, read from memory: not logged), 0012ee30 and 00332a10
    // (the generator's hooks), and the PORT-ONLY LAN session (cfg lan_hosting / lan_host_ok; no golden)
    std::string profile_text(std::int32_t p, std::uint32_t offset) override {
        if (offset == 0x95c) return region_ ? *region_ : prof("region").value_or("");
        if (offset == 0x8cc) {  // checkpoint 132on2b: the online password (prof pass with cfg acnt; memory)
            if (password_) return *password_;
            return script_.config("acnt") ? prof("pass").value_or("").substr(0, 15) : std::string();
        }
        return GuiHost::profile_text(p, offset);
    }
    // checkpoint 132on2b: 00129360 into the agent's password +8cc (a memory write: not logged)
    std::optional<std::string> password_;
    void profile_set_text(std::int32_t p, std::uint32_t offset, const std::string& s) override {
        if (offset == 0x95c) {  // checkpoint 132on2c: ONLIPREF's universe name (0x40 bytes)
            region_ = s.substr(0, 0x3f);
            return;
        }
        if (offset != 0x8cc) GuiHost::profile_set_text(p, offset, s);
        password_ = s;
    }
    void profile_set_word(std::int32_t p, std::uint32_t offset, std::int32_t v) override {
        if (offset != 0x9a0) return GuiHost::profile_set_word(p, offset, v);
        p9a0_ = v;  // checkpoint 132on2c: ONLIPREF's universe id
    }
    void profiles_clear_b70() override { log_.push_back("p12ee30"); }
    void level_progress(std::int32_t p, std::int32_t level, std::int32_t a, std::int32_t b) override {
        log_.push_back("p2a10 " + hex(static_cast<std::uint32_t>(p)) + " " + std::to_string(level) + " " +
                       std::to_string(a) + " " + std::to_string(b));
    }
    bool lan_hosting() override { return script_.config("lan_hosting") != 0; }
    // PORT-ONLY (132ln4): the LAN join services (logged; the clock steps 1 s per read so a wait loop ends)
    bool lan_joined_ = false;
    std::int64_t lan_clock_ = 0;
    void lan_browse(bool on) override { log_.push_back(std::string("lanbrowse ") + (on ? "1" : "0")); }
    std::vector<LanGame> lan_games() override { return script_.lgame; }
    bool lan_join(std::uint64_t sid) override {
        log_.push_back("lanjoin " + hex(static_cast<std::uint32_t>(sid >> 32)) + ":" + hex(static_cast<std::uint32_t>(sid)));
        lan_joined_ = script_.config("lan_join_ok") != 0;
        return lan_joined_;
    }
    LanJoin lan_join_state() override {
        if (!lan_joined_) return LanJoin::idle;
        switch (script_.config("lan_reply")) {
        case 1: return LanJoin::accepted;
        case 2: return LanJoin::rejected;
        case 3: return LanJoin::failed;
        default: return LanJoin::pending;
        }
    }
    void lan_join_cancel() override {
        log_.push_back("lancancel");
        lan_joined_ = false;
    }
    std::optional<std::string> lan_join_notice() override {  // 132md5: the script's "str PORT_LAN_NOTICE=..."
        for (auto it = script_.strings.rbegin(); it != script_.strings.rend(); ++it)
            if (it->first == "PORT_LAN_NOTICE") return it->second;
        return std::nullopt;
    }
    std::int64_t lan_clock_ms() override { return lan_clock_ += 1000; }
    // The LAN join's wait loop polls 002cc260(0, 0) (a nested GUI frame: none in the script runner); elsewhere the
    // poll stays unhosted as before.
    void card_poll() override {
        if (session) {  // checkpoint 132on2b: the lobby waits' 002cc260(0, 0), logged
            // (checkpoint 132on2c: hooked for MSNSETUP / JOINMIS only (Runner.install_msj); elsewhere, as ONLIPREF's
            // agent save, the native nested frame traces nothing in the goldens; 132on2i: CONTACTS' join, as
            // Runner.install_ct)
            if (script_.root == "GuiMissionSetup" || script_.root == "GuiNetJoinMission" ||
                script_.root == "GuiNetContacts")
                log_.push_back("p2cc260");
            return;
        }
        if (!lan_joined_) GuiHost::card_poll();
    }
    bool lan_host(const LanHostRequest& r) override {
        log_.push_back("lanhost " + r.host_name + " " + std::to_string(r.mission) + " " + std::to_string(r.num_agents) +
                       " " + std::to_string(r.join_filter));
        return script_.config("lan_host_ok") != 0;
    }
    std::int32_t profile_word(std::int32_t p, std::uint32_t offset) override {
        if (offset == 0x9a0) return p9a0_ ? *p9a0_ : static_cast<std::int32_t>(script_.config("p9a0"));
        // checkpoint 132on2c: ONLIPREF's agent save 00441a10 reads the agent slot +4 (0 on the zeroed stand-in)
        if (offset == 4 && session) return 0;
        return GuiHost::profile_word(p, offset);
    }
    // checkpoint 132on2: the DME session (cfg sess; 00429500(0) in the leave ends it) and the SFOLobby calls (the
    // generator's hooks, Runner.install_on: logged alike, the rows from the script's lcfg / lsrv / luni / lann / lpol)
    bool session = false;
    bool unread = false;
    std::function<void()> dnas_event;  // the 0044bab0 hook's 0x2002 event to the screen (cfg conn_dnas)
    bool net_session() override { return session; }
    std::int32_t cfg32(const char* k) const { return static_cast<std::int32_t>(script_.config(k)); }
    std::int32_t lobby_configs(std::vector<LobbyConfig>& configs, std::vector<LobbyServer>& servers) override {
        configs = script_.lcfg;
        servers = script_.lsrv;
        const std::int32_t r = cfg32("cfgok");
        log_.push_back("cfglist -> " + std::to_string(r));
        return r;
    }
    void lobby_disconnect() override { log_.push_back("p44c3c0"); }
    std::int32_t lobby_error() override {
        const std::int32_t r = neterr_ ? *neterr_ : cfg32("neterr");
        log_.push_back("neterr -> " + std::to_string(r));
        return r;
    }
    std::int32_t lobby_connect(std::int32_t row) override {
        const std::int32_t r = cfg32("connok");
        log_.push_back("connect " + std::to_string(row) + " -> " + std::to_string(r));
        if (script_.config("conn_dnas") && dnas_event) dnas_event();
        return r;
    }
    std::int32_t lobby_dnas_result() override {
        const std::int32_t r = cfg32("dnasres");
        log_.push_back("dnasres -> " + std::to_string(r));
        return r;
    }
    std::int32_t lobby_universes_begin() override {
        const std::int32_t r = cfg32("muisbegin");
        log_.push_back("muisbegin -> " + std::to_string(r));
        return r;
    }
    std::string setting(const std::string& key) override {
        log_.push_back("setting " + key);
        for (auto it = script_.sett.rbegin(); it != script_.sett.rend(); ++it)
            if (it->first == key) return it->second;
        return std::string();
    }
    std::int32_t lobby_muis_connect(std::int32_t server, std::vector<LobbyUniverse>& universes) override {
        universes = script_.luni;
        const std::int32_t r = cfg32("muisok");
        log_.push_back("muis " + std::to_string(server) + " -> " + std::to_string(r));
        return r;
    }
    std::int32_t lobby_universe_select(std::int32_t i) override {
        const std::int32_t r = cfg32("unisel");
        log_.push_back("unisel " + std::to_string(i) + " -> " + std::to_string(r));
        return r;
    }
    std::int32_t lobby_universe_default() override {
        const std::int32_t r = cfg32("unidef");
        log_.push_back("unidef -> " + std::to_string(r));
        return r;
    }
    void lobby_universes_end() override { log_.push_back("p442e90"); }
    std::int32_t lobby_policy_begin() override {
        log_.push_back("polbegin");
        return 1;
    }
    std::int32_t lobby_announcements(std::vector<LobbyAnnouncement>& out) override {
        out = script_.lann;
        log_.push_back("ann n=" + std::to_string(out.size()));
        return 1;
    }
    std::int32_t lobby_policy(bool privacy, std::vector<std::string>& out) override {
        for (const auto& [k, t] : script_.lpol)
            if ((k != 0) == privacy) out.push_back(t);
        const std::int32_t r = cfg32(privacy ? "privok" : "polok");
        log_.push_back(std::string("pol ") + (privacy ? "1" : "0") + " -> " + std::to_string(r));
        return r;
    }
    void lobby_policy_end() override { log_.push_back("p445dc0"); }
    void lobby_leave(std::int32_t a1) override {
        log_.push_back("p2c8090");
        log_.push_back("p453b00 " + std::to_string(a1));
        session = false;  // 00429500(0)
    }
    std::optional<std::string> lobby_notice() override { return std::nullopt; }  // session +3c is 0
    // checkpoint 132on2b: the SFOLobby account calls and the channel join (Runner.install_acl: cfg loginok / regok /
    // checkok / enterok; regerr, when set, the registration's own error for the next 00452ff0)
    std::optional<std::int32_t> neterr_;
    std::int32_t account_call(const char* tag, const char* key, const std::string& n, const std::string& pw) {
        const std::int32_t r = cfg32(key);
        log_.push_back(std::string(tag) + " " + n + " " + pw + " -> " + std::to_string(r));
        return r;
    }
    std::int32_t lobby_login(const std::string& n, const std::string& pw) override {
        return account_call("login", "loginok", n, pw);
    }
    std::int32_t lobby_register(const std::string& n, const std::string& pw) override {
        const std::int32_t r = account_call("register", "regok", n, pw);
        if (cfg32("regerr") >= 0) neterr_ = cfg32("regerr");
        return r;
    }
    std::int32_t lobby_check_account(const std::string& n, const std::string& pw) override {
        return account_call("acheck", "checkok", n, pw);
    }
    std::int32_t lobby_enter() override {
        const std::int32_t r = cfg32("enterok");
        log_.push_back("enter -> " + std::to_string(r));
        return r;
    }
    // checkpoint 132on2b (Runner.install_ccs): the stand-in session's +255c (cfg lstate), the mission channel
    // 00442510 (cfg missionok) and 002c8090 (hooked, logged)
    std::int32_t lobby_state() override { return lstate_ ? *lstate_ : cfg32("lstate"); }
    std::int32_t lobby_mission(std::int32_t mission) override {
        const std::int32_t r = cfg32("missionok");
        log_.push_back("mission " + std::to_string(mission) + " -> " + std::to_string(r));
        return r;
    }
    std::int32_t lobby_messages() override {
        log_.push_back("p2c8090");
        return 1;
    }
    // checkpoint 132on2b stages B3 / B4 (Runner.install_msj): the room and game calls, logged; the room calls keep
    // the stand-in session's +255c as 004426b0 / 004518f0 / 00451610 do; the update 004536b0 delivers the script's
    // callback (cfg cb, cba3, cbt0) once.
    std::optional<std::int32_t> lstate_;
    bool cb_sent_ = false;
    void lobby_channel_leave() override {
        log_.push_back("p4426b0");
        lstate_ = 9;
    }
    std::int32_t lobby_room_open() override {
        const std::int32_t r = cfg32("roomok");
        log_.push_back("room -> " + std::to_string(r));
        if (lobby_state() == 9) room_ = r != 0;  // 004518f0 registers the screen's lists +23ec .. (132on2c)
        if (r) lstate_ = 5;
        return r;
    }
    void lobby_room_leave() override {
        log_.push_back("p451610");
        const std::int32_t s = lobby_state();
        if (s == 5 || s == 0) lstate_ = 9;
    }
    void lobby_list_kind(std::int32_t kind) override { log_.push_back("listkind " + std::to_string(kind)); }
    void lobby_net_start() override {
        log_.push_back("p1698c0");
        log_.push_back("p165bc0");
    }
    std::int32_t lobby_create(std::int32_t mission, std::int32_t agents, std::int32_t filter) override {
        const std::int32_t r = cfg32("createok");
        log_.push_back("create " + std::to_string(mission) + " " + std::to_string(agents) + " " +
                       std::to_string(filter) + " -> " + std::to_string(r));
        return r;
    }
    std::optional<LobbyCallback> lobby_pump() override {
        log_.push_back("p4536b0");
        if (cb_sent_ || !cfg32("cb")) return std::nullopt;
        cb_sent_ = true;
        return LobbyCallback{cfg32("cba3"), cfg32("cbt0")};
    }
    void lobby_game_cleanup() override { log_.push_back("p42b1f0"); }
    // checkpoint 132on2b stage B4 (Runner.install_jms): the game info 0044e060 (cfg gi / gimax / giowner / gimission /
    // gipw), the join 0044e430 (cfg joinok), 0044e340 and 00429d30 (logged), +255c set silently, the account id
    // +2480 (cfg ownid), the join wait's clock (cfg clkstep per call, from 0) and the script's game lists (op glist)
    std::int64_t clk_ = 0;
    LobbyGameList glist_;
    std::optional<LobbyGameInfo> lobby_game_info(std::int32_t world) override {
        const std::int32_t v = cfg32("gi");
        log_.push_back("ginfo " + std::to_string(world) + " -> " + std::to_string(v));
        if (!v) return std::nullopt;
        // checkpoint 132on2i: cfg giname picks the GameName +134 (gen_gui_runtime.py GINAMES; 0: "A" with gipw)
        static const char* const names[] = {"", "Gabe\t2\t3\t1\t42", "PPPPPPPPPPPPPPPP\t2\t3\t1\t42", "\t2\t3\t1\t42",
                                            "QQQQQQQQQQQQQQQ"};
        const std::int32_t pick = cfg32("giname");
        std::string name = pick > 0 && pick < 5 ? names[pick] : cfg32("gipw") ? "A" : "";
        LobbyGameInfo info{cfg32("gimax"), cfg32("giowner"), cfg32("gimission"), !name.empty(), name};
        return info;
    }
    // checkpoint 132on2i (op gcache): the game cache +118 as 0044edc0 keeps it; the entries it added or changed
    std::vector<LobbyCachedGame> gcache_, gcache_new_;
    std::optional<LobbyCachedGame> lobby_game_cached(std::int32_t world) override {
        if (world == 0) return std::nullopt;
        for (const auto& e : gcache_)
            if (e.world == world) return e;
        return std::nullopt;
    }
    std::vector<LobbyCachedGame> lobby_game_cache_take() override {
        auto out = std::move(gcache_new_);
        gcache_new_.clear();
        return out;
    }
    std::int32_t lobby_join(std::int32_t world) override {
        const std::int32_t v = cfg32("joinok");
        log_.push_back("join " + std::to_string(world) + " -> " + std::to_string(v));
        return v;
    }
    void lobby_join_end() override { log_.push_back("p44e340"); }
    void lobby_set_state(std::int32_t state) override { lstate_ = state; }
    void net_update() override { log_.push_back("p429d30"); }
    std::int64_t lobby_clock_ms() override { return clk_ += script_.config("clkstep"); }
    std::int32_t lobby_own_id() override { return cfg32("ownid"); }
    LobbyGameList lobby_game_list() override { return glist_; }
    // checkpoint 132on2c (Runner.install_jms): the chat send 00451050 (cfg chatok, logged), the chat records the
    // script's chat op hands 00447bb0 / 00447c70 (kept while the room's lists are registered, +23ec; a private line
    // without them is the session's message of type 9) and the player lists of op plist (00447a30)
    bool room_ = false;
    std::vector<LobbyChatLine> chat_;
    LobbyPlayerList plist_;
    std::int32_t lobby_chat_send(const std::string& text, std::int32_t target) override {
        const std::int32_t r = cfg32("chatok");
        log_.push_back("chat " + std::to_string(target) + " '" + text + "' -> " + std::to_string(r));
        return r;
    }
    std::vector<LobbyChatLine> lobby_chat_take() override {
        auto out = std::move(chat_);
        chat_.clear();
        return out;
    }
    LobbyPlayerList lobby_player_list() override { return plist_; }
    // checkpoint 132on2h (Runner.install_ct): the contacts 0044df80 / 0044dd20 (logged; the stand-in's +255c as they
    // keep it, the chat records kept while 0044df80's lists are registered), the contact lists of op clist (00446830),
    // AccountGetID 00451370 (cfg acctid, acctiderr: the next 00452ff0), the add 0044db60 (cfg addok, adderr) and the
    // remove 0044d9c0 (cfg rmok)
    LobbyContactList clist_;
    std::int32_t lobby_contacts_open() override {
        log_.push_back("p44df80");
        const std::int32_t s = lobby_state();
        if (s != 9 && s != 0) {
            neterr_ = 0x21;
            return 0;
        }
        lstate_ = 6;
        room_ = true;
        return 1;
    }
    void lobby_contacts_close() override {
        log_.push_back("p44dd20");
        const std::int32_t s = lobby_state();
        if (s != 6 && s != 10) return;
        if (s == 6) lstate_ = 9;
        room_ = false;
    }
    LobbyContactList lobby_contact_list() override { return clist_; }
    std::int32_t lobby_contact_remove(std::int32_t id) override {
        const std::int32_t r = cfg32("rmok");
        log_.push_back("rmcontact " + std::to_string(id) + " -> " + std::to_string(r));
        return r;
    }
    std::int32_t lobby_account_id(const std::string& name) override {
        const std::int32_t r = cfg32("acctid");
        log_.push_back("acctid '" + name + "' -> " + std::to_string(r));
        neterr_ = cfg32("acctiderr");
        return r;
    }
    std::int32_t lobby_contact_add(std::int32_t id) override {
        const std::int32_t r = cfg32("addok");
        log_.push_back("addcontact " + std::to_string(id) + " -> " + std::to_string(r));
        if (!r) neterr_ = cfg32("adderr");
        return r;
    }
    // checkpoint 132on2i: the agency cell's member test 0044a230 and CreateClan 0044af60 (the EE's install_ccs)
    bool lobby_cell_member() override {
        const std::int32_t r = cfg32("cellmember");
        log_.push_back("cellmember -> " + std::to_string(r));
        return r != 0;
    }
    std::int32_t lobby_cell_create(const std::string& name) override {
        const std::int32_t r = cfg32("cellok");
        log_.push_back("cellcreate '" + name + "' -> " + std::to_string(r));
        if (!r) neterr_ = cfg32("cellerr");
        return r;
    }
    // the stand-in session is zeroed (cfg maxchat, else 0)
    std::int32_t lobby_max_chat() override { return cfg32("maxchat"); }
    // checkpoint 132on2c: the session's message list +1c (the script's lmsg rows, set up by 0044f880 / 0044f470: its
    // count +20 is the poll's "unread", its +18 changed byte set), 00451010 read from memory, the answer 00450f30
    // hooked (logged; 0044f2e0 removes the row) and ONLIPREF's universe list 00442d80 (cfg unilist, the luni rows)
    std::vector<LobbyMessage> msgs_;
    bool msgs_changed_ = false;
    bool lobby_unread() override { return msgs_.empty() ? unread : true; }
    std::int32_t lobby_message_count() override {
        return msgs_.empty() ? (unread ? 1 : 0) : static_cast<std::int32_t>(msgs_.size());
    }
    std::optional<LobbyMessage> lobby_message(std::int32_t i) override {
        if (i < 0 || static_cast<std::size_t>(i) >= msgs_.size()) return std::nullopt;
        return msgs_[static_cast<std::size_t>(i)];
    }
    void lobby_message_answer(std::int32_t i, std::int32_t accept) override {
        log_.push_back("msgans " + std::to_string(i) + " " + std::to_string(accept));
        if (i < 0 || static_cast<std::size_t>(i) >= msgs_.size()) return;
        msgs_.erase(msgs_.begin() + i);
    }
    bool lobby_messages_changed() override { return msgs_changed_; }
    void lobby_messages_set_changed(bool c) override { msgs_changed_ = c; }
    std::int32_t lobby_universe_list(std::vector<LobbyUniverse>& out) override {
        out = script_.luni;
        const std::int32_t r = static_cast<std::int32_t>(script_.config("unilist"));
        log_.push_back("unilist -> " + std::to_string(r));
        return r;
    }
    // the font 00418420("default") NETMESS's init colours (+10 / +20): kept for the state line
    std::optional<std::pair<std::array<float, 4>, std::array<float, 4>>> nm_colors;
    void font_colors(std::uint32_t, const std::array<float, 4>& a, const std::array<float, 4>& b) override {
        nm_colors.emplace(a, b);
    }
    // the agent's universe +9a0 / +95c stores (00129360; memory, not logged)
    std::optional<std::int32_t> p9a0_;
    std::optional<std::string> region_;
    std::int32_t popup_open(std::int32_t kind, const std::optional<std::string>& title,
                            const std::optional<std::string>& message, const Widget* owner, std::int32_t arg) override {
        const std::int32_t id = popup_id_++;
        auto q = [](const std::optional<std::string>& s) { return s ? "\"" + *s + "\"" : std::string("(null)"); };
        log_.push_back("popup " + std::to_string(kind) + " " + q(title) + " " + q(message) + " " + name(owner) + " " +
                       std::to_string(arg) + " -> " + std::to_string(id));
        return id;
    }
    void popup_close(std::int32_t id) override { log_.push_back("popclose " + std::to_string(id)); }
    std::int32_t popup_query(std::int32_t id) override {
        const auto r = static_cast<std::int32_t>(script_.config("caba0"));
        log_.push_back("popq " + std::to_string(id) + " -> " + std::to_string(r));
        return r;
    }
    std::uint8_t sound(std::uint32_t table, const Widget* sender, bool alternate) override {
        log_.push_back(std::string(alternate ? "snd2 " : "snd ") + hex(table) + " " + name(sender));
        return 1;
    }
    void pad_reset() override { log_.push_back("padreset"); }
    void text_object(const TextObject& o) override {
        char buf[160];
        std::snprintf(buf, sizeof buf, "tobj %d %d %d %d %x %08x %08x %08x %08x", o.rect[0], o.rect[1], o.rect[2],
                      o.rect[3], o.font, std::bit_cast<std::uint32_t>(o.rgba[0]),
                      std::bit_cast<std::uint32_t>(o.rgba[1]), std::bit_cast<std::uint32_t>(o.rgba[2]),
                      std::bit_cast<std::uint32_t>(o.rgba[3]));
        log_.push_back(buf);
    }
    void set_text(const std::string& text) override { log_.push_back("settext " + text); }
    Widget* factory(const std::string& cls, const std::string& file) override {
        Widget* r = nullptr;
        for (const auto& [k, i] : script_.factory)
            if (k == cls) r = i >= 0 && static_cast<std::size_t>(i) < screens.size() ? screens[i] : nullptr;
        log_.push_back("factory " + cls + " " + file + " -> " + name(r));
        return r;
    }
    void release(Widget* w) override { log_.push_back("release " + name(w)); }

    // checkpoint 118e: GuiAgentLoad's card load (00334790 hook), the copy at 00292c54 (observer) and
    // the classes 0041f960 declines ("stop", hook at its entry).
    std::uint8_t agent_load(std::int32_t agent, const std::string& n, std::int32_t slot) override {
        std::string h;
        for (unsigned char ch : n) {
            char b[4];
            std::snprintf(b, sizeof b, "%02x", ch);
            h += b;
        }
        const auto r = static_cast<std::uint8_t>(script_.config("load"));
        log_.push_back("agentload " + hex(static_cast<std::uint32_t>(agent)) + " " + (h.empty() ? "-" : h) + " " +
                       std::to_string(slot) + " -> " + std::to_string(r));
        return r;
    }
    void agent_activate(std::int32_t) override { log_.push_back("agentcopy"); }
    bool class_stopped(const std::string& id) override {
        for (const auto& s : script_.stop)
            if (s == id) {
                log_.push_back("stop " + id);
                return true;
            }
        return false;
    }

    // Native manager (gen_gui_runtime.py native mode: 004133b0 / 0036e1b0 / 0036dc60 hooks).
    std::optional<std::int32_t> resource(const std::string& n) override {
        for (auto it = script_.res.rbegin(); it != script_.res.rend(); ++it)
            if (it->first == n) {
                log_.push_back("res " + n + " -> 1");
                return 0x100 + it->second;
            }
        log_.push_back("res " + n + " -> 0");
        return std::nullopt;
    }
    const GuiNode* open_gui(const std::string& file, std::int32_t res) override {
        log_.push_back("open " + file + " " + hex(static_cast<std::uint32_t>(res)));
        const std::int32_t idx = res - 0x100;
        if (idx == -2) return nullptr;
        if (!files || idx < 0 || static_cast<std::size_t>(idx) >= files->size())
            throw std::runtime_error("gui script: no file " + std::to_string(idx));
        return &(*files)[static_cast<std::size_t>(idx)].root;
    }
    const std::vector<GuiFile>* files = nullptr;

    // 003d1660 (gen_gui_runtime.py "kbpoll" hook): the queued "key" reports.
    std::vector<UsbKeyReport> keys;
    std::optional<UsbKeyReport> usbkb_poll() override {
        if (keys.empty()) {
            log_.push_back("kbpoll -");
            return std::nullopt;
        }
        const UsbKeyReport k = keys.front();
        keys.erase(keys.begin());
        log_.push_back("kbpoll " + std::to_string(k.count) + " " + hex(k.mod) + " " + hex(k.code));
        return k;
    }

    // checkpoint 118g: the command centre's profile services (gen_gui_runtime.py Runner.install_cc)
    std::uint8_t pb68 = 0, pb72 = 0, pb73 = 0;
    std::optional<std::string> prof(const std::string& key) const {
        for (auto it = script_.prof.rbegin(); it != script_.prof.rend(); ++it)
            if (it->first == key) return it->second;
        return std::string();
    }
    static std::string q(const std::optional<std::string>& s) {
        return s ? "\"" + *s + "\"" : std::string("(null)");
    }
    // (checkpoint 132on2c: outside the command centre and GuiAccountLogin cases 00336170 / 00336110 run natively,
    // unlogged, on the script's profile)
    bool pnames_hooked() const { return script_.cc || script_.config("acnt"); }
    std::optional<std::string> profile_name(std::int32_t p) override {
        const auto v = prof("name");
        if (pnames_hooked()) log_.push_back("pname " + hex(static_cast<std::uint32_t>(p)) + " -> " + q(v));
        return v;
    }
    std::optional<std::string> profile_rank(std::int32_t p) override {
        const auto v = prof("rank");
        if (script_.cc) log_.push_back("prank " + hex(static_cast<std::uint32_t>(p)) + " -> " + q(v));
        return v;
    }
    std::int32_t profile_clp(std::int32_t p) override {
        const auto v = static_cast<std::int32_t>(script_.config("clp"));
        log_.push_back("pclp " + hex(static_cast<std::uint32_t>(p)) + " -> " + std::to_string(v));
        return v;
    }
    std::int32_t level_done(std::int32_t, const std::string& key) override {
        std::int32_t v = 0;
        for (std::size_t i = 0; i < cc_level_names.size(); ++i) {
            std::string n = cc_level_names[i];
            for (auto& ch : n) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (n.substr(0, 16) == key) {
                v = static_cast<std::int32_t>((script_.config("done") >> i) & 1);
                break;
            }
        }
        log_.push_back("pdone " + key + " -> " + std::to_string(v));
        return v;
    }
    std::int32_t level_flag(std::int32_t, std::int32_t s, std::int32_t k) override {
        std::int32_t v = 0;
        if (k == -1 && s >= 1) v = static_cast<std::int32_t>((script_.config("f8") >> (s - 1)) & 1);
        else if (k >= 0) v = static_cast<std::int32_t>((script_.config("zeus") >> ((6 * s + k) % 32)) & 1);
        log_.push_back("p2860 " + std::to_string(s) + " " + std::to_string(k) + " -> " + std::to_string(v));
        return v;
    }
    std::optional<std::string> profile_6060(std::int32_t, const std::string& key) override {
        std::optional<std::string> v;
        for (auto it = script_.prof.rbegin(); it != script_.prof.rend(); ++it)
            if (it->first == "ukr") {
                v = it->second;
                break;
            }
        log_.push_back("p6060 " + key + " -> " + q(v));
        return v;
    }
    std::int32_t profile_3b10(std::int32_t, const std::string& key) override {
        const auto v = static_cast<std::int32_t>(script_.config("ukc"));
        log_.push_back("p3b10 " + key + " -> " + std::to_string(v));
        return v;
    }
    std::vector<BonusRow> profile_vec(std::int32_t, std::int32_t kind) override {
        log_.push_back("pvec " + std::to_string(kind) + " n=" + std::to_string(script_.bvec.size()));
        std::vector<BonusRow> out;
        for (const auto& [i, b] : script_.bvec) out.push_back({i, b});
        return out;
    }
    std::vector<BonusItem> profile_items(std::int32_t, std::int32_t kind, std::int32_t index) override {
        std::vector<BonusItem> out;
        for (const auto& [idx, rows] : script_.bitems)
            if (idx == index) {
                out.clear();
                for (const auto& [n, b] : rows) out.push_back({n, b});
            }
        log_.push_back("pitems " + std::to_string(kind) + " " + std::to_string(index) + " n=" +
                       std::to_string(out.size()));
        return out;
    }
    std::string string_key(std::int32_t i) override {
        log_.push_back("skey " + std::to_string(i));
        for (auto it = script_.skey.rbegin(); it != script_.skey.rend(); ++it)
            if (it->first == i) return it->second;
        return "<key" + std::to_string(i) + ">";
    }
    void level_start(std::int32_t p, std::int32_t level, std::int32_t a, std::int32_t b, std::int32_t c) override {
        log_.push_back("p28d0 " + hex(static_cast<std::uint32_t>(p)) + " " + std::to_string(level) + " " +
                       std::to_string(a) + " " + std::to_string(b) + " " + std::to_string(c));
    }
    std::uint8_t profile_byte(std::int32_t, std::uint32_t offset) override {
        if (offset == 0xb68) return pb68;
        if (offset == 0xb72) return pb72;
        if (offset == 0xb73) return pb73;
        throw std::runtime_error("gui script: profile byte " + hex(offset));
    }

    static std::string hex(std::uint32_t v) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%x", v);
        return buf;
    }

private:
    const GuiScript& script_;
    std::vector<std::string>& log_;
    std::int32_t popup_id_ = 100;
};

void preorder(Widget* w, std::vector<Widget*>& out) {
    out.push_back(w);
    for (Widget* c : w->children) preorder(c, out);
}

}  // namespace

std::vector<std::string> run_gui_script(const GuiScript& script, const std::vector<GuiFile>& files) {
    std::vector<std::string> log;
    ScriptHost host(script, log);
    GuiRuntime rt(host);
    host.runtime = &rt;

    auto& g = rt.g;
    g.card.present = static_cast<std::uint8_t>(script.config("mc59d"));
    g.card.check_space = static_cast<std::uint8_t>(script.config("mc59e"));
    g.card.free = static_cast<std::int32_t>(script.config("mc5a4"));
    g.card.slot = static_cast<std::int32_t>(script.config("mc5a8"));
    const std::int64_t ctl = script.config("ctl");
    g.controller = ctl >= 0;
    g.controller_6a8 = ctl >= 0 ? static_cast<std::uint8_t>(ctl) : 0;
    g.app_loaded = static_cast<std::int32_t>(script.config("app260"));
    g.pad_state = static_cast<std::int32_t>(script.config("pad"));
    g.keyboard = static_cast<std::uint8_t>(script.config("bb1"));
    g.sound_gate = static_cast<std::uint8_t>(script.config("b38"));
    g.debug_triangle = static_cast<std::uint8_t>(script.config("eeb0"));
    g.card_checked = static_cast<std::uint8_t>(script.config("s330"));
    g.t1_base = static_cast<std::uint32_t>(script.config("t1base"));
    rt.keyboard_path = static_cast<std::uint8_t>(script.config("kb51"));
    rt.ascii_keys = static_cast<std::uint8_t>(script.config("kb52"));
    const bool kbmode = script.kbd >= 0;
    if (kbmode) {
        rt.lang = static_cast<std::int8_t>(static_cast<std::uint8_t>(script.config("lang")));
        rt.kb_a701 = static_cast<std::uint8_t>(script.config("a701"));
        rt.kb_mod = static_cast<std::uint32_t>(script.config("kbmod"));
    }
    g.iface = script.iface;
    host.files = &files;
    const bool native = script.native;
    if (native) {
        rt.native_app = true;
        rt.next_popup = static_cast<std::int32_t>(script.config("popup"));
    }

    const bool is_title = script.root == "GuiTitleScreen";
    WidgetClass root_class;
    if (is_title) root_class = WidgetClass::title;
    else if (script.root == "GuiWidget") root_class = WidgetClass::widget;
    else if (script.root == "GuiAgentLoad") root_class = WidgetClass::agentload;
    else if (script.root == "GuiOnlineOfflineSelect") root_class = WidgetClass::onoffsel;
    else if (script.root == "GuiCommandCenter") root_class = WidgetClass::cmdcentr;
    else if (script.root == "GuiOnlineConnect") root_class = WidgetClass::onlinecn;  // checkpoint 132ln2
    else if (script.root == "GuiAccountLogin") root_class = WidgetClass::acntlogn;   // checkpoint 132on2b
    else if (script.root == "GuiMissionSetup") root_class = WidgetClass::msnsetup;   // checkpoint 132ln3
    else if (script.root == "GuiNetJoinMission") root_class = WidgetClass::netjoin;  // checkpoint 132ln4
    else if (script.root == "GuiNetContacts") root_class = WidgetClass::netcontacts;  // checkpoint 132on2h
    else throw std::runtime_error("gui script: unsupported root class " + script.root);
    g.card.dir = script.dir;
    g.app_online = static_cast<std::uint8_t>(script.config("a2e"));
    g.b_6488 = static_cast<std::uint8_t>(script.config("b506488"));
    g.b_6490 = static_cast<std::uint8_t>(script.config("b506490"));
    g.b_eea0 = static_cast<std::uint8_t>(script.config("b51eea0"));
    g.b_eea8 = static_cast<std::uint8_t>(script.config("b51eea8"));
    g.b_23d0 = static_cast<std::uint8_t>(script.config("b5723d0"));
    g.b_2fe8 = static_cast<std::uint8_t>(script.config("b532fe8"));
    g.app_30 = static_cast<std::uint8_t>(script.config("a30"));
    g.app_31 = static_cast<std::uint8_t>(script.config("a31"));
    g.app_32 = static_cast<std::uint8_t>(script.config("a32"));
    host.lan_play = script.config("port_lan") != 0;  // PORT-ONLY (132ln1)
    g.lan_mode = static_cast<std::uint8_t>(script.config("lan_mode"));  // PORT-ONLY (132ln2): as after the LAN entry
    if (script.cc) {
        // checkpoint 118g: 0048b868, 004ffbb5, the saved panel / selection 0048b7a0 / 0048b7a4, the six saved
        // focus names 0048b7a8 + 0x20 k and the profile bytes +b68 / +b72 / +b73
        g.cc_want_bonus = static_cast<std::uint8_t>(script.config("b868"));
        g.b_bb5 = static_cast<std::uint8_t>(script.config("bb5"));
        g.cc_panel = static_cast<std::uint8_t>(script.config("svp"));
        g.cc_sel = static_cast<std::int32_t>(script.config("svi"));
        for (int k = 0; k < 6; ++k) {
            const std::string key = "sv" + std::to_string(k);
            for (auto it = script.prof.rbegin(); it != script.prof.rend(); ++it)
                if (it->first == key) {
                    if (it->second && !it->second->empty()) g.cc_names[k] = it->second->substr(0, 0x1f);
                    break;
                }
        }
        host.pb68 = static_cast<std::uint8_t>(script.config("pb68"));
        host.pb72 = static_cast<std::uint8_t>(script.config("pb72"));
        host.pb73 = static_cast<std::uint8_t>(script.config("pb73"));
    }

    auto file = [&](int i) -> const GuiNode& {
        if (i < 0 || static_cast<std::size_t>(i) >= files.size()) throw std::runtime_error("gui script: no file " + std::to_string(i));
        return files[i].root;
    };
    if (native && script.msgbox) {
        // 002cb498: 004187c0(mgr, "GuiMessageBox", "POPUP") into app +258
        rt.msgbox = rt.create("GuiMessageBox", std::string("POPUP"));
        if (!rt.msgbox) throw std::runtime_error("gui script: GuiMessageBox / POPUP did not load");
    }
    for (int i : script.screens) host.screens.push_back(rt.load(file(i), WidgetClass::widget));
    // checkpoint 118d: the on-screen keyboard (KEYBOARD.GUI), opened by "kbopen"
    Widget* kbd = kbmode ? rt.load(file(script.kbd), WidgetClass::keyboard) : nullptr;
    if (kbmode) {
        // the generator's observer on 0041e0f0 for 0032b2d0's result
        rt.send_observer = [&log, &host, kbd](Widget* sender, Widget* target, std::uint32_t type, std::int32_t a3) {
            const std::uint32_t t = type & 0xffff;
            if (sender != kbd || (t != 0x1a && t != 0x1b)) return;
            const auto& r = kbd->kb->result;
            char buf[64];
            std::snprintf(buf, sizeof buf, "kbres %s %x %d ", host.name(target).c_str(), t, a3);
            log.push_back(buf + (t == 0x1a && r ? "\"" + *r + "\"" : std::string("-")));
        };
    }
    // the generator's observer on 0041e040: the events a text list sends to its parents (0x10 moved / pressed,
    // 0x13 chosen, 0x14 / 0x15 row entered / left) which the menu screens consume
    rt.up_observer = [&log, &host](Widget* w, std::uint32_t type, std::int32_t a3, std::int32_t t0) {
        if (w->cls != WidgetClass::textlist) return;
        char buf[96];
        std::snprintf(buf, sizeof buf, "up %s %x %d %d", host.name(w).c_str(), type, a3, t0);
        log.push_back(buf);
    };
    Widget* root = rt.load(file(script.main), root_class);
    if (script.config("oc10")) root->id10 = 1;  // checkpoint 132ln2: the navigator 0033c320's op 0 store
    const bool sess_mode = script.config("sess") != 0;  // checkpoint 132on2
    host.session = sess_mode;
    host.unread = script.config("unread") != 0;
    host.msgs_ = script.lmsg;  // checkpoint 132on2c: 0044f880 / 0044f470 leave +18 set
    host.msgs_changed_ = !script.lmsg.empty();
    host.dnas_event = [&rt, root]() { rt.send(nullptr, root, 0x2002, 0, 0); };

    std::vector<Widget*> all;
    std::unordered_map<std::string, Widget*> by_name;
    if (native) {
        for (Widget* r : rt.roots) preorder(r, all);
    } else {
        for (Widget* s : host.screens) preorder(s, all);
        preorder(root, all);
        for (Widget* w : all)
            if (!by_name.emplace(w->name, w).second) throw std::runtime_error("gui script: duplicate widget name " + w->name);
    }
    by_name["canvas"] = &rt.canvas();
    auto refresh_all = [&]() {
        if (!native) return;
        std::vector<Widget*> now;
        for (Widget* r : rt.roots) preorder(r, now);
        for (Widget* w : now)
            if (std::find(all.begin(), all.end(), w) == all.end()) all.push_back(w);
    };
    auto lookup = [&](const std::string& n) -> Widget* {
        if (n == "-") return nullptr;
        if (native && n != "canvas") {
            refresh_all();
            // checkpoint 132on2c: "root/name", a widget under the named root only
            const auto slash = n.find('/');
            const std::string want = slash == std::string::npos ? n : n.substr(slash + 1);
            Widget* hit = nullptr;
            for (Widget* w : all) {
                if (w->deleted || w->name != want) continue;
                if (slash != std::string::npos) {
                    const Widget* up = w->parent;
                    while (up && up->name != n.substr(0, slash)) up = up->parent;
                    if (!up) continue;
                }
                if (hit) throw std::runtime_error("gui script: ambiguous widget " + n);
                hit = w;
            }
            if (!hit) throw std::runtime_error("gui script: unknown widget " + n);
            return hit;
        }
        const auto it = by_name.find(n);
        if (it == by_name.end()) throw std::runtime_error("gui script: unknown widget " + n);
        return it->second;
    };

    auto nm = [&](const Widget* w) { return host.name(w); };
    std::string last_state;
    bool have_state = false;
    std::unordered_map<const Widget*, std::string> last_w;
    auto state = [&]() {
        const Widget& c = rt.canvas();
        std::string scr;
        for (const Widget* w : c.children) {
            if (!scr.empty()) scr += ',';
            scr += nm(w) + ":" + std::to_string(static_cast<std::uint16_t>(w->layer));
        }
        std::string cnt, tm;
        for (int p = 0; p < 2; ++p) {
            for (int k = 0; k < 16; ++k) {
                const auto v = g.repeat_count[p][k];
                const auto t = g.press_time[p][k];
                const std::string key = std::to_string(p) + "." + std::to_string(k) + "=";
                if (v) cnt += (cnt.empty() ? "" : ",") + key + std::to_string(v);
                if (t) tm += (tm.empty() ? "" : ",") + key + std::to_string(t);
            }
        }
        char buf[512];
        std::string t = "-";
        if (is_title) {
            std::snprintf(buf, sizeof buf, "%d,%08x,%d", root->demo, std::bit_cast<std::uint32_t>(root->last_input),
                          root->popup);
            t = buf;
        }
        std::snprintf(buf, sizeof buf, "%d,%d,%d,%d,%d,%d,%d,%d,", g.card_ok, g.app_state, g.app_state_prev,
                      g.card_slot, g.no_pad_frames, g.no_pad_popup, g.card_checked, g.app_attract);
        const std::string gl = buf + g.app_mode;
        std::snprintf(buf, sizeof buf, "cfl=%x focus=%s top=%s scr=", c.flags, nm(c.focus).c_str(), nm(rt.top()).c_str());
        std::string s = buf + scr;
        std::snprintf(buf, sizeof buf, " fd=%s nx=%s pv=%s ph=%08x pend=%s,%s,%d t=", nm(g.dispatch_top).c_str(),
                      nm(g.next_from).c_str(), nm(g.prev_from).c_str(), std::bit_cast<std::uint32_t>(rt.phase),
                      nm(rt.pending_pop).c_str(), nm(rt.pending_push).c_str(), rt.pending);
        s += buf + t + " g=" + gl + " cnt=" + cnt + " tm=" + tm;
        if (native) {
            std::string pq;
            for (const PopupEntry& e : rt.popups) {
                if (!pq.empty()) pq += ';';
                pq += std::to_string(e.id) + ":" + std::to_string(e.type) + ":" + e.title + ":" + e.message + ":" +
                      nm(e.target) + ":" + std::to_string(e.arg);
            }
            std::string mb = "-";
            if (rt.msgbox) {
                const Widget* m = rt.msgbox;
                mb = std::to_string(m->id10) + "," + std::to_string(m->mb_type) + "," + nm(m->mb_target) + "," +
                     std::to_string(m->mb_arg) + "," + std::to_string(m->mb_layout);
            }
            std::string rs, pu;
            for (const Widget* r : rt.roots) {
                std::snprintf(buf, sizeof buf, "%s%s:%d:%x", rs.empty() ? "" : ",", nm(r).c_str(), r->refs, r->flags);
                rs += buf;
            }
            for (const Widget* r : rt.pending_unload) pu += (pu.empty() ? "" : ",") + nm(r);
            std::snprintf(buf, sizeof buf, " bl=%08x,%08x,%08x,%08x,%08x", std::bit_cast<std::uint32_t>(rt.blink),
                          std::bit_cast<std::uint32_t>(rt.blink_rgba[0]), std::bit_cast<std::uint32_t>(rt.blink_rgba[1]),
                          std::bit_cast<std::uint32_t>(rt.blink_rgba[2]), std::bit_cast<std::uint32_t>(rt.blink_rgba[3]));
            s += " pq=" + pq + " mb=" + mb + " pid=" + std::to_string(rt.next_popup) + " roots=" + rs + " pu=" + pu +
                 " if=" + g.iface + buf;
        }
        return s;
    };
    // checkpoint 132ln4: the GuiArrayWidget part (+48..+88) of an array widget that is not a text list
    auto aline = [&](const Widget* w) {
        const auto& l = *w->list;
        char buf[256];
        std::snprintf(buf, sizeof buf, " ls=%x,%x,%x,%x,%d,%x,%x,%x,%x,%x,%d,%d,%d,%d cw=", l.cols, l.rows, l.col, l.row,
                      l.mode, l.row_h, l.vis_cols, l.vis_rows, l.left, l.top, l.paging, l.wrap, l.no_scroll, l.a84);
        std::string s = buf;
        for (std::size_t i = 0; i < l.colw.size(); ++i) s += (i ? "," : "") + std::to_string(l.colw[i]);
        if (l.cursor) {
            std::snprintf(buf, sizeof buf, " lc=%s,%08x,%08x", nm(l.cursor).c_str(),
                          std::bit_cast<std::uint32_t>(l.cursor->rect[0]), std::bit_cast<std::uint32_t>(l.cursor->rect[1]));
            s += buf;
        } else {
            s += " lc=-";
        }
        return s;
    };
    // checkpoint 118e: GuiArrayWidget / GuiTextListWidget state (+48..+f1)
    auto lline = [&](const Widget* w) {
        const auto& l = *w->list;
        char buf[512];
        std::snprintf(buf, sizeof buf, " ls=%x,%x,%x,%x,%d,%x,%x,%x,%x,%x,%d,%d,%d,%d cw=", l.cols, l.rows, l.col, l.row,
                      l.mode, l.row_h, l.vis_cols, l.vis_rows, l.left, l.top, l.paging, l.wrap, l.no_scroll, l.a84);
        std::string s = buf;
        for (std::size_t i = 0; i < l.colw.size(); ++i) s += (i ? "," : "") + std::to_string(l.colw[i]);
        if (l.cursor) {
            std::snprintf(buf, sizeof buf, " lc=%s,%08x,%08x", nm(l.cursor).c_str(),
                          std::bit_cast<std::uint32_t>(l.cursor->rect[0]), std::bit_cast<std::uint32_t>(l.cursor->rect[1]));
            s += buf;
        } else {
            s += " lc=-";
        }
        std::snprintf(buf, sizeof buf, " lf=%x f1=%d a4=%d", l.font, l.f1, l.a4);
        s += buf;
        if (l.obj) {
            std::snprintf(buf, sizeof buf, " lo=%d,%d,%d,%d,%d", l.obj->b10, l.obj->b11, l.obj->b12, l.obj->b58,
                          l.obj->b5b);
            s += buf;
        } else {
            s += " lo=-";
        }
        s += " it=";
        for (std::size_t i = 0; i < l.items.size(); ++i) {
            std::snprintf(buf, sizeof buf, "|%d|%d", l.items[i].id, l.items[i].style);
            s += (i ? ";" : "") + l.items[i].text + buf;
        }
        return s;
    };
    auto wline = [&](const Widget* w) {
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s fl=%x lay=%d foc=%s par=%s", nm(w).c_str(), w->flags,
                      static_cast<std::uint16_t>(w->layer), nm(w->focus).c_str(), nm(w->parent).c_str());
        std::string s = buf;
        if (is_text(w)) {
            if (!w->obj) s += " obj=-";
            else {
                std::snprintf(buf, sizeof buf, " obj=%d,%d,%d,%d,%d", w->obj->b10, w->obj->b11, w->obj->b12,
                              w->obj->b58, w->obj->b5b);
                s += buf;
            }
        }
        if (w->deco) {
            std::snprintf(buf, sizeof buf, " deco=%08x,%08x", std::bit_cast<std::uint32_t>(w->deco->matrix[12]),
                          std::bit_cast<std::uint32_t>(w->deco->matrix[14]));
            s += buf;
        }
        if (w->cls == WidgetClass::textlist) s += lline(w);
        if (w->cls == WidgetClass::agentload) {  // checkpoint 118e: +78 / +48 / +7c and app +2f
            std::snprintf(buf, sizeof buf, " al=%08x,%d,%d,%d", std::bit_cast<std::uint32_t>(w->al_timer), w->al_popup,
                          w->al_7c, g.app_2f);
            s += buf;
        }
        if (w->cls == WidgetClass::onoffsel) {  // checkpoint 118f: +48, app +30 / +32, 005723d0, 00532fe8
            std::snprintf(buf, sizeof buf, " oo=%d,%d,%d,%d,%d", w->oo_online, g.app_30, g.app_32, g.b_23d0, g.b_2fe8);
            s += buf;
            if (host.lan_play) {  // PORT-ONLY (132ln1): only in runs with cfg port_lan, so the 118f traces are unchanged
                std::snprintf(buf, sizeof buf, " lan=%d", g.lan_mode);
                s += buf;
            }
        }
        if (w->cls == WidgetClass::onlinecn) {  // checkpoint 132ln2: GuiOnlineConnect and the app bytes it writes
            const auto& o = *w->oc;
            std::snprintf(buf, sizeof buf, " oc=%d,%d,%08x,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s", o.w84, o.state,
                          std::bit_cast<std::uint32_t>(o.t8c), o.config, o.connected, o.dnas_done, o.shown,
                          o.popup_11c, o.popup_120, o.save.popup, g.app_2a, g.app_265, o.b78, nm(o.dnas).c_str());
            s += buf;
            if (host.lan_play) {  // PORT-ONLY (132ln2): only in runs with cfg port_lan
                std::snprintf(buf, sizeof buf, " lan=%d", g.lan_mode);
                s += buf;
            }
            if (sess_mode) {  // checkpoint 132on2: the lists, app +2ec..+2f8 / +26c, start_button +10, the session
                const auto n = [&](const auto& v) { return g.app_2ec ? std::to_string(v.size()) : std::string("-"); };
                s += " on=" + std::to_string(o.configs.size()) + "," + std::to_string(o.servers.size()) + "," +
                     std::to_string(g.app_2ec) + "," + n(g.policy_usage) + "," + n(g.policy_privacy) + "," +
                     n(g.announcements) + "," + cstr_of(g.app_26c) + "," +
                     (o.start_button ? std::to_string(o.start_button->id10) : std::string("-")) + "," +
                     (host.session ? "1" : "0");
            }
        }
        if (sess_mode && w->cls == WidgetClass::mltext) {  // checkpoint 132on2: the ML text's +68 and lines (hex)
            const auto& m = *w->ml;
            s += " ml=" + std::to_string(m.top) + "," + std::to_string(m.lines.size()) + ":";
            for (std::size_t i = 0; i < m.lines.size(); ++i) {
                if (i) s += ".";
                for (unsigned char ch : m.lines[i]) {
                    std::snprintf(buf, sizeof buf, "%02x", ch);
                    s += buf;
                }
            }
        }
        if (w->cls == WidgetClass::acntlogn) {  // checkpoint 132on2b: GuiAccountLogin, its statics, app +264 / +268
            const auto& l = *w->acl;
            const auto hx = [&](const std::string& v) {
                std::string o;
                for (unsigned char ch : cstr_of(v)) {
                    std::snprintf(buf, sizeof buf, "%02x", ch);
                    o += buf;
                }
                return o;
            };
            const std::string user = hx(g.acl_user), pass = hx(g.acl_pass);
            std::snprintf(buf, sizeof buf, " acl=%d,%d,%d,%d,%d,%d,%d,%d,%d,", l.b78, l.active, l.w84, l.kb_ok, l.wait,
                          l.saved, l.mode, g.app_264, g.app_268);
            s += buf + user + "," + pass;
            std::snprintf(buf, sizeof buf, ",%d,%d,%d,%d", g.acl_kb_ok, g.acl_busy, g.reg_cached, g.reg_enabled);
            s += buf;
        }
        if (w->cls == WidgetClass::onlinepref) {  // checkpoint 132on2c: GuiOnlineOptions, app +268, the agent +9a0 / +95c
            const auto& o = *w->opt;
            const std::string region = host.region_ ? *host.region_ : host.prof("region").value_or("");
            std::snprintf(buf, sizeof buf, " op=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,", o.active, o.w84, o.b78, o.current,
                          o.picked, static_cast<int>(o.universes.size()), o.save.popup, o.save.mode, g.app_268,
                          host.p9a0_ ? *host.p9a0_ : static_cast<std::int32_t>(script.config("p9a0")));
            s += buf;
            for (unsigned char ch : cstr_of(region.substr(0, 0x3f))) {
                std::snprintf(buf, sizeof buf, "%02x", ch);
                s += buf;
            }
            s += " ow=" + nm(o.label) + "," + nm(o.value) + "," + nm(o.list) + "," + nm(o.select) + "," +
                 nm(o.start_button);
        }
        if (w->cls == WidgetClass::netmess) {  // checkpoint 132on2c: GuiNetMessages, the message count, the font colours
            const auto& m = *w->nm;
            std::snprintf(buf, sizeof buf, " nm=%d,%d,%d,%d,%d,%d,%d", m.active, m.w84, m.b78, m.shown, m.popup,
                          m.save.popup, host.lobby_message_count());
            s += buf;
            s += " nw=" + nm(m.box) + "," + nm(m.button1) + "," + nm(m.button2) + "," + nm(m.button3) + "," +
                 nm(m.info) + "," + nm(m.nomessage) + "," + nm(m.start_button) + " nmc=";
            if (!host.nm_colors) {
                s += "-";
            } else {
                for (int k = 0; k < 8; ++k) {
                    const float f = k < 4 ? host.nm_colors->first[k] : host.nm_colors->second[k - 4];
                    std::snprintf(buf, sizeof buf, "%s%08x", k ? "," : "", std::bit_cast<std::uint32_t>(f));
                    s += buf;
                }
            }
        }
        if (w->cls == WidgetClass::netmsgarray) {  // checkpoint 132on2c: GuiNetMessageArray
            const auto& a = *w->nma;
            s += " nma=" + std::string(a.source ? "1" : "0") + "," + nm(a.header) + "," + nm(a.icons[0]) + "," +
                 nm(a.icons[1]) + "," + nm(a.icons[2]);
            if (a.obj) {
                std::snprintf(buf, sizeof buf, " mo=%d,%d,%d,%d,%d", a.obj->b10, a.obj->b11, a.obj->b12, a.obj->b58,
                              a.obj->b5b);
                s += buf;
            } else {
                s += " mo=-";
            }
            s += aline(w);
        }
        if (w->cls == WidgetClass::msnsetup) {  // checkpoint 132ln3: GuiMissionSetup and the app fields it writes
            const auto& m = *w->ms;
            std::snprintf(buf, sizeof buf, " ms=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,%s,%s,%s,%s", m.active, m.w84,
                          m.room, m.b78, g.app_180, g.app_185, g.app_254, g.r_64a8, g.r_64b0, g.r_64b8,
                          static_cast<int>(g.app_state), nm(m.num_agents).c_str(), nm(m.join_filter).c_str(),
                          nm(m.begin).c_str(), nm(m.num_agents_txt).c_str(), nm(m.join_filter_txt).c_str());
            s += buf;
        }
        if (w->cls == WidgetClass::netjoin || w->cls == WidgetClass::netcontacts) {  // 132ln4 / 132on2h: GuiLobbyScreen
            const auto& l = *w->lb;
            const char* src = l.source == &l.players_c8 ? "c8" : l.source == &l.games_e4 ? "e4" : l.source ? "?" : "-";
            std::snprintf(buf, sizeof buf, " jm=%d,%d,%d,%d,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s", l.active, l.w84, l.b78,
                          l.mode, src, l.b94, l.w100, g.r_2ff0, g.r_2ff8, g.r_3000, static_cast<int>(g.app_state),
                          g.app_35, g.app_254, g.app_268, g.b_2fd0, cstr_of(g.level_name).c_str());
            s += buf;
            const bool ct = w->cls == WidgetClass::netcontacts;  // +160 / +164: GuiNetContacts' two buttons
            s += " jw=" + nm(l.list) + "," + nm(l.chat) + "," + nm(l.join_game) + "," + nm(l.agent_chat) + "," +
                 nm(l.agent_info) + "," + nm(l.agent_remove) + "," + nm(ct ? l.invite_contact : l.show_toggle) + "," +
                 nm(ct ? l.remove_contact : l.online_prefs) + "," + nm(l.host_game) + "," + nm(l.start_button);
        }
        if (w->cls == WidgetClass::missionlist) {  // checkpoint 132ln4: GuiMissionList
            const auto& m = *w->mls;
            const Widget* scr = w->parent;
            while (scr && scr->cls != WidgetClass::netjoin && scr->cls != WidgetClass::netcontacts) scr = scr->parent;
            const char* src = !m.source ? "-"
                              : scr && m.source == &scr->lb->players_c8 ? "c8"
                              : scr && m.source == &scr->lb->games_e4 ? "e4"
                                                                     : "?";
            std::snprintf(buf, sizeof buf, " mls=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s,%s rows=", m.kind, m.action, m.b4,
                          m.b5, m.b8, m.bc, m.sel_id, m.sel_index, m.sel_flags, m.sel_row, m.own_id, src,
                          nm(m.header_w).c_str());
            s += buf;
            for (std::size_t i = 0; i < m.rows.size(); ++i) {
                std::snprintf(buf, sizeof buf, "%s%d:%d:%d", i ? ";" : "", m.rows[i].id, m.rows[i].index, m.rows[i].flags);
                s += buf;
            }
            if (m.obj) {
                std::snprintf(buf, sizeof buf, " mo=%d,%d,%d,%d,%d", m.obj->b10, m.obj->b11, m.obj->b12, m.obj->b58,
                              m.obj->b5b);
                s += buf;
            } else {
                s += " mo=-";
            }
            s += aline(w);
            if (m.header_w && m.header_w->ta) {
                s += " hc=";
                const auto& rows = m.header_w->ta->rows;
                for (std::size_t r = 0; r < rows.size(); ++r)
                    for (std::size_t c = 0; c < rows[r].size(); ++c) s += (r || c ? "|" : "") + rows[r][c].text;
                s += aline(m.header_w);
            }
        }
        if (w->cls == WidgetClass::chatwin) {  // checkpoint 132ln4: GuiChatWindow (the ML text fields its screen sets)
            const auto& c = *w->ml;
            std::snprintf(buf, sizeof buf, " ch=%x,%d,%d,%d,%s", c.font, c.max_lines, c.per_page, c.top, nm(c.scroll).c_str());
            s += buf;
            if (!c.lines.empty()) {  // checkpoint 132on2c: its lines (hex), when there are any
                s += " cl=" + std::to_string(c.lines.size()) + ":";
                for (std::size_t i = 0; i < c.lines.size(); ++i) {
                    if (i) s += ".";
                    for (unsigned char ch : c.lines[i]) {
                        std::snprintf(buf, sizeof buf, "%02x", ch);
                        s += buf;
                    }
                }
            }
        }
        if (w->cls == WidgetClass::cmdcentr) {  // checkpoint 118g: GuiCommandCenter and the statics / app fields it writes
            const auto& c = *w->cc;
            const auto hx = [](const std::uint8_t* p, std::size_t n) {
                std::string h;
                char b[4];
                for (std::size_t i = 0; i < n; ++i) {
                    std::snprintf(b, sizeof b, "%02x", p[i]);
                    h += b;
                }
                return h;
            };
            const std::uint32_t cached4 = c.cached[0] | c.cached[1] << 8 | c.cached[2] << 16 |
                                          static_cast<std::uint32_t>(c.cached[3]) << 24;
            std::snprintf(buf, sizeof buf, " cc=%d,%s,%d,%s,%s,%s,%08x,%x,%d,%d,%d,%d,%d,", c.sel, nm(c.sel_w).c_str(),
                          static_cast<std::uint8_t>(c.panel), hx(c.open.data(), 24).c_str(), hx(c.bonus.data(), 18).c_str(),
                          hx(c.open.data() + 20, 4).c_str(), std::bit_cast<std::uint32_t>(c.pulse), cached4, c.keep,
                          c.exiting, c.active, c.net78, c.w84);
            s += buf;
            for (int k = 0; k < 6; ++k) s += (k ? "," : "") + nm(c.saved[k]);
            std::snprintf(buf, sizeof buf, " ccg=%d,%d,", g.cc_panel, g.cc_sel);
            s += buf;
            for (int k = 0; k < 6; ++k) s += (k ? "|" : "") + g.cc_names[k];
            s += "," + std::to_string(g.cc_want_bonus) + ",";
            for (int i = 0; i < 18; ++i) s += (i ? "," : "") + std::to_string(g.bonus_row[i]);
            std::snprintf(buf, sizeof buf, ",%d,%d,%d,%d,%d,%d,", g.app_33, g.app_34, g.app_35, g.app_state,
                          g.app_state_prev, g.level_494198);
            s += "," + g.level_name + "," + g.app_1c8 + buf + g.app_188;
            std::snprintf(buf, sizeof buf, ",%d,%d,%d,%d", g.app_248, g.app_24c, g.app_250, g.app_251);
            s += buf;
            if (!c.list) {
                s += " lcol=-";
            } else {
                const auto& l = *c.list->list;
                s += " lcol=";
                int n = 0;
                for (const auto* a : {&l.rgba, &l.rgba_c0, &l.rgba_d0})
                    for (float f : *a) {
                        std::snprintf(buf, sizeof buf, "%s%08x", n++ ? "," : "", std::bit_cast<std::uint32_t>(f));
                        s += buf;
                    }
            }
        }
        if (native) {
            if (is_text(w)) s += " tx=\"" + w->text + "\"";
            if (w->cls == WidgetClass::edit) {
                std::snprintf(buf, sizeof buf, " pw=%d max=%d cur=%d", w->password, static_cast<std::int32_t>(w->max_len),
                              w->cursor);
                s += buf;
            }
        }
        if (kbmode) {
            if (is_text(w) && w->obj) {
                std::snprintf(buf, sizeof buf, " o54=%08x", w->obj->w54);
                s += buf;
            }
            if (w->id10 != -1) s += " id=" + std::to_string(w->id10);
            if (w->cls == WidgetClass::keyboard) {
                const auto& k = *w->kb;
                char dk[16] = "-", li[16] = "-";
                if (k.dead) std::snprintf(dk, sizeof dk, "%x", static_cast<unsigned>(k.dead->key));
                if (k.layout >= 0) std::snprintf(li, sizeof li, "%d", k.layout);
                std::snprintf(buf, sizeof buf, " kb=%d,%x,%d,%x,%s,%d,%d,%d,%d,%s,%d,%s,%s,%s", k.key, k.mods, k.e1,
                              k.usb_mods, dk, k.altgr, k.max, k.arg, k.e0, nm(k.target).c_str(), k.lang, li,
                              nm(k.label).c_str(), nm(k.edit).c_str());
                s += buf;
            }
        }
        if (script.cc && is_text(w)) {  // checkpoint 118g: the widget colour +60..+6c (0041c070)
            std::snprintf(buf, sizeof buf, " rgba=%08x,%08x,%08x,%08x", std::bit_cast<std::uint32_t>(w->rgb[0]),
                          std::bit_cast<std::uint32_t>(w->rgb[1]), std::bit_cast<std::uint32_t>(w->rgb[2]),
                          std::bit_cast<std::uint32_t>(w->alpha));
            s += buf;
        }
        return s;
    };
    auto kvec = [&](const Widget* w) {
        const auto& k = *w->kb;
        std::string s = nm(w) + " keys=";
        for (std::size_t i = 0; i < k.keys.size(); ++i) s += (i ? "," : "") + nm(k.keys[i]);
        auto vec = [&](const char* n, const std::vector<std::int32_t>& v) {
            s += std::string(" ") + n + "=";
            char buf[16];
            for (std::size_t i = 0; i < v.size(); ++i) {
                if (v[i] < 0) std::snprintf(buf, sizeof buf, "%s-", i ? "," : "");
                else std::snprintf(buf, sizeof buf, "%s%x", i ? "," : "", static_cast<std::uint32_t>(v[i]));
                s += buf;
            }
        };
        vec("ids", k.ids);
        vec("lay", k.lay);
        vec("up", k.up);
        vec("dn", k.dn);
        vec("lf", k.lf);
        vec("rt", k.rt);
        return s;
    };
    std::unordered_map<const Widget*, std::string> last_k;
    auto emit = [&](const std::string& tag) {
        refresh_all();
        const std::string s = state();
        if (!have_state || s != last_state) {
            log.push_back("S" + tag + " " + s);
            last_state = s;
            have_state = true;
        }
        for (const Widget* w : all) {
            if (w->deleted) continue;
            const std::string l = wline(w);
            const auto it = last_w.find(w);
            if (it == last_w.end() || it->second != l) {
                log.push_back("W" + tag + " " + l);
                last_w[w] = l;
            }
            if (kbmode && w->cls == WidgetClass::keyboard) {
                const std::string k = kvec(w);
                const auto kt = last_k.find(w);
                if (kt == last_k.end() || kt->second != k) {
                    log.push_back("K" + tag + " " + k);
                    last_k[w] = k;
                }
            }
        }
    };

    emit("L");
    rt.push(root, script.layer);
    emit("P");
    std::array<std::uint32_t, 2> prev{};
    for (std::size_t i = 0; i < script.frames.size(); ++i) {
        const auto& f = script.frames[i];
        log.push_back("@" + std::to_string(i));
        host.ms = f.ms;
        host.save = f.save;
        for (const std::string& o : f.ops) {
            std::vector<std::string> p;
            std::size_t st = 0;
            for (;;) {
                const auto c = o.find(':', st);
                p.push_back(o.substr(st, c == std::string::npos ? std::string::npos : c - st));
                if (c == std::string::npos) break;
                st = c + 1;
            }
            auto need = [&](std::size_t n) {
                if (p.size() != n) throw std::runtime_error("gui script: bad op " + o);
            };
            if (p[0] == "ev") {
                if (p.size() != 5 && p.size() != 6) throw std::runtime_error("gui script: bad op " + o);
                Widget* tgt = lookup(p[1]);
                Widget* snd = lookup(p[2]);
                const std::uint8_t r = rt.send(snd, tgt, static_cast<std::uint32_t>(to_int(p[3])),
                                               static_cast<std::int32_t>(to_int(p[4])),
                                               p.size() == 6 ? static_cast<std::int32_t>(to_int(p[5])) : 0);
                log.push_back("ev -> " + std::to_string(r));
            } else if (p[0] == "push") {
                need(3);
                rt.push(lookup(p[1]), static_cast<std::int32_t>(to_int(p[2])));
            } else if (p[0] == "pop") {
                need(2);
                rt.pop(lookup(p[1]));
            } else if (p[0] == "switch") {
                need(3);
                rt.switch_screens(lookup(p[1]), lookup(p[2]));
            } else if (p[0] == "defer") {
                need(3);
                rt.defer(lookup(p[1]), lookup(p[2]));
            } else if (p[0] == "conn") {
                need(2);
                g.pad_state = static_cast<std::int32_t>(to_int(p[1]));
            } else if (p[0] == "al7c") {  // checkpoint 118e: GuiAgentLoad byte +7c
                need(3);
                lookup(p[1])->al_7c = static_cast<std::uint8_t>(to_int(p[2]));
            } else if (p[0] == "gset") {  // checkpoint 118f: one of GuiOnlineOfflineSelect's input bytes
                need(3);
                const auto v = static_cast<std::uint8_t>(to_int(p[2]));
                if (p[1] == "b51eea0") g.b_eea0 = v;
                else if (p[1] == "b51eea8") g.b_eea8 = v;
                else if (p[1] == "b506488") g.b_6488 = v;
                else if (p[1] == "b506490") g.b_6490 = v;
                else if (p[1] == "a31") g.app_31 = v;
                else if (p[1] == "mc59d") g.card.present = v;
                else if (p[1] == "pb68") host.pb68 = v;  // checkpoint 118g: profile +b68
                else if (p[1] == "eeb0") g.debug_triangle = v;  // checkpoint 132ln2: the update freeze 0051eeb0
                else throw std::runtime_error("gui script: bad op " + o);
            } else if (p[0] == "key") {
                if (p.size() < 2 || p.size() > 4) throw std::runtime_error("gui script: bad op " + o);
                GuiHost::UsbKeyReport k;
                k.code = static_cast<std::uint16_t>(to_int(p[1], 16));
                k.mod = p.size() > 2 ? static_cast<std::uint32_t>(to_int(p[2], 16)) : 0;
                k.count = p.size() > 3 ? static_cast<std::int32_t>(to_int(p[3])) : 1;
                host.keys.push_back(k);
            } else if (p[0] == "ins") {
                if (p.size() != 2 && p.size() != 3) throw std::runtime_error("gui script: bad op " + o);
                std::string s;
                const std::string h = p.size() > 2 ? p[2] : std::string();
                if (h.size() % 2) throw std::runtime_error("gui script: bad op " + o);
                for (std::size_t k = 0; k < h.size(); k += 2)
                    s.push_back(static_cast<char>(to_int(h.substr(k, 2), 16)));
                // 0041a2c0 takes a C string
                if (const auto z = s.find('\0'); z != std::string::npos) s.resize(z);
                log.push_back("ins -> " + std::to_string(rt.edit_insert(lookup(p[1]), s)));
            } else if (p[0] == "del") {
                need(3);
                log.push_back("del -> " + std::to_string(rt.edit_delete(lookup(p[1]), to_int(p[2]) != 0)));
            } else if (p[0] == "cur") {
                need(3);
                log.push_back("cur -> " + std::to_string(rt.edit_cursor(lookup(p[1]),
                                                                         static_cast<std::int32_t>(to_int(p[2])))));
            } else if (native && p[0] == "open") {
                need(6);
                auto opt = [](const std::string& v) { return v == "-" ? std::optional<std::string>() : std::optional<std::string>(v); };
                const std::int32_t r = rt.popup_open(static_cast<std::int32_t>(to_int(p[1])), opt(p[2]), opt(p[3]),
                                                     lookup(p[4]), static_cast<std::int32_t>(to_int(p[5])));
                log.push_back("open -> " + std::to_string(r));
            } else if (native && p[0] == "close") {
                need(2);
                log.push_back("close -> " + std::to_string(rt.popup_close(static_cast<std::int32_t>(to_int(p[1])))));
            } else if (native && p[0] == "query") {
                need(2);
                const auto id = static_cast<std::int32_t>(to_int(p[1]));
                if (rt.popup_query(id)) {
                    for (const PopupEntry& e : rt.popups)
                        if (e.id == id) {
                            log.push_back("query -> entry " + std::to_string(e.id) + " " + std::to_string(e.type));
                            break;
                        }
                } else {
                    log.push_back("query -> 0");
                }
            } else if (native && p[0] == "fact") {
                need(3);
                Widget* r = rt.factory(p[1], p[2] == "-" ? std::optional<std::string>() : std::optional<std::string>(p[2]));
                log.push_back("fact -> " + nm(r));
            } else if (native && p[0] == "rel") {
                need(2);
                log.push_back("rel -> " + std::to_string(rt.release(lookup(p[1]))));
            } else if (native && p[0] == "flush") {
                need(1);
                rt.flush();
            } else if (native && p[0] == "unl") {
                need(3);
                rt.unload(lookup(p[1]), to_int(p[2]) != 0);
            } else if (p[0] == "ocdnas") {  // checkpoint 132ln2: GuiOnlineConnect's DNAS error popup 0029d980
                need(2);
                if (root->cls != WidgetClass::onlinecn) throw std::runtime_error("gui script: ocdnas needs GuiOnlineConnect");
                rt.oc_dnas_popup(root, static_cast<std::int32_t>(to_int(p[1])));
            } else if (p[0] == "unread") {  // checkpoint 132on2: the session's +20 (an input to 0033cca0)
                need(2);
                host.unread = to_int(p[1]) != 0;
            } else if (p[0] == "gcache") {
                // checkpoint 132on2i: gcache:world,players,max,creator,mission,filter,seconds,address,status: one
                // GameInfoResponse (status 0) to 00446ae0 -> 0044edc0 (GameStats +8 is the world, +3c)
                need(2);
                std::vector<std::int32_t> v;
                for (std::size_t a = 0;;) {
                    const auto b = p[1].find(',', a);
                    v.push_back(static_cast<std::int32_t>(
                        to_int(p[1].substr(a, b == std::string::npos ? std::string::npos : b - a))));
                    if (b == std::string::npos) break;
                    a = b + 1;
                }
                if (v.size() != 9) throw std::runtime_error("gui script: bad op " + o);
                GuiHost::LobbyCachedGame cg;
                cg.world = v[0];
                cg.players = v[1];
                cg.max_players = v[2];
                cg.stats = {v[3], v[4], v[0], v[5], v[6], v[7]};
                cg.world_status = v[8];
                if (cg.world != 0) {
                    auto it = std::find_if(host.gcache_.begin(), host.gcache_.end(),
                                           [&](const GuiHost::LobbyCachedGame& e) { return e.world == cg.world; });
                    const bool same = it != host.gcache_.end() && it->players == cg.players &&
                                      it->max_players == cg.max_players && it->world_status == cg.world_status &&
                                      it->stats == cg.stats;
                    if (!same) {
                        if (it != host.gcache_.end()) *it = cg;
                        else host.gcache_.push_back(cg);
                        host.gcache_new_.push_back(cg);
                    }
                }
            } else if (p[0] == "glist") {  // checkpoint 132on2b: one complete game list (world,status,players,hex name)
                GuiHost::LobbyGameList l{host.glist_.serial + 1, {}};
                for (std::size_t k = 1; k < p.size(); ++k) {
                    std::vector<std::string> fld;
                    for (std::size_t a = 0;;) {
                        const auto b = p[k].find(',', a);
                        fld.push_back(p[k].substr(a, b == std::string::npos ? std::string::npos : b - a));
                        if (b == std::string::npos) break;
                        a = b + 1;
                    }
                    if (fld.size() != 4 || (fld[3] != "-" && fld[3].size() % 2))
                        throw std::runtime_error("gui script: bad op " + o);
                    GuiHost::LobbyGameRow r;
                    r.world = static_cast<std::int32_t>(to_int(fld[0]));
                    r.status = static_cast<std::int32_t>(to_int(fld[1]));
                    r.players = static_cast<std::int32_t>(to_int(fld[2]));
                    if (fld[3] != "-")
                        for (std::size_t c = 0; c < fld[3].size() && r.name.size() < 0x3f; c += 2) {
                            const char ch = static_cast<char>(to_int(fld[3].substr(c, 2), 16));
                            if (!ch) break;
                            r.name.push_back(ch);
                        }
                    l.rows.push_back(std::move(r));
                }
                host.glist_ = std::move(l);
            } else if (p[0] == "plist" || p[0] == "chat" || p[0] == "clist") {
                // checkpoint 132on2c: plist (id,status,hex name ...): one complete player list (00447a30); chat
                // (e,MessageID,status,hex own name,hex text: the TextFilter echo 00447bb0; f,id,type,hex name,hex
                // text: a ChatFwd 00447c70)
                auto fields = [&](const std::string& s) {
                    std::vector<std::string> fld;
                    for (std::size_t a = 0;;) {
                        const auto b = s.find(',', a);
                        fld.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
                        if (b == std::string::npos) break;
                        a = b + 1;
                    }
                    return fld;
                };
                auto unhex = [&](const std::string& h, std::size_t max) {
                    std::string v;
                    if (h == "-") return v;
                    if (h.size() % 2) throw std::runtime_error("gui script: bad op " + o);
                    for (std::size_t c = 0; c < h.size() && v.size() < max; c += 2) {
                        const char ch = static_cast<char>(to_int(h.substr(c, 2), 16));
                        if (!ch) break;
                        v.push_back(ch);
                    }
                    return v;
                };
                if (p[0] == "clist") {  // checkpoint 132on2h: clist (id,status,world,hex name ...): 00446830
                    GuiHost::LobbyContactList l{host.clist_.serial + 1, {}};
                    for (std::size_t k = 1; k < p.size(); ++k) {
                        const auto fld = fields(p[k]);
                        if (fld.size() != 4) throw std::runtime_error("gui script: bad op " + o);
                        GuiHost::LobbyContactRow r;
                        r.id = static_cast<std::int32_t>(to_int(fld[0]));
                        r.status = static_cast<std::int32_t>(to_int(fld[1]));
                        r.world = static_cast<std::int32_t>(to_int(fld[2]));
                        r.name = unhex(fld[3], 0x1f);
                        l.rows.push_back(std::move(r));
                    }
                    host.clist_ = std::move(l);
                } else if (p[0] == "plist") {
                    GuiHost::LobbyPlayerList l{host.plist_.serial + 1, {}};
                    for (std::size_t k = 1; k < p.size(); ++k) {
                        const auto fld = fields(p[k]);
                        if (fld.size() != 3) throw std::runtime_error("gui script: bad op " + o);
                        GuiHost::LobbyPlayerRow r;
                        r.id = static_cast<std::int32_t>(to_int(fld[0]));
                        r.status = static_cast<std::int32_t>(to_int(fld[1]));
                        r.name = unhex(fld[2], 0x1f);
                        l.rows.push_back(std::move(r));
                    }
                    host.plist_ = std::move(l);
                } else {
                    need(2);
                    const auto fld = fields(p[1]);
                    if (fld.size() != 5 || (fld[0] != "e" && fld[0] != "f"))
                        throw std::runtime_error("gui script: bad op " + o);
                    const std::int32_t id = static_cast<std::int32_t>(to_int(fld[1]));
                    const std::int32_t kind = static_cast<std::int32_t>(to_int(fld[2]));
                    GuiHost::LobbyChatLine line{id, unhex(fld[3], 0x1f), unhex(fld[4], 0x3f), false};
                    if (fld[0] == "e") {  // 00447bb0: with the room's lists, status 0 or 4; private when id != 0
                        line.whisper = id != 0;
                        if (host.room_ && (kind == 0 || kind == 4)) host.chat_.push_back(std::move(line));
                    } else if (host.room_) {  // 00447c70: private when the type is 1
                        line.whisper = kind == 1;
                        host.chat_.push_back(std::move(line));
                    } else if (kind == 1) {  // no room: a private line is the session's message of type 9
                        host.msgs_.push_back({9, id, line.name, line.text});
                        host.msgs_changed_ = true;
                    }
                }
            } else if (p[0] == "kbopen") {
                need(7);
                if (!kbd) throw std::runtime_error("gui script: kbopen without kbd");
                auto str = [&](const std::string& h) -> std::optional<std::string> {
                    if (h == "-") return std::nullopt;
                    if (h.size() % 2) throw std::runtime_error("gui script: bad op " + o);
                    std::string v;
                    for (std::size_t k = 0; k < h.size(); k += 2) v.push_back(static_cast<char>(to_int(h.substr(k, 2), 16)));
                    if (const auto z = v.find('\0'); z != std::string::npos) v.resize(z);
                    return v;
                };
                rt.kb_open(kbd, str(p[1]), str(p[2]), lookup(p[3]), static_cast<std::int32_t>(to_int(p[4])),
                           static_cast<std::int32_t>(to_int(p[5])), static_cast<std::uint8_t>(to_int(p[6])));
            } else if (p[0] == "kbclose") {
                need(2);
                if (!kbd) throw std::runtime_error("gui script: kbclose without kbd");
                rt.kb_close(kbd, to_int(p[1]) != 0);
            } else if (p[0] == "kbmod") {
                need(2);
                rt.kb_mod = static_cast<std::uint32_t>(to_int(p[1], 16));
            } else if (p[0] == "cmap") {
                // the tail of 003fdf40: 0043fbb0(0055ca90, path) -> byte 0055d450, on a test file
                if (!script.cdvd) throw std::runtime_error("gui script: cmap needs cdvd");
                if (p.size() != 1 && p.size() != 2) throw std::runtime_error("gui script: bad op " + o);
                std::vector<std::uint8_t> bytes;
                const std::string h = p.size() > 1 ? p[1] : std::string();
                if (h.size() % 2) throw std::runtime_error("gui script: bad op " + o);
                for (std::size_t k = 0; k < h.size(); k += 2)
                    bytes.push_back(static_cast<std::uint8_t>(to_int(h.substr(k, 2), 16)));
                const std::uint8_t r = rt.charmap.load(bytes);
                rt.charmap.ready = r;
                log.push_back("cmap -> " + std::to_string(r) + " n=" + std::to_string(rt.charmap.count));
            } else if (p[0] == "cq") {
                for (std::size_t k = 1; k < p.size(); ++k) {
                    char buf[64];
                    std::snprintf(buf, sizeof buf, "cq %s -> %x", p[k].c_str(),
                                  rt.charmap.lookup(static_cast<std::int32_t>(to_int(p[k], 16))));
                    log.push_back(buf);
                }
            } else if (p[0].size() > 1 && p[0][0] == 'l' && p.size() >= 2 && lookup(p[1]) &&
                       lookup(p[1])->cls == WidgetClass::textlist) {
                // checkpoint 118e: the GuiArrayWidget / GuiTextListWidget services the screens call
                Widget* w = lookup(p[1]);
                auto u = [&](std::size_t k) { return static_cast<std::uint32_t>(to_int(p[k])); };
                auto i32 = [&](std::size_t k) { return static_cast<std::int32_t>(to_int(p[k])); };
                auto hexs = [&](const std::string& h) -> std::optional<std::string> {
                    if (h == "-") return std::nullopt;
                    if (h.size() % 2) throw std::runtime_error("gui script: bad op " + o);
                    std::string v;
                    for (std::size_t k = 0; k < h.size(); k += 2) v.push_back(static_cast<char>(to_int(h.substr(k, 2), 16)));
                    if (const auto z = v.find('\0'); z != std::string::npos) v.resize(z);
                    return v;
                };
                const std::string& op = p[0];
                if (op == "ladd") {
                    need(5);
                    log.push_back("ladd -> " + std::to_string(rt.list_add(w, hexs(p[2]), i32(3),
                                                                           static_cast<std::int8_t>(i32(4)))));
                } else if (op == "lclr") {
                    need(2);
                    rt.list_clear(w);
                } else if (op == "ldel") {
                    need(3);
                    log.push_back("ldel -> " + std::to_string(rt.list_remove(w, i32(2))));
                } else if (op == "lref") {
                    need(2);
                    rt.list_refresh(w);
                } else if (op == "lsel") {
                    need(4);
                    rt.array_select(w, u(2), u(3));
                } else if (op == "ldim") {
                    need(4);
                    rt.array_set_dims(w, u(2), u(3));
                } else if (op == "lvis") {
                    need(4);
                    rt.array_set_visible(w, u(2), u(3));
                } else if (op == "lorg") {
                    need(4);
                    rt.array_set_origin(w, u(2), u(3));
                } else if (op == "lrowh") {
                    need(3);
                    rt.array_set_row_height(w, u(2));
                } else if (op == "lcolw") {
                    need(4);
                    rt.array_set_col_width(w, u(2), u(3));
                } else if (op == "l84") {
                    need(3);
                    rt.array_set_a84(w, i32(2));
                } else if (op == "lget") {
                    need(3);
                    const auto t = rt.list_text(w, i32(2));
                    log.push_back("lget -> " + (t ? "\"" + *t + "\"" : std::string("-")) + " " +
                                  std::to_string(rt.list_id(w, i32(2))) + " " + std::to_string(rt.list_style(w, i32(2))));
                } else if (op == "lfind") {
                    need(3);
                    log.push_back("lfind -> " + std::to_string(rt.list_find(w, hexs(p[2]).value_or(""))));
                } else if (op == "lfid") {
                    need(3);
                    log.push_back("lfid -> " + std::to_string(rt.list_find_id(w, i32(2))));
                } else if (op == "lsty") {
                    need(4);
                    rt.list_set_style(w, i32(2), static_cast<std::int8_t>(i32(3)));
                } else if (op == "lf1") {
                    need(3);
                    rt.list_set_f1(w, static_cast<std::uint8_t>(u(2)));
                } else if (op == "la4") {
                    need(3);
                    rt.list_set_a4(w, static_cast<std::uint8_t>(u(2)));
                } else if (op == "lfont") {
                    need(3);
                    rt.list_set_font(w, hexs(p[2]));
                } else if (op == "lb") {
                    // byte pokes, as screen code stores them: +58 mode, +80 paging, +81 wrap, +82 no scroll
                    need(4);
                    auto& l = *w->list;
                    const auto v = static_cast<std::uint8_t>(u(3));
                    const auto off = to_int(p[2], 16);
                    if (off == 0x58) l.mode = v;
                    else if (off == 0x80) l.paging = v;
                    else if (off == 0x81) l.wrap = v;
                    else if (off == 0x82) l.no_scroll = v;
                    else throw std::runtime_error("gui script: bad op " + o);
                } else if (op == "lvisq") {
                    need(4);
                    log.push_back("lvisq -> " + std::to_string(rt.array_cell_visible(w, u(2), u(3))));
                } else {
                    throw std::runtime_error("gui script: bad op " + o);
                }
            } else {
                throw std::runtime_error("gui script: bad op " + o);
            }
        }
        PadInput in;
        in.current = {f.pad0, f.pad1};
        in.previous = prev;
        prev = in.current;
        in.t1 = f.t1;
        in.ticks = f.ticks;
        rt.translate_pads(in);
        rt.update(std::bit_cast<float>(f.dt));
        emit(std::to_string(i));
    }
    return log;
}

// ---------------------------------------------------------------- GuiTextArrayWidget (vtable 004e0dc0)
// Gain / lose / input are the GuiArrayWidget entries; update / event the
// GuiWidget's. The cells are drawn by 00423670 -> vt +68 00425ea0 (gui_draw).

void GuiRuntime::ta_init(Widget* w) {  // 004262d0
    array_init(w);
    auto& t = *w->ta;
    t.font = host_.font("callout");  // 00418420(00414790(), "callout")
    if (t.font) {
        array_set_row_height(w, static_cast<std::uint32_t>(host_.font_height(t.font)) + 4);
        if (t.obj) t.obj->font = t.font;
    }
}

void GuiRuntime::ta_wake(Widget* w) {  // 004261d0
    array_wake(w);
    auto& t = *w->ta;
    if (t.font) {
        TextObject o;  // 003d0940(o, 0, 0, 10, 10, +94, +a0) (a fresh object each wake)
        o.rect = {0, 0, 10, 10};
        o.font = t.font;
        o.rgba = t.rgba;
        host_.text_object(o);
        t.obj = o;
    }
    if (!t.obj) return;
    t.obj->b12 = 1;
    t.obj->b10 = t.halign;
    t.obj->b58 = 1;
}

void GuiRuntime::ta_sleep(Widget* w) {  // 00426100
    base_sleep(w);  // 00424a50: 0041ebf0 then vt +70
    send_up(w, 0x15, static_cast<std::int32_t>(w->list->col), static_cast<std::int32_t>(w->list->row));
    w->ta->obj.reset();
}

void GuiRuntime::ta_set_dims(Widget* w, std::uint32_t cols, std::uint32_t rows) {  // 00426350
    if (w->list->cols == cols && w->list->rows == rows) return;
    array_set_dims(w, cols, rows);
    // 00426600: rows x cols default cells ("", flags 0, colour 1).
    auto& t = *w->ta;
    t.rows.assign(rows, std::vector<TextArrayCell>(cols));
}

const TextArrayCell* GuiRuntime::ta_cell(const Widget* w, std::uint32_t col, std::uint32_t row) const {  // 00426590
    const auto& t = *w->ta;
    if (row >= t.rows.size() || col >= t.rows[row].size()) return nullptr;
    return &t.rows[row][col];
}

void GuiRuntime::ta_set_cell(Widget* w, std::uint32_t col, std::uint32_t row,
                             const std::optional<std::string>& s) {  // 00426510
    auto& t = *w->ta;
    if (row >= t.rows.size() || col >= t.rows[row].size()) return;
    t.rows[row][col].text = s ? s->substr(0, s->find('\0')) : std::string();  // NULL: "" (004c1a38)
}

void GuiRuntime::ta_set_cell_rgba(Widget* w, std::uint32_t col, std::uint32_t row,
                                  const std::array<float, 4>& rgba) {  // 00425c30
    auto& t = *w->ta;
    if (row >= t.rows.size() || col >= t.rows[row].size()) return;
    t.rows[row][col].rgba = rgba;
    t.rows[row][col].flags |= 4;
}

void GuiRuntime::ta_set_cell_halign(Widget* w, std::uint32_t col, std::uint32_t row, std::uint8_t v) {  // 00425cd0
    auto& t = *w->ta;
    if (row >= t.rows.size() || col >= t.rows[row].size()) return;
    t.rows[row][col].halign = v;
    t.rows[row][col].flags |= 1;
}

void GuiRuntime::ta_set_font(Widget* w, const std::string& name) {  // 00425e20
    auto& t = *w->ta;
    t.font = host_.font(name);
    if (!t.font) return;
    array_set_row_height(w, static_cast<std::uint32_t>(host_.font_height(t.font)) + 4);
    if (t.obj) t.obj->font = t.font;
}

void GuiRuntime::ta_set_halign(Widget* w, std::uint8_t v) {  // 00425e00
    w->ta->halign = v;
    if (w->ta->obj) w->ta->obj->b10 = v;
}

// ---------------------------------------------------------------- GuiWeaponStats (vtable 004e1350)

void GuiRuntime::ws_init(Widget* w) {  // 00455b60
    ta_init(w);
    ta_set_dims(w, 2, 4);
    array_set_col_width(w, 0, 0x73);
    array_set_col_width(w, 1, 0xa);
    ta_set_font(w, "callout");
    ta_set_halign(w, 0);
    ws_fill(w, -1);
}

std::uint8_t GuiRuntime::ws_input(Widget* w, const PadEvent& ev) {  // 004557c0
    if (ev.kind == 1 && ev.port == 0 && ev.mask == pad_triangle && ev.released == 0) {
        sm_back(w);
        return 1;
    }
    return array_input(w, ev);
}

namespace {
// 002c8ec0(v, buf): the roman numeral (v >= 4000 leaves the buffer).
void roman_2c8ec0(std::int32_t v, std::string& buf) {
    if (v >= 4000) return;
    static constexpr std::pair<std::int32_t, const char*> t[] = {{1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"},
                                                                 {100, "C"},  {90, "XC"},  {50, "L"},  {40, "XL"},
                                                                 {10, "X"},   {9, "IX"},   {5, "V"},   {4, "IV"},
                                                                 {1, "I"}};
    buf.clear();
    for (const auto& [n, r] : t)
        while (v >= n) {
            buf += r;
            v -= n;
        }
}
// 00455840's rating: the first i with v <= t[i] gives i + 1, else 5.
std::int32_t ws_rating(std::int32_t v, const std::array<std::int32_t, 5>& t) {
    for (std::size_t i = 0; i < t.size(); ++i)
        if (v <= t[i]) return static_cast<std::int32_t>(i) + 1;
    return 5;
}
}  // namespace

void GuiRuntime::ws_fill(Widget* w, std::int32_t item) {  // 00455840
    for (std::uint32_t r = 0; r < 4; ++r) {
        char key[32];
        std::snprintf(key, sizeof key, "WeaponStat%u", r);
        ta_set_cell(w, 0, r, host_.get_string(key));
    }
    const auto rec = host_.weapon_record(item);
    if (!rec) return;
    for (std::uint32_t r = 0; r < 4; ++r) {
        std::string buf = "---";
        char num[16];
        if (r == 0 && rec->w60 != 0) roman_2c8ec0(ws_rating(rec->w60, {1, 3, 5, 8, 11}), buf);
        if (r == 1 && rec->w64 != 0) roman_2c8ec0(ws_rating(rec->w64, {10, 20, 35, 50, 60}), buf);
        if (r == 2 && rec->clip_70 != 0) {
            std::snprintf(num, sizeof num, "%d", rec->clip_70);
            buf = num;
        }
        if (r == 3) {
            const std::int32_t n = rec->clip_70 * host_.clips_for(rec->clips_74);  // 001861f0(actor, +74)
            if (n != 0) {
                std::snprintf(num, sizeof num, "%d", n);
                buf = num;
            }
        }
        ta_set_cell(w, 1, r, buf);
    }
}

std::array<float, 4> GuiRuntime::ws_cell_colour(std::uint32_t col) {  // 00455ae0
    if (col == 0)  // 00587d50
        return {std::bit_cast<float>(0x3f50d0d1u), std::bit_cast<float>(0x3f6eeeefu), 1.0f, 1.0f};
    return {0.0f, 1.0f, 1.0f, 1.0f};  // 00587d60
}

// ---------------------------------------------------------------- GuiWeaponDisplay (vtable 004e11a0)
// Wake / sleep / gain / lose / event are the GuiWidget entries, input
// 00293950 returns 0; the draw 00440580 is the drawer's host hook.

void GuiRuntime::wd_init(Widget* w) {  // 00441180
    base_init(w);
    Widget* n = find(w, "value_weaponname");  // *004976c8
    w->wd->name = n && n->cls == WidgetClass::text ? n : nullptr;  // isA "GuiTextWidget"
}

void GuiRuntime::wd_set(Widget* w, std::int32_t item) {  // 00441060
    auto& d = *w->wd;
    if (d.item == item) return;
    d.item = item;
    d.model = 0;
    if (item == -1) {
        if (d.name) set_text(d.name, std::string(), false);
        return;
    }
    auto pick = [&]() {
        const auto rec = host_.weapon_record(item);  // 00147840
        if (!rec) unhosted(0x441094, "weapon record of item " + std::to_string(item) + " is null");
        d.model = rec->hold_f0 == 7 && rec->icon_11c != 0 ? rec->icon_11c : rec->model_13c;
        return *rec;
    };
    auto rec = pick();
    if (d.model == 0) {
        host_.weapon_preload(item);  // 00145710(db, item, 0)
        rec = pick();
    }
    if (d.name) {
        const auto s = host_.get_string(rec.name_80);
        set_text(d.name, s ? *s : rec.name_80, false);
    }
}

void GuiRuntime::wd_set_item(Widget* w, std::int32_t item, std::int32_t model,
                             const std::optional<std::string>& name) {  // 00441000
    auto& d = *w->wd;
    if (d.item == item) return;
    d.item = item;
    d.model = model;
    if (d.name) set_text(d.name, name ? *name : std::string(), false);
}

void GuiRuntime::wd_update(Widget* w, float dt) {  // 00440ee0
    base_update(w, dt);  // 0041e3b0
    auto& d = *w->wd;
    if (!d.a4) return;
    d.a4 = 1;
    d.rot = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    d.t = fmodf_ee(fadd(d.t, dt), 120.0f);  // 00121ae8
    const float ang = fmul(std::bit_cast<float>(0x40490fdbu), fdiv(fmul(3.0f, d.t), 180.0f));
    // 00132130(tmp, 0, 1, 0, ang) (the axis 004c0390) then 0023b450(+60, tmp) on the identity.
    const NiMatrix m = ni_axis_angle(0.0f, 1.0f, 0.0f, ang);
    NiMatrix r{};
    for (std::size_t i = 0; i < 4; ++i)
        for (std::size_t c = 0; c < 4; ++c) r[i][c] = d.rot[4 * i + c];
    ni_rotate_rows(r, m);
    for (std::size_t i = 0; i < 4; ++i)
        for (std::size_t c = 0; c < 4; ++c) d.rot[4 * i + c] = r[i][c];
}

// ---------------------------------------------------------------- GuiEquipmentPane (vtable 004e12f0)
// Sleep / update / event are the GuiWidget entries; the draw 0041e470 the
// base (its children).

void GuiRuntime::ep_init(Widget* w) {  // 00455610
    base_init(w);
    auto& e = *w->ep;
    Widget* x = find(w, "weapon_placement");  // *00497998
    e.display = x && x->cls == WidgetClass::weapondisplay ? x : nullptr;  // isA "GuiWeaponDisplay"
    x = find(w, "label_weaponname");
    e.label = x && x->cls == WidgetClass::text ? x : nullptr;
    x = find(w, "value_weaponstats");
    e.stats = x && x->cls == WidgetClass::weaponstats ? x : nullptr;
    x = find(w, "weapon_desc");
    e.ml = x && x->cls == WidgetClass::mltext ? x : nullptr;
    if (!e.ml) return;
    e.ml->ml->max_lines = 0;
    Widget* sb = find(w, "vscroll");
    e.ml->ml->scroll = sb && sb->cls == WidgetClass::scrollbar ? sb : nullptr;
    e.ml->flags |= flag_focusable;
}

void GuiRuntime::ep_wake(Widget* w) {  // 00455550
    base_wake(w);
    if (!w->ep->display || !host_.equip_player()) return;
    const std::int32_t held = host_.equip_held();  // 00176110(player)
    if (held == -1) ep_cycle(w, true);
    else ep_set(w, held);
}

void GuiRuntime::ep_cycle(Widget* w, bool up) {  // 00454f80
    auto& e = *w->ep;
    if (!e.display || !host_.equip_player()) return;
    std::vector<std::int32_t> list;
    for (const std::int32_t it : host_.equip_slots())
        if (it != -1 && std::find(list.begin(), list.end(), it) == list.end()) list.push_back(it);
    for (const auto& s : host_.equip_stacks())
        if (s.count > 0) list.push_back(s.index + 10000);
    const auto n = static_cast<std::int32_t>(list.size());
    std::int32_t idx = -1;
    for (std::int32_t i = 0; i < n; ++i)
        if (list[static_cast<std::size_t>(i)] == e.display->wd->item) {
            idx = i;
            break;
        }
    if (idx < 0 && n > 0) idx = 0;
    if (idx == -1) return;
    idx += up ? 1 : -1;
    if (idx < 0) idx = n - 1;
    if (idx >= n) idx = 0;
    if (n >= 2) send(e.display, &canvas_, 0x10, 0, 0);
    ep_set(w, list[static_cast<std::size_t>(idx)]);
}

void GuiRuntime::ep_set(Widget* w, std::int32_t item) {  // 00455260
    auto& e = *w->ep;
    const bool gadget = item >= 10000;
    const std::int32_t index = item - 10000;
    if (gadget)  // 00441000(display, item, item table[index] resource +28, 00470270(index))
        wd_set_item(e.display, item, host_.item_model(index), host_.item_name(index));
    else
        wd_set(e.display, item);
    if (!e.label || !e.ml || !e.stats) return;
    std::optional<std::string> desc, name;
    bool show = false;
    if (gadget) {
        desc = host_.item_desc(index);  // 00470240
        name = host_.item_name(index);  // 00470270
        std::int32_t count = 0;
        for (const auto& s : host_.equip_stacks())  // 00470560(+3528, index)
            if (s.index == index) count = s.count;
        for (std::uint32_t r = 0; r < 3; ++r)
            for (std::uint32_t c = 0; c < 2; ++c) ta_set_cell(e.stats, c, r, std::string());  // 004c1a38
        ta_set_cell(e.stats, 0, 2, host_.get_string("ItemCount"));
        char num[16];
        std::snprintf(num, sizeof num, "%d", count);
        ta_set_cell(e.stats, 1, 2, std::string(num));
        show = true;
    } else {
        const auto rec = host_.weapon_record(item);
        if (!rec) unhosted(0x4552c0, "weapon record of item " + std::to_string(item) + " is null");
        desc = host_.get_string("D_" + rec->name_80.substr(0, 13));
        const auto n = host_.get_string(rec->name_80);
        name = n ? *n : rec->name_80;
        if (host_.weapon_class(item) < 6) {  // 00143d20
            ws_fill(e.stats, item);
            show = true;
        }
    }
    if (show) e.stats->flags |= flag_visible;
    else e.stats->flags &= ~flag_visible;
    ml_clear(e.ml);
    ml_set_text(e.ml, desc ? *desc : std::string());
    set_text(e.label, name ? *name : std::string(), false);
}

std::uint8_t GuiRuntime::ep_input(Widget* w, const PadEvent& ev) {  // 00454b10
    auto pressed = [&](std::uint32_t m) { return ev.kind == 1 && ev.port == 0 && ev.mask == m && ev.released == 0; };
    auto& e = *w->ep;
    if (pressed(pad_up) && e.ml) {
        ml_up(e.ml, false);
        return 1;
    }
    if (pressed(pad_down) && e.ml) {
        ml_down(e.ml, false);
        return 1;
    }
    if (pressed(pad_cross)) {
        const std::int32_t item = e.display ? e.display->wd->item : -1;
        if (item != -1) {
            bool changed = false;
            const std::int32_t cls = host_.weapon_class(item);  // 00143d20
            if (cls < 6) {
                if (!host_.weapon_is_held(item)) {  // 00147840(item) != 001437e0(actor inventory)
                    host_.equip_item(item);         // 00175ff0(player, item)
                    changed = true;
                }
            } else if (cls == 6 && host_.goggles_33e7() == 0) {
                host_.goggles_toggle();  // 00199860(actor)
                changed = true;
            } else if (cls == 7 && host_.goggles_33e7() != 0) {
                host_.goggles_toggle();
                changed = true;
            }
            if (changed) send(e.display, &canvas_, 0x10, 1, 0);
        }
    }
    if (pressed(1) || pressed(2)) {  // L2 / R2
        ep_cycle(w, ev.mask == 2);
        return 1;
    }
    if (pressed(pad_triangle)) {
        sm_back(w);
        return 1;
    }
    if (pressed(0x800)) {
        sm_exit(w);  // the manager's +68 pop, 00242900(*004ffc2c), 00131470(G, 0)
        return 1;
    }
    return 0;
}


// ---------------------------------------------------------------- GuiScoresEquip (vtable 004df140)
// The start menu's item 4 (SCOREONE.GUI solo, SCORES.GUI with 005721c8): the
// agents' statistics (score_grp) and their carried weapons (equipment_grp),
// toggled by the button "button_equip_score", over GuiCharacterDisplay's
// figure (0035b390). The ctor's display actors (+1a4: 00393ff0 with
// MALE.SKEL / FEMALE.SKEL by 004067e0, "DATA_ID_CasualStand"), its +1b4
// object (0x68 bytes, vtable 004da470) and the front end's 002c9d50 (byte
// 004ffba0 == 2) only serve that figure render; the port keeps the state
// 0035bfb0 writes into them (Widget::ScoresEquip::Figure).

namespace {
// 00492d10: the 20 field names (stride 0x14; init stores the player digit
// '1'..'4' in byte 0 of each).
constexpr std::array<const char*, 20> se_field_names = {
    "value_agentname",  "value_codename",   "value_shots",     "value_hits",      "value_headshots",
    "value_knife",      "value_grenade",    "value_kills",     "value_marks",     "value_elf",
    "value_non_lethal", "value_collateral", "value_deaths",    "value_agentnamee", "value_codenamee",
    "value_sidearm",    "value_back",       "value_auxiliary", "value_grenade2",  "value_melee"};

// 0012b290: newlib toupper over the ctype table 00499c70 + 1 (bit 2: lower
// case). The signed byte (lb) indexes below the table for 0x80..0xff, into
// unrelated data.
char se_toupper(char c) {
    const auto u = static_cast<unsigned char>(c);
    if (u >= 0x80) unhosted(0x12b290, "toupper of a byte >= 0x80 (reads below the ctype table 00499c71)");
    return u >= 'a' && u <= 'z' ? static_cast<char>(u - 0x20) : c;
}

std::string se_int(std::int32_t v) { return std::to_string(v); }  // 00128bd0 "%d" (and "%d%": no '%' printed)
}  // namespace

void GuiRuntime::se_init(Widget* w) {  // 0035cd00
    base_init(w);
    auto& e = *w->se;
    Widget* cd = find(w, "playercharacter");
    if (cd && cd->cls == WidgetClass::chardisplay) {  // *00533760 = w; +48 = 0035df30
        cd->cd_screen = w;
        cd->cd_callback = 0x35df30;
    }
    for (std::int32_t s = 0; s < 4; ++s) {
        for (std::size_t i = 0; i < se_field_names.size(); ++i) {
            Widget* t = find(w, std::to_string(s + 1) + se_field_names[i]);
            t = t && t->cls == WidgetClass::text ? t : nullptr;  // isA "GuiTextWidget"
            e.fields[static_cast<std::size_t>(s)][i] = t;
            if (!t) continue;
            t->b76 = 1;  // 0041c0d0(t, 1)
            if (t->obj) t->obj->b5b = 1;
            t->b75 = 1;  // 0041c0f0(t, 1)
            if (t->obj) t->obj->b58 = 1;
        }
    }
    if (e.b60) {
        for (std::size_t i = 0; i < 4; ++i) {
            e.net[i] = find(w, "networkconnection" + std::to_string(i + 1));
            if (e.net[i]) e.net[i]->flags &= ~flag_visible;
        }
        if (g.b_21c8 && e.net[0]) {
            // 0035cedc: the first icon's decorator onWake record supplies the
            // shared frame range for all four networkconnection widgets.
            deco_record_ = nullptr;
            if (send(w, e.net[0], 4, 0x4bffa8, 0) && deco_record_) {
                e.wake_58 = deco_record_->a;
                e.wake_5c = deco_record_->b;
            }
            deco_record_ = nullptr;
        }
    }
    for (std::size_t i = 0; i < 4; ++i) {
        e.grp_230[i] = find(w, std::to_string(i + 1) + "player_stats_grp");
        e.grp_234[i] = find(w, std::to_string(i + 1) + "player_stats_grpe");
    }
}

void GuiRuntime::se_view(Widget* w, std::int32_t v) {
    // 0035cbf0 / 0035b210 / 0035aec0: +224 = v; score_grp shown for 0,
    // equipment_grp for 1; the button reads ScoresEquipScr<v> ("" without).
    w->se->scores_224 = v;
    if (Widget* s = find(w, "score_grp")) {
        if (v == 0) s->flags |= flag_visible;
        else s->flags &= ~flag_visible;
    }
    if (Widget* q = find(w, "equipment_grp")) {
        if (v != 0) q->flags |= flag_visible;
        else q->flags &= ~flag_visible;
    }
    Widget* b = find(w, "button_equip_score");
    if (!b || b->cls != WidgetClass::button) return;  // isA "GuiButtonWidget"
    const auto t = host_.get_string(v == 0 ? "ScoresEquipScr0" : "ScoresEquipScr1");
    set_text(b, t ? *t : std::string(), false);
}

void GuiRuntime::se_wake(Widget* w) {  // 0035cbf0
    base_wake(w);
    se_view(w, 0);
    w->se->b22c = 1;
}

std::uint8_t GuiRuntime::se_input(Widget* w, const PadEvent& ev) {  // 0035aec0
    auto& e = *w->se;
    const bool pad0 = ev.kind == 1 && ev.port == 0;
    if (pad0 && ev.mask == pad_triangle && ev.released == 0) {
        if (e.scores_224 == 0) sm_back(w);  // 00456160
        else se_view(w, 0);
        return 1;
    }
    if (pad0 && ev.mask == 0x800 && ev.released == 0) {
        if (!g.controller) unhosted(0x35b090, "start press without the HUD (*004ffc2c)");
        if (g.controller_6a8) {  // *(*004ffc2c + 6a8)
            sm_exit(w);          // 00456100
            return 1;
        }
    }
    if (pad0 && ev.mask == 2) {  // R2: -1 held, +1 released
        e.turn_228 += ev.released == 0 ? -1.0f : 1.0f;
        return 1;
    }
    if (pad0 && ev.mask == 1) {  // L2: +1 held, -1 released
        e.turn_228 += ev.released == 0 ? 1.0f : -1.0f;
        return 1;
    }
    return 0;
}

std::uint8_t GuiRuntime::se_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                  std::int32_t t0) {  // 0035b210
    if ((type & 0xffff) != 2) return base_event(w, sender, type, a3, t0);  // 0041ef10
    se_view(w, w->se->scores_224 == 0 ? 1 : 0);
    return 1;
}

void GuiRuntime::se_update(Widget* w, float dt) {  // 0035bfb0
    auto& e = *w->se;
    auto text = [&](Widget* t, const std::string& v, std::uint32_t at) {
        if (!t) unhosted(at, "0041beb0 on a GuiScoresEquip text field the screen lacks");
        set_text(t, v, false);
    };
    for (auto& f : e.fields) {
        if (!f[0]) continue;
        text(f[0], "", 0x35c000);
        text(f[1], "", 0x35c014);
        text(f[13], "", 0x35c028);
        text(f[14], "", 0x35c03c);
    }
    for (std::int32_t i = 0; i < 4; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const auto id = host_.scores_player(i);  // 003fb8d0(i)
        if (id) {
            const std::int32_t slot =
                std::max<std::int32_t>(0, static_cast<std::int32_t>((static_cast<std::uint32_t>(*id) & 0x7f000000u) >> 24) - 1);
            if (slot > 3) unhosted(0x35c0ac, "a player slot above 3 (display +1a4 index)");
            const auto us = static_cast<std::size_t>(slot);
            auto& fig = e.figures[us];
            fig.attach = host_.scores_attach(i);  // 003927a0(display[slot], p + 60)
            fig.b2c71 = 1;
            e.shown_220[us] = 1;
            auto& f = e.fields[us];
            if (host_.scores_agent_b70(i)) {  // 00185c70(p) +b70
                const auto name = host_.scores_agent_name(i);  // 00336170
                text(f[0], name, 0x35c118);
                text(f[13], name, 0x35c130);
                const std::string key = "PlayerName" + std::to_string(slot);
                const auto pn = host_.get_string(key);  // 003fdb50
                if (!pn) unhosted(0x35c16c, "sprintf %s of the missing string '" + key + "'");
                std::string code = std::string("[") + '\x11' + *pn + '\x10' + "]";  // "[%c%s%c]" into 00533790
                for (char& c : code) c = se_toupper(c);
                text(f[1], code, 0x35c1d4);
                text(f[14], code, 0x35c1e8);
                auto stat = [&](std::uint32_t k) { return host_.scores_statistic(i, k); };  // 00333ee0
                auto percent = [&](std::uint32_t num, std::uint32_t den) {
                    std::int32_t d = stat(den);
                    if (d == 0) d = 1;
                    const auto n = static_cast<std::int32_t>(static_cast<std::uint32_t>(stat(num)) * 100u);
                    return static_cast<std::int32_t>(static_cast<std::int64_t>(n) / d);  // div
                };
                text(f[2], se_int(stat(0x1b)), 0x35c218);
                text(f[3], se_int(stat(0x1c)), 0x35c248);
                text(f[4], se_int(stat(0x14)), 0x35c278);
                text(f[5], se_int(stat(0x13)), 0x35c2a8);
                text(f[6], se_int(stat(0x16)), 0x35c2d8);
                text(f[7], se_int(stat(0x12)), 0x35c308);
                text(f[9], se_int(percent(0x1c, 0x1b)), 0x35c378);
                text(f[8], se_int(percent(0x1e, 0x1d)), 0x35c3e8);
                text(f[10], se_int(stat(0x18)), 0x35c418);
                text(f[11], se_int(stat(0x19)), 0x35c448);
                text(f[12], se_int(stat(0x1a)), 0x35c478);
            } else {
                text(f[0], "", 0x35c4a4);
                for (std::size_t k = 2; k < 13; ++k) text(f[k], "#", 0x35c4c4);
                for (std::size_t k = 15; k < 20; ++k) text(f[k], "", 0x35c4f4);
            }
            // The carried weapons from the copied table: the hand's (point 0)
            // goes to its record's +30 point, the five names are read, then
            // that point is cleared.
            auto& t = fig.attach;
            auto record = [&](std::int32_t item, std::uint32_t at) {
                const auto r = host_.weapon_record(item);  // 00147840(*004ffd30, model +40a)
                if (!r) unhosted(at, "a null weapon record of an attached model");
                return *r;
            };
            auto point_of_hand = [&](std::uint32_t at) {
                const std::uint8_t k = record(t[0], at).class_30;
                if (k >= t.size()) unhosted(at, "an attach point >= 10 from record +30");
                return static_cast<std::size_t>(k);
            };
            if (t[0] != -1) t[point_of_hand(0x35c530)] = t[0];
            auto weapon = [&](Widget* field, std::size_t k, std::uint32_t at) {
                if (t[k] == -1) {
                    text(field, "", at);
                    return;
                }
                const auto r = record(t[k], at);
                const auto s = host_.get_string(r.name_80);  // 003fdb50(rec + 80), else the key
                text(field, s ? *s : r.name_80, at);
            };
            weapon(f[15], 3, 0x35c580);
            weapon(f[16], 2, 0x35c5f8);
            weapon(f[17], 5, 0x35c670);
            weapon(f[18], 8, 0x35c6e8);
            weapon(f[19], 9, 0x35c760);
            if (t[0] != -1) t[point_of_hand(0x35c7bc)] = -1;
        } else {
            e.figures[ui].b2c3a = -1;
            auto& f = e.fields[ui];
            if (f[0]) {
                text(f[0], "", 0x35c7f8);
                for (std::size_t k = 2; k < 13; ++k) text(f[k], "#", 0x35c814);
                for (std::size_t k = 15; k < 20; ++k) text(f[k], "", 0x35c844);
            }
        }
        for (Widget* grp : {e.grp_230[ui], e.grp_234[ui]}) {
            if (!grp) continue;
            if (id) grp->flags |= flag_visible;
            else grp->flags &= ~flag_visible;
        }
    }
    if (e.b60) {
        for (std::size_t i = 0; i < 4; ++i) {
            Widget* icon = e.net[i];
            if (!icon) continue;
            // 0035c91c: no player or player +14 set hides the icon.
            const auto player = host_.scores_player(static_cast<std::int32_t>(i));
            const auto status = player ? host_.scores_connection_status(static_cast<std::int32_t>(i))
                                       : std::nullopt;
            if (!status) {
                icon->flags &= ~flag_visible;
                continue;
            }
            if (!std::isfinite(*status) || *status < 0.0f || *status > 1.0f)
                unhosted(0x42b320, "a connection status outside the original 0..1 range");
            icon->flags |= flag_visible;
            const float fraction = fsub(1.0f, *status);
            const std::int32_t frame = e.wake_58 +
                cvt_w(fmul(static_cast<float>(e.wake_5c - e.wake_58), fraction), 0x35ca04);
            send(w, icon, 0xb, frame, 0);  // 0041e0f0 -> decorator 003f44e0
        }
    }
    if (top() == w) {  // 004147a0()->+2c->+8
        constexpr float pi = 3.14159274f;  // 0x40490fdb
        constexpr float two_pi = 6.28318548f;  // 0x40c90fdb
        float r = e.rot_1c0[2] + pi * (dt / 30.0f) * e.turn_228;
        if (r > pi) r -= two_pi;
        else if (r < -pi) r += two_pi;
        e.rot_1c0[2] = r;
    } else {
        e.turn_228 = 0.0f;
    }
    base_update(w, dt);
}

// ---------------------------------------------------------------- port-only Cheats screen

// Port-only (checkpoint 132f5, not in the original): PortGuiCheats, the pause
// menu's Cheats screen (PORTCHEATS, synthesised by the host from GAMEOPT2's
// widgets). Each cheat is a GAMEOPT2 value button "pc_value%d" showing
// MB_On / MB_Off as the options' toggles do (go_refresh); Cross toggles it,
// Triangle returns to the start menu and Start closes it (as go_input / se_input).
void GuiRuntime::pc_init(Widget* w) {
    base_init(w);
    auto& o = *w->pc;
    o.values.clear();
    const std::int32_t n = host_.port_cheat_count();
    for (std::int32_t i = 0; i < n; ++i) {
        Widget* b = find(w, "pc_value" + std::to_string(i));
        if (!b || b->cls != WidgetClass::button) throw std::runtime_error("port cheats: PORTCHEATS has no button pc_value" + std::to_string(i));
        o.values.push_back(b);
    }
    o.on = host_.get_string("MB_On");
    o.off = host_.get_string("MB_Off");
}

void GuiRuntime::pc_wake(Widget* w) {
    base_wake(w);
    pc_refresh(w);
}

void GuiRuntime::pc_refresh(Widget* w) {
    auto& o = *w->pc;
    for (std::size_t i = 0; i < o.values.size(); ++i) {
        if (host_.port_cheat_action(static_cast<std::int32_t>(i))) {
            set_text(o.values[i], std::string(), false);
            continue;
        }
        const auto& s = host_.port_cheat_on(static_cast<std::int32_t>(i)) ? o.on : o.off;
        set_text(o.values[i], s ? *s : std::string(), false);
    }
}

std::uint8_t GuiRuntime::pc_input(Widget* w, const PadEvent& ev) {
    auto pressed = [&](std::uint32_t m) { return ev.kind == 1 && ev.port == 0 && ev.mask == m && ev.released == 0; };
    if (pressed(pad_triangle)) {
        sm_back(w);
        return 1;
    }
    if (pressed(0x800)) {
        sm_exit(w);
        return 1;
    }
    return 0;
}

std::uint8_t GuiRuntime::pc_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {
    if ((type & 0xffff) == 2) {
        const auto& v = w->pc->values;
        const auto it = std::find(v.begin(), v.end(), sender);
        if (it != v.end()) {
            if (host_.port_cheat_toggle(static_cast<std::int32_t>(it - v.begin()))) {
                sm_exit(w);
                return 1;
            }
            pc_refresh(w);
            return 1;
        }
    }
    return base_event(w, sender, type, a3, t0);
}

// ---------------------------------------------------------------- port-only Port Settings screen

// Port-only (checkpoint 132win11, not in the original): PortGuiSettings, the
// Port Settings screen (PORTSETTINGS, synthesised by the host from GAMEOPT2's
// widgets like PORTCHEATS). The host's PortSettingsScreen model owns the rows;
// the screen shows them in the fixed slots "ps_label%d" / "ps_value%d" (value
// buttons, focusable as the options' rows) and hides the slots of empty rows.
// Up/Down move between rows (canvas_navigate), Left/Right cycle a value, Cross
// presses it, L1/R1 turn the page; Triangle leaves (to the pause menu in game,
// to the options screen in the front end) and Start closes the pause menu, as
// in PortGuiCheats.
void GuiRuntime::ps_init(Widget* w) {
    base_init(w);
    auto& o = *w->port_settings_rows;
    o.labels.clear();
    o.values.clear();
    o.backgrounds.clear();
    for (std::int32_t i = 0; i < PortSettingsScreen::rows; ++i) {
        const auto n = std::to_string(i);
        Widget* v = find(w, "ps_value" + n);
        if (!v || v->cls != WidgetClass::button) throw std::runtime_error("port settings: PORTSETTINGS has no button ps_value" + n);
        o.values.push_back(v);
        o.labels.push_back(find(w, "ps_label" + n));
        o.backgrounds.push_back(find(w, "ps_background" + n));
    }
    o.title = find(w, "ps_title");
}

void GuiRuntime::ps_wake(Widget* w) {
    if (PortSettingsScreen* m = host_.port_settings_screen()) m->opened();
    w->port_settings_rows->shown = false;
    ps_refresh(w);
    base_wake(w);
}

void GuiRuntime::ps_refresh(Widget* w) {
    auto& o = *w->port_settings_rows;
    PortSettingsScreen* m = host_.port_settings_screen();
    if (!m) return;
    if (o.title) set_text(o.title, m->title(), false);
    Widget* first = nullptr;
    bool focus_lost = false;
    for (std::size_t i = 0; i < o.values.size(); ++i) {
        const std::string label = m->label(static_cast<std::int32_t>(i));
        const bool on = !label.empty();
        for (Widget* x : {o.labels[i], o.values[i], o.backgrounds[i]}) {
            if (!x) continue;
            if (on) x->flags |= flag_visible;
            else x->flags &= ~flag_visible;
        }
        if (!on && canvas_.focus == o.values[i]) focus_lost = true;
        if (!on) continue;
        if (!first) first = o.values[i];
        if (o.labels[i]) set_text(o.labels[i], label, false);
        set_text(o.values[i], m->value(static_cast<std::int32_t>(i)), false);
    }
    if (focus_lost && first) set_focus(first);
    o.revision = m->revision();
    o.shown = true;
}

void GuiRuntime::ps_update(Widget* w, float dt) {
    PortSettingsScreen* m = host_.port_settings_screen();
    if (m && (!w->port_settings_rows->shown || m->revision() != w->port_settings_rows->revision)) ps_refresh(w);
    base_update(w, dt);
}

void GuiRuntime::ps_leave(Widget* w) {
    if (PortSettingsScreen* m = host_.port_settings_screen()) m->closed();
    if (game_canvas) {
        sm_back(w);
        return;
    }
    const std::string ret = w->port_settings_rows->return_focus;
    Widget* back = factory("GuiGameOptions", std::string("GAMEOPT"));
    switch_screens(w, back);
    if (!back || !back->go || ret.empty()) return;
    Widget* f = find(back, ret);
    if (!f) return;
    auto& g2 = *back->go;
    for (std::size_t k = 0; k < g2.groups.size(); ++k) {
        bool inside = false;
        for (Widget* p = f->parent; p && !inside; p = p->parent) inside = p == g2.groups[k];
        if (!inside) continue;
        go_show(back, static_cast<std::int32_t>(k));
        go_enter(back);
        set_focus(f);
        back->focus = f;
        break;
    }
}

std::uint8_t GuiRuntime::ps_input(Widget* w, const PadEvent& ev) {
    auto pressed = [&](std::uint32_t m) { return ev.kind == 1 && ev.port == 0 && ev.mask == m && ev.released == 0; };
    PortSettingsScreen* m = host_.port_settings_screen();
    if (pressed(pad_triangle)) {
        if (m && m->back()) {
            send(w, &canvas_, 0x10, 4, 0);
            ps_refresh(w);
            return 1;
        }
        send(w, &canvas_, 0x10, 4, 0);
        ps_leave(w);
        return 1;
    }
    if (pressed(0x800) && game_canvas) {
        if (m) m->closed();
        sm_exit(w);
        return 1;
    }
    if (!m) return 0;
    const auto& v = w->port_settings_rows->values;
    const auto it = std::find(v.begin(), v.end(), canvas_.focus);
    const std::int32_t row = it == v.end() ? -1 : static_cast<std::int32_t>(it - v.begin());
    auto act = [&](PortSettingsScreen::Action a, std::int32_t r) {
        if (m->act(r, a)) send(w, &canvas_, 0x10, 0, 0);
        ps_refresh(w);
        return std::uint8_t{1};
    };
    if (pressed(0x8000) && row >= 0) return act(PortSettingsScreen::Action::previous, row);
    if (pressed(0x2000) && row >= 0) return act(PortSettingsScreen::Action::next, row);
    if (pressed(0x4)) return act(PortSettingsScreen::Action::page_previous, row);
    if (pressed(0x8)) return act(PortSettingsScreen::Action::page_next, row);
    return 0;
}

std::uint8_t GuiRuntime::ps_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3, std::int32_t t0) {
    if ((type & 0xffff) == 2) {
        const auto& v = w->port_settings_rows->values;
        const auto it = std::find(v.begin(), v.end(), sender);
        PortSettingsScreen* m = host_.port_settings_screen();
        if (it != v.end() && m) {
            if (m->act(static_cast<std::int32_t>(it - v.begin()), PortSettingsScreen::Action::press))
                send(w, &canvas_, 0x10, 1, 0);
            ps_refresh(w);
            return 1;
        }
    }
    return base_event(w, sender, type, a3, t0);
}

bool GuiRuntime::port_open_settings(Widget* options) {
    if (!options || !options->go || !host_.port_settings_screen()) return false;
    const std::string ret = canvas_.focus ? canvas_.focus->name : std::string();
    go_close(options, false);
    Widget* s = factory("PortGuiSettings", std::string("PORTSETTINGS"));
    if (!s) return false;
    s->port_settings_rows->return_focus = ret;
    s->flags |= flag_focusable;
    switch_screens(options, s);
    return true;
}

// ---------------------------------------------------------------- GuiEquipmentModify (vtable 004ddee0)
// The level-start loadout screen EQMODIFY (checkpoint 132f4). The preview
// model +4c's attach table and the player's are kept as weapon records (-1
// for an empty point); see Widget::EquipModify.
namespace {
constexpr std::int32_t em_empty = -1;
constexpr std::uint32_t em_on_pressed = 0x4ac360;  // "onPressed"
Widget* em_isa(Widget* c, WidgetClass k) { return c && c->cls == k ? c : nullptr; }
}  // namespace

void GuiRuntime::em_build(Widget* w) {  // 002c1100 (the player copy)
    auto& e = *w->em;
    e.built = true;
    e.cat_84 = -1;
    e.point_88 = 2;
    e.fc = 0;
    e.player = host_.scores_attach(0);  // +48: solo, the game's player 0
    e.preview = e.player;               // 0038c920 / 003913d0: the preview copies the player's model
    if (e.player[0] != em_empty) {
        const auto r = host_.weapon_record(e.player[0]);
        if (!r) unhosted(0x2c1150, "a null weapon record of the player's held model");
        if (r->class_30 > 0 && r->class_30 < 10) e.fc = r->class_30;
    }
    e.fc_100 = e.fc;
    e.initial.clear();
    for (const std::int32_t m : e.preview)
        if (m != em_empty) e.initial.push_back(m);  // +dc items, +f0 their record +30 (re-read from the record)
    e.box1 = e.box2 = nullptr;
    e.items.clear();
    e.sel_108 = 0;
    e.list_8c = 0;
    e.sel_90c.fill(0);
    e.ring.clear();
}

std::int32_t GuiRuntime::em_box_model(const Widget& screen, std::uint32_t callback) {
    if (!screen.em) unhosted(callback, "a weapon box callback without its GuiEquipmentModify screen *0051ee10");
    const auto& e = *screen.em;
    if (e.cat_84 == 0) return 0;
    std::int32_t record = -1;
    if (callback == 0x2c2290) {
        const std::int32_t point = e.point_88 == e.fc ? 0 : e.point_88;
        if (point < 0 || point >= static_cast<std::int32_t>(e.preview.size()))
            unhosted(0x2c22c0, "attach point +88 outside the preview actor's +2470 table");
        record = e.preview[static_cast<std::size_t>(point)];  // the model's +40a (-1: no model)
        if (record == -1 || !e.box1) return 0;
    } else if (callback == 0x2c21c0) {
        if (e.list_8c == 0) return 0;
        if (e.sel_108 >= static_cast<std::int32_t>(e.items.size())) return 0;
        const auto r = host_.weapon_record(e.items.at(static_cast<std::size_t>(e.sel_108)));
        if (!r) unhosted(0x2c2218, "a null weapon record for the selected list item (+13c of null)");
        if (r->model_13c == 0 || !e.box2) return 0;
        record = e.items.at(static_cast<std::size_t>(e.sel_108));  // the loaded model's +40a
        if (!host_.weapon_record(record)) unhosted(0x2c2248, "a null weapon record of the box model");
        return r->hold_f0 == 7 ? r->icon_11c : r->model_13c;
    } else {
        unhosted(callback, "GuiCharacterDisplay callback other than 002c2290 / 002c21c0 as a weapon box");
    }
    const auto r = host_.weapon_record(record);  // 00147840(*004ffd30, model +40a)
    if (!r) unhosted(0x2c22f8, "a null weapon record of the box model (+f0 of null)");
    return r->hold_f0 == 7 ? r->icon_11c : r->model_13c;
}

float GuiRuntime::em_figure_turn(const Widget& screen) {
    if (!screen.em) unhosted(0x2bfe18, "the figure callback without its GuiEquipmentModify screen *0051ee10");
    auto& e = *screen.em;
    if (e.cat_84 < -1 || e.cat_84 >= static_cast<std::int32_t>(e.turn_ac.size()))
        unhosted(0x2bfe28, "+ac + 4 * +84 outside the ctor's turn table");
    const float target = e.cat_84 == -1 ? std::bit_cast<float>(e.rows_a8) : e.turn_ac[static_cast<std::size_t>(e.cat_84)];
    const float now = ftz(e.yaw_68), to = ftz(target);
    if (now != to) e.yaw_68 = fadd(now, fdiv(fsub(to, now), 8.0f));  // c.eq.s, sub.s, div.s 8, add.s
    return e.yaw_68;
}

void GuiRuntime::em_init(Widget* w) {  // 002c0790
    base_init(w);
    auto& e = *w->em;
    if (Widget* cd = em_isa(find(w, "playercharacter"), WidgetClass::chardisplay)) {  // +48 = 002c2340
        cd->cd_screen = w;
        cd->cd_callback = 0x2c2340;
    }
    e.box1 = em_isa(find(w, "1weaponbox"), WidgetClass::chardisplay);
    if (e.box1) {  // +48 = 002c2290
        e.box1->cd_screen = w;
        e.box1->cd_callback = 0x2c2290;
    }
    e.box2 = em_isa(find(w, "2weaponbox"), WidgetClass::chardisplay);
    if (e.box2) {  // +48 = 002c21c0
        e.box2->cd_screen = w;
        e.box2->cd_callback = 0x2c21c0;
    }
    e.list = em_isa(find(w, "weaponlist"), WidgetClass::textlist);
    if (!e.list) unhosted(0x2c0924, "EQMODIFY without its GuiTextListWidget \"weaponlist\" (+34 of a null list)");
    e.x_98 = cvt_w(e.list->rect[0], 0x2c0928);
    e.y_9c = cvt_w(e.list->rect[1], 0x2c0940);
    e.row_a4 = static_cast<std::int32_t>(e.list->list->row_h);
    e.rows_a8 = cvt_w(e.list->rect[3] / static_cast<float>(e.row_a4) - 1.0f, 0x2c0974);
    e.highlight = find(w, "select_weapon_highlight");
    if (!e.highlight) unhosted(0x2c0998, "EQMODIFY without its \"select_weapon_highlight\" (+14 of a null widget)");
    e.highlight->flags &= ~flag_focusable;
    e.list->flags &= ~flag_focusable;
    auto text = [&](const char* name) {
        Widget* t = find(w, name);
        return t && is_text(t) ? t : nullptr;  // isA "GuiTextWidget"
    };
    auto put = [&](Widget* t, const std::optional<std::string>& s, std::uint32_t at) {
        if (!t) unhosted(at, "0041beb0 on an EQMODIFY text field the screen lacks");
        if (!s) unhosted(at, "0041beb0 of a null string");
        set_text(t, *s, false);
    };
    Widget* agent = text("value_agent");
    Widget* rank = text("value_rank");
    if (host_.scores_agent_b70(0)) {  // 00185c70(+48) +b70
        put(agent, host_.scores_agent_name(0), 0x2c0a54);  // 00336170
        put(rank, host_.eqm_agent_rank(), 0x2c0a74);       // 00336110
    } else {
        put(agent, std::string("Player"), 0x2c0a94);
        put(rank, host_.string_after("Rank_00", 1), 0x2c0ab8);  // 003fdb80(003fdb40("Rank_00") + 1)
    }
    if (g.app_251) {  // *004ffc04 +251: the categories are locked
        for (const char* name : {"equipbutton", "backbutton", "hipbutton", "legbutton", "grenadebutton", "meleebutton"}) {
            Widget* b = em_isa(find(w, name), WidgetClass::button);
            if (!b) unhosted(0x2c0b14, std::string("EQMODIFY without its GuiButtonWidget \"") + name + "\"");
            b->flags &= ~flag_enabled;
        }
        list_clear(e.list);
        e.list->flags &= ~flag_focusable;
        e.highlight->flags &= ~flag_focusable;
        e.highlight->flags &= ~flag_visible;
    }
    const auto tech1 = host_.get_string("level_techname1");
    if (Widget* t = text("levelname1"); t && tech1) set_text(t, *tech1, false);
    const auto tech2 = host_.get_string("level_techname2");
    if (Widget* t = text("levelname2"); t && tech2) set_text(t, *tech2, false);
    e.waiting = text("waitingforhost");
    const auto cd = host_.get_string("EQS_COUNTDOWN");
    e.countdown = cd ? *cd : std::string("Starting in %d Seconds...");  // 0048ba80
    if (g.b_21c8) unhosted(0x2c0e2c, "the network session *00506508 of EQMODIFY's \"waitingforhost\"");
    if (e.waiting) e.waiting->flags &= ~flag_visible;
    if (Widget* p = text("previousmenu")) put(p, host_.get_string(g.b_21c8 ? "ExitScreen" : "ExitMission"), 0x2c0edc);
}

Widget* GuiRuntime::em_button(Widget* w, std::int32_t cat) {  // 002bcb50 / 002bd560's +84 lookup
    static constexpr const char* names[7] = {"resetbutton", "backbutton", "hipbutton", "legbutton",
                                             "grenadebutton", "meleebutton", "equipbutton"};
    if (cat < 0 || cat > 6) return nullptr;
    return em_isa(find(w, names[cat]), WidgetClass::button);
}

void GuiRuntime::em_focus_list(Widget* w, Widget* button) {
    auto& e = *w->em;
    e.list_8c = 1;
    set_focus(e.list);  // 00414b30(004147a0(), +94)
    e.highlight->flags |= flag_visible;
    e.highlight->flags |= flag_awake;
    if (!button) return;
    send(w, button, 6, static_cast<std::int32_t>(em_on_pressed), 0);
    send(button, &canvas_, 0x10, 0, 0);
}

std::uint8_t GuiRuntime::em_input(Widget* w, const PadEvent& ev) {  // 002bcb50
    auto& e = *w->em;
    auto pressed = [&](std::uint32_t m) { return ev.kind == 1 && ev.port == 0 && ev.mask == m && ev.released == 0; };
    if (pressed(0x80)) {  // Square: accept
        auto& p = e.preview;
        if (p[0] != em_empty && e.fc != 0) {  // the hand's weapon goes back to its point, the off hand's to 4
            p[static_cast<std::size_t>(e.fc)] = p[0];
            p[0] = em_empty;
            if (p[1] != em_empty) {
                p[4] = p[1];
                p[1] = em_empty;
            }
        }
        host_.eqm_accept(p, e.fc);  // 00143b60 .. 00142c50 on the player's inventory (+48 +3524)
        if (g.b_21c8) unhosted(0x418a80, "the network game's GuiEquipmentSetup \"eqsetup\" (002bcd48)");
        host_.eqm_start();           // byte 004ffc29 = 0; 0036a0f0(0055a300)
        switch_screens(w, nullptr);  // 00414ac0(004147a0(), w, 0)
        host_.eqm_resume();          // 0012eee0(G, 0, 0)
        return 1;
    }
    if (pressed(pad_triangle)) {  // Triangle: quit
        if (g.b_21c8) unhosted(0x418a80, "the network game's GuiEquipmentSetup \"eqsetup\" (002bce20)");
        const auto title = host_.get_string("QuitTitle");
        const auto message = host_.get_string("QuitMessage");
        if (title && message) popup_open(5, title, message, w, 0);  // 002cae30(app, 5, ...)
        return 1;
    }
    if (pressed(0x8000)) {  // Left: back to the category's button
        Widget* b = em_button(w, e.cat_84);
        if (!b || b == canvas_.focus) return 1;
        e.list_8c = 0;
        set_focus(b);
        e.highlight->flags &= ~flag_focusable;
        e.list->flags &= ~flag_focusable;
        send(b, &canvas_, 0x10, 0, 0);
        return 1;
    }
    if (e.cat_84 <= 0) return 0;
    if (pressed(0x2000)) {  // Right: into the list
        if (canvas_.focus == e.list) return 1;
        e.list_8c = 1;
        em_focus_list(w, em_button(w, e.cat_84));
        return 1;
    }
    return 0;
}

void GuiRuntime::em_place(Widget* w, std::int32_t model) {  // 002bd560's 0x13 attach-table edit
    auto& e = *w->em;
    auto& p = e.preview;
    const auto point = e.point_88;
    if (point != 0 && point != e.fc) {
        p[static_cast<std::size_t>(point)] = model;
    } else if (point != 0) {
        p[0] = model;  // the held weapon's point: the hand
    } else {
        if (p[0] != em_empty) p[static_cast<std::size_t>(e.fc)] = p[0];
        for (std::int32_t t = 1; t < 10; ++t) {
            if (t == 1 || t == 4 || t == 6 || t == 7) continue;
            if (p[static_cast<std::size_t>(t)] == model) e.fc = t;  // compares the 0 handles too
        }
        p[0] = model;
        if (model != em_empty) p[static_cast<std::size_t>(e.fc)] = em_empty;
    }
    if (host_.eqm_dual_wield()) {  // 00332f90(001314f0(G, -1), 1)
        p[1] = e.fc == 3 ? p[0] : em_empty;
        p[4] = p[3];
    }
    // 0038c5e0(+4c + 60): the preview model's attachment refresh is drawn by 002c2340 (not ported).
}

std::uint8_t GuiRuntime::em_event(Widget* w, Widget* sender, std::uint32_t type, std::int32_t a3,
                                  std::int32_t t0) {  // 002bd560
    auto& e = *w->em;
    const std::uint32_t t = type & 0xffff;
    if (t == 0x13 && e.list_8c) {  // the list's Cross on row t0
        e.sel_108 = t0;
        e.sel_90c[static_cast<std::size_t>(e.point_88)] = static_cast<std::int16_t>(t0);
        std::int32_t model = em_empty;
        if (e.sel_108 < static_cast<std::int32_t>(e.items.size())) {
            const std::int32_t item = e.items[static_cast<std::size_t>(e.sel_108)];
            const auto r = host_.weapon_record(item);
            if (!r) unhosted(0x2bd600, "a null weapon record of a listed item");
            if (r->model_13c != 0) model = item;  // record +13c: 0 (not loaded) stores an empty point
        }
        em_place(w, model);
        em_column1(w);
        send(sender, &canvas_, 0x10, 1, 0);
    }
    if (t == 0x14 && e.list_8c) {  // the list's move to row t0
        e.sel_108 = t0;
        e.sel_90c[static_cast<std::size_t>(e.point_88)] = static_cast<std::int16_t>(t0);
        em_preload_window(w, 3);
        em_column2(w);
    }
    if (t == 0x10 && a3 == 1 && iequal(sender->name, "resetbutton")) return 1;  // 00128e80
    if (t == 2) {
        if (e.cat_84 > 0) {
            em_focus_list(w, sender);
        } else if (sender->name == "resetbutton") {  // 00129218
            e.preview = e.player;
            e.fc = e.fc_100;
            em_column1(w);
        }
    }
    if (t == 0) {  // a button took the focus (00129218 on its name)
        static constexpr std::pair<const char*, std::array<std::int32_t, 2>> cats[] = {
            {"backbutton", {1, 2}}, {"hipbutton", {2, 3}}, {"legbutton", {3, 5}},
            {"grenadebutton", {4, 8}}, {"meleebutton", {5, 9}}, {"equipbutton", {6, 0}}};
        bool hit = false;
        for (const auto& [name, cp] : cats) {
            if (sender->name != name) continue;
            hit = true;
            e.cat_84 = cp[0];
            e.point_88 = cp[1];
            e.sel_108 = e.sel_90c[static_cast<std::size_t>(e.point_88)];
            e.a0 = cvt_w(sender->rect[1], 0x2bd9ac);
            if (e.cat_84 == 6) em_build_equip(w);  // 002bf370
            else em_build_category(w);             // 002bf090
            break;
        }
        if (!hit && sender->name == "resetbutton") {
            em_clear(w);  // 002bdf90
            e.cat_84 = 0;
            if (e.list) list_clear(e.list);
        }
    }
    if (t == 0x1a && a3 == 0) {  // the quit popup's yes
        switch_screens(w, nullptr);
        host_.app_12fa60(3);
        return 1;
    }
    return base_event(w, sender, type, a3, t0);  // 0041ef10
}

void GuiRuntime::em_build_category(Widget* w) {  // 002bf090
    auto& e = *w->em;
    e.items.clear();
    auto class_of = [&](std::int32_t item, std::uint32_t at) {
        const auto r = host_.weapon_record(item);  // 00147840
        if (!r) unhosted(at, "a null weapon record");
        return static_cast<std::int32_t>(r->class_30);
    };
    if (host_.eqm_byte_51ee38()) {
        // 0x002bf230: the byte 0051ee38 set (no code of the original sets it; the port's --loadout-all-records):
        // every record 0 .. count - 1 whose class +30 is the category's point, in record order; neither the
        // agent's list (00332ac0) nor the screen's own +d8 list is read.
        const std::int32_t count = host_.weapon_record_count();
        for (std::int32_t item = 0; item < count; ++item)
            if (class_of(item, 0x2bf260) == e.point_88) e.items.push_back(item);
    } else {
        for (const std::int32_t item : host_.eqm_unlocked_items())  // 00332ac0
            if (class_of(item, 0x2bf124) == e.point_88) e.items.push_back(item);
        for (const std::int32_t item : e.initial) {
            if (class_of(item, 0x2bf18c) != e.point_88) continue;
            if (std::find(e.items.begin(), e.items.end(), item) == e.items.end()) e.items.push_back(item);
        }
    }
    host_.eqm_listed(e.items);  // PORT-ONLY observation (132wp1)
    em_preload_window(w, 1);
    if (e.row_a4 == 0) unhosted(0x2bf2c0, "div by a zero row height (+a4)");
    const std::int32_t n = static_cast<std::int32_t>(e.items.size());
    const std::int32_t d = e.y_9c - e.a0;
    const std::int32_t q = d / e.row_a4;
    const std::int32_t r = d - q * e.row_a4;
    std::int32_t a2 = std::max(0, q - (n >> 1));
    a2 = std::min(a2, std::max(0, e.rows_a8 - n));
    set_pos(e.list, static_cast<float>(e.x_98), static_cast<float>(e.y_9c - (r - 1) - a2 * e.row_a4));
    em_list(w);
    em_column1(w);
    em_column2(w);
}

void GuiRuntime::em_build_equip(Widget* w) {  // 002bf370
    auto& e = *w->em;
    e.items.clear();
    for (std::int32_t k = 0; k < 10; ++k) {
        if (k == 1 || k == 4 || k == 6 || k == 7) continue;
        if (e.preview[static_cast<std::size_t>(k)] != em_empty) e.items.push_back(e.preview[static_cast<std::size_t>(k)]);
    }
    const std::int32_t n = static_cast<std::int32_t>(e.items.size());
    if (e.sel_108 >= n) e.sel_108 = 0;
    if (e.row_a4 == 0) unhosted(0x2bf434, "div by a zero row height (+a4)");
    const std::int32_t d = e.y_9c - e.a0;
    const std::int32_t q = d / e.row_a4;
    const std::int32_t r = d - q * e.row_a4;
    const std::int32_t a2 = std::max(0, q - (n >> 1));
    set_pos(e.list, static_cast<float>(e.x_98), static_cast<float>(e.y_9c - (r - 1) - a2 * e.row_a4));
    em_list(w);
    em_column1(w);
    em_column2(w);
}

void GuiRuntime::em_list(Widget* w) {  // 002beee0
    auto& e = *w->em;
    const auto n = static_cast<std::uint32_t>(e.items.size());
    list_clear(e.list);
    array_set_dims(e.list, 1, n + 1);  // vt +5c
    array_set_visible(e.list, 1, static_cast<std::int32_t>(n) < e.rows_a8 ? n + 1 : static_cast<std::uint32_t>(e.rows_a8));
    array_select(e.list, 0, static_cast<std::uint32_t>(e.sel_108));
    e.list->list->wrap = 1;
    e.list->list->cursor = e.highlight;
    e.highlight->flags &= ~flag_visible;
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto r = host_.weapon_record(e.items[i]);
        if (!r) unhosted(0x2befb0, "a null weapon record of a listed item");
        const auto s = host_.get_string(r->name_80);  // 003fdb50(record + 80), else the key
        list_add(e.list, s ? *s : r->name_80, static_cast<std::int32_t>(i), 0);
    }
    list_add(e.list, host_.get_string("NoneOption"), static_cast<std::int32_t>(n), 0);
    array_select(e.list, 0, static_cast<std::uint32_t>(e.sel_108));
    e.highlight->flags &= ~flag_focusable;
    e.list->flags &= ~flag_focusable;
}

void GuiRuntime::em_preload_window(Widget* w, std::int32_t n) {  // 002bded0
    auto& e = *w->em;
    const std::int32_t count = static_cast<std::int32_t>(e.items.size());
    const std::int32_t s1 = e.sel_108 - (n >> 1);
    for (std::int32_t s0 = 0; s0 < n; ++s0) {
        std::int32_t a = s0 + s1;
        if (a < 0) a += count;
        if (a > count) a -= count;
        if (a >= 0 && a < count) em_ring_touch(w, e.items[static_cast<std::size_t>(a)]);
    }
}

void GuiRuntime::em_ring_touch(Widget* w, std::int32_t item) {  // 002bdcf0
    auto& ring = w->em->ring;
    const auto& p = w->em->preview;
    const auto it = std::find(ring.begin(), ring.end(), item);
    if (it != ring.end()) {  // most recently used last
        ring.erase(it);
        ring.push_back(item);
        return;
    }
    if (ring.size() >= 13) {
        // The first entry the preview does not attach is unloaded and dropped (none: the ring
        // stays full and the item is appended past it, as the original's +924 + 4 * 13 store).
        for (std::size_t i = 0; i < ring.size(); ++i) {
            if (std::find(p.begin(), p.end(), ring[i]) != p.end()) continue;
            host_.weapon_unload(ring[i]);  // 00145630(db, item, 1)
            ring.erase(ring.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
        if (ring.size() >= 13) unhosted(0x2bde98, "a full preload ring with every entry attached (the +958 store)");
    }
    ring.push_back(item);
    host_.weapon_preload(item);  // 00145710(db, item, 0)
}

void GuiRuntime::em_ring_release(Widget* w) {  // 002bdc10
    auto& e = *w->em;
    for (const std::int32_t item : e.ring)
        if (std::find(e.preview.begin(), e.preview.end(), item) == e.preview.end()) host_.weapon_unload(item);
    e.ring.clear();
}

void GuiRuntime::em_clear(Widget* w) {  // 002bdf90
    em_fill(w, '1', std::nullopt, 0x2bdf90);
    em_fill(w, '2', std::nullopt, 0x2bdf90);
}

void GuiRuntime::em_column1(Widget* w) {  // 002be910: the equipped weapon of the category
    auto& e = *w->em;
    std::optional<GuiHost::WeaponRecord> rec;
    if (e.cat_84 != 0) {
        std::int32_t m = e.preview[static_cast<std::size_t>(e.point_88)];
        if (m == em_empty && e.fc == e.point_88) m = e.preview[0];
        if (m != em_empty) {
            rec = host_.weapon_record(m);
            if (!rec) unhosted(0x2be95c, "a null weapon record of an attached model");
        }
    }
    em_fill(w, '1', rec, 0x2be910);
}

void GuiRuntime::em_column2(Widget* w) {  // 002be380: the list's selected weapon
    auto& e = *w->em;
    std::optional<GuiHost::WeaponRecord> rec;
    if (e.sel_108 < static_cast<std::int32_t>(e.items.size()) && e.list_8c) {
        rec = host_.weapon_record(e.items[static_cast<std::size_t>(e.sel_108)]);
        if (!rec) unhosted(0x2be3c4, "a null weapon record of a listed item");
    }
    em_fill(w, '2', rec, 0x2be380);
}

void GuiRuntime::em_fill(Widget* w, char column, const std::optional<GuiHost::WeaponRecord>& rec,
                         std::uint32_t at) {
    static constexpr const char* fields[5] = {"value_weaponname", "value_firerate", "value_damage", "value_clip_size",
                                              "value_max_rounds"};
    std::array<Widget*, 5> t{};
    for (std::size_t i = 0; i < 5; ++i) {
        Widget* f = find(w, std::string(fields[i]) + column);
        t[i] = f && is_text(f) ? f : nullptr;  // isA "GuiTextWidget"
    }
    if (!rec) {
        for (Widget* f : t)
            if (f) set_text(f, "", false);  // 004ac388 ""
        return;
    }
    const std::string none = "---";  // *0048c0a8
    auto number = [&](std::int32_t v) { return v != 0 ? std::to_string(v) : none; };  // 00128bd0 "%d"
    std::array<std::string, 5> s;
    const auto name = host_.get_string(rec->name_80);
    s[0] = name ? *name : rec->name_80;
    s[1] = none;
    if (rec->w60 != 0) roman_2c8ec0(ws_rating(rec->w60, {1, 3, 5, 8, 11}), s[1]);  // 002c9020 (0048c290)
    if (rec->byte_bc == 7 && rec->w64 == 0) {
        const auto nl = host_.get_string("DMG_NONLETH");
        if (!nl) unhosted(at, "sprintf %s of the missing string 'DMG_NONLETH'");
        s[2] = *nl;
    } else {
        s[2] = none;
        if (rec->w64 != 0) roman_2c8ec0(ws_rating(rec->w64, {10, 20, 35, 50, 60}), s[2]);  // 002c8fd0 (0048c2b0)
    }
    s[3] = number(rec->clip_70);
    s[4] = number(rec->clip_70 * host_.clips_for(rec->clips_74));  // 001861f0(+48, record +74)
    for (std::size_t i = 0; i < 5; ++i)
        if (t[i]) set_text(t[i], s[i], false);
}

}  // namespace omega::gui
