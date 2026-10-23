#pragma once
// Port-only (checkpoint 132md1, research/MODDING.md): the mod / asset override layer. Not game code: the PS2 game reads
// only its disc. A mod is a folder under the mods folder (default `mods\` next to the game, interactive runs only) with
// a manifest.txt and any of
//   files\<disc path>[\<HOG member>[\<nested HOG member>]]   a replacement of that disc file or archive member,
//   textures\<disc path>\<HOG member>[...]\<NAME>.TDX.png     a replacement image of that texture (.png or .dds; frame
//                                                             n > 0 of an animated TDX: <NAME>.TDX.<n>.png),
//   textures\by-hash\<16 hex digits>.png                      a replacement image of every texture whose decoded
//                                                             frame has that content hash (texture_content_hash).
//   models\<disc path>\<HOG member>[...].gltf (or .glb)        a replacement model (checkpoint 132md2, research/
//                                                             MODDING.md "Stage 3"): keyed by the original member
//                                                             with .gltf / .glb removed (its .bin / images beside it).
//   sounds\<disc path>\<HOG member>.wav (or .ogg)             a replacement of that sound stream (checkpoint 132md4:
//                                                             a VAG / VPK member; research/MODDING.md "Sounds and
//                                                             music"), keyed by the member with .wav / .ogg removed.
// Asset keys are the disc's names, upper case, joined with '/': "GAMEDATA/COMMON/MANTEX.HOG/GABE.TDX". Resolution is
// deterministic: the enabled mods ordered by priority (highest first), then folder name (ordinal, case-insensitive);
// the first mod with a key wins; anything no mod supplies comes from the user's disc. With no mods folder, no enabled
// mod, the setting mods=0 or --no-mods, nothing is installed and every read is the disc's (bit-identical).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega::mods {

// Checkpoint 132md16 (research/MODDING.md "Gameplay data overrides", data_overrides.hpp): one field a mod's
// data_overrides= file sets. table "weapon" (selector: a record id in decimal or its display key, upper case), "npc"
// ("<LEVEL> <id>", "<LEVEL> PLAYER", "<LEVEL> *", "* *"; LEVEL upper case) or "tohit" (no selector).
struct DataOverride {
    std::string table, selector, field;   // field: data_fields' name
    double value = 0;
    std::string file;                     // the file inside the mod's folder ('/'-joined)
    std::size_t line = 0;
};

