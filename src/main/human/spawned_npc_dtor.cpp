#include "spawned_npc_dtor.hpp"

#include <cstring>
#include <stdexcept>

namespace omega {
namespace {

// The profiling wrapper around each release: 0010dc40(9) when the byte
// 00533880 is set, then the counter 00533888 += 1 (stored before the wrapped
// call); afterwards 0010dca8(9) when the byte is set, then the counter -= 1.
void profile_lock(SpawnedActorMemory& m, SpawnedActorHost& h) {
    if (m.read(0x533880, 1)) h.call(m, 0x10dc40, {9});
    m.write(0x533888, 4, m.read(0x533888) + 1);
}

void profile_unlock(SpawnedActorMemory& m, SpawnedActorHost& h) {
    if (m.read(0x533880, 1)) h.call(m, 0x10dca8, {9});
    m.write(0x533888, 4, m.read(0x533888) - 1);
}

bool frees(std::uint32_t a1) { return std::int16_t(a1 & 0xffff) > 0; }

std::uint32_t virtual_at(const SpawnedActorMemory& m, std::uint32_t object, std::uint32_t slot) {
    return m.read(m.read(object) + slot);
}

} // namespace

void run_component_release(SpawnedActorMemory& m, SpawnedActorHost& h, std::uint32_t actor) {
    const auto component = m.read(actor + 0x58);
    if (!component) return;
    profile_lock(m, h);
    h.call(m, virtual_at(m, component, 8), {component, 0xffffffffU});
    h.call(m, 0x36b130, {0, component, 0x4bd228, 0x291});
    profile_unlock(m, h);
    m.write(actor + 0x58, 4, 0);
}

std::uint32_t run_ai_component_dtor(SpawnedActorMemory& m, SpawnedActorHost& h,
                                    std::uint32_t c, std::uint32_t a1) {
    if (!c) return 0;
    m.write(c, 4, 0x4d9380);
    if (!m.read(c + 0x159, 1)) {
        // The deactivate slot +4c, read from the vtable just stored.
        h.call(m, virtual_at(m, c, 0x4c), {c, a1});
    } else {
        const auto token = h.call(m, 0x3caa50, {0x4ffb50, 0x4ed9b0});
        h.call(m, 0x163c50, {token, m.read(c + 0x30)});
    }
    const auto owner = m.read(c + 0x30);
    if (h.call(m, virtual_at(m, owner, 0x7c), {owner})) h.call(m, 0x42df20, {m.read(c + 0x30) + 0xc});
    if (const auto p = m.read(c + 0x1b0)) {
        profile_lock(m, h);
        h.call(m, 0x161680, {p, 0xffffffffU});
        h.call(m, 0x36b130, {0, p, 0x49b670, 0x7e3});
        profile_unlock(m, h);
        m.write(c + 0x1b0, 4, 0);
    }
    if (const auto p = m.read(c + 0x1ac)) {
        profile_lock(m, h);
        h.call(m, 0x16dfd0, {p, 0xffffffffU});
        h.call(m, 0x36b130, {0, p, 0x49b670, 0x7e8});
        profile_unlock(m, h);
        m.write(c + 0x1ac, 4, 0);
    }
    h.call(m, 0x16f550, {c, 0});
    if (frees(a1)) h.call(m, 0x100740, {c});
    return c;
}

std::uint32_t run_inventory_dtor(SpawnedActorMemory& m, SpawnedActorHost& h,
                                 std::uint32_t inv, std::uint32_t a1) {
    if (!inv) return 0;
    if (const auto p = m.read(inv)) {
        profile_lock(m, h);
        h.call(m, 0x266470, {p, 0xffffffffU});
        h.call(m, 0x36b130, {0, p, 0x49b1d0, 0x4ce});
        profile_unlock(m, h);
    }
    h.call(m, 0x144ba0, {inv, 0});
    h.call(m, 0x144ba0, {inv, 1});
    // +d4's vtable is at the object's +4.
    if (const auto p = m.read(inv + 0xd4)) h.call(m, m.read(m.read(p + 4) + 8), {p, 1});
    if (const auto p = m.read(inv + 0xd8)) {
        h.call(m, 0x40c9a0, {p});
        h.call(m, 0x3cb230, {m.read(inv + 0xd8), 0x3dcccccd});
    }
    if (frees(a1)) h.call(m, 0x100740, {inv});
    return inv;
}

void run_actor_unlink(SpawnedActorMemory& m, SpawnedActorHost& h, std::uint32_t a) {
    h.call(m, 0x1a08d0, {a + 0x2ec0});
    h.call(m, 0x1a07f0, {a + 0x2fc0});
    const auto target = m.read(a + 0x3478);
    if (target && m.read(target + 0xc) == m.read(a + 0x347c))
        throw std::runtime_error("Spawned NPC unlink: linked target branch 001a0348 is not hosted");
}

std::uint32_t run_spawned_npc_dtor(SpawnedActorMemory& m, SpawnedActorHost& h,
                                   std::uint32_t a, std::uint32_t a1, std::uint32_t stack) {
    if (!a) return 0;
    const auto sp = stack - 0x240;
    m.write(a, 4, 0x4da690);
    if (const auto p = m.read(a + 0x3264)) {
        profile_lock(m, h);
        h.call(m, 0x36b130, {0, p, 0x49edb8, 0x39a});
        profile_unlock(m, h);
        m.write(a + 0x3264, 4, 0);
    }
    {
        // The inventory pointer is read before the lock callback; both calls
        // run even for a null inventory.
        const auto inv = m.read(a + 0x3524);
        profile_lock(m, h);
        h.call(m, 0x144c70, {inv, 0xffffffffU});
        h.call(m, 0x36b130, {0, inv, 0x49d8d0, 0x3e9});
        profile_unlock(m, h);
    }
    if (m.read(a + 0x31fc, 1)) {
        h.call(m, 0x3ccf30, {a});
        m.write(a + 0x31fc, 1, 0);
    }
    for (std::uint32_t k = 0; k < 3; ++k) h.call(m, 0x185d40, {a, k, 0, 0x3f800000});
    if (m.read(0x5721c8, 1)) {
        if (const auto p = m.read(a + 0x3584)) {
            if (h.call(m, 0x2d2210, {p})) h.call(m, 0x2ec2a8, {m.read(p + 0x24)});
            m.write(a + 0x3584, 4, 0);
        }
        m.write(sp + 0x220, 4, m.read(a + 0x10));
        h.call(m, 0x1b31a0, {sp + 0x21c, 0x55c7a0});
        h.call(m, 0x1b3170, {sp + 0x218, 0x55c7a0, sp + 0x220});
        if (h.call(m, 0x1b3150, {sp + 0x21c, sp + 0x218})) {
            h.call(m, 0x1b3170, {sp + 0x214, 0x55c7a0, sp + 0x220});
            h.call(m, 0x1b3120, {0x55c7a0, sp + 0x214});
        }
        h.call(m, 0x436c00, {sp + 0x23c});
        h.call(m, 0x433ce0, {sp + 0x23c, a + 0xc});
        h.call(m, 0x436ba0, {sp + 0x23c, 0xffffffffU});
    }
    if (const auto c = m.read(a + 0x58)) {
        if (h.call(m, virtual_at(m, c, 0x54), {c})) h.call(m, 0x3fb720, {a});
    }
    if (const auto p = m.read(a + 0x33ec)) {
        profile_lock(m, h);
        h.call(m, 0x45b990, {p, 0xffffffffU});
        h.call(m, 0x36b130, {0, p, 0x49d8d0, 0x402});
        profile_unlock(m, h);
    }
    if (m.read(a + 0x2d, 1) && m.read(a + 0x2c, 1) && !m.read(a + 0x2e, 1)) {
        const std::uint32_t slots[2][2] = {{0x3310, 0x408}, {0x3314, 0x40d}};
        for (const auto& s : slots) {
            const auto p = m.read(a + s[0]);
            if (!p) continue;
            profile_lock(m, h);
            h.call(m, virtual_at(m, p, 8), {p, 0xffffffffU});
            h.call(m, 0x36b130, {0, p, 0x49d8d0, s[1]});
            profile_unlock(m, h);
            m.write(a + s[0], 4, 0);
        }
    }
    {
        const auto list = m.read(a + 0x3528);
        profile_lock(m, h);
        if (list) h.call(m, 0x1be780, {list, 0});
        h.call(m, 0x36b130, {0, list, 0x49d8d0, 0x412});
        profile_unlock(m, h);
    }
    {
        // c.olt.s +33bc < 0 skips the stop; a NaN therefore runs it.
        const auto bits = m.read(a + 0x33bc);
        float stun;
        static_assert(sizeof stun == 4);
        std::memcpy(&stun, &bits, 4);
        if (!(stun < 0.0f)) {
            h.call(m, 0x20eba0, {sp + 0x40, 0x4f12d0, a});
            m.write(sp + 0x60, 1, 1);
            h.call(m, 0x3c9420, {sp + 0x40, 0x4ffb50, 1});
            m.write(sp + 0x40, 4, 0x4dadf0);
            h.call(m, 0x36e220, {sp + 0x154, 0xffffffffU});
            h.call(m, 0x36e220, {sp + 0xb8, 0xffffffffU});
            h.call(m, 0x3c9d60, {sp + 0x40, 0});
        }
    }
    h.call(m, 0x1a02c0, {a});
    h.call(m, 0x22eba0, {a + 0x33f0, 0xffffffffU});
    if (a + 0x32a0) {
        m.write(a + 0x32a0, 4, 0x4da2f0);
        h.call(m, 0x136300, {a + 0x32e0, 0xffffffffU});
        h.call(m, 0x136350, {a + 0x32a0, 0});
    }
    if (a + 0x320c) {
        for (const std::uint32_t off : {0x3258u, 0x3250u, 0x3248u}) h.call(m, 0x1b5f90, {a + off, 0xffffffffU});
        for (const std::uint32_t off : {0x3234u, 0x3220u, 0x320cu}) h.call(m, 0x1b5f40, {a + off, 0xffffffffU});
    }
    if (a + 0x3030) {
        h.call(m, 0x17ed20, {a + 0x30f0, 0xffffffffU});
        h.call(m, 0x1b5ed0, {a + 0x3088, 0xffffffffU});
        h.call(m, 0x1b5e70, {a + 0x3050, 0xffffffffU});
        h.call(m, 0x1b5e20, {a + 0x3030, 0xffffffffU});
    }
    h.call(m, 0x392c10, {a, 0});
    if (frees(a1)) h.call(m, 0x100740, {a});
    return a;
}

} // namespace omega
