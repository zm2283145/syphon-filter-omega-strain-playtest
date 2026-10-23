#include "teardown_state.hpp"

#include <cstdint>

namespace omega {
namespace {
constexpr std::uint32_t game = 0x4ffb50;
constexpr std::uint32_t state = game + 0x50;          // 004ffba0
constexpr std::uint32_t previous = game + 0x51;       // 004ffba1
constexpr std::uint32_t won = game + 0x1c6;           // 004ffd16
constexpr std::uint32_t video = 0x5381f0;
}  // namespace

void run_teardown_state(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t app, std::uint32_t sp) {
    if (m.read(state, 1) != 0x14) return;
    // 003e7cd0(app+0x2c): sb zero,1(a0); sb 1,2(a0); sw -7,4(a0).
    m.write(app + 0x2d, 1, 0);
    m.write(app + 0x2e, 1, 1);
    m.write(app + 0x30, 4, 0xfffffff9U);
    host.call_framed(m, 0x166190, {app}, sp);
    host.call(m, 0x134640, {game});
    host.call(m, 0x36c960, {});
    host.call(m, 0x3f9470, {game + 0x3c});
    host.call(m, 0x37e320, {video});
    host.call(m, 0x36b250, {1});
    m.write(0x533ca0, 4, 0);
    host.call(m, 0x36b250, {3});
    const std::uint32_t next = m.read(won, 1) ? 1 : 0x0c;
    m.write(previous, 1, m.read(state, 1));
    m.write(state, 1, next);
}

}  // namespace omega
