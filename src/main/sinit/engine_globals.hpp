#pragma once

#include "script.hpp"

#include <string>
#include <vector>

namespace omega {

// game::ToHit (registered by 004cef60) points at the static ToHitSettings at
// 0048ac80; these are its initial values. Members are the 14 floats that
// 00230290 registers at offsets 0x00..0x34, in order.
std::vector<EngineStruct::Field> to_hit_settings_defaults();

// Registered (native) enum: the value of each member in descriptor order, which
// is the member index scripts use for an external enum (opcode 58). Descriptors
// are {value, name, 0}; the tables come from the startup registration calls
// (callsites in the definition). Null for a name the engine does not register.
const std::vector<Word>* engine_enum(const std::string& name);

}
