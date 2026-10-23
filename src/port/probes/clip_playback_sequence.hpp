#pragma once
#include "animation_clip.hpp"
#include "skeleton.hpp"
#include <filesystem>
namespace omega {
void write_clip_playback_sequence(std::ostream& output,const AnimationClip& clip,
    const SkeletonDefinition& skeleton,const std::filesystem::path& path);
}
