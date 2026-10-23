#pragma once
// The DME library's init and registry (LAN phase 3 stage 2, checkpoint 132ln5b; research/NET_INLEVEL.md section 4).
//
// These are original DME library functions, ported natively on LevelLoadMemory at their original addresses: what
// 0042af00 (the DME init) and the 72 network registrations of 00165bc0 / 003fb250 reach. Bit-exact against the EE
// goldens of tools/ee/gen_net_registry.py (tests/net_registry_goldens.hpp, `lib` and `start`).
//
//   API wrappers (the gate: byte 0x0048c4d7 == 1 (else 0x11) and byte 0x0048c4dc == it (else 0x2b), the library
//   lock 00302260(0x005253f0, 0) (nonzero: 0x2b), the body, the unlock 003023d8(0x005253f0, 0) (nonzero: 0x2b)):
//     002e9a10 -> 002ecff0 (returns 0)          002e9a98 -> 002ecff8 (returns 0)
//     002e9b60 -> 002ed000 (class fields begin)  002e9be8 -> 002ed028 (class field)
//     002e9cd0 -> 002ed280 (class)               002e9d88 -> 002ed570 (class callbacks)
//     002e9e50 -> 002ed528 (message register)    002e9f08 -> 002ed550 (message register, category 2)
//     002ec168 -> 002dcd30 (0x0048c49c / 0x0048c4a0)
//   Init: 002ea4f0 (-> 002dd358 -> 002fb568: the allocator pair 0x004908e0 / 0x004908e4), 002e9480 (the init
//   parameters), 002e9320 (the version text "1.32.0089" at 0x0048c4cd), 002e94e0 (the library start) -> 002df290
//   (the world init) -> 003057f8 (the message registry), 002e8a00 (the DME's own 25 messages), 002db140, 002e92f0.
//   Allocation: 002dd2e8 / 002fb350 / 002fb2d8 call the allocator callback *0x004908e0 (libc malloc 00125368 when
//   none). Registry: 00305998 (a callback into category `cat`), 002ed168 (nested class fields).
//   libc: memset 001260f0, strcpy 00129360, strncpy 00129a70.
//   Network objects (checkpoint 132ln5c, NET_INLEVEL.md section 4.4): the API wrappers 002eaf28 -> 002e2480 (the
//   object create: ten arguments, the ninth and tenth on the stack at $sp+0 / $sp+8; 002eaf28's frame is 0x60)
//   and 002eb650 -> 002dd968 (the object lookup); the worlds 002e51b8 (lookup) / 002e51f0 (create), the client
//   records 002db188 (lookup) / 002db270 (create) / 002db470 (mark the record's handle runs used), the handle
//   spaces 002e68c0 (first fit over 0x1000 status bytes) / 002e69b8 (first fit over the 0x800 second-space
//   slots), the slots 002dd9b0 / 002dd9f8 (lazily allocated 0x800 x 0x44 bytes), 002dd328 / 002fb3c0 (the
//   release callback *0x004908e4, libc free 001253c0 when none) and memcpy 00125f38. 002db470 and 002e69b8
//   read a caller stack slot the original never initialises when the second-space allocation fails; that
//   path throws naming the function.
//
// The callees outside the library stay host boundaries by original address: the allocator and release callbacks, the lock
// 003021d0 / 00302260 / 003023d8 / 00302218, the clock 003060c0 / 00302088, 002fb788 / 002fc088 / 00314380 (the
// key / identity / socket layer), printf 00127358, malloc 00125368.
//
// `stack` is the caller's $sp at the jal; each function's frame goes below it as in the original (only 003060c0's
// slot and 002dd2e8's allocation out word are memory).

#include "level_load.hpp"

#include <cstdint>
#include <initializer_list>

namespace omega {

bool dme_library_has(std::uint32_t address);
std::uint32_t dme_library_call(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t address,
                               std::initializer_list<std::uint32_t> args, std::uint32_t stack = 0x1f00000);

}  // namespace omega
