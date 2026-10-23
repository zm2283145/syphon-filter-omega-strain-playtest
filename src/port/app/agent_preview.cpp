#include "agent_preview.hpp"

#include "animation_pose.hpp"
#include "animation_root_clip.hpp"
#include "appearance.hpp"
#include "bone_scales.hpp"
#include "character_appearance.hpp"
#include "disc.hpp"
#include "model_overrides.hpp"
#include "skeleton.hpp"
#include "skeleton_pose.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>

namespace omega {

namespace {
const std::vector<std::string> frontend_manskm{"GAMEDATA", "FRONTEND", "MANSKM.HOG"};
const std::vector<std::string> frontend_morph{"GAMEDATA", "FRONTEND", "MORPH.HOG"};
const std::vector<std::string> frontend_mantex{"GAMEDATA", "FRONTEND", "MANTEX.HOG"};
const std::vector<std::string> frontend_skel{"GAMEDATA", "FRONTEND", "SKEL.HOG"};
const std::vector<std::string> common_skel{"GAMEDATA", "COMMON", "SKEL.HOG"};
// 0022a140: the front end's idle clip D_004A6340 into the bank D_004F77A0.
const std::vector<std::string> frontend_idle{"GAMEDATA", "FRONTEND", "ANIMIDLE.SKA"};

bool has(const std::vector<std::string>& names, const std::string& name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

// The quaternion basis of the rotation finalizer 003ba2b0 (animation_pose.cpp apply_basis), row 3 the translation.
std::array<float, 16> placement_rows(const RootVector& q, const RootVector& t) {
    const float x2 = q[0] + q[0], y2 = q[1] + q[1], z2 = q[2] + q[2];
    const float xx = q[0] * x2, yy = q[1] * y2, zz = q[2] * z2;
    const float xy = q[1] * x2, xz = q[2] * x2, yz = q[2] * y2;
    const float wx = q[3] * x2, wy = q[3] * y2, wz = q[3] * z2;
    return {1.0F - yy - zz, xy + wz, xz - wy, 0.0F, xy - wz, 1.0F - xx - zz, yz + wx, 0.0F,
            xz + wy, yz - wx, 1.0F - xx - yy, 0.0F, t[0], t[1], t[2], 1.0F};
}
}  // namespace

AgentPreviewRenderer::AgentPreviewRenderer(DiscImage& disc)
    : disc_(disc),
      manskm_(disc.archive_member_names(frontend_manskm)),
      morph_(disc.archive_member_names(frontend_morph)),
      mantex_(disc.archive_member_names(frontend_mantex)),
      skel_(disc.archive_member_names(frontend_skel)) {}

const SkinnedMesh* AgentPreviewRenderer::mesh(const std::string& member) {
    if (const auto it = meshes_.find(member); it != meshes_.end()) return it->second.get();
    std::unique_ptr<SkinnedMesh> m;
    if (has(manskm_, member)) m = std::make_unique<SkinnedMesh>(decode_skinned_mesh(disc_.archive_member(frontend_manskm, member)));
    else if (has(morph_, member)) m = std::make_unique<SkinnedMesh>(decode_skinned_mesh(disc_.archive_member(frontend_morph, member)));
    return meshes_.emplace(member, std::move(m)).first->second.get();
}

const Texture* AgentPreviewRenderer::texture(const std::string& member) {
    if (const auto it = textures_.find(member); it != textures_.end()) return it->second.get();
    std::unique_ptr<Texture> t;
    if (has(mantex_, member)) t = std::make_unique<Texture>(decode_tdx(disc_.archive_member(frontend_mantex, member)));
    return textures_.emplace(member, std::move(t)).first->second.get();
}

void AgentPreviewRenderer::build(const AgentPacking& packing) {
    parts_.clear();
    drawn_.clear();
    skipped_.clear();
    AppearancePacking ap;
    std::copy(packing.begin(), packing.end(), ap.bytes.begin());
    ap.initialized.fill(true);
    const CharacterAppearance look = resolve_character_appearance_custom(ap);
    skeleton_ = decode_skeleton(has(skel_, look.skeleton) ? disc_.archive_member(frontend_skel, look.skeleton)
                                                          : disc_.archive_member(common_skel, look.skeleton));
    std::vector<SkinMatrix> world;
    inverse_.clear();
    for (const auto& bone : skeleton_bind_pose(skeleton_)) {
        world.push_back(bone.model);
        inverse_.push_back(bone.inverse_bind);
    }
    scales_.clear();
    const std::string bon = look.bone_scale_preset + ".BON";
    if (look.bone_scale_preset != "BONENOSCALE" && has(manskm_, bon))
        scales_ = bone_scale_matrices(decode_bone_scales(disc_.archive_member(frontend_manskm, bon)));
    const auto palette = make_skin_palette(world, inverse_, scales_);
    std::vector<std::string> own_keys, common_keys;   // 132md3: per part, its front-end key and the COMMON one
    for (const auto& part : look.parts) {
        if (part.mesh.blank) continue;
        const SkinnedMesh* m = mesh(part.mesh.members[0]);
        if (!m) {
            skipped_.push_back(part.mesh.members[0]);
            continue;
        }
        Part p;
        p.mesh = m;
        p.geometry = skin_skinned_mesh(*m, palette, SkinMorphContext{0});  // the base shape (no morph targets), as the play
        p.image = part.texture.blank ? nullptr : texture(part.texture.members[0]);
        if (p.image) p.texture = part.texture.members[0];
        parts_.push_back(std::move(p));
        drawn_.push_back(part.mesh.members[0] + (parts_.back().image ? " / " + part.texture.members[0] : " / untextured"));
        const bool skm = has(manskm_, part.mesh.members[0]);
        own_keys.push_back(mods::part_key(skm ? frontend_manskm : frontend_morph, part.mesh.members[0]));
        common_keys.push_back(mods::part_key({"GAMEDATA", "COMMON", skm ? "MANSKM.HOG" : "MORPH.HOG"}, part.mesh.members[0]));
    }
    // 132md3 (port-only): the mods' replacements, by the front end's own key first, else the play session's COMMON key
    // (the front end's HOGs hold copies of the same members). The skeleton name names its HOG (the cache is per name).
    replaced_.clear();
    const std::string skeleton_name = (has(skel_, look.skeleton) ? "FRONTEND/" : "") + look.skeleton;
    const auto own = mods::character_model_overrides(skeleton_, skeleton_name, own_keys, {});
    std::vector<std::string> rest = common_keys;
    for (std::size_t i = 0; own && i < rest.size(); ++i)
        if (own->lod0[i]) rest[i].clear();
    const auto common = mods::character_model_overrides(skeleton_, skeleton_name, rest, {});
    for (std::size_t i = 0; i < parts_.size(); ++i) {
        const bool first = own && own->lod0[i];
        auto model = first ? own->lod0[i] : common ? common->lod0[i] : nullptr;
        if (!model) continue;
        auto& p = parts_[i];
        const auto& key = first ? own_keys[i] : common_keys[i];
        gfx::skin_replacement_into(*model, palette, p.geometry);
        if (model->image) {
            p.model_image = std::make_unique<Texture>(mods::model_image_texture(*model->image, p.image ? *p.image : Texture{}));
            p.image = p.model_image.get();
            p.texture = "mods:" + key;
        }
        p.model = std::move(model);
        replaced_.push_back(key);
    }
    if (!idle_ || idle_->bone_count != skeleton_.bones.size())
        idle_ = decode_animation_clip(disc_.read(disc_.file(frontend_idle)),
                                      static_cast<std::uint32_t>(skeleton_.bones.size()));
    posed_frame_ = -1;
    built_ = packing;
}

// The idle clip at seconds (looping over frame_intervals x frame_period; ANIMIDLE: 357 x 1/60 s); a fresh
// retained pose per sample. Bone 0 is the clip's root pose under its anchor: ANIMIDLE's root channel names the
// anchor key "" (zero), the key 0028d870 reads from 004a9a40 to look the anchor up (003ae5e0) and place it with
// the camera's model rows (191100). The anchor is the model origin here (agent_figure_draw applies those rows),
// so the pelvis stands at the clip's 3.42 above it; the root motion is not applied (the draw re-places the
// anchor every frame).
void AgentPreviewRenderer::pose(float seconds) {
    const float duration = static_cast<float>(idle_->frame_intervals) * idle_->frame_period;
    float t = std::fmod(std::max(seconds, 0.0F), duration);
    if (!(t >= 0.0F && t < duration)) t = 0.0F;
    const auto frame = static_cast<std::int64_t>(t / idle_->frame_period);
    if (frame == posed_frame_) return;
    const float phase = std::min(t / duration, std::nextafter(1.0F, 0.0F));
    NonRootPose nonroot = make_initial_nonroot_pose(skeleton_);
    const WeightedNonRootSample sample{1.0F, sample_default_nonroot_bones(*idle_, skeleton_, phase)};
    update_nonroot_pose(nonroot, std::span<const WeightedNonRootSample>(&sample, 1));
    const RootClipSample root = sample_default_root_clip(*idle_, skeleton_, phase, 0.0F);
    if (root.anchor_key != RootAnchorKey{})
        throw std::runtime_error("0028d870: the front-end idle clip's root anchor is not the key \"\" of 004a9a40");
    const std::array anchors{RootAnchorDefinition{root.anchor_key, {}}};
    const std::array contributions{WeightedRootClipSample{1.0F, root, 0, false}};
    const auto accumulation = accumulate_root_clip_contributions(anchors, contributions);
    RootAnimationState anchor = make_initial_root_animation();
    (void)finalize_root_animation(anchor, accumulation.anchors.at(0).frame);
    std::vector<std::array<float, 16>> locals{placement_rows(anchor.pose_orientation, anchor.pose_position)};
    for (const auto& bone : nonroot.bones) locals.push_back(bone.local);
    const auto world = skeleton_world_pose(skeleton_, locals);
    const auto palette = make_skin_palette(world, inverse_, scales_);
    for (auto& p : parts_) {
        p.geometry = skin_skinned_mesh(*p.mesh, palette, SkinMorphContext{0});
        if (p.model) gfx::skin_replacement_into(*p.model, palette, p.geometry);   // 132md3 (port-only)
    }
    posed_frame_ = frame;
}

std::vector<AgentPreviewRenderer::PosedPart> AgentPreviewRenderer::posed(const AgentPacking& packing, float seconds) {
    if (!built_ || *built_ != packing) build(packing);
    pose(seconds);
    std::vector<PosedPart> out;
    for (const auto& p : parts_) out.push_back({&p.geometry, p.image, p.texture});
    return out;
}

}  // namespace omega
