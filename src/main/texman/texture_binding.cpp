#include "texture_binding.hpp"

#include "disc.hpp"

#include <cctype>
#include <stdexcept>

namespace omega {

std::string TextureBinder::key_of(const std::string& path) {
    const auto slash = path.find_last_of('\\');
    auto key = slash == std::string::npos ? path : path.substr(slash + 1);
    const auto dot = key.find('.');
    if (dot != std::string::npos) key.resize(dot);
    return key;
}

bool TextureBinder::same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

TextureBinder::Container TextureBinder::hog_container(std::string label, std::span<const std::uint8_t> hog) {
    Container container{std::move(label), hog_member_names(hog, hog.size()), {}};
    container.read = [hog](const std::string& member) {
        const auto extent = hog_member(hog, hog.size(), member);
        const auto bytes = hog.subspan(static_cast<std::size_t>(extent.offset), static_cast<std::size_t>(extent.size));
        return std::vector<std::uint8_t>(bytes.begin(), bytes.end());
    };
    return container;
}

std::int32_t TextureBinder::find(const std::string& key) {
    for (std::size_t i = 0; i < entries_.size(); ++i)
        if (same_name(entries_[i].key, key)) {
            ++entries_[i].references;
            return static_cast<std::int32_t>(i);
        }
    return absent;
}

std::int32_t TextureBinder::lookup_or_load(const std::string& name, const Container& container) {
    const auto path = name + ".tdx";
    const auto key = key_of(path);
    if (const auto index = find(key); index != absent) {
        ++stats_.reused;
        return index;
    }
    // 0037fea0: the member (0036d2a0, case-insensitive first match) must
    // exist (0036def0) and be smaller than 0x100041 (0036de50).
    for (const auto& member : container.names) {
        if (!same_name(member, path)) continue;
        auto bytes = container.read(member);
        if (bytes.size() >= size_limit) return absent;
        entries_.push_back({key, container.label, std::move(bytes), 1});
        ++stats_.loaded;
        return static_cast<std::int32_t>(entries_.size() - 1);
    }
    return absent;
}

std::vector<std::int32_t> TextureBinder::bind_model(const std::vector<std::string>& names, const Container& container) {
    std::vector<std::int32_t> result;
    result.reserve(names.size());
    for (const auto& name : names) {
        auto index = lookup_or_load(name, container);
        if (index == absent) {
            misses_.push_back(container.label + ":" + name);
            if (!error_set_) throw std::runtime_error("TextureBinder: ERROR.TDX fallback (00377570) not provided");
            index = find("ERROR");
            if (index == absent && !error_bytes_.empty() && error_bytes_.size() < size_limit) {
                entries_.push_back({"ERROR", "GAMEDATA\\COMMON", error_bytes_, 1});
                index = static_cast<std::int32_t>(entries_.size() - 1);
            }
            if (index == absent) ++stats_.unresolved;
            else ++stats_.error_fallbacks;
        }
        result.push_back(index);
    }
    return result;
}

}
