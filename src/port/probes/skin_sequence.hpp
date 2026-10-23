#pragma once
#include "skinned_mesh.hpp"
#include <filesystem>
namespace omega {
void write_skin_sequence(std::ostream& output,const SkinnedMesh& mesh,
    const std::filesystem::path& path,const std::filesystem::path& destination);
}
