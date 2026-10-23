#pragma once
#include "actor_motion.hpp"
#include "animation_clip.hpp"
#include "animation_graph.hpp"
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace omega {
// The clip bank (animation_player.hpp; the same alias, not included here).
using AnimationClipBank = std::map<std::string, AnimationClip>;
// game::LoadAnimation (001bfd30 -> 001bfd60 -> 001bff40; SCRIPT_HOSTING.md,
// "Update (checkpoint 74)").
//
// 001bfd60's key: signed bytes 0x41..0x5a get +0x20, nothing else changes, no
// length cap (EE interpreter goldens, gen_loadanim_key.py).
std::string level_animation_key(std::string_view name);
// The SKALEVEL.HOG member: key + ".ska" (0x49ee48), found by the case-insensitive
// first match 0036d2a0. The port's archive lookup is exact, so it asks for the
// stored uppercase name: a..z uppercased, then ".SKA".
std::string level_animation_member(std::string_view key);
// 003b4f10 -> 003b4540: the clip-node defaults (a manifest LEAF with default
// fields) with duration f32(f32(u32 word1) * word3) and looping word4 != 0.
AnimationGraphNode level_animation_node(const std::string& key, const std::string& clip_key, const AnimationClip& clip);

struct LevelAnimationModel {
    AnimationGraph* graph{};
    ActorMotionRegistry* registry{};
};
struct LevelAnimationLoad {
    std::string key, member;
    bool skipped{};
    std::optional<std::size_t> id;
};
// 001bff40 on the hosted models: skipped when every model already has the key
// (001c3e90); otherwise the member is read and decoded, and one leaf is
// appended to every model (003b54c0 -> 003b4800: the ID is the old count). The
// clip is added to the bank under the member name. Throws where the original
// is silent or diverges: an empty key or one over 47 bytes (the 48-byte key),
// no models, models that disagree on having the key or on the new ID, a
// missing member (the reader throws), a clip that is not version 3 (the
// decoder), and a member name already in the clip bank.
LevelAnimationLoad load_level_animation(std::string_view name, std::span<const LevelAnimationModel> models,
    AnimationClipBank& clips, const std::function<AnimationClip(const std::string& member)>& read);
}
