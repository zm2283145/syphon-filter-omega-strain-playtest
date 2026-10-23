#pragma once
// DME.cc, the game side of the in-level network layer (LAN phase 3, checkpoint 132ln5; research/NET_INLEVEL.md).
//
// These are the original functions that sit between the level-load wrapper 001693f0 and the DME public API
// (0x002e9320..0x002ecf08, NET_INLEVEL.md section 3). They run unchanged in a LAN game: everything they call is
// an explicit host boundary by original address (LevelLoadHost::call), so the DME API calls reach the PORT-ONLY
// LanDme stand-in (option M, src/port/net/lan_dme.*), the EE kernel calls reach the PORT-ONLY DME thread model,
// and every game callee that is not ported yet throws at its address in the caller's host.
//
// Each function writes EE memory exactly where the original does (globals 0x005721a8.. / 0x00572400..,
// 0x00494178, 0x00493a10, 0x0055c7c0.., the T0 timer registers 0x10000000 / 0x10000010 / 0x10000020) and puts its
// stack locals at the original frame offsets below `stack` (the caller's $sp), so the interpreter goldens
// (tools/ee/gen_dme_game.py, tests/dme_game_goldens.hpp) compare call traces and memory bit for bit.

#include "level_load.hpp"

#include <cstdint>

namespace omega {

// Addresses shared with the hosts (the DME globals the game keeps in DME.cc's .bss).
namespace dme {
constexpr std::uint32_t kWorld = 0x5721a8;          // the DME world index the game talks to (0 in LAN)
constexpr std::uint32_t kHostRole = 0x5721c0;       // 1 on the session host
constexpr std::uint32_t kNetworkGame = 0x5721c8;    // 1 in a network game
constexpr std::uint32_t kInLevel = 0x5721d0;        // set by 0042b0c0 once the level runs networked
constexpr std::uint32_t kQuitRequest = 0x5721d8;    // the disconnect / quit flag (with the text at 0x005721e0)
constexpr std::uint32_t kQuitText = 0x5721e0;       // 16 bytes
constexpr std::uint32_t kMessagesReady = 0x5723d0;  // 00165bc0 registered the message types and object classes
constexpr std::uint32_t kBusy = 0x5723c8;           // the update's re-entry byte (set and cleared around blocks)
constexpr std::uint32_t kPeerQuit = 0x5723b8;       // another client dropped (00429d30's status walk)
constexpr std::uint32_t kState = 0x5724b0;          // the session state byte (NET_INLEVEL.md 2.4)
constexpr std::uint32_t kLocalSlot = 0x494178;      // the local client index, -1 until 002ebaf8 fills it
constexpr std::uint32_t kThreadDone = 0x572500;     // the DME thread reached its exit
constexpr std::uint32_t kThreadWake = 0x572508;
constexpr std::uint32_t kThreadId = 0x572510;       // the DME update thread (the main thread's id before 00429380)
constexpr std::uint32_t kTimerHandler = 0x572518;   // AddIntcHandler's id for INTC 9 (TIM0)
constexpr std::uint32_t kMainThread = 0x572520;
constexpr std::uint32_t kThreadStarted = 0x572528;
constexpr std::uint32_t kThreadStop = 0x572530;
constexpr std::uint32_t kTicks = 0x572538;          // TIM0 compare events (about 1.02 ms each)
constexpr std::uint32_t kLastWakeTick = 0x582540;
constexpr std::uint32_t kIntcInstalled = 0x533880;  // the allocator brackets its calls with DisableIntc(9)
constexpr std::uint32_t kT0Count = 0x10000000;
constexpr std::uint32_t kT0Mode = 0x10000010;
constexpr std::uint32_t kT0Comp = 0x10000020;
constexpr std::uint32_t kSyncCallback = 0x429c00;   // the start callback (0042a1b0's parameter +0x20)
constexpr std::uint32_t kClientJoinedGame = 0x3faab0;  // the remote-client-joined callback (+0x28)
constexpr std::uint32_t kThreadEntry = 0x4295e0;
constexpr std::uint32_t kTimerInterrupt = 0x4292b0;
// The NetMap (0x0055c7a0, a std::map built by 004d6aa0's static constructor 003fcca0: +0 count, +4 the header
// node whose +0 is the root, +0xc the leftmost node; empty: +0xc = 0x0055c7a4).
constexpr std::uint32_t kNetMap = 0x55c7a0;
}  // namespace dme

// 00429430: the TIM0 setup (mode 0xc00, compare 0x24a, count 0, mode 0x382), AddIntcHandler(9, 004292b0, 0) into
// 0x00572518 and, when that is not -1, EnableIntc(9). Returns 1 when the handler was installed.
std::uint32_t dme_timer_install_00429430(LevelLoadMemory& m, LevelLoadHost& host);

// 004292b0: the TIM0 interrupt handler. Counts compare events in 0x00572538 (clearing the count register) and,
// once the thread runs and 100 events passed since the last wake, wakes the DME thread with iWakeupThread
// (0010e010; "ERROR IN iWAKEUPTHREAD" 0x004bf990 through 0010f388 when the result is not the thread id).
std::uint32_t dme_timer_interrupt_004292b0(LevelLoadMemory& m, LevelLoadHost& host);

// 0042a770 = 00429380(004295e0): start the DME update thread once (0x00572528). The main thread (id in
// 0x00572510 until then, kept in 0x00572520) drops to priority 10; the new thread (stack 0x00572540, 0x10000
// bytes, gp 0x004e9b70, priority 5) is created into 0x00572510 and started; 0x00533880 = 0x00572528 = 1.
// The ThreadParam block sits at stack - 0x30 as in the original frame. Returns 1.
std::uint32_t dme_thread_start_0042a770(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack = 0x1f00000);
std::uint32_t dme_thread_start_00429380(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t entry,
                                        std::uint32_t stack = 0x1f00000);

// 004295e0, the thread body: while 0x00572530 is clear { 00429d30(); EI 001161f8; SleepThread 0010d080;
// DI 001161a0 } then EI, 0x00572500 = 1, ExitDeleteThread 0010cfa0. A host has no EE threads, so the body is
// resumable: resume() runs from the entry or from the return of SleepThread up to the next SleepThread (true:
// the thread sleeps) or through the exit (false). Driving resume() after every SleepThread replays the
// original's call sequence exactly (the goldens check it that way).
class DmeThreadBody {
public:
    bool resume(LevelLoadMemory& m, LevelLoadHost& host);
    bool exited() const { return at_ == At::Exited; }
    bool started() const { return at_ != At::Entry; }
private:
    enum class At { Entry, AfterSleep, Exited };
    At at_ = At::Entry;
};

// 0042a1b0: stop the DME thread (ReferThreadStatus both threads, priority 0 for the DME thread unless the main
// thread already outranks it, 0x00572530 = 1, iWakeupThread, DelayThread(10) until 0x00572500; then the five
// thread bytes are cleared) and, unless the state is 0xa or 7, run the level start barrier: state 8, the DME
// start parameters (002eabf0 defaults; +4 world, +8 "Foo" 12 bytes, +0x14 500, +0x18 0, +0x1c host-or-solo,
// +0x20 00429c00, +0x28 003faab0, +0x24 / +0x2c 0) to 002eac88, then 00429d30 until the state leaves 8 (a host)
// or 8 / 9 (a client) or 15000 ms pass on the DME clock (002ebc48). A timeout or state 0xb ends the session:
// 0x00497540 = 1, 00437bf0, 002ec6a0(0), 002ec738(0), 0042b230, 0x005721d8 = 1, "F_TO_JOINGAME" to 0x005721e0.
void dme_stop_thread_and_sync_0042a1b0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack = 0x1f00000);

// 00437bf0 (checkpoint 132ln5h7): the headset voice teardown. Returns at once while the byte 0x00582b88 (a voice
// stream runs, 00438340) is clear, which it always is without the headset library (lgaud); otherwise 002eb650,
// 002ec460, 00325a28 / 00325bb8 / 003254b0 on 0x00497548, 002ec878, 0042bf90, 0042c1d0 and the resets, which throw.
void dme_voice_end_00437bf0(LevelLoadMemory& m, LevelLoadHost& host);

// 0042b230 (checkpoint 132ln5h7): the session globals' reset: 0x005721d8 = 0x005723b8 = 0, the world 0x005721a8 =
// -1, 0x00494180 = 4, 0x00494178 = 0x00494198 = -1, 0x005721c0 = 0x00572410 = 0, 0x005723f0 = 0, the bytes
// 0x00572418..0x0057241b and 0x00572468 = 0, the level-info sends 0x00572428 / 0x0057242c / 0x00572430 = 0.
void dme_session_reset_0042b230(LevelLoadMemory& m);

// 0042b1f0 (checkpoint 132ln5h7): end the network session (0042a1b0's failure path has it inlined): 0x00497540 = 1,
// 00437bf0, 002ec6a0(0) (leave the world), 002ec738(0) (disconnect it), 0042b230.
void dme_session_end_0042b1f0(LevelLoadMemory& m, LevelLoadHost& host);

