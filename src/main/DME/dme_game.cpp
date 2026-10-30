#include "dme_game.hpp"

#include "game_libm.hpp"

#include <bit>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace omega {
namespace {
using namespace dme;

std::uint32_t r8(const LevelLoadMemory& m, std::uint32_t a) { return m.read(a, 1); }
std::uint32_t r32(const LevelLoadMemory& m, std::uint32_t a) { return m.read(a, 4); }
void w8(LevelLoadMemory& m, std::uint32_t a, std::uint32_t v) { m.write(a, 1, v & 0xffU); }
void w32(LevelLoadMemory& m, std::uint32_t a, std::uint32_t v) { m.write(a, 4, v); }

// The re-entry byte pattern every DME callback opens with (set when clear) and closes with (clear).
void busy_enter(LevelLoadMemory& m) {
    if (!r8(m, kBusy)) w8(m, kBusy, 1);
}

// `byte 005721c8 ? byte 005721c0 : 1`: a host or a solo game.
std::uint32_t host_or_solo(const LevelLoadMemory& m) { return r8(m, kNetworkGame) ? r8(m, kHostRole) : 1U; }

// The local client index as 00429d30 / 003faab0 / 00429790 read it: 0 in a solo game; in a network game
// 0x00494178, filled by 002ebaf8(0x00494178, world) while it is -1.
std::uint32_t local_slot(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t network, std::uint32_t world) {
    if (!network) return 0;
    if (r32(m, kLocalSlot) == 0xffffffffU) host.call(m, 0x2ebaf8, {kLocalSlot, world});
    return r32(m, kLocalSlot);
}

std::int32_t s32(std::uint32_t v) { return static_cast<std::int32_t>(v); }
}  // namespace

std::uint32_t dme_timer_install_00429430(LevelLoadMemory& m, LevelLoadHost& host) {
    w32(m, kT0Mode, 0xc00);
    w32(m, kT0Comp, 0x24a);
    w32(m, kT0Count, 0);
    w32(m, kT0Mode, 0x382);
    w32(m, kTimerHandler, host.call(m, 0x10ce40, {9, kTimerInterrupt, 0}));
    if (r32(m, kTimerHandler) == 0xffffffffU) return 0;
    host.call(m, 0x10dca8, {9});
    return 1;
}

std::uint32_t dme_timer_interrupt_004292b0(LevelLoadMemory& m, LevelLoadHost& host) {
    const auto mode = r32(m, kT0Mode);
    w32(m, kT0Mode, mode);  // writing the flags back clears them
    if (mode & 0x400U) {
        w32(m, kTicks, r32(m, kTicks) + 1);
        w32(m, kT0Count, 0);
    }
    if (r8(m, kThreadStarted) && r32(m, kTicks) - r32(m, kLastWakeTick) >= 100U) {
        if (host.call(m, 0x10e010, {r32(m, kThreadId)}) != r32(m, kThreadId)) host.call(m, 0x10f388, {0x4bf990});
        w32(m, kT0Count, 0);
        w32(m, kThreadWake, 0);
        w32(m, kLastWakeTick, r32(m, kTicks));
    }
    return 0;  // sync; ei
}

std::uint32_t dme_thread_start_00429380(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t entry,
                                        std::uint32_t stack) {
    if (r8(m, kThreadStarted)) return 1;
    const auto param = stack - 0x40 + 0x10;  // ThreadParam on the 0x40-byte frame
    w32(m, param + 0x04, entry);
    w32(m, param + 0x00, 0x10);
    const auto main = r32(m, kThreadId);
    w32(m, param + 0x08, 0x572540);   // the thread's stack
    w32(m, param + 0x24, 0);
    w32(m, param + 0x0c, 0x10000);    // stack size
    w32(m, param + 0x2c, 0);
    w32(m, param + 0x10, 0x4e9b70);   // gp
    w32(m, kMainThread, main);
    w32(m, param + 0x14, 5);          // initial priority
    host.call(m, 0x10cff0, {main, 0xa});
    w32(m, kThreadId, host.call(m, 0x10cf60, {param}));
    host.call(m, 0x10cf80, {r32(m, kThreadId), 0});
    w8(m, kIntcInstalled, 1);
    w8(m, kThreadStarted, 1);
    return 1;
}

std::uint32_t dme_thread_start_0042a770(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack) {
    return dme_thread_start_00429380(m, host, kThreadEntry, stack);
}

bool DmeThreadBody::resume(LevelLoadMemory& m, LevelLoadHost& host) {
    if (at_ == At::Exited) throw std::runtime_error("DME thread 004295e0 resumed after its exit");
    if (at_ == At::AfterSleep) host.call(m, 0x1161a0, {});  // DI after SleepThread returns
    if (!r8(m, kThreadStop)) {
        host.call(m, 0x429d30, {});
        host.call(m, 0x1161f8, {});
        at_ = At::AfterSleep;
        host.call(m, 0x10d080, {});  // SleepThread: the body yields here
        return true;
    }
    host.call(m, 0x1161f8, {});
    w8(m, kThreadDone, 1);
    at_ = At::Exited;
    host.call(m, 0x10cfa0, {});      // ExitDeleteThread
    return false;
}

void dme_stop_thread_and_sync_0042a1b0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack) {
    const auto sp = stack - 0xe0;
    host.call(m, 0x10d060, {r32(m, kThreadId), sp + 0x60});
    host.call(m, 0x10d060, {r32(m, kMainThread), sp + 0x30});
    // Current priorities (+0x18): unless the main thread already outranks the DME thread, raise the DME thread.
    if (!(s32(r32(m, sp + 0x48)) < s32(r32(m, sp + 0x78)))) host.call(m, 0x10cff0, {r32(m, kThreadId), 0});
    w8(m, kThreadStop, 1);
    const auto woken = host.call(m, 0x10e010, {r32(m, kThreadId)});
    host.call(m, 0x10e1c0, {0xa});
    if (woken == r32(m, kThreadId) && !r8(m, kThreadDone)) {
        do host.call(m, 0x10e1c0, {0xa});
        while (!r8(m, kThreadDone));
    }
    w8(m, kThreadStop, 0);
    w8(m, kThreadStarted, 0);
    w32(m, kThreadWake, 0);
    w8(m, kThreadDone, 0);
    w8(m, kIntcInstalled, 0);
    {
        const auto state = r8(m, kState);
        if (state == 0xa || state == 7) return;
    }
    w8(m, kState, 8);
    const auto params = sp + 0x90;
    host.call(m, 0x2eabf0, {params});
    w32(m, params + 0x04, r32(m, kWorld));
    host.call(m, 0x129a70, {params + 0x08, 0x4bfa10, 0xc});  // "Foo"
    w32(m, params + 0x14, 500);
    w32(m, params + 0x18, 0);
    w32(m, params + 0x1c, host_or_solo(m) ? 1U : 0U);
    w32(m, params + 0x24, 0);
    w32(m, params + 0x2c, 0);
    w32(m, params + 0x20, kSyncCallback);
    w32(m, params + 0x28, kClientJoinedGame);
    auto failed = host.call(m, 0x2eac88, {params, sp + 0xc8});
    if (failed) return;
    const auto clock = [&](std::uint32_t slot) {
        return host.call(m, 0x2ebc48, {sp + slot}) ? 0xffffffffU : r32(m, sp + slot);
    };
    const auto deadline = clock(0xdc) + 15000U;
    if (host_or_solo(m)) {
        while (!failed && r8(m, kState) == 8) {
            failed = host.call(m, 0x429d30, {});
            if (s32(deadline) < s32(clock(0xd8))) { failed = 1; break; }
        }
    } else {
        while (!failed && r8(m, kState) - 8U < 2U) {
            failed = host.call(m, 0x429d30, {});
            if (s32(deadline) < s32(clock(0xd4))) { failed = 1; break; }
        }
    }
    if (!failed) {
        const auto state = r8(m, kState);
        if (state == 0xa || state != 0xb) return;
    }
    w8(m, 0x497540, 1);
    host.call(m, 0x437bf0, {});
    host.call(m, 0x2ec6a0, {0});
    host.call(m, 0x2ec738, {0});
    host.call(m, 0x42b230, {});
    w8(m, kQuitRequest, 1);
    host.call(m, 0x128bd0, {kQuitText, 0x4bfa18});  // "F_TO_JOINGAME"
}

void dme_voice_end_00437bf0(LevelLoadMemory& m, LevelLoadHost&) {
    if (!r8(m, 0x582b88)) return;
    throw std::runtime_error("00437bf0: the headset voice teardown (0x00582b88 set: 002eb650, 002ec460, 00325a28, 002ec878, "
                             "0042bf90, 0042c1d0) is not ported");
}

void dme_session_reset_0042b230(LevelLoadMemory& m) {
    w8(m, kQuitRequest, 0);
    w8(m, kPeerQuit, 0);
    w32(m, kWorld, 0xffffffffU);
    w32(m, 0x494180, 4);
    w32(m, kLocalSlot, 0xffffffffU);
    w32(m, 0x494198, 0xffffffffU);
    w8(m, kHostRole, 0);
    w8(m, 0x572410, 0);
    w32(m, 0x5723f0, 0);
    for (std::uint32_t k = 0; k < 4; ++k) w8(m, 0x572418 + k, 0);
    w8(m, 0x572468, 0);
    w32(m, 0x572428, 0);
    w32(m, 0x57242c, 0);
    w32(m, 0x572430, 0);
}

void dme_session_end_0042b1f0(LevelLoadMemory& m, LevelLoadHost& host) {
    w8(m, 0x497540, 1);
    host.call(m, 0x437bf0, {});
    host.call(m, 0x2ec6a0, {0});
    host.call(m, 0x2ec738, {0});
    host.call(m, 0x42b230, {});
}

void dme_sync_callback_00429c00(LevelLoadMemory& m, std::uint32_t arg) {
    busy_enter(m);
    if (r32(m, arg) == 0) {
        if (host_or_solo(m)) w8(m, kState, 0xa);
        else w8(m, kState, r8(m, kState) == 9 ? 0xa : 9);
    } else {
        w8(m, kState, 0xb);
    }
    w8(m, kBusy, 0);
}

void dme_remote_client_joined_003faab0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t arg) {
    if (r32(m, arg) != 0) return;
    host.call(m, 0x127358, {0x4be280, r32(m, arg + 4), r32(m, arg + 8)});
    const auto network = r8(m, kNetworkGame);
    if (network ? r8(m, kHostRole) : 1U) w8(m, kState, 0xa);
    else w8(m, kState, r8(m, kState) == 9 ? 0xa : 9);
    w8(m, 0x493a10 + r32(m, arg + 8), 0);
    const auto world = r32(m, kWorld);
    auto slot = r32(m, arg + 8);
    const auto local = local_slot(m, host, network, world);
    if (!(s32(slot) < s32(local))) slot -= 1;
    const auto client = r32(m, arg + 8);
    const auto record = 0x55c7c0 + slot * 0x98;
    w8(m, 0x572418 + client, 0);
    if (host.call(m, 0x2eb9a8, {record, r32(m, arg + 4), r32(m, arg + 8)})) return;
    w8(m, 0x55c7d4 + slot * 0x98, 1);
    w32(m, 0x55c7d0 + slot * 0x98, r32(m, arg + 8));
}

void dme_client_joined_00429a90(LevelLoadMemory& m, std::uint32_t arg) {
    busy_enter(m);
    w32(m, 0x5723f0, r32(m, 0x5723f0) + 1);
    w8(m, 0x572418 + r32(m, arg + 4), 0);
    if (host_or_solo(m) && r32(m, arg + 4) != 0) w32(m, 0x572424 + r32(m, arg + 4) * 4, 0x0ffffffeU);
    w8(m, kBusy, 0);
}

void dme_client_left_00429920(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t arg, std::uint32_t stack) {
    const auto sp = stack - 0x30;
    busy_enter(m);
    w32(m, 0x5723f0, r32(m, 0x5723f0) - 1);
    host.call(m, 0x2ebba0, {r32(m, kWorld), sp + 0x24});
    if (r32(m, arg + 4) == r32(m, sp + 0x24)) {  // the session host left
        w8(m, 0x572410, 1);
        w8(m, kQuitRequest, 1);
        host.call(m, 0x128aa8, {kQuitText, 0x10, 0x4bf9d0});  // "HOST_DISCONNECT"
        w8(m, kState, 7);
    }
    w8(m, 0x572418 + r32(m, arg + 4), 1);
    if (host_or_solo(m)) {
        if (r32(m, 0x572430) == 0xffffffffU) w32(m, 0x572430, 0);
        else if (r32(m, 0x57242c) == 0xffffffffU) w32(m, 0x57242c, 0);
        else if (r32(m, 0x572428) == 0xffffffffU) w32(m, 0x572428, 0);
    }
    host.call(m, 0x43b190, {sp + 0x28});
    host.call(m, 0x43acb0, {sp + 0x28, r32(m, arg + 4)});
    w8(m, kBusy, 0);
}

void geninfo_client_left_0043acb0(LevelLoadMemory& m, std::uint32_t client) {
    if (r32(m, 0x583888)) w8(m, 0x5838a0 + client, 1);
}

void dme_level_net_start_0042b0c0(LevelLoadMemory& m, LevelLoadHost& host) {
    w8(m, kInLevel, 1);
    const auto list = r32(m, 0x571740);  // 0040ca20
    const auto vector = list + 4;         // 001339a0
    const auto end = r32(m, vector + 8) + r32(m, vector + 4) * 4;  // 00133960
    for (auto at = r32(m, vector + 8); at != end; at += 4) {         // 00133990 / 00133980
        const auto object = r32(m, at);
        host.call(m, r32(m, r32(m, object) + 0xfc), {object, 0});
    }
}

std::uint32_t dme_clock_0042b1a0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t local, std::uint32_t stack) {
    const auto out = stack - 0x20 + 0x1c;
    const auto failed = host.call(m, local ? 0x2ebc48 : 0x2ebce0, {out});
    return failed ? 0xffffffffU : r32(m, out);
}

void dme_send_network_data_003fac40(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t flag) {
    const auto again = flag ? r8(m, flag) : 0U;
    const auto due = r32(m, 0x55c990);
    if (host.call(m, 0x42b1a0, {1}) < due) return;
    w32(m, 0x55c990, host.call(m, 0x42b1a0, {1}) + 100);
    const auto in_level = r8(m, kInLevel);
    for (std::uint32_t client = 0; client < 4; ++client) {
        const auto sent = r8(m, 0x493a10 + client);
        if (sent != 0 && !(again && sent == 1)) continue;
        if (!in_level) {
            if (!flag) w8(m, 0x55c780 + client, 1);
            continue;
        }
        w8(m, 0x493a10 + client, 1);
        host.call(m, 0x3fa940, {client});
        if (flag) w8(m, flag, 0);
        return;
    }
}

void dme_geninfo_update_0043a810(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack) {
    constexpr std::uint32_t kGenInfoHandle = 0x497578, kGenInfoDirty = 0x583890;
    const auto out = stack - 0x20 + 0x1c;
    host.call(m, 0x2eb650, {out, r32(m, kGenInfoHandle)});
    if (r8(m, kGenInfoDirty)) {
        const auto index = local_slot(m, host, r8(m, kNetworkGame), r32(m, kWorld));
        if (r32(m, r32(m, out) + 4) == index) host.call(m, 0x2ec460, {r32(m, kGenInfoHandle)});
    }
    w8(m, kGenInfoDirty, 0);
}

std::uint32_t npc_info_update_00436c00(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t arg) {
    if (!r8(m, 0x582778)) host.call(m, 0x4355d0, {arg});
    w32(m, 0x582a68, r32(m, 0x582a68) + 1);
    return arg;
}

namespace {
// NPCInfoObject.cc's five NPC info blocks (0x1d0 bytes: 29 entries of 16) and their DME objects, 8 bytes apart.
constexpr std::uint32_t kNpcBlocks = 0x5827f0, kNpcHandles = 0x582a78, kNpcDirty = 0x5827a0, kNpcSending = 0x5827c8;
constexpr std::uint32_t kNpcTimers = 0x582b60, kNpcCreated = 0x582778, kNpcReleased = 0x582780;
constexpr std::uint32_t kNpcSource = 0x4bfca0;  // "NPCInfoObject.cc"
constexpr std::uint32_t kHeapCounter = 0x533888;

// The allocator's guard around 0036b180 / 0036b130: DisableIntc(9) while 0x00533880 is set, 0x00533888 + 1; after
// the call EnableIntc(9) (0x00533880 read again) and - 1.
void heap_enter(LevelLoadMemory& m, LevelLoadHost& host) {
    if (r8(m, kIntcInstalled)) host.call(m, 0x10dc40, {9});
    w32(m, kHeapCounter, r32(m, kHeapCounter) + 1);
}
void heap_leave(LevelLoadMemory& m, LevelLoadHost& host) {
    if (r8(m, kIntcInstalled)) host.call(m, 0x10dca8, {9});
    w32(m, kHeapCounter, r32(m, kHeapCounter) - 1);
}
}  // namespace

void npc_info_objects_004355d0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t arg) {
    if (r8(m, 0x5721b8)) return;
    if (r8(m, kInLevel)) {
        if (r8(m, kNpcCreated) && !r8(m, kNpcReleased) && host_or_solo(m)) {
            std::uint32_t blocks[5];
            for (std::uint32_t k = 0; k < 5; ++k) blocks[k] = r32(m, kNpcBlocks + 8 * k);
            host.call(m, 0x433a60, {arg});
            for (std::uint32_t k = 0; k < 5; ++k) {
                if (!blocks[k]) continue;
                heap_enter(m, host);
                host.call(m, 0x36b130, {0, blocks[k], kNpcSource, 0x24e + 3 * k});
                heap_leave(m, host);
            }
            w8(m, kNpcReleased, 1);
        }
        host.call(m, 0x434740, {arg});
        return;
    }
    if (r8(m, kNpcCreated) || !r8(m, kNetworkGame)) return;
    for (std::uint32_t k = 0; k < 5; ++k) {
        heap_enter(m, host);
        const auto block = host.call(m, 0x36b180, {0, 0x1d0, kNpcSource, 0x269 + k});
        heap_leave(m, host);
        if (block)
            for (std::uint32_t e = block; e != block + 0x1d0; e += 0x10) {
                w32(m, e, 0xffffffffU);
                w8(m, e + 4, 0xff);
                w8(m, e + 5, 0xff);
                for (std::uint32_t b = 6; b < 0xa; ++b) w8(m, e + b, 0);
                for (std::uint32_t b = 0xa; b < 0xe; ++b) w8(m, e + b, 1);
            }
        w32(m, kNpcBlocks + 8 * k, block);
    }
    w8(m, kNpcCreated, 1);
}

void npc_info_objects_create_00433a60(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack) {
    const auto sp = stack - 0x40;
    if (!r8(m, kInLevel) || !host_or_solo(m)) return;
    for (std::uint32_t k = 0; k < 5; ++k) {
        const auto handle = kNpcHandles + 8 * k;
        const auto cls = r32(m, 0x582a70);
        w32(m, sp, 0x4bfcc8);  // "cNPCInfoObject" (sd: the upper word is 0)
        w32(m, sp + 4, 0);
        w32(m, sp + 8, cls);   // lw then sd: sign-extended
        w32(m, sp + 0xc, (cls & 0x80000000U) ? 0xffffffffU : 0U);
        if (host.call_framed(m, 0x2eaf28, {handle, 2, 4, 0, 0, 0, 0, r32(m, kNpcBlocks + 8 * k)}, sp)) continue;
        const auto out = sp + 0x3c - 4 * k;
        if (host.call(m, 0x2eb650, {out, r32(m, handle)})) continue;
        w32(m, kNpcBlocks + 8 * k, r32(m, r32(m, out) + 0x1c));
    }
}

void npc_info_send_timers_00434740(LevelLoadMemory& m, LevelLoadHost& host) {
    static constexpr std::uint32_t period[5] = {0x78, 0x96, 0xb4, 0xd2, 0xf0};
    for (std::uint32_t k = 0; k < 5; ++k) {
        const auto timer = r32(m, kNpcTimers + 8 * k);
        if (!(timer < host.call(m, 0x42b1a0, {1}))) continue;
        if (r8(m, kNpcSending + 8 * k) && host.call(m, 0x2ec3c8, {r32(m, kNpcHandles + 8 * k)}) == 0)
            w8(m, kNpcSending + 8 * k, 0);
        w32(m, kNpcTimers + 8 * k, host.call(m, 0x42b1a0, {1}) + period[k]);
    }
}

void npc_info_send_changed_004352c0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack) {
    const auto sp = stack - 0x40;
    for (std::uint32_t k = 0; k < 5; ++k) {
        const auto dirty = kNpcDirty + 8 * k;
        if (!r8(m, dirty)) continue;
        const auto handle = r32(m, kNpcHandles + 8 * k);
        const auto out = sp + 0x3c - 4 * k;
        host.call(m, 0x2eb650, {out, handle});  // the result is not tested: the word at `out` is read as it is
        if (const auto object = r32(m, out)) {
            const auto index = local_slot(m, host, r8(m, kNetworkGame), r32(m, kWorld));
            if (r32(m, r32(m, out) + 4) == index) host.call(m, 0x2ec460, {handle});  // the word is read again
        }
        w8(m, dirty, 0);
    }
}

std::uint32_t npc_info_release_00436ba0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t object,
                                        std::uint32_t mode) {
    if (object) {
        w32(m, 0x582a68, r32(m, 0x582a68) - 1);
        if (static_cast<std::int16_t>(mode & 0xffffU) > 0) host.call(m, 0x100740, {object, mode});
    }
    return object;
}

namespace {
// 0013a2a0: the NetMap iterator's ++ (a red-black tree node: +0 left, +4 right, +8 parent | colour bit).
std::uint32_t net_map_next(const LevelLoadMemory& m, std::uint32_t node) {
    if (auto n = r32(m, node + 4)) {
        while (const auto left = r32(m, n)) n = left;
        return n;
    }
    for (;;) {
        const auto parent = r32(m, node + 8) & ~1U;
        if (node == r32(m, parent)) return parent;
        node = parent;
    }
}
}  // namespace

std::uint32_t net_map_take_003fa780(LevelLoadMemory& m, std::uint32_t a_out, std::uint32_t flag_out,
                                    std::uint32_t data_out) {
    if (r8(m, kState) == 7) return 0;
    const auto end = kNetMap + 4;                                       // 0021d360
    for (auto it = r32(m, kNetMap + 0xc); it != end; it = net_map_next(m, it)) {  // 003fa8b0 / 0013a2a0
        if (!r32(m, it + 0x1c)) continue;
        w8(m, flag_out, r8(m, it + 0x18));
        w32(m, a_out, r32(m, it + 0x14));
        w32(m, data_out, r32(m, it + 0x1c));
        w32(m, it + 0x1c, 0);
        return 1;
    }
    return 0;
}

