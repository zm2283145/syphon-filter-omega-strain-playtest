#include "float_bits.hpp"
#include "animation_model.hpp"
#include "actor_animation.hpp"
#include "actor_attachment_runtime.hpp"
#include "skeleton_pose.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
struct ModelOperation {
    bool& active;bool& stepping;
    ModelOperation(bool& a,bool& s,bool frame=false):active(a),stepping(s) {
        if(active)throw std::runtime_error("Recursive animation model mutation is unsupported");
        active=true;stepping=frame;
    }
    ~ModelOperation(){active=false;stepping=false;}
};
}
AnimationModel::AnimationModel(const SkeletonDefinition& skeleton,const AnimationGraphManifest& manifest)
    :default_crossfade_(manifest.default_crossfade_duration),motions_(manifest.roots),
     channels_(manifest.channel_count),player_(make_animation_player(skeleton,manifest.anchors,
         manifest.channel_count,manifest.selector_count)) {
    if(!::omega::float_bits::finite(default_crossfade_)||default_crossfade_<0)
        throw std::runtime_error("Animation model default crossfade is invalid");
}
AnimationChannel& AnimationModel::checked_channel(std::size_t index) {
    if(operation_active_)throw std::runtime_error("Animation channel mutation during receiver delivery is unsupported");
    if(index>=channels_.size()) throw std::runtime_error("Animation model channel index outside array");
    return channels_[index];
}
AnimationModel AnimationModel::clone_for_transaction() const {
    if(operation_active_)throw std::runtime_error("Animation model cannot be cloned during an operation");
    AnimationModel result;
    result.default_crossfade_=default_crossfade_;result.motions_=motions_;
    result.channels_.reserve(channels_.size());
    for(const auto& channel:channels_)result.channels_.push_back(channel.clone_for_transaction());
    result.controller_=controller_;result.player_=player_;result.completed_=completed_;result.delivery_=delivery_;
    return result;
}
const AnimationChannelState& AnimationModel::channel(std::size_t index) const {
    if(index>=channels_.size()) throw std::runtime_error("Animation model channel index outside array");
    return channels_[index].state();
}
void AnimationModel::set_value(std::size_t index,float value) {
    auto& channel=checked_channel(index);channel.set_value(value);
    player_.graph.channels[index]=channel.state().current;
}
void AnimationModel::approach(std::size_t index,float target,float maximum_step,bool wrapped) {
    auto& channel=checked_channel(index);channel.approach(target,maximum_step,wrapped);
    player_.graph.channels[index]=channel.state().current;
}
void AnimationModel::set_target(std::size_t index,float target,float duration,float damping,bool wrapped) {
    checked_channel(index).set_target(target,duration,damping,wrapped);
}
void AnimationModel::select(std::size_t index,std::size_t value) {
    if(operation_active_)throw std::runtime_error("Animation selector mutation during receiver delivery is unsupported");
    if(index>=player_.graph.selectors.size()) throw std::runtime_error("Animation model selector index outside array");
    player_.graph.selectors[index].requested=value;
}
void AnimationModel::apply_commands(std::span<const ActorChannelCommand> commands) {
    ModelOperation guard(operation_active_,stepping_);
    std::vector<AnimationChannel> channels;channels.reserve(channels_.size());
    for(const auto& channel:channels_)channels.push_back(channel.clone_for_transaction());
    auto selectors=player_.graph.selectors;
    for(const auto& command:commands) {
        if(command.operation==ActorChannelOperation::select) {
            if(command.index>=selectors.size()||!::omega::float_bits::finite(command.value)||command.value<0||
               command.value>=static_cast<float>(std::numeric_limits<std::size_t>::max())||std::floor(command.value)!=command.value)
                throw std::runtime_error("Animation selector command outside integer domain");
            selectors[command.index].requested=static_cast<std::size_t>(command.value);
            continue;
        }
        if(command.index>=channels.size())throw std::runtime_error("Animation channel command index");
        auto& channel=channels[command.index];
        switch(command.operation) {
        case ActorChannelOperation::set:channel.set_value(command.value);break;
        case ActorChannelOperation::approach:channel.approach(command.value,command.parameter,command.wrapped);break;
        case ActorChannelOperation::target:channel.set_target(command.value,command.parameter,command.damping,command.wrapped);break;
        default:throw std::runtime_error("Unknown animation channel command");
        }
    }
    auto values=player_.graph.channels;
    for(std::size_t i=0;i<channels.size();++i)values[i]=channels[i].state().current;
    channels_=std::move(channels);player_.graph.channels=std::move(values);player_.graph.selectors=std::move(selectors);
}
void AnimationModel::play(std::string_view name,MotionInstallMode mode,std::optional<float> duration) {
    if(stepping_)return;
    ModelOperation guard(operation_active_,stepping_);
    const auto found=std::find_if(motions_.begin(),motions_.end(),[&](const auto& root){return root.name==name;});
    if(found==motions_.end()) throw std::runtime_error("Animation model named motion is unavailable");
    if(found->group_pair[0]!=0) throw std::runtime_error("Named motion contains an unresolved original node identity");
    auto request=make_motion_request(found->node_index,duration.value_or(default_crossfade_));
    request.selector.group=std::bit_cast<std::int32_t>(found->group_pair[1]);
    controller_operation([&](auto& controller,const auto& delivery){install_motion(controller,request,mode,delivery);});
}
void AnimationModel::queue(std::string_view name,std::optional<float> duration) {
    play(name,MotionInstallMode::queue,duration);
}
void AnimationModel::play(const MotionRequest& request,MotionInstallMode mode) {
    if(stepping_)return;
    ModelOperation guard(operation_active_,stepping_);
    controller_operation([&](auto& controller,const auto& delivery){install_motion(controller,request,mode,delivery);});
}
void AnimationModel::write_node_locals(const SkeletonDefinition& skeleton,std::span<const std::optional<std::array<float,12>>> rows,
    const std::optional<RootVector>& root_translation) {
    ModelOperation guard(operation_active_,stepping_);
    if(player_.pose.bones.size()+1!=skeleton.bones.size()||rows.size()>skeleton.bones.size())
        throw std::runtime_error("Node local write outside the model's skeleton");
    auto next=player_;
    for(std::size_t k=0;k<rows.size();++k){
        if(!rows[k])continue;
        for(const float f:*rows[k])if(!::omega::float_bits::finite(f))throw std::runtime_error("Nonfinite node local rows");
        if(k&&next.pose.bones[k-1].bone_index!=k)throw std::runtime_error("Node local write: pose bone order differs from the skeleton");
        auto& local=k?next.pose.bones[k-1].local:next.root_local;
        std::copy(rows[k]->begin(),rows[k]->end(),local.begin());
    }
    if(root_translation){
        for(const float f:*root_translation)if(!::omega::float_bits::finite(f))throw std::runtime_error("Nonfinite node 0 translation");
        std::copy(root_translation->begin(),root_translation->end(),next.root_local.begin()+12);
    }
    std::vector<std::array<float,16>> locals{next.root_local};
    for(const auto& bone:next.pose.bones)locals.push_back(bone.local);
    next.world=skeleton_world_pose(skeleton,locals);
    player_=std::move(next);
}
void AnimationModel::clear() {
    ModelOperation guard(operation_active_,stepping_);
    controller_operation([](auto& controller,const auto& delivery){clear_motions(controller,delivery);});
}
bool AnimationModel::play_actor_motion(const ActorMotionRegistry& registry,const ActorMotionSelection& selection,
    ActorMotionTransitionState& transition,const MotionRequest& payload,MotionInstallMode mode) {
    if(operation_active_&&!stepping_)throw std::runtime_error("Recursive actor motion installation is unsupported");
    return submit_actor_motion(registry,selection,transition,default_crossfade_,payload,mode,stepping_,
        [&](const MotionRequest& request,MotionInstallMode install_mode){play(request,install_mode);});
}
void AnimationModel::controller_operation(const std::function<void(AnimationController&,const MotionDeliveryHooks&)>& operation) {
    std::vector<AnimationChannel> channels;channels.reserve(channels_.size());
    for(const auto& channel:channels_)channels.push_back(channel.clone_for_transaction());
    auto player=player_;auto controller=controller_;
    const auto wrap=[&](const MotionDeliveryHooks::Sink& sink)->MotionDeliveryHooks::Sink {
        if(!sink)return {};
        return [&,sink](const MotionEffect& effect,const MotionDeliveryView& original){
            auto view=MotionDeliveryView(original.controller,&player,original.installation_locked)
                .with_channel_setter([&](std::size_t index,float value){
                    if(index>=channels.size())throw std::runtime_error("Animation callback channel index outside array");
                    channels[index].set_value(value);return channels[index].state().current;
                }).with_channel_target_setter([&](std::size_t index,float target,float duration,float damping,bool wrapped){
                    if(index>=channels.size())throw std::runtime_error("Animation callback target index outside array");
                    channels[index].set_target(target,duration,damping,wrapped);
                });
            sink(effect,view);
        };
    };
    const MotionDeliveryHooks delivery{wrap(delivery_.immediate),wrap(delivery_.enqueue)};
    operation(controller,delivery);
    channels_=std::move(channels);player_=std::move(player);controller_=std::move(controller);
}
void AnimationModel::set_delivery_hooks(MotionDeliveryHooks hooks) {
    if(operation_active_)throw std::runtime_error("Animation delivery hook mutation during operation is unsupported");
    delivery_=std::move(hooks);
}
void AnimationModel::set_playback_rate(float rate) {
    if(operation_active_)throw std::runtime_error("Animation playback rate mutation during operation is unsupported");
    if(!::omega::float_bits::finite(rate)||rate<0||::omega::float_bits::subnormal(rate))
        throw std::runtime_error("Animation playback rate must be finite and nonnegative");
    player_.playback_rate=rate;
}
AnimationPlayerAnchor& AnimationModel::checked_anchor(const RootAnchorKey& key) {
    if(operation_active_)throw std::runtime_error("Animation anchor mutation during receiver delivery is unsupported");
    const auto found=std::find_if(player_.anchors.begin(),player_.anchors.end(),
        [&](const auto& anchor){return anchor.key==key;});
    if(found==player_.anchors.end())throw std::runtime_error("Animation model root anchor is unavailable");
    return *found;
}
void AnimationModel::set_anchor_placement(const RootAnchorKey& key,const RootVector& orientation,const RootVector& position) {
    auto& anchor=checked_anchor(key);
    auto candidate=anchor.state;
    set_root_placement(candidate,orientation,position);
    (void)root_world_placement(candidate,anchor.parent?&*anchor.parent:nullptr);
    anchor.state=std::move(candidate);
}
void AnimationModel::set_anchor_world_orientation(const RootAnchorKey& key,const RootVector& orientation,bool reset_counter) {
    auto& anchor=checked_anchor(key);auto next=anchor.state;
    set_root_world_orientation(next,orientation,anchor.parent?&*anchor.parent:nullptr);
    if(reset_counter)next.normalization_counter=0;
    anchor.state=std::move(next);
}
void AnimationModel::set_anchor_world_placement(const RootAnchorKey& key,const RootParentTransform& placement,bool reset_counter) {
    auto& anchor=checked_anchor(key);auto next=anchor.state;
    set_root_world_placement(next,placement,anchor.parent?&*anchor.parent:nullptr);
    if(reset_counter)next.normalization_counter=0;
    anchor.state=std::move(next);
}
void AnimationModel::set_anchor_world_position(const RootAnchorKey& key,const RootVector& position) {
    auto& anchor=checked_anchor(key);auto next=anchor.state;
    set_root_world_position(next,position,anchor.parent?&*anchor.parent:nullptr);
    anchor.state=std::move(next);
}
void AnimationModel::set_anchor_state(const RootAnchorKey& key,const RootAnimationState& state) {
    auto& anchor=checked_anchor(key);
    const auto* parent=anchor.parent?&*anchor.parent:nullptr;
    (void)root_world_placement(state,parent);
    auto pose=state;pose.orientation=state.pose_orientation;pose.position=state.pose_position;
    (void)root_world_placement(pose,parent);
    for(const auto* vector:{&state.motion_cache,&state.pose_orientation,&state.pose_position})
        for(const float value:*vector)
            if(::omega::float_bits::nonfinite_or_subnormal(value))
                throw std::runtime_error("Animation model root state contains invalid retained arithmetic");
    if(state.sign_reference)for(const float value:*state.sign_reference)
        if(::omega::float_bits::nonfinite_or_subnormal(value))
            throw std::runtime_error("Animation model root state contains an invalid sign reference");
    if(state.position[3]!=1.0F||state.pose_position[3]!=1.0F)
        throw std::runtime_error("Animation model root state placement is not affine");
    anchor.state=state;
}
void AnimationModel::set_anchor_parent(const RootAnchorKey& key,std::optional<RootParentTransform> parent) {
    auto& anchor=checked_anchor(key);
    (void)root_world_placement(anchor.state,parent?&*parent:nullptr);
    anchor.parent=std::move(parent);
}
void AnimationModel::set_anchor_constraint(const RootAnchorKey& key,std::optional<RootMotionConstraint> constraint) {
    auto& anchor=checked_anchor(key);
    if(constraint){
        if(constraint->kind!=RootConstraintKind::none&&constraint->kind!=RootConstraintKind::keep_axis&&constraint->kind!=RootConstraintKind::reject_axis)
            throw std::runtime_error("Animation model root constraint kind");
        for(float value:constraint->axis)if(!::omega::float_bits::finite(value))throw std::runtime_error("Animation model root constraint axis");
    }
    anchor.constraint=std::move(constraint);
}
void AnimationModel::update_actor_curves(AnimationRotationCurveState& heading,ActorCurveUpdateState& state,
    const RootAnchorKey& key,bool position_enabled,float elapsed,ActorAttachmentDraft* attachments) {
    ModelOperation guard(operation_active_,stepping_);
    auto next_player=player_;
    const auto resolve=[&]() -> AnimationPlayerAnchor& {
        const auto found=std::find_if(next_player.anchors.begin(),next_player.anchors.end(),
            [&](const auto& anchor){return anchor.key==key;});
        if(found==next_player.anchors.end())throw std::runtime_error("Actor curve root anchor is unavailable");
        return *found;
    };
    ActorCurveUpdateHost host;
    host.position_enabled=[&]{return position_enabled;};
    host.root_weight=[&]{return resolve().weight;};
    host.root_world_position=[&]{
        const auto& root=resolve();
        return root_world_placement(root.state,root.parent?&*root.parent:nullptr).position;
    };
    host.publish_root_world_position=[&](const RootVector& position){
        if(attachments){
            attachments->set_root_world_position(key,position,MotionDeliveryView(controller_,&next_player,false));
            return;
        }
        auto& root=resolve();set_root_world_position(root.state,position,root.parent?&*root.parent:nullptr);
    };
    omega::update_actor_curves(heading,state,elapsed,host);
    player_=std::move(next_player);
}
void AnimationModel::step(const AnimationGraph& graph,const SkeletonDefinition& skeleton,
    const AnimationClipBank& clips,float elapsed,const RootMotionConstraint& constraint) {
    ModelOperation guard(operation_active_,stepping_,true);
    if(!::omega::float_bits::finite(elapsed)||elapsed<=0) throw std::runtime_error("Animation model elapsed time must be finite and positive");
    std::vector<AnimationChannel> draft_channels;
    draft_channels.reserve(channels_.size());
    for(const auto& channel:channels_) draft_channels.push_back(channel.clone_for_transaction());
    auto draft_player=player_;auto draft_controller=controller_;
    std::vector<std::size_t> completed;
    for(std::size_t i=0;i<draft_channels.size();++i) {
        if(draft_channels[i].step(elapsed)) completed.push_back(i);
        draft_player.graph.channels[i]=draft_channels[i].state().current;
    }
    const auto wrap=[&](const MotionDeliveryHooks::Sink& sink)->MotionDeliveryHooks::Sink {
        if(!sink)return {};
        return [&,sink](const MotionEffect& effect,const MotionDeliveryView& view){
            auto bound=view.with_channel_setter([&](std::size_t index,float value){
                if(index>=draft_channels.size())throw std::runtime_error("Animation callback channel index outside array");
                draft_channels[index].set_value(value);
                return draft_channels[index].state().current;
            }).with_channel_target_setter([&](std::size_t index,float target,float duration,float damping,bool wrapped){
                if(index>=draft_channels.size())throw std::runtime_error("Animation callback target index outside array");
                draft_channels[index].set_target(target,duration,damping,wrapped);
            });
            sink(effect,bound);
        };
    };
    const MotionDeliveryHooks delivery{wrap(delivery_.immediate),wrap(delivery_.enqueue)};
    update_animation_controller(draft_controller,draft_player,graph,skeleton,clips,elapsed,constraint,delivery);
    channels_=std::move(draft_channels);player_=std::move(draft_player);
    controller_=std::move(draft_controller);completed_=std::move(completed);
}
}
