#include "spawned_npc_msg.hpp"

#include <stdexcept>

namespace omega {

std::uint32_t spawned_npc_msg_get_npc(SpawnedActorMemory& memory, std::uint32_t arguments,
                                      const SpawnedNpcPredicate& predicate) {
    const std::uint32_t message = memory.read(arguments);             // 0021d070
    std::uint32_t component = 0;
    if (const std::uint32_t actor = memory.read(message + 0x24U)) {   // 0021d074
        bool player = false;
        if (const std::uint32_t current = memory.read(actor + 0x58U)) {   // 0021d080
            if (!predicate) throw std::runtime_error("GetNPC 0021d060: the component's virtual +0x54 is not hosted");
            const std::uint32_t target = memory.read(memory.read(current) + 0x54U);
            player = predicate(memory, target, current) != 0;   // 0021d09c: beqz on the 64-bit v0
        }
        if (!player) component = memory.read(memory.read(message + 0x24U) + 0x58U);   // 0021d0b0..0021d0b4
    }
    return component;   // 0014a670
}

}  // namespace omega