namespace {
// 0013cf60 / 0013cff0: the red-black tree's left / right rotation at x; *root_slot is the caller's root copy,
// the parent's link (the header's +0 for the root) is rewritten through x's parent.
void net_map_rotate(LevelLoadMemory& m, std::uint32_t x, std::uint32_t& root_slot, bool left) {
    const auto near = left ? 4U : 0U, far = left ? 0U : 4U;  // y = x.right (left rotation) / x.left
    const auto y = r32(m, x + near);
    if (root_slot == x) root_slot = y;
    w32(m, x + near, r32(m, y + far));
    if (const auto inner = r32(m, y + far)) w32(m, inner + 8, x | (r32(m, inner + 8) & 1));
    w32(m, y + 8, (r32(m, x + 8) & ~1U) | (r32(m, y + 8) & 1));
    const auto parent = r32(m, x + 8) & ~1U;
    if (x == r32(m, parent)) w32(m, parent, y);
    else w32(m, parent + 4, y);
    w32(m, y + far, x);
    w32(m, x + 8, y | (r32(m, x + 8) & 1));
}

// 0013cd40(node, root): the insert fix-up (colour bit 1 = red); the root copy ends black.
void net_map_rebalance(LevelLoadMemory& m, std::uint32_t node, std::uint32_t root) {
    const auto parent_of = [&](std::uint32_t n) { return r32(m, n + 8) & ~1U; };
    const auto red = [&](std::uint32_t n) { return (r32(m, n + 8) & 1) != 0; };
    const auto paint = [&](std::uint32_t n, bool r) { w32(m, n + 8, (r32(m, n + 8) & ~1U) | (r ? 1U : 0U)); };
    w32(m, node + 8, r32(m, node + 8) | 1);
    while (node != root && red(parent_of(node))) {
        const auto parent = parent_of(node);
        const auto grand = parent_of(parent);
        const bool on_left = parent == r32(m, grand);
        const auto uncle = r32(m, grand + (on_left ? 4 : 0));
        if (uncle && red(uncle)) {
            paint(parent, false);
            paint(uncle, false);
            node = parent_of(parent_of(node));
            paint(node, true);
            continue;
        }
        if (node == r32(m, parent + (on_left ? 4 : 0))) {
            node = parent;
            net_map_rotate(m, node, root, on_left);
        }
        paint(parent_of(node), false);
        paint(parent_of(parent_of(node)), true);
        net_map_rotate(m, parent_of(parent_of(node)), root, !on_left);
    }
    paint(root, false);
}

// 002d1430(map, parent, left, leftmost, pair): a new 0x24-byte node ("allocator.hh" line 0x5c) holding the pair.
std::uint32_t net_map_insert(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t map, std::uint32_t parent,
                             bool left, bool leftmost, std::uint32_t key) {
    if (r32(m, map) == 0xffffffffU) {
        host.call(m, 0x123be8, {r32(m, r32(m, 0x489738) + 0xc), 0x4ad4c0});
        host.call(m, 0x121b60, {});
        throw std::runtime_error("002d1430: the NetMap's length_error (00121b60 does not return)");
    }
    heap_enter(m, host);
    const auto node = host.call(m, 0x36b180, {0, 0x24, 0x4ad508, 0x5c});  // "allocator.hh"
    heap_leave(m, host);
    if (node + 0xc != 0) {
        w32(m, node + 0xc, key);
        w32(m, node + 0x10, 0);
        w32(m, node + 0x14, 0xffffffffU);
        w8(m, node + 0x18, 0);
        w32(m, node + 0x1c, 0);
        w32(m, node + 0x20, 0);
    }
    w32(m, node + 4, 0);
    w32(m, node, 0);
    w32(m, node + 8, parent | (r32(m, node + 8) & 1));
    w32(m, parent + (left ? 0 : 4), node);
    w32(m, map, r32(m, map) + 1);
    net_map_rebalance(m, node, r32(m, map + 4));
    if (leftmost) w32(m, map + 0xc, node);
    return node;
}
}  // namespace

std::uint32_t net_map_index_002d1360(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t map, std::uint32_t key) {
    bool left = true, leftmost = true;
    std::uint32_t parent = map + 4, last_right = 0;
    for (auto n = r32(m, map + 4); n;) {
        parent = n;
        if (s32(key) < s32(r32(m, n + 0xc))) {
            n = r32(m, n);
            left = true;
        } else {
            last_right = n;
            n = r32(m, n + 4);
            left = leftmost = false;
        }
    }
    if (last_right && !(s32(r32(m, last_right + 0xc)) < s32(key))) return last_right + 0xc;
    return net_map_insert(m, host, map, parent, left, leftmost, key) + 0xc;
}

void net_map_set_003fa8c0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t a, std::uint32_t key, std::uint32_t data,
                          std::uint32_t flag) {
    w32(m, net_map_index_002d1360(m, host, kNetMap, key) + 8, a);
    w32(m, net_map_index_002d1360(m, host, kNetMap, key) + 0x10, data);
    w8(m, net_map_index_002d1360(m, host, kNetMap, key) + 0xc, flag);
}

std::uint32_t net_map_set_object_003fb650(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t key, std::uint32_t a,
                                          std::uint32_t object) {
    if (r8(m, kNetworkGame)) {
        w32(m, net_map_index_002d1360(m, host, kNetMap, key) + 4, object);
        w32(m, net_map_index_002d1360(m, host, kNetMap, key) + 8, a);
        return net_map_index_002d1360(m, host, kNetMap, key) + 4;
    }
    if (!r8(m, 0x55c9b8)) {  // the static dummy entry, built once
        w32(m, 0x55c9c0, 0);
        w32(m, 0x55c9c4, 0xffffffffU);
        w8(m, 0x55c9b8, 1);
        w8(m, 0x55c9c8, 0);
        w32(m, 0x55c9cc, 0);
        w32(m, 0x55c9d0, 0);
    }
    return 0x55c9c0;
}

std::uint32_t local_player_0040bf10(const LevelLoadMemory& m) {
    const auto player = r32(m, 0x571770);
    if (!player || r32(m, player + 0xc) != r32(m, 0x571774)) return 0;
    return player;
}

void net_client_loaded_003fa940(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t client) {
    w8(m, 0x55c780 + client, 2);
    if (r8(m, 0x572400)) host.call(m, 0x429b30, {});
    const auto player = host.call(m, 0x40bf10, {});
    if (!player) throw std::runtime_error("003fa940: no local player (0040bf10 returned 0; the original reads its vtable)");
    host.call(m, r32(m, r32(m, player) + 0x1c), {player, client});
    const auto end = kNetMap + 4;
    for (auto it = r32(m, kNetMap + 0xc); it != end; it = net_map_next(m, it)) {
        const auto object = r32(m, it + 0x10);
        if (object && r8(m, object + 0x14) && r32(m, object + 0xc) != r32(m, player + 0xc))
            host.call(m, r32(m, r32(m, object) + 0x1c), {object, client});
        host.call(m, 0x3fac40, {0});
    }
}

void net_frame_003fbec0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack) {
    const auto sp = stack - 0x20;
    w8(m, 0x5724d0, 1);
    if (r8(m, 0x493a18)) w8(m, 0x493a18, 0);
    host.call(m, 0x436c00, {sp + 0x1c});
    host.call(m, 0x4355d0, {sp + 0x1c});
    host.call(m, 0x43b190, {sp + 0x18});
    host.call(m, 0x43aa30, {sp + 0x18});
    w32(m, 0x55c7b8, host.call(m, 0x42b1a0, {0}) + 0x64);
    host.call(m, 0x42eb20, {});
    host.call(m, 0x438770, {0});
    host.call(m, 0x436ba0, {sp + 0x1c, 0xffffffffU});
}

void net_update_00169390(LevelLoadMemory& m, LevelLoadHost& host) {
    if (r8(m, kNetworkGame)) {
        host.call(m, 0x472080, {});
        host.call(m, 0x429d30, {});
    }
    if (r32(m, 0x4ffc04) && host.call(m, 0x4294c0, {})) host.call(m, 0x2ca580, {r32(m, 0x4ffc04)});
}

void net_throttle_send_00472080(LevelLoadMemory& m, LevelLoadHost& host) {
    host.call(m, 0x42b1a0, {1});
    // 00472070 / 00471ea0 / 00471e90: the map's leftmost node (+0xc) against its header (+4).
    if (r32(m, 0x58802c) != 0x588024)
        throw std::runtime_error("00472080: cNetMsgThrottle's queued messages (the map 0x00588020) are not ported");
}

void lobby_keepalive_002ca580(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t app) {
    if (r8(m, 0x51eeb0)) return;
    const auto s = host.call(m, 0x4294c0, {});
    if (!s) return;
    host.call(m, 0x4536b0, {s});
    if (!host.call(m, 0x452fd0, {s})) return;
    if (r32(m, s + 0x255c) == 0xa) {
        w8(m, kPeerQuit, 1);
        return;
    }
    host.call(m, 0x419260, {host.call(m, 0x414790, {})});
    host.call(m, 0x2ca6f0, {app});
    host.call(m, 0x2ca640, {app, 1});
    host.call(m, 0x453b00, {s, 1});
}

namespace {
// GenInfoObject.cc: the general-info block (0x37 entries of 8 bytes), its DME object and the send timer.
constexpr std::uint32_t kGenInfoBlock = 0x583888, kGenInfoCreated = 0x583880, kGenInfoHandle = 0x497578;
constexpr std::uint32_t kGenInfoSource = 0x4bfd60;  // "GenInfoObject.cc"
}  // namespace

void geninfo_objects_00439670(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack) {
    const auto sp = stack - 0x40;
    if (!r8(m, kNetworkGame) || r8(m, 0x5721b8)) return;
    if (r8(m, kInLevel)) {
        if (r8(m, kGenInfoCreated) || !host_or_solo(m)) return;
        const auto block = r32(m, kGenInfoBlock);
        const auto cls = r32(m, 0x497570);
        w32(m, sp, 0x4bfd38);  // "cGenInfoObject" (sd: the upper word is 0)
        w32(m, sp + 4, 0);
        w32(m, sp + 8, cls);   // lw then sd: sign-extended
        w32(m, sp + 0xc, (cls & 0x80000000U) ? 0xffffffffU : 0U);
        if (!host.call_framed(m, 0x2eaf28, {kGenInfoHandle, 2, 4, 0, 0, 0, 0, block}, sp) &&
            !host.call(m, 0x2eb650, {sp + 0x3c, r32(m, kGenInfoHandle)}))
            w32(m, kGenInfoBlock, r32(m, r32(m, sp + 0x3c) + 0x1c));
        if (block) {
            heap_enter(m, host);
            host.call(m, 0x36b130, {0, block, kGenInfoSource, 0x211});
            heap_leave(m, host);
        }
        w8(m, kGenInfoCreated, 1);
        return;
    }
    if (r32(m, kGenInfoBlock)) return;
    heap_enter(m, host);
    const auto block = host.call(m, 0x36b180, {0, 0x1b8, kGenInfoSource, 0x21b});
    heap_leave(m, host);
    if (block)
        for (std::uint32_t e = block; e != block + 0x1b8; e += 8) {
            w32(m, e, 0xffffffffU);
            w8(m, e + 4, 0x0f);  // two bit fields: the low nibble 0xf, the high nibble 0
        }
    w32(m, kGenInfoBlock, block);
}

std::uint32_t geninfo_create_0043b190(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t arg) {
    host.call(m, 0x439670, {});
    return arg;
}

void geninfo_send_timer_0043a8b0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack) {
    const auto out = stack - 0x30 + 0x2c;
    const auto due = r32(m, 0x583910);
    if (!(due < host.call(m, 0x42b1a0, {1}))) return;
    const bool changed = r32(m, 0x5838b8) || r32(m, 0x5838d8) || r32(m, 0x583900) || r8(m, 0x5838a0) ||
                         r8(m, 0x5838a1) || r8(m, 0x5838a2) || r8(m, 0x5838a3);
    if (changed) {
        host.call(m, 0x2eb650, {out, r32(m, kGenInfoHandle)});  // the result is not tested
        const auto index = local_slot(m, host, r8(m, kNetworkGame), r32(m, kWorld));
        if (r32(m, r32(m, out) + 4) != index) {
            host.call(m, 0x2ec3c8, {r32(m, kGenInfoHandle)});
        } else {
            const auto local = local_slot(m, host, r8(m, kNetworkGame), r32(m, kWorld));
            // $s2 is 00167c40's (003caa50's object), taken as nonzero (an assumption: see 00438e20).
            host.call(m, 0x439390, {local, r32(m, kGenInfoHandle), 0xfffffffeU, 1});
        }
    }
    w32(m, 0x583910, host.call(m, 0x42b1a0, {1}) + 0x1f4);
}

void geninfo_send_00439390(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t handle, std::uint32_t mode,
                           std::uint32_t stack, std::uint32_t s2) {
    const auto sp = stack - 0x40;
    if (host.call(m, 0x2eb650, {sp + 0x3c, handle})) return;
    const auto object = r32(m, sp + 0x3c);
    const auto block = r32(m, object + 0x1c);  // loaded in the branch's delay slot
    if (mode != 0xfffffffeU) return;
    if (r32(m, object + 4) != local_slot(m, host, r8(m, kNetworkGame), r32(m, kWorld))) return;
    host.call(m, 0x438e20, {block, s2});
    host.call(m, 0x439030, {block});
    host.call(m, 0x438c40, {block});
    for (std::uint32_t client = 0; client < 4; ++client) {
        if (!r8(m, 0x5838a0 + client)) continue;
        if (host_or_solo(m)) {
            const std::uint32_t bit = 1U << client;
            for (std::uint32_t e = block; e != block + 0x37 * 8; e += 8) {
                if (r32(m, e) == 0xffffffffU) continue;
                auto b = r8(m, e + 4);
                if ((b & 0xfU) == client) b &= 0xf0U;  // the low nibble (the owner): the left client's become 0
                w8(m, e + 4, (b & 0x0fU) | (((b >> 4) & ~bit & 0xfU) << 4));  // the high nibble: a client mask
            }
        }
        w8(m, 0x5838a0 + client, 0);
    }
    w8(m, 0x583890, 1);  // the GenInfo object is dirty (0043a810 sends it: 002ec460)
}

void net_objects_update_0042eb20(LevelLoadMemory& m, LevelLoadHost& host) {
    // 0042eaf0 / 0042da20 / 0042da10: the map's leftmost node (+0xc) against its header (+4).
    if (r32(m, 0x58270c) != 0x582704)
        throw std::runtime_error("0042eb20: the network objects in the map 0x00582700 (their updates) are not ported");
    host.call(m, 0x42cf30, {});
}

void net_objects_send_0042cf30(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack) {
    const auto sp = stack - 0x70;
    // 0011c7a0 / 0011bd70 / 0011c268 / 0011c200 / 0011c858 (libgcc soft double, no side effects) turn the frame
    // count 0x00572538 into a period that only the per-object loop reads.
    host.call(m, 0x436c00, {sp + 0x68});
    if (r32(m, 0x58272c) != 0x582724)  // 0042d100 / 0042d0f0 / 0042d0e0
        throw std::runtime_error("0042cf30: the network objects in the map 0x00582720 (their sends) are not ported");
    host.call(m, 0x436ba0, {sp + 0x68, 0xffffffffU});
}

std::uint32_t voice_headset_00438340(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack) {
    const auto sp = stack - 0x50;
    std::uint32_t result = 1;
    if (host_or_solo(m) && !r32(m, 0x582cf0)) {
        std::uint32_t data = 0;
        if (r8(m, kInLevel)) {
            const auto cls = r32(m, 0x583868);
            w32(m, sp, 0x4bfd20);  // "HeadsetSemaphore" (sd: the upper word is 0)
            w32(m, sp + 4, 0);
            w32(m, sp + 8, cls);   // lw then sd: sign-extended
            w32(m, sp + 0xc, (cls & 0x80000000U) ? 0xffffffffU : 0U);
            if (!host.call_framed(m, 0x2eaf28, {sp + 0x48, 2, 4, 0x3e8, 0, 0, 0, 0}, sp)) {
                w32(m, 0x583870, r32(m, sp + 0x48));
                if (!host.call(m, 0x2eb650, {sp + 0x44, r32(m, sp + 0x48)})) data = r32(m, r32(m, sp + 0x44) + 0x1c);
            }
        }
        w32(m, 0x582cf0, data);
    }
    if (r8(m, 0x497568)) {
        w8(m, 0x497568, 0);
        w32(m, 0x497548, 0xffffffffU);
        w8(m, 0x582b88, 0);
        if (!host.call(m, 0x3241c8, {0x2caa70, 0x2ca9b0})) w8(m, 0x582b90, 1);  // lgaud.c's init
    }
    if (r8(m, 0x582b90)) {
        host.call(m, 0x325280, {sp + 0x4c});
        if ((r32(m, sp + 0x4c) == 1 || !r8(m, 0x582b88)) && !host.call(m, 0x4384c0, {})) result = 0;
    }
    return result;
}

void voice_frame_00438770(LevelLoadMemory& m, LevelLoadHost& host) {
    if (!host.call(m, 0x438340, {}) || !r8(m, 0x582b88)) return;
    throw std::runtime_error("00438770: the headset voice stream (0x00582b88 set) is not ported");
}

void net_create_characters_00185840(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack) {
    const auto sp = stack - 0x30;
    const auto a = sp + 0x24, flag = sp + 0x2f, data = sp + 0x28;
    while (host.call(m, 0x3fa780, {a, flag, data})) {
        host.call(m, r8(m, flag) ? 0x2c6b70 : 0x2c6690, {r32(m, data), r32(m, a)});
        const auto block = r32(m, data);
        heap_enter(m, host);
        host.call(m, 0x36b130, {0, block, 0x49d8d0, 0x447d});  // "human.cc"
        heap_leave(m, host);
    }
}

void net_load_network_data_003fa5d0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack) {
    const auto sp = stack - 0x60;
    if (r8(m, kState) == 7) return;
    for (std::uint32_t client = 0; client < 4; ++client) {
        if (r8(m, 0x55c780 + client) != 1) {
            host.call(m, 0x127358, {0x4be240, client});  // "Trying to LoadNetworkData but %d is already updated"
            continue;
        }
        const auto list = r32(m, 0x571748);  // 0040ca30; the vector at +4 (001339a0)
        auto it = r32(m, list + 0xc);         // 00133990
        while (it != r32(m, list + 0xc) + 4 * r32(m, list + 8)) {  // 00133960 / 00133980
            const auto component = r32(m, r32(m, it) + 0x58);
            bool ready = false;
            if (component && host.call(m, r32(m, r32(m, component) + 0x54), {component, sp + 0x50})) ready = true;
            if (ready) {
                const auto object = r32(m, it);
                if (r8(m, object + 0x14)) {
                    host.call(m, r32(m, r32(m, object) + 0x1c), {object, client});
                    break;
                }
            }
            it += 4;
        }
    }
}

std::uint32_t dme_update_00429d30(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack) {
    const auto sp = stack - 0xf0;
    // The level-info sends queued by 00429a90 for clients 1..3 (0x00572428..0x00572430).
    for (std::uint32_t i = 0; i < 3; ++i) {
        const auto queued = 0x572428 + 4 * i;
        if (r32(m, queued) != 0x0ffffffeU) continue;
        const auto message = sp + 0x80;
        const auto value = r32(m, 0x494198);
        host.call(m, 0x3c9e30, {message, 0x5839a0});
        w32(m, message, 0x4e1180);         // the NetLevelInfoMsg vtable
        w32(m, sp + 0xa4, value);
        w32(m, sp + 0x9c, 0x40);           // reliable and ordered
        const auto length = host.call(m, 0x440450, {message, 0, sp + 0xd0, 0, 0, 0x20, sp + 0x60});
        if (s32(length) >= 0) {
            const auto flags = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(r8(m, sp + 0x9c))));
            const auto error = host.call(m, 0x2ea0a0, {flags, r32(m, kWorld), i + 1, 2, r32(m, sp + 0xd0), length, sp + 0x60});
            if (error) host.call(m, 0x127358, {0x4bf9e0, 0x4bf9b0, 0x528, error});  // "%s : %d SHIT - %d"
        }
        w32(m, queued, 0xffffffffU);
        w32(m, message, 0x4e1180);
        host.call(m, 0x3c9d60, {message, 0});
    }

    std::uint32_t result = 0x00ffffff;
    if (r8(m, kMessagesReady)) {
        if (!r8(m, kBusy)) {
            busy_enter(m);
            w8(m, kBusy, 0);
            if (r8(m, kInLevel) && r8(m, 0x572420)) w32(m, 0x5724e0, 0);
            w32(m, 0x5724d8, host.call(m, 0x2ebce0, {sp + 0xcc}) ? 0xffffffffU : r32(m, sp + 0xcc));
            if (r8(m, 0x5724d0)) host.call(m, 0x3fbeb0, {});
            if (r8(m, kInLevel)) {
                host.call(m, 0x43a810, {});
                host.call(m, 0x436c00, {sp + 0xec});
                host.call(m, 0x4352c0, {sp + 0xec});
                host.call(m, 0x436ba0, {sp + 0xec, 0xffffffffU});
            }
            result = host.call(m, 0x2ec4f8, {});
            if (r8(m, kInLevel)) {
                for (std::uint32_t client = 0; client < 4; ++client) {
                    const auto world = r32(m, kWorld);
                    if (client == local_slot(m, host, r8(m, kNetworkGame), world)) continue;
                    host.call(m, 0x2eb7a0, {sp + 0xc8, r32(m, kWorld), client});
                    if (r32(m, sp + 0xc8) < 2U) continue;
                    host.call(m, 0x2ecf08, {r32(m, kWorld), client, sp + 0xe8, sp + 0xe0, sp + 0xe4, sp + 0xdc});
                    const auto ring = 0x5724f0 + 4 * client;
                    w32(m, 0x572270 + client * 0x50 + r32(m, ring) * 4, r32(m, sp + 0xe8));
                    w32(m, ring, r32(m, ring) + 1);
                    if (s32(r32(m, ring)) >= 20) w32(m, ring, 0);
                }
            }
            if (result) {
                w32(m, sp + 0xb0, 0x18);
                host.call(m, 0x2ec580, {sp + 0xb0});
                for (std::uint32_t client = 0; client < 4; ++client) {
                    const auto status = r32(m, sp + 0xb4 + 4 * client);
                    if (!status) continue;
                    if (client != r32(m, kWorld)) {
                        if (status != 2) w8(m, kPeerQuit, 1);
                    } else if (status != 0xc) {
                        w8(m, kQuitRequest, 1);
                        host.call(m, 0x128aa8, {kQuitText, 0x10, 0x4bfa00});  // "GAME_DISCONNECT"
                    }
                }
            }
        }
        host.call(m, 0x3fac40, {0x572408});
    }
    if (r8(m, kThreadStarted) && r32(m, 0x572480) && r8(m, 0x572488) && !r8(m, kBusy)) {
        busy_enter(m);
        w8(m, kBusy, 0);
        host.call(m, 0x4536b0, {r32(m, 0x572480)});  // SFOLobby_Main's update of the lobby session
    }
    return result;
}

void dme_init_0042af00(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack) {
    const auto sp = stack - 0x60;
    for (std::uint32_t k = 0; k < 4; ++k) w8(m, 0x572418 + k, 0);
    if (host.call(m, 0x2ea4f0, {0x2caa70, 0x2ca9b0})) return;
    host.call(m, 0x2e9480, {sp + 0x40});
    if (host.call(m, 0x2e94e0, {sp + 0x40, sp + 0x20})) {
        w8(m, kState, 2);
        return;
    }
    host.call(m, 0x2e9320, {sp + 0x10});
    if (host.call(m, 0x2ec168, {0x6000, 0x6000})) return;
    w8(m, kState, 1);
}

namespace {
std::uint32_t r16(const LevelLoadMemory& m, std::uint32_t a) { return m.read(a, 2); }
void w16(LevelLoadMemory& m, std::uint32_t a, std::uint32_t v) { m.write(a, 2, v & 0xffffU); }
std::uint32_t sext16(std::uint32_t v) { return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(v))); }
std::uint32_t sext8(std::uint32_t v) { return static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(v))); }
float rf(const LevelLoadMemory& m, std::uint32_t a) { return std::bit_cast<float>(r32(m, a)); }
void wf(LevelLoadMemory& m, std::uint32_t a, float v) { w32(m, a, std::bit_cast<std::uint32_t>(v)); }
// cvt.w.s (truncation); the port refuses what the EE would saturate.
std::uint32_t truncate_word(float v) {
    if (!(std::fabs(v) < 2147483648.0F)) throw std::runtime_error("cvt.w.s of a value outside the word range");
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
}
// The client byte 0x0055c780[client] the character sends test: 1 (joined) or 2 (loaded).
bool client_ready(const LevelLoadMemory& m, std::uint32_t client) {
    const auto state = r8(m, 0x55c780 + client);
    return state == 1 || state == 2;
}
constexpr std::uint32_t kCharacterList = 0x4ffd44;  // the game's character list (0040b6d0 / 0040b720 / 0040b790)
}  // namespace

std::uint32_t player_send_00185a30(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t player, std::uint32_t client,
                                   std::uint32_t stack) {
    const auto sp = stack - 0xb0;
    const auto gate = [&]() -> std::uint32_t {
        if (!r8(m, player + 0x2d)) return 0;
        if (!r8(m, player + 0x2c)) return 0;
        return r8(m, player + 0x2e) ? 0U : 1U;
    };
    if (!gate()) return 1;
    const auto weapons = sp + 0xa0;
    for (std::uint32_t k = 0; k < 12; k += 4) w32(m, weapons + k, r32(m, 0x48a2f8 + k));
    if (const auto inventory = r32(m, player + 0x3524)) {
        const auto slot = r8(m, inventory + 0x84);
        const auto current = slot == 6 ? 0xffffffffU : r32(m, inventory + slot * 16 + 8);
        w16(m, weapons, current);
        const auto held = sext16(current);
        auto out = weapons + 2;
        for (std::uint32_t k = 0; k < 6; ++k) {
            const auto weapon = r32(m, r32(m, player + 0x3524) + k * 16 + 8);
            if (weapon == 0xffffffffU || weapon == held) continue;
            w16(m, out, weapon);
            out += 2;
        }
    }
    bool human = false;
    if (const auto controller = r32(m, player + 0x58))
        human = host.call(m, r32(m, r32(m, controller) + 0x54), {controller}) != 0;
    if (human) {
        const auto health = truncate_word(rf(m, player + 0x40));
        const auto component = r32(m, player + 0x50);
        const auto key = r32(m, player + 0x10), id = r32(m, player + 0xc);
        if (r8(m, component + 0x50)) host.call(m, r32(m, r32(m, component) + 0x58), {component});
        host.call(m, 0x2c72a0, {client, key, id, component + 0x10, player + 0xa0, weapons, health});
        return 1;
    }
    const auto key = r32(m, player + 0x10), id = r32(m, player + 0xc), component = r32(m, player + 0x50);
    const auto a = gate();
    const auto health = truncate_word(rf(m, player + 0x40));
    if (r8(m, component + 0x50)) host.call(m, r32(m, r32(m, component) + 0x58), {component});
    w32(m, sp, health);  // sd: sign-extended
    w32(m, sp + 4, (health & 0x80000000U) ? 0xffffffffU : 0U);
    const auto b = r8(m, player + 0x3590);
    host.call_framed(m, 0x2c7110, {client, key, id, component + 0x10, player + 0x3318, weapons, a, b}, sp);
    return 1;
}

