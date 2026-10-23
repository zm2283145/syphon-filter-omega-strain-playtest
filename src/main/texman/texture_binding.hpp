#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace omega {

// Model of the texture manager (texman, global 00539248) as reached from the
// vu1model texture bind 00377570:
//  - each model texture name becomes the path "<name>.tdx" in the model's own
//    texture container (0036dfb0 with the model's texture context);
//  - 00380a40 keys the texture by the path's basename (after the last '\',
//    up to the first '.'), compared case-insensitively (00128e80); a key
//    already registered is reused (refcount +0x64) whatever container it came
//    from;
//  - otherwise 0037fea0 loads it: the member lookup 0036d2a0 is a
//    case-insensitive first match, a missing member or a size >= 0x100041
//    gives -1 and nothing is registered;
//  - on -1, 00377570 binds GAMEDATA\COMMON\ERROR.TDX instead (004bc190),
//    through the same lookup (key "ERROR").
// GOB models (003951b0) use the object's own <NAME>.HOG inside OBJECTS.HOG as
// their texture context; terrain (001c2cb0) and the sky (001c1b80) use the
// mission TEX.HOG.
class TextureBinder {
public:
    static constexpr std::int32_t absent = -1;
    static constexpr std::uint64_t size_limit = 0x100041;

    struct Entry {
        std::string key;        // basename, as first registered
        std::string container;  // label of the container it was loaded from
        std::vector<std::uint8_t> bytes;
        std::uint32_t references{};
    };
    struct Stats {
        std::size_t reused{}, loaded{}, error_fallbacks{}, unresolved{};
    };

    // A texture container: its member names in index order and a reader for
    // an exact member name. hog_container wraps a HOG image in memory.
    struct Container {
        std::string label;
        std::vector<std::string> names;
        std::function<std::vector<std::uint8_t>(const std::string&)> read;
    };
    static Container hog_container(std::string label, std::span<const std::uint8_t> hog);

    // 00380a40 on "<name>.tdx" in the container; the registered index or -1.
    std::int32_t lookup_or_load(const std::string& name, const Container& container);
    // The ERROR.TDX fallback (plain file GAMEDATA\COMMON\ERROR.TDX).
    void set_error_texture(std::vector<std::uint8_t> bytes) { error_bytes_ = std::move(bytes); error_set_ = true; }
    // 00377570: one index per model texture name, ERROR.TDX for misses.
    std::vector<std::int32_t> bind_model(const std::vector<std::string>& names, const Container& container);

    const std::vector<Entry>& entries() const { return entries_; }
    const Stats& stats() const { return stats_; }
    const std::vector<std::string>& misses() const { return misses_; }

    static std::string key_of(const std::string& path);
    static bool same_name(const std::string& a, const std::string& b);

private:
    std::int32_t find(const std::string& key);
    std::vector<Entry> entries_;
    std::vector<std::uint8_t> error_bytes_;
    bool error_set_{};
    Stats stats_;
    std::vector<std::string> misses_;
};

}
