#pragma once
// Human actor health, damage and kill (ACTOR_DAMAGE_AND_HEALTH.md): the life
// state 001872f0 (virtual +108), the damage virtual 00187360 (+100) with the
// base damage 003ccb00, and the kill virtual 00186bf0 (+10c). Single player
// only (005721c8 clear): the multiplayer branches of 003ccb00/003cc990 are not
// reproduced. Every outgoing message, sound, rumble and helper call is
// returned as an event in call order; nothing is played. The calls that
// return a value or run synchronously in the original (positions, the
// explosion LOS request, the hit-direction classifier 00186950, the
// controller's +84 and the immediate death action) go through the host, and
// an empty host function that is reached throws.
#include "animation_root.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

// Actor fields read and written by the damage path.
struct ActorHealthState {
    float health_40{};                   // +40 (root object field)
    std::int32_t max_health_3398{};      // +3398
    std::int32_t max_armor_339c{};       // +339c
    std::int32_t armor_33a0{};           // +33a0
    bool invulnerable_33a4{};            // +33a4
    bool killed_33a5{};                  // +33a5 (life state 3; lowers the fall thresholds)
    std::int32_t fire_damage_33b0{};     // +33b0 (type 4 accumulation)
    bool fire_immune_33b8{};             // +33b8
    bool burning_326a{};                 // +326a
    std::uint8_t gender_3260{};          // +3260 (1 selects the _F voice descriptors)
    bool operator==(const ActorHealthState&) const=default;
};
// 001b5fe0 (00062a0..62a8): +3398 = health word, +40 = (float)health word
// through 003cc990 (cvt.s.w), +339c = +33a0 = armor word; +33b8 = 0. The
// population record's first two parameter words are these two words. The
// agent-data unlocks (x1.5, player only, 001b5fe0 later) are not applied.
ActorHealthState make_actor_health(std::uint32_t health_word,std::uint32_t armor_word);
// 001ae450 (called by 001b5fe0 after those stores) for role 1, the local
// player: +339c = +33a0 = StartingArmor (0048a238, a registered tunable; 250
// in the boot image). The record's armor word therefore applies to role-2
// NPCs only (interpreter, damage_re/t5_role.py).
inline constexpr std::int32_t actor_starting_armor=250;
void apply_player_role_armor(ActorHealthState& state,std::int32_t starting_armor=actor_starting_armor);
// 001872f0: 3 with +33a5 or top action 0x1f, 2 with top action 0x1b,
// otherwise 3 when trunc(+40) == 0 (cvt.w.s), else 0.
std::uint8_t actor_life_state(const ActorHealthState& state,std::int32_t top_action_320c);

// The +58 controller's predicates: virtual +54 (player), +58 and +5c. The
// player component (vtable 004d9cf0) answers 1 / 0 / 0.
struct ActorDamageController {
    bool player_54{},virtual_58{},virtual_5c{};
};
// A damage source or the fire setter: identity (0 is null) and its +58
// controller (nullopt: no controller).
struct ActorDamageParty {
    std::uint64_t id{};
    std::optional<ActorDamageController> controller;
};

// Sound descriptors (bss, filled by static initializers), sent through
// 0020f990 as MSGGAME_PLAYSND to 004ffb50 with transport 1.
enum class ActorDamageSound : std::uint32_t {
    body_hit_player=0x004ef0b0U,     // B_HIT_PC_M
    body_hit_npc=0x004ef250U,        // B_HIT_NPC_M
    body_hit_kevlar=0x004ef730U,     // B_HIT_KEVLAR
    voice_player_hit_m=0x004f05d0U,  // V_PC_HIT_M
    voice_player_hit_f=0x004f0910U,  // V_PC_HIT_F
    voice_npc_hit_m=0x004eff50U,     // V_NPC_HIT_M
    voice_npc_hit_f=0x004f0290U,     // V_NPC_HIT_F
    voice_death_m=0x004f00f0U,       // V_NPC_DETH_M (also for the player)
    voice_death_f=0x004f0430U,       // V_NPC_DETH_F
};

