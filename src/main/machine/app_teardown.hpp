#pragma once

#include "level_load.hpp"

#include <cstdint>

namespace omega {

// Original 00166190..00166428 (00166430 starts the next function), called as
// 00166190(app) by the main update: the state-0x12 first entry and the state-0x14
// teardown. sp is the caller's $sp at the jal; the function's frame is
// sp-0x100 and its file object sits at sp-0xb0. With app+0x274 null it returns
// at once. Otherwise it removes the 0048a038 interrupt handler (syscall wrappers
// 0010dc40 _DisableIntc(2), 0010ce60 RemoveIntcHandler) and the 0048a030
// thread (0010cfb0 TerminateThread, 0010cf70 DeleteThread), clears 004ee500 and
// 004ee508, sets 0048a058 to -1, frees 004ee4e8 ("machine.cc" line 0x7f3),
// sets the 0048a048 thread's priority to 5 (0010cff0), releases and frees the
// app+0x274 object (00271cd0, 002720a0, line 0x7fd) and, when the app+0x270
// archive's byte +0x14c is set, passes each of its entries through 003816f0
// before 003f9530 removes it from G+0x3c. Every callee is a LevelLoadHost call.
void run_app_teardown(LevelLoadMemory& memory, LevelLoadHost& host,
                      std::uint32_t app, std::uint32_t sp);

}  // namespace omega
