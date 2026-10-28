#include "dme_library.hpp"

#include <bit>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace omega {
namespace {
using u32 = std::uint32_t;

constexpr u32 kLock = 0x5253f0;        // the library lock 003021d0 creates
constexpr u32 kGateUp = 0x48c4d7;      // byte: 002e94e0 succeeded (the init parameters' version, 1)
constexpr u32 kGateVersion = 0x48c4dc; // byte: the version 002e94e0 is starting / started
constexpr u32 kRefCount = 0x48c4d8;
constexpr u32 kWorldUp = 0x48c458;     // 002df290 ran
constexpr u32 kAllocator = 0x4908e0;
constexpr u32 kRegistryUp = 0x4912e8;  // byte: 003057f8 ran
constexpr u32 kRegCallbacks = 0x4912f0, kRegCounts = 0x4912f4, kRegBuffer = 0x4912ec, kRegSpare = 0x4912f8;
constexpr u32 kRegMax = 0x529808, kRegCategories = 0x52980c, kRegBufferSize = 0x529810;
constexpr u32 kFieldsOpen = 0x48c4e0;  // fields since 002ed000 (the class being described)
constexpr u32 kFieldCount = 0x58df6c;  // field records at 0x00590358 (0x20 bytes each, 0x400)
constexpr u32 kClassCount = 0x58df70;  // class records at 0x0058df98 (0x228 bytes each, 0x10)
constexpr u32 kFields = 0x590358;
constexpr u32 kClasses = 0x58df98;
constexpr u32 kClassTypes = 0x58bd18;  // the class records indexed by a field's type (type 0x10 = class 0)
constexpr u32 kOpenFieldIndex = 0x525408;
constexpr u32 kCurrentWorld = 0x48c450;  // the world the barrier 002e15e0 started (its parameters +4)
constexpr u32 kHandleTop = 0x48c464;     // one past the highest handle in use
constexpr u32 kSlots = 0x48c4a8;         // the second handle space (0x800 entries of 0x44 bytes, made on first use)
constexpr u32 kWorlds = 0x51fed8;        // the four worlds (0x450 bytes each, 002e51f0)
constexpr u32 kRecords = 0x51f0d0;       // the client records' table (0x100 pointers to 0x1c-byte records)
constexpr u32 kSessionClock = 0x58806c;  // the session clock (002dd2d8 sets it, 002ddb08 reads it)
constexpr u32 kObjects = 0x5880b0;       // the network objects by handle (0x1000)
constexpr u32 kStatus = 0x58c178;        // the handles' status bytes (0 free, 1 reserved, 2 in use)

u32 r8(const LevelLoadMemory& m, u32 a) { return m.read(a, 1); }
u32 r16(const LevelLoadMemory& m, u32 a) { return m.read(a, 2); }
u32 r32(const LevelLoadMemory& m, u32 a) { return m.read(a, 4); }
void w8(LevelLoadMemory& m, u32 a, u32 v) { m.write(a, 1, v & 0xffU); }
void w16(LevelLoadMemory& m, u32 a, u32 v) { m.write(a, 2, v & 0xffffU); }
void w32(LevelLoadMemory& m, u32 a, u32 v) { m.write(a, 4, v); }
std::int32_t s32(u32 v) { return static_cast<std::int32_t>(v); }

// 001260f0
void memset_001260f0(LevelLoadMemory& m, u32 p, u32 v, u32 n) {
    for (u32 k = 0; k < n; ++k) w8(m, p + k, v);
}
// 00129360 (the doubleword / quadword paths test for a zero byte before each store: byte-exact strcpy)
void strcpy_00129360(LevelLoadMemory& m, u32 d, u32 s) {
    for (u32 k = 0;; ++k) {
        const auto c = r8(m, s + k);
        w8(m, d + k, c);
        if (!c) break;
    }
}
// 00125f38
void memcpy_00125f38(LevelLoadMemory& m, u32 d, u32 s, u32 n) {
    for (u32 k = 0; k < n; ++k) w8(m, d + k, r8(m, s + k));
}
// 00129a70
void strncpy_00129a70(LevelLoadMemory& m, u32 d, u32 s, u32 n) {
    bool ended = false;
    for (u32 k = 0; k < n; ++k) {
        const auto c = ended ? 0U : r8(m, s + k);
        if (!c) ended = true;
        w8(m, d + k, c);
    }
}

struct Lib {
    LevelLoadMemory& m;
    LevelLoadHost& host;

    // The API wrappers' gate around `body` (002e9a10 .. 002e9f08, 002ec168).
    template <class F> u32 gate(F body) {
        const auto up = r8(m, kGateUp);
        if (up != 1) return 0x11;
        if (r8(m, kGateVersion) != up) return 0x2b;
        if (host.call(m, 0x302260, {kLock, 0})) return 0x2b;
        const auto r = body();
        if (host.call(m, 0x3023d8, {kLock, 0})) return 0x2b;
        return r;
    }

    // 002fb568 via 002ea4f0 -> 002dd358
    u32 allocator_002ea4f0(u32 alloc, u32 release) {
        w32(m, kAllocator, alloc);
        w32(m, kAllocator + 4, release);
        return 0;
    }

    // 002e9480
    u32 init_params_002e9480(u32 p) {
        if (!p) return 0x17;
        memset_001260f0(m, p, 0, 0x1c);
        const auto version = r32(m, 0x497e50);  // 00476998
        w32(m, p, 1);
        w32(m, p + 0xc, 0xe4a);
        w32(m, p + 0x18, 0);
        w32(m, p + 8, 0);
        w32(m, p + 0x14, 0);
        w32(m, p + 4, version);
        return 0;
    }

    // 002e9320
    u32 version_002e9320(u32 out) {
        if (!out) return 0x17;
        strcpy_00129360(m, out, 0x48c4cd);
        return 0;
    }

    // 002fb2d8
    u32 alloc_002fb2d8(u32 p, u32 size) {
        const auto cb = r32(m, kAllocator);
        const auto got = host.call(m, cb ? cb : 0x125368, {size});
        w32(m, p, got);
        return got ? 0 : 1;
    }

    // 002fb350
    u32 alloc_zeroed_002fb350(u32 p, u32 size) {
        if (!p) return 1;
        w32(m, p, 0);
        if (const auto r = alloc_002fb2d8(p, size)) return r;
        const auto got = r32(m, p);
        if (!got) return 1;
        memset_001260f0(m, got, 0, size);
        return 0;
    }

    // 002dd2e8
    u32 alloc_002dd2e8(u32 size, u32 stack) {
        const auto sp = stack - 0x20;
        w32(m, sp, 0);
        if (alloc_zeroed_002fb350(sp, size)) host.call(m, 0x127358, {0x4adb30, size});
        return r32(m, sp);
    }

    // 003057f8
    u32 registry_init_003057f8(u32 buffer, u32 max, u32 categories) {
        if (r8(m, kRegistryUp)) return 1;
        if (max >= 0x101) return 2;
        if (categories >= 0x101) return 2;
        w32(m, kRegMax, max);
        w32(m, kRegCategories, categories);
        w32(m, kRegBufferSize, buffer);
        if (alloc_zeroed_002fb350(kRegCounts, categories * 4)) return 6;
        const auto table = categories * max * 4;
        if (alloc_zeroed_002fb350(kRegCallbacks, table)) return 6;
        if (alloc_zeroed_002fb350(kRegBuffer, buffer)) return 6;
        if (alloc_zeroed_002fb350(kRegSpare, table)) return 6;
        w8(m, kRegistryUp, 1);
        return 0;
    }

    // 00305998
    u32 registry_add_00305998(u32 out, u32 category, u32 callback) {
        if (r8(m, kRegistryUp) != 1) return 3;
        const auto callbacks = r32(m, kRegCallbacks);
        if (!callbacks) return 3;
        const auto counts = r32(m, kRegCounts);
        if (!counts) return 3;
        if (!out) return 2;
        if (category >= r32(m, kRegCategories)) return 2;
        const auto slot = counts + category * 4;
        const auto count = r32(m, slot);
        if (r32(m, kRegMax) < count) return 5;
        w32(m, slot, count + 1);
        const auto max = r32(m, kRegMax);
        const auto table = r32(m, kRegCallbacks);
        w32(m, out, count);
        w32(m, (category * max + count) * 4 + table, callback);
        return 0;
    }

    // 002ed528 / 002ed550
    u32 message_002ed528(u32 out, u32 category, u32 callback) {
        return registry_add_00305998(out, category, callback) ? 0x1e : 0;
    }

    // 002e8a00: the library's own messages (category 0), their ids at 0x005983d8..; three slots on the stack.
    void own_messages_002e8a00(u32 stack) {
        const auto sp = stack - 0x20;
        static constexpr u32 kOwn[][2] = {
            {0x5983e8, 0x2e6bc0}, {0x5983dc, 0x2e7118}, {0x5983ec, 0x2e7328}, {0x5983f0, 0x2e7b60},
            {0x5983f4, 0x2e76d0}, {0x5983f8, 0x2e80d0}, {0, 0},               {0, 0},
            {0, 0},               {0x5983fc, 0x2e6f40}, {0x598400, 0x2e74c0}, {0x598404, 0x2e74e8},
            {0x5983e0, 0x2e7608}, {0x598408, 0x2e7670}, {0x59840c, 0x2e7dc8}, {0x598410, 0x2e6c78},
            {0x598414, 0x2e6aa8}, {0, 0},               {0x598418, 0x2e6c30}, {0x59841c, 0x2e6dc8},
            {0x5983d8, 0x2e6ed8}, {0x5983e4, 0x2e8228}, {0x598420, 0x2e8338}, {0x598424, 0x2e85b8},
            {0x598428, 0x2e86a8},
        };
        for (const auto& own : kOwn) message_002ed528(own[0] ? own[0] : sp, 0, own[1]);
    }

    // 002db140
    void table_002db140(u32 stack) {
        const auto p = alloc_002dd2e8(0x400, stack - 0x10);
        w32(m, 0x51f0d0, p);
        w8(m, 0x48c440, 0);
        memset_001260f0(m, p, 0, 0x400);
    }

