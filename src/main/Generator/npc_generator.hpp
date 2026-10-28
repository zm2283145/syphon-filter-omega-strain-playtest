#pragma once
// NPC generators (cGenerator, vtable 004db680, 0xd4 bytes, constructor
// 00221d20; local/handoff/analysis/agent_generator_spawning.md): the single
// player state, the script natives' field writes, the spawn check 0021edf0
// and the reject prefix of its spawn test 002204c0. Interpreter goldens:
// tests/generator_spawn_goldens.hpp (tools/ee/gen_generator_spawn.py).
// The per-frame tick (traversal 00133420: +4c = the prelogic 0021f0a0, then
// +48 = 0021edf0 behind the +2d / +2c / +2e gate) and the zone presence
// (00224330 / 00224180) are hosted; prelogic goldens:
// tests/generator_tick_goldens.hpp (tools/ee/gen_generator_tick.py).
// The generator's side of the spawn 0021e620 (the draw, the call order, the
// +90 / +9c insertion) is hosted; spawn goldens:
// tests/generator_spawn_actor_goldens.hpp (gen_generator_spawn_actor.py).
// The actor it creates (001b7b00 with 0015c170's matrix) and SpawnedNPCMsg
// delivery are hosted by omega_play (GENERATOR_RUNTIME.md "Checkpoint 119b";
// GetNPC: spawned_npc_msg.hpp). The spawn test's view cone and line of sight
// (002207d0..00220b64) are; goldens: tests/generator_spawn_view_goldens.hpp
// (gen_generator_spawn_view.py).

#include "animation_root.hpp"
#include "population.hpp"
#include "target_visibility.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace omega {

// A node of the generator's lists +90 / +9c (header: count +0, sentinel +4
// whose +4 is the first node; node +0 prev, +4 next, +8 actor, +c handle id).
// The node resolves when the actor is set and its +c equals the handle id.
// Actors are host keys (0: a null actor pointer).
struct GeneratorListEntry {
    std::uint32_t actor{};   // node +8
    std::uint32_t id{};      // node +c
    bool operator==(const GeneratorListEntry&) const = default;
};

struct GeneratorState {
    std::uint32_t id{};                  // +0c/+10: (category << 16) | index
    std::uint8_t enabled_2c{};           // +2c: the record byte; Enable / Disable
    std::uint8_t present_2d{};           // +2d: zone presence (generator_zone_activate / _deactivate)
    std::uint8_t suspended_2e{};         // +2e: traversal gate input (00224330 clears it)
    // +7c[0..+78): the spawn nodes as population node indices; -1 where the
    // record's ID is not a population node (the constructor's 0040c480
    // lookup is not hosted: using such a slot throws).
    std::vector<std::int32_t> spawn_nodes;
    float interval_84{};                 // +84, seconds
    std::int32_t alive_cap_88{};         // +88
    std::int32_t total_cap_8c{};         // +8c
    float min_distance_sq_b8{};          // +b8
    float timer_bc{};                    // +bc
    std::int32_t spawned_c0{};           // +c0: spawned total
    std::int32_t cursor_c4{};            // +c4: node cursor
    std::int32_t last_c8{};              // +c8: cursor of the last spawn
    std::uint8_t forced_d0{};            // +d0: forced spawn test result (writer not traced)
    std::uint8_t paused_d1{};            // +d1
    std::uint8_t no_refund_d2{};         // +d2: record byte (version >= 0x3e)
    // Multiplayer (005721c8 set; tests/generator_network_goldens.hpp): +cc the
    // clients interested in the generator (bit per client, 0xff: all), +d3 the
    // owner's "was active" byte the prelogic's hand-off reads.
    std::uint32_t interest_cc{};         // +cc
    std::uint8_t handoff_d3{};           // +d3
    // +90: the spawned actors (00222980 from 0021e8fc); +9c: those whose
    // SpawnedNPCMsg is still to be sent (0021e940). Both empty at load
    // (00222b60 x2 in 00221d20).
    std::list<GeneratorListEntry> spawned_90;
    std::list<GeneratorListEntry> pending_9c;
};

// The traversal gate of 00133420 / the activity test the prelogic inlines:
// +2d and +2c set, +2e clear.
bool generator_active(const GeneratorState& s);

