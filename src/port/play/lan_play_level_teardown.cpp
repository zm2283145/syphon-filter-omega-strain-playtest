#include "lan_play_level_teardown.hpp"

#include "dme_game.hpp"
#include "lan_net_core.hpp"
#include "level_load.hpp"

#include <stdexcept>

namespace omega {

PlayTeardownReport run_lan_play_level_teardown(lan::LanNetCore& core, const GameEndState& game,
                                               const PlayTeardownActions& actions) {
    auto& live = core.memory();
    if (!core.level_loaded() || live.read(dme::kNetworkGame, 1) != 1)
        throw std::runtime_error("LAN level teardown requires a loaded multiplayer game");
    // G+1c7 skips 0042b1f0 and intentionally retains the DME connection.
    // This adapter has no owner to carry that session into the next GUI pass.
    if (game.b1c7)
        throw std::runtime_error("LAN level teardown: G+1c7 keep-game continuation is not hosted");
    if (core.call(live, 0x4294c0, {}) && !core.has_lobby_exit())
        throw std::runtime_error("LAN level teardown requires a lobby-exit service");

    PlayTeardownNetwork network;
    network.multiplayer_5721c8 = static_cast<std::uint8_t>(live.read(dme::kNetworkGame, 1));
    network.quit_latch_5721d8 = static_cast<std::uint8_t>(live.read(dme::kQuitRequest, 1));
    network.popup_kind_5723b0 = static_cast<std::uint8_t>(live.read(0x5723b0, 1));
    network.network_5721b8 = static_cast<std::uint8_t>(live.read(0x5721b8, 1));
    network.call = [&core](LevelLoadMemory&, std::uint32_t address,
                           std::initializer_list<std::uint32_t> args) -> std::uint32_t {
        // PlayTeardownNetwork's memory is the G/world mirror. The DME natives
        // must read and write the core's live network memory instead.
        const auto result = core.call(core.memory(), address, args);
        if (address == 0x42b2d0 && result != 0)
            throw std::runtime_error("LAN level teardown: cDME_Restart did not free all network objects");
        return result;
    };

    auto report = run_play_level_teardown(game, actions, &network);
    // 00134640 clears these globals after its last network call. They live in
    // a separate EE mirror during run_play_level_teardown, so copy them back.
    live.write(dme::kNetworkGame, 1, report.multiplayer_after);
    live.write(dme::kQuitRequest, 1, report.quit_latch_after);
    live.write(0x5721b8, 1, report.network_after);
    core.finish_level_teardown();
    return report;
}

}  // namespace omega
