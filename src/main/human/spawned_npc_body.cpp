#include "spawned_npc_body.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace omega {
namespace {

float bits_to_float(std::uint32_t u) { float f; std::memcpy(&f, &u, sizeof f); return f; }

// cvt.w.s as the interpreter golden evaluates it: truncation, saturated.
// The interpreter has no defined result for NaN (inf*0 or a NaN operand).
// Subnormal products need no flush: |v| < 1 truncates to 0 either way.
std::uint32_t cvt_w(float v) {
    if (std::isnan(v)) throw std::runtime_error("Spawned NPC body: cvt.w.s of NaN is undefined");
    if (v >= 2147483647.0F) return 0x7fffffffU;
    if (v <= -2147483648.0F) return 0x80000000U;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
}

} // namespace

std::uint32_t run_ee_strlen(const SpawnedActorMemory& m, std::uint32_t text) {
    std::uint32_t n = 0;
    while (m.read(text + n, 1)) ++n;
    return n;
}

std::uint32_t run_ee_strcpy(SpawnedActorMemory& m, std::uint32_t dst, std::uint32_t src) {
    for (std::uint32_t i = 0;; ++i) {
        const auto b = m.read(src + i, 1);
        m.write(dst + i, 1, b);
        if (!b) return dst;
    }
}

std::uint32_t run_item_list_init(SpawnedActorMemory& m, std::uint32_t list) {
    m.write(list, 4, 0); m.write(list + 4, 4, 0); m.write(list + 8, 4, 0);
    m.write(list + 0xc, 1, 1);
    return list;
}

