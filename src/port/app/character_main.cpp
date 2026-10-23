#include "appearance.hpp"
#include "skeleton.hpp"
#include "animation_clip.hpp"
#include "animation_sampling.hpp"
#include "animation_root_clip.hpp"
#include "skeleton_pose.hpp"
#include "pose_sequence.hpp"
#include "skinned_mesh.hpp"
#include "animation_graph_manifest.hpp"
#include "clip_playback_sequence.hpp"
#include "animation_playback_sequence.hpp"
#include "bone_scales.hpp"
#include "skin_sequence.hpp"
#include "morph.hpp"
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
void reject_redirected_path(const std::filesystem::path& path) {
    std::filesystem::path prefix;
    for (const auto& component : path) {
        prefix /= component;
        if (!prefix.has_relative_path()) continue;
#ifdef _WIN32
        const auto attributes = GetFileAttributesW(prefix.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) continue;
            throw std::runtime_error("Cannot inspect character output path");
        }
        if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
            throw std::runtime_error("Character output path must not contain redirects");
#else
        std::error_code error;
        const auto status = std::filesystem::symlink_status(prefix, error);
        if (error && error != std::errc::no_such_file_or_directory)
            throw std::runtime_error("Cannot inspect character output path");
        if (std::filesystem::is_symlink(status))
            throw std::runtime_error("Character output path must not contain redirects");
#endif
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--help") {
            std::cout << "Usage: omega_character --input <local data> --output <local/report.json> [--skeleton [--bind-pose] | --clip --bones N]\n"
                         "Add --sample-phase <float> --skeleton-input <SKEL> with --clip for default non-root sampling.\n"
                         "Add --root-advance <normalized phase advance> for root sampling, one unit-weight contribution and an initial unparented root update.\n"
                         "Use --skeleton --pose-sequence <file> for retained non-root pose updates.\n"
                         "Use --skinned-mesh for character mesh packets, or --graph for an extracted graph manifest.\n"
                         "Add --skin-sequence <palette cases> to test skinning; --morph-input <SKF> --kind face|hand --shapes N --morph-target INDEX WEIGHT applies shape deltas first.\n"
                         "Use --morph --kind face|hand --shapes N --chunks N for SKF structure, --shape-names for BIN keys, --bone-scales for BON presets.\n"
                         "Use --clip --bones N --skeleton-input <SKEL> --playback-input <cases> for controlled clip samples.\n"
                         "Character data diagnostic; does not render or control a player.\n";
            return 0;
        }
        std::string input, output, skeleton_input, pose_sequence, playback_input,graph_input,playback_sequence,controller_sequence,skin_sequence,morph_kind,morph_input;
        std::vector<omega::SkinMorphTarget> morph_targets;
        std::optional<float> sample_phase;
        std::optional<float> root_advance;
        bool skeleton_mode = false;
        bool skinned_mode = false;
        bool graph_mode = false;
        bool scales_mode = false;
        bool morph_mode = false,shape_names_mode=false;
        bool clip_mode = false;
        bool bind_pose = false;
        unsigned bones = 0;
        unsigned shapes=0,chunks=0;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--input" && input.empty() && i + 1 < argc) input = argv[++i];
            else if (arg == "--output" && output.empty() && i + 1 < argc) output = argv[++i];
            else if (arg == "--skeleton" && !skeleton_mode) skeleton_mode = true;
            else if (arg == "--skinned-mesh" && !skinned_mode) skinned_mode = true;
            else if (arg == "--graph" && !graph_mode) graph_mode = true;
            else if (arg == "--bone-scales" && !scales_mode) scales_mode = true;
            else if (arg == "--morph" && !morph_mode) morph_mode = true;
            else if (arg == "--shape-names" && !shape_names_mode) shape_names_mode = true;
            else if (arg == "--kind" && morph_kind.empty() && i+1<argc) morph_kind=argv[++i];
            else if (arg == "--morph-input" && morph_input.empty() && i+1<argc) morph_input=argv[++i];
            else if(arg=="--morph-target" && i+2<argc){
                const std::string index=argv[++i],weight=argv[++i];omega::SkinMorphTarget target;
                const auto a=std::from_chars(index.data(),index.data()+index.size(),target.shape_index);
                const auto b=std::from_chars(weight.data(),weight.data()+weight.size(),target.weight);
                if(a.ec!=std::errc{}||a.ptr!=index.data()+index.size()||b.ec!=std::errc{}||b.ptr!=weight.data()+weight.size()||morph_targets.size()>=8)
                    throw std::runtime_error("Invalid morph target");
                morph_targets.push_back(target);
            }
            else if ((arg=="--shapes"||arg=="--chunks")&&i+1<argc){
                auto& value=arg=="--shapes"?shapes:chunks;
                if(value)throw std::runtime_error("Duplicate morph count");
                const std::string text=argv[++i];const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
                if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size()||!value||value>4096)
                    throw std::runtime_error("Invalid morph count");
            }
            else if (arg == "--clip" && !clip_mode) clip_mode = true;
            else if (arg == "--bind-pose" && !bind_pose) bind_pose = true;
            else if (arg == "--pose-sequence" && pose_sequence.empty() && i + 1 < argc) pose_sequence = argv[++i];
            else if (arg == "--skeleton-input" && skeleton_input.empty() && i + 1 < argc) skeleton_input = argv[++i];
            else if (arg == "--playback-input" && playback_input.empty() && i + 1 < argc) playback_input = argv[++i];
            else if (arg == "--graph-input" && graph_input.empty() && i + 1 < argc) graph_input=argv[++i];
            else if (arg == "--playback-sequence" && playback_sequence.empty() && i + 1 < argc) playback_sequence=argv[++i];
            else if (arg == "--controller-sequence" && controller_sequence.empty() && i + 1 < argc) controller_sequence=argv[++i];
            else if (arg == "--skin-sequence" && skin_sequence.empty() && i + 1 < argc) skin_sequence=argv[++i];
            else if (arg == "--sample-phase" && !sample_phase && i + 1 < argc) {
                const std::string text = argv[++i];
                float phase{};
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), phase);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                    throw std::runtime_error("Invalid sample phase");
                sample_phase = phase;
            }
            else if (arg == "--root-advance" && !root_advance && i + 1 < argc) {
                const std::string text = argv[++i];
                float advance{};
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), advance);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                    throw std::runtime_error("Invalid root phase advance");
                root_advance = advance;
            }
            else if (arg == "--bones" && bones == 0 && i + 1 < argc) {
                const std::string count = argv[++i];
                const auto parsed = std::from_chars(count.data(), count.data() + count.size(), bones);
                if (parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size() || !bones || bones > 32)
                    throw std::runtime_error("Clip bone count must be 1..32");
            }
            else throw std::runtime_error("Unknown, duplicate or incomplete character argument");
        }
        if (input.empty() || output.empty()) throw std::runtime_error("Both --input and --output are required");
        if(!skin_sequence.empty()&&!skinned_mode)throw std::runtime_error("Skin sequence requires --skinned-mesh");
        if(!morph_input.empty()&&!skinned_mode)throw std::runtime_error("Morph input requires skinned mesh mode");
        if(!morph_targets.empty()&&morph_input.empty())throw std::runtime_error("Morph targets require morph input");
        if((morph_mode||!morph_input.empty()) ? (morph_kind!="face"&&morph_kind!="hand")||!shapes||(morph_mode?!chunks:chunks!=0) : !morph_kind.empty()||shapes||chunks)
            throw std::runtime_error("Morph mode requires --kind face|hand --shapes N --chunks N");
        if (unsigned(skeleton_mode)+unsigned(clip_mode)+unsigned(skinned_mode)+unsigned(graph_mode)+unsigned(scales_mode)+unsigned(morph_mode)+unsigned(shape_names_mode)>1 ||
            clip_mode != (bones != 0) || (bind_pose && !skeleton_mode))
            throw std::runtime_error("Choose one character mode; --clip requires --bones");
        if ((sample_phase.has_value() || !playback_input.empty()) != !skeleton_input.empty() ||
            ((sample_phase || !playback_input.empty()) && !clip_mode) || (sample_phase && !playback_input.empty()))
            throw std::runtime_error("Sampling requires --clip, --sample-phase and --skeleton-input together");
        if (root_advance && !sample_phase)
            throw std::runtime_error("Root phase advance requires clip sampling inputs");
        if (!pose_sequence.empty() && (!skeleton_mode || bind_pose))
            throw std::runtime_error("Pose sequence requires skeleton mode without --bind-pose");
        if ((!controller_sequence.empty()&&!playback_sequence.empty()) ||
            graph_input.empty()!=(playback_sequence.empty()&&controller_sequence.empty())||(!graph_input.empty()&&
            (!skeleton_mode||bind_pose||!pose_sequence.empty())))
            throw std::runtime_error("Graph playback requires skeleton mode, graph input and playback sequence");
        namespace fs = std::filesystem;
        const auto root = fs::absolute(fs::current_path() / "local").lexically_normal();
        const auto destination = fs::absolute(output).lexically_normal();
        const auto relative = destination.lexically_relative(root);
        if (relative.empty() || relative.is_absolute() || *relative.begin() == ".." || destination == root)
            throw std::runtime_error("Character reports must stay under the workspace local directory");
        for (const auto& component : relative)
            if (component.string().find(':') != std::string::npos)
                throw std::runtime_error("Character output path must not contain stream names");
        reject_redirected_path(destination);
        auto aliases_destination = [&](const std::string& source_path) {
            return fs::absolute(source_path).lexically_normal() == destination ||
                (fs::exists(destination) && fs::equivalent(source_path, destination));
        };
        if (aliases_destination(input))
            throw std::runtime_error("Character output must not overwrite input");
        if (!skeleton_input.empty() && aliases_destination(skeleton_input))
            throw std::runtime_error("Character output must not overwrite skeleton input");
        if (!pose_sequence.empty() && aliases_destination(pose_sequence))
            throw std::runtime_error("Character output must not overwrite pose sequence input");
        if (!playback_input.empty() && aliases_destination(playback_input))
            throw std::runtime_error("Character output must not overwrite playback input");
        if(!morph_input.empty()&&aliases_destination(morph_input))throw std::runtime_error("Character output must not overwrite morph input");
        const auto size = fs::file_size(input);
        if (size > (clip_mode || graph_mode ? 64 : skinned_mode || morph_mode ? 16 : 1) * 1024 * 1024)
            throw std::runtime_error("Character input exceeds size limit");
        std::ifstream source(input, std::ios::binary);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (!source || !source.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
            throw std::runtime_error("Cannot read character input");
        std::ostringstream report;
        if (!controller_sequence.empty()) omega::write_animation_controller_sequence(report,omega::decode_skeleton(bytes),
            graph_input,controller_sequence,destination);
        else if (!graph_input.empty()) omega::write_animation_playback_sequence(report,omega::decode_skeleton(bytes),
            graph_input,playback_sequence,destination);
        else if (graph_mode) {
            std::istringstream manifest_text(std::string(bytes.begin(),bytes.end()));
            const auto manifest=omega::decode_animation_graph_manifest(manifest_text);
            report << "{\"nodes\":" << manifest.graph.nodes().size() << ",\"roots\":" << manifest.roots.size()
                << ",\"channels\":" << manifest.channel_count << ",\"selectors\":" << manifest.selector_count << '}';
        }
        else if (skinned_mode) {
            auto mesh=omega::decode_skinned_mesh(bytes);
            if(!morph_input.empty()){
                const auto morph_size=fs::file_size(morph_input);
                if(morph_size>16*1024*1024)throw std::runtime_error("Morph input size limit");
                std::ifstream morph_source(morph_input,std::ios::binary);
                std::vector<std::uint8_t> data(static_cast<std::size_t>(morph_size));
                if(!morph_source||!morph_source.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(morph_size)))
                    throw std::runtime_error("Cannot read morph input");
                const auto morph=omega::decode_morph(data,morph_kind=="face"?omega::MorphKind::face:omega::MorphKind::hand,
                    shapes,static_cast<unsigned>(mesh.chunks.size()));
                mesh=omega::deform_skinned_mesh(mesh,morph,morph_targets);
            }
            if(skin_sequence.empty())omega::write_skinned_mesh(report,mesh);
            else omega::write_skin_sequence(report,mesh,skin_sequence,destination);
        }
        else if (scales_mode) omega::write_bone_scales(report,omega::decode_bone_scales(bytes));
        else if (shape_names_mode) omega::write_shape_names(report,omega::decode_shape_names(bytes));
        else if (morph_mode) omega::write_morph(report,omega::decode_morph(bytes,
            morph_kind=="face"?omega::MorphKind::face:omega::MorphKind::hand,shapes,chunks));
        else if (!pose_sequence.empty()) omega::write_pose_sequence(report, omega::decode_skeleton(bytes), pose_sequence, destination);
        else if (sample_phase || !playback_input.empty()) {
            const auto skeleton_size = fs::file_size(skeleton_input);
            if (skeleton_size > 1024 * 1024) throw std::runtime_error("Skeleton input exceeds size limit");
            std::ifstream skeleton_source(skeleton_input, std::ios::binary);
            std::vector<std::uint8_t> skeleton_bytes(static_cast<std::size_t>(skeleton_size));
            if (!skeleton_source || !skeleton_source.read(reinterpret_cast<char*>(skeleton_bytes.data()),
                                                          static_cast<std::streamsize>(skeleton_size)))
                throw std::runtime_error("Cannot read sampling skeleton input");
            const auto clip = omega::decode_animation_clip(bytes, bones);
            const auto skeleton = omega::decode_skeleton(skeleton_bytes);
            if (!playback_input.empty()) omega::write_clip_playback_sequence(report,clip,skeleton,playback_input);
            else if (root_advance) {
                const auto sample = omega::sample_default_root_clip(clip, skeleton, *sample_phase, *root_advance);
                const std::array anchors{omega::RootAnchorDefinition{sample.anchor_key, {}}};
                const std::array contributions{omega::WeightedRootClipSample{1, sample, 0, false}};
                const auto accumulation = omega::accumulate_root_clip_contributions(anchors, contributions);
                auto root_state = omega::make_initial_root_animation();
                const auto root_output = omega::finalize_root_animation(root_state, accumulation.anchors[0].frame);
                report << "{\"sample\":";
                omega::write_root_clip_sample(report, sample);
                report << ",\"accumulation\":";
                omega::write_root_clip_accumulation(report, accumulation);
                report << ",\"root\":";
                omega::write_root_animation(report, root_state, root_output);
                report << '}';
            } else omega::write_nonroot_clip_sample(report,
                omega::sample_default_nonroot_bones(clip, skeleton, *sample_phase));
        }
        else if (clip_mode) omega::write_animation_clip(report, omega::decode_animation_clip(bytes, bones));
        else if (bind_pose) omega::write_skeleton_bind_pose(report, omega::skeleton_bind_pose(omega::decode_skeleton(bytes)));
        else if (skeleton_mode) omega::write_skeleton(report, omega::decode_skeleton(bytes));
        else omega::write_appearance(report, omega::pack_appearance(bytes));
        fs::create_directories(destination.parent_path());
        std::ofstream file(destination, std::ios::binary | std::ios::trunc);
        file << report.str() << '\n';
        file.close();
        if (!file) throw std::runtime_error("Cannot write character report");
        std::cout << "Character data diagnostic complete.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Character diagnostic failed: " << error.what() << '\n';
        return 1;
    }
}
