#include "loadout_state.hpp"

#include <bit>
#include <cstdint>
#include <initializer_list>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
constexpr std::uint32_t game = 0x4ffb50;
constexpr std::uint32_t state = game + 0x50;          // 004ffba0
constexpr std::uint32_t previous = game + 0x51;       // 004ffba1
constexpr std::uint32_t gui = game + 0xb4;            // 004ffc04
constexpr std::uint32_t player = game + 0xbc;         // 004ffc0c
constexpr std::uint32_t pending = game + 0xd9;        // 004ffc29: the loadout-pending byte
constexpr std::uint32_t load_ready = 0x4ee470;        // set to 1 by the state-0xe block
constexpr std::uint32_t last_time = 0x4ee478;         // u64
constexpr std::uint32_t gui_loader = 0x571fb8;        // 00414790
constexpr std::uint32_t gui_manager = 0x571fb0;       // 004147a0
constexpr std::uint32_t mp_flag = 0x5721c8;
constexpr std::uint32_t mp_host = 0x5721c0;
constexpr std::uint32_t video = 0x5381f0;
constexpr std::uint32_t pad = 0x55a300;

float bits_float(std::uint32_t bits) { return std::bit_cast<float>(bits); }

// 0011b888 (__floatdisf over the soft-double helpers 0011c7a0, 0011c268,
// 0011c1a8 and 0011c928): (double)high * 65536 * 65536 + (double)low, narrowed
// to single. The executable's soft-double add and narrowing both truncate
// (round toward zero), so for the non-negative values 001002d0 passes the result
// is the value truncated to 24 significant bits. Its bit-11 sticky step for
// values beyond 2^53 lies below single precision there and cannot change a
// truncated result.
float int64_to_float(std::uint64_t value) {
    const int length = std::bit_width(value);
    if (length > 24) value = value >> (length - 24) << (length - 24);
    return static_cast<float>(value);   // at most 24 significant bits: exact
}
}  // namespace

float loadout_u64_to_float(std::uint64_t value) {
    // 001002d0: positive signed values convert directly; the rest are halved
    // with the low bit kept as sticky and doubled with add.s.
    if (static_cast<std::int64_t>(value) > 0) return int64_to_float(value);
    const float half = int64_to_float((value >> 1) | (value & 1U));
    return half + half;
}

void run_start_level_scripts(LevelLoadMemory& memory, LevelLoadHost& host, std::uint32_t script) {
    if (!memory.read(0x4938d0, 1)) return;
    host.call(memory, 0x3e1b60, {script, 0x4bd8e0, 0});     // "init::ClientStartLevel"
    const bool run = memory.read(mp_flag, 1) ? memory.read(mp_host, 1) != 0 : true;
    if (run) host.call(memory, 0x3e1b60, {script, 0x4bd900, 0});   // "init::StartLevel"
}