    // 002df290
    u32 world_init_002df290(u32 params, u32 result, u32 stack) {
        const auto sp = stack - 0x30;
        if (!result) return 0x17;
        host.call(m, 0x3060c0, {sp});
        host.call(m, 0x302088, {r32(m, sp)});
        w32(m, result + 4, 0x12);
        w32(m, result, 1);
        if (const auto r = version_002e9320(result + 8)) {
            w32(m, result + 4, r);
            return r;
        }
        if (!params) {
            w32(m, result + 4, 0x17);
            return 0x17;
        }
        const auto version = r32(m, params);
        if (version != 1) {
            w32(m, result + 4, 0x26);
            return 0x17;
        }
        if (r32(m, kWorldUp)) {
            w32(m, result + 4, 0);
            return 0;
        }
        w32(m, kWorldUp, version);
        memset_001260f0(m, 0x51fed8, 0, 0x10);
        memset_001260f0(m, 0x51fee8, 0, 0x400);
        w32(m, 0x58d17c, 0);
        w32(m, 0x58d180, r32(m, params + 4));
        w32(m, 0x58806c, 0);  // 002dd2d8(0)
        if (registry_init_003057f8(0x600, 0x100, 6)) return 0x1e;
        for (u32 k = 0; k < 0x7f; ++k) w32(m, 0x58df5c - 0x1c * k, 0);  // 0x0058d188 + 0xdd4
        w32(m, 0x58df74, 1);
        w32(m, kFieldCount, 0);
        w32(m, kClassCount, 0);
        w32(m, 0x48c464, 0);
        w32(m, 0x58df78, 0);
        own_messages_002e8a00(sp);
        for (u32 k = 0; k < 0x1000; ++k) {
            w8(m, 0x58c178 + k, 0);
            w32(m, 0x5880b0 + 4 * k, 0);
        }
        for (u32 k = 0; k < 0x10; ++k) {
            w32(m, 0x58c0b8 + 4 * k, 0);
            w32(m, 0x58c0f8 + 4 * k, 0);
            w32(m, 0x58c138 + 4 * k, 0);
        }
        table_002db140(sp);
        memset_001260f0(m, 0x5227f0, 0, 0x2c00);  // 002e92f0 (returns 0)
        const auto key = r32(m, params + 8);
        if (key) {
            for (u32 k = 0; k < 0x80; ++k) w8(m, 0x522760 + k, r8(m, key + k));
            w8(m, 0x48c4b5, 1);
        } else {
            memset_001260f0(m, 0x522760, 0, 0x80);
            w8(m, 0x48c4b5, 0);
        }
        if (const auto r = host.call(m, 0x2fb788, {r32(m, params + 8)})) {
            host.call(m, 0x127358, {0x4adba0, r});
            w32(m, result + 4, 0x12);
            return 0x12;
        }
        if (const auto r = host.call(m, 0x2fc088, {0x58808c})) host.call(m, 0x127358, {0x4adbd0, r});
        strncpy_00129a70(m, 0x588074, 0x58808c, 0x10);
        const auto port = r16(m, params + 0xc);
        w16(m, 0x58c0b0, port);
        w32(m, 0x588070 + 0x14, port);
        w32(m, 0x588070, 1);
        w32(m, 0x588070 + 0x18, 2);
        w32(m, 0x588070 + 0x2c, port);
        w32(m, result + 4, 0);
        memset_001260f0(m, 0x520300, 0, 0x38);
        memset_001260f0(m, 0x520358, 0, 0x2400);
        w8(m, 0x48c4a4, 0);
        if (const auto r = host.call(m, 0x314380, {})) return r;
        w32(m, 0x48c490, r32(m, params + 0x14));
        w32(m, 0x48c494, r32(m, params + 0x18));
        const auto heap = r32(m, params + 0x10);
        w32(m, 0x48c460, heap ? heap : 0x80000);
        return 0;
    }

    // 00314380: the socket layer's reference-counted init (the first call clears 0x0052ac98 and sets 0x00491430).
    u32 sockets_00314380() {
        if (!r16(m, 0x491432)) {
            memset_001260f0(m, 0x52ac98, 0, 0x10);
            w8(m, 0x491430, 1);
        }
        w16(m, 0x491432, r16(m, 0x491432) + 1);
        return 0;
    }

    // 002e94e0
    u32 start_002e94e0(u32 params, u32 result, u32 stack) {
        if (!result) return 0x17;
        w32(m, result + 4, 0x12);
        w32(m, result, 1);
        if (!params) {
            w32(m, result + 4, 0x17);
            return 0x17;
        }
        const auto version = r32(m, params);
        if (version != 1) {
            w32(m, result + 4, 0x26);
            return 0x17;
        }
        if (const auto refs = r32(m, kRefCount)) {
            w32(m, kRefCount, refs + 1);
            w32(m, result + 4, 0);
            return 0;
        }
        if (r8(m, kGateVersion) == version || host.call(m, 0x3021d0, {kLock})) {
            w32(m, result + 4, 0x2b);
            return 0x2b;
        }
        w8(m, kGateVersion, version);
        if (host.call(m, 0x302260, {kLock, 0})) return 0x2b;
        const auto r = world_init_002df290(params, result, stack - 0x40);
        if (host.call(m, 0x3023d8, {kLock, 0})) return 0x2b;
        if (r) {
            host.call(m, 0x302218, {kLock});
            w8(m, kGateVersion, 0);
            w8(m, kGateUp, 0);
            return r;
        }
        w32(m, kRefCount, version);
        w8(m, kGateUp, version);
        return 0;
    }

    // 002ed000
    u32 fields_begin_002ed000() {
        if (!r32(m, kWorldUp)) return 0x11;
        w32(m, kFieldsOpen, 0);
        return 0;
    }

    // 002ed028
    u32 field_002ed028(u32 out, u32 offset, u32 size, u32 type, u32 count, u32 options) {
        if (!r32(m, kWorldUp)) return 0x11;
        if (!out) return 0x17;
        if (s32(size) <= 0) return 0x17;
        if (s32(count) <= 0) return 0x17;
        const auto open = r32(m, kFieldsOpen);
        if (s32(open) >= 0x40) return 8;
        const auto total = r32(m, kFieldCount);
        if (total >= 0x400) return 8;
        u32 bytes = 0;  // LO of the madd chain: the open fields' size * count, then this one's
        if (s32(open) > 0) {
            auto rec = kFields + (total - open) * 0x20;
            for (auto k = open; k != 0; --k, rec += 0x20) bytes += r32(m, rec + 4) * r32(m, rec + 8);
        }
        bytes += size * count;
        if (bytes >= 0x1db) return 0x1f;
        const auto rec = kFields + r32(m, kFieldCount) * 0x20;
        w32(m, rec, offset);
        w32(m, rec + 4, size);
        w32(m, rec + 0xc, type);
        w32(m, rec + 8, count);
        if (options) {
            for (u32 k = 0; k < 0x10; ++k) w8(m, rec + 0x10 + k, r8(m, options + k));
        }
        const auto index = r32(m, kFieldCount);
        const auto field = r32(m, kFieldsOpen);
        w32(m, kOpenFieldIndex + field * 4, index);
        w32(m, out, field);
        w32(m, kFieldsOpen, r32(m, kFieldsOpen) + 1);
        w32(m, kFieldCount, r32(m, kFieldCount) + 1);
        return 0;
    }

    // One field into the class record: its address (+0x128) and its record (+0x28).
    void class_field(u32 cls, u32 address, u32 field) {
        const auto n = r32(m, cls + 4);
        w32(m, cls + 0x128 + 4 * n, address);
        w32(m, cls + 4, n + 1);
        w32(m, cls + 0x28 + 4 * n, field);
    }

    // 002ed168: the fields of a nested class type at `base`.
    void nested_002ed168(u32 cls, u32 type, u32 base) {
        if (!cls || !type) return;
        if (s32(r32(m, type + 4)) <= 0) return;
        for (u32 k = 0;; ++k) {
            const auto field = r32(m, type + 0x28 + 4 * k);
            if (s32(r32(m, cls + 4)) >= 0x40) return;
            const auto t = r32(m, field + 0xc);
            if (s32(t) >= 0x10) nested_002ed168(cls, kClassTypes + t * 0x228, base + r32(m, field));
            else class_field(cls, base + r32(m, field), field);
            if (s32(k + 1) >= s32(r32(m, type + 4))) return;
        }
    }

    // 002ed280
    u32 class_002ed280(u32 out, u32 name, u32 size) {
        if (!r32(m, kWorldUp)) return 0x11;
        if (!out || !name) return 0x17;
        if (s32(size) <= 0) return 0x17;
        const auto index = r32(m, kClassCount);
        if (index >= 0x10) return 8;
        const auto cls = kClasses + index * 0x228;
        w32(m, cls, size);
        w32(m, cls + 4, 0);
        strncpy_00129a70(m, cls + 8, name, 0x20);
        if (s32(r32(m, kFieldsOpen)) > 0) {
            for (u32 k = 0;; ++k) {
                if (s32(r32(m, cls + 4)) >= 0x40) return 8;
                const auto field = kFields + r32(m, kOpenFieldIndex + 4 * k) * 0x20;
                const auto t = r32(m, field + 0xc);
                if (s32(t) >= 0x10) nested_002ed168(cls, kClassTypes + t * 0x228, r32(m, field));
                else class_field(cls, r32(m, field), field);
                if (s32(k + 1) >= s32(r32(m, kFieldsOpen))) break;
            }
        }
        w32(m, out, r32(m, kClassCount));
        w32(m, kClassCount, r32(m, kClassCount) + 1);
        return 0;
    }

    // 002ed570: the class's callbacks: create (0x0058c0b8, 002de8d8), owner change (0x0058c0f8, 002de950) and
    // remove (0x0058c138, 002de9f0).
    u32 class_callbacks_002ed570(u32 index, u32 create, u32 owner, u32 remove) {
        if (!r32(m, kWorldUp)) return 0x11;
        if (index >= 0x10) return 0x17;
        w32(m, 0x58c138 + 4 * index, remove);
        w32(m, 0x58c0b8 + 4 * index, create);
        w32(m, 0x58c0f8 + 4 * index, owner);
        return 0;
    }

    // ---- the worlds, the client records and the object handles (checkpoint 132ln5c) -------------------------
    // 002fb3c0: the release through the allocator's release callback (0x004908e4; none: the game's free 001253c0).
    u32 release_002fb3c0(u32 p) {
        const auto block = r32(m, p);
        if (!block) return 0;
        const auto cb = r32(m, kAllocator + 4);
        host.call(m, cb ? cb : 0x1253c0, {block});
        w32(m, p, 0);
        return 0;
    }

    // 002dd328
    u32 free_002dd328(u32 p) { return r32(m, p) ? release_002fb3c0(p) : 0U; }

    // 002e51b8: the world pointer (0x0051fed8, four worlds).
    u32 world_002e51b8(u32 world, u32 out) {
        if (!out) return 0x17;
        w32(m, out, 0);
        if (world >= 4) return 0xf;
        w32(m, out, r32(m, kWorlds + 4 * world));
        return 0;
    }

    // 002e51f0: the world, created (0x450 bytes) on the first call.
    u32 world_create_002e51f0(u32 world, u32 out, u32 stack) {
        if (!out) return 0x17;
        w32(m, out, 0);
        if (world >= 4) return 0x17;
        const auto slot = kWorlds + 4 * world;
        if (!r32(m, slot)) {
            const auto p = alloc_002dd2e8(0x450, stack - 0x20);
            w32(m, slot, p);
            if (!p) return 0x16;
        }
        w32(m, out, r32(m, slot));
        return 0;
    }

    // 002db188: the client's record (0x0051f0d0 table, the current world 0x0048c450 only).
    u32 record_002db188(u32 out, u32 world, u32 client) {
        if (!out) return 0x17;
        w32(m, out, 0);
        if (client >= 0x100) return 0x17;
        if (world >= 4) return 0x17;
        if (world != r32(m, kCurrentWorld)) return 0;
        const auto table = r32(m, kRecords);
        if (!table) return 0x15;
        w32(m, out, r32(m, table + 4 * client));
        return 0;
    }

    // 002db270: a new 0x1c-byte client record into the table (no null checks in the original: a failed
    // allocation writes the fields at address 0).
    u32 record_new_002db270(u32 world, u32 client, u32 stack) {
        if (client >= 0x100) return 0;  // 002e6088
        if (world >= 4) return 0;
        const auto rec = alloc_002dd2e8(0x1c, stack - 0x20);
        const auto table = r32(m, kRecords);
        w32(m, rec, 0);
        w16(m, rec + 4, 0);
        w32(m, table + 4 * client, rec);
        w16(m, rec + 6, 0);
        w16(m, rec + 8, 0);
        w16(m, rec + 0xa, 0);
        w16(m, rec + 0xc, 0);
        w8(m, rec + 0xe, 0);
        return rec;
    }

