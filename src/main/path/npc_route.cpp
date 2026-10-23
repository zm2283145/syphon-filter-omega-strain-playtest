#include "npc_route.hpp"
#include "npc_node_search.hpp"
#include "vu_float.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
bool bit(std::uint8_t v,unsigned i){return ((v>>i)&1U)!=0;}
void set_bit(std::uint8_t& v,unsigned i,bool on){v=static_cast<std::uint8_t>((v&~(1U<<i))|(on?(1U<<i):0U));}
std::uint32_t read_word(std::span<const std::uint8_t> b,std::size_t pos){
    if(pos+4>b.size())throw std::runtime_error("GRP: truncated section");
    return static_cast<std::uint32_t>(b[pos])|(static_cast<std::uint32_t>(b[pos+1])<<8)|
        (static_cast<std::uint32_t>(b[pos+2])<<16)|(static_cast<std::uint32_t>(b[pos+3])<<24);
}
// vsub.xyzw, vmul.xyz, then vmulax / vmadday / vmaddz (ACC order).
float distance_squared(const RootVector& a,const RootVector& b){
    const float x=a[0]-b[0],y=a[1]-b[1],z=a[2]-b[2];
    const float x2=x*x,y2=y*y,z2=z*z;
    return (x2+y2)+z2;
}
// ... then vsqrt Q, vaddq.x.
float distance(const RootVector& a,const RootVector& b){return vu::sqrt(distance_squared(a,b));}
// The VU sign flag of a - b in x, y or z (cfc2 vi17 & 0xe0): a negative
// result, -0.0 included.
bool any_sign(const RootVector& a,const RootVector& b){
    for(int i=0;i<3;++i)if(std::signbit(a[i]-b[i]))return true;
    return false;
}
// vmini / vmax as the interpreter models them (an ordinary comparison).
float vu_min(float a,float b){return b<a?b:a;}
float vu_max(float a,float b){return b>a?b:a;}
const NpcNodeGraph& graph_of(const NpcRouteManager& m){
    if(!m.graph)throw std::runtime_error("NPC route: no node collection");
    return *m.graph;
}
NpcRegions& regions_of(const NpcRouteManager& m){
    if(!m.regions)throw std::runtime_error("NPC route: no region groups");
    return *m.regions;
}
// The request handle's validity (+0 non-null and its +c equal to +4).
bool client_valid(const NpcRouteRequest& r){
    if(!r.client)return false;
    if(r.client->id!=r.id_04)return false;
    return !r.client->valid||r.client->valid();
}
// The ID compare of 00163e20 / 00163c50 / 00163fd0: a request whose handle
// does not resolve reads +c of a null object.
std::uint32_t queued_id(const NpcRouteRequest& r){
    if(!client_valid(r))throw std::runtime_error("NPC route: a queued request whose actor is gone (the original reads +c of null)");
    return r.client->id;
}
// Path event 2 / 3 (+28 = -1) to the request's actor +58 component, transport 1.
void deliver(const NpcRouteRequest& r,std::uint8_t event){
    if(!client_valid(r))return;
    if(!r.client->deliver)throw std::runtime_error("NPC route: the requesting actor has no delivery for path events");
    r.client->deliver({event,-1});
}
// State 3's failures (00162f70 a pool exhausted while linking, 001631a8 after
// closing, 00163298 the open list empty) send event 3 without the found
// path's guard (00162bd4): a null or stale handle sends to +58 of null
// (address 0x58, kernel memory; the EE interpreter holds 0 there).
void deliver_failure(const NpcRouteRequest& r,const char* at){
    if(!client_valid(r))
        throw std::runtime_error(std::string("NPC route: ")+at+" sends path event 3 for a request without a valid actor to +58 of null (address 0x58, kernel memory): not hosted");
    deliver(r,3);
}
// 00163410(request, node): a pool entry (none when the counter is 400) with
// g = |+30 - node|, f = g + |+20 - node|, no parent, inserted as open.
void insert_sorted(std::vector<std::int32_t>& list,const NpcRouteManager& m,std::int32_t e);
bool open_node(NpcRouteManager& m,NpcRouteRequest& r,std::int32_t node){
    if(!(m.pool_counter_798<static_cast<std::int32_t>(npc_route_pool_size)))return false;
    const auto e=m.pool_counter_798++;
    const auto p=graph_of(m).position(node);
    auto& x=m.pool[static_cast<std::size_t>(e)];
    const float g=distance(r.position_30,p);
    x.node_14=node;x.g_18=g;
    x.f_0c=g+distance(r.destination_20,p);
    x.parent_10=-1;x.list_1c=1;
    insert_sorted(r.open,m,e);++r.open_count_74;
    return true;
}
// 00164b70: before the first entry whose f is not below the new one
// (c.ole.s new, old), else at the end.
void insert_sorted(std::vector<std::int32_t>& list,const NpcRouteManager& m,std::int32_t e){
    const float f=m.pool[static_cast<std::size_t>(e)].f_0c;
    auto it=list.begin();
    for(;it!=list.end();++it)if(f<=m.pool[static_cast<std::size_t>(*it)].f_0c)break;
    list.insert(it,e);
}
std::int32_t find_node(const std::vector<std::int32_t>& list,const NpcRouteManager& m,std::int32_t node){
    for(const auto e:list)if(m.pool[static_cast<std::size_t>(e)].node_14==node)return e;
    return -1;
}
void erase(std::vector<std::int32_t>& list,std::int32_t e){
    list.erase(std::find(list.begin(),list.end(),e));
}
}