void run_loadout_state(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t app, std::uint32_t sp) {
    const auto r8 = [&](std::uint32_t a) { return m.read(a, 1); };
    const auto r32 = [&](std::uint32_t a) { return m.read(a); };
    const auto r64 = [&](std::uint32_t a) { return std::uint64_t(m.read(a)) | std::uint64_t(m.read(a + 4)) << 32; };
    const auto w8 = [&](std::uint32_t a, std::uint32_t v) { m.write(a, 1, v); };
    const auto w32 = [&](std::uint32_t a, std::uint32_t v) { m.write(a, 4, v); };
    const auto w64 = [&](std::uint32_t a, std::uint64_t v) {
        m.write(a, 4, static_cast<std::uint32_t>(v));
        m.write(a + 4, 4, static_cast<std::uint32_t>(v >> 32));
    };
    const auto call = [&](std::uint32_t a, std::initializer_list<std::uint32_t> args = {}) {
        return host.call(m, a, args);
    };
    const auto call_float = [&](std::uint32_t a, std::initializer_list<std::uint32_t> args,
                                std::initializer_list<std::uint32_t> floats) {
        return host.call_float(m, a, args, floats);
    };
    const auto required = [](std::uint32_t address, const char* what) {
        if (!address) throw std::runtime_error(std::string("Loadout state: ") + what + " is null");
        return address;
    };
    // lw t9,(object); lw t9,offset(t9); jalr t9.
    const auto method = [&](std::uint32_t object, std::uint32_t offset) {
        return r32(r32(required(object, "virtual receiver")) + offset);
    };
    // 001692d0: the text of a string object (bit 0 of +0: heap pointer at +8, else inline at +1).
    const auto text = [&](std::uint32_t object) { return (r32(object) & 1U) ? r32(object + 8) : object + 1; };
    // 00169330: the 005721e0 string's pool id when 005721f0 is set, else 0.
    const auto quit_title = [&]() -> std::uint32_t {
        return r8(0x5721f0) ? call(0x3fdb50, {0x5721e0}) : 0;
    };

    if (r8(state) != 0x12) return;
    auto actor = r32(required(r32(player), "player") + 0x30);
    if (!actor) return;

    if (r32(load_ready) && r8(actor + 0x2c3b)) {
        call(0x389a50, {actor + 0x60});
        if (r8(actor + 0x2c3b)) return;
        host.call_framed(m, 0x166190, {app}, sp);
        w32(load_ready, 0);
        w8(pending, 1);
        w64(last_time, 0);
        call(0x36a0f0, {pad});
        const auto screen = r8(mp_flag)
            ? call(0x418a80, {r32(gui_loader), 0x49c470, 0x49c488})     // GuiEquipmentSetup, eqsetup
            : call(0x418a80, {r32(gui_loader), 0x49c490, 0x49c4a8});    // GuiEquipmentModify, eqmodify
        const auto manager = r32(gui_manager);
        call(method(manager, 0x64), {manager, screen, 0});
        return;
    }

    if (r8(pending) != 1) {
        actor = r32(required(r32(player), "player") + 0x30);
        const auto registry = call(0x13d9f0);
        const auto entry = call(0x13d770, {registry, actor});
        if (!call(0x224e00, {r32(required(entry, "00013d770 result") + 8)})) return;
        call(0x245e30, {r32(game + 0xdc)});
        call(0x36a240, {pad});
        w8(previous, r8(state));
        w8(state, 0x0f);
        call(0x3c9e30, {sp + 0xb0, 0x4f5380});
        w32(sp + 0xb0, 0x4dadd0);
        call(0x3c9420, {sp + 0xb0, game, 1});
        w32(sp + 0xb0, 0x4dadd0);
        call(0x3c9d60, {sp + 0xb0, 0});
        call_float(0x25e6e0, {r32(game + 0xe8), 1}, {0x40000000});   // 2.0f
        call(0x3e1a20, {0x555070});
        call(0x12ee30, {game});
        call(0x256b40);
        return;
    }

    call(0x37a9f0, {video});
    const auto record = video + r32(0x538c60) * 0x110U;
    for (std::uint32_t i = 0; i < 0x10; i += 4) w32(record + 0x1f0 + i, r32(0x539170 + i));
    w8(record + 0x2f5, 1);
    w8(record + 0x2f6, 1);

    // T1_COUNT (10000800) plus 00533870 as an unsigned word (001686e4..0016870c).
    const std::uint32_t ticks = r32(0x10000800) + r32(0x533870);
    float count;
    if (static_cast<std::int32_t>(ticks) < 0) {
        count = static_cast<float>(static_cast<std::int32_t>((ticks >> 1) | (ticks & 1U)));
        count = count + count;
    } else {
        count = static_cast<float>(static_cast<std::int32_t>(ticks));
    }
    // count / 5.76 is at most 745654044, inside cvt.w.s range, and never negative.
    const auto whole = static_cast<std::uint32_t>(static_cast<std::int32_t>(count / bits_float(0x40b851ecU)));
    const std::uint64_t now = r64(0x533878) + whole;
    const std::uint64_t delta = now - r64(last_time);
    const auto elapsed = std::bit_cast<std::uint32_t>(loadout_u64_to_float(delta) / bits_float(0x47c35000U));

    call(0x3c8de0);
    call(0x40c5c0);
    call_float(0x40f3e0, {}, {elapsed});
    call_float(0x419260, {r32(gui_loader)}, {elapsed});
    call(0x415870, {r32(gui_manager)});
    auto manager = r32(gui_manager);
    call_float(method(manager, 0x60), {manager}, {elapsed});
    const auto message = call(0x3caa50, {game, 0x4f2cc0});
    call(0x3c9e30, {sp + 0xe0, 0x5436b0});
    w32(sp + 0xe0, 0x4d9160);
    call(0x3c9420, {sp + 0xe0, message, 1});
    w32(sp + 0xe0, 0x4d9160);
    call(0x3c9d60, {sp + 0xe0, 0});

    if (r8(mp_flag)) {
        call(0x438770, {0});
        if (r8(0x572400)) {
            w8(pending, 0);
            auto screen = call(0x418980, {r32(gui_loader), 0x49c470});     // GuiEquipmentSetup
            if (screen) {
                manager = r32(gui_manager);
                call(method(manager, 0x68), {manager, screen});
                call(0x418e00, {r32(gui_loader), screen, 1});
            }
            screen = call(0x418980, {r32(gui_loader), 0x49c490});          // GuiEquipmentModify
            if (screen) {
                manager = r32(gui_manager);
                call(method(manager, 0x68), {manager, screen});
            }
            while (call(0x2cac80, {r32(gui), 0xffffffffU})) {}
            actor = r32(required(r32(player), "player") + 0x30);
            call(method(actor, 0x74), {actor, 4});
        }
        // 00169360: read and clear the quit request 005721d8.
        bool quit = false;
        if (r8(0x5721d8)) { quit = true; w8(0x5721d8, 0); }
        if (quit) {
            const auto title = quit_title() ? quit_title() : call(0x3fdb50, {0x49c4b8});   // "QuitTitle"
            const std::uint32_t message_id = r8(0x5723b0) == 1 ? 0x668 : 0x667;
            const auto owner = r32(gui);
            // 001692f0: 005721f0 itself when its first byte is set, else the 005721e0 pool id.
            const auto name = r8(0x5721f0) ? 0x5721f0U : call(0x3fdb50, {0x5721e0});
            call(0x2cae30, {owner, 2, title, name, r32(gui_manager), message_id});
        }
        bool disconnected = false;
        if (r8(0x5723b8)) { disconnected = true; w8(0x5723b8, 0); }
        if (disconnected) {
            call(0x2ca640, {r32(gui), 0});
            if (r8(mp_flag) ? r8(mp_host) != 0 : true) {
                call(0x13b480, {sp + 0x1a0, call(0x3fdb50, {0x49c4c8})});   // "LOBBY_DISCON_T"
                call(0x13b480, {sp + 0x190, call(0x3fdb50, {0x49c4d8})});   // "LOBBY_DISCON_M"
                const auto body = text(sp + 0x190);
                const auto heading = text(sp + 0x1a0);
                call(0x2cae30, {r32(gui), 2, heading, body, 0, 0});
                call(0x138b70, {sp + 0x190, 0});
                call(0x138b70, {sp + 0x1a0, 0});
            }
        }
    }
    manager = r32(gui_manager);
    call(method(manager, 0x5c), {manager});
    w64(last_time, now);
}

}  // namespace omega
