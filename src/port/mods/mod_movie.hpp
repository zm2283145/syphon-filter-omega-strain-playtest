#pragma once
// Port-only (checkpoint 132md14, research/MODDING.md "Movie overrides"): a mod's replacement of a ZMOVIES.HOG movie
// (the manifest's movie_overrides=, ModSet::movie). Not game code: the PS2 game plays only its disc's .PSS movies.
// The front end's player (FrontendSession, 003f8220) pulls a picture every two fields (30000/1001 a second) and counts
// the fields for the subtitles; a replacement keeps that clock:
//   - the picture: an MPEG-2 video elementary stream (.m2v / .mpv, any frame_rate_code, up to 1920 x 1152, frame
//     pictures: the port's own decoder, mpeg2_decoder.hpp), decoded ahead on a worker thread into a few frames; at
//     the k-th pull the picture shown is the source's floor(k * fps * 1001 / 30000)-th (24 / 25 fps repeat pictures,
//     50 / 60 fps drop them), so a movie lasts what its stream lasts. Or a .pss in the disc's own format (29.97).
//     None given: the disc movie's.
//   - the sound: a .wav / .ogg (FittedAudio: 48 kHz stereo, the mod's volume) handed out at the picture clock, through
//     the k-th pull (k + 1) * 48000 * 1001 / 30000 frames; or the mod .pss's own sound; none given: the disc movie's
//     (read for its sound alone when the picture is the mod's).
//   - checkpoint 132md15: or an .mp4 / .m4v / .mov (H.264 + AAC) through the platform's own decoder
//     (platform_movie.hpp: Media Foundation on Windows; the Vita host's sceAvPlayer backend): decoded ahead on a
//     worker thread (PlatformVideo); at the k-th pull the picture shown is the last whose time is <= k * 1001 / 30000 s
//     (from the first picture's), and the file's own sound (fitted to 48 kHz stereo, aligned to the first picture) is
//     the sound unless a .wav / .ogg is given. No platform decoder: not played (the disc's movie plays, logged).
//   - the end: when the picture ends (the end message, the level-start hand-off and skipping are the session's, as for
//     a disc movie). Subtitles are the disc's .SUB (or the item's .sub: frontend_disc.cpp), timed by the same fields.
#include "mod_set.hpp"
#include "movie_pss.hpp"
#include "platform_movie.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace omega::mods {

class FittedAudio;

// The first sequence header of an MPEG-2 video elementary stream.
struct VideoInfo {
    int width = 0, height = 0;
    int aspect_code = 0;       // aspect_ratio_information: 1 square pixels, 2 4:3, 3 16:9, 4 2.21:1
    int rate_code = 0;         // frame_rate_code 1..8
    int rate_num = 0, rate_den = 1;   // the frame rate (24000/1001 ... 60/1)
    // The display aspect (width / height): code 1 the pixel size's, else the code's.
    float display_aspect() const;
};
// Scans `head` for the first sequence header (00 00 01 B3); nullopt when there is none or its values are invalid.
std::optional<VideoInfo> read_video_info(std::span<const std::uint8_t> head);

// The source picture (0-based) shown at the k-th pull (0-based) of a picture every 1001/30000 s.
std::uint64_t source_picture_at_pull(std::uint64_t pull, int rate_num, int rate_den);
// The audio frames (per channel) handed out through the k-th pull.
std::uint64_t audio_frames_through_pull(std::uint64_t pull, std::uint32_t rate = 48000);

// A byte source over a mod file: a plain file read as a stream; a stored (method 0) member of a mod archive streamed
// from the archive in place; a deflated member read whole first.
movie::PssMovie::Source mod_file_source(const std::filesystem::path& path);

// Checkpoint 132md15: a mod file for the platform decoder (platform_movie.hpp), read at any offset: a plain file; a
// stored member of a mod archive read in place; a deflated member read whole first.
movie::MovieFile mod_movie_file(const std::filesystem::path& path);

