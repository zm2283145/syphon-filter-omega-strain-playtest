#include "spawned_actor_ai.hpp"

#include <stdexcept>

namespace omega {

void run_spawned_actor_ai(SpawnedActorMemory& m, SpawnedActorHost& h,
                          const SpawnedActorAiArgs& q) {
    if (q.role != 2) throw std::runtime_error("Spawned actor AI: only role 2 is hosted");
    if (m.read(0x5721c8, 1)) throw std::runtime_error("Spawned actor AI: multiplayer branch 001ae478 unhosted");
    if (!q.actor) throw std::runtime_error("Spawned actor AI: required actor is null");
    const auto r8 = [&](std::uint32_t p) { return m.read(p, 1); };
    const auto r32 = [&](std::uint32_t p) { return m.read(p); };
    const auto w8 = [&](std::uint32_t p, std::uint32_t v) { m.write(p, 1, v); };
    const auto w32 = [&](std::uint32_t p, std::uint32_t v) { m.write(p, 4, v); };
    const auto call = [&](std::uint32_t address, std::initializer_list<std::uint32_t> args = {}) {
        return h.call(m, address, args);
    };

    // 001ae640: the pair selects the generator-specific component constructor.
    const std::uint32_t line = q.pair ? 0xbd8 : 0xbde;
    if (r8(0x533880)) call(0x10dc40, {9});
    w32(0x533888, r32(0x533888) + 1);
    const std::uint32_t component = call(0x36b180, {0, 0x1e0, 0x49d8d0, line});
    if (r8(0x533880)) call(0x10dca8, {9});
    w32(0x533888, r32(0x533888) - 1);

    if (component) {
        // Both 00158800 and 001588d0 first delegate their common base.
        // Only the generator branch loads pair[0] as a pointer to the ID.
        const auto generator_id_pointer = q.pair ? r32(q.pair) : 0;
        call(0x16f5b0, {component, q.actor});
        for (std::uint32_t off = 0xbc; off <= 0xdc; off += 4) w32(component + off, 0);
        w32(component, 0x4d9380);
        w32(component + 0x150, q.pair ? r32(generator_id_pointer) : 0);
        for (std::uint32_t off : {0x16cU, 0x180U, 0x194U}) {
            w32(component + off + 4, 0xfffffffe);
            w32(component + off, 0);
        }
        w32(component + 0x1cc, 0xfffffffe);
        w32(component + 0x1c8, 0);
        w32(component + 0x1b0, 0);
        w32(component + 0x1ac, 0);
        call(0x158bc0, {component, q.actor});
    }
    call(0x3cce30, {q.actor, component});
    w8(q.actor + 0x31fc, 1);
}

} // namespace omega
