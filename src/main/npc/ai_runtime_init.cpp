#include "ai_runtime_init.hpp"

#include <cstring>
#include <stdexcept>

namespace omega {

void run_ai_runtime_init(SpawnedActorMemory& m, SpawnedActorHost& h,
                         std::uint32_t c, std::uint32_t actor) {
    if (!c) throw std::runtime_error("AI runtime initializer: required component is null");
    if (!actor) throw std::runtime_error("AI runtime initializer: required actor is null");
    const auto r8 = [&](std::uint32_t p) { return m.read(p, 1); };
    const auto r32 = [&](std::uint32_t p) { return m.read(p); };
    const auto w8 = [&](std::uint32_t p, std::uint32_t v) { m.write(p, 1, v); };
    const auto w32 = [&](std::uint32_t p, std::uint32_t v) { m.write(p, 4, v); };
    const auto call = [&](std::uint32_t address, std::initializer_list<std::uint32_t> args = {}) {
        return h.call(m, address, args);
    };
    // The inlined allocation wrapper: the thread flag and counter are reread
    // around each child, and the counter is stored before 0036b180 runs.
    const auto allocate = [&](std::uint32_t size, std::uint32_t file, std::uint32_t line) {
        if (r8(0x533880)) call(0x10dc40, {9});
        w32(0x533888, r32(0x533888) + 1);
        const auto p = call(0x36b180, {0, size, file, line});
        if (r8(0x533880)) call(0x10dca8, {9});
        w32(0x533888, r32(0x533888) - 1);
        return p;
    };

    if (r32(actor + 0x3264)) {
        if (!r32(c + 0x18)) {
            const auto p = allocate(0xc, 0x49c088, 0x49); // "system.hh"
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
    if (!r32(c + 0x1ac)) {
        auto p = allocate(0x60, 0x49b670, 0x741); // "npc.cc"
        if (p) p = call(0x16e0a0, {p, c});
        w32(c + 0x1ac, p);
    }
    if (!r32(c + 0x1b0)) {
        auto p = allocate(0x80, 0x49b670, 0x745);
        if (p) p = call(0x161840, {p, actor, 0x64, r32(actor + 0x3548)});
        w32(c + 0x1b0, p);
    }

    w8(r32(actor + 0x3524) + 0x99, 1);
    w32(c + 0x1b4, 0);
    w32(c + 0x120, 0xffffffff);
    w32(c + 0x11c, 0xffffffff);
    w8(c + 0x13d, 0);
    w8(c + 0x13e, 1);
    w32(c + 0x118, 0);
    w32(c + 0x130, 0);
    w32(c + 0x134, 0);
    w32(c + 0x9c, 0xffffffff);
    w32(c + 0x64, 0xffffffff);
    w32(c + 0xa0, 0);
    w32(c + 0xac, 0);
    w32(c + 0xe0, 0xffffffff);
    w32(c + 0xe4, 0xffffffff);
    w32(c + 0x80, 0x42c80000); // 100.0f
    w32(c + 0x160, 0);
    w8(c + 0x164, 0);
    w32(c + 0x124, 0);
    w32(c + 0x128, 0);
    w8(c + 0x17c, 0);
    w32(c + 0x178, 0);
    w8(c + 0x1a8, 0);
    w8(c + 0x190, 0);
    w32(c + 0x18c, 0);
    w8(c + 0x1a9, 0);
    w8(c + 0x1a4, 0);
    w32(c + 0x1a0, 0);
    w8(c + 0x1aa, 0); // delay slot of the RNG call

    // +6c = 0.25 + 0.5 * (cvt.s.w(rand) / 2^31), single precision.
    const auto draw = static_cast<std::int32_t>(call(0x127dd0));
    const float scaled = 0.25f + 0.5f * (static_cast<float>(draw) / 2147483648.0f);
    std::uint32_t scaled_bits;
    std::memcpy(&scaled_bits, &scaled, 4);
    w32(c + 0x6c, scaled_bits);
    call(r32(r32(c) + 0x15c), {c, 0x14, 0x00000000, 0x43480000});

    w32(c + 0x7c, 0);
    w32(c + 0x78, 0xbf800000); // -1.0f
    w32(c + 0x114, 0);
    for (std::uint32_t off = 0xbc; off < 0xe0; off += 0xc) {
        w32(c + off, 0xffffffff);
        w32(c + off + 4, 0xffffffff);
        w32(c + off + 8, 0xffffffff);
    }
    const auto range = call(r32(r32(c) + 0x148), {c, 1});
    if (static_cast<std::int32_t>(r32(c + 0x64)) <= 0) {
        w32(c + 0x60, range);
        w32(c + 0x64, 0);
        w8(c + 0x141, r8(c + 0x141) & 0xbf);
    }
    w32(c + 0x1c8, 0);
    w32(c + 0x1cc, 0xffffffff); // via the stack word sp+40
    w32(c + 0x90, 0x40800000);  // 4.0f
    w32(c + 0x94, 0x420c0000);  // 35.0f
    w8(c + 0x141, (r8(c + 0x141) & 0xef) | 0x10);
    w32(c + 0xb8, 0);
    w32(c + 0x1c4, 0);
    w32(c + 0x1c0, 0x497423f0); // 1000000.0f
    w8(c + 0x13c, 0);
    w32(c + 0x138, 0xffffffff);
    w8(c + 0x141, (r8(c + 0x141) & 0xfb) | 0x04);
    w8(c + 0x159, 1);
    w8(c + 0x141, (r8(c + 0x141) & 0xdf) | 0x20);
    w8(c + 0x141, r8(c + 0x141) & 0x7f);
    w8(actor + 0x14, 1);
    w8(c + 0x14, 1);
    w32(c + 0x144, 0x40000000); // 2.0f
    w8(c + 0x1d5, 0);
    w8(c + 0x1d4, 0);
    w8(c + 0x1d6, 0);
    w32(c + 0x154, 0);
    w8(c + 0x169, 0);
}

} // namespace omega
