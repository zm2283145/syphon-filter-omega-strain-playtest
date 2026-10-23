#include "npc_roster.hpp"
#include <stdexcept>

namespace omega {
std::vector<NpcRosterEntry> make_npc_roster(const Population& population, std::int32_t first_serial) {
    if (population.npcs.empty()) throw std::runtime_error("NPC roster: population has no NPC records");
    std::vector<NpcRosterEntry> roster;
    std::int32_t counter = first_serial;                                   // 004ee968
    for (std::size_t i = 0; i < population.npcs.size(); ++i) {
        const auto& npc = population.npcs[i];
        NpcRosterEntry e;
        e.record = i;
        e.role = i == 0 ? std::uint8_t{1} : std::uint8_t{2};             // 001c1e70 (solo)
        e.actor_id = npc.id;
        e.spawn = npc_spawn_pose(population, i);
        e.health = make_actor_health(npc.parameters[0], npc.parameters[1]); // 001b5fe0
        if (e.role == 1) apply_player_role_armor(e.health);                 // 001ae450 role 1
        e.item_words = npc.trailing_parameters;                             // +3588[0..1]
        for (const auto word : npc.trailing_parameters)
            if (word != 0xffffffffU) e.starting_items.push_back(word);      // 00143df0 per word != -1
        e.start_node = npc.node_index;                                      // +3548
        if (npc.node_index >= 0) {
            if (static_cast<std::size_t>(npc.node_index) >= population.nodes.size())
                throw std::runtime_error("NPC roster: start node outside the node collection");
            e.start_node_path = (population.nodes[static_cast<std::size_t>(npc.node_index)].flags & 0x8U) != 0;
        }
        e.appearance = npc.model;
        e.script_name = npc.extra_name;
        if (e.role == 2) {                                                  // 001b6fc8: +31f0, counter + 1 (32-bit)
            e.serial = counter;
            counter = static_cast<std::int32_t>(static_cast<std::uint32_t>(counter) + 1U);
        }
        e.present_2d = e.role == 1;                                         // 001b53d0 via +5c(1), role 1 only
        e.ai_inactive_159 = e.role == 2;                                    // 00158bc0
        e.local_owner_14 = true;
        roster.push_back(std::move(e));
    }
    return roster;
}
}
