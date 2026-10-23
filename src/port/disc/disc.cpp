#include "disc.hpp"
#include "binary.hpp"
#include "data_overrides.hpp"
#include "mod_set.hpp"
#include "texture_replacement.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace omega {
namespace {

constexpr std::uint64_t sector_size = 2048;
constexpr std::uint64_t read_limit = 16 * 1024 * 1024;

std::uint32_t little(std::span<const std::uint8_t> data, std::size_t offset) {
    return read_u32(data, offset);
}

Extent iso_extent(std::span<const std::uint8_t> record) {
    if (record.size() < 34 || record[0] != record.size()) throw std::runtime_error("Invalid ISO directory record");
    if (record[1] || (record[25] & 0x80) || record[26] || record[27])
        throw std::runtime_error("Unsupported extended/interleaved/multi-extent ISO record");
    return {std::uint64_t(little(record, 2)) * sector_size, little(record, 10)};
}

bool identifier(std::span<const std::uint8_t> descriptor) {
    static constexpr std::uint8_t expected[]{'C', 'D', '0', '0', '1'};
    return std::equal(std::begin(expected), std::end(expected), descriptor.begin() + 1);
}

// Port-only (checkpoint 132md1, research/MODDING.md): the mod-resolved files, process-wide so an extent from one
// DiscImage reads the same bytes through any other. Slots are never released (extents stay valid); the resolution
// cache is per mods::install generation.
constexpr std::uint64_t virtual_slot_bits = 32;
constexpr std::size_t virtual_slot_limit = 1024;

struct VirtualFiles {
    std::mutex mutex;
    std::uint64_t generation = ~0ull;
    std::map<std::string, std::optional<Extent>> resolved;  // key -> its virtual extent, or nullopt: the disc's
    std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> slots;
};

VirtualFiles& virtual_files() {
    static VirtualFiles v;
    return v;
}

// Port-only (132vt2a2, research/VITA.md "Audio"): one seek + read at a time on the ISO streams. The PS Vita's sound
// mix thread reads the music's VPK chunks (SoundIopHost::decode_ahead) while the session reads other files from the
// same stream; interleaved, a chunk read another file's bytes ("VPK frame with flags": abort at level select's
// Triangle). The Windows hosts read from one thread; the bytes read are unchanged.
std::mutex& stream_mutex() {
    static std::mutex m;
    return m;
}

}

void DiscImage::check(Extent extent) const {
    if (extent.offset >= virtual_base) {
        const auto slot = (extent.offset - virtual_base) >> virtual_slot_bits;
        const auto within = (extent.offset - virtual_base) & ((1ull << virtual_slot_bits) - 1);
        auto& v = virtual_files();
        std::lock_guard lock(v.mutex);
        if (slot >= v.slots.size() || extent.size > (1ull << virtual_slot_bits) - within)
            throw std::runtime_error("Disc extent outside ISO");
        return;
    }
    if (extent.offset > size_ || extent.size > size_ - extent.offset)
        throw std::runtime_error("Disc extent outside ISO");
}

std::vector<std::uint8_t> DiscImage::read(Extent extent) {
    if (extent.offset < virtual_base) return read_disc(extent, false);
    check(extent);
    if (extent.size > read_limit) throw std::runtime_error("Disc read exceeds prototype allocation limit");
    const auto slot = (extent.offset - virtual_base) >> virtual_slot_bits;
    const auto within = (extent.offset - virtual_base) & ((1ull << virtual_slot_bits) - 1);
    std::shared_ptr<const std::vector<std::uint8_t>> buffer;
    {
        auto& v = virtual_files();
        std::lock_guard lock(v.mutex);
        buffer = v.slots.at(static_cast<std::size_t>(slot));
    }
    // Bytes past the resolved file read as zero (the disc would give the next file's bytes).
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(extent.size), 0);
    if (within < buffer->size()) {
        const auto count = std::min<std::uint64_t>(extent.size, buffer->size() - within);
        std::copy_n(buffer->begin() + static_cast<std::ptrdiff_t>(within), static_cast<std::size_t>(count), bytes.begin());
    }
    std::lock_guard lock(stream_mutex());
    ++stats_.reads;
    stats_.bytes += bytes.size();
    return bytes;
}

