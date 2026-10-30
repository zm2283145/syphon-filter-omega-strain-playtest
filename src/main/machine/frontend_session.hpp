#pragma once
// Checkpoint 126: the front end from boot to the level load, as one host of
// the main update's front-end blocks (GameFlow, game_flow.hpp) and the native
// GUI runtime (gui_runtime.hpp). It models the GUI application object's calls
// the blocks make (research/FRONTEND.md "Checkpoint 126 front-end flow"):
//   002cc3e0 the first front-end entry (once: 0048c248), then 002cb3d0
//   002cb3d0(app, 0) the GUI start: the POPUP message box (004187c0), the
//            canvas keyboard path / repeat mask, then TITLESCR, or CMDCENTR
//            when app +35 (002c9510 instead when the GUI was suspended)
//   002c9510 the resume after an attract movie (app +37)
//   002c9860 the suspend before one (state 4)
//   002cb160 the GUI teardown of state 3
//   002cbb30 the GUI frame (00419270 flush, 00415870 pads, +60 update)
//   002c9af0 the front-end music request (a PLAY of THEME_MAIN.. to gameSound)
// and the profile / card services of the screens on the path (title,
// GuiAgentLoad, GuiOnlineOfflineSelect, GuiCommandCenter) over one agent
// record and a FolderAgentCard. Checkpoint 126c: with FrontendServices::sound
// the GUI application's gameSound calls are made (the FRONTEND banks' init /
// level load / unload under the 004ffc90 swap, the music, the GS_* sounds and
// the frame update) and the 00167c40 prologue's clock counts the 003ea1d0
// events down. Calls whose effect is outside this layer (video mode, loading
// screen, network) are recorded, not applied;
// anything the path cannot reach faithfully throws (runtime_error) naming
// its address. GameFlow's G fields that alias the GUI globals (G+50 =
// 004ffba0, G+51, G+140 = 004ffc90, 00533870) and the GUI object's bytes
// (+30..+35, +39, +13c, +24c) are copied between the two models around
// every call into the runtime.
#include "agent_card.hpp"
#include "agent_profile_view.hpp"
#include "agent_statistics.hpp"
#include "frontend_formats.hpp"
#include "game_flow.hpp"
#include "game_sound.hpp"
#include "gui_decor.hpp"
#include "gui_runtime.hpp"
#include "movie_player.hpp"
#include "movie_pss.hpp"
#include "movie_subtitles.hpp"
#include "pad_vibration.hpp"
#include "profile_awards.hpp"
#include "script_scheduler.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

class WeaponModelLibrary;

// The GUI's GS_* sound descriptors (static inits 004d8180 / 004d8458): the
// name of the descriptor at `table`, nullptr when it is not one of them.
const char* gui_sound_name(std::uint32_t table);
// 00465c50 (PLAY: 0020faa0, class *004f53b8) / 00465b50 (STOP: 0020ecb0,
// *004f53d8): the descriptor's message delivered to gameSound (003c9420
// transport 1) with +20 = 1.
void gui_sound_deliver(GameSoundManager& sound, const char* name, bool stop);
// GuiGameOptions' sounds (0029b070, 00299b90, 0029a190): a 001ca210 / 001ca010 descriptor (type, name)
// wrapped by 0020ecb0 (kind 0, STOP), 0020faa0 (1, PLAY) or 0020f2a0 (2, QUIP) and delivered (003c9420
// transport 1) with +20 = 1.
void gui_sound_send(GameSoundManager& sound, std::int32_t kind, std::int32_t type, const std::string& name);
// 002cb3d0's command-centre precondition. The native online path needs its hosted-session byte; the port's LAN
// command centre is hosted by LanNetCore instead and deliberately keeps that online byte clear.
bool frontend_command_center_hosted(std::uint8_t online, std::uint8_t online_hosted, std::uint8_t lan_mode);

