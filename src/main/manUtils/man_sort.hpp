#pragma once
// The man part classification (CHARACTER_APPEARANCE.md, FLASHLIGHT.md §17): the MANSORT2 table the
// part cache 0055dda0 parses (00403490, GAMEDATA\FRONTEND\MANSORT2.TXT, records at cache +11c10, count
// +ec = 0055de8c) and Appearance_ReorderParts 00390570, which orders a model's parts by it and gives each
// one the thermal texture class (+2dd0) the thermal draw 0038a918 binds (D_0053B360 + 2 class, filled
// by 0038dd40 at Pop_LoadNPC). Interpreter goldens: tools/ee/gen_man_sort.py.
#include "appearance.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace omega {
using ManSortKey = std::array<std::uint8_t, 16>;

// One record (0x30 bytes): "%s %d %f %f %f %f %f %f %f" (D_004be800). The first int is "the IR texture
// number to use for that part" (the file's own comment); fov, location and rotation follow.
struct ManSortRecord {
    ManSortKey key{};                    // +0, the %s token (bytes past its terminator as the slot held them)
    std::int32_t thermal_class{};        // +10
    float fov{};                         // +14
    std::array<float, 3> location{};     // +18
    std::array<float, 3> rotation{};     // +24
};

// 00403490(cache): the file is compacted in place line by line. Each byte is copied, except NUL, '#',
// '$', '*', '\t' and '\n', which are dropped; '/' drops itself and every byte up to the next '\r' (its
// line is cut there). At '\r' the line ends (the byte after it is skipped) and is scanned with sscanf
// 00128c68 into the next record slot: 9 fields accept it (count + 1), -1 (nothing scanned) leaves the
// slot, any other count zeroes its key (the slot is reused). Text after the last '\r' is not scanned.
// %f converts with strtod and truncates the double toward zero to a float (the interpreter's result).
// A %s token that would write past its 0x30-byte record throws.
std::vector<ManSortRecord> parse_man_sort(std::span<const std::uint8_t> text);

// 00390570(model) over the pair table model +80 (pair i: the mesh key +80 + 20 i, the texture key
// +90 + 20 i; up to 0x20 pairs, a missing pair reads as zero). The part count +23c0 is the number of
// pairs before the first zero mesh key. A record applies to a part when the texture key's bytes 1..4
// equal the record key's (the category); its score is 1, + 2 when the mesh key's bytes 6..8 (cache mask
// +60) equal the record key's, + 1 for bytes 10..13 (+70), + 8 for byte 0 (+30), + 4 for byte 5 (+40).
// Each part keeps the first record with the highest score. A selection sort then moves the matched parts
// to the front by record index (the first minimum, swapping pairs and their +22c0 / +2300 handles): the
// position gets +2db0 = the record index and +2dd0 = its class (as bytes). The remaining positions up to
// 0x20: a pair with a mesh key gets 2db0 0 and 2dd0 1; an empty one the last record and its class.
struct ManSortOrder {
    std::uint32_t part_count{};                   // +23c0
    std::vector<std::size_t> order;               // 0x20 positions: the original pair index now there
    std::array<std::uint8_t, 32> record{};        // +2db0
    std::array<std::uint8_t, 32> thermal_class{}; // +2dd0
    // The class of the part that was pair `index` before the sort (index < part_count).
    std::uint8_t class_of(std::size_t index) const;
    // The position (Skm_Draw 003870c0's draw slot, 0..+23c0) of the part that was pair `index`.
    std::size_t position_of(std::size_t index) const;
};
struct ManSortPair { ManSortKey mesh{}, texture{}; };
ManSortOrder sort_man_parts(std::span<const ManSortPair> pairs, std::span<const ManSortRecord> records);

// The pairs 00390570 reads from a model's packing (model +40.. = packing +0; pair i at +40 + 20 i / +50 + 20 i),
// up to and including the first zero mesh key. A byte the sort compares (mesh 0..13, texture 1..4) or a mesh key
// whose zero test depends on an unwritten byte throws (unwritten state is unknown).
std::vector<ManSortPair> man_sort_pairs(const AppearancePacking& state);

// A 16-byte key from a name of at most 15 characters (the rest zero).
ManSortKey man_sort_key(std::string_view name);

// 0038dd40: the thermal texture names (00493410 + 0x10 k) requested through 00403a20 into the handles
// D_0053B360 + 2 k (LOD 0): class 0 black, 1 cool, 2 warm, 3 hot, 4 flame.
inline constexpr std::array<std::string_view, 5> man_thermal_texture_names{
    "_THER_BLA_----0", "_THER_COO_----0", "_THER_WAR_----0", "_THER_HOT_----0", "_THER_FLA_----0"};
}