    // 002dd9f8: the second handle space's entry (0x800 entries of 0x44 bytes at *0x0048c4a8, made on first use).
    u32 slot_002dd9f8(u32 index, u32 out, u32 stack) {
        if (!out) return 0x17;
        w32(m, out, 0);
        if (index >= 0x800) return 8;
        if (!r32(m, kSlots)) {
            const auto p = alloc_002dd2e8(0x22000, stack - 0x30);
            w32(m, kSlots, p);
            if (!p) return 0x16;
            for (u32 k = 0; k < 0x800; ++k) w8(m, r32(m, kSlots) + 0x44 * k, 0);
        }
        w32(m, out, r32(m, kSlots) + index * 0x44);
        return 0;
    }

    // 002dd9b0: as 002dd9f8 into its own zeroed slot; *out keeps its value unless an entry came back.
    u32 slot_002dd9b0(u32 out, u32 index, u32 stack) {
        const auto sp = stack - 0x20;
        w32(m, sp, 0);
        if (const auto r = slot_002dd9f8(index, sp, sp)) return r;
        if (const auto p = r32(m, sp)) w32(m, out, p);
        return 0;
    }

    // 002dd9b0 into a caller's stack slot the caller never initialised (002db470, 002e69b8): the first lookup
    // that brings back no entry would leave the stack's old contents in place (only after a failed allocation).
    u32 slot_into_frame(u32 slot, bool& defined, u32 index, u32 stack, u32 caller) {
        const auto r = slot_002dd9b0(slot, index, stack);
        if (r32(m, stack - 0x20)) defined = true;
        if (!defined) {
            char text[96];
            std::snprintf(text, sizeof text, "%08x: the second handle space could not be allocated (an undefined stack slot)", caller);
            throw std::runtime_error(text);
        }
        return r;
    }

    // 002db470: the record's handles marked in use (status 1 where 0), then its second-space entries.
    void mark_002db470(u32 rec, u32 stack) {
        if (!rec) return;
        if (r16(m, rec + 6) < 0x1000) {
            for (u32 k = 0; k < r16(m, rec + 8); ++k) {
                const auto at = kStatus + k + r16(m, rec + 6);
                if (!r8(m, at)) w8(m, at, 1);
            }
        }
        if (r16(m, rec + 0xa) >= 0x800) return;
        const auto sp = stack - 0x30;
        bool defined = false;
        for (u32 k = 0; k < r16(m, rec + 0xc); ++k) {
            slot_into_frame(sp, defined, k + r16(m, rec + 0xa), sp, 0x2db470);
            if (const auto p = r32(m, sp)) w8(m, p, 1);
        }
    }

    // 002e68c0: the first run of `count` free handles (status 0), marked 1; 0x100 for a count of 0x1000 or more.
    u32 handle_base_002e68c0(u32 count) {
        if (count >= 0x1000) return 0x100;
        const auto limit = s32(0x1000 - count);
        for (std::int32_t t = 0; t < limit;) {
            if (r8(m, kStatus + static_cast<u32>(t))) { ++t; continue; }
            const auto end = t + s32(count);
            bool free = true;
            for (auto k = t + 1; k < end; ++k)
                if (r8(m, kStatus + static_cast<u32>(k))) { free = false; break; }
            if (!free) { ++t; continue; }
            for (auto k = t; k < end; ++k) w8(m, kStatus + static_cast<u32>(k), 1);
            return static_cast<u32>(t);
        }
        return 0xffffffffU;
    }

    // 002e69b8: the first run of `count` free second-space entries (byte 0 clear); not marked.
    u32 second_base_002e69b8(u32 count, u32 stack) {
        if (count >= 0x800) return 0xffffffffU;
        const auto limit = s32(0x800 - count);
        const auto sp = stack - 0x50;
        bool defined = false;
        for (std::int32_t t = 0; t < limit;) {
            slot_into_frame(sp, defined, static_cast<u32>(t), sp, 0x2e69b8);
            if (r8(m, r32(m, sp))) { ++t; continue; }
            bool free = true;
            for (auto k = t + 1; k < t + s32(count); ++k) {
                slot_into_frame(sp, defined, static_cast<u32>(k), sp, 0x2e69b8);
                if (r8(m, r32(m, sp))) { free = false; break; }
            }
            if (free) return static_cast<u32>(t);
            ++t;
        }
        return 0xffffffffU;
    }

    // 002dd968: the object at a handle (0x005880b0).
    u32 object_002dd968(u32 out, u32 handle) {
        if (!out) return 0x17;
        if (handle >= 0x1000) {
            w32(m, out, 0);
            return 8;
        }
        w32(m, out, r32(m, kObjects + 4 * handle));
        return 0;
    }

    // 002de880: a game message of `size` bytes at `data` to client `to` (-1: every client), category `kind`
    // (sign-extended byte), message id `id` (< 0x100). The checks, then a tail jump to the send 002de398.
    u32 send_002de880(u32 kind, u32 world, u32 to, u32 id, u32 size, u32 data) {
        const auto k = static_cast<u32>(static_cast<std::int32_t>(static_cast<std::int8_t>(kind & 0xff)));
        if (id >= 0x100) return 0x17;
        if (s32(size) <= 0 || !data) return 0x17;
        return host.call(m, 0x2de398, {k, world, to, 0, id, size, data});
    }

    // 002de950: the owner-change callback of the object's class (0x0058c0f8 by the object's class byte) with
    // (owner, handle, a2); a2 is a client (< 0x40), -2 or -1. Frame 0x30 below `stack`.
    void owner_callback_002de950(u32 owner, u32 handle, u32 a2, u32 stack) {
        if (handle >= 0x1000) return;
        if (a2 >= 0x40 && a2 != 0xfffffffeU && a2 != 0xffffffffU) return;
        const auto sp = stack - 0x30;
        object_002dd968(sp, handle);
        const auto obj = r32(m, sp);
        if (!obj) return;
        const auto cb = r32(m, 0x58c0f8 + 4 * r8(m, obj));
        if (cb) host.call(m, cb, {owner, handle, a2});
    }

    // 002e2bb8: the ownership request (via 002ec3c8). The local client's own object (obj+8): owner +4 = it,
    // 002de950, and message *0x005983e0 {client byte, -, u16 handle} to everyone; another client's object: the
    // request message *0x00598404 {u16 handle} to its master. Frame 0x40 below `stack`.
    u32 own_002e2bb8(u32 handle, u32 stack) {
        const auto sp = stack - 0x40;
        w32(m, sp + 0x14, 0);
        w32(m, sp + 0x10, 0);
        if (handle >= 0x1000) return 0x17;
        if (r32(m, kCurrentWorld) >= 4) return 8;
        if (const auto r = world_002e51b8(r32(m, kCurrentWorld), sp + 0x10)) return r;
        if (!r32(m, sp + 0x10)) return 0;
        object_002dd968(sp + 0x14, handle);
        const auto obj = r32(m, sp + 0x14);
        if (!obj) return 8;
        const auto master = r32(m, obj + 8);
        if (master != r32(m, r32(m, sp + 0x10) + 0x400)) {
            if (r32(m, obj + 4) != 0xffffffffU) return 0xb;
            w16(m, sp + 0x18, handle);
            send_002de880(0x50, r32(m, kCurrentWorld), master, r32(m, 0x598404), 2, sp + 0x18);
            return 0;
        }
        if (r32(m, obj + 4) != 0xffffffffU) return 0xb;
        w32(m, obj + 4, master);
        owner_callback_002de950(r32(m, obj + 4), handle, 0xfffffffeU, sp);
        w16(m, sp + 2, handle);
        w8(m, sp, r32(m, obj + 4) & 0xff);  // 002e6090
        send_002de880(0x50, r32(m, kCurrentWorld), 0xffffffffU, r32(m, 0x5983e0), 4, sp);
        return 0;
    }

    // 002e2ce0: the ownership release (via 002ec460) of an object the local client owns: owner +4 = -1,
    // 002de950, 002e31d0 (h, 0x50, 1), and message *0x00598408 {u16 handle} to everyone. Frame 0x30.
    u32 release_002e2ce0(u32 handle, u32 stack) {
        const auto sp = stack - 0x30;
        w32(m, sp + 4, 0);
        w32(m, sp, 0);
        if (handle >= 0x1000) return 0x17;
        if (r32(m, kCurrentWorld) >= 4) return 8;
        if (const auto r = world_002e51b8(r32(m, kCurrentWorld), sp)) return r;
        if (!r32(m, sp)) return 0;
        object_002dd968(sp + 4, handle);
        const auto obj = r32(m, sp + 4);
        if (!obj) return 8;
        if (r32(m, obj + 4) != r32(m, r32(m, sp) + 0x400)) return 8;
        w32(m, obj + 4, 0xffffffffU);
        owner_callback_002de950(r32(m, obj + 4), handle, 0xfffffffeU, sp);
        w16(m, sp + 8, handle);
        host.call(m, 0x2e31d0, {handle, 0x50, 1});
        send_002de880(0x50, r32(m, kCurrentWorld), 0xffffffffU, r32(m, 0x598408), 2, sp + 8);
        return 0;
    }

    // ---- the object field flush 002e31d0 (checkpoint 132ln5g) ----------------------------------------------------
    // A field record (0x20 bytes, 002ed028): +0 offset, +4 size, +8 count, +0xc type; the options +0x10 the send
    // interval, +0x14 the mode (0: only when marked dirty, 1: any change, 5: the callback +0x18; others: the
    // threshold +0x18, a float), +0x1c the message kind bits.
    float f32_at(u32 a) const { return std::bit_cast<float>(r32(m, a)); }
    double f64_at(u32 a) const {
        return std::bit_cast<double>(static_cast<std::uint64_t>(r32(m, a)) | static_cast<std::uint64_t>(r32(m, a + 4)) << 32);
    }
    // cvt.w.s, and the unsigned conversion the compiler builds from it (2^31 and up: the high bit or'ed back).
    static u32 to_int(float f) { return static_cast<u32>(static_cast<std::int32_t>(f)); }
    static u32 to_uint(float f) {
        constexpr float k2p31 = 2147483648.0f;
        return k2p31 <= f ? to_int(f - k2p31) | 0x80000000U : to_int(f);
    }