// 00429c00: the start barrier's callback; arg = {status, user}. Status 0: a host (or solo) goes to 0xa, a client
// to 9, or to 0xa when it is 9 already; any other status: 0xb.
void dme_sync_callback_00429c00(LevelLoadMemory& m, std::uint32_t arg);

// 003faab0: DME's remote-client-joined-the-game callback; arg = {status, world, client, user}. Status 0 only:
// prints "Remote client joined the game: %d[%d]" (00127358), moves the state like 00429c00, clears 0x00493a10[client]
// and 0x00572418[client], and fills the remote player record 0x0055c7c0 + slot * 0x98 (slot = client, or
// client - 1 above the local client index 0x00494178) with 002eb9a8's 16 bytes, +0x14 = 1 and +0x10 = client.
void dme_remote_client_joined_003faab0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t arg);

// 00429a90 / 00429920: DME's client-joined-the-world / client-left callbacks (the create parameters +0x1c /
// +0x24); arg = {?, client}. Joined: count 0x005723f0 + 1, 0x00572418[client] = 0, and on the host (or solo) a
// client other than 0 gets its level-info send queued (0x00572424 + 4 * client = 0x0ffffffe). Left: count - 1;
// the host leaving (002ebba0) posts HOST_DISCONNECT (state 7); 0x00572418[client] = 1; a host frees one queued
// send slot; then 0043b190 / 0043acb0(client) (the generic info objects' owner change).
void dme_client_joined_00429a90(LevelLoadMemory& m, std::uint32_t arg);
void dme_client_left_00429920(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t arg,
                              std::uint32_t stack = 0x1f00000);
// 0043acb0(geninfo, client) (132ln5g): with the GenInfo block 0x00583888 set, the left client's byte
// 0x005838a0[client] = 1 (the first argument is not read).
void geninfo_client_left_0043acb0(LevelLoadMemory& m, std::uint32_t client);

// 0042b0c0: in-level networking on (0x005721d0 = 1), then every object of the list 0x00571740 (its vector at
// +4: count +8, data +0xc) gets its virtual +0xfc(object, 0).
void dme_level_net_start_0042b0c0(LevelLoadMemory& m, LevelLoadHost& host);

// 00429d30: the DME update (the thread's loop body; also called by the level-load wrapper and 0042a1b0). Returns
// 002ec4f8's result, or 0x00ffffff when the message types are not registered (0x005723d0) or the update is busy.
std::uint32_t dme_update_00429d30(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack = 0x1f00000);

// 0042b1a0: the network clock: local != 0 reads 002ebc48 (the local DME clock), 0 reads 002ebce0 (the session
// clock); -1 when the read fails.
std::uint32_t dme_clock_0042b1a0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t local,
                                 std::uint32_t stack = 0x1f00000);

// 003fac40: every 100 ms of the local clock (0x0055c990), the first client whose 0x00493a10 entry is 0 (or 1 with
// *flag set) gets its network data: in a running level (0x005721d0) the entry becomes 1 and 003fa940(client)
// sends it, clearing *flag; before the level only a null flag marks 0x0055c780[client] = 1.
void dme_send_network_data_003fac40(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t flag);

// 0043a810 (GenInfoObject.cc, from 00429d30 in a level, checkpoint 132ln5c): looks up the GenInfo network object
// (002eb650 on the handle 0x00497578, its result ignored: the object word at $sp+0x1c is read as it is). When
// 0x00583890 is set (00439390 sets it after the object's data changed), the owner of the object (+4) equal to the
// local client index (002ebaf8 into 0x00494178 when it is still -1; 0 outside a network game) sends it:
// 002ec460(handle). 0x00583890 is cleared either way.
void dme_geninfo_update_0043a810(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack = 0x1f00000);

// 00436c00 (NPCInfoObject.cc, from 00429d30 in a level): the NPC info objects' update 004355d0(arg) unless
// 0x00582778 is set, then the update counter 0x00582a68 += 1; returns arg.
std::uint32_t npc_info_update_00436c00(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t arg);

// 004355d0 (NPCInfoObject.cc, from 00436c00 and the NPC info setters; checkpoint 132ln5d): nothing while 0x005721b8
// is set. In a running level (0x005721d0): when the blocks exist (0x00582778), are not released yet (0x00582780) and
// this is the host (or solo), the five blocks 0x005827f0.. (read first) become DME objects (00433a60(arg)), the old
// blocks are freed (0036b130(0, block, "NPCInfoObject.cc", 0x24e + 3k) inside the allocator guard 0x00533880 /
// 0x00533888) and 0x00582780 = 1; then always 00434740(arg). Before the level, a network game without the blocks
// allocates five 0x1d0-byte blocks (0036b180, lines 0x269..0x26d; each entry +0 = -1, +4 / +5 = 0xff, +6..+9 = 0,
// +0xa..+0xd = 1), stores them (even when 0) and sets 0x00582778.
void npc_info_objects_004355d0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t arg);

// 00433a60: in a running level on the host (or solo), each block k becomes the data of a "cNPCInfoObject" network
// object: 002eaf28(&0x00582a78[k], 2, 4, 0, 0, 0, 0, block; stack: the name 0x004bfcc8, the class *0x00582a70);
// when that and 002eb650($sp + 0x3c - 4k, handle) succeed, block k = the object's +0x1c.
void npc_info_objects_create_00433a60(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack = 0x1f00000);

// 00434740: the five resend timers 0x00582b60.. (unsigned): when one is below 0042b1a0(1), a pending send
// (0x005827c8[k]) is retried with 002ec3c8(handle) and cleared when that returns 0, and the timer becomes
// 0042b1a0(1) + 0x78 / 0x96 / 0xb4 / 0xd2 / 0xf0.
void npc_info_send_timers_00434740(LevelLoadMemory& m, LevelLoadHost& host);

// 004352c0 (from 00429d30 in a level): each changed object (0x005827a0[k]) is looked up (002eb650, result not
// tested) and, when it exists and its owner (+4) is the local client index, sent with 002ec460(handle); the flag
// is cleared.
void npc_info_send_changed_004352c0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack = 0x1f00000);

// 00436ba0: the NPC info counter's release: a nonzero object takes 0x00582a68 - 1 and, when the low 16 bits of
// mode are positive, is deleted (00100740(object, mode)); returns the object.
std::uint32_t npc_info_release_00436ba0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t object,
                                        std::uint32_t mode);

// 003fa780 (checkpoint 132ln5d): unless the session state 0x005724b0 is 7, the first NetMap entry (in order:
// 003fa8b0, 0013a2a0) with a pending block (+0x1c) hands out its +0x18 byte (*flag_out), +0x14 (*a_out) and
// the block (*data_out), clears +0x1c and returns 1; 0 when none.
std::uint32_t net_map_take_003fa780(LevelLoadMemory& m, std::uint32_t a_out, std::uint32_t flag_out,
                                    std::uint32_t data_out);

// The NetMap 0x0055c7a0 (checkpoint 132ln5e): a red-black tree of 0x24-byte nodes (+0 left, +4 right, +8 parent |
// colour bit 1 = red, +0xc the key (a DME object handle), +0x10 the object, +0x14 a, +0x18 the flag byte, +0x1c the
// pending block, +0x20 0); the map: +0 count, +4 the header (its +0 the root), +0xc the leftmost node.
// 002d1360 (operator[]): the pair (node + 0xc) for the key, inserted by 002d1430 when missing (the pair {key, 0,
// -1, 0, 0, 0}, the node from 0036b180(0, 0x24, "allocator.hh", 0x5c) under the heap guard, the fix-up 0013cd40
// with the rotations 0013cf60 / 0013cff0); `map` is the tree, `key` the key's value.
std::uint32_t net_map_index_002d1360(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t map, std::uint32_t key);
// 003fa8c0(a, key, data, flag) (NetCreateCharacterMsg's receive side, 002c77d4 / 002c79a4): map[key] gets a (+0x14),
// the pending block (+0x1c) and the flag byte (+0x18).
void net_map_set_003fa8c0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t a, std::uint32_t key, std::uint32_t data,
                          std::uint32_t flag);
// 003fb650(key, a, object): in a network game (0x005721c8) map[key] gets the object (+0x10) and a (+0x14) and the
// result is &node+0x10; otherwise the static dummy entry 0x0055c9c0 (built once under the guard byte 0x0055c9b8).
std::uint32_t net_map_set_object_003fb650(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t key, std::uint32_t a,
                                          std::uint32_t object);
// 0040bf10: the local player 0x00571770 when it is nonzero and its +0xc equals 0x00571774; otherwise 0.
std::uint32_t local_player_0040bf10(const LevelLoadMemory& m);
// 003fa940(client) (from 003fac40 once the client is loaded): 0x0055c780[client] = 2; 00429b30 when 0x00572400 is
// set; the local player's (0040bf10) virtual +0x1c(player, client); then for each NetMap node in order: an object
// (+0x10) with its +0x14 byte set and a +0xc other than the player's gets its virtual +0x1c(object, client), and
// 003fac40(0) runs once per node.
void net_client_loaded_003fa940(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t client);

