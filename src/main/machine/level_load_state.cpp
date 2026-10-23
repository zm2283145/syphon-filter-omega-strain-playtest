#include "level_load_state.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace omega {
namespace {
constexpr std::uint32_t game = 0x4ffb50;
constexpr std::uint32_t mp_flag = 0x5721c8;

std::uint32_t video_scale_word(const LevelLoadMemory& memory) {
    const auto video = memory.read(0x4ffd2c);
    if (!video) throw std::runtime_error("Level load state: video object is null");
    const float scale = std::bit_cast<float>(memory.read(video + 0x1c));
    const float value = std::bit_cast<float>(0x433f4000U) * scale;  // 191.25
    if (!std::isfinite(value) || value < -2147483648.0f || value >= 2147483648.0f)
        throw std::runtime_error("Level load state: video scale outside cvt.w.s range");
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(value)) & 0xffU;
}
}  // namespace

void run_level_load_wrapper(LevelLoadMemory& memory, LevelLoadHost& host,
                            std::uint32_t root, std::uint32_t stack) {
    (void)root;  // 001693f0 does not read its incoming a0.
    const auto flag = [&] { return memory.read(mp_flag, 1); };
    const auto call = [&](std::uint32_t address) { host.call(memory, address, {}); };

    if (flag()) call(0x42a770);
    call(0x16a970);
    host.before_level_load(memory);
    run_level_load(memory, host, game, stack - 0x10);
    host.after_level_load(memory);

    if (flag()) {
        call(0x42a1b0);
        call(0x42b0c0);
        // 00169448 re-reads the flag. Zero uses the delay-slot v0=1;
        // nonzero reads the mode byte 005721c0.
        const auto post_setup_flag = flag();
        const auto route = post_setup_flag ? memory.read(0x5721c0, 1) : 1U;
        if (route) {
            call(0x16a970);
        } else {
            call(0x185840);
            call(0x3fa5d0);
        }
    }
    if (flag()) call(0x429d30);
}

void run_level_load_state(LevelLoadMemory& memory, LevelLoadHost& host,
                          std::uint32_t app) {
    const auto r8 = [&](std::uint32_t address) { return memory.read(address, 1); };
    const auto r32 = [&](std::uint32_t address) { return memory.read(address); };
    const auto w8 = [&](std::uint32_t address, std::uint32_t value) { memory.write(address, 1, value); };
    const auto w32 = [&](std::uint32_t address, std::uint32_t value) { memory.write(address, 4, value); };
    if (r8(game + 0x50) != 0x0e) return;

    host.call(memory, 0x1694b0, {app});
    host.call(memory, 0x3f9470, {game + 0x3c});
    host.call(memory, 0x37e320, {0x5381f0});
    host.call(memory, 0x37e780,
              {0x5381f0, r32(app + 0x254), r32(app + 0x258), 0x20, 0x10,
               r8(app + 0x250), 0x157c, 0xffffffffU});
    w32(0x5383d4, r32(app + 0x260));
    const std::uint64_t heap_top = host.call_return64(memory, 0x36c810, {0x533cb0});
    const std::uint64_t reduced = heap_top - std::uint64_t(0x1000);
    const std::uint64_t signed_low = static_cast<std::uint64_t>(
        static_cast<std::int64_t>(static_cast<std::int32_t>(static_cast<std::uint32_t>(reduced))));
    host.call_args64(memory, 0x36b3b0, {3, signed_low, 0x10});
    w32(0x533ca0, 3);
    w32(0x5383c0, video_scale_word(memory));
    host.call(memory, 0x165950, {app});
    const auto root = host.call(memory, 0x1698c0, {});
    run_level_load_wrapper(memory, host, root);
    w32(0x4ee470, 1);
    w8(game + 0x51, r8(game + 0x50));  // re-read after 001350e0
    w8(game + 0x50, 0x12);
}

}  // namespace omega
