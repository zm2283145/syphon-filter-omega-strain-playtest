#pragma once
// Pure-logic model of the PS2 movie player around the decoder (research/
// FRONTEND.md "Checkpoint 114 native movie decoder"; disassembly in
// local/frontend_movies/dis_movieplayer.txt). The Cross skip latch (0x4ee480),
// the 0xd state update and the end-of-movie jump table are GameFlow's
// (src/game_flow.cpp, 001681b8 / 001682d0); this file holds the parts below
// it: PlayMovie's volume override (00130520 / 0012fe28), the player update
// 003f8220 and the vblank presenter 003f7060.
#include <cstdint>
#include <deque>

namespace omega::movie {

// PlayMovie saves the channel-2 volume to 0x4ea048 and forces 0x400 (1.0 in the
// x1024 option scale); 0012fd60 restores the saved value after the player dies.
constexpr std::uint32_t movie_full_volume = 0x400;
struct MovieVolume {
    std::uint32_t saved_4ea048{};
    std::uint32_t enter(std::uint32_t channel2_volume) {
        saved_4ea048 = channel2_volume;
        return movie_full_volume;
    }
    std::uint32_t leave() const { return saved_4ea048; }
};

// Decoder thread state at player+0xa8 (003f5ce0).
enum class MovieDecoderState : std::uint8_t { running = 0, abort = 1, end_inserted = 2, finished = 3 };

struct MoviePlayerFlags {
    bool paused_231{};
    bool restart_234{};   // replay request: close and reopen the same movie
    bool stop_235{};      // set by the Cross skip (0012ff50) or at the natural end
    bool audio_236{};     // extra audio shutdown (003f6cd0) on stop
};

struct MoviePlayerStep {
    bool keep_playing{};   // 003f8220's return (0 -> 0012fd60 destroys the player)
    bool pumped{};         // 003f8000: feed the decoder / output queue
    bool shut_down{};      // stop path: wait for the decode thread, stop the stream (003f6f90)
    bool reopened{};       // restart path: 003f8390 close + 003f8450 open
};

// 003f8220 (after the subtitle update 00411db0): one game frame.
MoviePlayerStep update_movie_player(MoviePlayerFlags& flags, MovieDecoderState state);

// 003f7060, once per vblank field: the head of the decoded-picture queue is
// uploaded in two halves, top half on field 0 (entry state 2 -> 1) and bottom
// half on field 1 (1 -> 0, released), so every picture spans exactly two
// fields. The presented-field counter 0x55c5a8 (the subtitle clock) counts
// every field while a picture is queued, and stops while the queue is empty.
class MovieVblankPresenter {
public:
    struct Field {
        bool counted{};          // 0x55c5a8 incremented
        bool upload_top{};       // field 0 of a picture (subtitles drawn with half 0)
        bool upload_bottom{};    // field 1 of a picture; the entry is released
        std::int64_t picture{-1};
    };
    // A decoded picture entered the output queue (state 2).
    void push_picture(std::int64_t picture) { queue_.push_back({picture, 2}); }
    // `csr_field` is the GS CSR FIELD bit; with the display interlace flag
    // 0x55c740 set, parity instead comes from 0x55c228, toggled on every vblank
    // that finds a queued picture.
    Field vblank(int csr_field, bool interlace_flag = false);
    std::uint32_t presented_fields() const { return fields_5c5a8_; }
    std::size_t queued() const { return queue_.size(); }

private:
    struct Entry {
        std::int64_t picture;
        int state;
    };
    std::deque<Entry> queue_;
    std::uint32_t fields_5c5a8_{};
    int toggle_55c228_{};
};

}  // namespace omega::movie