// Checkpoint 132md15: a platform decoder's movie (platform_movie.hpp) run on a worker thread (the decoder is opened,
// used and closed there): a bounded queue of display-order pictures, each with its time from the first picture's, and
// the sound fitted to 48000 Hz stereo (channels as fit_audio's: mono to both, the first two of more; resampled
// linearly) and aligned to the first picture (silence before a later sound, an earlier sound's lead cut).
class PlatformVideo {
public:
    // Throws why the file cannot be opened, or when its pictures are past the 1920 x 1152 the front end shows.
    PlatformVideo(const movie::PlatformMovieBackend& backend, movie::MovieFile file, std::size_t queue = 4);
    ~PlatformVideo();
    PlatformVideo(const PlatformVideo&) = delete;
    PlatformVideo& operator=(const PlatformVideo&) = delete;
    const movie::PlatformMovieInfo& info() const { return info_; }
    const std::string& backend_name() const { return backend_name_; }
    bool has_sound() const { return info_.audio_rate > 0 && info_.audio_channels > 0; }
    // The next picture and its time in seconds from the first picture's (waits for the worker); false at the end.
    bool pop(mpeg2::Frame& frame, double& seconds);
    // Appends the 48 kHz stereo sound's frames [from, through) (waits for the worker; `from` is where the previous
    // call stopped, 0 at first) and returns how many it appended (fewer once the sound ends).
    std::uint64_t take_sound(std::uint64_t from, std::uint64_t through, std::vector<std::int16_t>& out);
    // The error that ended the pictures or the sound early, empty if none.
    std::string error();

private:
    void run(movie::PlatformMovieBackend backend, movie::MovieFile file);
    std::size_t queue_max_;
    std::string backend_name_;
    movie::PlatformMovieInfo info_;
    std::mutex mutex_;
    std::condition_variable changed_;
    struct Picture {
        mpeg2::Frame frame;
        double seconds = 0;
    };
    std::deque<Picture> pictures_;
    bool opened_ = false, pictures_done_ = false, sound_done_ = false, stop_ = false;
    std::string open_error_, error_;
    double last_picture_ = 0;                         // the latest queued picture's time
    std::vector<std::int16_t> sound_;                 // 48 kHz stereo frames [sound_base_, sound_end_)
    std::uint64_t sound_base_ = 0, sound_end_ = 0, sound_wanted_ = 0;
    std::thread worker_;
};

// Decodes an MPEG-2 video elementary stream on a worker thread into a bounded queue of display-order frames.
class ElementaryVideo {
public:
    ElementaryVideo(movie::PssMovie::Source source, std::size_t queue = 4, std::size_t chunk = 64 * 1024);
    ~ElementaryVideo();
    ElementaryVideo(const ElementaryVideo&) = delete;
    ElementaryVideo& operator=(const ElementaryVideo&) = delete;
    // Waits for the stream's sequence header; nullopt when the stream has none (an error: error()).
    std::optional<VideoInfo> info();
    // The next frame (waits for the worker); false at the end of the stream or after an error.
    bool pop(mpeg2::Frame& frame);
    // The decode error that ended the stream, empty if none.
    std::string error();

private:
    void run();
    movie::PssMovie::Source source_;
    std::size_t queue_max_, chunk_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<mpeg2::Frame> frames_;
    std::optional<VideoInfo> info_;
    bool info_done_ = false, done_ = false, stop_ = false;
    std::string error_;
    std::thread worker_;
};

class ModMovie final : public movie::MovieStream {
public:
    // `disc` opens the disc movie (for the picture or the sound the replacement does not give; called at most once,
    // here). Throws when a file cannot be opened or a .wav / .ogg not read.
    ModMovie(const ModSet::MovieSource& source, const std::function<std::unique_ptr<movie::PssMovie>()>& disc);
    ~ModMovie() override;
    bool next_frame(mpeg2::Frame& frame) override;
    bool audio_format(int& rate, int& channels) override;
    void take_audio(std::vector<std::int16_t>& out) override;
    float picture_aspect() const override { return aspect_; }
    // What plays, for the log: "picture <file> 1280x720 23.976 fps 16:9, sound <file>, subtitles the disc's".
    std::string describe();
    std::uint64_t pulls() const { return pulls_; }

private:
    std::unique_ptr<ElementaryVideo> elementary_;
    std::unique_ptr<PlatformVideo> platform_;     // 132md15: an .mp4 / .m4v / .mov picture
    bool platform_sound_ = false;                 // the sound is platform_'s own
    bool pending_ = false, platform_end_ = false;
    mpeg2::Frame pending_frame_;
    double pending_seconds_ = 0, current_seconds_ = 0, interval_ = 0;
    std::unique_ptr<movie::PssMovie> pictures_;   // a .pss picture (the mod's or the disc's)
    std::unique_ptr<movie::PssMovie> disc_sound_; // the disc movie read for its sound alone
    std::unique_ptr<FittedAudio> sound_;
    bool pictures_sound_ = false;   // the sound is pictures_'s own
    float gain_ = 1;
    std::optional<VideoInfo> info_;
    float aspect_ = 0;
    mpeg2::Frame current_;
    std::int64_t current_index_ = -1;
    std::uint64_t pulls_ = 0;
    bool ended_ = false;
    std::uint64_t audio_given_ = 0;   // frames handed out (sound_ / disc_sound_)
    std::vector<std::int16_t> disc_pcm_;   // disc_sound_'s samples read, not yet handed out
    std::string video_name_, audio_name_;
};

}  // namespace omega::mods
