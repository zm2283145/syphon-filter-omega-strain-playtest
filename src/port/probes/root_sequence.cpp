#include "root_sequence.hpp"
#include "animation_root.hpp"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
template<class T> T read(std::istream& input) {
    T value{};
    if (!(input >> value)) throw std::runtime_error("Invalid or truncated root diagnostic input");
    return value;
}
unsigned count(std::istream& input, unsigned limit) {
    const auto value = read<unsigned long long>(input);
    if (value > limit) throw std::runtime_error("Root diagnostic count exceeds limit");
    return static_cast<unsigned>(value);
}
RootVector vector(std::istream& input) {
    RootVector value{};
    for (auto& component : value) component = read<float>(input);
    return value;
}
}

void write_root_sequence(std::ostream& out, const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 8 * 1024 * 1024)
        throw std::runtime_error("Root diagnostic input must be a regular file no larger than 8 MiB");
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open root diagnostic input");
    if (read<std::string>(input) != "OMEGA_ROOT_CASES" || count(input, 1) != 1)
        throw std::runtime_error("Unsupported root diagnostic format");
    const auto cases = count(input, 128);
    if (!cases) throw std::runtime_error("Root diagnostic requires at least one case");
    std::ostringstream report;
    report << std::setprecision(9)
        << "{\"scope\":\"Unparented root finalizer; explicit aggregate frames and portable math approximations\",\"cases\":[";
    unsigned total = 0;
    for (unsigned i = 0; i < cases; ++i) {
        const auto name = read<std::string>(input);
        if (name.empty() || name.size() > 80 || name.find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
            throw std::runtime_error("Invalid root diagnostic case name");
        const auto operations = count(input, 1024);
        total += operations;
        if (total > 8192) throw std::runtime_error("Too many root diagnostic operations");
        auto state = make_initial_root_animation();
        if (i) report << ',';
        report << "{\"name\":\"" << name << "\",\"initial\":";
        write_root_animation(report, state, {});
        report << ",\"steps\":[";
        for (unsigned j = 0; j < operations; ++j) {
            const auto operation = read<std::string>(input);
            RootAnimationOutput result;
            if (operation == "PLACE") {
                const auto orientation = vector(input);
                const auto position = vector(input);
                set_root_placement(state, orientation, position);
            } else if (operation == "STEP") {
                RootFrame frame;
                frame.weight = read<float>(input);
                frame.angle = read<float>(input);
                frame.pose_quaternion = vector(input);
                frame.pose_translation = vector(input);
                for (auto& bucket : frame.motion_buckets) bucket = vector(input);
                RootMotionConstraint constraint;
                const auto mode = read<std::string>(input);
                if (mode == "none") constraint.kind = RootConstraintKind::none;
                else if (mode == "reject_axis") constraint.kind = RootConstraintKind::reject_axis;
                else if (mode == "keep_axis") constraint.kind = RootConstraintKind::keep_axis;
                else throw std::runtime_error("Unsupported root motion constraint");
                constraint.axis = vector(input);
                result = finalize_root_animation(state, frame, constraint);
            } else throw std::runtime_error("Unsupported root diagnostic operation");
            if (j) report << ',';
            write_root_animation(report, state, result);
        }
        report << "]}";
    }
    std::string trailing;
    if (input >> trailing) throw std::runtime_error("Trailing root diagnostic input");
    report << "]}\n";
    out << report.str(); // Publish only after every operation validates.
}
}
