#include "level_animation.hpp"
#include <stdexcept>

namespace omega {
std::string level_animation_key(std::string_view name) {
    std::string key(name);
    for (auto& c : key) {
        const auto byte = static_cast<signed char>(c);
        if (byte >= 0x41 && byte <= 0x5a) c = static_cast<char>(byte + 0x20);
    }
    return key;
}

std::string level_animation_member(std::string_view key) {
    std::string member(key);
    for (auto& c : member)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 0x20);
    return member + ".SKA";
}

AnimationGraphNode level_animation_node(const std::string& key, const std::string& clip_key, const AnimationClip& clip) {
    AnimationGraphNode node;
    node.kind = AnimationGraphKind::Leaf;
    node.name = key;
    node.leaf.clip_key = clip_key;
    const float frames = static_cast<float>(clip.frame_intervals);
    node.leaf.duration = frames * clip.frame_period;
    node.leaf.looping = clip.looping;
    return node;
}

LevelAnimationLoad load_level_animation(std::string_view name, std::span<const LevelAnimationModel> models,
    AnimationClipBank& clips, const std::function<AnimationClip(const std::string& member)>& read) {
    LevelAnimationLoad load;
    load.key = level_animation_key(name);
    if (load.key.empty())
        throw std::runtime_error("game::LoadAnimation of an empty name (the original looks up \".ska\" and does nothing)");
    if (load.key.size() > 47)
        throw std::runtime_error("game::LoadAnimation name over 47 bytes (the original's 48-byte key truncates it): " + load.key);
    for (unsigned char c : load.key)
        if (c < 32 || c > 126)
            throw std::runtime_error("game::LoadAnimation name with a byte outside printable ASCII (the registry's key domain)");
    if (models.empty()) throw std::runtime_error("game::LoadAnimation without hosted models");
    load.member = level_animation_member(load.key);
    std::size_t have = 0;
    for (const auto& model : models) {
        if (!model.graph || !model.registry) throw std::runtime_error("game::LoadAnimation: model without a graph or registry");
        if (model.registry->node_count() != model.graph->nodes().size())
            throw std::runtime_error("game::LoadAnimation: model registry and graph differ in node count");
        have += model.registry->has_name(load.key) ? 1 : 0;
    }
    if (have == models.size()) {
        load.skipped = true;
        return load;
    }
    if (have != 0)
        throw std::runtime_error("game::LoadAnimation: the hosted models disagree on having " + load.key +
                                 " (the original appends to the lacking ones only)");
    if (clips.contains(load.member))
        throw std::runtime_error("game::LoadAnimation: clip bank already holds " + load.member);
    auto clip = read(load.member);
    const auto node = level_animation_node(load.key, load.member, clip);
    for (const auto& model : models) {
        const auto index = model.graph->append_leaf(node);
        const auto id = model.registry->append_leaf(load.key);
        if (!id || index + 1 != model.registry->node_count())
            throw std::runtime_error("game::LoadAnimation: registry append out of step with the graph");
        if (load.id && *load.id != *id)
            throw std::runtime_error("game::LoadAnimation: the hosted models assign different IDs to " + load.key);
        load.id = id;
    }
    clips.emplace(load.member, std::move(clip));
    return load;
}
}
