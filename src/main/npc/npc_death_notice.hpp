#pragma once
// The AI component's death notice 0014f680 (virtual +7c; the kill 00186bf0
// calls it through 003ccac0 with the amount, the killer, the category and the
// damage type) with its callees, interpreter-verified
// (tests/npc_death_notice_goldens.hpp):
//   00163c50  the actor's route reservations in the manager 004ed9b0 (filled
//             only by 00163e20 from route and combat code that is not native,
//             so nothing is ever reserved natively: recorded);
//   004ff278  the script death notice (vtable 004dccf0, +20 = 2, +24 the
//             killer, +28 the damage type) to the actor, transport 0;
//   00186f30  the kill statistics and voice of a player killer (the
//             statistics are added to the agent record, agent_statistics.hpp,
//             and the warning line reads its statistic 0x19);
//   +198      0014b470, the death burst (+12c = -1, +130 = +a8, +134 =
//             trunc(+8c)) while a burst is pending (+a8 > 0, +84 <= 0), the
//             perception's +20 bits 0 and 2 are set, the held weapon is full
//             auto (00143820) and both secondary states fire;
//   00142170  the dropped weapon's ammunition (WeaponInventoryOps);
//   0016dc60  the perception's records cleared;
//   0015ad70  the actor's think-list entries removed and 0014ab90 the
//             component appended to the dead list 004ea370, both unless the
//             actor is present (+2d, +2c, !+2e) with +3468 set (the dead
//             list only when present).
// A killer that is a remote player (+14 clear) sends cNetUpdateStatsMsg to
// that player's client; the receiving console calls the statistics helper
// without replaying the rest of the death notice.
#include "agent_statistics.hpp"
#include "npc_ai.hpp"
#include "npc_perception.hpp"
#include "weapon_inventory.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

struct NpcDeathNoticeInput {
    std::int32_t amount{};
    std::uint64_t killer{};             // 0: none
    std::uint8_t category{},type{};
};
// 00186f30's view of the killer: its +58 controller (nullopt: none) and +14.
struct NpcKillerView {
    bool player{};                       // controller virtual +54
    bool byte_14{};
};
struct NpcDeathNoticeEvent {
    enum class Kind : std::uint8_t {
        route_reservations,  // 00163c50(004ed9b0, actor): nothing reserved natively
        script_death,        // 004ff278 to the actor, transport 0: object = killer, value = type
        statistic,           // 00333f10(stats, value, 1)
        voice,               // 0018c770(killer, value)
        death_burst,         // virtual +198(+a8, -1, 0.0)
        perception_cleared,  // 0016dc60
        think_list_removed,  // 0015ad70: value = entries removed
        dead_list_added,     // 0014ab90(004ea370, component)
    } kind{};
    std::int32_t value{};
    std::uint64_t object{};
    bool operator==(const NpcDeathNoticeEvent&) const=default;
};
struct NpcDeathNoticeHost {
    std::uint64_t actor{};                                     // component +30 (0: the notice does nothing)
    std::array<std::uint8_t,3> actor_root{};                   // +2c, +2d, +2e
    std::uint8_t actor_3468{},actor_3488{};
    std::optional<std::array<std::uint8_t,2>> secondary;       // +3314 present: +310 / +311
    std::function<std::optional<NpcKillerView>(std::uint64_t killer)> killer;
    std::function<bool(std::uint64_t killer)> killer_hostile;  // 003cc8e0(killer, actor)
    // 00186f30's remote-player branch: cNetUpdateStatsMsg carries the victim
    // and killer GOBJ keys plus the damage type/category to the killer's client.
    std::function<void(std::uint64_t victim,std::uint64_t killer,std::uint8_t type,std::uint8_t category)> remote_statistics;
    // The local agent record 001314f0(004ffb50, -1): 00333f10 adds each
    // statistic event, 00333ee0 reads 0x19 (null throws when a player kill is noted).
    AgentStatistics* statistics{};
    std::function<std::uint32_t()> random;                     // 00127dd0
    // 132fx2: MinNpcAmmoPct / MaxNpcAmmoPct (00489c18 / 00489c20) for 00142170, read live; unset: 0.05 / 0.3.
    std::function<std::array<float,2>()> ammo_pct;
    // The think list's entries for this actor (0015ad70 removes them).
    std::function<std::int32_t()> remove_from_think_list;
    std::function<void()> append_to_dead_list;                 // 0014ab90(004ea370, &component)
};
void npc_kill_statistics(const NpcDeathNoticeInput& input,const NpcDeathNoticeHost& host,
    std::vector<NpcDeathNoticeEvent>& events);
void npc_death_notice(NpcAiComponent& component,NpcPerception& perception,WeaponInventoryOps& inventory,
    const NpcDeathNoticeInput& input,const NpcDeathNoticeHost& host,std::vector<NpcDeathNoticeEvent>& events);

// 0014b610 (component virtual +164): +141 bit 2, unless the actor is present
// (+2d, +2c, !+2e) with +3468 set.
bool npc_corpse_removable_164(const NpcCorpseView& view);
// 00159020(component) in single player: true when the dead NPC may be removed
// (the AI update then calls actor virtual +60(1); otherwise the firing gate
// 00150340 runs). Dead (life 3), removable (+164), not held (+354c) and not
// in path-link action 7 with +3204 bit 7; with four or more corpses on the
// dead list 004ea370 the first such one in list order is removed at once;
// otherwise the eye must be at least 20 units (400 squared) from the camera
// object's translation, and either actor +38 is 0x80 or the eye is at least
// the far distance (0053834c) away.
bool npc_dead_removal_allowed(const NpcCorpseView& self,const std::vector<const NpcCorpseView*>& dead_list,
    const RootVector& camera_position,float far_distance);

}
