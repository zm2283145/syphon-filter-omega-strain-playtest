#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

// The front-end part of the main update 00167c40: the blocks 00167e60..00168434
// for the game states 1, 2, 3, 4, the movie group (7, 8, 9, 0xa, 0xb, 0xc) and
// 0xd, with the helpers 0012fc30, 0012fc80, 0012fcd0, 0012fd40, 0012fd60,
// 0012ff50, 0012ff80 (the movie request), 001301d0 (the attract pick) and
// 001303f0 (the movie-queue append). The blocks are tested in that order and
// fall through, so one frame can run several of them (state 1 -> 2 -> the
// state-3 block's else, the movie group -> 0xd). State 0xe onwards (00168438,
// the level load) is not part of this layer.
//
// G = 0x4ffb50. Everything the blocks call outside this layer is a
// GameFlowHost call (GUI, renderer, file, movie player, agent progress); the
// host methods are named by their address.

// The movie player object at G+b0 (created by 00130520, owned by the host).
struct MoviePlayer {
    std::uint8_t no_skip_231{};   // set: a skip request is ignored
    std::uint8_t skip_235{};      // 0012ff50 sets it to 1 on a skip
};

// The movie queue at G+ac (00130440: 0x808 bytes from the game heap): 16
// entries of 0x80 bytes, +800 the index of the next entry, +804 the count.
struct MovieQueue {
    std::array<std::string, 16> names{};
    std::int32_t index_800{};
    std::int32_t count_804{};
};

// The GUI object at G+b4 (only the bytes this layer reads or writes).
struct FlowGui {
    std::uint8_t done_2b{};          // 0012fcd0 sets it to 1 when G+64
    std::uint8_t multiplayer_30{};   // state 3's multiplayer path (00167fe8)
    // +32 set with +33 clear sends the MP path to the lobby state 5; nothing in the retail executable stores a
    // nonzero +32 (the only store, 002a544c, is in the unreferenced 002a5440), so both the host (+33 = 1 from
    // 002a0dc0) and the joiner go to the level load 0xe.
    std::uint8_t mp_32{}, mp_33{};
    std::uint8_t level_load_34{};    // state 3: 0xe (straight to the level load) or 0xb (the SOL movie)
    std::uint8_t done_35{};          // state 3 leaves only when set
    // gui+39 + 0x20*i: the movie list names as C strings (the entry text up
    // to its terminator); entries [0, max(count, 1)) must be present.
    std::vector<std::string> names_39;
    std::int32_t count_13c{};
    std::int32_t mission_24c{-1};    // gui+188+c4
};

// The application object (s0 of the main update).
struct FlowApp {
    std::uint8_t flags_250{};
    std::uint32_t width_254{}, height_258{};
    std::uint32_t value_260{};
};

struct GameFlow {
    std::uint8_t state_50{}, previous_51{};
    std::uint8_t movies_61{};        // 0012ff80 queues nothing when clear
    std::uint8_t front_end_62{};
    std::uint8_t dev_64{};
    MoviePlayer* player_b0{};        // not owned: the host creates and destroys it
    std::optional<MovieQueue> queue_ac;
    std::string level_140;
    float video_scale_1c{};          // the video object (G+1dc) +1c
    // Globals the blocks read or write.
    std::uint8_t skip_latch_4ee480{};
    std::uint32_t movie_message_4ea048{};   // written by 00130520 (host)
    std::int32_t last_attract_4ea050{-1};
    std::uint8_t thread_lock_533880{};
    std::uint32_t timer_base_533870{};
    std::uint32_t video_5383c0{}, video_5383d4{};
    std::uint8_t mp_5721c8{}, mp_5721b8{};
    std::uint8_t lobby_first_48a008{1};   // the state-5 first-entry byte (initially 1; lobby_state.hpp)
};

// 0037e780(0x5381f0, a1..t3).
struct VideoModeRequest {
    std::uint32_t width{}, height{};
    std::uint32_t depth{};        // a3 = 0x20
    std::uint32_t a4{};           // t0: 0x10 (state 1) or 0
    std::uint32_t flags{};        // t1
    std::uint32_t a6{}, a7{};     // t2, t3: 0xfa0, 0x1f4 (state 1) or -1, -1
};

