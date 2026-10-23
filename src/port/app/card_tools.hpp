#pragma once
// Port-only (checkpoint 132win13, not the original): memory card management from the command line. omega_play and
// OmegaStrain.exe run one of these and exit (no disc image, no window):
//   --card-export <file.ps2>    the PC card's save directory (BASCUS-97264) into a PCSX2 card image: an existing
//                               image keeps its other directories (and the PS2 icon files when the PC card has none)
//                               and is first copied to <file>.bak; a missing one is formatted (8 MB, with ECC)
//   --card-import <file.ps2>    the image's BASCUS-97264 into the PC card: new files only, or with --card-replace the
//                               PC save directory made equal to the image's
//   --card-restore <backup>     the PC save directory made equal to a rolling backup (<card root>/backups/<stamp>)
// The PC card is --frontend-card's folder (default local/frontend_card). Imports and restores make a rolling
// backup of the PC card first (card_backup.hpp; the port setting card_backups=<n>).
// Checkpoint 132win14: an export also writes the PS2 browser icon files the PS2 build's 0040d740 writes (MEMCARD.ICN and
// icon.sys, ps2_save_icons.hpp), made from the user's disc through CardCommand::icons, when neither the PC card nor the
// image's directory has them (the OmegaStrain.exe launcher uses the iso= image, omega_play --iso).
#include "ps2_card_image.hpp"

#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>

namespace omega {

struct CardCommand {
    enum class Kind { export_card, import_card, restore };
    Kind kind = Kind::export_card;
    std::filesystem::path file;       // the .ps2 image or the backup folder
    std::filesystem::path card_root;  // the FolderAgentCard root
    bool replace = false;             // --card-replace (imports)
    unsigned backups = 10;            // card_backups=<n>
    // 132win14: the save icon files from the disc (ps2_save_icon_files); empty: none (no disc image). A throw is
    // reported and the export goes on without them.
    std::function<std::vector<Ps2CardFile>()> icons;
    std::string icons_missing = "no disc image was given";  // the reason shown when icons is empty
};

// Prints the report to out; summary is a short text for the launcher's dialog. Returns the exit code (0 done).
int run_card_command(const CardCommand& command, std::ostream& out, std::string& summary);

}  // namespace omega
