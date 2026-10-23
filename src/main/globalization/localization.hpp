#pragma once

#include "string_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
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
    std::size_t base_count() const { return base_.entries().size(); }
    std::size_t mission_count() const { return mission_ ? mission_->entries().size() : 0; }
private:
    StringPool base_;
    std::optional<StringPool> mission_;
};

void write_localization_probe(std::ostream& out, const StringPool& base, const StringPool& mission);

}
