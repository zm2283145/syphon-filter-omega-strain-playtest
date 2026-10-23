#pragma once
#include "skeleton.hpp"
#include <filesystem>

namespace omega {
// Explicit diagnostic inputs, not a recovered actor/controller initialization.
// All file inputs and the output destination must remain in workspace/local.
void write_animation_playback_sequence(std::ostream& output, const SkeletonDefinition& skeleton,
    const std::filesystem::path& manifest_path, const std::filesystem::path& sequence_path,
    const std::filesystem::path& destination);
void write_animation_controller_sequence(std::ostream& output, const SkeletonDefinition& skeleton,
    const std::filesystem::path& manifest_path, const std::filesystem::path& sequence_path,
    const std::filesystem::path& destination);
}
