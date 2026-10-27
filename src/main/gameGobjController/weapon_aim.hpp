#pragma once
// Aim / ToHit (COMBAT.md §6): the controller's aim accumulator 0022f5b0
// (controller virtual +60, shared by the player component and the AI
// component) and the hit roll 003ea4a0. The values this reads from the
// shooter, the target and their controllers are inputs; the timer conversion
// (EE timer 0 and 00533870/00533878 into +40 milliseconds) is the host's.
#include "animation_root.hpp"
#include "weapon_database.hpp"
#include <cstdint>
#include <functional>
#include <optional>

namespace omega {

// game::ToHit (ToHitSettings 0048ac80; the 14 floats 00230290 registers).
struct ToHitSettings {
    float run_acc_factor{0.7F},run_aim_factor{0.7F},kneel_acc_factor{1.3F},kneel_aim_factor{2.0F};
    float moving_targ_acc_factor{0.7F},crouch_targ_acc_factor{0.8F},roll_targ_acc_factor{0.5F},max_velocity{20.0F};
    float npc_extra_aim_time{1.5F},npc_moving_targ_val{0.03F},npc_rolling_targ_val{0.02F},reload_aim_loss{0.1F};
    float npc_near_dist{12.0F},npc_near_mul{6.0F};
};

// The controller's aim block.
struct WeaponAimState {
    float progress_20{},accuracy_24{};
    std::uint64_t target_28{};          // weak handle object (0 none)
    std::int32_t target_id_2c{-1};      // and its +c
    float skill_38{1.0F};               // SetSkill (0022f2d0)
    std::int32_t subtarget_3c{};
    std::uint32_t time_40{};            // milliseconds
    bool operator==(const WeaponAimState&) const=default;
};

struct WeaponAimTargetController {
    bool player_54{};
    bool owned_14{};                    // the controller's +30 object's +14
    std::optional<RootVector> velocity_74;   // virtual +74 (null: not moving)
};
struct WeaponAimTarget {
    std::uint64_t object{};
    std::int32_t id_c{};
    RootVector eye_c0{};                // virtual +c0(target, -1)
    std::uint8_t stance_c8{};
    float size_cc{1.0F};                // virtual +cc(target, subtarget)
    std::int32_t stealth_44{};
    std::optional<WeaponAimTargetController> controller;   // +58
};
struct WeaponAimShooter {
    bool actor{true};                   // +4c is the actor class word 0049d010
    float blinded_33a8{};               // > 0 drops the target
    bool player_54{};                   // controller virtual +54
    float accuracy_multiplier{1.0F};    // 001b33d0(actor) (players only)
    bool ignore_size_3470{};
    bool ready_d4{};                    // shooter virtual +d4
    RootVector eye_c0{};
    std::uint8_t stance_c8{};
    std::optional<RootVector> velocity_74;   // controller virtual +74
};
struct WeaponAimInput {
    std::uint32_t now_ms{};             // the new +40
    const WeaponRecord* record{};       // null: weapon ID < 0
    std::optional<WeaponAimTarget> target;
    std::int32_t subtarget{};
    WeaponAimShooter shooter;
    bool target_manager_22cb10{};       // 0022cb10(004f77c0, shooter, target, 0, 0) (only read with a target)
    ToHitSettings settings;
    // The engine variables read on every call (132fx2): PlayerAimTimePct 0048acc8 (0022FAD8, a player's aim
    // time) and globalNpcSkill 0048acc0 (0022FC84, an NPC's skill); their registered values are 1.0.
    float player_aim_time_pct{1.0F};
    float global_npc_skill{1.0F};
};
// Target controller virtual +a0(target controller, controller, amount,
// subtarget, progress): the "being aimed at" notice.
struct WeaponAimNotice {
    std::uint64_t target{};
    std::int32_t amount{},subtarget{};
    float progress{};
    bool operator==(const WeaponAimNotice&) const=default;
};
std::optional<WeaponAimNotice> update_weapon_aim(WeaponAimState& state,const WeaponAimInput& input);

// 003ea4a0: an NPC (controller +54 clear) with progress < 0.9 (00493988)
// misses without drawing; otherwise one 00127dd0 draw r and
// hit = float(r) / 2^31 < progress x accuracy.
// npc_threshold: NpcDmgAimThresh 00493988, read on every roll (registered 0.9; 132fx2).
bool weapon_hit_roll(const WeaponAimState& state,bool player,const std::function<std::uint32_t()>& random,
    float npc_threshold=0.9F);

}