std::vector<std::vector<std::int32_t>> decode_population_groups(std::span<const std::uint8_t> bytes,const Population& population){
    const auto s=std::find_if(population.sections.begin(),population.sections.end(),[](const PopulationSection& x){return x.tag=="GRP";});
    std::vector<std::vector<std::int32_t>> groups;
    if(s==population.sections.end())return groups;                           // 001c0ee0: 004ffbe4 = 0
    std::size_t pos=s->offset+8;
    groups.reserve(s->count);
    for(std::uint32_t i=0;i<s->count;++i){
        const auto n=read_word(bytes,pos);pos+=4;
        if(n>(s->end-pos)/4)throw std::runtime_error("GRP: a group larger than its section");
        std::vector<std::int32_t> g;g.reserve(n);
        for(std::uint32_t k=0;k<n;++k){
            const auto v=static_cast<std::int32_t>(read_word(bytes,pos));pos+=4;
            if(v<0||static_cast<std::size_t>(v)>=population.nodes.size())throw std::runtime_error("GRP: a node index outside the node collection");
            g.push_back(v);
        }
        groups.push_back(std::move(g));
    }
    if(pos>s->end)throw std::runtime_error("GRP: groups beyond the section");
    return groups;
}

void build_npc_region_boxes(NpcRegions& regions,const NpcNodeGraph& graph){
    if(regions.boxes||regions.groups.empty())return;
    std::vector<NpcRegionBox> boxes;boxes.reserve(regions.groups.size());
    const float big=std::numeric_limits<float>::max();
    for(const auto& g:regions.groups){
        NpcRegionBox b{{big,big,big,1.0F},{-big,-big,-big,1.0F}};
        for(const auto n:g){
            const auto p=graph.position(n);
            for(int i=0;i<3;++i){b.minimum[i]=vu_min(b.minimum[i],p[i]);b.maximum[i]=vu_max(b.maximum[i],p[i]);}
        }
        // (2, 2, 2, 0) through vmulax / vmsubw: ACC 0 - v -> (-2, -2, -2, -0).
        for(int i=0;i<3;++i){b.minimum[i]=b.minimum[i]+(-2.0F);b.maximum[i]=b.maximum[i]+2.0F;}
        boxes.push_back(b);
    }
    regions.boxes=std::move(boxes);
}

NpcNearestNode npc_nearest_region_node(const NpcRegions& regions,const NpcNodeGraph& graph,std::int32_t region,
    const RootVector& point,std::uint32_t mask,std::int32_t exclude){
    NpcNearestNode out;
    if(region<0)return out;
    if(static_cast<std::size_t>(region)>=regions.groups.size())throw std::runtime_error("NPC route: region "+std::to_string(region)+" outside the groups");
    float best=std::numeric_limits<float>::max();
    for(const auto n:regions.groups[static_cast<std::size_t>(region)]){
        if(n==exclude)continue;
        if(!(mask&graph.node(n).flags))continue;
        const float d=distance_squared(point,graph.position(n));
        if(d<best){best=d;out.node=n;out.distance_squared=d;}
    }
    return out;
}

std::int32_t npc_region_at(NpcRegions& regions,const NpcNodeGraph& graph,const RootVector& point,std::int32_t previous,
    std::optional<float> caller_f20,const char* site){
    build_npc_region_boxes(regions,graph);
    auto inside=[&](std::size_t i){
        const auto& b=(*regions.boxes)[i];
        return !any_sign(point,b.minimum)&&!any_sign(b.maximum,point);
    };
    if(previous>=0&&regions.boxes){
        if(static_cast<std::size_t>(previous)>=regions.boxes->size())throw std::runtime_error("NPC route: region "+std::to_string(previous)+" outside the groups");
        if(inside(static_cast<std::size_t>(previous)))return previous;
    }
    std::int32_t best=-1;std::int32_t holding=0;float best_d=0.0F;
    for(std::size_t i=0;i<regions.groups.size();++i){
        if(!inside(i))continue;
        const auto r=static_cast<std::int32_t>(i);
        if(holding==0){best=r;++holding;continue;}
        if(holding==1){
            const auto first=npc_nearest_region_node(regions,graph,best,point,0x88,-1);
            if(first.node>=0)best_d=first.distance_squared;                    // 0015d958 / 0015d984
            else if(caller_f20)best_d=*caller_f20;                              // f20 as the caller left it
            else throw std::runtime_error(std::string("NPC route: 0015d790 with a first region without a Nav or Path node compares "
                "the caller's f20, which this call site does not host (")+(site?site:"unnamed caller")+")");
        }
        const auto n=npc_nearest_region_node(regions,graph,r,point,0x88,-1);
        if(n.node>=0&&n.distance_squared<best_d){best_d=n.distance_squared;best=r;}
        ++holding;
    }
    return best;
}

