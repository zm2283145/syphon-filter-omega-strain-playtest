#pragma once

#include <array>
#include <cstdint>
#include <functional>

namespace omega {

// The per-category counters 00571780 (256 words, zeroed by the object
// registry init 0040c2d0).
using ObjectIdCounters = std::array<std::uint32_t, 256>;

// 0040c480(category, index): whether an object with this ID is registered.
// 0040c530 / 0040c520 return "not found" for category >= 0x100 or index >=
// 0x10000 (unsigned); otherwise 0040c500 forms the key (index |
// (category << 16), top byte 0) and 0040c080 looks it up in the registry
// *00571740. `registered` is that lookup.
bool object_id_registered(std::uint32_t category, std::uint32_t index,
                          const std::function<bool(std::uint32_t key)>& registered);

// 0040c240(&id, category): a new object ID in the category (low byte). The
// counter is incremented, reset to 0 when the result is not below 0x10000
// (signed compare), stored, and the loop repeats while 0040c480 finds an
// object with that index. Returns (category << 16) | counter (an or of the
// whole counter word). The original loops forever when every index of the
// category is registered; that throws here (0x10000 consecutive hits).
std::uint32_t object_dynamic_id(ObjectIdCounters& counters, std::uint32_t category,
                                const std::function<bool(std::uint32_t key)>& registered);

}  // namespace omega