// PORT-ONLY (checkpoint 132on2, research/ONLINE.md §9): the online lobby behind the GuiHost DME-session calls
// (net_session / net_start and the lobby_* calls GuiOnlineConnect makes; see gui_runtime.hpp). The port's
// implementation (src/port/app/online_lobby_host) talks to omega_server through the transport; without one
// (FrontendServices::lobby null) the session keeps the GuiHost defaults: no DME session, a throwing start.
class FrontendLobby {
public:
    virtual ~FrontendLobby() = default;
    virtual bool session() = 0;          // 004294c0
    virtual std::int32_t start() = 0;    // 002ca710
    virtual std::int32_t configs(std::vector<gui::LobbyConfig>& configs, std::vector<gui::LobbyServer>& servers) = 0;
    virtual void disconnect() = 0;       // 0044c3c0
    virtual std::int32_t error() = 0;    // 00452ff0
    virtual std::int32_t connect(std::int32_t row) = 0;  // 0044bab0
    virtual std::int32_t dnas_result() = 0;              // 00452fe0
    virtual std::int32_t universes_begin() = 0;          // 00442f40
    virtual std::int32_t muis_connect(std::int32_t server, std::vector<gui::LobbyUniverse>& universes) = 0;  // 00442b50
    virtual std::int32_t universe_select(std::int32_t index) = 0;  // 00442840
    virtual std::int32_t universe_default() = 0;                   // 00442970
    virtual void universes_end() = 0;                              // 00442e90
    virtual std::string setting(const std::string& key) = 0;       // 003cba90
    virtual std::int32_t policy_begin() = 0;                       // 00445e80
    virtual std::int32_t announcements(std::vector<gui::LobbyAnnouncement>& out) = 0;  // 00445570
    virtual std::int32_t policy(bool privacy, std::vector<std::string>& out) = 0;     // 004456d0 / 004456c0
    virtual void policy_end() = 0;                                 // 00445dc0
    virtual void leave(std::int32_t a1) = 0;                       // 002ca640's session tail
    virtual std::optional<std::string> notice() = 0;               // 0033cca0
    virtual bool unread() = 0;                                     // session +20
    virtual bool region_choice() = 0;                              // GuiHost::lobby_region_choice
    // Checkpoint 132on2b (GuiAccountLogin): 1 succeeds; error() names the failure.
    virtual std::int32_t login(const std::string& name, const std::string& password) = 0;          // 00445a40
    virtual std::int32_t register_account(const std::string& name, const std::string& password) = 0;  // 00445b90
    virtual std::int32_t check_account(const std::string& name, const std::string& password) = 0;  // 004459d0
    virtual std::int32_t enter() = 0;                                                              // 00442750
    // Checkpoint 132on2b (GuiCommandCenter online): the lobby state (+255c: 9 logged in, 4 in the main channel),
    // the mission channel 00442510 (1 succeeds) and the count of the lobby's records (+1c) 002c8090 hands the agent.
    virtual std::int32_t state() = 0;
    virtual std::int32_t mission(std::int32_t mission) = 0;
    virtual std::int32_t records() = 0;
    // Checkpoint 132on2b stages B3 / B4 (GuiMissionSetup / GuiNetJoinMission online; the GuiHost calls of the same
    // names). The lobby's update 004536b0 (pump) hands back a request's completion (the callback event 0x1e).
    virtual void channel_leave() = 0;                                                              // 004426b0
    virtual std::int32_t room_open() = 0;                                                          // 004518f0
    virtual void room_leave() = 0;                                                                 // 00451610
    virtual void list_kind(std::int32_t kind) = 0;                                                 // 004515f0
    virtual std::int32_t create(std::int32_t mission, std::int32_t agents, std::int32_t filter) = 0;  // 0044e960
    virtual std::optional<gui::GuiHost::LobbyCallback> pump() = 0;                                 // 004536b0
    // 004536b0 outside a screen's callback wait (the navigator's wait thread 002c84b0): the port's queued callbacks
    // stay for the loop that waits on them
    virtual void update() = 0;
    virtual void game_cleanup() = 0;                                                               // 0042b1f0
    virtual std::optional<gui::GuiHost::LobbyGameInfo> game_info(std::int32_t world) = 0;          // 0044e060
    // Checkpoint 132on2i: the game cache +118 (0044f0c0; 0044edc0's additions and changes for 0044eff0).
    virtual std::optional<gui::GuiHost::LobbyCachedGame> game_cached(std::int32_t) { return std::nullopt; }
    virtual std::vector<gui::GuiHost::LobbyCachedGame> game_cache_take() { return {}; }
    virtual std::int32_t join(std::int32_t world) = 0;                                             // 0044e430
    virtual void join_end() = 0;                                                                   // 0044e340
    virtual void set_state(std::int32_t state) = 0;                                                // +255c
    virtual std::int32_t own_id() = 0;                                                             // +2480
    virtual gui::GuiHost::LobbyGameList game_list() = 0;  // the room's last game list (00447790's rows)
    // Checkpoint 132on2c (GuiOnlineOptions / GuiNetMessages; the GuiHost calls of the same names): the universe
    // list 00442d80 (refilled; 1 succeeds) and the lobby's message list (session +1c).
    virtual std::int32_t universe_list(std::vector<gui::LobbyUniverse>& universes) = 0;
    virtual std::int32_t message_count() = 0;                                          // list +4
    virtual std::optional<gui::GuiHost::LobbyMessage> message(std::int32_t index) = 0;  // 00451010
    virtual void message_answer(std::int32_t index, std::int32_t accept) = 0;          // 00450f30
    virtual bool messages_changed() = 0;                                                // list +18
    virtual void messages_set_changed(bool changed) = 0;                                // 0044f280
    // Checkpoint 132on2c (GuiLobbyScreen's chat and player list; the GuiHost calls of the same names).
    virtual std::int32_t chat_send(const std::string& text, std::int32_t target) = 0;  // 00451050
    virtual std::vector<gui::GuiHost::LobbyChatLine> chat_take() = 0;                  // 00447bb0 / 00447c70
    virtual gui::GuiHost::LobbyPlayerList player_list() = 0;                           // 00447a30
    virtual std::optional<std::int32_t> player_rank(std::int32_t account) = 0;          // 00450cf0 (132on2d)
    virtual void agent_edited() {}  // 0033fc80's 002cbd40 / 004505e0 (132on2e): the record uploaded after an agent edit
    // Checkpoint 132on2i (GuiAgentInfo mode 1 / the agency cell; the GuiHost calls lobby_agent_stats, lobby_cell_*,
    // lobby_is_contact). The defaults answer as a lobby with no cells.
    virtual bool agent_stats(std::int32_t, std::array<std::uint8_t, 0xfc>&) { return false; }  // 00450c60
    virtual bool cell_leader() { return false; }                                               // 0044a1d0
    virtual bool cell_member() { return false; }                                               // 0044a230
    virtual std::string cell_name() { return std::string(); }                                  // 0044a070
    virtual std::optional<std::string> cell_name_of(std::int32_t) { return std::nullopt; }     // 00448ff0
    virtual bool cell_has(std::int32_t) { return false; }                                      // 0044a0d0
    virtual bool is_contact(std::int32_t) { return false; }                                    // 0044d670
    virtual std::int32_t cell_invite(std::int32_t) { return 0; }                               // 0044ac10
    virtual std::int32_t cell_create(const std::string&) { return 0; }                         // 0044af60
    // Checkpoint 132on2h (GuiNetContacts; the GuiHost calls of the same names).
    virtual std::int32_t contacts_open() = 0;                                  // 0044df80
    virtual void contacts_close() = 0;                                         // 0044dd20
    virtual gui::GuiHost::LobbyContactList contact_list() = 0;                 // 00446830
    virtual std::int32_t contact_remove(std::int32_t id) = 0;                  // 0044d9c0
    virtual std::int32_t account_id(const std::string& name) = 0;              // 00451370
    virtual std::int32_t contact_add(std::int32_t id) = 0;                     // 0044db60
    virtual bool join_password_refused() = 0;               // PORT-ONLY: JoinGameResponse -978
    virtual void set_join_password(const std::string& password) = 0;  // PORT-ONLY: JoinGame +0x30
};

