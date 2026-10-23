#pragma once
// ZMOVIES.HOG .SUB subtitles (MovieSubtitles.cc, research/FRONTEND.md
// "Checkpoint 114 native movie decoder"): the text parser (0040ffa4..00412954,
// mirrored by local/frontend_movies/sub_parse.py) and the per-frame cue model
// of the subtitle object 0x571cf0 (start 00412220, update 00411db0).
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace omega::movie {

constexpr std::uint32_t subtitle_print = 1;    // "PRINT": all lines at once
constexpr std::uint32_t subtitle_left = 2;     // "LEFT"
constexpr std::uint32_t subtitle_center = 4;   // "CENTER" ("RIGHT" is no keyword: 0)
constexpr std::uint32_t subtitle_type = 8;     // "TYPE": typewriter

struct SubtitleCue {
    float start{};   // seconds (float, as the game stores them at cue+0x130)
    float end{};
    std::string raw_start, raw_end, method, position;
    std::uint32_t flags{};
    std::vector<std::string> lines;   // UTF-8 bytes, quotes stripped
};

struct SubtitleFeed {
    std::string language;   // the %FEED keyword
    int language_id{};      // ENGLISH 1 .. RUSSIAN 7, other 0
    std::vector<SubtitleCue> cues;
};

struct SubtitleFile {
    std::vector<std::pair<std::string, std::string>> init;   // %INIT "key value" lines in order
    std::vector<SubtitleFeed> feeds;
    std::optional<std::string> init_value(std::string_view key) const;   // last occurrence wins
    const SubtitleFeed* feed(int language_id) const;                     // first feed with this id
};

// strtod-style numeric prefix (leading whitespace, sign, digits, fraction,
// exponent); 0 when there is none.
double subtitle_strtod_prefix(std::string_view text);
// f0*60 + f1 + f2/100 with the three fields read by strtod at offsets 0, 3 and 6
// (float arithmetic, as 004102d0..00410374): "01:02:03" -> 62.03, and the
// malformed "00:015:50" -> 15.0 (offset 6 is ':', so the last field reads 0).
float subtitle_timestamp(std::string_view stamp);
// Parses decoded .SUB text (an optional UTF-8 BOM is skipped; a NUL tail is ignored).
SubtitleFile parse_subtitles(std::string_view text);

// Runtime model of the subtitle object for one feed.
class SubtitleTrack {
public:
    // 00412220: t = 0 and the first cue loaded (or the empty state).
    explicit SubtitleTrack(const SubtitleFeed* feed);
    // 00411db0, once per game frame. Uses the clock of the previous call, then
    // sets t = (float)fields / 60 from the presented-field counter 0x55c5a8.
    void update(std::uint32_t presented_fields);
    float time() const { return t_; }
    bool visible() const { return visible_; }
    bool has_cue() const { return loaded_; }
    std::size_t cue_index() const { return cue_; }   // index of the loaded cue
    std::uint32_t flags() const { return flags_; }
    float start() const { return start_; }
    float end() const { return end_; }
    // Raw typewriter reveal count per line (cue+0x6c - line pointer); may
    // exceed the line length by design; 0 = line not started.
    const std::vector<std::size_t>& revealed() const { return reveal_; }
    // The loaded cue's lines (+60[i], +78 of them) and which TYPE lines started
    // (their +6c is non-null; the overlay 004118e0 reads both).
    const std::vector<std::string>& lines() const { return lines_; }
    const std::vector<bool>& started() const { return started_; }
    // The text to draw: nothing while hidden, every line for PRINT, and for
    // TYPE the started lines cut to their revealed prefix (UTF-8 bytes).
    std::vector<std::string> visible_lines() const;

private:
    void load_next();
    void clear();
    const SubtitleFeed* feed_{};
    std::size_t next_{};   // feed+0x68: the next cue to load
    std::size_t cue_{};
    bool loaded_{};
    float t_{};
    float start_{}, end_{};
    std::uint32_t flags_{};
    bool visible_{};
    std::vector<std::string> lines_;
    std::vector<std::size_t> reveal_;
    std::vector<bool> started_;
};

// Language byte 0x55d470 -> subtitle feed language id (jump table 0x49a980).
int subtitle_feed_for_language(std::uint8_t language);

}  // namespace omega::movie
