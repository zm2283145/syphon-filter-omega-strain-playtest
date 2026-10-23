#include "gameplay_state.hpp"

#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
constexpr std::uint32_t game = 0x4ffb50;
constexpr std::uint32_t state = game + 0x50;          // 004ffba0
constexpr std::uint32_t previous = game + 0x51;       // 004ffba1
constexpr std::uint32_t gui = game + 0xb4;            // 004ffc04
constexpr std::uint32_t fader = game + 0xe8;          // 004ffc38
constexpr std::uint32_t gui_manager = 0x571fb0;       // 004147a0
constexpr std::uint32_t mp_flag = 0x5721c8;
constexpr std::uint32_t quit_request = 0x5721d8;      // 00169360
constexpr std::uint32_t name_flag = 0x5721f0;         // 00169330, 001692f0
constexpr std::uint32_t name_string = 0x5721e0;
constexpr std::uint32_t text_kind = 0x5723b0;
constexpr std::uint32_t disconnected = 0x5723b8;
constexpr std::uint32_t video = 0x5381f0;
constexpr std::uint32_t video_index = 0x538c60;
constexpr std::uint32_t matrix_index = 0x538c64;
constexpr std::uint32_t camera = 0x539170;
constexpr std::uint32_t t1_count = 0x10000800;
constexpr std::uint32_t tick_base = 0x533870;
constexpr std::uint32_t frame_counter = 0x55a818;     // 00165610
constexpr std::uint32_t one = 0x3f800000;             // 1.0f
}  // namespace

void run_gameplay_state(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t, std::uint32_t sp) {
    const auto r8 = [&](std::uint32_t a) { return m.read(a, 1); };
    const auto r32 = [&](std::uint32_t a) { return m.read(a); };
    const auto w8 = [&](std::uint32_t a, std::uint32_t v) { m.write(a, 1, v); };
    const auto w32 = [&](std::uint32_t a, std::uint32_t v) { m.write(a, 4, v); };
    const auto call = [&](std::uint32_t a, std::initializer_list<std::uint32_t> args = {}) {
        return host.call(m, a, args);
    };
    // 00169330: the 005721e0 string's pool id when 005721f0 is set, else 0.
    const auto quit_title = [&]() -> std::uint32_t { return r8(name_flag) ? call(0x3fdb50, {name_string}) : 0; };

    // 00168dc8: lb G+50; andi 0xff.
    const auto current = r8(state);
    if (current != 0x0f && current != 0x13) return;

    if (r8(mp_flag)) {
        call(0x3fbec0);
        if (r8(state) != 0x13) {
            // 00169360: read and clear the quit request.
            bool quit = false;
            if (r8(quit_request)) { quit = true; w8(quit_request, 0); }
            if (quit) {
                auto title = quit_title();
                title = title ? quit_title() : call(0x3fdb50, {0x49c4b8});   // "QuitTitle"
                const std::uint32_t message_id = r8(text_kind) == 1 ? 0x668 : 0x667;
                const auto owner = r32(gui);
                // 001692f0: 005721f0 itself when its first byte is set, else the 005721e0 pool id.
                const auto name = r8(name_flag) ? name_flag : call(0x3fdb50, {name_string});
                call(0x2cae30, {owner, 2, title, name, r32(gui_manager), message_id});
            }
        }
        bool lost = false;
        if (r8(disconnected)) { lost = true; w8(disconnected, 0); }
        if (lost) call(0x2ca640, {r32(gui), 0});
    }

    // 00168f10: the level-end fade check.
    if (r8(state) == 0x13) {
        const auto object = r32(fader);
        if (!object) throw std::runtime_error("Gameplay state: a null fader (004ffc38) is not hosted");
        const auto duration = r32(object + 4);
        if ((duration & 0x7f800000U) == 0 && (duration & 0x007fffffU) != 0)
            throw std::runtime_error("Gameplay state: a denormal fader duration (EE c.eq.s operand) is not hosted");
        if ((duration & 0x7fffffffU) == 0 && call(0x12ed60, {game})) {
            w8(previous, r8(state));
            w8(state, 0x14);
        }
    }

    // 00168f68: the frame.
    call(0x37a9f0, {video});
    auto record = video + r32(video_index) * 0x110U;
    for (std::uint32_t i = 0; i < 0x10; i += 4) w32(record + 0x1f0 + i, r32(camera + i));
    w8(record + 0x2f5, 1);
    w8(record + 0x2f6, 1);
    call(0x3c8de0);
    call(0x40c5c0);
    const auto message = sp + 0x80;
    call(0x3c9e30, {message, 0x5436b0});
    w32(message, 0x4d9160);
    call(0x3c9420, {message, game, 1});
    w32(message, 0x4d9160);
    call(0x3c9d60, {message, 0});
    const auto ticks = r32(t1_count);
    w32(tick_base, r32(tick_base) + ticks);
    w32(t1_count, 0);
    call(0x132e50, {game});
    const auto low = r32(0x4bc1b0);
    const auto high = r32(0x4bc1b4);
    record = video + r32(video_index) * 0x110U;
    w32(0x5392d8, low);
    w32(0x5392dc, high);
    w32(record + 0x2e0, 0);
    w32(record + 0x2e8, one);
    w8(record + 0x2f6, 1);
    w8(record + 0x2f4, 1);
    // sqc2 vf0 / qmfc2 / pcpyud / pexcw: an identity matrix (always 16-byte aligned).
    const auto matrix = video + (r32(matrix_index) << 6) + 0xa80;
    const auto row = [&](std::uint32_t offset, unsigned diagonal) {
        for (unsigned k = 0; k < 4; ++k) w32(matrix + offset + 4 * k, k == diagonal ? one : 0);
    };
    row(0x30, 3);
    row(0x10, 1);
    row(0x20, 2);
    row(0x00, 0);
    w8(0x538c68, 1);
    call(0x3d0c10, {call(0x3d1020)});
    // 00165610(app, 1): lw/addiu/sw of the frame counter; both arguments are unused.
    w32(frame_counter, r32(frame_counter) + 1);
}

}  // namespace omega
