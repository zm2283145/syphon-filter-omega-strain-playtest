#include "inventory_services.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace omega {
namespace {

float bits_to_float(std::uint32_t u) { float f; std::memcpy(&f, &u, sizeof f); return f; }
std::uint32_t sext8(std::uint32_t b) { return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(b))); }

constexpr std::uint32_t weapon_database_word = 0x4ffd30;
constexpr std::uint32_t slot_of_class = 0x489d60;
constexpr std::uint32_t holster_points = 0x489d70;
constexpr std::uint32_t hand_points = 0x489d78;

} // namespace

std::uint32_t run_weapon_record(const SpawnedActorMemory& m, std::uint32_t db, std::uint32_t item) {
    const auto i = static_cast<std::int32_t>(item);
    if (i < 0 || !(i < static_cast<std::int32_t>(m.read(m.read(db + 4) + 4)))) item = 0;
    return m.read(db + 0x14) + item * 0x140;
}

std::uint32_t run_clips_for_actor(SpawnedActorMemory& m, SpawnedActorHost& h, std::uint32_t actor,
                                  std::uint32_t clips) {
    const auto r32 = [&](std::uint32_t p) { return m.read(p); };
    const auto predicate = [&](std::uint32_t c) { return h.call(m, r32(r32(c) + 0x54), {c}); };
    auto c = r32(actor + 0x58);
    if (!c || !predicate(c)) return clips;
    std::uint32_t slot = 0;
    if (m.read(0x5721c8, 1)) {
        c = r32(actor + 0x58);
        const bool player = c && predicate(c);
        slot = player ? ((r32(actor + 0xc) & 0x7f000000U) >> 24) - 1 : 0xffffffffU;
    }
    c = r32(actor + 0x58);
    // A null component or a predicate with a zero low byte queries agent data 0.
    const auto data = c && (predicate(c) & 0xff) ? h.call(m, 0x1314f0, {0x4ffb50, slot}) : 0;
    if (!h.call(m, 0x332f90, {data, 5})) return clips;
    const float v = static_cast<float>(static_cast<std::int32_t>(clips)) * bits_to_float(r32(0x48a2d0));
    // cvt.w.s as the interpreter golden evaluates it (truncation, saturated);
    // NaN has no defined result. A denormal product flushes to 0 either way.
    if (std::isnan(v)) throw std::runtime_error("Clips for actor: cvt.w.s of NaN is undefined");
    std::uint32_t k;
    if (v >= 2147483647.0F) k = 0x7fffffffU;
    else if (v <= -2147483648.0F) k = 0x80000000U;
    else k = static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
    return k == clips ? k + 1 : k;
}

void run_model_attach_refresh(SpawnedActorMemory& m, std::uint32_t a) {
    const auto r8 = [&](std::uint32_t p) { return m.read(p, 1); };
    const auto r32 = [&](std::uint32_t p) { return m.read(p); };
    const auto w32 = [&](std::uint32_t p, std::uint32_t v) { m.write(p, 4, v); };
    // lbu results are never negative, so the original bltz/beq -1 guards never branch.
    const auto table = [&](std::uint32_t base, std::uint32_t index) {
        return sext8(r8(base + r8(a + 0x2c0c) * 48 + index));
    };
    if (const auto part = r32(a + 0x2414)) {
        const auto k = r8(part + 0x40d);
        w32(a + 0x23e4, 0x3f800000U);
        w32(a + 0x23e8, table(0x571528, k));
        w32(a + 0x23ec, table(0x571528, k + 1));
        w32(a + 0x23e0, 0);
    } else {
        w32(a + 0x23e4, 0);
        w32(a + 0x23e0, 0);
    }
    if (const auto part = r32(a + 0x2410)) {
        const auto k = r8(part + 0x40d);
        w32(a + 0x23d4, 0x3f800000U);
        w32(a + 0x23d8, table(0x571510, k));
        w32(a + 0x23dc, table(0x571510, k + 1));
        w32(a + 0x23d0, 0);
        const auto j = r8(r32(a + 0x2410) + 0x40e);
        w32(a + 0x23e4, 0x3f800000U);
        w32(a + 0x23e8, table(0x571528, j));
        w32(a + 0x23ec, table(0x571528, j));
        w32(a + 0x23e0, 0);
    } else {
        w32(a + 0x23d4, 0);
        w32(a + 0x23d0, 0);
    }
}

