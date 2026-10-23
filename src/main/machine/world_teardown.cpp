#include "world_teardown.hpp"

#include <initializer_list>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
constexpr std::uint32_t lock_flag = 0x533880;
constexpr std::uint32_t lock_count = 0x533888;
constexpr std::uint32_t world_cc = 0x49a890;      // the source-file name passed to the free 0036b130
constexpr std::uint32_t singleton_cc = 0x49aea0;  // the same for the 004ea0b8 object
constexpr std::uint32_t singleton = 0x4ea0b8;
constexpr std::uint32_t mp = 0x5721c8, mp_quit = 0x5721d8, mp_text = 0x5723b0, mp_b8 = 0x5721b8;
constexpr std::uint32_t none = 0xffffffffU;
}  // namespace

void run_world_teardown(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t g, std::uint32_t sp) {
    const auto r8 = [&](std::uint32_t a) { return m.read(a, 1); };
    const auto r32 = [&](std::uint32_t a) { return m.read(a); };
    const auto w8 = [&](std::uint32_t a, std::uint32_t v) { m.write(a, 1, v); };
    const auto w32 = [&](std::uint32_t a, std::uint32_t v) { m.write(a, 4, v); };
    const auto call = [&](std::uint32_t a, std::initializer_list<std::uint32_t> args = {}) {
        return host.call(m, a, args);
    };
    const std::uint32_t frame = sp - 0x80;
    // The inlined allocator lock: the flag is read before the object (in the
    // branch delay slot) and reread for the unlock; the count is reread each time.
    const auto lock = [&] {
        if (r8(lock_flag)) call(0x10dc40, {9});
        w32(lock_count, r32(lock_count) + 1);
    };
    const auto unlock = [&] {
        if (r8(lock_flag)) call(0x10dca8, {9});
    };
    const auto count_down = [&] { w32(lock_count, r32(lock_count) - 1); };
    const auto destroy = [&](std::uint32_t object) {
        if (!object) throw std::runtime_error("World_Teardown: vtable +8 call on a null object");
        call(r32(r32(object) + 8), {object, none});
    };

    call(0x2569c0, {g});
    const auto service = call(0x3caa50, {g, 0x4f2cc0});
    if (service) call(0x1c9680, {service});
    if (const auto objectives = call(0x225cd0)) call(0x2261c0, {objectives});

    auto object = r32(g + 0xec);
    lock();
    call(0x279d80, {object, none});
    call(0x36b130, {0, object, world_cc, 0x2cf});
    unlock();
    count_down();
    w32(g + 0xec, 0);
    call(0x26bd60);
    call(0x26be70, {0});

    object = r32(g + 0x8c);
    lock();
    call(0x272920, {object, none});
    call(0x36b130, {0, object, world_cc, 0x2d3});
    unlock();
    count_down();
    w32(g + 0x8c, 0);
    call(0x2609e0, {r32(g + 0xe0), 0, 1});
    host.call_float(m, 0x260990, {r32(g + 0xe0), 0}, {0, 0, 0, 0});
    call(0x25fbb0, {r32(g + 0xe0)});
    call(0x2b2f80, {r32(g + 0xe4)});
    w32(0x53b350, 0);
    w32(0x53b358, 0);
    call(0x275550);
    call(0x238f20);
    call(0x3e2150, {0x555070});
    call(0x2683f0);
    call(0x16aa80);
    call(0x3fdbb0);
    call(0x136bd0, {g});
    call(0x1784b0, {r32(g + 0x84)});
    w32(frame + 0x5c, g + 0xa0);
    w32(frame + 0x60, r32(g + 0xa4));
    call(0x138c60, {frame + 0x64, g + 0x9c, frame + 0x60, frame + 0x5c});
    call(0x170d30, {r32(g + 0x80)});
    call(0x170570);
    call(0x22d3c0, {0x4f77c0});
    call(0x408ea0, {0x53bb90});
    call(0x3ec3e0);
    call(0x136cd0, {g});
    call(0x257150);

    // The multiplayer block (005721c8): 005721d8 is a one-shot latch; 005723b0
    // 1 or 2 selects the session end passed to 00134110.
    std::uint32_t session_end = 0;
    if (r8(mp)) {
        bool latched = false;
        if (r8(mp_quit)) {
            latched = true;
            w8(mp_quit, 0);
        }
        if (latched) {
            const auto kind = r8(mp_text);
            if (kind == 1) session_end = 1;
            else if (kind == 2) session_end = 2;
        }
        call(0x42e6d0);
        call(0x3e56f0);
        call(0x42b2d0);
        if (!r8(g + 0x1c7)) {
            if (call(0x4294c0)) call(0x44e340, {call(0x4294c0)});
            call(0x42b1f0);
        }
    }
    if (!r8(g + 0x1c5)) call(0x134110, {g, session_end});
    call(0x3fbcd0);
    bool network = false;
    if (call(0x4294c0)) {
        call(0x42a770);
        network = true;
    }
    const auto words = r32(g + 0xe8);
    w32(words, 0);
    w32(words + 4, 0);
    w32(words + 8, 0);
    w32(frame + 0x50, g + 0xc8);
    w32(frame + 0x54, r32(g + 0xcc));
    call(0x138c60, {frame + 0x58, g + 0xc4, frame + 0x54, frame + 0x50});
    call(0x3d0fb0, {call(0x3d1020)});
    call(0x224140, {call(0x224530)});
    call(0x130b10, {g});
    call(0x1c3b00, {r32(g + 0x88)});
    call(0x132a40, {g});
    call(0x145200, {r32(g + 0x1e0)});
    call(0x4702a0);
    if (service) call(0x1c9710, {service});
    call(0x13e070, {r32(g + 0xd0)});
    call(0x3e9c20);
    call(0x40c5d0);

    for (const auto [field, line] : {std::pair{0x1f4U, 0x34dU}, std::pair{0x1f8U, 0x34eU}}) {
        const bool locked = r8(lock_flag) != 0;
        object = r32(g + field);
        if (locked) call(0x10dc40, {9});
        w32(lock_count, r32(lock_count) + 1);
        if (object) call(0x40b830, {object, 0});
        call(0x36b130, {0, object, world_cc, line});
        unlock();
        count_down();
    }
    w32(g + 0x1f4, 0);
    w32(g + 0x1f8, 0);
    call(0x3a3670, {0x53b540});
    call(0x16f4d0);

    object = r32(g + 0xdc);
    lock();
    call(0x2458e0, {object, none});
    call(0x36b130, {0, object, world_cc, 0x35c});
    unlock();
    count_down();
    w32(g + 0xdc, 0);
    call(0x1309a0, {g});
    call(0x130950, {g});

    object = call(0x3cab50, {g, call(0x3caa50, {g, 0x4ea0b0})});
    lock();
    destroy(object);
    call(0x36b130, {0, object, world_cc, 0x366});
    unlock();
    count_down();
    call(0x135020);
    call(0x3c2a80);
    call(0x1af7a0);
    call(0x40f570);

    if (const auto held = r32(singleton)) {
        lock();
        destroy(held);
        call(0x36b130, {0, held, singleton_cc, 0xb9});
        unlock();
        w32(singleton, 0);
        count_down();
    }
    call(0x3c2a80);
    call(0x374020, {0x535d20});
    call(0x236f80);
    call(0x3d5750, {0x543790});
    call(0x404fd0, {0x55dda0, none});
    call(0x407be0);
    call(0x3eb750, {call(0x3eba50)});
    call(0x227c60, {call(0x3caa50, {g, 0x4f7578})});
    if (network) call(0x42a680);
    w8(mp, 0);
    w8(mp_b8, 0);
    call(0x3c8c70);
    call(0x3e9c20);
}

}  // namespace omega
