#pragma once
// The engine variables the executable registers for the scripts: the static
// initializers call 003d8a30(name, address, type) with type descriptor
// 00493888 (1, int) or 00493890 (2, float); the scripts address them as
// "game::<name>" (opcode 22, variable_address). Values are the boot-time
// contents of each address (checked in the interpreter's booted snapshot;
// the ELF image holds the same words). The two struct variables, ToHit
// (ToHitSettings, hosted by MissionScriptHost) and targSettings, are not in
// this table.
#include <cstdint>
#include <span>

namespace omega {
class ScriptContext;
struct EngineVariableDefault {
    const char* name;          // without the "game::" prefix
    std::uint32_t address;     // the cell in the executable
    std::uint8_t type;         // 1 int, 2 float (primitive type keys 1 and 2)
    std::uint32_t bits;        // boot value
};
std::span<const EngineVariableDefault> engine_variable_defaults();
// Defines every table entry as "game::<name>" with its boot value, skipping
// names the context already defines.
void define_engine_variable_defaults(ScriptContext& context);
}