struct ActorDamageEvent {
    enum class Kind {
        damage_message,      // MSGLIB_DAMAGE_REQUEST 005716f0 (vtable 004e08a0) to the actor, transport 0
        los_request,         // type 2: LOS Request 005435a0 to 004ffb50 (reply through the host)
        flak_notice,         // 00242fa0(004ffc2c, actor+c, "Flak Jacket", -1, 0, 0, 5.0)
        ignite,              // 00186360(actor, 1, 0, 0, 0, -1.0)
        death_message,       // action 0x42 kind 2 (0055d480, vtable 004e01e0, +1c 0x40); immediate: transport 1
        unconscious_message, // Script AI Unconscious Message 004ff2c0 (vtable 004dcc30, +20 2), transport 0
        item_notice,         // 00143770(+3524)
        controller_hit,      // controller virtual +78(source, amount)
        ai_flag_1d6,         // controller byte +1d6 = 1
        controller_death,    // 003ccac0: controller virtual +7c(amount = value, source = object, category_2d, type_2c)
        refill,              // invulnerable kill: 003cc990(actor, +3398, 0)
        release_60,          // 0038cbf0(actor + 0x60)
        sound,               // 0020f990 descriptor
        rumble,              // 0026bde0(a0, a1, a2, a3, seconds)
    } kind{};
    std::int32_t value{};          // damage message +24 / death code / controller_hit amount / refill health / sound descriptor
    std::uint64_t object{};        // damage message +28 / unconscious +24 / controller_hit source
    std::uint8_t type_2c{},category_2d{},flags_20{},byte_2e{};   // damage message
    std::uint32_t word_1c{};       // damage message +1c
    bool immediate{};              // death message / sound transport (1: delivered at once)
    std::array<std::int32_t,4> arguments{};  // rumble a0..a3
    float seconds{};               // rumble f12
    RootVector position{},direction{};      // LOS request +30 / +40 (w = 1 / 0)
    bool operator==(const ActorDamageEvent&) const=default;
};

struct ActorDamageHost {
    std::function<std::uint32_t()> random;                      // 00127dd0 (the shared game RNG)
    std::function<std::int32_t()> top_action;                   // +320c, read at each life-state query
    std::function<RootVector(std::uint64_t)> position_e4;       // object virtual +e4
    std::function<RootVector()> position_c0;                    // victim virtual +c0(actor, 0)
    // The LOS request's reply byte (003caa50 -> virtual +c): true cancels.
    std::function<bool(const RootVector& position,const RootVector& direction)> los_blocked;
    // 00186950(actor, position, base) (hit_direction.hpp): a hit from behind
    // gives base, from the front base + 1; it also turns the heading toward
    // the hit (host side).
    std::function<std::int32_t(const RootVector& position,std::int32_t base)> hit_direction;
    // Controller virtual +84 (the player component's 0016fa10 returns the code).
    std::function<std::uint8_t(std::uint8_t code)> adjust_death_code;
    // The immediate death message (transport 1): the death action runs here.
    std::function<void(std::int32_t code)> death_action;
};
struct ActorDamageInput {
    std::uint64_t actor{};                                  // victim identity
    std::optional<ActorDamageController> controller;        // victim +58
    bool byte_14{};                                         // +14 (locally owned)
    ActorDamageParty source;
    // +3480/+3484 weak handle when valid (+3480 non-null and its +c equals +3484).
    std::optional<ActorDamageParty> fire_setter_3480;
    std::uint8_t category{},type{};
    std::optional<std::int32_t> penetration;                // t0 (in/out)
    bool quiet{};                                           // t1
    std::optional<RootVector> hit_point;                    // t2
    float amount{};                                         // f12
};
struct ActorDamageResult {
    bool dead{};                                            // life state 3 afterwards
    std::optional<std::int32_t> penetration;                // *t0 afterwards (with a penetration input)
};
// 00187360 with 003ccb00 and the kill 00186bf0. Events are appended to
// `events` as they happen, so host calls see the events before them.
ActorDamageResult apply_actor_damage(ActorHealthState& state,const ActorDamageInput& input,const ActorDamageHost& host,
    std::vector<ActorDamageEvent>& events);

// 00186bf0(actor, amount, source, category, type, hit point) as 003ccb00
// calls it. Appends its events.
struct ActorKillInput {
    std::uint64_t actor{};
    std::optional<ActorDamageController> controller;
    bool byte_14{};
    std::int32_t amount{};
    std::uint64_t source{};
    std::uint8_t category{},type{};
    std::optional<RootVector> hit_point;
};
void kill_actor(ActorHealthState& state,const ActorKillInput& input,const ActorDamageHost& host,std::vector<ActorDamageEvent>& events);
}