    // 002e3830: one element changed past the field's threshold (cur `a`, last sent `b`; the jump table 0x004adc40
    // by the type). Types 0..5 the integers (an absolute difference, wrapped to the type's width; type 5 none),
    // mode 1 any difference; 6 a float; 7 a double (the soft-float 0011c200 / 0011c750); 8 / 9 two / three int16,
    // 0xa / 0xb two / three int32, 0xc / 0xd two / three floats, 0xe / 0xf two / three doubles: the squared
    // distance above the threshold squared, whatever the mode.
    u32 element_changed_002e3830(u32 field, u32 a, u32 b) {
        if (!field || !a || !b) return 0;
        const auto type = r32(m, field + 0xc);
        if (type >= 0x10) return 0;
        const bool any = r32(m, field + 0x14) == 1;
        const auto thr = f32_at(field + 0x18);
        const auto sx8 = [](u32 v) { return static_cast<std::int32_t>(static_cast<std::int8_t>(v & 0xff)); };
        const auto sx16 = [](u32 v) { return static_cast<std::int32_t>(static_cast<std::int16_t>(v & 0xffff)); };
        switch (type) {
        case 0: {
            auto d = sx8(r8(m, a)) - sx8(r8(m, b));
            if (d < 0) d = sx8(r8(m, b)) - sx8(r8(m, a));
            const auto v = sx8(static_cast<u32>(d));
            if (any) return v != 0;
            return sx8(to_int(thr)) < v;
        }
        case 1: {
            const auto d = static_cast<std::int32_t>(r8(m, a) - r8(m, b));
            const auto v = (d >= 0 ? static_cast<u32>(d) : r8(m, b) - r8(m, a)) & 0xffU;
            if (any) return v != 0;
            return (to_uint(thr) & 0xffU) < v;
        }
        case 2: {
            auto d = sx16(r16(m, a)) - sx16(r16(m, b));
            if (d < 0) d = sx16(r16(m, b)) - sx16(r16(m, a));
            const auto v = sx16(static_cast<u32>(d));
            if (any) return v != 0;
            return sx16(to_int(thr)) < v;
        }
        case 3: {
            const auto d = static_cast<std::int32_t>(r16(m, a) - r16(m, b));
            const auto v = (d >= 0 ? static_cast<u32>(d) : r16(m, b) - r16(m, a)) & 0xffffU;
            if (any) return v != 0;
            return (to_uint(thr) & 0xffffU) < v;
        }
        case 4: {
            auto d = s32(r32(m, a) - r32(m, b));
            if (d < 0) d = s32(r32(m, b) - r32(m, a));
            if (any) return d != 0;
            return s32(to_int(thr)) < d;
        }
        case 5: {
            const auto d = r32(m, a) - r32(m, b);
            if (any) return d != 0;
            return to_uint(thr) < d;
        }
        case 6: {
            auto d = f32_at(a) - f32_at(b);
            if (d < 0.0f) d = -d;
            if (any) return d == 0.0f ? 0U : 1U;
            return thr < d;
        }
        case 7: {
            auto d = f64_at(a) - f64_at(b);
            if (d < 0.0) d = 0.0 - d;
            if (any) return d != 0.0;
            return d > static_cast<double>(thr);
        }
        case 8: case 0xa: {
            const bool w = type == 0xa;
            const auto at = [&](u32 p, u32 k) { return w ? s32(r32(m, p + 4 * k)) : sx16(r16(m, p + 2 * k)); };
            const auto dx = static_cast<u32>(at(a, 0) - at(b, 0)), dy = static_cast<u32>(at(a, 1) - at(b, 1));
            const auto sum = s32(dx * dx + dy * dy);
            return !(static_cast<float>(sum) <= thr * thr);
        }
        case 9: case 0xb: {
            const bool w = type == 0xb;
            const auto at = [&](u32 p, u32 k) { return w ? s32(r32(m, p + 4 * k)) : sx16(r16(m, p + 2 * k)); };
            const auto dx = static_cast<u32>(at(a, 0) - at(b, 0)), dy = static_cast<u32>(at(a, 1) - at(b, 1));
            const auto dz = static_cast<u32>(at(a, 2) - at(b, 2));
            const auto sum = s32(dx * dx + dy * dy + dz * dz);
            return !(static_cast<float>(sum) <= thr * thr);
        }
        case 0xc: {
            const auto dy = f32_at(a + 4) - f32_at(b + 4), dx = f32_at(a) - f32_at(b);
            return !(dx * dx + dy * dy <= thr * thr);
        }
        case 0xd: {
            const auto dx = f32_at(a) - f32_at(b), dy = f32_at(a + 4) - f32_at(b + 4);
            const auto dz = f32_at(a + 8) - f32_at(b + 8);
            return !(dx * dx + dy * dy + dz * dz <= thr * thr);
        }
        default: {  // 0xe / 0xf
            const auto dx = f64_at(a) - f64_at(b), dy = f64_at(a + 8) - f64_at(b + 8);
            auto sum = dx * dx + dy * dy;
            if (type == 0xf) {
                const auto dz = f64_at(a + 0x10) - f64_at(b + 0x10);
                sum = sum + dz * dz;
            }
            return sum > static_cast<double>(thr * thr);
        }
        }
    }

    // 002e3de0: field `index` of the object changed since it was last sent (the current data +0x1c, the last
    // sent +0x20): mode 5 the field's callback +0x18 with {handle, index, size, count, current, last} on its
    // frame (0x50 below `stack`), its byte; otherwise any element 002e3830 finds changed.
    u32 field_changed_002e3de0(u32 obj, u32 handle, u32 index, u32 stack) {
        index &= 0xff;
        if (!obj) return 0;
        const auto cls = kClasses + r8(m, obj) * 0x228;
        const auto field = r32(m, cls + 0x28 + 4 * index);
        if (!field) return 0;
        const auto offset = r32(m, cls + 0x128 + 4 * index);
        auto cur = r32(m, obj + 0x1c) + offset;
        if (!cur) return 0;
        auto last = r32(m, obj + 0x20) + offset;
        if (!last) return 0;
        if (r32(m, field + 0x14) == 5) {
            const auto sp = stack - 0x50;
            w32(m, sp, handle);
            w32(m, sp + 4, index);
            w32(m, sp + 0x10, cur);
            w32(m, sp + 0x14, last);
            w32(m, sp + 8, r32(m, field + 4));
            w32(m, sp + 0xc, r32(m, field + 8));
            const auto cb = r32(m, field + 0x18);
            if (!cb) return 0;
            return host.call(m, cb, {sp}) & 0xffU;
        }
        if (s32(r32(m, field + 8)) <= 0) return 0;
        for (u32 k = 0;;) {
            if (element_changed_002e3830(field, cur, last)) return 1;
            ++k;
            const auto size = r32(m, field + 4);
            last += size;
            cur += size;
            if (s32(k) >= s32(r32(m, field + 8))) return 0;
        }
    }

    // 002e2f30: the object's fields to send into `out` (each: its index byte, then size * count bytes), `*size`
    // counting (the message's 0x1fe bytes), the kind bits of the fields sent or'ed into the byte at `kind`. A field
    // goes when marked dirty (+0x28 bit index; from 0x20 +0x24, bit index / 2), or when its mode is not 0, its
    // interval since it was last sent (+0x3c [index]) passed or `force`, and 002e3de0 finds it changed. A field sent:
    // its data copied to the last sent copy (+0x20), its time the session clock. The count of fields sent.
    u32 pack_fields_002e2f30(u32 obj, u32 handle, u32 force, u32 kind, u32 out, u32 size, u32 stack) {
        force &= 0xff;
        const auto sp = stack - 0x60;
        u32 count = 0;
        if (!out || !size || !obj) return 0;
        if (r8(m, obj) >= 0x10) return 0;
        const auto cls = kClasses + r8(m, obj) * 0x228;
        if (s32(r32(m, cls + 4)) <= 0) return 0;
        for (u32 index = 0; s32(index) < s32(r32(m, cls + 4)); index = (index + 1) & 0xffU) {
            if (r32(m, size) >= 0x1ff) continue;
            const auto field = r32(m, cls + 0x28 + 4 * index);
            if (!field) continue;
            const auto offset = r32(m, cls + 0x128 + 4 * index);
            const auto bits = index < 0x20 ? r32(m, obj + 0x28) >> index : r32(m, obj + 0x24) >> ((index >> 1) & 0x1f);
            if (!(bits & 1)) {
                if (!r32(m, field + 0x14)) continue;
                if (!force && !(r32(m, r32(m, obj + 0x3c) + 4 * index) + r32(m, field + 0x10) < r32(m, kSessionClock)))
                    continue;
                if (!field_changed_002e3de0(obj, handle, index, sp)) continue;
            }
            const auto cur = r32(m, obj + 0x1c) + offset, last = r32(m, obj + 0x20) + offset;
            const auto n = r32(m, field + 4) * r32(m, field + 8);
            if (0x1feU - r32(m, size) < n + 1) continue;
            w8(m, out, index);
            ++out;
            w32(m, size, r32(m, size) + 1);
            memcpy_00125f38(m, out, cur, n);
            count = (count + 1) & 0xffU;
            w32(m, size, r32(m, size) + n);
            out += n;
            w8(m, kind, r8(m, kind) | r8(m, field + 0x1c));
            memcpy_00125f38(m, last, cur, n);
            w32(m, r32(m, obj + 0x3c) + 4 * index, r32(m, kSessionClock));
        }
        return count;
    }

    // 002e31d0: the object's changed fields (002e2f30) to the other clients, message *0x005983f0 {class byte,
    // field count, u16 handle, fields} in the buffer 0x0051f8d8 (kind `kind` | the fields' kind bits); the dirty
    // marks +0x24 / +0x28 cleared. An object with a relevance byte (+1): 002e2dd0's per-client set and 002de5e8;
    // otherwise 002de880 to every client. Frame 0x80 below `stack`.
    u32 flush_002e31d0(u32 handle, u32 kind, u32 force, u32 stack) {
        kind &= 0xff;
        force &= 0xff;
        const auto sp = stack - 0x80;
        constexpr u32 kBuffer = 0x51f8d8;
        w32(m, sp + 0x44, 0);
        w32(m, sp + 0x48, 0);
        w8(m, sp + 0x40, 0);
        if (handle >= 0x1000) return 0x17;
        if (const auto r = object_002dd968(sp + 0x44, handle)) return r;
        if (!r32(m, sp + 0x44)) return 0;
        if (r8(m, r32(m, sp + 0x44) + 1)) relevance_set_002e2dd0(sp + 0x10, handle, sp);
        const auto obj = r32(m, sp + 0x44);
        w32(m, sp, 0);
        w16(m, sp + 2, handle);
        w8(m, sp, r8(m, obj));
        w32(m, sp + 0x48, 4);
        w8(m, sp + 1, pack_fields_002e2f30(obj, handle, force, sp + 0x40, kBuffer + 4, sp + 0x48, sp));
        w32(m, kBuffer, r32(m, sp));
        w32(m, obj + 0x28, 0);
        w8(m, sp + 0x40, kind | r8(m, sp + 0x40));
        w32(m, obj + 0x24, 0);
        if (!r8(m, sp + 1)) return 0;
        const auto k = static_cast<u32>(static_cast<std::int32_t>(static_cast<std::int8_t>(r8(m, sp + 0x40))));
        if (r8(m, obj + 1)) {
            send_set_002de5e8(k, r32(m, kCurrentWorld), 0, r32(m, 0x5983f0), r32(m, sp + 0x48), kBuffer, sp + 0x10, sp);
            return 0;
        }
        send_002de880(k, r32(m, kCurrentWorld), 0xffffffffU, r32(m, 0x5983f0), r32(m, sp + 0x48), kBuffer);
        return 0;
    }

    // ---- the relevance sets and the object deletes (checkpoint 132ln5h5) ------------------------------------------
    // A relevance set: the clients' bits at +0..+0x20, the lowest client +0x24, the highest +0x28. No shipping
    // object has a relevance byte (research/NET_INLEVEL.md 4.16): 002e2dd0 / 002de5e8 are unreachable in the game.
    // 002dddd0: client `c` into the set.
    void set_add_002dddd0(u32 set, u32 c) {
        if (!set || c >= 0x100) return;
        if (s32(c) < s32(r32(m, set + 0x24))) w32(m, set + 0x24, c);
        if (s32(r32(m, set + 0x28)) < s32(c)) w32(m, set + 0x28, c);
        w8(m, set + (c >> 3), r8(m, set + (c >> 3)) | (1U << (c & 7)));
    }

