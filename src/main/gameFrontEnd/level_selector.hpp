#pragma once

#include "level_load.hpp"

#include <cstdint>

namespace omega {

// Original 002cc820. Index is a signed EE word. The comparator/copy services
// remain explicit host calls; metadata is read after the copy callback.
void set_level_selection(LevelLoadMemory& memory, LevelLoadHost& host,
                         std::uint32_t destination, std::int32_t index);

// Original 002cc8e0: case-insensitive scan of the 18-entry level-name table,
// then 002cc820 with the matching index or -1.
void select_level_by_name(LevelLoadMemory& memory, LevelLoadHost& host,
                          std::uint32_t destination, std::uint32_t name);

}  // namespace omega
