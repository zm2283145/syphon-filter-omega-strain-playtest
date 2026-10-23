#include "play_native_flow.hpp"

#include "gameplay_state.hpp"
#include "level_teardown_callees.hpp"
#include "remote_players_ready.hpp"
#include "static_instance.hpp"
#include "teardown_state.hpp"
#include "video_frame.hpp"
#include "world_teardown.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
constexpr std::uint32_t game = 0x4ffb50;
constexpr std::uint32_t state = game + 0x50;
constexpr std::uint32_t previous = game + 0x51;
constexpr std::uint32_t end_bytes = game + 0x1c4;   // +1c4..+1c7 (0012fa60)
constexpr std::uint32_t fader = game + 0xe8;        // 004ffc38
constexpr std::uint32_t mp_flag = 0x5721c8;
constexpr std::uint32_t stack = 0x1f00000;          // the generators' update frame

std::string hex8(std::uint32_t value) {
    char text[9];
    std::snprintf(text, sizeof text, "%08x", value);
    return text;
}

std::string hex2(std::uint32_t value) {
    char text[3];
    std::snprintf(text, sizeof text, "%02x", value & 0xffU);
    return text;
}
}  // namespace

const std::vector<PlayFlowBlocker>& play_native_flow_blockers() {
    static const std::vector<PlayFlowBlocker> table{
        {0x3fb8d0, "0x13", "network player map lookup (map 0055c770)",
         57, "59 functions / ~4064 instructions",
         "0012ed60's remote-players-ready loop: the 0x13 -> 0x14 level-end fade completion"},
        {0x2ebaf8, "MP", "lazy init of the local player slot word 00494178", 41,
         "11 functions / ~217 instructions", "0012ed60 in multiplayer (005721c8 set)"},
        {0x3c8de0, "0x0f/0x13", "message pump",
         308, "~60 functions / ~4899 instructions",
         "script and object message delivery (the mission pump, pathed-object notices)"},
        {0x40c5c0, "0x0f/0x13", "tail jump to 0040b4b0 (role not labelled)",
         2, "~98 functions / ~6643 instructions",
         "every frame"},
        {0x3c9e30, "0x0f/0x13", "message constructor (key 005436b0)",
         27, "0042b1a0 18",
         "the per-frame world-update message"},
        {0x3c9420, "0x0f/0x13", "synchronous send of message 005436b0 to G (receiver 00136a90 -> object loop 00133420)",
         346, "~661 functions / ~38398 instructions",
         "the world update: actors, NPCs, combat, pathed objects"},
        {0x3c9d60, "0x0f/0x13", "message destructor",
         19, "00100740",
         "the per-frame world-update message"},
        {0x132e50, "0x0f/0x13", "render pass (00133340 fader update 0025e660, zone tracker 0013d8a0)",
         205, "~1303 functions / ~91247 instructions",
         "rendering, the fader progress the 0x13 check waits on, zone activation"},
        {0x3d0c10, "0x0f/0x13", "consumer of the 003d1020 static object 00543760",
         88, "~142 functions / ~10747 instructions",
         "every frame"},
        {0x3fbec0, "MP", "network pump", 32, "~884 functions", "multiplayer only"},
        {0x3fdb50, "MP", "string pool id", 9, "", "multiplayer quit dialog"},
        {0x2cae30, "MP", "quit dialog", 170, "", "multiplayer quit dialog"},
        {0x2ca640, "MP", "disconnect dialog", 43, "", "multiplayer disconnect"},
    };
    return table;
}