NpcRouteManager::NpcRouteManager(const NpcNodeGraph& g,NpcRegions& r):graph(&g),regions(&r){
    for(std::size_t i=0;i<npc_route_request_count;++i){
        requests[i].id_04=0xffffffffU;                                         // 1644b0: +0 = 0, +4 = -1
        free_2c.push_back(i);
    }
}

std::optional<std::uint8_t> npc_route_request(NpcRouteManager& m,const std::shared_ptr<NpcRouteClient>& client,std::int32_t start,
    std::int32_t previous,std::int32_t region,std::int32_t destination_node,std::int32_t destination_region,const RootVector* point,
    const std::shared_ptr<NpcRouteBuffer>& buffer){
    if(!buffer)return std::uint8_t{0};
    const auto& graph=graph_of(m);
    const bool trivial=start==destination_node&&(!point||!client||client->local_14==0);
    if(trivial){                                                                 // 0015f288
        buffer->nodes.clear();
        if(destination_node>=0){
            if(!buffer->array_20)throw std::runtime_error("NPC route: a route into a buffer without its node array");
            if(static_cast<std::int32_t>(buffer->nodes.size())>=buffer->capacity_24)throw std::runtime_error("NPC route: a route longer than the buffer");
            buffer->nodes.push_back(destination_node);
        }
        if((client&&client->local_14!=0)||!point){
            buffer->point_00=graph.position(destination_node);buffer->destination_10=destination_node;
        }else{buffer->point_00=*point;buffer->destination_10=-1;}
        if(client){
            if(!client->deliver)throw std::runtime_error("NPC route: the requesting actor has no delivery for path events");
            client->deliver({2,-1});                                            // path event 2, transport 1
        }
        return std::uint8_t{1};
    }
    if(!client)throw std::runtime_error("NPC route: 00163e20 with a null actor");
    // 00163e20(manager, actor, 1); the actor's +f4 translation.
    const auto existing=npc_route_queued(m,client->id,1);
    if(!client->translation)throw std::runtime_error("NPC route: the requesting actor's translation (+f4) is not hosted");
    const RootVector position=client->translation();
    std::size_t index{};bool fresh=false;
    if(existing&&!(!m.queue_20.empty()&&m.queue_20.front()==*existing)){
        index=*existing;
    }else{
        // 0015f510 (a finished request in the free list with the same
        // start, destination and regions, within 4.0 of the positions) is
        // looked up, but its answer is tested with lbu +11 / bgez, which is
        // always taken: the cached failure never answers. 001642e0 then
        // takes the free list's first request.
        if(m.free_2c.empty())throw std::runtime_error("NPC route: no free request (001642e0 returns null)");
        index=m.free_2c.front();m.free_2c.pop_front();
        auto& r=m.requests[index];
        r.first_0d=1;r.byte_0e=0;r.kind_0c=1;
        r.client=client;r.id_04=client->id;r.id_08=client->id;
        fresh=true;
    }
    auto& r=m.requests[index];
    r.buffer_14=buffer;r.start_40=start;r.previous_44=previous;r.position_30=position;r.destination_node_4c=destination_node;
    if(point){r.destination_20=*point;r.destination_region_50=destination_region;r.node_destination_12=0;}
    else{
        r.destination_region_50=graph.node(destination_node).parameter_7c;
        r.destination_20=graph.position(destination_node);r.node_destination_12=1;
    }
    r.region_48=region;
    if(!fresh)return std::nullopt;
    r.pool_base_54=0;
    // 0015f238..0015f248: queued without a start node or a destination node,
    // or when the current region equals the destination region argument
    // (-1 for 0015fb20, which passes no region: an NPC outside every region).
    if(start<0||destination_node<0||region==destination_region){
        m.queue_20.push_back(index);                                            // 00164120
        return std::uint8_t{1};
    }
    const bool done=npc_route_search(m,r);
    m.free_2c.push_back(index);                                                 // 001642b0
    return static_cast<std::uint8_t>(done?1:0);
}

