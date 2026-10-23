#pragma once
#include "animation_root.hpp"
#include <functional>

namespace omega {
using ActorWorldMatrix=std::array<float,16>;
struct ActorWorldHistory {
    bool changed{};
    ActorWorldMatrix previous{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    ActorWorldMatrix relative{};
    RootVector displacement{},velocity{};
};
struct ActorWorldHistoryNode {
    ActorWorldMatrix matrix{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    bool dirty{};
};
struct ActorWorldHistoryHost {
    std::function<ActorWorldHistory*()> history;
    std::function<ActorWorldHistoryNode*()> node;
    std::function<void(ActorWorldHistoryNode&)> refresh;
};
// Original 003ce2a0 retained fields. Computes current * the original rigid
// transpose/translation inverse helper, without replacing actor/model roots.
// Positive dt and finite normal-or-zero inputs/intermediates are required.
void update_actor_world_history(ActorWorldHistory&,const ActorWorldMatrix& current,float dt);
// Optional history then optional node. Captures node before refresh but rereads
// history afterward. Captured node storage must survive refresh; external
// callback effects belong to the caller's transaction. Inactive paths ignore dt.
void update_actor_world_history(const ActorWorldHistoryHost&,float dt);
}
