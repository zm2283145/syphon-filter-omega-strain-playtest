#include "spawned_actor_ctor.hpp"
#include "agent_perks.hpp"
#include "spawned_actor_ai_base.hpp"
#include "actor_component_attach.hpp"
#include "ai_runtime_init.hpp"
#include "ai_component_bind.hpp"
#include "spawned_actor_ai.hpp"
#include "spawned_npc_body.hpp"
#include "inventory_services.hpp"
#include "spawned_npc_dtor.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace omega {

std::uint32_t SpawnedActorMemory::read(std::uint32_t address, unsigned size) const {
    if (size != 1 && size != 2 && size != 4) throw std::invalid_argument("spawned actor read size");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i) {
        const auto it = bytes_.find(address + i);
        if (it != bytes_.end()) value |= std::uint32_t(it->second) << (8 * i);
    }
    return value;
}

void SpawnedActorMemory::write(std::uint32_t address, unsigned size, std::uint32_t value) {
    if (size != 1 && size != 2 && size != 4) throw std::invalid_argument("spawned actor write size");
    for (unsigned i = 0; i < size; ++i) bytes_[address + i] = std::uint8_t(value >> (8 * i));
}

std::uint32_t SpawnedActorHost::call(SpawnedActorMemory& memory, std::uint32_t address,
                                     std::initializer_list<std::uint32_t> args) {
    if (address == 0x16f5b0) {
        if (args.size() != 2) throw std::invalid_argument("Spawned actor AI base requires two arguments");
        const auto* p = args.begin();
        return run_spawned_actor_ai_base(memory, *this, p[0], p[1]);
    }
    if (address == 0x1b8130) {
        // ResetNpc's rebuild: a0..t3, then the caller's sp (its four stack words).
        if (args.size() != 9) throw std::invalid_argument("Reset actor constructor requires nine arguments");
        const auto* p = args.begin();
        ResetActorArgs q;
        q.actor = p[0]; q.matrix = p[1]; q.node = p[2]; q.manifest = p[3]; q.health = p[4]; q.items = p[5];
        q.role = p[6]; q.name = p[7]; q.stack = p[8];
        return run_reset_actor_ctor_in_place(memory, *this, q);
    }
    if (address == 0x3cce30) {
        if (args.size() != 2) throw std::invalid_argument("Actor component attachment requires two arguments");
        const auto* p = args.begin();
        run_actor_component_attach(memory, *this, p[0], p[1]);
        // 003cce30 leaves v0 undefined; its only hosted caller 001ae450 ignores it.
        return 0;
    }
    if (address == 0x158bc0) {
        if (args.size() != 2) throw std::invalid_argument("AI runtime initializer requires two arguments");
        const auto* p = args.begin();
        run_ai_runtime_init(memory, *this, p[0], p[1]);
        // 00158bc0 leaves the +148 virtual's v0; both 00158800 and 001588d0 ignore it.
        return 0;
    }
    if (address == 0x2300d0) {
        if (args.size() != 2) throw std::invalid_argument("AI component binding requires two arguments");
        const auto* p = args.begin();
        run_ai_component_bind(memory, *this, p[0], p[1]);
        // 002300d0 leaves 003d9d30's (or its caller's) v0; 003cce30 ignores it.
        return 0;
    }
    if (address == 0x1ae450) {
        if (args.size() != 4) throw std::invalid_argument("Spawned actor AI requires four arguments");
        const auto* p = args.begin();
        run_spawned_actor_ai(memory, *this, {p[0], p[1], p[2], p[3]});
        // 001ae450's v0 is ignored by its hosted caller 001b5fe0.
        return 0;
    }
    if (address == 0x1b5fe0) {
        if (args.size() != 10) throw std::invalid_argument("Spawned NPC body requires ten arguments");
        const auto* p = args.begin();
        return run_spawned_npc_body(memory, *this, {p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9]});
    }
    if (address == 0x1294f8) {
        if (args.size() != 1) throw std::invalid_argument("EE strlen requires one argument");
        return run_ee_strlen(memory, *args.begin());
    }
    if (address == 0x129360) {
        if (args.size() != 2) throw std::invalid_argument("EE strcpy requires two arguments");
        const auto* p = args.begin();
        return run_ee_strcpy(memory, p[0], p[1]);
    }
    if (address == 0x1b6a60) {
        if (args.size() != 1) throw std::invalid_argument("Item list init requires one argument");
        return run_item_list_init(memory, *args.begin());
    }
    if (address == 0x1314f0) {
        if (args.size() != 2) throw std::invalid_argument("Agent record accessor requires two arguments");
        const auto* p = args.begin();
        return run_agent_record_accessor(memory, p[0], p[1]);
    }
    if (address == 0x332f90) {
        if (args.size() != 2) throw std::invalid_argument("Agent unlock test requires two arguments");
        const auto* p = args.begin();
        return run_agent_unlock_test(memory, p[0], p[1]);
    }
    if (address == 0x12f370) {
        // Slot +0x54 of the 004d9380/004d9890 AI tables: "jr ra; move v0,zero".
        if (args.size() != 1) throw std::invalid_argument("AI player predicate requires one argument");
        return 0;
    }
    if (address == 0x143df0) {
        if (args.size() != 7) throw std::invalid_argument("Inventory add requires seven arguments");
        const auto* p = args.begin();
        return run_inventory_add(memory, *this, {p[0], p[1], p[2], p[3], p[4], p[5], p[6]});
    }
    if (address == 0x143c50) {
        if (args.size() != 1) throw std::invalid_argument("Inventory equip-first requires one argument");
        return run_inventory_equip_first(memory, *this, *args.begin());
    }
    if (address == 0x147840) {
        if (args.size() != 2) throw std::invalid_argument("Weapon record lookup requires two arguments");
        const auto* p = args.begin();
        return run_weapon_record(memory, p[0], p[1]);
    }
    if (address == 0x1861f0) {
        if (args.size() != 2) throw std::invalid_argument("Clips for actor requires two arguments");
        const auto* p = args.begin();
        return run_clips_for_actor(memory, *this, p[0], p[1]);
    }
    if (address == 0x38c7d0) {
        if (args.size() != 3) throw std::invalid_argument("Model attach requires three arguments");
        const auto* p = args.begin();
        return run_model_attach(memory, *this, p[0], p[1], p[2]);
    }
    if (address == 0x38c5e0) {
        if (args.size() != 1) throw std::invalid_argument("Model attach refresh requires one argument");
        run_model_attach_refresh(memory, *args.begin());
        // 0038c5e0 leaves the caller's v0; its only hosted caller 0038c7d0 ignores it.
        return 0;
    }
    if (address == 0x1b57c0) {
        if (args.size() != 2) throw std::invalid_argument("Spawned NPC destructor requires two arguments");
        const auto* p = args.begin();
        return run_spawned_npc_dtor(memory, *this, p[0], p[1]);
    }
    if (address == 0x158550) {
        if (args.size() != 2) throw std::invalid_argument("AI component destructor requires two arguments");
        const auto* p = args.begin();
        return run_ai_component_dtor(memory, *this, p[0], p[1]);
    }
    if (address == 0x144c70) {
        if (args.size() != 2) throw std::invalid_argument("Inventory destructor requires two arguments");
        const auto* p = args.begin();
        return run_inventory_dtor(memory, *this, p[0], p[1]);
    }
    if (address == 0x3ccf30) {
        if (args.size() != 1) throw std::invalid_argument("Component release requires one argument");
        run_component_release(memory, *this, *args.begin());
        // 003ccf30 leaves the last callee's v0; its only hosted caller 001b57c0 ignores it.
        return 0;
    }
    if (address == 0x1a02c0) {
        if (args.size() != 1) throw std::invalid_argument("Actor unlink requires one argument");
        run_actor_unlink(memory, *this, *args.begin());
        // 001a02c0's v0 is ignored by 001b57c0.
        return 0;
    }
    if (address == 0x184cd0) {
        // Slot +7c of the spawned NPC table 004da690: "jr ra; lbu v0,0x3598(a0)".
        if (args.size() != 1) throw std::invalid_argument("Spawned NPC +3598 accessor requires one argument");
        return memory.read(*args.begin() + 0x3598, 1);
    }
    char hex[9];
    std::snprintf(hex, sizeof hex, "%08x", address);
    throw std::runtime_error(std::string("Spawned actor: unhosted EE service at ") + hex);
}

