#pragma once
#include "actor_body_query.hpp"

namespace omega {
enum class ActorSpecializedRoot { first_4a03d8,second_4a03d0 };
struct ActorSpecializedRootHost {
    std::function<float(ActorSpecializedRoot)> weight;
    std::function<RootParentTransform(ActorSpecializedRoot)> placement;
};
// Original001e5710 full inverse, including zero-quaternion behavior.
RootParentTransform invert_specialized_body_placement(const RootParentTransform&);
// Calls weight once to gate each root; admitted roots read placement twice and
// weight again. Getters must resolve actual ordinary world placement, not pose.
// External getter effects cannot roll back when a later input fails.
RootVector resolve_specialized_actor_body_center(const RootVector& origin,
    const RootVector& displacement,float radius,const ActorSpecializedRootHost&);
// Shared transformed constructor, explicitly unit scale. The center's W lane
// (not always exactly 1 after a fractional blend) is carried as plan.center_w.
ActorBodyQueryPlan make_specialized_actor_body_query(const ActorBodyQueryInput&,
    const ActorSpecializedRootHost&);
}