std::uint32_t run_spawned_npc_body(SpawnedActorMemory& m, SpawnedActorHost& h,
                                   const SpawnedNpcBodyArgs& q) {
    if (!q.actor) throw std::runtime_error("Spawned NPC body: required EE actor is null");
    // The EE ABI keeps sp quadword aligned (the frame is 0x790); the sq into
    // the local matrix at sp+0x610 relies on it.
    if (q.stack & 15) throw std::runtime_error("Spawned NPC body: EE stack must be quadword aligned");
    const auto a = q.actor;
    const auto entry = q.stack;
    const auto sp = entry - 0x790;
    const auto role = q.role & 0xff;
    const auto r8 = [&](std::uint32_t p) { return m.read(p, 1); };
    const auto r32 = [&](std::uint32_t p) { return m.read(p); };
    const auto w8 = [&](std::uint32_t p, std::uint32_t v) { m.write(p, 1, v); };
    const auto w32 = [&](std::uint32_t p, std::uint32_t v) { m.write(p, 4, v); };
    const auto call = [&](std::uint32_t address, std::initializer_list<std::uint32_t> args) {
        return h.call(m, address, args);
    };
    // Inlined lock/counter pattern around 0036b180 with "human.cc" 0049d8d0.
    const auto allocate = [&](std::uint32_t size, std::uint32_t line) {
        if (r8(0x533880)) call(0x10dc40, {9});
        w32(0x533888, r32(0x533888) + 1);
        const auto p = call(0x36b180, {0, size, 0x49d8d0, line});
        if (r8(0x533880)) call(0x10dca8, {9});
        w32(0x533888, r32(0x533888) - 1);
        return p;
    };
    // Lazy multiplayer character word 00494178 (-1 until 002ebaf8 fills it).
    const auto character = [&] {
        if (r32(0x494178) == 0xffffffffU) call(0x2ebaf8, {0x494178});
        return r32(0x494178);
    };
    const auto predicate = [&](std::uint32_t c) { return call(r32(r32(c) + 0x54), {c}); };

    w32(entry, q.name);
    const auto record = r32(0x4ffbd8); // read once, before any call
    w32(sp + 0xac, q.items);
    call(0x36e5d0, {sp + 0x650});
    call(0x36e5d0, {sp + 0x570});

    auto list = allocate(0x14, 0x31a);
    if (list) { call(0x1b6a60, {list}); w32(list + 0x10, a); }
    w32(a + 0x3528, list);
    w8(a + 0x326b, 0);
    if (role == 1 || role == 3) {
        auto light = allocate(0x1c, 0x31e);
        if (light) light = call(0x45bad0, {light, a});
        w32(a + 0x33ec, light);
    } else {
        w32(a + 0x33ec, 0);
    }

    for (std::uint32_t i = 0; i < 12; ++i) w32(a + 0x34e4 + 4 * i, 0xffffffffU);
    w32(a + 0x34e0, 0); w8(a + 0x33e7, 0); w32(a + 0x352c, 0); w8(a + 0x33b8, 0);
    w8(a + 0x3470, 0); w8(a + 0x346f, 0); w8(a + 0x346e, 0); w8(a + 0x3538, 0);
    w32(a + 0x33bc, 0xbf800000U); w8(a + 0x33b9, 0); w8(a + 0x3469, 0); w8(a + 0x346b, 0);
    w8(a + 0x346a, 0); w8(a + 0x346c, 0); w32(a + 0x3480, 0);
    w32(sp + 0x77c, 0xffffffffU);
    w32(a + 0x3484, r32(sp + 0x77c));
    w8(a + 0x3488, 0); w8(a + 0x346d, 0);
    for (std::uint32_t i = 0; i < 3; ++i) {
        w32(a + 0x354c + 8 * i, 0);
        w32(sp + 0x778, 0xffffffffU);
        w32(a + 0x3550 + 8 * i, r32(sp + 0x778));
    }

    call(0x128bd0, {sp + 0x6f0, 0x49e8f0, 0x48b2f8, 1});
    w32(a + 0x3398, q.health);
    call(0x3cc990, {a, q.health, 0});
    w32(a + 0x339c, q.armor);
    w32(a + 0x33a0, q.armor);

    const auto name = r32(entry);
    const auto length = name ? call(0x1294f8, {name}) : 0;
    if (length) {
        w32(a + 0x3264, allocate(length + 1, 0x33f));
        const auto source = r32(entry);
        call(0x129360, {r32(a + 0x3264), source});
    } else {
        w32(a + 0x3264, 0);
    }

    std::uint32_t skeleton = 0;
    if (q.manifest) {
        const bool multiplayer = r8(0x5721c8) != 0;
        const auto key = multiplayer ? a + 0x3318 : q.manifest;
        if (multiplayer && role == 1 && !r8(0x5721c0)) {
            (void)r32(0x5721a8); // dead load at 001b63b0
            const auto look = r8(0x5721c8) ? character() : 0;
            const std::uint32_t model = look == 3 ? 0x49e928 : look == 1 ? 0x49e908 : 0x49e918;
            call(0x129a70, {a + 0x3318, model, 0x80});
        } else {
            call(0x129a70, {a + 0x3318, q.manifest, 0x80});
        }
        skeleton = call(0x407da0, {0x55d4f8, key});
        if (!skeleton) {
            const auto file = call(0x3f96e0, {0x4ffb8c, sp + 0x6f0, 0, 0});
            call(0x36e1b0, {sp + 0x650, key, file, 0});
            if (call(0x36def0, {sp + 0x650}) & 0xff) {
                call(0x128bd0, {multiplayer ? sp + 0xf0 : sp + 0xb0, 0x49e940, q.manifest});
                call(0x36e220, {sp + 0x570, 0xffffffffU});
                call(0x36e220, {sp + 0x650, 0xffffffffU});
                return 0;
            }
            const auto parsed = call(0x36de50, {sp + 0x650});
            call(0x406700, {sp + 0x130, sp + 0x650, parsed});
            skeleton = sp + 0x130;
        }
    }
    w32(record, q.manifest);
    w32(record + 4, skeleton);
    w32(record + 8, r32(a + 0x31f0));

    // lq/sq ignore the low four address bits; order follows 001b6624..6644.
    using Quad = std::array<std::uint8_t, 16>;
    const auto lq = [&](std::uint32_t p) {
        Quad v{};
        for (std::uint32_t i = 0; i < 16; ++i) v[i] = static_cast<std::uint8_t>(m.read((p & ~15U) + i, 1));
        return v;
    };
    const auto sq = [&](std::uint32_t p, const Quad& v) {
        for (std::uint32_t i = 0; i < 16; ++i) m.write((p & ~15U) + i, 1, v[i]);
    };
    const auto local = sp + 0x610;
    const auto q0 = lq(q.matrix);
    const auto q1 = lq(q.matrix + 0x10);
    sq(local, q0);
    const auto q2 = lq(q.matrix + 0x20);
    sq(local + 0x10, q1);
    const auto q3 = lq(q.matrix + 0x30);
    sq(local + 0x20, q2);
    sq(local + 0x30, q3);
    call(r32(r32(a) + 0x40), {a, local});
    call(r32(r32(a) + 0x44), {a, record});

    for (std::uint32_t i = 0; i < 2; ++i) {
        const auto slot = r32(sp + 0xac) + 4 * i;
        w32(a + 0x3588 + 4 * i, r32(slot));
        const auto item = r32(slot);
        if (item == 0xffffffffU) continue;
        const std::uint32_t paired = r8(0x5721c8) && q.pair ? 1 : 0;
        call(0x143df0, {r32(a + 0x3524), item, 1, 0, 0, paired, sp});
    }
    call(0x143c50, {r32(a + 0x3524)});

    if (!r8(0x5721c8)) {
        call(0x1ae450, {a, q.role, q.pair, 0});
    } else {
        if (role != 4) call(0x1ae450, {a, q.role, q.pair, 0});
        if (role != 4 && role != 3 && !q.pair) {
            const auto token = sp + 0x78c;
            call(0x436c00, {token});
            (void)r32(0x5721a8); // dead load at 001b67a8
            const auto team = r8(0x5721c8) ? character() : 0;
            call(0x435f30, {token, a + 0xc, team});
            if (role == 1) {
                (void)r32(0x5721a8); // dead load at 001b6808
                const auto again = r8(0x5721c8) ? character() : 0;
                call(0x434600, {token, a + 0xc, again});
            }
            call(0x436ba0, {token, 0xffffffffU});
        }
    }

    auto component = r32(a + 0x58);
    if (component && predicate(component)) {
        std::uint32_t slot = 0;
        if (r8(0x5721c8)) {
            component = r32(a + 0x58);
            const bool player = component && predicate(component);
            slot = player ? ((r32(a + 0xc) & 0x7f000000U) >> 24) - 1 : 0xffffffffU;
        }
        component = r32(a + 0x58);
        // Only the low byte of this third predicate result is tested (001b6920).
        if (component && (predicate(component) & 0xff)) {
            const auto unlocks = call(0x1314f0, {0x4ffb50, slot});
            if (unlocks) {
                if (call(0x332f90, {unlocks, 3})) {
                    const auto armor = static_cast<float>(static_cast<std::int32_t>(r32(a + 0x33a0)));
                    const auto k = cvt_w(armor * bits_to_float(r32(0x48a1c8)));
                    w32(a + 0x339c, k);
                    w32(a + 0x33a0, k);
                }
                if (call(0x332f90, {unlocks, 4})) {
                    const auto k = cvt_w(bits_to_float(r32(a + 0x40)) * bits_to_float(r32(0x48a1d0)));
                    w32(a + 0x3398, k);
                    call(0x3cc990, {a, k, 0});
                }
            }
        }
    }

    call(r32(r32(a + 0x33f0) + 0x90), {a + 0x33f0, q.matrix, 0});
    w32(a + 0x3450, cvt_w(bits_to_float(r32(a + 0x40))));
    w32(a + 0x3458, r32(a + 0x33a0));
    call(0x36e220, {sp + 0x570, 0xffffffffU});
    call(0x36e220, {sp + 0x650, 0xffffffffU});
    return 1;
}

} // namespace omega
