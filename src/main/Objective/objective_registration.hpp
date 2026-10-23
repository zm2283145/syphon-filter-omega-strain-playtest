#pragma once

#include "script.hpp"
#include "string_pool.hpp"
#include <ostream>
#include <string_view>

namespace omega {
struct ObjectiveRegistration {
    Word index{};
    std::string key;
    std::int32_t points{};
    bool solo_required{};
    bool multi_required{};
    Word call_word{};
};
struct ObjectiveRegistrationPrefix {
    Word start_word{};
    Word end_word{}; // First non-source instruction outside the registration prefix.
    std::vector<ObjectiveRegistration> rows;
};
ObjectiveRegistrationPrefix read_objective_registration_prefix(const Script& script);

// game::RegisterLevel(name, int, int, int) (binding 004d452c -> 0032edf0).
// Arguments are popped top first: name is the last push. 00330180 builds the
// 24-byte row appended to the table at 00532af0 (+4 count, +8 rows): +0 the
// interned name, +4 a registration counter (004926e0), +8 -1, +c 0, +14
// argument 3 (the first push). Arguments 1 and 2 form the pair appended to
// 00532b90 (meaning not claimed).
struct LevelRegistration {
    std::string name;
    std::int32_t mission_number{};  // row +14
    std::int32_t pair_first{};      // argument 1 (the third push)
    std::int32_t pair_second{};     // argument 2 (the second push)
    Word call_word{};
};
// The literal-statement block of awards::Init that follows the objective
// prefix (push_float, float_to_int, float_mul, float_neg, string_reference,
// enum_value, source_line and void calls), read to its first other opcode;
// every RegisterLevel row in that block. Throws for any unexpected shape.
std::vector<LevelRegistration> read_level_registrations(const Script& awards);
// 00330b80: the first row whose name equals the level (00128e80, ASCII case
// insensitive) gives +14; no row gives 0x16. The original passes the level
// name buffer 004ffc90 (the GAMEDATA directory name, see 00135420).
std::int32_t level_mission_number(const std::vector<LevelRegistration>& levels, std::string_view level);
void write_objective_registration_probe(std::ostream& out, const Script& script);
void write_objective_registration_probe(std::ostream& out, const Script& script,
                                       const StringPool& english_base_pool);
}