namespace {
// 001b7b00 (spawned) and 001b8130 (ResetNpc's in-place rebuild) share their body
// instruction for instruction (local/ diff, checkpoint 132sx9) except: 001b8130's
// frame is 0x120, not 0x130; it takes the identity from its caller (393760's a1 is
// the stack word +0, no 0040c240), its stack words are +0 the identity pointer, +8
// the generator-spawned flag (+3598 and 001b5fe0's t2), +10 the serial and +18 an
// optional 0x440-byte appearance; its allocator line is 0x1c0 (not 0x21c); it does
// no +c identity tagging; and with an appearance it copies it to +a0 and passes a
// null manifest. Role 3 / 4 (sb zero +14) is not hosted: only role 2 runs.
struct CtorInputs {
    std::uint32_t actor{}, matrix{}, node{}, manifest{}, health{}, items{}, role{}, name{}, stack{};
};
std::uint32_t run_ctor(SpawnedActorMemory& m, SpawnedActorHost& h, const CtorInputs& q, bool reset) {
    if (q.role != 2) throw std::runtime_error("Spawned actor: only solo role 2 is hosted");
    if (!q.actor) throw std::runtime_error("Spawned actor: required EE actor is null");
    const auto a = q.actor;
    const auto sp = q.stack - (reset ? 0x120 : 0x130);
    const auto r8 = [&](std::uint32_t p) { return m.read(p, 1); };
    const auto r32 = [&](std::uint32_t p) { return m.read(p); };
    const auto w8 = [&](std::uint32_t p, std::uint32_t v) { m.write(p, 1, v); };
    const auto w32 = [&](std::uint32_t p, std::uint32_t v) { m.write(p, 4, v); };
    const auto call = [&](std::uint32_t address, std::initializer_list<std::uint32_t> args = {}) {
        return h.call(m, address, args);
    };

    if (reset) {
        call(0x393760, {a, r32(q.stack), 0x49d010});
    } else {
        call(0x40c240, {sp + 0x12c, 0x45});
        call(0x393760, {a, sp + 0x12c, 0x49d010});
    }
    const auto s = a + 0x3030;
    call(0x1b7ae0, {s});
    w8(s + 0xc, 1);
    auto token = call(0x136080);
    call(0x132020, {s + 0x10, token});
    call(0x1b7a40, {s + 0x20});
    call(0x1b79f0, {s + 0x58});
    w32(s + 0x6c, 0);
    w32(s + 0x70, 0);
    w32(s + 0x74, 0xff7fffff);
    w32(s + 0x78, 0);
    w8(s + 0x7c, 0); w8(s + 0x7d, 0);
    token = call(0x136080);
    call(0x132020, {s + 0x80, token});
    w32(s + 0x90, 0);
    token = call(0x136080);
    call(0x132020, {s + 0xa0, token});
    w32(s + 0xb0, 0);
    call(0x17f0e0, {s + 0xc0});
    for (std::uint32_t off = 0x180; off <= 0x187; ++off) w8(s + off, 0);
    token = call(0x136080);
    call(0x132020, {s + 0x190, token});
    w32(s + 0x1a0, 0); w32(s + 0x1a4, 0); w32(s + 0x1a8, 0);
    w8(s + 0x1ac, 1); w8(s + 0x1ad, 0); w32(s + 0x1b0, 0);
    w32(s + 0x68, call(0x1b7940, {s, 0, 0xbf800000}));
    call(0x1b7930, {s + 0x20, 0x41700000});
    call(0x1b7920, {s + 0xc0, 0x41200000});

    w32(a, 0x4da690);
    w32(a + 0x31f0, r32(q.stack + (reset ? 0x10 : 8)));
    w32(a + 0x31f4, 0xbf800000);
    w32(a + 0x31f8, 0x4212c28f);
    w8(a + 0x31fc, 0);
    for (std::uint32_t off : {0x3200U, 0x3204U, 0x3208U}) call(0x1b78f0, {a + off, 0});
    for (std::uint32_t off : {0x320cU, 0x3220U, 0x3234U}) call(0x1b75b0, {a + off, 0});
    for (std::uint32_t off : {0x3248U, 0x3250U, 0x3258U}) call(0x1b7580, {a + off, 0});
    w8(a + 0x3260, 0);
    w8(a + 0x3268, 0); w8(a + 0x3269, 0); w8(a + 0x326a, 0);
    const auto pair_a = sp + (reset ? 0x11c : 0x128), pair_b = sp + (reset ? 0x118 : 0x124);
    w32(pair_b, 0); w32(pair_a, 0);
    call(0x136540, {a + 0x32a0, pair_a, pair_b});
    w32(a + 0x32a0, 0x4da2f0);
    w32(a + 0x32c0, 0);
    token = call(0x136080);
    call(0x132020, {a + 0x32d0, token});
    call(0x1364b0, {a + 0x32e0});
    w32(a + 0x3300, 0);
    w32(a + 0x3310, 0); w32(a + 0x3314, 0);
    w32(a + 0x33a0, 0);
    w8(a + 0x33a4, 0); w8(a + 0x33a5, 0);
    for (std::uint32_t off : {0x33a8U,0x33acU,0x33b0U,0x33b4U}) w32(a + off, 0);
    w32(a + 0x33cc, r32(0x4bdd00));
    for (std::uint32_t off : {0x33d0U,0x33d4U,0x33d8U,0x33dcU,0x33e0U}) w32(a + off, 0);
    w8(a + 0x33e4, 0); w8(a + 0x33e5, 0); w8(a + 0x33e6, 0); w8(a + 0x33e8, 0);
    call(0x22eca0, {a + 0x33f0});
    w32(a + 0x3464, 0); w8(a + 0x3468, 0); w32(a + 0x3474, 0);
    w32(a + 0x347c, 0xffffffff); w32(a + 0x3478, 0);
    const auto route = sp + (reset ? 0x110 : 0x11c);
    w32(route, 0xffffffff); w32(a + 0x347c, r32(route));
    w32(a + 0x3484, 0xfffffffe); w32(a + 0x3480, 0); w8(a + 0x3490, 0);
    call(0x190060, {sp + 0xc0});
    call(0x17cd90, {a + 0x34a0, sp + 0xc0});
    call(0x17f960, {sp + 0xb0});
    call(0x132020, {a + 0x34c0, sp + 0xb0});
    token = call(0x136080);
    call(0x132020, {a + 0x34d0, token});
    w32(a + 0x3514, 0xffffffff); w8(a + 0x3518, 0xff);
    w32(a + 0x351c, 0); w32(a + 0x3520, 0);
    w32(a + 0x3530, 0x4da7b8); w32(a + 0x3530, 0x4da7a8);
    w32(a + 0x3534, 0); w8(a + 0x3539, 0); w8(a + 0x353a, 0);
    w32(a + 0x353c, 0); w32(a + 0x3540, 0x3f800000); w32(a + 0x3544, 0);
    w32(a + 0x3548, q.node);
    for (std::uint32_t off : {0x354cU,0x3554U,0x355cU}) { w32(a + off, 0); w32(a + off + 4, 0xfffffffe); }
    w32(a + 0x3570, 0); w32(a + 0x3574, 0);
    w32(a + 0x3578, 0x3f800000); w32(a + 0x357c, 0x80000000);
    w32(a + 0x3580, 0); w32(a + 0x3584, 0); w32(a + 0x3594, 0);
    const auto flag = q.stack + (reset ? 8 : 0);
    w8(a + 0x3598, r32(flag) ? 1 : 0);
    if (r8(0x533880)) call(0x10dc40, {9});
    w32(0x533888, r32(0x533888) + 1);
    const auto allocation = call(0x36b180, {0, 0xf8, 0x49d8d0, reset ? 0x1c0U : 0x21cU});
    if (r8(0x533880)) call(0x10dca8, {9});
    w32(0x533888, r32(0x533888) - 1);
    const auto component = allocation ? call(0x144dc0, {allocation, a, 0}) : 0;
    w32(a + 0x3524, component);
    w32(a + 0x3534, r32(a + 0x3524));
    auto manifest = q.manifest;
    if (reset) {
        // 001b863c..001b8668: lq / sq (16-byte aligned) 0x44 quadwords to +a0; the manifest becomes null.
        if (const auto source = r32(q.stack + 0x18)) {
            const auto from = source & ~0xfU, to = (a + 0xa0) & ~0xfU;
            for (std::uint32_t i = 0; i < 0x440; i += 4) w32(to + i, r32(from + i));
            manifest = 0;
        }
    } else if (r32(q.stack)) {
        if (r8(0x5721c8)) throw std::runtime_error("Spawned actor: multiplayer identity path 001b8030 unhosted");
        w32(a + 0xc, r32(a + 0xc) | 0x01000000);
    }
    if (const auto draw = r32(a + 0x31f0); draw == 0xffffffff) {
        auto counter = r32(0x4ee968);
        w32(a + 0x31f0, spawned_actor_serial(draw, counter));
        w32(0x4ee968, counter);
    }
    // The tenth argument is this frame's sp (001b5fe0's entry sp); the name is
    // the stack word there (sd at 001b80c8), marshalled by 001b5fe0 itself.
    call(0x1b5fe0, {a, q.matrix, manifest, q.health, 0, q.items, r32(flag), q.role, q.name, sp});
    w32(a + 0x30, 1);
    return a;
}
} // namespace

