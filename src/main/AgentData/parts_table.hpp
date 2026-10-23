#pragma once
// The parts table (research/FRONTEND.md §6): 003302a0 reads PARTS.PRN
// (GAMEDATA/COMMON/SCRIPTS.HOG member 3) at boot into the vector 00532b30 of
// rows {char name[16]; s32 value} (stride 0x14). The file is CR LF text in
// blocks separated by blank lines: a block's first line holds the column
// values (each token's offset and atoi), every later line names one part per
// column whose offset holds a non-space. The value is the rank threshold
// that makes the part available, or 64000 for the patches only
// awards::SetParts unlocks.
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace omega {

struct PartRow {
    std::string name;  // 1..15 bytes, never a space, CR or NUL
    std::int32_t value = 0;
};

// Appends the rows 003302a0 parses from `bytes` (an open that fails appends
// nothing; the caller skips the call). Throws where the EE would overrun its
// stack (a line of 0x200 bytes or more, a name of 16 or more) or where its
// atoi reads a byte >= 0x80 through newlib's signed-char ctype lookup.
void parse_parts_prn(std::span<const std::uint8_t> bytes, std::vector<PartRow>& rows);
std::vector<PartRow> parse_parts_prn(std::span<const std::uint8_t> bytes);

// 00121bc8 atoi(s) = (s32)strtol(s, 0, 10) over newlib's _strtol_r 0012ad58
// with a 64-bit long: leading isspace (\t \n \v \f \r and space) skipped, one
// sign, decimal digits, saturating at LONG_MIN / LONG_MAX; the low 32 bits
// are returned. `text` ends at its first NUL or at its end.
std::int32_t parts_atoi(std::span<const std::uint8_t> text);

}  // namespace omega
