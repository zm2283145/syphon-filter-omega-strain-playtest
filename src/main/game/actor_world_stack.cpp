#include "actor_world_stack.hpp"
#include <algorithm>
#include <stdexcept>

namespace omega {
namespace {
void validate(const ActorWorldStack& s,const ActorWorldStackHost& h) {
    if(!h.snapshot||!h.activate||!h.deactivate)
        throw std::runtime_error("Active world-object hosting is unresolved");
    if(s.objects.size()>4096||std::find(s.objects.begin(),s.objects.end(),0)!=s.objects.end())
        throw std::runtime_error("Active world-object list outside bounded domain");
}
bool active(const ActorWorldObjectSnapshot& s){return s.byte_2d&&s.byte_2c&&!s.byte_2e;}
void publish(ActorWorldStack& s,std::uint64_t id,const ActorWorldStackHost& h) {
    const auto generation=id?h.snapshot(id).generation:-1;
    s.published=id;s.published_generation=generation;
}
}
void push_actor_world_object(ActorWorldStack& s,std::uint64_t object,const ActorWorldStackHost& callbacks) {
    const auto h=callbacks;validate(s,h);
    if(!object)throw std::runtime_error("Cannot push a null original world object");   // 131360: virtual call on null
    if(!s.objects.empty())h.deactivate(s.objects.front(),s);
    validate(s,h);
    h.activate(object,s);validate(s,h);
    publish(s,object,h);
    s.objects.insert(s.objects.begin(),object);
}
std::uint64_t pop_actor_world_object(ActorWorldStack& s,const ActorWorldStackHost& callbacks) {
    const auto h=callbacks;validate(s,h);
    if(s.objects.empty())throw std::runtime_error("Cannot pop empty original world-object list");
    const auto old=s.objects.front();s.objects.erase(s.objects.begin());
    h.deactivate(old,s);validate(s,h);
    const auto next=s.objects.empty()?0:s.objects.front();
    if(next)h.activate(next,s);
    publish(s,next,h);return old;
}
void remove_actor_world_object(ActorWorldStack& s,std::uint64_t object,const ActorWorldStackHost& callbacks) {
    if(!object)return;
    const auto h=callbacks;validate(s,h);
    std::erase(s.objects,object);
    if(active(h.snapshot(object)))h.deactivate(object,s);
    validate(s,h);
    const auto next=s.objects.empty()?0:s.objects.front();
    if(next&&!active(h.snapshot(next))) {
        h.activate(next,s);publish(s,next,h);return;
    }
    // Original clears the published reference even if the remaining first
    // object was already active. Do not replace this with a usual stack rule.
    publish(s,0,h);
}
void finish_actor_world_pending(ActorWorldStack& s,const ActorWorldStackHost& h) {
    if(!s.pending)return;
    s.pending=false;
    remove_actor_world_object(s,s.pending_object,h);
}
}