struct Manifest {
    std::string name, version, author, description;
    int priority = 0;
    bool enabled = true;
    // Checkpoint 132md4: overrides= (comma-separated asset key patterns, upper case, '/'-joined): the assets the mod
    // declares it replaces. Empty: undeclared (everything the folder holds is used). A pattern is a key ("GAMEDATA/
    // COMMON/MANTEX.HOG": that key and everything below it) or a prefix ending in '*' ("GAMEDATA/TORONTO1/*").
    // by-hash textures are "BY-HASH/<16 hex digits>" (or "BY-HASH").
    std::vector<std::string> overrides;
    // Checkpoint 132md10 (research/MODDING.md "Mod archives"): game_version= (the disc the mod was made for; this port
    // reads SCUS-97264, game_version_text) and notes.<platform>= lines (pc, vita, ...: what the author says about that
    // platform; keys lower case). The load order is priority= (highest first).
    std::string game_version;
    std::map<std::string, std::string, std::less<>> notes;
    // Checkpoint 132md11 (research/MODDING.md "Mods on the Vita"): platform= (lower case), the platform a pack was
    // converted for (omega_mod_tool pack --platform vita writes "vita"); empty: any. Shown, never enforced: a Vita pack's
    // BC1 / BC3 textures load everywhere (decoded where the device does not sample them).
    std::string platform;
    // Checkpoint 132md12 (research/MODDING.md "Wide backgrounds"): wide.<screen>= lines, the native wide images (paths
    // inside the mod's folder, comma-separated, .png / .dds: e.g. a 16:9 and a 21:9 one) the wide aspect draws behind a
    // screen, keyed by the screen (lower case): loading, loading.<level> (that level's loading screen), frontend,
    // frontend.<screen> (a front-end screen, by the name the front end's log gives it). Not asset keys: overrides= does
    // not cover them.
    std::map<std::string, std::vector<std::string>, std::less<>> wide;
    // Checkpoint 132md13 (research/MODDING.md "Audio overrides"): audio_overrides= lines, <target>=<file>[,<target>=
    // <file>...] (the one key that may repeat: the lines accumulate, in order). A target (upper case, '/'-joined) is a
    // stream member key ("ZMEDIA/MUSIC.HOG/THEME_MAIN.VPK"), a bank sample key ("<bank member key>/<n>"), a bank sound
    // name ("<bank member key>/<SOUND NAME>": every sample that sound plays), or the end of any of these at a '/'
    // ("THEME_MAIN.VPK", "WEPS.BNK/13", "ENVIRON.BNK/O_BANK_ALARM": every level's). The file is a .wav / .ogg inside
    // the mod's folder. Not asset keys of the folder: overrides= does not filter them. audio_volume=<percent> (0..400,
    // default 100): the gain on every sound of the mod (sounds\ and audio_overrides=).
    struct AudioOverride {
        std::string target, file;
    };
    std::vector<AudioOverride> audio_overrides;
    unsigned audio_volume = 100;
    // Checkpoint 132md14 (research/MODDING.md "Movie overrides"): movie_overrides= lines, <MOVIE>=<file>[+<file>...]
    // [,<MOVIE>=...] (repeatable, as audio_overrides=). MOVIE is a ZMOVIES.HOG movie ("INTRO" or "INTRO.PSS"; kept
    // upper case with .PSS). The files (inside the mod's folder), one of each kind at most: the picture (.m2v / .mpv:
    // an MPEG-2 video elementary stream; .pss: a movie in the disc's own format, its sound included; checkpoint
    // 132md15: .mp4 / .m4v / .mov, H.264 + AAC through the platform's decoder, platform_movie.hpp), the sound
    // (.wav / .ogg) and the subtitles (.sub: the disc's .SUB text format). A kind not given is the disc movie's.
    struct MovieOverride {
        std::string target, video, audio, subtitles;
    };
    std::vector<MovieOverride> movie_overrides;
    // Checkpoint 132md16 (research/MODDING.md "Gameplay data overrides"): data_overrides=<file>[,<file>...], INI files
    // inside the mod's folder (.ini) naming tuning-table fields (data_overrides.hpp). Not asset keys: overrides= does
    // not filter them. A mod with any is gameplay-changing (mod_fingerprint.hpp).
    std::vector<std::string> data_overrides;
};
// The disc this port reads (Syphon Filter: The Omega Strain, USA).
inline constexpr std::string_view game_version_text = "SCUS-97264";
// True when "version" is empty or names game_version_text (case, '-', '_' and '.' ignored: "scus_972.64" matches).
bool game_version_matches(std::string_view version);
// The platform the build runs on, as the notes.<platform>= keys name it ("pc"; 132md11: "vita" in a Vita build).
std::string_view current_platform();
// manifest.txt: "key=value" lines (name, version, author, description, priority = an integer, enabled = 0|1|true|
// false, overrides = patterns, game_version, notes.<platform>, platform, wide.<screen>, audio_overrides, audio_volume,
// movie_overrides, data_overrides); blank lines and '#' comments ignored; unknown keys ignored (later fields). name is required. Throws
// on a malformed line, a repeated key (audio_overrides and movie_overrides may repeat), or a bad priority / enabled /
// overrides / wide / audio_overrides / audio_volume / movie_overrides / data_overrides value.
Manifest parse_manifest(std::string_view text);
// Checkpoint 132md13: how an audio_overrides= target names a sound. `key` is a stream member key or a bank sample key
// (<bank member key>/<n>); `names` the sounds that play that bank sample (BankSample::sounds; any case). The target
// matches when it equals, or is a '/'-bounded end of, the key (2) or <bank member key>/<NAME> for one of the names (1);
// 0: no match.
int audio_target_match(std::string_view target, std::string_view key, std::span<const std::string> names = {});
// Checkpoint 132md4: whether the manifest's overrides= covers an asset key (always, with no overrides= line).
// Checkpoint 132md12: on the stem. A pattern (or key) naming a mod file is taken as the asset key it supplies: a leading
// "TEXTURES/", "MODELS/", "SOUNDS/" or "FILES/" folder and a trailing ".PNG" / ".DDS" / ".GLTF" / ".GLB" / ".WAV" /
// ".OGG" are dropped (case-insensitive), so "textures/GAMEDATA/COMMON/MANTEX.HOG/GABE.TDX.png" covers GABE.TDX
// whether the mod holds GABE.TDX.png or GABE.TDX.dds (omega_mod_tool pack --platform vita's conversion).
bool manifest_declares(const Manifest& manifest, std::string_view key);
// The asset key a pattern or mod file path names (the stem rule above), upper case.
std::string override_stem(std::string_view pattern);

