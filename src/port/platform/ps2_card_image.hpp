#pragma once
// Host import of a PS2 memory card image (the PCSX2 .ps2 file, with or
// without the 16 ECC bytes per page) into the PC card folder FolderAgentCard
// reads (agent_card.hpp). Nothing here is from the game: it is the standard
// PS2 card file system (superblock, indirect FAT, 512-byte directory entries),
// used so a save made on the PS2 build can be loaded by the native front end.
//
// Checkpoint 132win13 (port-only save management): the other direction too. Ps2CardImage::format makes a
// fresh 8 MB card as the PS2 formats one (the layout of a PCSX2 card: 528-byte pages of 512 data bytes, 12 ECC
// bytes and 4 FF bytes, 1024-byte clusters, the FAT at clusters 9..40 behind the indirect cluster 8, 8135
// allocatable clusters from 41, the two backup erase blocks at the end), replace_directory rewrites one root
// directory (its old clusters freed, the other directories untouched) and every page written gets its ECC
// (the PS2 card ECC: 3 bytes per 128-byte chunk). export_ps2_card_directory writes the PC card's
// BASCUS-97264 into a card image, the reverse of import_ps2_card_directory.
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace omega {

class FolderAgentCard;

// A directory entry's time (+8 created, +18 modified): the PS2 clock's JST (UTC+9) fields.
struct Ps2CardTime {
    std::uint8_t second = 0, minute = 0, hour = 0, day = 1, month = 1;
    std::uint16_t year = 2000;
    bool operator==(const Ps2CardTime&) const = default;
};
// The current time as the PS2 clock keeps it (UTC + 9 h).
Ps2CardTime ps2_card_time_now();

struct Ps2CardFile {
    std::string name;
    std::vector<std::uint8_t> bytes;
    bool operator==(const Ps2CardFile&) const = default;
};

struct Ps2CardEntry {
    std::uint16_t mode = 0;      // +0 (0x8000 exists, 0x20 directory, 0x10 file)
    std::uint32_t length = 0;    // +4 (bytes; directories: entry count)
    std::uint32_t cluster = 0;   // +10 (relative to the allocation offset)
    std::string name;            // +40 (32 bytes, NUL terminated)
    bool exists() const { return (mode & 0x8000) != 0; }
    bool directory() const { return (mode & 0x20) != 0; }
    bool file() const { return (mode & 0x10) != 0; }
};

class Ps2CardImage {
public:
    // Throws when the image is not a formatted card of a known page layout.
    explicit Ps2CardImage(std::vector<std::uint8_t> image);
    static Ps2CardImage read(const std::filesystem::path& path);

    std::vector<Ps2CardEntry> root() const;
    // The existing entries of a directory, without "." and "..".
    std::vector<Ps2CardEntry> list(const Ps2CardEntry& directory) const;
    std::vector<std::uint8_t> read(const Ps2CardEntry& file) const;

    std::uint32_t page_size() const { return page_len_; }
    std::uint32_t cluster_size() const { return page_len_ * pages_per_cluster_; }
    bool has_ecc() const { return raw_page_ == page_len_ + 16; }

    // Checkpoint 132win13 (port-only). A freshly formatted 8 MB card with ECC (8,650,752 bytes): the empty root
    // ("." and "..") made at `time`.
    static Ps2CardImage format(const Ps2CardTime& time);
    // The root directory `name` (created when missing) with exactly these files, in this order: the directory's
    // old clusters and its files' are freed, new ones taken first-fit; its created time is kept when it existed.
    // Throws when the card has too few free clusters (nothing is changed then).
    void replace_directory(const std::string& name, const std::vector<Ps2CardFile>& files, const Ps2CardTime& time);
    // The files of root directory `name` in directory order (nullopt when there is none).
    std::optional<std::vector<Ps2CardFile>> directory_files(const std::string& name) const;
    std::uint32_t free_clusters() const;
    // Pages whose spare area holds an ECC that does not match the data (erased pages, data and spare all FF,
    // count as good); 0 for an image without ECC.
    std::size_t bad_ecc_pages() const;
    const std::vector<std::uint8_t>& bytes() const { return image_; }
    // Written to <path>.tmp~ and renamed over path.
    void write(const std::filesystem::path& path) const;

private:
    std::vector<std::uint8_t> cluster(std::uint32_t absolute) const;
    std::uint32_t fat(std::uint32_t relative) const;
    std::vector<std::uint8_t> chain(std::uint32_t relative, std::size_t bytes) const;
    std::vector<Ps2CardEntry> entries(std::uint32_t relative, std::uint32_t count) const;
    // 132win13: the writer's pieces.
    std::vector<std::uint32_t> chain_clusters(std::uint32_t relative) const;
    void write_cluster(std::uint32_t absolute, std::span<const std::uint8_t> data);
    void set_fat(std::uint32_t relative, std::uint32_t value);
    void write_chain(const std::vector<std::uint32_t>& clusters, std::span<const std::uint8_t> data);

    std::vector<std::uint8_t> image_;
    std::uint32_t page_len_ = 0, raw_page_ = 0, pages_per_cluster_ = 0, clusters_ = 0;
    std::uint32_t alloc_offset_ = 0, alloc_end_ = 0, root_cluster_ = 0;
    std::vector<std::uint32_t> ifc_;
};

// The 3 ECC bytes of one 128-byte chunk (the PS2 card's Hamming code; a page's spare area holds its 4 chunks'
// 12 bytes, then FF FF FF FF).
std::array<std::uint8_t, 3> ps2_card_ecc(std::span<const std::uint8_t, 128> chunk);

struct Ps2CardImport {
    std::vector<std::string> copied;   // files written
    std::vector<std::string> kept;     // files already in the folder (never overwritten)
    bool found = false;                // the card holds the directory
};

// Copies every file of the card directory `directory` (e.g. BASCUS-97264)
// into the PC card through FolderAgentCard::write (so the names get its disk
// spelling), skipping entries the PC card already lists.
// Checkpoint 132win13: with replace, the folder is made equal to the card directory instead (files that differ
// are overwritten, files the card lacks removed; `kept` then lists the files already equal).
Ps2CardImport import_ps2_card_directory(const Ps2CardImage& card, const std::string& directory,
                                        FolderAgentCard& folder, bool replace = false);

struct Ps2CardExport {
    std::vector<std::string> written;      // the PC card's files
    std::vector<std::string> kept_icons;   // icon.sys / MEMCARD.ICN / <directory> kept from the card's directory
    std::vector<std::string> added_icons;  // 132win14: icon files added from `icons` (ps2_save_icons.hpp)
    bool unchanged = false;                // the card's directory already held exactly these files (not rewritten)
};

// Checkpoint 132win13 (port-only): the PC card's save directory written into `card` as the root directory
// `directory`. The PS2 build's 0040d740 also writes the PS2 browser's icon files (MEMCARD.ICN, icon.sys and a
// file named as the directory) that the PC card never makes: the card's own copies are kept when the PC card
// has none. Files keep the card directory's order; new ones follow in the PC card's order.
// Checkpoint 132win14: `icons` (the files 0040d740 writes first, ps2_save_icon_files) are put first, in their order,
// each one that neither the PC card nor the card's directory has (added_icons).
Ps2CardExport export_ps2_card_directory(const FolderAgentCard& folder, const std::string& directory, Ps2CardImage& card,
                                        const Ps2CardTime& time, const std::vector<Ps2CardFile>& icons = {});

}  // namespace omega
