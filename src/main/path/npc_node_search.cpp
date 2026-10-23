#include "npc_node_search.hpp"
#include "vu_float.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
const NpcNodeGraph& graph_of(const NpcRouteManager& m){
    if(!m.graph)throw std::runtime_error("NPC node search: no node collection");
    return *m.graph;
}
const NpcRegions& regions_of(const NpcRouteManager& m){
    if(!m.regions)throw std::runtime_error("NPC node search: no region groups");
    return *m.regions;
}
NpcLosService& los_of(const NpcRouteManager& m){
    if(!m.los)throw std::runtime_error("NPC node search: the LOS manager 004ea0b0 is not linked (NpcRouteManager::los)");
    return *m.los;
}
bool client_valid(const NpcRouteRequest& r){
    if(!r.client||r.client->id!=r.id_04)return false;
    return !r.client->valid||r.client->valid();
}
RootVector sub4(const RootVector& a,const RootVector& b){return {a[0]-b[0],a[1]-b[1],a[2]-b[2],a[3]-b[3]};}
// vmul.xyz, then vmulax.w / vmadday.w / vmaddz.w with vf0.w = 1.
float length_squared(const RootVector& v){
    const float x=v[0]*v[0],y=v[1]*v[1],z=v[2]*v[2];
    return (x+y)+z;
}
// vaddx.xyz vf1, vf1, vf0 and cfc2 vi17 == 0xe: x, y and z are zero.
bool zero_xyz(const RootVector& v){return v[0]==0.0F&&v[1]==0.0F&&v[2]==0.0F;}
// vsub.xyzw and cfc2 vi17 == 0xf: every lane's zero flag and no sign flag.
bool same4(const RootVector& a,const RootVector& b){
    for(std::size_t k=0;k<4;++k){
        const float d=a[k]-b[k];
        if(d!=0.0F||std::signbit(d))return false;
    }
    return true;
}
// Path event 0 / 1 (path message 004ed9b8, +28, +2c) to the request's actor
// (+58 component), transport 1. The callers run with a valid handle.
void deliver(const NpcRouteRequest& r,std::uint8_t event,std::int32_t node,std::int32_t second){
    if(!client_valid(r))throw std::runtime_error("NPC node search: an answer to an actor that is gone (the original reads +58 of null)");
    if(!r.client->deliver)throw std::runtime_error("NPC node search: the requesting actor has no delivery for path events");
    NpcPathEvent e;e.event=event;e.value=node;e.second_2c=second;
    r.client->deliver(e);
}
void deliver_client(const std::shared_ptr<NpcRouteClient>& c,std::uint8_t event,std::int32_t node,std::int32_t second){
    if(!c->deliver)throw std::runtime_error("NPC node search: the requesting actor has no delivery for path events");
    NpcPathEvent e;e.event=event;e.value=node;e.second_2c=second;
    c->deliver(e);
}
// 00164b10(list, slot): before the first entry whose score is not below the
// new one (c.ole.s new, old), else at the end; the count +2c + 1.
void insert_sorted(NpcNodeSearch& s,std::size_t slot){
    const float f=s.slots[slot].score_0c;
    auto it=s.list.begin();
    for(;it!=s.list.end();++it)if(f<=s.slots[*it].score_0c)break;
    s.list.insert(it,slot);
}
// 001647b0: the first entry leaves the list (count - 1).
std::size_t pop_front(NpcNodeSearch& s){
    if(s.list.empty())throw std::runtime_error("NPC node search: 001647b0 on an empty candidate list (the original unlinks the sentinel)");
    const auto k=s.list.front();s.list.erase(s.list.begin());return k;
}
const NpcSearchCandidate& front(const NpcNodeSearch& s){
    if(s.list.empty())throw std::runtime_error("NPC node search: the first candidate of an empty list (the original reads the sentinel)");
    return s.slots[s.list.front()];
}
const std::vector<std::int32_t>& group(const NpcRouteManager& m,std::int32_t region){
    const auto& groups=regions_of(m).groups;
    if(region<0)throw std::runtime_error("NPC node search: the group of region "+std::to_string(region)+" (the original reads the 16 bytes before the group table)");
    if(static_cast<std::size_t>(region)>=groups.size())throw std::runtime_error("NPC node search: region "+std::to_string(region)+" outside the groups");
    return groups[static_cast<std::size_t>(region)];
}
// 0015e830(request, entry): the regions behind the type-10 links of the
// entry's Nav nodes, appended when not yet in the table (region, -1, 0.0).
void expand_region(const NpcRouteManager& m,NpcNodeSearch& s,std::size_t entry){
    const auto& graph=graph_of(m);
    for(const auto n:group(m,s.regions[entry].region_484)){
        const auto& node=graph.node(n);
        if(!(node.flags&0x80U))continue;
        for(const auto& l:node.links){
            if(l.type!=10)continue;
            const auto region=graph.node(l.target).parameter_7c;
            bool known=false;
            for(std::int32_t i=0;i<s.region_count_604;++i)if(s.regions[static_cast<std::size_t>(i)].region_484==region){known=true;break;}
            if(known)continue;
            // (A distance from the entry is computed into f20 and not stored.)
            if(s.region_count_604>=static_cast<std::int32_t>(npc_search_region_slots))
                throw std::runtime_error("NPC node search: more than 32 regions (0015e830 writes past its table)");
            s.regions[static_cast<std::size_t>(s.region_count_604)]={region,-1,0.0F};
            ++s.region_count_604;
        }
    }
}
// The candidate (node, second, score) into the list (0015e610..0015e69c).
void add_candidate(NpcNodeSearch& s,std::int32_t node,std::int32_t second,float score){
    if(s.list.size()<npc_search_candidate_slots){
        const auto k=s.list.size();
        s.slots[k]={score,node,second,-1,-1};
        insert_sorted(s,k);
        return;
    }
    const auto last=s.list.back();
    if(!(score<s.slots[last].score_0c))return;
    s.list.pop_back();
    s.slots[last]={score,node,second,-1,-1};
    insert_sorted(s,last);
}
// 0015e110(request, levels).
void collect(const NpcRouteManager& m,const NpcRouteRequest& r,NpcNodeSearch& s,std::int32_t levels){
    const auto& graph=graph_of(m);
    s.region_count_604=0;
    if(s.start_region_464>=0){
        s.regions[0]={s.start_region_464,-1,0.0F};s.region_count_604=1;
        for(std::int32_t level=0;level<levels;++level){
            const auto n=s.region_count_604;
            for(std::int32_t i=0;i<n;++i)expand_region(m,s,static_cast<std::size_t>(i));
        }
    }
    if(!client_valid(r))throw std::runtime_error("NPC node search: 0015e110 reads +58 of an actor that is gone");
    if(r.client->allowed_region)
        for(std::int32_t i=0;i<s.region_count_604;++i){
            auto& e=s.regions[static_cast<std::size_t>(i)];
            if(!r.client->allowed_region(e.region_484))e.region_484=-1;
        }
    for(std::int32_t i=0;i<s.region_count_604;++i){
        const auto entry=s.regions[static_cast<std::size_t>(i)];
        if(entry.region_484<0)continue;
        if(!(entry.distance_48c<s.radius_460))continue;
        for(const auto n:group(m,entry.region_484)){
            if(n==s.exclude_470)continue;
            const auto& node=graph.node(n);
            if(!(node.flags&s.mask_468))continue;
            if(!m.node_reserved_by)throw std::runtime_error("NPC node search: the reservations 0014fa00 are not linked (NpcRouteManager::node_reserved_by)");
            const auto owner=m.node_reserved_by(n);
            if(!s.route_482&&owner&&*owner!=r.client->id)continue;
            const auto p=graph.position(n);
            if(!(vu::sqrt(length_squared(sub4(s.target_450,p)))<s.radius_460))continue;
            float score=0.0F;
            if(!s.route_482)
                score=npc_node_search_score({s.query_430,s.query_434,s.query_438},p,0,s.self_440,s.target_450);
            else score=std::fabs(s.self_440[2]-p[2])<std::bit_cast<float>(0x40266666U)?0.0F:5000.0F;   // 2.6
            if(!(score<=5000.0F))continue;
            std::uint32_t second_mask=0;std::int32_t second=-1;
            std::size_t link=node.links.size();
            if(s.query_438){
                second_mask=(s.mask_468&0x40U)?6U:0x40U;
                if(node.flags&second_mask)second=n;
                link=0;
            }
            for(;;){
                float f=score;
                if(s.query_438&&second<0)f=score+1000.0F;
                add_candidate(s,n,second,f);
                second=-1;
                // The links: the first one (not type 7) whose target has a
                // second-node type; at the end of the links the last target
                // looked at (not type 7) is taken too.
                while(link<node.links.size()){
                    const auto& l=node.links[link++];
                    second=l.target;
                    if(l.type==7){second=-1;continue;}
                    if(graph.node(second).flags&second_mask)break;
                }
                if(second<0)break;
            }
        }
    }
    for(auto it=s.list.begin();it!=s.list.end();){                             // 0015e7a0: drop scores not below 5000
        if(s.slots[*it].score_0c<5000.0F)++it;
        else it=s.list.erase(it);
    }
}
// 00161bc0(request, node, mask): the LOS check of a candidate node (a
// deferred request, mode 2, answered to the route manager).
void los_check(NpcRouteManager& m,NpcRouteRequest& r,std::int32_t node,std::uint32_t mask){
    const auto& graph=graph_of(m);
    auto& s=r.search;
    RootVector origin=s.target_450;
    origin[2]=origin[2]+(s.route_482?std::bit_cast<float>(0x40266666U):4.5F);    // 2.6 / 4.5
    RootVector d=sub4(graph.position(node),s.target_450);                       // from the target, not the raised origin
    if((mask&graph.node(node).flags)&4U)d[2]=d[2]-3.0F;
    NpcLosRequest q;
    q.origin_30=origin;q.displacement_40=d;
    NpcRouteManager* manager=&m;
    q.receiver_24=[manager](const NpcLosResult& res){npc_route_los_result(*manager,res.hit_30);};
    q.serial_28=&r.id_08;                                                       // +28 = request +8
    q.mask_58=4;q.mode_5c=2;
    if(!npc_los_request(los_of(m),q))
        throw std::runtime_error("NPC node search: the LOS manager's 30 requests are in use (the original drops the check and the route manager waits for a result that never comes)");
    m.los_pending_9d0=1;
}
// 00162010(request, node, clear) / 00161fa0(request, node, clear).
void mark_node(const NpcRouteManager& m,NpcNodeSearch& s,std::int32_t node,std::int32_t clear){
    const bool crouch=(graph_of(m).node(node).flags&4U)!=0;
    for(const auto k:s.list){
        auto& c=s.slots[k];
        if(c.node_10==node)c.clear_18=clear;
        if(c.second_14==node&&!crouch)c.second_clear_1c=clear;
    }
}
void mark_second(const NpcRouteManager& m,NpcNodeSearch& s,std::int32_t node,std::int32_t clear){
    const bool crouch=(graph_of(m).node(node).flags&4U)!=0;
    for(const auto k:s.list){
        auto& c=s.slots[k];
        if(c.second_14==node)c.second_clear_1c=clear;
        if(c.node_10==node&&!crouch)c.clear_18=clear;
    }
}
}

