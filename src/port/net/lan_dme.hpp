#pragma once
// PORT-ONLY (not in the original; LAN phase 3, checkpoint 132ln5): LanDme, the stand-in for the DME public API
// that the game's network code calls (research/NET_INLEVEL.md section 3, LAN_PLAY.md section 5.2 option M as
// refined by 132ln5). The game side (DME.cc 0x004292b0.., NetObj, NetObjectMgr, the message and object
// registrations) runs unchanged; each of its calls into the DME library goes through one of ~45 locked API
// wrappers at 0x002e9320..0x002ecf08, and those are what LanDme answers, by original address, on a LanTransport
// (the star through the hosting peer).
//
// Implemented so far (each one's contract is what the game reads; NET_INLEVEL.md section 3 has the evidence):
//   002ebaf8(out, world)        NetGetMyClientIndex (DME 002dd178): world +0x400 (132fx5; PORT-ONLY before the
//                               barrier creates world 0: LanTransport::local_index)
//   002ebba0(world, out)        the session host's client index (002dd1f8) -> 0, the LanTransport host
//   002ebc48(out)               the local millisecond clock (002ddac8 / 00306018)
//   002ebce0(out)               the session clock (002ddb08: 0x0058806c) -> the local clock plus the JOIN_ACK
//                               offset on a client
//   002eabf0(params)            the start parameters' defaults (002e1500: 0x38 bytes zeroed, +0 1, +4 -1, +0x1c 2)
//   002eac88(params, out)       the level start barrier (002e1578 / 002e15e0), below
//   002ec4f8()                  the update (002e4a30): the transport poll, the barrier's steps and the callbacks
//   002ec580(out)               the per-world connection status (002e4c48: 0x18 bytes from 0x00520340, filled by
//                               002e47b0; 132ln5h): world 0's link status, 0 while the link to the host holds
//   002eb9a8(out, world, client) the client's 16-byte record head (002dd4e8) -> the client's name
//   002ea0a0(kind, world, to, category, id, size, data) the game message send (gate, lock, 002de398) -> a channel
//                               0x02 frame (send_message, below; checkpoint 132ln5f)
// Every other DME API address throws, naming it. Each wrapper opens with the library gate (0x11 / 0x2b), as the
// original's 002ea508 .. 002ecf08 do (132fx5).
//
// The level start barrier (static decode of DME 002e15e0 / 002e8730 / 002e6f40 / 002e5d68 / 002e6c78 /
// 002db720; the DME's own messages are replaced by LanDme control frames):
//   - start: the caller's client record goes to state 3 and every connected peer gets START_READY (DME's
//     internal message *0x005983e4, payload the four bytes {3, client, +0x14, +0x18}: the counts' low bytes);
//     alone, the caller goes straight to 5 and is the world's session host (002e5550: +0x404).
//   - the host (parameters +0x1c 1 or 2), in each update: when no connected client is in state 0, 4 or 5 and the
//     lowest client in state 3 is the host itself, it goes to 5, announces itself (CLIENT_INFO), calls the start
//     callback with status 0 and sends START_GO to every client in state 3 (002e8c50).
//   - a client: START_GO from the host puts it in state 4 and it announces itself; a START_GO reaching a host
//     calls the start callback with status 7 (002e6f40).
//   - in each update, once the caller is in state 4 or 5 and no connected client is in state 0, the start
//     callback (if not called yet) gets status 0 (002e5d68).
//   - CLIENT_INFO from a client while the caller is in state 4 or 5 calls the joined-the-game callback
//     {0, world, client, +0x2c} once per client (002e6c78 -> 002db720). DME drops an early one (0x24); LanDme
//     keeps it until the caller has started (static inference: the original's resend is not decoded), and
//     delivers one per update, and none in an update that called the start callback (checkpoint 132ln5h,
//     PORT-ONLY pacing: 00429c00 and 003faab0 each toggle a joiner's barrier state).
//   - a late joiner (checkpoint 132ln5h, PORT-ONLY): a started client answers its START_READY with CLIENT_INFO,
//     which the original's joiner has from the world join's member list (002e0118, not ported).
// The callbacks are game functions (00429c00, 003faab0): LanDme writes their argument block to `scratch` in the
// game's EE memory and calls `invoke(function, block)`.
//
// The world and the client records (checkpoint 132ln5c, NET_INLEVEL.md section 4.4). The DME library's own
// tables are native (src/main/DME/dme_library.cpp); LanDme calls them through `library(address, args)` (the
// game's LevelLoadHost: LanNetCore). What the original's lobby and protocol leave in them, LanDme does at the
// matching step, with the library functions where the original calls one:
//   - the barrier (002eac88): world 0 (002e51f0; PORT-ONLY stand-in for the lobby's world join 002dfba0..002e0e2c,
//     not ported: +0x400 the local client index, +0x438 1, +0x444 kLanMaxClients), then 002e15e0's own steps:
//     the parameters into 0x0058df88 / 0x0048c478 / 0x0058df8c / 0x0048c47c / 0x0048c488 / 0x0048c48c /
//     0x0058df90, 0x0048c450 = the world, 0x0048c464 = 0x0048c45c = 0, and the local record 002db270 with
//     +8 = (u16) +0x14, +6 = 0x1000, +0xa = 0x800, +0xc = (u16) +0x18, +0xe the name (strncpy 0xb), +0x19 0.
//   - the start (alone 002e1918, the host 002e8730, a client receiving START_GO 002e6f40): +6 / +0xa the handle
//     bases (0 / 0 alone and on the host; the message's on a client, whose +0 is the message clock and
//     0x0058806c too), 0x0048c454 = 1, then 002db688: +4 = 0x1000 and 002db470 (the record's handles in use).
//   - the host's START_GO to each client in state 3 (002e8c50): unless the host already has that client's record,
//     a new record (002db270), +6 = 002e68c0(the client's +0x14) and +0xa = 002e69b8(its +0x18) when nonzero, and
//     START_GO {0x0058806c, +6, +0xa}.
//   - each update writes the session clock to 0x0058806c (the original's update 002e4a30 keeps it there).
//   - world +0x404, the session host (002e5550): 0x100 from the barrier's world (the world join's value), the
//     local client at the host's start (002e8730) or the alone start (132fx5).
//   - the joined-the-game step (002db720): the joined client's record (002db270 where there is none), +0 = the
//     session clock word, the info's +0x29 = 1 and 002db470, before the callback (132fx5).
// The client-record broadcast 002db550 / receive 002e6c78 and the join-order message *0x00598424 are carried as
// control kinds 4 and 5. The latter also fills the PORT-ONLY world-member list omitted with 002e0118, so every
// peer has the +0x24 ordering that 002dbbf8 would use. Host replacement itself remains unavailable because
// LanTransport's star topology ends a client session when client 0 disappears. 002e1470 is the transport and
// 002e8c50's three 002e1d50 pings are represented by LanTransport's clock synchronization.
//
// Control frames (DATA payloads, reliable): {0x01, kind, client, payload}; kind 1 START_READY {u16 +0x14,
// u16 +0x18}, 2 START_GO {u32 session clock, u16 base, u16 second base}, 3 CLIENT_INFO {}, 4 CLIENT_RECORD
// {the exact 0x24-byte 002db550 payload}, 5 JOIN_ORDER {u16 client, u32 order}. Little-endian. Byte 0
// is the LanDme channel: 0x01 DME control, 0x02 game messages (132ln5h3: 132ln5g's PORT-ONLY channel 0x03 is
// used only for the game-side backpack bridge described below; locomotion travels as the original's game message,
// research/NET_INLEVEL.md 4.14).
//
// Game messages (checkpoint 132ln5f; static decode of 002ea0a0 / 002de398 / 002e64e0 / 002def08 / 00305a48):
//   - send_message(kind, world, to, category, id, size, data) is 002de398 (002ea0a0 after its gate; the native
//     library's 002de880 calls it directly): its checks (category < 6, id < 0x100, size > 0, data: else 0x17; a
//     world other than 0: 0xf), then the frame {0x02, origin, category, id, data[size]} to `to` (-1 / 0xffff: every
//     other client). The send mode `kind` (a sign-extended byte) picks the delivery as Net_TranslateSendFlags does:
//     0x40 reliable and ordered, 0x10 sequenced, 0x80 unreliable. PORT-ONLY: one frame per message (DME splits a
//     message of 0x1ff bytes or more into 0x1ea-byte pieces, which one LAN frame of kLanMaxPayload holds); a
//     message to the local client or past client 3 gives 0x17, an unconnected client 8.
//   - received in update() in the arrival order with the control frames, through 00305a48's loop: per message the
//     category and id bytes (each below the registry's 0x0052980c / 0x00529808), the callback from the registry
//     table 0x004912f0 (00305c40: [(category * max + id) * 4]), its counter (0x004912f8) + 1, the rest of the data
//     copied to the registry buffer *0x004912ec, then callback(0, world +0x43c (002dd280), origin, buffer) through
//     `library`; it returns the bytes it consumed. Where DME returns 7 (no registry, a bad id, no callback, a
//     negative or oversized consumption) LanDme throws, naming the address.

