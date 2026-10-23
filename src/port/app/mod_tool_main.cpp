// Port-only (checkpoint 132md1, research/MODDING.md): omega_mod_tool, the modder's starting point. Not game code.
//   omega_mod_tool export-textures [--iso <file>] --out <mod folder> [--match <text>] [--by-hash] <disc path>...
//       Every texture (TDX) in the disc files / archives named (nested HOG archives included) as PNG in the mod layout
//       (textures\<disc path>\<member>.TDX.png, animation frame n: .TDX.<n>.png; --by-hash: textures\by-hash\<hash>.png)
//       plus <mod folder>\textures_index.csv (key, frame, size, alpha flags, content hash).
//   omega_mod_tool export-files [--iso <file>] --out <mod folder> <disc path>[/<member>[/<nested member>]]...
//       The disc's bytes of files / archive members into files\<path> (same-format replacements start from these).
//   omega_mod_tool export-model [--iso <file>] --out <mod folder> [--skeleton MALE.SKEL|FEMALE.SKEL] <what>...
//       (checkpoint 132md2) Character models as glTF 2.0 rigged to the original skeleton (research/MODDING.md
//       "Stage 3"). <what> is a character (a .SKL appearance, e.g. GAMEDATA/UKRAINE/SKL.HOG/UKRAINE_GABE.SKL: its
//       parts resolved as the game does) or a part (GAMEDATA/COMMON/MANSKM.HOG/<name>.SKM[=<disc path of its .TDX>]).
//       A bare part / texture name is looked up as play's --part / --morph-part / --tex are (COMMON MORPH.HOG then
//       MANSKM.HOG; MANTEX.HOG then MORPHTEX.HOG), e.g. 2BOOT_COM_B01-0.SKM=_BOOT_COM_BLAC0.TDX. Each part goes to models\<key>.gltf (+ .bin, .png), the file a mod replaces; a character also gets
//       reference\<name>.gltf, every part on one skeleton (for viewing / editing the whole body; not loaded).
//       (132md3) A face / hand part of MORPH.HOG with a <stem>.SKF carries its shapes as glTF morph targets named by
//       the shape table's keys (mesh.extras.targetNames; research/MODDING.md "Face and hand shapes").
//       Level models ("Stage 3b"): <what> is a VUM (GAMEDATA/<M>/DATA.HOG/<X>.HOG/<X>.VUM, OBJECTS.HOG likewise), a
//       model container (<X>.HOG) or a whole DATA.HOG / OBJECTS.HOG (every model; --match <text> filters the keys):
//       models\<key>.gltf with the model's textures, the static mesh a mod replaces (its collision stays the disc's).
//       (132md3) Gun models: <what> is a gun archive (GAMEDATA/COMMON/WPNHOG.HOG, GAMEDATA/<M>/WPN.HOG) or one gun in
//       it (.../WPNHOG.HOG/<X>.HOG; --match filters the guns): each part's .SKM as models\<key>.gltf rigged to the
//       gun's 11 bones (research/MODDING.md "Gun models") and reference\<X>.gltf, the whole gun.
//   omega_mod_tool members [--iso <file>] <disc path>       The archive's members (nested archives indented).
//   omega_mod_tool list [--mods <folder>] [--settings <file>]  Every mod folder (enabled or not: name, version, author,
//       priority, overrides=, what it supplies) and what the enabled ones override, in resolution order. (132md4)
//       --settings: the port settings whose mod.<folder>=0|1 choices apply (default local\port_settings.txt).
//   omega_mod_tool conflicts [--mods <folder>] [--settings <file>] [--strict]   (checkpoint 132md4) Where the enabled
//       mods meet: entries shadowed by a higher-priority mod, entries layered over another mod's replaced archive,
//       entries ignored (outside a manifest's overrides=, forbidden, malformed). --strict: exit 1 on shadowed / ignored.
//   omega_mod_tool enable|disable [--mods <folder>] [--settings <file>] <mod folder name>...   (checkpoint 132md4)
//       Writes mod.<folder>=1|0 to the port settings (the in-game Port Settings page's MODS rows do the same).
//   omega_mod_tool move [--mods <folder>] [--settings <file>] <mod folder> up|down   (checkpoint 132md5) One place in
//       the resolution order: the manifests' priority= rewritten (the fewest; the launcher's mod panel does the same).
//   omega_mod_tool declare <mod folder>...   (checkpoint 132md4) Writes the manifest's overrides= from what the
//       folder holds (each entry's archive: its key up to the first .HOG component).
//   omega_mod_tool check-models [--iso <file>] [--mods <folder>] [--skeleton <SKEL>] [--match <text>]
//       (checkpoint 132md3) Loads every model the folder's enabled mods install as the play session would, against the
//       disc's original (research/MODDING.md "Checking models"): one line each, accepted (with what is kept) or refused
//       (why). Exit 1 when one is refused. An editor's round trip (tools/blender_round_trip.py) is checked with it.
//   omega_mod_tool export-sounds [--iso <file>] --out <mod folder> [--match <text>] <disc path>...   (checkpoint 132md4)
//       Every stream (VAG quips / speech, VPK music) in the archives named as 16-bit WAV in the mod layout
//       (sounds\<disc path>\<member>.wav, the file a mod replaces with a .wav or .ogg) plus sounds_index.csv.
//       (132md5) A 989snd sound bank (.BNK) exports each of its samples as sounds\<disc path>\<bank>.BNK\<n>.wav at
//       the rate the game plays it by default; the index names the sounds using each (research/MODDING.md "Sound
//       effects in the banks").
//   omega_mod_tool check-sounds [--iso <file>] [--mods <folder>] [--settings <file>] [--match <text>]   (132md4)
//       Decodes every sound the enabled mods install against the disc's original stream: one line each, accepted
//       (the conversion the game applies: rate, channels, the original's length) or refused (why). Exit 1 if refused.
//       (132md5) A bank sample (<bank>.BNK/<n>) is checked against the disc's bank: its rate, length and loop.
//   omega_mod_tool decode-audio --out <folder> <.wav | .ogg>...   (checkpoint 132md4) Decodes each file with the
//       port's own decoder to a 16-bit WAV at its own rate and channels (a check of a file before it goes in a mod).
//   omega_mod_tool report [--mods <folder>] [--settings <file>]   (checkpoint 132md4) The mod report the game sends
//       omega_server (research/MODDING.md "Online: the mod report"): the class (none / cosmetic / gameplay), each
//       gameplay entry, the fingerprint, and which server mod_policy values let it create / join a game.
//   omega_mod_tool validate [--iso <file>] [--mods <folder>] [--settings <file>] [--match <text>] [--skeleton <SKEL>]
//       [--strict] [<mod folder name>...]   (checkpoint 132md9) The author's check of the enabled mods (or only the
//       folders named, enabled whatever their manifests say) against the disc (research/MODDING.md "Validating a
//       mod"): invalid manifests, unused entries (ignored, shadowed, replacing nothing on the disc, overrides=
//       patterns matching nothing), unreadable images / models / sounds, image sizes (the original's shape, smaller
//       than the original, past a platform's texture side), the parts skinned on the CPU (morph targets, the bone
//       limit), and each level's texture memory on the PC and the Vita (its 64 MB cap; 128 MB CDRAM, 512 MB RAM).
//       Exit 1 on an error; --strict: also on a warning.
//   omega_mod_tool pack [--out <file.zip>] <mod folder>...   (checkpoint 132md10) Packs each mod folder into one .zip
//       (research/MODDING.md "Mod archives"; default <folder>.zip beside it, --out with one folder): the manifest must
//       parse; the members are the folder's files at the archive's root, deflated or stored (deterministic bytes); the
//       archive is read back member by member and indexed as a mod (the same entries as the folder). The game and the
//       tool's other commands read a .zip in the mods folder in place; a folder of the same name wins over it.
//       (checkpoint 132md11) --platform vita [--max-size <px>, default 1024]: every textures\ image is converted for
//       the Vita's GXM (research/MODDING.md "Mods on the Vita"; mods::platform_texture): power-of-two sides at most
//       --max-size, the full mip chain, BC1 (opaque) / BC3 blocks, stored as <name>.dds; the manifest gets
//       platform=vita and (unless it has one) a notes.vita= line describing the conversion. --platform pc packs as is.
//       (checkpoint 132md13) --platform vita also converts every sounds\ file and audio_overrides= file to a 16-bit
//       WAV at the rate and channels the game fits it to (the disc sound it replaces; several: the highest), at most
//       48000 Hz stereo (the Vita mixer's), so the Vita resamples and decodes no Vorbis at load; an .ogg becomes .wav
//       (the manifest's audio_overrides= rewritten). Without a disc image each file keeps its own rate (a warning).
//   omega_mod_tool volume [--mods <folder>] [--settings <file>] <mod folder> <percent>   (checkpoint 132md13) Writes
//       mod_volume.<folder>=<percent> (0..400) to the port settings: the player's volume for the mod's sounds, over its
//       manifest's audio_volume= (research/MODDING.md "Audio overrides").
//   (checkpoint 132md13) list, check-sounds and validate also cover the manifests' audio_overrides= targets (the disc
//       sounds each names, the format the game fits it to, the sounds sharing a replaced bank sample) and the volumes.
//   (checkpoint 132md14) list, conflicts and validate cover the manifests' movie_overrides= (research/MODDING.md "Movie
//       overrides"): validate checks each target is a ZMOVIES.HOG movie, decodes the picture whole (its size, aspect,
//       frame rate and length against the disc movie's), the sound (its length against the picture's) and the
//       subtitles; pack --platform vita notes the movies in notes.vita= (they are not converted).
// A new mod folder gets a manifest.txt with enabled=0. Without --iso the disc image is the iso= line of
// local\port_settings.txt (the launcher's choice), else an image found next to the tool. Everything read comes from
// the user's disc image and is written only where --out points (keep it out of the repository: disc-derived data is
// never committed).
#include "appearance.hpp"
#include "audio_files.hpp"
#include "audio_formats.hpp"
#include "bank_sounds.hpp"
#include "character_appearance.hpp"
#include "character_model.hpp"
#include "data_overrides.hpp"
#include "disc.hpp"
#include "gun_model_draw.hpp"
#include "gun_model_load.hpp"
#include "image_files.hpp"
#include "mod_archive.hpp"
#include "mod_fingerprint.hpp"
#include "mod_manager.hpp"
#include "mod_movie.hpp"
#include "mod_set.hpp"
#include "model_overrides.hpp"
#include "movie_pss.hpp"
#include "movie_subtitles.hpp"
#include "population.hpp"
#include "port_settings.hpp"
#include "skeleton.hpp"
#include "skinned_mesh.hpp"
#include "static_model.hpp"
#include "texture.hpp"
#include "texture_replacement.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace omega;
using Bytes = std::vector<std::uint8_t>;

struct Options {
    std::string command;
    fs::path iso, out, mods = "mods", settings = port_settings_default_file;
    std::string match, skeleton;
    bool by_hash = false, strict = false;
    std::string platform;           // pack --platform (132md11)
    std::uint32_t max_size = 1024;  // pack --platform vita --max-size
    std::vector<std::string> paths;
};

[[noreturn]] void usage(const std::string& problem) {
    throw std::runtime_error(problem + "\nusage: omega_mod_tool export-textures|export-files [--iso <file>] --out <mod folder> "
                                       "[--match <text>] [--by-hash] <disc path>...\n"
                                       "       omega_mod_tool export-model [--iso <file>] --out <mod folder> [--skeleton <SKEL>] "
                                       "<character .SKL | part .SKM[=<.TDX>] | level .VUM | .HOG | gun archive [/<X>.HOG]> [--match <text>]...\n"
                                       "       omega_mod_tool members [--iso <file>] <disc path>\n"
                                       "       omega_mod_tool list|conflicts [--mods <folder>] [--settings <file>] [--strict]\n"
                                       "       omega_mod_tool enable|disable [--mods <folder>] [--settings <file>] <mod folder name>...\n"
                                       "       omega_mod_tool volume [--mods <folder>] [--settings <file>] <mod folder> <percent>\n"
                                       "       omega_mod_tool move [--mods <folder>] [--settings <file>] <mod folder> up|down\n"
                                       "       omega_mod_tool declare <mod folder>...\n"
                                       "       omega_mod_tool check-models [--iso <file>] [--mods <folder>] [--skeleton <SKEL>] [--match <text>]\n"
                                       "       omega_mod_tool export-sounds [--iso <file>] --out <mod folder> [--match <text>] <disc path>...\n"
                                       "       omega_mod_tool check-sounds [--iso <file>] [--mods <folder>] [--settings <file>] [--match <text>]\n"
                                       "       omega_mod_tool decode-audio --out <folder> <.wav | .ogg>...\n"
                                       "       omega_mod_tool report [--mods <folder>] [--settings <file>]\n"
                                       "       omega_mod_tool validate [--iso <file>] [--mods <folder>] [--settings <file>] [--match <text>] "
                                       "[--skeleton <SKEL>] [--strict] [<mod folder name>...]\n"
                                       "       omega_mod_tool pack [--iso <file>] [--out <file.zip>] [--platform pc|vita] [--max-size <px>] <mod folder>...");
}

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    std::string part;
    for (const char c : path) {
        if (c == '/' || c == '\\') {
            if (!part.empty()) parts.push_back(mods::upper(part));
            part.clear();
        } else {
            part.push_back(c);
        }
    }
    if (!part.empty()) parts.push_back(mods::upper(part));
    if (parts.empty()) usage("empty disc path");
    return parts;
}

fs::path key_path(const std::string& key) {
    fs::path path;
    for (const auto& part : split_path(key)) path /= part;
    return path;
}

struct Disc {
    std::ifstream stream;
    std::optional<DiscImage> image;
    explicit Disc(fs::path iso) {
        if (iso.empty()) {
            // The launcher's choice (local\port_settings.txt iso=, a packaged build), else an image next to the game.
            if (fs::exists("local/port_settings.txt")) try {
                const auto bytes = mods::read_file("local/port_settings.txt");
                const auto s = parse_port_settings(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
                for (const auto& [k, v] : s.other)
                    if (k == "iso" && !v.empty()) iso = fs::path(std::u8string(v.begin(), v.end()));
            } catch (const std::exception&) {
            }
            if (iso.empty() || !fs::exists(iso)) {
                const auto found = movie::locate_game_iso(".");
                if (!found) throw std::runtime_error("No disc image found; give --iso <file>");
                iso = *found;
            }
        }
        stream.open(iso, std::ios::binary);
        if (!stream) throw std::runtime_error("Cannot open " + iso.string());
        image.emplace(stream, fs::file_size(iso));
        image->set_original_only(true);   // exports are the disc's own bytes, whatever is installed
    }
};

// A disc path, optionally continued by archive members: the disc file (the longest leading components naming a
// file) and its member chain.
struct Located {
    std::vector<std::string> file, members;
    Bytes bytes;   // the innermost item's bytes
    Bytes parent;  // the archive holding it (empty for a disc file)
    std::size_t offset = 0;
};

Located locate(DiscImage& disc, const std::string& path) {
    const auto parts = split_path(path);
    for (std::size_t n = 1; n <= parts.size(); ++n) {
        const std::vector<std::string> file(parts.begin(), parts.begin() + n);
        Extent extent;
        try {
            extent = disc.file(file);
        } catch (const std::exception&) {
            continue;
        }
        Located result{file, {parts.begin() + n, parts.end()}, {}, {}, 0};
        if (extent.size > (1ull << 31)) throw std::runtime_error(path + " is too large");
        result.bytes.resize(static_cast<std::size_t>(extent.size));
        for (std::uint64_t at = 0; at < extent.size; at += 8 << 20) {
            const auto chunk = disc.read({extent.offset + at, std::min<std::uint64_t>(8 << 20, extent.size - at)});
            std::copy(chunk.begin(), chunk.end(), result.bytes.begin() + static_cast<std::ptrdiff_t>(at));
        }
        for (const auto& member : result.members) {
            const auto members = mods::hog_members(result.bytes);
            const auto it = std::find_if(members.begin(), members.end(),
                                         [&](const mods::HogMember& m) { return mods::upper(m.name) == member; });
            if (it == members.end()) throw std::runtime_error(mods::asset_key(result.file) + " has no member " + member);
            result.parent = result.bytes;
            result.offset = it->offset;
            result.bytes = Bytes(result.parent.begin() + it->offset, result.parent.begin() + it->offset + it->size);
        }
        return result;
    }
    throw std::runtime_error("No disc file " + path);
}

bool is_tdx(const std::string& key) { return key.size() > 4 && key.compare(key.size() - 4, 4, ".TDX") == 0; }

void write_manifest(const fs::path& mod) {
    const auto path = mod / "manifest.txt";
    if (fs::exists(path)) return;
    fs::create_directories(mod);
    std::ofstream out(path, std::ios::binary);
    out << "# Mod manifest (research/MODDING.md). Exported by omega_mod_tool from your own disc image: everything in this\n"
           "# folder is the original game's data. Edit the files you want to change, delete the others (anything a mod\n"
           "# does not supply comes from the disc), then set enabled=1. Do not redistribute the game's own data.\n"
        << "name=" << mod.filename().string() << "\nversion=0.1\nauthor=\ndescription=\npriority=0\nenabled=0\n";
}

struct TextureExport {
    const Options& options;
    std::ofstream index;
    std::set<std::uint64_t> hashes;
    std::size_t written = 0, failed = 0, skipped = 0;

    void frame(const std::string& key, std::size_t n, const Texture& texture) {
        const auto hash = mods::texture_content_hash(texture);
        const auto frame_key = n == 0 ? key : key + "." + std::to_string(n);
        fs::path relative = options.by_hash ? fs::path("textures") / "by-hash" / (mods::hash_text(hash) + ".png")
                                            : fs::path("textures") / key_path(frame_key).concat(".png");
        index << frame_key << ',' << n << ',' << texture.width << ',' << texture.height << ',' << (texture.uses_alpha ? 1 : 0)
              << ',' << (texture.raw_alpha.empty() ? 0 : 1) << ',' << mods::hash_text(hash) << ','
              << relative.generic_string() << '\n';
        if (options.by_hash && !hashes.insert(hash).second) {
            ++skipped;
            return;
        }
        const auto path = options.out / relative;
        fs::create_directories(path.parent_path());
        mods::write_png(path, {texture.width, texture.height, texture.rgba});
        ++written;
    }

