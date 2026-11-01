#pragma once
// PORT-ONLY (not in the original; LAN phase 3, checkpoint 132ln5): LanNetCore, one network game's EE-visible
// memory and host for the native DME.cc in-level game side (src/main/DME/dme_game.*). research/NET_INLEVEL.md
// section 5 ("where LAN plugs in").
//
//  - The memory holds the game's network globals at their original addresses. The constructor gives the
//    executable's initial values to the .data words the code reads (0x00494178 / 0x00494198 = -1, 0x00493a10..13
//    = 2, 0x00497540 = 1, the "Foo" start name at 0x004bfa10, the team latch 0x0048a100 = 1, the DME's current
//    world 0x0048c450 = 4, the GenInfo handle 0x00497578 = -1), and the session state the replaced lobby calls
//    leave: 0x005721c8 = 1, 0x005721c0 = the host role, 0x005721a8 = world 0, 0x005724b0 = 6 (0042a870 stores 5,
//    the connect callback 00429790 6; LanSession::host / join stand in for both).
//  - call() dispatches by original address: DME.cc's natives (00429d30, 0042a770, 0042a1b0, 0042b0c0, 0042b1a0,
//    003fac40), the callbacks LanDme makes (00429c00, 003faab0), the DME API (LanDme), strncpy 00129a70, the
//    game's printf 00127358 / 0010f388 (kept in log()), and the EE kernel calls of the DME thread (below). The
//    level-load callees 001350e0 / 00185840 / 003fa5d0 go to the caller's level host; anything else throws, naming
//    its address. The team setup 0016a970 / 0016a0a0 is native on this memory (checkpoint 132ln5c): the team data
//    (0x004ee620) of a network game is a DME network object (002eaf28, call_framed) once 0042b0c0 has set
//    0x005721d0; before that it is a 0x100-byte block from the game's heap 0036b180 (PORT-ONLY: the arena below;
//    0036b130 frees nothing). 0010dc40 / 0010dca8 (DisableIntc / EnableIntc 9) guard the heap when 0x00533880 is
//    set.
//  - The DME update thread (00429380 / 004295e0) has no EE thread here: the model keeps its priority and state;
//    StartThread and iWakeupThread run the body (DmeThreadBody::resume) at once, as the EE kernel does (the DME
//    thread, priority 5, outranks the main thread, priority 10 after 00429380); SleepThread ends the run.
//    advance_timer() plays the TIM0 compare events (586 counts at BUSCLK / 256 = about 1.017 ms each) into the
//    original handler 004292b0, which wakes the thread every 100 events. The handler is installed by the
//    original 00429430 in the constructor (the original runs it at boot, 0013d470).
//  - The characters (checkpoint 132ln5f, research/NET_INLEVEL.md section 4.9). The play's player is mirrored as the
//    local player 0x00571770 (set_local_player: the fields 00185a30 reads, with the player vtable 0x004da690, whose
//    +0x1c is 00185a30); 003fa940 sends it to a loaded client through the native 00185a30 -> 002c72a0 -> 003fad50
//    -> 002ea0a0 (LanDme). A received NetCreateCharacterMsg runs the native 002c7810 -> 002c6b70, whose engine
//    callees are PORT-ONLY stand-ins here: 001b8130 (the human's constructor) sets up the human block's fields the
//    DME.cc code reads and queues a RemoteHuman for the play (take_remote_humans), which puts a figure there; the
//    weapon, team, script and draw callees (00143df0 / 001439b0 / 0040b790 / 003cc990 / 001ae450 / 001b8750 /
//    002c9270 / 00242fa0) do nothing; 0040b6d0 finds a remote human by its key; 001b8740 answers team 0;
//    002d85d0 returns its block. 132ln5h6 (research/NET_INLEVEL.md section 4.17): the appearance travels as the
//    original's: 002c72a0 compacts the player's packing (+0xa0) with the native 004073c0 (agent_packing_compact over
//    the level's part cache, set_part_cache) into the scratchpad 0x70000000; 002c6b70 expands the message's with
//    the native 00406ec0 (agent_packing_expand) and the human's descriptor (+0x18 of 001b8130's stack arguments) is
//    the RemoteHuman's packing, which the play dresses the figure in. Without a cache (no set_part_cache) the
//    compact is PORT-ONLY empty (length 0) and the expand is not run.
//  - The leave and the per-frame data (checkpoint 132ln5g, research/NET_INLEVEL.md section 4.10). LanDme calls the
//    world's client-left callback 00429920 (native) for a client the transport lost; each net_frame() then runs the
//    native 001b2e90 (the human's virtual +0x4c, whose per-frame caller is not decoded) on each remote human, which
//    removes a left client's human: the HUD line (003fa3f0 logged), 003fb750 and the NetMap erase (native), the
//    character list 0040b720 (the key forgotten here) and the release virtual +0x60 (a RemoteLeft for the play).
//    132ln5h3: the per-frame movement travels as the original's locomotion message (see RemoteMove).
//  - The remote human's controller (checkpoint 132ln5h, research/NET_INLEVEL.md section 4.11). 002c6b70 makes it
//    with the native 002d85d0 (vtable 0x004de530: +0x40 the update 002d83c0, +0x54 002d7c70, +0x60 the aim
//    accumulator 0022f5b0, +0x88 002d7c60); 001ae450 (PORT-ONLY stand-in) stores it at the human's +0x58. Each
//    net_frame() runs the native 002d83c0 on each remote human's controller (PORT-ONLY caller: the component update
//    that calls +0x40 is not decoded), which replays the other console's record block (+0x3584: the aim point or the
//    locked object, the aim mode, the shot and weapon counters) as actor actions. Their engine callees are
//    PORT-ONLY stand-ins that queue them for the play: 003c9420 (an actor action: RemoteAction), 0022f5b0 (the aim
//    update: RemoteAim), 001439b0 (the held weapon: RemoteWeapon); 0040c540 finds the local player, a remote human
//    or (an NPC) a proxy object with the id; their life state virtual +0x108 (001872f0 for the player) answers 0.
//    The local player's record block is its cNetHuman's data (+0x3584, created by 002d1ff0 and linked by 002d1dc0
//    from the player update's network tail 001b53d0, run each frame by net_frame()): the play's targeting, shots and
//    weapon switches write it with the native 002494e0 / 00249420 / 00249490 / 00249530 / 002493c0 (record_*), and
//    the DME library's field flush (002e4550 -> 002e31d0) sends the changed fields. Object ids on the wire ("LAN
//    ids"): an NPC's +c, a human's key (its +0x10) for the players.

#include "agent_modify.hpp"
#include "dme_game.hpp"
#include "lan_dme.hpp"
#include "npc_generator.hpp"   // 132ln5h12: GeneratorSpawnMessage / GeneratorSpawnReceiveHost by value

#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <span>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace omega::lan {

class LanNetCore final : public LevelLoadHost {
public:
    LanNetCore(LanTransport& transport, LanClock clock, bool host_role);
    ~LanNetCore() override;
    LanNetCore(const LanNetCore&) = delete;
    LanNetCore& operator=(const LanNetCore&) = delete;

    LevelLoadMemory& memory() { return memory_; }
    LanDme& dme() { return *dme_; }

    std::uint32_t call(LevelLoadMemory& memory, std::uint32_t address, std::initializer_list<std::uint32_t> args) override;
    // 002eaf28 (from 0016a0a0): its ninth and tenth arguments are the stack words $sp+0 / $sp+8.
    std::uint32_t call_framed(LevelLoadMemory& memory, std::uint32_t address, std::initializer_list<std::uint32_t> args,
                              std::uint32_t sp) override;
    std::uint32_t game_heap_frees() const { return game_frees_; }

    // 001693f0's network sequence, in the order of run_level_load_wrapper (level_load_state.cpp, the "level load
    // state" goldens): 0042a770, 0016a970, 001350e0(0x004ffb50), 0042a1b0, 0042b0c0, then 0016a970 on a host (or
    // solo) or 00185840 + 003fa5d0 on a client, then 00429d30. `level` serves 001350e0 / 00185840 / 003fa5d0;
    // nullptr: they throw.
    void level_load_001693f0(LevelLoadHost* level);
    // The same sequence in two parts around 001350e0 (checkpoint 132ln5e): begin runs 0042a770 and the first
    // 0016a970 (the front end's hand-off); finish runs 001350e0(0x004ffb50) on level and the rest.
    void level_load_begin(LevelLoadHost* level);
    void level_load_finish(LevelLoadHost* level);
    bool level_loaded() const { return level_loaded_; }

    // The network game's per-frame updates in the level (checkpoint 132ln5f): 00169390 (00167c40 runs it every
    // frame in every state but the level load: 00472080 and the DME update 00429d30 on this thread, then the lobby
    // keep-alive 002ca580), then 003fbec0 (states 0xf / 0x13 with 005721c8 set).
    void net_frame();
    // Retail 00134640 stops its cleanup update thread at teardown tail;
    // frontend InitGui starts the lobby's DME thread again with MP off.
    // Call once per GUI
    // frame with elapsed milliseconds while this core owns the transport;
    // LanSession may pump discovery only, never transport.poll({}). The next
    // 001693f0 reuses this thread and 0042a1b0 stops it.
    void retained_frontend_tick(double milliseconds);
    // 00134110 (game_33.c:262): each end-of-mission screen iteration runs cMachine_NetUpdate 00169390, World_Teardown
    // still having g_IsMultiplayer set (00134640 clears it after the screens): 00472080 and the DME update 00429d30,
    // then the lobby keep-alive 002ca580. The elapsed real time first plays into the TIM0 model, as net_frame's caller
    // does. Not the in-level 003fbec0 world services.
    void summary_net_update(double milliseconds);
    // Frontend state 3 re-arms multiplayer before the next 001693f0 hand-off.
    // The retained room cannot change host role between missions.
    void resume_retained_level(bool host_role);
    // 132ln5h7: 00169360 on the network memory (run_gameplay_state reads and clears the quit request 0x005721d8
    // before its quit dialog 002cae30); quit_text() is the dialog's message, the C string at 0x005721e0.
    bool take_quit_request();
    // 133lt3: 00169360's tail (cDME_Quit): read and clear 0x005723b8 (set by 002ca580 when the lobby session's
    // fatal error 00452fd0 comes in session state 10, or by 00429d30's world-status walk); the caller then runs
    // cGameFrontEnd_LobbyDestroy 002ca640(app, 0) (FrontendApp::network_lobby_destroy).
    bool take_lobby_lost();
    // 133lt3: cDME_ActivateLobby 00429500(on): the byte 0x00572488 (004294c0 answers no lobby while it is clear).
    void activate_lobby(bool on);
    // 133lt3 (PORT-ONLY boundary): SFOLobby_IsFatalError 00452fd0 for the online lobby. The callback answers the
    // lobby session's state (+0x255c) when its server connection is lost, nullopt while it holds. A LAN lobby has no
    // server: without a callback 00452fd0 keeps answering 0.
    void set_lobby_fatal(std::function<std::optional<std::uint32_t>()> fatal);
    std::string quit_text();
    struct QuitNotice {
        std::uint8_t kind{};  // 005723b0: 1 selects canvas message 0x668, otherwise 0x667
        std::string key;      // cDME::m_ErrStr[16]
        std::optional<std::string> message;  // nonempty cDME::m_ErrMsg[128]
    };
    // Snapshot the original error fields without consuming the request latch.
    QuitNotice quit_notice() const;
    // PORT-ONLY (132ln5f): the lobby session's update 004536b0 (from 002ca580): LanSession's discovery (the
    // advertisement and the browser) stands in for SFOLobby's Medius lobby connection. With a pump set,
    // 004294c0 returns a sentinel session; without one, 0 (no lobby, as when 0x00572488 is clear).
    // 132ln5h14: the sentinel is the DME session pointer 0x00572480 with its byte 0x00572488 set (004294c0 reads
    // them natively), so the DME thread's 00429d30 also updates the lobby (004536b0) while it runs during the
    // level load, as the original does with its Medius session: the host keeps answering the LAN browse.
    void set_lobby(std::function<void()> pump);
    // 0044e340 is the caller's lobby transition (LanSession::exit_game on LAN).
    // It must retain the transport until 0042b1f0 has left the DME world.
    // A live lobby without this service is an unhosted teardown path.
    void set_lobby_exit(std::function<void()> exit_game);
    bool has_lobby_exit() const { return static_cast<bool>(lobby_exit_); }
    // The state-0x14 pass has destroyed its level objects. The native DME
    // cleanup is dispatched separately by the composed teardown adapter.
    // G+1c7 keeps the lobby/transport for the next GUI pass. The exit
    // callback always dies with this level because it may capture play locals.
    void finish_level_teardown(bool retain_session = false);

