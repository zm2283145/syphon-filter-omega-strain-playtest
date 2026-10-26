#include "mod_set.hpp"
#include "data_overrides.hpp"
#include "mod_archive.hpp"
#include "texture_replacement.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace omega::mods {
namespace {

std::string trim(std::string_view text) {
    std::size_t begin = 0, end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

std::uint32_t le32(std::span<const std::uint8_t> bytes, std::size_t at) {
    if (at > bytes.size() || bytes.size() - at < 4) throw std::runtime_error("HOG read past its end");
    return std::uint32_t(bytes[at]) | std::uint32_t(bytes[at + 1]) << 8 | std::uint32_t(bytes[at + 2]) << 16 |
           std::uint32_t(bytes[at + 3]) << 24;
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint64_t value) {
    if (value > 0xffffffffull) throw std::runtime_error("Rebuilt HOG exceeds 4 GiB");
    for (int i = 0; i < 4; ++i) bytes[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

// Paths a mod may not replace: the boot files (the port runs the disc's executable image for its data tables and
// derived data) and the movie archive (PSS movies are streamed straight from the disc; out of scope).
bool forbidden(const std::string& key) {
    return key == "SYSTEM.CNF" || key == "SCUS_972.64" || key == "ZMEDIA/ZMOVIES.HOG" ||
           key.starts_with("ZMEDIA/ZMOVIES.HOG/");
}

std::vector<std::string> split_key(std::string_view key) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= key.size()) {
        const auto slash = key.find('/', begin);
        const auto end = slash == std::string_view::npos ? key.size() : slash;
        parts.emplace_back(key.substr(begin, end - begin));
        if (slash == std::string_view::npos) break;
        begin = slash + 1;
    }
    return parts;
}

// The '/'-joined upper-case key of a path relative to a mod sub-folder, or nullopt (with a message) when a component
// is not plain printable ASCII.
std::optional<std::string> relative_key(const std::filesystem::path& relative, std::string& problem) {
    std::string key;
    for (const auto& part : relative) {
        const auto text = part.u8string();
        std::string component;
        for (const auto c : text) {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte >= 0x7f || byte == ':' || byte == '/' || byte == '\\') {
                problem = "a path component is not plain ASCII";
                return std::nullopt;
            }
            component.push_back(static_cast<char>(std::toupper(byte)));
        }
        if (component.empty() || component == "." || component == "..") {
            problem = "an empty or relative path component";
            return std::nullopt;
        }
        if (!key.empty()) key.push_back('/');
        key += component;
    }
    if (key.empty()) {
        problem = "an empty path";
        return std::nullopt;
    }
    return key;
}

// Checkpoint 132md10: an archive mod's members, or the folder's files.
std::vector<std::filesystem::path> files_below(const std::filesystem::path& folder) { return mod_files_below(folder); }

void add_parents(std::set<std::string, std::less<>>& parents, const std::string& key) {
    for (auto slash = key.find('/'); slash != std::string::npos; slash = key.find('/', slash + 1))
        parents.insert(key.substr(0, slash));
}

bool is_hex16(std::string_view text) {
    return text.size() == 16 &&
           std::all_of(text.begin(), text.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
}

struct Installed {
    std::mutex mutex;
    std::shared_ptr<const ModSet> set;
    std::function<void(const std::string&)> log;
    std::set<std::string> noted_set;
    std::vector<std::string> noted;
    // Checkpoint 132md15: the sets installed before, kept to the process's end (a level-boundary install replaces a set
    // whose raw pointers, active() and Source::mod, a thread may still hold).
    std::vector<std::shared_ptr<const ModSet>> retired;
};

Installed& installed() {
    static Installed state;
    return state;
}

std::atomic<const ModSet*> active_set{nullptr};
std::atomic<std::uint64_t> install_generation{0};

}  // namespace

std::string upper(std::string_view text) {
    std::string result(text);
    for (auto& c : result) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return result;
}

std::string asset_key(const std::vector<std::string>& components) {
    std::string key;
    for (const auto& component : components) {
        if (!key.empty()) key.push_back('/');
        key += upper(component);
    }
    return key;
}

std::string member_key(std::string_view archive_key, std::string_view member) {
    return std::string(archive_key) + "/" + upper(member);
}

std::string hash_text(std::uint64_t hash) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

Manifest parse_manifest(std::string_view text) {
    Manifest manifest;
    std::set<std::string> seen;
    std::size_t line_number = 0;
    for (std::size_t begin = 0; begin < text.size();) {
        auto end = text.find('\n', begin);
        if (end == std::string_view::npos) end = text.size();
        ++line_number;
        auto line = trim(text.substr(begin, end - begin));
        begin = end + 1;
        if (line_number == 1 && line.starts_with("\xEF\xBB\xBF")) line = trim(line.substr(3));
        if (line.empty() || line[0] == '#') continue;
        const auto equals = line.find('=');
        if (equals == std::string::npos)
            throw std::runtime_error("manifest line " + std::to_string(line_number) + ": expected key=value");
        const auto key = lower(trim(std::string_view(line).substr(0, equals)));
        const auto value = trim(std::string_view(line).substr(equals + 1));
        if (key.empty()) throw std::runtime_error("manifest line " + std::to_string(line_number) + ": empty key");
        if (!seen.insert(key).second && key != "audio_overrides" && key != "movie_overrides")
            throw std::runtime_error("manifest line " + std::to_string(line_number) + ": repeated key '" + key + "'");
        if (key == "name") manifest.name = value;
        else if (key == "version") manifest.version = value;
        else if (key == "author") manifest.author = value;
        else if (key == "description") manifest.description = value;
        else if (key == "game_version") manifest.game_version = value;
        else if (key == "platform") manifest.platform = lower(value);
        else if (key.starts_with("notes.") && key.size() > 6) manifest.notes[key.substr(6)] = value;
        else if (key.starts_with("wide.") && key.size() > 5) {
            // Checkpoint 132md12: wide.<screen>=<image>[,<image>...], paths inside the mod's folder.
            const auto where = "manifest line " + std::to_string(line_number) + ": ";
            std::vector<std::string> images;
            for (std::size_t at = 0; at <= value.size();) {
                auto comma = value.find(',', at);
                if (comma == std::string::npos) comma = value.size();
                auto path = trim(std::string_view(value).substr(at, comma - at));
                at = comma + 1;
                std::replace(path.begin(), path.end(), '\\', '/');
                if (path.empty()) continue;
                const auto extension = lower(std::string_view(path).substr(path.size() >= 4 ? path.size() - 4 : 0));
                if (extension != ".png" && extension != ".dds")
                    throw std::runtime_error(where + key + " images are .png or .dds ('" + path + "')");
                const auto parts = split_key(path);
                if (path.starts_with('/') || path.find(':') != std::string::npos ||
                    std::any_of(parts.begin(), parts.end(), [](const std::string& p) { return p.empty() || p == "." || p == ".."; }))
                    throw std::runtime_error(where + key + " images are paths inside the mod's folder ('" + path + "')");
                images.push_back(path);
            }
            if (images.empty()) throw std::runtime_error(where + key + " is empty");
            manifest.wide[key.substr(5)] = std::move(images);
        }
        else if (key == "audio_overrides") {
            // Checkpoint 132md13: <target>=<file>[,<target>=<file>...]; the lines accumulate.
            const auto where = "manifest line " + std::to_string(line_number) + ": audio_overrides ";
            std::size_t items = 0;
            for (std::size_t at = 0; at <= value.size();) {
                auto comma = value.find(',', at);
                if (comma == std::string::npos) comma = value.size();
                const auto item = trim(std::string_view(value).substr(at, comma - at));
                at = comma + 1;
                if (item.empty()) continue;
                const auto equals_at = item.find('=');
                if (equals_at == std::string::npos)
                    throw std::runtime_error(where + "items are <target>=<file> ('" + item + "')");
                auto target = upper(trim(std::string_view(item).substr(0, equals_at)));
                auto path = trim(std::string_view(item).substr(equals_at + 1));
                std::replace(target.begin(), target.end(), '\\', '/');
                std::replace(path.begin(), path.end(), '\\', '/');
                while (target.starts_with('/')) target.erase(0, 1);
                while (target.ends_with('/')) target.pop_back();
                const auto target_parts = split_key(target);
                if (target.empty() || target.find_first_of("*:") != std::string::npos ||
                    std::any_of(target_parts.begin(), target_parts.end(), [](const std::string& p) { return p.empty() || p == "." || p == ".."; }))
                    throw std::runtime_error(where + "target '" + target + "' is not a sound key, bank sound or key end");
                if (target.find('/') == std::string::npos &&
                    std::all_of(target.begin(), target.end(), [](char c) { return c >= '0' && c <= '9'; }))
                    throw std::runtime_error(where + "target '" + target +
                                             "' is a bare sample number (every bank's); name the bank: WEPS.BNK/" + target);
                const auto extension = lower(std::string_view(path).substr(path.size() >= 4 ? path.size() - 4 : 0));
                if (extension != ".wav" && extension != ".ogg")
                    throw std::runtime_error(where + "files are .wav or .ogg ('" + path + "')");
                const auto parts = split_key(path);
                if (path.starts_with('/') || path.find(':') != std::string::npos ||
                    std::any_of(parts.begin(), parts.end(), [](const std::string& p) { return p.empty() || p == "." || p == ".."; }))
                    throw std::runtime_error(where + "files are paths inside the mod's folder ('" + path + "')");
                manifest.audio_overrides.push_back({target, path});
                ++items;
            }
            if (!items) throw std::runtime_error(where + "is empty");
        } else if (key == "movie_overrides") {
            // Checkpoint 132md14: <MOVIE>=<file>[+<file>...][,<MOVIE>=...]; the lines accumulate.
            const auto where = "manifest line " + std::to_string(line_number) + ": movie_overrides ";
            std::size_t items = 0;
            for (std::size_t at = 0; at <= value.size();) {
                auto comma = value.find(',', at);
                if (comma == std::string::npos) comma = value.size();
                const auto item = trim(std::string_view(value).substr(at, comma - at));
                at = comma + 1;
                if (item.empty()) continue;
                const auto equals_at = item.find('=');
                if (equals_at == std::string::npos)
                    throw std::runtime_error(where + "items are <MOVIE>=<file>[+<file>...] ('" + item + "')");
                Manifest::MovieOverride movie;
                movie.target = upper(trim(std::string_view(item).substr(0, equals_at)));
                if (movie.target.find('.') == std::string::npos) movie.target += ".PSS";
                if (movie.target.size() <= 4 || !movie.target.ends_with(".PSS") ||
                    movie.target.find_first_of("/\\*:") != std::string::npos ||
                    movie.target.find('.') != movie.target.size() - 4)
                    throw std::runtime_error(where + "target '" + movie.target + "' is not a movie name (INTRO or INTRO.PSS)");
                if (std::any_of(manifest.movie_overrides.begin(), manifest.movie_overrides.end(),
                                [&](const Manifest::MovieOverride& m) { return m.target == movie.target; }))
                    throw std::runtime_error(where + movie.target + " is given twice");
                const auto files = std::string_view(item).substr(equals_at + 1);
                for (std::size_t from = 0; from <= files.size();) {
                    auto plus = files.find('+', from);
                    if (plus == std::string_view::npos) plus = files.size();
                    auto path = trim(files.substr(from, plus - from));
                    from = plus + 1;
                    std::replace(path.begin(), path.end(), '\\', '/');
                    if (path.empty()) continue;
                    const auto extension = lower(std::string_view(path).substr(path.size() >= 4 ? path.size() - 4 : 0));
                    std::string* slot = extension == ".m2v" || extension == ".mpv" || extension == ".pss" || extension == ".mp4" ||
                                                extension == ".m4v" || extension == ".mov"
                                            ? &movie.video
                                        : extension == ".wav" || extension == ".ogg"                       ? &movie.audio
                                        : extension == ".sub"                                              ? &movie.subtitles
                                                                                                           : nullptr;
                    if (!slot)
                        throw std::runtime_error(where + "files are .m2v / .mpv / .pss / .mp4 / .m4v / .mov (picture), .wav / .ogg (sound) or .sub ('" +
                                                 path + "')");
                    const auto parts = split_key(path);
                    if (path.starts_with('/') || path.find(':') != std::string::npos ||
                        std::any_of(parts.begin(), parts.end(), [](const std::string& p) { return p.empty() || p == "." || p == ".."; }))
                        throw std::runtime_error(where + "files are paths inside the mod's folder ('" + path + "')");
                    if (!slot->empty())
                        throw std::runtime_error(where + movie.target + " has two " +
                                                 (slot == &movie.video ? "pictures" : slot == &movie.audio ? "sounds" : "subtitle files"));
                    *slot = path;
                }
                if (movie.video.empty() && movie.audio.empty() && movie.subtitles.empty())
                    throw std::runtime_error(where + movie.target + " names no file");
                manifest.movie_overrides.push_back(std::move(movie));
                ++items;
            }
            if (!items) throw std::runtime_error(where + "is empty");
        } else if (key == "data_overrides") {
            // Checkpoint 132md16: <file>[,<file>...], .ini files inside the mod's folder.
            const auto where = "manifest line " + std::to_string(line_number) + ": data_overrides ";
            for (std::size_t at = 0; at <= value.size();) {
                auto comma = value.find(',', at);
                if (comma == std::string::npos) comma = value.size();
                auto path = trim(std::string_view(value).substr(at, comma - at));
                at = comma + 1;
                std::replace(path.begin(), path.end(), '\\', '/');
                if (path.empty()) continue;
                if (lower(std::string_view(path).substr(path.size() >= 4 ? path.size() - 4 : 0)) != ".ini")
                    throw std::runtime_error(where + "files are .ini ('" + path + "')");
                const auto parts = split_key(path);
                if (path.starts_with('/') || path.find(':') != std::string::npos ||
                    std::any_of(parts.begin(), parts.end(), [](const std::string& p) { return p.empty() || p == "." || p == ".."; }))
                    throw std::runtime_error(where + "files are paths inside the mod's folder ('" + path + "')");
                if (std::find(manifest.data_overrides.begin(), manifest.data_overrides.end(), path) != manifest.data_overrides.end())
                    throw std::runtime_error(where + path + " is given twice");
                manifest.data_overrides.push_back(path);
            }
            if (manifest.data_overrides.empty()) throw std::runtime_error(where + "is empty");
        } else if (key == "audio_volume") {
            unsigned percent = 0;
            const auto [ptr, error] = std::from_chars(value.data(), value.data() + value.size(), percent);
            if (value.empty() || error != std::errc{} || ptr != value.data() + value.size() || percent > 400)
                throw std::runtime_error("manifest line " + std::to_string(line_number) + ": audio_volume is a percent, 0..400");
            manifest.audio_volume = percent;
        }
        else if (key == "priority") {
            int priority = 0;
            const auto* first = value.data();
            const auto* last = value.data() + value.size();
            if (!value.empty() && *first == '+') ++first;
            const auto [ptr, error] = std::from_chars(first, last, priority);
            if (value.empty() || error != std::errc{} || ptr != last)
                throw std::runtime_error("manifest line " + std::to_string(line_number) + ": priority must be an integer");
            manifest.priority = priority;
        } else if (key == "enabled") {
            const auto v = lower(value);
            if (v == "1" || v == "true" || v == "yes") manifest.enabled = true;
            else if (v == "0" || v == "false" || v == "no") manifest.enabled = false;
            else throw std::runtime_error("manifest line " + std::to_string(line_number) + ": enabled must be 0 or 1");
        } else if (key == "overrides") {
            for (std::size_t at = 0; at <= value.size();) {
                auto comma = value.find(',', at);
                if (comma == std::string::npos) comma = value.size();
                auto pattern = upper(trim(std::string_view(value).substr(at, comma - at)));
                at = comma + 1;
                std::replace(pattern.begin(), pattern.end(), '\\', '/');
                while (pattern.starts_with('/')) pattern.erase(0, 1);
                while (pattern.ends_with('/')) pattern.pop_back();
                if (pattern.empty()) continue;
                const auto star = pattern.find('*');
                if (star != std::string::npos && star + 1 != pattern.size())
                    throw std::runtime_error("manifest line " + std::to_string(line_number) +
                                             ": overrides patterns take '*' only at the end ('" + pattern + "')");
                manifest.overrides.push_back(pattern);
            }
            if (manifest.overrides.empty())
                throw std::runtime_error("manifest line " + std::to_string(line_number) + ": overrides is empty");
        }
    }
    if (manifest.name.empty()) throw std::runtime_error("manifest has no name");
    return manifest;
}

bool game_version_matches(std::string_view version) {
    const auto plain = [](std::string_view text) {
        std::string out;
        for (const auto c : text)
            if (std::isalnum(static_cast<unsigned char>(c))) out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        return out;
    };
    return trim(version).empty() || plain(version) == plain(game_version_text);
}

std::string_view current_platform() {
#if defined(__vita__)
    return "vita";
#else
    return "pc";
#endif
}

std::string override_stem(std::string_view pattern) {
    auto key = upper(pattern);
    std::replace(key.begin(), key.end(), '\\', '/');
    while (key.starts_with('/')) key.erase(0, 1);
    for (const std::string_view folder : {"TEXTURES/", "MODELS/", "SOUNDS/", "FILES/"})
        if (key.starts_with(folder)) {
            key.erase(0, folder.size());
            break;
        }
    if (key.ends_with('*')) return key;
    for (const std::string_view extension : {".PNG", ".DDS", ".GLTF", ".GLB", ".WAV", ".OGG"})
        if (key.size() > extension.size() && key.ends_with(extension)) {
            key.resize(key.size() - extension.size());
            break;
        }
    return key;
}

bool manifest_declares(const Manifest& manifest, std::string_view key) {
    if (manifest.overrides.empty()) return true;
    const auto stem = override_stem(key);   // 132md12: patterns and keys compared on their stems
    for (const auto& raw : manifest.overrides) {
        const auto pattern = override_stem(raw);
        if (pattern.ends_with('*')) {
            if (stem.starts_with(std::string_view(pattern).substr(0, pattern.size() - 1))) return true;
        } else if (stem == pattern || (stem.size() > pattern.size() && stem.starts_with(pattern) && stem[pattern.size()] == '/')) {
            return true;
        }
    }
    return false;
}

namespace {

bool key_ends_with(std::string_view key, std::string_view target) {
    return key == target ||
           (key.size() > target.size() && key.ends_with(target) && key[key.size() - target.size() - 1] == '/');
}

}  // namespace

int audio_target_match(std::string_view target, std::string_view key, std::span<const std::string> names) {
    if (target.empty()) return 0;
    if (key_ends_with(key, target)) return 2;
    const auto slash = key.rfind('/');
    if (names.empty() || slash == std::string_view::npos) return 0;
    std::string named(key.substr(0, slash + 1));
    for (const auto& name : names) {
        named.resize(slash + 1);
        named += upper(name);
        if (key_ends_with(named, target)) return 1;
    }
    return 0;
}

namespace {

// Checkpoint 132md10: the mods of a mods folder (sub-folders and .zip archives), in path order. An archive's root is
// the .zip path (a registered archive acts as a folder) or its single top folder when that holds the manifest.
struct Candidate {
    std::string folder;
    std::filesystem::path root, archive;
    std::string problem;   // an unreadable archive
};

std::vector<Candidate> mod_candidates(const std::filesystem::path& folder) {
    std::vector<std::filesystem::path> paths;
    std::error_code error;
    for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
        if (it->is_directory(error) || (is_zip_name(it->path()) && it->is_regular_file(error))) paths.push_back(it->path());
    std::sort(paths.begin(), paths.end());
    std::set<std::string> folders;
    for (const auto& path : paths)
        if (!is_zip_name(path) || std::filesystem::is_directory(path, error)) {
            const auto name = path.filename().u8string();
            folders.insert(upper(std::string(name.begin(), name.end())));
        }
    std::vector<Candidate> result;
    for (const auto& path : paths) {
        Candidate c;
        const bool zip = is_zip_name(path) && !std::filesystem::is_directory(path, error);
        const auto name = (zip ? path.stem() : path.filename()).u8string();
        c.folder.assign(name.begin(), name.end());
        c.root = std::filesystem::absolute(path);
        if (zip) {
            c.archive = c.root;
            if (folders.contains(upper(c.folder))) {
                c.problem = "a mod named " + c.folder + " is already in the mods folder (the folder is used)";
                result.push_back(std::move(c));
                continue;
            }
            folders.insert(upper(c.folder));
            try {
                const auto archive = register_archive(path);
                if (!archive->find("manifest.txt")) {
                    std::set<std::string> tops;
                    for (const auto& m : archive->members()) tops.insert(m.name.substr(0, m.name.find('/')));
                    if (tops.size() == 1 && archive->find(*tops.begin() + "/manifest.txt"))
                        c.root /= std::filesystem::path(std::u8string(tops.begin()->begin(), tops.begin()->end()));
                }
            } catch (const std::exception& e) {
                c.problem = e.what();
            }
        }
        result.push_back(std::move(c));
    }
    return result;
}

}  // namespace

std::vector<ModListing> list_mods(const std::filesystem::path& folder, const ModChoices& choices) {
    std::vector<ModListing> result;
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) return result;
    for (auto& candidate : mod_candidates(folder)) {
        ModListing listing;
        listing.folder = std::move(candidate.folder);
        listing.root = std::move(candidate.root);
        listing.archive = std::move(candidate.archive);
        const auto manifest_path = listing.root / "manifest.txt";
        if (!candidate.problem.empty()) {
            listing.problem = candidate.problem;
        } else if (!mod_file_exists(manifest_path)) {
            listing.problem = "no manifest.txt";
        } else {
            try {
                const auto bytes = read_file(manifest_path);
                listing.manifest = parse_manifest(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
                listing.enabled = listing.manifest->enabled;
                if (const auto choice = choices.find(upper(listing.folder)); choice != choices.end()) {
                    listing.enabled = choice->second;
                    listing.chosen = true;
                }
            } catch (const std::exception& e) {
                listing.problem = e.what();
            }
        }
        result.push_back(std::move(listing));
    }
    std::stable_sort(result.begin(), result.end(), [](const ModListing& a, const ModListing& b) {
        if (a.manifest.has_value() != b.manifest.has_value()) return a.manifest.has_value();
        if (a.manifest && a.manifest->priority != b.manifest->priority) return a.manifest->priority > b.manifest->priority;
        return upper(a.folder) < upper(b.folder);
    });
    return result;
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    if (auto archived = read_archived(path)) return std::move(*archived);
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open " + path.string());
    input.seekg(0, std::ios::end);
    const auto size = static_cast<std::streamoff>(input.tellg());
    if (size < 0) throw std::runtime_error("Cannot size " + path.string());
    if (static_cast<std::uint64_t>(size) > (1ull << 31)) throw std::runtime_error("Mod file too large: " + path.string());
    input.seekg(0);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input) throw std::runtime_error("Cannot read " + path.string());
    return bytes;
}

