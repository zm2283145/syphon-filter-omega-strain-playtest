#include "string_pool.hpp"

#include <algorithm>
#include <stdexcept>

namespace omega {
namespace {
void check(bool condition) {
    if (!condition) throw std::runtime_error("Malformed or unsupported string pool");
}
void normalize(std::array<std::uint8_t, 16>& key) {
    const auto nul = std::find(key.begin(), key.end(), 0);
    check(nul != key.end());
    for (auto cursor = key.begin(); cursor != nul; ++cursor) {
        check(*cursor >= 32 && *cursor <= 126);
        if (*cursor >= 'A' && *cursor <= 'Z') *cursor += 'a' - 'A';
    }
}
}

StringPool decode_string_pool(std::span<const std::uint8_t> bytes) {
    check(bytes.size() >= 4 && bytes.size() <= 16 * 1024 * 1024);
    std::size_t cursor = 0;
    const auto word = [&]() {
        check(cursor <= bytes.size() && bytes.size() - cursor >= 4);
        const auto result = static_cast<std::uint32_t>(bytes[cursor])
            | static_cast<std::uint32_t>(bytes[cursor + 1]) << 8
            | static_cast<std::uint32_t>(bytes[cursor + 2]) << 16
            | static_cast<std::uint32_t>(bytes[cursor + 3]) << 24;
        cursor += 4;
        return result;
    };
    const auto count = word();
    check(count <= 65536 && count <= (bytes.size() - 4) / 24);
    StringPool result;
    result.entries_.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        check(bytes.size() - cursor >= 20);
        StringPoolEntry entry;
        std::copy_n(bytes.begin() + cursor, 16, entry.key.begin());
        cursor += 16;
        normalize(entry.key);
        const auto length = word();
        check(length > 0 && length <= 1024 * 1024);
        const auto padded = (static_cast<std::size_t>(length) + 3) & ~std::size_t{3};
        check(padded <= bytes.size() - cursor);
        const auto payload = bytes.subspan(cursor, length);
        const auto nul = std::find(payload.begin(), payload.end(), 0);
        check(nul != payload.end());
        entry.text.assign(reinterpret_cast<const char*>(payload.data()),
                          static_cast<std::size_t>(nul - payload.begin()));
        cursor += padded;
        result.entries_.push_back(std::move(entry));
    }
    check(cursor == bytes.size());
    return result;
}

std::int32_t StringPool::lookup(std::string_view value) const {
    check(value.size() <= 15);
    std::array<std::uint8_t, 16> key{};
    for (std::size_t i = 0; i < value.size(); ++i) {
        // Embedded NUL arguments are outside this explicit string-view boundary.
        check(static_cast<unsigned char>(value[i]) >= 32 && static_cast<unsigned char>(value[i]) <= 126);
        key[i] = static_cast<std::uint8_t>(value[i]);
    }
    normalize(key);
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].key == key) return static_cast<std::int32_t>(i);
    }
    return -1;
}
}
