#pragma once
#include "morph_host.hpp"
#include <filesystem>
namespace omega {
void write_morph_host_state(std::ostream& output,const MorphHostState& state);
void write_morph_host_sequence(std::ostream& output,const std::filesystem::path& input);
}
