#pragma once
// Checkpoint 132zf4: the state-0x14 level teardown 001690f0 run by omega_play at the level end.
//
// run_play_level_teardown runs the native block run_teardown_state (003e7cd0
// inlined, 00166190, World_Teardown 00134640, 0036c960, 003f9470, 0037e320,
// 0036b250(1), 00533ca0 = 0, 0036b250(3), the next state) on an EE mirror of
// the level's end state, with every service the natives reach answered at its
// original call site in the original order:
//   native   - a ported function (world_teardown.hpp, level_teardown_callees.hpp,
//              video_frame.hpp 0036c960, static_instance.hpp 003d1020);
//   port     - the port's counterpart runs here (PlayTeardownActions): the pad
//              vibration stop 0026bd60 / 0026be70(0), the tremor list free
//              00272920 and the vision effect resets 002609e0 / 00260990 / 0025fbb0;
//   pass     - the level object the service destroys is, in the port, a local of
//              omega_play's level pass, released when the pass ends (the next
//              level's pass builds its own);
//   renderer - the level's renderer state, which in the port is the play
//              window's device, released when the window closes at the resume;
//   ps2      - PS2 kernel, heap or DMA service with no PC counterpart;
//   mirror   - a lookup that answers the mirror (G component tokens; 004294c0
//              is null offline).
// A service with no disposition throws naming its address.
//
// The mirror: G+50 = 0x14 and G+51 / G+1c4..+1c7 from GameEndState; the MP
// bytes 005721c8 / 005721d8 / 005723b0 / 005721b8 = 0 (omega_play is solo);
// the allocator lock flag 00533880 = 0 (no other EE thread at the level end);
// app+274 = 0 because 00166190 already ran at state 0x12's first entry (the
// port's LoadingScreenHost::stop after the setup), so 00166190 returns at once
// here as on the PS2; G's object fields are tokens outside the 32 MiB EE RAM
// (0x0a000000 + the field offset) and G component lookups answer tokens; the
// archive manager list G+3c is empty (the port's disc reads hold no archive
// object); the deferred-free queue 00533c90 is empty; the heap slots 1 and 3
// are live (+0 a block token, +10 = 0). The video object 005381f0 is the state
// the level load's 0037e780(005381f0, 640, 448, 0x20, 0x10, 0, 0x157c, -1)
// (level_load_state.cpp) leaves: renderer_init_0037e780 runs on the mirror
// first with its allocations answered by tokens, so the report can check that
// 0037e320 frees every renderer allocation 0037e780 made.

#include "game_end_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace omega {

struct PlayTeardownActions {
    std::function<void()> stop_pad_vibration;                     // 0026bd60
    std::function<void(std::uint32_t)> enable_pad_vibration;      // 0026be70(0)
    std::function<void()> free_tremors;                           // 00272920(G+8c, -1)
    std::function<void(std::uint32_t, std::uint32_t)> vision_mode; // 002609e0(G+e0, 0, 1)
    std::function<void(const std::array<float, 4>&)> vision_flair; // 00260990(G+e0, 0; f12..f15 = 0)
    std::function<void()> vision_prepare;                          // 0025fbb0(G+e0)
    std::function<void(std::uint32_t, std::uint32_t)> debrief;     // 00134110(G, a1) (G+1c5 clear)
};

struct PlayTeardownCall {
    std::uint32_t address{};
    std::vector<std::uint32_t> args;
    std::string kind;          // native, port, pass, renderer, ps2, mirror
    std::string role;
};

struct PlayTeardownReport {
    std::uint8_t state_50{};
    std::uint8_t previous_51{};
    std::vector<PlayTeardownCall> calls;   // in the original order
    std::size_t renderer_allocations{};    // 0036b180 by the mirror's 0037e780
    std::size_t renderer_frees{};          // 0036b130 by 0037e320 (file 004bc200)
    bool renderer_balanced{};              // every allocation freed exactly once
    std::size_t world_frees{};             // 0036b130 by World_Teardown (machine 0049a890 / 0049aea0)
    std::size_t count(const std::string& kind) const;
};

PlayTeardownReport run_play_level_teardown(const GameEndState& game, const PlayTeardownActions& actions);

}  // namespace omega