    void texture(const std::string& key, std::span<const std::uint8_t> container, std::size_t offset, std::size_t size) {
        if (!options.match.empty() && key.find(mods::upper(options.match)) == std::string::npos) return;
        Texture texture;
        try {
            try {
                texture = decode_tdx(container.subspan(offset, size));
            } catch (const std::exception&) {
                texture = decode_tdx(tdx_in_place(container, offset, size));
            }
        } catch (const std::exception& e) {
            std::cerr << "  " << key << ": not decodable (" << e.what() << "); skipped\n";
            ++failed;
            return;
        }
        frame(key, 0, texture);
        for (std::size_t n = 0; n < texture.animation.size(); ++n) frame(key, n + 1, texture.animation[n]);
    }

    void walk(const std::string& key, std::span<const std::uint8_t> bytes, int depth) {
        std::vector<mods::HogMember> members;
        try {
            members = mods::hog_members(bytes);
        } catch (const std::exception&) {
            return;   // not an archive: no textures inside
        }
        for (const auto& member : members) {
            const auto child = mods::member_key(key, member.name);
            if (is_tdx(child)) texture(child, bytes, member.offset, member.size);
            else if (depth < 4) walk(child, bytes.subspan(member.offset, member.size), depth + 1);
        }
    }
};

int export_textures(const Options& options) {
    Disc disc(options.iso);
    write_manifest(options.out);
    TextureExport e{options, std::ofstream(options.out / "textures_index.csv", std::ios::binary | std::ios::app), {}};
    if (fs::file_size(options.out / "textures_index.csv") == 0)
        e.index << "key,frame,width,height,uses_alpha,raw_alpha,content_hash,file\n";
    std::size_t missing = 0;
    for (const auto& path : options.paths) {
        Located located;
        try {
            located = locate(*disc.image, path);
        } catch (const std::exception& error) {
            std::cerr << "  " << error.what() << "; skipped\n";
            ++missing;
            continue;
        }
        auto key = mods::asset_key(located.file);
        for (const auto& member : located.members) key = mods::member_key(key, member);
        const auto before = e.written;
        if (is_tdx(key)) {
            if (located.parent.empty()) e.texture(key, located.bytes, 0, located.bytes.size());
            else e.texture(key, located.parent, located.offset, located.bytes.size());
        } else {
            e.walk(key, located.bytes, 0);
        }
        std::cout << key << ": " << (e.written - before) << " image(s)\n";
    }
    std::cout << "Exported " << e.written << " PNG(s) to " << fs::absolute(options.out).string() << " (" << e.failed
              << " undecodable, " << e.skipped << " duplicate hash(es), " << missing
              << " path(s) not on the disc); index textures_index.csv\n";
    return missing == options.paths.size() ? 1 : 0;
}

int export_files(const Options& options) {
    Disc disc(options.iso);
    write_manifest(options.out);
    for (const auto& path : options.paths) {
        const auto located = locate(*disc.image, path);
        auto key = mods::asset_key(located.file);
        for (const auto& member : located.members) key = mods::member_key(key, member);
        const auto target = options.out / "files" / key_path(key);
        if (fs::is_directory(target)) throw std::runtime_error(target.string() + " is a folder (its members are replaced)");
        fs::create_directories(target.parent_path());
        write_bytes(target, located.bytes);
        std::cout << key << ": " << located.bytes.size() << " bytes -> " << target.string() << "\n";
    }
    return 0;
}

// --- export-model (checkpoint 132md2) ---

struct ModelPart {
    std::string key;        // GAMEDATA/.../<ARCHIVE>/<MEMBER>.SKM, the models\ key a mod replaces
    SkinnedMesh mesh;
    std::optional<mods::RgbaImage> image;
    std::optional<MorphData> morph;               // 132md3: a MORPH.HOG face / hand part's shapes
    const ShapeNames* shape_names = nullptr;
};

// 132md3: MORPH.HOG's shape tables as play_main's MorphTables reads them (00404060): faces[0] FEMALE.BIN, faces[1]
// MALE.BIN; hands[g][h] "<g'>HAND<h'>.BIN" (g' '1' table 1, '2' table 0; h' '1' hand 1, '2' hand 0); the SKF members.
struct ShapeTables {
    std::array<ShapeNames, 2> faces;
    std::array<std::array<ShapeNames, 2>, 2> hands;
    std::set<std::string> skf;
};
const ShapeTables& shape_tables(DiscImage& disc) {
    static std::optional<ShapeTables> tables;
    if (tables) return *tables;
    const std::vector<std::string> hog{"GAMEDATA", "COMMON", "MORPH.HOG"};
    ShapeTables t;
    t.faces[0] = decode_shape_names(disc.archive_member(hog, "FEMALE.BIN"));
    t.faces[1] = decode_shape_names(disc.archive_member(hog, "MALE.BIN"));
    for (const auto& name : disc.archive_member_names(hog)) {
        if (name.ends_with(".SKF")) t.skf.insert(name);
        if (name.size() < 6 || name.compare(1, 4, "HAND") != 0 || !name.ends_with(".BIN")) continue;
        if ((name[0] != '1' && name[0] != '2') || (name[5] != '1' && name[5] != '2')) continue;
        t.hands[name[0] == '1' ? 1 : 0][name[5] == '1' ? 1 : 0] = decode_shape_names(disc.archive_member(hog, name));
    }
    tables = std::move(t);
    return *tables;
}
// The play session's bind_morph_part (00404060 / 00385d40): a MORPH.HOG part with a <stem>.SKF member is a hand part
// (byte 5 '1' hand 1, '2' hand 0) or a face part over shape table `table` (0 FEMALE, 1 MALE).
void bind_part_morph(DiscImage& disc, ModelPart& part, std::uint8_t table) {
    const std::string prefix = "GAMEDATA/COMMON/MORPH.HOG/";
    if (table > 1 || !part.key.starts_with(prefix)) return;
    const auto member = part.key.substr(prefix.size());
    if (member.size() < 6 || !member.ends_with(".SKM")) return;
    auto skf = member;
    skf.replace(skf.size() - 4, 4, ".SKF");
    const auto& tables = shape_tables(disc);
    if (!tables.skf.count(skf)) return;
    const bool hand = member[5] == '1' || member[5] == '2';
    part.shape_names = hand ? &tables.hands[table][member[5] == '1' ? 1 : 0] : &tables.faces[table];
    part.morph = decode_morph(disc.archive_member({"GAMEDATA", "COMMON", "MORPH.HOG"}, skf), hand ? MorphKind::hand : MorphKind::face,
                              static_cast<std::uint32_t>(part.shape_names->size()), static_cast<std::uint32_t>(part.mesh.chunks.size()));
}

std::optional<mods::RgbaImage> part_image(DiscImage& disc, const std::string& path) {
    try {
        const auto located = locate(disc, path);
        Texture texture;
        try {
            texture = decode_tdx(located.bytes);
        } catch (const std::exception&) {
            if (located.parent.empty()) throw;
            texture = decode_tdx(tdx_in_place(located.parent, located.offset, located.bytes.size()));
        }
        return mods::RgbaImage{texture.width, texture.height, texture.rgba};
    } catch (const std::exception& error) {
        std::cerr << "  " << path << ": " << error.what() << " (exported untextured)\n";
        return std::nullopt;
    }
}

std::string with_skm(std::string name) {
    if (name.size() < 4 || name.compare(name.size() - 4, 4, ".SKM") != 0) name += ".SKM";
    return name;
}

// A bare member name (no folder: 2BOOT_COM_B01-0.SKM, _BOOT_COM_BLAC0.TDX) as the play session's --part / --morph-part /
// --tex find it: the first of these COMMON archives holding it.
std::string qualify(DiscImage& disc, const std::string& name, std::initializer_list<const char*> archives) {
    if (name.find_first_of("/\\") != std::string::npos) return name;
    const auto member = mods::upper(name);
    for (const auto* archive : archives) {
        try {
            const auto names = disc.archive_member_names({"GAMEDATA", "COMMON", archive});
            if (std::find(names.begin(), names.end(), member) != names.end())
                return std::string("GAMEDATA/COMMON/") + archive + "/" + member;
        } catch (const std::exception&) {
        }
    }
    return name;
}

// A character's LOD 0 parts as the play session resolves them (play_main make_npc_figure: 00404750 / 00403a20): a
// preregistered mesh (COMMON MORPH.HOG, the mission's NPCSKM.HOG) or texture (the mission's NPCTEX.HOG) serves the
// request, else MANSKM.HOG / MANTEX.HOG.
std::vector<ModelPart> character_parts(DiscImage& disc, const std::vector<std::string>& skl, std::string& skeleton) {
    if (skl.size() != 4 || skl[2] != "SKL.HOG") usage("a character is GAMEDATA/<MISSION>/SKL.HOG/<NAME>.SKL");
    const std::vector<std::string> archive(skl.begin(), skl.begin() + 3), morph{"GAMEDATA", "COMMON", "MORPH.HOG"},
        npcskm{"GAMEDATA", skl[1], "NPCSKM.HOG"}, npctex{"GAMEDATA", skl[1], "NPCTEX.HOG"},
        manskm{"GAMEDATA", "COMMON", "MANSKM.HOG"}, mantex{"GAMEDATA", "COMMON", "MANTEX.HOG"};
    std::vector<AppearanceRegistryEntry> registry;
    for (const auto& name : disc.archive_member_names(archive))
        registry.push_back({appearance_registry_key(name), pack_appearance(disc.archive_member(archive, name))});
    const auto names = [&](const std::vector<std::string>& a) {
        try {
            return disc.archive_member_names(a);
        } catch (const std::exception&) {
            return std::vector<std::string>{};
        }
    };
    using Member = std::pair<std::vector<std::string>, std::string>;
    std::vector<Member> mesh_cache, texture_cache;
    for (const auto* a : {&morph, &npcskm})
        for (const auto& n : names(*a))
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".SKM") == 0) mesh_cache.push_back({*a, n});
    for (const auto& n : names(npctex)) texture_cache.push_back({npctex, n});
    const auto manskm_names = names(manskm), mantex_names = names(mantex);
    const auto has = [](const std::vector<std::string>& list, const std::string& n) {
        return std::find(list.begin(), list.end(), n) != list.end();
    };
    const auto stem = skl[3].substr(0, skl[3].find('.'));
    const auto look = resolve_character_appearance(stem, -1, registry);
    skeleton = look.skeleton;
    std::vector<ModelPart> parts;
    for (const auto& part : look.parts) {
        bool registered = false;
        const Member* mesh = nullptr;
        for (const auto& m : mesh_cache)
            if (const auto slot = appearance_preregistered_slot(part.mesh, m.second)) {
                registered = true;
                if (*slot == 0) mesh = &m;
            }
        Member source;
        if (mesh) source = *mesh;
        else if (!registered && !part.mesh.blank && has(manskm_names, part.mesh.members[0])) source = {manskm, part.mesh.members[0]};
        else continue;
        ModelPart out;
        out.key = mods::member_key(mods::asset_key(source.first), source.second);
        out.mesh = decode_skinned_mesh(disc.archive_member(source.first, source.second));
        const Member* texture = nullptr;
        for (const auto& m : texture_cache)
            if (const auto slot = appearance_preregistered_texture_slot(part.texture, m.second); slot && (!texture || *slot == 0)) texture = &m;
        if (texture) out.image = part_image(disc, mods::member_key(mods::asset_key(texture->first), texture->second));
        else if (!part.texture.blank && has(mantex_names, part.texture.members[0]))
            out.image = part_image(disc, mods::member_key(mods::asset_key(mantex), part.texture.members[0]));
        parts.push_back(std::move(out));
    }
    for (auto& part : parts) bind_part_morph(disc, part, appearance_shape_table(look.state));
    return parts;
}