struct Mod {
    std::string folder;               // the mod's folder name (an archive's: its file name without .zip)
    std::filesystem::path root;       // its absolute folder (an archive's: the .zip path, or <zip>\<top folder>)
    Manifest manifest;
    std::filesystem::path archive;    // checkpoint 132md10: the .zip a packed mod is read from (empty: a folder)
};

// Checkpoint 132md4: the player's choices over the manifests' enabled= (the port settings' mod.<folder>=0|1, keyed
// by the folder name upper-cased): a choice wins over the manifest.
using ModChoices = std::map<std::string, bool, std::less<>>;
// Checkpoint 132md13: the player's audio volumes over the manifests' audio_volume= (the port settings'
// mod_volume.<folder>=<percent>, keyed by the folder name upper-cased).
using ModVolumes = std::map<std::string, unsigned, std::less<>>;

// Checkpoint 132md10: a mod is also a .zip archive (mod_archive.hpp) holding the folder's contents at its root or in
// one top folder; its folder name is the file name without .zip. A folder and an archive of the same name: the folder
// is used (scan's messages / the listing's problem say so).
// Checkpoint 132md4: every mod folder of a mods folder, enabled or not (the in-game mod list, omega_mod_tool list):
// valid manifests first in resolution order (priority, highest first, then folder name), then the invalid ones by
// folder name.
struct ModListing {
    std::string folder;
    std::filesystem::path root;
    std::optional<Manifest> manifest;   // nullopt: no or an invalid manifest.txt (problem says which)
    std::string problem;
    bool enabled = false;               // after the player's choice
    bool chosen = false;                // a ModChoices entry decided enabled
    std::filesystem::path archive;      // checkpoint 132md10: the .zip (empty: a folder)
};
std::vector<ModListing> list_mods(const std::filesystem::path& folder, const ModChoices& choices = {});

// The upper-case '/'-joined asset key of a disc path (and member).
std::string asset_key(const std::vector<std::string>& components);
std::string member_key(std::string_view archive_key, std::string_view member);
std::string upper(std::string_view text);
// 16 lower-case hex digits.
std::string hash_text(std::uint64_t hash);

class ModSet {
public:
    struct Source {
        std::filesystem::path path;   // the mod file
        const Mod* mod{};             // the mod supplying it
    };
    // Scans `folder` (each sub-folder with a manifest.txt is a mod). Folders without a manifest, disabled mods, and
    // mods whose manifest or files are invalid are skipped with a message in `messages`. `choices` (132md4) enable or
    // disable mods over their manifests. A mod's entries outside its manifest's overrides= are ignored (a message).
    static std::unique_ptr<ModSet> scan(const std::filesystem::path& folder, std::vector<std::string>& messages,
                                        const ModChoices& choices = {});
    // A set from explicit mods (tests): each mod's files are indexed as scan does.
    static std::unique_ptr<ModSet> from_mods(std::vector<Mod> mods, std::vector<std::string>& messages);

    const std::vector<Mod>& mods() const { return mods_; }   // the enabled mods in resolution order
    bool empty() const {
        return files_.empty() && textures_.empty() && hashes_.empty() && models_.empty() && sounds_.empty() && wide_.empty() &&
               audio_.empty() && movies_.empty() && data_.empty();
    }