std::uint32_t send_named_character_002c7110(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t client,
                                            std::uint32_t key, std::uint32_t, std::uint32_t matrix, std::uint32_t name,
                                            std::uint32_t weapons, std::uint32_t a, std::uint32_t b, std::uint32_t stack) {
    const auto sp = stack - 0xf0;
    if (!client_ready(m, client)) return 0;
    const auto msg = sp + 0xa0;
    w32(m, msg + 4, key);
    w32(m, msg, 0x4de158);  // the NetCreateNamedCharacterMsg vtable
    host.call(m, 0x1bd590, {sp + 0x90, matrix});
    for (std::uint32_t k = 0; k < 12; k += 4) w32(m, msg + 8 + k, r32(m, sp + 0x90 + k));
    for (std::uint32_t k = 0; k < 12; k += 4) w32(m, msg + 0x14 + k, r32(m, matrix + 0x30 + k));
    host.call(m, 0x1260f0, {sp + 0xcc, 0, 0x20});
    const auto length = host.call(m, 0x1294f8, {name});
    host.call(m, 0x129a70, {sp + 0xcc, name, length});
    w16(m, msg + 0x4e, r32(m, stack));
    for (std::uint32_t k = 0; k < 6; ++k) w16(m, msg + 0x20 + 2 * k, r16(m, weapons + 2 * k));
    w8(m, msg + 0x4c, a);
    w8(m, msg + 0x4d, b);
    return host.call(m, 0x3fad50, {r32(m, 0x51ee78), 0x50, msg, client, 0x40}) ? 1U : 0U;
}

std::uint32_t send_character_002c72a0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t client, std::uint32_t key,
                                      std::uint32_t, std::uint32_t matrix, std::uint32_t source, std::uint32_t weapons,
                                      std::uint32_t health, std::uint32_t stack) {
    const auto sp = stack - 0x4d0;
    if (!client_ready(m, client)) return 0;
    const auto msg = sp + 0x90;
    w32(m, msg + 4, key);
    w32(m, msg, 0x4de148);  // the NetCreateCharacterMsg vtable
    host.call(m, 0x1bd590, {sp + 0x80, matrix});
    for (std::uint32_t k = 0; k < 12; k += 4) w32(m, msg + 8 + k, r32(m, sp + 0x80 + k));
    for (std::uint32_t k = 0; k < 12; k += 4) w32(m, msg + 0x14 + k, r32(m, matrix + 0x30 + k));
    w32(m, msg + 0x20, health);
    for (std::uint32_t k = 0; k < 6; ++k) w16(m, msg + 0x24 + 2 * k, r16(m, weapons + 2 * k));
    const auto data = host.call(m, 0x4073c0, {source, sp + 0x4cc});
    const auto length = s32(r32(m, sp + 0x4cc));
    // The message holds 0x400 bytes after +0x30; the original copies past it (its frame) without a check.
    if (length > 0x400) throw std::runtime_error("002c72a0: a compact appearance of more than 0x400 bytes overruns the message");
    for (std::int32_t k = 0; k < length; ++k) w8(m, msg + 0x30 + static_cast<std::uint32_t>(k), r8(m, data + static_cast<std::uint32_t>(k)));
    return host.call(m, 0x3fad50, {r32(m, 0x51ee70), 0x430, msg, client, 0x40}) ? 1U : 0U;
}

std::uint32_t net_send_003fad50(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t type, std::uint32_t size,
                                std::uint32_t msg, std::uint32_t client, std::uint32_t flags) {
    if (!r8(m, kInLevel)) return 0;
    bool reset = r8(m, 0x55c9e0) != 0;
    if (!reset) {
        const auto due = r32(m, 0x55c9d8) + 1000;
        reset = s32(due) < s32(host.call(m, 0x42b1a0, {1}));
    }
    if (reset) {
        w32(m, 0x55c9d8, host.call(m, 0x42b1a0, {1}));
        for (std::uint32_t k = 0; k < 0x14; k += 4) w32(m, 0x55c9a0 + k, 0);
        w8(m, 0x55c9e0, 0);
    }
    if (s32(client) >= 0 && !client_ready(m, client)) return 0;
    const auto mode = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(flags & 0xffU)));
    const auto error = host.call(m, 0x2ea0a0, {mode, r32(m, kWorld), client, 2, type, size, msg});
    if (!error) return 1;
    std::uint32_t result = 0;
    if (!host.call(m, 0x429d30, {}) && flags != 0x40) {
        const auto again = host.call(m, 0x2ea0a0, {mode, r32(m, kWorld), client, 2, type, size, msg});
        if (!again) result = 1;
        else host.call(m, 0x127358, {0x4be2b0, again});  // "cNetObj::SendMessage : unable to send: %d."
    }
    if (error != 0x25 && error != 0x23) return result;
    const auto counter = client == 0xffffffffU ? 0x55c9a0 : 0x55c9a4 + 4 * client;
    w32(m, counter, r32(m, counter) + 1);
    if (s32(r32(m, counter)) < 26) return result;
    host.call(m, 0x127358, {client == 0xffffffffU ? 0x4be2e0U : 0x4be330U});  // "Quitting because of too many send errors.."
    w8(m, 0x5723b0, 0);
    w8(m, kQuitRequest, 1);
    host.call(m, 0x129a70, {kQuitText, 0x4be320, 0x10});  // "NET_ERROR"
    w8(m, 0x5721f0, 0);
    return result;
}

std::uint32_t recv_named_character_002c7630(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t client,
                                            std::uint32_t msg) {
    busy_enter(m);
    if (r8(m, kInLevel)) host.call(m, 0x2c6690, {msg, client});
    w8(m, kBusy, 0);
    return 0x50;
}

std::uint32_t recv_character_002c7810(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t client, std::uint32_t msg) {
    busy_enter(m);
    if (r8(m, kInLevel)) {
        host.call(m, 0x2c6b70, {msg, client});
    } else {
        heap_enter(m, host);
        const auto copy = host.call(m, 0x36b180, {0, 0x430, 0x4ac980, 0x11});  // "NetCreateCharacterMsg.cc"
        heap_leave(m, host);
        if (copy) {
            w32(m, copy, 0x4de168);  // the base class's vtable, then the message's
            w32(m, copy, 0x4de148);
            w32(m, copy + 4, r32(m, msg + 4));
            for (std::uint32_t k = 8; k < 0x20; k += 4) w32(m, copy + k, r32(m, msg + k));
            w32(m, copy + 0x20, r32(m, msg + 0x20));
            for (std::uint32_t k = 0x24; k < 0x30; k += 2) w16(m, copy + k, r16(m, msg + k));
            for (std::uint32_t k = 0x30; k < 0x430; ++k) w8(m, copy + k, r8(m, msg + k));
        }
        host.call(m, 0x3fa8c0, {client, r32(m, msg + 4), copy, 1});
    }
    w8(m, kBusy, 0);
    return 0x430;
}

void create_remote_human_002c6b70(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t client,
                                  std::uint32_t stack) {
    const auto sp = stack - 0x620;
    // 0040b6d0 on the list, then the found character's chain (+0x1c, then +0) must reach the list.
    const auto find = [&](std::uint32_t slot) -> std::uint32_t {
        const auto list = r32(m, kCharacterList);
        w32(m, slot, r32(m, msg + 4));
        const auto found = host.call(m, 0x40b6d0, {list, slot});
        if (!found) return 0;
        for (auto link = r32(m, found + 0x1c); link; link = r32(m, link))
            if (link == list) return found;
        return 0;
    };
    if (find(sp + 0x618)) {
        host.call(m, 0x3fb750, {client});
        const auto old = find(sp + 0x614);
        host.call(m, 0x40b720, {r32(m, kCharacterList), old});
        host.call(m, 0x3cb230, {find(sp + 0x610), 0x3dcccccdU});  // f12 = 0.1
    }
    const float x = rf(m, msg + 8), y = rf(m, msg + 0xc), z = rf(m, msg + 0x10);
    const float cy = game_cosf(y), sy = game_sinf(y);  // 00140e80 / 00140e70
    const float cx = game_cosf(x), sx = game_sinf(x);
    const float cz = game_cosf(z), sz = game_sinf(z);
    const float szcy = sz * cy, szsy = sz * sy, czcy = cz * cy, czsy = cz * sy;
    // 00140e20: the rows (f12 f15 f18 0) (f13 f16 f19 0) (f14 f17 stack 0); the fourth row is the position.
    const auto matrix = sp + 0x5a0;
    const float rows[3][3] = {
        {czcy - szsy * sx, szcy + czsy * sx, -cx * sy},
        {-sz * cx, cz * cx, sx},
        {czsy + szcy * sx, szsy - czcy * sx, cx * cy},
    };
    for (std::uint32_t r = 0; r < 3; ++r) {
        for (std::uint32_t c = 0; c < 3; ++c) wf(m, matrix + r * 16 + c * 4, rows[r][c]);
        w32(m, matrix + r * 16 + 12, 0);
    }
    for (std::uint32_t k = 0; k < 12; k += 4) w32(m, matrix + 0x30 + k, r32(m, msg + 0x14 + k));
    w32(m, matrix + 0x3c, 0x3f800000);
    w32(m, sp + 0x61c, r32(m, msg + 4));
    host.call(m, 0x406ec0, {sp + 0x160, msg + 0x30});
    const auto world = r32(m, r32(m, 0x4ffc0c) + 0x30);
    const auto loader = r32(m, world + 0x3264);
    w32(m, sp + 0x600, r32(m, 0x48c0c8));
    w32(m, sp + 0x604, r32(m, 0x48c0cc));
    heap_enter(m, host);
    auto human = host.call(m, 0x36b180, {0, 0x35a0, 0x4ac940, 0x1af});  // "human.cc"
    heap_leave(m, host);
    if (human) {
        for (std::uint32_t k = 0; k < 0x40; k += 4) w32(m, sp + 0x90 + k, r32(m, matrix + k));
        w32(m, sp, sp + 0x61c);  // sd: the key's address, 0, -1, the descriptor
        w32(m, sp + 4, 0);
        w32(m, sp + 8, 0);
        w32(m, sp + 0xc, 0);
        w32(m, sp + 0x10, 0xffffffffU);
        w32(m, sp + 0x14, 0xffffffffU);
        w32(m, sp + 0x18, sp + 0x160);
        w32(m, sp + 0x1c, 0);
        human = host.call_framed(m, 0x1b8130, {human, sp + 0x90, 0xffffffffU, 0, r32(m, msg + 0x20), sp + 0x600, 3, loader}, sp);
    }
    w8(m, human + 0x14, 0);
    for (std::uint32_t k = 0; k < 6; ++k) {
        const auto weapon = sext16(r16(m, msg + 0x24 + 2 * k));
        if (weapon != 0xffffffffU) host.call(m, 0x143df0, {r32(m, human + 0x3524), weapon, 1, 0, 1, 1});
    }
    if (const auto current = sext16(r16(m, msg + 0x24)); current != 0xffffffffU)
        host.call(m, 0x1439b0, {r32(m, human + 0x3524), current});
    const auto list = r32(m, kCharacterList);
    w32(m, human + 0x1c, list);
    host.call(m, 0x40b790, {list, human});
    const auto health = r32(m, msg + 0x20);
    w32(m, human + 0x3398, health);
    host.call(m, 0x3cc990, {human, health, 0});
    const auto component = r32(m, human + 0x50);
    host.call(m, r32(m, r32(m, component) + 0xc), {component, matrix});
    heap_enter(m, host);
    auto controller = host.call(m, 0x36b180, {0, 0x54, 0x4ac940, 0x1c9});
    heap_leave(m, host);
    if (controller) controller = host.call(m, 0x2d85d0, {controller, 0, human});
    host.call(m, 0x1ae450, {human, 3, 0, controller});
    const auto team = host.call(m, 0x1b8740, {r32(m, r32(m, 0x4ffc0c) + 0x30)});
    host.call(m, 0x1b8750, {human, team & 0xffU});
    host.call(m, r32(m, r32(m, human) + 0x20), {human});
    const auto entry = host.call(m, 0x3fb650, {r32(m, msg + 4), client, human});
    host.call(m, 0x185940, {human, r32(m, msg + 4)});
    host.call(m, 0x185970, {human, entry});
    w8(m, sp + 0xd0, 0);
    host.call(m, 0x2c9270, {sp + 0xd0, human, client, 1});
    host.call(m, 0x242fa0, {r32(m, 0x4ffc2c), human + 0xc, sp + 0xd1, 0, 0, 0, 0xbf800000U, 0});  // f12 -1, f13 0
}

void human_set_key_00185940(LevelLoadMemory& m, std::uint32_t human, std::uint32_t key) {
    w32(m, human + 0x10, key == 0xffffffffU ? r32(m, human + 0xc) : key);
}

void human_set_net_entry_00185970(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t human, std::uint32_t entry) {
    const auto object = r32(m, entry + 0x10);
    if (!object) return;
    w32(m, human + 0x3584, object);
    const auto controller = r32(m, human + 0x58);
    const bool remote = (host.call(m, r32(m, r32(m, controller) + 0x88), {controller}) & 0xffU) == 2;
    const auto value = r32(m, entry + 0x10);
    w32(m, r32(m, human + 0x58) + (remote ? 0x1e0 : 0x44), value);
    const auto data = r32(m, entry + 0x10);
    w32(m, r32(m, human + 0x58) + (remote ? 0x1e4 : 0x48), data ? data + 0x1c : 0);
}

namespace {
// 0013c800(z, root): unlinks z from its red-black tree with the erase fix-up (colour bit 1 = red); root is the
// root's value, kept as a local copy the rotations (0013cf60 / 0013cff0) update.
void rb_unlink_0013c800(LevelLoadMemory& m, std::uint32_t z, std::uint32_t root) {
    const auto parent_of = [&](std::uint32_t n) { return r32(m, n + 8) & ~1U; };
    const auto red = [&](std::uint32_t n) { return (r32(m, n + 8) & 1) != 0; };
    const auto paint = [&](std::uint32_t n, bool r) { w32(m, n + 8, (r32(m, n + 8) & ~1U) | (r ? 1U : 0U)); };
    auto y = z;
    if (r32(m, z) && r32(m, z + 4)) y = net_map_next(m, y);
    auto x = r32(m, y) ? r32(m, y) : r32(m, y + 4);
    auto x_parent = parent_of(y);
    if (x) w32(m, x + 8, x_parent | (r32(m, x + 8) & 1));
    bool x_left = false;
    if (const auto p = parent_of(y); y == r32(m, p)) {
        w32(m, p, x);
        x_left = true;
    } else {
        w32(m, p + 4, x);
    }
    bool fix = !red(y);
    if (y != z) {
        w32(m, y + 8, parent_of(z) | (r32(m, y + 8) & 1));
        const auto pz = parent_of(y);
        w32(m, pz + (z == r32(m, pz) ? 0 : 4), y);
        w32(m, y, r32(m, z));
        if (const auto l = r32(m, y)) w32(m, l + 8, y | (r32(m, l + 8) & 1));
        else x_parent = y;
        w32(m, y + 4, r32(m, z + 4));
        if (const auto r = r32(m, y + 4)) w32(m, r + 8, y | (r32(m, r + 8) & 1));
        else x_parent = y;
        paint(y, red(z));
        if (root == z) root = y;
    } else if (!x && z == root) {
        fix = false;
    }
    if (!fix) return;
    while (x != root && (!x || !red(x))) {
        const auto near = x_left ? 4U : 0U;  // the sibling's side
        auto w = r32(m, x_parent + near);
        if (red(w)) {
            paint(w, false);
            paint(x_parent, true);
            net_map_rotate(m, x_parent, root, x_left);
            w = r32(m, x_parent + near);
        }
        const auto inner = r32(m, w + (x_left ? 0 : 4)), outer = r32(m, w + near);
        if ((!inner || !red(inner)) && (!outer || !red(outer))) {
            paint(w, true);
            x = x_parent;
            x_parent = parent_of(x);
            x_left = x == r32(m, x_parent);
            continue;
        }
        if (!outer || !red(outer)) {
            paint(inner, false);
            paint(w, true);
            net_map_rotate(m, w, root, !x_left);
            w = r32(m, x_parent + near);
        }
        paint(w, red(x_parent));
        paint(x_parent, false);
        paint(r32(m, w + near), false);
        net_map_rotate(m, x_parent, root, x_left);
        x = root;
    }
    if (x) paint(x, false);
}

// 003fc0c0 / 001be870(map, &it): erase the node (the leftmost +0xc moves on first), free it ("allocator.hh" line
// 0x64: 0x004be3a0 / 0x0049ed08) and count - 1.
void rb_map_erase(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t map, std::uint32_t node, std::uint32_t file) {
    if (node == r32(m, map + 0xc)) w32(m, map + 0xc, net_map_next(m, node));
    rb_unlink_0013c800(m, node, r32(m, map + 4));
    if (node) {
        heap_enter(m, host);
        host.call(m, 0x36b130, {0, node, file, 0x64});
        heap_leave(m, host);
    }
    w32(m, map, r32(m, map) - 1);
}

// 003fc200 / 003fc080: the two NetObj red-black trees free their nodes
// left subtree, right subtree, then parent. Unlike a repeated erase, clearing
// does not rebalance or run a NetObj receiver's destructor. The pair stored in
// each node contains only scalar fields and pointers; the pointed-to game
// objects are destroyed later in World_Teardown.
void rb_map_clear_nodes(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t node, std::uint32_t file) {
    if (const auto left = r32(m, node)) rb_map_clear_nodes(m, host, left, file);
    if (const auto right = r32(m, node + 4)) rb_map_clear_nodes(m, host, right, file);
    heap_enter(m, host);
    host.call(m, 0x36b130, {0, node, file, 0x64});
    heap_leave(m, host);
}

void rb_map_clear(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t map, std::uint32_t file) {
    if (const auto root = r32(m, map + 4)) {
        rb_map_clear_nodes(m, host, root, file);
        w32(m, map, 0);
        w32(m, map + 4, 0);
        w32(m, map + 0xc, map + 4);
    }
}

// 003fc000 / 001bb080(&out, map, &key): the node with the key (+0xc, signed), or the end (map + 4).
std::uint32_t rb_map_find(const LevelLoadMemory& m, std::uint32_t map, std::uint32_t key) {
    const auto end = map + 4;
    auto at = end;
    for (auto n = r32(m, map + 4); n;) {
        if (s32(r32(m, n + 0xc)) < s32(key)) {
            n = r32(m, n + 4);
        } else {
            at = n;
            n = r32(m, n);
        }
    }
    if (at == end || s32(key) < s32(r32(m, at + 0xc))) return end;
    return at;
}
}  // namespace

// 003fbcd0, cNetObj_Clear, runs after the end screens and before the level's
// GOBJ destruction. The NetObj and client maps own their tree nodes, not the
// receivers. The NPC/GenInfo constructors make temporary stack objects whose
// clear calls reset the corresponding global stores.
void net_objects_clear_003fbcd0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack) {
    constexpr std::uint32_t kClientMap = 0x55c770;
    const auto frame = stack - 0x30;
    const auto npc_temp = frame + 0x2c;
    const auto gen_temp = frame + 0x28;
    w32(m, 0x55c990, 0);
    if (s32(r32(m, kNetMap)) > 0) rb_map_clear(m, host, kNetMap, 0x49ed08);
    rb_map_clear(m, host, kClientMap, 0x4be3a0);
    host.call(m, 0x436c00, {npc_temp});
    host.call(m, 0x436380, {npc_temp});
    host.call(m, 0x43b190, {gen_temp});
    host.call(m, 0x43ace0, {gen_temp});
    for (std::uint32_t slot = 0; slot < 3; ++slot) {
        const auto base = 0x55c7c0 + slot * 0x98;
        w8(m, base + 0x14, 0);
        w32(m, base + 0x18, 0);
        w16(m, base + 0x94, 0);
        for (std::uint32_t sub = 0; sub < 5; ++sub)
            for (std::uint32_t offset = 0x1c; offset <= 0x30; offset += 4)
                w32(m, base + sub * 0x18 + offset, 0);
    }
    if (const auto npc = r32(m, 0x55c988)) {
        // The global NPC object has its own outer allocator guard. The
        // original reenables interrupts, clears the pointer, then lowers the
        // guard depth; heap_leave would reverse the last two writes.
        heap_enter(m, host);
        host.call(m, 0x436ba0, {npc, 0xffffffffU});
        host.call(m, 0x36b130, {0, npc, 0x4be228, 0x12e});
        if (r8(m, kIntcInstalled)) host.call(m, 0x10dca8, {9});
        w32(m, 0x55c988, 0);
        w32(m, kHeapCounter, r32(m, kHeapCounter) - 1);
    }
    for (std::uint32_t client = 0; client < 4; ++client) w8(m, 0x55c780 + client, 0);
    for (std::uint32_t client = 0; client < 4; ++client) w8(m, 0x493a10 + client, 2);
    for (std::uint32_t offset = 0; offset < 0x14; offset += 4) w32(m, 0x55c9a0 + offset, 0);
    host.call(m, 0x436ba0, {npc_temp, 0xffffffffU});
}

std::uint32_t client_map_remove_003fb750(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t client) {
    constexpr std::uint32_t kClientMap = 0x55c770;
    if (s32(client) >= 4 || s32(client) < 0) return 0;
    const auto node = rb_map_find(m, kClientMap, client);
    if (node == kClientMap + 4) return 0;
    const auto player = r32(m, node + 0x10);
    if (player && r32(m, player + 0xc) == r32(m, node + 0x14)) {
        const auto controller = r32(m, player + 0x58);
        if (!controller || !host.call(m, r32(m, r32(m, controller) + 0x54), {controller})) return 0;
    }
    rb_map_erase(m, host, kClientMap, node, 0x4be3a0);
    return 1;
}

std::uint32_t human_left_check_001b2e90(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t human,
                                        std::uint32_t stack) {
    const auto sp = stack - 0x160;
    const auto controller_answers = [&] {
        const auto controller = r32(m, human + 0x58);
        return controller && host.call(m, r32(m, r32(m, controller) + 0x54), {controller}) != 0;
    };
    if (!controller_answers() || r8(m, human + 0x14)) return 0;
    const auto client = ((r32(m, human + 0xc) & 0x7f000000U) >> 24) - 1;
    if (!r8(m, 0x572418 + client)) return 0;
    std::uint32_t shown = 0;
    if (r8(m, kNetworkGame)) shown = controller_answers() ? client : 0xffffffffU;
    const auto text = host.call(m, 0x3fdb50, {0x49e8b0});  // "PLAYER_LEFTGAME"
    const auto name = host.call(m, 0x12f5d0, {0x4ffb50, shown});
    host.call(m, 0x128bd0, {sp + 0x50, text, name});
    w32(m, sp + 0x40, 0x3f800000U);
    w32(m, sp + 0x4c, 0x3f800000U);
    w32(m, sp + 0x44, 0);
    w32(m, sp + 0x48, 0);
    host.call(m, 0x3fa3f0, {r32(m, 0x4ffc2c) + 0x100, sp + 0x50, sp + 0x40, 1, 0});
    host.call(m, 0x18c770, {human, 3});
    w8(m, 0x55c780 + client, 0);
    // The store at 001b3010 sits after the call's delay slot: it clears +0xb70 of the returned player block.
    w8(m, host.call(m, 0x1314f0, {0x4ffb50, client}) + 0xb70, 0);
    host.call(m, 0x3fb750, {client});
    const auto key = r32(m, human + 0xc);
    w32(m, sp + 0x15c, key);
    if (const auto node = rb_map_find(m, kNetMap, key); node != kNetMap + 4) rb_map_erase(m, host, kNetMap, node, 0x49ed08);
    host.call(m, 0x40b720, {r32(m, kCharacterList), human});
    host.call(m, 0x3cb230, {human, 0x3dcccccdU});  // f12 = 0.1f
    host.call(m, r32(m, r32(m, human) + 0x60), {human, 1});
    if (host_or_solo(m) && r32(m, human + 0x3584)) host.call(m, 0x2ec2a8, {r32(m, r32(m, human + 0x3584) + 0x24)});
    host.call(m, 0x257040, {client});
    return 1;
}

