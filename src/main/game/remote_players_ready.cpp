#include "remote_players_ready.hpp"

namespace omega {
namespace {
constexpr std::uint32_t mp_flag = 0x5721c8;
constexpr std::uint32_t local_slot = 0x494178;
}  // namespace

std::uint32_t remote_players_ready_0012ed60(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t game) {
    for (std::uint32_t slot = 0; slot < 4; ++slot) {
        std::uint32_t local = 0;
        if (m.read(mp_flag, 1)) {
            if (m.read(local_slot) == 0xffffffffU) host.call(m, 0x2ebaf8, {local_slot});
            local = m.read(local_slot);
        }
        if (slot == local) continue;
        if (!host.call(m, 0x3fb8d0, {slot})) continue;
        const auto agent = m.read(game + 0x1c8 + 4 * slot);
        if (!agent) continue;
        if (!m.read(agent + 0xb70, 1)) return 0;
    }
    return 1;
}

}  // namespace omega