float npc_node_search_score(const NpcNodeSearchQuery& query,const RootVector& node_position,std::int32_t a2,const RootVector& self,
    const RootVector& target){
    RootVector self_node=sub4(self,node_position);
    RootVector node_target=sub4(node_position,target);
    RootVector self_target=sub4(self,target);
    bool unnormalized=true;
    if(!zero_xyz(self_target)){
        const float q=vu::rsqrt(length_squared(self_target));                 // vrsqrt Q, vf0.w, vf3.w
        for(std::size_t k=0;k<3;++k)self_target[k]=self_target[k]*q;            // vmulq.xyz
        unnormalized=false;
    }
    float length=0.0F,dot=0.0F;
    if(!zero_xyz(node_target)){
        length=vu::sqrt(length_squared(node_target));
        const float q=vu::div(1.0F,length);                                    // vdiv Q, vf0.w, vf4.x
        for(std::size_t k=0;k<3;++k)node_target[k]=node_target[k]*q;
        if(!unnormalized){
            const float x=self_target[0]*node_target[0],y=self_target[1]*node_target[1],z=self_target[2]*node_target[2];
            dot=(x+y)+z;
        }
    }
    if(dot<std::bit_cast<float>(0xbe84816fU))return 10000.0F;                // cos 105 degrees
    const float d=vu::sqrt(length_squared(self_node));
    float score=0.0F;
    if(query.far_4<=0.0F)score=(-0.25F*d)-length;
    else score=d+d;
    if(length<query.near_0||!(length<=query.far_4))score=score+500.0F;
    if(query.byte_8&&a2<0)score=score+1000.0F;
    return score;
}

