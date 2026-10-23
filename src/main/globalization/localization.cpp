#include "localization.hpp"

#include <algorithm>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace omega {
Localization::Localization(StringPool base) : base_(std::move(base)) {
    if (base_count() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()))
        throw std::runtime_error("Localization ID range exceeded");
}

void Localization::replace_mission(StringPool mission) {
    if (mission.entries().size() > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) - base_count())
        throw std::runtime_error("Localization ID range exceeded");
    mission_ = std::move(mission);
}

std::int32_t Localization::lookup(std::optional<std::string_view> key) const {
    if (!key) return -1;
    const auto base_id = base_.lookup(*key);
    if (base_id >= 0) return base_id;
    if (!mission_) return -1;
    const auto mission_id = mission_->lookup(*key);
    return mission_id < 0 ? -1 : static_cast<std::int32_t>(base_count()) + mission_id;
}

std::optional<std::string_view> Localization::text(std::int32_t id) const {
    if (id < 0) return std::nullopt;
    const auto index = static_cast<std::size_t>(id);
    if (index < base_count()) return std::string_view(base_.entries()[index].text);
    const auto mission_index = index - base_count();
    if (!mission_ || mission_index >= mission_count())
        throw std::runtime_error("Localization text ID outside loaded pools");
    return std::string_view(mission_->entries()[mission_index].text);
}

std::optional<std::string_view> Localization::resolve(std::optional<std::string_view> key) const {
    return text(lookup(key));
}

void write_localization_probe(std::ostream& out, const StringPool& base, const StringPool& mission) {
    Localization localization(base);
    localization.replace_mission(mission);
    const auto hex = [&](std::span<const std::uint8_t> bytes) {
        constexpr char digits[] = "0123456789abcdef";
        out << '"';
        for (const auto byte : bytes) out << digits[byte >> 4] << digits[byte & 15];
        out << '"';
    };
    out << "{\"base_count\":" << localization.base_count()
        << ",\"mission_count\":" << localization.mission_count() << ",\"entries\":[";
    bool first = true;
    const auto rows = [&](const StringPool& pool, const char* origin, std::size_t offset) {
        for (std::size_t i = 0; i < pool.entries().size(); ++i) {
            if (!first) out << ',';
            first = false;
            const auto& entry = pool.entries()[i];
            const auto prefix_size = static_cast<std::size_t>(std::find(entry.key.begin(), entry.key.end(), 0) - entry.key.begin());
            const std::string_view key(reinterpret_cast<const char*>(entry.key.data()), prefix_size);
            const auto id = static_cast<std::int32_t>(offset + i);
            out << "{\"origin\":\"" << origin << "\",\"index\":" << i << ",\"id\":" << id << ",\"key_hex\":";
            hex(entry.key);
            out << ",\"text_hex\":";
            const auto text_value = localization.text(id).value();
            hex(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text_value.data()), text_value.size()));
            out << ",\"resolved_id\":" << localization.lookup(key) << '}';
        }
    };
    rows(base, "base", 0);
    rows(mission, "mission", localization.base_count());
    out << "]}\n";
}
}