// What the session reads from the disc and the PC (supplied by the window).
struct FrontendServices {
    std::function<std::optional<std::string>(const std::string& key)> strings;   // 003fdb50
    std::function<std::uint32_t(const std::string& name)> font;                  // 00418420
    std::uint32_t default_font = 12;                                             // manager +10
    std::function<std::uint8_t(std::uint32_t handle)> font_height;               // 0036a2f0
    // 0036aa50(font, s, n) (checkpoint 132on2: GuiOnlineConnect's policy page wraps its lines with it).
    std::function<std::int32_t(std::uint32_t handle, const std::string& s, std::int32_t n)> font_width;
    // FRONTEND/NTSC.HOG member names (the resources 004133b0 finds) and the
    // <SCREEN>.GUI of <SCREEN>.HOG.
    std::vector<std::string> gui_resources;
    std::function<std::vector<std::uint8_t>(const std::string& screen)> gui_file;
    GuiDecor::Resolver decor;
    const ProfileAwards* awards = nullptr;
    std::optional<std::vector<std::uint8_t>> charmap;   // COMMON/SFO_CHAR.MAP (nullopt: no file)
    std::filesystem::path card_root;                    // FolderAgentCard root
    // A ZMOVIES.HOG movie by member name (case-insensitive); nullptr when the
    // HOG has no such member.
    std::function<std::unique_ptr<movie::MovieStream>(const std::string& name)> open_movie;
    std::vector<std::string> movie_members;             // ZMOVIES.HOG member names
    // Checkpoint 132cp11: a ZMOVIES.HOG member's bytes by name (case-insensitive;
    // nullopt when absent): 00412600 loads the movie's .SUB subtitles.
    std::function<std::optional<std::vector<std::uint8_t>>(const std::string& name)> movie_member;
    // Checkpoint 126c: gameSound (003caa50; nullptr: its calls are recorded,
    // not made) and the 004ffc90 its host reads, which the GUI application
    // swaps to "FRONTEND" around the init / level load / unload.
    GameSoundManager* sound = nullptr;
    std::function<void(const std::string& level)> sound_level;
    // Checkpoint 132zf: FRONTEND/LANG/ENGLISH/ZEUS.HOG, the ZEUS files'
    // pictures (*(*004ffc04 + 25c)); unset: no archive mounted. A member's
    // index without case (0036d2a0, -1: none), a member's texture slot and
    // size (0043c730: 00380a40, shared per stem, counted) and its release
    // (003813a0).
    std::function<std::int32_t(const std::string& name)> zeus_lookup;
    std::function<gui::GuiHost::BitmapTexture(const std::string& name)> zeus_load;
    std::function<void(std::int32_t slot)> zeus_release;
    // Checkpoint 132cc4: 002cbad0 inside a card wait's poll (00441c10): draw the canvas and present
    // (0037a9f0(005381f0)) now, within the GUI event that started the card call (nullptr: not drawn).
    std::function<void()> nested_frame;
    // Checkpoint 132cc4b: GuiAgentModify's services (unset: the calls throw naming their address).
    // The part cache 0055dda0 (frontCharacter 0022a220), archive 0's pictures (0036e4b0: the front
    // end's MANTEX.HOG; released by zeus_release, one slot table), the string table's index
    // lookup / text (003fe650 / 003fdb80) and the PORT-ONLY model preview (width 0 releases it).
    std::function<AgentPartCache*()> agent_parts;
    std::function<gui::GuiHost::BitmapTexture(const std::string& name)> file_load;
    std::function<std::int32_t(const std::string& key)> loc_find;
    std::function<std::optional<std::string>(std::int32_t id)> loc_text;
    // Checkpoint 132cc5: GuiAgentCreate's Skl_Load (00402840) reads of GAMEDATA\FRONTEND\<name>.SKL.
    std::function<std::vector<std::uint8_t>(const std::string& path)> frontend_file;
    // Checkpoint 132f13: the weapon database *004ffd30 (WEAPONS.WDB, the game init's) with the front
    // end's db+1c archive COMMON\WPNHOG.HOG (00145160 from 002c9750 / 002cb7c0) as the model library
    // the personnel pages' GuiWeaponDisplay reads (00147840, 00145710). Unset: the calls throw.
    std::function<WeaponModelLibrary*()> weapons;
    // Port-only (checkpoint 132ob6, not the original): the rows a port host adds to the title screen's
    // GAMEOPT (port_options_row.hpp, the "CPU Partner" setting). patch edits a screen's decoded GUI file
    // before its first load (screen: the HOG name without ".HOG"); wake / pressed are GuiHost's
    // port_options_wake / port_options_pressed. Unset: the disc's screens, nothing added.
    struct PortOptionRows {
        std::function<void(const std::string& screen, GuiFile& file)> patch;
        std::function<void(gui::GuiRuntime& rt, gui::Widget* screen)> wake;
        std::function<bool(gui::GuiRuntime& rt, gui::Widget* screen, gui::Widget* sender)> pressed;
        // Port-only (checkpoint 132win11): the Port Settings page (port_settings_page.hpp) the PORT SETTINGS row
        // opens; the session makes its screen PORTSETTINGS from GAMEOPT2 (port_cheats.hpp port_settings_screen).
        gui::PortSettingsScreen* settings = nullptr;
    };
    PortOptionRows port_rows;
    // Port-only (checkpoint 132ln1, research/LAN_PLAY.md): GuiHost::port_lan_play (LAN Play on ONOFFSEL).
    bool lan_play = false;
    // Port-only (checkpoint 132ln3, research/LAN_PLAY.md §4 row 6b): the LAN session behind GuiHost::lan_host /
    // lan_hosting (MSNSETUP's begin_mission in LAN mode). Unset: hosting throws, lan_hosting answers false.
    std::function<bool(const gui::GuiHost::LanHostRequest& request)> lan_host;
    std::function<bool()> lan_hosting;
    // Port-only (checkpoint 132ln4, §4 rows 7 / 7b): the LAN session behind GuiNetJoinMission's LAN mode (the
    // GuiHost::lan_browse / lan_games / lan_join / lan_join_state / lan_join_cancel / lan_pump / lan_clock_ms
    // services). Unset: nothing is listed and a join throws.
    std::function<void(bool on)> lan_browse;
    std::function<std::vector<gui::GuiHost::LanGame>()> lan_games;
    std::function<bool(std::uint64_t session)> lan_join;
    std::function<gui::GuiHost::LanJoin()> lan_join_state;
    std::function<void()> lan_join_cancel;
    std::function<std::optional<std::string>()> lan_join_notice;  // 132md5: GuiHost::lan_join_notice
    std::function<void()> lan_pump;
    std::function<std::int64_t()> lan_clock_ms;
    // Port-only (checkpoint 132on2, research/ONLINE.md §9): the online lobby (omega_server). Null: no session.
    std::shared_ptr<FrontendLobby> lobby;
};

