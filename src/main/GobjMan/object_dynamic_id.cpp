#include "object_dynamic_id.hpp"

#include <stdexcept>
#include <string>

namespace omega {

bool object_id_registered(std::uint32_t category, std::uint32_t index,
                          const std::function<bool(std::uint32_t key)>& registered) {
    if (category >= 0x100U) return false;   // 0040c530
    if (index >= 0x10000U) return false;    // 0040c520
    if (!registered) throw std::runtime_error("Object ID lookup 0040c080: no registry host");
    return registered(index | (category << 16));   // 0040c500 (a3 = -1: top byte 0), 0040c080
}

std::uint32_t object_dynamic_id(ObjectIdCounters& counters, std::uint32_t category,
                                const std::function<bool(std::uint32_t key)>& registered) {
    const std::uint32_t c = category & 0xffU;
    auto& counter = counters[c];
    for (std::uint32_t hits = 0;; ++hits) {
        if (hits == 0x10000U)
            throw std::runtime_error("Object dynamic ID 0040c240: every index of category " + std::to_string(c)
                                     + " is registered (the original loops forever)");
        const std::uint32_t next = counter + 1U;
        counter = static_cast<std::int32_t>(next) < 0x10000 ? next : 0U;   // slt with 0x10000
        if (!object_id_registered(c, counter, registered)) break;
    }
    return counter | (c << 16);
}

}  // namespace omega
