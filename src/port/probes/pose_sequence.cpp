#include "pose_sequence.hpp"
#include "animation_clip.hpp"
#include "animation_sampling.hpp"
#include "animation_pose.hpp"
#include <fstream>
#include <iomanip>
#include <map>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
bool same_file(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (std::filesystem::absolute(a).lexically_normal() == std::filesystem::absolute(b).lexically_normal()) return true;
    std::error_code error;
    const bool equal = std::filesystem::equivalent(a, b, error);
    return !error && equal;
}
}
void write_pose_sequence(std::ostream& output, const SkeletonDefinition& skeleton,
                         const std::filesystem::path& path, const std::filesystem::path& destination) {
    if (same_file(path, destination)) throw std::runtime_error("Pose output would overwrite sequence input");
    if (std::filesystem::file_size(path) > 1024*1024) throw std::runtime_error("Pose sequence input exceeds limit");
    std::ifstream input(path);
    std::string magic;
    unsigned version{}, steps{};
    if (!(input >> magic >> version >> steps) || magic != "OMEGA_POSE_SEQUENCE" || version != 1 || !steps || steps > 256)
        throw std::runtime_error("Invalid pose sequence header");
    auto pose = make_initial_nonroot_pose(skeleton);
    std::map<std::filesystem::path, AnimationClip> clips;
    std::uint64_t input_bytes{}, decoded_slots{};
    output << "{\"scope\":\"Default non-root retained pose; explicit diagnostic phases and weights\",\"initial\":";
    write_nonroot_pose(output, pose);
    output << ",\"steps\":[";
    for (unsigned step=0; step<steps; ++step) {
        unsigned count{};
        if (!(input >> count) || count > 32) throw std::runtime_error("Invalid pose contribution count");
        std::vector<WeightedNonRootSample> contributions;
        contributions.reserve(count);
        for (unsigned c=0; c<count; ++c) {
            std::string clip_path;
            float phase{}, weight{};
            if (!(input >> std::quoted(clip_path) >> phase >> weight) || clip_path.empty() || clip_path.size()>4096)
                throw std::runtime_error("Invalid pose contribution");
            const auto key = std::filesystem::absolute(clip_path).lexically_normal();
            if (same_file(key, destination)) throw std::runtime_error("Pose output would overwrite clip input");
            auto found = clips.find(key);
            if (found == clips.end()) {
                const auto size = std::filesystem::file_size(key);
                if (size > 64*1024*1024 || input_bytes+size > 128*1024*1024 || clips.size()>=64)
                    throw std::runtime_error("Pose clip inputs exceed safety limits");
                input_bytes += size;
                std::ifstream source(key, std::ios::binary);
                std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
                if (!source || !source.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
                    throw std::runtime_error("Cannot read pose clip input");
                auto clip = decode_animation_clip(bytes, static_cast<std::uint32_t>(skeleton.bones.size()));
                decoded_slots += clip.frames.size()*clip.bone_count;
                if (decoded_slots>1000000) throw std::runtime_error("Pose decoded clip count exceeds safety limit");
                found = clips.emplace(key, std::move(clip)).first;
            }
            contributions.push_back({weight, sample_default_nonroot_bones(found->second, skeleton, phase)});
        }
        update_nonroot_pose(pose, contributions);
        if (step) output << ',';
        write_nonroot_pose(output, pose);
    }
    std::string trailing;
    if (input >> trailing) throw std::runtime_error("Unexpected trailing pose sequence data");
    if (!input.eof()) throw std::runtime_error("Cannot finish reading pose sequence");
    output << "]}";
}
}