// The multiplayer calls of the generator's code (cGenerator 004db680 with
// 005721c8 set), named by address. The GenInfo natives (GenInfoObject.cc) take
// the generator's key (+c, a pointer to it in the original); the general-info
// object 0043b190 constructs is not passed. Interpreter goldens:
// tests/generator_network_goldens.hpp (tools/ee/gen_generator_network.py).
struct GeneratorNetHost {
    bool network{};                                                         // 005721c8
    bool host_role{};                                                       // 005721c0
    // *00494178 (002ebaf8 fills it when -1): read where the original builds the
    // local slot (only in a network game; 0 otherwise, without the call).
    std::function<std::int32_t()> local_slot;
    std::function<void()> geninfo_43b190;                                   // 0043b190 -> 00439670
    std::function<bool(std::uint32_t key)> owned_43aa40;                    // entry owned by the local client
    std::function<std::int32_t(std::uint32_t key)> owner_439c70;            // the entry's owner, -1 none
    std::function<bool(std::uint32_t key)> claimed_43ab20;                  // the entry has an owner
    std::function<bool(std::uint32_t key, std::int32_t client)> interest_43aba0;
    std::function<void(std::uint32_t key, std::int32_t owner)> add_4398e0;
    std::function<void(std::uint32_t key, std::int32_t owner)> set_owner_439d00;
    std::function<void(std::uint32_t key)> clear_interest_43a030;
    std::function<void(std::uint32_t key)> set_interest_43a420;
    // 002ebba0(*005721a8, &out): the session host's client (the out word; nullopt: not written).
    std::function<std::optional<std::int32_t>()> session_host_2ebba0;
    std::function<void(std::uint32_t key, std::int32_t client)> netmap_insert_3fb650;  // a2 = the generator
    std::function<std::int32_t()> clock_42b1a0;                             // 0042b1a0(1)
    // 003ce850(gen, 0) for the generator with this key: the nearest present
    // player's +c word (nullopt: none).
    std::function<std::optional<std::uint32_t>(std::uint32_t key)> nearest_player_3ce850;
    std::function<bool(std::int32_t client)> client_player_3fb8d0;          // 003fb8d0(client) != 0
    // cEnableMsg (003c9e30 key 0055d4b8, vtable 004e0880, +24 = a1, +1c = 0x40,
    // +20 = 4) through 003ff930(msg, +10); cDisableMsg (0055d498, 004e0860)
    // through 003ff650(msg, +10, -1); each destroyed (003c9d60) after. The
    // key is the generator's (+10).
    std::function<void(std::uint32_t key, std::int32_t a1)> send_enable_3ff930;
    std::function<void(std::uint32_t key, std::int32_t a1)> send_disable_3ff650;
};

// 00221d20 for one GEN: record: +2c, +7c, +84, +88, +8c and +d2 from the
// record, then (00222320..00222348) +c0 = +c4 = 0, +c8 = +c4, +d0 = 0,
// +bc = +84, +d1 = 0, +b8 = 256.0f (0x43800000). +2d / +2e start clear.
GeneratorState make_generator_state(const PopulationGenerator& record, std::span<const PopulationNode> nodes);

// The natives' single player effects (the multiplayer byte 005721c8 clear).
// Enable 0021cf40, EnableAnon 0021cc60, EnableBy 0021cdd0: virtual +64(-1) =
// 00220fd0, +2c = 1 (00220ff0). Disable 0021cf10, DisableAnon 0021cc30,
// DisableBy 0021cc90: virtual +68(-1) = 00220d70, solo branch 00220e40 +2c = 0.
void generator_enable(GeneratorState& s);
void generator_disable(GeneratorState& s);
// The generator's native receiver 00221610 (vtable 004db680 +0x0c) for a
// type-1/6/7 hotbox link message (00172ae0 / 00172450 / 00171f30 / 00172088),
// after the script filter: the key at the message's +4 (003c9c20) against
// cEnableMsg 0055d4b8 (00221a9c) -> virtual +64(+24), cDisableMsg 0055d498
// (00221b24) -> virtual +68(+24); single player both ignore +24 (00220ff0 /
// 00220e40). Returns 00221ba0's v0 (1).
bool generator_receive_link_message(GeneratorState& s, bool enable);
void generator_pause(GeneratorState& s);        // 0021cb90: +d1 = 1
void generator_unpause(GeneratorState& s);      // 0021caf0: +d1 = 0
void generator_reset_pulse(GeneratorState& s);  // 0021c9a0: +c0 = 0
// 0021ca30: +b8 = d * d (mul.s, stored at 0021ca58). Throws where binary32
// and the EE FPU can differ: a non-finite or denormal d, or a product that
// overflows or is denormal.
void generator_set_min_spawn_distance(GeneratorState& s, float d);

