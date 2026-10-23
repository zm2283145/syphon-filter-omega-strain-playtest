#include "spawned_actor_ai_base.hpp"

#include <stdexcept>

namespace omega {

std::uint32_t run_spawned_actor_ai_base(SpawnedActorMemory& m, SpawnedActorHost& h,
                                        std::uint32_t component, std::uint32_t actor) {
    if (!component) throw std::runtime_error("Spawned actor AI base: required component is null");
    const auto r32 = [&](std::uint32_t p) { return m.read(p); };
    const auto w32 = [&](std::uint32_t p, std::uint32_t v) { m.write(p, 4, v); };
    const auto w8 = [&](std::uint32_t p, std::uint32_t v) { m.write(p, 1, v); };

    h.call(m, 0x230240, {component});
    w32(component + 0x44, 0);
    w32(component + 0x4c, 0);
    // The original immediately rereads +4c and resets the free-list fields
    // when its signed value is nonpositive (always true after the zero store).
    if (static_cast<std::int32_t>(r32(component + 0x4c)) <= 0) {
        w32(component + 0x48, 0);
        w8(component + 0x50, 1);
        w32(component + 0x4c, 0);
    }
    w32(component, 0x4d9890);

    std::uint32_t team = 4;
    if (actor) {
        const std::uint32_t old = r32(actor + 0x58);
        if (old) {
            const std::uint32_t predicate = r32(r32(old) + 0x54);
            if (h.call(m, predicate, {old}))
                team = h.call(m, 0x3cc950, {actor});
        }
    }
    const std::uint32_t setter = r32(r32(component) + 0xa0);
    h.call(m, setter, {component, team});
    w32(component + 0x58, 0xbf800000);
    w8(component + 0x5c, 0);
    w8(component + 0x5d, 0);
    return component;
}

} // namespace omega