// Level models (checkpoint 132md2, "Stage 3b"): a VUM member (GAMEDATA/<M>/DATA.HOG/<X>.HOG/<X>.VUM), a model
// container (<X>.HOG: its VUMs) or a level archive (DATA.HOG / OBJECTS.HOG: every model in it; --match filters the
// keys). Each goes to models\<key>.gltf (+ .bin, its textures as <X>.VUM.<slot>.png). Textures are found as the play
// session binds them (00377570, TextureBinder: "<name>.tdx", case-insensitive): an object's own container, then the
// mission's TEX.HOG; DATA.HOG models the reverse. 132md4: the HUD map's models (MAPVUM.HOG) bind from the mission's
// MAPTEX.HOG (0026e070), as DATA.HOG models.
std::optional<mods::RgbaImage> member_image(std::span<const std::uint8_t> container, const mods::HogMember& member) {
    Texture texture;
    try {
        texture = decode_tdx(container.subspan(member.offset, member.size));
    } catch (const std::exception&) {
        try {
            texture = decode_tdx(tdx_in_place(container, member.offset, member.size));
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return mods::RgbaImage{texture.width, texture.height, texture.rgba};
}

std::size_t export_level_models(DiscImage& disc, const std::string& path, const Options& options) {
    const auto located = locate(disc, path);
    auto key = mods::asset_key(located.file);
    for (const auto& member : located.members) key = mods::member_key(key, member);
    struct Container { Bytes bytes; std::vector<mods::HogMember> members; };
    std::optional<Container> tex_hog;
    const bool map = std::find(located.file.begin(), located.file.end(), "MAPVUM.HOG") != located.file.end();
    if (located.file.size() >= 2 && located.file[0] == "GAMEDATA") try {
        auto bytes = locate(disc, "GAMEDATA/" + located.file[1] + (map ? "/MAPTEX.HOG" : "/TEX.HOG")).bytes;
        auto members = mods::hog_members(bytes);
        tex_hog = Container{std::move(bytes), std::move(members)};
    } catch (const std::exception&) {
    }
    const bool objects = !map && std::find(located.file.begin(), located.file.end(), "OBJECTS.HOG") != located.file.end();
    std::size_t written = 0, skipped = 0;
    const auto one = [&](const std::string& vum_key, std::span<const std::uint8_t> vum_bytes, std::span<const std::uint8_t> owner) {
        if (!options.match.empty() && vum_key.find(mods::upper(options.match)) == std::string::npos) return;
        Mesh mesh;
        try {
            mesh = decode_vum(vum_bytes);
        } catch (const std::exception& error) {
            std::cerr << "  " << vum_key << ": not decodable (" << error.what() << "); skipped\n";
            ++skipped;
            return;
        }
        std::vector<mods::HogMember> owner_members;
        if (!owner.empty()) try {
            owner_members = mods::hog_members(owner);
        } catch (const std::exception&) {
        }
        std::vector<std::optional<mods::RgbaImage>> images(mesh.textures.size());
        std::vector<const mods::RgbaImage*> pointers(mesh.textures.size(), nullptr);
        std::size_t found = 0;
        for (std::size_t t = 0; t < mesh.textures.size(); ++t) {
            const auto wanted = mods::upper(mesh.textures[t] + ".tdx");
            const auto search = [&](std::span<const std::uint8_t> bytes, const std::vector<mods::HogMember>& members) {
                for (const auto& m : members)
                    if (mods::upper(m.name) == wanted) return member_image(bytes, m);
                return std::optional<mods::RgbaImage>{};
            };
            const auto own = [&] { return search(owner, owner_members); };
            const auto shared = [&] { return tex_hog ? search(tex_hog->bytes, tex_hog->members) : std::nullopt; };
            images[t] = objects ? own() : shared();
            if (!images[t]) images[t] = objects ? shared() : own();
            if (images[t]) {
                pointers[t] = &*images[t];
                ++found;
            }
        }
        const auto relative = fs::path("models") / key_path(vum_key);
        const auto stem = relative.filename().string();
        gfx::write_exported(options.out / relative.parent_path(), stem, gfx::export_static_model(mesh, pointers, stem));
        ++written;
        std::cout << vum_key << " (" << mesh.vertices.size() << " vertices, " << mesh.indices.size() / 3 << " triangles, "
                  << mesh.textures.size() << " textures, " << found << " found) -> " << (options.out / relative).string() << ".gltf\n";
        if (const auto blocker = gfx::static_model_blocker(mesh); !blocker.empty())
            std::cout << "  note: a replacement of this model is refused: " << blocker << "\n";
        else if (const auto remaps = gfx::static_model_remaps(mesh); !remaps.empty())
            std::cout << "  note: a replacement keeps the disc's " << remaps << "\n";
    };
    const auto is_vum = [](const std::string& name) { return mods::upper(name).ends_with(".VUM"); };
    if (is_vum(key)) {
        one(key, located.bytes, located.parent);
    } else {
        std::function<void(const std::string&, std::span<const std::uint8_t>, int)> walk =
            [&](const std::string& at, std::span<const std::uint8_t> bytes, int depth) {
                std::vector<mods::HogMember> members;
                try {
                    members = mods::hog_members(bytes);
                } catch (const std::exception&) {
                    return;
                }
                for (const auto& m : members) {
                    const auto child = mods::member_key(at, m.name);
                    const auto inner = bytes.subspan(m.offset, m.size);
                    if (is_vum(m.name)) one(child, inner, bytes);
                    else if (depth < 2 && mods::upper(m.name).ends_with(".HOG")) walk(child, inner, depth + 1);
                }
            };
        walk(key, located.bytes, 0);
    }
    std::cout << key << ": " << written << " level model(s)" << (skipped ? ", " + std::to_string(skipped) + " undecodable" : std::string())
              << "\n";
    return written;
}

// Gun models (132md3): a gun archive (WPNHOG.HOG / WPN.HOG: every gun HOG in it, --match filtering the names) or one
// gun HOG in it. The model is built as the play session's library builds it (0038d530 / 0038cd90), its textures
// found as the library finds them ("<name>.TDX" in the gun HOG); each part's LOD 0 / LOD 1 .SKM (once each) goes to
// models\<archive>\<X>.HOG\<member>.gltf rigged to the gun's 11 bones, the LOD 0 parts together to reference\<X>.gltf.
bool gun_archive(const std::vector<std::string>& parts) {
    return std::any_of(parts.begin(), parts.end(), [](const std::string& p) { return p == "WPNHOG.HOG" || p == "WPN.HOG"; });
}

struct ToolGunTextures final : GunTextureHost {
    std::span<const std::uint8_t> hog;
    std::vector<mods::HogMember> members;
    std::vector<std::optional<mods::RgbaImage>> images;
    void tinit() override {}
    void tload(const std::string&) override {}
    std::int16_t tex(const std::array<std::uint8_t, 16>& name, const std::array<bool, 16>& defined) override {
        std::string n;
        for (std::size_t k = 0; k < 16 && defined[k] && name[k] != 0; ++k) n.push_back(static_cast<char>(name[k]));
        const auto wanted = mods::upper(n + ".TDX");
        for (const auto& m : members)
            if (mods::upper(m.name) == wanted) {
                images.push_back(member_image(hog, m));
                return static_cast<std::int16_t>(images.size() - 1);
            }
        return -1;
    }
    std::int16_t texd() override { return -1; }
    void tfree() override {}
};

std::size_t export_gun_models(DiscImage& disc, const std::string& path, const Options& options) {
    const auto located = locate(disc, path);
    const auto archive_key = mods::asset_key(located.file);
    if (located.members.size() > 1) usage(path + ": a gun is <gun archive>/<X>.HOG");
    std::vector<std::pair<std::string, Bytes>> guns;
    if (located.members.empty()) {
        for (const auto& m : mods::hog_members(located.bytes))
            if (mods::upper(m.name).ends_with(".HOG") &&
                (options.match.empty() || mods::upper(m.name).find(mods::upper(options.match)) != std::string::npos))
                guns.emplace_back(mods::upper(m.name), Bytes(located.bytes.begin() + static_cast<std::ptrdiff_t>(m.offset),
                                                             located.bytes.begin() + static_cast<std::ptrdiff_t>(m.offset + m.size)));
    } else {
        guns.emplace_back(located.members.front(), located.bytes);
    }
    std::size_t written = 0;
    for (const auto& [member, bytes] : guns) {
        const auto stem = member.substr(0, member.find('.'));
        GunModel model;
        ToolGunTextures host;
        std::vector<gfx::ExportPart> all;
        std::vector<GunPartMesh> meshes;
        std::vector<std::string> names;
        SkeletonDefinition skeleton;
        try {
            host.hog = bytes;
            host.members = mods::hog_members(bytes);
            gun_model_construct(model, stem, -1, 0, 1, 0, 0);
            gun_model_load(model, GunHog{bytes, {}}, &host);
            skeleton = mods::gun_skeleton(model);
            const std::size_t parts = std::min<std::size_t>(model.bytes[0x40c], 8);
            meshes.reserve(2 * parts);
            for (std::int32_t lod = 0; lod < 2; ++lod)
                for (std::size_t p = 0; p < parts; ++p) {
                    const auto skm = mods::gun_part_member(model, bytes, p, lod);
                    if (skm.empty() || std::find(names.begin(), names.end(), skm) != names.end()) continue;
                    auto mesh = gun_part_mesh(model, bytes, p, lod);
                    if (!gun_part_skinnable(mesh.mesh)) {
                        std::cerr << "  " << stem << " part " << p << " LOD " << lod << " (" << skm
                                  << "): not drawn by the port's skinning; skipped\n";
                        continue;
                    }
                    names.push_back(skm);
                    meshes.push_back(std::move(mesh));
                    const auto& m = meshes.back();
                    const auto key = mods::gun_part_key(archive_key, mods::gun_model_name(model), skm);
                    const auto relative = fs::path("models") / key_path(key);
                    const auto part_stem = relative.filename().string();
                    const mods::RgbaImage* image = nullptr;
                    if (m.texture >= 0 && static_cast<std::size_t>(m.texture) < host.images.size() && host.images[static_cast<std::size_t>(m.texture)])
                        image = &*host.images[static_cast<std::size_t>(m.texture)];
                    gfx::ExportPart one{part_stem, &m.mesh, image};
                    one.bone_local = true;
                    gfx::write_exported(options.out / relative.parent_path(), part_stem, gfx::export_model(skeleton, std::span(&one, 1), part_stem));
                    if (lod == 0) all.push_back(one);
                    ++written;
                    std::cout << key << " (part " << p << ", LOD " << lod << ", " << m.mesh.chunks.size() << " chunks"
                              << (image ? ", textured" : "") << (gun_part_flare_program(m.mesh) ? ", flare" : "") << ") -> "
                              << (options.out / relative).string() << ".gltf\n";
                }
        } catch (const std::exception& error) {
            std::cerr << "  " << archive_key << "/" << member << ": " << error.what() << "; skipped\n";
            continue;
        }
        if (!all.empty()) {
            gfx::write_exported(options.out / "reference", stem, gfx::export_model(skeleton, all, stem));
            std::cout << archive_key << "/" << member << ": whole gun -> " << (options.out / "reference" / stem).string() << ".gltf\n";
        }
    }
    std::cout << archive_key << ": " << guns.size() << " gun(s), " << written << " part model(s)\n";
    return written;
}

int export_model(const Options& options) {
    Disc disc(options.iso);
    write_manifest(options.out);
    std::size_t written = 0;
    for (const auto& what : options.paths) {
        const auto equals = what.find('=');
        const auto raw = what.substr(0, equals);
        const auto upper_raw = mods::upper(raw);
        if (gun_archive(split_path(raw))) {
            written += export_gun_models(*disc.image, raw, options);
            continue;
        }
        if (upper_raw.ends_with(".VUM") || upper_raw.ends_with(".HOG")) {
            written += export_level_models(*disc.image, raw, options);
            continue;
        }
        const bool character = upper_raw.ends_with(".SKL");
        const auto mesh_path = character ? raw : qualify(*disc.image, with_skm(raw), {"MORPH.HOG", "MANSKM.HOG"});
        const auto path = split_path(mesh_path);
        std::vector<ModelPart> parts;
        std::string skeleton_name = options.skeleton;
        if (character) {
            std::string resolved;
            parts = character_parts(*disc.image, path, resolved);
            if (skeleton_name.empty()) skeleton_name = resolved;
        } else {
            const auto located = locate(*disc.image, mesh_path);
            ModelPart part;
            part.key = mods::asset_key(located.file);
            for (const auto& member : located.members) part.key = mods::member_key(part.key, member);
            part.key = with_skm(part.key);
            part.mesh = decode_skinned_mesh(located.bytes);
            if (equals != std::string::npos)
                part.image = part_image(*disc.image, qualify(*disc.image, what.substr(equals + 1), {"MANTEX.HOG", "MORPHTEX.HOG"}));
            // A part named 2... is a female agent's (the play session's 2*.SKM parts on FEMALE.SKEL).
            if (skeleton_name.empty()) skeleton_name = path.back().starts_with("2") ? "FEMALE.SKEL" : "MALE.SKEL";
            // Its shape table as the play session's --part list picks one (a '1' name MALE.BIN, '2' FEMALE.BIN), else
            // the skeleton's.
            const auto& member = path.back();
            const std::uint8_t table = member.starts_with("1") ? 1 : member.starts_with("2") ? 0 : mods::upper(skeleton_name) == "FEMALE.SKEL" ? 0 : 1;
            bind_part_morph(*disc.image, part, table);
            parts.push_back(std::move(part));
        }
        if (skeleton_name.empty()) skeleton_name = "MALE.SKEL";
        skeleton_name = mods::upper(skeleton_name);
        const auto skeleton = decode_skeleton(disc.image->archive_member({"GAMEDATA", "COMMON", "SKEL.HOG"}, skeleton_name));
        std::vector<gfx::ExportPart> all;
        for (const auto& part : parts) {
            const auto relative = fs::path("models") / key_path(part.key);
            const auto stem = relative.filename().string();
            gfx::ExportPart one{stem, &part.mesh, part.image ? &*part.image : nullptr};
            if (part.morph) {
                one.morph = &*part.morph;
                one.shape_names = part.shape_names;
            }
            gfx::write_exported(options.out / relative.parent_path(), stem, gfx::export_model(skeleton, std::span(&one, 1), stem));
            all.push_back(one);
            ++written;
            std::cout << part.key << " (" << skeleton_name << (part.image ? ", textured" : "")
                      << (part.morph ? std::string(", ") + std::to_string(part.morph->shapes.size()) +
                                           (part.morph->kind == MorphKind::face ? " face" : " hand") + " shapes"
                                     : std::string())
                      << ") -> " << (options.out / relative).string() << ".gltf\n";
        }
        if (character) {
            const auto stem = path.back().substr(0, path.back().find('.'));
            gfx::write_exported(options.out / "reference", stem, gfx::export_model(skeleton, all, stem));
            std::cout << what << ": " << parts.size() << " part(s) on " << skeleton_name << "; whole body -> "
                      << (options.out / "reference" / stem).string() << ".gltf\n";
        }
    }
    std::cout << "Exported " << written << " model(s) to " << fs::absolute(options.out).string()
              << " (models\\ replaces; reference\\ is for viewing)\n";
    return written ? 0 : 1;
}

// --- check-models (checkpoint 132md3) ---
// Every model the folder's enabled mods install, loaded as the play session loads it (model_overrides.cpp) against
// the disc's original:
//   - a level model (.VUM key) against the disc's VUM (gfx::load_static_replacement: colour patches remapped, animated
//     materials laid out);
//   - a gun part (a WPNHOG.HOG / WPN.HOG key) against its gun's 11 bones (mods::gun_skeleton), naming the parts and
//     LODs that draw it;
//   - a character part against its skeleton (--skeleton, else FEMALE.SKEL for a 2* member, MALE.SKEL otherwise, then
//     the other one; the play session checks it against the skeleton of whoever draws it), its morph targets bound to
//     a face / hand part's shape table (gfx::bind_morphs, the table chosen as export-model chooses it).
std::string count_text(std::size_t n, const char* what) { return std::to_string(n) + " " + what; }

std::string skinned_summary(const gfx::ReplacementModel& model, const std::string& skeleton) {
    std::string what = skeleton + ", " + count_text(model.vertices.size(), "vertices") + ", " +
                       count_text(model.triangles.size(), "triangles");
    if (model.image) what += ", image " + std::to_string(model.image->width) + "x" + std::to_string(model.image->height);
    if (model.vertices.empty()) what += ", hidden";
    return what;
}

// One model entry checked against the disc's original (check-models, validate). Throws why it is refused.
struct ModelCheck {
    std::string what;
    std::vector<std::string> notes;
    std::string skinning;            // empty: a static or hidden model; else "GPU" or "CPU (<why>)" (132md9)
    std::size_t images = 0;          // the replacement's own images (not scaled by the texture memory cap)
    std::uint64_t image_bytes = 0;   // their RGBA8 size with mips (mods::replacement_bytes)
};

// Checkpoint 132md9: where a skinned replacement is drawn when the display device skins (gfx::Capabilities):
// the play session's rule (gfx::gpu_skinnable against the drawing skeleton's palette).
std::string skinning_path(const gfx::ReplacementModel& model, std::size_t palette) {
    if (model.triangles.empty()) return {};
    if (gfx::gpu_skinnable(model, palette)) return "GPU";
    if (!model.morphs.empty()) return "CPU (morph targets)";
    return "CPU (" + std::to_string(gfx::replacement_bones_used(model)) + " bones used of a " + std::to_string(palette) +
           "-bone palette; the GPU limit is " + std::to_string(gfx::gpu_palette_limit) + ")";
}

void count_image(ModelCheck& check, const mods::RgbaImage& image) {
    ++check.images;
    check.image_bytes += mods::replacement_bytes(image.width, image.height);
}

ModelCheck check_model(DiscImage& disc, const mods::ModSet::Entry& entry, std::map<std::string, SkeletonDefinition>& skeletons,
                       const std::string& skeleton_option) {
    const auto& key = entry.key;
    const auto parts = split_path(key);
    ModelCheck check;
    auto& what = check.what;
    if (key.ends_with(".VUM")) {
        const auto original = decode_vum(locate(disc, key).bytes);
        const auto loaded = gfx::load_static_replacement(entry.source.path, original);
        for (const auto& image : loaded.images)
            if (image) count_image(check, *image);
        what = "static, " + count_text(loaded.mesh.vertices.size(), "vertices") + ", " +
               count_text(loaded.mesh.indices.size() / 3, "triangles") + ", " +
               count_text(loaded.mesh.materials.size(), "materials");
        if (check.images) what += ", " + count_text(check.images, "images");
        if (loaded.mesh.indices.empty()) what += ", hidden";
        if (!loaded.mesh.color_patches.empty())
            what += ", " + count_text(loaded.mesh.color_patches.size(), "colour patch sets on ") +
                    count_text(loaded.patched_vertices, "vertices");
        if (loaded.animated_surfaces) what += ", " + count_text(loaded.animated_surfaces, "animated surfaces kept");
        if (loaded.overshoot > 0) {
            char text[96];
            std::snprintf(text, sizeof text, ", extends %.4g outside the declared bounds", static_cast<double>(loaded.overshoot));
            what += text;
        }
        check.notes = loaded.notes;
    } else if (gun_archive(parts)) {
        if (parts.size() < 3 || !parts[parts.size() - 2].ends_with(".HOG")) throw std::runtime_error("not a gun part key (<gun archive>/<X>.HOG/<member>)");
        const auto gun = locate(disc, key.substr(0, key.rfind('/')));
        const auto& member = parts.back();
        const auto stem = parts[parts.size() - 2].substr(0, parts[parts.size() - 2].find('.'));
        GunModel model;
        ToolGunTextures host;
        host.hog = gun.bytes;
        host.members = mods::hog_members(gun.bytes);
        gun_model_construct(model, stem, -1, 0, 1, 0, 0);
        gun_model_load(model, GunHog{gun.bytes, {}}, &host);
        std::string slots;
        const std::size_t count = std::min<std::size_t>(model.bytes[0x40c], 8);
        for (std::int32_t lod = 0; lod < 2; ++lod)
            for (std::size_t p = 0; p < count; ++p)
                if (mods::gun_part_member(model, gun.bytes, p, lod) == member)
                    slots += (slots.empty() ? "" : ", ") + std::string("part ") + std::to_string(p) + " LOD " + std::to_string(lod);
        if (slots.empty()) throw std::runtime_error("the gun " + stem + " draws no part from " + member);
        const auto skeleton = mods::gun_skeleton(model);
        const auto loaded = gfx::load_replacement(entry.source.path, skeleton);
        if (loaded.image) count_image(check, *loaded.image);
        check.skinning = skinning_path(loaded, skeleton.bones.size());
        what = skinned_summary(loaded, mods::gun_model_name(model) + ".HOG's bones") + "; " + slots;
    } else {
        const auto located = locate(disc, key);
        // Without --skeleton: the name's skeleton (2*: FEMALE.SKEL), else the other one (the part may be drawn by
        // either); refused with the first's error when neither matches.
        std::vector<std::string> candidates;
        if (!skeleton_option.empty()) candidates = {mods::upper(skeleton_option)};
        else if (parts.back().starts_with("2")) candidates = {"FEMALE.SKEL", "MALE.SKEL"};
        else candidates = {"MALE.SKEL", "FEMALE.SKEL"};
        std::optional<gfx::ReplacementModel> model;
        std::string skeleton_name, first_error;
        std::size_t palette = 0;
        for (const auto& candidate : candidates) {
            auto found = skeletons.find(candidate);
            if (found == skeletons.end())
                found = skeletons.emplace(candidate, decode_skeleton(disc.archive_member({"GAMEDATA", "COMMON", "SKEL.HOG"}, candidate))).first;
            try {
                model = gfx::load_replacement(entry.source.path, found->second);
                skeleton_name = candidate;
                palette = found->second.bones.size();
                break;
            } catch (const std::exception& error) {
                if (first_error.empty()) first_error = error.what();
            }
        }
        if (!model) throw std::runtime_error(first_error + " (" + candidates.front() + ")");
        const auto& loaded = *model;
        if (loaded.image) count_image(check, *loaded.image);
        check.skinning = skinning_path(loaded, palette);
        what = skinned_summary(loaded, skeleton_name);
        ModelPart part;
        part.key = key;
        part.mesh = decode_skinned_mesh(located.bytes);
        const auto& m = parts.back();
        const std::uint8_t table = m.starts_with("1") ? 1 : m.starts_with("2") ? 0 : skeleton_name == "FEMALE.SKEL" ? 0 : 1;
        bind_part_morph(disc, part, table);
        if (!loaded.morphs.empty() && !part.shape_names) {
            what += ", " + count_text(loaded.morphs.size(), "morph targets not drawn (the part has no face / hand shapes)");
        } else if (!loaded.morphs.empty()) {
            const auto shapes = gfx::bind_morphs(loaded, *part.shape_names);
            const auto bound = static_cast<std::size_t>(std::count_if(shapes.begin(), shapes.end(), [](std::int16_t s) { return s >= 0; }));
            what += ", " + count_text(loaded.morphs.size(), "morph targets on ") + std::to_string(bound) + " of " +
                    count_text(part.shape_names->size(), "shapes");
        } else if (part.shape_names && !loaded.vertices.empty()) {
            what += "; no morph targets: the part's " + count_text(part.shape_names->size(), "shapes do not move it");
        }
    }
    return check;
}

int check_models(const Options& options) {
    std::vector<std::string> messages;
    const auto set = mods::ModSet::scan(options.mods, messages);
    for (const auto& m : messages) std::cout << m << "\n";
    Disc disc(options.iso);
    std::map<std::string, SkeletonDefinition> skeletons;
    std::size_t accepted = 0, refused = 0;
    for (const auto& entry : set->entries()) {
        if (entry.kind != "models") continue;
        const auto& key = entry.key;
        if (!options.match.empty() && key.find(mods::upper(options.match)) == std::string::npos) continue;
        const auto& folder = entry.source.mod->folder;
        try {
            const auto check = check_model(*disc.image, entry, skeletons, options.skeleton);
            std::cout << "models/" << key << " <- " << folder << " (" << check.what << ")\n";
            for (const auto& note : check.notes) std::cout << "  note: " << note << "\n";
            ++accepted;
        } catch (const std::exception& error) {
            std::cout << "models/" << key << " from " << folder << " REFUSED (" << error.what() << "); the disc's model would be drawn\n";
            ++refused;
        }
    }
    std::cout << count_text(accepted + refused, "model(s): ") << accepted << " accepted, " << refused << " refused\n";
    return refused ? 1 : 0;
}

int list_members(const Options& options) {
    Disc disc(options.iso);
    for (const auto& path : options.paths) {
        const auto located = locate(*disc.image, path);
        auto key = mods::asset_key(located.file);
        for (const auto& member : located.members) key = mods::member_key(key, member);
        std::function<void(std::span<const std::uint8_t>, int)> walk = [&](std::span<const std::uint8_t> bytes, int depth) {
            std::vector<mods::HogMember> members;
            try {
                members = mods::hog_members(bytes);
            } catch (const std::exception&) {
                return;
            }
            for (const auto& member : members) {
                std::cout << std::string(std::size_t(depth) * 2 + 2, ' ') << member.name << "  " << member.size << "\n";
                if (depth < 4 && !is_tdx(mods::upper(member.name))) walk(bytes.subspan(member.offset, member.size), depth + 1);
            }
        };
        std::cout << key << " (" << located.bytes.size() << " bytes)\n";
        walk(located.bytes, 0);
    }
    return 0;
}

// Checkpoint 132md4: the player's mod.<folder>=0|1 choices from the port settings (--settings, default
// local\port_settings.txt; a missing file: none).
mods::ModChoices settings_choices(const Options& options) {
    mods::ModChoices choices;
    if (!fs::exists(options.settings)) return choices;
    for (const auto& [folder, on] : mod_choices_setting(read_port_settings(options.settings))) choices[folder] = on;
    return choices;
}

// Checkpoint 132md13: the player's mod_volume.<folder>= volumes from the same settings.
mods::ModVolumes settings_volumes(const Options& options) {
    mods::ModVolumes volumes;
    if (!fs::exists(options.settings)) return volumes;
    for (const auto& [folder, percent] : mod_volumes_setting(read_port_settings(options.settings))) volumes[folder] = percent;
    return volumes;
}

std::string join(const std::vector<std::string>& items, const char* separator) {
    std::string text;
    for (const auto& item : items) text += (text.empty() ? "" : separator) + item;
    return text;
}

int list_mods(const Options& options) {
    // Checkpoint 132md4: every mod folder (enabled or not) with its manifest, then the enabled set's resolution.
    const auto choices = settings_choices(options);
    const auto listing = mods::list_mods(options.mods, choices);
    std::cout << "Mods folder " << fs::absolute(options.mods).string() << ": " << listing.size() << " mod(s)";
    if (!choices.empty()) std::cout << " (" << choices.size() << " choice(s) from " << options.settings.string() << ")";
    std::cout << "\n";
    std::vector<std::string> messages;
    const auto set = mods::ModSet::scan(options.mods, messages, choices);
    const auto volumes = settings_volumes(options);   // 132md13
    set->set_volumes(volumes);
    std::map<std::string, std::map<std::string, std::size_t>> counts;   // folder -> kind -> entries used
    for (const auto& entry : set->entries()) ++counts[entry.source.mod->folder][entry.kind];
    for (const auto& m : listing) {
        if (!m.manifest) {
            std::cout << "  " << m.folder << ": INVALID (" << m.problem << ")\n";
            continue;
        }
        const auto& f = *m.manifest;
        std::cout << "  " << m.folder << (m.archive.empty() ? std::string() : " [" + m.archive.filename().string() + "]") << ": \"" << f.name << "\"" << (f.version.empty() ? "" : " " + f.version)
                  << (f.author.empty() ? "" : " by " + f.author) << ", priority " << f.priority << ", "
                  << (m.enabled ? "enabled" : "disabled")
                  << (m.chosen ? std::string(" (mod.") + m.folder + "=" + (m.enabled ? "1" : "0") + " in the settings)" : "")
                  << "\n";
        if (!f.description.empty()) std::cout << "      " << f.description << "\n";
        if (!f.game_version.empty())
            std::cout << "      game_version: " << f.game_version << (mods::game_version_matches(f.game_version) ? "" : " (NOT this game)") << "\n";
        if (!f.platform.empty()) std::cout << "      platform: " << f.platform << "\n";
        for (const auto& [platform, note] : f.notes) std::cout << "      notes." << platform << ": " << note << "\n";
        std::cout << "      overrides: " << (f.overrides.empty() ? std::string("(undeclared: everything in the folder)") : join(f.overrides, ", "))
                  << "\n";
        if (!f.audio_overrides.empty()) {   // 132md13
            std::vector<std::string> items;
            for (const auto& o : f.audio_overrides) items.push_back(o.target + "=" + o.file);
            std::cout << "      audio_overrides: " << join(items, ", ") << "\n";
        }
        if (const auto chosen = volumes.find(mods::upper(m.folder)); chosen != volumes.end())
            std::cout << "      audio volume: " << chosen->second << "% (mod_volume." << m.folder << "= in the settings; the manifest's "
                      << f.audio_volume << "%)\n";
        else if (f.audio_volume != 100)
            std::cout << "      audio volume: " << f.audio_volume << "% (audio_volume=)\n";
        if (m.enabled) {
            std::string used;
            for (const auto& [kind, n] : counts[m.folder]) used += (used.empty() ? "" : ", ") + std::to_string(n) + " " + kind;
            std::cout << "      used: " << (used.empty() ? std::string("nothing") : used) << "\n";
        }
    }
    for (const auto& m : messages) std::cout << m << "\n";
    std::cout << "Resolution order (first wins, then the disc):";
    for (const auto& mod : set->mods()) std::cout << " " << mod.folder << " (" << mod.manifest.priority << ")";
    std::cout << "\n";
    for (const auto& entry : set->entries()) {
        std::cout << entry.kind << " " << entry.key << " <- " << entry.source.mod->folder;
        for (const auto& s : entry.shadowed) std::cout << " (shadows " << s << ")";
        std::cout << "\n";
    }
    for (const auto& [screen, wide] : set->wide_backgrounds()) {   // 132md12
        std::cout << "wide." << screen << " <- " << wide.mod->folder << ": " << wide.images.size() << " image(s)";
        for (const auto& s : wide.shadowed) std::cout << " (shadows " << s << ")";
        std::cout << "\n";
    }
    return 0;
}

// Checkpoint 132md4: where the enabled mods meet (ModSet::conflicts) and what each ignores. Exit 0; with --strict,
// 1 when a mod's entry is shadowed or ignored.
int list_conflicts(const Options& options) {
    std::vector<std::string> messages;
    const auto set = mods::ModSet::scan(options.mods, messages, settings_choices(options));
    const auto summary = mods::summarize_conflicts(*set, messages);   // 132md5: shared with the launcher's mod panel
    for (const auto& line : summary.lines) std::cout << line << "\n";
    return options.strict && (summary.shadowed || summary.ignored) ? 1 : 0;
}

// Checkpoint 132md4 (ONLINE.md §6.9): the mod report the game would send omega_server. Exit 0.
int mod_report(const Options& options) {
    std::vector<std::string> messages;
    const auto set = mods::ModSet::scan(options.mods, messages, settings_choices(options));
    for (const auto& e : set->entries())
        if (mods::entry_class(e.kind, e.key) == online::ModClass::Gameplay)
            std::cout << "gameplay: " << e.kind << "/" << e.key << " (" << e.source.mod->folder << ")\n";
    const auto report = mods::online_report(set.get());
    std::cout << "class: " << online::mod_class_name(report.mod_class) << "\n"
              << "mods: " << report.mods << (report.summary.empty() ? std::string() : " (" + report.summary + ")") << "\n"
              << "entries: " << report.cosmetic_entries << " cosmetic, " << report.gameplay_entries << " gameplay\n"
              << "fingerprint: " << (report.mod_fingerprint.empty() ? std::string("(none)") : report.mod_fingerprint) << "\n"
              << "gameplay fingerprint: " << (report.gameplay_fingerprint.empty() ? std::string("(none)") : report.gameplay_fingerprint)
              << "\n";
    for (const auto policy : {online::ModPolicy::Any, online::ModPolicy::Cosmetic, online::ModPolicy::None}) {
        const auto why = online::mod_refusal(policy, &report, false);
        std::cout << "mod_policy=" << online::mod_policy_name(policy) << ": " << (why ? "refused (" + *why + ")" : std::string("allowed"))
                  << "\n";
    }
    // 132md16: the default policy (online mod_policy=, LAN lan_mod_policy=): the game's creator / host decides.
    std::cout << "mod_policy=gameplay / lan_mod_policy=gameplay: "
              << (report.gameplay_fingerprint.empty() ? std::string("joins games whose host has no gameplay mods")
                                                      : "joins games whose host's gameplay fingerprint is " +
                                                            report.gameplay_fingerprint.substr(0, 16) + "...")
              << " (cosmetic mods may differ)\n";
    return 0;
}

// Checkpoint 132md4: enable / disable <folder>... writes mod.<folder>=1|0 to the port settings (--settings).
int choose_mods(const Options& options, bool on) {
    const auto listing = mods::list_mods(options.mods);
    PortSettings s = read_port_settings(options.settings);
    for (const auto& name : options.paths) {
        const auto it = std::find_if(listing.begin(), listing.end(),
                                     [&](const mods::ModListing& m) { return mods::upper(m.folder) == mods::upper(name); });
        if (it == listing.end()) throw std::runtime_error("no mod folder " + name + " in " + options.mods.string());
        if (!it->manifest) throw std::runtime_error(it->folder + " has no valid manifest (" + it->problem + ")");
        const std::string key = "mod." + it->folder;
        mods::set_mod_choice(s, it->folder, on);   // 132md5: shared with the launcher's mod panel
        std::cout << key << "=" << (on ? 1 : 0) << " (\"" << it->manifest->name << "\"; its manifest says enabled="
                  << (it->manifest->enabled ? 1 : 0) << ")\n";
    }
    write_port_settings(options.settings, s);
    std::cout << "written to " << options.settings.string() << " (applies at the next level of a running game, else at its start)\n";
    return 0;
}

// Checkpoint 132md13: volume <folder> <percent> writes mod_volume.<folder>=<percent> to the port settings (--settings).
int set_volume(const Options& options) {
    if (options.paths.size() != 2) usage("volume needs <mod folder> <percent>");
    const auto& text = options.paths[1];
    if (text.empty() || text.size() > 3 || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
        std::stoul(text) > 400)
        usage("volume's percent is 0..400");
    const auto percent = static_cast<unsigned>(std::stoul(text));
    const auto listing = mods::list_mods(options.mods);
    const auto it = std::find_if(listing.begin(), listing.end(),
                                 [&](const mods::ModListing& m) { return mods::upper(m.folder) == mods::upper(options.paths[0]); });
    if (it == listing.end()) throw std::runtime_error("no mod folder " + options.paths[0] + " in " + options.mods.string());
    if (!it->manifest) throw std::runtime_error(it->folder + " has no valid manifest (" + it->problem + ")");
    PortSettings s = read_port_settings(options.settings);
    mods::set_mod_volume(s, it->folder, percent);
    write_port_settings(options.settings, s);
    std::cout << "mod_volume." << it->folder << "=" << percent << " (\"" << it->manifest->name << "\"; its manifest says audio_volume="
              << it->manifest->audio_volume << ")\n"
              << "written to " << options.settings.string() << " (applies at the next level of a running game, else at its start)\n";
    return 0;
}

// Checkpoint 132md5: move <folder> up|down: one place in the resolution order, through the manifests' priority= (the
// launcher's mod panel's Move up / Move down; mods::move_mod changes the fewest manifests).
int move_mod(const Options& options) {
    if (options.paths.size() != 2 || (options.paths[1] != "up" && options.paths[1] != "down"))
        usage("move needs <mod folder> up|down");
    const auto listing = mods::list_mods(options.mods, settings_choices(options));
    const auto it = std::find_if(listing.begin(), listing.end(),
                                 [&](const mods::ModListing& m) { return mods::upper(m.folder) == mods::upper(options.paths[0]); });
    if (it == listing.end()) throw std::runtime_error("no mod folder " + options.paths[0] + " in " + options.mods.string());
    if (!it->manifest) throw std::runtime_error(it->folder + " has no valid manifest (" + it->problem + ")");
    const auto changes = mods::move_mod(listing, static_cast<std::size_t>(it - listing.begin()), options.paths[1] == "up" ? -1 : 1);
    if (changes.empty()) {
        std::cout << it->folder << " is already " << (options.paths[1] == "up" ? "first" : "last") << "; nothing changed\n";
        return 0;
    }
    mods::apply_priority_changes(changes);
    for (const auto& c : changes)
        std::cout << (c.root / "manifest.txt").string() << ": priority=" << c.to << " (was " << c.from << ")\n";
    std::cout << "Mod order (the first enabled wins, then the disc):";
    for (const auto& m : mods::list_mods(options.mods, settings_choices(options)))
        if (m.manifest) std::cout << " " << m.folder << " (" << m.manifest->priority << (m.enabled ? "" : ", off") << ")";
    std::cout << "\n";
    return 0;
}

// Checkpoint 132md4: declare <mod folder>: writes the manifest's overrides= from what the folder holds (each entry's
// archive: the key up to and including its first .HOG component, else the key; by-hash images as BY-HASH).
int declare_overrides(const Options& options) {
    for (const auto& folder : options.paths) {
        const fs::path root(folder);
        const auto manifest_path = root / "manifest.txt";
        const auto bytes = mods::read_file(manifest_path);
        std::string text(bytes.begin(), bytes.end());
        auto manifest = mods::parse_manifest(text);
        manifest.overrides.clear();
        manifest.enabled = true;
        std::vector<std::string> messages;
        const auto set = mods::ModSet::from_mods({{root.filename().string(), fs::absolute(root), manifest}}, messages);
        std::set<std::string> patterns;
        for (const auto& entry : set->entries()) {
            if (entry.kind == "audio" || entry.kind == "movies" || entry.kind == "data") continue;   // 132md13-16: not asset keys
            if (entry.kind == "by-hash") {
                patterns.insert("BY-HASH");
                continue;
            }
            std::string pattern;
            std::size_t at = 0;
            for (;;) {
                const auto slash = entry.key.find('/', at);
                const auto part = entry.key.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
                pattern += (pattern.empty() ? "" : "/") + part;
                if (part.ends_with(".HOG") || slash == std::string::npos) break;
                at = slash + 1;
            }
            patterns.insert(pattern);
        }
        if (patterns.empty()) throw std::runtime_error(folder + ": nothing to declare (no files, textures or models)");
        const std::string value = join(std::vector<std::string>(patterns.begin(), patterns.end()), ",");
        const std::string line = "overrides=" + value;
        const auto out = mods::with_manifest_value(text, "overrides", value);
        std::ofstream(manifest_path, std::ios::binary) << out;
        std::cout << manifest_path.string() << ": " << line << " (" << set->entries().size() << (set->entries().size() == 1 ? " entry" : " entries") << ")\n";
    }
    return 0;
}

// --- audio overrides (checkpoint 132md13, research/MODDING.md "Audio overrides") ---

// The levels whose GAMEDATA\<level>\SND.HOG / SNDVAG.HOG may hold sounds (tools/level_model_sweep.py's missions, the
// front end and COMMON; a HOG the disc lacks is skipped).
constexpr const char* sound_levels[] = {"BELARUS1", "BELARUS2", "CHECHNYA", "COMMON",   "FRONTEND", "ITALY",    "KYRGSTAN",
                                        "LORELEI",  "MINSK",    "MNTNEGR1", "MYANMAR",  "TOKYO",    "TORONTO1", "TORONTO2",
                                        "TORONTO3", "TRAINING", "UKRAINE",  "YEMEN1",   "YEMEN2",   "ZURICH"};

// A disc sound an audio_overrides= target can name (a VAG / VPK stream member or a bank sample) with the format the
// game fits a replacement to.
struct DiscSound {
    std::string key;
    std::uint32_t rate = 0, channels = 1;
    std::size_t frames = 0;
    bool bank = false, loops = false;
    std::vector<std::string> sounds;   // a bank sample's sounds
};

// The sounds of ZMEDIA\MUSIC.HOG, ZMEDIA\SNDVAG.HOG, each level's SND.HOG / SNDVAG.HOG and `extra_hogs` (disc paths):
// a stream from its 0x30-byte header, a bank's samples from the bank.
std::vector<DiscSound> disc_sounds(DiscImage& disc, const std::set<std::string>& extra_hogs) {
    std::set<std::string> hogs = extra_hogs;
    hogs.insert("ZMEDIA/MUSIC.HOG");
    hogs.insert("ZMEDIA/SNDVAG.HOG");
    for (const auto* level : sound_levels)
        for (const auto* name : {"SND.HOG", "SNDVAG.HOG"}) hogs.insert(std::string("GAMEDATA/") + level + "/" + name);
    std::vector<DiscSound> result;
    for (const auto& hog : hogs) {
        std::vector<DiscImage::ArchiveMember> members;
        try {
            members = disc.archive_members(split_path(hog));
        } catch (const std::exception&) {
            continue;
        }
        const auto hog_key = mods::asset_key(split_path(hog));
        for (const auto& member : members) {
            const auto name = mods::upper(member.name);
            const auto key = mods::member_key(hog_key, name);
            try {
                if (name.ends_with(".VAG") || name.ends_with(".VPK")) {
                    const auto header = disc.read({member.extent.offset, std::min<std::uint64_t>(0x30, member.extent.size)});
                    DiscSound sound{key};
                    if (audio::is_vpk(header)) {
                        const auto h = audio::parse_vpk_header(header);
                        sound.rate = h.rate;
                        sound.channels = h.channels;
                        sound.frames = std::size_t(h.channel_bytes) / 16 * 28;
                    } else if (audio::is_vag(header)) {
                        const auto h = audio::parse_vag_header(header);
                        sound.rate = h.rate;
                        sound.frames = std::size_t(h.data_size) / 16 * 28;
                    }
                    if (sound.rate && sound.channels) result.push_back(std::move(sound));
                } else if (name.ends_with(".BNK") && member.extent.size <= (64u << 20)) {
                    const auto bytes = disc.read(member.extent);
                    if (!mods::is_sound_bank(bytes)) continue;
                    for (const auto& sample : mods::bank_samples(bytes))
                        if (sample.rate)
                            result.push_back({key + "/" + std::to_string(sample.index), sample.rate, 1, sample.frames(), true, sample.loops,
                                              sample.sounds});
                }
            } catch (const std::exception&) {
            }
        }
    }
    return result;
}

// The HOGs a full-key target names (searched besides the usual ones).
std::set<std::string> target_hogs(const std::vector<std::string>& targets) {
    std::set<std::string> hogs;
    for (const auto& target : targets)
        if (const auto at = target.find(".HOG/"); at != std::string::npos) hogs.insert(target.substr(0, at + 4));
    return hogs;
}

std::set<std::string> target_hogs(const mods::ModSet& set) {
    std::vector<std::string> targets;
    for (const auto& entry : set.entries())
        if (entry.kind == "audio") targets.push_back(entry.key);
    return target_hogs(targets);
}

std::string sound_format(const DiscSound& sound) {
    char text[96];
    std::snprintf(text, sizeof text, "%u Hz %s%s, %.2f s", sound.rate,
                  sound.channels == 1 ? "mono" : sound.channels == 2 ? "stereo" : "multi-channel",
                  sound.bank ? (sound.loops ? " bank sample, looping" : " bank sample") : " stream", double(sound.frames) / sound.rate);
    return text;
}

// One audio_overrides= target (an "audio" entry) against the disc: the sounds it names, its file decoded and fitted
// to each of their formats (bank samples ADPCM-encoded) as the game does, and which of them it supplies (another
// entry wins the rest). Throws why it is refused (names nothing, an unreadable file).
struct AudioCheck {
    std::string text;
    std::size_t matched = 0, used = 0;
    std::vector<std::string> notes;
};

AudioCheck check_audio_override(const std::vector<DiscSound>& catalogue, const mods::ModSet& set, const mods::ModSet::Entry& entry) {
    std::vector<std::pair<const DiscSound*, int>> matches;
    for (const auto& sound : catalogue)
        if (const int rank = mods::audio_target_match(entry.key, sound.key, sound.sounds)) matches.push_back({&sound, rank});
    if (matches.empty())
        throw std::runtime_error("names no sound on the disc (no VAG / VPK member, bank sample or bank sound of that name in the "
                                 "music, SNDVAG and level SND HOGs)");
    const auto decoded = mods::decode_audio_file(mods::read_file(entry.source.path));
    AudioCheck check;
    check.matched = matches.size();
    std::set<std::tuple<bool, std::uint32_t, std::uint32_t, bool, std::size_t>> fitted;
    std::vector<std::string> used, formats;
    std::map<std::string, std::vector<std::string>> elsewhere;   // the winning entry -> the keys it takes
    const auto name = entry.key.substr(entry.key.rfind('/') + 1);
    for (const auto& [sound, rank] : matches) {
        const bool cut = sound->bank && !sound->loops;
        if (fitted.insert({sound->bank, sound->rate, sound->channels, sound->loops, cut ? sound->frames : 0}).second) {
            const auto clip = mods::fit_audio(decoded, sound->rate, sound->channels,
                                              cut ? std::optional<std::size_t>(sound->frames) : std::nullopt);
            if (sound->bank) (void)mods::encode_adpcm(clip.samples, sound->loops);
            formats.push_back(std::to_string(sound->rate) + " Hz x" + std::to_string(sound->channels));
        }
        const auto source = set.audio(sound->key, sound->sounds);
        if (source && source->mod == entry.source.mod && source->via == "audio_overrides " + entry.key) {
            ++check.used;
            used.push_back(sound->key + " (" + sound_format(*sound) + ")");
        } else {
            elsewhere[source ? source->mod->folder + "'s " + source->via : std::string("the disc")].push_back(sound->key);
        }
        std::vector<std::string> sharing;
        for (const auto& s : sound->sounds)
            if (rank != 1 || mods::upper(s) != name) sharing.push_back(s);
        if (sound->bank && !sharing.empty() && (rank == 1 || sound->sounds.size() > 1))
            check.notes.push_back(sound->key + ": the sample is shared: " + join(sharing, ", ") + (sharing.size() == 1 ? " plays" : " play") +
                                  " it too (replaced for " + (sharing.size() == 1 ? "it" : "them") + " as well)");
    }
    for (const auto& [who, keys] : elsewhere)
        check.notes.push_back(std::to_string(keys.size()) + " named sound(s) come from " + who + " instead: " +
                              join(std::vector<std::string>(keys.begin(), keys.begin() + std::min<std::size_t>(keys.size(), 4)), ", ") +
                              (keys.size() > 4 ? ", ..." : ""));
    std::ostringstream line;
    line.setf(std::ios::fixed);
    line.precision(2);
    line << decoded.rate << " Hz x" << decoded.channels << ", " << double(decoded.frames()) / decoded.rate << " s, volume "
         << set.volume(*entry.source.mod) << "% -> " << check.matched << " disc sound(s) named, " << check.used << " supplied";
    if (!used.empty())
        line << ": " << join(std::vector<std::string>(used.begin(), used.begin() + std::min<std::size_t>(used.size(), 4)), "; ")
             << (used.size() > 4 ? "; ..." : "");
    line << " (fitted to " << join(formats, ", ") << ")";
    check.text = line.str();
    return check;
}

std::string lower_text(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// The manifest text with each audio_overrides= item whose file is in `renamed` (lower case, '/') ending .wav for .ogg.
std::string wav_audio_overrides(const std::string& text, const std::set<std::string>& renamed) {
    std::string result;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto nl = text.find('\n', at);
        auto line = text.substr(at, (nl == std::string::npos ? text.size() : nl) - at);
        at = nl == std::string::npos ? text.size() : nl + 1;
        const auto eq = line.find('=');
        const auto first = line.find_first_not_of(" \t");
        if (eq != std::string::npos && first != std::string::npos && first < eq && line[first] != '#') {
            auto key = line.substr(first, eq - first);
            key.erase(key.find_last_not_of(" \t") + 1);
            if (lower_text(key) == "audio_overrides") {
                const auto value = line.substr(eq + 1);
                std::string rebuilt;
                std::size_t from = 0;
                for (;;) {
                    const auto comma = value.find(',', from);
                    auto item = value.substr(from, comma == std::string::npos ? std::string::npos : comma - from);
                    if (const auto item_eq = item.find('='); item_eq != std::string::npos) {
                        const auto last = item.find_last_not_of(" \t\r");
                        const auto start = item.find_first_not_of(" \t", item_eq + 1);
                        if (last != std::string::npos && start != std::string::npos && start <= last) {
                            auto file = lower_text(item.substr(start, last + 1 - start));
                            std::replace(file.begin(), file.end(), '\\', '/');
                            if (renamed.contains(file)) item.replace(last - 3, 4, ".wav");
                        }
                    }
                    rebuilt += item;
                    if (comma == std::string::npos) break;
                    rebuilt += ',';
                    from = comma + 1;
                }
                line = line.substr(0, eq + 1) + rebuilt;
            }
        }
        result += line;
        if (nl != std::string::npos) result += '\n';
    }
    return result;
}

// Checkpoint 132md13: pack --platform vita's audio: every sounds\ file and audio_overrides= file (.wav / .ogg) as a
// 16-bit WAV at the rate and channels the game fits it to (the disc sounds it replaces; several: the highest rate and
// channels), at most 48000 Hz stereo (audio::Mixer's rate, the Vita's SceAudioOut too); .ogg renamed .wav (the
// manifest's audio_overrides= rewritten). No gain is applied (audio_volume= and the player's volume apply at load).
// Returns the notes.vita text for it ("" with no audio).
std::string convert_audio_for_vita(std::vector<mods::ZipInput>& inputs, const mods::Manifest& manifest, const Options& options) {
    std::map<std::string, std::size_t> files;   // lower-case name -> input
    std::map<std::string, std::size_t> by_name;
    for (std::size_t i = 0; i < inputs.size(); ++i) by_name[lower_text(inputs[i].name)] = i;
    for (const auto& [name, i] : by_name)
        if (name.starts_with("sounds/") && (name.ends_with(".wav") || name.ends_with(".ogg"))) files[name] = i;
    std::map<std::string, std::vector<std::string>> targets;   // lower-case file -> the targets naming it
    std::vector<std::string> all_targets;
    for (const auto& o : manifest.audio_overrides) {
        auto file = lower_text(o.file);
        std::replace(file.begin(), file.end(), '\\', '/');
        targets[file].push_back(o.target);
        all_targets.push_back(o.target);
        if (const auto it = by_name.find(file); it != by_name.end()) files[file] = it->second;
    }
    if (files.empty()) return {};
    std::optional<std::vector<DiscSound>> catalogue;
    try {
        Disc disc(options.iso);
        catalogue = disc_sounds(*disc.image, target_hogs(all_targets));
    } catch (const std::exception& e) {
        std::cout << "  vita: warning: no disc image (" << e.what()
                  << "); each sound is kept at its own rate and channels (at most 48000 Hz stereo); the game resamples at load\n";
    }
    std::set<std::string> names;
    for (const auto& [name, i] : by_name) names.insert(name);
    std::set<std::string> renamed;
    std::uint64_t before = 0, after = 0;
    std::size_t oggs = 0, unmatched = 0;
    for (const auto& [name, i] : files) {
        std::uint32_t rate = 0, channels = 0;
        if (catalogue) {
            const auto fold = [&](const DiscSound& s) {
                rate = std::max(rate, s.rate);
                channels = std::max(channels, s.channels);
            };
            if (name.starts_with("sounds/")) {
                const auto key = mods::upper(name.substr(7, name.size() - 11));
                for (const auto& s : *catalogue)
                    if (s.key == key) fold(s);
            }
            if (const auto it = targets.find(name); it != targets.end())
                for (const auto& target : it->second)
                    for (const auto& s : *catalogue)
                        if (mods::audio_target_match(target, s.key, s.sounds)) fold(s);
            if (!rate) {
                ++unmatched;
                channels = 0;
                std::cout << "  vita: warning: " << inputs[i].name << " replaces no disc sound; kept at its own rate\n";
            }
        }
        auto& input = inputs[i];
        before += input.bytes.size();
        try {
            input.bytes = mods::convert_audio_file(input.bytes, std::min<std::uint32_t>(rate, 48000), std::min<std::uint32_t>(channels, 2));
        } catch (const std::exception& e) {
            throw std::runtime_error(input.name + ": " + e.what());
        }
        after += input.bytes.size();
        if (name.ends_with(".ogg")) {
            ++oggs;
            const auto wav = input.name.substr(0, input.name.size() - 4) + ".wav";
            if (!names.insert(lower_text(wav)).second) throw std::runtime_error(wav + ": the folder holds this sound as both .ogg and .wav");
            renamed.insert(name);
            input.name = wav;
        }
    }
    if (!renamed.empty() && !manifest.audio_overrides.empty()) {
        const auto manifest_input =
            std::find_if(inputs.begin(), inputs.end(), [&](const mods::ZipInput& in) { return lower_text(in.name) == "manifest.txt"; });
        if (manifest_input == inputs.end()) throw std::runtime_error("no manifest.txt at the folder's root");
        const auto text = wav_audio_overrides(std::string(manifest_input->bytes.begin(), manifest_input->bytes.end()), renamed);
        manifest_input->bytes.assign(text.begin(), text.end());
    }
    char sizes[64];
    std::snprintf(sizes, sizeof sizes, "%.1f MB -> %.1f MB", static_cast<double>(before) / (1 << 20), static_cast<double>(after) / (1 << 20));
    const auto note = std::to_string(files.size()) + " sound(s) as 16-bit WAV at the game's rates (" + std::to_string(oggs) +
                      " OGG decoded; " + sizes + ")";
    std::cout << "  vita: " << note << (unmatched ? "; " + std::to_string(unmatched) + " at their own rate" : std::string()) << "\n";
    return note;
}

constexpr std::uint32_t vita_screen_width = 960, vita_screen_height = 544;   // 132md12: the Vita's 960 x 544 screen

// Checkpoint 132md11: pack --platform vita: the textures\ images converted (mods::platform_texture, across threads) and
// renamed .dds, the manifest given platform=vita and a notes.vita= line (when it has none). 132md12: the wide.<screen>=
// images keep their names and aspect (a wide background is never power-of-two): a PNG at most the Vita's 960 px wide.
void convert_for_vita(std::vector<mods::ZipInput>& inputs, const mods::Manifest& manifest, std::uint32_t max_size,
                      const std::string& audio_note) {
    const auto lower = [](std::string text) {
        for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    std::set<std::string> wide_names;
    for (const auto& [screen, paths] : manifest.wide)
        for (const auto& path : paths) wide_names.insert(lower(path));
    std::vector<std::size_t> images;
    std::set<std::string> names;
    std::size_t wide = 0, wide_resized = 0;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto name = lower(inputs[i].name);
        if (wide_names.contains(name)) {
            ++wide;
            const auto size = mods::image_size(inputs[i].bytes);
            if (size.width > vita_screen_width) {
                const auto image = mods::decode_image(inputs[i].bytes);
                const auto height = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(
                    (std::uint64_t(image.height) * vita_screen_width + image.width / 2) / image.width));
                const auto png = mods::encode_png(mods::resample_image(image, vita_screen_width, height));
                // A .dds name keeps its name for the manifest; the reader goes by the signature.
                inputs[i].bytes = png;
                ++wide_resized;
            }
            continue;
        }
        if (name.starts_with("textures/") && (name.ends_with(".png") || name.ends_with(".dds"))) images.push_back(i);
    }
    if (wide)
        std::cout << "  vita: " << wide << " wide background image(s) kept as PNGs at their aspect (" << wide_resized << " scaled to "
                  << vita_screen_width << " px wide)\n";
    std::vector<mods::PlatformTexture> converted(images.size());
    std::vector<std::string> errors(images.size());
    std::vector<std::uint64_t> before(images.size());
    std::atomic<std::size_t> next{0};
    const auto run = [&] {
        for (std::size_t k; (k = next.fetch_add(1)) < images.size();) try {
                // A DDS already in the Vita's form (BC, power-of-two, within --max-size, the full chain) is kept as is.
                if (const auto blocks = mods::dds_blocks(inputs[images[k]].bytes)) {
                    const auto pot = [](std::uint32_t v) { return v >= 4 && (v & (v - 1)) == 0; };
                    std::size_t levels = 1;
                    for (auto side = std::max(blocks->width, blocks->height); side > 1; side /= 2) ++levels;
                    if (pot(blocks->width) && pot(blocks->height) && std::max(blocks->width, blocks->height) <= max_size &&
                        blocks->levels.size() == levels) {
                        before[k] = mods::replacement_bytes(blocks->width, blocks->height);
                        converted[k] = {inputs[images[k]].bytes, blocks->width, blocks->height, blocks->block_bytes};
                        continue;
                    }
                }
                const auto image = mods::decode_image(inputs[images[k]].bytes);
                before[k] = mods::replacement_bytes(image.width, image.height);
                converted[k] = mods::platform_texture(image, max_size);
            } catch (const std::exception& e) {
                errors[k] = e.what();
            }
    };
    std::vector<std::thread> pool;
    const auto threads = std::min<std::size_t>(images.size(), std::max(1u, std::thread::hardware_concurrency())) - (images.empty() ? 0 : 1);
    try {
        for (std::size_t t = 0; t < threads; ++t) pool.emplace_back(run);
    } catch (const std::system_error&) {
    }
    run();
    for (auto& t : pool) t.join();
    std::uint64_t rgba = 0, blocks = 0;
    std::size_t bc1 = 0, resized = 0;
    std::uint32_t largest = 0;
    for (std::size_t k = 0; k < images.size(); ++k) {
        auto& input = inputs[images[k]];
        if (!errors[k].empty()) throw std::runtime_error(input.name + ": " + errors[k]);
        const auto image_size = mods::image_size(input.bytes);
        resized += image_size.width != converted[k].width || image_size.height != converted[k].height;
        input.name = input.name.substr(0, input.name.size() - 4) + ".dds";
        if (!names.insert(lower(input.name)).second)
            throw std::runtime_error(input.name + ": the folder holds this texture as both .png and .dds");
        input.bytes = std::move(converted[k].dds);
        rgba += before[k];
        blocks += input.bytes.size() - 128;
        bc1 += converted[k].block_bytes == 8;
        largest = std::max({largest, converted[k].width, converted[k].height});
    }
    const auto mb = [](std::uint64_t bytes) {
        char text[32];
        std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / (1 << 20));
        return std::string(text);
    };
    const auto manifest_input = std::find_if(inputs.begin(), inputs.end(), [&](const mods::ZipInput& in) { return lower(in.name) == "manifest.txt"; });
    if (manifest_input == inputs.end()) throw std::runtime_error("no manifest.txt at the folder's root");
    std::string text(manifest_input->bytes.begin(), manifest_input->bytes.end());
    text = mods::with_manifest_value(text, "platform", "vita");
    if (!manifest.notes.contains("vita")) {
        std::vector<std::string> parts;
        if (!images.empty() || audio_note.empty())
            parts.push_back(std::to_string(images.size()) + " textures as BC1/BC3 (DXT1/DXT5) with mips, power-of-two, at most " +
                            std::to_string(max_size) + " px a side (" + mb(blocks) + ")");
        if (!audio_note.empty()) parts.push_back(audio_note);
        text = mods::with_manifest_value(text, "notes.vita", "packed by omega_mod_tool pack --platform vita: " + join(parts, "; "));
    }
    manifest_input->bytes.assign(text.begin(), text.end());
    std::cout << "  vita: " << images.size() << " texture(s) converted (" << bc1 << " BC1, " << images.size() - bc1 << " BC3; "
              << resized << " resized; largest side " << largest << " px): " << mb(rgba) << " as RGBA8 with mips -> " << mb(blocks)
              << " of blocks\n";
}

