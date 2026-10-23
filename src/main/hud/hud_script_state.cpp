#include "hud_script_state.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace omega {

namespace {
auto find_callout(std::vector<HudCallout>& callouts, std::uint32_t key, std::uint32_t slot) {
    return std::find_if(callouts.begin(), callouts.end(),
                        [&](const HudCallout& c) { return c.key == key && c.slot == slot; });
}
} // namespace

bool HudScriptState::set_callout(std::uint32_t key, std::uint32_t slot, std::string text, float value, bool flag_18) {
    const auto found = find_callout(callouts_, key, slot);
    if (found != callouts_.end()) {
        found->text = std::move(text);   // 003e7ec0(record +c, text)
        found->value = value;            // swc1 f20, 4(record)
        return false;
    }
    callouts_.push_back({key, value, slot, std::move(text), flag_18});
    return true;
}

bool HudScriptState::clear_callout(std::uint32_t key, std::uint32_t slot) {
    const auto found = find_callout(callouts_, key, slot);
    if (found == callouts_.end()) return false;
    callouts_.erase(found);
    return true;
}

std::string transfer_ammo_notice(const Localization& localization, std::string_view record_name_80) {
    const auto name = localization.resolve(record_name_80);
    const auto suffix = localization.resolve(std::string_view("TransferAmmo"));
    if (!name || !suffix)
        throw std::runtime_error("0013fdb8: TransferAmmo notice with an unresolved string (sprintf of NULL): " +
                                 std::string(name ? "TransferAmmo" : record_name_80));
    return std::string(*name) + " " + std::string(*suffix);
}

} // namespace omega