class FrontendSession final : public gui::GuiHost, public GameFlowHost {
public:
    explicit FrontendSession(FrontendServices services);
    FrontendSession(const FrontendSession&) = delete;
    FrontendSession& operator=(const FrontendSession&) = delete;
    ~FrontendSession() override;

    // One 60 Hz main-update frame (run_game_flow_frame) with the pad word
    // 0x55a300 + 14c (the 0055a44c held mask).
    void frame(std::uint16_t held);
    // Checkpoint 132win6: pad 0's libpad state as 00369c50 stores it (0055a4ec, read by the controller check
    // 00465870): 6 ready, 0 no controller (pad_connection.hpp). While it is not 6 the pad driver does not read, so
    // frame() keeps the last held and previous masks (+10c / +118) instead of taking held.
    void set_pad_state(std::int32_t state);
    std::int32_t pad_state() const;

    std::uint8_t state() const { return g_.state_50; }
    // State 0xe: the level load (00168438) of level().
    bool handoff() const { return g_.state_50 == 0xe; }
    const std::string& level() const { return g_.level_140; }
    std::int64_t frames() const { return frames_; }
    std::int32_t now_ms() const { return ms_; }
    // 003ea1d0: an event on the callback list 0055a2e0, which the 00167c40
    // prologue's 003e9e30 counts down in ms (gameSound's 2000 / 4000 ms events).
    void schedule(std::int32_t delay, std::function<void()> event);

    gui::GuiRuntime& runtime() { return rt_; }
    GuiDecor& decor() { return decor_; }
    // App +28: the GUI runs (002cbb30 / 002cbad0 update and draw it).
    bool gui_active() const { return app_28_ != 0; }
    // 002cbad0 ran this frame (state 2 draws the canvas).
    bool gui_drawn() const { return drawn_; }
    const std::string& loading_screen() const { return loading_name_; }
    // The movie playing (G+b0) and its last decoded picture.
    bool movie_playing() const { return g_.player_b0 != nullptr; }
    const std::string& movie_name() const { return movie_name_; }
    const mpeg2::Frame* movie_frame() const { return frame_valid_ ? &frame_ : nullptr; }
    std::uint64_t movie_serial() const { return serial_; }
    // Port-only (checkpoint 132md14): the playing movie's picture aspect when a mod's replacement is not 4:3 (0: 4:3).
    float movie_aspect() const { return movie_ ? movie_->picture_aspect() : 0.0F; }
    // Checkpoint 132cp11: the subtitle object 0x571cf0's cue model (nullptr when
    // the movie has no .SUB or its feed) and its Color (+80 once a cue loads).
    const movie::SubtitleTrack* movie_subtitles() const { return subtitle_track_.get(); }
    const std::array<float, 4>& movie_subtitle_colour() const { return subtitle_colour_; }
    // The movie's PCM since the last call (interleaved) and its format.
    std::vector<std::int16_t> take_movie_audio();
    int movie_rate() const { return audio_rate_; }
    int movie_channels() const { return audio_channels_; }
    // New "recorded, not applied" calls since the last call (each text once).
    std::vector<std::string> take_log();

    // Checkpoint 132cp3: the pad vibration manager (004feed0; pad_vibration.hpp) that GuiGameOptions drives
    // on the title screen's GAMEOPT (0029b070 stop, 0029b920 update, 0029a360 start, 0045a3a0 enable); the
    // window forwards vibration().last[0] to the controller.
    const PadVibration& vibration() const { return vibration_; }

    const AgentLoadState& agent_state() const { return agent_state_; }
    std::span<const std::uint8_t, agent_record_size> agent() const { return agent_; }
    FolderAgentCard& card() { return card_; }

    // PORT-ONLY unlock helpers (port_cheats.hpp), used by --unlock-all and tests. The visible cheat controls live
    // only in the in-game pause menu. Each helper applies its unlocks to the agent now and stays on for the session:
    // the refill
    // 00335e50 (load, create, level end) clears the unlock bits, so
    // refill_host re-applies them after the script.
    void port_unlock_all();
    void port_unlock_all_weapons();
    // --unlock-all: on from boot, applied by the refill of the agent the front end loads or creates
    // (the constructed agent before that has no award lists to unlock).
    void port_unlock_all_at_boot();
    bool port_unlock_all_on() const { return port_unlock_all_; }
    bool port_unlock_weapons_on() const { return port_unlock_weapons_; }

