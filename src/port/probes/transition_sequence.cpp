#include "transition_sequence.hpp"
#include "animation_transition.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
template<class T> T read(std::istream& input) {
    T value{};
    if (!(input >> value)) throw std::runtime_error("Invalid or truncated transition input");
    return value;
}
unsigned count(std::istream& input, unsigned maximum) {
    const auto value = read<unsigned long long>(input);
    if (value > maximum) throw std::runtime_error("Transition input count exceeds limit");
    return static_cast<unsigned>(value);
}
}

void write_transition_sequence(std::ostream& out, const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 8 * 1024 * 1024)
        throw std::runtime_error("Transition input must be a regular file no larger than 8 MiB");
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open transition input");
    if (read<std::string>(input) != "OMEGA_TRANSITION_CASES" || count(input, 1) != 1)
        throw std::runtime_error("Unsupported transition input format");
    const auto cases = count(input, 128);
    if (!cases) throw std::runtime_error("Transition input requires at least one case");
    std::ostringstream report;
    report << std::setprecision(9) << std::boolalpha << "{\"cases\":[";
    unsigned total = 0;
    for (unsigned i = 0; i < cases; ++i) {
        const auto name = read<std::string>(input);
        if (name.empty() || name.size() > 80 || name.find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
            throw std::runtime_error("Invalid transition case name");
        TransitionBlendState state;
        state.progress = read<float>(input);
        state.rate = read<float>(input);
        state.width = read<float>(input);
        state.bias = read<float>(input);
        state.elapsed = read<float>(input);
        state.limit = read<float>(input);
        bool paused = count(input, 1) != 0;
        const auto operations = count(input, 4096);
        total += operations;
        if (!operations || total > 8192) throw std::runtime_error("Invalid transition operation count");
        if (i) report << ',';
        report << "{\"name\":\"" << name << "\",\"states\":[";
        for (unsigned j = 0; j < operations; ++j) {
            const auto weight = read<float>(input), delta = read<float>(input);
            const auto result = step_transition_blend(state, weight, delta, paused);
            paused = result.paused;
            if (j) report << ',';
            report << "{\"progress\":" << state.progress << ",\"rate\":" << state.rate
                << ",\"width\":" << state.width << ",\"bias\":" << state.bias
                << ",\"elapsed\":" << state.elapsed << ",\"limit\":" << state.limit
                << ",\"sampled_weight\":" << result.sampled_weight
                << ",\"remaining_weight\":" << result.remaining_weight
                << ",\"crossed\":" << result.crossed << ",\"boundary\":" << result.boundary
                << ",\"paused\":" << result.paused << ",\"submit_node\":" << result.submit_node << '}';
        }
        report << "]}";
    }
    std::string trailing;
    if (input >> trailing) throw std::runtime_error("Trailing transition input");
    report << "]}\n";
    out << report.str();
}
}