// 003fbec0 (DME.cc): the network game's per-frame update, from the game loop 00167c40 in states 0xf / 0x13 with
// 005721c8 set: 0x005724d0 = 1, 0x00493a18 cleared, an NPCInfoObject on the stack (00436c00 .. 00436ba0) around
// 004355d0, a GenInfoObject (0043b190 / 0043aa30), 0x0055c7b8 = 0042b1a0(0) + 100, then 0042eb20 and 00438770(0).
void net_frame_003fbec0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack);

// 00169390 (checkpoint 132ln5f): the network update the game loop 00167c40 runs every frame in every state but the
// level load 0xe (00168444 / 00168548), so in the level before 003fbec0: with 005721c8 set, cNetMsgThrottle's send
// loop 00472080 and the DME update 00429d30 (on the main thread: 0042a1b0 stopped the DME thread at the level
// load); then, when the GUI app 0x004ffc04 is set and 004294c0 (the lobby session) returns one, 002ca580(app).
void net_update_00169390(LevelLoadMemory& m, LevelLoadHost& host);
// 00472080 (checkpoint 132ln5f): cNetMsgThrottle's send loop over its map 0x00588020 (the static constructor
// 004d8c10 / 00472380 leaves it empty). 0042b1a0(1) first, then the map's leftmost node (+0xc) against its header
// (+4): nothing more for the empty map; the queued messages' sends (00472100..) are not ported (they throw).
void net_throttle_send_00472080(LevelLoadMemory& m, LevelLoadHost& host);
// 002ca580(app) (checkpoint 132ln5f): the lobby session's keep-alive (from 00169390 and the front end's states).
// Unless 0x0051eeb0 is set: s = 004294c0(); with s, SFOLobby's update 004536b0(s), then its disconnect check
// 00452fd0(s); when that reports one, 0x005723b8 = 1 if s+0x255c is 0xa, otherwise 00414790 / 00419260(it) /
// 002ca6f0(app) / 002ca640(app, 1) / 00453b00(s, 1).
void lobby_keepalive_002ca580(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t app);

// 00439670 (GenInfoObject.cc, from 0043b190): nothing unless 005721c8 is set and 005721b8 clear. In a level
// (005721d0), once (0x00583880) and only on the host: the DME object 0x00497578 is created over the block
// 0x00583888 (002eaf28(.., 2, 4, 0, 0, 0, 0, block) with "cGenInfoObject" and the class 0x00497570 on the stack);
// when 002eb650 finds it, its +0x1c becomes the block; the old block is freed (0036b130 line 0x211). Before the
// level the 0x1b8-byte block (0x37 entries: -1 and the byte 0x0f) is allocated (0036b180 line 0x21b) unless one exists.
void geninfo_objects_00439670(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack = 0x1f00000);
// 0043b190(a0): 00439670(), returns a0.
std::uint32_t geninfo_create_0043b190(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t arg);
// 0043a8b0 (0043aa30 jumps here): when the timer 0x00583910 is below 0042b1a0(1) (unsigned) and any GenInfo field
// changed (the words 0x005838b8 / 0x005838d8 / 0x00583900, the bytes 0x005838a0..a3), the object (002eb650, result
// untested) is sent: owned by another client, 002ec3c8 asks for it; owned locally, 00439390(local, handle, -2).
// The timer becomes 0042b1a0(1) + 500.
void geninfo_send_timer_0043a8b0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack = 0x1f00000);
// 00439390(local, handle, mode) (132ln5g): the locally owned GenInfo object's update. With 002eb650 finding the
// object, mode -2 and the object's +4 the local slot: the queues 00438e20 (0x005838b8), 00439030 (0x00583900) and
// 00438c40 (0x005838d8) drain into its block +0x1c (0x37 entries of 8 bytes); then for each client whose byte
// 0x005838a0[client] is set (0043acb0: it left), on a host or solo game, each entry it owned (byte +4's low nibble)
// becomes the host's (0) and its bit leaves the entry's client mask (the high nibble); the byte is cleared. The
// object is marked dirty (0x00583890 = 1: 0043a810 sends it through 002ec460). The first argument is not read.
// s2: the caller's $s2, which 00439390 leaves untouched for 00438e20 (see geninfo_apply_adds_00438e20).
void geninfo_send_00439390(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t handle, std::uint32_t mode,
                           std::uint32_t stack = 0x1f00000, std::uint32_t s2 = 1);

// GenInfoObject.cc's entry API and queues (132ln5h9). Each entry of the block 0x00583888 is a key word and a byte
// (the low nibble: the owning client, 0xf none; the high nibble: the clients' interest mask). Every function first
// looks the key *keyp up in the block (no block: not found). The first argument (the GenInfo object) is not read.
// 00439c70(g, keyp): the entry's owner, or -1 (no entry, or owner 0xf).
std::uint32_t geninfo_owner_00439c70(const LevelLoadMemory& m, std::uint32_t keyp);
// 0043aa40(g, keyp): 1 when the entry exists and the local client owns it.
std::uint32_t geninfo_owned_0043aa40(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t keyp);
// 0043ab20(g, keyp): 1 when the entry exists and has an owner.
std::uint32_t geninfo_claimed_0043ab20(const LevelLoadMemory& m, std::uint32_t keyp);
// 0043aba0(g, keyp, client): 0 for a client >= 5 (signed) or no entry; -1 means the local client; else whether the
// client's bit is in the entry's interest mask.
std::uint32_t geninfo_interest_0043aba0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t keyp, std::uint32_t client);
// 004398e0(g, keyp, owner) (the generator's registration; owner < 0 means none, 0xf): nothing when the entry
// exists. In a level (005721d0) unless an add with the key is queued: a new 8-byte copy {key, owner} (0036b180
// line 0x1ec) joins the list 0x005838b8 and, with the object 0x00497578 (002eb650, result untested) owned
// locally, 00439390(local, handle, -2) sends it ($s2: keyp). Before the level the first free entry takes the key
// and the owner (the interest mask kept).
void geninfo_add_004398e0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t keyp, std::uint32_t owner,
                          std::uint32_t stack = 0x1f00000);
// 00439d00(g, keyp, owner): with the entry present and its owner changing (owner < 0: 0xf): a queued change for the
// key (0x005838d8) takes the new owner unless it already has it; otherwise a new copy {key, owner} (line 0x1bb)
// is queued and sent as in 004398e0 ($s2: owner).
void geninfo_set_owner_00439d00(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t keyp, std::uint32_t owner,
                                std::uint32_t stack = 0x1f00000);
// 0043a030(g, keyp) / 0043a420(g, keyp): the local client's interest bit leaves / joins the entry. Nothing without
// the entry or when the bit is already clear / set. A queued change for the key (the map 0x00583900, keyed by the
// copy's address) takes the new byte (0 / 1); otherwise the bit changes in the block, a copy {key, 0xf} (line
// 0x196 / 0x173) is inserted with the byte (0x00497588: 0 / 0x00497580: 1) and sent as in 004398e0. s2: the
// caller's $s2 (00439390 passes it on).
void geninfo_clear_interest_0043a030(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t keyp,
                                     std::uint32_t stack, std::uint32_t s2);
void geninfo_set_interest_0043a420(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t keyp,
                                   std::uint32_t stack, std::uint32_t s2);
// 00438c40(block): each queued owner change (0x005838d8) found in the block sets the entry's owner; the copy is
// freed (line 0x32a) and the node erased (00138da0).
void geninfo_apply_owners_00438c40(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t block);
// 00438e20(block): each queued add (0x005838b8). The original keeps the found entry in $s2 without clearing it
// first: $s2 starts as the caller's (s2) and, once an entry is found, stays set for the later adds. Only while
// it is 0: 0043b190 (the block's creation) and the first free entry takes the copy's key and owner. The copy is
// freed (line 0x2fd) and the node erased.
void geninfo_apply_adds_00438e20(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t block, std::uint32_t s2,
                                 std::uint32_t stack = 0x1f00000);
// 00439030(block): each queued interest change (0x00583900, in order): with an entry found for the copy's key (the
// last found entry is kept, as $s3, when a later key is not found), the byte 1 sets the local bit, 0 clears it
// when set; the copy is freed (line 0x2b6) and the node erased (0043b200); then the map is cleared (0043b1c0).
void geninfo_apply_interest_00439030(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t block);
// 0043ace0 (from 003fbcd0, the level's network teardown): a block not created by the DME object is freed (line
// 0x2b); the block, the created byte, the handle (-1), the dirty byte, 0x00583898 and the timer reset; every
// queued copy is freed (lines 0x38 / 0x41 / 0x4c) and the queues emptied; the left-client bytes cleared.
void geninfo_reset_0043ace0(LevelLoadMemory& m, LevelLoadHost& host);