    // 002e2dd0: the object's relevance set: each client with a record but the local one for which the relevance
    // byte's callback (0x00590218) answers non-zero. Frame 0x40 below `stack`.
    void relevance_set_002e2dd0(u32 set, u32 handle, u32 stack) {
        const auto sp = stack - 0x40;
        w32(m, sp + 4, 0);
        w32(m, sp, 0);
        if (!set || handle >= 0x1000) return;
        if (r32(m, kCurrentWorld) >= 4) return;
        if (world_002e51b8(r32(m, kCurrentWorld), sp)) return;
        if (!r32(m, sp)) return;
        object_002dd968(sp + 4, handle);
        const auto obj = r32(m, sp + 4);
        if (!obj) return;
        w32(m, set + 0x28, 0xffffffffU);
        w32(m, set + 0x24, r32(m, r32(m, sp) + 0x444));
        const auto relevance = r8(m, obj + 1);
        const auto cb = r32(m, 0x590218 + 4 * relevance);
        memset_001260f0(m, set, 0, 0x21);
        for (u32 c = 0; s32(c) < s32(r32(m, r32(m, sp) + 0x444)); ++c) {
            w32(m, sp + 8, 0);
            record_002db188(sp + 8, r32(m, kCurrentWorld), c);
            if (!r32(m, sp + 8) || c == r32(m, r32(m, sp) + 0x400)) continue;
            if (!cb) {
                char text[96];
                std::snprintf(text, sizeof text, "002e2dd0: relevance byte %u has no callback (0x00590218): a jump to 0",
                              relevance);
                throw std::runtime_error(text);
            }
            if (host.call(m, cb, {c, handle})) set_add_002dddd0(set, c);
        }
    }

    // 002de5e8: a game message to each client of the set (clients +0x24 .. +0x28 with a record and the bit, the
    // local one skipped) through the send 002de398; 0xd when a send failed. `world` is the world index. Frame 0x60.
    u32 send_set_002de5e8(u32 kind, u32 world, u32 category, u32 id, u32 size, u32 data, u32 set, u32 stack) {
        const auto sp = stack - 0x60;
        const auto k = static_cast<u32>(static_cast<std::int32_t>(static_cast<std::int8_t>(kind & 0xff)));
        w32(m, sp, 0);
        w32(m, sp + 4, 0);
        if (category >= 6 || id >= 0x100 || s32(size) <= 0 || !data || !set) return 0x17;
        if (const auto r = world_002e51b8(world, sp)) return r;
        if (!r32(m, sp)) return 0;
        if (r32(m, r32(m, sp) + 0x44c) == 4) return 0xf;
        if (s32(r32(m, set + 0x28)) < 0) return 0;
        auto c = r32(m, set + 0x24);
        if (s32(r32(m, set + 0x28)) < s32(c)) return 0;
        u32 result = 0;
        do {
            if (c != r32(m, r32(m, sp) + 0x400)) {
                record_002db188(sp + 4, world, c);
                if (r32(m, sp + 4) && (r8(m, set + static_cast<u32>(s32(c) >> 3)) >> (c & 7) & 1) &&
                    host.call(m, 0x2de398, {k, world, c, category, id, size, data}))
                    result = 0xd;
            }
            ++c;
        } while (s32(c) <= s32(r32(m, set + 0x28)));
        return result;
    }

    // 002de9f0: the remove callback of the object's class (0x0058c138 by the class byte) with (master, handle).
    void remove_callback_002de9f0(u32 master, u32 handle, u32 stack) {
        if (master >= 0x100 || handle >= 0x1000) return;
        const auto sp = stack - 0x30;
        object_002dd968(sp, handle);
        const auto obj = r32(m, sp);
        if (!obj) return;
        const auto cb = r32(m, 0x58c138 + 4 * r8(m, obj));
        if (cb) host.call(m, cb, {master, handle});
    }

    // 002dea68: the object deleted here: its class's remove callback (002de9f0), then (the object still there)
    // the handle's status 1, or 0 when it lies outside its master's handle range (record +6, +8 entries); the
    // object's blocks +0x20, +0x1c, +0x38, +0x3c and the object freed, its handle slot cleared. Frame 0x30.
    u32 delete_002dea68(u32 handle, u32 stack) {
        const auto sp = stack - 0x30;
        w32(m, sp + 4, 0);
        if (handle >= 0x1000) return 0x17;
        if (r32(m, kCurrentWorld) >= 4) return 8;
        object_002dd968(sp, handle);
        if (!r32(m, sp)) return 8;
        remove_callback_002de9f0(r32(m, r32(m, sp) + 8), handle, sp);
        object_002dd968(sp, handle);
        if (!r32(m, sp)) return 0;
        const auto status = kStatus + handle;
        w8(m, status, 1);
        if (s32(r32(m, kCurrentWorld)) < 4) {
            record_002db188(sp + 4, r32(m, kCurrentWorld), r32(m, r32(m, sp) + 8));
            if (const auto rec = r32(m, sp + 4)) {
                const auto base = s32(r16(m, rec + 6));
                if (s32(handle) < base || s32(handle) >= base + s32(r16(m, rec + 8))) w8(m, status, 0);
            }
        }
        for (const u32 off : {0x20U, 0x1cU, 0x38U, 0x3cU}) {
            if (!r32(m, r32(m, sp) + off)) continue;
            free_002dd328(r32(m, sp) + off);
            w32(m, r32(m, sp) + off, 0);
        }
        w32(m, kObjects + 4 * handle, 0);
        free_002dd328(sp);
        return 0;
    }

    // 002debe0: the local client's object deleted (002dea68) and the delete message *0x00598400 {u16 handle} to
    // every client (its result not read). Frame 0x30.
    u32 delete_local_002debe0(u32 handle, u32 stack) {
        const auto sp = stack - 0x30;
        if (handle >= 0x1000) return 0x17;
        if (r32(m, kCurrentWorld) >= 4) return 8;
        delete_002dea68(handle, sp);
        w16(m, sp, handle);
        send_002de880(0x50, r32(m, kCurrentWorld), 0xffffffffU, r32(m, 0x598400), 2, sp);
        return 0;
    }

    // 002dec60 (via 002ec2a8): the object released: the local client's (master +8) deleted and announced
    // (002debe0), another client's deleted here only (002dea68). Frame 0x30.
    u32 release_002dec60(u32 handle, u32 stack) {
        const auto sp = stack - 0x30;
        w32(m, sp + 4, 0);
        w32(m, sp, 0);
        if (handle >= 0x1000) return 0x17;
        if (r32(m, kCurrentWorld) >= 4) return 8;
        object_002dd968(sp, handle);
        if (!r32(m, sp)) return 8;
        if (const auto r = world_002e51b8(r32(m, kCurrentWorld), sp + 4)) return r;
        const auto world = r32(m, sp + 4);
        if (!world) return 0;
        if (r32(m, r32(m, sp) + 8) == r32(m, world + 0x400)) delete_local_002debe0(handle, sp);
        else delete_002dea68(handle, sp);
        return 0;
    }

    // 002e74c0: the delete message *0x00598400 {u16 handle} received: 002dea68; 2, or -1 without a message.
    u32 delete_receive_002e74c0(u32 msg, u32 stack) {
        if (!msg) return 0xffffffffU;
        delete_002dea68(r16(m, msg), stack - 0x10);
        return 2;
    }

    // ---- the object replication: announce, receive, the per-frame sends (checkpoint 132ln5h) ---------------------
    // 002e52d8: the client's info in the world (the world's first 0x100 words; 0x2c bytes each, 002e5360). *out
    // is cleared before the world index is checked.
    u32 client_002e52d8(u32 world, u32 client, u32 out, u32 stack) {
        const auto sp = stack - 0x30;
        w32(m, sp, 0);
        if (!out) return 0x17;
        w32(m, out, 0);
        if (world >= 4) return 0x17;
        if (client >= 0x100) return 0x17;
        if (const auto r = world_002e51b8(world, sp)) return r;
        if (!r32(m, sp)) return 0xa;
        w32(m, out, r32(m, r32(m, sp) + 4 * client));
        return 0;
    }

    // 002e5360: as 002e52d8, the info allocated (0x2c bytes) where the client has none. Client info: +0 the name,
    // +0x14 the state, +0x28 connected (002dd5b8), +0x29 newly joined (002db720; 002e4550 clears it).
    u32 client_info_002e5360(u32 world, u32 client, u32 out, u32 stack) {
        const auto sp = stack - 0x30;
        w32(m, sp, 0);
        if (!out) return 0x17;
        w32(m, out, 0);
        if (world >= 4) return 0x17;
        if (client >= 0x100) return 0x17;
        if (const auto r = world_002e51b8(world, sp)) return r;
        if (!r32(m, sp)) return 0xa;
        const auto slot = r32(m, sp) + 4 * client;
        if (!r32(m, slot)) {
            const auto p = alloc_002dd2e8(0x2c, sp);
            w32(m, r32(m, sp) + 4 * client, p);
            if (!r32(m, r32(m, sp) + 4 * client)) return 0x16;
        }
        w32(m, out, r32(m, r32(m, sp) + 4 * client));
        return 0;
    }

    // 002de8d8: the create callback of the object's class (0x0058c0b8 by the class byte) with (master, handle).
    void create_callback_002de8d8(u32 master, u32 handle, u32 stack) {
        if (master >= 0x100 || handle >= 0x1000) return;
        const auto sp = stack - 0x30;
        object_002dd968(sp, handle);
        const auto obj = r32(m, sp);
        if (!obj) return;
        const auto cb = r32(m, 0x58c0b8 + 4 * r8(m, obj));
        if (cb) host.call(m, cb, {master, handle});
    }

