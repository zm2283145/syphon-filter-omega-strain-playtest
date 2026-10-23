#pragma once

#include "level_load.hpp"

#include <cstdint>

namespace omega {

// Port of 003d1020, the getter of the function-static object at 00543760 that
// the gameplay block passes to 003d0c10. While the guard byte 00543740 is zero
// (lb/bnez: any nonzero byte counts as built) it constructs the object: +0 = 0,
// two empty list headers at +4 (003d1560) and +10 (00139070), each {0, h+4,
// h+4}, byte +1c = 1; it registers the destructor 003d0b90 through 00100b30
// with the node 00543748 ({*004e1db0, 003d0b90, 00543760}, then *004e1db0 =
// 00543748) and sets the guard to 1. Returns 00543760.
std::uint32_t static_instance_003d1020(LevelLoadMemory& memory);

}  // namespace omega
