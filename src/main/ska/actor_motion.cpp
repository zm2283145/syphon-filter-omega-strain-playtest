#include "float_bits.hpp"
#include "actor_motion.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
float scalar(float value) {
    if(::omega::float_bits::nonfinite_or_subnormal(value))
        throw std::runtime_error("Actor motion requires finite normal-or-zero timing");
    return value;
}
void transition_values(const ActorMotionTransition& value) {
    for(auto v:{value.width,value.bias,value.rate,value.secondary_rate})scalar(v);
}
void name_domain(std::string_view name) {
    if(name.size()>47)throw std::runtime_error("Actor motion node name exceeds fixed-key domain");
    for(unsigned char c:name)if(c<32||c>126)throw std::runtime_error("Actor motion name outside bounded ASCII domain");
}
}
ActorMotionRegistry::ActorMotionRegistry(const AnimationGraph& graph,
    std::vector<std::optional<std::size_t>> numeric_nodes,std::map<std::string,std::size_t> names)
    :numeric_nodes_(std::move(numeric_nodes)),names_(std::move(names)) {
    if(graph.nodes().size()>65536||numeric_nodes_.size()>65536||names_.size()>65536)
        throw std::runtime_error("Actor motion registry exceeds bounded extent");
    for(const auto& node:graph.nodes())nodes_.push_back({node.kind,node.leaf.group});
    for(auto node:numeric_nodes_)if(node&&*node>=nodes_.size())throw std::runtime_error("Actor motion registry node outside graph");
    for(const auto& [name,id]:names_) {
        name_domain(name);if(id>=numeric_nodes_.size())throw std::runtime_error("Actor motion name ID outside registry");
    }
}
void ActorMotionRegistry::validate(const ActorMotionRegistration& value) const {
    if(value.node&&*value.node>=nodes_.size())throw std::runtime_error("Actor motion descriptor node outside graph");
    if(value.selector.group< -1)throw std::runtime_error("Actor motion descriptor group outside bounded domain");
    if(value.selector.node) {
        const auto i=*value.selector.node;
        if(i>=nodes_.size()||(nodes_[i].kind!=AnimationGraphKind::Leaf&&nodes_[i].kind!=AnimationGraphKind::ControlledLeaf))
            throw std::runtime_error("Actor motion selector identity must resolve to a leaf");
        if(value.selector.group!=-1||nodes_[i].group!=-1)
            throw std::runtime_error("Actor motion selector conflicts with numbered group");
    }
}
ActorMotionRegistration ActorMotionRegistry::resolve(const ActorMotionSelection& selection) const {
    if(selection.tag==2) {validate(selection.registration);return selection.registration;}
    if(selection.tag>2)return {};
    std::int64_t id=selection.numeric_id;
    if(selection.tag==0) {
        name_domain(selection.name);
        const auto found=names_.find(selection.name);
        id=found==names_.end()?0:static_cast<std::int64_t>(found->second);
    }
    if(id<0||static_cast<std::uint64_t>(id)>=numeric_nodes_.size())
        throw std::runtime_error("Original unchecked numeric motion lookup is outside admitted registry");
    ActorMotionRegistration result;result.node=numeric_nodes_[static_cast<std::size_t>(id)];
    if(result.node) {
        const auto& node=nodes_[*result.node];
        if(node.kind==AnimationGraphKind::Leaf||node.kind==AnimationGraphKind::ControlledLeaf) {
            result.selector.group=node.group;
            if(node.group==-1)result.selector.node=result.node;
        }
    }
    validate(result);
    return result;
}
std::optional<std::size_t> ActorMotionRegistry::append_leaf(const std::string& name) {
    name_domain(name);
    if(names_.contains(name))return std::nullopt;
    if(nodes_.size()>=65536||numeric_nodes_.size()>=65536)throw std::runtime_error("Actor motion registry exceeds bounded extent");
    const auto id=numeric_nodes_.size();
    nodes_.push_back({AnimationGraphKind::Leaf,-1});
    numeric_nodes_.push_back(nodes_.size()-1);
    names_.emplace(name,id);
    return id;
}
ActorMotionRegistry make_actor_motion_registry(const AnimationGraphManifest& manifest) {
    if(manifest.expected_builders!=17||manifest.completed_builders!=17)
        throw std::runtime_error("Actor motion registry requires complete verified extraction");
    std::vector<std::optional<std::size_t>> nodes;std::map<std::string,std::size_t> names;
    for(const auto& node:manifest.graph.nodes()) {
        const auto index=nodes.size();nodes.push_back(index);
        if(!names.emplace(node.name,index).second)throw std::runtime_error("Duplicate extracted motion node name");
    }
    return ActorMotionRegistry(manifest.graph,std::move(nodes),std::move(names));
}
ActorMotionRegistration actor_motion_registration(const GraphNamedRoot& root) {
    if(root.group_pair[0]!=0)throw std::runtime_error("Unresolved original descriptor pointer identity");
    return {root.node_index,{std::nullopt,std::bit_cast<std::int32_t>(root.group_pair[1])}};
}
ActorMotionTransition actor_motion_transition(float duration) {
    if(scalar(duration)<0)throw std::runtime_error("Negative actor default transition duration");
    const auto rate=duration==0?std::numeric_limits<float>::max():scalar(1.0F/duration);
    return {duration,duration,rate,rate};
}
void set_actor_motion_transition(ActorMotionTransitionState& state,const ActorMotionTransition& value) {
    transition_values(value);state={true,value};
}
bool submit_actor_motion(const ActorMotionRegistry& registry,const ActorMotionSelection& selection,
    ActorMotionTransitionState& transition,float default_duration,const MotionRequest& payload,
    MotionInstallMode mode,bool installation_locked,
    const std::function<void(const MotionRequest&,MotionInstallMode)>& install) {
    const auto resolved=registry.resolve(selection);
    if(!resolved.node)return false;
    const auto parameters=transition.pending?transition.value:actor_motion_transition(default_duration);
    transition_values(parameters);
    if(!installation_locked&&!install)throw std::runtime_error("Actor motion installation host missing");
    if(static_cast<unsigned>(mode)>3)throw std::runtime_error("Actor motion install mode outside admitted domain");
    auto request=payload;request.node=*resolved.node;request.selector=resolved.selector;
    request.width=parameters.width;request.bias=parameters.bias;request.rate=parameters.rate;
    request.secondary_rate=parameters.secondary_rate;
    transition.pending=false;
    if(!installation_locked)install(request,mode);
    return true;
}
}