std::optional<std::uint8_t> npc_route_to_node(NpcRouteManager& m,const NpcPathFollower& pf,const std::shared_ptr<NpcRouteClient>& client,
    std::int32_t node){
    std::int32_t start=pf.hold_4c,previous=-1;
    if((bit(pf.flags_60,1)||bit(pf.flags_60,2))&&start<0){start=pf.current_50;previous=pf.previous_54;}
    return npc_route_request(m,client,start,previous,pf.destination_5c,node,-1,nullptr,pf.route_74);
}
std::optional<std::uint8_t> npc_route_to_point(NpcRouteManager& m,const NpcPathFollower& pf,const std::shared_ptr<NpcRouteClient>& client,
    const RootVector& point,std::int32_t region){
    std::int32_t start=pf.hold_4c,previous=-1;
    if((bit(pf.flags_60,1)||bit(pf.flags_60,2))&&start<0){start=pf.current_50;previous=pf.previous_54;}
    return npc_route_request(m,client,start,previous,pf.destination_5c,-1,region,&point,pf.route_74);
}

namespace {
// The path message's target of 00162080: the request's actor (+58
// component or the actor), transport 1. 00163ad0 runs the search only for a
// valid handle.
void deliver_route_event(const NpcRouteRequest& r,std::uint8_t event){
    if(!client_valid(r))throw std::runtime_error("NPC route: a route answer to an actor that is gone (the original reads +58 of null)");
    if(!r.client->deliver)throw std::runtime_error("NPC route: the requesting actor has no delivery for path events");
    r.client->deliver({event,-1});
}
// 00162550..00162690: the global node search object for states 0-2.
void set_up_global_search(NpcRouteManager& m,const NpcRouteRequest& r){
    auto& g=m.global_9f0;
    const bool valid=client_valid(r);
    m.search_done_9fe=0;
    g.client=valid?r.client:nullptr;
    g.first_0d=1;g.kind_0c=2;
    g.id_04=valid?r.client->id:0xffffffffU;
    if(valid)g.id_08=r.client->id;
    auto& s=g.search;
    s.mask_468=0x80;s.exclude_470=-1;s.kind_46c=1;
    s.checking_second_480=0;s.byte_481=1;s.route_482=1;
    s.query_430=0.0F;s.query_434=0.0F;s.query_438=0;
    if(!m.far_distance_34c)throw std::runtime_error("NPC route: the global search's radius 0053834c is not linked (link_npc_node_searches)");
    s.radius_460=*m.far_distance_34c;                                           // 0053834c
}
// 00162140-style: the state-2 LOS request from the destination raised by 2.6
// to the actor's position (receiver the route manager, the serial into +8,
// mask 4, mode 2); 004ed9d0 = 1 whether or not it was taken.
void request_route_los(NpcRouteManager& m,NpcRouteRequest& r){
    if(!m.los)throw std::runtime_error("NPC route: the state-2 LOS check needs the LOS manager 004ea0b0 (NpcRouteManager::los)");
    RootVector origin=r.destination_20;
    origin[2]=origin[2]+std::bit_cast<float>(0x40266666U);                     // 2.6
    NpcLosRequest q;
    q.origin_30=origin;
    q.displacement_40={r.position_30[0]-r.destination_20[0],r.position_30[1]-r.destination_20[1],r.position_30[2]-r.destination_20[2],0.0F};
    NpcRouteManager* manager=&m;
    q.receiver_24=[manager](const NpcLosResult& res){npc_route_los_result(*manager,res.hit_30);};
    q.serial_28=&r.id_08;q.mask_58=4;q.mode_5c=2;
    if(!npc_los_request(*m.los,q))
        throw std::runtime_error("NPC route: the LOS manager's 30 requests are in use (the original drops the check and the route manager waits for a result that never comes)");
    m.los_pending_9d0=1;
}
}