// Checkpoint 132md14: pack --platform vita's movies: kept as they are (MPEG-2, the port's own decoder on every
// platform; the sound is fitted to 48000 Hz stereo at play), each picture's size checked against the Vita's screen.
// Checkpoint 132md15: MP4 (H.264 + AAC) is kept too, noted as needing the Vita's sceAvPlayer movie backend.
// Returns the notes.vita text for them ("" with none).
std::string movie_note_for_vita(const std::vector<mods::ZipInput>& inputs, const mods::Manifest& manifest) {
    std::vector<std::string> sizes;
    for (const auto& o : manifest.movie_overrides) {
        if (o.video.empty()) continue;
        auto file = lower_text(o.video);
        std::replace(file.begin(), file.end(), '\\', '/');
        const auto in = std::find_if(inputs.begin(), inputs.end(), [&](const mods::ZipInput& i) { return lower_text(i.name) == file; });
        if (in == inputs.end()) continue;
        std::string size = "PSS";
        if (movie::is_platform_movie_name(in->name)) {
            // Checkpoint 132md15: kept as it is; the Vita plays it only through the host's sceAvPlayer backend
            // (research/MODDING.md "Platform movie decoders"), else the disc's movie plays there.
            std::cout << "  vita: movie " << o.target << " (" << in->name << ") is MP4: played on the Vita only through its "
                      << "sceAvPlayer movie backend (H.264 + AAC; a 960x544 copy decodes cheapest), else the disc's movie plays\n";
            sizes.push_back(o.target + " MP4");
            continue;
        }
        if (const auto info = mods::read_video_info(in->bytes)) {
            size = std::to_string(info->width) + "x" + std::to_string(info->height);
            if (std::uint32_t(info->width) > vita_screen_width || std::uint32_t(info->height) > vita_screen_height)
                std::cout << "  vita: warning: movie " << o.target << " (" << in->name << ") is " << size << ", past the "
                          << vita_screen_width << "x" << vita_screen_height << " screen: decoded in software there, a "
                          << "smaller copy plays more smoothly\n";
        }
        sizes.push_back(o.target + " " + size);
    }
    if (sizes.empty()) return {};
    const bool mp4 = std::any_of(sizes.begin(), sizes.end(), [](const std::string& s) { return s.ends_with(" MP4"); });
    std::cout << "  vita: " << sizes.size() << " movie(s) kept as they are: " << join(sizes, ", ") << "\n";
    return std::to_string(sizes.size()) + " movie(s) kept as they are (" + join(sizes, ", ") + "; the sound fitted to 48000 Hz stereo at play" +
           (mp4 ? "; MP4 plays through the Vita's sceAvPlayer movie backend, else the disc's movie" : "") + ")";
}