std::int32_t npc_search_start_region(const NpcPathFollower& pf,const NpcNodeGraph& graph){
    std::int32_t region=pf.destination_5c;
    if(region>=0||!(pf.flags_60&1U))return region;
    std::int32_t node=pf.hold_4c;
    std::size_t steps=0;
    while(node>=0&&region<0){                                                   // 00161340
        if(++steps>graph.nodes.size())
            throw std::runtime_error("NPC node search: 001612d0 walks a patrol cycle that does not return to the hold node (the original never ends)");
        node=npc_next_patrol_node(graph,node);                                  // 0015d680
        if(node==pf.hold_4c)return region;
        if(node>=0){
            const auto r=graph.node(node).parameter_7c;
            if(r!=0)region=r;                                                   // region 0 does not count
        }
    }
    return region;
}

std::optional<std::uint32_t> npc_node_reserved_by(const NpcAiSystem& system,std::int32_t node){
    for(const auto& k:system.think_list){
        if(!k.component)throw std::runtime_error("NPC node search: a think-list entry without its component");
        if(!k.life_state)throw std::runtime_error("NPC node search: a think-list entry without its life state");
        if(k.life_state()==3)continue;
        const auto& c=*k.component;
        if(c.cover_node_e4==node||c.firing_node_e0==node||c.path.field_58==node)return k.actor_id;
    }
    return std::nullopt;
}

