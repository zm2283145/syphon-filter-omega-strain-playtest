#pragma once
// AI team relations (TEAM_RELATIONS.md): the 16x16 byte matrix at *0x4ee620
// (ai.cc "cTeam"); bit 0 of [team][other] is "team treats other as an enemy".
// Teams 0-3 are the players, 4-15 the NPCs. The multiplayer "cTeam" resource
// branch of 0016a0a0 and the other bits are outside this component.

#include <array>
#include <cstdint>

namespace omega {

class TeamRelations {
public:
    static constexpr std::int32_t teams = 16;
    static constexpr std::int32_t players = 4;
    // 0016a970 (solo) -> 0016a750: the player rows are enemies of the NPC
    // columns and the NPC rows of the player columns; everything else clear.
    TeamRelations();
    void reset();
    // game::TeamSetEnemy / TeamDelEnemy (00169fc0 / 00169f60): bit 0 of
    // [team][other]. The original does not check the range; out of range
    // throws here.
    void set_enemy(std::int32_t team, std::int32_t other, bool enemy);
    // game::Team{Set,Del}EnemyAllPlayers (00169ca0 / 00169c00): columns 0-3.
    void set_enemy_all_players(std::int32_t team, bool enemy);
    // game::Team{Set,Del}EnemyAllNpcs (00169e50 / 00169d40): columns 4-15.
    void set_enemy_all_npcs(std::int32_t team, bool enemy);
    // game::TeamEnemy (0016a020): bit 0, false for an index outside 0-15.
    bool enemy(std::int32_t team, std::int32_t other) const;
private:
    static void check(std::int32_t team);
    std::array<std::uint8_t, teams * teams> bits_{};
};

}