    // PORT-ONLY (132ln5e): the TIM0 events while a level setup on the calling thread stands in for 001350e0. On
    // the original the DME thread wakes every 100 events during the load (keeping the session's traffic going);
    // here a background thread plays the real time since start_load_pump() into advance_timer() until
    // stop_load_pump(), which returns the milliseconds played and rethrows what the pump stopped at. Nothing else
    // may touch this LanNetCore in between.
    void start_load_pump();
    double stop_load_pump();
    // PORT-ONLY (132ln5e): the level object lists 0x00571740 (0042b0c0) and 0x00571748 (0040ca30, 003fa5d0) as
    // empty lists: the play's level objects are not mirrored into this memory.
    void mirror_empty_level_objects();

    // PORT-ONLY (132ln5f): the play's player as the local player 0x00571770 (see the header): the root position, the
    // rotation rows (the component's +0x10 matrix) and the health (+0x40). Call it before net_frame(). 132ln5h3: the
    // floor plane (physical +0xf0 while +0xe8 is set: the player's virtual +0xf8 0014d3d0; none: it answers 0), which
    // the locomotion message carries (+0x40).
    void set_local_player(const std::array<float, 3>& position, const std::array<std::array<float, 3>, 3>& rows,
                          float health, const std::optional<std::array<float, 4>>& floor_plane = std::nullopt);
    // A remote human 002c6b70 created (its 001b8130 stand-in): the sender, the key (+0x10), the 4x4 matrix 002c6b70
    // built from the message (rows 0..2 the rotation, row 3 the position), the health and (132ln5h6) the appearance
    // 00406ec0 expanded (none: no part cache, or a message without one).
    struct RemoteHuman {
        std::uint16_t client = 0;
        std::uint32_t key = 0;
        std::uint32_t human = 0;
        std::array<float, 16> matrix{};
        std::int32_t health = 0;
        std::optional<AgentPacking> packing;
    };
    std::vector<RemoteHuman> take_remote_humans();
    std::uint32_t remote_humans_created() const { return remote_created_; }
    // 132ln5h6: the level's part cache 0055dda0 (the static registrations of the level load: COMMON MORPH.HOG,
    // the mission's NPCTEX.HOG and NPCSKM.HOG), which 004073c0 / 00406ec0 index; it must outlive this LanNetCore.
    void set_part_cache(const AgentPartCache* cache) { part_cache_ = cache; }
    // 132ln5h6: the play's player's appearance block (the human's +0xa0, which 00185a30 gives 002c72a0).
    void set_local_packing(const AgentPacking& packing);
    // 134lt7 (PORT-ONLY mirror): the play's player's +3269 (the revive's logic gate, SoloActorSimulation::logic_gate_3269)
    // on the local player 0x00571770, which 001b1710's entry tests: set, the update (and its network section) returns.
    void set_local_logic_gate_3269(std::uint8_t gate);
    std::uint32_t sections_gated_3269() const { return sections_gated_3269_; }
    // 132ln5h3 (research/NET_INLEVEL.md 4.14; it replaces 132ln5g's PORT-ONLY channel 0x03): a locomotion message
    // (action 0, kind 0xd) for a remote human. The other console's player controller update 001770f0 sends its
    // player's every 250 ms (003c9420 -> 003fb020 -> the serialiser 003e57b0, a 0x28-byte game message; none while
    // the position is the last one sent); the receive 003e4430 rebuilds it and delivers it (003c9350) to the key's
    // human through the message queue 003ca0e0, whose PORT-ONLY stand-in queues this for the play: the human's
    // message handler 001a92a0 (kind 0xd, the network branch of a human whose +0x14 is clear) is the play's. The
    // stale check (+0xc below the human's +0x3594: 0049e4a0 printed, dropped) runs here; `clock` is 0042b1a0(0) at
    // the delivery, for 001a92a0's 150 ms line-of-sight check (+0x3580).
    struct RemoteMove {
        std::uint32_t key = 0;
        std::uint32_t human = 0;
        std::uint32_t stamp = 0;               // +0xc: the sender's clock
        std::uint32_t clock = 0;
        std::array<float, 4> position{};       // +0x30 (w 1)
        std::array<float, 4> floor_plane{};    // +0x40 (the human's +0x3570)
        float heading = 0;                     // +0x50
        float speed = 0;                       // +0x54
    };
    std::vector<RemoteMove> take_remote_moves();
    std::uint32_t moves_sent() const { return moves_sent_; }
    std::uint32_t moves_received() const { return moves_received_; }
    // 132ln5g: a remote human 001b2e90 removed after its client left (00429920 set 0x00572418[client]): the human's
    // virtual +0x60 (the release; its stand-in queues this), with the HUD line 003fa3f0 showed ("PLAYER_LEFTGAME").
    struct RemoteLeft {
        std::uint16_t client = 0;
        std::uint32_t key = 0;
        std::uint32_t human = 0;
        std::string message;
        // 134lt6: 001b2e90 went on to 00257040(client) (the release of the client's backpacks): the play applies it
        // (CombatPlaySession::release_client_backpacks).
        bool backpacks_00257040 = false;
    };
    std::vector<RemoteLeft> take_remote_left();
    // 132ln5h: an actor action 002d83c0 / 002d7ed0 / 002d7d10 sent a remote human (003c9420(msg, human, transport)):
    // the message's +0x24 action, byte +0x28 kind, +0x2c, +0x30 (an object: a LAN id), +0x34, byte +0x38, the point
    // (kind 4: +0x30..+0x3c; kind 0xb: +0x40..+0x4c) and +0x50.
    struct RemoteAction {
        std::uint32_t key = 0;
        std::uint32_t human = 0;
        std::int32_t action = 0;
        std::uint8_t kind = 0;
        std::uint8_t transport = 0;
        std::int32_t word_2c = 0, word_30 = 0, word_34 = 0;
        std::uint8_t byte_38 = 0;
        std::array<float, 4> point{};
        float power_50 = 0;
    };
    std::vector<RemoteAction> take_remote_actions();
    // 132ln5h: the aim update 002d83c0 gave the remote human's controller (virtual +0x60 = 0022f5b0(this, target,
    // item, -1); the controller's skill +0x38 is 1.0 (00230240)): the target as a LAN id.
    struct RemoteAim {
        std::uint32_t key = 0;
        std::uint32_t human = 0;
        std::uint32_t target = 0;
        std::int32_t item = -1;
        std::int32_t subtarget = -1;
    };
    std::vector<RemoteAim> take_remote_aims();
    // 132ln5h: 001439b0(inventory, item) from 002c6b70 or 002d83c0: the remote human holds `item` (-1: nothing).
    struct RemoteWeapon {
        std::uint32_t key = 0;
        std::uint32_t human = 0;
        std::int32_t item = -1;
    };
    std::vector<RemoteWeapon> take_remote_weapons();
    // 132ln5h4 (research/NET_INLEVEL.md 4.15): the animation fields of a remote human after its network section
    // (001b1710's network part, human_network_section_001b1710) applied its cNetHuman block: +0x3248 bits 0 / 1
    // (block +0x50 bits 1 / 2) and 30 / 31 (block +0x51: 0xb / 0xc), +0x31b3 / +0x31b4 / +0x31b5 (bits 4 / 8 /
    // 0x20), the physical byte the action 0x43 (kind 1, to the human at once) sets (001ad5e0: +0x12 / +0x13; block
    // bit 0x40 clear: 1), the health 003cc990(human, hp, 1) set (+0x40) and the killed byte +0x33a5 (block +0x35).
    struct RemoteAnimation {
        std::uint32_t key = 0;
        std::uint32_t human = 0;
        std::uint32_t flags_3248 = 0;
        bool turn_correction_31b3 = false, accumulate_31b4 = false, byte_31b5 = false;
        std::optional<std::uint8_t> physical_12;
        std::optional<std::int32_t> health;
        bool killed_33a5 = false;
    };
    std::vector<RemoteAnimation> take_remote_animations();
    // 132ln5h4: the play's view of a remote human's figure before net_frame(): its health (the human's +0x40, which
    // the section compares with the block's +0x38) and the killed byte +0x33a5. Unknown keys are ignored.
    void set_remote_state(std::uint32_t key, float health_40, bool killed_33a5);
    // 132ln5h4: the local player's animation fields before net_frame(): +0x3248, +0x3204 (bit 0x10: 0019a6b0's
    // pending crouch), +0x31b3 / +0x31b4 / +0x31b5, the physical +0x12 (0: block +0x50 bit 0x40), the flashlight
    // (+0x33ec's byte) and +0x33a5. PORT-ONLY: their writers into the network block (block +0x50 / +0x51; the
    // original's are 001a92a0's actions and 0019a6b0, not hosted) are mirrored here (the header table).
    void set_local_animation(std::uint32_t flags_3248, std::uint32_t current_3204, bool turn_correction_31b3,
                             bool accumulate_31b4, bool byte_31b5, std::uint8_t physical_12, bool flashlight_on,
                             bool killed_33a5);
    std::uint32_t sections_run() const { return sections_run_; }
    // 132ln5h: the local player's inventory (00185a30 sends its items; 002493c0 the held one): the six slots' items
    // (-1: empty) and the held slot (6: nothing).
    void set_local_inventory(const std::array<std::int32_t, 6>& items, std::uint8_t held_slot);
    // 132ln5h: the local player's record writes (the play's PlayerTargetingHost::record, the shot, the switch), on
    // the local player's network block (+0x3584) with the native writers: a lock 002494e0 + 00249420(1) (the locked
    // object as a LAN id), the lowered aim 00249420(0), an aim point 00249490, a shot 00249530, the held weapon
    // 002493c0. Before the block is linked (the level's first player update) the writers find none and skip.
    void record_lock(std::uint32_t target, std::int32_t subtarget);
    void record_lower();
    void record_point(const std::array<float, 3>& point);
    void record_shot();
    void record_weapon(std::int32_t item);
    // 0024a850 -> 00249460 (cPlayerControls_NetDisableManualAim): leaving the manual aim view in multiplayer clears
    // the record block's byte +0 (the aim-point flag 00249490 sets) without touching the lock fields +4..+0xc.
    void record_clear_point();
    // 133lt1 diagnostics (read only): a record block's byte +0 (1: the aim point 00249490 is live, 0: cleared by
    // 00249460 / 00249420). Key 0 or the local player's key reads the local player's block; another key reads that
    // remote human's received block (its +0x3584). nullopt before the block exists.
    std::optional<std::uint8_t> record_point_flag(std::uint32_t key) const;
    std::uint32_t remote_controller_runs() const { return controller_runs_; }
    // 132ln5h stage 2: how many times the local player's +0x3584 became its cNetHuman's data (001b53d0's tail).
    std::uint32_t local_block_links() const { return local_links_; }
    std::size_t remote_humans() const { return remote_keys_.size(); }
    std::uint32_t clients_left() const { return dme_->left_callbacks(); }
    static constexpr std::uint32_t kLocalPlayer = 0x1d40000, kLocalPlayerId = 1;
    std::uint32_t local_player_key() const;
    // 002c7110 / 002c7630 / 002c6690: an authored NPC already exists in both
    // level worlds. The owner mirrors its native actor fields into a network
    // block so 003fa940 sends the exact 0x50-byte NetCreateNamedCharacterMsg
    // to a newly loaded client. The receiver resolves the stable object ID in
    // the live world; it never constructs a substitute actor.
    struct NamedNpcSnapshot {
        std::uint32_t key{};
        std::array<std::array<float, 4>, 4> matrix{};
        std::array<std::int16_t, 6> weapons{-1, -1, -1, -1, -1, -1};
        std::string name;
        std::int16_t health{};
        std::uint8_t state{};
        bool active{};
    };
    struct RemoteNamedNpc {
        std::uint16_t origin{};
        std::uint32_t key{};
        std::array<float, 3> angles{};
        std::array<float, 3> position{};
        std::array<std::int16_t, 6> weapons{};
        std::string name;
        std::int16_t health{};
        std::uint8_t state{};
        bool active{};
    };
    using NamedNpcReceiver = std::function<bool(const RemoteNamedNpc&)>;
    void mirror_named_npc(const NamedNpcSnapshot& npc);
    void set_named_npc_receiver(NamedNpcReceiver receiver) { named_npc_receiver_ = std::move(receiver); }
    std::uint32_t named_npcs_sent() const { return named_npcs_sent_; }
    std::uint32_t named_npcs_received() const { return named_npcs_received_; }

