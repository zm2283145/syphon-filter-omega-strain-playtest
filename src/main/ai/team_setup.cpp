#include "team_setup.hpp"

#include <cstdint>

namespace omega {
namespace {
constexpr std::uint32_t ptr = 0x4ee620;
constexpr std::uint32_t owner = 0x4ee628;
constexpr std::uint32_t latch = 0x48a100;
constexpr std::uint32_t flag = 0x5721c8;
constexpr std::uint32_t thread = 0x533880;
constexpr std::uint32_t counter = 0x533888;
constexpr std::uint32_t source = 0x49c650;

void enter_thread_section(LevelLoadMemory& m, LevelLoadHost& host) {
    if (m.read(thread, 1)) host.call(m, 0x10dc40, {9});
    m.write(counter, 4, m.read(counter) + 1);
}

void leave_thread_section(LevelLoadMemory& m, LevelLoadHost& host) {
    if (m.read(thread, 1)) host.call(m, 0x10dca8, {9});
    m.write(counter, 4, m.read(counter) - 1);
}
}  // namespace

void initialize_team_bits(LevelLoadMemory& m, std::uint32_t object) {
    for (std::uint32_t i = 0; i < 0x100; ++i) {
        const auto old = m.read(object + i, 1);
        const auto bit = ((i >= 0x40) != ((i & 15U) >= 4U)) ? 1U : 0U;
        m.write(object + i, 1, (old & ~1U) | bit);
    }
}

void run_team_setup(LevelLoadMemory& m, LevelLoadHost& host) {
    if (m.read(flag, 1)) {
        if (!m.read(owner, 1)) host.call(m, 0x16a0a0, {});
        if (m.read(latch, 1)) {
            m.write(latch, 1, 0);
            initialize_team_bits(m, m.read(ptr));
        }
        return;
    }

    auto object = m.read(ptr);
    if (!object) {
        enter_thread_section(m, host);
        object = host.call(m, 0x36b180, {0, 0x100, source, 0x71b});
        leave_thread_section(m, host);
        m.write(ptr, 4, object);
    }
    initialize_team_bits(m, m.read(ptr));
}

void team_data_object_0016a0a0(LevelLoadMemory& m, LevelLoadHost& host, std::uint32_t stack) {
    const auto frame = stack - 0x60;
    const auto multi = m.read(flag, 1) ? m.read(0x5721c0, 1) : 1U;
    std::uint32_t line = 0x80b;
    if (multi) {
        if (m.read(0x5721d0, 1)) {
            if (m.read(0x4ee618)) return;
            m.write(frame, 4, 0x49c648);  // "cTeam" (sd: the upper word is 0)
            m.write(frame + 4, 4, 0);
            m.write(frame + 8, 4, m.read(0x4ee610));
            m.write(frame + 12, 4, 0);
            if (host.call_framed(m, 0x2eaf28, {frame + 0x5c, 1, 4, 0, 0, 0, 0, m.read(ptr)}, frame)) return;
            if (host.call(m, 0x2eb650, {frame + 0x58, m.read(frame + 0x5c)})) return;
            const auto object = m.read(frame + 0x58);
            const auto previous = m.read(ptr);
            const auto data = m.read(object + 0x1c);
            if (previous && !m.read(owner, 1)) {
                enter_thread_section(m, host);
                host.call(m, 0x36b130, {0, previous, 0x49c800, 0x1fe});
                leave_thread_section(m, host);
            }
            m.write(ptr, 4, data);
            m.write(owner, 1, 1);
            return;
        }
        line = 0x806;
    }
    enter_thread_section(m, host);
    const auto block = host.call(m, 0x36b180, {0, 0x100, source, line});
    leave_thread_section(m, host);
    const auto previous = m.read(ptr);
    if (previous && !m.read(owner, 1)) {
        enter_thread_section(m, host);
        host.call(m, 0x36b130, {0, previous, 0x49c800, 0x1fe});
        leave_thread_section(m, host);
    }
    m.write(ptr, 4, block);
}

void run_team_teardown(LevelLoadMemory& m, LevelLoadHost& host) {
    const auto object = m.read(ptr);  // saved s0; callback may replace the global
    if (object && !m.read(owner, 1)) {
        enter_thread_section(m, host);
        host.call(m, 0x36b130, {0, object, source, 0x6fe});
        leave_thread_section(m, host);
    }
    m.write(ptr, 4, 0);
    m.write(owner, 1, 0);
    m.write(latch, 1, 1);
}

}  // namespace omega
