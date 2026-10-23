#pragma once
// The secondary (upper-body) component's motion playback as far as the throw
// and reload one-shots need it (research/COMBAT_ITEMS.md, C6b). The component
// keeps two playback records: +e0 for the current state +310 and +110 for the
// next state +311 (0x24 bytes: +0 the clip, +4 the skeleton binding, +8 the
// rate in frames per second, +c the elapsed frame time, +10 a per-bone cursor,
// +14 the fraction, +18 / +1c the key pointers, +20 the clamped byte).
// Interpreter-verified:
//   001d7480  after the state step: +e0 = +110 when the pair completed, a new
//             +110 for the name of +311 when it changed (the model registry's
//             0018a670 / 0018a660 lookup, leaf entries only), +e0 = +110 on the
//             first update;
//   001d6120  its timing tail: each playing record advances by dt (00190b00);
//             when a one-shot state's motion (0x15 / 0x16) gets closer to its
//             end than the blend duration +320, +312 = +313. This is what ends
//             the reload and throw one-shots: the state step then blends out of
//             the one-shot and posts its completion (action 4);
//   001cab60  the one-shot phase by name (elapsed / frame count, -1 when the
//             name is not playing), read by 00197e00;
//   001d7890 / 00190b00  the record constructor and its time setter.
// The per-bone sampling loop of 001d6120 is ported as upper_body_sample
// (upper_body_pose.hpp); it only moves records' +10 / +18 / +1c, which
// 00190b00 resets, and play runs it inside the upper-body pass
// (upper_body_pass.hpp, Checkpoint 120s). Not ported: the idle layer +830 (no
// instruction makes it active).
#include "secondary_component.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace omega {

using SecondaryMotionName=std::array<std::uint8_t,48>;
// The callers' bounded copy (00197560, 00197e00, 00197140): at most 47
// characters, the remaining bytes zero.
SecondaryMotionName secondary_motion_name(std::string_view text);
// The name as a C string (up to the first zero byte).
std::string_view secondary_motion_text(const SecondaryMotionName& name);
// 0049f790 row `state` +0x10 as the boot image's initializer leaves it (the
// names of the upper-body motions; rows 0, 1, 0x15 and 0x16 are empty).
// States above 24 throw.
SecondaryMotionName secondary_state_table_name(std::uint8_t state);
// The name of a state as 001cab60 / 001d7480 read it: the second word
// +18 + 8 * state when set (only the one-shot buffers of 0x15 / 0x16 are
// ever set: component one_shots), else the first word (the table row) + 0x10.
SecondaryMotionName secondary_state_motion_name(const SecondaryComponent& component,std::uint8_t state);
// The comparison loop of 001cab60 / 001d7480 (per quadword: pceqw, ppach of
// the word masks, a 64-bit compare with -1): equal when both first bytes are
// zero, else when all 48 bytes are equal (bytes after the terminator count).
bool secondary_motion_names_match(const SecondaryMotionName& query,const SecondaryMotionName& name);

// The clip fields the playback reads: +0 version (key base +70 for >= 3, +40
// for 2, else +30), +4 frame count, +8 key words per frame, +c frame period,
// +10 loop word.
struct SecondaryClipTiming {
    std::uint32_t version_0{};
    std::int32_t frames_4{};
    std::int32_t stride_8{};
    float frame_period_c{};
    bool looping_10{};
    // Native: the registry id the record +0 clip pointer was installed from,
    // naming the key words 001d6120 samples (SecondaryMotionRegistry::keys).
    std::uint32_t source_id{};
    bool operator==(const SecondaryClipTiming&) const=default;
};
struct SecondaryMotionSlot {
    std::optional<SecondaryClipTiming> clip;     // +0 (nullopt: null)
    float rate_8{};                               // a null record keeps an unwritten word here (unused)
    float elapsed_c{};
    std::int32_t cursor_10{};
    float fraction_14{};
    std::int32_t key_18{},next_key_1c{};          // word offsets from the clip's key base
    std::uint8_t ended_20{};
    bool operator==(const SecondaryMotionSlot&) const=default;
};
struct SecondaryPlayback {
    SecondaryMotionSlot current_e0,next_110;
    bool operator==(const SecondaryPlayback&) const=default;
};

// 001d7890(record, clip, binding): rate = float(frames) / (frames as an
// unsigned float x period), then 00190b00(record, 0).
SecondaryMotionSlot make_secondary_motion_slot(const SecondaryClipTiming& clip);
// 00190b00(record, time): looping clips subtract the frame count until below
// it, other clips clamp to it (ended = 1); then the cursor and key offsets.
void set_secondary_motion_time(SecondaryMotionSlot& slot,float time);
// 001cab60(component, name).
float secondary_motion_phase(const SecondaryComponent& component,const SecondaryPlayback& playback,const SecondaryMotionName& name);

// One entry of the model's motion registry (0018a660: table +3c [id]).
struct SecondaryMotionEntry {
    std::uint8_t type_4{};                        // 0: a leaf (the graph manifest's node type)
    std::optional<SecondaryClipTiming> clip_24;
};
// 001d6120's view of a clip: header +14 (root translation), +18 (root yaw),
// +1c / +20 (translation / rotation bone masks) and the key words.
struct SecondaryClipKeys {
    std::uint32_t root_translation_14{},root_yaw_18{},translation_mask_1c{},rotation_mask_20{};
    std::vector<std::uint32_t> words;
};
struct SecondaryMotionRegistry {
    // 0018a670(registry, name): the found node's +40; nullopt when the name is
    // absent (the original then uses id 0).
    std::function<std::optional<std::uint32_t>(std::string_view name)> find;
    // The table entry for an id; nullopt is a null entry (the original
    // dereferences it, so it throws).
    std::function<std::optional<SecondaryMotionEntry>(std::uint32_t id)> entry;
    // The key words of an id's clip from its key base (+70 for version 3) and
    // the header words 001d6120 reads; null when not hosted.
    std::function<std::shared_ptr<const SecondaryClipKeys>(std::uint32_t id)> keys;
};
// 001d7480(component, snapped, changed, first) with the step's flags; `first`
// is "+310 or +311 was 0 before the step" (the step then sets the pair).
void install_secondary_motion_slots(SecondaryPlayback& playback,const SecondaryComponent& component,
    bool snapped,bool changed,bool first,const SecondaryMotionRegistry& registry);
// The timing tail of 001d6120(component, pose, dt). Returns true when it
// wrote +312 = +313.
bool advance_secondary_motion_slots(SecondaryPlayback& playback,SecondaryComponent& component,float dt);
// What 001cb160 does after its state step (001cc8d4..001ccb18): 001d7480,
// then (not on the first update) 001d6120. `first` as for the install.
bool run_secondary_playback(SecondaryPlayback& playback,SecondaryComponent& component,const SecondaryStep& step,
    bool first,float dt,const SecondaryMotionRegistry& registry);

}