// 0042eb20 (from 003fbec0): the network objects' update over the map 0x00582700 (00431380 empties it at the
// static construction: +0xc = +4), then 0042cf30. Only the empty map is ported; objects in it throw.
void net_objects_update_0042eb20(LevelLoadMemory& m, LevelLoadHost& host);
// 0042cf30: a period from 0x00572538 (soft double, read only per object), 00436c00 on the stack, the sends over the
// map 0x00582720 (004313a0 empties it), 00436ba0(.., -1). Only the empty map is ported; objects in it throw.
void net_objects_send_0042cf30(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack = 0x1f00000);
// 00438340 (from 00438770): on the host / solo, once (0x00582cf0 clear): in a level the "HeadsetSemaphore" DME object
// (002eaf28(sp+0x48, 2, 4, 1000, 0, 0, 0, 0), the class 0x00583868; its handle into 0x00583870, its +0x1c into
// 0x00582cf0 when 002eb650 finds it), before the level 0x00582cf0 = 0. The first call (0x00497568) runs the headset
// library's init 003241c8(002caa70, 002ca9b0) (0x00582b90 = 1 when it returns 0); with the library up 00325280 counts
// the devices and 004384c0 opens one (returns 0 when that fails). Returns 1, or 0 when 004384c0 failed.
std::uint32_t voice_headset_00438340(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack = 0x1f00000);
// 00438770(a0) (from 003fbec0, 00168838 and 00453ff4): returns at once when 00438340 returns 0 or no headset runs
// (0x00582b88 clear); the voice stream itself is not ported (throws).
void voice_frame_00438770(LevelLoadMemory& m, LevelLoadHost& host);

// 00185840 (human.cc, the client's level load): every pending NetMap record (003fa780) creates its remote
// character (002c6b70(data, a) when the flag is set, else 002c6690(data, a)) and the record's block is freed
// (0036b130(0, data, "human.cc", 0x447d) in the allocator guard).
void net_create_characters_00185840(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack = 0x1f00000);

// 003fa5d0 (LoadNetworkData, the client's level load): unless the session state 0x005724b0 is 7, for each
// client 0..3 whose 0x0055c780 entry is 1 the first level object (the vector at *0x00571748 + 4) whose
// component +0x58 answers its vtable +0x54 and whose +0x14 byte is set gets vtable +0x1c(object, client); any
// other client prints "Trying to LoadNetworkData but %d is already updated" (00127358).
void net_load_network_data_003fa5d0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack = 0x1f00000);

// 0042af00: the DME init (00165bc0 on the first network start, checkpoint 132ln5b): 0x00572418..1b = 0; the
// allocator pair 002ea4f0(002caa70, 002ca9b0) (nonzero: return); 002e9480(params at $sp+0x40) fills the init
// parameters, 002e94e0(params, result at $sp+0x20) starts the library (nonzero: 0x005724b0 = 2); then
// 002e9320($sp+0x10) (the version text) and 002ec168(0x6000, 0x6000); a zero result sets 0x005724b0 = 1.
void dme_init_0042af00(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack = 0x1f00000);

// The remote character messages (checkpoint 132ln5f, NetCreateCharacterMsg.cc / NetCreateNamedCharacterMsg.cc,
// research/NET_INLEVEL.md section 4.8).
// 00185a30(player, client) (the player vtable 0x004da690's +0x1c, from 003fa940 / 003fa5d0): unless +0x2d and +0x2c
// are set and +0x2e clear, returns 1 at once. The weapon list at $sp+0xa0 (12 bytes from 0x0048a2f8): with the
// inventory +0x3524, the current weapon (slot byte +0x84, 6: none = -1) as a halfword, then every other held
// weapon (inventory + k*16 + 8, k < 6, not -1). A controller (+0x58) whose virtual +0x54 answers nonzero (a
// human) sends 002c72a0(client, +0x10, +0xc, component +0x50 + 0x10, player + 0xa0, list, (int) health +0x40);
// otherwise 002c7110(client, +0x10, +0xc, component + 0x10, player + 0x3318, list, the gate, byte +0x3590) with the
// health on the stack. Either after the component's virtual +0x58 when its byte +0x50 is set. Returns 1.
std::uint32_t player_send_00185a30(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t player, std::uint32_t client,
                                   std::uint32_t stack = 0x1f00000);
// 002c7110(client, key, -, matrix, name, weapons, a, b; stack word: c): NetCreateNamedCharacterMsg (0x50 bytes, vtable
// 0x004de158) when 0x0055c780[client] is 1 or 2: +4 key, +8 the angles 001bd590(matrix), +0x14 the position
// (matrix + 0x30), +0x20 six weapon halfwords, +0x2c the name (32 bytes: memset, then strncpy(strlen)), +0x4c a,
// +0x4d b, +0x4e c (halfword); sent by 003fad50(*0x0051ee78, 0x50, msg, client, 0x40). Returns the send's success.
std::uint32_t send_named_character_002c7110(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t client,
                                            std::uint32_t key, std::uint32_t unused, std::uint32_t matrix,
                                            std::uint32_t name, std::uint32_t weapons, std::uint32_t a, std::uint32_t b,
                                            std::uint32_t stack = 0x1f00000);
// 002c72a0(client, key, -, matrix, source, weapons, health): NetCreateCharacterMsg (0x430 bytes, vtable 0x004de148)
// when 0x0055c780[client] is 1 or 2: +4 key, +8 the angles, +0x14 the position, +0x20 health, +0x24 six weapon
// halfwords, +0x30 the descriptor 004073c0(source, &length) (length bytes); sent by 003fad50(*0x0051ee70, 0x430, ..).
std::uint32_t send_character_002c72a0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t client, std::uint32_t key,
                                      std::uint32_t unused, std::uint32_t matrix, std::uint32_t source,
                                      std::uint32_t weapons, std::uint32_t health, std::uint32_t stack = 0x1f00000);
// 003fad50(type, size, msg, client, flags): the game message send. 0 outside a networked level; every 1000 ms
// (0042b1a0(1), the reset byte 0x0055c9e0) the error counts 0x0055c9a0..b0 are cleared; a client other than -1
// must be 1 or 2 in 0x0055c780. 002ea0a0((s8) flags, world, client, 2, type, size, msg): 0 returns 1. On an error,
// unless 00429d30() answers nonzero or flags is 0x40, the send is retried (a second error is printed); errors 0x25 /
// 0x23 count per client (-1: 0x0055c9a0) and the 26th quits the game ("NET_ERROR" into 0x005721e0).
std::uint32_t net_send_003fad50(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t type, std::uint32_t size,
                                std::uint32_t msg, std::uint32_t client, std::uint32_t flags);
// 002c7630(-, world, client, msg) (NetCreateNamedCharacterMsg's receive): in a level 002c6690(msg, client) (the
// allocation path is dead code); returns 0x50.
std::uint32_t recv_named_character_002c7630(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t client,
                                            std::uint32_t msg);
// 002c7810(-, world, client, msg) (NetCreateCharacterMsg's receive): in a level 002c6b70(msg, client); before it the
// message is copied (0036b180(0, 0x430, "NetCreateCharacterMsg.cc", 0x11)) and kept in the NetMap
// (003fa8c0(client, key, copy, 1)) for 00185840. Returns 0x430.
std::uint32_t recv_character_002c7810(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t client, std::uint32_t msg);
// 002c6b70(msg, client) (human.cc): a remote human. An existing character with the key (0040b6d0 on the list
// *0x004ffd44) is removed first (003fb750(client), 0040b720, 003cb230(.., 0.1)). The matrix from the angles
// (cos 00140e80 / sin 00140e70, 00140e20) and the position; the descriptor 00406ec0; the 0x35a0-byte human
// (0036b180 line 0x1af) built by 001b8130(human, matrix, -1, 0, health, *0x0048c0c8, 3, *(world + 0x3264); stack:
// &key, 0, -1, descriptor); its weapons 00143df0 / 001439b0, the list 0040b790, health 003cc990, the component's
// virtual +0xc(matrix), the 0x54-byte controller 002d85d0 attached by 001ae450(human, 3, 0, it), 001b8750(human,
// 001b8740(world)), its virtual +0x20, the NetMap 003fb650 / 00185940 / 00185970, 002c9270 and 00242fa0.
void create_remote_human_002c6b70(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t client,
                                  std::uint32_t stack = 0x1f00000);
// 00185940(human, key): +0x10 = key, or +0xc when key is -1.
void human_set_key_00185940(LevelLoadMemory& m, std::uint32_t human, std::uint32_t key);
// 00185970(human, entry): with the entry's +0x10 set, +0x3584 = it and the controller (+0x58) gets it at +0x1e0 /
// +0x1e4 (its virtual +0x88 answers 2) or +0x44 / +0x48 (with +0x1c added, or 0).
void human_set_net_entry_00185970(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t human, std::uint32_t entry);