bool npc_route_search(NpcRouteManager& m,NpcRouteRequest& r){
    const auto& graph=graph_of(m);
    if(!r.buffer_14)throw std::runtime_error("NPC route: a route search without a buffer");
    auto& buffer=*r.buffer_14;
    if(r.first_0d){
        r.first_0d=0;r.found_11=0;
        if(r.pool_base_54==0)r.pool_base_54=m.pool_counter_798;
        r.open.clear();r.closed.clear();r.open_count_74=0;r.closed_count_94=0;
        m.region_candidates_ee010.clear();                                     // 00163330(004ee010, 0, 00489ff8)
        if(r.start_40>=0){
            open_node(m,r,r.start_40);
            if(r.previous_44>=0){open_node(m,r,r.previous_44);r.start_40=-1;}
            r.state_10=r.destination_node_4c>=0?3:1;
        }else{
            if(r.region_48==r.destination_region_50&&
                std::fabs(r.position_30[2]-r.destination_20[2])<std::bit_cast<float>(0x40266666U)){
                r.state_10=2;
                request_route_los(m,r);
            }else{
                r.state_10=0;
                if(!client_valid(r))throw std::runtime_error("NPC route: 00162080 reads +58 of an actor that is gone");
                if(r.client->path_nodes_50_54){                                 // an AI controller (virtual +88 = 1 or 2)
                    const auto nodes=r.client->path_nodes_50_54();
                    for(const auto n:nodes)
                        if(n>=0&&(graph.node(n).flags&0x80U))open_node(m,r,n);
                }
            }
            auto& s=m.global_9f0.search;                                        // 00162518
            s.self_440=r.position_30;s.target_450=r.position_30;s.start_region_464=r.region_48;
        }
        if(r.state_10!=3)set_up_global_search(m,r);
    }
    auto& g=m.global_9f0;
    bool done=false;
    if(r.state_10==0){
        if(!npc_node_search_step(m,g))return false;
        if(r.open_count_74==0){if(m.port_route_trace)m.port_route_trace(r,"event 3, 00162080 state 0");m.search_done_9fe=1;deliver_route_event(r,3);done=true;}
        else r.state_10=r.destination_node_4c>=0?3:1;
        g.first_0d=1;
        if(done)m.pool_counter_798=r.pool_base_54;
        return done;
    }
    if(r.state_10==1){
        if(g.first_0d){
            g.search.self_440=r.destination_20;g.search.target_450=r.destination_20;g.search.start_region_464=r.destination_region_50;
            if(r.previous_44>=0)g.search.mask_468|=8U;
        }
        if(!npc_node_search_step(m,g))return false;
        if(m.region_candidates_ee010.empty()){if(m.port_route_trace)m.port_route_trace(r,"event 3, 00162080 state 1");m.search_done_9fe=1;deliver_route_event(r,3);done=true;}
        else r.state_10=3;
        g.first_0d=1;
        if(done)m.pool_counter_798=r.pool_base_54;
        return done;
    }
    if(r.state_10==2){
        if(g.first_0d)return false;                                             // the LOS answer is pending
        if(g.search.node_478==0){r.state_10=0;g.first_0d=1;return false;}      // blocked
        buffer.nodes.clear();                                                   // clear: the destination alone
        if(r.destination_node_4c>=0){
            if(!buffer.array_20)throw std::runtime_error("NPC route: a route into a buffer without its node array");
            buffer.nodes.push_back(r.destination_node_4c);
        }
        buffer.point_00=r.destination_20;
        buffer.destination_10=r.destination_node_4c;
        m.search_done_9fe=1;
        deliver_route_event(r,2);
        m.pool_counter_798=r.pool_base_54;
        return true;
    }
    if(r.state_10!=3)return false;                                              // (no other state is set)
    while(r.open_count_74!=0){                                                  // 001631f8
        // 00164780: the first open entry.
        const auto s0=r.open.front();r.open.erase(r.open.begin());--r.open_count_74;
        const auto entry=m.pool[static_cast<std::size_t>(s0)];
        const auto& node=graph.node(entry.node_14);
        bool found=false;
        if(r.destination_region_50==node.parameter_7c){
            if(r.destination_node_4c==entry.node_14)found=true;
            else for(const auto n:m.region_candidates_ee010)if(n==entry.node_14){found=true;break;}   // 004ee014
        }
        if(found){
            buffer.nodes.clear();
            if(!buffer.array_20)throw std::runtime_error("NPC route: a route into a buffer without its node array");
            for(auto e=s0;e>=0;e=m.pool[static_cast<std::size_t>(e)].parent_10){
                // No bound check in the original (the array has +24 words).
                if(static_cast<std::int32_t>(buffer.nodes.size())>=buffer.capacity_24)
                    throw std::runtime_error("NPC route: a route of more nodes than the buffer holds (the original writes past its array)");
                buffer.nodes.push_back(m.pool[static_cast<std::size_t>(e)].node_14);
            }
            buffer.point_00=r.destination_20;
            buffer.destination_10=r.node_destination_12?r.destination_node_4c:-1;
            m.search_done_9fe=1;
            deliver(r,2);
            r.found_11=1;
            done=true;break;
        }
        bool failed=false;
        if(r.start_40==entry.node_14||(node.flags&0x88U)){
            const auto from=graph.position(entry.node_14);
            // Port-only (132ob5): a client with port_overlay_links also takes the co-op overlay's entry links.
            const auto* overlay=r.client&&r.client->port_overlay_links&&static_cast<std::size_t>(entry.node_14)<graph.port_overlay.size()?
                &graph.port_overlay[static_cast<std::size_t>(entry.node_14)]:nullptr;
            const std::size_t own=node.links.size(),all=own+(overlay?overlay->size():0);
            for(std::size_t k=0;k<all;++k){
                const auto& l=k<own?node.links[k]:(*overlay)[k-own];
                if(l.type==7||l.target<0)continue;
                const auto to=graph.position(l.target);
                const float cost=entry.g_18+distance(to,from);
                std::int32_t a=find_node(r.open,m,l.target);
                if(a<0)a=find_node(r.closed,m,l.target);
                if(a>=0&&!(cost<m.pool[static_cast<std::size_t>(a)].g_18))continue;
                if(a<0){
                    if(!(m.pool_counter_798<static_cast<std::int32_t>(npc_route_pool_size))){
                        if(m.port_route_trace)m.port_route_trace(r,"event 3, 00162f70");m.search_done_9fe=1;deliver_failure(r,"00162f70");    // pool exhausted
                        failed=true;break;
                    }
                    a=m.pool_counter_798++;
                }else{
                    auto& old=m.pool[static_cast<std::size_t>(a)];
                    if(old.list_1c==1){erase(r.open,a);--r.open_count_74;}
                    else if(old.list_1c==2){erase(r.closed,a);--r.closed_count_94;}
                }
                auto& x=m.pool[static_cast<std::size_t>(a)];
                x.node_14=l.target;x.g_18=cost;
                x.f_0c=cost+distance(r.destination_20,to);
                x.parent_10=s0;x.list_1c=1;
                insert_sorted(r.open,m,a);++r.open_count_74;
            }
        }
        m.pool[static_cast<std::size_t>(s0)].list_1c=2;
        insert_sorted(r.closed,m,s0);++r.closed_count_94;
        if(!(m.pool_counter_798<static_cast<std::int32_t>(npc_route_pool_size))){
            if(m.port_route_trace)m.port_route_trace(r,"event 3, 001631a8");m.search_done_9fe=1;deliver_failure(r,"001631a8");
            done=true;break;
        }
        if(failed){done=true;break;}                                            // (not reached: the counter is 400 here)
    }
    if(!done){if(m.port_route_trace)m.port_route_trace(r,"event 3, 00163298");m.search_done_9fe=1;deliver_failure(r,"00163298");done=true;}
    m.pool_counter_798=r.pool_base_54;
    return done;
}

