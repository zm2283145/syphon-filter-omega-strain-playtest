#pragma once
#include "animation_controller.hpp"
#include "animation_graph_manifest.hpp"
#include <functional>
#include <map>
#include <string>

namespace omega {
struct ActorMotionRegistration {
    std::optional<std::size_t> node;
    MotionGroupSelector selector;
};
struct ActorMotionSelection {
    std::uint8_t tag{}; // 0=node name,1=numeric registry ID,2=registration value.
    std::string name;
    std::int64_t numeric_id{};
    ActorMotionRegistration registration;
};
// Owns registry metadata, not clips or graph execution state. Explicit mapping
// permits a native graph layout different from the original registry order.
class ActorMotionRegistry {
public:
    ActorMotionRegistry(const AnimationGraph& graph,
        std::vector<std::optional<std::size_t>> numeric_nodes,
        std::map<std::string,std::size_t> names);
    ActorMotionRegistration resolve(const ActorMotionSelection& selection) const;
    // 003b4800 for a clip node pushed onto the model (LoadAnimation): a name
    // already in the map answers nullopt (the sentinel *0x4bcd60) and changes
    // nothing; otherwise the new leaf (group -1) becomes node node_count() and
    // the name gets the next numeric ID (the old count), which is returned.
    std::optional<std::size_t> append_leaf(const std::string& name);
    bool has_name(const std::string& name) const {return names_.contains(name);}
    std::size_t node_count() const {return nodes_.size();}
    std::size_t id_count() const {return numeric_nodes_.size();}
private:
    struct Node {AnimationGraphKind kind;std::int32_t group;};
    std::vector<Node> nodes_;
    std::vector<std::optional<std::size_t>> numeric_nodes_;
    std::map<std::string,std::size_t> names_;
    void validate(const ActorMotionRegistration& value) const;
};
// Complete extractor manifests preserve successful003b4800 append order.
// Thus their flat node indices are the recovered numeric registry identities.
ActorMotionRegistry make_actor_motion_registry(const AnimationGraphManifest& manifest);
ActorMotionRegistration actor_motion_registration(const GraphNamedRoot& root);
struct ActorMotionTransition {
    float width{},bias{},rate{},secondary_rate{};
};
struct ActorMotionTransitionState {
    bool pending{};
    ActorMotionTransition value;
};
ActorMotionTransition actor_motion_transition(float duration);
void set_actor_motion_transition(ActorMotionTransitionState& state,const ActorMotionTransition& value);
// Resolves first. Null resolution leaves override and skipped default timing
// untouched. Success consumes the flag before the controller call, including
// when installation is locked. Payload retains its phase/limit/hooks/messages.
// An installation callback exception does not restore the consumed actor flag.
bool submit_actor_motion(const ActorMotionRegistry& registry,const ActorMotionSelection& selection,
    ActorMotionTransitionState& transition,float default_duration,const MotionRequest& payload,
    MotionInstallMode mode,bool installation_locked,
    const std::function<void(const MotionRequest&,MotionInstallMode)>& install);
}