// 003fb750(client) (132ln5g): the client -> player map 0x0055c770 (a red-black tree keyed by the client, signed;
// the node's +0x10 the player, +0x14 its id) loses the client's entry: 0 for a client outside 0..3 or not in the
// map; a live player (its +0xc equal to the node's +0x14) whose controller (+0x58) is null or answers 0 to its
// virtual +0x54 keeps it (0); otherwise the node is erased (003fc0c0: the leftmost update 0013a2a0, the unlink and
// fix-up 0013c800, the free 0036b130 "allocator.hh" line 0x64 under the heap guard, the count - 1) and 1 returned.
std::uint32_t client_map_remove_003fb750(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t client);
// 001b2e90(human) (132ln5g; the player vtable 0x004da690 +0x4c): a remote human whose client has left. 0 unless the
// controller (+0x58) answers nonzero to its virtual +0x54, the human is not local (byte +0x14 clear) and the left
// flag 0x00572418[client] is set (client = ((+0xc & 0x7f000000) >> 24) - 1). Then the HUD message
// "PLAYER_LEFTGAME" (003fdb50, 0012f5d0(0x004ffb50, client or -1), sprintf 00128bd0, 003fa3f0(*0x004ffc2c +
// 0x100, text, red {1, 0, 0, 1}, 1, 0)), 0018c770(human, 3), 0x0055c780[client] = 0x0055d2f0[client] = 0,
// 001314f0(0x004ffb50, client), 003fb750(client), the NetMap 0x0055c7a0's entry for +0xc erased (001b3170 /
// 001b3120 -> 001be870), 0040b720(*0x004ffd44, human), 003cb230(human, 0.1f), the virtual +0x60(human, 1), on a
// host (or outside a network game) 002ec2a8(*(+0x3584 + 0x24)) when +0x3584 is set, 00257040(client); returns 1.
std::uint32_t human_left_check_001b2e90(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t human,
                                        std::uint32_t stack = 0x1f00000);

// The multiplayer record (132ln5h): with 0x005721c8 set the player's input (00247d60, 0024b5d0) writes the local
// human's network block (+0x3584, the NetMap object the field replication sends): the input caches +0x13c (the
// counters, block + 0x1c, 0 while the block is missing) and +0x140 (the block). Block: byte +0 the aim mode (1: a
// point at +0x10..+0x18), +4 the locked object's +0x10, +8 its id, byte +0xc the subtarget; counters: u16 +0 the
// shots, u16 +2 the shots replayed (the receiver's), byte +5 the lock flag, s16 +6 the held item.
// 002493c0(input, actor): +6 = the held item (inventory +0x3524: slot byte +0x84, 6 nothing (-1), else slot +8).
void net_record_weapon_002493c0(LevelLoadMemory& m, std::uint32_t input, std::uint32_t actor);
// 00249420(input, actor, -, mode): +5 = mode (when the counters exist), then 00249460.
void net_record_aim_mode_00249420(LevelLoadMemory& m, std::uint32_t input, std::uint32_t actor, std::uint32_t mode);
// 00249460(input, actor): block byte +0 = 0.
void net_record_clear_point_00249460(LevelLoadMemory& m, std::uint32_t input, std::uint32_t actor);
// 00249490(input, actor, &point): block byte +0 = 1, +0x10..+0x18 = the point.
void net_record_point_00249490(LevelLoadMemory& m, std::uint32_t input, std::uint32_t actor, std::uint32_t point);
// 002494e0(input, actor, key, &id, subtarget): block +4 = key, +8 = *id, byte +0xc = subtarget.
void net_record_lock_002494e0(LevelLoadMemory& m, std::uint32_t input, std::uint32_t actor, std::uint32_t key,
                              std::uint32_t id, std::uint32_t subtarget);
// 00249530(input, actor): the shot count u16 +0 + 1.
void net_record_shot_00249530(LevelLoadMemory& m, std::uint32_t input, std::uint32_t actor);
// 002c4f20(counters): with the byte 0x0048c0c0 set (1 in the ELF: the first call) +2 = 0 and the byte cleared;
// then a shot not yet replayed (+2 < +0) is taken (+2 + 1, returns 1); else 0.
std::uint32_t net_counter_take_002c4f20(LevelLoadMemory& m, std::uint32_t counters);
// The remote human's controller (vtable 0x004de530, 002d85d0: +0x44 the network block, +0x48 its counters, +0x4c
// the human). 002d7d10(this): one replayed shot, 0x3b kind 0xb (vtable 0x004e0200) to the human with transport 0:
// +0x30 the locked id (-1 when aiming at a point), +0x34 the subtarget, +0x38 = 0, +0x40 the zero vector
// 0x004e1d90 (guard 0x004e1d88), +0x50 = 0.0f.
void remote_fire_002d7d10(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t self, std::uint32_t stack = 0x1f00000);
// 002d7ed0(this, target): the aim actions with transport 1. No block, no counters, or no point and no eligible
// target (+0x2d and +0x2c set, +0x2e clear): 0x3f and 0x39 kind 1 (vtable 0x004d9320, byte +0x2c = 0x0048c430). No
// point and no lock flag or target: the same with 0x0048c428. No point: 0x3e kind 5 (vtable 0x004e01a0, +0x2c the
// target's +0xc) and 0x39 kind 0xb (+0x30 the target's +0xc, +0x34 the subtarget). A point: 0x3f and 0x39 kind 4
// (vtable 0x004e0140, +0x30 the point with w 1.0). Then each shot 002c4f20 takes gives 002d7d10.
void remote_actions_002d7ed0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t self, std::uint32_t target,
                             std::uint32_t stack = 0x1f00000);
// 002d83c0(this) (the controller's virtual +0x40, its per-frame update): +0x44 / +0x48 filled from the human's
// +0x3584 while zero; the held item follows +6 (001439b0(inventory, item)); the locked object 0040c540(&id) when
// its virtual +0x108 does not answer 3 and it is eligible gets the aim update (virtual +0x60(this, it, item, -1));
// then 002d7ed0(this, it or 0).
void remote_controller_update_002d83c0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t self,
                                       std::uint32_t stack = 0x1f00000);
// 003cb4b0(this, type): the game object base: +0 = 0x004ddde0, +4 = 0xbebaafde (the live tag), +8 = type, +0xc and
// +0x10 = -1, byte +0x14 = 1, +0x18 = 0. Returns this.
std::uint32_t object_base_construct_003cb4b0(LevelLoadMemory& m, std::uint32_t self, std::uint32_t type);
// 003ea630(this): the aim base: 003cb4b0(this, 0x0055a840), +0 = 0x004e06c0, +0x20..+0x30 = 0 but +0x2c = -1, byte
// +0x34 = 0. Returns this.
std::uint32_t aim_base_construct_003ea630(LevelLoadMemory& m, std::uint32_t self);
// 00230240(this): the AI aim component's constructor (003ea630, the base), +0 = 0x004dba20, the skill +0x38 = 1.0f,
// +0x3c = -1, +0x40 = 0. Returns this.
std::uint32_t aim_component_construct_00230240(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t self);
// 002d85d0(this, block, human): the remote human's controller (vtable 0x004de530; 002c6b70 makes it): 00230240,
// +0x44 = block, +0x48 = 0, +0x4c = human, h +0x50 = 0; with the human's network block +0x3584 set, +0x44 (when
// zero) = that block and +0x48 = its +0x1c (the counters). Returns this.
std::uint32_t remote_controller_construct_002d85d0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t self,
                                                  std::uint32_t block, std::uint32_t human);

// ---- cNetHuman (class 28, 0x58 bytes, callbacks 002d1f00 / 002d1dc0 / 002d1d50), the team data (0016a580 /
// 0016a550 / 0016a450) and the GenInfo object's create / remove (00439560 / 00438c20). Checkpoint 132ln5h.
// 00316980(data, character) / 002c4ea0(counters, character): the character's controller +0x58 (when set) by its
// type (virtual +0x88 & 0xff): 4 (the remote human's) +0x44 = data / +0x48 = counters; 2: +0x1e0 / +0x1e4.
void net_human_link_block_00316980(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t data, std::uint32_t character);
void net_human_link_counters_002c4ea0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t counters,
                                      std::uint32_t character);
// 00316a00(p): the record block's reset: +0 = +1 = 0, +4 = +8 = -1, +0xc = 0xff, +0x10 / +0x14 / +0x18 = 0.
void net_record_init_00316a00(LevelLoadMemory& m, std::uint32_t p);
// 00316a30(p, mode): its release: 00100740(p, mode) when p is set and (s16)mode > 0. Returns p.
std::uint32_t net_record_release_00316a30(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t p, std::uint32_t mode);
// 003fbb10(id): the NetMap (0x0055c7a0) entry's object slot (node + 0x10) when the id has one with an object set.
std::uint32_t net_map_object_003fbb10(LevelLoadMemory& m, std::uint32_t id);
// 002d1ff0(this, id): in a networked level (byte 0x005721d0) the local human's cNetHuman: a 0x58-byte block on the
// frame (+0x30; 00316a00 on it, +0x2c = id, +0x29 = 1, +0x30 = h 4, +0x38 = 1000.0f) created by 002eaf28(&handle,
// 2, 4, 0, 0, 0, 0, block; "cNetHuman", *0x0051f078); created: byte 0x0051f080 = 1, 002ec3c8(handle) and
// 0x0051f088 + 1. Then 00316a30(block, 0). Frame 0x90 below `stack`.
void net_human_create_002d1ff0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t self, std::uint32_t id,
                               std::uint32_t stack);
