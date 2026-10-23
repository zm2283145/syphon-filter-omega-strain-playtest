#pragma once
#include <filesystem>
#include <iosfwd>
namespace omega {
void write_group_sequence(std::ostream& output, const std::filesystem::path& path);
}
