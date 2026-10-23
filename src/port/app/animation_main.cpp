#include "animation_channel.hpp"
#include "root_sequence.hpp"
#include "transition_sequence.hpp"
#include "slider_sequence.hpp"
#include "group_sequence.hpp"
#include "graph_sequence.hpp"
#include "morph_host_sequence.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
template<class T> T read(std::istream& input) {
    T result{};
    if (!(input >> result)) throw std::runtime_error("Invalid or truncated animation diagnostic input");
    return result;
}
unsigned count(std::istream& input, unsigned limit) {
    const auto result = read<unsigned long long>(input);
    if (result > limit) throw std::runtime_error("Animation diagnostic count exceeds limit");
    return static_cast<unsigned>(result);
}
void write_state(std::ostream& out, const omega::AnimationChannelState& state, bool completed) {
    out << "{\"previous\":" << state.previous << ",\"current\":" << state.current
        << ",\"rate\":" << state.rate << ",\"target\":" << state.target << ",\"damping\":" << state.damping
        << ",\"curve\":[" << state.curve.start << ',' << state.curve.start_tangent << ','
        << state.curve.end << ',' << state.curve.end_tangent << ',' << state.curve.duration << ','
        << state.curve.inverse_duration << "],\"remaining\":" << state.remaining
        << ",\"wrapped\":" << (state.wrapped ? "true" : "false")
        << ",\"completed\":" << (completed ? "true" : "false") << '}';
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "Usage: omega_animation --input <synthetic channel cases>\n"
                         "       omega_animation --root-input <synthetic aggregate root frames>\n"
                         "       omega_animation --transition-input <synthetic transition records>\n"
                         "       omega_animation --slider-input <synthetic slider cases>\n"
                         "       omega_animation --group-input <group timing cases>\n"
                         "       omega_animation --graph-cases <graph frame cases>\n"
                         "Scalar-channel diagnostic with optional wrapping.\n";
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--graph-cases") {
            std::ostringstream report;
            omega::write_graph_cases(report,argv[2]);
            std::cout<<report.str();return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--morph-host-input") {
            std::ostringstream report;
            omega::write_morph_host_sequence(report,argv[2]);
            std::cout<<report.str();return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--root-input") {
            omega::write_root_sequence(std::cout, argv[2]);
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--transition-input") {
            omega::write_transition_sequence(std::cout, argv[2]);
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--slider-input") {
            omega::write_slider_sequence(std::cout, argv[2]);
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--group-input") {
            omega::write_group_sequence(std::cout, argv[2]);
            return 0;
        }
        if (argc != 3 || std::string(argv[1]) != "--input")
            throw std::runtime_error("Use --input <synthetic channel cases>");
        std::ifstream input(argv[2]);
        if (!input) throw std::runtime_error("Cannot open animation cases");
        if (read<std::string>(input) != "OMEGA_CHANNEL_CASES" || count(input, 1) != 1)
            throw std::runtime_error("Unsupported animation case format");
        const auto cases = count(input, 10000);
        unsigned total = 0;
        std::ostringstream report;
        report << std::setprecision(9) << "{\"cases\":[";
        for (unsigned i = 0; i < cases; ++i) {
            const auto name = read<std::string>(input);
            if (name.empty() || name.size() > 80 || name.find_first_not_of(
                    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
                throw std::runtime_error("Invalid animation case name");
            const auto operations = count(input, 10000);
            total += operations;
            if (total > 100000) throw std::runtime_error("Too many animation operations");
            omega::AnimationChannel channel;
            if (i) report << ',';
            report << "{\"name\":\"" << name << "\",\"states\":[";
            for (unsigned j = 0; j < operations; ++j) {
                const auto command = read<std::string>(input);
                bool completed = false;
                if (command == "SET") channel.set_value(read<float>(input));
                else if (command == "APPROACH" || command == "WRAP_APPROACH") {
                    const auto target = read<float>(input);
                    const auto step = read<float>(input);
                    channel.approach(target, step, command == "WRAP_APPROACH");
                } else if (command == "TARGET" || command == "WRAP_TARGET") {
                    const auto target = read<float>(input);
                    const auto duration = read<float>(input);
                    const auto damping = read<float>(input);
                    channel.set_target(target, duration, damping, command == "WRAP_TARGET");
                } else if (command == "STEP") completed = channel.step(read<float>(input));
                else if (command == "COPY") channel = channel.original_copy();
                else throw std::runtime_error("Unknown animation operation");
                if (j) report << ',';
                write_state(report, channel.state(), completed);
            }
            report << "]}";
        }
        std::string trailing;
        if (input >> trailing) throw std::runtime_error("Trailing animation diagnostic input");
        report << "]}\n";
        std::cout << report.str(); // Never publish partial success after a later failure.
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Animation diagnostic failed: " << error.what() << '\n';
        return 1;
    }
}