// 002d1dc0(owner, handle, mode): the owner callback. For -2 (the local client took it) and the object owned by
// the local client with 0x0051f080 set: data +0x24 = handle, +0x28 = 1; with +0x29 the character 0040c540(+0x2c)
// is linked (002c4ea0 / 00316980, +0x3584 = data) and 0x0051f080 = +0x29 = 0. Frame 0x50.
void net_human_owner_002d1dc0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t owner, std::uint32_t handle,
                              std::uint32_t mode, std::uint32_t stack);
// 002d1f00(master, handle): the create callback (a cNetHuman another client announced): data +0x29 = +0x28 = 0,
// +0x24 = handle; the character 0040c540(+0x2c) linked as above, or the NetMap entry's object slot (003fbb10, or
// 003fb650(id, master, 0) adds it) +0x10 = data. Frame 0x50.
void net_human_created_002d1f00(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t master, std::uint32_t handle,
                                std::uint32_t stack);
// 002d1d50: the remove callback (the re-entry byte set and cleared).
void net_human_removed_002d1d50(LevelLoadMemory& m);
// 002d1d80(data): the per-frame ownership request: with +0x28 clear, 002ec3c8(+0x24); 0 sets +0x28.
void net_human_send_002d1d80(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t data);
// 002d2110(data): the ownership release (132ln5h13): 002eb650(+0x24); when the object's owner +4 is the local slot,
// or -1 on the host (or outside a network game), 002ec460(+0x24); 0 clears +0x28. Frame 0x40.
void net_human_release_002d2110(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t data, std::uint32_t stack);
// 0016a580(master, handle): the team data object created: the old block 0x004ee620 (when the game's own, byte
// 0x004ee628 clear) freed ("ai.hh" 0x1fe); 0x004ee620 = the object's data, 0x004ee628 = 1. Frame 0x40.
void team_data_created_0016a580(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t handle, std::uint32_t stack);
// 0016a550: the owner callback (the re-entry byte set and cleared).
void team_data_owner_0016a550(LevelLoadMemory& m);
// 0016a450: removed: the game's own block freed ("ai.cc" 0x6fe); 0x004ee620 = 0, 0x004ee628 = 0, 0x0048a100 = 1.
void team_data_removed_0016a450(LevelLoadMemory& m, LevelLoadHost& host);
// 00439560(master, handle): the GenInfo object created: the old block 0x00583888 freed ("GenInfoObject.cc"
// 0x234); 0x00583888 = the object's data, byte 0x00583880 = 1, 0x00497578 = handle. Frame 0x40.
void geninfo_created_00439560(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t handle, std::uint32_t stack);
// 00438c20: removed: 0x00583888 = 0, 0x00583880 = 0.
void geninfo_removed_00438c20(LevelLoadMemory& m);

// ---- cNPCInfoObject (class 30: 00433160 / 00432230 / 004321d0), HeadsetSemaphore (00437830 / 00437640 /
// 004375d0), the NetMap client 003fb4b0 and the NPC info lookups 004361c0 / 004341f0. Checkpoint 132ln5h.
// 00433160(master, handle): the NPC info object created: the first of the five slots whose received byte
// (0x00582780 + 8k) is clear takes the object's data (0x005827f0 + 8k) and handle (0x00582a78 + 8k); slot 0 also
// sets 0x00582778. The old block is freed ("NPCInfoObject.cc" 0x629 / 0x635 / 0x640 / 0x64a) but slot 0's, which a
// joiner merges (00435f30 / 00434600, not ported: thrown by name) and frees (0x620). Frame 0x60 below `stack`.
void npc_info_created_00433160(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t handle, std::uint32_t stack);
// 00432230(owner, handle, mode): the owner callback: with slot 1's block, the object, -2 and the local client the
// owner: 004355d0 when the blocks are not created, 0x00582a68 raised, the six queues drained into the object's
// block (00432b10 / 00432f50 / 00432910 / 00432750 / 00432d00 / 00432510 with (sp+0x48, data, handle)), the
// local client's entries whose character (0040c540) has a controller (+0x58) whose virtual +0x44 is false are
// cleared (004361c0: id -1, owner 0xff, interest 0), the matching slot's dirty byte (0x005827a0 + 8k) set and
// 0x00582a68 lowered. Frame 0x50.
void npc_info_owner_00432230(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t handle, std::uint32_t mode,
                             std::uint32_t stack);
// The owner's drains (a0 the token; the block `data`; the object's handle). Queued copies are 0x14 bytes: +0 id,
// +4 owner, +5 next owner, +6..+9 interest, +0xa..+0xd alive, +0x10 the handle; the copies are freed
// ("NPCInfoObject.cc" at the given lines) and their nodes erased.
// 00432b10: the adds 0x00582af0 (all handles): an id not yet known (004361c0 with sp+0x7c) takes the block's first
// free entry (id, bytes +4..+9; line 0x6ec); without one the copy stays queued. Frame 0x80.
void npc_info_drain_adds_00432b10(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t data, std::uint32_t stack);
// 00432f50 / 00432910: the interest set 0x00582ab0 / clear 0x00582ad0: the entry's local interest byte 1 / 0
// (lines 0x683 / 0x71e).
void npc_info_drain_interest_set_00432f50(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t data,
                                          std::uint32_t handle);
void npc_info_drain_interest_clear_00432910(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t data,
                                            std::uint32_t handle);
// 00432750: the owners 0x00582b10: the entry's +4 / +5 (line 0x74f).
void npc_info_drain_owners_00432750(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t data, std::uint32_t handle);
// 00432d00: the alive map 0x00582b50 (node +0xc the copy, +0x10 the value): the entry's alive byte +0xa + local
// (line 0x6b6; the node erased with the name 0x004bfd10).
void npc_info_drain_alive_00432d00(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t data, std::uint32_t handle);
// 00432510: the host's owner changes 0x00582b30: owner +4 (next owner 0xff) or, with no owner, the next owner;
// the copy's +5 client's interest cleared (line 0x78b).
void npc_info_drain_owner_changes_00432510(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t data,
                                           std::uint32_t handle);
// 00434aa0(a0, src) / 00435070(a0, src): the copy of src (0036b180 "NPCInfoObject.cc" 0x30b / 0x2b5) queued on the
// adds / the interest set; 00435070 also frees (0x2c6) the interest clears of the same id.
void npc_info_queue_add_00434aa0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t src);
void npc_info_queue_interest_00435070(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t src);
// 00434e20(a0, src) (checkpoint 132ln5h14): the mirror of 00435070: the copy (0x2d5) queued on the interest clears,
// the interest sets of the same id freed (0x2e6) and erased.
void npc_info_queue_uninterest_00434e20(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t src);
// 00434d10(a0, src): the copy of src (0x30b's sibling, line 0x2f4) queued on the owners 0x00582b10.
void npc_info_queue_owner_00434d10(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t src);
// 00434600(a0, idp, owner): in a network game 004355d0(a0); only on the host: the entry of *idp (004361c0, handle
// at sp+0x5c) whose owner (+4; a negative owner is 0xff) changes: +5 = the old owner, +4 = the new one, its copy at
// sp+0x40 (handle +0x10) queued (00434d10) and the object flushed (00434920). Frame 0x60.
void npc_info_set_owner_00434600(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t a0, std::uint32_t idp,
                                 std::uint32_t owner, std::uint32_t stack);
// 004344a0(a0, key, owner): 00434600(a0, &key, owner) with the key at sp+0x1c. Frame 0x20.
void npc_info_set_owner_key_004344a0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t a0, std::uint32_t key,
                                     std::uint32_t owner, std::uint32_t stack);
// 00434920(a0, handle): the object (002eb650) owned by another client marks the slot's sending byte
// (0x005827c8 + 8k); else the owner callback 00432230(local, handle, -2). Frame 0x30.
void npc_info_flush_00434920(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t handle, std::uint32_t stack);
// 00435f30(a0, idp, client): the spawned NPC's entry: 004355d0(a0); once the blocks are released (0x00582780) the
// entry (id, owner client, no next owner, no interest, alive) is queued (00434aa0) and the five objects flushed
// (00434920); before, the known entry (004361c0) or the first free one of the five blocks takes the id. Frame 0x60.
void npc_info_add_00435f30(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t a0, std::uint32_t idp,
                           std::uint32_t client, std::uint32_t stack);
// 00434440 / 00434380(a0, idp): 004355d0(a0); the entry's owner (+4) / next owner (+5), -1 for none or 0xff.
std::uint32_t npc_info_owner_of_00434440(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t a0, std::uint32_t idp);
std::uint32_t npc_info_next_owner_00434380(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t a0, std::uint32_t idp);
// 004340c0(a0, idp): 1 when another client than the local one is interested in the entry.
std::uint32_t npc_info_others_interested_004340c0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t idp);
// 00434190(a0, idp, s): client s's (0..3) interest byte of the entry, else 0.
std::uint32_t npc_info_interested_00434190(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t idp, std::uint32_t s);
// 00433ec0(info, &id, slot) (checkpoint 132ln5h13): the found entry's contact byte +0xa + slot (0 when not found or
// the slot is outside 0..3). 0014bff0 compares it with the NPC's +158 every network frame.
std::uint32_t npc_info_contact_00433ec0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t idp, std::uint32_t s);
// 00433f20(info, &id, value) (checkpoint 132ln5h13): in a network game, 004355d0 then 004361c0(&id, &handle); found:
// a stack copy (frame 0x70, at +0x50) of the entry's +0..+0xd with its local-slot contact byte = value and +0x10 =
// the handle, the entry's own byte set too, then 00434be0(info, copy, value) and 00434920(info, handle).
void npc_info_set_contact_00433f20(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t a0, std::uint32_t idp,
                                   std::uint32_t value, std::uint32_t stack);