    // 132fx5: cNetBackpackMsg's observable payload and NetMap binding. LanDme replaces only the generic message
    // table/routing layer; CombatPlay still produces and applies the native message types.
    using RemoteBackpack = LanDme::BackpackMessage;
    void bind_backpack(std::uint32_t id, std::uint16_t owner, std::uint32_t address);
    void send_backpack(std::uint8_t type, std::uint8_t owner, std::uint32_t id, std::uint32_t target,
                       std::uint16_t recipient = kLanAll, const std::vector<std::uint8_t>& contents = {});
    std::vector<RemoteBackpack> take_remote_backpacks() { return dme_->take_backpacks(); }
    // Checkpoint 134eq1: the level-start roster GuiEquipmentSetup's services on this memory. A human by its key (the
    // local player or a remote human 002c6b70 created): its network block +0x3584's status byte +0x32 (cHuman_GetStatus
    // / SetStatus 00184c80 / 00184c60: nothing without the block; the DME library replicates the owner's field) and
    // headset byte +0x33 (00184ca0).
    std::uint32_t human_by_key(std::uint32_t key) const;  // 0: none
    std::uint8_t human_status(std::uint32_t key) const;
    void set_human_status(std::uint32_t key, std::uint8_t status);
    std::uint8_t human_headset(std::uint32_t key) const;
    void write_human_headset(std::uint32_t key);          // block +0x33 = byte 0x00582b88 (002a2f00)
    // PORT-ONLY stand-in for 002ebec8 NetGetValidClientCount(&n, *0x005721a8) (the DME world's client records below
    // the library, not ported): the transport's connected clients and the local one.
    std::int32_t valid_client_count() const;
    // 002a3ba0(0x0055c770): the client map's word +0 (its size). PORT-ONLY: the map's inserts 003fb9c0 are not ported
    // (client_player_003fb8d0 stands in for its lookups), so its size is the number of clients 0..3 that lookup finds.
    std::int32_t player_map_count();
    bool level_info_pending();                            // 0042b4e0 on this memory
    void send_play_game();                                // 00429b30 cDME_SendPlayGame
    bool play_game() const { return memory_.read(0x572400, 1) != 0; }  // byte 0x00572400
    std::uint32_t play_games_received() const { return play_games_received_; }
    std::uint16_t local_client_index() const { return dme_->local_index(); }
    bool hosting() const { return transport_.is_host(); }
    std::uint16_t session_host() const { return 0; }
    std::uint32_t session_clock_ms() const { return dme_->shared_clock(); }
    std::uint32_t local_clock_ms() const { return dme_->local_clock_ms(); }
    std::uint32_t summary_updates() const { return summary_updates_; }

    // 00245560 / 002467d0 / 00245750: cNetTimerMsg is generic cNetMsg inner
    // ID 16. The authority sends the shared absolute HUD end time followed by
    // the localization key as a u16-sized NUL-terminated string. The receiver
    // schedules the same +6c4 timer without posting E_TIMER again.
    struct RemoteGlobalTimer {
        std::uint16_t origin{};
        float end{};
        std::string label_key;
        std::uint32_t timestamp{};
        std::uint32_t target_key{};
    };
    void send_global_timer(float end, std::string_view label_key);
    std::vector<RemoteGlobalTimer> take_global_timers();
    std::uint32_t global_timers_sent() const { return global_timers_sent_; }
    std::uint32_t global_timers_received() const { return global_timers_received_; }
    // 00267860 / 00269880 / 002679f0: cNetLocalTimerMsg is generic cNetMsg
    // inner ID 19. Its addressed header names the target player's NetMap key;
    // the eight-byte body is the relative float duration and localization index.
    struct RemotePersonalTimer {
        std::uint16_t origin{};
        float seconds{};
        std::int32_t localization_key{-1};
        std::uint32_t timestamp{};
        std::uint32_t target_key{};
    };
    void send_personal_timer(std::uint16_t recipient, std::uint32_t target_key, float seconds,
                             std::int32_t localization_key = -1);
    std::vector<RemotePersonalTimer> take_personal_timers();
    std::uint32_t personal_timers_sent() const { return personal_timers_sent_; }
    std::uint32_t personal_timers_received() const { return personal_timers_received_; }
    // 00186f30 / 001ba510 / 00187230: cNetUpdateStatsMsg is generic inner ID
    // 6. The NPC owner addresses the remote killer's client; the body names
    // the killer GOBJ and carries damage type/category.
    struct RemoteKillStatistics {
        std::uint16_t origin{};
        std::uint32_t timestamp{},victim_key{},killer_key{};
        std::uint8_t type{},category{};
    };
    void send_kill_statistics(std::uint32_t victim_key,std::uint32_t killer_key,
                              std::uint8_t type,std::uint8_t category);
    std::vector<RemoteKillStatistics> take_kill_statistics();
    std::uint32_t kill_statistics_sent() const { return kill_statistics_sent_; }
    std::uint32_t kill_statistics_received() const { return kill_statistics_received_; }
    // 133on2: 001992b0 / 00199500 / 001b97b0: cEquipGogglesMsg is generic
    // cNetMsg inner ID 2 (*004eee88, read from the retail savestate). Kind
    // +20 = 4: 003c9420 sends it to every peer (recipient -1, +1c = 0x40) with
    // the switching human's NetMap key in the header; the body is one byte,
    // the goggles value. The receiver runs 001992b0(object, byte != 0, 1).
    struct RemoteGoggles {
        std::uint16_t origin{};
        std::uint32_t timestamp{}, target_key{};
        std::uint8_t value{};
    };
    void send_goggles(std::uint32_t target_key, std::uint8_t value);
    std::vector<RemoteGoggles> take_goggles();
    std::uint32_t goggles_sent() const { return goggles_sent_; }
    std::uint32_t goggles_received() const { return goggles_received_; }
    // 134on3: NetLabelMsg (NetLabelMsg.c), its own DME message *0051f090 on channel 2 (InitNet 002d2850),
    // size 0x18 (Pack 002d2920: Event_PackHeader, +8 kind, +c object, +10 / +14 string-pool indices from
    // 003fdb40, +1c = 0x40). cNetObj_SendMessage 003fb020(0, msg, 0, recipient): +20 = 4 or the constructor's
    // 5, so 003fad50(type, 0x18, packet, recipient, 0x40) with recipient -1 (every peer) or a client.
    // Senders: kind 0 cObjectiveMan_SetDisplayLabel 00227a90, kind 1 nHudTargets_AddHudObjective 002750e0,
    // kind 2 the PDA quips 001c63b0 / 001c6500 / 001c6200. The receive 002d2650 (guarded by 005721d0)
    // returns 0x18; the play resolves the indices with 003fdae0 and runs the quiet / silent call.
    struct RemoteLabel {
        std::uint16_t origin{};
        std::uint8_t kind{};
        std::int32_t object{}, key0{-1}, key1{-1};
    };
    void send_label(std::uint8_t kind, std::int32_t object, std::int32_t key0, std::int32_t key1,
                    std::uint32_t recipient = 0xffffffffU);
    std::vector<RemoteLabel> take_labels();
    std::uint32_t labels_sent() const { return labels_sent_; }
    std::uint32_t labels_received() const { return labels_received_; }
    // 134on3: NetCalloutMsg (NetCalloutMsg.c), DME message *0051ee40 (InitNet 002c5270), size 0x20 (Pack
    // 002c53a0: +8 the clear byte (+24), +9 / +a the bytes +48 / +49, +c 003fdb40 of the 16-byte key +34,
    // +10 the object key +28, +14 / +18 the floats +2c / +30, +1c the slot +44). Senders:
    // cHud_SetCalloutLabel 00242e40 (clear 0) and cHud_ClearCallout 00242b40 (clear 1) with their send
    // argument in a network game, every peer (+20 = 4, +1c = 0x40). The receive 002c50e0 returns 0x20.
    struct RemoteCallout {
        std::uint16_t origin{};
        bool clear{};
        std::uint8_t byte_48{}, byte_49{};
        std::int32_t key_index{-1}, object{}, slot{};
        float value{}, colour{};
    };
    void send_callout(const RemoteCallout& callout);
    std::vector<RemoteCallout> take_callouts();
    std::uint32_t callouts_sent() const { return callouts_sent_; }
    std::uint32_t callouts_received() const { return callouts_received_; }
    // 134on3: cNetQuickChatMsg, generic cNetMsg inner ID 27 (*0052ace0 = *0052ace8 = 27, the retail savestate's
    // words), +1c = 0x40 (Pack 00323d40: the two words a, b). The header key is the object it is queued on:
    // the local player for GuiQuickChat's line (002b1350, +20 = 4: every peer), the player GOBJ for a location
    // update (002b0f00, a = -1, +20 = 8: its owner). The receive 00323ad0 reads both words.
    struct RemoteQuickChat {
        std::uint16_t origin{};
        std::uint32_t key{};
        std::int32_t a{}, b{};
    };
    void send_quick_chat(std::uint32_t key, std::int32_t a, std::int32_t b, std::uint32_t recipient = 0xffffffffU);
    std::vector<RemoteQuickChat> take_quick_chats();
    std::uint32_t quick_chats_sent() const { return quick_chats_sent_; }
    std::uint32_t quick_chats_received() const { return quick_chats_received_; }
    struct RemotePlayerSound {
        std::uint16_t origin = 0;
        bool stop = false;
        std::int32_t type = 0, priority = 0;
        std::uint32_t owner_key = 0;
        std::uint8_t flags = 0;
        std::string name;
    };
    void send_player_sound(std::uint16_t recipient, bool stop, std::int32_t type, std::string_view name,
                           std::int32_t priority = 0, std::uint32_t owner_key = 0, std::uint8_t flags = 0);
    std::vector<RemotePlayerSound> take_player_sounds();
    std::uint32_t player_sounds_sent() const { return player_sounds_sent_; }
    std::uint32_t player_sounds_received() const { return player_sounds_received_; }