    // The play that this session handed off to (state 0xe) ran the level on
    // this agent record: 001314f0(G, -1) -> 00335830(agent, completed) from
    // 0012fa60 (game_end_state.hpp GameEndState::end_agent), with the HUD's
    // mission clock 00245890 and the MANTEX.HOG member names (0036d200) of
    // the parts tail 00405a60.
    void end_agent_level(std::uint32_t completed, const std::function<float()>& mission_time,
                         std::span<const std::string> mantex_members);
    // 00335d50(agent, 1), the level load's agent start (level_load.cpp): the level statistics cleared, the
    // refill 00335e50(agent, 1, 1) (mark 1: the awards held before the level read 2), the objective words
    // +a4 cleared, the snapshots +b2c (unlock words) / +b54 (rank) the debrief's awards compare, +d4 = -1.
    void start_agent_level();
    // The agent record's statistics block (+8..+c3: statistics 0..0x1e, the objective words +84 / +a4) that the
    // original's play adds to in place (00333f10, the script natives AddObjective) and the port's play keeps in
    // its own session (combat_play's AgentStatistics, the mission scripts' AgentObjectiveBits).
    AgentStatistics agent_statistics() const;
    // Back from play before 00335830: the statistics +8..+80 as play left them; the objective words OR-ed in.
    void sync_play_statistics(const AgentStatistics& play);
    // 001690f0 after the level's teardown: G+50 / G+51 as level_end_teardown
    // left them (0x0c or 1, 0x14); the main update's front-end blocks run
    // again from the next frame (state 0x0c: the EOL movie, then state 1 and
    // 002cb3d0, which opens CMDCENTR while app +35 is set).
    void resume_after_level(std::uint8_t state_50, std::uint8_t previous_51);

    // Port-only (checkpoint 132f5, the pause menu's cheats). The agent patch
    // runs after each AWARDS script call of the award refill 00335e50 (the
    // load's, the level end's and port_refill_agent's), so the unlocks it sets
    // stand through the refill's clear of +b08..+b2b / +b5c. port_refill_agent
    // runs that refill (00335e50(agent, 0, 1), as the level end does) now.
    using PortAgentPatch =
        std::function<void(std::span<std::uint8_t, agent_record_size>, AgentLoadState&, const ProfileAwards&)>;
    void set_port_agent_patch(PortAgentPatch patch) { port_agent_patch_ = std::move(patch); }
    void port_refill_agent();