// One entry of the spawned list +90 (nodes: +8 actor, +c handle id) as
// 0021edf0 and 002204c0 read it.
struct GeneratorSpawnedEntry {
    bool valid{};                // the actor pointer is set and its +c matches the node's +c
    std::uint8_t life_108{};     // actor virtual +108, low byte
    RootVector translation{};    // actor virtual +f4 matrix row +30 (002204c0 only)
};
// 0021ee50..0021eecc: entries that resolve and whose +108 low byte is not 3
// (an entry that does not resolve is skipped without the call).
std::int32_t generator_alive_count(std::span<const GeneratorSpawnedEntry> spawned);
// 0014b220: entries of the think list 004ea190 whose +108 low byte is
// neither 2 nor 3. A nullopt entry does not resolve: the original calls
// +108 on a null pointer, so it throws.
std::int32_t generator_thinking_count(std::span<const std::optional<std::uint8_t>> think_life_states);

// The calls 0021edf0 makes, in its order.
struct GeneratorSpawnHost {
    std::function<std::int32_t()> alive_count;                         // 0021ee50..0021eecc (every call)
    std::function<std::int32_t()> thinking_count;                      // 0014b220
    std::function<std::int32_t()> max_active_npc_count;                // *004ea350 (0021ef38)
    std::function<std::uint8_t(std::int32_t node)> spawn_test;         // 002204c0, low byte
    // 0021e620(gen, node, cursor) up to its bookkeeping 0021e94c.
    std::function<void(std::int32_t node, std::int32_t cursor)> spawn;
    // 0042b1a0(1) (002ebc48): -1 unless the network library's bytes 0048c4d7
    // and 0048c4dc are 1, else its locked query. nullopt: not known to the
    // host (the stamp is then unknown).
    std::function<std::optional<std::int32_t>()> clock_42b1a0;
    // 132ln5h12: 0021e968..0021ea70, in a network game after the bookkeeping: the spawn message of the actor just
    // created (003c9e30 type 0051f058, vtable 004de240, 003c9420(msg, gen, 0), 003c9d60). Required there.
    std::function<void()> send_spawn_msg;
};
// 0021e94c..0021e964, the end of 0021e620's single player path: +c0 += 1,
// +bc = 0, +c8 = +c4.
void generator_spawn_bookkeeping(GeneratorState& s);

// What 0021e620 hands the actor constructor 001b7b00 (0021e840..0021e888)
// besides the generator's template words (+64 manifest, +68, &+6c, &+a8,
// role 2, which the host reads from the GEN: record): a2 = the spawn node
// (target +78, the actor's +3548; -1 when idx < 0, where the target is the
// generator itself and 0015c170 builds the matrix from it), the stack pair
// {&gen+0c, +c0} (+c0 before the bookkeeping) and the 00127dd0 draw. a1 is
// the 0015c170 matrix of the target (node +8c yaw, row 3 = node +80).
struct GeneratorSpawnRequest {
    std::int32_t node{};             // a2 (+3548)
    bool generator_target{};         // idx < 0: 0015c170 of the generator itself
    std::uint32_t generator_id{};    // *(gen+0c)
    std::int32_t spawned_c0{};       // +c0
    std::uint32_t random{};          // the draw at 0021e660
};
// The calls 0021e620 makes for the actor it creates (single player).
struct GeneratorActorHost {
    std::function<std::uint32_t()> random;                                 // 00127dd0 (0021e660, first)
    // new 0x35a0 (004a4a90 line 0x73f), 0015c170, 001b7b00: the actor (a
    // nonzero host key; the null block of a failed allocation is not modelled).
    std::function<std::uint32_t(const GeneratorSpawnRequest&)> create;
    std::function<void(std::uint32_t actor)> register_actor;               // +3598 = 1, +1c = *004ffd44, 0040b790
    std::function<void(std::uint32_t actor)> activate;                     // virtual +5c(1) (001b53d0)
    std::function<std::uint32_t(std::uint32_t actor)> actor_id;            // +c, read after the activation
    // 132ln5h12: in a network game (005721c8; 0021e740..0021e7bc) right after the creation: the actor's virtual
    // +fc(1) (001856d0) and its NPC info entry (00436c00, 00435f30(obj, &+c, the local slot), 00436ba0(obj, -1)).
    // Empty in single player.
    std::function<void(std::uint32_t actor)> network_register;
};
// 0021e620(gen, target, idx) single player up to 0021e948: the draw, the
// creation, the registration and activation, then {actor, +c} appended to
// +90 (00222980 from 0021e8fc) and to +9c (0021e944), each with its own read
// of +c. `node` is the target's +78 (ignored when cursor < 0). The
// bookkeeping (generator_spawn_bookkeeping) follows in 0021e620 itself.
void generator_spawn_actor(GeneratorState& s, std::int32_t node, std::int32_t cursor, const GeneratorActorHost& host);
// 0021edf0 (single player), the +48 slot. `spawn_stamp_4f56b8` receives the
// write at 0021f030..0021f044 after a spawn (read only by the multiplayer gate
// 0021ef10). A generator without spawn nodes (target = the generator itself,
// 0021efa4) or a cursor on an unresolved node throws. An unknown clock
// leaves the stamp unknown (nullopt). With `net` in a network game, the gate
// 0021eeec..0021ef28 after +d1: the local client owns the GenInfo entry
// (0043aa40) and the stamp is before the clock (slt *004f56b8, 0042b1a0(1));
// an unknown stamp there throws.
void generator_spawn_check(GeneratorState& s, float dt, const GeneratorSpawnHost& host,
                           std::optional<std::int32_t>& spawn_stamp_4f56b8, const GeneratorNetHost* net = nullptr);