#include "level_load.hpp"
#include "lan_dme_commit.hpp"
#include "lan_transport.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <vector>

namespace omega::lan {

class LanDme {
public:
    using Invoke = std::function<void(LevelLoadMemory& memory, std::uint32_t function, std::uint32_t arg)>;
    using Library = std::function<std::uint32_t(LevelLoadMemory& memory, std::uint32_t address,
                                                std::initializer_list<std::uint32_t> args)>;
    LanDme(LanTransport& transport, LanClock clock, Invoke invoke, Library library, std::uint32_t scratch = 0x01ff0000);

    static bool is_dme_api(std::uint32_t address) { return address >= 0x2e9320 && address <= 0x2ecf08; }
    std::uint32_t call(LevelLoadMemory& memory, std::uint32_t address, std::initializer_list<std::uint32_t> args);

    // Diagnostics for the tests and the log.
    std::uint8_t state(std::uint16_t client) const { return client < kLanMaxClients ? state_[client] : 0; }
    bool started() const { return started_; }
    // The native world's assigned client slot survives a transport close.
    // LanTransport resets its handshake index to zero when the socket closes.
    std::uint16_t local_index() const { return local_index_; }
    // The client's record in world 0 (002db188), 0 when it has none.
    std::uint32_t record(LevelLoadMemory& memory, std::uint16_t client);
    std::uint32_t joined_callbacks() const { return joined_calls_; }
    // 002de398 (see the header); also the native library's 002de880 (category 0) through LanNetCore.
    std::uint32_t send_message(LevelLoadMemory& memory, std::uint32_t kind, std::uint32_t world, std::uint32_t to,
                               std::uint32_t category, std::uint32_t id, std::uint32_t size, std::uint32_t data);
    std::uint32_t messages_sent() const { return messages_sent_; }
    std::uint32_t messages_received() const { return messages_received_; }
    // Game messages a callback refused (00305a48's 7: the rest of the message dropped), the last one's callback
    // (its category in the top byte) and origin.
    std::uint32_t refused_messages() const { return refused_; }
    std::uint32_t last_refused() const { return last_refused_; }
    std::uint16_t last_refused_origin() const { return last_refused_origin_; }
    // Only while a registered game-message callback runs: bytes remaining
    // after its category/type pair in the current DME frame. A standalone
    // direct callback has no frame bound (0xffffffff).
    std::uint32_t incoming_message_remaining() const { return incoming_message_remaining_; }

