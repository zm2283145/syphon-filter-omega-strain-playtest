#pragma once
#include "animation_graph.hpp"
#include <array>
#include <filesystem>
#include <istream>
#include <string_view>
#include <utility>

namespace omega {
struct GraphNamedRoot {
    std::string name;
    std::size_t node_index{};
    std::array<std::uint32_t, 2> group_pair{};
};
struct AnimationGraphManifest {
    explicit AnimationGraphManifest(AnimationGraph value) : graph(std::move(value)) {}
    AnimationGraph graph;
    std::string source_skas, iso_sha256, executable_sha256, skas_sha256;
    float default_crossfade_duration{};
    std::vector<std::array<std::uint8_t, 4>> anchors;
    std::size_t expected_builders{}, completed_builders{}, channel_count{}, selector_count{};
    std::vector<GraphNamedRoot> roots;
    const GraphNamedRoot& root(std::string_view name) const;
};

// Versioned, complete extraction only, with bounded text/counts and typed fields.
// Provenance is retained metadata, not authentication: caller verifies the ISO.
// Does not invent channel initialization or associate SKAS with a skeleton.
AnimationGraphManifest decode_animation_graph_manifest(std::istream& input);
AnimationGraphManifest load_animation_graph_manifest(const std::filesystem::path& path);
}