// 002204c0 up to its view cone (single player): the forced byte +d0 (1 is
// cleared and returned, any other nonzero value is returned as is), the
// occupancy loop over +90 (an entry that does not resolve throws, life 3 is
// skipped, a live actor whose translation is closer than 3 (d2 < 9.0f) to
// the spawn position rejects, without leaving the loop early) and the local
// player distance (d2 < +b8 rejects). Returns the low byte the caller sees,
// or nullopt where the view cone 002207d0 decides
// (generator_spawn_test_view). `spawn_position` is 0015c2c0 of the node (+80, w = 1);
// `player_translation` the player's transform +40 after its refresh (+58
// when the +50 byte is set).
std::optional<std::uint8_t> generator_spawn_test_prefix(GeneratorState& s, const RootVector& spawn_position,
                                                         std::span<const GeneratorSpawnedEntry> spawned,
                                                         const RootVector& player_translation);

// The calls the rest of 002204c0 makes (002207d0..00220b64, single player), in
// its order. Interpreter goldens: tests/generator_spawn_view_goldens.hpp
// (tools/ee/gen_generator_spawn_view.py).
struct GeneratorSpawnViewHost {
    // 00159560(004ffc14) (the list accessor *(a0 + 8) + 8) -> the camera
    // object, its +50 transform refreshed (virtual +58) when the transform's
    // +50 byte is set, read at `offset`: 0x40 (the translation, matrix row 3)
    // or 0x20 (matrix row 1). One call per read, in the order 0x40, 0x20 (the
    // view cone), then 0x40, 0x20 (the line of sight).
    std::function<RootVector(std::uint32_t offset)> camera_transform;
    std::function<std::uint64_t()> local_player;                        // *(*004ffc0c + 30) (00220934): request +50
    // 003caa50(004ffb50, 004ea0b0) (the LOS manager) virtual +0c (00138a10 ->
    // 0013a480) on the request (class 005435a0, vtable 004d9180); only the
    // result byte +0 is read.
    std::function<LosServiceResult(const LosServiceRequest&)> los;
};
// 002207d0..00220b80, after a prefix that did not decide (+d0 = 0; the
// result is 1 = spawn). The view cone: the horizontal direction from the
// camera translation to the spawn position (z = 0, normalized by VRSQRT)
// against the camera's matrix row 1 with z = 0 (dot = (x + y) + z in the w
// lane); with c = cosf(0x3f5f66f3) (00121688, about 50 degrees), dot +
// (1 - c) < 1.0 (sub.s, add.s, c.olt.s) is outside the view: 1 without a
// line of sight. Inside, the line of sight from the spawn position raised by
// 6.0 (add.s) to the camera translation + 1.5 * row 1 (vmulx, vadd, vsub,
// all lanes): a hit (result byte nonzero) is 1, a clear view 0. A result of
// 1 clears +d0 (00220b78). Throws where the radicand is 0 (the spawn
// straight above or below the camera: VRSQRT's divide-by-zero result is not
// hosted) and on non-finite arithmetic; a missing host function throws.
std::uint8_t generator_spawn_test_view(GeneratorState& s, const RootVector& spawn_position, const GeneratorSpawnViewHost& host);

