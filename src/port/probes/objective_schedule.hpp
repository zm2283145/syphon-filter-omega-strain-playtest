#pragma once
#include "script.hpp"
#include <ostream>
namespace omega {
// Diagnostic invocation of the real solo LevelEnd with CHA fixture states.
// Records RequestLevelEnd; does not implement a gameplay level transition.
void write_objective_schedule_probe(std::ostream& output, const Script& script);
}
