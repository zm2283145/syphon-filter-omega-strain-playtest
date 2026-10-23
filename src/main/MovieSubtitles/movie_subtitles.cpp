#include "movie_subtitles.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace omega::movie {
namespace {

// Python str.isspace() for the ASCII range (sub_parse.py strips with it).
bool space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' || (c >= 0x1c && c <= 0x1f);
}

bool digit(char c) { return c >= '0' && c <= '9'; }

std::string_view strip(std::string_view s) {
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

std::vector<std::string_view> split_ws(std::string_view s) {
    std::vector<std::string_view> parts;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && space(s[i])) ++i;
        const std::size_t b = i;
        while (i < s.size() && !space(s[i])) ++i;
        if (i > b) parts.push_back(s.substr(b, i - b));
    }
    return parts;
}

// str.splitlines() for the ASCII line boundaries.
std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t b = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\n' || c == '\r' || c == '\v' || c == '\f' || c == 0x1c || c == 0x1d || c == 0x1e) {
            lines.push_back(text.substr(b, i - b));
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            b = i + 1;
        }
    }
    if (b < text.size()) lines.push_back(text.substr(b));
    return lines;
}

int language_id(std::string_view name) {
    static constexpr std::pair<std::string_view, int> table[]{{"ENGLISH", 1}, {"FRENCH", 2},   {"SPANISH", 3},
                                                              {"ITALIAN", 4}, {"GERMAN", 5},   {"JAPANESE", 6},
                                                              {"RUSSIAN", 7}};
    for (const auto& [n, id] : table)
        if (n == name) return id;
    return 0;
}

}  // namespace

double subtitle_strtod_prefix(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size() && space(s[i])) ++i;
    const std::size_t begin = i;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    const std::size_t mantissa = i;
    while (i < s.size() && digit(s[i])) ++i;
    bool any = i > mantissa;
    if (i < s.size() && s[i] == '.') {
        const std::size_t frac = i + 1;
        std::size_t j = frac;
        while (j < s.size() && digit(s[j])) ++j;
        if (any || j > frac) {
            any = true;
            i = j;
        }
    }
    if (!any) return 0.0;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        std::size_t j = i + 1;
        if (j < s.size() && (s[j] == '+' || s[j] == '-')) ++j;
        const std::size_t exp = j;
        while (j < s.size() && digit(s[j])) ++j;
        if (j > exp) i = j;
    }
    return std::strtod(std::string(s.substr(begin, i - begin)).c_str(), nullptr);
}

float subtitle_timestamp(std::string_view stamp) {
    const auto field = [&](std::size_t at) {
        return static_cast<float>(subtitle_strtod_prefix(at < stamp.size() ? stamp.substr(at) : std::string_view{}));
    };
    const float minutes = field(0), seconds = field(3), hundredths = field(6);
    return minutes * 60.0f + seconds + hundredths / 100.0f;
}

std::optional<std::string> SubtitleFile::init_value(std::string_view key) const {
    std::optional<std::string> value;
    for (const auto& [k, v] : init)
        if (k == key) value = v;
    return value;
}

const SubtitleFeed* SubtitleFile::feed(int language_id) const {
    for (const auto& f : feeds)
        if (f.language_id == language_id) return &f;
    return nullptr;
}

SubtitleFile parse_subtitles(std::string_view text) {
    while (!text.empty() && text.back() == '\0') text.remove_suffix(1);
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    SubtitleFile result;
    enum class Section { none, init, feed } section = Section::none;
    SubtitleFeed* feed = nullptr;
    SubtitleCue* cue = nullptr;
    for (const auto raw : split_lines(text)) {
        const std::string_view s = strip(raw);
        if (s.empty() || s.front() == '#') continue;
        if (s.front() == '%') {
            const auto kw = split_ws(s.substr(1));
            if (kw.empty()) throw std::runtime_error("SUB: empty % directive");   // IndexError in sub_parse.py
            if (kw[0] == "INIT") {
                section = Section::init;
            } else if (kw[0] == "FEED") {
                section = Section::feed;
                SubtitleFeed f;
                f.language = kw.size() > 1 ? std::string(kw[1]) : std::string();
                f.language_id = language_id(f.language);
                result.feeds.push_back(std::move(f));
                feed = &result.feeds.back();
                cue = nullptr;
            } else if (kw[0] == "END") {
                section = Section::none;
                cue = nullptr;
            }
            continue;
        }
        if (section == Section::init) {
            const auto sp = s.find(' ');
            const std::string_view key = s.substr(0, sp);
            const std::string_view value = sp == std::string_view::npos ? std::string_view{} : strip(s.substr(sp + 1));
            result.init.emplace_back(std::string(key), std::string(value));
        } else if (section == Section::feed) {
            if (s.front() == '"') {
                if (cue) {
                    std::string_view line = s;
                    while (!line.empty() && line.front() == '"') line.remove_prefix(1);
                    while (!line.empty() && line.back() == '"') line.remove_suffix(1);
                    cue->lines.emplace_back(line);
                }
                continue;
            }
            const auto parts = split_ws(s);
            if (parts.size() >= 2 && parts[0].find(':') != std::string_view::npos) {
                SubtitleCue c;
                c.raw_start = parts[0];
                c.raw_end = parts[1];
                c.method = parts.size() > 2 ? std::string(parts[2]) : std::string();
                c.position = parts.size() > 3 ? std::string(parts[3]) : std::string();
                c.start = subtitle_timestamp(c.raw_start);
                c.end = subtitle_timestamp(c.raw_end);
                c.flags = c.method.starts_with("PRINT") ? subtitle_print
                          : c.method.starts_with("TYPE") ? subtitle_type
                                                         : 0;
                c.flags |= c.position.starts_with("LEFT")     ? subtitle_left
                           : c.position.starts_with("CENTER") ? subtitle_center
                                                              : 0;
                feed->cues.push_back(std::move(c));
                cue = &feed->cues.back();
            }
        }
    }
    return result;
}