    // 002e3340: the object announced (message *0x005983f4) to client `to` (-1: every client) from the buffer
    // 0x0051fad8: the header {master byte, class, relevance, +3, +2, -, u16 owner, u16 handle, -, -, +0x2c, +0x30,
    // the name (0x10)} and its fields in order while they fit (the first: 0x1da bytes; then the message's 0x1fe).
    // The fields' times, the object's send time +0x34 and the client's +0x38 [to] (every client with a record:
    // the last sent copy +0x20 as well) the session clock. An object with a relevance byte: 002e2dd0's set and
    // 002de5e8 to every client, or the class's relevance callback (0x00590218) for one. Frame 0xc0 below `stack`;
    // the header's bytes 5, 0xa, 0xb and the name's tail are the frame's old contents (zero here).
    u32 announce_002e3340(u32 to, u32 handle, u32 stack) {
        const auto sp = stack - 0xc0;
        constexpr u32 kBuffer = 0x51fad8;
        w32(m, sp + 0x60, 0);
        w32(m, sp + 0x64, 0);
        if (handle >= 0x1000) return 0x17;
        object_002dd968(sp + 0x60, handle);
        if (!r32(m, sp + 0x60)) return 0xa;
        if (const auto r = world_002e51b8(r32(m, kCurrentWorld), sp + 0x64)) return r;
        const auto world = r32(m, sp + 0x64);
        if (!world) return 0;
        if (r8(m, r32(m, sp + 0x60) + 1)) {
            if (to == 0xffffffffU) {
                relevance_set_002e2dd0(sp + 0x30, handle, sp);
            } else {
                const auto cb = r32(m, 0x590218 + 4 * r8(m, r32(m, sp + 0x60) + 1));
                if (!host.call(m, cb, {to, handle})) return 0;
            }
        }
        const auto obj = r32(m, sp + 0x60);
        const auto cls = kClasses + r8(m, obj) * 0x228;
        const auto owner = r32(m, obj + 4);
        if (owner != 0xffffffffU && owner != r32(m, world + 0x400)) return 8;
        memset_001260f0(m, sp, 0, 0x24);  // PORT-ONLY: the undefined bytes
        w8(m, sp, r32(m, obj + 8));  // 002e6090
        w16(m, sp + 8, handle);
        w16(m, sp + 6, r16(m, obj + 4));
        w8(m, sp + 1, r8(m, obj));
        w8(m, sp + 2, r8(m, obj + 1));
        w8(m, sp + 4, r8(m, obj + 2));
        w8(m, sp + 3, r8(m, obj + 3));
        w32(m, sp + 0xc, r32(m, obj + 0x2c));
        w32(m, sp + 0x10, r32(m, obj + 0x30));
        strcpy_00129360(m, sp + 0x14, obj + 0xc);
        memcpy_00125f38(m, kBuffer, sp, 0x24);
        u32 size = 0x24;
        if (s32(r32(m, cls + 4)) > 0) {
            for (u32 i = 0;;) {
                const auto field = r32(m, cls + 0x28 + 4 * i);
                if (!field) return 0xa;
                const auto n = r32(m, field + 4) * r32(m, field + 8);
                if (i == 0 ? s32(0x1daU) < s32(n) : s32(0x1feU - size) < s32(n)) break;
                memcpy_00125f38(m, kBuffer + size, r32(m, obj + 0x1c) + r32(m, cls + 0x128 + 4 * i), n);
                size += n;
                w32(m, r32(m, obj + 0x3c) + 4 * i, r32(m, kSessionClock));
                if (s32(++i) >= s32(r32(m, cls + 4))) break;
            }
        }
        w32(m, obj + 0x34, r32(m, kSessionClock));
        if (to == 0xffffffffU) {
            memcpy_00125f38(m, r32(m, obj + 0x20), r32(m, obj + 0x1c), r32(m, cls));
            for (u32 c = 0; s32(c) < s32(r32(m, world + 0x444)); ++c) {
                record_002db188(sp + 0x68, r32(m, kCurrentWorld), c);
                if (r32(m, sp + 0x68)) w32(m, r32(m, obj + 0x38) + 4 * c, r32(m, kSessionClock));
            }
        } else {
            w32(m, r32(m, obj + 0x38) + 4 * to, r32(m, kSessionClock));
        }
        if (r8(m, obj + 1) && to == 0xffffffffU) {
            send_set_002de5e8(0x50, r32(m, kCurrentWorld), 0, r32(m, 0x5983f4), size, kBuffer, sp + 0x30, sp);
            return 0;
        }
        send_002de880(0x50, r32(m, kCurrentWorld), to, r32(m, 0x5983f4), size, kBuffer);
        return 0;
    }

    // 002e4550: the local client's objects (owned, or unowned with it as the master) announced: to every client
    // when never sent (+0x34 0) or the period +0x30 passed (no flush); otherwise to each newly joined client
    // (+0x29 1) and the changed fields flushed (002e31d0). Then the clients' newly joined marks cleared. Frame 0x60.
    void objects_002e4550(u32 stack) {
        const auto sp = stack - 0x60;
        w32(m, sp + 4, 0);
        w32(m, sp, 0);
        w32(m, sp + 0xc, 0);
        if (r32(m, kCurrentWorld) >= 4) return;
        if (world_002e51b8(r32(m, kCurrentWorld), sp)) return;
        if (!r32(m, sp)) return;
        const auto local = r32(m, r32(m, sp) + 0x400);
        if (local >= 0x100) return;
        for (u32 h = 0; s32(h) < s32(r32(m, kHandleTop)); ++h) {
            if (static_cast<std::int8_t>(r8(m, kStatus + h)) != 2) continue;
            if (object_002dd968(sp + 4, h)) continue;
            const auto obj = r32(m, sp + 4);
            if (!obj) continue;
            const auto owner = r32(m, obj + 4);
            if (owner != local && (owner != 0xffffffffU || r32(m, obj + 8) != local)) continue;
            const auto sent = r32(m, obj + 0x34), period = r32(m, obj + 0x30);
            if (!sent || (period && sent + period < r32(m, kSessionClock))) {
                host.call(m, 0x2e3340, {0xffffffffU, h});
                continue;
            }
            for (u32 c = 0; s32(c) < s32(r32(m, r32(m, sp) + 0x444)); ++c) {
                w32(m, sp + 8, 0);
                record_002db188(sp + 8, r32(m, kCurrentWorld), c);
                if (!r32(m, sp + 8)) continue;
                client_002e52d8(r32(m, kCurrentWorld), c, sp + 0xc, sp);
                const auto info = r32(m, sp + 0xc);
                if (!info || c == local || r8(m, info + 0x29) != 1) continue;
                host.call(m, 0x2e3340, {c, h});
            }
            host.call(m, 0x2e31d0, {h, 0, 0});
        }
        for (u32 c = 0; s32(c) < s32(r32(m, r32(m, sp) + 0x444)); ++c) {
            client_002e52d8(r32(m, kCurrentWorld), c, sp + 0xc, sp);
            const auto info = r32(m, sp + 0xc);
            if (info && c != local && r8(m, info + 0x29) == 1) w8(m, info + 0x29, 0);
        }
    }

    // 002ddb30(out, world): the world's second handle space's base (+0x404) into *out (0x100 first). 0x17 for a world
    // of 4 or more or no out; the world lookup's error. Frame 0x20.
    u32 second_base_002ddb30(u32 out, u32 world, u32 stack) {
        const auto sp = stack - 0x20;
        w32(m, sp, 0);
        if (world >= 4 || !out) return 0x17;
        w32(m, out, 0x100);
        if (const auto r = world_002e51b8(world, sp)) return r;
        if (const auto w = r32(m, sp)) w32(m, out, r32(m, w + 0x404));
        return 0;
    }

    // 002e4228 (SendClientDataStreams): the local client's second-space objects (DME data streams, handles rec +0xa
    // .. + rec +0xc, 002dd9f8 NetGetStreamChannel) streamed: those in state 2 send their queued data
    // (rt_circ_buf_get_used 002f5818, the rate test, NetGetDataStreamPacket 002e3f20 / NetSendDmeMessage 002de880
    // message *0x0059840c, rt_circ_buf_free 002f53b8 / rt_circ_buf_normalize 002f56f8) or finish (DmeFree 002dd328 /
    // rt_circ_buf_destroy 002f5348 / SendCloseDataStream 002e4078). Not ported, and unreachable in this game
    // (132fx5): a stream reaches state 2 only through NetIOpenDataStream 002e2878, whose one caller is the API
    // wrapper NetOpenDataStream 002eb150, and nothing in the ELF calls or points at 002eb150 (nor at
    // NetRegisterDataStream 002ea380, NetAddToDataStream 002eb260, NetEndDataStream 002eb318); voice chat uses
    // NetStreamMedia instead. Such an object throws by name. Frame 0x70 below `stack`.
    void second_space_002e4228(u32 stack) {
        const auto sp = stack - 0x70;
        w32(m, sp + 8, 0x100);
        w32(m, sp + 0xc, 0);
        w32(m, sp + 4, 0);
        w32(m, sp, 0);
        if (r32(m, kCurrentWorld) >= 4) return;
        if (world_002e51b8(r32(m, kCurrentWorld), sp)) return;
        if (!r32(m, sp)) return;
        if (record_002db188(sp + 4, r32(m, kCurrentWorld), r32(m, r32(m, sp) + 0x400))) return;
        if (!r32(m, sp + 4) || r16(m, r32(m, sp + 4) + 0xa) >= 0x800) return;
        if (second_base_002ddb30(sp + 8, r32(m, kCurrentWorld), sp)) return;
        if (r32(m, sp + 8) >= 0x100) return;
        for (u32 k = 0; k < r16(m, r32(m, sp + 4) + 0xc); ++k) {
            if (slot_002dd9f8(k + r16(m, r32(m, sp + 4) + 0xa), sp + 0xc, sp)) continue;
            const auto obj = r32(m, sp + 0xc);
            if (!obj || r8(m, obj) != 2) continue;
            throw std::runtime_error("002e4228: a DME data stream in state 2 (002f5818 / 002e3f20 / 002de880; unreachable: no caller opens one) is not ported");
        }
    }

    // 002e76d0: an object announcement (002e3340) received from client `from` (a2): a new object (0x44 bytes and
    // its data) in the announced handle, or the existing one updated; the header's fields, then each field's data
    // (their times the session clock). The local client's own announcement for a handle not in use: a handle of
    // its name (002ded20) only measures the message; otherwise a new handle (002dee30). A new object: the
    // class's create callback (002de8d8); an existing one: the owner callback (002de950, -1). The bytes read.
    u32 announce_receive_002e76d0(u32 a0, u32 from, u32 msg, u32 stack) {
        const auto sp = stack - 0x60;
        w32(m, sp + 4, 0);
        w32(m, sp, 0);
        w32(m, sp + 8, 0);
        if (!msg) return 0xffffffffU;
        if (r32(m, kCurrentWorld) >= 4 || a0 >= 4 || from >= 0x100) return 0xffffffffU;
        if (world_002e51b8(r32(m, kCurrentWorld), sp)) return 0xffffffffU;
        const auto world = r32(m, sp);
        if (!world) return 0xffffffffU;
        const auto cls = kClasses + r8(m, msg + 1) * 0x228;
        auto handle = r16(m, msg + 8);
        const auto in_use = [&](u32 h) { return static_cast<std::int8_t>(r8(m, kStatus + h)) == 2; };
        bool existing = false;
        if (r8(m, msg) == r32(m, world + 0x400)) {
            if (in_use(handle)) {
                existing = true;
            } else {
                w32(m, sp + 4, 0);
                if (s32(host.call(m, 0x2ded20, {msg + 0x14, a0})) >= 0) {
                    u32 size = 0x24;
                    for (u32 k = 0; s32(k) < s32(r32(m, cls + 4)); ++k) {
                        const auto field = r32(m, cls + 0x28 + 4 * k);
                        size += r32(m, field + 4) * r32(m, field + 8);
                    }
                    return size;
                }
                handle = host.call(m, 0x2dee30, {});
                if (s32(handle) >= 0) object_002dd968(sp + 4, handle);
            }
        }
        if (!existing && in_use(handle)) existing = true;
        bool created = false;
        if (existing) {
            object_002dd968(sp + 4, handle);
        } else {
            const auto old = r8(m, kStatus + handle);
            const auto top = r32(m, kHandleTop);
            const auto obj = alloc_002dd2e8(0x44, sp);
            w32(m, sp + 4, obj);
            if (!obj) return 0x24;
            w8(m, kStatus + handle, 2);
            w32(m, kObjects + 4 * handle, obj);
            if (s32(r32(m, kHandleTop)) < s32(handle + 1)) w32(m, kHandleTop, handle + 1);
            w32(m, obj + 8, r8(m, msg));
            w32(m, obj + 0x2c, r32(m, msg + 0xc));
            w32(m, obj + 0x30, r32(m, msg + 0x10));
            w32(m, obj + 0x34, 0);
            w32(m, obj + 0x1c, alloc_002dd2e8(r32(m, cls), sp));
            w32(m, obj + 0x20, alloc_002dd2e8(r32(m, cls), sp));
            w32(m, obj + 0x38, alloc_002dd2e8(r32(m, world + 0x444) * 4, sp));
            w32(m, obj + 0x3c, alloc_002dd2e8(r32(m, cls + 4) * 4, sp));
            if (r32(m, obj + 0x1c) && r32(m, obj + 0x20) && r32(m, obj + 0x38) && r32(m, obj + 0x3c)) {
                created = true;
            } else {
                for (const u32 field : {0x1cU, 0x20U, 0x38U, 0x3cU}) {
                    if (!r32(m, obj + field)) continue;
                    free_002dd328(obj + field);
                    w32(m, obj + field, 0);
                }
                free_002dd328(sp + 4);
                w32(m, kHandleTop, top);
                w8(m, kStatus + handle, old);
                w32(m, kObjects + 4 * handle, 0);
                return 0x24;
            }
        }
        const auto obj = r32(m, sp + 4);
        if (!obj) return 0x24;
        w32(m, obj + 4, static_cast<u32>(static_cast<std::int16_t>(r16(m, msg + 6))));
        w8(m, obj, r8(m, msg + 1));
        strcpy_00129360(m, obj + 0xc, msg + 0x14);
        w8(m, obj + 1, r8(m, msg + 2));
        w8(m, obj + 2, r8(m, msg + 4));
        w8(m, obj + 3, r8(m, msg + 3));
        u32 size = 0x24, src = msg + 0x24;
        for (u32 k = 0; s32(k) < s32(r32(m, cls + 4)); ++k) {
            const auto field = r32(m, cls + 0x28 + 4 * k);
            const auto n = r32(m, field + 4) * r32(m, field + 8);
            memcpy_00125f38(m, r32(m, obj + 0x1c) + r32(m, cls + 0x128 + 4 * k), src, n);
            w32(m, r32(m, obj + 0x3c) + 4 * k, r32(m, kSessionClock));
            size += n;
            src += n;
        }
        memcpy_00125f38(m, r32(m, obj + 0x20), r32(m, obj + 0x1c), r32(m, cls));
        w32(m, obj + 0x34, r32(m, kSessionClock));
        if (created) create_callback_002de8d8(r32(m, obj + 8), handle, sp);
        else owner_callback_002de950(r32(m, obj + 8), handle, 0xffffffffU, sp);
        return size;
    }