namespace {
// The player input's lazily filled caches of the actor's network block (+0x3584): +0x13c the counter part
// (+0x1c into it, 0 while there is no block), +0x140 the block itself.
std::uint32_t record_counters(LevelLoadMemory& m, std::uint32_t input, std::uint32_t actor) {
    if (!r32(m, input + 0x13c)) {
        const auto block = r32(m, actor + 0x3584);
        w32(m, input + 0x13c, block ? block + 0x1c : 0);
    }
    return r32(m, input + 0x13c);
}
std::uint32_t record_block(LevelLoadMemory& m, std::uint32_t input, std::uint32_t actor) {
    if (!r32(m, input + 0x140)) w32(m, input + 0x140, r32(m, actor + 0x3584));
    return r32(m, input + 0x140);
}
// The held item of an inventory: the slot byte +0x84 (6: nothing, -1) and the slot's +8.
std::uint32_t held_item(const LevelLoadMemory& m, std::uint32_t inventory) {
    const auto slot = r8(m, inventory + 0x84);
    return slot == 6 ? 0xffffffffU : r32(m, inventory + slot * 16 + 8);
}
// The 16 zero bytes 0x004e1d90 (once guard 0x004e1d88, a signed byte) 002d7ed0 / 002d7d10 copy into +0x40.
void zero_vector_once(LevelLoadMemory& m) {
    if (r8(m, 0x4e1d88)) return;
    w32(m, 0x4e1d90, 0);
    w8(m, 0x4e1d88, 1);
    w32(m, 0x4e1d94, 0);
    w32(m, 0x4e1d98, 0);
    w32(m, 0x4e1d9c, 0);
}
// A remote action message on the stack: 003c9e30(msg, 0x0055d480), +0 = 0x004e0840 and the kind's vtable, +0x24
// the action, byte +0x28 the kind; fill writes the fields; 003c9420(msg, human, transport); +0 = 0x004e0840 and
// 003c9d60(msg, 0).
template <class F>
void remote_action(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t human, std::uint32_t action,
                   std::uint32_t kind, std::uint32_t vtable, std::uint32_t transport, F fill) {
    host.call(m, 0x3c9e30, {msg, 0x55d480});
    w32(m, msg, 0x4e0840);
    w32(m, msg + 0x24, action);
    w8(m, msg + 0x28, kind);
    if (vtable) w32(m, msg, vtable);
    fill();
    host.call(m, 0x3c9420, {msg, human, transport});
    w32(m, msg, 0x4e0840);
    host.call(m, 0x3c9d60, {msg, 0});
}
// 0x3b / 0x39 kind 0xb (vtable 0x004e0200): +0x30 the object, +0x34 the subtarget, byte +0x38 = 0, +0x40 the zero
// vector, +0x50 = 0.0f.
void object_fields(LevelLoadMemory& m, std::uint32_t msg, std::uint32_t object, std::uint32_t sub) {
    zero_vector_once(m);
    w32(m, msg + 0x30, object);
    w32(m, msg + 0x34, sub);
    w8(m, msg + 0x38, 0);
    for (std::uint32_t k = 0; k < 16; k += 4) w32(m, msg + 0x40 + k, r32(m, 0x4e1d90 + k));
    w32(m, msg + 0x50, 0);
}
}  // namespace

void net_record_weapon_002493c0(LevelLoadMemory& m, std::uint32_t input, std::uint32_t actor) {
    if (const auto c = record_counters(m, input, actor)) w16(m, c + 6, held_item(m, r32(m, actor + 0x3524)));
}

void net_record_aim_mode_00249420(LevelLoadMemory& m, std::uint32_t input, std::uint32_t actor, std::uint32_t mode) {
    if (const auto c = record_counters(m, input, actor)) w8(m, c + 5, mode);
    net_record_clear_point_00249460(m, input, actor);
}

void net_record_clear_point_00249460(LevelLoadMemory& m, std::uint32_t input, std::uint32_t actor) {
    if (const auto b = record_block(m, input, actor)) w8(m, b, 0);
}

void net_record_point_00249490(LevelLoadMemory& m, std::uint32_t input, std::uint32_t actor, std::uint32_t point) {
    if (const auto b = record_block(m, input, actor)) {
        w8(m, b, 1);
        for (std::uint32_t k = 0; k < 12; k += 4) w32(m, b + 0x10 + k, r32(m, point + k));
    }
}

void net_record_lock_002494e0(LevelLoadMemory& m, std::uint32_t input, std::uint32_t actor, std::uint32_t key,
                              std::uint32_t id, std::uint32_t subtarget) {
    if (const auto b = record_block(m, input, actor)) {
        w32(m, b + 4, key);
        w32(m, b + 8, r32(m, id));
        w8(m, r32(m, input + 0x140) + 0xc, subtarget);
    }
}

void net_record_shot_00249530(LevelLoadMemory& m, std::uint32_t input, std::uint32_t actor) {
    if (const auto c = record_counters(m, input, actor)) w16(m, c, r16(m, c) + 1);
}

std::uint32_t net_counter_take_002c4f20(LevelLoadMemory& m, std::uint32_t counters) {
    if (r8(m, 0x48c0c0)) {
        w16(m, counters + 2, 0);
        w8(m, 0x48c0c0, 0);
    }
    const auto taken = r16(m, counters + 2);
    if (!(taken < r16(m, counters))) return 0;
    w16(m, counters + 2, taken + 1);
    return 1;
}

void remote_fire_002d7d10(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t self, std::uint32_t stack) {
    const auto sp = stack - 0xb0;
    const auto block = r32(m, self + 0x44);
    if (!block) return;
    const auto sub = sext8(r8(m, block + 0xc));
    const auto object = r8(m, block) ? 0xffffffffU : r32(m, block + 8);
    remote_action(m, host, sp + 0x40, r32(m, self + 0x4c), 0x3b, 0xb, 0x4e0200, 0,
                  [&] { object_fields(m, sp + 0x40, object, sub); });
}

void remote_actions_002d7ed0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t self, std::uint32_t target,
                             std::uint32_t stack) {
    const auto sp = stack - 0x220;
    const auto human = [&] { return r32(m, self + 0x4c); };
    const auto block = [&] { return r32(m, self + 0x44); };
    const auto clear_look = [&](std::uint32_t msg) { remote_action(m, host, msg, human(), 0x3f, 0, 0, 1, [] {}); };
    // 0x39 kind 1 (vtable 0x004d9320): byte +0x2c from the byte global.
    const auto lower = [&](std::uint32_t msg, std::uint32_t global) {
        remote_action(m, host, msg, human(), 0x39, 1, 0x4d9320, 1, [&] { w8(m, msg + 0x2c, r8(m, global)); });
    };
    const bool eligible = target && r8(m, target + 0x2d) && r8(m, target + 0x2c) && !r8(m, target + 0x2e);
    if (!block() || (!eligible && !r8(m, block())) || !r32(m, self + 0x48)) {
        clear_look(sp + 0x80);  // path A (0x002d82a8)
        lower(sp + 0x50, 0x48c430);
    } else if (!r8(m, block()) && (!r8(m, r32(m, self + 0x48) + 5) || !target)) {
        clear_look(sp + 0xe0);  // 0x002d81e0
        lower(sp + 0xb0, 0x48c428);
    } else if (!r8(m, block())) {
        remote_action(m, host, sp + 0x1f0, human(), 0x3e, 5, 0x4e01a0, 1,
                      [&] { w32(m, sp + 0x1f0 + 0x2c, r32(m, target + 0xc)); });
        const auto sub = sext8(r8(m, block() + 0xc));
        remote_action(m, host, sp + 0x190, human(), 0x39, 0xb, 0x4e0200, 1,
                      [&] { object_fields(m, sp + 0x190, r32(m, target + 0xc), sub); });
    } else {
        clear_look(sp + 0x160);  // 0x002d80e8
        const auto b = block();
        for (std::uint32_t k = 0; k < 12; k += 4) w32(m, sp + 0x150 + k, r32(m, b + 0x10 + k));
        w32(m, sp + 0x15c, 0x3f800000U);
        remote_action(m, host, sp + 0x110, human(), 0x39, 4, 0x4e0140, 1, [&] {
            for (std::uint32_t k = 0; k < 16; k += 4) w32(m, sp + 0x110 + 0x30 + k, r32(m, sp + 0x150 + k));
        });
    }
    if (const auto counters = r32(m, self + 0x48); counters && net_counter_take_002c4f20(m, counters))
        remote_fire_002d7d10(m, host, self, sp);
}

void remote_controller_update_002d83c0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t self, std::uint32_t stack) {
    const auto sp = stack - 0x40;
    const auto net_block = [&] { return r32(m, r32(m, self + 0x4c) + 0x3584); };
    if (!r32(m, self + 0x44)) {
        if (const auto b = net_block()) w32(m, self + 0x44, b);
    }
    if (!r32(m, self + 0x48)) {
        if (const auto b = net_block()) w32(m, self + 0x48, b + 0x1c);
    }
    if (r32(m, self + 0x44) && r32(m, self + 0x48)) {
        const auto wanted = sext16(r16(m, r32(m, self + 0x48) + 6));
        const auto inventory = r32(m, r32(m, self + 0x4c) + 0x3524);
        if (held_item(m, inventory) != wanted) host.call(m, 0x1439b0, {inventory, wanted});
    }
    std::uint32_t target = 0;
    if (const auto block = r32(m, self + 0x44)) {
        w32(m, sp + 0x3c, r32(m, block + 8));
        if (const auto v = host.call(m, 0x40c540, {sp + 0x3c})) {
            const bool alive = (host.call(m, r32(m, r32(m, v) + 0x108), {v}) & 0xffU) != 3;
            if (alive && r8(m, v + 0x2d) && r8(m, v + 0x2c) && !r8(m, v + 0x2e)) {
                const auto item = held_item(m, r32(m, r32(m, self + 0x4c) + 0x3524));
                host.call(m, r32(m, r32(m, self) + 0x60), {self, v, item, 0xffffffffU});
                target = v;
            }
        }
    }
    remote_actions_002d7ed0(m, host, self, target, sp);
}

std::uint32_t object_base_construct_003cb4b0(LevelLoadMemory& m, std::uint32_t self, std::uint32_t type) {
    w32(m, self, 0x4ddde0);
    w32(m, self + 4, 0xbebaafdeU);
    w32(m, self + 8, type);
    w32(m, self + 0xc, 0xffffffffU);
    w32(m, self + 0x10, 0xffffffffU);
    w8(m, self + 0x14, 1);
    w32(m, self + 0x18, 0);
    return self;
}

std::uint32_t aim_base_construct_003ea630(LevelLoadMemory& m, std::uint32_t self) {
    object_base_construct_003cb4b0(m, self, 0x55a840);
    w32(m, self, 0x4e06c0);
    w32(m, self + 0x2c, 0xfffffffeU);
    w32(m, self + 0x28, 0);
    w32(m, self + 0x30, 0);
    w32(m, self + 0x20, 0);
    w32(m, self + 0x24, 0);
    w32(m, self + 0x28, 0);
    w32(m, self + 0x2c, 0xffffffffU);  // through the stack word -1
    w8(m, self + 0x34, 0);
    return self;
}

std::uint32_t aim_component_construct_00230240(LevelLoadMemory& m, LevelLoadHost&, std::uint32_t self) {
    aim_base_construct_003ea630(m, self);
    w32(m, self, 0x4dba20);
    w32(m, self + 0x38, 0x3f800000U);
    w32(m, self + 0x3c, 0xffffffffU);
    w32(m, self + 0x40, 0);
    return self;
}

std::uint32_t remote_controller_construct_002d85d0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t self,
                                                  std::uint32_t block, std::uint32_t human) {
    aim_component_construct_00230240(m, host, self);
    w32(m, self, 0x4de530);
    w32(m, self + 0x44, block);
    w32(m, self + 0x48, 0);
    w32(m, self + 0x4c, human);
    w16(m, self + 0x50, 0);
    if (const auto h = r32(m, self + 0x4c); h && r32(m, h + 0x3584)) {
        if (!r32(m, self + 0x44)) w32(m, self + 0x44, r32(m, h + 0x3584));
        const auto b = r32(m, r32(m, self + 0x4c) + 0x3584);
        w32(m, self + 0x48, b ? b + 0x1c : 0);
    }
    return self;
}

// ---- cNetHuman, the team data and the GenInfo object's class callbacks (checkpoint 132ln5h) ------------------
namespace {
constexpr std::uint32_t kHumanCreated = 0x51f080, kHumanCount = 0x51f088, kHumanClass = 0x51f078;

// The controller type (virtual +0x88 & 0xff) of the character's controller +0x58: 4 (the remote human's) or 2.
std::uint32_t controller_type(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t controller) {
    return host.call(m, r32(m, r32(m, controller) + 0x88), {controller}) & 0xffU;
}
}  // namespace

void net_human_link_block_00316980(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t data, std::uint32_t character) {
    const auto controller = r32(m, character + 0x58);
    if (!controller) return;
    const auto type = controller_type(m, host, controller);
    if (type == 4) w32(m, r32(m, character + 0x58) + 0x44, data);
    else if (type == 2) w32(m, r32(m, character + 0x58) + 0x1e0, data);
}

void net_human_link_counters_002c4ea0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t counters,
                                      std::uint32_t character) {
    const auto controller = r32(m, character + 0x58);
    if (!controller) return;
    const auto type = controller_type(m, host, controller);
    if (type == 4) w32(m, r32(m, character + 0x58) + 0x48, counters);
    else if (type == 2) w32(m, r32(m, character + 0x58) + 0x1e4, counters);
}

void net_record_init_00316a00(LevelLoadMemory& m, std::uint32_t p) {
    w32(m, p + 8, 0xffffffffU);
    w8(m, p, 0);
    w8(m, p + 1, 0);
    w32(m, p + 4, 0xffffffffU);
    w32(m, p + 8, 0xffffffffU);
    w8(m, p + 0xc, 0xff);
    w32(m, p + 0x10, 0);
    w32(m, p + 0x14, 0);
    w32(m, p + 0x18, 0);
}

std::uint32_t net_record_release_00316a30(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t p, std::uint32_t mode) {
    if (p && static_cast<std::int16_t>(mode & 0xffffU) > 0) host.call(m, 0x100740, {p, mode});
    return p;
}

std::uint32_t net_map_object_003fbb10(LevelLoadMemory& m, std::uint32_t id) {
    const auto node = rb_map_find(m, kNetMap, id);
    if (node == kNetMap + 4 || !r32(m, node + 0x10)) return 0;
    return node + 0x10;
}

void net_human_create_002d1ff0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t, std::uint32_t id,
                               std::uint32_t stack) {
    const auto sp = stack - 0x90, data = sp + 0x30;
    if (!r8(m, kInLevel)) return;
    // PORT-ONLY: the original leaves the block's other bytes (+2, +0xd, +0x1e, +0x23..+0x27, +0x2a, +0x34..,
    // +0x44.., +0x51..) as its frame held them and 002eaf28 copies all 0x58; the port zeroes them.
    for (std::uint32_t k = 0; k < 0x58; k += 4) w32(m, data + k, 0);
    w16(m, sp + 0x60, 4);
    w32(m, sp + 0x34, 0xffffffffU);
    w32(m, sp + 0x68, 0x447a0000U);  // 1000.0f
    w32(m, sp + 0x38, 0xffffffffU);
    w8(m, sp + 0x3c, 0xff);
    w8(m, sp + 0x30, 0);
    w8(m, sp + 0x31, 0);
    w32(m, sp + 0x40, 0);
    w32(m, sp + 0x44, 0);
    w32(m, sp + 0x48, 0);
    w8(m, sp + 0x58, 0);
    w8(m, sp + 0x62, 0);
    w8(m, sp + 0x63, 0);
    w32(m, sp + 0x6c, 0);
    w32(m, sp + 0x70, 0);
    w8(m, sp + 0x80, 0);
    w16(m, sp + 0x52, 0);
    w8(m, sp + 0x51, 0);
    w16(m, sp + 0x4c, 0);
    w8(m, sp + 0x50, 0);
    host.call(m, 0x316a00, {data});
    w32(m, sp + 0x5c, id);
    w8(m, sp + 0x59, 1);
    const auto cls = r32(m, kHumanClass);
    w32(m, sp, 0x4ad588);  // "cNetHuman" (sd: the upper word is 0)
    w32(m, sp + 4, 0);
    w32(m, sp + 8, cls);   // lw then sd: sign-extended
    w32(m, sp + 0xc, (cls & 0x80000000U) ? 0xffffffffU : 0U);
    if (!host.call_framed(m, 0x2eaf28, {sp + 0x8c, 2, 4, 0, 0, 0, 0, data}, sp)) {
        const auto handle = r32(m, sp + 0x8c);
        w8(m, kHumanCreated, 1);
        host.call(m, 0x2ec3c8, {handle});
        w32(m, kHumanCount, r32(m, kHumanCount) + 1);
    }
    host.call(m, 0x316a30, {data, 0});
}

void net_human_owner_002d1dc0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t, std::uint32_t handle,
                              std::uint32_t mode, std::uint32_t stack) {
    const auto sp = stack - 0x50;
    busy_enter(m);
    if (!host.call(m, 0x2eb650, {sp + 0x48, handle})) {
        const auto data = r32(m, r32(m, sp + 0x48) + 0x1c);
        if (mode == 0xfffffffeU) {
            const auto local = local_slot(m, host, r8(m, kNetworkGame), r32(m, kWorld));
            if (r32(m, r32(m, sp + 0x48) + 4) == local && r8(m, kHumanCreated)) {
                w32(m, data + 0x24, handle);
                w8(m, data + 0x28, 1);
                if (r8(m, data + 0x29)) {
                    w32(m, sp + 0x44, r32(m, data + 0x2c));
                    if (const auto character = host.call(m, 0x40c540, {sp + 0x44})) {
                        host.call(m, 0x2c4ea0, {data + 0x1c, character});
                        host.call(m, 0x316980, {data, character});
                        w32(m, character + 0x3584, data);
                    }
                    w8(m, kHumanCreated, 0);
                    w8(m, data + 0x29, 0);
                }
            }
        }
    }
    w8(m, kBusy, 0);
}

void net_human_created_002d1f00(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t master, std::uint32_t handle,
                                std::uint32_t stack) {
    const auto sp = stack - 0x50;
    busy_enter(m);
    if (!host.call(m, 0x2eb650, {sp + 0x48, handle})) {
        const auto data = r32(m, r32(m, sp + 0x48) + 0x1c);
        w8(m, data + 0x29, 0);
        w8(m, data + 0x28, 0);
        w32(m, data + 0x24, handle);
        w32(m, sp + 0x44, r32(m, data + 0x2c));
        if (const auto character = host.call(m, 0x40c540, {sp + 0x44})) {
            host.call(m, 0x2c4ea0, {data + 0x1c, character});
            host.call(m, 0x316980, {data, character});
            w32(m, character + 0x3584, data);
        } else if (const auto slot = host.call(m, 0x3fbb10, {r32(m, data + 0x2c)})) {
            w32(m, slot + 0x10, data);
        } else {
            w32(m, host.call(m, 0x3fb650, {r32(m, data + 0x2c), master, 0}) + 0x10, data);
        }
    }
    w8(m, kBusy, 0);
}

void net_human_removed_002d1d50(LevelLoadMemory& m) {
    busy_enter(m);
    w8(m, kBusy, 0);
}

void net_human_send_002d1d80(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t data) {
    if (r8(m, data + 0x28)) return;
    if (!host.call(m, 0x2ec3c8, {r32(m, data + 0x24)})) w8(m, data + 0x28, 1);
}

void net_human_release_002d2110(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t data, std::uint32_t stack) {
    const auto sp = stack - 0x40;
    bool owner = false;
    if (!host.call(m, 0x2eb650, {sp + 0x3c, r32(m, data + 0x24)}) && r32(m, sp + 0x3c)) {
        const auto local = local_slot(m, host, r8(m, kNetworkGame), r32(m, kWorld));
        const auto who = r32(m, r32(m, sp + 0x3c) + 4);
        owner = who == local || (who == 0xffffffffU && host_or_solo(m));
    }
    if (owner && !host.call(m, 0x2ec460, {r32(m, data + 0x24)})) w8(m, data + 0x28, 0);
}

namespace {
constexpr std::uint32_t kTeamData = 0x4ee620, kTeamDataNet = 0x4ee628;

void heap_free(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t block, std::uint32_t file, std::uint32_t line) {
    heap_enter(m, host);
    host.call(m, 0x36b130, {0, block, file, line});
    heap_leave(m, host);
}
}  // namespace

void team_data_created_0016a580(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t handle, std::uint32_t stack) {
    const auto sp = stack - 0x40;
    busy_enter(m);
    if (!host.call(m, 0x2eb650, {sp + 0x38, handle})) {
        const auto data = r32(m, r32(m, sp + 0x38) + 0x1c);
        if (const auto old = r32(m, kTeamData); old && !r8(m, kTeamDataNet)) heap_free(m, host, old, 0x49c800, 0x1fe);
        w32(m, kTeamData, data);
        w8(m, kTeamDataNet, 1);
    }
    w8(m, kBusy, 0);
}

void team_data_owner_0016a550(LevelLoadMemory& m) {
    busy_enter(m);
    w8(m, kBusy, 0);
}

void team_data_removed_0016a450(LevelLoadMemory& m, LevelLoadHost& host) {
    busy_enter(m);
    if (const auto old = r32(m, kTeamData); old && !r8(m, kTeamDataNet)) heap_free(m, host, old, 0x49c650, 0x6fe);
    w32(m, kTeamData, 0);
    w8(m, kTeamDataNet, 0);
    w8(m, 0x48a100, 1);
    w8(m, kBusy, 0);
}

void geninfo_created_00439560(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t handle, std::uint32_t stack) {
    const auto sp = stack - 0x40;
    busy_enter(m);
    if (!host.call(m, 0x2eb650, {sp + 0x38, handle})) {
        if (const auto old = r32(m, kGenInfoBlock)) heap_free(m, host, old, kGenInfoSource, 0x234);
        w32(m, kGenInfoBlock, r32(m, r32(m, sp + 0x38) + 0x1c));
        w8(m, kGenInfoCreated, 1);
        w32(m, kGenInfoHandle, handle);
    }
    w8(m, kBusy, 0);
}

void geninfo_removed_00438c20(LevelLoadMemory& m) {
    w32(m, kGenInfoBlock, 0);
    w8(m, kGenInfoCreated, 0);
}