std::vector<std::uint8_t> DiscImage::read_disc(Extent extent, bool unlimited) {
    check(extent);
    if (!unlimited && extent.size > read_limit) throw std::runtime_error("Disc read exceeds prototype allocation limit");
    const auto begun = std::chrono::steady_clock::now();
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(extent.size));
    std::lock_guard lock(stream_mutex());
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(extent.offset));
    if (!stream_) throw std::runtime_error("Could not seek in ISO");
    if (!bytes.empty()) {
        stream_.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (stream_.gcount() != static_cast<std::streamsize>(bytes.size()))
            throw std::runtime_error("Short ISO read");
    }
    ++stats_.reads;
    stats_.bytes += bytes.size();
    stats_.seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - begun).count();
    return bytes;
}

Extent DiscImage::file(const std::vector<std::string>& components) {
    const auto extent = disc_file(components);
    const auto* set = original_only_ ? nullptr : mods::active();
    if (!set) return extent;
    const auto key = mods::asset_key(components);
    const bool files = set->file(key) || set->has_files_below(key);
    const bool textures = set->has_texture(key) || set->has_textures_below(key);
    const bool data = set->has_data(key);
    if (!files && !textures && !data) return extent;
    auto& v = virtual_files();
    std::lock_guard lock(v.mutex);
    if (v.generation != mods::generation()) {
        v.generation = mods::generation();
        v.resolved.clear();
    }
    if (const auto it = v.resolved.find(key); it != v.resolved.end()) return it->second ? *it->second : extent;
    std::optional<Extent> result;
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
    if (files || data) {
        std::optional<std::vector<std::uint8_t>> resolved;
        if (files) try {
                resolved = mods::resolve_bytes(*set, key, [&] { return read_disc(extent, true); });
                if (resolved && resolved->size() >= (1ull << virtual_slot_bits))
                    throw std::runtime_error("the replaced file exceeds 4 GiB");
                if (resolved && v.slots.size() >= virtual_slot_limit) throw std::runtime_error("too many replaced disc files");
            } catch (const std::exception& e) {
                mods::note("files/" + key + " not used (" + e.what() + "); the disc's file is read");
                resolved.reset();
            }
        if (data) try {
                // 132md16: data_overrides= fields patched over the winning bytes (a files\ replacement's or the disc's).
                if (!resolved && v.slots.size() >= virtual_slot_limit) throw std::runtime_error("too many replaced disc files");
                const auto base = resolved ? *resolved : read_disc(extent, true);
                if (auto patched = mods::apply_data_overrides(*set, key, base)) resolved = std::move(patched);
            } catch (const std::exception& e) {
                mods::note("data overrides of " + key + " not used (" + e.what() + ")");
            }
        if (resolved && mods::texture_memory_cap()) try {
                // 132md10: the replaced file's own textures count toward the texture memory cap.
                mods::register_file_textures(key, *resolved, read_disc(extent, true));
            } catch (const std::exception& e) {
                mods::note("files/" + key + ": textures not checked against the memory cap (" + e.what() + ")");
            }
        if (resolved) {
            bytes = std::make_shared<const std::vector<std::uint8_t>>(std::move(*resolved));
            v.slots.push_back(bytes);
            result = Extent{virtual_base + (std::uint64_t(v.slots.size() - 1) << virtual_slot_bits), bytes->size()};
        }
    }
    if (textures) {
        try {
            if (!bytes) bytes = std::make_shared<const std::vector<std::uint8_t>>(read_disc(extent, true));
            mods::register_texture_paths(*set, key, *bytes);
        } catch (const std::exception& e) {
            mods::note("textures below " + key + " not used (" + e.what() + ")");
        }
    }
    v.resolved[key] = result;
    return result ? *result : extent;
}