    // GuiHost.
    std::optional<std::string> get_string(const std::string& key) override;
    std::uint32_t font(const std::string& name) override;
    std::int32_t ie_lookup(const std::string&) override { return -1; }
    std::int32_t demo_mode() override { return demo_count_1302f0(); }
    std::int32_t time_ms() override { return ms_; }
    std::uint8_t save_present() override;
    void card_refresh() override;
    std::int32_t card_space_needed(std::int32_t size) override;
    std::int32_t profile_size(std::uint32_t) override { return agent_profile_size; }
    std::int32_t current_profile() override { return profile_handle; }
    void profile_select(std::uint32_t profile) override;
    void app_2c9af0(std::int32_t value) override;
    void app_2c9aa0() override;  // checkpoint 132on2: the music stop (GuiOnlineConnect's update)
    void app_131470(std::int32_t value) override;
    void app_2c82f0() override;
    void app_2c81d0() override;
    void card_sync() override;  // 0040d5d0 (checkpoint 132ln2: GuiOnlineConnect's wake)
    // Checkpoint 132ln3: GuiMissionSetup's begin_mission services and its PORT-ONLY LAN session.
    void profiles_clear_b70() override;                                                         // 0012ee30
    void level_progress(std::int32_t profile, std::int32_t level, std::int32_t a, std::int32_t b) override;  // 00332a10
    bool lan_hosting() override;
    bool lan_host(const LanHostRequest& request) override;
    void lan_browse(bool on) override;                     // port-only (132ln4)
    std::vector<LanGame> lan_games() override;
    bool lan_join(std::uint64_t session) override;
    LanJoin lan_join_state() override;
    void lan_join_cancel() override;
    std::optional<std::string> lan_join_notice() override;  // port-only (132md5)
    void lan_pump() override;
    std::int64_t lan_clock_ms() override;
    // Port-only (checkpoint 132on2): the DME session and the lobby calls through FrontendServices::lobby.
    bool net_session() override;
    std::int32_t net_start() override;
    std::int32_t lobby_configs(std::vector<LobbyConfig>& configs, std::vector<LobbyServer>& servers) override;
    void lobby_disconnect() override;
    std::int32_t lobby_error() override;
    std::int32_t lobby_connect(std::int32_t row) override;
    std::int32_t lobby_dnas_result() override;
    std::int32_t lobby_universes_begin() override;
    std::int32_t lobby_muis_connect(std::int32_t server, std::vector<LobbyUniverse>& universes) override;
    std::int32_t lobby_universe_select(std::int32_t index) override;
    std::int32_t lobby_universe_default() override;
    void lobby_universes_end() override;
    std::string setting(const std::string& key) override;
    std::int32_t lobby_policy_begin() override;
    std::int32_t lobby_announcements(std::vector<LobbyAnnouncement>& out) override;
    std::int32_t lobby_policy(bool privacy, std::vector<std::string>& out) override;
    void lobby_policy_end() override;
    void lobby_leave(std::int32_t a1) override;
    std::optional<std::string> lobby_notice() override;
    bool lobby_unread() override;
    bool lobby_region_choice() override;
    std::int32_t lobby_universe_list(std::vector<LobbyUniverse>& universes) override;  // checkpoint 132on2c
    std::int32_t lobby_message_count() override;
    std::optional<LobbyMessage> lobby_message(std::int32_t index) override;
    void lobby_message_answer(std::int32_t index, std::int32_t accept) override;
    bool lobby_messages_changed() override;
    void lobby_messages_set_changed(bool changed) override;
    std::int32_t lobby_login(const std::string& name, const std::string& password) override;
    std::int32_t lobby_register(const std::string& name, const std::string& password) override;
    std::int32_t lobby_check_account(const std::string& name, const std::string& password) override;
    std::int32_t lobby_enter() override;
    std::int32_t lobby_state() override;
    std::int32_t lobby_mission(std::int32_t mission) override;
    std::int32_t lobby_messages() override;
    void lobby_channel_leave() override;  // checkpoint 132on2b (B3 / B4)
    std::int32_t lobby_room_open() override;
    void lobby_room_leave() override;
    void lobby_list_kind(std::int32_t kind) override;
    void lobby_net_start() override;
    std::int32_t lobby_create(std::int32_t mission, std::int32_t agents, std::int32_t filter) override;
    std::optional<LobbyCallback> lobby_pump() override;
    void lobby_game_cleanup() override;
    std::optional<LobbyGameInfo> lobby_game_info(std::int32_t world) override;
    std::optional<LobbyCachedGame> lobby_game_cached(std::int32_t world) override;
    std::vector<LobbyCachedGame> lobby_game_cache_take() override;
    std::int32_t lobby_join(std::int32_t world) override;
    void lobby_join_end() override;
    void lobby_set_state(std::int32_t state) override;
    void net_update() override;
    std::int64_t lobby_clock_ms() override;
    std::int32_t lobby_own_id() override;
    LobbyGameList lobby_game_list() override;
    std::int32_t lobby_chat_send(const std::string& text, std::int32_t target) override;
    std::vector<LobbyChatLine> lobby_chat_take() override;
    LobbyPlayerList lobby_player_list() override;
    std::int32_t lobby_contacts_open() override;
    void lobby_contacts_close() override;
    LobbyContactList lobby_contact_list() override;
    std::int32_t lobby_contact_remove(std::int32_t id) override;
    std::int32_t lobby_account_id(const std::string& name) override;
    std::int32_t lobby_contact_add(std::int32_t id) override;
    std::optional<std::int32_t> lobby_player_rank(std::int32_t account) override;
    void lobby_agent_edited() override;
    bool lobby_agent_stats(std::int32_t id, std::array<std::uint8_t, 0xfc>& out) override;
    bool lobby_cell_leader() override;
    bool lobby_cell_member() override;
    std::string lobby_cell_name() override;
    std::optional<std::string> lobby_cell_name_of(std::int32_t clan) override;
    bool lobby_cell_has(std::int32_t id) override;
    bool lobby_is_contact(std::int32_t id) override;
    std::int32_t lobby_cell_invite(std::int32_t id) override;
    std::int32_t lobby_cell_create(const std::string& name) override;
    bool lobby_join_password_refused() override;
    void lobby_set_join_password(const std::string& password) override;
    // App +310: the "please wait" thread (002c82f0 / 002c81d0) runs. Wait-thread frames drawn so far.
    bool wait_thread() const { return app_310_ != 0; }
    std::int64_t wait_frames() const { return wait_frames_; }
    // Checkpoint 132ln4: which 002cc260 the current nested GUI frame (services.nested_frame) belongs to: the card
    // wait's poll (00441c10 -> 002cc260(0, 0)) or the wait thread's first pass (002c84b0 -> 002cc260(0, 0x3039)).
    bool nested_in_wait_thread() const { return nested_wait_thread_ != 0; }
    // Checkpoint 132ln4: the multiplayer byte 005721c8 (the state-3 multiplayer path sets it) and the host role
    // 005721c0 (Net_StartSession's create; in LAN mode MSNSETUP's LAN host) at the hand-off.
    bool multiplayer() const { return g_.mp_5721c8 != 0; }
    bool host_role() const { return rt_.g.b_21c0 != 0; }
    std::int32_t popup_open(std::int32_t, const std::optional<std::string>&, const std::optional<std::string>&,
                            const gui::Widget*, std::int32_t) override;
    void popup_close(std::int32_t) override;
    std::int32_t popup_query(std::int32_t) override;
    std::uint8_t sound(std::uint32_t table, const gui::Widget*, bool alternate) override;
    void pad_reset() override { record("0036a240 pad reset"); }
    void rumble_stop() override;
    void rumble_start(std::int32_t port, std::int32_t small, std::int32_t large, std::int32_t period, float duration) override;
    void rumble_update(float dt) override;
    void rumble_enable(std::uint8_t enabled) override;
    bool options_player() override { return false; }   // 0045a3c0: *004ffc0c, no player in the front end
    bool sound_manager() override { return services_.sound != nullptr; }   // 003caa50
    void sound_volume(std::int32_t channel, std::int32_t volume) override;
    void sound_mode(std::int32_t mode) override;
    void options_sound(std::int32_t kind, std::int32_t type, const std::string& name) override;
    void text_object(const gui::TextObject&) override {}
    void set_text(const std::string&) override {}
    gui::Widget* factory(const std::string& cls, const std::string& file) override;
    void release(gui::Widget*) override;
    std::optional<std::int32_t> resource(const std::string& name) override;
    const GuiNode* open_gui(const std::string& file, std::int32_t res) override;
    // 003d1660: the queued key reports, one per poll (none on a PC with no queue: the pad drives GuiKeyboard).
    std::optional<UsbKeyReport> usbkb_poll() override;
    // PORT-ONLY test (checkpoint 132on2b): the front-end script's "keys TEXT" types into the USB keyboard path.
    void usbkb_queue(const UsbKeyReport& k) { usb_keys_.push_back(k); }
    std::uint32_t default_font() override { return services_.default_font; }
    std::uint8_t font_height(std::uint32_t handle) override;
    std::int32_t font_width(std::uint32_t handle, const std::string& s, std::int32_t n) override;
    std::uint8_t agent_load(std::int32_t agent, const std::string& name, std::int32_t slot) override;
    void agent_activate(std::int32_t agent) override;
    std::optional<std::string> profile_name(std::int32_t profile) override;
    std::optional<std::string> profile_rank(std::int32_t profile) override;
    std::int32_t profile_clp(std::int32_t profile) override;
    std::int32_t level_done(std::int32_t profile, const std::string& key) override;
    std::int32_t level_flag(std::int32_t profile, std::int32_t level, std::int32_t k) override;
    std::vector<std::array<std::int32_t, 8>> profile_level_records(std::int32_t profile) override;
    std::optional<std::string> profile_6060(std::int32_t profile, const std::string& key) override;
    std::int32_t profile_3b10(std::int32_t profile, const std::string& key) override;
    std::vector<BonusRow> profile_vec(std::int32_t profile, std::int32_t kind) override;
    std::vector<BonusRow> profile_ratings(std::int32_t profile) override;   // 00333600
    RankRow profile_rank_row(std::int32_t profile) override;                 // *(agent + b50)
    std::int32_t profile_statistic(std::int32_t profile, std::int32_t index) override;  // 00333ee0
    std::optional<AgentPacking> profile_appearance(std::int32_t profile) override;      // *(agent + 9a4)
    bool quip_playing(const std::string& name) override;                     // 001c95d0
    std::vector<BonusItem> profile_items(std::int32_t profile, std::int32_t kind, std::int32_t index) override;
    std::vector<BonusItem> profile_level_objectives(std::int32_t profile, const std::string& short_name) override;
    std::vector<RankEntry> profile_ranks() override;     // 00532b50
    std::vector<RankEntry> profile_dossiers() override;  // 00532b70
    std::string string_key(std::int32_t index) override;
    void level_start(std::int32_t profile, std::int32_t level, std::int32_t a, std::int32_t b, std::int32_t c) override;
    std::uint8_t profile_byte(std::int32_t profile, std::uint32_t offset) override;
    std::int32_t profile_word(std::int32_t profile, std::uint32_t offset) override;
    std::string profile_text(std::int32_t profile, std::uint32_t offset) override;
    std::uint32_t card_make_directory() override;
    std::uint32_t card_format() override;
    void card_remove(const std::string& name) override;
    void card_rename(const std::string& from, const std::string& to) override;
    std::uint32_t card_exists(const std::string& name) override;
    std::uint32_t agent_save(std::int32_t profile) override;
    void options_frontend_sleep() override;
    void options_frontend_wake() override;
    void app_suspend(std::uint32_t mode) override;
    void app_resume() override;
    bool boot_video_pal() override;
    void video_mode_set(std::uint8_t mode) override;
    void port_options_wake(gui::Widget* screen) override;
    bool port_options_pressed(gui::Widget* screen, gui::Widget* sender) override;
    bool port_lan_play() override { return services_.lan_play; }  // port-only (132ln1)
    gui::PortSettingsScreen* port_settings_screen() override { return services_.port_rows.settings; }  // 132win11
    void radio_sound(std::int32_t kind, std::int32_t type, const std::string& name, std::int32_t priority) override;
    std::uint32_t zeus_archive() override;
    std::int32_t zeus_lookup(std::uint32_t archive, const std::string& name) override;
    std::int32_t zeus_has(std::int32_t profile, std::int32_t a1, std::int32_t a2, std::int32_t a3) override;
    std::int32_t zeus_viewed(std::int32_t profile, std::int32_t a1, std::int32_t a2, std::int32_t a3) override;
    BitmapTexture bitmap_load(std::uint32_t archive, const std::string& name) override;
    void bitmap_release(std::int32_t slot) override;
    std::uint32_t agent_create(std::int32_t profile, std::int32_t slot, std::uint32_t nosave) override;
    void agent_set_name(std::int32_t profile, const std::string& name) override;
    // Checkpoint 132cc4b: GuiAgentModify's services.
    AgentPartCache* agent_parts() override;
    std::optional<WeaponRecord> weapon_record(std::int32_t item) override;   // 00147840(*004ffd30, item)
    void weapon_preload(std::int32_t item) override;                         // 00145710(db, item, 0)
    std::vector<std::string> agent_locked_parts(std::int32_t profile) override;
    void agent_store_appearance(std::int32_t profile, const AgentPacking& packing, const AgentPartCache& cache) override;
    void profile_set_byte(std::int32_t profile, std::uint32_t offset, std::uint8_t v) override;
    std::int32_t loc_find(const std::string& key) override;
    std::optional<std::string> loc_text(std::int32_t id) override;
    void app_40f540() override;
    // Checkpoint 132cc5: GuiAgentCreate's services.
    std::vector<std::uint8_t> skl_file(const std::string& path) override;
    void profile_set_word(std::int32_t profile, std::uint32_t offset, std::int32_t v) override;
    void profile_set_text(std::int32_t profile, std::uint32_t offset, const std::string& s) override;
    // Checkpoint 132cc4: the options file and the card wait's nested GUI frame.
    std::uint32_t options_save(const gui::GameOptionsRecord& record) override;   // 0045a2a0
    void card_poll() override;                                                  // 00441c10
    // Nested frames drawn by card_poll (002cbad0 inside a card wait) since construction.
    std::int64_t nested_frames() const { return nested_frames_; }

