#pragma once
#include <filesystem>
#include <iosfwd>

namespace omega {
// Explicit authored/extracted graphs and frame controls; no clips or actor host.
void write_graph_cases(std::ostream& output, const std::filesystem::path& path);
}
