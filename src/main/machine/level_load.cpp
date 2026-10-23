#include "level_load.hpp"
#include "app_teardown.hpp"
#include "team_setup.hpp"
#include "level_selector.hpp"
#include "loadout_state.hpp"

#include <stdexcept>
#include <string>
#include <cstdio>

namespace omega {

std::uint32_t LevelLoadMemory::read(std::uint32_t address, unsigned size) const {
    if (size != 1 && size != 2 && size != 4) throw std::invalid_argument("level load read size");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i) {
        const auto it = bytes_.find(address + i);
        if (it != bytes_.end()) value |= std::uint32_t(it->second) << (8 * i);
        else if (sealed_) unmodelled(address + i, "read");
    }
    return value;
}

void LevelLoadMemory::write(std::uint32_t address, unsigned size, std::uint32_t value) {
    if (size != 1 && size != 2 && size != 4) throw std::invalid_argument("level load write size");
    if (sealed_)
        for (unsigned i = 0; i < size; ++i)
            if (!bytes_.count(address + i)) unmodelled(address + i, "write");
    for (unsigned i = 0; i < size; ++i) bytes_[address + i] = std::uint8_t(value >> (8 * i));
}

void LevelLoadMemory::unmodelled(std::uint32_t address, const char* access) {
    char hex[9];
    std::snprintf(hex, sizeof hex, "%08x", address);
    throw std::runtime_error(std::string("EE memory ") + access + " at " + hex + " is not modelled (sealed mirror)");
}

void LevelLoadMemory::copy(std::uint32_t address, const char* text) {
    while (*text) write(address++, 1, static_cast<std::uint8_t>(*text++));
    write(address, 1, 0);
}

std::uint32_t LevelLoadHost::call(LevelLoadMemory& memory, std::uint32_t address,
                                  std::initializer_list<std::uint32_t> args) {
    // The original wrapper calls this native service through the same host
    // dispatch. Recording hosts may override it for independent caller goldens.
    if (address == 0x16a970) { run_team_setup(memory, *this); return 0; }
    if (address == 0x16aa80) { run_team_teardown(memory, *this); return 0; }
    if (address == 0x16a0a0) { team_data_object_0016a0a0(memory, *this, kTeamDataStack); return 0; }
    if (address == 0x2cc8e0) {
        if (args.size() != 2) throw std::invalid_argument("level selector arguments");
        const auto p = args.begin();
        select_level_by_name(memory, *this, p[0], p[1]);
        return 0;
    }
    if (address == 0x3e1a20) {
        if (args.size() != 1) throw std::invalid_argument("start level scripts arguments");
        run_start_level_scripts(memory, *this, *args.begin());
        return 0;
    }
    if (address == 0x2cc820) {
        if (args.size() != 2) throw std::invalid_argument("level setter arguments");
        const auto p = args.begin();
        set_level_selection(memory, *this, p[0], static_cast<std::int32_t>(p[1]));
        return 0;
    }
    char hex[9];
    std::snprintf(hex, sizeof hex, "%08x", address);
    throw std::runtime_error(std::string("Level load: unhosted EE service at ") + hex);
}

std::uint64_t LevelLoadHost::call_return64(LevelLoadMemory&, std::uint32_t address,
                                           std::initializer_list<std::uint32_t>) {
    char hex[9];
    std::snprintf(hex, sizeof hex, "%08x", address);
    throw std::runtime_error(std::string("Level load: unhosted EE 64-bit return service at ") + hex);
}

std::uint32_t LevelLoadHost::call_args64(LevelLoadMemory&, std::uint32_t address,
                                         std::initializer_list<std::uint64_t>) {
    char hex[9];
    std::snprintf(hex, sizeof hex, "%08x", address);
    throw std::runtime_error(std::string("Level load: unhosted EE 64-bit argument service at ") + hex);
}

std::uint32_t LevelLoadHost::call_float(LevelLoadMemory&, std::uint32_t address,
                                        std::initializer_list<std::uint32_t>,
                                        std::initializer_list<std::uint32_t>) {
    char hex[9];
    std::snprintf(hex, sizeof hex, "%08x", address);
    throw std::runtime_error(std::string("Level load: unhosted EE float-argument service at ") + hex);
}