std::unique_ptr<ModSet> ModSet::scan(const std::filesystem::path& folder, std::vector<std::string>& messages,
                                     const ModChoices& choices) {
    std::vector<Mod> mods;
    std::error_code error;
    if (!std::filesystem::is_directory(folder, error)) {
        messages.push_back("no mods folder " + folder.string());
        return from_mods({}, messages);
    }
    for (auto& candidate : mod_candidates(folder)) {
        const auto& folder_name = candidate.folder;
        const auto manifest_path = candidate.root / "manifest.txt";
        if (!candidate.problem.empty()) {
            const auto file = candidate.archive.filename().u8string();
            messages.push_back("skipped " + std::string(file.begin(), file.end()) + ": " + candidate.problem);
            continue;
        }
        if (!mod_file_exists(manifest_path)) {
            messages.push_back("skipped " + folder_name + ": no manifest.txt");
            continue;
        }
        try {
            const auto bytes = read_file(manifest_path);
            const auto manifest = parse_manifest(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            const auto choice = choices.find(upper(folder_name));
            if (choice != choices.end()) {
                if (!choice->second) {
                    messages.push_back("skipped " + folder_name + ": disabled in the settings (mod." + folder_name + "=0)");
                    continue;
                }
                if (!manifest.enabled)
                    messages.push_back(folder_name + ": enabled in the settings (mod." + folder_name +
                                       "=1) over its manifest's enabled=0");
            } else if (!manifest.enabled) {
                messages.push_back("skipped " + folder_name + ": disabled in its manifest");
                continue;
            }
            if (!game_version_matches(manifest.game_version))
                messages.push_back(folder_name + ": made for game_version=" + manifest.game_version + " (this game is " +
                                   std::string(game_version_text) + "); used anyway");
            mods.push_back({folder_name, candidate.root, manifest, candidate.archive});
        } catch (const std::exception& e) {
            messages.push_back("skipped " + folder_name + ": " + e.what());
        }
    }
    return from_mods(std::move(mods), messages);
}

std::unique_ptr<ModSet> ModSet::from_mods(std::vector<Mod> mods, std::vector<std::string>& messages) {
    std::stable_sort(mods.begin(), mods.end(), [](const Mod& a, const Mod& b) {
        if (a.manifest.priority != b.manifest.priority) return a.manifest.priority > b.manifest.priority;
        return upper(a.folder) < upper(b.folder);
    });
    auto set = std::unique_ptr<ModSet>(new ModSet());
    set->mods_ = std::move(mods);
    set->index(messages);
    return set;
}

void ModSet::index(std::vector<std::string>& messages) {
    const auto insert = [&](auto& map, const auto& key, const std::filesystem::path& path, const Mod& mod,
                            const std::string& label) {
        auto it = map.find(key);
        if (it == map.end()) {
            map.emplace(key, Indexed{{path, &mod}, {}});
            return true;
        }
        if (it->second.source.mod == &mod) {
            messages.push_back(mod.folder + ": " + label + " supplied twice; kept " + it->second.source.path.filename().string());
            return false;
        }
        it->second.shadowed.push_back(mod.folder);
        messages.push_back("conflict: " + label + " from " + it->second.source.mod->folder + " (priority " +
                           std::to_string(it->second.source.mod->manifest.priority) + ") shadows " + mod.folder +
                           " (priority " + std::to_string(mod.manifest.priority) + ")");
        return false;
    };
    // Checkpoint 132md4: a mod's entries outside its manifest's overrides= are not used.
    const auto declared = [&](const Mod& mod, const std::string& key, const std::string& label) {
        if (manifest_declares(mod.manifest, key)) return true;
        messages.push_back(mod.folder + ": ignored " + label + " (not in its manifest's overrides=)");
        return false;
    };
    for (const auto& mod : mods_) {
        std::size_t count = 0;
        const auto files_root = mod.root / "files";
        for (const auto& path : files_below(files_root)) {
            std::string problem;
            const auto key = relative_key(path.lexically_relative(files_root), problem);
            if (!key) {
                messages.push_back(mod.folder + ": ignored " + path.string() + " (" + problem + ")");
                continue;
            }
            if (forbidden(*key)) {
                messages.push_back(mod.folder + ": ignored files/" + *key + " (that disc file cannot be replaced)");
                continue;
            }
            if (!declared(mod, *key, "files/" + *key)) continue;
            if (insert(files_, *key, path, mod, "files/" + *key)) add_parents(file_parents_, *key);
            ++count;
        }
        const auto textures_root = mod.root / "textures";
        for (const auto& path : files_below(textures_root)) {
            std::string problem;
            auto key = relative_key(path.lexically_relative(textures_root), problem);
            if (!key) {
                messages.push_back(mod.folder + ": ignored " + path.string() + " (" + problem + ")");
                continue;
            }
            std::string base;
            if (key->ends_with(".PNG") || key->ends_with(".DDS")) base = key->substr(0, key->size() - 4);
            else {
                messages.push_back(mod.folder + ": ignored textures/" + *key + " (not a .png or .dds)");
                continue;
            }
            if (base.starts_with("BY-HASH/")) {
                const auto digits = base.substr(8);
                if (!is_hex16(digits)) {
                    messages.push_back(mod.folder + ": ignored textures/" + *key + " (by-hash names are 16 hex digits)");
                    continue;
                }
                if (!declared(mod, base, "textures/" + *key)) continue;
                const auto hash = std::stoull(digits, nullptr, 16);
                insert(hashes_, hash, path, mod, "textures/by-hash/" + lower(digits));
                ++count;
                continue;
            }
            const auto slash = base.rfind('/');
            const auto leaf = slash == std::string::npos ? base : base.substr(slash + 1);
            const auto tdx = leaf.find(".TDX");
            bool valid = slash != std::string::npos && tdx != std::string::npos && tdx > 0;
            if (valid && tdx + 4 != leaf.size()) {
                const auto suffix = leaf.substr(tdx + 4);
                valid = suffix.size() >= 2 && suffix[0] == '.' &&
                        std::all_of(suffix.begin() + 1, suffix.end(), [](char c) { return c >= '0' && c <= '9'; }) &&
                        suffix != ".0" && suffix.size() <= 6;
            }
            if (!valid) {
                messages.push_back(mod.folder + ": ignored textures/" + *key +
                                   " (expected textures/<disc path>/<member>.TDX[.<frame>].png)");
                continue;
            }
            if (forbidden(base)) {
                messages.push_back(mod.folder + ": ignored textures/" + *key + " (that disc file cannot be replaced)");
                continue;
            }
            if (!declared(mod, base, "textures/" + *key)) continue;
            if (insert(textures_, base, path, mod, "textures/" + base)) {
                add_parents(texture_parents_, base);
                texture_bases_.insert(base.substr(0, base.rfind(".TDX") + 4));
            }
            ++count;
        }
        // Checkpoint 132md2: models\ (glTF replacements); a model's companion files (.bin, images) are not entries.
        const auto models_root = mod.root / "models";
        for (const auto& path : files_below(models_root)) {
            std::string problem;
            auto key = relative_key(path.lexically_relative(models_root), problem);
            if (!key) {
                messages.push_back(mod.folder + ": ignored " + path.string() + " (" + problem + ")");
                continue;
            }
            std::string base;
            if (key->ends_with(".GLTF")) base = key->substr(0, key->size() - 5);
            else if (key->ends_with(".GLB")) base = key->substr(0, key->size() - 4);
            else continue;
            if (base.find('/') == std::string::npos || forbidden(base)) {
                messages.push_back(mod.folder + ": ignored models/" + *key + " (expected models/<disc path>/<member>.gltf)");
                continue;
            }
            if (!declared(mod, base, "models/" + *key)) continue;
            insert(models_, base, path, mod, "models/" + base);
            ++count;
        }
        // Checkpoint 132md4: sounds\ (stream replacements, .wav / .ogg).
        const auto sounds_root = mod.root / "sounds";
        for (const auto& path : files_below(sounds_root)) {
            std::string problem;
            auto key = relative_key(path.lexically_relative(sounds_root), problem);
            if (!key) {
                messages.push_back(mod.folder + ": ignored " + path.string() + " (" + problem + ")");
                continue;
            }
            std::string base;
            if (key->ends_with(".WAV") || key->ends_with(".OGG")) base = key->substr(0, key->size() - 4);
            if (base.empty() || base.find('/') == std::string::npos || forbidden(base)) {
                messages.push_back(mod.folder + ": ignored sounds/" + *key +
                                   " (expected sounds/<disc path>/<member>.wav or .ogg)");
                continue;
            }
            if (!declared(mod, base, "sounds/" + *key)) continue;
            insert(sounds_, base, path, mod, "sounds/" + base);
            ++count;
        }
        // Checkpoint 132md12: the manifest's wide.<screen>= backgrounds.
        for (const auto& [screen, images] : mod.manifest.wide) {
            WideBackground found{screen, {}, &mod, {}};
            for (const auto& image : images) {
                const auto path = mod.root / std::filesystem::path(std::u8string(image.begin(), image.end()));
                if (mod_file_exists(path)) found.images.push_back(path);
                else messages.push_back(mod.folder + ": ignored wide." + screen + " image " + image + " (no such file in the mod)");
            }
            if (found.images.empty()) continue;
            if (const auto it = wide_.find(screen); it != wide_.end()) {
                it->second.shadowed.push_back(mod.folder);
                messages.push_back("conflict: wide." + screen + " from " + it->second.mod->folder + " (priority " +
                                   std::to_string(it->second.mod->manifest.priority) + ") shadows " + mod.folder +
                                   " (priority " + std::to_string(mod.manifest.priority) + ")");
                continue;
            }
            count += found.images.size();
            wide_.emplace(screen, std::move(found));
        }
        // Checkpoint 132md13: the manifest's audio_overrides= targets (explicit declarations: overrides= does not filter
        // them). The same target from a later mod never wins (the earlier one names every key it does): shadowed.
        for (const auto& item : mod.manifest.audio_overrides) {
            const auto label = "audio_overrides " + item.target;
            const auto path = mod.root / std::filesystem::path(std::u8string(item.file.begin(), item.file.end()));
            if (!mod_file_exists(path)) {
                messages.push_back(mod.folder + ": ignored " + label + "=" + item.file + " (no such file in the mod)");
                continue;
            }
            const auto same = std::find_if(audio_.begin(), audio_.end(), [&](const IndexedAudio& a) { return a.target == item.target; });
            if (same != audio_.end()) {
                if (same->source.mod == &mod) {
                    messages.push_back(mod.folder + ": " + label + " given twice; kept " + same->source.path.filename().string());
                    continue;
                }
                same->shadowed.push_back(mod.folder);
                messages.push_back("conflict: " + label + " from " + same->source.mod->folder + " (priority " +
                                   std::to_string(same->source.mod->manifest.priority) + ") shadows " + mod.folder +
                                   " (priority " + std::to_string(mod.manifest.priority) + ")");
                continue;
            }
            audio_.push_back({item.target, {path, &mod}, {}});
            ++count;
        }
        // Checkpoint 132md14: the manifest's movie_overrides= (explicit, as audio_overrides=: overrides= does not filter
        // them). An item naming a file the mod lacks is ignored whole; the same movie from a later mod: shadowed.
        for (const auto& item : mod.manifest.movie_overrides) {
            const auto label = "movie_overrides " + item.target;
            MovieSource found{item.target, {}, {}, {}, &mod, 1};
            bool missing = false;
            for (const auto& [file, out] : {std::pair{&item.video, &found.video}, std::pair{&item.audio, &found.audio},
                                            std::pair{&item.subtitles, &found.subtitles}}) {
                if (file->empty()) continue;
                *out = mod.root / std::filesystem::path(std::u8string(file->begin(), file->end()));
                if (!mod_file_exists(*out)) {
                    messages.push_back(mod.folder + ": ignored " + label + " (no file " + *file + " in the mod)");
                    missing = true;
                }
            }
            if (missing) continue;
            if (const auto it = movies_.find(item.target); it != movies_.end()) {
                it->second.shadowed.push_back(mod.folder);
                messages.push_back("conflict: " + label + " from " + it->second.source.mod->folder + " (priority " +
                                   std::to_string(it->second.source.mod->manifest.priority) + ") shadows " + mod.folder +
                                   " (priority " + std::to_string(mod.manifest.priority) + ")");
                continue;
            }
            movies_.emplace(item.target, IndexedMovie{std::move(found), {}});
            ++count;
        }
        // Checkpoint 132md16: the manifest's data_overrides= files (explicit, as audio_overrides=: overrides= does not
        // filter them). A file the mod lacks, or one that does not parse, is ignored whole; a field another mod set
        // first: shadowed.
        std::size_t data_count = 0;
        for (const auto& file : mod.manifest.data_overrides) {
            const auto path = mod.root / std::filesystem::path(std::u8string(file.begin(), file.end()));
            std::vector<DataOverride> items;
            try {
                if (!mod_file_exists(path)) throw std::runtime_error("no such file in the mod");
                const auto bytes = read_file(path);
                items = parse_data_overrides(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), file);
            } catch (const std::exception& e) {
                messages.push_back(mod.folder + ": ignored data_overrides " + file + " (" + e.what() + ")");
                continue;
            }
            for (auto& item : items) {
                const auto key = data_entry_key(item);
                const auto same = std::find_if(data_.begin(), data_.end(), [&](const DataSource& d) { return data_entry_key(d.item) == key; });
                if (same != data_.end()) {
                    if (same->source.mod == &mod) {
                        messages.push_back(mod.folder + ": data " + key + " given twice (" + same->item.file + " line " +
                                           std::to_string(same->item.line) + ", " + file + " line " + std::to_string(item.line) +
                                           "); kept the first");
                        continue;
                    }
                    same->shadowed.push_back(mod.folder);
                    messages.push_back("conflict: data " + key + " from " + same->source.mod->folder + " (priority " +
                                       std::to_string(same->source.mod->manifest.priority) + ") shadows " + mod.folder +
                                       " (priority " + std::to_string(mod.manifest.priority) + ")");
                    continue;
                }
                if (item.table == "weapon") data_files_.insert("GAMEDATA/COMMON/WEAPONS.WDB");
                else if (item.table == "npc") {
                    const auto level = item.selector.substr(0, item.selector.find(' '));
                    if (level == "*") data_every_pop_ = true;
                    else data_files_.insert("GAMEDATA/" + level + "/DATA.POP");
                }
                data_.push_back({std::move(item), {path, &mod}, {}});
                ++data_count;
            }
        }
        messages.push_back("mod " + mod.folder + " \"" + mod.manifest.name + "\"" +
                           (mod.manifest.version.empty() ? "" : " " + mod.manifest.version) + " priority " +
                           std::to_string(mod.manifest.priority) + ": " + std::to_string(count) + " file(s)" +
                           (data_count ? ", " + std::to_string(data_count) + " data field(s)" : std::string()));
    }
}

