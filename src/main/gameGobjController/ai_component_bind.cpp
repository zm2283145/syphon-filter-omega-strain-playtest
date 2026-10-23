#include "ai_component_bind.hpp"

#include <stdexcept>

namespace omega {

void run_ai_component_bind(SpawnedActorMemory& m, SpawnedActorHost& h,
                           std::uint32_t c, std::uint32_t actor) {
    if (!c) throw std::runtime_error("AI component binding: required component is null");
    const auto r8 = [&](std::uint32_t p) { return m.read(p, 1); };
    const auto r32 = [&](std::uint32_t p) { return m.read(p); };
    const auto w32 = [&](std::uint32_t p, std::uint32_t v) { m.write(p, 4, v); };
    const auto call = [&](std::uint32_t address, std::initializer_list<std::uint32_t> args = {}) {
        return h.call(m, address, args);
    };

    w32(c + 0x30, actor); // delay slot of the null-actor branch
    if (!actor) return;
    if (r32(actor + 0x4c) != r32(0x49d010)) return;
    if (!r32(actor + 0x3264)) return;
    if (!r32(c + 0x18)) {
        if (r8(0x533880)) call(0x10dc40, {9});
        w32(0x533888, r32(0x533888) + 1);
        const auto p = call(0x36b180, {0, 0xc, 0x49c088, 0x49}); // "system.hh"
        if (r8(0x533880)) call(0x10dca8, {9});
        w32(0x533888, r32(0x533888) - 1);
        if (p) {
            w32(p, 0);
            w32(p + 4, 0);
            w32(p + 8, 0xffffffff);
        }
        w32(c + 0x18, p);
    }
    const auto entity = r32(actor + 0x3264);
    call(0x3d9d30, {r32(c + 0x18), entity});
}

} // namespace omega