namespace {
// GenInfoObject.cc's queues: two std::lists of 8-byte entry copies (+0 count, +4 the header: +0 prev, +4 next;
// 0xc-byte nodes: +0 prev, +4 next, +8 the copy) and a map keyed by the copy's address (unsigned; 0x14-byte
// nodes: +0xc the copy, +0x10 a byte; +4 the root, +0xc the leftmost).
constexpr std::uint32_t kGenInfoAdds = 0x5838b8, kGenInfoOwners = 0x5838d8, kGenInfoInterest = 0x583900;
constexpr std::uint32_t kGenInfoEntries = 0x37;

// The inline lookup of 00439c70 / 004398e0 / ...: the key is read, then the block; no block, no entry.
std::uint32_t geninfo_find(const LevelLoadMemory& m, std::uint32_t keyp) {
    const auto key = r32(m, keyp);
    const auto block = r32(m, kGenInfoBlock);
    if (!block) return 0;
    for (std::uint32_t i = 0; i < kGenInfoEntries; ++i)
        if (r32(m, block + 8 * i) == key) return block + 8 * i;
    return 0;
}
// The drains' lookup in their block argument (no null test) and the first free entry (key -1).
std::uint32_t geninfo_find_in(const LevelLoadMemory& m, std::uint32_t block, std::uint32_t key) {
    for (std::uint32_t i = 0; i < kGenInfoEntries; ++i)
        if (r32(m, block + 8 * i) == key) return block + 8 * i;
    return 0;
}
std::uint32_t geninfo_free_entry(const LevelLoadMemory& m, std::uint32_t block) {
    for (std::uint32_t i = 0; i < kGenInfoEntries; ++i)
        if (r32(m, block + 8 * i) == 0xffffffffU) return block + 8 * i;
    return 0;
}
void geninfo_set_low(LevelLoadMemory& m, std::uint32_t entry, std::uint32_t low) {
    w8(m, entry + 4, (r8(m, entry + 4) & 0xf0U) | (low & 0xfU));
}
// The interest mask (the high nibble) with the bit set / cleared.
void geninfo_set_mask_bit(LevelLoadMemory& m, std::uint32_t entry, std::uint32_t bit) {
    const auto b = r8(m, entry + 4);
    w8(m, entry + 4, (b & 0x0fU) | ((((b >> 4) | (bit & 0xffU)) & 0xfU) << 4));
}
void geninfo_clear_mask_bit(LevelLoadMemory& m, std::uint32_t entry, std::uint32_t bit) {
    const auto b = r8(m, entry + 4);
    w8(m, entry + 4, (b & 0x0fU) | ((((b >> 4) & (~bit & 0xffU)) & 0xfU) << 4));
}
std::uint32_t local_bit(LevelLoadMemory& m, LevelLoadHost& host) {
    return 1U << (local_slot(m, host, r8(m, kNetworkGame), r32(m, kWorld)) & 31U);
}

// A new 8-byte copy {key, low nibble, high nibble 0} (0036b180 under the guard; the fields only when allocated).
std::uint32_t geninfo_new_copy(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t keyp, std::uint32_t low,
                               std::uint32_t line) {
    heap_enter(m, host);
    const auto copy = host.call(m, 0x36b180, {0, 8, kGenInfoSource, line});
    heap_leave(m, host);
    if (copy) {
        w32(m, copy, r32(m, keyp));
        w8(m, copy + 4, low & 0xfU);
    }
    return copy;
}

// 00439c40 -> 00138ea0: push_back (a 0xc-byte node, "list.hh"-style allocator 0x0049ae70 line 0x5c).
void geninfo_list_push(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t list, std::uint32_t data) {
    heap_enter(m, host);
    const auto node = host.call(m, 0x36b180, {0, 0xc, 0x49ae70, 0x5c});
    heap_leave(m, host);
    if (node + 8 != 0) w32(m, node + 8, data);
    const auto header = list + 4;
    w32(m, r32(m, header) + 4, node);
    w32(m, node, r32(m, header));
    w32(m, header, node);
    w32(m, node + 4, header);
    w32(m, list, r32(m, list) + 1);
}
// 00138da0: erase one node (freed, 0x0049ae80 line 0x64, when nonzero).
void geninfo_list_erase(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t list, std::uint32_t node) {
    w32(m, r32(m, node) + 4, r32(m, node + 4));
    w32(m, r32(m, node + 4), r32(m, node));
    if (node) heap_free(m, host, node, 0x49ae80, 0x64);
    w32(m, list, r32(m, list) - 1);
}
// 00138c60(out, list, &first, &last): unlink [first, last), then free each node (when nonzero), count - 1 each.
void geninfo_list_erase_range(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t list, std::uint32_t first,
                              std::uint32_t last) {
    if (first == last) return;
    const auto last_prev = r32(m, last);
    w32(m, r32(m, first) + 4, r32(m, last_prev + 4));
    w32(m, r32(m, last_prev + 4), r32(m, first));
    for (auto node = first; node != last;) {
        const auto next = r32(m, node + 4);
        if (node) heap_free(m, host, node, 0x49ae80, 0x64);
        w32(m, list, r32(m, list) - 1);
        node = next;
    }
}

// 0043b340 -> 0043b6f0: insert {copy, byte} unless the copy's address is already a key (unsigned compares). NPCInfo's
// twin 00436dd0 -> 00437180 is the same code with the node allocated under 0x004bfd00 (checkpoint 132ln5h13).
void geninfo_map_insert(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t map, std::uint32_t copy,
                        std::uint32_t byte, std::uint32_t source = 0x4bfda0) {
    bool left = true, leftmost = true;
    std::uint32_t parent = map + 4, last_right = 0;
    for (auto n = r32(m, map + 4); n;) {
        parent = n;
        if (copy < r32(m, n + 0xc)) {
            n = r32(m, n);
            left = true;
        } else {
            last_right = n;
            n = r32(m, n + 4);
            left = leftmost = false;
        }
    }
    if (last_right && !(r32(m, last_right + 0xc) < copy)) return;
    if (r32(m, map) == 0xffffffffU)
        throw std::runtime_error(source == 0x4bfda0
                                     ? "0043b6f0: the GenInfo interest map's length_error (00121b60) is not ported"
                                     : "00437180: the NPC alive map's length_error (00121b60) is not ported");
    heap_enter(m, host);
    const auto node = host.call(m, 0x36b180, {0, 0x14, source, 0x5c});
    heap_leave(m, host);
    if (node + 0xc != 0) {
        w32(m, node + 0xc, copy);
        w8(m, node + 0x10, byte);
    }
    w32(m, node + 4, 0);
    w32(m, node, 0);
    w32(m, node + 8, parent | (r32(m, node + 8) & 1));
    w32(m, parent + (left ? 0 : 4), node);
    w32(m, map, r32(m, map) + 1);
    net_map_rebalance(m, node, r32(m, map + 4));
    if (leftmost) w32(m, map + 0xc, node);
}
// 0043b410 / 00436c50: both info maps free their nodes left, right, root;
// NPCInfo's allocator source is 004bfd10 rather than GenInfo's 004bfdb0.
void info_map_free_subtree(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t node, std::uint32_t source) {
    if (const auto left = r32(m, node)) info_map_free_subtree(m, host, left, source);
    if (const auto right = r32(m, node + 4)) info_map_free_subtree(m, host, right, source);
    heap_free(m, host, node, source, 0x64);
}
// 0043b1c0 / 00436c50: with a root, every node is freed and the map emptied.
void info_map_clear(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t map, std::uint32_t source) {
    const auto root = r32(m, map + 4);
    if (!root) return;
    info_map_free_subtree(m, host, root, source);
    w32(m, map, 0);
    w32(m, map + 4, 0);
    w32(m, map + 0xc, map + 4);
}

// The queue's send in 004398e0 / 00439d00 / 0043a030 / 0043a420: 002eb650 (result untested) and, with the object
// owned locally (the local slot read twice), 00439390(local, handle, -2) with the caller's $s2.
void geninfo_queue_send(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t out, std::uint32_t s2) {
    host.call(m, 0x2eb650, {out, r32(m, kGenInfoHandle)});
    const auto local = local_slot(m, host, r8(m, kNetworkGame), r32(m, kWorld));
    if (r32(m, r32(m, out) + 4) != local) return;
    const auto again = local_slot(m, host, r8(m, kNetworkGame), r32(m, kWorld));
    host.call(m, 0x439390, {again, r32(m, kGenInfoHandle), 0xfffffffeU, s2});
}
}  // namespace

std::uint32_t geninfo_owner_00439c70(const LevelLoadMemory& m, std::uint32_t keyp) {
    const auto e = geninfo_find(m, keyp);
    if (!e) return 0xffffffffU;
    const auto owner = r8(m, e + 4) & 0xfU;
    return owner == 0xf ? 0xffffffffU : owner;
}

std::uint32_t geninfo_owned_0043aa40(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t keyp) {
    const auto e = geninfo_find(m, keyp);
    if (!e) return 0;
    const auto local = local_slot(m, host, r8(m, kNetworkGame), r32(m, kWorld));
    return (r8(m, e + 4) & 0xfU) == local ? 1U : 0U;
}

std::uint32_t geninfo_claimed_0043ab20(const LevelLoadMemory& m, std::uint32_t keyp) {
    const auto e = geninfo_find(m, keyp);
    return e && (r8(m, e + 4) & 0xfU) != 0xf ? 1U : 0U;
}

std::uint32_t geninfo_interest_0043aba0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t keyp,
                                        std::uint32_t client) {
    if (s32(client) >= 5) return 0;
    const auto e = geninfo_find(m, keyp);
    if (!e) return 0;
    if (client == 0xffffffffU) client = local_slot(m, host, r8(m, kNetworkGame), r32(m, kWorld));
    return ((r8(m, e + 4) >> 4) & (1U << (client & 31U))) != 0 ? 1U : 0U;
}

void geninfo_add_004398e0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t keyp, std::uint32_t owner,
                          std::uint32_t stack) {
    const auto sp = stack - 0x70;
    const auto key = r32(m, keyp);
    const auto block = r32(m, kGenInfoBlock);
    if (block && geninfo_find_in(m, block, key)) return;
    const auto low = s32(owner) < 0 ? 0xfU : owner;
    if (r8(m, kInLevel)) {
        for (auto n = r32(m, kGenInfoAdds + 8); n != kGenInfoAdds + 4; n = r32(m, n + 4))
            if (r32(m, r32(m, n + 8)) == r32(m, keyp)) return;
        const auto copy = geninfo_new_copy(m, host, keyp, low, 0x1ec);
        geninfo_list_push(m, host, kGenInfoAdds, copy);
        geninfo_queue_send(m, host, sp + 0x58, keyp);
        return;
    }
    // Before the level: the block as read above (not tested for null here).
    for (std::uint32_t i = 0; i < kGenInfoEntries; ++i) {
        const auto e = block + 8 * i;
        if (r32(m, e) != 0xffffffffU) continue;
        w32(m, e, key);
        geninfo_set_low(m, e, low);
        return;
    }
}

void geninfo_set_owner_00439d00(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t keyp, std::uint32_t owner,
                                std::uint32_t stack) {
    const auto sp = stack - 0x80;
    const auto e = geninfo_find(m, keyp);
    if (!e) return;
    const auto low = s32(owner) < 0 ? 0xfU : owner;
    if ((r8(m, e + 4) & 0xfU) == low) return;
    for (auto n = r32(m, kGenInfoOwners + 8); n != kGenInfoOwners + 4; n = r32(m, n + 4)) {
        const auto copy = r32(m, n + 8);
        if (r32(m, copy) != r32(m, keyp)) continue;
        if ((r8(m, copy + 4) & 0xfU) != low) geninfo_set_low(m, copy, owner);
        return;
    }
    const auto copy = geninfo_new_copy(m, host, keyp, low, 0x1bb);
    geninfo_list_push(m, host, kGenInfoOwners, copy);
    geninfo_queue_send(m, host, sp + 0x68, owner);
}

void geninfo_clear_interest_0043a030(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t keyp,
                                     std::uint32_t stack, std::uint32_t s2) {
    const auto sp = stack - 0x60;
    const auto e = geninfo_find(m, keyp);
    if (!e) return;
    if (!((r8(m, e + 4) >> 4) & local_bit(m, host))) return;
    for (auto n = r32(m, kGenInfoInterest + 0xc); n != kGenInfoInterest + 4; n = net_map_next(m, n)) {
        if (r32(m, r32(m, n + 0xc)) != r32(m, keyp)) continue;
        w8(m, n + 0x10, 0);
        return;
    }
    geninfo_clear_mask_bit(m, e, local_bit(m, host));
    const auto copy = geninfo_new_copy(m, host, keyp, 0xf, 0x196);
    geninfo_map_insert(m, host, kGenInfoInterest, copy, 0);  // 0x00497588: false
    geninfo_queue_send(m, host, sp + 0x4c, s2);
}

void geninfo_set_interest_0043a420(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t keyp,
                                   std::uint32_t stack, std::uint32_t s2) {
    const auto sp = stack - 0x60;
    const auto e = geninfo_find(m, keyp);
    if (!e) return;
    if ((r8(m, e + 4) >> 4) & local_bit(m, host)) return;
    for (auto n = r32(m, kGenInfoInterest + 0xc); n != kGenInfoInterest + 4; n = net_map_next(m, n)) {
        if (r32(m, r32(m, n + 0xc)) != r32(m, keyp)) continue;
        w8(m, n + 0x10, 1);
        return;
    }
    geninfo_set_mask_bit(m, e, local_bit(m, host));
    const auto copy = geninfo_new_copy(m, host, keyp, 0xf, 0x173);
    geninfo_map_insert(m, host, kGenInfoInterest, copy, 1);  // 0x00497580: true
    geninfo_queue_send(m, host, sp + 0x4c, s2);
}

void geninfo_apply_owners_00438c40(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t block) {
    for (auto n = r32(m, kGenInfoOwners + 8); n != kGenInfoOwners + 4;) {
        const auto copy = r32(m, n + 8);
        const auto next = r32(m, n + 4);
        if (const auto e = geninfo_find_in(m, block, r32(m, copy))) geninfo_set_low(m, e, r8(m, copy + 4));
        heap_free(m, host, copy, kGenInfoSource, 0x32a);
        geninfo_list_erase(m, host, kGenInfoOwners, n);
        n = next;
    }
}

void geninfo_apply_adds_00438e20(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t block, std::uint32_t s2,
                                 std::uint32_t stack) {
    const auto sp = stack - 0x90;
    for (auto n = r32(m, kGenInfoAdds + 8); n != kGenInfoAdds + 4;) {
        const auto copy = r32(m, n + 8);
        const auto next = r32(m, n + 4);
        if (const auto e = geninfo_find_in(m, block, r32(m, copy))) s2 = e;
        if (!s2) {
            host.call(m, 0x43b190, {sp + 0x8c});
            const auto low = r8(m, copy + 4) & 0xfU;
            const auto key = r32(m, copy);
            if (const auto e = geninfo_free_entry(m, block)) {
                w32(m, e, key);
                geninfo_set_low(m, e, low);
            }
        }
        heap_free(m, host, copy, kGenInfoSource, 0x2fd);
        geninfo_list_erase(m, host, kGenInfoAdds, n);
        n = next;
    }
}

void geninfo_apply_interest_00439030(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t block) {
    std::uint32_t found = 0;  // $s3: kept from an earlier copy when a later key is not in the block
    for (auto n = r32(m, kGenInfoInterest + 0xc); n != kGenInfoInterest + 4;) {
        const auto copy = r32(m, n + 0xc);
        const auto set = r8(m, n + 0x10);
        const auto next = net_map_next(m, n);
        if (const auto e = geninfo_find_in(m, block, r32(m, copy))) found = e;
        if (found) {
            if (set) {
                geninfo_set_mask_bit(m, found, local_bit(m, host));
            } else if ((r8(m, found + 4) >> 4) & local_bit(m, host)) {
                geninfo_clear_mask_bit(m, found, local_bit(m, host));
            }
        }
        heap_free(m, host, copy, kGenInfoSource, 0x2b6);
        rb_map_erase(m, host, kGenInfoInterest, n, 0x4bfdb0);
        n = next;
    }
    info_map_clear(m, host, kGenInfoInterest, 0x4bfdb0);
}

void geninfo_reset_0043ace0(LevelLoadMemory& m, LevelLoadHost& host) {
    if (const auto block = r32(m, kGenInfoBlock); block && !r8(m, kGenInfoCreated))
        heap_free(m, host, block, kGenInfoSource, 0x2b);
    w8(m, kGenInfoCreated, 0);
    w32(m, kGenInfoBlock, 0);
    w32(m, kGenInfoHandle, 0xffffffffU);
    w8(m, 0x583890, 0);
    w32(m, 0x583898, 0);
    w32(m, 0x583910, 0);
    for (const auto [list, line] : {std::pair{kGenInfoAdds, 0x38U}, std::pair{kGenInfoOwners, 0x41U}}) {
        for (auto n = r32(m, list + 8); n != list + 4; n = r32(m, n + 4))
            heap_free(m, host, r32(m, n + 8), kGenInfoSource, line);
        geninfo_list_erase_range(m, host, list, r32(m, list + 8), list + 4);
    }
    for (auto n = r32(m, kGenInfoInterest + 0xc); n != kGenInfoInterest + 4; n = net_map_next(m, n))
        heap_free(m, host, r32(m, n + 0xc), kGenInfoSource, 0x4c);
    info_map_clear(m, host, kGenInfoInterest, 0x4bfdb0);
    for (std::uint32_t client = 0; client < 4; ++client) w8(m, 0x5838a0 + client, 0);
}

void npc_info_created_00433160(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t handle, std::uint32_t stack) {
    const auto sp = stack - 0x60;
    busy_enter(m);
    if (!host.call(m, 0x2eb650, {sp + 0x58, handle})) {
        const auto data = [&] { return r32(m, r32(m, sp + 0x58) + 0x1c); };
        if (!r8(m, kNpcReleased)) {
            const auto old = r32(m, kNpcBlocks);
            w32(m, kNpcBlocks, data());
            w8(m, kNpcCreated, 1);
            w8(m, kNpcReleased, 1);
            const auto host_side = r8(m, kNetworkGame) ? r8(m, kHostRole) : 1U;
            if (!host_side && old) {
                // 00433218..004332d4: retain the joiner's local entry after the
                // replicated block replaces slot 0. The two calls use the same
                // stack key and original owner byte before the old block is freed.
                w32(m, 0x582a68, r32(m, 0x582a68) + 1);
                w32(m, sp + 0x50, r32(m, old));
                host.call(m, 0x435f30, {sp + 0x54, sp + 0x50, r8(m, old + 4)});
                host.call(m, 0x434600, {sp + 0x54, sp + 0x50, r8(m, old + 4)});
                heap_free(m, host, old, kNpcSource, 0x620);
                w32(m, 0x582a68, r32(m, 0x582a68) - 1);
            }
            w32(m, kNpcHandles, handle);
        } else {
            static constexpr std::uint32_t line[5] = {0x620, 0x629, 0x635, 0x640, 0x64a};
            std::uint32_t k = 1;
            while (k < 4 && r8(m, kNpcReleased + 8 * k)) ++k;
            if (const auto old = r32(m, kNpcBlocks + 8 * k)) heap_free(m, host, old, kNpcSource, line[k]);
            w32(m, kNpcBlocks + 8 * k, data());
            w32(m, kNpcHandles + 8 * k, handle);
            if (k < 4) w8(m, kNpcReleased + 8 * k, 1);
        }
    }
    w8(m, kBusy, 0);
}

// ---- NPCInfoObject.cc's queues and the owner's drains (checkpoint 132ln5h12; NET_INLEVEL.md 4.22) ----------------
namespace {
// The queued 0x14-byte entry copies: +0 the NPC's +c, +4..+0xd the entry's bytes, +0x10 the NPC info object's handle.
constexpr std::uint32_t kNpcAdds = 0x582af0, kNpcInterestSet = 0x582ab0, kNpcInterestClear = 0x582ad0;
constexpr std::uint32_t kNpcOwners = 0x582b10, kNpcOwnerChanges = 0x582b30, kNpcAlive = 0x582b50;
constexpr std::uint32_t kNpcEntries = 0x1d;

// The drains' inline lookup in their block argument (no null test): the first entry with the copy's +0.
std::uint32_t npc_entry_in(const LevelLoadMemory& m, std::uint32_t data, std::uint32_t id) {
    for (std::uint32_t i = 0; i < kNpcEntries; ++i)
        if (r32(m, data + 16 * i) == id) return data + 16 * i;
    return 0;
}
std::uint32_t local_of(LevelLoadMemory& m, LevelLoadHost& host) {
    return local_slot(m, host, r8(m, kNetworkGame), r32(m, kWorld));
}
// The common drain over a queue: each copy with the handle (+0x10) applied to its entry (when found), freed and
// erased (found or not); `next` is read before the body.
template <class Apply>
void npc_drain(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t list, std::uint32_t data, std::uint32_t handle,
               std::uint32_t line, Apply apply) {
    for (auto node = r32(m, list + 8); node != list + 4;) {
        const auto copy = r32(m, node + 8);
        const auto next = r32(m, node + 4);
        if (r32(m, copy + 0x10) == handle) {
            if (const auto entry = npc_entry_in(m, data, r32(m, copy))) apply(entry, copy);
            heap_free(m, host, copy, kNpcSource, line);
            geninfo_list_erase(m, host, list, node);
        }
        node = next;
    }
}
}  // namespace

void npc_info_drain_adds_00432b10(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t data, std::uint32_t stack) {
    const auto sp = stack - 0x80;
    for (auto node = r32(m, kNpcAdds + 8); node != kNpcAdds + 4;) {
        const auto copy = r32(m, node + 8);
        const auto next = r32(m, node + 4);
        w32(m, sp + 0x7c, r32(m, copy));
        if (!host.call(m, 0x4361c0, {sp + 0x7c, 0})) {
            if (const auto entry = npc_entry_in(m, data, 0xffffffffU)) {
                w32(m, entry, r32(m, copy));
                for (std::uint32_t b = 4; b < 0xa; ++b) w8(m, entry + b, r8(m, copy + b));
                heap_free(m, host, copy, kNpcSource, 0x6ec);
                geninfo_list_erase(m, host, kNpcAdds, node);
            }
        }
        node = next;
    }
}

void npc_info_drain_interest_set_00432f50(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t data,
                                          std::uint32_t handle) {
    npc_drain(m, host, kNpcInterestSet, data, handle, 0x683,
              [&](std::uint32_t entry, std::uint32_t) { w8(m, entry + 6 + local_of(m, host), 1); });
}

void npc_info_drain_interest_clear_00432910(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t data,
                                            std::uint32_t handle) {
    npc_drain(m, host, kNpcInterestClear, data, handle, 0x71e,
              [&](std::uint32_t entry, std::uint32_t) { w8(m, entry + 6 + local_of(m, host), 0); });
}

void npc_info_drain_owners_00432750(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t data, std::uint32_t handle) {
    npc_drain(m, host, kNpcOwners, data, handle, 0x74f, [&](std::uint32_t entry, std::uint32_t copy) {
        w8(m, entry + 4, r8(m, copy + 4));
        w8(m, entry + 5, r8(m, copy + 5));
    });
}

void npc_info_drain_alive_00432d00(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t data, std::uint32_t handle) {
    for (auto node = r32(m, kNpcAlive + 0xc); node != kNpcAlive + 4;) {
        const auto copy = r32(m, node + 0xc);
        const auto value = r8(m, node + 0x10);
        const auto at = node;
        node = net_map_next(m, node);
        if (r32(m, copy + 0x10) != handle) continue;
        if (const auto entry = npc_entry_in(m, data, r32(m, copy))) w8(m, entry + 0xa + local_of(m, host), value);
        heap_free(m, host, copy, kNpcSource, 0x6b6);
        rb_map_erase(m, host, kNpcAlive, at, 0x4bfd10);
    }
}

void npc_info_drain_owner_changes_00432510(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t data,
                                           std::uint32_t handle) {
    if (!(r8(m, kNetworkGame) ? r8(m, kHostRole) : 1U) || !r32(m, kNpcOwnerChanges)) return;
    npc_drain(m, host, kNpcOwnerChanges, data, handle, 0x78b, [&](std::uint32_t entry, std::uint32_t copy) {
        if (r8(m, entry + 4) != 0xffU) {
            w8(m, entry + 4, r8(m, copy + 4));
            w8(m, entry + 5, 0xff);
        } else {
            w8(m, entry + 5, r8(m, copy + 4));
        }
        w8(m, entry + 6 + r8(m, copy + 5), 0);
    });
}

void npc_info_owner_00432230(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t handle, std::uint32_t mode,
                             std::uint32_t stack) {
    const auto sp = stack - 0x50;
    w32(m, sp + 0x4c, 0);
    const auto r = host.call(m, 0x2eb650, {sp + 0x4c, handle});
    if (!r32(m, kNpcBlocks + 8) || !r32(m, sp + 0x4c) || r) return;
    if (mode != 0xfffffffeU) return;
    if (r32(m, r32(m, sp + 0x4c) + 4) != local_of(m, host)) return;
    const auto data = r32(m, r32(m, sp + 0x4c) + 0x1c);
    if (!r8(m, kNpcCreated)) host.call(m, 0x4355d0, {sp + 0x48});
    w32(m, 0x582a68, r32(m, 0x582a68) + 1);
    for (const auto drain : {0x432b10U, 0x432f50U, 0x432910U, 0x432750U, 0x432d00U, 0x432510U})
        host.call(m, drain, {sp + 0x48, data, handle});
    // The entries the local console owns whose character is gone (no +58 controller or its +44 false) are freed.
    for (std::uint32_t e = 0; e < kNpcEntries; ++e) {
        const auto entry = data + 16 * e;
        if (r32(m, entry) == 0xffffffffU) continue;
        if (r8(m, entry + 4) != local_of(m, host)) continue;
        w32(m, sp + 0x40, r32(m, entry));
        const auto character = host.call(m, 0x40c540, {sp + 0x40});
        if (!character) continue;
        const auto controller = r32(m, character + 0x58);
        if (!controller || host.call(m, r32(m, r32(m, controller) + 0x44), {controller})) continue;
        w32(m, sp + 0x44, r32(m, entry));
        if (const auto found = host.call(m, 0x4361c0, {sp + 0x44, 0})) {
            w32(m, found, 0xffffffffU);
            w8(m, found + 4, 0xff);
            for (std::uint32_t b = 6; b < 0xa; ++b) w8(m, found + b, 0);
        }
    }
    for (std::uint32_t k = 0; k < 5; ++k)
        if (handle == r32(m, kNpcHandles + 8 * k)) w8(m, kNpcDirty + 8 * k, 1);
    w32(m, 0x582a68, r32(m, 0x582a68) - 1);
}

