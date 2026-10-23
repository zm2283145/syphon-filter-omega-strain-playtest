#include "card_tools.hpp"

#include "agent_card.hpp"
#include "card_backup.hpp"
#include "ps2_card_image.hpp"

#include <algorithm>
#include <fstream>
#include <ostream>
#include <stdexcept>

namespace omega {
namespace fs = std::filesystem;
namespace {

std::string join(const std::vector<std::string>& names) {
    std::string s;
    for (const auto& n : names) s += (s.empty() ? "" : ", ") + n;
    return s.empty() ? "none" : s;
}

}  // namespace

int run_card_command(const CardCommand& c, std::ostream& out, std::string& summary) {
    const std::string dir(agent_card_directory);
    FolderAgentCard pc(c.card_root);
    // A restore keeps one more, so its own backup does not delete the backup being restored.
    CardBackups backups(pc.directory(),
                        c.kind == CardCommand::Kind::restore && c.backups > 0 ? c.backups + 1 : c.backups);
    const auto backup = [&] {
        const auto r = backups.before_change();
        if (!r.made && r.skipped == "the same save") return;  // the command's later writes
        out << "Memory card (port-only): backup of " << pc.directory().string() << ": "
            << (r.made ? r.made->string() : "none (" + r.skipped + ")");
        if (!r.pruned.empty()) out << ", " << r.pruned.size() << " old backup(s) deleted";
        out << "\n";
    };
    pc.set_before_change(backup);
    try {
        switch (c.kind) {
        case CardCommand::Kind::export_card: {
            const auto names = pc.list();
            if (names.empty()) throw std::runtime_error("the PC memory card " + pc.directory().string() + " holds no save files");
            const bool existed = fs::exists(c.file);
            Ps2CardImage card = existed ? Ps2CardImage::read(c.file) : Ps2CardImage::format(ps2_card_time_now());
            if (existed && !card.has_ecc())
                out << "Memory card (port-only): " << c.file.string() << " has no ECC bytes (512-byte pages); kept so\n";
            const auto bad_before = card.bad_ecc_pages();
            // 132win14: the PS2 browser icon files from the disc (0040d740's first two writes).
            std::vector<Ps2CardFile> icons;
            std::string icons_missing = c.icons_missing;
            if (c.icons) try {
                icons = c.icons();
            } catch (const std::exception& e) {
                icons_missing = e.what();
                out << "Memory card (port-only): the PS2 save icons could not be made: " << e.what() << "\n";
            }
            const auto r = export_ps2_card_directory(pc, dir, card, ps2_card_time_now(), icons);
            if (r.unchanged) {
                summary = c.file.string() + " already holds the PC card's " + std::to_string(names.size()) + " save file(s).";
                out << "Memory card (port-only): export: " << summary << " (not rewritten)\n";
                return 0;
            }
            if (existed) {
                auto bak = c.file;
                bak += ".bak";
                fs::copy_file(c.file, bak, fs::copy_options::overwrite_existing);
                out << "Memory card (port-only): the previous image copied to " << bak.string() << "\n";
            }
            card.write(c.file);
            // Read back: every PC file is in the image's directory, and no page lost its ECC.
            const auto back = Ps2CardImage::read(c.file);
            const auto files = back.directory_files(dir);
            if (!files) throw std::runtime_error("the written image has no " + dir);
            for (const auto& name : names) {
                std::vector<std::uint8_t> bytes(pc.size(name));
                pc.read(name, bytes);
                if (std::none_of(files->begin(), files->end(),
                                 [&](const Ps2CardFile& f) { return f.name == name && f.bytes == bytes; }))
                    throw std::runtime_error("the written image's " + name + " differs from the PC card's");
            }
            for (const auto& name : r.added_icons) {
                const auto made = std::find_if(icons.begin(), icons.end(), [&](const Ps2CardFile& f) { return f.name == name; });
                if (std::none_of(files->begin(), files->end(), [&](const Ps2CardFile& f) { return f.name == name && f.bytes == made->bytes; }))
                    throw std::runtime_error("the written image's " + name + " differs from the one made from the disc");
            }
            const auto bad_after = back.bad_ecc_pages();
            if (bad_after > bad_before) throw std::runtime_error("the written image has pages with a bad ECC");
            out << "Memory card (port-only): exported " << pc.directory().string() << " to " << c.file.string() << " ("
                << (existed ? "merged into the existing card" : "a newly formatted card") << "): " << r.written.size()
                << " file(s): " << join(r.written) << "; PS2 icon files kept from the card: " << join(r.kept_icons)
                << "; PS2 icon files added from the disc (0040d740): " << join(r.added_icons)
                << "; read back: " << files->size() << " file(s) in " << dir << ", " << back.free_clusters()
                << " free clusters, " << bad_after << " page(s) with a bad ECC\n";
            const auto has = [](const std::vector<std::string>& v) { return std::find(v.begin(), v.end(), "icon.sys") != v.end(); };
            if (!has(names) && !has(r.kept_icons) && !has(r.added_icons))
                out << "Memory card (port-only): note: the PC card has no PS2 browser icon files (icon.sys, MEMCARD.ICN; "
                       "the PS2 build's 0040d740 writes them) and they were not made from the disc ("
                    << icons_missing << "), so the PS2 system browser shows no icon for this save\n";
            summary = "Exported " + std::to_string(r.written.size()) + " save file(s) to " + c.file.string() + ".";
            return 0;
        }
        case CardCommand::Kind::import_card: {
            const auto card = Ps2CardImage::read(c.file);
            const auto r = import_ps2_card_directory(card, dir, pc, c.replace);
            if (!r.found) throw std::runtime_error(c.file.string() + " holds no " + dir + " directory (no Omega Strain saves)");
            out << "Memory card (port-only): imported " << dir << " from " << c.file.string() << " into "
                << pc.directory().string() << (c.replace ? " (replacing)" : " (new files only)") << ": "
                << r.copied.size() << " copied (" << join(r.copied) << "), " << r.kept.size() << " kept (" << join(r.kept)
                << ")\n";
            summary = "Imported " + std::to_string(r.copied.size()) + " save file(s) from " + c.file.string() + ".";
            return 0;
        }
        case CardCommand::Kind::restore: {
            fs::path from = c.file;
            if (fs::is_directory(from / dir)) from /= dir;
            const auto files = card_folder_files(from);
            if (files.empty()) throw std::runtime_error(c.file.string() + " is not a memory card backup (no save files)");
            if (card_folder_files(pc.directory()) == files) {
                summary = "The PC memory card already equals " + c.file.string() + ".";
                out << "Memory card (port-only): restore: " << summary << "\n";
                return 0;
            }
            backup();
            pc.create_save_directory();
            for (const auto& [name, bytes] : card_folder_files(pc.directory())) fs::remove(pc.directory() / name);
            for (const auto& [name, bytes] : files) {
                std::ofstream f(pc.directory() / name, std::ios::binary | std::ios::trunc);
                f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                if (!f) throw std::runtime_error("cannot write " + (pc.directory() / name).string());
            }
            out << "Memory card (port-only): restored " << files.size() << " file(s) from " << from.string() << " into "
                << pc.directory().string() << "\n";
            summary = "Restored " + std::to_string(files.size()) + " save file(s) from " + c.file.string() + ".";
            return 0;
        }
        }
    } catch (const std::exception& e) {
        summary = std::string("Memory card: ") + e.what();
        out << "Memory card (port-only): error: " << e.what() << "\n";
        return 1;
    }
    return 1;
}

}  // namespace omega