// ----------------------------------------------------------------- runtime
SubtitleTrack::SubtitleTrack(const SubtitleFeed* feed) : feed_(feed) {
    t_ = 0.0f;
    load_next();
}

void SubtitleTrack::clear() {
    // 00412050: no lines, start 1e32 (0x749dc5ae), end -1, flags 3.
    loaded_ = false;
    lines_.clear();
    reveal_.clear();
    started_.clear();
    std::uint32_t bits = 0x749dc5aeu;
    float huge;
    std::memcpy(&huge, &bits, sizeof huge);
    start_ = huge;
    end_ = -1.0f;
    flags_ = 3;
    visible_ = false;
}

void SubtitleTrack::load_next() {
    if (!feed_ || next_ >= feed_->cues.size()) {
        clear();
        return;
    }
    const auto& c = feed_->cues[next_];
    cue_ = next_++;
    loaded_ = true;
    flags_ = c.flags;
    lines_ = c.lines;
    reveal_.assign(lines_.size(), 0);
    started_.assign(lines_.size(), false);
    start_ = c.start;
    end_ = c.end;
}

void SubtitleTrack::update(std::uint32_t presented_fields) {
    if (!(t_ < start_)) {
        if (t_ < end_) {
            visible_ = true;
            if (flags_ & subtitle_type) {
                const float step = 0x1.99999ap-4f;   // 0x3dcccccd = 0.1f
                for (std::size_t i = 0; i < lines_.size(); ++i) {
                    if (!started_[i]) {
                        bool begin = false;
                        if (i == 0) {
                            begin = true;
                        } else if (started_[i - 1] && reveal_[i - 1] == lines_[i - 1].size()) {
                            start_ = t_;   // the next line restarts the typewriter clock
                            begin = true;
                        }
                        if (begin) {
                            started_[i] = true;
                            reveal_[i] = 0;
                        }
                    }
                    const std::size_t length = lines_[i].size();
                    if (started_[i] && reveal_[i] < length) {
                        const float n = static_cast<float>(static_cast<std::int32_t>(length));
                        const float k = (t_ - start_) / (step * n) * n;
                        // cvt.w.s truncates; the EE saturates out-of-range values.
                        const std::int32_t whole = k >= 2147483520.0f ? 0x7fffffff : static_cast<std::int32_t>(k);
                        reveal_[i] = static_cast<std::size_t>(static_cast<std::int64_t>(whole) + 1);
                    }
                }
            }
        }
        if (!(t_ < end_) && visible_) {
            visible_ = false;
            load_next();
            if (!(t_ < start_)) visible_ = true;
        }
    }
    t_ = static_cast<float>(presented_fields) / 60.0f;
}

std::vector<std::string> SubtitleTrack::visible_lines() const {
    std::vector<std::string> out;
    if (!visible_ || !loaded_) return out;
    for (std::size_t i = 0; i < lines_.size(); ++i) {
        if (!(flags_ & subtitle_type)) {
            out.push_back(lines_[i]);
        } else if (started_[i]) {
            out.push_back(lines_[i].substr(0, std::min(reveal_[i], lines_[i].size())));
        }
    }
    return out;
}

int subtitle_feed_for_language(std::uint8_t language) {
    static constexpr int table[]{1, 1, 2, 4, 5, 3, 6};
    return language < 7 ? table[language] : 0;
}

}  // namespace omega::movie
