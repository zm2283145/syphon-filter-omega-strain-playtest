#include "float_bits.hpp"
#include "animated_character.hpp"
#include "skeleton_pose.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace omega {
namespace {
// Attachment publication is the last operation which may reject. Every
// subsequent owned-state publication below must be nonthrowing.
static_assert(std::is_nothrow_move_assignable_v<AnimationModel>);
static_assert(std::is_nothrow_move_assignable_v<MorphHostState>);
static_assert(std::is_nothrow_move_assignable_v<std::vector<AnimatedCharacterGeometry>>);
static_assert(std::is_nothrow_copy_assignable_v<AnimationRotationCurveState>);
static_assert(std::is_nothrow_copy_assignable_v<ActorCurveUpdateState>);
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
float finite(float value){check(::omega::float_bits::finite(value),"Character world palette arithmetic overflow");return value;}
SkinMatrix compose_world(const SkinMatrix& world,const SkinMatrix& bone){
    SkinMatrix result{};
    for(unsigned column=0;column<4;++column)for(unsigned row=0;row<4;++row){
        float value=finite(world[row]*bone[column*4]);
        for(unsigned axis=1;axis<4;++axis)value=finite(value+finite(world[axis*4+row]*bone[column*4+axis]));
        result[column*4+row]=value;
    }
    return result;
}
}
AnimatedCharacter::AnimatedCharacter(SkeletonDefinition skeleton,const AnimationGraphManifest& manifest,
    std::vector<AnimatedCharacterPart> parts,MorphHostConfig morph_config,std::vector<SkinMatrix> scales)
    :skeleton_(std::move(skeleton)),parts_(std::move(parts)),morph_config_(std::move(morph_config)),
     scales_(std::move(scales)),model_(skeleton_,manifest){
    check_parts(parts_);
    for(const auto& bone:skeleton_bind_pose(skeleton_))inverse_bind_.push_back(bone.inverse_bind);
    geometry_=build_geometry(model_.player(),morph_);
}
void AnimatedCharacter::check_parts(const std::vector<AnimatedCharacterPart>& parts)const{
    check(!parts.empty()&&parts.size()<=64,"Invalid animated character part count");
    std::uint64_t vertices=0;
    for(const auto& part:parts){
        check(part.mesh.bone_count==skeleton_.bones.size(),"Character part skeleton count differs");
        check(part.binding==CharacterMorphBinding::none||part.binding==CharacterMorphBinding::face||
              part.binding==CharacterMorphBinding::hand0||part.binding==CharacterMorphBinding::hand1,"Unknown character morph binding");
        check(part.morph.has_value()==(part.binding!=CharacterMorphBinding::none),"Character morph resource/binding mismatch");
        if(part.morph)check(part.morph->kind==(part.binding==CharacterMorphBinding::face?MorphKind::face:MorphKind::hand),"Character morph kind differs from binding");
        for(const auto& chunk:part.mesh.chunks)vertices+=chunk.vertices.size();
    }
    check(vertices<=2000000,"Animated character vertex limit");
}
void AnimatedCharacter::replace_parts(std::vector<AnimatedCharacterPart> parts){
    require_idle();
    check_parts(parts);
    auto previous=std::move(parts_);
    parts_=std::move(parts);
    try{geometry_=build_geometry(model_.player(),morph_);geometry_stale_=false;}
    catch(...){parts_=std::move(previous);throw;}
}
void AnimatedCharacter::require_idle()const{check(!candidate_,"Animated character frame already active");}
void AnimatedCharacter::write_node_locals(std::span<const std::optional<std::array<float,12>>> rows,const std::optional<RootVector>& root_translation){
    require_idle();
    auto next=model_.clone_for_transaction();
    next.write_node_locals(skeleton_,rows,root_translation);
    if(deferred_skinning_){model_=std::move(next);geometry_stale_=true;return;}   // 132vt2d: skinned at the next geometry()
    auto next_geometry=build_geometry(next.player(),morph_);
    model_=std::move(next);geometry_=std::move(next_geometry);
}
// 132vt2d: the commands change channels, selectors, the controller and anchors, never the skinning's inputs
// (the player's world, orientation and position change only in AnimationModel::step and write_node_locals).
AnimationModel& AnimatedCharacter::command_model(){return candidate_?*candidate_:model_;}
// 132vt2d: a deferred skinning runs on the pose and morph it was deferred on, before either changes outside a step.
void AnimatedCharacter::materialize_geometry()const{
    build_geometry_into(model_.player(),morph_,spare_geometry_,morph_scratch_);
    geometry_.swap(spare_geometry_);geometry_stale_=false;
}
void AnimatedCharacter::materialize_before_change(){if(geometry_stale_)materialize_geometry();}
namespace {
// 132vt2d: the morph state's fields build_geometry_into reads (morph_face_targets / morph_hand_targets), bit for bit.
bool same_skin_morph(const MorphHostState& a,const MorphHostState& b){
    const auto same=[](float x,float y){return std::bit_cast<std::uint32_t>(x)==std::bit_cast<std::uint32_t>(y);};
    if(a.active_count!=b.active_count)return false;
    for(std::size_t i=0;i<a.active_count&&i<a.face_slots.size();++i)
        if(a.face_slots[i].shape_index!=b.face_slots[i].shape_index||!same(a.face_slots[i].weight,b.face_slots[i].weight))return false;
    for(std::size_t h=0;h<a.grips.size();++h){
        const auto& x=a.grips[h];const auto& y=b.grips[h];
        if(x.primary_shape!=y.primary_shape||x.secondary_shape!=y.secondary_shape||!same(x.primary_weight,y.primary_weight)||!same(x.secondary_weight,y.secondary_weight))return false;
    }
    return true;
}
}
void AnimatedCharacter::set_morph_state(const MorphHostState& state){
    require_idle();
    if(geometry_stale_&&!same_skin_morph(morph_,state))materialize_geometry();
    morph_=state;
}
void AnimatedCharacter::set_base_shape(std::uint8_t shape,bool enabled){require_idle();materialize_before_change();set_morph_base_shape(morph_,shape,enabled);}
void AnimatedCharacter::start_speech(const MorphSpeechTracks& tracks,std::int32_t start_ms){require_idle();materialize_before_change();start_morph_speech(morph_,tracks,start_ms);}
void AnimatedCharacter::stop_speech(){require_idle();materialize_before_change();morph_.speech_active=false;}
void AnimatedCharacter::set_value(std::size_t channel,float value){command_model().set_value(channel,value);}
void AnimatedCharacter::approach(std::size_t channel,float target,float maximum_step,bool wrapped){command_model().approach(channel,target,maximum_step,wrapped);}
void AnimatedCharacter::set_target(std::size_t channel,float target,float duration,float damping,bool wrapped){command_model().set_target(channel,target,duration,damping,wrapped);}
void AnimatedCharacter::select(std::size_t selector,std::size_t value){command_model().select(selector,value);}
void AnimatedCharacter::set_playback_rate(float rate){command_model().set_playback_rate(rate);}
void AnimatedCharacter::apply_commands(std::span<const ActorChannelCommand> commands){command_model().apply_commands(commands);}
void AnimatedCharacter::play(std::string_view motion,MotionInstallMode mode,std::optional<float> duration){command_model().play(motion,mode,duration);}
void AnimatedCharacter::play(const MotionRequest& request,MotionInstallMode mode){command_model().play(request,mode);}
void AnimatedCharacter::queue(std::string_view motion,std::optional<float> duration){command_model().queue(motion,duration);}
void AnimatedCharacter::clear(){command_model().clear();}
std::vector<ActorAttachmentChange> AnimatedCharacter::controller_with_attachments(
    ActorAttachmentDraft& draft,ActorAttachmentRuntime& owner,const std::function<void(AnimationModel&)>& operation){
    require_idle();materialize_before_change();auto next_model=model_.clone_for_transaction();
    struct Reset {AnimationModel*& slot;~Reset(){slot=nullptr;}} reset{candidate_};candidate_=&next_model;
    operation(next_model);
    auto changes=draft.commit(owner);
    model_=std::move(next_model);
    return changes;
}
std::vector<ActorAttachmentChange> AnimatedCharacter::play_with_attachments(ActorAttachmentDraft& draft,
    ActorAttachmentRuntime& owner,const MotionRequest& request,MotionInstallMode mode){
    return controller_with_attachments(draft,owner,[&](AnimationModel& model){model.play(request,mode);});
}
std::vector<ActorAttachmentChange> AnimatedCharacter::play_with_attachments(ActorAttachmentDraft& draft,
    ActorAttachmentRuntime& owner,std::string_view motion,MotionInstallMode mode,std::optional<float> duration){
    return controller_with_attachments(draft,owner,[&](AnimationModel& model){model.play(motion,mode,duration);});
}
std::vector<ActorAttachmentChange> AnimatedCharacter::clear_with_attachments(ActorAttachmentDraft& draft,ActorAttachmentRuntime& owner){
    return controller_with_attachments(draft,owner,[](AnimationModel& model){model.clear();});
}
void AnimatedCharacter::set_delivery_hooks(MotionDeliveryHooks hooks){command_model().set_delivery_hooks(std::move(hooks));}
void AnimatedCharacter::set_anchor_placement(const RootAnchorKey& key,const RootVector& q,const RootVector& p){command_model().set_anchor_placement(key,q,p);}
void AnimatedCharacter::set_anchor_world_orientation(const RootAnchorKey& key,const RootVector& q,bool reset_counter){command_model().set_anchor_world_orientation(key,q,reset_counter);}
void AnimatedCharacter::set_anchor_world_position(const RootAnchorKey& key,const RootVector& position){command_model().set_anchor_world_position(key,position);}
bool AnimatedCharacter::play_actor_motion(const ActorMotionRegistry& registry,const ActorMotionSelection& selection,
    ActorMotionTransitionState& transition,const MotionRequest& payload,MotionInstallMode mode){
    return command_model().play_actor_motion(registry,selection,transition,payload,mode);
}
void AnimatedCharacter::set_anchor_world_placement(const RootAnchorKey& key,const RootParentTransform& placement,bool reset_counter){command_model().set_anchor_world_placement(key,placement,reset_counter);}
void AnimatedCharacter::set_anchor_state(const RootAnchorKey& key,const RootAnimationState& state){command_model().set_anchor_state(key,state);}
void AnimatedCharacter::set_anchor_parent(const RootAnchorKey& key,std::optional<RootParentTransform> parent){command_model().set_anchor_parent(key,std::move(parent));}
void AnimatedCharacter::set_anchor_constraint(const RootAnchorKey& key,std::optional<RootMotionConstraint> constraint){command_model().set_anchor_constraint(key,std::move(constraint));}
std::vector<SkinPaletteBone> AnimatedCharacter::placed_palette(const AnimationPlayerState& player)const{
    auto palette=make_skin_palette(player.world,inverse_bind_,scales_);
    const auto placement=root_placement_matrix({player.orientation,player.position});
    auto normal_placement=placement;normal_placement[12]=normal_placement[13]=normal_placement[14]=0;
    normal_placement[15]=1;
    // 003ab510 publishes a quaternion-to-matrix basis. 003a98d0 composes
    // incoming position/normal matrices before vertex/influence evaluation.
    // This world-space path omits the original camera/projection/lighting.
    for(auto& bone:palette){
        // 132vc1 hw6 (port-side speed only): the same products on NEON where available; any doubt: compose_world.
        SkinMatrix m;
        if(skin_matrix_multiply_neon(placement,bone.position,m))bone.position=m;else bone.position=compose_world(placement,bone.position);
        if(skin_matrix_multiply_neon(normal_placement,bone.normal,m))bone.normal=m;else bone.normal=compose_world(normal_placement,bone.normal);
    }
    return palette;
}
std::vector<AnimatedCharacterGeometry> AnimatedCharacter::build_geometry(const AnimationPlayerState& player,const MorphHostState& morph)const{
    std::vector<AnimatedCharacterGeometry> result;std::vector<SkinnedMesh> scratch;
    build_geometry_into(player,morph,result,scratch);
    return result;
}
// Checkpoint 132win10 (port-side speed only): the per-frame skinning written into a retained part list (the
// previous frame's geometry, swapped back in by step_impl) so the vectors' capacity is reused.
void AnimatedCharacter::build_geometry_into(const AnimationPlayerState& player,const MorphHostState& morph,
    std::vector<AnimatedCharacterGeometry>& result,std::vector<SkinnedMesh>& morph_scratch)const{
    const auto palette=placed_palette(player);
    result.resize(parts_.size());
    // 132vc1 hw6 (port-side speed only): a deform scratch per part, so the face's and the hands' deformed copies
    // (deform_skinned_mesh_into: result = mesh) reuse their own storage every frame instead of reallocating it.
    if(morph_scratch.size()<parts_.size())morph_scratch.resize(parts_.size());
    for(std::size_t i=0;i<parts_.size();++i){
        const auto& part=parts_[i];
        auto& output=result[i];
        output.name=part.name;
        if(part.binding==CharacterMorphBinding::none)skin_skinned_mesh_into(part.mesh,palette,SkinMorphContext{0},output.mesh);
        else if(part.binding==CharacterMorphBinding::face)skin_morphed_mesh_into(part.mesh,palette,*part.morph,morph_face_targets(morph),morph_scratch[i],output.mesh);
        else skin_morphed_mesh_into(part.mesh,palette,*part.morph,morph_hand_targets(morph,part.binding==CharacterMorphBinding::hand0?0:1),morph_scratch[i],output.mesh);
    }
}
void AnimatedCharacter::timed_skinning(std::uint64_t (*clock_us)(),std::vector<AnimatedCharacterGeometry>& result,
    std::vector<SkinnedMesh>& scratch,std::uint64_t (&us)[3])const{
    us[0]=us[1]=us[2]=0;
    const auto t0=clock_us();
    const auto palette=placed_palette(model_.player());
    us[0]=clock_us()-t0;
    result.resize(parts_.size());
    if(scratch.size()<parts_.size())scratch.resize(parts_.size());
    for(std::size_t i=0;i<parts_.size();++i){
        const auto& part=parts_[i];
        auto& output=result[i];
        output.name=part.name;
        const auto t1=clock_us();
        if(part.binding==CharacterMorphBinding::none)skin_skinned_mesh_into(part.mesh,palette,SkinMorphContext{0},output.mesh);
        else if(part.binding==CharacterMorphBinding::face)skin_morphed_mesh_into(part.mesh,palette,*part.morph,morph_face_targets(morph_),scratch[i],output.mesh);
        else skin_morphed_mesh_into(part.mesh,palette,*part.morph,morph_hand_targets(morph_,part.binding==CharacterMorphBinding::hand0?0:1),scratch[i],output.mesh);
        us[part.binding==CharacterMorphBinding::none?1:2]+=clock_us()-t1;
    }
}
std::vector<AnimatedCharacterGeometry> AnimatedCharacter::skin_parts(std::span<const SkinnedMesh* const> meshes)const{
    std::vector<AnimatedCharacterGeometry> result;
    skin_parts(meshes,result);
    return result;
}
void AnimatedCharacter::skin_parts(std::span<const SkinnedMesh* const> meshes,std::vector<AnimatedCharacterGeometry>& result)const{
    if(meshes.size()!=parts_.size())throw std::runtime_error("AnimatedCharacter::skin_parts: one mesh per part");
    const auto palette=placed_palette(model_.player());
    result.resize(parts_.size());
    for(std::size_t i=0;i<parts_.size();++i){
        result[i].name=parts_[i].name;
        if(meshes[i])skin_skinned_mesh_into(*meshes[i],palette,SkinMorphContext{0},result[i].mesh);
        else{result[i].mesh.vertices.clear();result[i].mesh.triangles.clear();result[i].mesh.triangle_chunks.clear();}
    }
}
// Port-only (checkpoint 132md2): the palette skin_parts uses, for the mod layer's replacement models.
std::vector<SkinPaletteBone> AnimatedCharacter::committed_palette()const{return placed_palette(model_.player());}
void AnimatedCharacter::step(const AnimationGraph& graph,const AnimationClipBank& clips,
    std::span<const ActorChannelCommand> commands,const MorphHostInput& input,float elapsed,const RootMotionConstraint& constraint,
    const AnimatedCharacterResourceInput& resources,CharacterActorCurveFrame* actor_curves){
    (void)step_impl(graph,clips,commands,input,elapsed,constraint,resources,nullptr,nullptr,actor_curves);
}
std::vector<ActorAttachmentChange> AnimatedCharacter::step_with_attachments(ActorAttachmentDraft& draft,ActorAttachmentRuntime& owner,
    const AnimationGraph& graph,const AnimationClipBank& clips,std::span<const ActorChannelCommand> commands,
    const MorphHostInput& input,float elapsed,const RootMotionConstraint& constraint,const AnimatedCharacterResourceInput& resources,
    CharacterActorCurveFrame* actor_curves){
    return step_impl(graph,clips,commands,input,elapsed,constraint,resources,&draft,&owner,actor_curves);
}
std::vector<ActorAttachmentChange> AnimatedCharacter::step_impl(const AnimationGraph& graph,const AnimationClipBank& clips,
    std::span<const ActorChannelCommand> commands,const MorphHostInput& input,float elapsed,const RootMotionConstraint& constraint,
    const AnimatedCharacterResourceInput& resources,ActorAttachmentDraft* attachments,ActorAttachmentRuntime* attachment_owner,
    CharacterActorCurveFrame* actor_curves){
    require_idle();auto next_model=model_.clone_for_transaction();auto next_morph=morph_;
    struct Reset {AnimationModel*& slot;~Reset(){slot=nullptr;}} reset{candidate_};
    candidate_=&next_model;
    std::optional<AnimationRotationCurveState> next_heading;
    std::optional<ActorCurveUpdateState> next_curves;
    next_model.apply_commands(commands);
    if(actor_curves){
        next_heading=actor_curves->heading;next_curves=actor_curves->state;
        next_model.update_actor_curves(*next_heading,*next_curves,actor_curves->root,actor_curves->position_enabled,elapsed,attachments);
    }
    next_model.step(graph,skeleton_,clips,elapsed,constraint);
    auto resolved=input;
    check(resources.grip_map||(!resources.equipment[0]&&!resources.equipment[1]),"Character equipment requires a grip map");
    if(resources.grip_map&&resolved.expressions_enabled)
        resolved.secondary_grip_shapes=equipment_secondary_grips(next_morph,*resources.grip_map,resources.equipment);
    if(resources.gaze&&resolved.expressions_enabled){
        check(!input.local_gaze,"Character gaze supplied in both local and world space");
        const auto& request=*resources.gaze;
        if(request.live_target||request.fallback[3]!=0){
            const auto& player=next_model.player();
            const auto root=root_placement_matrix({player.orientation,player.position});
            std::optional<SkinMatrix> head;
            if(request.model_present){check(player.world.size()>14,"Character gaze requires model node14");head=player.world[14];}
            resolved.local_gaze=resolve_morph_gaze(root,head,request.live_target,request.fallback);
        }else resolved.local_gaze.reset();
    }
    update_morph_host(next_morph,morph_config_,resolved);
    if(!deferred_skinning_)build_geometry_into(next_model.player(),next_morph,spare_geometry_,morph_scratch_);
    auto changes=attachments?attachments->commit(*attachment_owner):std::vector<ActorAttachmentChange>{};
    model_=std::move(next_model);morph_=std::move(next_morph);
    if(deferred_skinning_)geometry_stale_=true;else geometry_.swap(spare_geometry_);
    if(actor_curves){actor_curves->heading=*next_heading;actor_curves->state=*next_curves;}
    return changes;
}
}