// The calls the prelogic 0021f0a0 makes on a spawned actor (single player
// path 0021fee0..0022034c), in its order. Each is made on a resolved actor.
struct GeneratorPrelogicHost {
    std::function<std::uint32_t(std::uint32_t actor)> actor_id;             // actor +c (the handles' resolution)
    // +2d and +2c set, +2e clear; read fresh at each of the three tests.
    std::function<bool(std::uint32_t actor)> actor_active;
    // 0022015c: actor +58 (nullopt: null), else its virtual +44 != 0.
    std::function<std::optional<bool>(std::uint32_t actor)> component_alive_44;
    std::function<void(std::uint32_t actor)> activate_5c;                   // actor virtual +5c(1)
    std::function<void(std::uint32_t actor)> deactivate_60;                 // actor virtual +60(1)
    std::function<std::uint8_t(std::uint32_t actor)> life_108;              // actor virtual +108, low byte
    // 00220038..00220098: SpawnedNPCMsg (003c9e30 key 004a4a80, vtable
    // 004db640, +20 = 1, +24 = the actor) sent to the generator through
    // 003c9420(msg, gen, 0), then destroyed (vtable 004db660, 003c9d60).
    std::function<void(std::uint32_t actor)> send_spawned_npc_msg;
    std::function<void(std::uint32_t actor)> unregister_40b720;             // 0040b720(*004ffd44, actor): the traversal array
    std::function<void(std::uint32_t actor)> script_drop_3d9f40;            // 003d9f40(00555070, actor)
    std::function<bool(std::uint32_t actor)> object_magic;                  // actor +4 == 0xbebaafde
    std::function<void(std::uint32_t actor)> release_3cb230;                // 003cb230(actor, 1.0f)
};
// 0021f0a0 (single player; the multiplayer byte 005721c8 clear; dt unused).
// For every resolving node of +90 (the next node taken before the body, the
// end re-read): the first +9c node resolving to the same actor gets its
// SpawnedNPCMsg sent and is erased (00222880); then the activity sync (the
// generator active and the actor not: +5c(1) when +58 is set and its +44
// answers; the generator inactive and the actor active: +60(1)); then, the
// actor's activity read again, an inactive actor is despawned: +c0 -= 1
// unless its life is 2 / 3 or +d2 is set, the node erased, 0040b720,
// 003d9f40 and, for the object magic, 003cb230. After the loop: +d2, the
// generator inactive and +90 empty: +c0 = 0 (002202dc); +90 empty and +9c
// not: +9c cleared (00177f60).
void generator_prelogic(GeneratorState& s, const GeneratorPrelogicHost& host);

// 003cb230(object, f12 seconds): a 0x10-byte timer (0036b180), +0 =
// cvt.w.s(1000.0f * seconds) ms (003cb2d0..003cb2ec, truncation), +4 = 1
// (it counts the game clock, 003e9e30's a1), +5 = 1, +8 = vtable 004dfe20,
// +c = the object, queued by 003ea1d0 on the callback list 0055a2e0 (the
// flag 0055a2f8 is only ever stored 0). The prelogic passes 1.0f.
std::int32_t deferred_release_delay_ms(float seconds);
// Its fire 003cb4f0 (vtable 004dfe20 +10): when the object's +4 is still the
// object magic 0xbebaafde (written by the cObject constructors 003cb0a0 /
// 003cb120), 0040bf50 deletes it (virtual +8 with -1, then 0036b130) when its
// key +c is in the root's pending-removal vector (0040c120 over *00571740
// +14) or no longer in the tree (0040c080); a still-traversed object is kept.
bool deferred_release_deletes(bool magic_4, bool removal_pending, bool in_tree);