    // Objective.c's category-0xF6 NetObj registrations start at F60001 each level;
    // objective script IDs only index this port-side binding and never appear on the wire.
    struct RemoteObjectiveOutcome {
        std::uint32_t local_script_id = 0;
        bool success = false;
        std::uint32_t network_key = 0;
        std::uint32_t timestamp = 0;
        std::int32_t stage = -1;
        std::int32_t value = -1;
        std::array<std::uint8_t, 16> name{};
    };
    void register_objective(std::uint32_t local_script_id);
    void unregister_objective(std::uint32_t local_script_id);
    std::uint32_t objective_network_key(std::uint32_t local_script_id) const;
    void send_objective_outcome(std::uint32_t local_script_id, bool success);
    std::vector<RemoteObjectiveOutcome> take_objective_outcomes();
    std::uint32_t objective_messages_sent() const { return objective_sent_; }
    std::uint32_t objective_messages_received() const { return objective_received_; }
    // 00229000 / 00229100 send the separate cActivateMsg / cDeactivateMsg
    // after changing the local active byte. Their receivers call objective
    // virtuals +0x18 / +0x1c without re-sending.
    struct RemoteObjectiveActive {
        std::uint32_t local_script_id = 0;
        bool active = false;
        std::uint32_t network_key = 0;
    };
    void send_objective_active(std::uint32_t local_script_id, bool active);
    bool objective_network_active(std::uint32_t local_script_id) const;
    std::vector<RemoteObjectiveActive> take_objective_actives();
    std::uint32_t objective_actives_sent() const { return objective_active_sent_; }
    std::uint32_t objective_actives_received() const { return objective_active_received_; }

    // 0012eee0 / 002823e0: cNetAgentUpdateMsg's inner stream rides in the
    // generic cNetMsg category-2 envelope. The caller owns the AgentData
    // record, its AWARDS state, and the inner codec. A forwarded packet's
    // declared client can differ from its immediate transport origin.
    struct RemoteAgentUpdate {
        std::uint16_t origin{};
        std::uint8_t client{};
        std::uint32_t timestamp{};
        std::uint32_t target_key{};
        std::vector<std::uint8_t> inner;
    };
    void send_agent_update(std::span<const std::uint8_t> inner);
    std::vector<RemoteAgentUpdate> take_agent_updates();
    std::uint32_t agent_updates_sent() const { return agent_updates_sent_; }
    std::uint32_t agent_updates_received() const { return agent_updates_received_; }

    // 0012fa60 / 0020e4c0: cEndLevelMsg is a direct category-2 DME packet.
    // The native client message has flags 1 and is delivered locally only;
    // the host's flags 5 broadcast it. Gameplay applies a received event
    // after checking its own GameState (0020e350 permits 0x0f or 0x12).
    struct RemoteEndLevel {
        std::uint16_t origin{};
        std::int8_t kind{};
        std::uint32_t quit{};
        std::uint32_t timestamp{};
        std::uint32_t target_key{};
    };
    void send_end_level(std::uint8_t kind, bool quit);
    std::vector<RemoteEndLevel> take_end_levels();
    std::uint32_t end_levels_sent() const { return end_levels_sent_; }
    std::uint32_t end_levels_received() const { return end_levels_received_; }

    // 133lt4 (review2 W1-W5, research/NET_INLEVEL.md "World message routing"): the world messages QueueMessage 003c9420 routes to the
    // session host or to an object's owner. Each is the matched Pack's packet (Event_PackHeader 003c9ca0: +0 the
    // clock << 2 | transport (0), +4 the target NetObj key) sent with 003fad50 under the registration's type; the
    // receives are the registered callbacks (net_registrations.inl), queued for the play while 005721d0 is set.
    // The level objects' NetObj keys are their population IDs (the +0x10 their constructors give cNetObj_AddSystem:
    // the hotbox 00172ae0, the GameGOBJ 0021add0), the players' their LAN keys.
    //  - cHotboxMsg (00170160, 0x18 bytes, type 0x004ee6a0): +8 the action (+24), +c the code (+28), +10 the value
    //    (+2c), +14 the occupant's +c key (+30). recipient: nullopt = the session host (003fb020's kind-2 branch,
    //    002ebba0) for a joiner's box event; a player's owner (kind 8) for the host's event to a remote occupant.
    //    Receive 00170200 (hotbox_7.c): the box or player of +4 and the occupant of +14.
    struct RemoteHotbox {
        std::uint16_t origin{};
        std::uint32_t target_key{}, who_key{}, code{}, value{};
        std::uint8_t action{};
    };
    // flags: the message's +1c (00170660's enter / exit 0x40; 001714c0's action 3 and 001726b0's actor events 0).
    void send_hotbox_message(std::uint32_t target_key, std::uint8_t action, std::uint32_t code, std::uint32_t value,
                             std::uint32_t who_key, std::optional<std::uint16_t> recipient, std::uint32_t flags);
    std::vector<RemoteHotbox> take_remote_hotboxes();
    std::uint32_t hotbox_messages_sent() const { return hotbox_sent_; }
    std::uint32_t hotbox_messages_received() const { return hotbox_received_; }
    //  - cAddCheckpointMsg (0022e680, 76 bytes, type 0x004f7948): +8 the node's +c key (+24's), +c the label
    //    (strncpy 0x40). MSGGAME_ADD_CHECKPOINT keeps the cMessage constructor's event byte 5 (003c9e30): queued
    //    locally and, for a network player, sent to every client (003fb020's flags & 0x15, recipient -1). Receive
    //    0022e710 (Checkpoint_10.c): 0022e800 rebuilds it and queues it to the NetMap object of +4.
    struct RemoteCheckpoint {
        std::uint16_t origin{};
        std::uint32_t target_key{}, node_key{};
        std::string label;
    };
    void send_checkpoint(std::uint32_t target_key, std::uint32_t node_key, std::string_view label);
    std::vector<RemoteCheckpoint> take_remote_checkpoints();
    std::uint32_t checkpoints_sent() const { return checkpoints_sent_; }
    std::uint32_t checkpoints_received() const { return checkpoints_received_; }
    //  - cNetLockElevatorMsg (00211680, 16 bytes, type 0x004f54c0): +8 the floor (+24), byte +c the lock (+28);
    //    002155f0 / 00215530 send it (event byte 4: every client) unless silent. Receive 002115e0 (GameGOBJ_9.c):
    //    LockFloor / UnlockFloor(the NetObj of +4, floor, 1).
    struct RemoteLiftLock {
        std::uint16_t origin{};
        std::uint32_t lift_key{};
        std::int32_t floor{};
        bool locked{};
    };
    void send_lift_lock(std::uint32_t lift_key, std::int32_t floor, bool locked);
    std::vector<RemoteLiftLock> take_remote_lift_locks();
    std::uint32_t lift_locks_sent() const { return lift_locks_sent_; }
    std::uint32_t lift_locks_received() const { return lift_locks_received_; }
    //  - cGOBJTriggerEventMsg (002118d0, 24 bytes, type 0x004f5470; the Pack sets +1c = 0x40): u16 +8 the action
    //    (+24), +c (+28), +10 the source (+30), +14 the other (+2c). Receive 00211700 (GameGOBJ_9.c): the message for
    //    the NetObj of +4 when the +2c object is absent or its IsDeliverable (vt+30) accepts it.
    //    recipient: nullopt = the session host (a joiner's kind 2), kLanAll = every client (kind 4 / 5).
    struct RemoteTrigger {
        std::uint16_t origin{};
        std::uint32_t target_key{}, word_28{}, source_30{}, other_2c{};
        std::uint16_t action{};
    };
    void send_trigger(std::uint32_t target_key, std::uint16_t action, std::uint32_t word_28, std::uint32_t other_2c,
                      std::uint32_t source_30, std::optional<std::uint16_t> recipient);
    std::vector<RemoteTrigger> take_remote_triggers();
    std::uint32_t triggers_sent() const { return triggers_sent_; }
    std::uint32_t triggers_received() const { return triggers_received_; }
    //  - 133lt4 (batch2): cNotifyCheckpointMsg (0022e490, 8 bytes, type 0x004f7950; receive 0022e4f0) and
    //    cRespawnMsg (0022e2c0, 12 bytes, type 0x004f7958; receive 0022e320: +8 the RespawnMsg's +24). Both keep the
    //    cMessage constructor's event byte 5: every client. 0022e2c0's body writes nothing at +8 (the original's
    //    buffer byte there is whatever it held); the port sends the message's +24 (0 from 0022e1d0).
    struct RemotePlayerMessage {
        std::uint16_t origin{};
        bool respawn{};               // false: NotifyCheckpoint
        std::uint32_t target_key{};
        std::uint32_t value{};        // respawn: +8
    };
    void send_notify_checkpoint(std::uint32_t target_key);
    void send_respawn(std::uint32_t target_key, std::uint32_t value);
    std::vector<RemotePlayerMessage> take_remote_player_messages();
    std::uint32_t player_messages_sent() const { return player_messages_sent_; }
    std::uint32_t player_messages_received() const { return player_messages_received_; }