std::optional<ModSet::Source> ModSet::file(std::string_view key) const {
    const auto it = files_.find(key);
    if (it == files_.end()) return std::nullopt;
    return it->second.source;
}

std::optional<ModSet::Source> ModSet::texture(std::string_view key) const {
    const auto it = textures_.find(key);
    if (it == textures_.end()) return std::nullopt;
    return it->second.source;
}

std::optional<ModSet::Source> ModSet::model(std::string_view key) const {
    const auto it = models_.find(key);
    if (it == models_.end()) return std::nullopt;
    return it->second.source;
}

std::optional<ModSet::Source> ModSet::sound(std::string_view key) const {
    const auto it = sounds_.find(key);
    if (it == sounds_.end()) return std::nullopt;
    return it->second.source;
}

std::optional<ModSet::Source> ModSet::texture_hash(std::uint64_t hash) const {
    const auto it = hashes_.find(hash);
    if (it == hashes_.end()) return std::nullopt;
    return it->second.source;
}

unsigned ModSet::volume(const Mod& mod) const {
    if (const auto it = volumes_.find(upper(mod.folder)); it != volumes_.end()) return it->second;
    return mod.manifest.audio_volume;
}

bool ModSet::volume_chosen(const Mod& mod) const { return volumes_.contains(upper(mod.folder)); }