// 00224330 on a generator: +2e = 0 and, without a model (002244d0: none),
// virtual +5c(0) = 002214b0 -> 003ce980: +2d = 1 (+58 is null: 003cf0e0
// clears it, 00221d20 does not set it; the rest of 002214b0 is
// multiplayer); +2d = 1.
void generator_zone_activate(GeneratorState& s);
// Virtual +5c = 002214b0 (single player, the argument unused: 002214b4)
// -> 003ce980: +2d = 1 (+58 null). The constructor's call for a generator
// whose region names no object (002222b0 / 0022225c): present for good.
void generator_activate_5c(GeneratorState& s);
// 00224180 on a generator: when forced, 00220bb0 cGenerator::DeactivateImmediate
// (Generator.cc:1128, matched C; Checkpoint 132fx1): for each +90 node in
// order, its actor (the handle resolved: an actor whose +c differs is null,
// and the original calls through it) gets virtual +60(1) (Deactivate) and the
// generator a DespawnedNPCMsg (key 004a4a88, vtable 004db620, +24 the actor,
// +20 = 2, transport 0), both by `deactivate_immediate` (without it a forced
// deactivation with spawned NPCs throws); then 003ce790: +2d = 0. Unforced:
// virtual +60(0) = 00221460 -> 003ce790: +2d = 0.
void generator_zone_deactivate(GeneratorState& s, bool forced,
                               const std::function<void(const GeneratorListEntry&)>& deactivate_immediate = {});

// ---- Multiplayer (005721c8 set). Goldens: tests/generator_network_goldens.hpp.

// 0022234c..002223dc, the constructor's tail in a network game: +d3 = 0, +cc
// = the record's enable byte ? 0xff : 0, the NetMap insert 003fb650(+10, the
// session host's client 002ebba0, gen), the host's GenInfo entry (0043b190,
// 004398e0(key, -1)) and the spawn stamp *004f56b8 = 0042b1a0(1) + 0xfa.
// Nothing in single player. A 002ebba0 that does not write its out word throws.
void generator_network_construct(GeneratorState& s, const GeneratorNetHost& net,
                                 std::optional<std::int32_t>& spawn_stamp_4f56b8);
// 00220fd0 (virtual +64, Enable; a1 -1 from the natives / the link message):
// +2c = 1; in a network game the active generator's owner (0043b190; a1 < 0:
// the nearest player's client 003ce850 or the local client, 00439d00; a1 the
// local client with +cc clear: 00439d00(local)), 0043a420; an inactive one
// with a1 < 0 hands the entry to the nearest player's client (-1 none) unless
// it is the local client; +cc |= 1 << a1 (a1 < 0: 0xff); the cEnableMsg.
void generator_enable_network(GeneratorState& s, std::int32_t a1, const GeneratorNetHost& net);
// 00220d70 (virtual +68, Disable): single player +2c = 0 (00220e40); network
// +cc &= ~(1 << a1) (a1 >= 0), +cc = 0: 0043b190, 0043a030, +2c = 0; the
// cDisableMsg.
void generator_disable_network(GeneratorState& s, std::int32_t a1, const GeneratorNetHost& net);
// 00220e60 (virtual +28, a remote client's cEnableMsg 003ff8b0): +2c = 1; the
// active generator: 0043b190, the client the local one with +cc clear:
// 00439d00(local), 0043a420; +cc |= 1 << client (client < 0: 0xff).
void generator_remote_enable(GeneratorState& s, std::int32_t client, const GeneratorNetHost& net);
// 00220d00 (virtual +2c, cDisableMsg 003ff5d0): +cc &= ~(1 << client) (client
// < 0: +cc = 0); +cc = 0: +2c = 0, 0043b190, 0043a030.
void generator_remote_disable(GeneratorState& s, std::int32_t client, const GeneratorNetHost& net);
// 002214b0 (virtual +5c) in a network game after 003ce980 (+2d = 1): the
// active generator: 0043b190, the local client's +cc bit set and no owner
// (0043ab20): 00439d00(local); 0043a420.
void generator_activate_5c_network(GeneratorState& s, const GeneratorNetHost& net);
// 00221460 (virtual +60) after 003ce790 (+2d = 0): network 0043b190, 0043a030.
void generator_deactivate_60_network(GeneratorState& s, const GeneratorNetHost& net);
// 00224330 / 00224180 with 002214b0 / 00221460's network parts.
void generator_zone_activate_network(GeneratorState& s, const GeneratorNetHost& net);
void generator_zone_deactivate_network(GeneratorState& s, bool forced, const GeneratorNetHost& net);

