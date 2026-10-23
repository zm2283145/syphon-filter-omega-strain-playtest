#include "static_instance.hpp"

namespace omega {
namespace {
constexpr std::uint32_t guard = 0x543740;
constexpr std::uint32_t node = 0x543748;
constexpr std::uint32_t object = 0x543760;
constexpr std::uint32_t destructor = 0x3d0b90;
constexpr std::uint32_t destructor_head = 0x4e1db0;   // 00100b30

// 003d1560 / 00139070: sw zero,0(a0); sw a0+4,8(a0); sw a0+4,4(a0).
void empty_list(LevelLoadMemory& m, std::uint32_t header) {
    m.write(header, 4, 0);
    m.write(header + 8, 4, header + 4);
    m.write(header + 4, 4, header + 4);
}
}  // namespace

std::uint32_t static_instance_003d1020(LevelLoadMemory& m) {
    if (m.read(guard, 1) == 0) {
        m.write(object, 4, 0);
        empty_list(m, object + 4);
        empty_list(m, object + 0x10);
        m.write(object + 0x1c, 1, 1);
        // 00100b30(object, 003d0b90, node): push the node on the destructor list.
        m.write(node, 4, m.read(destructor_head));
        m.write(node + 4, 4, destructor);
        m.write(node + 8, 4, object);
        m.write(destructor_head, 4, node);
        m.write(guard, 1, 1);
    }
    return object;
}

}  // namespace omega