std::optional<ModSet::AudioSource> ModSet::audio(std::string_view key, std::span<const std::string> names) const {
    const auto exact = sounds_.find(key);
    const Mod* exact_mod = exact == sounds_.end() ? nullptr : exact->second.source.mod;
    const auto from_sounds = [&] {
        return AudioSource{exact->second.source.path, exact_mod, static_cast<float>(volume(*exact_mod)) / 100.0F,
                           "sounds/" + std::string(key)};
    };
    if (audio_.empty()) {
        if (!exact_mod) return std::nullopt;
        return from_sounds();
    }
    for (const auto& mod : mods_) {
        if (&mod == exact_mod) return from_sounds();
        const IndexedAudio* best = nullptr;
        int best_rank = 0;
        for (const auto& a : audio_) {
            if (a.source.mod != &mod) continue;
            const int rank = audio_target_match(a.target, key, names);
            if (rank && (!best || rank > best_rank || (rank == best_rank && a.target.size() > best->target.size()))) {
                best = &a;
                best_rank = rank;
            }
        }
        if (best)
            return AudioSource{best->source.path, &mod, static_cast<float>(volume(mod)) / 100.0F, "audio_overrides " + best->target};
    }
    return std::nullopt;
}

std::optional<ModSet::MovieSource> ModSet::movie(std::string_view name) const {
    if (movies_.empty()) return std::nullopt;
    const auto it = movies_.find(upper(name));
    if (it == movies_.end()) return std::nullopt;
    MovieSource source = it->second.source;
    source.gain = static_cast<float>(volume(*source.mod)) / 100.0F;
    return source;
}

