#pragma once
// The level end's appearance patch 00405a60(app, list) (research/LEVEL_END.md
// "The parts tail"): the parts the agent cannot have yet (00332500's list)
// are struck from the MANTEX.HOG member names, then the PATC1 / PATC2 / PATC3
// slots' texture names step to the next member still available.
//
// app is the 0x440-byte appearance block *(agent + 9a4): slots of 0x20 bytes
// from +40 (mesh name at +0, texture name at +10). The slot scans use `lq`,
// which ignores the address's low 4 bits; the block is a 16-byte aligned heap
// block (0036b180), so the port reads it as is. A read past the block throws.
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

// 0036d200(archive, i) over every member of the loaded GAMEDATA/COMMON/
// MANTEX.HOG (*00571248): the i-th NUL-terminated name of the names block
// (header +0c up to the data at +10), in member order.
std::vector<std::string> hog_member_names(std::span<const std::uint8_t> hog);

// 00407ec0(dst, 15, src): src up to its first NUL or '.', at most 15 bytes,
// zero-filled to 15, dst[15] = 0.
std::array<std::uint8_t, 16> appearance_name_key(std::string_view src);

// 00405a60(app, list): names[i] = 00407ec0(15, members[i]); 00405e10 clears
// (to the zero quad D_005716e0) the first name equal to each list row name
// without a '.' (00129088); the PATCn slots are found by 00405a60's scan
// (below) and 00406040 (PATC1, PATC2) / 00405f10 (PATC3) step their texture
// name until it is one of the names (32 tries), storing it at slot +10.
// `locked` holds the list's row names in list order.
//
// The scan: from p = app + 40, idx 0, while the low 8 bytes at p are nonzero,
// strncmp(p + 1, "PATCn", 5) == 0 finds idx; else p += 0x20, idx + 1. At the
// zero slot, strncmp(p + 11, "PATCn", 5) == 0 finds that idx, else none.
//
// Not modelled: 00405a60 loads MANTEX.HOG (0036cef0(a, a, 1, 0)) when its
// +14c byte is clear and then unloads it (0036cd80); when it was loaded the
// unload depends on the caller's s7 (00335830 leaves its level-loop value).
// Either way only the archive's residency changes, not the names.
void patch_appearance_parts(std::span<std::uint8_t> app, std::span<const std::string> locked,
    std::span<const std::string> members);

}  // namespace omega