std::uint32_t run_spawned_actor_ctor(SpawnedActorMemory& m, SpawnedActorHost& h,
                                      const SpawnedActorArgs& q) {
    // The caller's two stack words are explicit API inputs; child callbacks
    // may mutate them before the constructor's later lw instructions.
    m.write(q.stack, 4, q.pair);
    m.write(q.stack + 8, 4, q.serial);
    return run_ctor(m, h, {q.actor, q.matrix, q.node, q.manifest, q.health, q.items, q.role, q.name, q.stack}, false);
}

std::uint32_t run_reset_actor_ctor(SpawnedActorMemory& m, SpawnedActorHost& h, const ResetActorArgs& q) {
    m.write(q.stack, 4, q.id_pointer);
    m.write(q.stack + 8, 4, q.generator_spawned);
    m.write(q.stack + 0x10, 4, q.serial);
    m.write(q.stack + 0x18, 4, q.appearance);
    return run_reset_actor_ctor_in_place(m, h, q);
}

std::uint32_t run_reset_actor_ctor_in_place(SpawnedActorMemory& m, SpawnedActorHost& h, const ResetActorArgs& q) {
    return run_ctor(m, h, {q.actor, q.matrix, q.node, q.manifest, q.health, q.items, q.role, q.name, q.stack}, true);
}