// The further calls of the prelogic's multiplayer path 0021f0dc..0021fed4 and
// of 0021eaa0, in their order.
struct GeneratorNetPrelogicHost {
    std::function<void()> npc_info_begin_436c00;                            // 00436c00 (the stack object)
    std::function<void()> npc_info_end_436ba0;                              // 00436ba0(.., -1)
    std::function<std::int32_t(std::uint32_t actor_id)> npc_owner_434440;
    std::function<void(std::uint32_t actor_id)> npc_apply_4341f0;
    std::function<bool(std::uint32_t actor_id)> npc_known_4340c0;
    // actor +58 (nullopt: null), else its virtual +58 / +5c != 0.
    std::function<std::optional<bool>(std::uint32_t actor)> component_58;
    std::function<std::optional<bool>(std::uint32_t actor)> component_5c;
    std::function<void(std::uint32_t actor)> actor_20;                      // actor virtual +20()
    std::function<void(std::uint32_t actor)> actor_24;                      // actor virtual +24()
    std::function<void(std::uint32_t actor)> activate_5c0;                  // actor virtual +5c(0)
    std::function<void(std::uint32_t actor)> deactivate_600;                // actor virtual +60(0)
    std::function<std::uint32_t(std::uint32_t actor)> actor_3584;           // actor +3584
    std::function<std::uint32_t(std::uint32_t actor)> actor_key_10;         // actor +10
    std::function<std::uint32_t(std::uint32_t key)> netmap_object_3fbb10;
    std::function<void(std::uint32_t actor, std::uint32_t object)> attach_185970;
    // The despawn message (003c9e30 key 0051f068, vtable 004de260, +2c = the
    // actor's +c, +30 its +10, +1c = 0x40, +20 = 4) to the generator, 003c9420(msg, gen, 0).
    std::function<void(std::uint32_t actor)> send_despawn_msg;
    // 0021eaa0's message (003c9e30 key 00582730, vtable 004d9300, byte +24 =
    // kind) to the actor, 003c9420(msg, actor, 1).
    std::function<void(std::uint32_t actor, std::uint8_t kind)> send_control_msg;
    std::function<void()> message_destroyed;                                // 003c9d60 of the stack message
    std::function<void(std::uint32_t actor, std::uint8_t value)> set_actor_14;
    std::function<std::int32_t(std::uint32_t actor_id)> netmap_client_3fb340;
    std::function<void(std::uint32_t actor_id, std::int32_t client)> netmap_set_client_3fb4b0;
};
// 0021eaa0: 0043b190; with an owner (0043ab20), the owner the local client:
// every resolving actor whose component answers +5c gets the control message
// 1, +14 = 1 and 003fb4b0(+c, owner); else those answering +58 the message 2,
// +14 = 0 and 003fb4b0.
void generator_owner_sync(GeneratorState& s, const GeneratorPrelogicHost& host, const GeneratorNetPrelogicHost& mp,
                          const GeneratorNetHost& net);
// 0021f0a0's multiplayer path (0021f0dc..0021fed4, then the common tail
// 002202e0): the per-actor sync against the GenInfo owner (s4 = 00439c70),
// the despawn messages of the owner, 0021eaa0, the owner's hand-off when the
// generator is inactive (+d3), the host's NetMap client fix-up, +c0 = 0 for
// +d2 without an owner, the departed clients' +cc bits. A node that does not
// resolve is erased in the first loop; one in the host's fix-up throws (the
// original reads +c of null).
void generator_prelogic_network(GeneratorState& s, const GeneratorPrelogicHost& host,
                                const GeneratorNetPrelogicHost& mp, const GeneratorNetHost& net);

