#include "voice_sound_bridge.hpp"

#include <array>
#include <stdexcept>

namespace omega {
namespace {
constexpr std::array<Word, 5> plain_dispatch{0xffffffffU,0xffffffffU,0xffffffffU,0xffffffffU,0};

std::size_t unique_function(const Script& script, bool external) {
    std::optional<std::size_t> found;
    for (std::size_t i = 0; i < script.functions.size(); ++i) {
        const auto& function = script.functions[i];
        if (function.name != "voice::PlaySound") continue;
        if (found || function.external != external || function.return_type != 4
            || function.dispatch_fields != plain_dispatch || function.parameters.size() != 3
            || script.type_key(function.parameters[0]) != "primitive:3"
            || script.type_key(function.parameters[1]) != "object:GOBJ"
            || script.type_key(function.parameters[2]) != "enum:SFX_ACTION")
            throw std::runtime_error("Unsupported or ambiguous voice::PlaySound signature");
        found = i;
    }
    if (!found) throw std::runtime_error("Missing voice::PlaySound binding");
    return *found;
}

std::size_t unique_actions(const Script& script, bool external) {
    std::optional<std::size_t> found;
    for (std::size_t i = 0; i < script.enums.size(); ++i) {
        const auto& type = script.enums[i];
        if (type.name != "SFX_ACTION") continue;
        if (found || type.external != external)
            throw std::runtime_error("Unsupported or ambiguous SFX_ACTION enum");
        if (external) {
            if (!type.members.empty()) throw std::runtime_error("External action enum has local members");
        } else if (type.members.size() != 2 || type.members[0].first != "START" || type.members[1].first != "STOP") {
            throw std::runtime_error("Unsupported SFX_ACTION declaration order");
        }
        found = i;
    }
    if (!found) throw std::runtime_error("Missing SFX_ACTION enum");
    return *found;
}
}

VoiceSoundBridge::VoiceSoundBridge(const Script& consumer, const Script& provider)
    : consumer_(&consumer), provider_(&provider) {
    import_index_ = unique_function(consumer, true);
    export_index_ = unique_function(provider, false);
    consumer_enum_index_ = unique_actions(consumer, true);
    provider_enum_index_ = unique_actions(provider, false);
    if (consumer.functions[import_index_].parameters[2] != consumer_enum_index_ + 10
        || provider.functions[export_index_].parameters[2] != provider_enum_index_ + 10)
        throw std::runtime_error("Voice action parameter does not identify the bound enum");
}

Word VoiceSoundBridge::action_member(Word index) const {
    if (unique_actions(*consumer_, true) != consumer_enum_index_
        || unique_actions(*provider_, false) != provider_enum_index_)
        throw std::runtime_error("Voice action binding changed");
    const auto& members = provider_->enums.at(provider_enum_index_).members;
    if (index >= members.size()) throw std::runtime_error("Voice action member outside declaration");
    return members[index].second;
}

std::optional<Value> VoiceSoundBridge::invoke(Vm& provider_vm, const Script& caller,
                                             const Function& import, const std::vector<Value>& args) const {
    if (&caller != consumer_ || &provider_vm.script() != provider_
        || &import != &consumer_->functions.at(import_index_))
        throw std::runtime_error("Voice sound call belongs to a different script binding");
    // Re-check the exact binding before crossing VM boundaries. This is not a
    // general linker or an implicit conversion between arbitrary script types.
    if (unique_function(*consumer_, true) != import_index_
        || unique_function(*provider_, false) != export_index_
        || unique_actions(*consumer_, true) != consumer_enum_index_
        || unique_actions(*provider_, false) != provider_enum_index_)
        throw std::runtime_error("Voice sound binding changed");
    if (args.size() != 3 || !std::holds_alternative<std::string>(args[0])
        || !std::holds_alternative<RuntimeObjectHandle>(args[1]) || !std::holds_alternative<Word>(args[2]))
        throw std::runtime_error("Unsupported voice sound argument values");
    const auto object = std::get<RuntimeObjectHandle>(args[1]);
    if (object.kind != RuntimeObjectHandle::Kind::world_object || object.generation == 0)
        throw std::runtime_error("Voice sound requires a nonnull world-object handle");
    // Unrecognized numeric actions intentionally reach the original script;
    // its branches decide whether a native sound operation is requested.
    return provider_vm.invoke(provider_->functions[export_index_].name, args);
}
}