// Checkpoint 132md10: pack <mod folder>...: each folder as one .zip (mod_archive.hpp), checked by reading it back.
int pack_mods(const Options& options) {
    if (!options.out.empty() && options.paths.size() != 1) usage("pack --out takes one mod folder");
    if (!options.platform.empty() && options.platform != "pc" && options.platform != "vita") usage("pack --platform is pc or vita");
    const bool vita = options.platform == "vita";
    for (const auto& folder : options.paths) {
        const auto root = fs::absolute(fs::path(folder)).lexically_normal();
        if (!fs::is_directory(root)) throw std::runtime_error(folder + " is not a folder");
        const auto name = root.filename().u8string();
        const std::string id(name.begin(), name.end());
        const auto bytes = mods::read_file(root / "manifest.txt");
        const auto manifest = mods::parse_manifest(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        auto zip = options.out.empty() ? root.parent_path() / (root.filename().u8string() + u8".zip") : fs::absolute(options.out);
        if (!mods::is_zip_name(zip)) throw std::runtime_error(zip.string() + ": the archive's name must end in .zip");
        std::vector<mods::ZipInput> inputs;
        std::uint64_t total = 0;
        for (const auto& path : mods::mod_files_below(root)) {
            std::error_code same;
            if (fs::equivalent(path, zip, same)) continue;
            const auto rel = path.lexically_relative(root).generic_u8string();
            inputs.push_back({std::string(rel.begin(), rel.end()), mods::read_file(path)});
            total += inputs.back().bytes.size();
        }
        auto packed = manifest;   // 132md13: as packed (--platform vita renames .ogg audio_overrides= files)
        if (vita) {
            auto note = convert_audio_for_vita(inputs, manifest, options);
            if (const auto movies = movie_note_for_vita(inputs, manifest); !movies.empty()) note += (note.empty() ? "" : "; ") + movies;
            convert_for_vita(inputs, manifest, options.max_size, note);
            const auto in = std::find_if(inputs.begin(), inputs.end(), [](const mods::ZipInput& i) { return lower_text(i.name) == "manifest.txt"; });
            packed = mods::parse_manifest(std::string_view(reinterpret_cast<const char*>(in->bytes.data()), in->bytes.size()));
        }
        std::map<std::string, Bytes> expected;
        for (const auto& input : inputs) expected[input.name] = input.bytes;
        mods::write_zip_file(zip, inputs);
        // Read back: every member's bytes, then the archive indexed as the folder is.
        const auto archive = mods::register_archive(zip);
        std::size_t deflated = 0;
        for (const auto& m : archive->members()) {
            const auto it = expected.find(m.name);
            if (it == expected.end() || archive->read(m) != it->second) throw std::runtime_error(zip.string() + ": member " + m.name + " reads back wrong");
            deflated += m.method == 8;
        }
        if (archive->members().size() != expected.size()) throw std::runtime_error(zip.string() + ": members missing");
        std::vector<std::string> folder_messages, zip_messages;
        const auto from_folder = mods::ModSet::from_mods({{id, root, manifest}}, folder_messages);
        const auto from_zip = mods::ModSet::from_mods({{id, fs::absolute(zip), packed, fs::absolute(zip)}}, zip_messages);
        if (from_folder->wide_backgrounds().size() != from_zip->wide_backgrounds().size())
            throw std::runtime_error(zip.string() + ": indexes " + std::to_string(from_zip->wide_backgrounds().size()) +
                                     " wide background(s), the folder " + std::to_string(from_folder->wide_backgrounds().size()));
        if (from_folder->entries().size() != from_zip->entries().size())
            throw std::runtime_error(zip.string() + ": indexes " + std::to_string(from_zip->entries().size()) + " entries, the folder " +
                                     std::to_string(from_folder->entries().size()));
        std::cout << zip.string() << ": \"" << manifest.name << "\"" << (manifest.version.empty() ? "" : " " + manifest.version) << ", "
                  << inputs.size() << " file(s), " << total << " bytes -> " << fs::file_size(zip) << " bytes (" << deflated
                  << " deflated, " << inputs.size() - deflated << " stored); read back, " << from_zip->entries().size()
                  << " entr" << (from_zip->entries().size() == 1 ? "y" : "ies") << " as in the folder\n";
        if (manifest.game_version.empty())
            std::cout << "  note: no game_version= in the manifest (this game is " << mods::game_version_text << ")\n";
        else if (!mods::game_version_matches(manifest.game_version))
            std::cout << "  warning: game_version=" << manifest.game_version << " is not this game (" << mods::game_version_text << ")\n";
        if (zip.parent_path() == root.parent_path())
            std::cout << "  note: the folder " << id << " beside it is used instead while both are in the mods folder\n";
    }
    return 0;
}

// --- sounds (checkpoint 132md4) ---

bool is_stream(std::span<const std::uint8_t> bytes) { return audio::is_vag(bytes) || audio::is_vpk(bytes); }

int export_sounds(const Options& options) {
    Disc disc(options.iso);
    write_manifest(options.out);
    std::ofstream index(options.out / "sounds_index.csv", std::ios::binary | std::ios::app);
    if (fs::file_size(options.out / "sounds_index.csv") == 0) index << "key,format,rate,channels,frames,seconds,file,sounds\n";
    std::size_t written = 0, failed = 0, missing = 0;
    const auto stream = [&](const std::string& key, std::span<const std::uint8_t> bytes) {
        if (!options.match.empty() && key.find(mods::upper(options.match)) == std::string::npos) return;
        audio::PcmClip clip;
        try {
            clip = audio::decode_stream(bytes);
        } catch (const std::exception& e) {
            std::cerr << "  " << key << ": not decodable (" << e.what() << "); skipped\n";
            ++failed;
            return;
        }
        const auto relative = fs::path("sounds") / key_path(key).concat(".wav");
        const auto path = options.out / relative;
        fs::create_directories(path.parent_path());
        write_bytes(path, mods::encode_wav(clip));
        index << key << ',' << (audio::is_vpk(bytes) ? "VPK" : "VAG") << ',' << clip.rate << ',' << clip.channels << ','
              << clip.frames() << ',' << (clip.rate ? double(clip.frames()) / clip.rate : 0.0) << ','
              << relative.generic_string() << ",\n";
        ++written;
    };
    // Checkpoint 132md5: a bank's samples as <bank key>/<n>.wav ("BNK" or "BNK-loop"; the sounds using each).
    const auto bank = [&](const std::string& key, std::span<const std::uint8_t> bytes) {
        std::vector<mods::BankSample> samples;
        try {
            samples = mods::bank_samples(bytes);
        } catch (const std::exception& e) {
            std::cerr << "  " << key << ": the sample table is not readable (" << e.what() << "); skipped\n";
            ++failed;
            return;
        }
        for (const auto& sample : samples) {
            const auto sample_key = key + "/" + std::to_string(sample.index);
            if (!options.match.empty() && sample_key.find(mods::upper(options.match)) == std::string::npos) continue;
            audio::PcmClip clip;
            try {
                clip = mods::decode_bank_sample(bytes, sample);
            } catch (const std::exception& e) {
                std::cerr << "  " << sample_key << ": not decodable (" << e.what() << "); skipped\n";
                ++failed;
                continue;
            }
            const auto relative = fs::path("sounds") / key_path(key) / (std::to_string(sample.index) + ".wav");
            const auto path = options.out / relative;
            fs::create_directories(path.parent_path());
            write_bytes(path, mods::encode_wav(clip));
            std::string users;
            for (const auto& name : sample.sounds) users += (users.empty() ? "" : " ") + name;
            index << sample_key << ',' << (sample.loops ? "BNK-loop" : "BNK") << ',' << clip.rate << ",1,"
                  << clip.frames() << ',' << double(clip.frames()) / clip.rate << ',' << relative.generic_string() << ','
                  << users << '\n';
            ++written;
        }
    };
    std::function<void(const std::string&, std::span<const std::uint8_t>, int)> walk =
        [&](const std::string& key, std::span<const std::uint8_t> bytes, int depth) {
            if (is_stream(bytes)) {
                stream(key, bytes);
                return;
            }
            if (mods::is_sound_bank(bytes)) {
                bank(key, bytes);
                return;
            }
            std::vector<mods::HogMember> members;
            try {
                members = mods::hog_members(bytes);
            } catch (const std::exception&) {
                return;
            }
            for (const auto& member : members)
                if (depth < 4) walk(mods::member_key(key, member.name), bytes.subspan(member.offset, member.size), depth + 1);
        };
    for (const auto& path : options.paths) {
        Located located;
        try {
            located = locate(*disc.image, path);
        } catch (const std::exception& error) {
            std::cerr << "  " << error.what() << "; skipped\n";
            ++missing;
            continue;
        }
        auto key = mods::asset_key(located.file);
        for (const auto& member : located.members) key = mods::member_key(key, member);
        const auto before = written;
        walk(key, located.bytes, 0);
        std::cout << key << ": " << (written - before) << " sound(s)\n";
    }
    std::cout << "Exported " << written << " WAV(s) to " << fs::absolute(options.out).string() << " (" << failed
              << " undecodable, " << missing << " path(s) not on the disc); index sounds_index.csv\n";
    return missing == options.paths.size() ? 1 : 0;
}

// One sound entry against the disc's original: the conversion the game applies (check-sounds' text after "accepted:
// <entry>: "). Throws why it is refused.
std::string check_sound(DiscImage& disc, const mods::ModSet::Entry& entry) {
    std::ostringstream line;
    line.setf(std::ios::fixed);
    line.precision(2);
    // Checkpoint 132md5: <bank>.BNK/<n>, a bank sample.
    const auto slash = entry.key.rfind('/');
    const auto bank_key = entry.key.substr(0, slash);
    if (bank_key.ends_with(".BNK")) {
        const auto located = locate(disc, bank_key);
        if (!mods::is_sound_bank(located.bytes)) throw std::runtime_error("the disc's " + bank_key + " is not a sound bank");
        const auto samples = mods::bank_samples(located.bytes);
        const auto number = entry.key.substr(slash + 1);
        std::size_t n = 0;
        if (number.empty() || number.size() > 6 || !std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
            (n = std::stoul(number)) >= samples.size())
            throw std::runtime_error("the disc's " + bank_key + " has " + std::to_string(samples.size()) +
                                     " samples, numbered from 0 (" + number + " is not one)");
        const auto& sample = samples[n];
        const auto decoded = mods::decode_audio_file(mods::read_file(entry.source.path));
        const auto fitted = mods::fit_audio(decoded, sample.rate, 1,
                                            sample.loops ? std::nullopt : std::optional<std::size_t>(sample.frames()));
        (void)mods::encode_adpcm(fitted.samples, sample.loops);
        std::string users;
        for (const auto& name : sample.sounds) users += (users.empty() ? "" : " ") + name;
        line << decoded.rate << " Hz x" << decoded.channels << ", " << double(decoded.frames()) / decoded.rate
             << " s -> bank sample " << n << " at " << sample.rate << " Hz mono; " << double(fitted.frames()) / sample.rate
             << " s against the original's " << double(sample.frames()) / sample.rate << " s ("
             << (sample.loops ? "a looping sample: its own length, looped whole" : "cut / padded to the original's length")
             << "); sounds: " << (users.empty() ? "-" : users);
        return line.str();
    }
    const auto located = locate(disc, entry.key);
    if (!is_stream(located.bytes)) throw std::runtime_error("the disc's " + entry.key + " is not a VAG / VPK stream");
    const auto original = audio::decode_stream(located.bytes);
    const auto decoded = mods::decode_audio_file(mods::read_file(entry.source.path));
    const auto fitted = mods::fit_audio(decoded, original.rate, original.channels);
    line << decoded.rate << " Hz x" << decoded.channels << ", " << double(decoded.frames()) / decoded.rate
         << " s -> the original's " << original.rate << " Hz x" << original.channels << "; "
         << double(fitted.frames()) / original.rate << " s against the original's "
         << double(original.frames()) / original.rate
         << " s (a stream the game does not loop is cut / padded to the original's length)";
    return line.str();
}

std::string sound_label(const mods::ModSet::Entry& entry) {
    return entry.key + " <- " + entry.source.mod->folder + " (" + entry.source.path.filename().string() + ")";
}

int check_sounds(const Options& options) {
    Disc disc(options.iso);
    std::vector<std::string> messages;
    const auto set = mods::ModSet::scan(options.mods, messages, settings_choices(options));
    set->set_volumes(settings_volumes(options));   // 132md13
    for (const auto& m : messages)
        if (m.find(": ignored ") != std::string::npos) std::cout << m << "\n";
    std::size_t accepted = 0, refused = 0;
    std::optional<std::vector<DiscSound>> catalogue;   // 132md13: built for the first audio_overrides= target
    for (const auto& entry : set->entries()) {
        if (entry.kind != "sounds" && entry.kind != "audio") continue;
        if (!options.match.empty() && entry.key.find(mods::upper(options.match)) == std::string::npos) continue;
        const auto label = (entry.kind == "audio" ? "audio_overrides " : "") + sound_label(entry);
        try {
            if (entry.kind == "audio") {
                if (!catalogue) catalogue = disc_sounds(*disc.image, target_hogs(*set));
                const auto check = check_audio_override(*catalogue, *set, entry);
                std::cout << "accepted: " << label << ": " << check.text << "\n";
                for (const auto& note : check.notes) std::cout << "  note: " << note << "\n";
            } else {
                const auto volume = set->volume(*entry.source.mod);
                std::cout << "accepted: " << label << ": " << check_sound(*disc.image, entry)
                          << (volume == 100 ? std::string() : "; volume " + std::to_string(volume) + "%") << "\n";
            }
            ++accepted;
        } catch (const std::exception& e) {
            std::cout << "refused: " << label << ": " << e.what() << "\n";
            ++refused;
        }
    }
    std::cout << (accepted + refused) << " sound(s): " << accepted << " accepted, " << refused << " refused\n";
    return refused ? 1 : 0;
}

int decode_audio(const Options& options) {
    fs::create_directories(options.out);
    int status = 0;
    for (const auto& file : options.paths) {
        try {
            const auto decoded = mods::decode_audio_file(mods::read_file(file));
            const auto clip = mods::fit_audio(decoded, decoded.rate, decoded.channels);
            const auto target = options.out / fs::path(file).filename().replace_extension(".wav");
            write_bytes(target, mods::encode_wav(clip));
            std::cout << file << ": " << decoded.rate << " Hz x" << decoded.channels << ", " << decoded.frames()
                      << " frames -> " << target.string() << "\n";
        } catch (const std::exception& e) {
            std::cout << file << ": " << e.what() << "\n";
            status = 1;
        }
    }
    return status;
}

// --- movie overrides (checkpoint 132md14, research/MODDING.md "Movie overrides") ---

std::string seconds_text(double s) {
    char text[32];
    std::snprintf(text, sizeof text, "%.2f s", s);
    return text;
}

// One movie_overrides= target (a "movies" entry) against the disc: the target is a ZMOVIES.HOG movie, the picture
// decoded whole as the game decodes it (its size, rate, aspect and length against the original's), the sound decoded,
// the subtitles parsed. Throws why it is refused (no such movie, an unreadable file); warnings and notes are returned.
struct MovieCheck {
    std::string text;
    std::vector<std::string> warnings, notes;
    std::uint32_t width = 0, height = 0;
};

MovieCheck check_movie_override(DiscImage& disc, const mods::ModSet& set, const mods::ModSet::Entry& entry) {
    const auto source = set.movie(entry.key);
    if (!source) throw std::runtime_error("not indexed");
    std::optional<DiscImage::ArchiveMember> original;
    for (const auto& m : disc.archive_members(movie::movies_hog()))
        if (mods::upper(m.name) == entry.key) original = m;
    if (!original)
        throw std::runtime_error("ZMEDIA/ZMOVIES.HOG has no movie " + entry.key + " (omega_movie --list names them); it is never played");
    // The original's length: the picture start codes (00 00 01 00) of its video stream (demuxed: the PCM sound has
    // the same bytes) at 30000/1001 a second. Read in pieces (a movie is larger than one disc read may be); the video
    // bytes' last 3 are carried into the next piece (a code across two pieces is counted once).
    double original_seconds = 0;
    {
        constexpr std::uint64_t piece = 4u << 20;
        movie::PssDemuxer demux;
        std::size_t pictures = 0;
        const auto count = [&] {
            auto& v = demux.video();
            for (std::size_t i = 0; i + 3 < v.size(); ++i)
                if (v[i] == 0 && v[i + 1] == 0 && v[i + 2] == 1 && v[i + 3] == 0) ++pictures;
            if (v.size() > 3) v.erase(v.begin(), v.end() - 3);
            demux.audio().clear();
        };
        for (std::uint64_t at = 0; at < original->extent.size; at += piece) {
            demux.push(disc.read({original->extent.offset + at, std::min(piece, original->extent.size - at)}));
            count();
        }
        original_seconds = pictures * 1001.0 / 30000.0;
    }
    MovieCheck check;
    std::ostringstream line;
    double picture_seconds = original_seconds;
    std::string own_sound;   // 132md15: an MP4's own sound, for the line
    if (!source->video.empty()) {
        const auto name = source->video.filename().string();
        std::size_t frames = 0;
        std::string error;
        if (lower_text(source->video.extension().string()) == ".pss") {
            movie::PssMovie pss(mods::mod_file_source(source->video), 256 * 1024);
            mpeg2::Frame frame;
            try {
                while (pss.next_frame(frame)) {
                    if (!frames) check.width = static_cast<std::uint32_t>(frame.width), check.height = static_cast<std::uint32_t>(frame.height);
                    ++frames;
                }
            } catch (const std::exception& e) {
                error = e.what();
            }
            picture_seconds = frames * 1001.0 / 30000.0;
            line << "picture " << name << " (PSS) " << check.width << "x" << check.height << ", " << frames << " pictures, "
                 << seconds_text(picture_seconds);
        } else if (movie::is_platform_movie_name(name)) {
            // Checkpoint 132md15: through the platform's decoder (platform_movie.hpp), as the front end plays it.
            const auto* backend = movie::platform_movie_backend();
            if (!backend)
                throw std::runtime_error(name + ": this build has no MP4 movie decoder (Windows: Media Foundation; the Vita: the "
                                         "host's sceAvPlayer backend)");
            mods::PlatformVideo video(*backend, mods::mod_movie_file(source->video));
            const auto info = video.info();
            mpeg2::Frame frame;
            double seconds = 0, last = 0, interval = 0;
            while (video.pop(frame, seconds)) {
                if (frames) interval = seconds - last;
                last = seconds;
                if (frame.width != info.width || frame.height != info.height)
                    check.warnings.push_back(name + ": picture " + std::to_string(frames) + " is " + std::to_string(frame.width) + "x" +
                                             std::to_string(frame.height) + ", not the stream's " + std::to_string(info.width) + "x" +
                                             std::to_string(info.height));
                ++frames;
            }
            check.width = static_cast<std::uint32_t>(info.width);
            check.height = static_cast<std::uint32_t>(info.height);
            picture_seconds = frames ? last + (interval > 0 ? interval : 1.0 / 30.0) : 0.0;
            char fps[32] = "variable";
            if (info.frame_rate > 0) std::snprintf(fps, sizeof fps, "%.3f", info.frame_rate);
            const float aspect = info.display_aspect > 0 ? info.display_aspect : 4.0F / 3.0F;
            char ratio[32];
            std::snprintf(ratio, sizeof ratio, "%.2f:1", aspect);
            line << "picture " << name << " " << info.width << "x" << info.height << " " << info.video_codec << " " << fps << " fps ("
                 << backend->name << "), display " << ratio << ", " << frames << " pictures, " << seconds_text(picture_seconds);
            if (aspect > 4.0F / 3.0F + 0.01F)
                check.notes.push_back(name + ": wider than 4:3: letterboxed in the 4:3 frame, drawn at its aspect in the wide one");
            else if (aspect < 4.0F / 3.0F - 0.01F)
                check.notes.push_back(name + ": narrower than 4:3: pillarboxed");
            if (info.frame_rate > 0 && std::fabs(info.frame_rate - 30000.0 / 1001.0) > 0.01)
                check.notes.push_back(name + ": shown at the player's 29.97 pictures a second (" + fps + " fps: pictures " +
                                      (info.frame_rate < 29.9 ? "repeated" : "dropped") + ", by their times)");
            if (source->audio.empty() && video.has_sound()) {
                std::vector<std::int16_t> pcm;
                std::uint64_t sound_frames = 0;
                for (;;) {
                    pcm.clear();
                    const auto got = video.take_sound(sound_frames, sound_frames + 480000, pcm);
                    sound_frames += got;
                    if (got < 480000) break;
                }
                const double sound_seconds = double(sound_frames) / 48000.0;
                own_sound = ", sound " + name + "'s own " + info.audio_codec + " " + std::to_string(info.audio_rate) + " Hz x" +
                            std::to_string(info.audio_channels) + ", " + seconds_text(sound_seconds) +
                            " (played at 48000 Hz stereo, volume " + std::to_string(set.volume(*source->mod)) + "%)";
                if (sound_seconds + 1.0 < picture_seconds)
                    check.warnings.push_back(name + ": " + seconds_text(sound_seconds) + " of sound under " + seconds_text(picture_seconds) +
                                             " of picture (silence after it)");
            } else if (source->audio.empty()) {
                own_sound = ", sound the disc movie's (" + name + " has none" +
                            (info.audio_codec.empty() ? std::string() : ": " + info.audio_codec) + ")";
            }
            error = video.error();
        } else {
            mods::ElementaryVideo video(mods::mod_file_source(source->video));
            const auto info = video.info();
            if (!info) throw std::runtime_error(name + ": " + video.error());
            mpeg2::Frame frame;
            while (video.pop(frame)) {
                if (frame.width != info->width || frame.height != info->height)
                    check.warnings.push_back(name + ": picture " + std::to_string(frames) + " is " + std::to_string(frame.width) + "x" +
                                             std::to_string(frame.height) + ", not the sequence's " + std::to_string(info->width) + "x" +
                                             std::to_string(info->height));
                ++frames;
            }
            error = video.error();
            check.width = static_cast<std::uint32_t>(info->width);
            check.height = static_cast<std::uint32_t>(info->height);
            picture_seconds = frames * double(info->rate_den) / info->rate_num;
            char fps[32];
            std::snprintf(fps, sizeof fps, "%.3f", double(info->rate_num) / info->rate_den);
            const float aspect = info->display_aspect();
            char ratio[32];
            std::snprintf(ratio, sizeof ratio, "%.2f:1", aspect);
            line << "picture " << name << " " << info->width << "x" << info->height << " " << fps << " fps, display " << ratio << ", "
                 << frames << " pictures, " << seconds_text(picture_seconds);
            if (aspect > 4.0F / 3.0F + 0.01F)
                check.notes.push_back(name + ": wider than 4:3: letterboxed in the 4:3 frame, drawn at its aspect in the wide one");
            else if (aspect < 4.0F / 3.0F - 0.01F)
                check.notes.push_back(name + ": narrower than 4:3: pillarboxed");
            if (info->rate_code != 4)
                check.notes.push_back(name + ": shown at the player's 29.97 pictures a second (" + fps + " fps: pictures " +
                                      (double(info->rate_num) / info->rate_den < 29.9 ? "repeated" : "dropped") + ")");
        }
        if (!error.empty()) {
            if (!frames) throw std::runtime_error(name + ": " + error);
            check.warnings.push_back(name + ": " + error + " after " + std::to_string(frames) + " picture(s); the movie ends there");
        }
        if (!frames) throw std::runtime_error(name + ": no pictures");
    } else {
        line << "picture the disc's";
    }
    line << " (original " << seconds_text(original_seconds) << ")";
    if (std::abs(picture_seconds - original_seconds) > 1.0)
        check.notes.push_back("the movie lasts " + seconds_text(picture_seconds) + " against the original's " + seconds_text(original_seconds) +
                              ": it ends (and the game goes on) when its picture ends" +
                              (source->subtitles.empty() ? "; the disc's subtitles keep the original's timing (give a .sub to retime them)" : ""));
    if (!source->audio.empty()) {
        const auto name = source->audio.filename().string();
        const auto decoded = mods::decode_audio_file(mods::read_file(source->audio));
        const double seconds = double(decoded.frames()) / decoded.rate;
        line << ", sound " << name << " " << decoded.rate << " Hz x" << decoded.channels << ", " << seconds_text(seconds) << " (played at 48000 Hz stereo, volume "
             << set.volume(*source->mod) << "%)";
        if (seconds + 1.0 < picture_seconds)
            check.warnings.push_back(name + ": " + seconds_text(seconds) + " of sound under " + seconds_text(picture_seconds) + " of picture (silence after it)");
        else if (seconds > picture_seconds + 1.0)
            check.warnings.push_back(name + ": " + seconds_text(seconds) + " of sound under " + seconds_text(picture_seconds) +
                                     " of picture (the rest is not played)");
    } else if (!own_sound.empty()) {
        line << own_sound;
    } else if (!source->video.empty()) {
        line << (lower_text(source->video.extension().string()) == ".pss" ? ", sound the PSS's own" : ", sound the disc movie's");
    }
    if (!source->subtitles.empty()) {
        const auto bytes = mods::read_file(source->subtitles);
        const auto parsed = movie::parse_subtitles(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        line << ", subtitles " << source->subtitles.filename().string() << " (" << parsed.feeds.size() << " feed(s))";
        if (parsed.feeds.empty()) check.warnings.push_back(source->subtitles.filename().string() + ": no subtitle feeds");
    }
    check.text = line.str();
    return check;
}

// --- validate (checkpoint 132md9, research/MODDING.md "Validating a mod") ---
// Platform figures the memory table is measured against. The PC renders every image at its full size (no cap). The
// Vita (PlayStation Vita: 512 MB RAM, 128 MB CDRAM; conservative) runs with the texture memory cap's Vita default
// (mods::default_texture_memory_cap_mb there: 64 MB, half the CDRAM) and GXM's 4096 px texture side.
constexpr std::uint32_t vita_cap_mb = 64, vita_cdram_mb = 128, vita_ram_mb = 512, vita_max_side = 4096;

constexpr std::uint32_t pc_max_side = 16384;   // Direct3D 11's texture side

struct Findings {
    std::size_t errors = 0, warnings = 0, notes = 0;
    void error(const std::string& what) {
        std::cout << "  ERROR: " << what << "\n";
        ++errors;
    }
    void warning(const std::string& what) {
        std::cout << "  warning: " << what << "\n";
        ++warnings;
    }
    void note(const std::string& what) {
        std::cout << "  note: " << what << "\n";
        ++notes;
    }
};

std::string megabytes(std::uint64_t bytes) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / (1 << 20));
    return text;
}

