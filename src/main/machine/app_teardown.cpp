#include "app_teardown.hpp"

#include <initializer_list>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
constexpr std::uint32_t thread_id = 0x48a030;       // terminated and deleted
constexpr std::uint32_t handler_id = 0x48a038;      // interrupt-handler id for cause 2
constexpr std::uint32_t priority_thread = 0x48a048; // priority set to 5
constexpr std::uint32_t word_058 = 0x48a058;
constexpr std::uint32_t buffer = 0x4ee4e8;
constexpr std::uint32_t lock_flag = 0x533880;
constexpr std::uint32_t lock_count = 0x533888;
constexpr std::uint32_t registry = 0x539248;
constexpr std::uint32_t archives = 0x4ffb8c;        // G+0x3c
constexpr std::uint32_t source_file = 0x49c390;     // "machine.cc"
constexpr std::uint32_t none = 0xffffffffU;
}  // namespace

void run_app_teardown(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t app, std::uint32_t sp) {
    const auto r8 = [&](std::uint32_t a) { return m.read(a, 1); };
    const auto r32 = [&](std::uint32_t a) { return m.read(a); };
    const auto w32 = [&](std::uint32_t a, std::uint32_t v) { m.write(a, 4, v); };
    const auto call = [&](std::uint32_t a, std::initializer_list<std::uint32_t> args = {}) {
        return host.call(m, a, args);
    };
    const auto required = [](std::uint32_t address, const char* what) {
        if (!address) throw std::runtime_error(std::string("App teardown: ") + what + " is null");
        return address;
    };
    const std::uint32_t file = sp - 0x100 + 0x50;

    if (!r32(app + 0x274)) return;
    if (r32(handler_id) != none) {
        call(0x10dc40, {2});
        call(0x10ce60, {2, r32(handler_id)});
        w32(handler_id, none);
    }
    if (const auto thread = r32(thread_id); thread != none) {
        call(0x10cfb0, {thread});
        call(0x10cf70, {r32(thread_id)});
        w32(thread_id, none);
    }
    w32(0x4ee500, 0);
    w32(0x4ee508, 0);
    w32(word_058, none);

    // The inlined allocator lock pattern: the flag is reread for the unlock.
    const auto data = r32(buffer);
    if (r8(lock_flag)) call(0x10dc40, {9});
    w32(lock_count, r32(lock_count) + 1);
    call(0x36b130, {0, data, source_file, 0x7f3});
    if (r8(lock_flag)) call(0x10dca8, {9});
    w32(buffer, 0);
    w32(lock_count, r32(lock_count) - 1);
    call(0x10cff0, {r32(priority_thread), 5});

    if (const auto object = r32(app + 0x274)) {
        call(0x271cd0, {object});
        const auto current = r32(app + 0x274);
        if (r8(lock_flag)) call(0x10dc40, {9});
        w32(lock_count, r32(lock_count) + 1);
        call(0x2720a0, {current, none});
        call(0x36b130, {0, current, source_file, 0x7fd});
        if (r8(lock_flag)) call(0x10dca8, {9});
        w32(lock_count, r32(lock_count) - 1);
        w32(app + 0x274, 0);
    }

    if (!r8(required(r32(app + 0x270), "archive") + 0x14c)) return;
    call(0x36e5d0, {file});
    const auto list = r32(required(r32(app + 0x270), "archive") + 0x148);
    const auto count = static_cast<std::int32_t>(r32(required(list, "archive list") + 4));
    for (std::int32_t i = 0; i < count; ++i) {
        const auto archive = r32(app + 0x270);
        const auto name = call(0x36d200, {archive, static_cast<std::uint32_t>(i)});
        call(0x36e1b0, {file, name, archive, 0});
        call(0x36def0, {file});
        call(0x3816f0, {r32(registry), file});
    }
    call(0x3f9530, {archives, r32(app + 0x270)});
    call(0x36e220, {file, none});
}

}  // namespace omega