Extent DiscImage::disc_file(const std::vector<std::string>& components) {
    if (components.empty()) throw std::runtime_error("Empty ISO path");
    std::optional<Extent> directory;
    for (std::uint64_t sector = 16; sector < 256 && sector < size_ / sector_size; ++sector) {
        const auto descriptor = read({sector * sector_size, sector_size});
        if (!identifier(descriptor)) throw std::runtime_error("Expected a plain ISO9660 image");
        if (descriptor[0] == 1) {
            const auto length = descriptor[156];
            if (length < 34 || std::size_t(length) > descriptor.size() - 156)
                throw std::runtime_error("Invalid ISO root record");
            const auto record = std::span(descriptor).subspan(156, length);
            if (!(record[25] & 2)) throw std::runtime_error("ISO root is not a directory");
            directory = iso_extent(record);
            check(*directory);
            break;
        }
        if (descriptor[0] == 255) break;
    }
    if (!directory) throw std::runtime_error("ISO primary volume descriptor not found");
    for (std::size_t part = 0; part < components.size(); ++part) {
        const auto& requested = components[part];
        if (requested.empty() || requested == "." || requested == ".." || requested.find_first_of("/\\:") != std::string::npos)
            throw std::runtime_error("Invalid ISO path component");
        const auto bytes = read(*directory);
        std::optional<Extent> found;
        bool is_directory = false;
        for (std::size_t cursor = 0; cursor < bytes.size();) {
            const auto length = bytes[cursor];
            if (length == 0) {
                cursor = (cursor / sector_size + 1) * sector_size;
                continue;
            }
            if (length > bytes.size() - cursor || length > sector_size - cursor % sector_size)
                throw std::runtime_error("Truncated ISO directory record");
            const auto record = std::span(bytes).subspan(cursor, length);
            const auto extent = iso_extent(record);
            check(extent);
            const auto name_length = record[32];
            if (name_length > record.size() - 33) throw std::runtime_error("Truncated ISO filename");
            cursor += length;
            if (name_length == 1 && record[33] <= 1) continue;
            std::string name(reinterpret_cast<const char*>(record.data() + 33), name_length);
            const auto version = name.find(';');
            if (version != std::string::npos) name.resize(version);
            if (name != requested) continue;
            if (found) throw std::runtime_error("Ambiguous ISO path component");
            found = extent;
            is_directory = (record[25] & 2) != 0;
        }
        if (!found) throw std::runtime_error("ISO file component not found: " + requested);
        if (is_directory != (part + 1 < components.size()))
            throw std::runtime_error("Unexpected ISO file/directory kind");
        directory = found;
    }
    return *directory;
}

std::vector<std::string> hog_member_names(std::span<const std::uint8_t> index, std::uint64_t total_size) {
    if (index.size() < 20) throw std::runtime_error("Truncated HOG header");
    const auto count = little(index, 4);
    const auto table = little(index, 8);
    const auto names = little(index, 12);
    const auto data = little(index, 16);
    if (count == 0 || count > 100000 || table < 20 || table >= names || names > data
        || data > total_size || data > index.size()
        || std::uint64_t(table) + (std::uint64_t(count) + 1) * 4 > names)
        throw std::runtime_error("Invalid HOG index bounds");
    std::vector<std::string> result;
    std::size_t name_offset = names;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::string name;
        while (name_offset < data && index[name_offset] != 0) {
            const auto byte = index[name_offset++];
            if (byte < 32 || byte >= 127) throw std::runtime_error("Invalid HOG member name");
            name.push_back(static_cast<char>(byte));
        }
        if (name.empty() || name_offset >= data) throw std::runtime_error("Unterminated HOG member name");
        ++name_offset;
        result.push_back(std::move(name));
    }
    return result;
}

