#pragma once

#include "level_load.hpp"

namespace omega {

// Original 0016a750: reset the low relation bit of a 16x16 byte matrix.
void initialize_team_bits(LevelLoadMemory& memory, std::uint32_t object);

// Original 0016a970 and adjacent 0016aa80. The same EE-addressed memory is
// owned by the level-load caller. Network init, allocation and thread services
// remain explicit LevelLoadHost calls; an unprovided service throws.
void run_team_setup(LevelLoadMemory& memory, LevelLoadHost& host);
void run_team_teardown(LevelLoadMemory& memory, LevelLoadHost& host);

// Original 0016a0a0 (checkpoint 132ln5c, research/NET_INLEVEL.md section 6): the multiplayer team data. With
// 0x005721c8 set and 0x005721c0 == 0 (the joiner), or with 0x005721d0 == 0 (no network object), it allocates
// 0x100 bytes (36b180 "ai.cc" line 0x80b / 0x806) and frees an unowned previous block (36b130 "ai.hh" 0x1fe);
// otherwise, unless word 0x004ee618 is set, it creates the network object "cTeam" (002eaf28: a0 = &handle,
// 1, 4, 0, 0, 0, 0, t3 = *0x004ee620; stack words 0x0049c648 at $sp+0 and *0x004ee610 at $sp+8), looks it up
// (002eb650), frees an unowned previous block, stores object+0x1c in 0x004ee620 and sets the owner byte
// 0x004ee628. `stack` is the caller's $sp at the jal (the frame is stack - 0x60); 002eaf28 goes through
// LevelLoadHost::call_framed with that frame, so its stack words are memory. Every callee is a host boundary.
void team_data_object_0016a0a0(LevelLoadMemory& memory, LevelLoadHost& host, std::uint32_t stack);
// The $sp 0016a970 passes to 0016a0a0 under the level-load harness stack 0x01f00000 (0016a970's frame is 0x30).
constexpr std::uint32_t kTeamDataStack = 0x1f00000 - 0x30;

}  // namespace omega
