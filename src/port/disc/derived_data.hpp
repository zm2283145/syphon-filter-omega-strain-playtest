#pragma once
// Port-only (checkpoint 132win8): the play host's disc-derived data, built natively from the user's own verified
// disc so a packaged build needs no Python. These are C++ ports of the repository's extractors, byte for byte:
//   motion graph manifests  tools/motion_graph_extract.py (a bounded constructor tracer over SCUS_972.64's 17
//                           motion-graph builders, dispatcher 001efb60; MALE.SKAS and FEMALE.SKAS),
//   actor animation tuning  tools/actor_animation_data.py (the literal stores of 004c6c40's prefix up to its call
//                           to 00100b30: 0049ced8 and 0049cf48..0049cfe8),
//   navigation tuning       tools/actor_navigation_data.py (004cc78c..004cc810 and 004c6df4..004c6e0c's stores
//                           to 004a0410, 004a0420 and 0049cff0).
// Nothing here is game code: the outputs are the same files the Python tools write under local/ (CRLF lines, as the
// tools' text-mode writes produce on Windows), read by the play host's existing loaders. No disc content is in
// source; the outputs stay under the user's local/ folder.

#include "disc.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega::derived {

// tools/native_bindings.py ElfImage: little-endian ELF32, reads only file-backed PT_LOAD bytes.
class ElfImage {
public:
    explicit ElfImage(std::vector<std::uint8_t> data);
    std::span<const std::uint8_t> read(std::uint32_t address, std::uint32_t length) const;
    std::uint32_t word(std::uint32_t address) const;
    std::string string(std::uint32_t address) const;  // printable ASCII, NUL-terminated, at most 256 bytes
private:
    struct Segment {
        std::uint32_t type{}, offset{}, vaddr{}, filesz{};
    };
    std::vector<std::uint8_t> data_;
    std::vector<Segment> segments_;
};

// Python's format(value, '.9g') and json.dumps(text, ensure_ascii=True).
std::string format_g9(double value);
std::string json_quote(std::string_view text);
// Python's f32(): round to binary32; nonfinite results throw.
double f32(double value);

struct SkasConfig {
    double default_crossfade_duration{};
    std::vector<std::array<int, 4>> anchors;
    struct Clip {
        std::string name, filename;
    };
    std::vector<Clip> clips;
};
SkasConfig parse_skas(std::span<const std::uint8_t> data);

struct ClipHeader {
    std::string filename;
    double duration{};
    bool looping{};
};
// SKA.HOG's version-3 clip headers: duration = f32(f32(count) * period).
ClipHeader clip_header(std::string filename, std::span<const std::uint8_t> header20);

struct Provenance {
    std::string iso_sha256, executable_sha256, skas_sha256;
};

// The graph manifest text (LF lines) for one SKAS source ("MALE.SKAS" / "FEMALE.SKAS").
std::string graph_manifest(const ElfImage& image, const SkasConfig& config, const std::vector<ClipHeader>& clips,
                           const Provenance& provenance, const std::string& source);
// "OMEGA_ACTOR_TUNING 1" and "OMEGA_NAVIGATION_TUNING 1" texts (LF lines).
std::string actor_tuning(const ElfImage& image);
std::string navigation_tuning(const ElfImage& image);
// LF -> CRLF, as the Python tools' text-mode writes on Windows.
std::string crlf(std::string_view text);

struct DerivedFiles {
    std::filesystem::path graph, female_graph, actor_tuning, navigation_tuning;
};
DerivedFiles derived_files(const std::filesystem::path& folder);
struct DiscFingerprint {
    std::string iso_sha256, boot_executable, executable_sha256;
    std::uint64_t executable_size{};
};
// Reads SCUS_972.64 (size and SHA-256 checked), SKAS.HOG and SKA.HOG's clip headers from the opened, already
// verified disc and writes the four files into folder (created).
DerivedFiles derive_game_data(DiscImage& disc, const DiscFingerprint& fingerprint, const std::filesystem::path& folder);
// True when all four files exist and the folder's derived.stamp (written last by derive_game_data) names this extractor
// version, disc and boot executable (so a stale or partly written set is rebuilt).
bool derived_data_current(const DerivedFiles& files, const DiscFingerprint& fingerprint);

}  // namespace omega::derived