Extent hog_member(std::span<const std::uint8_t> index, std::uint64_t total_size,
                  const std::string& requested) {
    if (index.size() < 20) throw std::runtime_error("Truncated HOG header");
    const auto count = little(index, 4);
    const auto table = little(index, 8);
    const auto names = little(index, 12);
    const auto data = little(index, 16);
    if (count == 0 || count > 100000 || table < 20 || table >= names || names > data
        || data > total_size || data > index.size()
        || std::uint64_t(table) + (std::uint64_t(count) + 1) * 4 > names)
        throw std::runtime_error("Invalid HOG index bounds");
    if (little(index, table) != 0) throw std::runtime_error("HOG first offset is not zero");
    std::size_t name_offset = names;
    std::optional<Extent> result;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto start = little(index, table + std::size_t(i) * 4);
        const auto end = little(index, table + std::size_t(i + 1) * 4);
        if (start > end || std::uint64_t(data) + end > total_size)
            throw std::runtime_error("HOG member outside archive");
        std::string name;
        while (name_offset < data && index[name_offset] != 0) {
            const auto byte = index[name_offset++];
            if (byte < 32 || byte >= 127) throw std::runtime_error("Invalid HOG member name");
            name.push_back(static_cast<char>(byte));
        }
        if (name.empty() || name_offset >= data) throw std::runtime_error("Unterminated HOG member name");
        ++name_offset;
        if (name == requested) {
            if (result) throw std::runtime_error("Duplicate HOG member name");
            result = Extent{std::uint64_t(data) + start, std::uint64_t(end) - start};
        }
    }
    if (!result) throw std::runtime_error("HOG member not found: " + requested);
    return *result;
}

Extent DiscImage::archive_member_extent(const std::vector<std::string>& archive,
                                        const std::string& member) {
    const auto container = file(archive);
    if (container.size < 20) throw std::runtime_error("Truncated HOG archive");
    const auto header = read({container.offset, 20});
    const auto index_size = little(header, 16);
    if (index_size < 20 || index_size > container.size) throw std::runtime_error("Invalid HOG header size");
    const auto index = read({container.offset, index_size});
    const auto extent = hog_member(index, container.size, member);
    const Extent absolute{container.offset + extent.offset, extent.size};
    check(absolute);
    return absolute;
}

std::vector<std::uint8_t> DiscImage::archive_member(const std::vector<std::string>& archive,
                                                  const std::string& member) {
    return read(archive_member_extent(archive, member));
}

std::vector<std::uint8_t> DiscImage::archive_bytes_after(const std::vector<std::string>& archive,
                                                         const std::string& member, std::size_t count) {
    const auto container = file(archive);
    const auto extent = archive_member_extent(archive, member);
    const auto start = extent.offset + extent.size;
    const auto end = container.offset + container.size;
    if (start > end) throw std::runtime_error("HOG member outside its archive");
    return read({start, std::min<std::uint64_t>(count, end - start)});
}

std::vector<std::string> DiscImage::archive_member_names(const std::vector<std::string>& archive) {
    const auto container = file(archive);
    if (container.size < 20) throw std::runtime_error("Truncated HOG archive");
    const auto header = read({container.offset, 20});
    const auto index_size = little(header, 16);
    if (index_size < 20 || index_size > container.size) throw std::runtime_error("Invalid HOG header size");
    return hog_member_names(read({container.offset, index_size}), container.size);
}

std::vector<DiscImage::ArchiveMember> DiscImage::archive_members(const std::vector<std::string>& archive) {
    const auto container = file(archive);
    if (container.size < 20) throw std::runtime_error("Truncated HOG archive");
    const auto header = read({container.offset, 20});
    const auto index_size = little(header, 16);
    if (index_size < 20 || index_size > container.size) throw std::runtime_error("Invalid HOG header size");
    const auto index = read({container.offset, index_size});
    const auto names = hog_member_names(index, container.size);
    const auto table = little(index, 8);
    const auto data = little(index, 16);
    std::vector<ArchiveMember> result;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto start = little(index, table + i * 4), end = little(index, table + (i + 1) * 4);
        const Extent absolute{container.offset + data + start, std::uint64_t(end) - start};
        check(absolute);
        result.push_back({names[i], absolute});
    }
    return result;
}

void write_bytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("Cannot create extracted script");
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) throw std::runtime_error("Failed to write extracted script");
}

}
