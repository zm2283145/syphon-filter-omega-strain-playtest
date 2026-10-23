#pragma once
// Port-only (checkpoint 132md10, research/MODDING.md "Mod archives"): single-file mods. A mod may be a .zip in the mods
// folder instead of a folder: the archive holds what the folder would (manifest.txt, files\, textures\, models\,
// sounds\) at its root, or inside one top folder that holds manifest.txt. The game and omega_mod_tool read the members
// in place (nothing is extracted): a registered archive's path acts as a folder, so "<mods>\hd.zip\textures\by-hash\
// 0123456789abcdef.png" names a member, and every mod file read (mods::read_file, the image readers, the glTF loader,
// the mod set's listing, the online fingerprint) goes through read_mod_file / mod_files_below below.
//   - Read: the classic zip format (no zip64, no encryption, no multi-disk), members stored or deflated (method 0 / 8),
//     each read checked against its CRC-32; names '/'- or '\'-separated, no absolute or ".." components, no two names
//     equal ignoring case (a folder on Windows could not hold both). Directory entries are skipped.
//   - Write (omega_mod_tool pack, the launcher's load-order change): deterministic bytes for the same inputs (names
//     sorted, a fixed 1980-01-01 time stamp, no extra fields); each member deflated (LZ77 with the fixed Huffman code)
//     or stored when that is not smaller (PNG / OGG data is already compressed); movies (.mp4 / .m4v / .mov / .m2v /
//     .pss) are always stored, so the player streams them in place (132md15).
// Thread-safe: an archive is immutable once opened and each read opens its own stream; the registry is mutex-guarded
// (and costs one relaxed load when no archive was ever registered: the no-mod path).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega::mods {

struct ZipMember {
    std::string name;                  // '/'-separated, as stored (after '\' -> '/')
    std::uint16_t method{};            // 0 stored, 8 deflate
    std::uint32_t crc{}, compressed{}, size{};
    std::uint32_t local_offset{};
};

class ZipArchive {
public:
    // Reads the central directory. Throws std::runtime_error naming the archive and the problem.
    static std::shared_ptr<const ZipArchive> open(const std::filesystem::path& path);
    const std::filesystem::path& path() const { return path_; }
    // In name order (ordinal).
    const std::vector<ZipMember>& members() const { return members_; }
    // By exact name, else ignoring ASCII case; nullptr when absent.
    const ZipMember* find(std::string_view name) const;
    std::vector<std::uint8_t> read(const ZipMember& member) const;
    // The first `count` bytes (or fewer); a stored member reads only those.
    std::vector<std::uint8_t> read_prefix(const ZipMember& member, std::size_t count) const;
    // The member's data as stored (compressed for method 8), at most count bytes; not checked.
    std::vector<std::uint8_t> read_raw(const ZipMember& member, std::size_t count) const;

private:
    std::filesystem::path path_;
    std::vector<ZipMember> members_;
};

struct ZipInput {
    std::string name;                  // '/'-separated
    std::vector<std::uint8_t> bytes;
};
// The archive bytes; throws on an empty, repeated (ignoring case) or unsafe name, or a member past 2 GiB.
std::vector<std::uint8_t> write_zip(std::vector<ZipInput> inputs);
// write_zip to `path` through a temporary file beside it, then a rename (an open reader keeps the old file intact).
void write_zip_file(const std::filesystem::path& path, std::vector<ZipInput> inputs);
// The archive at `path` with member `name` replaced by (or added as) `bytes`; the other members' stored data is copied
// unchanged (not compressed again). Written as write_zip_file does; the archive is registered again (register_archive).
void replace_zip_member(const std::filesystem::path& path, std::string_view name, std::vector<std::uint8_t> bytes);
// Raw RFC 1951 deflate (one fixed-Huffman block); inflate_raw (image_files.hpp) reverses it.
std::vector<std::uint8_t> deflate_raw(std::span<const std::uint8_t> data);

// A file name ending in ".zip" (any case).
bool is_zip_name(const std::filesystem::path& path);

// The registry of mod archives (the mod set's scan and listing register each archive they open).
// Opens (or re-opens: a rewritten archive) the archive and registers it under its absolute path; throws when invalid.
std::shared_ptr<const ZipArchive> register_archive(const std::filesystem::path& zip);
// The registered archive whose path is a proper prefix of `path` (with `member` set to the rest, '/'-separated), or
// nullptr: a plain file.
std::shared_ptr<const ZipArchive> archive_holding(const std::filesystem::path& path, std::string& member);
// A file below a registered archive's path: its member's bytes (throws when the archive has no such member); nullopt
// for a path outside every registered archive (the caller reads the file system).
std::optional<std::vector<std::uint8_t>> read_archived(const std::filesystem::path& path);
// The regular files below `folder` (recursive, sorted), from the archive when `folder` is (inside) a registered
// archive, else from the file system (empty when not a folder).
std::vector<std::filesystem::path> mod_files_below(const std::filesystem::path& folder);
// Whether `path` names a regular file (an archive member when inside a registered archive).
bool mod_file_exists(const std::filesystem::path& path);

}  // namespace omega::mods
