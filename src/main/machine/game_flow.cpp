#include "game_flow.hpp"

#include <bit>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace omega {
namespace {

void set_state(GameFlow& g, std::uint8_t state) {
    g.previous_51 = g.state_50;   // lb G+50, sb G+51
    g.state_50 = state;
}

// 00167ea4..00167ee4 (and 00168244..00168284): 0x5383d4 = app+260,
// 0x5383c0 = (u8)cvt.w.s(191.25 (0x4c1fc8) x video+1c).
void store_video(GameFlow& g, const FlowApp& app) {
    g.video_5383d4 = app.value_260;
    const float product = std::bit_cast<float>(0x433f4000U) * g.video_scale_1c;
    if (!std::isfinite(product) || !(product > -2147483648.0F && product < 2147483648.0F))
        throw std::runtime_error("Game flow: a video scale outside the cvt.w.s range is not hosted");
    g.video_5383c0 = static_cast<std::uint32_t>(static_cast<std::int32_t>(product)) & 0xffU;   // cvt.w.s truncates
}

// 00128e80 == 0: strcasecmp through the ctype table 0x499c71 (bit 1: upper
// case, A..Z for 0..0x7f). A byte >= 0x80 indexes before the table (lb): not
// hosted.
bool same_ignoring_case(const std::string& a, const char* b) {
    const auto fold = [](unsigned char c) {
        if (c >= 0x80) throw std::runtime_error("Game flow 00128e80: a byte >= 0x80 is not hosted");
        return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c + 0x20) : c;
    };
    std::size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (fold(static_cast<unsigned char>(a[i])) != fold(static_cast<unsigned char>(b[i]))) return false;
    if (i < a.size()) fold(static_cast<unsigned char>(a[i]));
    return i == a.size() && !b[i];
}

const std::string& gui_name(const FlowGui& gui, std::int32_t i) {
    if (i < 0 || static_cast<std::size_t>(i) >= gui.names_39.size())
        throw std::runtime_error("Game flow: a GUI movie-list name is missing");
    return gui.names_39[static_cast<std::size_t>(i)];
}

template <class F>
void with_lock(const GameFlow& g, GameFlowHost& host, F&& body) {
    // The 0x533888 counter is incremented around the body and decremented
    // after it: no net change.
    if (g.thread_lock_533880) host.lock_10dc40();
    body();
    if (g.thread_lock_533880) host.unlock_10dca8();
}

// 001303f0: append a name to the queue (allocated on first use), at most 16.
void append_movie(GameFlow& g, GameFlowHost& host, const std::string& name) {
    if (!g.queue_ac) {
        bool allocated = false;
        with_lock(g, host, [&] { allocated = host.allocate_queue(); });
        if (!allocated) throw std::runtime_error("Game flow 001303f0: a failed queue allocation (null G+ac) is not hosted");
        g.queue_ac.emplace();
    }
    auto& q = *g.queue_ac;
    if (q.count_804 < 0) throw std::runtime_error("Game flow 001303f0: a negative queue count is not hosted");
    if (q.count_804 < 16) {
        if (name.size() >= 0x80) throw std::runtime_error("Game flow 001303f0: an unterminated 0x80-byte entry is not hosted");
        q.names[static_cast<std::size_t>(q.count_804)] = name;   // strncpy 0x80
        ++q.count_804;
    }
}

// 001301d0: the attract-mode movie: (T1_COUNT + 0x533870) mod the DEMOnn
// count (signed remainder), stepped past the last pick (0x4ea050) when there
// are at least two; 0 is INTRO.
std::string attract_pick(GameFlow& g, GameFlowHost& host) {
    const std::int32_t count = host.demo_count_1302f0();
    if (count <= 0) throw std::runtime_error("Game flow 001301d0: a DEMOnn count <= 0 (div) is not hosted");
    const auto sum = static_cast<std::int32_t>(host.timer_t1() + g.timer_base_533870);
    std::int32_t index = sum % count;
    if (index == g.last_attract_4ea050 && count >= 2) index = index == count - 1 ? 0 : index + 1;
    g.last_attract_4ea050 = index;
    if (index == 0) return "INTRO";
    char text[32];
    std::snprintf(text, sizeof text, "DEMO%2.2d", index);
    return text;
}

// 0012fd60: the movie player update, then the queue.
void update_movie(GameFlow& g, GameFlowHost& host) {
    if (g.player_b0) {
        const float dt = host.fn_133340();
        if (host.fn_3f8220(*g.player_b0, dt) == 0) {
            MoviePlayer* const player = g.player_b0;
            with_lock(g, host, [&] { host.destroy_player(*player); });
            g.player_b0 = nullptr;
            host.fn_36b250(1);
            host.movie_end_message(g.movie_message_4ea048);
        }
    }
    if (g.player_b0 || !g.queue_ac) return;
    auto& q = *g.queue_ac;
    if (q.index_800 != q.count_804) {
        ++q.index_800;
        if (q.count_804 < q.index_800) throw std::runtime_error("Game flow 0012fd60: a queue index past the count (null name) is not hosted");
        if (q.index_800 < 1 || q.index_800 > 16) throw std::runtime_error("Game flow 0012fd60: a queue index outside the 16 entries is not hosted");
        host.start_movie(g, q.names[static_cast<std::size_t>(q.index_800 - 1)]);
    } else {
        with_lock(g, host, [&] { host.free_queue(); });
        g.queue_ac.reset();
    }
}

}  // namespace