bool ModSet::has_files_below(std::string_view key) const { return file_parents_.contains(key); }

bool ModSet::has_data(std::string_view key) const {
    if (data_.empty()) return false;
    if (data_files_.contains(key)) return true;
    return data_every_pop_ && key.starts_with("GAMEDATA/") && key.ends_with("/DATA.POP") &&
           std::count(key.begin(), key.end(), '/') == 2;
}

bool ModSet::has_textures_below(std::string_view key) const { return texture_parents_.contains(key); }

std::vector<std::string> ModSet::textures_below(std::string_view key) const {
    std::vector<std::string> result;
    const auto prefix = std::string(key) + "/";
    for (auto it = textures_.lower_bound(prefix); it != textures_.end() && it->first.starts_with(prefix); ++it)
        result.push_back(it->first);
    return result;
}

std::vector<std::string> ModSet::file_children(std::string_view key) const {
    std::vector<std::string> result;
    const auto prefix = std::string(key) + "/";
    for (auto it = files_.lower_bound(prefix); it != files_.end() && it->first.starts_with(prefix); ++it) {
        const auto rest = std::string_view(it->first).substr(prefix.size());
        const auto child = std::string(rest.substr(0, rest.find('/')));
        if (result.empty() || result.back() != child) result.push_back(child);
    }
    return result;
}

