#pragma once
#include <filesystem>
#include <iosfwd>

namespace omega {
void write_transition_sequence(std::ostream& out, const std::filesystem::path& input);
}