// 00434be0(info, src, value) (checkpoint 132ln5h13): a 0x14-byte copy ("NPCInfoObject.cc" line 0x300; fields only
// when allocated) inserted with the value byte into the map 0x00582b50 by 00436dd0 -> 00437180 (node line 0x5c of
// 0x004bfd00) unless already a key. Frame 0x60.
void npc_info_queue_alive_00434be0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t src, std::uint32_t value,
                                   std::uint32_t stack);
// 004321d0: removed: the five block pointers and the five bytes 0x00582778 + 8k cleared.
void npc_info_removed_004321d0(LevelLoadMemory& m);
// 00437830(master, handle): the headset semaphore created: 0x00583870 = handle, 0x00582cf0 = its data. Frame 0x30.
void headset_created_00437830(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t handle, std::uint32_t stack);
// 00437640(owner, handle, mode): for -2 with the local client the owner: the talk request (byte 0x00583860:
// 00325960 on the device, 0x00582d20 = 1) or the release (002ec460 of 0x00583870 when the local client owns it,
// 0x00582d28 = 1; 00325a28 and the 2000 ms timer 0x00582d08 when byte 0x00582b88). Frame 0x30.
void headset_owner_00437640(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t handle, std::uint32_t mode,
                            std::uint32_t stack);
// 004375d0: removed: 0x00582cf0 = 0.
void headset_removed_004375d0(LevelLoadMemory& m);
// 003fb4b0(key, client): the NetMap entry of the key (003fbb10) gets the client (+4), but for a character
// 0040c540(key) with a controller and byte +0x49 == 2 whose type (virtual +0x88) is 1 or 2: the NPC info object
// 0x0055c988 (made once: 0036b180 "NetObj.cc" 0x20c, 00436c00) takes the owner through 004344a0(obj, key, client).
void net_map_set_client_003fb4b0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t key, std::uint32_t client,
                                 std::uint32_t stack);
// 004361c0(idp, out): the NPC info entry (16 bytes, 0x1d per block) whose id is *idp, the blocks interleaved by
// entry; out (when set) = the block's handle (-1 when none). Returns the entry or 0.
std::uint32_t npc_info_find_004361c0(LevelLoadMemory& m, std::uint32_t idp, std::uint32_t out);
// 004341f0(a0, idp): in a network game 004355d0(a0), then the entry of *idp (004361c0 with its handle at sp+0x5c):
// the local client's interest set in the entry and in its copy at sp+0x40 (handle +0x10), queued (00435070) and the
// object flushed (00434920). Frame 0x60 below `stack`.
void npc_info_apply_004341f0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t a0, std::uint32_t idp,
                             std::uint32_t stack);
// 00433ce0(a0, idp) (checkpoint 132ln5h14; 001b5190's network part): 004355d0(a0), then the entry of *idp
// (004361c0 with its handle at sp+0x5c), not gated on a network game. In a running level (0x005721d0) the local
// client's interest cleared in the entry and in its copy at sp+0x40 (handle +0x10), queued (00434e20) and the object
// flushed (00434920); before it, only the entry's byte cleared. Frame 0x60 below `stack`.
void npc_info_clear_interest_00433ce0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t a0, std::uint32_t idp,
                                      std::uint32_t stack);

// ---- the actor messages and the locomotion replication (NET_INLEVEL.md 4.14) ----
// 003c9e30(msg, type): the message base (vtable 0x004dfd80, +4 type, +0x18 / +0x1c 0, byte +0x20 = 5); in a
// network game in the level +0xc = the clock 0042b1a0(0).
std::uint32_t message_construct_003c9e30(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t type);
// 003c9d60(msg, flag): the base destructor (00100740 when (s16)flag > 0).
std::uint32_t message_destroy_003c9d60(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t flag);
// 00429cb0(delay): the latency average (0x005723e8 count, 0x005723e0 total seconds, 0x005723d8 mean) for 0..999 ms.
void net_latency_00429cb0(LevelLoadMemory& m, std::uint32_t delay);
// 003c9db0(msg, type, buf): the received message base: +0xc = buf word >> 2 (the sender's clock), +0x18 = its
// low 2 bits (the transport), +0x14 = the key buf+4; the latency 00429cb0(clock - +0xc).
std::uint32_t message_construct_received_003c9db0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                                  std::uint32_t type, std::uint32_t buf);
// 0014d270(msg, flag): the locomotion message's destructor (vtable 0x004e0100 +8).
std::uint32_t move_message_destroy_0014d270(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t flag);
// 003e5d70(msg): its clone (+0xc): 0x60 bytes ("character.cc" 0x40e), the base, action and +0x30..+0x58 words.
std::uint32_t move_message_clone_003e5d70(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg);
// 003e57b0(msg, key, typeptr, clock, t0, cap, buf): its serialiser (+0x10): nothing (-1) when the position equals
// the last one sent (0x0055a228, created on first use); else the 0x28-byte record: 003ff330's header, the
// position, the velocity, s16 heading * 8192 (+0x24), u16 speed * 32768 (+0x26). The ground-relative position
// (+0x58 with +0x50 / +0x54) throws 003e5a48. The last message is replaced by the clone.
std::uint32_t move_message_serialise_003e57b0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                              std::uint32_t key, std::uint32_t typeptr, std::uint32_t clock,
                                              std::uint32_t t0, std::uint32_t cap, std::uint32_t buf);
// 003fb020(key, msg, client, a3): serialise (virtual +0x10 into a 0x400 frame buffer) and send by the message's
// flags (+0x20: 0x15 to a3, 8 to the client, 2 to the host from a client) through 003fad50. Returns size != 0.
// Frame 0x470 below `stack`.
std::uint32_t net_message_send_003fb020(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t key, std::uint32_t msg,
                                        std::uint32_t client, std::uint32_t a3, std::uint32_t stack);
// 003c9420(msg, target, transport): the actor message send: the immediate delivery (transport 1, flag 1), the
// clone, the network send (003fb020) by flags 4 / 8 / 2 / 0x20 and the local queue 003ca0e0 or the clone's release.
// Frame 0x100.
std::uint32_t actor_message_send_003c9420(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                          std::uint32_t target, std::uint32_t transport, std::uint32_t stack);
// 003c9350(msg, target, transport): the local delivery (immediate for transport 1, else the queued clone). Frame 0x50.
std::uint32_t actor_message_deliver_003c9350(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                             std::uint32_t target, std::uint32_t transport, std::uint32_t stack);
// 003e4430(.., buf): the locomotion receive: the message rebuilt from the 0x28-byte record and delivered to the
// key's object (003fbb10, 003c9350). A ground-relative record (u16 +0x26 = 0xffff) throws 003e4520. Frame 0x130.
std::uint32_t move_message_receive_003e4430(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t buf,
                                            std::uint32_t stack);
// 001770f0(controller, dt): the player controller's network update: every 250 ms (+0x1c0) the locomotion message
// (position, velocity, heading, speed 1, ground) to the human through 003c9420; then 0024b5d0 / 0024aaf0 / 00261540.
// Frame 0x140.
void player_net_update_001770f0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t self, std::uint32_t f12,
                                std::uint32_t stack);

// ---- the human's network section and the animation fields (checkpoint 132ln5h4; NET_INLEVEL.md 4.15) ----------
// 001b2d20(human): the held weapon against the network block's counters (+0x1c)'s +6. A remote human (+0x14 == 0)
// first finds its block through 003fbb10(+0xc) and switches to the sent weapon with 001439b0(inventory, weapon);
// the local human writes its held weapon (0xffffffff in slot 6) into the counters.
void human_weapon_sync_001b2d20(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t human);
// 001b1710's network part 001b173c..001b1bec (frame 0x340; runs only when 005721c8 is set). Remote: the block's
// health (+0x38) through 003cc990(human, health, 1), the killed flag +0x35 into +0x33a5 and the flashlight +0x52
// through 0045b610. Local: the health, the flashlight and +0x33a5 into the block. Then 001b2d20, the controller
// tails (vtable +0x5c: 002d2110 / the leave print; +0x58: the block's clock +0x54 and 002d1d80; +0x54: 001b2e90)
// and, for a remote human, the animation fields: block +0x50 bits 1 / 2 into +0x3248 bits 0 / 1, bits 4 / 8 /
// 0x20 into +0x31b3 / +0x31b4 / +0x31b5, +0x51 (0xc / 0xb / else) into +0x3248 bits 31 / 30, and action
// message 0x43 kind 1 (payload 0 when bit 0x40 is set) to the human through 003c9420(msg, human, 1).
void human_network_section_001b1710(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t human, std::uint32_t stack);

