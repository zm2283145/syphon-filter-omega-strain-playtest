#pragma once

#include "actor_fader.hpp"
#include "game_end_state.hpp"
#include "level_load.hpp"

#include <cstdint>

#include <vector>

namespace omega {

// One explicit service of the main update's in-level blocks (00168dc8 gameplay
// 0x0f/0x13, 001690f0 teardown 0x14) that omega_play cannot host natively.
// Sizes are static-disassembly instruction counts of the entry function (up to
// its last reachable return); the closure is the reachable callee set.
struct PlayFlowBlocker {
    std::uint32_t address;
    const char* block;        // "0x0f/0x13", "0x13", "0x14" or "MP"
    const char* role;
    unsigned instructions;
    const char* closure;
    const char* needed_by;    // the omega_play features that depend on it
};
const std::vector<PlayFlowBlocker>& play_native_flow_blockers();

// The --native-flow host. 003d1020 (static_instance.hpp), 0012ed60
// (remote_players_ready.hpp), 0037a9f0, 0036c960 and the level-load renderer
// init 0037e780 (video_frame.hpp) run
// natively, and since checkpoint 132zf4 so do the state-0x14 teardown's World_Teardown
// 00134640, 003f9470, 0037e320 and 0036b250 (world_teardown.hpp,
// level_teardown_callees.hpp), each failure rethrown with the service named; 00166190 keeps the
// default native call_framed; every blocker throws with its address, role and
// size; anything else falls to LevelLoadHost::call (which throws for unknown
// addresses).
class PlayFlowHost : public LevelLoadHost {
public:
    std::uint32_t call(LevelLoadMemory& memory, std::uint32_t address,
                       std::initializer_list<std::uint32_t> args) override;
};

// The sealed EE mirror of the play state the blocks read: G+50, G+51 and
// G+1c4..+1c7 from GameEndState, the MP byte 005721c8 = 0 (omega_play is
// solo) and the fader pointer G+e8 (004ffc38). The fader's EE heap address is
// not modelled: the pointer names a host-only word outside the 32 MiB EE RAM
// (fader_mirror) holding ActorFader::duration_04's bits, so it cannot alias
// modelled memory. Every other byte is unmodelled and throws on access.
constexpr std::uint32_t fader_mirror = 0x02000000;
LevelLoadMemory play_flow_mirror(const GameEndState& game, const ActorFader& fader);

// One pass of the main update's in-level blocks on the play state: the
// gameplay block (run_gameplay_state) then the teardown block
// (run_teardown_state), with the PlayFlowHost on a sealed mirror; G+50/+51
// are written back to `game`. In states 0x0f/0x13 the frame stops: the native
// 0037a9f0 reads the video object 005381f0, which the mirror does not model
// (its in-level state comes from 0037e780 at level load, which play never runs
// on the mirror: see FRONTEND.md "Checkpoint 116 renderer init"),
// and the frame's later services throw (play_native_flow_blockers); a native
// frame that completed would also throw, since omega_play's own frame body
// would run the frame again. The app
// object ($s0 of 00167e60; 003e7cd0's app+0x2c..0x30, 00166190's
// app+0x270/+0x274) is not modelled, so state 0x14 throws. A return means no
// block ran. Every failure is rethrown as a std::runtime_error naming the
// frame, the state and the block.
void run_play_native_flow(GameEndState& game, const ActorFader& fader, unsigned frame);

}  // namespace omega
