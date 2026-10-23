#pragma once
// The game's network registrations (LAN phase 3 stage 2, checkpoint 132ln5b; research/NET_INLEVEL.md section 4).
//
// 00165bc0 (the first network start) runs the DME init 0042af00 once, then 47 registration functions and
// 003fb250 (25 more, then 0x0055c7b8 = 0042b1a0(0)) and sets 0x005723d0 ("the message types are registered",
// which 00429d30 tests). Each registration function is straight-line code: a message or class description on its
// stack, then DME API calls (0x002e9a10 .. 0x002e9f08), an abort on the first nonzero result and the copy of the
// returned ids into the game's globals. The port replays each one from the table tools/ee/gen_net_registry.py
// extracts from the executable by running it in the EE interpreter (net_registrations.inl): every API call with
// its arguments (constants, addresses in the function's frame, ids a previous call returned), the bytes it passes
// through a frame pointer (the 16 field-option bytes, a class name), the global words written between the calls,
// and for each call's failure the return value and the writes after it. tests/net_registry_tests.cpp checks the
// replay against the interpreter on every success and failure path, and the whole 00165bc0 against the original.
//
// The DME API calls go to the host (LevelLoadHost::call by original address), where the native DME library
// (dme_library.hpp) answers them.

#include "level_load.hpp"

#include <cstdint>

namespace omega {

// One of the 72 registration functions (kTopRegistrations / kRegisterAllRegistrations).
bool net_registration_has(std::uint32_t function);

// Runs registration function `function` as the original: `stack` is $sp at its jal (its frame offsets go below).
std::uint32_t net_registration_call(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t function,
                                    std::uint32_t stack);

// The executable's .rodata bytes the DME reads through constant pointers (the class names), and the .data the
// library reads at init (0x00497e50 = 0x28ab, the version text "1.32.0089" at 0x0048c4cd). A memory that starts
// empty (the port's LevelLoadMemory) gets them before 00165bc0.
void net_registry_image(LevelLoadMemory& m);

// 00165bc0: 0x004ee530 clear -> set it, 003a6710(1) (the IRX loader), and when 0x0051eea8 is clear
// 0x0051eea0 = 1 and 0042af00; then, when 0x005723d0 is clear, the 47 registrations, 003fb250 and
// 0x005723d0 = 1. Every callee goes through the host.
void net_start_00165bc0(LevelLoadMemory& m, LevelLoadHost& host);

// 003fb250: the 25 registrations, then 0x0055c7b8 = 0042b1a0(0) (the session clock).
void net_register_all_003fb250(LevelLoadMemory& m, LevelLoadHost& host);

}  // namespace omega