std::string size_text(std::uint32_t width, std::uint32_t height) { return std::to_string(width) + "x" + std::to_string(height); }

// A texture key's TDX and frame ("<...>.TDX" frame 0, "<...>.TDX.<n>" frame n).
std::optional<std::pair<std::string, std::size_t>> texture_frame(const std::string& key) {
    if (key.ends_with(".TDX")) return std::pair{key, std::size_t{0}};
    const auto at = key.rfind(".TDX.");
    if (at == std::string::npos) return std::nullopt;
    const auto digits = key.substr(at + 5);
    if (digits.empty() || digits.size() > 4 || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return std::nullopt;
    return std::pair{key.substr(0, at + 4), static_cast<std::size_t>(std::stoul(digits))};
}

// The disc's texture frame sizes by TDX key; the archive holding them is kept while the (key-ordered) entries stay in it.
struct OriginalTextures {
    DiscImage& disc;
    std::string archive_key;
    std::optional<Located> archive;
    std::map<std::string, std::vector<std::pair<std::uint32_t, std::uint32_t>>> sizes;

    // Throws (why) when the disc has no such texture.
    const std::vector<std::pair<std::uint32_t, std::uint32_t>>& frames(const std::string& tdx) {
        if (const auto found = sizes.find(tdx); found != sizes.end()) return found->second;
        const auto slash = tdx.rfind('/');
        const auto parent = slash == std::string::npos ? std::string() : tdx.substr(0, slash);
        if (parent != archive_key) {
            archive_key = parent;
            archive.reset();
            sizes.clear();
            try {
                if (!parent.empty()) archive = locate(disc, parent);
            } catch (const std::exception&) {
            }
        }
        Texture texture;
        if (archive) {
            std::vector<mods::HogMember> members;
            try {
                members = mods::hog_members(archive->bytes);
            } catch (const std::exception&) {
                throw std::runtime_error("the disc's " + parent + " is not an archive");
            }
            const auto name = tdx.substr(slash + 1);
            const auto it = std::find_if(members.begin(), members.end(), [&](const mods::HogMember& m) { return mods::upper(m.name) == name; });
            if (it == members.end()) throw std::runtime_error("the disc's " + parent + " has no member " + name);
            try {
                texture = decode_tdx(std::span<const std::uint8_t>(archive->bytes).subspan(it->offset, it->size));
            } catch (const std::exception&) {
                texture = decode_tdx(tdx_in_place(archive->bytes, it->offset, it->size));
            }
        } else {
            texture = decode_tdx(locate(disc, tdx).bytes);
        }
        auto& result = sizes[tdx];
        result.emplace_back(texture.width, texture.height);
        for (const auto& frame : texture.animation) result.emplace_back(frame.width, frame.height);
        return result;
    }
};

struct ImageUse {
    std::string label;
    std::uint32_t width{}, height{};
    std::uint64_t bytes{};
};

// --- data overrides (checkpoint 132md16, research/MODDING.md "Gameplay data overrides") ---

// The disc's bytes of a data file key, through the set's files\ replacement of it when there is one (what the game
// patches).
std::vector<std::uint8_t> data_file_bytes(DiscImage& disc, const mods::ModSet& set, const std::string& key) {
    const auto extent = disc.file(split_path(key));
    auto bytes = disc.read(extent);
    if (set.file(key)) {
        if (auto resolved = mods::resolve_bytes(set, key, [&] { return bytes; })) return std::move(*resolved);
    }
    return bytes;
}

// Each data_overrides= field: its selector names a record (a weapon id / display key, a level with a DATA.POP, an NPC
// id of it), and what the game will use (the original -> the new value, per patched record).
void check_data_overrides(DiscImage& disc, const mods::ModSet& set, Findings& found) {
    std::cout << "Data overrides (gameplay: the LAN / online gameplay policies need the host's set):\n";
    std::set<std::string> keys;   // the data files to patch
    std::set<std::string> levels; // the levels a [npc] selector names
    bool every_level = false;
    std::optional<std::vector<std::string>> weapon_keys;
    for (const auto& d : set.data()) {
        const auto label = "data " + mods::data_entry_key(d.item) + " <- " + d.source.mod->folder + " (" + d.item.file + " line " +
                           std::to_string(d.item.line) + ")";
        if (d.item.table == "weapon") {
            keys.insert("GAMEDATA/COMMON/WEAPONS.WDB");
            try {
                if (!weapon_keys) weapon_keys = mods::weapon_record_keys(data_file_bytes(disc, set, "GAMEDATA/COMMON/WEAPONS.WDB"));
                const bool by_id = std::all_of(d.item.selector.begin(), d.item.selector.end(), [](char c) { return c >= '0' && c <= '9'; });
                const auto matches = by_id ? (std::stoul(d.item.selector) < weapon_keys->size() ? 1u : 0u)
                                           : static_cast<unsigned>(std::count_if(weapon_keys->begin(), weapon_keys->end(), [&](const std::string& k) {
                                                 return mods::upper(k) == d.item.selector;
                                             }));
                if (!matches)
                    found.warning(label + ": unused, " + (by_id ? "record " + d.item.selector + " is past the " +
                                                                      std::to_string(weapon_keys->size()) + " records"
                                                                : "no record's display key is " + d.item.selector));
                else if (matches > 1)
                    found.note(label + ": " + std::to_string(matches) + " records have the key " + d.item.selector + "; every one is changed");
            } catch (const std::exception& e) {
                found.error(label + ": the weapons database is not readable (" + e.what() + ")");
            }
        } else if (d.item.table == "npc") {
            const auto space = d.item.selector.find(' ');
            const auto level = d.item.selector.substr(0, space), id = d.item.selector.substr(space + 1);
            if (level == "*") {
                every_level = true;
                continue;
            }
            levels.insert(level);
            const auto key = "GAMEDATA/" + level + "/DATA.POP";
            try {
                const auto population = decode_population(data_file_bytes(disc, set, key));
                keys.insert(key);
                if (id != "*" && id != "PLAYER" &&
                    std::none_of(population.npcs.begin(), population.npcs.end(),
                                 [&](const PopulationNpc& n) { return std::to_string(n.id) == id; }))
                    found.warning(label + ": unused, " + level + " has no NPC " + id + " (the play log's \"Hit at frame\" lines give ids)");
                if (id != "*" && !population.npcs.empty() && std::to_string(population.npcs.front().id) == id)
                    found.note(label + ": NPC " + id + " is " + level + "'s player record");
            } catch (const std::exception& e) {
                found.warning(label + ": unused, no readable " + key + " (" + e.what() + ")");
            }
            if (id == "PLAYER" && d.item.field == "armor")
                found.note(label + ": the player's armor is StartingArmor's at run time, whatever the record says");
        } else if (d.item.table == "tohit") {
            std::cout << "  " << label << ": game::ToHit " << d.item.field << " = " << d.item.value << " (every level)\n";
        }
    }
    if (every_level)
        for (const auto* level : sound_levels) {
            const std::string name(level);
            if (name == "COMMON" || name == "FRONTEND") continue;
            try {
                (void)disc.file(split_path("GAMEDATA/" + name + "/DATA.POP"));
                keys.insert("GAMEDATA/" + name + "/DATA.POP");
            } catch (const std::exception&) {
            }
        }
    for (const auto& key : keys) {
        std::vector<mods::DataChange> changes;
        try {
            const auto bytes = data_file_bytes(disc, set, key);
            if (!mods::apply_data_overrides(set, key, bytes, &changes)) continue;
        } catch (const std::exception& e) {
            found.error(key + ": the overrides cannot be applied (" + e.what() + "); the game reads the file unchanged");
            continue;
        }
        std::size_t same = 0;
        for (const auto& c : changes) {
            if (c.from == c.to) {
                ++same;
                continue;
            }
            std::cout << "  " << c.what << ": " << c.from << " -> " << c.to << " (" << c.mod << ")\n";
        }
        if (same)
            found.note(key + ": " + std::to_string(same) + " field(s) keep the disc's value (already that value, or 0 scaled by a percentage)");
    }
}

int validate_mods(const Options& options) {
    // validate <mod folder>...: those folders alone, enabled whatever the manifests / settings say.
    auto choices = settings_choices(options);
    if (!options.paths.empty()) {
        choices.clear();
        std::set<std::string> named;
        for (const auto& path : options.paths) named.insert(mods::upper(path));
        for (const auto& listing : mods::list_mods(options.mods)) choices[mods::upper(listing.folder)] = named.erase(mods::upper(listing.folder)) > 0;
        if (!named.empty()) throw std::runtime_error("no mod folder " + *named.begin() + " in " + fs::absolute(options.mods).string());
    }
    Findings found;
    std::vector<std::string> messages;
    const auto set = mods::ModSet::scan(options.mods, messages, choices);
    set->set_volumes(settings_volumes(options));   // 132md13
    std::cout << "Mods folder " << fs::absolute(options.mods).string() << ": " << set->mods().size() << " enabled mod(s)\n";
    for (const auto& m : messages) {
        if (m.starts_with("mod ") || m.find(": enabled in the settings") != std::string::npos ||
            (m.starts_with("skipped ") && m.find(": disabled ") != std::string::npos))
            std::cout << "  " << m << "\n";
        else if (m.starts_with("no mods folder") ||
                 (m.starts_with("skipped ") && m.find(": no manifest.txt") == std::string::npos) ||
                 m.find(": ignored data_overrides ") != std::string::npos)   // 132md16: a bad name / range
            found.error(m);
        else
            found.warning(m.starts_with("conflict: ") ? "unused (shadowed): " + m.substr(10) : m);
    }
    if (set->mods().empty()) {
        std::cout << "validate: nothing to check; " << found.errors << " error(s), " << found.warnings << " warning(s)\n";
        return found.errors || (options.strict && found.warnings) ? 1 : 0;
    }
    const auto entries = set->entries();
    // overrides= patterns no entry of the mod (used or shadowed) falls under.
    for (const auto& mod : set->mods()) {
        if (const auto volume = set->volume(mod); volume != 100 || set->volume_chosen(mod))   // 132md13
            std::cout << "  " << mod.folder << ": audio volume " << volume << "%"
                      << (set->volume_chosen(mod) ? " (mod_volume." + mod.folder + "= in the settings)" : std::string(" (audio_volume=)")) << "\n";
        for (const auto& pattern : mod.manifest.overrides) {
            mods::Manifest one;
            one.overrides = {pattern};
            const bool used = std::any_of(entries.begin(), entries.end(), [&](const mods::ModSet::Entry& e) {
                if (e.source.mod != &mod && std::find(e.shadowed.begin(), e.shadowed.end(), mod.folder) == e.shadowed.end()) return false;
                return mods::manifest_declares(one, e.kind == "by-hash" ? "BY-HASH/" + mods::upper(e.key) : e.key);
            });
            if (!used) found.warning(mod.folder + ": overrides= " + pattern + " matches nothing the folder holds");
        }
    }

    Disc disc(options.iso);
    OriginalTextures originals{*disc.image, {}, {}, {}};
    std::map<std::string, std::vector<mods::CapItem>> levels;   // "" : COMMON, by-hash and anything outside a level
    std::vector<ImageUse> images;
    std::size_t textures = 0, hashed = 0, files = 0, models = 0, sounds = 0, movies = 0, data = 0;
    std::size_t gpu = 0, cpu = 0, model_images = 0;
    std::uint64_t model_image_bytes = 0;
    std::map<std::string, SkeletonDefinition> skeletons;
    std::optional<std::vector<DiscSound>> catalogue;   // 132md13: the disc's sounds, for the audio_overrides= targets
    std::cout << "Checking " << entries.size() << " entries against the disc\n";
    for (const auto& entry : entries) {
        if (!options.match.empty() && entry.key.find(mods::upper(options.match)) == std::string::npos) continue;
        const auto& folder = entry.source.mod->folder;
        const auto label = (entry.kind == "by-hash"  ? "textures/by-hash/"
                            : entry.kind == "audio"  ? "audio_overrides "
                            : entry.kind == "movies" ? "movie_overrides "
                                                     : entry.kind + "/") +
                           entry.key + " <- " + folder;
        if (entry.kind == "textures" || entry.kind == "by-hash") {
            entry.kind == "by-hash" ? ++hashed : ++textures;
            mods::RgbaImage image;
            std::uint32_t block_bytes = 0;  // 132md11: a BC1 / BC3 DDS stays blocks in memory
            try {
                const auto bytes = mods::read_image_bytes(entry.source.path);
                image = mods::decode_image(bytes);
                block_bytes = mods::image_size(bytes).block_bytes;
            } catch (const std::exception& e) {
                found.error(label + ": not a readable PNG / DDS (" + e.what() + "); the disc's texture is drawn");
                continue;
            }
            const auto w = image.width, h = image.height;
            image = {};
            if (std::max(w, h) > pc_max_side) {
                found.error(label + ": " + size_text(w, h) + " is past the PC renderer's " + std::to_string(pc_max_side) + " px texture side");
                continue;
            }
            if (std::max(w, h) > vita_max_side)
                found.warning(label + ": " + size_text(w, h) + " is past the Vita's " + std::to_string(vita_max_side) + " px texture side");
            mods::CapItem item{w, h, 1, 1, block_bytes};
            std::string level;
            if (entry.kind == "textures") {
                const auto frame = texture_frame(entry.key);
                std::vector<std::pair<std::uint32_t, std::uint32_t>> frames;
                try {
                    if (!frame) throw std::runtime_error("not a texture key");
                    frames = originals.frames(frame->first);
                } catch (const std::exception& e) {
                    found.warning(label + ": unused, replaces nothing (" + e.what() + ")");
                    continue;
                }
                if (frame->second >= frames.size()) {
                    found.warning(label + ": unused, replaces nothing (the disc's " + frame->first + " has " +
                                  count_text(frames.size(), "frame(s)") + ")");
                    continue;
                }
                const auto [ow, oh] = frames[frame->second];
                item.min_width = ow;
                item.min_height = oh;
                if (std::uint64_t(w) * oh != std::uint64_t(h) * ow)
                    found.warning(label + ": " + size_text(w, h) + " is not the original's shape (" + size_text(ow, oh) + "); it is stretched over it");
                if (w < ow || h < oh) found.warning(label + ": " + size_text(w, h) + " is smaller than the original (" + size_text(ow, oh) + ")");
                const auto parts = split_path(entry.key);
                if (parts.size() > 2 && parts[0] == "GAMEDATA" && parts[1] != "COMMON") level = parts[1];
                // The menus decode a screen's images when it opens (no level batch): one row per screen, the
                // largest shown.
                if (level == "FRONTEND" && parts.size() > 3) level += "/" + parts[parts.size() > 4 ? 3 : 2];
            }
            levels[level].push_back(item);
            images.push_back({label, w, h, mods::replacement_bytes(w, h, block_bytes)});
        } else if (entry.kind == "models") {
            ++models;
            try {
                const auto check = check_model(*disc.image, entry, skeletons, options.skeleton);
                model_images += check.images;
                model_image_bytes += check.image_bytes;
                const bool cpu_part = !check.skinning.empty() && check.skinning != "GPU";
                if (!cpu_part)
                    std::cout << "  " << label << ": " << (check.skinning.empty() ? "static or hidden" : "skinned on the GPU") << "\n";
                for (const auto& note : check.notes) found.note(label + ": " + note);
                if (check.skinning == "GPU") ++gpu;
                else if (cpu_part) {
                    ++cpu;
                    found.note(label + ": skinned on the CPU (" + check.skinning.substr(5, check.skinning.size() - 6) + ")");
                }
            } catch (const std::exception& e) {
                found.error(label + ": refused (" + e.what() + "); the disc's model is drawn");
            }
        } else if (entry.kind == "sounds") {
            ++sounds;
            try {
                (void)check_sound(*disc.image, entry);
            } catch (const std::exception& e) {
                found.error(label + ": refused (" + e.what() + ")");
            }
        } else if (entry.kind == "audio") {   // 132md13
            ++sounds;
            try {
                if (!catalogue) catalogue = disc_sounds(*disc.image, target_hogs(*set));
                const auto check = check_audio_override(*catalogue, *set, entry);
                std::cout << "  " << label << ": " << check.text << "\n";
                if (!check.used) found.warning(label + ": unused, every sound it names comes from another entry");
                for (const auto& note : check.notes) found.note(label + ": " + note);
            } catch (const std::exception& e) {
                found.error(label + ": refused (" + e.what() + ")");
            }
        } else if (entry.kind == "movies") {   // 132md14
            ++movies;
            try {
                const auto check = check_movie_override(*disc.image, *set, entry);
                std::cout << "  " << label << ": " << check.text << "\n";
                for (const auto& w : check.warnings) found.warning(label + ": " + w);
                for (const auto& n : check.notes) found.note(label + ": " + n);
                if (check.width > vita_screen_width || check.height > vita_screen_height)
                    found.warning(label + ": " + size_text(check.width, check.height) + " is past the Vita's " +
                                  size_text(vita_screen_width, vita_screen_height) + " screen (decoded in software there: " +
                                  "a smaller copy plays more smoothly)");
            } catch (const std::exception& e) {
                found.error(label + ": refused (" + e.what() + "); the disc's movie plays");
            }
        } else if (entry.kind == "data") {   // 132md16: checked below, file by file
            ++data;
        } else if (entry.kind == "files") {
            ++files;
            try {
                (void)locate(*disc.image, entry.key);
            } catch (const std::exception& e) {
                found.warning(label + ": unused, replaces nothing (" + e.what() + ")");
            }
        }
    }
    // 132md12: the wide backgrounds (MODDING.md "Wide backgrounds"): readable, wider than 4:3, within the sides.
    std::size_t wide_images = 0;
    for (const auto& [screen, wide] : set->wide_backgrounds()) {
        for (const auto& path : wide.images) {
            ++wide_images;
            const auto name = path.lexically_relative(wide.mod->root).generic_u8string();
            const auto label = "wide." + screen + " " + std::string(name.begin(), name.end()) + " <- " + wide.mod->folder;
            std::uint32_t w = 0, h = 0;
            try {
                const auto image = mods::decode_image(mods::read_image_bytes(path));
                w = image.width, h = image.height;
            } catch (const std::exception& e) {
                found.error(label + ": not a readable PNG / DDS (" + e.what() + "); the 4:3 screen's own side fill is drawn");
                continue;
            }
            std::cout << "  " << label << ": " << size_text(w, h) << "\n";
            if (std::uint64_t(w) * 3 <= std::uint64_t(h) * 4)
                found.warning(label + ": " + size_text(w, h) + " is not wider than 4:3; it is never drawn");
            if (std::max(w, h) > pc_max_side)
                found.error(label + ": " + size_text(w, h) + " is past the PC renderer's " + std::to_string(pc_max_side) + " px texture side");
            else if (std::max(w, h) > vita_max_side)
                found.warning(label + ": " + size_text(w, h) + " is past the Vita's " + std::to_string(vita_max_side) + " px texture side");
        }
    }
    if (wide_images) std::cout << "Checked " << wide_images << " wide background image(s) (MODDING.md \"Wide backgrounds\")\n";
    if (data) check_data_overrides(*disc.image, *set, found);
    std::cout << "Checked " << textures << " texture image(s), " << hashed << " by-hash image(s) (matched by content at run time; not "
              << "checked against the disc), " << models << " model(s) (" << gpu << " skinned on the GPU, " << cpu
              << " on the CPU), " << sounds << " sound(s), " << movies << " movie(s), " << files << " file(s), " << data
              << " data field(s)\n";

    // Texture memory per level: the images a level can bind (its own, COMMON's, every by-hash image), as uploaded.
    if (!images.empty()) {
        std::cout << "Texture memory per level (RGBA8 with mips as uploaded; COMMON and by-hash images count in every level):\n";
        char line[256];
        std::snprintf(line, sizeof line, "  %-12s %7s %11s  %-30s %8s %8s\n", "level", "images", "PC (no cap)",
                      "Vita (64 MB cap)", "CDRAM", "RAM");
        std::cout << line;
        std::snprintf(line, sizeof line, "  %-12s %7s %11s  %-30s %8s %8s\n", "", "", "", "", "128 MB", "512 MB");
        std::cout << line;
        const auto& shared = levels[""];
        // Each level, and the front end's largest screen (by its images' size as uploaded on the Vita).
        std::vector<std::pair<std::string, std::string>> rows;   // (row name, levels key)
        std::string screen;
        std::uint64_t screen_bytes = 0;
        std::size_t screens = 0;
        for (const auto& [name, items] : levels) {
            if (name.empty()) continue;
            if (!name.starts_with("FRONTEND/")) {
                rows.push_back({name, name});
                continue;
            }
            ++screens;
            auto all = shared;
            all.insert(all.end(), items.begin(), items.end());
            const auto bytes = mods::plan_texture_cap(all, std::uint64_t(vita_cap_mb) << 20).planned_bytes;
            if (screen.empty() || bytes > screen_bytes) screen = name, screen_bytes = bytes;
        }
        if (!screen.empty()) rows.push_back({"FRONTEND*", screen});
        if (rows.empty()) rows.push_back({"(any level)", ""});
        for (const auto& [row, name] : rows) {
            auto items = shared;
            if (!name.empty()) items.insert(items.end(), levels[name].begin(), levels[name].end());
            const auto plan = mods::plan_texture_cap(items, std::uint64_t(vita_cap_mb) << 20);
            const auto vita = !plan.fits     ? "originals' sizes: " + megabytes(plan.planned_bytes)
                              : plan.limit == 0 ? "full size: " + megabytes(plan.planned_bytes)
                                                : "at most " + std::to_string(plan.limit) + " px: " + megabytes(plan.planned_bytes);
            const auto percent = [&](std::uint32_t budget_mb) {
                return std::to_string((plan.planned_bytes * 100 + (std::uint64_t(budget_mb) << 20) - 1) / (std::uint64_t(budget_mb) << 20)) + "%";
            };
            std::snprintf(line, sizeof line, "  %-12s %7zu %11s  %-30s %8s %8s\n", row.c_str(), items.size(),
                          megabytes(plan.full_bytes).c_str(), vita.c_str(), percent(vita_cdram_mb).c_str(),
                          percent(vita_ram_mb).c_str());
            std::cout << line;
            const auto who = name.empty() ? std::string("the images") : name + "'s images";
            if (!plan.fits)
                found.warning(who + " take " + megabytes(plan.planned_bytes) + " on the Vita even at their originals' sizes, over its " +
                              std::to_string(vita_cap_mb) + " MB cap");
            else if (plan.limit)
                found.note(who + " are scaled on the Vita to at most " + std::to_string(plan.limit) + " px a side (" +
                           megabytes(plan.full_bytes) + " at full size)");
        }
        if (!screen.empty())
            std::cout << "  * the front end's largest of " << screens << " screen(s): " << screen.substr(9)
                      << " (the menus decode a screen's images when it opens; the cap there is a running total)\n";
        std::sort(images.begin(), images.end(), [](const ImageUse& a, const ImageUse& b) { return a.bytes > b.bytes; });
        std::cout << "Largest images:\n";
        for (std::size_t i = 0; i < std::min<std::size_t>(5, images.size()); ++i)
            std::cout << "  " << images[i].label << ": " << size_text(images[i].width, images[i].height) << ", " << megabytes(images[i].bytes) << "\n";
    }
    if (model_images)
        std::cout << "Model images (glTF materials; not in the table: under a texture memory cap they are halved by the running "
                  << "total, never below the part's original texture): " << model_images << ", " << megabytes(model_image_bytes) << "\n";
    std::cout << "validate: " << found.errors << " error(s), " << found.warnings << " warning(s), " << found.notes << " note(s)\n";
    return found.errors || (options.strict && found.warnings) ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        Options options;
        if (argc < 2) usage("no command");
        options.command = argv[1];
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            const auto value = [&]() -> std::string {
                if (++i >= argc) usage(arg + " needs a value");
                return argv[i];
            };
            if (arg == "--iso") options.iso = value();
            else if (arg == "--out") options.out = value();
            else if (arg == "--mods") options.mods = value();
            else if (arg == "--match") options.match = value();
            else if (arg == "--by-hash") options.by_hash = true;
            else if (arg == "--skeleton") options.skeleton = value();
            else if (arg == "--settings") options.settings = value();
            else if (arg == "--strict") options.strict = true;
            else if (arg == "--platform") options.platform = value();
            else if (arg == "--max-size") {
                const auto text = value();
                try {
                    options.max_size = static_cast<std::uint32_t>(std::stoul(text));
                } catch (const std::exception&) {
                    usage("--max-size needs a number of pixels");
                }
                if (options.max_size < 4 || options.max_size > 4096) usage("--max-size is 4..4096 (GXM's texture side)");
            }
            else if (arg.starts_with("--")) usage("unknown option " + arg);
            else options.paths.push_back(arg);
        }
        if (options.command == "list") return list_mods(options);
        if (options.command == "check-models") return check_models(options);
        if (options.command == "conflicts") return list_conflicts(options);
        if (options.command == "enable" || options.command == "disable") {
            if (options.paths.empty()) usage("no mod folder");
            return choose_mods(options, options.command == "enable");
        }
        if (options.command == "check-sounds") return check_sounds(options);
        if (options.command == "volume") return set_volume(options);
        if (options.command == "report") return mod_report(options);
        if (options.command == "validate") return validate_mods(options);
        if (options.command == "move") return move_mod(options);
        if (options.command == "pack") {
            if (options.paths.empty()) usage("no mod folder");
            return pack_mods(options);
        }
        if (options.command == "declare") {
            if (options.paths.empty()) usage("no mod folder");
            return declare_overrides(options);
        }
        if (options.paths.empty()) usage("no disc path");
        if (options.command == "members") return list_members(options);
        if (options.out.empty()) usage("--out <mod folder> is required");
        if (options.command == "export-textures") return export_textures(options);
        if (options.command == "export-files") return export_files(options);
        if (options.command == "export-model") return export_model(options);
        if (options.command == "export-sounds") return export_sounds(options);
        if (options.command == "decode-audio") return decode_audio(options);
        usage("unknown command " + options.command);
    } catch (const std::exception& e) {
        std::cerr << "omega_mod_tool: " << e.what() << "\n";
        return 1;
    }
}
