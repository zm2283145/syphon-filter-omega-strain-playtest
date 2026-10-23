#pragma once

#include "script.hpp"

#include <cstddef>

namespace omega {

// One explicit cross-script binding. Both scripts must outlive the bridge/VMs
// and remain unchanged. Live object-registry validation belongs to the caller.
class VoiceSoundBridge {
public:
    VoiceSoundBridge(const Script& consumer, const Script& provider);
    Word action_member(Word index) const;
    std::optional<Value> invoke(Vm& provider_vm, const Script& caller,
                                const Function& import, const std::vector<Value>& args) const;
private:
    const Script* consumer_;
    const Script* provider_;
    std::size_t import_index_{};
    std::size_t export_index_{};
    std::size_t consumer_enum_index_{};
    std::size_t provider_enum_index_{};
};

}
