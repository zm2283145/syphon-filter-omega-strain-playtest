#include "card_wait_poll.hpp"

#include <stdexcept>

namespace omega {

CardWaitPoll card_wait_poll(std::uint8_t& guard, const CardWaitApp& app, const CardWaitPollHost& host) {
    if (guard) return CardWaitPoll::reentered;   // 0x002cc270
    guard = 1;
    CardWaitPoll r = CardWaitPoll::character_skip;
    if (!app.character_310) {                    // 0x002cc294: a1 = 0 is never 0x3039
        if (app.gui_28 || app.flag_2d) {         // 002cbb30 0x002cbb44 / 002cbad0 0x002cbad8
            if (!host.gui_frame || !host.gui_draw) {
                guard = 0;
                throw std::runtime_error("002cc260: the app's GUI runs (+28 / +2d) but the nested frame is not hosted");
            }
            host.gui_frame();
            host.gui_draw();
            r = CardWaitPoll::drawn;
        } else {
            r = CardWaitPoll::idle;
        }
    }
    guard = 0;                                   // 0x002cc3c4
    return r;
}

}  // namespace omega