    // 132ln5h11 (research/NET_INLEVEL.md 4.21): the level's generators (cGenerator 004db680) in the network game.
    // register_generator gives a generator (its population id: the block's +0xc and +0x10, the key everywhere) a
    // PORT-ONLY block in this memory (vtable: +0x28 the remote enable 00220e60, +0x2c the remote disable 00220d00,
    // whose stand-ins hand the play the receiver's call; +0xc0 the position at +0x30), the object the NetMap insert
    // 003fb650 of its network constructor tail gets and the receives 003ff8b0 / 003ff5d0 find (003fbb10).
    // generator_net_host() answers the generators' network calls (npc_generator.hpp GeneratorNetHost) with the
    // native GenInfo entry API (0043b190, 0043aa40, 00439c70, 0043ab20, 0043aba0, 004398e0, 00439d00, 0043a030,
    // 0043a420 with the key's pointer, $s2 1), 002ebba0, 003fb650, 0042b1a0(1), 003ce850 (the nearest player's +c;
    // PORT-ONLY: the local player's is its key), 003fb8d0 (a PORT-ONLY stand-in, the header) and the cEnableMsg /
    // cDisableMsg sends (003c9e30, 003ff930 / 003ff650, 003c9d60). generator_prelogic_host() answers the prelogic's
    // NPC info pass (00436c00 / 00436ba0) and, 132ln5h12, its per-actor calls on the NPC blocks (register_npc below).
    void register_generator(std::uint32_t id, const std::array<float, 3>& position);
    void clear_generators() {
        generator_blocks_.clear();
        next_generator_ = kGenerators;
    }
    GeneratorNetHost generator_net_host();
    // The NPC key (c) of one of the play's actors, for the hosts below (the play's spawned actors).
    using ActorKey = std::function<std::uint32_t(std::uint32_t actor)>;
    // 132ln5h12: `key` maps the play's actors to their NPC keys for the per-actor calls (an empty key: none hosted).
    GeneratorNetPrelogicHost generator_prelogic_host(ActorKey key = {});
    // A remote client's cEnableMsg / cDisableMsg for a registered generator: the generator's virtual +0x28 / +0x2c
    // (00220e60 / 00220d00) with the client the message carries (+0x24); the play applies it
    // (generator_remote_enable / generator_remote_disable). Without a receiver the message throws.
    using GeneratorReceiver = std::function<void(std::uint32_t id, std::int32_t client, bool enable)>;
    void set_generator_receiver(GeneratorReceiver receiver) { generator_receiver_ = std::move(receiver); }
    std::uint32_t generator_messages_sent() const { return generator_sent_; }
    std::uint32_t generator_messages_received() const { return generator_received_; }
    // 134lt6: the level's PLR: particle placements (cGameParticle, vtable 004daa10) in the network game. With 005721c8
    // set, 001c59d0 clears +0x14 on a joiner and inserts the object into the NetMap (003fb650) with the local client
    // (0x00494178 by 002ebaf8) on the host, the host's client (002ebba0) on a joiner. register_particle gives the
    // placement a PORT-ONLY block (kParticles up, 0x30 bytes: vtable kParticleVtable whose +0x28 / +0x2c stand in for
    // NetEnable 001c5650 / NetDisable 001c5660 and hand the play the receiver's call; +0xc / +0x10 the key; +0x14 1 on
    // the host, 0 on a joiner) and runs that insert. send_particle_toggle is the network part of Enable 001c5710 /
    // Disable 001c5680 (event_toggle_send.h): 003c9e30(msg, 0055d4b8 / 0055d498), the vtable 004e0880 / 004e0860,
    // +0x24 = -1, +0x1c = 0x40 (byte +0x20 stays the constructor's 5), 003ff930(msg, key) / 003ff650(msg, key, -1),
    // the vtable again and 003c9d60(msg, 0). Without a receiver a remote message throws naming 001c5650 / 001c5660.
    void register_particle(std::uint32_t key);
    void clear_particles() {
        particle_blocks_.clear();
        next_particle_ = kParticles;
    }
    bool particle_registered(std::uint32_t key) const { return particle_blocks_.contains(key); }
    void send_particle_toggle(std::uint32_t key, bool on);
    using ParticleReceiver = std::function<void(std::uint32_t key, bool on)>;
    void set_particle_receiver(ParticleReceiver receiver) { particle_receiver_ = std::move(receiver); }
    std::uint32_t particle_messages_sent() const { return particle_sent_; }
    std::uint32_t particle_messages_received() const { return particle_received_; }