void update_npc_route_manager(NpcRouteManager& m,const NpcRouteFrameHost& host){
    if(!m.queue_20.empty()&&m.los_pending_9d0==0){
        const auto index=m.queue_20.front();
        auto& r=m.requests[index];
        bool done=true;
        if(client_valid(r)){
            if(r.kind_0c==1){
                if(r.buffer_14&&r.buffer_14->array_20)done=npc_route_search(m,r);
            }else done=npc_node_search_step(m,r);                               // 00161950 (N5c)
        }
        if(done&&!client_valid(r)&&r.first_0d==0&&m.port_route_trace)m.port_route_trace(r,"00163ad0 frees a started request of an actor that is gone");
        if(done){m.queue_20.pop_front();m.free_2c.push_back(index);}
    }
    const auto slot=host.frame&3U;
    if(!host.player_translation)throw std::runtime_error("NPC route: the manager's update needs the player slots (003fb8d0)");
    if(const auto p=host.player_translation(slot)){
        const auto region=npc_region_at(regions_of(m),graph_of(m),*p,m.player_regions_9c0[slot],host.frame_delta_f20,
            "00163c1c: the main update's frame delta 00168764");
        if(region>=0)m.player_regions_9c0[slot]=region;
    }
}

void npc_route_los_result(NpcRouteManager& m,std::uint8_t hit){
    if(m.los_pending_9d0==2){m.los_pending_9d0=0;return;}                       // a released request's late result
    if(m.queue_20.empty())return;
    m.los_pending_9d0=0;
    auto& r=m.requests[m.queue_20.front()];
    if(r.kind_0c!=1){npc_node_search_los_result(m,r,hit);return;}             // 00161db0
    auto& g=m.global_9f0;                                                       // 00163580
    if(r.state_10==2){
        g.first_0d=0;
        g.search.node_478=hit==0?1:0;                                           // 004ede68 = clear
        return;
    }
    if(r.state_10!=0&&r.state_10!=1)return;
    npc_node_search_los_result(m,g,hit);
    const auto node=g.search.node_478;
    if(node<0)return;
    if(r.state_10==0)open_node(m,r,node);                                       // 00163410
    else if(m.region_candidates_ee010.size()<64)m.region_candidates_ee010.push_back(node);
    g.search.node_478=-1;
}