std::uint32_t LevelLoadHost::call_framed(LevelLoadMemory& memory, std::uint32_t address,
                                         std::initializer_list<std::uint32_t> args, std::uint32_t sp) {
    if (address != 0x166190) return call(memory, address, args);
    if (args.size() != 1) throw std::invalid_argument("app teardown arguments");
    run_app_teardown(memory, *this, *args.begin(), sp);
    return 0;
}

void LevelLoadHost::before_level_load(LevelLoadMemory&) {}
void LevelLoadHost::after_level_load(LevelLoadMemory&) {}

void run_level_load(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t g, std::uint32_t stack) {
    const std::uint32_t sp = stack - 0x580;
    const auto r8 = [&](std::uint32_t a) { return m.read(a, 1); };
    const auto r32 = [&](std::uint32_t a) { return m.read(a); };
    const auto w8 = [&](std::uint32_t a, std::uint32_t v) { m.write(a, 1, v); };
    const auto w32 = [&](std::uint32_t a, std::uint32_t v) { m.write(a, 4, v); };
    const auto call = [&](std::uint32_t a, std::initializer_list<std::uint32_t> args = {}) {
        return host.call(m, a, args);
    };
    const auto locked_alloc = [&](std::uint32_t size, std::uint32_t line) {
        if (r8(0x533880)) call(0x10dc40, {9});
        w32(0x533888, r32(0x533888) + 1);
        const auto p = call(0x36b180, {0, size, 0x49a890, line});
        if (r8(0x533880)) call(0x10dca8, {9});
        w32(0x533888, r32(0x533888) - 1);
        return p;
    };
    const auto required = [&](std::uint32_t address) {
        if (!address) throw std::runtime_error("Level load: required EE object is null");
        return address;
    };

    const std::uint32_t gui = required(r32(g + 0xb4));
    if (r32(gui + 0x24c) == 0xffffffffU) call(0x2cc8e0, {gui + 0x188, g + 0x140});
    call(0x12f630, {g});
    const auto archive = call(0x136990, {g + 0x3c});
    call(0x3e2140, {0x555070, archive});
    call(0x136710, {g});
    for (std::uint32_t i = 0; i != 4; ++i) {
        if (r8(0x489ba8)) {
            call(0x128bd0, {sp + 0x550, 0x49aa60, i});
            const auto id = call(0x3fdb50, {sp + 0x550});
            call(0x12f5e0, {g, i, id});
        }
    }
    const auto agent = call(0x1314f0, {g, 0xffffffffU});
    call(0x335d50, {agent, 1});
    call(0x1c0860, {r32(g + 0x88), g + 0x3c});
    call(0x40c2d0);

    auto p = locked_alloc(0x24, 0x1e3);
    if (p) {
        const auto renderer = call(0x40ca30);
        call(0x40ba10, {p, 0x82, renderer});
    }
    w32(g + 0x1f4, p);
    p = locked_alloc(0x24, 0x1e4);
    if (p) {
        const auto renderer = call(0x40ca30);
        call(0x40ba10, {p, 0x3e8, renderer});
    }
    w32(g + 0x1f8, p);
    call(0x257380);
    call(0x1698c0);
    if (!r32(g + 0x98)) {
        p = locked_alloc(0x10, 0x1ec);
        if (p) {
            call(0x1366f0, {p});
            w8(p + 0xc, 1);
        }
        w32(g + 0x98, p);
    }
    if (!r32(g + 0x90)) {
        p = locked_alloc(0x10, 0x1ef);
        if (p) {
            call(0x1365e0, {p});
            w8(p + 0xc, 1);
        }
        w32(g + 0x90, p);
    }

    call(0x3fdca0, {0x48b2f8, 0x4ffc90});
    call(0x128bd0, {sp + 0x4d0, 0x49aa70, 0x48b2f8, 0x4ffc90});
    call(0x36e4b0, {sp + 0x430, sp + 0x4d0, 0, 0});
    if (call(0x36def0, {sp + 0x430}) & 0xffU)
        call(0x128bd0, {sp + 0x4d0, 0x49aa90, 0x48b2f8});
    const auto loader = call(0x1698c0);
    call(0x166430, {loader, 0, sp + 0x4d0});
    call(0x3a3910, {0x53b540, 0x1388});

    p = locked_alloc(0x230, 0x20c);
    if (p) p = call(0x27a020, {p, 0x64, 0x14});
    w32(g + 0xec, p);
    p = locked_alloc(0xc, 0x20f);
    if (p) call(0x139070, {p});
    w32(g + 0x8c, p);
    call(0x16f510);
    call(0x3e1720, {0x555070, 0x49aaa8, 0});
    call(0x3e1b60, {0x555070, 0x49aab0, 0});
    call(0x3c2de0, {0x96, 0, 0, 0});
    call(0x136200, {0x12c});
    call(0x40f650);
    call(0x1afb20);
    call(0x3c2de0, {0, 0, 0, 0});
    call(0x1360d0, {0x320});
    call(0x26be70, {r8(required(r32(g + 0x1dc)) + 5)});
    p = locked_alloc(0x4c, 0x22e);
    if (p) {
        call(0x135e70, {p, 0x1e});
        w32(p, 0x4d9100);
    }
    call(0x3cac40, {g, p});
    const auto msg = call(0x3caa50, {g, 0x4f2cc0});
    if (r8(0x489ba8)) call(0x1c9bf0, {msg});
    call(0x1c98d0, {msg});
    call(0x1ca210, {sp + 0x60, 5, 0x49a908, 0, 0});
    call(0x20faa0, {sp + 0x270, sp + 0x60});
    call(0x36e220, {sp + 0x144, 0xffffffffU});
    call(0x36e220, {sp + 0xa8, 0xffffffffU});
    w8(sp + 0x290, 1);
    call(0x3c9420, {sp + 0x270, 0x4ffb50, 0});
    call(0x1c01d0, {r32(g + 0x88), g + 0x3c});
    call(0x1c4190, {r32(g + 0x88), g + 0x3c});
    call(0x128bd0, {sp + 0x1f0, 0x49aac0, 0x48b2f8});
    const auto level_file = call(0x3f96e0, {g + 0x3c, sp + 0x1f0, 0, 0});
    call(0x4089f0, {0x53bb90, level_file});
    call(0x1368d0, {g + 0x3c});
    call(0x132bc0, {g});
    call(0x1307f0, {g});
    call(0x3c9e30, {sp + 0x30, 0x4f53f0});
    w32(sp + 0x30, 0x4d90e0);
    call(0x3c9420, {sp + 0x30, r32(g + 0xbc), 1});
    w32(sp + 0x30, 0x4d90e0);
    call(0x3c9d60, {sp + 0x30, 0});
    w8(required(r32(g + 0xbc)) + 0x174, r8(required(r32(g + 0x1dc)) + 4));
    w8(required(r32(g + 0xbc)) + 0x175, r8(required(r32(g + 0x1dc)) + 6));
    w8(required(r32(g + 0xbc)) + 0x176, r8(required(r32(g + 0x1dc)) + 7));
    w32(required(r32(g + 0x80)) + 0x20, r32(required(r32(g + 0xbc)) + 0x30));
    call(0x1c0180, {r32(g + 0x88), g + 0x3c});
    call(0x3e20a0, {0x555070});
    call(0x4703b0);
    call(0x1c0130, {r32(g + 0x88), g + 0x3c});
    call(0x3d57a0, {0x543790});
    auto object = required(r32(0x53924c));
    w8(object + 0xc, 1);
    object = call(0x13d9f0);
    const auto linked_object = call(0x13d770, {object, r32(required(r32(g + 0xbc)) + 0x30)});
    w8(required(linked_object) + 4, 1);
    object = call(0x13d9f0);
    call(0x13d8a0, {object});
    w8(0x489ba8, 0);
    for (std::uint32_t i = 0; i < 4; ++i) w8(g + 0x1c4 + i, 0);
    const auto end = call(0x1698c0);
    w8(required(end) + 0x278, 1);
    w32(g + 0x1f0, 0);
    w32(sp + 0x270, 0x4dae30);
    call(0x36e220, {sp + 0x384, 0xffffffffU});
    call(0x36e220, {sp + 0x2e8, 0xffffffffU});
    call(0x3c9d60, {sp + 0x270, 0});
    call(0x36e220, {sp + 0x430, 0xffffffffU});
}

}  // namespace omega
