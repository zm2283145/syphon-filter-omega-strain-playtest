#pragma once

#include "string_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace omega {

// Fresh common-pool lifetime plus one replaceable mission pool. Language/file
// selection, original allocation arenas and unrelated runtime strings are absent.
class Localization {
public:
    explicit Localization(StringPool base);
    void replace_mission(StringPool mission);
    std::int32_t lookup(std::optional<std::string_view> key) const;
    std::optional<std::string_view> text(std::int32_t id) const;
    std::optional<std::string_view> resolve(std::optional<std::string_view> key) const;
    // 003fdae0 nGlobalization_GetStringTag(id): "" (004be400) for a negative id, else the
    // pool row's 16-byte key (lower case) up to its NUL: the base pool below base_count(),
    // the mission pool's row id - base_count() above (134on3, for NetLabelMsg 002d2650).
    std::string tag(std::int32_t id) const;
    std::size_t base_count() const { return base_.entries().size(); }
    std::size_t mission_count() const { return mission_ ? mission_->entries().size() : 0; }
private:
    StringPool base_;
    std::optional<StringPool> mission_;
};

void write_localization_probe(std::ostream& out, const StringPool& base, const StringPool& mission);

}
