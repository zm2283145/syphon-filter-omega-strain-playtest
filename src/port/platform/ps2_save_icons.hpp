#pragma once
// Checkpoint 132win14 (port-only export of the original's PS2 save icons). The PS2 build's card_make_directory
// 0040d740 writes, after making BASCUS-97264, the files the PS2 system browser shows the save with:
//   MEMCARD.ICN  the disc file GAMEDATA\COMMON\ + *0x494008 ("MEMCARD.ICN", the string at 0x4bed00), read whole
//                (0036e140 / 0036de50 / 0036d990; whole sectors) and written as the card file named *0x494008 (0040e090);
//   icon.sys     the 0x3c4 bytes at 0x493c40, filled once by the card object's constructor 0040f190: strcpy of
//                *0x494008 to +0x144, +0x104 and +0x184 (the copy / list / delete icon names), strncpy(+0, "PS2D"
//                0x4beda8, 4), and 0040f290(+0xc0, the title 0x4bedb0, 0x20) (ps2_title_sjis);
//   then a file named *0x494010 ("BASCUS-97264") of 0x20 bytes from an uninitialised stack buffer (sp + 0xa0) and
//   the card id marker "#%d" (0x4bed58; 0x20 stack bytes, n from T1_COUNT and 00533870 / 00533878); the port writes
//   neither (no defined content; the card id is FRONTEND.md "The port's card id differs").
// The PC card (FolderAgentCard) never makes these files; card_tools' --card-export adds the first two to an exported
// image from the user's own disc at run time (nothing of the game's is in the source). Built by the functions
// below from the bytes of the boot executable and the disc, so they are tested on synthetic data.
#include "ps2_card_image.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace omega {

// The addresses 0040f190 / 0040d740 read (SCUS_972.64).
struct Ps2SaveIconAddresses {
    static constexpr std::uint32_t icon_sys = 0x493c40, icon_sys_size = 0x3c4;
    static constexpr std::uint32_t icon_name_pointer = 0x494008;  // -> "MEMCARD.ICN"
    static constexpr std::uint32_t magic = 0x4beda8;              // "PS2D" (strncpy 4 bytes, 00129a70)
    static constexpr std::uint32_t title = 0x4bedb0;              // the browser title (0040f290, at most 0x20 chars)
    static constexpr std::uint32_t icon_folder = 0x4bed20;        // "GAMEDATA\COMMON\"
    static constexpr std::uint32_t sjis_table = 0x494020;         // 0x21 entries {u16 code, char ascii, pad}
    static constexpr std::uint32_t sjis_entries = 0x21;
};

// 0040f290(dst, src, max): n = min(strlen(src), max) full-width Shift-JIS codes, stored high byte first, then a 0
// u16: a..z -> c + 0x8220, A..Z and 1..9 -> c + 0x821f ('0' is not in the digit range), other characters the
// table's code for that ASCII byte (the first match of 0x21 entries), else 0. `table`: 0x21 * 4 bytes.
std::vector<std::uint8_t> ps2_title_sjis(const std::string& title, std::span<const std::uint8_t> table, std::size_t max = 0x20);

struct Ps2SaveIconSources {
    std::vector<std::uint8_t> icon_sys;   // the 0x3c4 bytes at 0x493c40 as loaded
    std::vector<std::uint8_t> magic;      // 4 bytes at 0x4beda8
    std::string title;                    // the NUL-terminated string at 0x4bedb0
    std::vector<std::uint8_t> sjis_table; // 0x21 * 4 bytes at 0x494020
    std::string icon_name;                // *0x494008
    std::string icon_folder;              // 0x4bed20
};

// The icon.sys 0040f190 leaves at 0x493c40 (throws on a malformed source: wrong sizes, a name too long for its
// 64-byte field, or a result without the "PS2D" magic).
std::vector<std::uint8_t> ps2_icon_sys(const Ps2SaveIconSources& sources);

// The sources from the boot executable: read(address, length) gives its loaded bytes (throws past the image).
Ps2SaveIconSources ps2_save_icon_sources(const std::function<std::vector<std::uint8_t>(std::uint32_t, std::uint32_t)>& read);

// The two files 0040d740 writes first, in its order: {icon_name, the disc file icon_folder + icon_name} and
// {"icon.sys", ps2_icon_sys}. read_disc(path) gives a disc file by its backslash path, as the game's stream reads it:
// its whole 2048-byte sectors (0036de50's size; the PS2 game's own save in PCSX2's Mcd001.ps2 holds MEMCARD.ICN as
// 40960 bytes for the 39224-byte disc file). The icon file must start with the PS2 icon magic 00 00 01 00 (else this
// throws: not the expected disc).
std::vector<Ps2CardFile> ps2_save_icon_files(const Ps2SaveIconSources& sources,
                                             const std::function<std::vector<std::uint8_t>(const std::string&)>& read_disc);

}  // namespace omega
