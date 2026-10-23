#include "texture_replacement.hpp"
#include "texture_blocks.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace omega::mods {
namespace {

thread_local bool decoding_for_registration = false;

struct Registration {
    ModSet::Source source;
    std::string key;
};

// A decoded replacement: the texture, or the decode's error.
struct Decoded {
    std::shared_ptr<const Texture> texture;
    std::string error;
    std::uint32_t full_width = 0, full_height = 0;   // 132md9: the file's size when the memory cap scaled it down
};
// 132md8 (research/MODDING.md "Speed"): a replacement registered by path (register_texture_paths) and not decoded
// yet: its cache key and the inputs replacement_texture takes from the original. attach_replacement decodes it when
// a decode first references it, so a level decodes only the replacements it draws (132md7 decoded every matched one
// of a read archive ahead, on threads).
struct Prefetch {
    std::string cache_key;
    std::filesystem::path path;
    bool uses_alpha = false, raw = false;
    std::uint32_t min_width = 1, min_height = 1;   // 132md9: the original's size (the memory cap's floor)
};

struct State {
    std::mutex mutex;
    std::uint64_t generation = ~0ull;
    std::set<std::string> archives;
    std::map<std::uint64_t, Registration> by_path;
    std::map<std::string, std::shared_ptr<const Texture>> images;
    std::set<std::string> failed;
    std::map<std::string, Prefetch> deferred;
    std::set<std::string> decoding;   // decoded outside the mutex by one caller; the others wait for `decoded`
    std::condition_variable decoded;
    std::map<std::string, Decoded> ready;   // decode_replacements_ahead's results, not yet attached
    std::uint64_t cap = 0;                   // 132md9: set_texture_memory_cap (0: none)
    std::uint32_t limit = 0;                 // 132md9: the side limit the last level's plan chose (0: none)
    // 132md10: the frames of TDX textures a files\ replacement supplies (register_file_textures; only under a cap),
    // by content hash, and the model images made under the cap (capped_replacement_texture), counted while held.
    struct FileTexture {
        std::string label;
        std::uint32_t min_width = 1, min_height = 1;   // the disc's frame (its own size when the disc has none)
    };
    std::map<std::uint64_t, FileTexture> files;
    std::atomic<bool> any_files{false};
    std::vector<std::weak_ptr<const Texture>> held;
    // The files\ textures drawn at their own size since the last level plan (no replacement holds them), by cache key:
    // their bytes count in the running total.
    std::map<std::string, std::uint64_t> file_full;
};

// attach_replacement's cache key of a replacement image for an original texture.
std::string cache_key_of(const std::filesystem::path& path, bool uses_alpha, bool raw) {
    return path.string() + (uses_alpha ? "|a" : "|-") + (raw ? "|raw" : "");
}

State& state() {
    static State s;
    return s;
}

// Caller holds the mutex.
void sync_generation(State& s) {
    const auto now = generation();
    if (s.generation == now) return;
    s.generation = now;
    s.archives.clear();
    s.by_path.clear();
    s.images.clear();
    s.failed.clear();
    s.deferred.clear();
    s.decoding.clear();
    s.ready.clear();
    s.limit = 0;
    s.files.clear();
    s.any_files.store(false, std::memory_order_relaxed);
    s.held.clear();
    s.file_full.clear();
    s.decoded.notify_all();
}

// The replacement a decoded frame resolves to (by archive path, else by content hash). Caller holds the mutex.
struct Resolved {
    ModSet::Source source;
    std::string label, cache_key;
};
std::optional<Resolved> resolve(const State& s, const ModSet& set, const Texture& texture, std::uint64_t hash) {
    Resolved r;
    if (const auto it = s.by_path.find(hash); it != s.by_path.end()) {
        r.source = it->second.source;
        r.label = "textures/" + it->second.key;
    } else if (const auto by_hash = set.texture_hash(hash)) {
        r.source = *by_hash;
        r.label = "textures/by-hash/" + hash_text(hash);
    } else {
        return std::nullopt;
    }
    r.cache_key = cache_key_of(r.source.path, texture.uses_alpha, !texture.raw_alpha.empty());
    return r;
}

struct Suppress {
    bool previous = decoding_for_registration;
    Suppress() { decoding_for_registration = true; }
    ~Suppress() { decoding_for_registration = previous; }
};

std::vector<std::uint8_t> box_halve(const std::vector<std::uint8_t>& rgba, std::uint32_t width, std::uint32_t height) {
    const auto w = std::max(1u, width / 2), h = std::max(1u, height / 2);
    std::vector<std::uint8_t> result(std::size_t(w) * h * 4);
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            for (std::uint32_t c = 0; c < 4; ++c) {
                const auto x0 = std::min(width - 1, x * 2), x1 = std::min(width - 1, x * 2 + 1);
                const auto y0 = std::min(height - 1, y * 2), y1 = std::min(height - 1, y * 2 + 1);
                const auto at = [&](std::uint32_t px, std::uint32_t py) { return rgba[(std::size_t(py) * width + px) * 4 + c]; };
                result[(std::size_t(y) * w + x) * 4 + c] =
                    static_cast<std::uint8_t>((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
            }
    return result;
}

struct Pending {
    std::vector<std::string> notes;
    ~Pending() {
        for (const auto& line : notes) note(line);
    }
};

void register_frame(State& s, const ModSet& set, const std::string& key, const Texture& frame,
                    std::set<std::string>& matched, Pending& pending, std::vector<Prefetch>& prefetch) {
    const auto source = set.texture(key);
    if (!source) return;
    matched.insert(key);
    const auto hash = texture_content_hash(frame);
    const auto [it, inserted] = s.by_path.emplace(hash, Registration{*source, key});
    if (!inserted && it->second.source.path != source->path)
        pending.notes.push_back("textures/" + key + " has the same original image as textures/" + it->second.key + "; using " +
                                it->second.source.mod->folder + "'s " + it->second.key);
    if (frame.width == 0 || frame.height == 0) return;
    const auto& path = it->second.source.path;
    auto cache_key = cache_key_of(path, frame.uses_alpha, !frame.raw_alpha.empty());
    if (s.images.contains(cache_key) || s.failed.contains(cache_key) || s.deferred.contains(cache_key)) return;
    if (std::any_of(prefetch.begin(), prefetch.end(), [&](const Prefetch& p) { return p.cache_key == cache_key; })) return;
    prefetch.push_back({std::move(cache_key), path, frame.uses_alpha, !frame.raw_alpha.empty(), frame.width, frame.height});
}

// 132md11: whether a block-compressed image holds every level down to 1 x 1.
bool full_chain(const BlockImage& image) {
    std::size_t levels = 1;
    for (auto w = image.width, h = image.height; w > 1 || h > 1; w = std::max(1u, w / 2), h = std::max(1u, h / 2)) ++levels;
    return image.levels.size() == levels;
}

// A replacement's decode (PNG / DDS, mip chain) as for an original with its alpha flags; 132md9: halved first while
// the memory cap's side limit (0: none) wants it smaller.
Decoded decode_one(const Prefetch& p, std::uint32_t limit) {
    Decoded result;
    try {
        Texture original;
        original.width = original.height = 1;
        original.uses_alpha = p.uses_alpha;
        if (p.raw) original.raw_alpha.emplace_back(1, std::uint8_t{0});
        const auto bytes = read_image_bytes(p.path);
        // 132md11: a BC1 / BC3 DDS with its full mip chain keeps its blocks (the memory cap drops its top levels);
        // an original with raw alpha needs RGBA (the raw alpha levels), so it is decoded as any other image.
        if (!p.raw && bytes.size() >= 4 && std::memcmp(bytes.data(), "DDS ", 4) == 0)
            if (auto blocks = dds_blocks(bytes); blocks && full_chain(*blocks)) {
                std::uint32_t width = blocks->width, height = blocks->height;
                if (limit) capped_size({width, height, p.min_width, p.min_height, blocks->block_bytes}, limit, width, height);
                std::size_t first = 0;
                while (std::max(1u, blocks->width >> first) != width || std::max(1u, blocks->height >> first) != height) ++first;
                if (first) {
                    result.full_width = blocks->width;
                    result.full_height = blocks->height;
                }
                auto texture = std::make_shared<Texture>();
                texture->width = width;
                texture->height = height;
                texture->uses_alpha = p.uses_alpha;
                texture->block_bytes = blocks->block_bytes;
                texture->blocks.assign(std::make_move_iterator(blocks->levels.begin() + static_cast<std::ptrdiff_t>(first)),
                                       std::make_move_iterator(blocks->levels.end()));
                result.texture = std::move(texture);
                return result;
            }
        auto image = decode_image(bytes);
        if (limit) {
            std::uint32_t width = 0, height = 0;
            capped_size({image.width, image.height, p.min_width, p.min_height}, limit, width, height);
            if (width != image.width || height != image.height) {
                result.full_width = image.width;
                result.full_height = image.height;
            }
            while (image.width != width || image.height != height) {
                image.rgba = box_halve(image.rgba, image.width, image.height);
                image.width = std::max(1u, image.width / 2);
                image.height = std::max(1u, image.height / 2);
            }
        }
        result.texture = std::make_shared<const Texture>(replacement_texture(image, original));
    } catch (const std::exception& e) {
        result.error = e.what();
    }
    return result;
}

void register_texture(State& s, const ModSet& set, const std::string& key, std::span<const std::uint8_t> image,
                      std::size_t offset, std::size_t size, std::set<std::string>& matched, Pending& pending,
                      std::vector<Prefetch>& prefetch) {
    Texture texture;
    try {
        Suppress suppress;
        try {
            texture = decode_tdx(image.subspan(offset, size));
        } catch (const std::exception&) {
            texture = decode_tdx(tdx_in_place(image, offset, size));
        }
    } catch (const std::exception& e) {
        pending.notes.push_back("textures/" + key + " ignored: the original texture does not decode (" + e.what() + ")");
        return;
    }
    register_frame(s, set, key, texture, matched, pending, prefetch);
    for (std::size_t n = 0; n < texture.animation.size(); ++n)
        register_frame(s, set, key + "." + std::to_string(n + 1), texture.animation[n], matched, pending, prefetch);
}

void register_archive(State& s, const ModSet& set, const std::string& key, std::span<const std::uint8_t> archive,
                      int depth, std::set<std::string>& matched, Pending& pending, std::vector<Prefetch>& prefetch) {
    std::vector<HogMember> members;
    try {
        members = hog_members(archive);
    } catch (const std::exception& e) {
        pending.notes.push_back("textures below " + key + " ignored: it is not a HOG archive (" + e.what() + ")");
        return;
    }
    for (const auto& member : members) {
        const auto child = member_key(key, member.name);
        if (set.has_texture(child)) register_texture(s, set, child, archive, member.offset, member.size, matched, pending, prefetch);
        if (set.has_textures_below(child) && depth < 4)
            register_archive(s, set, child, archive.subspan(member.offset, member.size), depth + 1, matched, pending, prefetch);
    }
}

}  // namespace

std::uint64_t texture_content_hash(const Texture& texture) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    const auto mix = [&](std::uint8_t byte) {
        hash ^= byte;
        hash *= 0x100000001b3ull;
    };
    for (int i = 0; i < 4; ++i) mix(static_cast<std::uint8_t>(texture.width >> (8 * i)));
    for (int i = 0; i < 4; ++i) mix(static_cast<std::uint8_t>(texture.height >> (8 * i)));
    for (const auto byte : texture.rgba) mix(byte);
    return hash;
}

Texture replacement_texture(const RgbaImage& image, const Texture& original) {
    if (image.width == 0 || image.height == 0 || image.rgba.size() != std::size_t(image.width) * image.height * 4)
        throw std::runtime_error("Invalid replacement image");
    Texture result;
    result.width = image.width;
    result.height = image.height;
    result.uses_alpha = original.uses_alpha;
    result.rgba = image.rgba;
    auto width = image.width, height = image.height;
    const std::vector<std::uint8_t>* level = &result.rgba;
    while (width > 1 || height > 1) {
        result.mips.push_back(box_halve(*level, width, height));
        level = &result.mips.back();
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
    if (!original.raw_alpha.empty()) {
        const auto raw = [](const std::vector<std::uint8_t>& rgba) {
            std::vector<std::uint8_t> alpha(rgba.size() / 4);
            for (std::size_t i = 0; i < alpha.size(); ++i) alpha[i] = static_cast<std::uint8_t>((rgba[i * 4 + 3] + 1) / 2);
            return alpha;
        };
        result.raw_alpha.push_back(raw(result.rgba));
        for (const auto& mip : result.mips) result.raw_alpha.push_back(raw(mip));
    }
    return result;
}

void register_texture_paths(const ModSet& set, const std::string& archive_key, std::span<const std::uint8_t> archive) {
    auto& s = state();
    Pending pending;  // noted after the lock is released
    std::lock_guard lock(s.mutex);
    sync_generation(s);
    if (!s.archives.insert(archive_key).second) return;
    std::set<std::string> matched;
    std::vector<Prefetch> prefetch;
    if (set.has_texture(archive_key)) register_texture(s, set, archive_key, archive, 0, archive.size(), matched, pending, prefetch);
    if (set.has_textures_below(archive_key)) register_archive(s, set, archive_key, archive, 0, matched, pending, prefetch);
    for (auto& p : prefetch) {
        auto key = p.cache_key;
        s.deferred.emplace(std::move(key), std::move(p));
    }
    auto expected = set.textures_below(archive_key);
    if (set.texture(archive_key)) expected.push_back(archive_key);
    for (const auto& key : expected)
        if (!matched.contains(key) && !s.failed.contains("unmatched " + key)) {
            s.failed.insert("unmatched " + key);
            pending.notes.push_back("textures/" + key + " matched no texture (no such member or animation frame)");
        }
}

namespace {

// 132md9: the side limit under a memory cap of an image decoded outside a level's batch (a front-end screen's, or
// one first referenced in play): the last level plan's, halved further while the cached images and this one exceed
// the cap (the cached images nothing holds are dropped first). Never below the original's size (capped_size).
// Caller holds the mutex. 132md10: the model images made under the cap (still held) count too.
std::uint32_t lazy_side(State& s, const CapItem& item, std::uint32_t limit) {
    const auto bytes = [&](std::uint32_t side) {
        std::uint32_t w = 0, h = 0;
        capped_size(item, side, w, h);
        return replacement_bytes(w, h, item.block_bytes);
    };
    const auto cached = [&] {
        std::uint64_t total = 0;
        for (const auto& [key, image] : s.images)
            if (image) total += texture_bytes(*image);
        for (const auto& weak : s.held)
            if (const auto image = weak.lock()) total += texture_bytes(*image);
        for (const auto& [key, size] : s.file_full) total += size;
        return total;
    };
    if (cached() + bytes(limit) <= s.cap) return limit;
    for (auto it = s.images.begin(); it != s.images.end();) it = it->second.use_count() == 1 ? s.images.erase(it) : std::next(it);
    std::erase_if(s.held, [](const std::weak_ptr<const Texture>& weak) { return weak.expired(); });
    const auto held = cached();
    auto side = limit ? limit : std::max(item.width, item.height);
    while (side > 1 && held + bytes(side) > s.cap) side /= 2;
    return side;
}

std::uint32_t lazy_limit(State& s, const Prefetch& p, std::uint32_t limit) {
    ImageSize size;
    try {
        size = read_image_size(p.path);
    } catch (const std::exception&) {
        return limit;   // the decode reports it
    }
    std::lock_guard lock(s.mutex);
    return lazy_side(s, {size.width, size.height, p.min_width, p.min_height, p.raw ? 0 : size.block_bytes}, limit);
}

// An image halved (box filter) to width x height.
void halve_to(RgbaImage& image, std::uint32_t width, std::uint32_t height) {
    while (image.width != width || image.height != height) {
        image.rgba = box_halve(image.rgba, image.width, image.height);
        image.width = std::max(1u, image.width / 2);
        image.height = std::max(1u, image.height / 2);
    }
}

std::string size_text(std::uint32_t width, std::uint32_t height) { return std::to_string(width) + "x" + std::to_string(height); }

// 132md10: a files\ texture's frame past the cap's side limit: its pixels halved as the replacement the GPU draws (the
// Texture keeps its size and pixels for every CPU reader). Its raw alpha (As > 1.0) is halved from the frame's own.
// Caller holds the mutex.
void attach_file_texture(State& s, Texture& texture, std::uint64_t hash, Pending& pending) {
    const auto it = s.files.find(hash);
    if (it == s.files.end() || !s.cap) return;
    const bool raw = !texture.raw_alpha.empty();
    const auto cache_key = "files|" + hash_text(hash) + (texture.uses_alpha ? "|a" : "|-") + (raw ? "|raw" : "");
    if (const auto cached = s.images.find(cache_key); cached != s.images.end()) {
        texture.replacement = cached->second;
        return;
    }
    if (s.failed.contains(cache_key) || s.file_full.contains(cache_key)) return;
    const CapItem item{texture.width, texture.height, it->second.min_width, it->second.min_height};
    std::uint32_t width = 0, height = 0;
    capped_size(item, lazy_side(s, item, s.limit), width, height);
    if (width == texture.width && height == texture.height) {   // under the cap: drawn as it is (and counted)
        s.file_full[cache_key] = replacement_bytes(width, height);
        return;
    }
    try {
        RgbaImage image{texture.width, texture.height, texture.rgba};
        halve_to(image, width, height);
        auto result = replacement_texture(image, texture);
        if (raw) {
            const auto& alpha = texture.raw_alpha.front();
            if (alpha.size() != std::size_t(texture.width) * texture.height) throw std::runtime_error("raw alpha size");
            RgbaImage as{texture.width, texture.height, std::vector<std::uint8_t>(alpha.size() * 4)};
            for (std::size_t i = 0; i < alpha.size(); ++i) as.rgba[i * 4 + 3] = alpha[i];
            halve_to(as, width, height);
            const auto levels = replacement_texture(as, Texture{});
            const auto take = [](const std::vector<std::uint8_t>& rgba) {
                std::vector<std::uint8_t> a(rgba.size() / 4);
                for (std::size_t i = 0; i < a.size(); ++i) a[i] = rgba[i * 4 + 3];
                return a;
            };
            result.raw_alpha.clear();
            result.raw_alpha.push_back(take(levels.rgba));
            for (const auto& mip : levels.mips) result.raw_alpha.push_back(take(mip));
        }
        const auto& cached = s.images[cache_key] = std::make_shared<const Texture>(std::move(result));
        texture.replacement = cached;
        pending.notes.push_back(it->second.label + ": " + size_text(texture.width, texture.height) + " drawn at " +
                                size_text(width, height) + " (scaled by the memory cap)");
    } catch (const std::exception& e) {
        s.failed.insert(cache_key);
        pending.notes.push_back(it->second.label + ": not scaled by the memory cap (" + e.what() + ")");
    }
}

void register_file_frames(State& s, const std::string& label, const Texture& mod, const Texture* disc, std::size_t& count) {
    const auto frame = [](const Texture& t, std::size_t n) -> const Texture* {
        return n == 0 ? &t : n <= t.animation.size() ? &t.animation[n - 1] : nullptr;
    };
    for (std::size_t n = 0; n <= mod.animation.size(); ++n) {
        const auto* f = frame(mod, n);
        if (f->width == 0 || f->height == 0) continue;
        const auto hash = texture_content_hash(*f);
        const auto* original = disc ? frame(*disc, n) : nullptr;
        if (original && texture_content_hash(*original) == hash) continue;   // the disc's own frame
        State::FileTexture entry{label + (n ? "." + std::to_string(n) : std::string()), f->width, f->height};
        if (original && original->width && original->height) {
            entry.min_width = original->width;
            entry.min_height = original->height;
        }
        s.files.emplace(hash, std::move(entry));
        ++count;
    }
}

std::optional<Texture> decode_member(std::span<const std::uint8_t> archive, std::size_t offset, std::size_t size) {
    try {
        Suppress suppress;
        try {
            return decode_tdx(archive.subspan(offset, size));
        } catch (const std::exception&) {
            return decode_tdx(tdx_in_place(archive, offset, size));
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool ends_with_upper(const std::string& text, std::string_view suffix) {
    return text.size() >= suffix.size() && upper(text.substr(text.size() - suffix.size())) == suffix;
}

// The TDX textures of a replaced file / archive (nested HOG members too) whose frames differ from the disc's.
void walk_file_textures(State& s, const std::string& key, std::span<const std::uint8_t> mod, std::size_t mod_offset,
                        std::size_t mod_size, std::span<const std::uint8_t> disc, std::optional<std::pair<std::size_t, std::size_t>> disc_at,
                        int depth, std::size_t& count) {
    const auto mine = mod.subspan(mod_offset, mod_size);
    if (disc_at && disc_at->second == mod_size && std::equal(mine.begin(), mine.end(), disc.begin() + disc_at->first)) return;
    if (ends_with_upper(key, ".TDX")) {
        const auto texture = decode_member(mod, mod_offset, mod_size);
        if (!texture) return;
        const auto original = disc_at ? decode_member(disc, disc_at->first, disc_at->second) : std::nullopt;
        register_file_frames(s, "files/" + key, *texture, original ? &*original : nullptr, count);
        return;
    }
    if (depth >= 4) return;
    std::vector<HogMember> members, originals;
    try {
        members = hog_members(mine);
    } catch (const std::exception&) {
        return;
    }
    if (disc_at) try {
            originals = hog_members(disc.subspan(disc_at->first, disc_at->second));
        } catch (const std::exception&) {
        }
    for (const auto& member : members) {
        const auto child = member_key(key, member.name);
        if (!ends_with_upper(child, ".TDX") && !ends_with_upper(child, ".HOG")) continue;
        std::optional<std::pair<std::size_t, std::size_t>> at;
        const auto it = std::find_if(originals.begin(), originals.end(),
                                     [&](const HogMember& m) { return upper(m.name) == upper(member.name); });
        if (it != originals.end()) at = std::pair{disc_at->first + it->offset, it->size};
        walk_file_textures(s, child, mod, mod_offset + member.offset, member.size, disc, at, depth + 1, count);
    }
}

}  // namespace

void attach_replacement(Texture& texture) {
    if (decoding_for_registration) return;
    const auto* set = active();
    auto& s = state();
    if (!set || texture.width == 0 || texture.height == 0) return;
    const bool files = s.any_files.load(std::memory_order_relaxed);
    if (!set->has_textures() && !files) return;
    const auto hash = texture_content_hash(texture);
    Pending pending;
    std::unique_lock lock(s.mutex);
    sync_generation(s);
    const auto resolved = resolve(s, *set, texture, hash);
    if (!resolved) {
        if (files) attach_file_texture(s, texture, hash, pending);
        return;
    }
    const auto& source = resolved->source;
    const auto& label = resolved->label;
    const auto& cache_key = resolved->cache_key;
    // Another caller decoding the same image: wait for it (its result is cached or failed below).
    s.decoded.wait(lock, [&] { return !s.decoding.contains(cache_key); });
    if (s.failed.contains(cache_key)) return;
    if (const auto cached = s.images.find(cache_key); cached != s.images.end()) {
        texture.replacement = cached->second;
        return;
    }
    Decoded decoded;
    if (const auto ahead = s.ready.find(cache_key); ahead != s.ready.end()) {
        decoded = std::move(ahead->second);
        s.ready.erase(ahead);
        s.deferred.erase(cache_key);
    } else {
        Prefetch input{cache_key, source.path, texture.uses_alpha, !texture.raw_alpha.empty(), texture.width, texture.height};
        if (const auto registered = s.deferred.find(cache_key); registered != s.deferred.end()) {
            input = std::move(registered->second);
            s.deferred.erase(registered);
        }
        const auto at = s.generation;
        const auto limit = s.limit;
        const bool capped = s.cap != 0;
        s.decoding.insert(cache_key);
        lock.unlock();
        decoded = decode_one(input, capped ? lazy_limit(s, input, limit) : limit);
        lock.lock();
        if (s.generation != at) return;   // the mods changed meanwhile (the marker is gone): the original is drawn
        s.decoding.erase(cache_key);
        s.decoded.notify_all();
    }
    if (!decoded.texture) {
        s.failed.insert(cache_key);
        pending.notes.push_back(label + " from " + source.mod->folder + " not used (" + decoded.error +
                                "); the original is drawn");
        return;
    }
    const auto& cached = s.images[cache_key] = std::move(decoded.texture);
    pending.notes.push_back(label + " <- " + source.mod->folder + " (" + std::to_string(texture.width) + "x" +
                            std::to_string(texture.height) + " -> " + std::to_string(cached->width) + "x" +
                            std::to_string(cached->height) +
                            (decoded.full_width ? ", scaled from " + std::to_string(decoded.full_width) + "x" +
                                                      std::to_string(decoded.full_height) + " by the memory cap"
                                                : std::string()) +
                            ")");
    texture.replacement = cached;
}

WithoutReplacements::WithoutReplacements() : previous(decoding_for_registration) { decoding_for_registration = true; }
WithoutReplacements::~WithoutReplacements() { decoding_for_registration = previous; }

namespace {

// 132md9: the memory cap's plan for a level's images (decode_replacements_ahead with a cap). Sets the side limit,
// drops the cached images the level does not reference that nothing holds, and the cached or decoded-ahead images
// of the level whose size differs from the plan's (decoded again below). Returns false if the mods changed meanwhile.
bool plan_level(State& s, const ModSet& set, std::span<const Texture* const> originals, std::uint64_t& at) {
    std::map<std::string, Prefetch> level;
    std::map<std::string, CapItem> file_items;   // 132md10: the level's files\ textures (their own pixels)
    {
        std::lock_guard lock(s.mutex);
        sync_generation(s);
        at = s.generation;
        s.file_full.clear();   // a new level: its files\ textures are counted again as they are attached
        const auto add = [&](const Texture& frame) {
            if (frame.width == 0 || frame.height == 0) return;
            const auto hash = texture_content_hash(frame);
            const auto resolved = resolve(s, set, frame, hash);
            if (!resolved) {
                if (const auto file = s.files.find(hash); file != s.files.end())
                    file_items.emplace("files|" + hash_text(hash) + (frame.uses_alpha ? "|a" : "|-") + (frame.raw_alpha.empty() ? "" : "|raw"),
                                       CapItem{frame.width, frame.height, file->second.min_width, file->second.min_height});
                return;
            }
            if (level.contains(resolved->cache_key)) return;
            const auto registered = s.deferred.find(resolved->cache_key);
            level[resolved->cache_key] =
                registered != s.deferred.end()
                    ? registered->second
                    : Prefetch{resolved->cache_key, resolved->source.path, frame.uses_alpha, !frame.raw_alpha.empty(),
                               frame.width, frame.height};
        };
        for (const auto* original : originals) {
            if (!original) continue;
            add(*original);
            for (const auto& frame : original->animation) add(frame);
        }
    }
    if (level.empty() && file_items.empty()) return true;
    // The files' sizes from their headers, outside the mutex (a file that cannot be read fails its decode later).
    std::vector<CapItem> items;
    std::map<std::string, CapItem> sizes = file_items;
    for (const auto& [key, item] : file_items) items.push_back(item);
    for (const auto& [key, p] : level) try {
            const auto size = read_image_size(p.path);
            const CapItem item{size.width, size.height, p.min_width, p.min_height, p.raw ? 0 : size.block_bytes};
            items.push_back(item);
            sizes[key] = item;
        } catch (const std::exception&) {
        }
    std::lock_guard lock(s.mutex);
    if (s.generation != at) return false;
    const auto plan = plan_texture_cap(items, s.cap);
    s.limit = plan.limit;
    const auto mb = [](std::uint64_t bytes) {
        const auto tenths = (bytes * 10 + (1u << 19)) >> 20;
        return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " MB";
    };
    note("texture memory cap " + mb(s.cap) + ": the level's " + std::to_string(items.size()) +
         " replacement images take " + mb(plan.full_bytes) +
         (!plan.limit ? std::string(", under the cap")
                      : ", scaled to at most " + std::to_string(plan.limit) + " px a side: " + mb(plan.planned_bytes) +
                            (plan.fits ? std::string() : " (over the cap at their originals' sizes)")));
    const auto planned = [&](const std::string& key, std::uint32_t width, std::uint32_t height) {
        const auto it = sizes.find(key);
        if (it == sizes.end()) return true;
        std::uint32_t w = 0, h = 0;
        capped_size(it->second, plan.limit, w, h);
        return w == width && h == height;
    };
    for (auto it = s.images.begin(); it != s.images.end();) {
        const bool used = level.contains(it->first) || file_items.contains(it->first);
        if ((!used && it->second.use_count() == 1) || (used && !planned(it->first, it->second->width, it->second->height))) {
            if (level.contains(it->first)) s.deferred.emplace(it->first, level[it->first]);
            it = s.images.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = s.ready.begin(); it != s.ready.end();) {
        if (it->second.texture && level.contains(it->first) &&
            !planned(it->first, it->second.texture->width, it->second.texture->height)) {
            s.deferred.emplace(it->first, level[it->first]);
            it = s.ready.erase(it);
        } else {
            ++it;
        }
    }
    return true;
}

}  // namespace

void decode_replacements_ahead(std::span<const Texture* const> originals) {
    const auto* set = active();
    auto& s = state();
    if (!set || (!set->has_textures() && !s.any_files.load(std::memory_order_relaxed))) return;
    std::vector<Prefetch> work;
    std::uint64_t at = 0;
    std::uint32_t limit = 0;
    bool capped;
    {
        std::lock_guard lock(s.mutex);
        capped = s.cap != 0;
    }
    if (capped && !plan_level(s, *set, originals, at)) return;
    {
        std::lock_guard lock(s.mutex);
        sync_generation(s);
        if (capped && s.generation != at) return;
        at = s.generation;
        limit = s.limit;
        std::set<std::string> taken;
        const auto add = [&](const Texture& frame) {
            if (frame.width == 0 || frame.height == 0) return;
            const auto resolved = resolve(s, *set, frame, texture_content_hash(frame));
            if (!resolved) return;
            const auto& key = resolved->cache_key;
            if (s.images.contains(key) || s.failed.contains(key) || s.decoding.contains(key) || s.ready.contains(key) ||
                !taken.insert(key).second)
                return;
            if (const auto registered = s.deferred.find(key); registered != s.deferred.end())
                work.push_back(registered->second);
            else
                work.push_back({key, resolved->source.path, frame.uses_alpha, !frame.raw_alpha.empty(), frame.width,
                                frame.height});
        };
        for (const auto* original : originals) {
            if (!original) continue;
            add(*original);
            for (const auto& frame : original->animation) add(frame);
        }
        // Claimed: a concurrent attach_replacement of one of these waits for it rather than decoding it again.
        for (const auto& p : work) s.decoding.insert(p.cache_key);
    }
    if (work.empty()) return;
    std::vector<Decoded> results(work.size());
    std::atomic<std::size_t> next{0};
    const auto run = [&] {
        for (std::size_t i; (i = next.fetch_add(1)) < work.size();) results[i] = decode_one(work[i], limit);
    };
    const auto threads = std::min<std::size_t>(work.size() - 1, std::max(1u, std::thread::hardware_concurrency()) - 1);
    std::vector<std::thread> pool;
    try {
        for (std::size_t t = 0; t < threads; ++t) pool.emplace_back(run);
    } catch (const std::system_error&) {
        // fewer threads: the rest is decoded below
    }
    run();
    for (auto& t : pool) t.join();
    std::lock_guard lock(s.mutex);
    if (s.generation != at) return;   // the mods changed meanwhile (the markers are gone)
    for (std::size_t i = 0; i < work.size(); ++i) {
        s.decoding.erase(work[i].cache_key);
        s.ready[work[i].cache_key] = std::move(results[i]);
    }
    s.decoded.notify_all();
}

void attach_replacements(Texture& texture) {
    attach_replacement(texture);
    for (auto& frame : texture.animation) attach_replacement(frame);
}

void reset_texture_replacements() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.generation = ~0ull;
    s.archives.clear();
    s.by_path.clear();
    s.images.clear();
    s.failed.clear();
    s.deferred.clear();
    s.decoding.clear();
    s.ready.clear();
    s.limit = 0;
    s.file_full.clear();
    s.decoded.notify_all();
}

std::shared_ptr<const Texture> capped_replacement_texture(const RgbaImage& image, const Texture& base) {
    auto& s = state();
    std::unique_lock lock(s.mutex);
    if (!s.cap) {
        lock.unlock();
        return std::make_shared<const Texture>(replacement_texture(image, base));
    }
    sync_generation(s);
    const CapItem item{image.width, image.height, std::max(1u, base.width), std::max(1u, base.height)};
    std::uint32_t width = 0, height = 0;
    capped_size(item, lazy_side(s, item, s.limit), width, height);
    lock.unlock();
    std::shared_ptr<const Texture> result;
    if (width == image.width && height == image.height) {
        result = std::make_shared<const Texture>(replacement_texture(image, base));
    } else {
        auto scaled = image;
        halve_to(scaled, width, height);
        result = std::make_shared<const Texture>(replacement_texture(scaled, base));
        note("model image " + size_text(image.width, image.height) + " drawn at " + size_text(width, height) +
             " (scaled by the memory cap)");
    }
    lock.lock();
    s.held.push_back(result);
    return result;
}

void register_file_textures(const std::string& key, std::span<const std::uint8_t> replaced, std::span<const std::uint8_t> disc) {
    auto& s = state();
    std::size_t count = 0;
    std::lock_guard lock(s.mutex);
    if (!s.cap) return;
    sync_generation(s);
    walk_file_textures(s, key, replaced, 0, replaced.size(), disc, std::pair<std::size_t, std::size_t>{0, disc.size()}, 0, count);
    if (count) s.any_files.store(true, std::memory_order_relaxed);
}

void register_file_texture(const std::string& label, const Texture& replaced, const Texture* disc) {
    auto& s = state();
    std::size_t count = 0;
    std::lock_guard lock(s.mutex);
    if (!s.cap) return;
    sync_generation(s);
    register_file_frames(s, label, replaced, disc, count);
    if (count) s.any_files.store(true, std::memory_order_relaxed);
}

void set_texture_memory_cap(std::uint64_t bytes) {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    s.cap = bytes;
    if (!bytes) s.limit = 0;
}

std::uint64_t texture_memory_cap() {
    auto& s = state();
    std::lock_guard lock(s.mutex);
    return s.cap;
}

std::uint64_t replacement_bytes(std::uint32_t width, std::uint32_t height, std::uint32_t block_bytes) {
    std::uint64_t bytes = 0;
    for (;;) {
        bytes += block_bytes ? gfx::block_level_bytes(width, height, block_bytes) : std::uint64_t(width) * height * 4;
        if (width <= 1 && height <= 1) return bytes;
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
    }
}

RgbaImage resample_image(const RgbaImage& image, std::uint32_t width, std::uint32_t height) {
    if (image.width == 0 || image.height == 0 || image.rgba.size() != std::size_t(image.width) * image.height * 4 || !width || !height)
        throw std::runtime_error("Invalid image to resample");
    if (width == image.width && height == image.height) return image;
    // The input texels each output texel covers along one axis, with their coverage.
    struct Tap {
        std::uint32_t at;
        double weight;
    };
    const auto taps = [](std::uint32_t from, std::uint32_t to) {
        std::vector<std::vector<Tap>> result(to);
        const double scale = double(from) / to;
        for (std::uint32_t i = 0; i < to; ++i) {
            const double begin = i * scale, end = (i + 1) * scale;
            for (auto k = static_cast<std::uint32_t>(begin); k < from && k < end; ++k) {
                const double cover = std::min(end, k + 1.0) - std::max(begin, double(k));
                if (cover > 0) result[i].push_back({k, cover / scale});
            }
        }
        return result;
    };
    const auto across = taps(image.width, width), down = taps(image.height, height);
    std::vector<double> rows(std::size_t(image.height) * width * 4);
    for (std::uint32_t y = 0; y < image.height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            for (const auto& tap : across[x])
                for (int c = 0; c < 4; ++c)
                    rows[(std::size_t(y) * width + x) * 4 + c] += tap.weight * image.rgba[(std::size_t(y) * image.width + tap.at) * 4 + c];
    RgbaImage out{width, height, std::vector<std::uint8_t>(std::size_t(width) * height * 4)};
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            for (int c = 0; c < 4; ++c) {
                double sum = 0;
                for (const auto& tap : down[y]) sum += tap.weight * rows[(std::size_t(tap.at) * width + x) * 4 + c];
                out.rgba[(std::size_t(y) * width + x) * 4 + c] = static_cast<std::uint8_t>(std::clamp(sum + 0.5, 0.0, 255.0));
            }
    return out;
}

PlatformTexture platform_texture(const RgbaImage& image, std::uint32_t max_side) {
    if (image.width == 0 || image.height == 0 || image.rgba.size() != std::size_t(image.width) * image.height * 4)
        throw std::runtime_error("Invalid image to convert");
    const auto side = [&](std::uint32_t size) {
        std::uint32_t pot = 4;
        while (pot * 2 <= size) pot *= 2;
        return pot;
    };
    std::uint32_t width = side(image.width), height = side(image.height);
    const auto limit = std::max(4u, max_side);
    while (width > limit || height > limit) {
        width = std::max(4u, width / 2);
        height = std::max(4u, height / 2);
    }
    const auto sized = resample_image(image, width, height);
    bool opaque = true;
    for (std::size_t i = 3; i < sized.rgba.size() && opaque; i += 4) opaque = sized.rgba[i] == 255;
    const auto chain = replacement_texture(sized, Texture{});
    BlockImage blocks{width, height, opaque ? 8u : 16u, {}};
    blocks.levels.push_back(gfx::encode_blocks(chain.rgba, width, height, blocks.block_bytes));
    for (std::size_t level = 0; level < chain.mips.size(); ++level)
        blocks.levels.push_back(gfx::encode_blocks(chain.mips[level], std::max(1u, width >> (level + 1)), std::max(1u, height >> (level + 1)),
                                                   blocks.block_bytes));
    return {encode_dds(blocks), width, height, blocks.block_bytes};
}

std::uint64_t texture_bytes(const Texture& texture) {
    if (!texture.block_bytes) return replacement_bytes(texture.width, texture.height);
    std::uint64_t bytes = 0;
    for (const auto& level : texture.blocks) bytes += level.size();
    return bytes;
}

void capped_size(const CapItem& item, std::uint32_t limit, std::uint32_t& width, std::uint32_t& height) {
    width = item.width;
    height = item.height;
    if (!limit) return;
    while ((width > limit || height > limit) && (width > 1 || height > 1)) {
        const auto w = std::max(1u, width / 2), h = std::max(1u, height / 2);
        if (w < item.min_width || h < item.min_height) return;
        width = w;
        height = h;
    }
}

CapPlan plan_texture_cap(std::span<const CapItem> items, std::uint64_t cap_bytes) {
    CapPlan plan;
    const auto total = [&](std::uint32_t limit) {
        std::uint64_t bytes = 0;
        for (const auto& item : items) {
            std::uint32_t w = 0, h = 0;
            capped_size(item, limit, w, h);
            bytes += replacement_bytes(w, h, item.block_bytes);
        }
        return bytes;
    };
    plan.full_bytes = plan.planned_bytes = total(0);
    if (!cap_bytes || plan.full_bytes <= cap_bytes) return plan;
    std::uint32_t largest = 1;
    for (const auto& item : items) largest = std::max({largest, item.width, item.height});
    std::uint32_t limit = 1;
    while (limit * 2 < largest) limit *= 2;   // the largest limit that scales an image
    for (;; limit /= 2) {
        plan.limit = limit;
        plan.planned_bytes = total(limit);
        if (plan.planned_bytes <= cap_bytes) return plan;
        if (limit == 1) break;
    }
    plan.fits = false;
    return plan;
}

namespace {

struct WideCache {
    std::mutex mutex;
    std::uint64_t generation = ~0ull;
    std::map<std::string, std::shared_ptr<const std::vector<WideImage>>, std::less<>> by_screen;
};

WideCache& wide_cache() {
    static WideCache cache;
    return cache;
}

bool wider_than_4_3(std::uint64_t width, std::uint64_t height) { return width && height && width * 3 > height * 4; }

}  // namespace

std::shared_ptr<const std::vector<WideImage>> wide_background_images(std::string_view screen) {
    const ModSet* set = active();
    if (!set || set->wide_backgrounds().empty()) return nullptr;
    const auto* wide = set->wide_background(screen);
    if (!wide) return nullptr;
    auto& c = wide_cache();
    std::lock_guard lock(c.mutex);   // a screen's images are decoded once; a second caller waits for them
    if (c.generation != generation()) {
        c.by_screen.clear();
        c.generation = generation();
    }
    auto& found = c.by_screen[wide->screen];
    if (found) return found;
    auto images = std::make_shared<std::vector<WideImage>>();
    for (const auto& path : wide->images) {
        const auto name = path.lexically_relative(wide->mod->root).generic_u8string();
        const auto label = "wide." + wide->screen + " " + wide->mod->folder + "/" + std::string(name.begin(), name.end());
        try {
            const auto image = read_image_file(path);
            Texture base;
            base.width = std::min(image.width, 640u);
            base.height = std::min(image.height, 448u);
            auto texture = capped_replacement_texture(image, base);
            note(label + ": " + size_text(image.width, image.height) +
                 (wider_than_4_3(image.width, image.height) ? "" : " (not wider than 4:3: never drawn)"));
            images->push_back({std::move(texture), image.width, image.height});
        } catch (const std::exception& e) {
            note(label + ": not used (" + e.what() + ")");
        }
    }
    found = std::move(images);
    return found;
}

const WideImage* pick_wide_background(std::span<const WideImage> images, unsigned width, unsigned height) {
    if (!wider_than_4_3(width, height)) return nullptr;
    const double target = std::log(static_cast<double>(width) / height);
    const WideImage* best = nullptr;
    double best_distance = 0.0;
    for (const auto& image : images) {
        if (!image.texture || !wider_than_4_3(image.width, image.height)) continue;
        const double distance = std::abs(std::log(static_cast<double>(image.width) / image.height) - target);
        const bool wider = best && std::uint64_t(image.width) * best->height > std::uint64_t(best->width) * image.height;
        if (!best || distance < best_distance - 1e-9 || (distance <= best_distance + 1e-9 && wider)) {
            best = &image;
            best_distance = distance;
        }
    }
    return best;
}

WideRect wide_background_rect(std::uint32_t image_width, std::uint32_t image_height, unsigned width, unsigned height) {
    if (!image_width || !image_height || !width || !height) return {};
    const double w = static_cast<double>(height) * image_width / image_height;
    return {(static_cast<double>(width) - w) / 2.0, 0.0, w, static_cast<double>(height)};
}

}  // namespace omega::mods