void link_npc_node_searches(NpcRouteManager& m,NpcAiSystem& system,NpcLosService& los){
    m.los=&los;
    m.far_distance_34c=&system.far_distance;                                   // 0053834c, one global
    NpcAiSystem* s=&system;
    m.node_reserved_by=[s](std::int32_t node){return npc_node_reserved_by(*s,node);};
}

std::uint8_t npc_node_search_request(NpcRouteManager& m,const std::shared_ptr<NpcRouteClient>& client,float radius,
    const NpcNodeSearchQuery& query,const RootVector& self,const RootVector& target,std::int32_t start_region,std::uint32_t mask,
    std::int8_t kind,std::int32_t exclude){
    if(!client)throw std::runtime_error("NPC node search: a request without an actor");
    if(start_region<0){deliver_client(client,1,-1,0);return 0;}                // 0015ee58: path event 1 (+2c not written)
    NpcRouteRequest* r=nullptr;
    bool fresh=false;
    if(const auto q=npc_route_queued(m,client->id,2))r=&m.requests[*q];        // 00163e20(manager, actor, 2)
    else{
        // 0015f780: the free list in order.
        for(const auto i:m.free_2c){
            const auto& c=m.requests[i];
            if(!client_valid(c)||c.client->id!=client->id)continue;
            if(c.kind_0c!=2||c.byte_0e==0||c.search.mask_468!=mask)continue;
            if(!same4(self,c.search.self_440)||!same4(target,c.search.target_450))continue;
            if(std::isnan(query.near_0)||std::isnan(query.far_4)||query.byte_8!=c.search.query_438)continue;
            if(c.search.node_478>=0)deliver_client(client,0,c.search.node_478,c.search.second_47c);
            else deliver_client(client,1,-1,0);
            return 1;
        }
        if(m.free_2c.empty())throw std::runtime_error("NPC node search: no free request (001642e0 returns null)");
        const auto i=m.free_2c.front();m.free_2c.pop_front();
        r=&m.requests[i];
        r->first_0d=1;r->byte_0e=0;r->kind_0c=2;
        r->client=client;r->id_04=client->id;r->id_08=client->id;
        fresh=true;
    }
    auto& s=r->search;
    s.query_430=query.near_0;s.query_434=query.far_4;s.query_438=query.byte_8;
    s.self_440=self;s.target_450=target;s.radius_460=radius;s.start_region_464=start_region;s.mask_468=mask;
    s.kind_46c=kind;s.exclude_470=exclude;
    s.checking_second_480=0;s.byte_481=0;s.route_482=0;
    if(!fresh)return 0;
    m.queue_20.push_back(static_cast<std::size_t>(r-m.requests.data()));        // 00164120
    return 1;
}