std::uint32_t PlayFlowHost::call(LevelLoadMemory& memory, std::uint32_t address,
                                 std::initializer_list<std::uint32_t> args) {
    if (address == 0x3d1020) {
        if (args.size() != 0) throw std::invalid_argument("003d1020 arguments");
        return static_instance_003d1020(memory);
    }
    // All three drop nothing the callers read except 0012ed60's v0
    // (00168f10 tests it; 00168f68 and 001690f0 drop the others' v0).
    if (address == 0x12ed60) {
        if (args.size() != 1) throw std::invalid_argument("0012ed60 arguments");
        try {
            return remote_players_ready_0012ed60(memory, *this, *args.begin());
        } catch (const std::exception& error) {
            throw std::runtime_error("native remote-players-ready 0012ed60: " + std::string(error.what()));
        }
    }
    if (address == 0x37a9f0) {
        if (args.size() != 1) throw std::invalid_argument("0037a9f0 arguments");
        try {
            video_frame_begin_0037a9f0(memory, *this, *args.begin());
        } catch (const std::exception& error) {
            throw std::runtime_error("native video frame 0037a9f0 on " + hex8(*args.begin()) + ": " + error.what());
        }
        return 0;
    }
    if (address == 0x36c960) {
        if (args.size() != 0) throw std::invalid_argument("0036c960 arguments");
        try {
            deferred_free_pump_0036c960(memory, *this);
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string("native deferred-free pump 0036c960: ") + error.what());
        }
        return 0;
    }
    // Level-load state 0xe's renderer init; its v0 is dropped.
    if (address == 0x37e780) {
        if (args.size() != 8) throw std::invalid_argument("0037e780 arguments");
        const auto* a = args.begin();
        try {
            renderer_init_0037e780(memory, *this, a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
        } catch (const std::exception& error) {
            throw std::runtime_error("native renderer init 0037e780 on " + hex8(a[0]) + ": " + error.what());
        }
        return 0;
    }
    // The state-0x14 teardown's services (001690f0): World_Teardown and its
    // three callees after 0036c960 are native (world_teardown.hpp,
    // level_teardown_callees.hpp); each one's v0 is dropped by the caller.
    if (address == 0x134640 || address == 0x3f9470 || address == 0x37e320 || address == 0x36b250) {
        if (args.size() != 1) throw std::invalid_argument(hex8(address) + " arguments");
        const auto a = *args.begin();
        try {
            if (address == 0x134640) run_world_teardown(memory, *this, a, stack);
            else if (address == 0x3f9470) archive_release_003f9470(memory, *this, a, stack);
            else if (address == 0x37e320) renderer_teardown_0037e320(memory, *this, a);
            else heap_slot_release_0036b250(memory, *this, a);
        } catch (const std::exception& error) {
            throw std::runtime_error("native teardown service " + hex8(address) + " on " + hex8(a) + ": " +
                                     error.what());
        }
        return 0;
    }
    for (const auto& b : play_native_flow_blockers()) {
        if (b.address != address) continue;
        throw std::runtime_error("EE service " + hex8(address) + " (" + b.role + "; " +
                                 std::to_string(b.instructions) + " instructions" +
                                 (*b.closure ? std::string(", closure ") + b.closure : std::string()) +
                                 ") has no native implementation; needed by " + b.needed_by);
    }
    return LevelLoadHost::call(memory, address, args);
}

LevelLoadMemory play_flow_mirror(const GameEndState& g, const ActorFader& f) {
    LevelLoadMemory m;
    m.write(state, 1, g.state_50);
    m.write(previous, 1, g.previous_51);
    m.write(end_bytes + 0, 1, g.b1c4);
    m.write(end_bytes + 1, 1, g.b1c5);
    m.write(end_bytes + 2, 1, g.b1c6);
    m.write(end_bytes + 3, 1, g.b1c7);
    m.write(mp_flag, 1, 0);
    m.write(fader, 4, fader_mirror);
    std::uint32_t bits;
    std::memcpy(&bits, &f.duration_04, sizeof bits);
    m.write(fader_mirror + 4, 4, bits);
    m.seal();
    return m;
}

void run_play_native_flow(GameEndState& g, const ActorFader& f, unsigned frame) {
    const auto entry = g.state_50;
    const char* block = "gameplay block 00168dc8";
    try {
        auto m = play_flow_mirror(g, f);
        PlayFlowHost host;
        // The app object is the update's $s0; 0 keeps every app access unmodelled.
        constexpr std::uint32_t app = 0;
        run_gameplay_state(m, host, app, stack);
        if (entry == 0x0f || entry == 0x13)
            // Unreachable while the frame services throw: omega_play's own
            // frame body would otherwise run the same frame a second time.
            throw std::logic_error("the native frame completed, but omega_play's frame body is not yet "
                                   "replaced by it");
        block = "teardown block 001690f0";
        if (m.read(state, 1) == 0x14)
            throw std::runtime_error("the app object ($s0 of 00167e60: 003e7cd0's app+0x2c..+0x30, 00166190's "
                                     "app+0x270/+0x274) is not modelled");
        run_teardown_state(m, host, app, stack);
        g.state_50 = static_cast<std::uint8_t>(m.read(state, 1));
        g.previous_51 = static_cast<std::uint8_t>(m.read(previous, 1));
    } catch (const std::exception& error) {
        throw std::runtime_error("Native flow (--native-flow) blocked at frame " + std::to_string(frame) +
                                 " in state 0x" + hex2(entry) + ", " + block + ": " + error.what());
    }
}

}  // namespace omega