    // 132ln5h12 (research/NET_INLEVEL.md 4.22): a generator's spawned NPCs in the network game. register_npc gives
    // the NPC (its key c: the actor's +c, the population id with the owning client + 1 in the top byte) a PORT-ONLY
    // character block in this memory (0x35a0-byte cHuman layout: vtable 0x004da690, +c / +10 the key, +0x2c..+0x2e
    // clear, byte +0x49 2, +0x58 the AI controller 0x004d9380 whose +0x30 is the block, +0x3584 / +0x3598 clear),
    // the object 0040c540 answers for the key, so the native NetMap / NPC info paths (001856d0, 003fb340, 003fb4b0,
    // 0021f0dc's calls) run on it. The play's figure is the actor.
    void register_npc(std::uint32_t c, std::uint32_t generator);
    void forget_npc(std::uint32_t c);
    // 0021e740..0021e7bc on the spawning console: the actor's virtual +0xfc (001856d0) and its NPC info entry
    // (00436c00, 00435f30(obj, &+c, the local slot), 00436ba0(obj, -1)).
    void npc_network_register(std::uint32_t c);
    // 134lt6 (tests): 003fb650(c, owner, the NPC block), the NetMap insert a received spawn makes for another console's NPC.
    void npc_netmap_insert(std::uint32_t c, std::int32_t owner);
    // 0021e968..0021ea70: the spawn message (003c9e30 type 0x0051f058, vtable 0x004de240; +0x30 the position, +0x40
    // 001bd590 of the actor's matrix rows, +0x50 / +0x54 its +c / +10, +0x58 the generator's health, +0x5c the draw,
    // +0x60 the node, +0x1c 0x40, byte +0x20 4) to the generator: 003c9420(msg, gen, 0) (clone 002d0ee0, 003fb020,
    // the serialiser 002d1100), 003c9d60.
    struct NpcSpawn {
        std::uint32_t generator = 0, c = 0, health = 0, random = 0;
        std::int32_t node = -1;
        std::array<std::array<float, 4>, 3> rows{};
        std::array<float, 3> position{};
    };
    void send_npc_spawn(const NpcSpawn& spawn);
    // The other console's spawn message: its receive 002d11d0 (002d1290, 003fbb10, 003c9350) delivers it to the
    // generator through the message queue 003ca0e0, whose PORT-ONLY stand-in queues it here; the play runs the
    // generator's handler 0021e0c0 (generator_spawn_receive) with generator_spawn_receive_host().
    struct RemoteSpawn {
        std::uint32_t generator = 0;
        GeneratorSpawnMessage message;
    };
    std::vector<RemoteSpawn> take_remote_spawns();
    std::uint32_t spawns_sent() const { return spawns_sent_; }
    std::uint32_t spawns_received() const { return spawns_received_; }
    // 0021e0c0's calls on the NPC block of the actor (set_key_185940, netmap_insert_3fb650, npc_info_merge_434600,
    // attach_185970, mark_remote; remote_controller the PORT-ONLY stand-in for 002d6030); create, register_actor, activate_20,
    // place_msg and actor_id are the play's.
    GeneratorSpawnReceiveHost generator_spawn_receive_host(ActorKey key);
    // The control message 0x00582730's controller swaps (its receiver 001b3d98..001b4008 is ported on the block,
    // 132ln5h13; only an actual swap is queued): 2, the owner is another console, the controller became the remote
    // NPC controller 0x004de2a0 (the play holds its AI and steers it from the owner's block, npc_remote_states); 1,
    // the AI controller 0x004d9380 again (the play resumes its AI). puppet: a received NPC given the remote NPC
    // controller 002d6030 by 0021e380..0021e408 (kind 2).
    struct NpcControl {
        std::uint32_t c = 0;
        std::uint8_t kind = 0;
        bool puppet = false;
    };
    std::vector<NpcControl> take_npc_controls();
    // 132ln5h13: 001b53d0's network tail on the NPC block (as player_network_001b53d0): with +0x3584 clear and
    // *0x005721d0 the cNetHuman 002d1ff0(&+c, +10) and its NetMap entry 003fb650(+10, slot, block), 003fb4b0(+10,
    // slot), the NPC info pass (00436c00, 004341f0(&+c), +0x14 = 1, 002d1d80(+0x3584), 00436ba0), then +0x3268 = 1.
    // The play calls it from the NPC's activation in a network game (the actor's +5c / +20).
    void npc_activate_001b53d0(std::uint32_t c);
    // 132ln5h14: the network part of the NPC's deactivation on its block. 001b5190 (+60, 001b51f8..001b5274): in a
    // network game the NPC info pass (00436c00, 00433ce0(&+c): the local interest cleared, 00436ba0) and, with
    // +0x3584, 002d2110(+0x3584) unless the actor is dead (virtual +108 == 3) with cvt.w.s(+40) <= 0 (the play
    // gives its life state and health: the block does not mirror them). 001b4970 (+24, 001b49a4..001b4af8): the NPC
    // info pass alone, not gated on the network game. Frames 0x50 / 0x40.
    void npc_deactivate_001b5190(std::uint32_t c, std::uint8_t life_108, float health_40);
    void npc_deactivate_001b4970(std::uint32_t c);
    // 132ln5h13: 0014bff0's network game part on the NPC's block (NpcAiHost::network_contact / network_speed /
    // network_heading): the NPC info pass with 00433ec0(&+c, slot) and, when it differs from +158, 00433f20(&+c,
    // +158); with +0x14 and +0x3584 the cNetHuman's +44..+4c = the world position (virtual +e8 row 3); the speed
    // (+3c) and the navigation request's yaw (+40, written by the locomotion message's receiver 001a99a4) likewise.
    void npc_contact_0014c510(std::uint32_t c, std::uint8_t contact_158, const std::array<float, 3>& position);
    void npc_speed_0014c6e0(std::uint32_t c, float speed);
    void npc_heading_001a99a4(std::uint32_t c, float yaw);
    // 132ln5h13: what the remote NPC controller 002d6030 (+1e0 / +1e4, linked from +0x3584) and 002d5160 read on
    // the NPC's block. link: +1e0 and +1e4 set (the cNetHuman's +0x20 aiming, +0x21 fire-ready, +8 the target key,
    // +c the subtarget, +10..+18 the aim point, +1c/+1e the 002c4f20 max/current counter); motion: +0x3584 set
    // (+3c speed, +40 heading, +44..+4c position).
    struct NpcNetLink {
        bool aiming_20 = false, firing_21 = false;
        bool point_valid_0 = false;
        std::uint32_t target_8 = 0;
        std::int8_t subtarget_c = -1;
        std::array<float, 3> point_10{};
    };
    struct NpcNetMotion {
        float speed_3c = 0, heading_40 = 0;
        std::array<float, 3> position_44{};
    };
    std::optional<NpcNetLink> npc_net_link(std::uint32_t c) const;
    bool npc_fire_gate_002c4f20(std::uint32_t c);
    std::optional<NpcNetMotion> npc_net_motion(std::uint32_t c) const;
    // 002d5160's write of the cNetHuman's +44..+4c (the node teleport of +214 bit 2).
    void npc_net_write_position(std::uint32_t c, const std::array<float, 3>& position);
    // 002d6030's copies: the AI controller's +0x150 (0021e548: the receiving generator's +c; 0 otherwise).
    std::uint32_t npc_controller_150(std::uint32_t c) const;
    // 132ln5h13: 0040c540(&word) non-null with +4 == 0xbebaafde, for the remote controller's +150 tests (002d4554,
    // 00157c3c): a generator the port registered answers at once; other words are looked up in the NetMap's memory.
    bool npc_object_live(std::uint32_t word);
    // 132ln5h13: 001b4f40's controller part (the actor's virtual +20 for an actor that is not present): with a
    // controller that is not the player's (+54) and whose +88 does not answer 2, new 0x220 and 002d6030(it, +0x3584,
    // actor), 001ae450(actor, 4, 0, it) (the block as install_remote_npc_controller). True: it was installed, so the
    // play's NpcAiController takes its remote mode (set_remote). The rest of 001b4f40 is the play's activation.
    bool npc_remote_controller_001b4f40(std::uint32_t c);
    // 132ln5h13: the controller swaps (NpcControl) go to this handler as they happen (the original's synchronous
    // 003c9420(msg, actor, 1) delivery and 0021e380's construction); unset, they are queued for take_npc_controls.
    void set_npc_control_handler(std::function<void(const NpcControl&)> handler) { npc_control_handler_ = std::move(handler); }
    // 132ln5h13: the "Generator Message" (003c9e30 type 0x004a4a68, vtable 0x004db600; +0x24 the NPC's block, byte
    // +0x28 = 1) the AI's Deactivate (00157c44..00157c9c) and the remote NPC controller's (002d4554..) send with
    // 003c9420(msg, 0040c540(&+150), 0) when the word of +150 is a live object (a generator: other objects throw):
    // the clone 00222410, 003fb020 (the serialiser 0021d370; the other console's receive 0021d6c0 delivers its
    // "GeneratorBase Message" to the generator, whose receiver 00221610 handles none of its keys) and the queue
    // 003ca0e0, whose PORT-ONLY stand-in queues the generator's receiver 00221610 for the play here: +0x28 == 1, the
    // GenInfo 0043b190 (take_generator_releases).
    void send_generator_release(std::uint32_t c, std::uint32_t word_150);
    // 134lt6: 002d4460's release for a remote controller without +150 on the console whose virtual +44 answers: the NPC
    // info object (00436c00), GetOwner 00434440(&c) (its answer unused), 0042d910(the NPC block) when `terrain`, 00436ba0.
    // 134lt7: 0042d910 is native (remove_from_terrain_activation_0042d910): a hit frees the reference and erases the node.
    void npc_remote_release_002d4460(std::uint32_t c, bool terrain);
    // 134lt6: cNetObjectMgr_TerrainActivation 0042d790 (NetObjectMgr_3.c) for an NPC: nothing when its virtual +7c answers
    // (cHuman: the generator-spawned byte +3598); otherwise the TransferReference insert into 0x00582700
    // (terrain_activation_insert_0042d790) and, unless the controller's +5c answers (cNetNPC_IsNetNPC 002d2b20: 1), the
    // notice 0x00582730 (not ported: it throws). PORT-ONLY: the block's +2d mirrors the actor's +60(1) that came first.
    // Each network frame 0042eb20 then asks an inactive entry's controller +88 (2), +94 (cNetNPC_IsNodeInActiveTerrain
    // 002d2ab0: the play's NpcTerrainHosts::node_active) and +44, and calls the object's +20 (cHuman_NetActivate 001b4f40,
    // npc_net_activate_001b4f40) before releasing the reference.
    // 134lt7: `lost_contact` false is the cNetTerrainActivation(0) receive (00431d60), which has no +60(1) before it.
    void npc_terrain_activation_0042d790(std::uint32_t c, bool generator_created, bool lost_contact = true);
    struct NpcTerrainHosts {
        std::function<bool(std::uint32_t c)> node_active;   // 002d2ab0: the remote controller's node +1e8 in active terrain
        // 134lt7: the play's part of 001b4f40 (PORT-ONLY boundary: 001b5560, 00392900, 001af460 / 001af190, 0022c650: the
        // actor's activation); `remote_built`: 001b4f40 installed a cNetNPC (the play's AI takes its remote mode).
        std::function<void(std::uint32_t c, bool remote_built)> net_activate;
    };
    // 134lt7: cHuman_NetActivate 001b4f40 (human_119.c) on an NPC block (its virtual +0x20, called by 0042eb20): Ready
    // (+2d && +2c && !+2e) returns; the controller part (npc_remote_controller_001b4f40); the play's activation
    // (NpcTerrainHosts::net_activate); 003fb650(+10, 003fb340(+c) or the local index, the block); +3268 = 1; the NPC
    // info object's SetActive 004341f0(&+c).
    void npc_net_activate_001b4f40(std::uint32_t block);
    // 134lt7: cNPC_DeactivateGlobal 00157510 (npc_39.c), its network part after the play's SetEnabled(-1, 0) and +60(1):
    // 0042d910(body); then, unless the body's +7c answers (cHuman: the generator-spawned +3598), for every other
    // loaded client (0x0055c780 == 2): cDeactivateMsg (003c9e30 0x00572198, vtable 004e0e80, 00429200 = 003fb020(+c,
    // msg, -1, i)) and cNetTerrainActivation (0x00582760, vtable 004e0f00, +24 -1, +28 1: the receiver's 0042d910).
    void npc_deactivate_global_00157510(std::uint32_t c, bool generator_created);
    std::uint32_t npc_deactivate_globals_sent() const { return npc_deactivate_globals_sent_; }
    // 134lt7: cDeactivateMsg's receive 00429190 on an NPC block: its +0x24, cHuman_NetDeactivate 001b4970: the NPC info
    // object's Deactivate (npc_deactivate_001b4970) here, and the key queued for the play's part (cMan_UnDead,
    // cTargetManager_RemoveGobj, cSKM_GOBJ_NetDeactivate, the collision routine: the actor's deactivation).
    void npc_net_deactivate_001b4970(std::uint32_t block);
    std::vector<std::uint32_t> take_remote_npc_deactivates();
    std::uint32_t npc_deactivates_received() const { return npc_deactivates_received_; }
    std::uint32_t terrain_activations_received() const { return terrain_activations_received_; }
    void set_npc_terrain_hosts(NpcTerrainHosts hosts) { npc_terrain_hosts_ = std::move(hosts); }
    std::uint32_t npc_terrain_references() const;       // the entries of 0x00582700
    std::uint32_t npc_net_activations() const { return npc_net_activations_; }
    std::uint32_t npc_block_address(std::uint32_t c) const { return npc_block(c); }   // tests
    // 134lt6: the cNetNPCMsgs channel. send_npc_message is 002d7290(target, type, len, a, b, c, 0) for the NPC of key c:
    // 003c9e30(msg, 0x0051f0a8), vtable 0x004de4d0, +2c type, +1c 0x40, +30 / +34 / +38 a / b / c, +20 len (8: to the
    // object's owner), 003c9420(msg, the NPC block, 0), 003c9d60. The receive 002d7190 (registered by 002d7020 as
    // *0x0051f0a0) finds the object (003fbb10) and queues {key, type, a, b, c}; the play runs DoNPCMethod 002d63e0 on it.
    void send_npc_message(std::uint32_t c, std::uint8_t type, std::uint8_t len, std::uint32_t a, std::uint32_t b,
                          std::uint32_t cc);
    struct RemoteNpcMessage {
        std::uint32_t key = 0;
        std::uint8_t type = 0;
        std::uint32_t a = 0, b = 0, c = 0;
    };
    std::vector<RemoteNpcMessage> take_remote_npc_messages();
    std::uint32_t npc_messages_sent() const { return npc_messages_sent_; }
    std::uint32_t npc_messages_received() const { return npc_messages_received_; }    std::uint32_t npc_remote_releases() const { return npc_remote_releases_; }
    struct GeneratorRelease {
        std::uint32_t generator = 0, c = 0;
    };
    std::vector<GeneratorRelease> take_generator_releases();
    std::uint32_t generator_releases_sent() const { return generator_releases_sent_; }
    std::uint32_t generator_releases_received() const { return generator_releases_received_; }
    // 132ln5h14 (research/NET_INLEVEL.md 4.24): the despawn message (003c9e30 type 0x0051f068, vtable 0x004de260;
    // +0x2c / +0x30 the NPC's +c / +10, +0x1c 0x40, byte +0x20 4) the generator's owner sends its generator when
    // its prelogic despawns a spawned NPC (0021f6c8..0021f7e0, 0021fbd8..): 003c9420(msg, gen, 0) (the clone
    // 002d15c0, 003fb020, the serialiser 002d1790), 003c9d60. The other console's receive 002d1810 delivers it to
    // the generator through the queue 003ca0e0, whose PORT-ONLY stand-in queues the generator's receiver 00221610
    // (002216c8, generator_despawn_receive) for the play here.
    void send_npc_despawn(std::uint32_t c);
    struct RemoteDespawn {
        std::uint32_t generator = 0, c = 0, key_30 = 0;
    };
    std::vector<RemoteDespawn> take_remote_despawns();
    std::uint32_t despawns_sent() const { return despawns_sent_; }
    std::uint32_t despawns_received() const { return despawns_received_; }
    // 132ln5h14b (research/NET_INLEVEL.md 4.25): 001b1710's network section (human_network_section_001b1710) on a
    // spawned NPC's block, once per frame after net_frame() (PORT-ONLY caller: the game's per-frame character
    // traversal is the play's). The play's view of the NPC goes in first: the block's +0x40 (the health), +0x33a5,
    // its inventory +0x3524 (PORT-ONLY: six slots and +0x84 in the block, the held item the play's) and, on the
    // owner (+0x14 set), the animation fields mirrored into the cNetHuman's +0x50 / +0x51 as the local player's
    // (set_local_animation). The section then runs natively: the owner writes the cNetHuman's +0x38 / +0x52 / +0x35
    // and the held item (001b2d20: counters +6), stamps +0x54 (0042b1a0) and claims the object (the AI controller's
    // +0x58 answers 1: 002d1d80 while +0x28 is clear); a non-owner (+0x14 clear) takes the owner's health
    // (003cc990(npc, hp, 1); with the remote controller, whose +0x5c answers 1, only while both are above 0), the
    // killed byte, the held item (001439b0) and the animation fields with the action 0x43 (the physical's +0x12),
    // and releases a claimed object (002d2110). NpcSectionOut has what the play applies to the NPC's actor.
    struct NpcSectionIn {
        float health_40 = 0;
        bool killed_33a5 = false;
        std::int32_t held_item = -1;
        std::uint32_t flags_3248 = 0, current_3204 = 0;
        bool turn_correction_31b3 = false, accumulate_31b4 = false, byte_31b5 = false;
        std::uint8_t physical_12 = 1;
    };
    struct NpcSectionOut {
        bool owner = false;                       // +0x14 after the section
        bool linked = false;                      // a cNetHuman (+0x3584)
        std::optional<std::int32_t> health;       // 003cc990(npc, hp, 1)
        bool killed_33a5 = false;
        std::optional<std::int32_t> weapon;       // 001439b0(inventory, item) from 001b2d20 (-1 holsters)
        std::optional<std::uint8_t> physical_12;  // the action 0x43's byte
        std::uint32_t flags_3248 = 0;
        bool turn_correction_31b3 = false, accumulate_31b4 = false, byte_31b5 = false;
        bool claimed = false, released = false;   // the cNetHuman's +0x28 set (002d1d80) / cleared (002d2110)
    };
    NpcSectionOut npc_section_001b1710(std::uint32_t c, const NpcSectionIn& in);
    bool has_npc(std::uint32_t c) const { return npc_blocks_.contains(c); }
    // 132ln5h14c (research/NET_INLEVEL.md 4.26): 003cc990(human, hp, 0) on a spawned NPC's block (c): +0x40 = hp
    // and, in a network game, the health message (vtable 0x004e0ee0, type 0x00582750; +0x28 the block's +c, +0x2c
    // hp, byte +0x20 4) through 003c9420(msg, human, 0) (the clone 00431960, 003fb020 to all, the serialiser
    // 00431bc0). The other consoles' receive 00431a70 runs 003cc990(0040c540(key), hp, 1): the received changes
    // (outside 001b1710's section) are kept for the play (take_remote_healths). PORT-ONLY: the local player's
    // 003cc990(.., 0) (00186c40's refill) is not sent (its block's +c is the port's id, not its LAN key).
    void set_health_003cc990(std::uint32_t c, std::int32_t hp);
    struct RemoteHealth {
        std::uint32_t key = 0;     // the object's +c (an NPC block's, the local player's LAN key or a remote human's)
        bool local_player = false;
        std::int32_t hp = 0;
    };
    std::vector<RemoteHealth> take_remote_healths();
    std::uint32_t healths_sent() const { return healths_sent_; }
    std::uint32_t healths_received() const { return healths_received_; }
    // 132ln5h14c (research/NET_INLEVEL.md 4.27): the damage message MSGLIB_DAMAGE_REQUEST (type 0x005716f0, vtable
    // 0x004e08a0) a network game's 003ccb00 (+20 2) and the replica 00187b58 (+20 8) send through 003c9420(msg,
    // actor, 0): the clone 00407f40, the serialiser 004083d0 (the source's key, +2e, +2d, +2c, +24) to the host
    // (+20 2 from a client) or the actor's owner (+20 8); the receive 004081e0 runs the damage virtual +100 of the
    // object of the key with (source, category, type, 0, 1, 0, amount) (a notice, +2e 1, is queued to it instead).
    // True: 003c9420 queued the message on this console too (the play's pump takes it).
    struct DamageSend {
        std::uint32_t victim_key = 0, source_key = 0;   // the LAN keys (the source's: 0 when it has none)
        std::uint32_t word_1c = 0;
        std::uint8_t flags_20 = 0, type_2c = 0, category_2d = 0, byte_2e = 0;
        std::int32_t value = 0;
    };
    bool send_damage_message(const DamageSend& d);
    // The received damage messages' +100 calls (PORT-ONLY: the play's actor of the key takes the damage).
    struct RemoteDamage {
        std::uint32_t victim_key = 0, source_key = 0;
        bool local_player = false;
        std::uint8_t category = 0, type = 0;
        float amount = 0;
    };
    std::vector<RemoteDamage> take_remote_damages();
    std::uint32_t damages_sent() const { return damages_sent_; }
    std::uint32_t damages_received() const { return damages_received_; }
    std::uint32_t damage_notices_dropped() const { return damage_notices_dropped_; }
    // 132ln5h14c (research/NET_INLEVEL.md 4.28): the kill's death action (00186e04..00186e58: action 0x42 kind 2, the
    // death code +2c, +1c 0x40, flags 5) sent with 003c9420(msg, actor, 1) to every console (+20 bit 4: 003fb020 of
    // the actor's key); clone 003e7370, serialiser 003e6270 (the action header, cvt.s.w(code)); the receive 003e4f70
    // delivers it (003c9350, transport 0) to the object of the key: the play's actor runs 0018a950 a frame later.
    // PORT-ONLY: the message is sent with flags 4 (the play already ran the death action here, bit 1's delivery).
    void send_death_action(std::uint32_t key, std::int32_t code);
    struct RemoteDeath {
        std::uint32_t key = 0;
        bool local_player = false;
        std::int32_t code = 0;
    };
    std::vector<RemoteDeath> take_remote_deaths();
    std::uint32_t deaths_sent() const { return deaths_sent_; }
    std::uint32_t deaths_received() const { return deaths_received_; }
    bool host_console() const { return memory_.read(0x5721c0, 1) != 0; }   // 005721c0
    std::uint32_t npc_sections_run() const { return npc_sections_run_; }