void run_reset_npc(SpawnedActorMemory& m, SpawnedActorHost& h, std::uint32_t component, std::uint32_t stack) {
    const auto sp = stack - 0x1b0;
    const auto r8 = [&](std::uint32_t p) { return m.read(p, 1); };
    const auto r32 = [&](std::uint32_t p) { return m.read(p); };
    const auto w32 = [&](std::uint32_t p, std::uint32_t v) { m.write(p, 4, v); };
    const auto call = [&](std::uint32_t address, std::initializer_list<std::uint32_t> args = {}) {
        return h.call(m, address, args);
    };
    // 00148624..00148638: the actor's +7c (004da690: 00184cd0, lbu +3598).
    const auto gate = r32(component + 0x30);
    if (call(r32(r32(gate) + 0x7c), {gate})) return;
    const auto actor = r32(component + 0x30);
    std::uint32_t copy = 0;
    call(0x129360, {sp + 0x110, actor + 0x3318});
    const auto health = r32(actor + 0x3398);
    w32(sp + 0x1ac, r32(r32(component + 0x30) + 0xc));
    w32(sp + 0x198, r32(actor + 0x3588));
    w32(sp + 0x19c, r32(actor + 0x358c));
    const auto name = r32(actor + 0x3264);
    const auto node = r32(actor + 0x3548);
    const auto length = name ? call(0x1294f8, {name}) : 0;
    const auto heap = [&](std::uint32_t address, std::uint32_t a1, std::uint32_t line) {
        if (r8(0x533880)) call(0x10dc40, {9});
        w32(0x533888, r32(0x533888) + 1);
        const auto v = call(address, {0, a1, 0x49b670, line});
        if (r8(0x533880)) call(0x10dca8, {9});
        w32(0x533888, r32(0x533888) - 1);
        return v;
    };
    if (length) {
        copy = heap(0x36b180, length + 1, 0x1f02);
        call(0x129360, {copy, r32(actor + 0x3264)});
    }
    call(r32(r32(actor + 0x33f0) + 0xe8), {sp + 0xd0, actor + 0x33f0});
    call(r32(r32(component) + 0x4c), {component});
    const auto serial = r32(actor + 0x31f0);
    call(r32(r32(actor) + 8), {actor, 0xffffffff});
    if (actor) {
        for (std::uint32_t i = 0; i < 0x40; i += 4) w32(sp + 0x90 + i, r32(sp + 0xd0 + i));
        // 001487ac..001487bc: sd, the high words the sign of each 32-bit register.
        const auto sd = [&](std::uint32_t at, std::uint32_t v) { w32(at, v); w32(at + 4, (v & 0x80000000U) ? 0xffffffffU : 0); };
        sd(sp, sp + 0x1ac); sd(sp + 8, 0); sd(sp + 0x10, serial); sd(sp + 0x18, 0);
        call(0x1b8130, {actor, sp + 0x90, node, sp + 0x110, health, sp + 0x198, 2, copy, sp});
    }
    if (copy) heap(0x36b130, copy, 0x1f13);
}

std::uint32_t spawned_actor_serial(std::uint32_t draw, std::uint32_t& counter_004ee968) {
    if (draw != 0xffffffffU) return draw;
    return counter_004ee968++;
}

} // namespace omega