const ModSet::WideBackground* ModSet::wide_background(std::string_view screen) const {
    if (wide_.empty()) return nullptr;
    const auto key = lower(screen);
    if (const auto it = wide_.find(key); it != wide_.end()) return &it->second;
    if (const auto dot = key.find('.'); dot != std::string::npos)
        if (const auto it = wide_.find(std::string_view(key).substr(0, dot)); it != wide_.end()) return &it->second;
    return nullptr;
}

std::vector<ModSet::Entry> ModSet::entries() const {
    std::vector<Entry> result;
    for (const auto& [key, indexed] : files_) result.push_back({"files", key, indexed.source, indexed.shadowed});
    for (const auto& [key, indexed] : textures_) result.push_back({"textures", key, indexed.source, indexed.shadowed});
    for (const auto& [hash, indexed] : hashes_)
        result.push_back({"by-hash", hash_text(hash), indexed.source, indexed.shadowed});
    for (const auto& [key, indexed] : models_) result.push_back({"models", key, indexed.source, indexed.shadowed});
    for (const auto& [key, indexed] : sounds_) result.push_back({"sounds", key, indexed.source, indexed.shadowed});
    for (const auto& a : audio_) result.push_back({"audio", a.target, a.source, a.shadowed});   // 132md13
    for (const auto& [name, m] : movies_)   // 132md14
        result.push_back({"movies", name,
                          {!m.source.video.empty() ? m.source.video : !m.source.audio.empty() ? m.source.audio : m.source.subtitles,
                           m.source.mod},
                          m.shadowed});
    for (const auto& d : data_) result.push_back({"data", data_entry_key(d.item), d.source, d.shadowed});   // 132md16
    return result;
}