std::optional<std::size_t> npc_route_queued(const NpcRouteManager& m,std::uint32_t actor_id,std::uint8_t kinds){
    for(const auto i:m.queue_20){
        const auto& r=m.requests[i];
        if(queued_id(r)==actor_id&&(kinds&r.kind_0c))return i;
    }
    return std::nullopt;
}
void npc_route_release(NpcRouteManager& m,std::uint32_t actor_id){
    for(bool again=true;again;){
        again=false;
        for(auto it=m.queue_20.begin();it!=m.queue_20.end();++it){
            if(queued_id(m.requests[*it])!=actor_id)continue;
            if(it==m.queue_20.begin()&&m.los_pending_9d0==1)m.los_pending_9d0=2;
            if(it==m.queue_20.begin()&&m.requests[*it].first_0d==0&&m.port_route_trace)m.port_route_trace(m.requests[*it],"00163c50 frees the started head request");
            m.free_2c.push_back(*it);
            m.queue_20.erase(it);
            again=true;break;
        }
    }
}
bool npc_route_head_pending(const NpcRouteManager& m,std::uint32_t actor_id){
    if(m.queue_20.empty())return false;
    const auto& r=m.requests[m.queue_20.front()];
    if(queued_id(r)!=actor_id)return false;
    return r.byte_0e==0;
}

std::uint8_t npc_route_rebuild(NpcRouteManager& m,std::int32_t start,std::int32_t end,const std::shared_ptr<NpcRouteBuffer>& buffer){
    if(!buffer)throw std::runtime_error("NPC route: 0015f3d0 without a route buffer");
    const auto& graph=graph_of(m);
    const auto saved_10=buffer->destination_10;
    if(m.free_2c.empty())throw std::runtime_error("NPC route: 0015f3d0 with no free request (001642e0 returns null and the original writes through it)");
    if(!buffer->point_00)throw std::runtime_error("NPC route: 0015f3d0 reads the buffer's point +0, which no route has written (heap content)");
    const auto index=m.free_2c.front();m.free_2c.pop_front();                   // 001642e0
    auto& r=m.requests[index];
    r.first_0d=1;r.byte_0e=0;r.kind_0c=1;
    r.client=nullptr;r.id_04=0xffffffffU;                                       // +0 = 0, +4 = -1 (+8 kept)
    r.buffer_14=buffer;r.start_40=start;r.previous_44=-1;r.destination_node_4c=end;
    r.node_destination_12=1;
    r.destination_20=*buffer->point_00;                                         // lq +0
    r.destination_region_50=graph.node(end).parameter_7c;
    r.region_48=graph.node(start).parameter_7c;
    r.position_30=graph.position(start);                                        // 0015c2c0, w = 1
    r.pool_base_54=0;
    const bool done=npc_route_search(m,r);                                      // 00162080
    m.free_2c.push_back(index);                                                 // 001642b0
    buffer->destination_10=saved_10;
    return static_cast<std::uint8_t>(done?1:0);
}

