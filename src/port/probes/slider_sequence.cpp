#include "slider_sequence.hpp"
#include "animation_slider.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace omega {
namespace {
template<class T> T read(std::istream& input) {
    T result{};
    if (!(input >> result)) throw std::runtime_error("Invalid or truncated slider input");
    return result;
}
unsigned count(std::istream& input, unsigned limit) {
    const auto result = read<unsigned long long>(input);
    if (result > limit) throw std::runtime_error("Slider input count exceeds limit");
    return static_cast<unsigned>(result);
}
}

void write_slider_sequence(std::ostream& output, const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 8 * 1024 * 1024)
        throw std::runtime_error("Slider input must be a regular file no larger than 8 MiB");
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open slider input");
    if (read<std::string>(input) != "OMEGA_SLIDER_CASES" || count(input, 1) != 1)
        throw std::runtime_error("Unsupported slider input format");
    const auto cases = count(input, 128);
    if (!cases) throw std::runtime_error("Slider input requires at least one case");
    std::ostringstream report;
    report << std::setprecision(9) << "{\"cases\":[";
    unsigned total = 0;
    for (unsigned i = 0; i < cases; ++i) {
        const auto name = read<std::string>(input);
        if (name.empty() || name.size() > 80 || name.find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
            throw std::runtime_error("Invalid slider case name");
        const bool smooth = count(input, 1) != 0;
        const auto child_count = count(input, 256);
        const auto queries = count(input, 4096);
        total += queries;
        if (!child_count || !queries || total > 8192)
            throw std::runtime_error("Invalid slider child or query count");
        std::vector<SliderChild> children;
        children.reserve(child_count);
        for (unsigned j = 0; j < child_count; ++j) {
            const auto start = read<float>(input), end = read<float>(input);
            children.push_back({start, end});
        }
        if (i) report << ',';
        report << "{\"name\":\"" << name << "\",\"states\":[";
        for (unsigned j = 0; j < queries; ++j) {
            const auto value = read<float>(input), parent = read<float>(input);
            const auto contributions = select_slider_contributions(children, value, parent, smooth);
            if (j) report << ',';
            report << "{\"contributions\":[";
            for (std::size_t k = 0; k < contributions.size(); ++k) {
                if (k) report << ',';
                report << "{\"child_index\":" << contributions[k].child_index
                    << ",\"weight\":" << contributions[k].weight << '}';
            }
            report << "]}";
        }
        report << "]}";
    }
    std::string trailing;
    if (input >> trailing) throw std::runtime_error("Trailing slider input");
    report << "]}\n";
    output << report.str();
}
}