std::uint32_t run_model_attach(SpawnedActorMemory& m, SpawnedActorHost& h, std::uint32_t model,
                               std::uint32_t point, std::uint32_t part) {
    if (part && m.read(part + 0x40c, 1) >= 8) return 0;
    const auto at = model + point * 4;
    m.write(at + 0x2410, 4, part);
    h.call(m, 0x38c5e0, {model});
    return at;
}

std::uint32_t run_inventory_equip_first(SpawnedActorMemory& m, SpawnedActorHost& h, std::uint32_t inv) {
    for (std::uint32_t k = 0; k < 6; ++k)
        if (m.read(inv + 8 + 16 * k) != 0xffffffffU) return h.call(m, 0x142c50, {inv, k});
    return 0;
}

std::uint32_t run_inventory_add(SpawnedActorMemory& m, SpawnedActorHost& h, const InventoryAddArgs& q) {
    const auto inv = q.inventory;
    const auto item = q.item;
    const auto sp = q.stack - 0xf0;
    const auto r8 = [&](std::uint32_t p) { return m.read(p, 1); };
    const auto r32 = [&](std::uint32_t p) { return m.read(p); };
    const auto w8 = [&](std::uint32_t p, std::uint32_t v) { m.write(p, 1, v); };
    const auto w32 = [&](std::uint32_t p, std::uint32_t v) { m.write(p, 4, v); };
    const auto call = [&](std::uint32_t address, std::initializer_list<std::uint32_t> args) {
        return h.call(m, address, args);
    };

    const auto db = r32(weapon_database_word); // read once into s1
    w8(sp + 0xaf, q.no_net);
    const auto record = call(0x147840, {db, item});
    auto slot = r8(slot_of_class + r8(record + 0x30)); // lb; only its low byte is used
    auto index = slot;
    auto entry = inv + index * 16 + 4;
    if (q.preload) call(0x145710, {r32(weapon_database_word), item, 0});
    const auto held = r32(entry + 4);
    if (held != 0xffffffffU && slot == 0 && item == held && r32(inv + 0x18) == 0xffffffffU) {
        slot = 1;
        index = 1;
        entry = inv + 0x14;
    }
    w32(entry + 4, item);
    if (q.give_ammo) {
        const auto ammo = call(0x147840, {db, item});
        w32(sp + 0xa8, ammo);
        w32(entry + 8, r32(ammo + 0x70));
        const auto clips = call(0x1861f0, {r32(inv + 0x88), r32(ammo + 0x74)});
        // mult rd: the low word of the product.
        w32(entry + 0xc, r32(r32(sp + 0xa8) + 0x70) * clips);
    } else {
        w32(entry + 8, 0);
        w32(entry + 0xc, 0);
    }
    const auto current = r8(inv + 0x84);
    auto point = r8((slot == current ? hand_points : holster_points) + index);
    if (slot == 1 && current == 0) point = 1;
    const auto model = r32(call(0x147840, {db, item}) + 0x13c);
    if (model) call(0x38c7d0, {r32(inv + 0x88) + 0x60, point, model});
    if (slot == 6 || slot == 7) w8(r32(inv + 0x88) + point + 0x24a2, 0);
    if (r8(record + 0xf0) == 5) {
        if (const auto object = r32(inv)) w32(object, record);
    }
    if (r8(0x5721c8) && !r8(sp + 0xaf)) {
        const auto message = sp + 0xb0;
        call(0x3c9e30, {message, 0x52acc8});
        w32(sp + 0xe8, (q.give_ammo & 0xff) | ((q.preload & 0xff) << 1));
        w8(sp + 0xdc, 5);
        w32(message, 0x4de610);
        w8(sp + 0xe4, item);
        w32(sp + 0xcc, 0x40);
        w8(sp + 0xd0, 4);
        call(0x3c9420, {message, r32(inv + 0x88), 0});
        w32(message, 0x4de610);
        call(0x3c9d60, {message, 0});
    }
    return 0;
}

} // namespace omega