    std::optional<Source> file(std::string_view key) const;
    // A replacement image for a texture key (frame 0: "<...>.TDX"; frame n: "<...>.TDX.<n>").
    std::optional<Source> texture(std::string_view key) const;
    std::optional<Source> texture_hash(std::uint64_t hash) const;
    // A replacement glTF for a model key (the original member's key: "GAMEDATA/COMMON/MANSKM.HOG/<PART>.SKM").
    std::optional<Source> model(std::string_view key) const;
    bool has_models() const { return !models_.empty(); }
    // Checkpoint 132md4: a replacement sound file (.wav / .ogg) for a stream member key
    // ("ZMEDIA/MUSIC.HOG/BELARUS1_ACT.VPK", "GAMEDATA/TORONTO1/SNDVAG.HOG/1_2_1.VAG").
    std::optional<Source> sound(std::string_view key) const;
    // Checkpoint 132md13 (research/MODDING.md "Audio overrides"): the sound a stream member key or a bank sample key
    // (with `names`, the sounds that play the sample) is replaced by: the first mod in resolution order supplying one,
    // and within it its sounds\ file for the key, else its audio_overrides= target naming the key (a sample number over
    // a sound name, then the longer target, then the manifest's order). gain: the mod's volume (1 at 100%).
    struct AudioSource {
        std::filesystem::path path;
        const Mod* mod{};
        float gain = 1;
        std::string via;   // "sounds/<key>" or "audio_overrides <target>"
    };
    std::optional<AudioSource> audio(std::string_view key, std::span<const std::string> names = {}) const;
    // Checkpoint 132md14 (research/MODDING.md "Movie overrides"): a movie's replacement (movie_overrides=), by its
    // ZMOVIES.HOG name ("INTRO.PSS", any case): the first mod in resolution order naming it supplies the whole item
    // (an empty path: that part stays the disc's). gain: the mod's audio volume (its .wav / .ogg / .pss sound).
    struct MovieSource {
        std::string target;   // "INTRO.PSS"
        std::filesystem::path video, audio, subtitles;
        const Mod* mod{};
        float gain = 1;
    };
    std::optional<MovieSource> movie(std::string_view name) const;
    bool has_movies() const { return !movies_.empty(); }
    // Checkpoint 132md16 (research/MODDING.md "Gameplay data overrides"): the data_overrides= fields, one per
    // (table, selector, field), the first mod in resolution order winning (the others shadowed), in resolution then
    // file order.
    struct DataSource {
        DataOverride item;
        Source source;   // the data_overrides= file and its mod
        std::vector<std::string> shadowed;
    };
    const std::vector<DataSource>& data() const { return data_; }
    bool has_data() const { return !data_.empty(); }
    // True when the data overrides patch that disc file key (WEAPONS.WDB, a level's DATA.POP).
    bool has_data(std::string_view key) const;
    // sounds\ entries or audio_overrides= targets.
    bool has_sounds() const { return !sounds_.empty() || !audio_.empty(); }
    // A mod's audio volume in percent: the player's (set_volumes) over its manifest's audio_volume=.
    unsigned volume(const Mod& mod) const;
    // The player's volumes (mod_volume.<folder>=; before the set is installed).
    void set_volumes(const ModVolumes& volumes) { volumes_ = volumes; }
    bool volume_chosen(const Mod& mod) const;
    bool has_files() const { return !files_.empty(); }
    // True when a files\ entry lies strictly below `key` (a member of that archive is replaced).
    bool has_files_below(std::string_view key) const;
    // True when a textures\ path entry lies strictly below `key`.
    bool has_textures_below(std::string_view key) const;
    bool has_textures() const { return !textures_.empty() || !hashes_.empty(); }
    // True when a textures\ entry names `key` itself (frame 0 or any animation frame).
    bool has_texture(std::string_view key) const { return texture_bases_.contains(key); }
    // The texture keys strictly below `key` (frame suffix included).
    std::vector<std::string> textures_below(std::string_view key) const;
    // The distinct next path components of the files\ keys strictly below `key` (the archive members a mod replaces).
    std::vector<std::string> file_children(std::string_view key) const;
    // Every indexed entry, "files" / "textures" / "by-hash" / "models" / "sounds", in key order (the mod tool's listing);
    // 132md13: then "audio" (the audio_overrides= targets as written, in resolution then manifest order; shadowed: the
    // later mods with the same target); 132md14: then "movies" (by movie name; the source path is the picture, else the
    // sound, else the subtitles); 132md16: then "data" (data_entry_key: "weapon SOCOMSILENCED damage_near"; the source
// path is the data_overrides= file).
    struct Entry {
        std::string kind, key;
        Source source;
        std::vector<std::string> shadowed;   // lower-priority mods with the same key
    };
    std::vector<Entry> entries() const;
    // Checkpoint 132md4 (omega_mod_tool conflicts): where enabled mods meet. "shadowed": the same entry from several
    // mods (the first in resolution order wins, the rest are unused). "layered": a mod's files\ replacement of an
    // archive (or file) with another mod's entry below it, or a texture / model of the same key: both apply (the
    // member replacement over the replaced archive, the image / model over the replaced member).
    struct Conflict {
        std::string kind;     // "shadowed" | "layered"
        std::string key;      // "files/GAMEDATA/..." (the winner's entry)
        std::string winner;   // the folder whose entry is used (shadowed) / the archive's replacer (layered)
        std::vector<std::string> others;
        std::string detail;
    };
    std::vector<Conflict> conflicts() const;
    // Checkpoint 132md12 (research/MODDING.md "Wide backgrounds"): the wide backgrounds of the manifests' wide.<screen>=
    // lines, per screen key, the first mod in resolution order naming a key winning (the others shadowed); images whose
    // file the mod lacks are dropped at the scan (a message). Not entries(): they replace no asset.
    struct WideBackground {
        std::string screen;                          // the key ("loading.toronto1")
        std::vector<std::filesystem::path> images;   // in the manifest's order
        const Mod* mod{};
        std::vector<std::string> shadowed;
    };
    const std::map<std::string, WideBackground, std::less<>>& wide_backgrounds() const { return wide_; }
    // The wide background for a screen key (case-insensitive): the key's own, else (for "kind.name") its kind's
    // ("loading" for "loading.toronto1"); nullptr: none.
    const WideBackground* wide_background(std::string_view screen) const;

private:
    std::vector<Mod> mods_;
    struct Indexed {
        Source source;
        std::vector<std::string> shadowed;
    };
    std::map<std::string, Indexed, std::less<>> files_, textures_, models_, sounds_;
    std::map<std::uint64_t, Indexed> hashes_;
    std::map<std::string, WideBackground, std::less<>> wide_;
    std::set<std::string, std::less<>> file_parents_, texture_parents_, texture_bases_;
    struct IndexedAudio {
        std::string target;
        Source source;
        std::vector<std::string> shadowed;
    };
    std::vector<IndexedAudio> audio_;   // 132md13: resolution order, then each manifest's order
    struct IndexedMovie {
        MovieSource source;
        std::vector<std::string> shadowed;
    };
    std::map<std::string, IndexedMovie, std::less<>> movies_;   // 132md14
    std::vector<DataSource> data_;                               // 132md16
    std::set<std::string, std::less<>> data_files_;              // the data file keys patched (a level's DATA.POP)
    bool data_every_pop_ = false;                                // an [npc * *] section: every level's DATA.POP
    ModVolumes volumes_;
    void index(std::vector<std::string>& messages);
};

