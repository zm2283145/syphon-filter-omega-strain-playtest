#pragma once

#include "level_load.hpp"

#include <cstdint>

namespace omega {

// Original 001693f0. The incoming root register is unused by its solo body,
// but retained to match the 00168518 caller. 001350e0 runs on the same memory.
void run_level_load_wrapper(LevelLoadMemory& memory, LevelLoadHost& host,
                            std::uint32_t root, std::uint32_t stack = 0x1f00000);

// State 0x0e main-update block 00168438..00168544, stopped before the next
// unconditional 00169390 call. The first non-0x0e state returns untouched.
void run_level_load_state(LevelLoadMemory& memory, LevelLoadHost& host,
                          std::uint32_t app);

}  // namespace omega