    // PORT-ONLY transport for cNetBackpackMsg's exact observable six-byte payload, plus the target's LAN key.
    // The native message class is still applied by CombatPlay; this channel replaces only NetMap routing and the
    // unported generic cNetMsg table entry.
    struct BackpackMessage {
        std::uint16_t origin{};
        std::uint8_t type{}, owner{};
        std::uint32_t backpack{}, target{};
    };
    void send_backpack(std::uint8_t type, std::uint8_t owner, std::uint32_t backpack, std::uint32_t target,
                       std::uint16_t recipient = kLanAll);
    std::vector<BackpackMessage> take_backpacks();

    // The world's client-left callback (checkpoint 132ln5g): DME calls the world create parameters' +0x24
    // (00429920 from 0042a870 / 00429790, which LanSession replaces) with the block {0, client} when a client of the
    // world leaves; LanDme makes the call in update(), after the poll's frames, for each client the transport lost
    // (a BYE or the timeout) that had reached the barrier. 0: no call.
    void set_client_left_callback(std::uint32_t function) { left_cb_ = function; }
    std::uint32_t left_callbacks() const { return left_calls_; }
    // 002ec6a0 (left the world) / 002ec738 (disconnected it) were called (132ln5h7, 0042b1f0).
    bool left_world() const { return left_; }
    bool disconnected() const { return disconnected_; }
    // PORT-ONLY host-migration groundwork. The live message path does not envelope events yet: the play must first
    // provide the post-application state checkpoint described in research/LAN_PLAY.md §5.5.
    LanDmeCommitLog& application_commits() { return application_commits_; }
    const LanDmeCommitLog& application_commits() const { return application_commits_; }

private:
    void send_control(std::uint16_t to, std::uint8_t kind, std::initializer_list<std::uint32_t> words = {});
    void start_records(LevelLoadMemory& memory, std::uint32_t params);
    void activate(LevelLoadMemory& memory, std::uint32_t base, std::uint32_t base2);
    void start_go(LevelLoadMemory& memory, std::uint16_t client);
    void send_client_record(LevelLoadMemory& memory, std::uint16_t to);
    void receive_client_record(LevelLoadMemory& memory, std::uint16_t origin, const std::vector<std::uint8_t>& bytes);
    void send_join_order(LevelLoadMemory& memory, std::uint16_t to, std::uint16_t client, std::uint32_t order);
    void receive_join_order(LevelLoadMemory& memory, const std::vector<std::uint8_t>& bytes);
    std::uint32_t member(LevelLoadMemory& memory, std::uint16_t client);
    std::uint32_t next_join_order(LevelLoadMemory& memory);
    std::uint32_t update(LevelLoadMemory& memory);
    void control(LevelLoadMemory& memory, std::uint16_t origin, const std::vector<std::uint8_t>& bytes);
    void game_message(LevelLoadMemory& memory, std::uint16_t origin, const std::vector<std::uint8_t>& bytes);
    void sync_callback(LevelLoadMemory& memory, std::uint32_t status);
    void joined(LevelLoadMemory& memory, std::uint16_t client);
    void client_left(LevelLoadMemory& memory, std::uint16_t client);
    void newly_joined(LevelLoadMemory& memory, std::uint16_t client);
    void set_session_host(LevelLoadMemory& memory, std::uint32_t client);
    std::vector<std::uint16_t> connected() const;  // the session's clients, the local one included
    std::uint32_t local_clock() const;
    std::uint32_t session_clock() const;

