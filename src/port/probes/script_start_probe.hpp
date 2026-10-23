#pragma once

#include "population.hpp"
#include "script.hpp"

#include <map>
#include <ostream>
#include <string>

namespace omega {

// Diagnostic for script hosting S1: runs the original level start (003e20a0)
// and then init::ClientStartLevel / init::StartLevel over a mission's modules
// (keyed by lower-case base name) with a host that records void natives and
// refuses everything it does not host. Writes JSON: per stage the result or the
// first error, the manager log, the recorded native calls, and every object
// lookup (opcode 61/65) with the population section it resolved to.
//
// Lookups resolve in one diagnostic scope holding every decoded population
// record by its (category << 16) | index ID (the sections share one ID space).
// The first NPC is projected as the player and the rest as AI controllers.
// Which records the original's object table (0040c540) holds is not claimed.
void write_script_start_probe(std::ostream& out, const std::map<std::string, Script>& modules,
                              const Population& population);

}