// The process-wide override layer the disc reader (disc.hpp DiscImage) consults. install(nullptr) removes it.
// `log` receives one line per first use of each override ("Mods (port-only): ..."); default std::cout.
// Checkpoint 132md15 (research/MODDING.md "Mod changes at the next level"): install may run again between levels; the
// set it replaces is kept to the process's end, so a pointer from active() (or a Source::mod) never dangles.
void install(std::shared_ptr<const ModSet> set, std::function<void(const std::string&)> log = {});
const ModSet* active();
// Checkpoint 132md15: what a level-boundary install compares: the enabled mods in order (folder, place, volume), every
// entry (kind, key, winner, file, shadowed mods), the movies' and wide backgrounds' files, and the size and time of
// each manifest, archive and folder-mod file. Equal signatures: the same set. "" for null or an empty set.
std::string signature(const ModSet* set);
// Incremented by every install (the disc layer's resolution cache is per generation).
std::uint64_t generation();
// One log line per distinct `what` (thread-safe); a no-op with no set installed.
void note(const std::string& what);
// The lines noted so far (tests).
std::vector<std::string> noted();

// Reads a whole file.
std::vector<std::uint8_t> read_file(const std::filesystem::path& path);

// HOG archives (the disc's container: research/FINDINGS.md): the member names in index order and their bytes.
struct HogMember {
    std::string name;
    std::size_t offset{}, size{};   // within the archive image
};
std::vector<HogMember> hog_members(std::span<const std::uint8_t> archive);
// The archive with members replaced (name -> bytes; names compared exactly): the original index (header word 0, the
// member order and names) is kept, the offset table rewritten, a replaced member that is not the last zero-padded to
// the archive's member alignment (the largest power of two <= 2048 dividing every original member offset).
std::vector<std::uint8_t> rebuild_hog(std::span<const std::uint8_t> archive,
                                      const std::map<std::string, std::vector<std::uint8_t>>& replaced);

// The override resolution of one asset key: `original` is the disc's bytes (called only when needed). Applies, in
// order: the highest-priority files\ replacement of the key itself (else the original), then (for a HOG) every
// replaced member below it, recursively (a member key no longer in the resolved archive is logged and skipped).
// Returns nullopt when no files\ entry is at or below the key (the caller keeps the disc's extent).
std::optional<std::vector<std::uint8_t>> resolve_bytes(const ModSet& set, const std::string& key,
                                                       const std::function<std::vector<std::uint8_t>()>& original);

}  // namespace omega::mods
