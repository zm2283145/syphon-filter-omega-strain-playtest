#pragma once
// Port-only (checkpoint 132md15, research/MODDING.md "Movie overrides"): a platform's own movie decoder, for a mod's
// movie_overrides= picture in a container the port does not decode itself (H.264 / AAC in MP4: an HD remaster of a
// cutscene). Not game code: the PS2 game plays only its disc's .PSS movies (mpeg2_decoder.hpp decodes those and a
// mod's MPEG-2 on every platform).
//
// A backend decodes one file into display-order pictures (I420, mpeg2::Frame, each with its presentation time) and
// interleaved 16-bit sound; the mod layer (mods::PlatformVideo, mod_movie.hpp) paces the pictures on the front end's
// clock and fits the sound to 48000 Hz stereo, so a backend does neither. Backends:
//   - Windows: Media Foundation (platform_movie_mf.cpp, a system component: nothing is shipped), the default there.
//   - PlayStation Vita: sceAvPlayer, installed by the Vita host with set_platform_movie_backend() (research/MODDING.md
//     "The Vita's movie backend"; MovieFile maps onto sceAvPlayer's file-replacement callbacks).
//   - Elsewhere, or none installed: an MP4 movie override is not played (logged; the disc's movie plays).
#include "mpeg2_decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace omega::movie {

// A movie file read at any offset: a plain file, or a member of a mod archive (mods::mod_movie_file). `read` fills
// `out` from `offset` and returns the bytes read (fewer only at the end); it may be called from any one thread at a
// time (a backend's own threads included).
struct MovieFile {
    std::string name;   // the file's name, for messages and the container type (its extension)
    std::uint64_t size = 0;
    std::function<std::size_t(std::uint64_t offset, std::span<std::uint8_t> out)> read;
};

struct PlatformMovieInfo {
    int width = 0, height = 0;         // the pictures' shown size (the decoder's padding cropped)
    float display_aspect = 0;          // width / height as displayed (the pixel aspect applied)
    double frame_rate = 0;             // nominal pictures a second (0: unknown; pacing uses the times)
    double seconds = 0;                // the container's duration (0: unknown)
    int audio_rate = 0, audio_channels = 0;   // 0: no sound
    std::string video_codec, audio_codec;     // "H.264", "AAC", ... for messages
};

// One open movie. Every call (and the destruction) is made on the thread that opened it.
class PlatformMovieDecoder {
public:
    virtual ~PlatformMovieDecoder() = default;
    virtual const PlatformMovieInfo& info() const = 0;
    // The next picture in display order: I420 at info()'s width x height ((w + 1) / 2 x (h + 1) / 2 chroma), `seconds`
    // its presentation time. False at the end of the picture stream or on an error (error()).
    virtual bool next_picture(mpeg2::Frame& frame, double& seconds) = 0;
    // Appends the next piece of sound (interleaved int16 at info()'s rate and channels), `seconds` its first sample's
    // time. False at the end of the sound (or when there is none) or on an error.
    virtual bool next_audio(std::vector<std::int16_t>& out, double& seconds) = 0;
    // What ended the streams early, empty if nothing did.
    virtual std::string error() const = 0;
};

struct PlatformMovieBackend {
    std::string name;                      // "Media Foundation", "sceAvPlayer"
    std::vector<std::string> extensions;   // lower case with the dot: ".mp4", ".m4v", ".mov"
    // Opens a file (throws std::runtime_error naming why not: not a movie, an unsupported codec).
    std::function<std::unique_ptr<PlatformMovieDecoder>(MovieFile file)> open;
};

// Installs `backend` in place of the platform's default (nullopt: back to the default). Call before the front end
// opens a movie (the Vita host at start-up; tests).
void set_platform_movie_backend(std::optional<PlatformMovieBackend> backend);
// Installs "no backend" (as on a platform without one): MP4 overrides are refused. Tests.
void disable_platform_movie_backend();
// The backend in use: the installed one, else the platform's default (Windows: Media Foundation), else nullptr.
const PlatformMovieBackend* platform_movie_backend();
// Whether a file of this name is the platform decoder's (by extension: .mp4 / .m4v / .mov), whatever the backend.
bool is_platform_movie_name(const std::string& name);

}  // namespace omega::movie
