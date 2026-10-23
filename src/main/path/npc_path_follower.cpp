#include "npc_path_follower.hpp"
#include "npc_route.hpp"
#include "vu_float.hpp"
#include <stdexcept>
#include <string>

namespace omega {
namespace {
bool bit(std::uint8_t v,unsigned i){return ((v>>i)&1U)!=0;}
void set_bit(std::uint8_t& v,unsigned i,bool on){v=static_cast<std::uint8_t>((v&~(1U<<i))|(on?(1U<<i):0U));}
}
const NpcNode& NpcNodeGraph::node(std::int32_t index) const{
    if(index<0||static_cast<std::size_t>(index)>=nodes.size())throw std::runtime_error("NPC path: node index "+std::to_string(index)+" out of range");
    return nodes[static_cast<std::size_t>(index)];
}
RootVector NpcNodeGraph::position(std::int32_t index) const{
    const auto& p=node(index).position;return {p[0],p[1],p[2],1.0F};
}
std::optional<std::int32_t> NpcNodeGraph::node_of_object(std::uint64_t object) const{
    if(object<npc_node_object_base||object-npc_node_object_base>=nodes.size())return std::nullopt;
    return static_cast<std::int32_t>(object-npc_node_object_base);
}
std::int32_t npc_next_patrol_node(const NpcNodeGraph& graph,std::int32_t index){
    for(const auto& l:graph.node(index).links)
        if(graph.node(l.target).flags&npc_node_path)return l.target;
    return -1;
}
std::int32_t npc_link_type(const NpcNodeGraph& graph,std::int32_t from,std::int32_t to){
    for(const auto& l:graph.node(from).links)if(l.target==to)return l.type;
    if(static_cast<std::size_t>(from)<graph.port_overlay.size())                 // port-only co-op overlay (132ob5)
        for(const auto& l:graph.port_overlay[static_cast<std::size_t>(from)])if(l.target==to)return l.type;
    return -1;
}
std::int32_t npc_first_link_of_type(const NpcNodeGraph& graph,std::int32_t from,std::int32_t type){
    for(const auto& l:graph.node(from).links)if(l.type==type){graph.node(l.target);return l.target;}
    return -1;
}
void construct_npc_path_follower(NpcPathFollower& pf,std::int32_t capacity,std::int32_t hold){
    pf.hold_4c=hold;pf.capacity_78=capacity;
    pf.destination_5c=-1;pf.current_50=-1;pf.previous_54=-1;pf.field_58=-1;
    pf.flags_60=static_cast<std::uint8_t>((pf.flags_60&0x80U)|0x78U);
    pf.direction_10={0,0,0,0};
}
void hold_npc_path_node(NpcPathFollower& pf,const NpcNodeGraph& graph,std::int32_t node){
    pf.hold_4c=node;
    if(node<0)return;
    const auto& n=graph.node(node);
    pf.saved_20=graph.position(node);
    if(n.parameter_7c>=0)pf.destination_5c=n.parameter_7c;
    set_bit(pf.flags_60,1,(n.flags&npc_node_path)!=0);
}
bool set_npc_path_segment_to(NpcPathFollower& pf,const NpcNodeGraph& graph,const RootVector& point,std::int32_t node,const NpcPathHost& host){
    RootVector from;
    if(pf.current_50>=0){
        from=graph.position(pf.current_50);
        hold_npc_path_node(pf,graph,pf.current_50);
    }else from=host.actor_translation();
    RootVector d;for(int i=0;i<4;++i)d[i]=point[i]-from[i];                 // vsub.xyzw
    pf.direction_10=d;
    if(d[0]==0.0F&&d[1]==0.0F&&d[2]==0.0F)pf.remaining_40=0;                 // vaddx.xyz zero flags 0xe
    else{
        const float x2=d[0]*d[0],y2=d[1]*d[1],z2=d[2]*d[2];
        const float sum=(x2+y2)+z2;                                          // vmulax/vmadday/vmaddz .w
        const float length=vu::sqrt(sum);                                    // vsqrt, vaddq.x
        const float inverse=vu::div(1.0F,length);                            // vdiv Q, vf0.w, vf4.x
        for(int i=0;i<3;++i)pf.direction_10[i]=d[i]*inverse;
        pf.remaining_40=length;
    }
    pf.segment_44=pf.remaining_40;
    if(pf.current_50!=node){
        if(pf.hold_4c>=0&&node>=0)host.send({7,npc_link_type(graph,pf.hold_4c,node)});
        pf.current_50=node;
    }
    if(!(pf.remaining_40<=2.0F)){pf.target_00=point;return true;}           // c.ole.s +40, 2.0
    return false;
}
bool set_npc_path_segment(NpcPathFollower& pf,const NpcNodeGraph& graph,std::int32_t node,const NpcPathHost& host){
    if(node==pf.hold_4c)return false;
    return set_npc_path_segment_to(pf,graph,graph.position(node),node,host);
}
namespace {
void start_npc_path(NpcPathFollower& pf,const NpcNodeGraph& graph,const NpcPathHost& host){
    if(!bit(pf.flags_60,6))return;
    set_bit(pf.flags_60,6,false);
    if(pf.hold_4c<0){
        pf.saved_20=host.actor_translation();pf.target_00=pf.saved_20;
        return;
    }
    const auto& n=graph.node(pf.hold_4c);
    hold_npc_path_node(pf,graph,pf.hold_4c);
    pf.target_00=pf.saved_20;
    if(n.flags&npc_node_path){
        set_bit(pf.flags_60,0,true);set_bit(pf.flags_60,1,true);
        const auto next=npc_next_patrol_node(graph,pf.hold_4c);
        if(next>=0){
            pf.current_50=pf.hold_4c;
            const bool segment=set_npc_path_segment(pf,graph,next,host);
            set_bit(pf.flags_60,3,!segment);set_bit(pf.flags_60,4,false);
            if(!bit(pf.flags_60,3))host.send({4,-1});
        }
    }
    if(n.parameter_7c>=0)pf.destination_5c=n.parameter_7c;
}
}
void start_npc_path_follower(NpcPathFollower& pf,const NpcNodeGraph& graph,const NpcPathHost& host,NpcRouteManager* routes){
    start_npc_path(pf,graph,host);
    allocate_npc_route_buffer(pf,routes);                                    // 00161150..001612ac
}
void npc_path_arrival_input(NpcPathFollower& pf,std::uint8_t arrived){
    if((bit(pf.flags_60,5)?1U:0U)!=arrived){
        const bool b=(arrived&1U)!=0;
        set_bit(pf.flags_60,4,b);set_bit(pf.flags_60,5,b);
    }else set_bit(pf.flags_60,4,false);
}
void update_npc_path_follower(NpcPathFollower& pf,const NpcNodeGraph& graph,const NpcPathHost& host){
    const RootVector p=host.actor_translation();
    if(bit(pf.flags_60,3))return;
    {
        RootVector d;for(int i=0;i<4;++i)d[i]=pf.target_00[i]-p[i];
        const float sum=(d[0]*d[0]+d[1]*d[1])+d[2]*d[2];
        pf.remaining_40=vu::sqrt(sum);                                       // vsqrt, vaddq.x
    }
    if(pf.hold_4c>=0){
        const auto h=graph.position(pf.hold_4c);
        RootVector d;for(int i=0;i<4;++i)d[i]=h[i]-p[i];
        const float sum=(d[0]*d[0]+d[1]*d[1])+d[2]*d[2];                    // ACC .x with vf3.x = 1
        if(!(sum<=4.0F)){pf.previous_54=pf.hold_4c;pf.hold_4c=-1;}
    }
    if(!bit(pf.flags_60,4))return;
    set_bit(pf.flags_60,4,false);
    if(!bit(pf.flags_60,0)){                                                  // 00160670
        set_bit(pf.flags_60,3,!step_npc_route(pf,graph,host));
        return;
    }
    const auto next=npc_next_patrol_node(graph,pf.current_50);
    if(next>=0){
        host.send({5,pf.current_50});
        const bool segment=set_npc_path_segment(pf,graph,next,host);
        set_bit(pf.flags_60,3,!segment);
        pf.destination_5c=graph.node(pf.current_50).parameter_7c;
        return;
    }
    const auto& n=graph.node(pf.current_50);
    hold_npc_path_node(pf,graph,pf.current_50);
    set_bit(pf.flags_60,3,true);
    host.send({6,pf.current_50});
    if(!(n.flags&npc_node_nav))hold_npc_path_node(pf,graph,-1);
}
float npc_path_height(const NpcPathFollower& pf,const NpcNodeGraph& graph,bool a1,bool a2){
    if(pf.hold_4c>=0&&!a2)return graph.node(pf.hold_4c).position[2];
    if(pf.previous_54<0||pf.current_50<0)return pf.saved_20[2];
    if(!a1&&pf.remaining_40<=1.0F+pf.segment_44&&!a2)
        return (pf.saved_20[2]*pf.remaining_40+pf.target_00[2]*(pf.segment_44-pf.remaining_40))/pf.segment_44;
    // min.s f0, f1 (+28), f0 (+08): the first operand unless the second is smaller.
    return pf.target_00[2]<pf.saved_20[2]?pf.target_00[2]:pf.saved_20[2];
}
void set_npc_path_height(NpcPathFollower& pf,float z){pf.target_00[2]=z;pf.saved_20[2]=z;}
std::optional<std::int32_t> npc_path_reference_node(const NpcPathFollower& pf){
    const auto n=pf.hold_4c>=0?pf.hold_4c:pf.previous_54;
    if(n<0)return std::nullopt;
    return n;
}
}