std::vector<ModSet::Conflict> ModSet::conflicts() const {
    std::vector<Conflict> result;
    const auto all = entries();
    for (const auto& entry : all)
        if (!entry.shadowed.empty())
            result.push_back({"shadowed", entry.kind + "/" + entry.key, entry.source.mod->folder, entry.shadowed,
                              "the first in resolution order is used; the others' are not"});
    for (const auto& [screen, wide] : wide_)   // 132md12
        if (!wide.shadowed.empty())
            result.push_back({"shadowed", "wide." + screen, wide.mod->folder, wide.shadowed,
                              "the first in resolution order is used; the others' are not"});
    // 132md13: a sounds\ file and another mod's audio_overrides= target naming the same key: the first mod wins.
    for (const auto& a : audio_)
        if (const auto it = sounds_.find(a.target); it != sounds_.end() && it->second.source.mod != a.source.mod) {
            const bool sounds_first = it->second.source.mod < a.source.mod;   // mods_ is in resolution order
            const auto* winner = sounds_first ? it->second.source.mod : a.source.mod;
            const auto* other = sounds_first ? a.source.mod : it->second.source.mod;
            result.push_back({"shadowed", "sounds/" + a.target, winner->folder, {other->folder},
                              sounds_first ? "the sounds\\ file is used over " + other->folder + "'s audio_overrides= target"
                                           : "the audio_overrides= target is used over " + other->folder + "'s sounds\\ file"});
        }
    for (const auto& [archive, indexed] : files_) {
        const auto* owner = indexed.source.mod;
        std::map<std::string, std::vector<std::string>> below;   // folder -> its entries at / below the archive
        for (const auto& entry : all) {
            if (entry.kind == "by-hash" || entry.source.mod == owner) continue;
            const bool under = entry.key.size() > archive.size() && entry.key.starts_with(archive) &&
                               entry.key[archive.size()] == '/';
            const bool same = entry.kind != "files" &&
                              (entry.key == archive ||
                               (entry.kind == "textures" && entry.key.starts_with(archive + ".")));
            if (under || same) below[entry.source.mod->folder].push_back(entry.kind + "/" + entry.key);
        }
        for (const auto& [folder, keys] : below) {
            std::string detail = std::to_string(keys.size()) + " entr" + (keys.size() == 1 ? "y" : "ies") + " of " + folder +
                                 (keys.size() == 1 ? " applies" : " apply") + " over " + owner->folder + "'s replacement (e.g. " + keys.front() + ")";
            result.push_back({"layered", "files/" + archive, owner->folder, {folder}, detail});
        }
    }
    return result;
}

void install(std::shared_ptr<const ModSet> set, std::function<void(const std::string&)> log) {
    if (set && set->empty()) set.reset();
    set_texture_decode_hook(nullptr);
    reset_texture_replacements();
    // 132md10: with files\ replacements the hook also scales their textures under a memory cap (register_file_textures).
    const bool textures = set && (set->has_textures() || set->has_files());
    {
        auto& state = installed();
        std::lock_guard lock(state.mutex);
        if (state.set) state.retired.push_back(state.set);
        state.set = std::move(set);
        state.log = std::move(log);
        state.noted_set.clear();
        state.noted.clear();
        ++install_generation;
        active_set.store(state.set.get());
    }
    if (textures) set_texture_decode_hook(&attach_replacement);
}