namespace {
// 00434aa0 / 00435070's 0x14-byte copy ("NPCInfoObject.cc"; the fields only when allocated).
std::uint32_t npc_copy(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t src, std::uint32_t line) {
    heap_enter(m, host);
    const auto copy = host.call(m, 0x36b180, {0, 0x14, kNpcSource, line});
    heap_leave(m, host);
    if (copy) {
        w32(m, copy, r32(m, src));
        for (std::uint32_t b = 4; b < 0xe; ++b) w8(m, copy + b, r8(m, src + b));
        w32(m, copy + 0x10, r32(m, src + 0x10));
    }
    return copy;
}
}  // namespace

void npc_info_queue_add_00434aa0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t src) {
    geninfo_list_push(m, host, kNpcAdds, npc_copy(m, host, src, 0x30b));
}

void npc_info_queue_owner_00434d10(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t src) {
    geninfo_list_push(m, host, kNpcOwners, npc_copy(m, host, src, 0x2f4));
}

void npc_info_set_owner_00434600(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t a0, std::uint32_t idp,
                                 std::uint32_t owner, std::uint32_t stack) {
    const auto sp = stack - 0x60;
    if (!r8(m, kNetworkGame)) return;
    host.call(m, 0x4355d0, {a0});
    if (r8(m, kNetworkGame) && !r8(m, kHostRole)) return;
    const auto found = host.call(m, 0x4361c0, {idp, sp + 0x5c});
    if (s32(owner) < 0) owner = 0xff;
    if (!found || r8(m, found + 4) == owner) return;
    const auto c = sp + 0x40;
    w8(m, c + 4, r8(m, found + 4));
    w32(m, c, r32(m, found));
    for (std::uint32_t b = 5; b < 0xe; ++b) w8(m, c + b, r8(m, found + b));
    w8(m, c + 5, r8(m, found + 4));
    w8(m, c + 4, owner);
    w32(m, c + 0x10, r32(m, sp + 0x5c));
    w8(m, found + 5, r8(m, found + 4));
    w8(m, found + 4, owner);
    host.call(m, 0x434d10, {a0, c});
    host.call(m, 0x434920, {a0, r32(m, sp + 0x5c)});
}

void npc_info_set_owner_key_004344a0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t a0, std::uint32_t key,
                                     std::uint32_t owner, std::uint32_t stack) {
    const auto keyp = stack - 0x20 + 0x1c;
    w32(m, keyp, key);
    host.call(m, 0x434600, {a0, keyp, owner});
}

void npc_info_queue_interest_00435070(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t src) {
    geninfo_list_push(m, host, kNpcInterestSet, npc_copy(m, host, src, 0x2b5));
    for (auto node = r32(m, kNpcInterestClear + 8); node != kNpcInterestClear + 4;) {
        const auto copy = r32(m, node + 8);
        const auto next = r32(m, node + 4);
        if (r32(m, copy) == r32(m, src)) {
            heap_free(m, host, copy, kNpcSource, 0x2c6);
            geninfo_list_erase(m, host, kNpcInterestClear, node);
        }
        node = next;
    }
}

void npc_info_queue_uninterest_00434e20(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t src) {
    geninfo_list_push(m, host, kNpcInterestClear, npc_copy(m, host, src, 0x2d5));
    for (auto node = r32(m, kNpcInterestSet + 8); node != kNpcInterestSet + 4;) {
        const auto copy = r32(m, node + 8);
        const auto next = r32(m, node + 4);
        if (r32(m, copy) == r32(m, src)) {
            heap_free(m, host, copy, kNpcSource, 0x2e6);
            geninfo_list_erase(m, host, kNpcInterestSet, node);
        }
        node = next;
    }
}

void npc_info_flush_00434920(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t handle, std::uint32_t stack) {
    const auto sp = stack - 0x30;
    host.call(m, 0x2eb650, {sp + 0x2c, handle});
    const auto object = r32(m, sp + 0x2c);
    if (object && r32(m, object + 4) != local_of(m, host)) {
        for (std::uint32_t k = 0; k < 5; ++k)
            if (handle == r32(m, kNpcHandles + 8 * k)) {
                w8(m, kNpcSending + 8 * k, 1);
                break;
            }
        return;
    }
    host.call(m, 0x432230, {local_of(m, host), handle, 0xfffffffeU});
}

void npc_info_add_00435f30(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t a0, std::uint32_t idp,
                           std::uint32_t client, std::uint32_t stack) {
    const auto sp = stack - 0x60;
    host.call(m, 0x4355d0, {a0});
    if (r8(m, kNpcReleased)) {
        const auto e = sp + 0x40;
        w32(m, e, r32(m, idp));
        w8(m, e + 4, client);
        w8(m, e + 5, 0xff);
        for (std::uint32_t b = 6; b < 0xa; ++b) w8(m, e + b, 0);
        for (std::uint32_t b = 0xa; b < 0xe; ++b) w8(m, e + b, 1);
        w32(m, e + 0x10, 0);
        host.call(m, 0x434aa0, {a0, e});
        for (std::uint32_t k = 0; k < 5; ++k) host.call(m, 0x434920, {a0, r32(m, kNpcHandles + 8 * k)});
        return;
    }
    if (const auto found = host.call(m, 0x4361c0, {idp, 0})) {
        w32(m, found, r32(m, idp));
        return;
    }
    for (std::uint32_t k = 0; k < 5; ++k)
        if (const auto entry = npc_entry_in(m, r32(m, kNpcBlocks + 8 * k), 0xffffffffU)) {
            w32(m, entry, r32(m, idp));
            return;
        }
}

std::uint32_t npc_info_owner_of_00434440(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t a0, std::uint32_t idp) {
    host.call(m, 0x4355d0, {a0});
    const auto found = host.call(m, 0x4361c0, {idp, 0});
    return found && r8(m, found + 4) != 0xffU ? r8(m, found + 4) : 0xffffffffU;
}

std::uint32_t npc_info_next_owner_00434380(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t a0, std::uint32_t idp) {
    host.call(m, 0x4355d0, {a0});
    const auto found = host.call(m, 0x4361c0, {idp, 0});
    return found && r8(m, found + 5) != 0xffU ? r8(m, found + 5) : 0xffffffffU;
}

std::uint32_t npc_info_others_interested_004340c0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t idp) {
    std::uint32_t result = 0;
    if (const auto found = host.call(m, 0x4361c0, {idp, 0}))
        for (std::uint32_t s = 0; s < 4; ++s)
            if (s != local_of(m, host) && r8(m, found + 6 + s)) result = 1;
    return result;
}

std::uint32_t npc_info_interested_00434190(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t idp, std::uint32_t s) {
    const auto found = host.call(m, 0x4361c0, {idp, 0});
    return found && s < 4 ? r8(m, found + 6 + s) : 0;
}

std::uint32_t npc_info_contact_00433ec0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t idp, std::uint32_t s) {
    const auto found = host.call(m, 0x4361c0, {idp, 0});
    return found && s < 4 ? r8(m, found + 0xa + s) : 0;
}

void npc_info_queue_alive_00434be0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t src, std::uint32_t value,
                                   std::uint32_t stack) {
    const auto sp = stack - 0x60;
    const auto copy = npc_copy(m, host, src, 0x300);
    w32(m, sp + 0x48, copy);
    w8(m, sp + 0x4c, value);
    geninfo_map_insert(m, host, kNpcAlive, copy, value & 0xff, 0x4bfd00);
}

void npc_info_set_contact_00433f20(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t a0, std::uint32_t idp,
                                   std::uint32_t value, std::uint32_t stack) {
    const auto sp = stack - 0x70;
    if (!r8(m, kNetworkGame)) return;
    host.call(m, 0x4355d0, {a0});
    const auto found = host.call(m, 0x4361c0, {idp, sp + 0x6c});
    if (!found) return;
    const auto c = sp + 0x50;
    w8(m, c + 4, r8(m, found + 4));
    w32(m, c, r32(m, found));
    for (std::uint32_t b = 5; b < 0xe; ++b) w8(m, c + b, r8(m, found + b));
    w8(m, c + 0xa + local_of(m, host), value);
    w32(m, c + 0x10, r32(m, sp + 0x6c));
    w8(m, found + 0xa + local_of(m, host), value);
    host.call(m, 0x434be0, {a0, c, value & 0xff});
    host.call(m, 0x434920, {a0, r32(m, sp + 0x6c)});
}

void npc_info_removed_004321d0(LevelLoadMemory& m) {
    for (std::uint32_t k = 0; k < 5; ++k) w32(m, kNpcBlocks + 8 * k, 0);
    for (std::uint32_t k = 0; k < 5; ++k) w8(m, kNpcCreated + 8 * k, 0);
}

// 00436380: the five NPCInfo queues and one alive tree are cleared before
// the five block pointers and module state. This is only the Clear body;
// 003fbcd0's surrounding NPCInfo ctor/dtor own the shared-reference count.
void npc_info_clear_00436380(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t self) {
    (void)self;  // 00433760's DestroyNPCInfoStruct ignores the temporary object.
    const auto clear_list = [&](std::uint32_t list, std::uint32_t line) {
        for (auto node = r32(m, list + 8); node != list + 4; node = r32(m, node + 4))
            heap_free(m, host, r32(m, node + 8), kNpcSource, line);
        geninfo_list_erase_range(m, host, list, r32(m, list + 8), list + 4);
    };
    clear_list(kNpcInterestSet, 0x7b);
    clear_list(kNpcInterestClear, 0x86);
    clear_list(kNpcAdds, 0x91);
    for (auto node = r32(m, kNpcAlive + 0xc); node != kNpcAlive + 4; node = net_map_next(m, node))
        heap_free(m, host, r32(m, node + 0xc), kNpcSource, 0x9c);
    info_map_clear(m, host, kNpcAlive, 0x4bfd10);
    clear_list(kNpcOwnerChanges, 0xa7);
    clear_list(kNpcOwners, 0xb1);

    // 00433760: when block 0 is not owned by the DME object, free every
    // non-null block in order, then clear all five pointers. Otherwise the
    // blocks belong to the DME object and only the pointers are cleared.
    if (!r8(m, kNpcReleased)) {
        for (std::uint32_t k = 0; k < 5; ++k)
            if (const auto block = r32(m, kNpcBlocks + 8 * k))
                heap_free(m, host, block, kNpcSource, 0x5aa + 3 * k);
    }
    for (std::uint32_t k = 0; k < 5; ++k) w32(m, kNpcBlocks + 8 * k, 0);
    for (std::uint32_t k = 0; k < 5; ++k) w8(m, kNpcCreated + 8 * k, 0);
    for (std::uint32_t k = 0; k < 5; ++k) w32(m, kNpcTimers + 8 * k, 0);
}

namespace {
constexpr std::uint32_t kHeadsetHandle = 0x583870, kHeadsetData = 0x582cf0, kHeadsetDevice = 0x497548;
}  // namespace

void headset_created_00437830(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t handle, std::uint32_t stack) {
    const auto sp = stack - 0x30;
    busy_enter(m);
    if (!host.call(m, 0x2eb650, {sp + 0x28, handle})) {
        const auto data = r32(m, r32(m, sp + 0x28) + 0x1c);
        w32(m, kHeadsetHandle, handle);
        w32(m, kHeadsetData, data);
    }
    w8(m, kBusy, 0);
}

void headset_owner_00437640(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t handle, std::uint32_t mode,
                            std::uint32_t stack) {
    const auto sp = stack - 0x30;
    busy_enter(m);
    const auto local = [&] { return local_slot(m, host, r8(m, kNetworkGame), r32(m, kWorld)); };
    if (!host.call(m, 0x2eb650, {sp + 0x28, handle}) && mode == 0xfffffffeU &&
        r32(m, r32(m, sp + 0x28) + 4) == local()) {
        if (r8(m, 0x583860)) {
            if (r8(m, 0x582b88)) {
                const auto r = host.call(m, 0x325960, {r32(m, kHeadsetDevice)});
                if (!r) w8(m, 0x583050, 1);
                else if (r == 0x80000006U) w8(m, 0x582b88, 0);
            }
            w8(m, 0x583860, 0);
            w8(m, 0x582d20, 1);
        } else {
            host.call(m, 0x2eb650, {sp + 0x24, r32(m, kHeadsetHandle)});
            const auto object = r32(m, sp + 0x24);
            const auto owner = object ? r32(m, object + 4) : 0xffffffffU;
            if (owner == local()) {
                host.call(m, 0x2ec460, {r32(m, kHeadsetHandle)});
                w8(m, 0x582d28, 1);
            }
            if (r8(m, 0x582b88)) {
                if (host.call(m, 0x325a28, {r32(m, kHeadsetDevice)}) == 0x80000006U) w8(m, 0x582b88, 0);
                w32(m, 0x582d08, host.call(m, 0x42b1a0, {1}) + 0x7d0);
            }
            w8(m, 0x583860, 0);
        }
    }
    w8(m, kBusy, 0);
}

void headset_removed_004375d0(LevelLoadMemory& m) {
    busy_enter(m);
    w32(m, kHeadsetData, 0);
    w8(m, kBusy, 0);
}

void net_map_set_client_003fb4b0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t key, std::uint32_t client,
                                 std::uint32_t stack) {
    const auto sp = stack - 0x60;
    const auto slot = host.call(m, 0x3fbb10, {key});
    w32(m, sp + 0x5c, key);
    const auto character = host.call(m, 0x40c540, {sp + 0x5c});
    if (!slot) return;
    if (character && r32(m, character + 0x58) && r8(m, character + 0x49) == 2) {
        const auto controller = r32(m, character + 0x58);
        bool npc = (host.call(m, r32(m, r32(m, controller) + 0x88), {controller}) & 0xffU) == 2;
        if (!npc) {
            const auto again = r32(m, character + 0x58);
            npc = (host.call(m, r32(m, r32(m, again) + 0x88), {again}) & 0xffU) == 1;
        }
        if (npc) {  // 132ln5h12: the NPC info object (made once: NetObj.cc 0x20c, 00436c00) takes the owner
            constexpr std::uint32_t kNpcInfoObj = 0x55c988;
            if (!r32(m, kNpcInfoObj)) {
                heap_enter(m, host);
                auto made = host.call(m, 0x36b180, {0, 1, 0x4be228, 0x20c});
                heap_leave(m, host);
                if (made) made = host.call(m, 0x436c00, {made});
                w32(m, kNpcInfoObj, made);
            }
            host.call(m, 0x4344a0, {r32(m, kNpcInfoObj), key, client});
            return;
        }
    }
    w32(m, slot + 4, client);
}

std::uint32_t npc_info_find_004361c0(LevelLoadMemory& m, std::uint32_t idp, std::uint32_t out) {
    std::uint32_t first[5];
    for (std::uint32_t k = 0; k < 5; ++k) first[k] = r32(m, kNpcBlocks + 8 * k);
    for (std::uint32_t e = 0; e < 0x1d; ++e)
        for (std::uint32_t k = 0; k < 5; ++k) {
            if (!first[k] || r32(m, first[k] + 16 * e) != r32(m, idp)) continue;
            if (out) w32(m, out, r32(m, kNpcHandles + 8 * k));
            return r32(m, kNpcBlocks + 8 * k) + 16 * e;
        }
    if (out) w32(m, out, 0xffffffffU);
    return 0;
}

void npc_info_apply_004341f0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t a0, std::uint32_t idp,
                             std::uint32_t stack) {
    const auto sp = stack - 0x60;
    if (!r8(m, kNetworkGame)) return;
    host.call(m, 0x4355d0, {a0});
    const auto found = host.call(m, 0x4361c0, {idp, sp + 0x5c});
    if (!found) return;
    // The entry copied to sp+40 with the local client's interest byte set, in the entry as well.
    w8(m, sp + 0x44, r8(m, found + 4));
    w32(m, sp + 0x40, r32(m, found));
    w8(m, sp + 0x45, r8(m, found + 5));
    for (std::uint32_t b = 0; b < 4; ++b) {
        w8(m, sp + 0x46 + b, r8(m, found + 6 + b));
        w8(m, sp + 0x4a + b, r8(m, found + 0xa + b));
    }
    w8(m, sp + 0x46 + local_of(m, host), 1);
    w8(m, found + 6 + local_of(m, host), 1);
    w32(m, sp + 0x50, r32(m, sp + 0x5c));
    host.call(m, 0x435070, {a0, sp + 0x40});
    host.call(m, 0x434920, {a0, r32(m, sp + 0x5c)});
}

void npc_info_clear_interest_00433ce0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t a0, std::uint32_t idp,
                                      std::uint32_t stack) {
    const auto sp = stack - 0x60;
    host.call(m, 0x4355d0, {a0});
    const auto found = host.call(m, 0x4361c0, {idp, sp + 0x5c});
    if (!found) return;
    w8(m, sp + 0x44, r8(m, found + 4));
    w32(m, sp + 0x40, r32(m, found));
    w8(m, sp + 0x45, r8(m, found + 5));
    for (std::uint32_t b = 0; b < 4; ++b) {
        w8(m, sp + 0x46 + b, r8(m, found + 6 + b));
        w8(m, sp + 0x4a + b, r8(m, found + 0xa + b));
    }
    if (!r8(m, kInLevel)) {  // 00433e50: only the entry's local interest byte
        w8(m, found + 6 + local_of(m, host), 0);
        return;
    }
    w32(m, sp + 0x50, r32(m, sp + 0x5c));
    w8(m, sp + 0x46 + local_of(m, host), 0);
    w8(m, found + 6 + local_of(m, host), 0);
    host.call(m, 0x434e20, {a0, sp + 0x40});
    host.call(m, 0x434920, {a0, r32(m, sp + 0x5c)});
}

// ---- the actor messages and the locomotion replication (checkpoint 132ln5h3; NET_INLEVEL.md 4.14) -------------
namespace {
constexpr std::uint32_t kMessageBase = 0x4dfd80, kActionMessage = 0x4e0840, kMoveMessage = 0x4e0100;
constexpr std::uint32_t kActionType = 0x55d480, kMoveLast = 0x55a228;

// cvt.w.s: truncation, saturated to the word range.
std::uint32_t cvt_w_s(float v) {
    if (v >= 2147483647.0F) return 0x7fffffffU;
    if (v <= -2147483648.0F) return 0x80000000U;
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(v));
}
void copy16(LevelLoadMemory& m, std::uint32_t to, std::uint32_t from) {
    for (std::uint32_t k = 0; k < 16; k += 4) w32(m, to + k, r32(m, from + k));
}
// The message queue 003ca0e0(out, 0x005435c0, &0x005435c4, {transport, clone, target}) at the frame's slots.
void queue_message(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t out, std::uint32_t at, std::uint32_t entry,
                   std::uint32_t transport, std::uint32_t clone, std::uint32_t target) {
    w32(m, at, 0x5435c4);
    w32(m, entry, transport);
    w32(m, entry + 4, clone);
    w32(m, entry + 8, target);
    host.call(m, 0x3ca0e0, {out, 0x5435c0, at, entry});
}
// 003c9ca0(msg, key, .., cap t1, buf t2): the header word (the clock << 2 | the transport, 3 when above 3) and the
// key; always -1.
std::uint32_t message_header_003c9ca0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t key,
                                      std::uint32_t cap, std::uint32_t buf) {
    if (cap < 8 || !buf) return 0xffffffffU;
    const auto low = s32(r32(m, msg + 0x18)) < 4 ? r32(m, msg + 0x18) & 3U : 3U;
    w32(m, buf, host.call(m, 0x42b1a0, {0}) << 2 | low);
    w32(m, buf + 4, key);
    return 0xffffffffU;
}
// 003ff330(msg, key, typeptr, .., cap t1, buf t2): the action header (+8 the action byte, +9 the kind), *typeptr the
// action type 0x0055d490; 0xc, or -1 when there is no room or no typeptr.
std::uint32_t action_header_003ff330(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t key,
                                     std::uint32_t typeptr, std::uint32_t cap, std::uint32_t buf) {
    if (cap < 0xc || !buf) return 0xffffffffU;
    message_header_003c9ca0(m, host, msg, key, cap, buf);
    w8(m, buf + 8, r32(m, msg + 0x24));
    w8(m, buf + 9, r8(m, msg + 0x28));
    if (!typeptr) return 0xffffffffU;
    w32(m, typeptr, r32(m, 0x55d490));
    return 0xc;
}
}  // namespace

std::uint32_t message_construct_003c9e30(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t type) {
    w32(m, msg, kMessageBase);
    w32(m, msg + 4, type);
    w32(m, msg + 0x18, 0);
    w32(m, msg + 0x1c, 0);
    w8(m, msg + 0x20, 5);
    if (r8(m, kNetworkGame) && r8(m, kInLevel)) w32(m, msg + 0xc, host.call(m, 0x42b1a0, {0}));
    return msg;
}

std::uint32_t message_destroy_003c9d60(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t flag) {
    if (!msg) return msg;
    w32(m, msg, kMessageBase);
    if (static_cast<std::int16_t>(flag & 0xffffU) > 0) host.call(m, 0x100740, {msg, flag});
    return msg;
}

void net_latency_00429cb0(LevelLoadMemory& m, std::uint32_t delay) {
    if (s32(delay) >= 1000 || s32(delay) < 0) return;
    const auto count = r32(m, 0x5723e8) + 1;
    w32(m, 0x5723e8, count);
    const float total = rf(m, 0x5723e0) + std::bit_cast<float>(0x3a83126fU) * static_cast<float>(s32(delay));
    wf(m, 0x5723e0, total);
    wf(m, 0x5723d8, total / static_cast<float>(s32(count)));
}

std::uint32_t message_construct_received_003c9db0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                                  std::uint32_t type, std::uint32_t buf) {
    w32(m, msg, kMessageBase);
    w32(m, msg + 4, type);
    w32(m, msg + 0x14, r32(m, buf + 4));
    w32(m, msg + 0x1c, 0);
    w8(m, msg + 0x20, 5);
    w32(m, msg + 0xc, r32(m, buf) >> 2);
    w32(m, msg + 0x18, r8(m, buf) & 3U);
    net_latency_00429cb0(m, host.call(m, 0x42b1a0, {0}) - r32(m, msg + 0xc));
    return msg;
}

std::uint32_t move_message_destroy_0014d270(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t flag) {
    if (!msg) return msg;
    w32(m, msg, kMoveMessage);
    w32(m, msg, kActionMessage);
    host.call(m, 0x3c9d60, {msg, 0});
    if (static_cast<std::int16_t>(flag & 0xffffU) > 0) host.call(m, 0x100740, {msg, 0});  // $a1 still 003c9d60's 0
    return msg;
}

std::uint32_t move_message_clone_003e5d70(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg) {
    heap_enter(m, host);
    const auto copy = host.call(m, 0x36b180, {0, 0x60, 0x4bdde8, 0x40e});  // "character.cc"
    heap_leave(m, host);
    if (copy) {
        w32(m, copy, kMessageBase);
        for (std::uint32_t o = 4; o < 0x20; o += 4) w32(m, copy + o, r32(m, msg + o));
        w8(m, copy + 0x20, r8(m, msg + 0x20));
        w32(m, copy, kActionMessage);
        w32(m, copy + 0x24, r32(m, msg + 0x24));
        w8(m, copy + 0x28, r8(m, msg + 0x28));
        copy16(m, copy + 0x30, msg + 0x30);
        copy16(m, copy + 0x40, msg + 0x40);
        for (std::uint32_t o = 0x50; o < 0x5c; o += 4) w32(m, copy + o, r32(m, msg + o));
        w32(m, copy, kMoveMessage);
    }
    w32(m, copy + 0xc, r32(m, msg + 0xc));
    return copy;
}

