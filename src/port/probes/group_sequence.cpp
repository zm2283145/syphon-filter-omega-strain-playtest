#include "group_sequence.hpp"
#include "animation_group.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace omega {
namespace {
template<class T> T read(std::istream& input) {
    T value{};
    if (!(input >> value)) throw std::runtime_error("Invalid or truncated group input");
    return value;
}
unsigned count(std::istream& input, unsigned maximum) {
    const auto value = read<unsigned long long>(input);
    if (value > maximum) throw std::runtime_error("Group input count exceeds limit");
    return static_cast<unsigned>(value);
}
}
void write_group_sequence(std::ostream& output, const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 8 * 1024 * 1024)
        throw std::runtime_error("Group input must be a regular file no larger than 8 MiB");
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open group input");
    if (read<std::string>(input) != "OMEGA_GROUP_CASES" || count(input, 1) != 1)
        throw std::runtime_error("Unsupported group input format");
    const auto cases = count(input, 128);
    if (!cases) throw std::runtime_error("Group input requires at least one case");
    std::ostringstream report;
    report << std::setprecision(9) << std::boolalpha << "{\"cases\":[";
    unsigned steps_total = 0, contributions_total = 0;
    for (unsigned i = 0; i < cases; ++i) {
        const auto name = read<std::string>(input);
        if (name.empty() || name.size() > 80 || name.find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
            throw std::runtime_error("Invalid group case name");
        AnimationGroupPhase state;
        state.previous = read<float>(input);
        state.current = read<float>(input);
        state.advance = read<float>(input);
        state.independent_bounds = count(input, 1) != 0;
        state.looping = count(input, 1) != 0;
        state.finished = count(input, 1) != 0;
        state.boundary = count(input, 1) != 0;
        const auto steps = count(input, 4096);
        steps_total += steps;
        if (!steps || steps_total > 8192) throw std::runtime_error("Invalid group step count");
        if (i) report << ',';
        report << "{\"name\":\"" << name << "\",\"states\":[";
        for (unsigned j = 0; j < steps; ++j) {
            const bool active = count(input, 1) != 0;
            const auto contributions = count(input, 10000);
            contributions_total += contributions;
            if (contributions_total > 10000) throw std::runtime_error("Too many group contributions");
            const auto dt = read<float>(input);
            std::vector<DurationContribution> entries;
            entries.reserve(contributions);
            for (unsigned k = 0; k < contributions; ++k) {
                const auto weight = read<float>(input), duration = read<float>(input);
                entries.push_back({weight, duration});
            }
            const auto totals = accumulate_group_duration(entries);
            step_animation_group(state, active, totals.total_weight, totals.weighted_duration, dt);
            if (j) report << ',';
            report << "{\"previous\":" << state.previous << ",\"current\":" << state.current
                << ",\"advance\":" << state.advance << ",\"independent_bounds\":" << state.independent_bounds
                << ",\"looping\":" << state.looping << ",\"finished\":" << state.finished
                << ",\"boundary\":" << state.boundary << ",\"total_weight\":" << totals.total_weight
                << ",\"weighted_duration\":" << totals.weighted_duration << '}';
        }
        report << "]}";
    }
    std::string trailing;
    if (input >> trailing) throw std::runtime_error("Trailing group input");
    report << "]}\n";
    output << report.str();
}
}