const ModSet* active() { return active_set.load(std::memory_order_acquire); }

namespace {

std::string path_text(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return std::string(text.begin(), text.end());
}

}  // namespace

std::string signature(const ModSet* set) {
    if (!set || set->empty()) return {};
    std::string out;
    const auto stamp = [&](const std::filesystem::path& path) {
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error) {
            out += " -";
            return;
        }
        const auto time = std::filesystem::last_write_time(path, error);
        out += " " + std::to_string(size) + "@" + std::to_string(error ? 0 : static_cast<long long>(time.time_since_epoch().count()));
    };
    // A file of a folder mod is stamped; an archive's members are covered by the archive's stamp.
    const auto source = [&](const std::filesystem::path& path, const Mod* mod) {
        out += "|" + path_text(path);
        if (!path.empty() && mod && mod->archive.empty()) stamp(path);
    };
    for (const auto& mod : set->mods()) {
        out += "mod " + mod.folder + "|" + path_text(mod.root) + "|" + path_text(mod.archive) + "|" + std::to_string(set->volume(mod));
        stamp(mod.archive.empty() ? mod.root / "manifest.txt" : mod.archive);
        out += "\n";
    }
    for (const auto& e : set->entries()) {
        out += e.kind + " " + e.key + "|" + (e.source.mod ? e.source.mod->folder : std::string());
        source(e.source.path, e.source.mod);
        for (const auto& s : e.shadowed) out += "|" + s;
        if (e.kind == "movies")
            if (const auto m = set->movie(e.key)) {
                source(m->video, m->mod);
                source(m->audio, m->mod);
                source(m->subtitles, m->mod);
            }
        out += "\n";
    }
    for (const auto& [screen, wide] : set->wide_backgrounds()) {
        out += "wide " + screen + "|" + (wide.mod ? wide.mod->folder : std::string());
        for (const auto& image : wide.images) source(image, wide.mod);
        for (const auto& s : wide.shadowed) out += "|" + s;
        out += "\n";
    }
    return out;
}

std::uint64_t generation() { return install_generation.load(); }

void note(const std::string& what) {
    auto& state = installed();
    std::lock_guard lock(state.mutex);
    if (!state.set || !state.noted_set.insert(what).second) return;
    state.noted.push_back(what);
    const auto line = "Mods (port-only): " + what;
    if (state.log) state.log(line);
    else std::cout << line << "\n";
}

std::vector<std::string> noted() {
    auto& state = installed();
    std::lock_guard lock(state.mutex);
    return state.noted;
}

std::vector<HogMember> hog_members(std::span<const std::uint8_t> archive) {
    if (archive.size() < 20) throw std::runtime_error("Truncated HOG header");
    const auto count = le32(archive, 4), table = le32(archive, 8), names = le32(archive, 12), data = le32(archive, 16);
    if (count == 0 || count > 100000 || table < 20 || table >= names || names > data || data > archive.size() ||
        std::uint64_t(table) + (std::uint64_t(count) + 1) * 4 > names)
        throw std::runtime_error("Invalid HOG index bounds");
    if (le32(archive, table) != 0) throw std::runtime_error("HOG first offset is not zero");
    std::vector<HogMember> result;
    std::size_t name_offset = names;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto start = le32(archive, table + std::size_t(i) * 4), end = le32(archive, table + std::size_t(i + 1) * 4);
        if (start > end || std::uint64_t(data) + end > archive.size()) throw std::runtime_error("HOG member outside archive");
        std::string name;
        while (name_offset < data && archive[name_offset] != 0) {
            const auto byte = archive[name_offset++];
            if (byte < 32 || byte >= 127) throw std::runtime_error("Invalid HOG member name");
            name.push_back(static_cast<char>(byte));
        }
        if (name.empty() || name_offset >= data) throw std::runtime_error("Unterminated HOG member name");
        ++name_offset;
        result.push_back({std::move(name), std::size_t(data) + start, std::size_t(end) - start});
    }
    return result;
}

std::vector<std::uint8_t> rebuild_hog(std::span<const std::uint8_t> archive,
                                      const std::map<std::string, std::vector<std::uint8_t>>& replaced) {
    const auto members = hog_members(archive);
    const auto table = le32(archive, 8), data = le32(archive, 16);
    std::uint64_t alignment = 2048;
    for (const auto& member : members)
        while (alignment > 1 && (member.offset - data) % alignment) alignment /= 2;
    std::vector<std::uint8_t> result(archive.begin(), archive.begin() + data);
    for (std::size_t i = 0; i < members.size(); ++i) {
        put32(result, table + i * 4, result.size() - data);
        const auto it = replaced.find(members[i].name);
        if (it == replaced.end()) {
            const auto bytes = archive.subspan(members[i].offset, members[i].size);
            result.insert(result.end(), bytes.begin(), bytes.end());
            continue;
        }
        result.insert(result.end(), it->second.begin(), it->second.end());
        if (i + 1 < members.size())
            result.resize(data + (result.size() - data + alignment - 1) / alignment * alignment, 0);
    }
    put32(result, table + members.size() * 4, result.size() - data);
    // Bytes after the last member (sector padding) are kept.
    const auto tail = members.back().offset + members.back().size;
    result.insert(result.end(), archive.begin() + tail, archive.end());
    return result;
}

std::optional<std::vector<std::uint8_t>> resolve_bytes(const ModSet& set, const std::string& key,
                                                       const std::function<std::vector<std::uint8_t>()>& original) {
    const auto self = set.file(key);
    const bool below = set.has_files_below(key);
    if (!self && !below) return std::nullopt;
    std::vector<std::uint8_t> bytes;
    if (self) {
        bytes = read_file(self->path);
        note("files/" + key + " <- " + self->mod->folder);
    } else {
        bytes = original();
    }
    if (!below) return bytes;
    std::vector<HogMember> members;
    try {
        members = hog_members(bytes);
    } catch (const std::exception& e) {
        note("files below " + key + " ignored: it is not a HOG archive (" + e.what() + ")");
        return bytes;
    }
    std::map<std::string, std::vector<std::uint8_t>> replaced;
    for (const auto& child : set.file_children(key)) {
        const auto it = std::find_if(members.begin(), members.end(),
                                     [&](const HogMember& member) { return upper(member.name) == child; });
        if (it == members.end()) {
            note("files/" + key + "/" + child + " ignored: " + key + " has no member " + child);
            continue;
        }
        const auto member = *it;
        auto resolved = resolve_bytes(set, key + "/" + child, [&] {
            return std::vector<std::uint8_t>(bytes.begin() + member.offset, bytes.begin() + member.offset + member.size);
        });
        if (resolved) replaced[member.name] = std::move(*resolved);
    }
    if (replaced.empty()) return bytes;
    return rebuild_hog(bytes, replaced);
}

}  // namespace omega::mods
