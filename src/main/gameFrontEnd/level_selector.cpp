#include "level_selector.hpp"

namespace omega {
namespace {
constexpr std::uint32_t table = 0x48c160;
constexpr std::uint32_t metadata = 0x4acb60;
constexpr std::uint32_t byte1 = 0x4acbb0;
constexpr std::uint32_t byte2 = 0x4acbd0;
constexpr std::uint32_t global_index = 0x494198;
}  // namespace

void set_level_selection(LevelLoadMemory& m, LevelLoadHost& host,
                         std::uint32_t destination, std::int32_t index) {
    if (index < 0 || index >= 18) {
        m.write(destination, 1, 0);
        m.write(destination + 0x40, 1, 0);
        m.write(destination + 0xc0, 4, 0xffffffffU);
        m.write(destination + 0xc4, 4, 0xffffffffU);
        m.write(destination + 0xc8, 1, 0);
        return;
    }
    const auto row = static_cast<std::uint32_t>(index);
    // 002cc85c reads the name pointer at the setter, after the selector's
    // comparison callback. The copy callback can change metadata below.
    host.call(m, 0x129360, {destination, m.read(table + 4 * row)});
    m.write(destination + 0xc0, 4, m.read(metadata + 4 * row));
    m.write(destination + 0xc4, 4, row);
    m.write(destination + 0xc8, 1, m.read(byte1 + row, 1));
    m.write(destination + 0xc9, 1, m.read(byte2 + row, 1));
    m.write(global_index, 4, row);
}

void select_level_by_name(LevelLoadMemory& m, LevelLoadHost& host,
                          std::uint32_t destination, std::uint32_t name) {
    for (std::uint32_t i = 0; i < 18; ++i) {
        if (!host.call(m, 0x128e80, {m.read(table + 4 * i), name})) {
            set_level_selection(m, host, destination, static_cast<std::int32_t>(i));
            return;
        }
    }
    set_level_selection(m, host, destination, -1);
}

}  // namespace omega