    // 002e7b60: an object's fields (002e31d0's message {class, count, u16 handle, then index byte + data each})
    // received: into the object's data and last sent copy, the field's time the session clock, and the owner
    // callback (002de950) with the field's index. A handle not in use: the message only measured. The bytes read;
    // -1 for a bad field index or a missing field record.
    u32 fields_receive_002e7b60(u32 from, u32 msg, u32 stack) {
        const auto sp = stack - 0x60;
        w32(m, sp, 0);
        if (!msg || from >= 0x100) return 0xffffffffU;
        const auto handle = r16(m, msg + 2);
        if (handle >= 0x1000) return 0xffffffffU;
        u32 total = 4, p = msg + 4;
        if (static_cast<std::int8_t>(r8(m, kStatus + handle)) != 2) {
            const auto cls = kClasses + r8(m, msg) * 0x228;
            const auto count = r8(m, msg + 1), fields = r32(m, cls + 4);
            for (u32 k = 0; k < count; ++k) {
                const auto index = r8(m, p++);
                if (s32(index) >= s32(fields)) return 0xffffffffU;
                const auto field = r32(m, cls + 0x28 + 4 * index);
                if (!field) return 0xffffffffU;
                const auto n = r32(m, field + 4) * r32(m, field + 8);
                total += 1 + n;
                p += n;
            }
            return total;
        }
        if (object_002dd968(sp, handle)) return 0xffffffffU;
        const auto obj = r32(m, sp);
        if (!obj) return 0xffffffffU;
        const auto cls = kClasses + r8(m, obj) * 0x228;
        for (u32 k = 0; k < r8(m, msg + 1); ++k) {
            const auto index = r8(m, p++);
            if (s32(index) >= s32(r32(m, cls + 4))) return 0xffffffffU;
            const auto field = r32(m, cls + 0x28 + 4 * index);
            if (!field) return 0xffffffffU;
            const auto offset = r32(m, cls + 0x128 + 4 * index);
            const auto n = r32(m, field + 4) * r32(m, field + 8);
            memcpy_00125f38(m, r32(m, obj + 0x1c) + offset, p, n);
            memcpy_00125f38(m, r32(m, obj + 0x20) + offset, p, n);
            w32(m, r32(m, obj + 0x3c) + 4 * index, r32(m, kSessionClock));
            total += 1 + n;
            p += n;
            owner_callback_002de950(r32(m, r32(m, sp) + 4), handle, index, sp);
        }
        return total;
    }

    // 002e7608: an owner change (002e2bb8's / 002e74e8's message *0x005983e0 {owner byte, -, u16 handle}): the
    // object's owner +4 = the byte and the owner callback (002de950, -2). A handle not in use: nothing. 4 bytes
    // read; -1 without a message. Frame 0x30 below `stack`.
    u32 owner_receive_002e7608(u32 msg, u32 stack) {
        const auto sp = stack - 0x30;
        if (!msg) return 0xffffffffU;
        const auto owner = r8(m, msg);
        object_002dd968(sp, r16(m, msg + 2));
        if (const auto obj = r32(m, sp)) {
            w32(m, obj + 4, owner);
            owner_callback_002de950(r32(m, r32(m, sp) + 4), r16(m, msg + 2), 0xfffffffeU, sp);
        }
        return 4;
    }

    // 002e7670: an ownership release (002e2ce0's message *0x00598408 {u16 handle}): the object's owner +4 = -1
    // and the owner callback (002de950 with that -1, -2). A handle not in use: nothing. 2 bytes read; -1 without a
    // message. Frame 0x20 below `stack`.
    u32 release_receive_002e7670(u32 msg, u32 stack) {
        const auto sp = stack - 0x20;
        if (!msg) return 0xffffffffU;
        object_002dd968(sp, r16(m, msg));
        if (const auto obj = r32(m, sp)) {
            w32(m, obj + 4, 0xffffffffU);
            owner_callback_002de950(r32(m, r32(m, sp) + 4), r16(m, msg), 0xfffffffeU, sp);
        }
        return 2;
    }

    // 002e74e8: an ownership request (002e2bb8's message *0x00598404 {u16 handle}) from client `from`. The local
    // client's object without an owner: owner +4 = `from`, the owner callback (002de950, -2), and the owner
    // change *0x005983e0 {from, -, u16 handle} to everyone in world `world`. 2 bytes read; -1 for no message, no
    // world, a bad handle or no object. Frame 0x50 below `stack`.
    u32 own_request_receive_002e74e8(u32 world_index, u32 from, u32 msg, u32 stack) {
        const auto sp = stack - 0x50;
        w32(m, sp + 0x14, 0);
        w32(m, sp + 0x10, 0);
        if (!msg) return 0xffffffffU;
        if (r32(m, kCurrentWorld) >= 4) return 0xffffffffU;
        const auto handle = r16(m, msg);
        if (handle >= 0x1000) return 0xffffffffU;
        object_002dd968(sp + 0x10, handle);
        if (!r32(m, sp + 0x10)) return 0xffffffffU;
        if (world_002e51b8(r32(m, kCurrentWorld), sp + 0x14)) return 0xffffffffU;
        const auto world = r32(m, sp + 0x14);
        if (!world) return 0xffffffffU;
        const auto obj = r32(m, sp + 0x10);
        if (r32(m, obj + 8) != r32(m, world + 0x400)) return 2;
        if (r32(m, obj + 4) != 0xffffffffU) return 2;
        w32(m, obj + 4, from);
        owner_callback_002de950(r32(m, r32(m, sp + 0x10) + 4), handle, 0xfffffffeU, sp);
        w8(m, sp, from & 0xff);  // 002e6090
        w16(m, sp + 2, handle);
        send_002de880(0x50, world_index, 0xffffffffU, r32(m, 0x5983e0), 4, sp);
        return 2;
    }

    // 002e2480: a network object of class `cls` in the first free handle of the local client's range.
    // a0 out, a1 kind (2: no owner), a2 / t2 / t0 bytes +2 / +3 / +1, a3 the clock offset (+0x2c), t1 +0x30,
    // t3 the initial data (class size bytes), stack +0 the name, +8 the class.
    u32 object_create_002e2480(const u32* a, u32 stack) {
        const auto out = a[0], name = a[8], cls = a[9];
        const auto sp = stack - 0x80;
        if (!out) return 0x17;
        w32(m, out, 0xffffffffU);
        if (!name) return 0x17;
        if (cls >= 0x10) return 0x17;
        const auto world_index = r32(m, kCurrentWorld);
        if (world_index >= 4) return 0xf;
        if (const auto r = world_002e51b8(world_index, sp)) return r;
        const auto world = r32(m, sp);
        if (!world) return 0;
        const auto local = r32(m, world + 0x400);
        if (s32(local) < 0 || s32(local) >= s32(r32(m, world + 0x444))) return 0x15;
        record_002db188(sp + 4, r32(m, kCurrentWorld), local);
        const auto rec = r32(m, sp + 4);
        if (!rec) return 0x15;
        if (!r16(m, rec + 8)) return 0x13;
        const auto klass = kClasses + cls * 0x228;
        for (u32 k = 0;;) {
            const auto handle = k + r16(m, rec + 6);
            ++k;
            if (handle < 0x1000) {
                const auto status = r8(m, kStatus + handle);
                if (status != 2) return object_fill(a, stack, world, klass, handle, status);
            }
            if (s32(k) >= s32(r16(m, rec + 8))) return 0x13;
        }
    }

    u32 object_fill(const u32* a, u32 stack, u32 world, u32 klass, u32 handle, u32 status) {
        const auto out = a[0], kind = a[1], name = a[8], cls = a[9];
        const auto sp = stack - 0x80;
        const auto size = r32(m, klass);
        const auto high = r32(m, kHandleTop);
        const auto obj = alloc_002dd2e8(0x44, sp);
        w32(m, sp + 8, obj);
        if (!obj) return 0x16;
        w32(m, kObjects + 4 * handle, obj);
        w8(m, kStatus + handle, 2);
        if (s32(r32(m, kHandleTop)) < s32(handle + 1)) w32(m, kHandleTop, handle + 1);
        w32(m, obj + 4, kind == 2 ? 0xffffffffU : r32(m, world + 0x400));
        w8(m, obj + 2, a[2]);
        w32(m, obj + 8, r32(m, world + 0x400));
        w8(m, obj + 3, a[6]);
        w8(m, obj + 1, a[4]);
        w32(m, obj + 0x2c, r32(m, kSessionClock) + a[3]);
        w32(m, obj + 0x1c, alloc_002dd2e8(size, sp));
        w32(m, obj + 0x20, alloc_002dd2e8(size, sp));
        w32(m, obj + 0x38, alloc_002dd2e8(r32(m, world + 0x444) * 4, sp));
        w32(m, obj + 0x3c, alloc_002dd2e8(r32(m, klass + 4) * 4, sp));
        if (!r32(m, obj + 0x1c) || !r32(m, obj + 0x20) || !r32(m, obj + 0x38) || !r32(m, obj + 0x3c)) {
            for (const u32 field : {0x1cU, 0x20U, 0x38U, 0x3cU}) {
                if (!r32(m, obj + field)) continue;
                free_002dd328(obj + field);
                w32(m, obj + field, 0);
            }
            free_002dd328(sp + 8);
            w32(m, kHandleTop, high);
            w8(m, kStatus + handle, status);
            w32(m, kObjects + 4 * handle, 0);
            return 0x16;
        }
        strncpy_00129a70(m, obj + 0xc, name, 0xf);
        w8(m, obj + 0x1b, 0);
        w8(m, obj, cls);
        w32(m, obj + 0x34, 0);
        w32(m, obj + 0x30, a[5]);
        if (a[7]) memcpy_00125f38(m, r32(m, obj + 0x1c), a[7], size);
        w32(m, out, handle);
        return 0;
    }
};

[[noreturn]] void not_library(u32 address) {
    char text[64];
    std::snprintf(text, sizeof text, "dme_library_call(0x%08x): not a ported DME library function", address);
    throw std::runtime_error(text);
}
}  // namespace

