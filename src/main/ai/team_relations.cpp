#include "team_relations.hpp"

#include <stdexcept>
#include <string>

namespace omega {

TeamRelations::TeamRelations() { reset(); }

void TeamRelations::reset() {
    for (std::int32_t team = 0; team < teams; ++team)
        for (std::int32_t other = 0; other < teams; ++other)
            bits_[static_cast<std::size_t>(team * teams + other)] = (team < players) != (other < players) ? 1 : 0;
}

void TeamRelations::check(std::int32_t team) {
    if (team < 0 || team >= teams) throw std::runtime_error("Team index outside 0-15: " + std::to_string(team));
}

void TeamRelations::set_enemy(std::int32_t team, std::int32_t other, bool enemy) {
    check(team);
    check(other);
    auto& cell = bits_[static_cast<std::size_t>(team * teams + other)];
    cell = static_cast<std::uint8_t>((cell & ~1U) | (enemy ? 1U : 0U));
}

void TeamRelations::set_enemy_all_players(std::int32_t team, bool enemy) {
    for (std::int32_t other = 0; other < players; ++other) set_enemy(team, other, enemy);
}

void TeamRelations::set_enemy_all_npcs(std::int32_t team, bool enemy) {
    for (std::int32_t other = players; other < teams; ++other) set_enemy(team, other, enemy);
}

bool TeamRelations::enemy(std::int32_t team, std::int32_t other) const {
    if (team < 0 || team >= teams || other < 0 || other >= teams) return false;
    return (bits_[static_cast<std::size_t>(team * teams + other)] & 1U) != 0;
}

}