std::uint32_t move_message_serialise_003e57b0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                              std::uint32_t key, std::uint32_t typeptr, std::uint32_t, std::uint32_t,
                                              std::uint32_t cap, std::uint32_t buf) {
    std::uint32_t size = 0xffffffffU;
    if (!r32(m, kMoveLast)) {
        heap_enter(m, host);
        const auto first = host.call(m, 0x36b180, {0, 0x60, 0x4bdd08, 0x2c5});  // "character.cc"
        heap_leave(m, host);
        if (first) {
            host.call(m, 0x3c9e30, {first, kActionType});
            w32(m, first, kActionMessage);
            w32(m, first + 0x24, 0);
            w8(m, first + 0x28, 0xd);
            for (std::uint32_t o = 0x30; o < 0x5c; o += 4) w32(m, first + o, 0);
            w32(m, first, kMoveMessage);
        }
        w32(m, kMoveLast, first);
    }
    const auto last = r32(m, kMoveLast);
    if (rf(m, last + 0x30) == rf(m, msg + 0x30) && rf(m, last + 0x34) == rf(m, msg + 0x34) &&
        rf(m, last + 0x38) == rf(m, msg + 0x38))
        return size;
    if (cap < 0x28 || !buf) return size;
    action_header_003ff330(m, host, msg, key, 0, cap, buf);
    if (const auto ground = r32(m, msg + 0x58); ground && r32(m, ground + 0x50) && r32(m, ground + 0x54))
        throw std::runtime_error("003e5a48: the locomotion message's ground-relative position is not ported");
    for (std::uint32_t k = 0; k < 12; k += 4) w32(m, buf + 8 + k, r32(m, msg + 0x30 + k));
    for (std::uint32_t k = 0; k < 16; k += 4) w32(m, buf + 0x14 + k, r32(m, msg + 0x40 + k));
    m.write(buf + 0x26, 2, cvt_w_s(32768.0F * rf(m, msg + 0x54)) & 0xffffU);
    m.write(buf + 0x24, 2, cvt_w_s(8192.0F * rf(m, msg + 0x50)) & 0xffffU);
    if (typeptr) {
        w32(m, typeptr, r32(m, 0x55a210));
        size = 0x28;
    }
    heap_enter(m, host);
    const auto old = r32(m, kMoveLast);
    host.call(m, r32(m, r32(m, old) + 8), {old, 0xffffffffU});
    host.call(m, 0x36b130, {0, old, 0x4bdd08, 0x363});
    heap_leave(m, host);
    w32(m, kMoveLast, host.call(m, r32(m, r32(m, msg) + 0xc), {msg}));
    return size;
}

std::uint32_t net_message_send_003fb020(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t key, std::uint32_t msg,
                                        std::uint32_t client, std::uint32_t a3, std::uint32_t stack) {
    const auto sp = stack - 0x470, type = sp + 0x46c, buf = sp + 0x60;
    const auto clock = host.call(m, 0x42b1a0, {0});
    auto size = host.call(m, r32(m, r32(m, msg) + 0x10), {msg, key, type, clock, r32(m, 0x55c7b8), 0x400, buf});
    const auto done = [&] { return size != 0 ? 1U : 0U; };
    if (!r8(m, kInLevel) || s32(key) < 0 || s32(size) < 0) return done();
    if (r32(m, r32(m, msg + 4)) == r32(m, kActionType)) {
        const auto kind = r8(m, msg + 0x28), action = r32(m, msg + 0x24);
        if (kind == 1 || kind == 0xc || action - 1 < 2 || action == 0x43 || action - 0x3e < 2 || action - 0x39 < 2 ||
            action == 0xd)
            return done();
    }
    const auto flags = r8(m, msg + 0x20);
    if (flags == 1) return done();
    if (flags & 0x15) {
        if (!host.call(m, 0x3fad50, {r32(m, type), size, buf, a3, r32(m, msg + 0x1c)})) size = 0xffffffffU;
    }
    if (r8(m, msg + 0x20) & 8) {
        if (!host.call(m, 0x3fad50, {r32(m, type), size, buf, client, r32(m, msg + 0x1c)})) size = 0xffffffffU;
    }
    if ((r8(m, msg + 0x20) & 2) && !host_or_solo(m)) {
        const auto flag = r32(m, msg + 0x1c);
        host.call(m, 0x2ebba0, {r32(m, kWorld), sp + 0x468});
        if (!host.call(m, 0x3fad50, {r32(m, type), size, buf, r32(m, sp + 0x468), flag})) size = 0xffffffffU;
    }
    return done();
}

std::uint32_t actor_message_send_003c9420(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                          std::uint32_t target, std::uint32_t transport, std::uint32_t stack) {
    const auto sp = stack - 0x100;
    bool delivered = false;
    if (transport == 1 && (r8(m, msg + 0x20) & 1)) {
        host.call(m, r32(m, r32(m, target) + 0xc), {target, msg});
        delivered = true;
        if (!r8(m, kNetworkGame)) return 1;
    }
    const auto clone = host.call(m, r32(m, r32(m, msg) + 0xc), {msg});
    if (!clone) {
        host.call(m, 0x127358, {0x4bd150, delivered ? msg : target, transport});  // the registers left over
        return 0;
    }
    w32(m, clone + 8, 0xbebaafdeU);
    if (!r8(m, kNetworkGame)) {
        queue_message(m, host, sp + 0xd8, sp + 0xd4, sp + 0x88, transport, clone, target);
        return 1;
    }
    if (!target) return 1;
    const auto slot = [&] { return local_slot(m, host, r8(m, kNetworkGame), r32(m, kWorld)); };
    const auto client_of = [&] { return ((r32(m, target + 0xc) & 0x7f000000U) >> 24) - 1; };
    if (r8(m, kInLevel) && host.call(m, r32(m, r32(m, target) + 0x18), {target})) {
        const auto flags = r8(m, msg + 0x20);
        bool sent = false;
        if (flags & 4) {
            w32(m, msg + 0x18, transport);
            host.call(m, 0x3fb020, {r32(m, target + 0x10), clone, 0xffffffffU, 0xffffffffU});
            sent = true;
        } else if (flags & 8) {
            const auto key = r32(m, target + 0x10);
            const auto here = slot();
            if (host.call(m, 0x3fb340, {key}) != here) {
                w32(m, msg + 0x18, transport);
                const auto k2 = r32(m, target + 0x10);
                host.call(m, 0x3fb020, {k2, clone, host.call(m, 0x3fb340, {k2}), 0xffffffffU});
                sent = true;
            }
        }
        if (!sent) {
            const auto again = r8(m, msg + 0x20);
            if ((again & 2) && !host_or_solo(m)) {
                w32(m, msg + 0x18, transport);
                host.call(m, 0x3fb020, {r32(m, target + 0x10), clone, 0xffffffffU, 0xffffffffU});
            } else if (again & 0x20) {
                const auto c = client_of();
                if (slot() != c) {
                    w32(m, msg + 0x18, transport);
                    host.call(m, 0x3fb020, {r32(m, target + 0x10), clone, client_of(), 0xffffffffU});
                }
            }
        }
    }
    const auto flags = [&] { return r8(m, msg + 0x20); };
    if (!delivered && (flags() & 1)) {
        queue_message(m, host, sp + 0xf8, sp + 0xf4, sp + 0xc8, transport, clone, target);
        return 1;
    }
    if (flags() & 8) {
        const auto key = r32(m, target + 0xc);
        const auto here = slot();
        if (host.call(m, 0x3fb340, {key}) == here) {
            queue_message(m, host, sp + 0xf0, sp + 0xec, sp + 0xb8, transport, clone, target);
            return 1;
        }
    }
    const auto f = flags();
    if ((f & 2) && host_or_solo(m)) {
        queue_message(m, host, sp + 0xe8, sp + 0xe4, sp + 0xa8, transport, clone, target);
        return 1;
    } else if (f & 0x20) {
        const auto c = client_of();
        if (slot() == c) {
            queue_message(m, host, sp + 0xe0, sp + 0xdc, sp + 0x98, transport, clone, target);
            return 1;
        }
    }
    w32(m, clone + 8, 0);
    heap_enter(m, host);
    host.call(m, r32(m, r32(m, clone) + 8), {clone, 0xffffffffU});
    host.call(m, 0x36b130, {0, clone, 0x4bd118, 0x132});  // "message.cc"
    heap_leave(m, host);
    return 1;
}

std::uint32_t actor_message_deliver_003c9350(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                             std::uint32_t target, std::uint32_t transport, std::uint32_t stack) {
    const auto sp = stack - 0x50;
    if (!target) return 1;
    if (transport == 1) {
        host.call(m, r32(m, r32(m, target) + 0xc), {target, msg});
        return 1;
    }
    const auto clone = host.call(m, r32(m, r32(m, msg) + 0xc), {msg});
    if (!clone) {
        host.call(m, 0x127358, {0x4bd130, target, transport});  // the registers left over
        return 0;
    }
    w32(m, clone + 8, 0xbebaafdeU);
    queue_message(m, host, sp + 0x4c, sp + 0x48, sp + 0x38, transport, clone, target);
    return 1;
}

std::uint32_t move_message_receive_003e4430(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t buf,
                                            std::uint32_t stack) {
    const auto sp = stack - 0x130, msg = sp + 0xc0;
    busy_enter(m);
    if (r8(m, kInLevel)) {
        message_construct_received_003c9db0(m, host, msg, kActionType, buf);  // 003e4940(msg, buf, 0, 0xd)
        w32(m, msg, kActionMessage);
        w32(m, msg + 0x24, 0);
        w8(m, msg + 0x28, 0xd);
        for (std::uint32_t k = 0; k < 12; k += 4) w32(m, msg + 0x30 + k, r32(m, buf + 8 + k));  // 003e4880
        w32(m, msg + 0x3c, 0x3f800000U);
        for (std::uint32_t k = 0; k < 16; k += 4) w32(m, msg + 0x40 + k, r32(m, buf + 0x14 + k));
        const float heading = static_cast<float>(static_cast<std::int16_t>(m.read(buf + 0x24, 2))) * 0.0001220703125F;
        const float speed = static_cast<float>(m.read(buf + 0x26, 2)) * 0.000030517578125F;
        wf(m, msg + 0x50, heading);
        wf(m, msg + 0x54, speed < 1.0F ? speed : 1.0F);
        w32(m, msg + 0x58, 0);
        w32(m, msg, kMoveMessage);
        if (m.read(buf + 0x26, 2) == 0xffffU)
            throw std::runtime_error("003e4520: the locomotion message's ground-relative position is not ported");
        if (const auto entry = host.call(m, 0x3fbb10, {r32(m, buf + 4)}))
            host.call(m, 0x3c9350, {msg, r32(m, entry), 0, 0});
        w32(m, msg, kActionMessage);
        host.call(m, 0x3c9d60, {msg, 0});
    }
    w8(m, kBusy, 0);
    return 0x28;
}

void player_net_update_001770f0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t self, std::uint32_t f12,
                                std::uint32_t stack) {
    const auto sp = stack - 0x140;
    if (r8(m, kNetworkGame)) {
        const auto human = r32(m, self + 0x30);
        if (r8(m, human + 0x14)) {
            if (human) {
                if (const auto block = r32(m, human + 0x3584)) w8(m, block + 0x33, r8(m, 0x582b88));
            }
            const auto due = r32(m, self + 0x1c0);
            if (s32(due) < s32(host.call(m, 0x42b1a0, {1}))) {
                const auto me = r32(m, self + 0x30);
                host.call(m, r32(m, r32(m, me) + 0xf4), {sp + 0x100, me});
                std::uint32_t ground = 0;
                if (!host.call(m, r32(m, r32(m, human) + 0xf8), {human, sp + 0xf0})) {
                    for (std::uint32_t k = 0; k < 16; k += 4) w32(m, sp + 0xf0 + k, 0);
                } else if (const auto v = host.call(m, r32(m, r32(m, human) + 0x110), {human})) {
                    ground = r32(m, r32(m, v + 0x64));
                }
                copy16(m, sp + 0x70, sp + 0x100);  // 00151780(m, 0) / (m, 1): the rows
                copy16(m, sp + 0x60, sp + 0x110);
                const float heading = rf(m, sp + 0x78) < rf(m, sp + 0x68)
                                          ? game_atan2f(rf(m, sp + 0x74), rf(m, sp + 0x70))
                                          : game_atan2f(-rf(m, sp + 0x60), rf(m, sp + 0x64));
                const auto msg = sp + 0x80;
                host.call(m, 0x3c9e30, {msg, kActionType});
                w32(m, msg, kActionMessage);
                w32(m, msg + 0x24, 0);
                w8(m, msg + 0x28, 0xd);
                copy16(m, msg + 0x30, sp + 0x130);
                copy16(m, msg + 0x40, sp + 0xf0);
                wf(m, msg + 0x50, heading);
                w32(m, msg + 0x54, 0x3f800000U);
                w32(m, msg + 0x58, ground);
                w32(m, msg, kMoveMessage);
                host.call(m, 0x3c9420, {msg, r32(m, self + 0x30), 0});
                w32(m, msg, kActionMessage);
                host.call(m, 0x3c9d60, {msg, 0});
                w32(m, self + 0x1c0, host.call(m, 0x42b1a0, {1}) + 0xfa);
            }
        }
    }
    host.call(m, 0x24b5d0, {self + 0x50, r32(m, self + 0x1a0), r32(m, self + 0x30), f12});
    host.call(m, 0x24aaf0, {self + 0x50, r32(m, self + 0x1a0), r32(m, self + 0x30)});
    host.call(m, 0x261540, {r32(m, 0x4ffc2c) + 0xd0, 1, r32(m, self + 0x1ac)});
    w32(m, self + 0x1a8, 0);
    w32(m, self + 0x1ac, 0);
}

// ---- the human's network section and the animation fields (checkpoint 132ln5h4; NET_INLEVEL.md 4.15) ----------
void human_weapon_sync_001b2d20(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t human) {
    const auto held_weapon = [&] {
        const auto inv = r32(m, human + 0x3524);
        const auto slot = r8(m, inv + 0x84);
        return slot == 6 ? 0xffffffffU : r32(m, inv + slot * 16 + 8);
    };
    if (!r8(m, human + 0x14)) {
        if (!r32(m, human + 0x3584)) {
            const auto entry = host.call(m, 0x3fbb10, {r32(m, human + 0xc)});
            if (entry && r32(m, entry + 0x10)) w32(m, human + 0x3584, r32(m, entry + 0x10));
        }
        const auto block = r32(m, human + 0x3584);
        const auto counters = block ? block + 0x1c : 0U;
        if (!counters) return;
        const auto inv = r32(m, human + 0x3524);
        const auto want = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(m.read(counters + 6, 2))));
        if (held_weapon() != want) host.call(m, 0x1439b0, {inv, want});
        return;
    }
    const auto block = r32(m, human + 0x3584);
    const auto counters = block ? block + 0x1c : 0U;
    if (!counters) return;
    const auto want = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int16_t>(m.read(counters + 6, 2))));
    const auto held = held_weapon();
    if (held != want) m.write(counters + 6, 2, held & 0xffffU);
}

// 001b1710's network part (001b173c..001b1bec): runs only in a network game; the rest of the function (from
// 001b1bf0, 0019f6c0 on) is the play's own human update.
void human_network_section_001b1710(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t human, std::uint32_t stack) {
    const auto sp = stack - 0x340;
    if (!r8(m, kNetworkGame)) return;
    const auto block_of = [&] { return r32(m, human + 0x3584); };
    const auto controller_says = [&](std::uint32_t slot) {
        const auto controller = r32(m, human + 0x58);
        return controller && host.call(m, r32(m, r32(m, controller) + slot), {controller}) != 0;
    };
    const auto flashlight_on = [&]() -> std::uint32_t {
        const auto light = r32(m, human + 0x33ec);
        return light && r8(m, light) ? 1U : 0U;
    };
    if (const auto block = block_of()) {
        if (!r8(m, human + 0x14)) {
            if (cvt_w_s(rf(m, human + 0x40)) != cvt_w_s(rf(m, block + 0x38))) {
                bool set = true;
                if (controller_says(0x5c))
                    set = s32(cvt_w_s(rf(m, human + 0x40))) > 0 && s32(cvt_w_s(rf(m, block_of() + 0x38))) > 0;
                if (set) host.call(m, 0x3cc990, {human, cvt_w_s(rf(m, block_of() + 0x38)), 1});
            }
            const auto killed = r8(m, block_of() + 0x35);
            if (r8(m, human + 0x33a5) != killed) w8(m, human + 0x33a5, killed);
            if (flashlight_on() != r8(m, block_of() + 0x52)) {
                const auto light = r32(m, human + 0x33ec);
                const auto want = r8(m, block_of() + 0x52) != 0 ? 1U : 0U;
                if (light) host.call(m, 0x45b610, {light, want});
            }
        } else {
            wf(m, block + 0x38, static_cast<float>(s32(cvt_w_s(rf(m, human + 0x40)))));
            w8(m, block_of() + 0x52, flashlight_on());
            w8(m, block_of() + 0x35, r8(m, human + 0x33a5));
        }
    }
    host.call(m, 0x1b2d20, {human});
    if (controller_says(0x5c)) {
        if (const auto block = block_of(); block && r8(m, block + 0x28)) {
            host.call(m, 0x127358, {0x49e850, r32(m, human + 0xc)});
            host.call(m, 0x2d2110, {block_of()});
        }
        if (r8(m, human + 0x14)) {
            host.call(m, 0x127358, {0x49e880, r32(m, human + 0xc)});
            w8(m, human + 0x14, 0);
        }
    } else if (controller_says(0x58)) {
        if (const auto block = block_of()) {
            w32(m, block + 0x54, host.call(m, 0x42b1a0, {1}));
            const auto now = block_of();
            if (!r8(m, now + 0x28)) host.call(m, 0x2d1d80, {now});
        }
    }
    if (controller_says(0x54) && !r8(m, human + 0x14)) host.call(m, 0x1b2e90, {human});
    const auto block = block_of();
    if (!block || r8(m, human + 0x14)) return;
    auto flags = r32(m, human + 0x3248);
    flags = r8(m, block + 0x50) & 1 ? flags | 1U : flags & ~1U;
    flags = r8(m, block_of() + 0x50) & 2 ? flags | 2U : flags & ~2U;
    w32(m, human + 0x3248, flags);
    w8(m, human + 0x31b3, r8(m, block_of() + 0x50) & 4 ? 1 : 0);
    w8(m, human + 0x31b4, r8(m, block_of() + 0x50) & 8 ? 1 : 0);
    w8(m, human + 0x31b5, r8(m, block_of() + 0x50) & 0x20 ? 1 : 0);
    const auto mode = r8(m, block_of() + 0x51);
    if (mode == 0xc) flags = (flags & 0xbfffffffU) | 0x80000000U;
    else if (mode == 0xb) flags = (flags & 0x7fffffffU) | 0x40000000U;
    else flags &= 0x3fffffffU;
    w32(m, human + 0x3248, flags);
    // action 0x43: the physical's +0x12 / +0x13 (001ad5e0); payload 0 when +0x50 bit 0x40 is set
    const auto payload = r8(m, block_of() + 0x50) & 0x40 ? 0U : 1U;
    const auto msg = sp + 0x100;
    host.call(m, 0x3c9e30, {msg, kActionType});
    w32(m, msg, kActionMessage);
    w32(m, msg + 0x24, 0x43);
    w8(m, msg + 0x28, 1);
    w32(m, msg, 0x4d9320);
    w8(m, msg + 0x2c, payload);
    w8(m, msg + 0x20, 1);
    host.call(m, 0x3c9420, {msg, human, 1});
    w32(m, msg, kActionMessage);
    host.call(m, 0x3c9d60, {msg, 0});
}

// ---- the generators' cEnableMsg / cDisableMsg and 003ce850 (checkpoint 132ln5h11; NET_INLEVEL.md 4.21) --------
namespace {
constexpr std::uint32_t kDisableMessage = 0x4e0860, kEnableMessage = 0x4e0880;

std::uint32_t generator_message_destroy(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t flag,
                                        std::uint32_t vtable) {
    if (!msg) return msg;
    w32(m, msg, vtable);
    host.call(m, 0x3c9d60, {msg, 0});
    if (static_cast<std::int16_t>(flag & 0xffffU) > 0) host.call(m, 0x100740, {msg, 0});  // $a1 still 003c9d60's 0
    return msg;
}
std::uint32_t generator_message_clone(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t name,
                                      std::uint32_t vtable) {
    heap_enter(m, host);
    const auto copy = host.call(m, 0x36b180, {0, 0x28, name, 0x21});
    heap_leave(m, host);
    if (copy) {
        w32(m, copy, kMessageBase);
        for (std::uint32_t o = 4; o < 0x20; o += 4) w32(m, copy + o, r32(m, msg + o));
        w8(m, copy + 0x20, r8(m, msg + 0x20));
        w32(m, copy, vtable);
        w32(m, copy + 0x24, r32(m, msg + 0x24));
    }
    return copy;
}
std::uint32_t generator_message_serialise(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t key,
                                          std::uint32_t typeptr, std::uint32_t cap, std::uint32_t buf,
                                          std::uint32_t type) {
    message_header_003c9ca0(m, host, msg, key, cap, buf);
    w32(m, buf + 8, r32(m, msg + 0x24));
    w32(m, typeptr, r32(m, type));
    return 0xc;
}
std::uint32_t generator_message_receive(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t buf, std::uint32_t slot) {
    busy_enter(m);
    if (r8(m, kInLevel)) {
        if (const auto entry = host.call(m, 0x3fbb10, {r32(m, buf + 4)})) {
            const auto object = r32(m, entry);
            host.call(m, r32(m, r32(m, object) + slot), {object, r32(m, buf + 8)});
        }
    }
    w8(m, kBusy, 0);
    return 0xc;
}
}  // namespace

std::uint32_t disable_message_destroy_0016fc70(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t flag) {
    return generator_message_destroy(m, host, msg, flag, kDisableMessage);
}
std::uint32_t enable_message_destroy_0016fd90(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t flag) {
    return generator_message_destroy(m, host, msg, flag, kEnableMessage);
}
std::uint32_t disable_message_clone_003ff3b0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg) {
    return generator_message_clone(m, host, msg, 0x4be5a0, kDisableMessage);
}
std::uint32_t enable_message_clone_003ff690(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg) {
    return generator_message_clone(m, host, msg, 0x4be5e0, kEnableMessage);
}
std::uint32_t disable_message_serialise_003ff570(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                                 std::uint32_t key, std::uint32_t typeptr, std::uint32_t cap,
                                                 std::uint32_t buf) {
    return generator_message_serialise(m, host, msg, key, typeptr, cap, buf, 0x55d4a0);
}
std::uint32_t enable_message_serialise_003ff850(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                                std::uint32_t key, std::uint32_t typeptr, std::uint32_t cap,
                                                std::uint32_t buf) {
    return generator_message_serialise(m, host, msg, key, typeptr, cap, buf, 0x55d4c0);
}
std::uint32_t disable_message_receive_003ff5d0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t buf) {
    return generator_message_receive(m, host, buf, 0x2c);
}
std::uint32_t enable_message_receive_003ff8b0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t buf) {
    return generator_message_receive(m, host, buf, 0x28);
}
void disable_message_send_003ff650(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t key,
                                   std::uint32_t a2) {
    if (r8(m, kNetworkGame)) host.call(m, 0x3fb020, {key, msg, 0xffffffffU, a2});
}
void enable_message_send_003ff930(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t key) {
    if (r8(m, kNetworkGame)) host.call(m, 0x3fb020, {key, msg, 0xffffffffU, 0xffffffffU});
}

std::uint32_t nearest_player_003ce850(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t object,
                                      std::uint32_t outdist, std::uint32_t stack) {
    const auto sp = stack - 0x90;
    auto best = host.call(m, 0x40bf10, {});
    float dist = std::bit_cast<float>(0x4cbebc20U);  // 1.0e8
    for (std::uint32_t c = 0; c < 4; ++c) {
        const auto player = host.call(m, 0x3fb8d0, {c});
        if (!player) continue;
        host.call(m, r32(m, r32(m, object) + 0xc0), {sp + 0x80, object, 0xffffffffU});
        host.call(m, r32(m, r32(m, player) + 0xc0), {sp + 0x70, player, 0xffffffffU});
        // vsub.xyzw, vmul.xyz, vmulax.x / vmadday.x / vmaddz.x with vf3.x = 1: (x*x + y*y) + z*z
        const float dx = rf(m, sp + 0x80) - rf(m, sp + 0x70), dy = rf(m, sp + 0x84) - rf(m, sp + 0x74),
                    dz = rf(m, sp + 0x88) - rf(m, sp + 0x78);
        const float d = (dx * dx + dy * dy) + dz * dz;
        if (!std::isfinite(d)) throw std::runtime_error("003ce850: non-finite distance (the VU clamp is not hosted)");
        if (d < dist) {
            dist = d;
            best = player;
        }
    }
    if (outdist) wf(m, outdist, dist);
    return best;
}