    // GameFlowHost.
    void fn_1694b0() override { record("001694b0"); }
    void fn_37e780(const VideoModeRequest& mode) override;
    void fn_165950() override { record("00165950"); }
    void fn_2cb8c0(std::uint32_t a1) override;
    void fn_2c7d90(std::uint32_t show) override;
    void fn_2c7c40(std::string_view, std::uint32_t) override;
    void fn_136710() override;
    void fn_2cc3e0() override;
    void fn_2cbb30() override;
    void fn_2cbad0() override { drawn_ = true; }
    void fn_37e320() override { record("0037e320"); }
    void fn_3e9c20() override;
    void fn_2cb160() override;
    void fn_3c8c70() override { record("003c8c70"); }
    void fn_3f9470() override { record("003f9470"); }
    void fn_2ca580() override { record("002ca580"); }
    void fn_2c9420() override { record("002c9420"); }
    void fn_2c9860(std::uint32_t a1) override;
    bool fn_2c7cb0() override;
    void fn_36a240() override { record("0036a240 pad reset"); }
    float fn_133340() override { return 1.0F / 60.0F; }
    std::int32_t fn_3f8220(MoviePlayer& player, float dt) override;
    void lock_10dc40() override {}
    void unlock_10dca8() override {}
    void destroy_player(MoviePlayer& player) override;
    void fn_36b250(std::uint32_t a0) override { record("0036b250 " + std::to_string(a0)); }
    void movie_end_message(std::uint32_t message) override;
    void start_movie(GameFlow& g, std::string_view name) override;
    void agent_332a10(std::int32_t, std::int32_t, std::int32_t) override;
    void agent_3328d0(std::int32_t a1, std::int32_t a2, std::int32_t a3, std::int32_t t0) override;
    std::int32_t demo_count_1302f0() override;
    std::uint32_t timer_t1() override;
    bool allocate_queue() override { return true; }
    void free_queue() override {}