// 132ln5h12: the spawn message (type 0051f058, vtable 004de240) as the generator's handler 0021e0c0 reads it.
struct GeneratorSpawnMessage {
    std::array<float, 4> position{};   // +30 (w 1)
    std::array<float, 3> angles{};     // +40 (001bd590 of the sender's matrix)
    std::uint32_t key_50{};            // the sender's actor +c
    std::uint32_t key_54{};            // its +10
    std::uint32_t health_58{};         // the generator's +68
    std::uint32_t random_5c{};         // the sender's 00127dd0 draw (the serial)
    std::int32_t node_60{};            // the spawn node (-1: none)
};
// The calls of 0021e0c0 for the actor it creates, in their order.
struct GeneratorSpawnReceiveHost {
    // 0021e0f0..0021e2a0: the matrix of the angles (00140e80 / 00140e70, 00140e20), row 3 the position; new 0x35a0
    // (004a4a90 line 0x777) and 001b8130 with the message's node, health, key and serial: the actor (nonzero).
    std::function<std::uint32_t(const GeneratorSpawnMessage&)> create;
    std::function<void(std::uint32_t actor)> register_actor;              // +1c = *004ffd44, 0040b790, +14 = 0
    std::function<void(std::uint32_t actor, std::uint32_t key)> set_key_185940;  // 00185940(actor, msg +54)
    std::function<void(std::uint32_t actor)> activate_20;                 // virtual +20 (an active generator)
    // 0021e380..0021e408 (an inactive generator): new 0x220 (line 0x791), 002d6030(it, 0, actor),
    // 001ae450(actor, 4, {&gen+c, +c0}, it).
    std::function<void(std::uint32_t actor)> remote_controller;
    std::function<void(std::uint32_t actor, std::int32_t client)> netmap_insert_3fb650;  // 003fb650(+10, client, actor)
    // 0021e458..0021e494 (the session host): 00436c00, 00434600(obj, &+c, owner), 00436ba0(obj, -1).
    std::function<void(std::uint32_t actor, std::int32_t owner)> npc_info_merge_434600;
    // 0021e498..0021e510: the message 0055d4e0 (vtable 004d9360, byte +24 = 2, +30 the matrix) to the actor,
    // 003c9350(msg, actor, 1, 0), 003c9d60.
    std::function<void(std::uint32_t actor)> place_msg;
    std::function<void(std::uint32_t actor)> attach_185970;               // 003fbb10(+10) -> 00185970 when found
    std::function<void(std::uint32_t actor)> mark_remote;                 // +3598 = 1, +58's +150 = gen +c
    std::function<std::uint32_t(std::uint32_t actor)> actor_id;           // +c
};
// 0021e0c0(gen, msg), the generator's spawn message handler (on the clients): the actor created and registered,
// +10 from the message; an active generator activates it (+c0 += 1), an inactive one (+c0 += 1 with +d2) gives it
// the remote controller and the NetMap entry of the GenInfo owner; the session host merges its NPC info entry;
// the place message, the attach, then {actor, +c} appended to +90 and +9c, +bc = 0, +c8 = +c4.
void generator_spawn_receive(GeneratorState& s, const GeneratorSpawnMessage& msg, const GeneratorSpawnReceiveHost& host,
                             const GeneratorNetHost& net);

// 132ln5h14: 00221610's despawn branch 002216c8..002219e8 (the message type [0051f068], the other console's
// despawn of a spawned NPC, NET_INLEVEL.md 4.24) for the actor 0040c540 found for its +2c (the caller returns 1
// without one): +60(0) when +58 answers +58, else +24; the first +90 node resolving to the actor (00220410: the
// resolved handles equal): with +9c's count > 0 the first such +9c node is erased (00222880), +c0 -= 1 unless the
// life (+108) is 2 / 3 or +d2 is set, the node erased; then, found or not, 0040b720, 003d9f40 and, for the object
// magic, 003cb230(actor, 1.0f). Returns whether the +90 node was found.
bool generator_despawn_receive(GeneratorState& s, std::uint32_t actor, const GeneratorPrelogicHost& host,
                               const GeneratorNetPrelogicHost& mp);

// The level's generators by population ID (Population::generators).
class GeneratorWorld {
public:
    explicit GeneratorWorld(const Population& population);
    GeneratorState* find(std::uint32_t id);
    const GeneratorState* find(std::uint32_t id) const;
    std::size_t size() const { return generators_.size(); }
    // Spawn node slots whose ID is not a population node.
    std::size_t unresolved_nodes() const;
    // GEN record order (001c2ae0 registers them in the traversal array in
    // file order).
    const std::vector<std::uint32_t>& order() const { return order_; }
    // The generators' entries of one traversal 00133420, in record order:
    // virtual +4c (the prelogic, `prelogic`), then, the gate read fresh,
    // virtual +48 (generator_spawn_check with dt and the host `spawn_host`
    // builds for it). With `net` in a network game, the prelogic's multiplayer
    // path (generator_prelogic_network with `mp`) and the spawn gate.
    void tick(float dt, const GeneratorPrelogicHost& prelogic,
              const std::function<GeneratorSpawnHost(GeneratorState&)>& spawn_host,
              std::optional<std::int32_t>& spawn_stamp_4f56b8, const GeneratorNetPrelogicHost* mp = nullptr,
              const GeneratorNetHost* net = nullptr);
    // A network game's calls (the host sets it once the generators' network constructor tails ran; empty in
    // single player): the Generator natives' Enable / Disable take their +64 / +68 network paths with it.
    std::function<GeneratorNetHost()> network_host;

private:
    std::unordered_map<std::uint32_t, GeneratorState> generators_;
    std::vector<std::uint32_t> order_;
};

}  // namespace omega