bool dme_library_has(std::uint32_t address) {
    switch (address) {
    case 0x2e9a10: case 0x2e9a98: case 0x2e9b60: case 0x2e9be8: case 0x2e9cd0: case 0x2e9d88: case 0x2e9e50:
    case 0x2e9f08: case 0x2ec168: case 0x2ea4f0: case 0x2e9480: case 0x2e9320: case 0x2e94e0: case 0x2df290:
    case 0x3057f8: case 0x305998: case 0x2ed528: case 0x2ed550: case 0x2ed000: case 0x2ed028: case 0x2ed280:
    case 0x2ed168: case 0x2ed570: case 0x2ecff0: case 0x2ecff8: case 0x2e8a00: case 0x2db140: case 0x2e92f0:
    case 0x2dd2e8: case 0x2fb350: case 0x2fb2d8: case 0x314380: case 0x2eaf28: case 0x2e2480: case 0x2eb650:
    case 0x2dd968: case 0x2e51b8: case 0x2e51f0: case 0x2db188: case 0x2db270: case 0x2dd328: case 0x2fb3c0:
    case 0x2e68c0: case 0x2e69b8: case 0x2db470: case 0x2dd9b0: case 0x2dd9f8:
    case 0x2ec3c8: case 0x2ec460: case 0x2e2bb8: case 0x2e2ce0: case 0x2de950: case 0x2de880: case 0x2e6090:
    case 0x2e31d0: case 0x2e2f30: case 0x2e3de0: case 0x2e3830:
    case 0x2e52d8: case 0x2e5360: case 0x2de8d8: case 0x2e3340: case 0x2e4550: case 0x2e76d0: case 0x2e7b60:
    case 0x2e7608: case 0x2e74e8: case 0x2ddb30: case 0x2e4228: case 0x2e7670:
    case 0x2dddd0: case 0x2e2dd0: case 0x2de5e8: case 0x2de9f0: case 0x2dea68: case 0x2debe0: case 0x2dec60:
    case 0x2ec2a8: case 0x2e74c0:
        return true;
    default:
        return false;
    }
}

std::uint32_t dme_library_call(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t address,
                               std::initializer_list<std::uint32_t> list, std::uint32_t stack) {
    const std::vector<u32> a(list);
    const auto arg = [&](std::size_t i) { return i < a.size() ? a[i] : 0U; };
    Lib lib{m, host};
    switch (address) {
    case 0x2e9a10: return lib.gate([] { return 0U; });
    case 0x2e9a98: return lib.gate([] { return 0U; });
    case 0x2e9b60: return lib.gate([&] { return lib.fields_begin_002ed000(); });
    case 0x2e9be8:
        return lib.gate([&] { return lib.field_002ed028(arg(0), arg(1), arg(2), arg(3), arg(4), arg(5)); });
    case 0x2e9cd0: return lib.gate([&] { return lib.class_002ed280(arg(0), arg(1), arg(2)); });
    case 0x2e9d88: return lib.gate([&] { return lib.class_callbacks_002ed570(arg(0), arg(1), arg(2), arg(3)); });
    case 0x2e9e50: return lib.gate([&] { return lib.message_002ed528(arg(0), arg(1), arg(2)); });
    case 0x2e9f08: return lib.gate([&] { return lib.message_002ed528(arg(0), 2, arg(1)); });
    case 0x2ec168:
        return lib.gate([&] {
            w32(m, 0x48c49c, arg(0));  // 002dcd30
            w32(m, 0x48c4a0, arg(1));
            return 0U;
        });
    case 0x2ea4f0: return lib.allocator_002ea4f0(arg(0), arg(1));
    case 0x2e9480: return lib.init_params_002e9480(arg(0));
    case 0x2e9320: return lib.version_002e9320(arg(0));
    case 0x2e94e0: return lib.start_002e94e0(arg(0), arg(1), stack);
    case 0x2df290: return lib.world_init_002df290(arg(0), arg(1), stack);
    case 0x3057f8: return lib.registry_init_003057f8(arg(0), arg(1), arg(2));
    case 0x305998: return lib.registry_add_00305998(arg(0), arg(1), arg(2));
    case 0x2ed528: return lib.message_002ed528(arg(0), arg(1), arg(2));
    case 0x2ed550: return lib.message_002ed528(arg(0), 2, arg(1));
    case 0x2ed000: return lib.fields_begin_002ed000();
    case 0x2ed028: return lib.field_002ed028(arg(0), arg(1), arg(2), arg(3), arg(4), arg(5));
    case 0x2ed280: return lib.class_002ed280(arg(0), arg(1), arg(2));
    case 0x2ed168: lib.nested_002ed168(arg(0), arg(1), arg(2)); return 0;
    case 0x2ed570: return lib.class_callbacks_002ed570(arg(0), arg(1), arg(2), arg(3));
    case 0x2ecff0:
    case 0x2ecff8: return 0;
    case 0x2e8a00: lib.own_messages_002e8a00(stack); return 0;
    case 0x2db140: lib.table_002db140(stack); return 0;
    case 0x2e92f0: memset_001260f0(m, 0x5227f0, 0, 0x2c00); return 0;
    case 0x2dd2e8: return lib.alloc_002dd2e8(arg(0), stack);
    case 0x2fb350: return lib.alloc_zeroed_002fb350(arg(0), arg(1));
    case 0x2fb2d8: return lib.alloc_002fb2d8(arg(0), arg(1));
    case 0x314380: return lib.sockets_00314380();
    case 0x2eaf28: {
        const u32 v[10] = {arg(0), arg(1), arg(2), arg(3), arg(4), arg(5), arg(6), arg(7), arg(8), arg(9)};
        return lib.gate([&] { return lib.object_create_002e2480(v, stack - 0x60); });
    }
    case 0x2e2480: {
        const u32 v[10] = {arg(0), arg(1), arg(2), arg(3), arg(4), arg(5), arg(6), arg(7), arg(8), arg(9)};
        return lib.object_create_002e2480(v, stack);
    }
    case 0x2eb650: return lib.gate([&] { return lib.object_002dd968(arg(0), arg(1)); });
    case 0x2dd968: return lib.object_002dd968(arg(0), arg(1));
    case 0x2e51b8: return lib.world_002e51b8(arg(0), arg(1));
    case 0x2e51f0: return lib.world_create_002e51f0(arg(0), arg(1), stack);
    case 0x2db188: return lib.record_002db188(arg(0), arg(1), arg(2));
    case 0x2db270: return lib.record_new_002db270(arg(0), arg(1), stack);
    case 0x2dd328: return lib.free_002dd328(arg(0));
    case 0x2fb3c0: return lib.release_002fb3c0(arg(0));
    case 0x2e68c0: return lib.handle_base_002e68c0(arg(0));
    case 0x2e69b8: return lib.second_base_002e69b8(arg(0), stack);
    case 0x2db470: lib.mark_002db470(arg(0), stack); return 0;
    case 0x2dd9b0: return lib.slot_002dd9b0(arg(0), arg(1), stack);
    case 0x2dd9f8: return lib.slot_002dd9f8(arg(0), arg(1), stack);
    case 0x2ec3c8: return lib.gate([&] { return lib.own_002e2bb8(arg(0), stack - 0x20); });
    case 0x2ec460: return lib.gate([&] { return lib.release_002e2ce0(arg(0), stack - 0x20); });
    case 0x2e2bb8: return lib.own_002e2bb8(arg(0), stack);
    case 0x2e2ce0: return lib.release_002e2ce0(arg(0), stack);
    case 0x2de950: lib.owner_callback_002de950(arg(0), arg(1), arg(2), stack); return 0;
    case 0x2de880: return lib.send_002de880(arg(0), arg(1), arg(2), arg(3), arg(4), arg(5));
    case 0x2e6090: return arg(0) & 0xff;
    case 0x2e31d0: return lib.flush_002e31d0(arg(0), arg(1), arg(2), stack);
    case 0x2e2f30: return lib.pack_fields_002e2f30(arg(0), arg(1), arg(2), arg(3), arg(4), arg(5), stack);
    case 0x2e3de0: return lib.field_changed_002e3de0(arg(0), arg(1), arg(2), stack);
    case 0x2e3830: return lib.element_changed_002e3830(arg(0), arg(1), arg(2));
    case 0x2e52d8: return lib.client_002e52d8(arg(0), arg(1), arg(2), stack);
    case 0x2e5360: return lib.client_info_002e5360(arg(0), arg(1), arg(2), stack);
    case 0x2de8d8: lib.create_callback_002de8d8(arg(0), arg(1), stack); return 0;
    case 0x2e3340: return lib.announce_002e3340(arg(0), arg(1), stack);
    case 0x2e4550: lib.objects_002e4550(stack); return 0;
    case 0x2ddb30: return lib.second_base_002ddb30(arg(0), arg(1), stack);
    case 0x2e4228: lib.second_space_002e4228(stack); return 0;
    case 0x2e76d0: return lib.announce_receive_002e76d0(arg(0), arg(2), arg(3), stack);
    case 0x2e7b60: return lib.fields_receive_002e7b60(arg(2), arg(3), stack);
    case 0x2e7608: return lib.owner_receive_002e7608(arg(3), stack);
    case 0x2e7670: return lib.release_receive_002e7670(arg(3), stack);
    case 0x2e74e8: return lib.own_request_receive_002e74e8(arg(0), arg(2), arg(3), stack);
    case 0x2dddd0: lib.set_add_002dddd0(arg(0), arg(1)); return 0;
    case 0x2e2dd0: lib.relevance_set_002e2dd0(arg(0), arg(1), stack); return 0;
    case 0x2de5e8: return lib.send_set_002de5e8(arg(0), arg(1), arg(2), arg(3), arg(4), arg(5), arg(6), stack);
    case 0x2de9f0: lib.remove_callback_002de9f0(arg(0), arg(1), stack); return 0;
    case 0x2dea68: return lib.delete_002dea68(arg(0), stack);
    case 0x2debe0: return lib.delete_local_002debe0(arg(0), stack);
    case 0x2dec60: return lib.release_002dec60(arg(0), stack);
    case 0x2ec2a8: return lib.gate([&] { return lib.release_002dec60(arg(0), stack - 0x20); });
    case 0x2e74c0: return lib.delete_receive_002e74c0(arg(3), stack);
    default: not_library(address);
    }
}

}  // namespace omega
