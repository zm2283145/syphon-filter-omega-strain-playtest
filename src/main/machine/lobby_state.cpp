#include "lobby_state.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>

namespace omega {
namespace {
constexpr std::uint32_t game = 0x4ffb50;
constexpr std::uint32_t state = game + 0x50;          // 004ffba0
constexpr std::uint32_t previous = game + 0x51;       // 004ffba1
constexpr std::uint32_t video_object = game + 0x1dc;  // 004ffd2c
constexpr std::uint32_t lobby_first = 0x48a008;       // state-5 first-entry byte (initially 1)
constexpr std::uint32_t server_first = 0x48a020;      // state-6 first-entry byte (initially 1)
constexpr std::uint32_t players_needed = 0x48a028;    // initially 2
constexpr std::uint32_t is_server = 0x4ee488;
constexpr std::uint32_t connected = 0x4ee490;
constexpr std::uint32_t players_connected = 0x5723f0;
constexpr std::uint32_t video = 0x5381f0;
constexpr std::uint32_t boot_as_server = 0x49c4e8;    // "BOOT_AS_SERVER"
}  // namespace

void run_lobby_states(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t app, std::uint32_t sp) {
    const auto r8 = [&](std::uint32_t a) { return m.read(a, 1); };
    const auto r32 = [&](std::uint32_t a) { return m.read(a); };
    const auto w8 = [&](std::uint32_t a, std::uint32_t v) { m.write(a, 1, v); };
    const auto w32 = [&](std::uint32_t a, std::uint32_t v) { m.write(a, 4, v); };
    const auto call = [&](std::uint32_t a, std::initializer_list<std::uint32_t> args = {}) {
        return host.call(m, a, args);
    };

    // 00168bcc: state 5.
    if (r8(state) == 5) {
        if (r8(lobby_first)) {
            call(0x37e780, {video, r32(app + 0x254), r32(app + 0x258), 0x20, 0x10, r8(app + 0x250),
                            0xffffffffU, 0xffffffffU});
            const std::uint32_t value = r32(app + 0x260);
            const std::uint32_t object = r32(video_object);
            w32(0x5383d4, value);
            if (!object) throw std::runtime_error("Lobby state: a null video object (004ffd2c) is not hosted");
            // 0x5383c0 = (u8)cvt.w.s(191.25 (0x4c1fc8) x video+1c), stored in 00165950's delay slot.
            const float product = std::bit_cast<float>(0x433f4000U) * std::bit_cast<float>(r32(object + 0x1c));
            if (!std::isfinite(product) || !(product > -2147483648.0F && product < 2147483648.0F))
                throw std::runtime_error("Lobby state: a video scale outside the cvt.w.s range is not hosted");
            w32(0x5383c0, static_cast<std::uint32_t>(static_cast<std::int32_t>(product)) & 0xffU);
            call(0x165950, {app});
            w8(lobby_first, 0);
        }
        call(0x167630, {app});
    }

    // 00168c68: state 6.
    if (r8(state) != 6) return;
    if (r8(server_first)) {
        call(0x165bc0);
        const std::uint32_t text = call(0x3cba90, {boot_as_server});
        std::uint32_t server = 0;
        if (text && call(0x121bc8, {text}) == 1) server = 1;
        w8(is_server, server);
        if (r8(is_server)) call(0x42a870, {0});
        call(0x1674e0, {app});
        w8(server_first, 0);
    }
    if (!r8(is_server) && !r8(connected)) {
        call(0x42aec0);
        w32(sp + 0x180, 0xe4a);
        if (call(0x42a520, {sp + 0x170})) {
            call(0x42a780, {sp + 0x170});
            w8(connected, 1);
        }
    }
    const std::uint32_t count = r32(players_connected);
    call(0x166fe0, {app, count, r32(players_needed)});
    if (!r8(is_server) && !r8(connected)) return;
    const std::uint32_t now = r32(players_connected);
    if (r32(players_needed) - 1 != now) return;
    call(0x166f20, {app});
    call(0x37e320, {video});
    w8(previous, r8(state));
    w8(state, 9);
}

}  // namespace omega