bool npc_node_search_step(NpcRouteManager& m,NpcRouteRequest& r){
    auto& s=r.search;
    if(r.first_0d){
        r.first_0d=0;
        s.list.clear();
        collect(m,r,s,s.route_482?0:2);
        s.node_478=-1;s.second_47c=-1;s.found_474=0;
        if(!s.list.empty()){
            if(s.kind_46c==0){
                const auto k=pop_front(s);
                s.node_478=s.slots[k].node_10;s.second_47c=s.slots[k].second_14;
            }else los_check(m,r,front(s).node_10,s.mask_468);
        }
    }
    if(s.found_474==2)s.found_474=0;
    if(s.byte_481)return s.list.empty();
    if(s.node_478>=0){
        r.byte_0e=1;
        deliver(r,0,s.node_478,s.second_47c);
        return true;
    }
    if(!s.list.empty())return false;
    r.byte_0e=1;
    deliver(r,1,-1,0);
    return true;
}

void npc_node_search_los_result(NpcRouteManager& m,NpcRouteRequest& r,std::uint8_t hit){
    auto& s=r.search;
    if(s.list.empty())throw std::runtime_error("NPC node search: an LOS result for an empty candidate list (the original reads the sentinel)");
    const std::int32_t clear=hit!=0?0:1;
    if(!s.checking_second_480)mark_node(m,s,front(s).node_10,clear);
    else mark_second(m,s,front(s).second_14,clear);
    const std::int32_t want=s.kind_46c!=2?1:0;
    bool asked=false;
    do{
        if(s.list.empty())throw std::runtime_error("NPC node search: 00161db0 on an empty candidate list (the original reads the sentinel)");
        std::size_t at=s.list.front();
        const auto f=s.slots[at];
        if(f.second_14<0){
            if(f.clear_18==want){s.node_478=f.node_10;s.second_47c=-1;s.found_474=2;}
            at=pop_front(s);
        }else if(!s.checking_second_480){
            if(f.clear_18==want)s.checking_second_480=1;
            else at=pop_front(s);
        }else{
            if(f.second_clear_1c!=want){s.node_478=f.node_10;s.second_47c=f.second_14;s.found_474=2;}
            at=pop_front(s);
            s.checking_second_480=0;
        }
        if(s.checking_second_480){
            if(s.slots[at].second_clear_1c<0){
                los_check(m,r,s.slots[at].second_14,~s.mask_468);
                asked=true;
            }
        }else if(!(s.node_478>=0&&!s.byte_481)&&!s.list.empty()&&front(s).clear_18<0){
            los_check(m,r,front(s).node_10,s.mask_468);
            asked=true;
        }
    }while(s.found_474!=2&&!asked&&!s.list.empty());
}
}
