#pragma once

#include <cstdint>
#include <filesystem>
#include <istream>
#include <span>
#include <string>
#include <vector>

namespace omega {

struct Extent {
    std::uint64_t offset{};
    std::uint64_t size{};
};

class DiscImage {
public:
    DiscImage(std::istream& stream, std::uint64_t size) : stream_(stream), size_(size) {}
    std::vector<std::uint8_t> read(Extent extent);
    // With a mod set installed (mods::install, research/MODDING.md; port-only, checkpoint 132md1) a file a mod
    // replaces, or an archive with replaced members, resolves to a virtual extent (offset >= virtual_base) that read()
    // serves from the resolved bytes, in every DiscImage; archive_member* follow it. Otherwise the disc's extent.
    Extent file(const std::vector<std::string>& components);
    std::vector<std::uint8_t> archive_member(const std::vector<std::string>& archive,
                                           const std::string& member);
    // Absolute ISO extent of a HOG member (for members too large for read()).
    Extent archive_member_extent(const std::vector<std::string>& archive, const std::string& member);
    // The archive's bytes after a member (the next member's start: the offset table makes members contiguous), at most
    // `count`, fewer at the archive's end. An archive Hog_Load reads into memory keeps them after the member.
    std::vector<std::uint8_t> archive_bytes_after(const std::vector<std::string>& archive, const std::string& member,
                                                  std::size_t count);
    // Member names in archive index order.
    std::vector<std::string> archive_member_names(const std::vector<std::string>& archive);
    // Port-only (checkpoint 132win8, the launcher's derived data): names with absolute ISO extents, index order.
    struct ArchiveMember {
        std::string name;
        Extent extent;
    };
    std::vector<ArchiveMember> archive_members(const std::vector<std::string>& archive);
    // Port-only diagnostic (checkpoint 132win5, the front end's --frontend-profile): the reads made so far.
    struct ReadStats {
        std::uint64_t reads = 0, bytes = 0;
        double seconds = 0.0;
    };
    const ReadStats& read_stats() const { return stats_; }
    // Port-only (checkpoint 132md1): ignore installed mods (the disc's own bytes: derived data, hashes, exports).
    void set_original_only(bool original_only) { original_only_ = original_only; }
    bool original_only() const { return original_only_; }
    // The start of the virtual extents of mod-resolved files (each slot 4 GiB, sector numbers fit 32 bits).
    static constexpr std::uint64_t virtual_base = 1ull << 40;
private:
    std::istream& stream_;
    std::uint64_t size_;
    ReadStats stats_;
    bool original_only_ = false;
    void check(Extent extent) const;
    Extent disc_file(const std::vector<std::string>& components);
    std::vector<std::uint8_t> read_disc(Extent extent, bool unlimited);
};

Extent hog_member(std::span<const std::uint8_t> index, std::uint64_t total_size,
                  const std::string& name);
// Member names in index order (the same bounds checks as hog_member).
std::vector<std::string> hog_member_names(std::span<const std::uint8_t> index, std::uint64_t total_size);
void write_bytes(const std::filesystem::path& path, std::span<const std::uint8_t> bytes);

}