// ---- the generators' cEnableMsg (vtable 0x004e0880) / cDisableMsg (0x004e0860) (checkpoint 132ln5h11) -----------
// Destructors (+8): the own vtable, 003c9d60(msg, 0), 00100740 when (s16) flag > 0.
std::uint32_t disable_message_destroy_0016fc70(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t flag);
std::uint32_t enable_message_destroy_0016fd90(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t flag);
// Clones (+0xc): 0x28 bytes (0036b180 name 0x004be5a0 / 0x004be5e0, line 0x21), the base words and +0x24.
std::uint32_t disable_message_clone_003ff3b0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg);
std::uint32_t enable_message_clone_003ff690(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg);
// Serialisers (+0x10)(msg, key, typeptr, .., cap t1, buf t2): 003c9ca0's header, buf +8 = +0x24, *typeptr the type
// word 0x0055d4a0 / 0x0055d4c0; 0xc (no null checks).
std::uint32_t disable_message_serialise_003ff570(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                                 std::uint32_t key, std::uint32_t typeptr, std::uint32_t cap,
                                                 std::uint32_t buf);
std::uint32_t enable_message_serialise_003ff850(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                                std::uint32_t key, std::uint32_t typeptr, std::uint32_t cap,
                                                std::uint32_t buf);
// Receives (.., buf a3): in the level, the key's object (003fbb10) gets its virtual +0x2c (disable) / +0x28
// (enable)(object, buf +8); 0xc.
std::uint32_t disable_message_receive_003ff5d0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t buf);
std::uint32_t enable_message_receive_003ff8b0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t buf);
// Sends: in a network game 003fb020(key, msg, -1, a2) / 003fb020(key, msg, -1, -1).
void disable_message_send_003ff650(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t key,
                                   std::uint32_t a2);
void enable_message_send_003ff930(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t key);
// 003ce850(object, outdist): the local player (0040bf10), or the client player (003fb8d0, clients 0..3) whose
// position (virtual +0xc0(out, p, -1)) is nearest the object's below 1e8 (squared); *outdist the distance when set.
// Frame 0x90.
std::uint32_t nearest_player_003ce850(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t object,
                                      std::uint32_t outdist, std::uint32_t stack);

// ---- the generator's spawn message (vtable 0x004de240, type 0x0051f058, serial type 0x0051f060; 132ln5h12) -----
// +0x30 the position (w 1), +0x40 the angles, +0x50 the NPC's key (+c), +0x54 its +10, +0x58 the generator's
// health (+0x68), +0x5c the serial (the generator's draw), +0x60 the node (-1 for none).
// 0021df80(msg, flag): the destructor (+8). 002d0ee0(msg): the clone (+0xc; 0x70 bytes, 0x004ad520 line 0x22).
std::uint32_t spawn_message_destroy_0021df80(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                             std::uint32_t flag);
std::uint32_t spawn_message_clone_002d0ee0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg);
// 002d1100(msg, key, typeptr, .., cap t1, buf t2): the 0x34-byte record: 003c9ca0's header, +8 the keys and health,
// +0x14 the angles, +0x20 the position, +0x2c the serial, +0x30 the node; -1 for cap < 0x34, no buf or no typeptr.
std::uint32_t spawn_message_serialise_002d1100(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                               std::uint32_t key, std::uint32_t typeptr, std::uint32_t cap,
                                               std::uint32_t buf);
// 002d1290(msg, buf): the message rebuilt from the record (003c9e30 type 0x0051f058; +0x4c the frame's unwritten
// word below `stack`). Frame 0x50.
std::uint32_t spawn_message_build_002d1290(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                           std::uint32_t buf, std::uint32_t stack);
// 002d11d0(.., buf a3): the receive: in the level the message (002d1290 at sp+0x30) delivered to the generator of
// the key (003fbb10, 002d1360, 003c9350(msg, object, 0, 0)) and destroyed (003c9d60); 0x34. Frame 0xb0.
std::uint32_t spawn_message_receive_002d11d0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t buf,
                                             std::uint32_t stack);

// ---- the generator's despawn message (vtable 0x004de260, type 0x0051f068, serial type 0x0051f070; 132ln5h14) ---
// 0021f6c8 (the owner's despawn of a spawned NPC): +0x1c = 0x40, byte +0x20 = 4, +0x2c the NPC's key (+c), +0x30
// its +10; 003c9420(msg, generator, 0). The generators' handler 00221610 at 002216c8 takes it on the other consoles.
// 00220350(msg, flag): the destructor (+8). 002d15c0(msg): the clone (+0xc; 0x34 bytes, 0x004ad570 line 0x1a).
std::uint32_t despawn_message_destroy_00220350(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                               std::uint32_t flag);
std::uint32_t despawn_message_clone_002d15c0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg);
// 002d1790(msg, key, typeptr, .., cap t1, buf t2): the 0x10-byte record: 003c9ca0's header, +8 / +0xc the keys;
// -1 for cap < 0x10 or no buf; without typeptr the record is written and -1 returned.
std::uint32_t despawn_message_serialise_002d1790(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                                 std::uint32_t key, std::uint32_t typeptr, std::uint32_t cap,
                                                 std::uint32_t buf);
// 002d1810(.., buf a3): the receive: in the level the message (003c9e30 type 0x0051f068 at sp+0x40, the keys from
// the record) delivered to the NetMap node of buf +4 (001bb080; 002d1360's +4: 003c9350(msg, object, 0, 0)) and
// destroyed (003c9d60); 0x10. Frame 0x90.
std::uint32_t despawn_message_receive_002d1810(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t buf,
                                               std::uint32_t stack);

// ---- the generator message "Generator Message" (vtable 0x004db600, type 0x004a4a68; 132ln5h13, 4.23) -----------
// The AI's Deactivate (00157c3c) and the remote NPC controller's (002d4554) send it to the generator of +150: +0x24
// the actor, byte +0x28 = 1 (00221610: GenInfo 0043b190). The base vtable is 0x004db660 (cGeneratorMessageBase).
// 00157d20(msg, flag): the destructor (+8: the vtables 0x004db600, 0x004db660, 003c9d60, 00100740 when flag > 0).
std::uint32_t generator_release_message_destroy_00157d20(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                                         std::uint32_t flag);
// 00222410(msg): the clone (+0xc; 0x2c bytes, 0x004a4c88 line 0xe3): the header words, +0x24 and byte +0x28.
std::uint32_t generator_release_message_clone_00222410(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg);
// 0021d370(msg, key, typeptr, .., cap t1, buf t2): the 0x14-byte record: 003c9ca0's header, +0xc the actor's key
// (+0xc), byte +8 = 3 (kind), halfword +0x10 = byte +0x28; *typeptr = [0x004f56c8]. -1 for cap < 0x14 or no buf;
// without typeptr the record is written and -1 returned.
std::uint32_t generator_release_message_serialise_0021d370(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                                           std::uint32_t key, std::uint32_t typeptr, std::uint32_t cap,
                                                           std::uint32_t buf);
// 0021d6c0(.., buf a3): the receive: in the level, for the NetMap node of buf +4 (001bb080 on 0x0055c7a0), a
// "GeneratorBase Message" (003c9e30 type 0x004a4a70; +0x24 the actor 0040c540(&buf +0xc)) of the kind byte buf +8
// (0 vtable 0x004db660, 1 0x004db640, 2 0x004db620, 3 0x004db600 with byte +0x28 = halfword buf +0x10) delivered to
// the node's object +0x10 (003c9350(msg, object, 0, 0)) and destroyed (base vtable, 003c9d60); 0x14. Frame 0x120.
std::uint32_t generator_base_message_receive_0021d6c0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t buf,
                                                      std::uint32_t stack);

// ---- an actor's NetMap registration and its NPC info owner (checkpoint 132ln5h12; NET_INLEVEL.md 4.22) ---------
// 004343e0(obj, key): 004355d0(obj), 004361c0(&key, 0): the entry's owner byte +4, -1 if missing or 0xff. Frame 0x20.
std::uint32_t npc_info_owner_004343e0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t obj, std::uint32_t key,
                                      std::uint32_t stack);
// 003fbc40(key): 1 if the NPC info owner of the key (004343e0 on [0x55c988]) is the local slot (0 solo).
std::uint32_t npc_owned_locally_003fbc40(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t key);
// 003fb340(key): the NetMap client of a key (entry +4; -1 without one); for an object +0x49 == 2 whose controller's
// vt+0x88 answers 2 or 1 the NPC info owner instead (making [0x55c988] once: 0036b180, 00436c00).
std::uint32_t net_map_client_003fb340(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t key);
// 001856d0(actor), an actor's vt+0xfc: +0x10 = +0xc; a human controller (vt+0x54) or a locally owned key (003fbc40)
// of an active actor sends its object (002d1ff0); then the NetMap insert 003fb650(+0x10, local slot, actor).
void actor_net_register_001856d0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t actor);

}  // namespace omega
