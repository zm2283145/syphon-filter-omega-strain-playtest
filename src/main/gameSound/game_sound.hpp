#pragma once
// gameSound.cc's sound manager (constructor 001c9ea0, message receiver
// 001c9d00) and the request layer behind the script natives PlayQuip
// (001c7120 / 001c7180), StopMusic (001c71c0) and PlayMusic (001c7200):
//
//   workers   001c6720 / 001c67c0 PlayQuip, 001c6890 StopMusic, 001c6a00
//             PlayMusic: a 001ca210 request wrapped by 0020f2a0 (quip, class
//             *004f53c8), 0020faa0 (play, *004f53b8) or 0020ecb0 (stop,
//             *004f53d8) and posted to the world 004ffb50 with transport 0
//             (003c9420: queued, delivered by the next message pump 003c8de0)
//   receiver  init 001c9bf0 (class *004f5370), level load 001c98d0
//             (*004f5378), the frame update 001c8e10 (*005436b0), PLAY
//             001c8770 (*004f53b8 / *004f53c8), STOP 001c80f0 (*004f53d8),
//             PAUSE 001c7e00 (*004f53e8, built by 0020e860) and its 4000 ms
//             event 001c80b0 (vtable 004dac30)
//   helpers   slot allocation 001c75c0, volume / pan 001c78e0, the stream
//             transition 001c83a0 and its 2000 ms event 001c86f0, the stream
//             and lip-sync callbacks 001c9430 / 001c9400
//
// The manager holds 50 slots (+30 + i*1b0, constructor 001c9f80) and the
// sound HOGs of the level (+54a8 GAMEDATA\<level>\SNDVAG.HOG<lang>, +54ac
// ZMEDIA\SNDVAG<lang>.HOG, +54b0 ZMEDIA\MUSIC.HOG). Everything below the
// EE request layer (the 989snd library 003a6xxx..003a8xxx and the IOP) is a
// host call with its arguments (GameSoundHost::snd names the address). Bank
// sounds (types 0..3; checkpoint 119k) take PLAY's bank branch 001c8c98:
// 003a8000 (the sound's 16 user-data bytes DMAed to the slot's H+0, near /
// far) then 003a8070 (play), their handle arriving through 001c9430. PlaySnd
// (001c7290) is the worker 001c6d00.
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace omega {

// 0036e5d0 / 0036e1b0's resource record (a slot's +48 VAG and +e4 LPD). The
// int16 at +84 is never initialised by either and is not modelled.
struct GameSoundResource {
    std::uint32_t flags_0{};  // 0 after 0036e5d0; 2 | (4 when flagged) after 0036e1b0
    std::string name_4;       // 128 bytes (0036e1b0's strncpy); untouched by 0036e5d0
    std::uint32_t word_88{}, word_8c{}, word_90{}; // zeroed by 0036e5d0 (0003ebef0)
    std::uint8_t loaded_94{}; // set when the asynchronous load (003eae60) completes
    std::int32_t hog_98{};    // the HOG (GameSoundManager id, 1-based; 0 none)
};

// The 001ca210 request record (the message payload at +30) and a slot's
// record (+50 + i*1b0), which share the layout.
struct GameSoundRecord {
    std::int32_t type_0{-1};                // 4 quip / voice, 5 music, 0..3 bank sounds
    std::array<std::uint8_t, 16> name_10{}; // 00129360 copy
    std::uint32_t owner_20{};               // the owning object (0: none)
    std::int32_t key_24{-1};                // the owner's +c when recorded, -1 without one
    std::array<float, 4> position_30{};
    std::uint32_t handle_40{};              // the 989snd stream handle
    std::uint8_t state_44{};                // 0 free, 1 playing, 2 start, 3 LPD loading, 4 LPD ready, 6 restart
    GameSoundResource vag_48;
    GameSoundResource lpd_e4;
    std::int32_t priority_180{};
    std::int16_t slot_184{-1};
    std::uint8_t subtitle_186{};
    std::string name() const;
};

struct GameSoundSlot {
    std::int32_t near_0{};  // H+0 (0: 5)
    std::int32_t far_4{};   // H+4 (0: 200)
    std::int32_t word_8{}, word_c{}; // H+8 / H+c: the rest of a bank sound's user data (003a8000)
    std::uint8_t loop_14{}; // H+14 (set by 001c75c0): the music stream's flag 4
    GameSoundRecord record; // H+20
};

// The HOG object 003f96e0 returns (+14c loaded, +84 first sector, +148
// header: +4 count, +10 data start; +15c names, +160 offsets).
struct GameSoundHog {
    std::string path;
    bool loaded_14c{};
    std::uint32_t sector_84{};
    std::int32_t data_10{};
    std::vector<std::string> names;
    std::vector<std::uint32_t> offsets; // one per name
};

struct GameSoundOwnerView {
    std::uint8_t byte_2c{}, byte_2d{}, byte_2e{}; // present: +2d && +2c && !+2e
    std::uint8_t class_49{};                      // 2: an actor
    std::array<float, 4> root_40{};               // the +50 root's +40 (after its virtual +58 when +50 is set)
};

struct GameSoundCamera {
    std::array<float, 4> root_40{};     // the camera object's +50 root +40
    float heading_64{};                 // the camera object's +64
};

struct GameSoundMessage {
    enum class Kind : std::uint8_t { play_20faa0, quip_20f2a0, stop_20ecb0, pause_20e860 };
    Kind kind{};
    std::uint8_t flag_20{5};            // 003c9e30's 5; 1 on the receiver's own reposts
    GameSoundRecord request;
    std::uint8_t pause_24{};            // 0020e860's flag (class *004f53e8): 1 pause, 0 resume
};

class GameSoundHost {
public:
    virtual ~GameSoundHost() = default;
    // *(owner + c) (the object's serial).
    virtual std::uint32_t owner_serial(std::uint32_t owner) = 0;
    virtual GameSoundOwnerView owner_view(std::uint32_t owner) = 0;
    virtual std::uint8_t owner_state_108(std::uint32_t owner) = 0;   // virtual +108 & 0xff (3: dead)
    virtual void owner_clear_2dc8(std::uint32_t owner) = 0;
    // 00391810(owner + 60, 0036dc60(slot +e4), 0).
    virtual void lip_sync_391810(std::uint32_t owner, const GameSoundResource& lpd) = 0;
    // *(004ffc0c) +30's root +40 (nullopt when either pointer is null).
    virtual std::optional<std::array<float, 4>> listener_4ffc0c() = 0;
    // *(00159560(004ffc14)) (nullopt when null).
    virtual std::optional<GameSoundCamera> camera_4ffc14() = 0;
    virtual bool hud_4ffc2c() = 0;                                   // *(004ffc2c) != 0
    virtual void subtitle_245e00(const std::array<std::uint8_t, 16>& name) = 0; // 00245e00(hud, name, 0, 0)
    virtual std::string level_name_4ffc90() = 0;
    virtual std::uint8_t language_55d470() = 0;
    virtual std::uint8_t game_mode_4ffba0() = 0;
    virtual bool multiplayer_5721c8() = 0;
    virtual bool authority_5721c0() { return true; }
    virtual bool local_player_14(std::uint32_t) { return true; }
    virtual void send_player_music(std::uint32_t owner, std::string_view name, bool stop);
    virtual std::uint32_t output_mode_4ffd2c_14() = 0;               // *(*(004ffd2c) + 14)
    virtual GameSoundHog open_hog_3f96e0(const std::string& path) = 0;
    // 003eae60(003eba50(), slot +e4, 001c9400, slot, 0, 0, 1): completes later
    // through GameSoundManager::lpd_loaded, never inside this call.
    virtual void load_lpd_3eae60(std::size_t slot, const GameSoundResource& lpd) = 0;
    virtual void release_lpd_3eaa60(std::size_t slot, const GameSoundResource& lpd) = 0; // 003eaa60(003eba50(), slot +e4, 1)
    // 003ea1d0 on the script context's callback list (delay word +0).
    virtual void schedule_3ea1d0(std::int32_t delay, std::function<void()> event) = 0;
    // A 989snd EE call (address and arguments; a callback record is passed as
    // its slot index). 003a85e0 runs the IOP replies.
    virtual std::int32_t snd(std::uint32_t address, const std::vector<std::int32_t>& args) = 0;
};

class GameSoundManager {
public:
    static constexpr std::size_t slot_count = 50;

    explicit GameSoundManager(GameSoundHost& host); // 001c9ea0 (+ 001c9f80 x 50)

    // 001ca210(rec, type, name, owner, prio).
    GameSoundRecord request(std::int32_t type, std::string_view name, std::uint32_t owner, std::int32_t priority);
    // The workers (each posts at most one message).
    void play_quip(std::string_view name, std::uint32_t owner, std::int32_t priority); // 001c6720
    void play_quip(std::string_view name, std::int32_t priority);                      // 001c67c0
    void stop_music(std::string_view name, std::uint32_t player = 0);                  // 001c6890
    void play_music(std::string_view name, std::uint32_t player = 0);                  // 001c6a00
    // The cNetObj receiver callbacks 0020edd0 / 0020e8b0 queue the reconstructed
    // message with +20 = 1 on the addressed console.
    void receive_player_music(std::string_view name, bool stop);
    // 001c6d00(type, name, owner): PlaySnd's worker (type 4: a quip message, else a play message).
    void play_sound(std::int8_t type, std::string_view name, std::uint32_t owner);
    // 001c6ea0(type, name): PlaySnd(type, name)'s worker (001c7310; checkpoint 132sx12). It builds the message twice
    // (001ca210 with owner 0, wrapped by 0020f2a0 for type 4, else 0020faa0): the first copy gets +20 = 1 and is
    // destroyed unsent; the second keeps 003c9e30's +20 = 5 and is posted (003c9420 transport 0). No multiplayer branch.
    void play_sound(std::int8_t type, std::string_view name);
    // 001c6c50(type, name) / 001c6b70(type, name, owner): StopSnd's workers (one STOP message, +20 = 1).
    void stop_sound(std::int8_t type, std::string_view name, std::uint32_t owner);
    // 001c6650 (StopAllQuips 001c7100 -> 001c6650(0); a0 unused): 001ca210(rec, 4, "" (0049f498), 0, 0)
    // wrapped by 0020ecb0(msg, rec, 0) with +20 left at 003c9e30's 5, posted with 003c9420 transport 0.
    // The receiver's type-4 stop (001c80f0) is transition(4, -1, immediate) whatever the name.
    void stop_all_quips();
    // 0020f990(msg, desc, owner) (PLAY, class *004f53b8) and 0020eba0 (STOP, *004f53d8): an owned message
    // from a 001ca210 descriptor: type +0 and name +10, and for PLAY the descriptor's +180 / +186. The owner
    // is not validated: key = *(owner + c) (owner_serial), -1 without an owner; the position is zero. The
    // callers set +20 = 1 and send with 003c9420 (transport 0: post, 1: deliver).
    GameSoundMessage owned_play(const GameSoundRecord& descriptor, std::uint32_t owner);
    GameSoundMessage owned_stop(const GameSoundRecord& descriptor, std::uint32_t owner);
    // 0020f860(msg, desc, pos) (PLAY, class *004f53b8): as owned_play without an owner (+20 0, key -1)
    // and with the position +30 = the four words at pos (the footstep's probe point, 00188fb0).
    GameSoundMessage positional_play(const GameSoundRecord& descriptor, const std::array<float, 4>& position);

    // The message pump 003c8de0 for the sound messages: the queued messages
    // are taken as a snapshot and delivered in order; posts made meanwhile
    // wait for the next pump.
    void pump();
    void deliver(const GameSoundMessage& message);
    void post(GameSoundMessage message) { queue_.push_back(std::move(message)); }
    const std::vector<GameSoundMessage>& queue() const { return queue_; }
    void discard_queue() { queue_.clear(); }

    void init();       // 001c9bf0
    bool level_load(); // 001c98d0
    // 001c9710: everything stopped (3a7fe0, 3a7310, 3a7200), the 50 slots' +40 / +44 cleared, the banks
    // unloaded (3a8210: WEPS, ENVIRON, VOX, SHELL; +5490.. keep their values), 3a85e0 until 0, the three
    // stream HOGs closed (003f9530) and +54a8 / +54ac / +54b0 cleared.
    void level_unload();
    void frame();      // 001c8e10
    void play(const GameSoundRecord& request); // 001c8770
    void stop(const GameSoundRecord& request); // 001c80f0
    void pause(bool on);                       // 001c7e00
    // 001c7d80 (the front end's music stop 002c9aa0, checkpoint 132on2): each of the 50 slots whose record is music
    // (type 5): its stream stopped (003a8040) when it has a handle, the handle and the state (+44) cleared; then one
    // 003a85e0.
    void stop_music_streams();
    // 001c96d0(mgr, v): 003a7fb0(-1) (pause every group's streams) when v, else
    // 003a7f80(-1); the start menu's pause (GuiStartMenu wake / sleep).
    void pause_groups(bool on);
    std::size_t pause_events() const { return pause_events_; }
    // 001c9540(mgr, channel, volume): channel 0 (sfx) the 989snd groups 0 and 3, 1 (music) group 1,
    // 2 (voices) groups 2 and 0xe, each 003a81d0(group, volume); another channel does nothing.
    void set_volume(std::uint8_t channel, std::int32_t volume);
    void set_mode(std::int32_t mode);  // 001c9670: 003a80f0(mode)
    // 001c95d0(mgr, name): the first slot whose record is a quip (type 4) of that name (00128e80): its
    // +44 state != 0; false when no slot matches (the debrief's voice wait, 00462270).
    bool quip_playing(std::string_view name) const;
    int allocate(const GameSoundRecord& request, std::uint8_t state); // 001c75c0: slot or -1
    std::pair<std::int32_t, std::int32_t> volume_pan(std::size_t slot); // 001c78e0
    void transition(std::int32_t type, int new_slot, bool immediate);   // 001c83a0
    void fire_transition();                                             // 001c86f0
    void lpd_loaded(std::size_t slot);                                  // 001c9400
    void stream_result(std::int32_t result, std::size_t slot);          // 001c9430
    // 003a8000's IOP reply: the 16 user-data bytes DMAed to the slot's H+0 (only when the sound was found).
    void bank_user_data(std::size_t slot, const std::array<std::int32_t, 4>& words);
    // 0036d2a0: the member index, -1 when absent, 0 when the HOG is not loaded.
    std::int32_t find(std::int32_t hog, std::string_view name) const;
    // The member's sector (*(hog+84) + (*(hdr+10) + 0036d520(hog, i)) / 2048).
    std::int32_t member_sector(std::int32_t hog, std::int32_t index, std::uint32_t site) const;
    GameSoundRecord message_copy(const GameSoundRecord& source, bool stop);

    const std::array<GameSoundSlot, slot_count>& slots() const { return slots_; }
    std::array<GameSoundSlot, slot_count>& slots() { return slots_; }
    const std::vector<GameSoundHog>& hogs() const { return hogs_; }
    std::int32_t add_hog(GameSoundHog hog); // 003f96e0's list (by path); returns the id
    std::array<std::int32_t, 6> banks_5490{}; // 5490 ENVIRON, 5494 WEPS, 5498 VOX, 549c SHELL, 54a0, 54a4
    std::int32_t vag_hog_54a8{}, zmedia_hog_54ac{}, music_hog_54b0{};
    std::uint8_t paused_20{};
    // 004f2cd0 the stop slot, 004f2cd4 the pending slot, 004f2cd8 active,
    // 0048a330 the one-shot reset (1 at boot and after each level load).
    int transition_stop{-1}, transition_pending{-1};
    std::uint8_t transition_active{};
    std::uint8_t transition_reset{1};
    std::size_t transition_events() const { return transition_events_; }

private:
    GameSoundHost& host_;
    std::array<GameSoundSlot, slot_count> slots_{};
    std::vector<GameSoundHog> hogs_;
    std::vector<GameSoundMessage> queue_;
    std::size_t transition_events_{};
    std::size_t pause_events_{};
    std::uint32_t valid_owner(const GameSoundRecord& r);
    void schedule_transition_event();
};

// The 6-entry language suffix table 00493a40 read by 003fe090.
std::string game_sound_language_suffix(std::uint8_t language);

}