    // The host handle 001314f0(004ffb50, -1) answers for the one agent.
    static constexpr std::int32_t profile_handle = 0x1000;
    // What +b50 holds for rank row k (the EE stores the row's heap address).
    static std::uint32_t rank_value(std::size_t k) { return 0x01000000U + 0x24U * static_cast<std::uint32_t>(k); }

private:
    void record(const std::string& text);
    void to_runtime();
    void from_runtime();
    void gui_start();     // 002cb3d0(app, 0)
    void loading_show(const std::string& name);
    void loading_hide(bool defer);
    void loading_update();
    bool loading_ready();
    void options_load();  // 0045a150(*004ffd2c)
    // 002c9510; nested: called from inside the GUI runtime (a GuiGameOptions event), so no state is copied.
    void gui_resume(bool nested = false);
    void music(std::int32_t value);   // 002c9af0
    void prologue();                  // 00167c40 before the state blocks
    template <class F> void front_end_sound(F&& f);   // 003caa50 under the 004ffc90 "FRONTEND" swap
    template <class F> void in_runtime(F&& f);
    gui::PadInput pad_input() const;
    void check_profile(std::int32_t profile) const;
    AgentRecordView view() const { return AgentRecordView(agent_); }
    // profile_refill_host, followed by the PORT-ONLY cheats that are on.
    AgentRefillHost refill_host();
    FrontendServices services_;
    bool port_unlock_all_ = false;
    bool port_unlock_weapons_ = false;
    gui::GuiRuntime rt_;
    std::deque<UsbKeyReport> usb_keys_;
    GuiDecor decor_;
    GameFlow g_;
    FlowGui gui_;
    FlowApp app_{0, 640, 448, 0x1e};
    FolderAgentCard card_;
    std::array<std::uint8_t, agent_record_size> agent_{};
    AgentLoadState agent_state_;
    PortAgentPatch port_agent_patch_;
    AgentRefillHost refill_host() const;
    std::unique_ptr<AgentSaveBuffer> save_buffer_;
    std::map<std::string, std::unique_ptr<GuiFile>> gui_files_;

    std::int64_t frames_ = 0;
    std::int32_t ms_ = 0;
    std::uint16_t held_ = 0, previous_held_ = 0;
    std::int32_t last_update_ms_ = 0;   // 002cbb30's clock
    std::int32_t clock_ms_ = -1;        // 0048a018 (00167c40)
    std::uint8_t loading_51ee90_ = 0;   // byte 0051ee90 (the loading screen's thread runs)
    gui::Widget* loading_widget_ = nullptr;
    gui::Widget* loading_anim_ = nullptr;
    std::string loading_name_;
    std::int32_t loading_started_ms_ = 0;
    std::int32_t loading_target_ = 0;
    std::int64_t loading_hide_frame_ = -1;
    ScriptScheduler events_;            // 0055a2e0 (003ea1d0)
    bool drawn_ = false;
    bool constructed_ = false;          // 00136710 ran
    bool first_entry_ = true;           // byte 0048c248
    std::int32_t demo_cache_ = -1;      // 00489be0
    // App bytes / words.
    std::uint8_t app_28_ = 0, app_29_ = 0, app_2c_ = 0, app_36_ = 0, app_37_ = 0, app_38_ = 0;
    std::int32_t app_30c_ = -1;
    std::int64_t nested_frames_ = 0;
    std::uint8_t app_310_ = 0;          // app +310 (002c82f0 sets it, 002c81d0 clears it)
    std::int64_t wait_frames_ = 0;
    std::uint8_t nested_wait_thread_ = 0;   // port-side label only (132ln4): nested_service runs for 002c84b0
    void nested_service();              // 002cc260's GUI part past its guards: 002cbb30 / 002cbad0 (+28 / +2d)

    // The movie player (G+b0) and its stream.
    std::unique_ptr<MoviePlayer> player_;
    std::unique_ptr<movie::MovieStream> movie_;
    movie::MoviePlayerFlags flags_;
    bool movie_ended_ = false;
    int field_ = 0;
    std::string movie_name_;
    std::unique_ptr<movie::SubtitleFile> subtitle_file_;
    std::unique_ptr<movie::SubtitleTrack> subtitle_track_;
    std::array<float, 4> subtitle_colour_{};
    mpeg2::Frame frame_;
    bool frame_valid_ = false;
    std::uint64_t serial_ = 0;
    std::vector<std::int16_t> audio_;
    int audio_rate_ = 0, audio_channels_ = 0;

    PadVibration vibration_;            // 004feed0
    std::array<std::uint8_t, 2> vibration_logged_{};
    void vibration_log(std::uint32_t address);

    std::set<std::string> logged_;
    std::vector<std::string> log_;
};

}  // namespace omega