void allocate_npc_route_buffer(NpcPathFollower& pf,NpcRouteManager* m){
    if(!pf.route_74){
        pf.route_74=std::make_shared<NpcRouteBuffer>();
        pf.route_74->capacity_24=pf.capacity_78;pf.route_74->start_14=-1;pf.route_74->destination_10=-1;
    }
    if(!pf.route_74->array_20){pf.route_74->array_20=true;pf.route_74->nodes.clear();}
    if(pf.route_74->start_14>=0){                                               // 00161280..001612ac
        if(!m)throw std::runtime_error("NPC route: 00160f60 rebuilds a deactivated route (0015f3d0) without the route manager (003caa50)");
        npc_route_rebuild(*m,pf.route_74->start_14,pf.route_74->end_18,pf.route_74);
    }
}
bool step_npc_route(NpcPathFollower& pf,const NpcNodeGraph& graph,const NpcPathHost& host){
    if(!pf.route_74)throw std::runtime_error("NPC route: the route step without a route buffer");
    const auto& b=*pf.route_74;
    if(!b.point_00)throw std::runtime_error("NPC route: the route step on a buffer no route was written to (its +0 and the follower's +48 are heap content)");
    bool segment=false;
    while(pf.index_48>0&&!segment){
        if(pf.current_50>=0)host.send({5,pf.current_50});
        --pf.index_48;
        segment=set_npc_path_segment(pf,graph,b.nodes.at(static_cast<std::size_t>(pf.index_48)),host);
    }
    if(!segment){
        const auto at=pf.current_50;
        if(at>=0&&at!=b.destination_10)host.send({5,at});
        if(pf.index_48>=0){
            --pf.index_48;
            if(b.destination_10<0)segment=set_npc_path_segment_to(pf,graph,*b.point_00,-1,host);
            else segment=set_npc_path_segment(pf,graph,b.destination_10,host);
        }
    }
    if(!segment){
        hold_npc_path_node(pf,graph,b.destination_10);
        host.send({6,pf.hold_4c});
    }
    return segment;
}
void relocate_npc_path_follower(NpcPathFollower& pf,const NpcNodeGraph& graph,std::int32_t node,const NpcPathHost& host){
    relocate_npc_path_follower_15ffd0(pf,graph,node,host);
    hold_npc_path_node(pf,graph,node);                                             // 0015ffb0 00160c40
}
std::uint8_t npc_route_order_1600a0(const NpcPathFollower& pf,std::int32_t node){
    if(!pf.route_74)throw std::runtime_error("NPC route: 001600a0 reads the route buffer +74 +28 through null");
    const auto& nodes=pf.route_74->nodes;
    std::int32_t t=-1;
    for(std::size_t i=0;i<nodes.size();++i)if(nodes[i]==node){t=static_cast<std::int32_t>(i);break;}   // 001600c0..001600e4
    if(t<0)return 0;
    if(t==pf.index_48+1)return 1;
    return pf.index_48+1<t?3:2;                                                    // 00160108..00160120
}
void relocate_npc_path_follower_15ffd0(NpcPathFollower& pf,const NpcNodeGraph& graph,std::int32_t node,const NpcPathHost& host){
    if(!bit(pf.flags_60,3)&&node!=pf.current_50&&node!=pf.hold_4c){               // 0015ffe4..00160008
        if(!pf.route_74)throw std::runtime_error("NPC route: 0015ffd0 reads the route buffer +74 +28 through null");
        const auto& nodes=pf.route_74->nodes;                                     // 00160010..00160024
        for(std::size_t i=0;i<nodes.size();++i){
            if(nodes[i]!=node)continue;
            pf.index_48=static_cast<std::int32_t>(i);                              // 00160034
            pf.current_50=node;                                                    // 00160040
            set_bit(pf.flags_60,3,!step_npc_route(pf,graph,host));                 // 0016003c 00160a00, 00160044..00160068
            break;
        }
    }
    hold_npc_path_node(pf,graph,node);                                             // 00160084 00160c40 (0015ffd0's tail)
}
bool start_npc_route(NpcPathFollower& pf,const NpcNodeGraph& graph,const NpcPathHost& host){
    if(!pf.route_74)throw std::runtime_error("NPC route: the route start without a route buffer");
    if(bit(pf.flags_60,0)){set_bit(pf.flags_60,0,false);hold_npc_path_node(pf,graph,-1);}
    const auto& nodes=pf.route_74->nodes;
    pf.index_48=static_cast<std::int32_t>(nodes.size());
    if(pf.index_48>=2){
        const auto n=nodes.size();
        if(nodes[n-1]==pf.previous_54&&nodes[n-2]==pf.current_50)pf.index_48=static_cast<std::int32_t>(n)-1;
    }
    pf.current_50=-1;
    pf.field_58=pf.route_74->destination_10;
    const bool segment=step_npc_route(pf,graph,host);
    set_bit(pf.flags_60,3,!segment);
    return !bit(pf.flags_60,3);
}
bool npc_route_direct_to_node(NpcPathFollower& pf,const NpcNodeGraph& graph,std::int32_t node,
    const std::function<void(const NpcPathEvent&)>& deliver){
    if(!pf.route_74)throw std::runtime_error("NPC route: 0015fe80 without a route buffer (the original reads +74 +20 of null)");
    auto& b=*pf.route_74;
    if(!b.array_20)return false;                                                 // 0015fea0
    b.nodes.clear();b.nodes.push_back(node);                                     // +28 = 0, then +20[+28++] = node
    b.point_00=graph.position(node);                                             // 0015ddb0 -> 0015c2c0 (w = 1)
    b.destination_10=node;
    if(!deliver)throw std::runtime_error("NPC route: 0015fe80's path event 2 (003c9420, transport 1) is not hosted");
    deliver({2,-1});
    return true;
}
std::uint8_t npc_death_code_84(NpcPathFollower& pf,const NpcNodeGraph& graph,std::uint8_t code,
    const std::function<void(const NpcPathEvent&)>& deliver){
    if(pf.hold_4c<0)return code;                                                 // 0014f40c
    const auto target=npc_first_link_of_type(graph,pf.hold_4c,7);                // 0015d280(node, 7)
    if(target<0)return code;
    return npc_route_direct_to_node(pf,graph,target,deliver)?std::uint8_t{2}:code;   // 0014f428..0014f438
}
void release_npc_route_buffer(NpcPathFollower& pf){
    if(!pf.route_74)return;
    auto& b=*pf.route_74;
    if(!b.nodes.empty()){b.start_14=b.nodes.back();b.end_18=b.nodes.front();}
    else b.start_14=-1;
    if(!b.array_20)return;
    b.array_20=false;b.nodes.clear();
}
float npc_route_distance(const NpcPathFollower& pf,const RootVector& actor_translation){
    if(!pf.route_74)return 0.0F;
    if(!pf.route_74->point_00)throw std::runtime_error("NPC route: 00160130 reads the route buffer's point before any route (heap content)");
    return distance(actor_translation,*pf.route_74->point_00);
}
float npc_unwritten_route_distance(const RootVector& actor_translation){
    return distance(actor_translation,RootVector{0.0F,0.0F,0.0F,0.0F});
}
}
