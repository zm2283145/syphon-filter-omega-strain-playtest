#include "actor_edge_presets.hpp"
#include <stdexcept>

namespace omega {
void latch_actor_edge_request(ActorEdgeSlotState& slot,bool passive){
    slot.query_only=slot.request?(slot.query_only&&passive):passive;   // 001adb30 / 0024fbc0
    slot.request=true;
}

bool run_actor_edge_presets(ActorEdgePresetState& state,std::int32_t mode,std::uint32_t bits,const ActorEdgePresetHost& host){
    auto next=state;
    const bool flag1=((bits>>1)&1U)!=0;
    if(flag1){next.slots[6].query_only=false;next.slots[6].request=true;} // 1dfbb8..1dfbec
    bool any=false;
    for(auto& slot:next.slots){                                  // 1dfc00..1dfc74
        slot.result=false;slot.mask=0;
        any=any||slot.request;
    }
    const bool detect=(mode==0&&any)||flag1||mode==9;
    if(detect){
        if(!host.detect)throw std::runtime_error("Edge presets: detection path (001dfb80 body) is not hosted");
        host.detect(next);
    }
    // 001e1600: scans slots 8..0; without any request it returns without effect.
    bool requested=false;for(const auto& slot:next.slots)requested=requested||slot.request;
    if(requested){
        if(!host.report_requests)throw std::runtime_error("Edge presets: 001e1600 request handling is not hosted");
        host.report_requests(next);
    }
    for(auto& slot:next.slots)slot.request=false;               // 1e0df8..1e0e14
    state=next;
    return detect;
}
}