void request_state_movie(GameFlow& g, FlowGui& gui, GameFlowHost& host) {
    if (!g.movies_61) return;
    switch (g.state_50) {
    case 8:   // the GUI list, "%s.PSS" each
        for (std::int32_t i = 0; i < gui.count_13c; ++i) append_movie(g, host, gui_name(gui, i) + ".PSS");
        return;
    case 0xc: {   // the end-of-level movie
        const std::int32_t mission = gui.mission_24c;
        if (mission != -1 && mission != 0x11) {
            host.agent_332a10(static_cast<std::int32_t>(static_cast<std::uint32_t>(mission) + 1U), 6, 2);
            host.agent_3328d0(static_cast<std::int32_t>(static_cast<std::uint32_t>(gui.mission_24c) + 1U), 5, 1, 1);
        }
        const bool ukraine = same_ignoring_case(g.level_140, "UKRAINE");
        host.fn_2c9420();
        set_state(g, 8);
        if (ukraine) request_state_movie(g, gui, host);   // the GUI list
        else host.start_movie(g, g.level_140 + "_EOL.PSS");
        return;
    }
    case 0xb: host.start_movie(g, g.level_140 + "_SOL.PSS"); return;
    case 9: host.start_movie(g, "INTRO.PSS"); return;
    case 7: host.start_movie(g, attract_pick(g, host) + ".PSS"); return;
    default: return;   // 0xa and anything else
    }
}

std::int32_t attract_demo_count(std::int32_t& cache_489be0, bool hog_loaded,
                                const std::function<bool(const std::string&)>& has_member) {
    if (cache_489be0 != -1) return cache_489be0;
    if (hog_loaded) {
        cache_489be0 = 0;
        for (;;) {
            char name[32];
            std::snprintf(name, sizeof name, "DEMO%2.2d.PSS", cache_489be0 + 1);
            if (!has_member(name)) break;
            ++cache_489be0;
        }
    }
    ++cache_489be0;
    return cache_489be0;
}

void run_game_flow_frame(GameFlow& g, FlowGui& gui, const FlowApp& app, GameFlowHost& host, std::uint32_t pad_14c) {
    const bool cross = (pad_14c & 0x40U) != 0;
    if (g.state_50 == 1) {   // 00167e60
        host.fn_1694b0();
        host.fn_37e780({app.width_254, app.height_258, 0x20, 0x10, app.flags_250, 0xfa0, 0x1f4});
        store_video(g, app);
        host.fn_165950();
        if (g.front_end_62) {
            host.fn_2cb8c0(0);
            host.fn_2c7d90(1);
        } else {
            host.fn_2c7c40("splash", 3000);
        }
        host.fn_136710();   // 0012fcd0
        if (g.dev_64) {
            gui.done_2b = 1;
            host.fn_2cc3e0();
        } else if (g.front_end_62) {
            host.fn_2cc3e0();
        }
        set_state(g, 2);
    }
    if (g.state_50 == 2) {   // 00167f60
        const bool menus = g.front_end_62 || g.dev_64;
        if (menus) {   // 0012fc80
            host.fn_2cbb30();
            host.fn_2cbad0();
        }
        if (!menus) {   // 0012fd40
            host.fn_37e320();
            set_state(g, 0xb);
        }
    }
    if (g.state_50 == 3) {   // 00167fb8
        host.fn_3e9c20();   // 0012fc30
        if (g.front_end_62) host.fn_2cb160();
        host.fn_3c8c70();
        host.fn_3f9470();
        if (gui.done_35) {
            if (gui.multiplayer_30) {   // 00167fe8: the multiplayer path (gui+34 and 005721b8 are not read)
                g.mp_5721c8 = 1;
                if (gui.mp_32 && !gui.mp_33) {   // 00168008: to the lobby
                    g.lobby_first_48a008 = 1;
                    set_state(g, 5);
                } else {
                    set_state(g, 0xe);
                }
            } else {
                set_state(g, gui.level_load_34 ? 0xe : 0xb);
                g.mp_5721c8 = 0;
                g.mp_5721b8 = 0;
            }
        }
    } else {
        host.fn_2ca580();
    }
    if (g.state_50 == 4) {   // 001680c8
        const std::string& first = gui_name(gui, 0);
        if (same_ignoring_case(first, "IDLE")) {
            set_state(g, 7);
        } else {
            if (same_ignoring_case(first, "FINAL")) host.fn_2c9420();
            set_state(g, 8);
        }
        host.fn_2c9860(0);
    }
    if (const auto s = g.state_50; s >= 7 && s <= 0xc) {   // 00168170: the movie group
        if (s == 9) {
            while (!host.fn_2c7cb0()) {
            }
            host.fn_2c7d90(0);
        }
        std::uint32_t flags = app.flags_250 | 8U;
        if ((app.flags_250 & 4U) == 0) flags |= 1U;
        host.fn_37e780({app.width_254, app.height_258, 0x20, 0, flags, 0xffffffffU, 0xffffffffU});
        store_video(g, app);
        host.fn_165950();
        g.skip_latch_4ee480 = cross ? 0 : 1;
        request_state_movie(g, gui, host);
        set_state(g, 0xd);
    }
    if (g.state_50 == 0xd) {   // 001682d0
        if (g.player_b0) {
            if (!g.skip_latch_4ee480) {
                g.skip_latch_4ee480 = cross ? 0 : 1;   // a held Cross must be released first
            } else if (cross) {
                if (!g.player_b0->no_skip_231) g.player_b0->skip_235 = 1;   // 0012ff50
                host.fn_36a240();
            }
        }
        update_movie(g, host);
        if (!g.player_b0) {   // 0x49c500 on G+51 - 7
            std::uint8_t next = 0xe;
            switch (g.previous_51) {
            case 7: case 8: case 9: case 0xc: next = 1; break;
            case 0xa: next = 2; break;
            default: next = 0xe; break;
            }
            set_state(g, next);
            host.fn_37e320();
        }
    }
}

}  // namespace omega
