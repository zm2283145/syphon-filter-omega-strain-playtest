#pragma once
// PORT-ONLY (Checkpoint 132cc4b): the agent screens' model parts. The original draws the front end's
// model actor (GuiAgentModify 00341f00, GuiAgentCreate 0028d870); the port skins the edited packing
// on the CPU (the parts resolve_character_appearance_custom names, read from GAMEDATA/FRONTEND's
// MANSKM.HOG / MORPH.HOG / MANTEX.HOG, the skeleton from SKEL.HOG, the .BON bone scales).
// Checkpoint 132cc5: posed by the front end's idle clip GAMEDATA\FRONTEND\ANIMIDLE.SKA (the clip
// 0022a140 loads into the front end's bank D_004F77A0 for the model actor), sampled at the
// caller's seconds (looping, no root motion).
// Checkpoint 132cc6: the parts are drawn by the host as figure batches through the agent screens'
// camera (agent_camera.hpp, frontend_disc.cpp agent_figure_draw), in the packing's pair order (the
// MANSORT2 order 00390570 left); the port's own framing, turn and light are gone.
// Checkpoint 132md3 (port-only, research/MODDING.md "Front-end figures"): a mod's character model replacement of a
// part (models\GAMEDATA\FRONTEND\<HOG>\<member>.gltf, else the same member's COMMON key, the play session's) is
// skinned by the same palette (bone scales included) in its place, with its glTF image; no mods: unchanged.
#include "agent_modify.hpp"
#include "animation_clip.hpp"
#include "character_model.hpp"
#include "skeleton.hpp"
#include "skinned_mesh.hpp"
#include "texture.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace omega {

class DiscImage;

class AgentPreviewRenderer {
public:
    explicit AgentPreviewRenderer(DiscImage& disc);
    // The packing's parts posed at `seconds` (the model frame, Z up), in the pairs' order; `image`
    // null and `texture` empty: a part without a texture member.
    struct PosedPart {
        const SkinnedMeshGeometry* geometry = nullptr;
        const Texture* image = nullptr;
        std::string texture;
    };
    std::vector<PosedPart> posed(const AgentPacking& packing, float seconds);
    // The idle clip frame the last render posed (-1: the bind pose, no clip).
    std::int64_t posed_frame() const { return posed_frame_; }
    // The parts the last render drew and skipped (missing members), for the log.
    const std::vector<std::string>& drawn() const { return drawn_; }
    const std::vector<std::string>& skipped() const { return skipped_; }
    // 132md3 (port-only): the parts a mod replaced (their model keys), for the log.
    const std::vector<std::string>& replaced() const { return replaced_; }

private:
    struct Part {
        const SkinnedMesh* mesh = nullptr;
        SkinnedMeshGeometry geometry;
        const Texture* image = nullptr;   // nullptr: untextured
        std::string texture;
        // Checkpoint 132md3 (port-only, research/MODDING.md "Front-end figures"): a mod's replacement of the part,
        // skinned in its place, and its glTF image (the part's texture with the image as its GPU replacement).
        std::shared_ptr<const gfx::ReplacementModel> model;
        std::unique_ptr<Texture> model_image;
    };
    const Texture* texture(const std::string& member);
    const SkinnedMesh* mesh(const std::string& member);
    void build(const AgentPacking& packing);
    void pose(float seconds);

    DiscImage& disc_;
    std::vector<std::string> manskm_, morph_, mantex_, skel_;
    std::map<std::string, std::unique_ptr<SkinnedMesh>> meshes_;
    std::map<std::string, std::unique_ptr<Texture>> textures_;
    std::optional<AgentPacking> built_;
    std::vector<Part> parts_;
    SkeletonDefinition skeleton_;
    std::vector<SkinMatrix> inverse_, scales_;
    std::optional<AnimationClip> idle_;
    std::int64_t posed_frame_ = -2;
    std::vector<std::string> drawn_, skipped_, replaced_;
};

}  // namespace omega