// ---- the generator's spawn message (vtable 0x004de240; checkpoint 132ln5h12; NET_INLEVEL.md 4.22) -------------
namespace {
constexpr std::uint32_t kSpawnMessage = 0x4de240, kSpawnType = 0x51f058, kSpawnSerialType = 0x51f060;
}  // namespace

std::uint32_t spawn_message_destroy_0021df80(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                             std::uint32_t flag) {
    return generator_message_destroy(m, host, msg, flag, kSpawnMessage);
}

std::uint32_t spawn_message_clone_002d0ee0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg) {
    heap_enter(m, host);
    const auto copy = host.call(m, 0x36b180, {0, 0x70, 0x4ad520, 0x22});
    heap_leave(m, host);
    if (copy) {
        w32(m, copy, kMessageBase);
        for (std::uint32_t o = 4; o < 0x20; o += 4) w32(m, copy + o, r32(m, msg + o));
        w8(m, copy + 0x20, r8(m, msg + 0x20));
        w32(m, copy, kSpawnMessage);
        for (std::uint32_t o = 0x30; o < 0x4c; o += 4) w32(m, copy + o, r32(m, msg + o));  // +0x4c not copied
        for (std::uint32_t o = 0x50; o < 0x64; o += 4) w32(m, copy + o, r32(m, msg + o));
    }
    return copy;
}

std::uint32_t spawn_message_serialise_002d1100(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                               std::uint32_t key, std::uint32_t typeptr, std::uint32_t cap,
                                               std::uint32_t buf) {
    if (cap < 0x34 || !buf) return 0xffffffffU;
    message_header_003c9ca0(m, host, msg, key, cap, buf);
    for (std::uint32_t i = 0; i < 3; ++i) {
        w32(m, buf + 8 + 4 * i, r32(m, msg + 0x50 + 4 * i));
        w32(m, buf + 0x14 + 4 * i, r32(m, msg + 0x40 + 4 * i));
        w32(m, buf + 0x20 + 4 * i, r32(m, msg + 0x30 + 4 * i));
    }
    w32(m, buf + 0x2c, r32(m, msg + 0x5c));
    w32(m, buf + 0x30, r32(m, msg + 0x60));
    if (!typeptr) return 0xffffffffU;
    w32(m, typeptr, r32(m, kSpawnSerialType));
    return 0x34;
}

std::uint32_t spawn_message_build_002d1290(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                           std::uint32_t buf, std::uint32_t stack) {
    host.call(m, 0x3c9e30, {msg, kSpawnType});
    w32(m, msg, kSpawnMessage);
    for (std::uint32_t i = 0; i < 3; ++i) {
        w32(m, msg + 0x50 + 4 * i, r32(m, buf + 8 + 4 * i));
        w32(m, msg + 0x40 + 4 * i, r32(m, buf + 0x14 + 4 * i));
        w32(m, msg + 0x30 + 4 * i, r32(m, buf + 0x20 + 4 * i));
    }
    w32(m, msg + 0x4c, r32(m, stack - 4));  // the quadword store carries the frame's unwritten word
    w32(m, msg + 0x3c, 0x3f800000);
    w32(m, msg + 0x5c, r32(m, buf + 0x2c));
    w32(m, msg + 0x60, r32(m, buf + 0x30));
    return msg;
}

std::uint32_t spawn_message_receive_002d11d0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t buf,
                                             std::uint32_t stack) {
    const auto sp = stack - 0xb0;
    busy_enter(m);
    if (r8(m, kInLevel)) {
        host.call(m, 0x2d1290, {sp + 0x30, buf});
        if (host.call(m, 0x3fbb10, {r32(m, buf + 4)})) {
            w32(m, sp + 0xa8, r32(m, buf + 4));
            const auto entry = net_map_index_002d1360(m, host, kNetMap, r32(m, sp + 0xa8));
            host.call(m, 0x3c9350, {sp + 0x30, r32(m, entry + 4), 0, 0});
        }
        w32(m, sp + 0x30, kSpawnMessage);
        host.call(m, 0x3c9d60, {sp + 0x30, 0});
    }
    w8(m, kBusy, 0);
    return 0x34;
}

// ---- the generator's despawn message (vtable 0x004de260; checkpoint 132ln5h14; NET_INLEVEL.md 4.24) ------------
namespace {
constexpr std::uint32_t kDespawnMessage = 0x4de260, kDespawnType = 0x51f068, kDespawnSerialType = 0x51f070;
}  // namespace

std::uint32_t despawn_message_destroy_00220350(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                               std::uint32_t flag) {
    return generator_message_destroy(m, host, msg, flag, kDespawnMessage);
}

std::uint32_t despawn_message_clone_002d15c0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg) {
    heap_enter(m, host);
    const auto copy = host.call(m, 0x36b180, {0, 0x34, 0x4ad570, 0x1a});
    heap_leave(m, host);
    if (copy) {
        w32(m, copy, kMessageBase);
        for (std::uint32_t o = 4; o < 0x20; o += 4) w32(m, copy + o, r32(m, msg + o));
        w8(m, copy + 0x20, r8(m, msg + 0x20));
        w32(m, copy, kDespawnMessage);
        for (std::uint32_t o = 0x24; o < 0x34; o += 4) w32(m, copy + o, r32(m, msg + o));
    }
    return copy;
}

std::uint32_t despawn_message_serialise_002d1790(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                                 std::uint32_t key, std::uint32_t typeptr, std::uint32_t cap,
                                                 std::uint32_t buf) {
    if (cap < 0x10 || !buf) return 0xffffffffU;
    message_header_003c9ca0(m, host, msg, key, cap, buf);
    w32(m, buf + 8, r32(m, msg + 0x2c));
    w32(m, buf + 0xc, r32(m, msg + 0x30));
    if (!typeptr) return 0xffffffffU;
    w32(m, typeptr, r32(m, kDespawnSerialType));
    return 0x10;
}

std::uint32_t despawn_message_receive_002d1810(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t buf,
                                               std::uint32_t stack) {
    const auto sp = stack - 0x90, msg = sp + 0x40;
    busy_enter(m);
    if (r8(m, kInLevel)) {
        host.call(m, 0x3c9e30, {msg, kDespawnType});
        w32(m, msg, kDespawnMessage);
        w32(m, msg + 0x2c, r32(m, buf + 8));
        w32(m, msg + 0x30, r32(m, buf + 0xc));
        w32(m, sp + 0x88, r32(m, buf + 4));
        const auto node = rb_map_find(m, kNetMap, r32(m, sp + 0x88));  // 001bb080
        w32(m, sp + 0x7c, node);
        w32(m, sp + 0x78, kNetMap + 4);  // 0021d360 / 0021d350: the end
        w32(m, sp + 0x84, kNetMap + 4);
        if (node != kNetMap + 4) {
            w32(m, sp + 0x80, r32(m, buf + 4));
            const auto entry = net_map_index_002d1360(m, host, kNetMap, r32(m, sp + 0x80));
            host.call(m, 0x3c9350, {msg, r32(m, entry + 4), 0, 0});
        }
        w32(m, msg, kDespawnMessage);
        host.call(m, 0x3c9d60, {msg, 0});
    }
    w8(m, kBusy, 0);
    return 0x10;
}

// ---- the health message (vtable 0x004e0ee0; checkpoint 132ln5h14c; NET_INLEVEL.md 4.26) ------------------------
namespace {
constexpr std::uint32_t kHealthMessage = 0x4e0ee0, kHealthType = 0x582750, kHealthSerialType = 0x582748;
}  // namespace

void health_set_003cc990(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t human, std::uint32_t hp, std::uint32_t a2,
                         std::uint32_t stack) {
    w32(m, human + 0x40, std::bit_cast<std::uint32_t>(static_cast<float>(static_cast<std::int32_t>(hp))));
    if (!r8(m, kNetworkGame) || a2) return;
    const auto msg = stack - 0x60 + 0x30;
    host.call(m, 0x3c9e30, {msg, kHealthType});
    w8(m, msg + 0x24, 0);
    w32(m, msg, kHealthMessage);
    w32(m, msg + 0x28, r32(m, human + 0xc));
    w8(m, msg + 0x20, 4);
    w32(m, msg + 0x2c, hp);
    host.call(m, 0x3c9420, {msg, human, 0});
    w32(m, msg, kHealthMessage);
    host.call(m, 0x3c9d60, {msg, 0});
}

std::uint32_t health_message_destroy_003cc670(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                              std::uint32_t flag) {
    return generator_message_destroy(m, host, msg, flag, kHealthMessage);
}

std::uint32_t health_message_clone_00431960(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg) {
    heap_enter(m, host);
    const auto copy = host.call(m, 0x36b180, {0, 0x30, 0x4bfc10, 0x1a});
    heap_leave(m, host);
    if (copy) {
        w32(m, copy, kMessageBase);
        for (std::uint32_t o = 4; o < 0x20; o += 4) w32(m, copy + o, r32(m, msg + o));
        w8(m, copy + 0x20, r8(m, msg + 0x20));
        w32(m, copy, kHealthMessage);
        w8(m, copy + 0x24, r8(m, msg + 0x24));
        w32(m, copy + 0x28, r32(m, msg + 0x28));
        w32(m, copy + 0x2c, r32(m, msg + 0x2c));
    }
    return copy;
}

std::uint32_t health_message_serialise_00431bc0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                                std::uint32_t key, std::uint32_t typeptr, std::uint32_t cap,
                                                std::uint32_t buf) {
    if (cap < 0x14 || !buf) return 0xffffffffU;
    message_header_003c9ca0(m, host, msg, key, cap, buf);
    w8(m, buf + 8, r8(m, msg + 0x24));
    w32(m, buf + 0xc, r32(m, msg + 0x28));
    w32(m, buf + 0x10, r32(m, msg + 0x2c));
    if (!typeptr) return 0xffffffffU;
    w32(m, typeptr, r32(m, kHealthSerialType));
    return 0x14;
}

std::uint32_t health_message_receive_00431a70(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t buf,
                                              std::uint32_t stack) {
    const auto sp = stack - 0x30;
    busy_enter(m);
    if (r8(m, kInLevel) && !r8(m, buf + 8)) {
        w32(m, sp + 0x28, r32(m, buf + 0xc));
        if (const auto object = host.call(m, 0x40c540, {sp + 0x28})) host.call(m, 0x3cc990, {object, r32(m, buf + 0x10), 1});
    }
    w8(m, kBusy, 0);
    return 0x14;
}

// ---- the damage message MSGLIB_DAMAGE_REQUEST (vtable 0x004e08a0; checkpoint 132ln5h14c; NET_INLEVEL.md 4.27) ------
namespace {
constexpr std::uint32_t kDamageMessage = 0x4e08a0, kDamageType = 0x5716f0, kDamageSerialType = 0x5716f8;
}  // namespace

std::uint32_t damage_message_destroy_001880e0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t flag) {
    return generator_message_destroy(m, host, msg, flag, kDamageMessage);
}

std::uint32_t damage_message_clone_00407f40(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg) {
    heap_enter(m, host);
    const auto copy = host.call(m, 0x36b180, {0, 0x30, 0x4be9f0, 0x20});
    heap_leave(m, host);
    if (copy) {
        w32(m, copy, kMessageBase);
        for (std::uint32_t o = 4; o < 0x20; o += 4) w32(m, copy + o, r32(m, msg + o));
        w8(m, copy + 0x20, r8(m, msg + 0x20));
        w32(m, copy, kDamageMessage);
        w32(m, copy + 0x24, r32(m, msg + 0x24));
        w32(m, copy + 0x28, r32(m, msg + 0x28));
        w8(m, copy + 0x2c, r8(m, msg + 0x2c));
        w8(m, copy + 0x2d, r8(m, msg + 0x2d));
        w8(m, copy + 0x2e, r8(m, msg + 0x2e));
    }
    return copy;
}

std::uint32_t damage_message_serialise_004083d0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t key,
                                                std::uint32_t typeptr, std::uint32_t cap, std::uint32_t buf) {
    if (cap < 0x18 || !buf) return 0xffffffffU;
    message_header_003c9ca0(m, host, msg, key, cap, buf);
    w8(m, buf + 8, r8(m, msg + 0x2e));
    w32(m, buf + 0xc, r32(m, r32(m, msg + 0x28) + 0xc));
    w16(m, buf + 0x10, r8(m, msg + 0x2d));
    w16(m, buf + 0x14, r32(m, msg + 0x24) & 0xffffU);
    w16(m, buf + 0x12, r8(m, msg + 0x2c));
    if (!typeptr) return 0xffffffffU;
    w32(m, typeptr, r32(m, kDamageSerialType));
    return 0x18;
}

std::uint32_t damage_message_receive_004081e0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t buf, std::uint32_t stack) {
    const auto sp = stack - 0x80;
    busy_enter(m);
    if (r8(m, kInLevel)) {
        const auto msg = sp + 0x40;
        host.call(m, 0x3c9e30, {msg, kDamageType});
        w32(m, msg, kDamageMessage);
        w32(m, sp + 0x74, r32(m, buf + 0xc));
        const auto source = host.call(m, 0x40c540, {sp + 0x74});
        w32(m, msg + 0x28, source);
        w8(m, msg + 0x2d, r16(m, buf + 0x10) & 0xffU);
        w32(m, msg + 0x24, r16(m, buf + 0x14));
        w8(m, msg + 0x2c, r16(m, buf + 0x12) & 0xffU);
        std::uint32_t victim = 0;
        if (const auto entry = host.call(m, 0x3fbb10, {r32(m, buf + 4)})) {
            victim = r32(m, entry);
        } else {
            w32(m, sp + 0x78, r32(m, buf + 4));
            victim = host.call(m, 0x40c540, {sp + 0x78});
        }
        if (victim && r32(m, msg + 0x28)) {
            if (r8(m, buf + 8)) {
                host.call(m, 0x3c9350, {msg, victim, 0, 0});
            } else {
                bool player = false;
                if (const auto controller = r32(m, victim + 0x58)) player = host.call(m, r32(m, r32(m, controller) + 0x54), {controller}) != 0;
                if (!(player && r8(m, victim + 0x14) && r32(m, r32(m, msg + 0x28) + 0x38) == 0x80)) {
                    const auto category = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(r8(m, msg + 0x2d))));
                    const auto type = static_cast<std::uint32_t>(static_cast<std::int32_t>(static_cast<std::int8_t>(r8(m, msg + 0x2c))));
                    const float amount = static_cast<float>(static_cast<std::int32_t>(r32(m, msg + 0x24)));
                    host.call(m, r32(m, r32(m, victim) + 0x100),
                              {victim, r32(m, msg + 0x28), category, type, 0, 1, 0, std::bit_cast<std::uint32_t>(amount)});
                }
            }
        }
        w32(m, msg, kDamageMessage);
        host.call(m, 0x3c9d60, {msg, 0});
    }
    w8(m, kBusy, 0);
    return 0x18;
}

// ---- the death action message (vtable 0x004e01e0 over the action message 0x004e0840, type 0x0055d480; 132ln5h14c) -
namespace {
constexpr std::uint32_t kDeathActionMessage = 0x4e01e0, kDeathActionSerialType = 0x55a220;
}  // namespace

std::uint32_t death_action_destroy_00184eb0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t flag) {
    if (!msg) return msg;
    w32(m, msg, kDeathActionMessage);
    w32(m, msg, kActionMessage);
    host.call(m, 0x3c9d60, {msg, 0});
    if (static_cast<std::int16_t>(flag & 0xffffU) > 0) host.call(m, 0x100740, {msg, 0});  // $a1 still 003c9d60's 0
    return msg;
}

std::uint32_t death_action_clone_003e7370(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg) {
    heap_enter(m, host);
    const auto copy = host.call(m, 0x36b180, {0, 0x30, 0x4bde68, 0x1df});
    heap_leave(m, host);
    if (copy) {
        w32(m, copy, kMessageBase);
        for (std::uint32_t o = 4; o < 0x20; o += 4) w32(m, copy + o, r32(m, msg + o));
        w8(m, copy + 0x20, r8(m, msg + 0x20));
        w32(m, copy, kActionMessage);
        w32(m, copy + 0x24, r32(m, msg + 0x24));
        w8(m, copy + 0x28, r8(m, msg + 0x28));
        w32(m, copy, kDeathActionMessage);
        w32(m, copy + 0x2c, r32(m, msg + 0x2c));
    }
    return copy;
}

std::uint32_t death_action_serialise_003e6270(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg, std::uint32_t key,
                                              std::uint32_t typeptr, std::uint32_t cap, std::uint32_t buf) {
    if (cap < 0x10 || !buf) return 0xffffffffU;
    action_header_003ff330(m, host, msg, key, 0, cap, buf);
    wf(m, buf + 0xc, static_cast<float>(static_cast<std::int32_t>(r32(m, msg + 0x2c))));
    if (!typeptr) return 0xffffffffU;
    w32(m, typeptr, r32(m, kDeathActionSerialType));
    return 0x10;
}

std::uint32_t death_action_receive_003e4f70(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t buf, std::uint32_t stack) {
    const auto sp = stack - 0x60, msg = sp + 0x20;
    busy_enter(m);
    if (r8(m, kInLevel)) {
        message_construct_received_003c9db0(m, host, msg, kActionType, buf);
        w32(m, msg, kActionMessage);
        w32(m, msg + 0x24, r8(m, buf + 8));
        w8(m, msg + 0x28, r8(m, buf + 9));
        w32(m, msg, kDeathActionMessage);
        w32(m, msg + 0x2c, cvt_w_s(std::bit_cast<float>(r32(m, buf + 0xc))));
        if (const auto entry = host.call(m, 0x3fbb10, {r32(m, buf + 4)}))
            host.call(m, 0x3c9350, {msg, r32(m, entry), 0, 0});
        w32(m, msg, kActionMessage);
        host.call(m, 0x3c9d60, {msg, 0});
    }
    w8(m, kBusy, 0);
    return 0x10;
}

// ---- the generator message "Generator Message" (vtable 0x004db600; checkpoint 132ln5h13; NET_INLEVEL.md 4.23) -----
namespace {
constexpr std::uint32_t kGeneratorReleaseMessage = 0x4db600, kGeneratorMessageBase = 0x4db660;
constexpr std::uint32_t kGeneratorBaseType = 0x4a4a70, kGeneratorBaseSerialType = 0x4f56c8;
}  // namespace

std::uint32_t generator_release_message_destroy_00157d20(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                                         std::uint32_t flag) {
    if (!msg) return msg;
    w32(m, msg, kGeneratorReleaseMessage);
    w32(m, msg, kGeneratorMessageBase);
    host.call(m, 0x3c9d60, {msg, 0});
    if (static_cast<std::int16_t>(flag & 0xffffU) > 0) host.call(m, 0x100740, {msg, 0});  // $a1 still 003c9d60's 0
    return msg;
}

std::uint32_t generator_release_message_clone_00222410(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg) {
    heap_enter(m, host);
    const auto copy = host.call(m, 0x36b180, {0, 0x2c, 0x4a4c88, 0xe3});
    heap_leave(m, host);
    if (copy) {
        w32(m, copy, kMessageBase);
        for (std::uint32_t o = 4; o < 0x20; o += 4) w32(m, copy + o, r32(m, msg + o));
        w8(m, copy + 0x20, r8(m, msg + 0x20));
        w32(m, copy, kGeneratorMessageBase);
        w32(m, copy + 0x24, r32(m, msg + 0x24));
        w32(m, copy, kGeneratorReleaseMessage);
        w8(m, copy + 0x28, r8(m, msg + 0x28));
    }
    return copy;
}

std::uint32_t generator_release_message_serialise_0021d370(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t msg,
                                                           std::uint32_t key, std::uint32_t typeptr, std::uint32_t cap,
                                                           std::uint32_t buf) {
    if (cap < 0x14 || !buf) return 0xffffffffU;
    std::uint32_t result = 0xffffffffU;
    message_header_003c9ca0(m, host, msg, key, cap, buf);
    w32(m, buf + 0xc, r32(m, r32(m, msg + 0x24) + 0xc));
    w8(m, buf + 8, 0);
    if (typeptr) {
        result = 0x14;
        w32(m, typeptr, r32(m, kGeneratorBaseSerialType));
    }
    w16(m, buf + 0x10, r8(m, msg + 0x28));
    w8(m, buf + 8, 3);
    return result;
}

std::uint32_t generator_base_message_receive_0021d6c0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t buf,
                                                      std::uint32_t stack) {
    const auto sp = stack - 0x120;
    busy_enter(m);
    if (r8(m, kInLevel)) {
        w32(m, sp + 0x118, r32(m, buf + 4));
        const auto node = rb_map_find(m, kNetMap, r32(m, sp + 0x118));  // 001bb080; 0021d360 / 0021d350: the end
        const auto kind = r8(m, buf + 8);
        if (node != kNetMap + 4 && kind <= 3) {
            static constexpr std::uint32_t kSlots[4] = {0xd0, 0xa0, 0x70, 0x40};
            static constexpr std::uint32_t kKeys[4] = {0x10c, 0x108, 0x104, 0x100};
            static constexpr std::uint32_t kVtables[4] = {kGeneratorMessageBase, 0x4db640, 0x4db620,
                                                          kGeneratorReleaseMessage};
            const auto msg = sp + kSlots[kind];
            host.call(m, 0x3c9e30, {msg, kGeneratorBaseType});
            w32(m, msg, kGeneratorMessageBase);
            w32(m, sp + kKeys[kind], r32(m, buf + 0xc));
            w32(m, msg + 0x24, host.call(m, 0x40c540, {sp + kKeys[kind]}));
            w32(m, msg, kVtables[kind]);
            if (kind == 3) w8(m, msg + 0x28, m.read(buf + 0x10, 2));
            host.call(m, 0x3c9350, {msg, r32(m, node + 0x10), 0, 0});
            w32(m, msg, kGeneratorMessageBase);
            host.call(m, 0x3c9d60, {msg, 0});
        }
    }
    w8(m, kBusy, 0);
    return 0x14;
}

// ---- an actor's NetMap registration and its NPC info owner (checkpoint 132ln5h12; NET_INLEVEL.md 4.22) ---------
namespace {
constexpr std::uint32_t kNpcInfoObject = 0x55c988;  // NetObj.cc's NPC info object (003fb340 makes it, 00436c00)
}  // namespace

std::uint32_t npc_info_owner_004343e0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t obj, std::uint32_t key,
                                      std::uint32_t stack) {
    const auto keyp = stack - 0x20 + 0x1c;
    w32(m, keyp, key);
    host.call(m, 0x4355d0, {obj});
    const auto found = host.call(m, 0x4361c0, {keyp, 0});
    return found && r8(m, found + 4) != 0xffU ? r8(m, found + 4) : 0xffffffffU;
}

std::uint32_t npc_owned_locally_003fbc40(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t key) {
    const auto local = local_slot(m, host, r8(m, kNetworkGame), r32(m, kWorld));
    return host.call(m, 0x4343e0, {r32(m, kNpcInfoObject), key}) == local ? 1U : 0U;
}

std::uint32_t net_map_client_003fb340(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t key) {
    const auto slot = host.call(m, 0x3fbb10, {key});
    if (!slot) return 0xffffffffU;
    const auto object = r32(m, slot);
    const auto controller = r32(m, object + 0x58);
    if (!controller || r8(m, object + 0x49) != 2) return r32(m, slot + 4);
    const auto type = [&] { return host.call(m, r32(m, r32(m, r32(m, object + 0x58)) + 0x88), {r32(m, object + 0x58)}) & 0xffU; };
    if (type() != 2 && type() != 1) return r32(m, slot + 4);
    if (!r32(m, kNpcInfoObject)) {
        heap_enter(m, host);
        auto made = host.call(m, 0x36b180, {0, 1, 0x4be228, 0x22f});
        heap_leave(m, host);
        if (made) made = host.call(m, 0x436c00, {made});
        w32(m, kNpcInfoObject, made);
    }
    return host.call(m, 0x4343e0, {r32(m, kNpcInfoObject), key});
}

void actor_net_register_001856d0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t actor) {
    const auto human = [&] {
        const auto controller = r32(m, actor + 0x58);
        return controller && host.call(m, r32(m, r32(m, controller) + 0x54), {controller}) != 0;
    };
    human();
    w32(m, actor + 0x10, r32(m, actor + 0xc));
    if (human() || host.call(m, 0x3fbc40, {r32(m, actor + 0x10)})) {
        if (r8(m, actor + 0x2d) && r8(m, actor + 0x2c) && !r8(m, actor + 0x2e))
            host.call(m, 0x2d1ff0, {actor + 0xc, r32(m, actor + 0x10)});
    }
    const auto key = r32(m, actor + 0x10);
    host.call(m, 0x3fb650, {key, local_slot(m, host, r8(m, kNetworkGame), r32(m, kWorld)), actor});
}

}  // namespace omega