    LanTransport& transport_;
    const std::uint16_t local_index_;
    LanClock clock_;
    LanDmeCommitLog application_commits_;
    Invoke invoke_;
    Library library_;
    std::uint32_t scratch_;
    std::array<std::uint8_t, kLanMaxClients> state_{};      // the barrier state per client (DME record +0x14)
    std::array<bool, kLanMaxClients> joined_{};               // the joined-the-game callback was made
    std::array<bool, kLanMaxClients> info_pending_{};         // CLIENT_INFO received before the local start
    std::array<std::uint16_t, kLanMaxClients> counts_{}, counts2_{};  // START_READY's +0x14 / +0x18 per client
    bool barrier_ = false;                                    // 002eac88 was called
    bool started_ = false;                                    // DME 0x0048c454
    bool all_ready_ = false;                                  // DME 0x0048c440
    bool left_ = false, disconnected_ = false;                // 002ec6a0 / 002ec738
    std::uint32_t host_flag_ = 0;                             // parameters +0x1c (DME 0x0058df90)
    std::uint32_t sync_cb_ = 0, sync_user_ = 0;               // +0x20 / +0x24 (DME 0x0058df88 / 0x0048c478)
    std::uint32_t joined_cb_ = 0, joined_user_ = 0;           // +0x28 / +0x2c (DME 0x0058df8c / 0x0048c47c)
    // [I] 003144c8's code when world 0 has no connection (0x00491432 <= 0); any nonzero other than 0xc posts
    // GAME_DISCONNECT in 00429d30's walk.
    static constexpr std::uint32_t kLinkLost = 0x11;
    std::uint32_t link_status_ = 0;  // world 0's transient 002e47b0 status, cleared at each update
    std::uint32_t joined_calls_ = 0;
    std::uint32_t left_cb_ = 0, left_calls_ = 0;
    std::uint32_t messages_sent_ = 0, messages_received_ = 0;
    std::uint32_t refused_ = 0, last_refused_ = 0;
    std::uint16_t last_refused_origin_ = 0;
    std::vector<BackpackMessage> backpacks_;
    std::uint32_t incoming_message_remaining_ = 0xffffffffU;
};

}  // namespace omega::lan