    void advance_timer(double milliseconds);
    bool thread_alive() const { return thread_ != nullptr && !thread_->exited(); }
    std::uint32_t thread_runs() const { return thread_runs_; }
    const std::vector<std::string>& log() const { return log_; }

    static constexpr std::uint32_t kDmeThreadId = 2;      // PORT-ONLY: the id CreateThread returns
    // PORT-ONLY: the $sp the native functions get. The main thread's calls start at kStack; the DME thread's
    // at kThreadStack (its stack is 0x00572540..0x00582540 in the original); each nested call goes 0x200 lower.
    static constexpr std::uint32_t kStack = 0x1f00000;
    static constexpr std::uint32_t kThreadStack = 0x582500;

private:
    std::uint32_t kernel(std::uint32_t address, const std::vector<std::uint32_t>& a);
    void run_thread();

    LevelLoadMemory memory_;
    LanTransport& transport_;
    std::unique_ptr<LanDme> dme_;
    LevelLoadHost* level_ = nullptr;
    std::unique_ptr<DmeThreadBody> thread_;
    bool sleeping_ = false;
    bool running_ = false;
    bool npc_fire_gate_reset_ = true;
    std::uint32_t thread_runs_ = 0;
    std::uint32_t main_priority_ = 0, dme_priority_ = 0;
    std::uint32_t sp_ = kStack;
    // PORT-ONLY: the arena the DME's allocator callback 002caa70 hands out (the registry and the DME tables,
    // about 16 KB); the original allocates from the game's heap.
    static constexpr std::uint32_t kHeapBase = 0x1c00000, kHeapEnd = 0x1d00000;
    std::uint32_t heap_ = kHeapBase;
    // PORT-ONLY: the game heap 0036b180 / 0036b130 that 0016a0a0 allocates the local team data from.
    // The remote humans (0x35a0 bytes, 002c6b70) come from it too (132ln5f). 132ln5h8: a first-fit heap whose
    // freed blocks are reused (merged with free neighbours), as the original heap's are; the bump arena of
    // 132ln5f filled after about 7000 in-level frames (the per-frame message blocks 0036b130 frees).
    static constexpr std::uint32_t kGameHeapBase = 0x1d00000, kGameHeapEnd = 0x1d20000;
    std::uint32_t game_heap_ = kGameHeapBase;
    std::uint32_t game_frees_ = 0;
    std::map<std::uint32_t, std::uint32_t> game_live_, game_free_;   // block -> size (16-byte multiples)
    std::uint32_t game_alloc(std::uint32_t size);
    void game_free(std::uint32_t block);
    // PORT-ONLY: the empty level object lists of mirror_empty_level_objects().
    static constexpr std::uint32_t kLevelObjects = 0x1d20000;
    double timer_ms_ = 0;
    std::vector<std::string> log_;
    bool level_loaded_ = false;
    std::function<void()> lobby_;
    std::function<void()> lobby_exit_;
    std::function<std::optional<std::uint32_t>()> lobby_fatal_;  // 133lt3: 00452fd0's online stand-in
    // PORT-ONLY: the GUI app 0x004ffc04 (the front end's; nothing here reads it) and the lobby session 004294c0
    // returns (only 002ca580 reads it, at +0x255c, after 00452fd0 reports a disconnect, which a LAN lobby never does).
    static constexpr std::uint32_t kGuiApp = 0x1d30000, kLobbySession = 0x1d31000;
    std::thread pump_;
    std::atomic<bool> pump_stop_{false};
    std::exception_ptr pump_error_;
    double pumped_ms_ = 0;
    // PORT-ONLY (132ln5f): the characters' stand-ins (see the header). Their virtual functions are port addresses
    // that call() answers: kFnNop does nothing, kFnHuman answers 1 (the local player's controller +0x54: a human),
    // kFnRemote 2 (a remote human's controller +0x88).
    static constexpr std::uint32_t kFakes = 0x1d38000, kFnNop = 0x1d38f00, kFnHuman = 0x1d38f04, kFnRemote = 0x1d38f08;
    // 132ln5g: kFnLeave, the remote human's virtual +0x60 (001b2e90's release): queues a RemoteLeft.
    static constexpr std::uint32_t kFnLeave = 0x1d38f0c;
    // 132ln5h: the record writers' player input (+0x13c / +0x140, the block caches), the local inventory, a scratch
    // word, and the NPC proxy 0040c540 answers with (its vtable kProxyVtable).
    static constexpr std::uint32_t kLocalInput = kLocalPlayer + 0x3a00, kLocalInventory = kLocalPlayer + 0x3c00;
    static constexpr std::uint32_t kScratch = kLocalPlayer + 0x3d00, kNpcProxy = kFakes + 0x700, kProxyVtable = kFakes + 0x800;
    static constexpr std::uint32_t kHumanVtable = kFakes, kComponentVtable = kFakes + 0x100;
    static constexpr std::uint32_t kRemoteControllerVtable = kFakes + 0x200, kPlayerControllerVtable = kFakes + 0x300;
    static constexpr std::uint32_t kRemoteComponent = kFakes + 0x400, kCharacters = kFakes + 0x500;
    static constexpr std::uint32_t kPlayerComponent = kLocalPlayer + 0x3600, kPlayerController = kLocalPlayer + 0x3700;
    std::uint32_t human_constructor_001b8130(LevelLoadMemory& memory, const std::vector<std::uint32_t>& a, std::uint32_t sp);
    std::uint16_t remote_client_ = 0;   // the client of the 002c6b70 running
    std::uint32_t remote_created_ = 0;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> remote_keys_;  // key -> human (0040b6d0)
    std::uint32_t play_games_received_ = 0;  // 134eq1: NetPlayGameMsg receives (00431850)
    std::map<std::uint32_t, std::pair<std::uint16_t, std::uint32_t>> backpack_bindings_;
    std::vector<RemoteHuman> remote_events_;
    // kPlayerBlocks is the block 001314f0's stand-in returns (001b2e90 clears its +0xb70).
    static constexpr std::uint32_t kPlayerBlocks = kFakes + 0x600;
    void player_network_001b53d0();
    std::uint32_t queue_message_003ca0e0(LevelLoadMemory& m, std::uint32_t entry);
    std::vector<RemoteMove> remote_moves_;
    std::vector<RemoteLeft> remote_left_;
    std::vector<RemoteAction> remote_actions_;
    std::vector<RemoteAim> remote_aims_;
    std::vector<RemoteWeapon> remote_weapons_;
    std::vector<RemoteAnimation> remote_animations_;
    // 132ln5h4: the section's results for the human it runs on (003cc990's health, the action 0x43's byte).
    void human_update_001b1710(std::uint32_t human);
    std::optional<std::int32_t> pending_health_;
    std::optional<std::uint8_t> pending_physical_;
    std::uint32_t sections_run_ = 0;
    std::uint32_t section_human_ = 0;  // the human whose 001b1710 section runs (001b2e90 may release it first)
    bool section_npc_ = false;         // 132ln5h14b: section_human_ is an NPC block (npc_section_001b1710)
    std::optional<std::int32_t> pending_npc_weapon_;
    std::uint32_t npc_sections_run_ = 0;
    static void mirror_animation_block(LevelLoadMemory& m, std::uint32_t block, std::uint32_t flags_3248,
                                       std::uint32_t current_3204, bool turn_correction_31b3, bool accumulate_31b4,
                                       bool byte_31b5, std::uint8_t physical_12);
    std::uint32_t controller_runs_ = 0;
    std::uint32_t refused_logged_ = 0;
    bool local_inventory_ = false;
    std::uint32_t lan_id(LevelLoadMemory& m, std::uint32_t object) const;
    std::uint32_t human_key(std::uint32_t human) const;
    std::string hud_line_;
    std::uint32_t moves_sent_ = 0, moves_received_ = 0;
    bool local_set_ = false;
    const AgentPartCache* part_cache_ = nullptr;
    std::optional<std::array<float, 4>> local_floor_plane_;
    std::uint32_t local_links_ = 0;
    // 132ln5h11: the generators' blocks (register_generator; kFakes + 0x1000 up, 0x40 bytes each), their vtable and
    // virtual functions, the position virtual +0xc0 of the players (003ce850: the local player's component +0x40, a
    // remote human's last position: its creation matrix row 3, then each RemoteMove's), the key word the GenInfo
    // calls point at and the NPC info pass's stack object.
    static constexpr std::uint32_t kGeneratorVtable = kFakes + 0xa00, kGenerators = kFakes + 0x1000;
    static constexpr std::uint32_t kFnGenEnable = 0x1d38f10, kFnGenDisable = 0x1d38f14, kFnGenPosition = 0x1d38f18;
    static constexpr std::uint32_t kFnHumanPosition = 0x1d38f1c;
    // 132ln5h14c: the damage virtual +100 (003ccb00) of the objects 004081e0 reaches, and the damage message's
    // source object (+28; the serialiser 004083d0 sends its +c, the LAN key).
    static constexpr std::uint32_t kFnDamage = 0x1d38f20, kDamageSource = kScratch + 0x100;
    static constexpr std::uint32_t kGeneratorKey = kScratch + 0x80, kGeneratorNpcInfo = kScratch + 0x90;
    std::map<std::uint32_t, std::uint32_t> generator_blocks_;  // id -> block
    std::uint32_t next_generator_ = kGenerators;
    GeneratorReceiver generator_receiver_;
    std::uint32_t generator_sent_ = 0, generator_received_ = 0;
    std::uint32_t npc_remote_releases_ = 0;
    NpcTerrainHosts npc_terrain_hosts_;
    std::uint32_t npc_net_activations_ = 0;
    std::vector<std::uint32_t> remote_npc_deactivates_;
    std::uint32_t sections_gated_3269_ = 0;
    std::uint32_t npc_deactivate_globals_sent_ = 0, npc_deactivates_received_ = 0, terrain_activations_received_ = 0;
    std::uint32_t receive_terrain_activation_00431d60(std::uint32_t buf);
    static constexpr std::uint32_t kFnNpcNodeActive = 0x1d38f2c;
    std::vector<RemoteNpcMessage> remote_npc_messages_;
    std::uint32_t npc_messages_sent_ = 0, npc_messages_received_ = 0;
    std::uint32_t receive_npc_message_002d7190(std::uint32_t buf);
    std::map<std::uint32_t, std::array<float, 3>> remote_positions_;  // human -> its last position
    std::uint32_t generator_block(std::uint32_t id) const;
    std::uint32_t client_player_003fb8d0(std::int32_t client);
    void send_generator_message(std::uint32_t id, std::int32_t a1, bool enable);
    // 134lt6: the particle placements' blocks (register_particle), their vtable and virtual functions.
    static constexpr std::uint32_t kParticleVtable = kFakes + 0xb00, kParticles = 0x1d34000, kParticlesEnd = kFakes;
    static constexpr std::uint32_t kFnParticleEnable = 0x1d38f24, kFnParticleDisable = 0x1d38f28;
    std::map<std::uint32_t, std::uint32_t> particle_blocks_;  // key -> block
    std::uint32_t next_particle_ = kParticles;
    ParticleReceiver particle_receiver_;
    std::uint32_t particle_sent_ = 0, particle_received_ = 0;    // 132ln5h12: the NPC blocks (register_npc; 0x4000 bytes each from kNpcBlocks, below the stack), key -> block.
    static constexpr std::uint32_t kNpcBlocks = 0x1d50000, kNpcBlockSize = 0x4000, kNpcBlockCount = 0x60;
    static constexpr std::uint32_t kNpcController = 0x3800, kNpcInventory = 0x3c00;   // 132ln5h14b: +0x3524's
    std::map<std::uint32_t, std::uint32_t> npc_blocks_, npc_generators_;
    std::set<std::uint32_t> named_npc_registered_;
    std::map<std::uint32_t, bool> named_npc_active_;
    NamedNpcReceiver named_npc_receiver_;
    std::uint32_t named_npcs_sent_ = 0, named_npcs_received_ = 0;
    std::vector<std::uint32_t> free_npc_blocks_;
    void install_remote_npc_controller(std::uint32_t block);
    void bind_remote_named_npc(std::uint32_t key, std::uint16_t client, bool found);
    std::uint32_t next_npc_block_ = kNpcBlocks;
    std::uint32_t npc_block(std::uint32_t c) const;
    std::vector<RemoteSpawn> remote_spawns_;
    std::vector<NpcControl> npc_controls_;
    std::function<void(const NpcControl&)> npc_control_handler_;
    void npc_control(const NpcControl& control) {
        if (npc_control_handler_) npc_control_handler_(control);
        else npc_controls_.push_back(control);
    }
    std::uint32_t spawns_sent_ = 0, spawns_received_ = 0;
    std::vector<GeneratorRelease> generator_releases_;
    std::uint32_t generator_releases_sent_ = 0, generator_releases_received_ = 0;
    std::vector<RemoteDespawn> remote_despawns_;
    std::vector<RemoteHealth> remote_healths_;
    std::uint32_t healths_sent_ = 0, healths_received_ = 0;
    std::vector<RemoteDamage> remote_damages_;
    bool sending_damage_ = false, damage_queued_ = false;
    std::uint32_t damages_sent_ = 0, damages_received_ = 0, damage_notices_dropped_ = 0;
    std::vector<RemoteDeath> remote_deaths_;
    std::uint32_t deaths_sent_ = 0, deaths_received_ = 0;
    std::uint32_t despawns_sent_ = 0, despawns_received_ = 0;
    static constexpr std::uint32_t kObjectiveBlocks = 0x1d46100;
    std::uint32_t objective_counter_ = 0;
    std::uint32_t next_objective_block_ = kObjectiveBlocks;
    std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> objective_bindings_; // script id -> (key, block)
    std::vector<RemoteObjectiveOutcome> objective_outcomes_;
    std::uint32_t objective_sent_ = 0, objective_received_ = 0;
    std::uint32_t receive_objective_message(std::uint32_t data);
    void require_objective_main_thread() const;
    std::vector<RemoteObjectiveActive> objective_actives_;
    std::uint32_t objective_active_sent_ = 0, objective_active_received_ = 0;
    std::uint32_t receive_objective_active(std::uint32_t data, bool active);
    std::vector<RemoteAgentUpdate> agent_updates_;
    std::uint32_t agent_updates_sent_ = 0, agent_updates_received_ = 0;
    std::vector<RemoteGlobalTimer> global_timers_;
    std::uint32_t global_timers_sent_ = 0, global_timers_received_ = 0;
    std::vector<RemotePersonalTimer> personal_timers_;
    std::vector<RemoteGoggles> goggles_;
    std::uint32_t goggles_sent_ = 0, goggles_received_ = 0;
    std::vector<RemoteLabel> labels_;
    std::uint32_t labels_sent_ = 0, labels_received_ = 0;
    std::vector<RemoteCallout> callouts_;
    std::uint32_t callouts_sent_ = 0, callouts_received_ = 0;
    std::vector<RemoteQuickChat> quick_chats_;
    std::uint32_t quick_chats_sent_ = 0, quick_chats_received_ = 0;
    std::uint32_t receive_label(std::uint32_t origin, std::uint32_t data);
    std::uint32_t receive_callout(std::uint32_t origin, std::uint32_t data);
    std::uint32_t personal_timers_sent_ = 0, personal_timers_received_ = 0;
    std::uint32_t summary_updates_ = 0;   // 133lt1: 00134110's cMachine_NetUpdate calls
    std::vector<RemoteKillStatistics> kill_statistics_;
    std::uint32_t kill_statistics_sent_ = 0, kill_statistics_received_ = 0;
    std::uint32_t receive_generic_message(std::uint32_t origin, std::uint32_t data);
    std::vector<RemotePlayerSound> player_sounds_;
    std::uint32_t player_sounds_sent_ = 0, player_sounds_received_ = 0;
    std::uint32_t receive_player_sound(std::uint32_t origin, std::uint32_t data, bool stop);
    std::vector<RemoteEndLevel> end_levels_;
    std::uint32_t end_levels_sent_ = 0, end_levels_received_ = 0;
    std::uint32_t receive_end_level(std::uint32_t origin, std::uint32_t data);
    // 133lt4: the world messages (above).
    std::uint16_t session_host_client();
    // fields: {offset, size, value} after the header; text: the checkpoint label at +0xc (strncpy 0x40).
    bool send_world_packet(std::uint32_t type_address, std::uint32_t pack, std::uint32_t size,
                           const std::vector<std::array<std::uint32_t, 3>>& fields, std::uint32_t target_key,
                           std::uint32_t recipient, std::uint32_t flags, std::optional<std::string_view> text = {});
    std::uint32_t receive_world_message(std::uint32_t callback, std::uint32_t origin, std::uint32_t data);
    std::vector<RemoteHotbox> remote_hotboxes_;
    std::vector<RemoteCheckpoint> remote_checkpoints_;
    std::vector<RemoteLiftLock> remote_lift_locks_;
    std::vector<RemoteTrigger> remote_triggers_;
    std::vector<RemotePlayerMessage> remote_player_messages_;
    std::uint32_t player_messages_sent_ = 0, player_messages_received_ = 0;
    std::uint32_t hotbox_sent_ = 0, hotbox_received_ = 0, checkpoints_sent_ = 0, checkpoints_received_ = 0;
    std::uint32_t lift_locks_sent_ = 0, lift_locks_received_ = 0, triggers_sent_ = 0, triggers_received_ = 0;
};

}  // namespace omega::lan
