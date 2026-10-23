#include "movie_player.hpp"

namespace omega::movie {

MoviePlayerStep update_movie_player(MoviePlayerFlags& flags, MovieDecoderState state) {
    MoviePlayerStep step;
    if (flags.stop_235) return step;   // 003f8240: an earlier stop ends the player now
    if (flags.restart_234) flags.stop_235 = true;
    if (state != MovieDecoderState::finished) {
        step.pumped = true;
    } else if (!flags.paused_231) {
        flags.stop_235 = true;   // natural end (sequence end code drained)
    }
    if (flags.stop_235) step.shut_down = true;
    if (flags.restart_234) {
        step.reopened = true;
        flags.restart_234 = false;
        flags.stop_235 = false;
    }
    step.keep_playing = !flags.stop_235;
    return step;
}

MovieVblankPresenter::Field MovieVblankPresenter::vblank(int csr_field, bool interlace_flag) {
    Field out;
    if (queue_.empty()) return out;   // no picture: the clock does not advance
    int field = csr_field & 1;
    if (interlace_flag) {
        field = toggle_55c228_;
        toggle_55c228_ ^= 1;
    }
    auto& head = queue_.front();
    out.picture = head.picture;
    if (field == 0 && head.state == 2) {
        head.state = 1;
        out.upload_top = true;
    } else if (field == 1 && head.state == 1) {
        out.upload_bottom = true;
        queue_.pop_front();
    }
    ++fields_5c5a8_;
    out.counted = true;
    return out;
}

}  // namespace omega::movie