class GameFlowHost {
public:
    virtual ~GameFlowHost() = default;
    virtual void fn_1694b0() = 0;                                  // (app)
    virtual void fn_37e780(const VideoModeRequest& mode) = 0;      // video mode (0x5381f0)
    virtual void fn_165950() = 0;                                  // (app)
    virtual void fn_2cb8c0(std::uint32_t a1) = 0;                  // (gui, a1)
    virtual void fn_2c7d90(std::uint32_t show) = 0;                // (gui, show, 0, 0): the loading screen
    virtual void fn_2c7c40(std::string_view name, std::uint32_t ms) = 0;   // the splash
    virtual void fn_136710() = 0;
    virtual void fn_2cc3e0() = 0;                                  // (gui)
    virtual void fn_2cbb30() = 0;                                  // (gui)
    virtual void fn_2cbad0() = 0;                                  // (gui)
    virtual void fn_37e320() = 0;                                  // (0x5381f0)
    virtual void fn_3e9c20() = 0;
    virtual void fn_2cb160() = 0;                                  // (gui)
    virtual void fn_3c8c70() = 0;
    virtual void fn_3f9470() = 0;                                  // (G+3c)
    virtual void fn_2ca580() = 0;                                  // (gui)
    virtual void fn_2c9420() = 0;                                  // (gui)
    virtual void fn_2c9860(std::uint32_t a1) = 0;                  // (gui, a1)
    virtual bool fn_2c7cb0() = 0;                                  // (gui): state 9 polls it until nonzero
    virtual void fn_36a240() = 0;                                  // (pad 0x55a300)
    virtual float fn_133340() = 0;                                 // the frame time
    virtual std::int32_t fn_3f8220(MoviePlayer& player, float dt) = 0;   // 0: the movie ended
    virtual void lock_10dc40() = 0;                                // (9), only when 0x533880 is set
    virtual void unlock_10dca8() = 0;                              // (9), only when 0x533880 is set
    // 003f9300(player, -1) then 0036b130(0, player, "game.cc", 0xae2).
    virtual void destroy_player(MoviePlayer& player) = 0;
    virtual void fn_36b250(std::uint32_t a0) = 0;
    // 003caa50(G, 0x4f2cc0) then 001c9540(that object, 2, message).
    virtual void movie_end_message(std::uint32_t message) = 0;
    // 00130520(G, name, 1): may start a movie; it writes G+b0 (the player)
    // and 0x4ea048 itself.
    virtual void start_movie(GameFlow& g, std::string_view name) = 0;
    // 001314f0(G, -1) then 00332a10(agent, a1, a2, a3).
    virtual void agent_332a10(std::int32_t a1, std::int32_t a2, std::int32_t a3) = 0;
    // 001314f0(G, -1) then 003328d0(agent, a1, a2, a3, t0).
    virtual void agent_3328d0(std::int32_t a1, std::int32_t a2, std::int32_t a3, std::int32_t t0) = 0;
    virtual std::int32_t demo_count_1302f0() = 0;                  // (G)
    virtual std::uint32_t timer_t1() = 0;                          // T1_COUNT 0x10000800
    // 0036b180(0, 0x808, "game.cc", 0x9fd): the queue block (false: null).
    virtual bool allocate_queue() = 0;
    // 0036b130(0, queue, "game.cc", 0xaf7).
    virtual void free_queue() = 0;
};

// One main-update frame from 00167e60 to 00168438. pad_14c is the pad word
// 0x55a300+14c (bit 0x40: Cross). The multiplayer path of state 3 (gui+30,
// 00167fe8 .. 00168054, checkpoint 132ln4) sets 005721c8 and goes to the lobby
// state 5 (0048a008 = 1) or to the level load 0xe.
void run_game_flow_frame(GameFlow& g, FlowGui& gui, const FlowApp& app, GameFlowHost& host, std::uint32_t pad_14c);

// 0012ff80 for G+50 (exposed for tests): queue or start the movie of the state.
void request_state_movie(GameFlow& g, FlowGui& gui, GameFlowHost& host);

// 001302f0(G): the number of attract movies, cached in 00489be0 (-1 until
// the first call). The first call opens "ZMEDIA\ZMOVIES.HOG" (0049a950)
// through 003f96e0(G+3c, path, 1, 0); when the HOG loaded (+14c) the cache
// counts the members "DEMO%2.2d.PSS" (0049a968) of 1, 2, ... that the HOG
// lookup 0036d2a0(hog, name, 1) finds (has_member, case-insensitive) up to
// the first missing one. 003f9530 releases the HOG, then the cache is
// incremented: the count plus one (INTRO is pick 0), or 0 when the HOG did
// not load.
std::int32_t attract_demo_count(std::int32_t& cache_489be0, bool hog_loaded,
                                const std::function<bool(const std::string&)>& has_member);

}  // namespace omega
