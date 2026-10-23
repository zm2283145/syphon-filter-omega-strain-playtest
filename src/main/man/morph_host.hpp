#pragma once
#include "skinned_mesh.hpp"
#include <array>
#include <optional>
#include <istream>

namespace omega {
struct MorphCurveKey { std::int32_t frame{}; float value{}; };
using MorphCurve = std::vector<MorphCurveKey>;
struct MorphHostConfig {
    MorphCurve grip_curve, active_expression_curve, periodic_expression_curve;
    std::uint8_t active_expression_shape{}, periodic_expression_shape{};
};
struct MorphHostGrip {
    std::uint8_t counter{};
    std::uint8_t primary_shape{}, secondary_shape{};
    float primary_weight{}, secondary_weight{};
};
struct MorphSpeechKey { std::int16_t time{}; std::uint16_t weight{}; };
using MorphSpeechTracks = std::array<std::vector<MorphSpeechKey>,21>;
struct MorphSpeechChannel {
    std::vector<MorphSpeechKey> keys;
    std::uint16_t index{};
    std::int16_t previous_time{}, next_time{};
    float previous_weight{}, next_weight{};
};
struct MorphHostState {
    bool base_enabled{}, speech_active{};
    std::array<SkinMorphTarget,8> face_slots{};
    std::uint8_t active_count{};
    std::array<MorphHostGrip,2> grips{};
    std::uint8_t active_expression_counter{};
    std::int16_t periodic_counter{-1};
    std::uint16_t periodic_reset{};
    std::array<MorphSpeechChannel,21> speech;
    // +2d6c: the speech clock (morph_speech_clock_ms) when 00391810 started the LPD.
    std::int32_t speech_start{};
};
struct MorphHostInput {
    bool expressions_enabled{true};
    // null means no equipped object; value is its resolved secondary shape.
    std::array<std::optional<std::uint8_t>,2> secondary_grip_shapes;
    // Elapsed speech milliseconds, used when speech_clock is absent.
    std::int32_t speech_elapsed_tick{};
    // 003915b0: the speech clock now; elapsed = clock - speech_start (subu).
    std::optional<std::int32_t> speech_clock;
    // Supplied only when the original periodic-reset branch requests RNG.
    std::optional<std::uint32_t> periodic_random;
    // Point/vector already resolved through the original head-space transform.
    std::optional<std::array<float,3>> local_gaze;
};
float sample_morph_curve(const MorphCurve& curve,std::int32_t frame);
MorphHostConfig read_morph_host_config(std::istream& input);
MorphSpeechTracks decode_morph_speech(std::span<const std::uint8_t> bytes);
// 00391810 / 003915b0 clock: (T1_COUNT 10000800 + *00533870) as an unsigned float / 5.76, truncated,
// plus the 64-bit *00533878, divided by 100 (milliseconds; the clock of 003cfab0, ie_text_clock_ms).
std::int32_t morph_speech_clock_ms(std::uint32_t t1_count,std::uint32_t t1_base,std::int64_t ticks);
// 00391810: the channels primed from the tracks and +2d6c = start_ms.
void start_morph_speech(MorphHostState& state,const MorphSpeechTracks& tracks,std::int32_t start_ms=0);
void set_morph_base_shape(MorphHostState& state,std::uint8_t shape,bool enabled);
// Original count reset, speech, grip/expression order and counter updates.
// Explicit time/RNG/resource inputs; no actor clock, equipment or RNG invention.
void update_morph_host(MorphHostState& state,const MorphHostConfig& config,const MorphHostInput& input);
// True when the next update_morph_host with expressions enabled takes 0038b530's periodic-reset
// branch, the only one that draws the game RNG (00127dd0).
bool morph_periodic_reset_due(const MorphHostState& state);
std::vector<SkinMorphTarget> morph_face_targets(const MorphHostState& state);
std::array<SkinMorphTarget,2> morph_hand_targets(const MorphHostState& state,std::size_t hand);
}
