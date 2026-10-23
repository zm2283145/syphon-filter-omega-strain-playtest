#pragma once
#include "skeleton.hpp"
#include <filesystem>
#include <ostream>
namespace omega {
// Diagnostic input supplies phases and weights explicitly, not an animation graph.
void write_pose_sequence(std::ostream& output, const SkeletonDefinition& skeleton,
                         const std::filesystem::path& sequence,
                         const std::filesystem::path& destination);
}
