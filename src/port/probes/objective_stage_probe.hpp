#pragma once

#include "script.hpp"
#include "string_pool.hpp"
#include "population.hpp"
#include <ostream>

namespace omega {
void write_objective_stage_probe(std::ostream& output, const Script& script);
void write_objective_outcome_probe(std::ostream& output, const Script& script);
void write_objective_completion_probe(std::ostream& output, const Script& script,
    const Script& awards, const StringPool& pool);
void write_objective_voice_probe(std::ostream& output, const Script& script,
    const Script& awards, const StringPool& pool, const Script& voice);
void write_objective_proust_probe(std::ostream& output, const Script& script,
    const Script& awards, const StringPool& base_pool, const StringPool& mission_pool,
    const Script& voice, const Population& population);
void write_objective_solo_end_probe(std::ostream& output, const Script& script,
    const Script& awards, const StringPool& base_pool, const StringPool& mission_pool,
    const Script& voice, const Population& population);
}
