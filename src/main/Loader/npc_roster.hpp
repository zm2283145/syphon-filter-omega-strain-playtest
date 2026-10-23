#pragma once
// NPC records as the solo loader 001c1e70 turns them into actors (NPC_SPAWN_AND_AI.md
// sections 1-2, milestone N1): role, identity, loader pose, health/armor, starting
// items, start node, appearance manifest, the 004ee968 serial and the activation
// state right after loading. No AI, inventory or model object is built here.
#include "actor_damage.hpp"
#include "population.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace omega {
struct NpcRosterEntry {
    std::size_t record{};
    std::uint8_t role{};                   // 001c1e70 solo: 1 for record 0 (local player), 2 otherwise
    std::uint32_t actor_id{};              // actor +c = (category << 16) | index
    SpawnPose spawn;                       // npc_spawn_pose(record): node position when +3548 >= 0
    ActorHealthState health;               // 001b5fe0 stores, then 001ae450 StartingArmor for role 1
    std::array<std::uint32_t, 2> item_words{}; // actor +3588[0..1] as stored
    std::vector<std::uint32_t> starting_items; // the words != 0xffffffff, in 00143df0 call order (not equipped here)
    std::int32_t start_node{-1};           // actor +3548; seeds the AI path follower's hold node (role 2)
    bool start_node_path{};                // node type bit 0x8 (Path): patrols from activation 00160f60
    std::string appearance;                // record model string, actor +3318 (resolve_character_appearance)
    std::string script_name;               // record extra name, actor +3264 (AI script object when non-empty)
    std::int32_t serial{-1};               // actor +31f0: role 2 takes the 004ee968 counter, then it increments
    // Right after 001c1e70: role 1 is activated (virtual +5c -> 001b53d0 sets +2d = 1);
    // role 2 stays inactive (+2d = 0, AI component +159 = 1, no physical component).
    bool present_2d{};
    bool ai_inactive_159{};
    bool local_owner_14{};                 // +14 = 1 for both solo roles (AI init 00158bc0 for role 2)
};
// Every NPC record in file order. `first_serial` is the 004ee968 counter when the
// first role-2 record is constructed: 0 in the boot image, but every role 2/4 actor
// built earlier in the session (001b6ab0, 001b7b00, 001b8130) has advanced it, so
// the host supplies it. Throws for an empty NPC section or a node index outside
// the node collection; records with a nonempty extra name are reported, not loaded.
std::vector<NpcRosterEntry> make_npc_roster(const Population& population, std::int32_t first_serial);
}
