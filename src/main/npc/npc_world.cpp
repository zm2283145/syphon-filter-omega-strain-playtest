#include "npc_world.hpp"
#include "morph_resources.hpp"
#include "npc_alert.hpp"
#include "npc_attack.hpp"
#include "npc_cover.hpp"
#include "npc_follow.hpp"
#include "actor_fire.hpp"
#include "npc_route.hpp"
#include "npc_script_control.hpp"
#include "npc_search.hpp"
#include "water_effects.hpp"
#include "world_los.hpp"
#include <algorithm>
#include <stdexcept>

namespace omega {
NpcNodeGraph make_npc_node_graph(const Population& population){
    NpcNodeGraph g;g.nodes.reserve(population.nodes.size());
    for(const auto& n:population.nodes){
        NpcNode node;node.position={n.position.x,n.position.y,n.position.z};node.flags=n.flags;
        node.parameter_7c=static_cast<std::int32_t>(n.parameter);node.id=n.id;node.yaw_8c=population_node_yaw(n);
        node.association=n.parent_index>=0?NpcNode::Association::unknown:NpcNode::Association::none;
        if(n.parent_index>=0)node.association_key=(n.id&0xffff0000U)|static_cast<std::uint32_t>(n.parent_index);
        for(const auto& l:n.links){
            if(l[0]>=population.nodes.size())throw std::runtime_error("NPC nodes: a link target outside the node collection");
            node.links.push_back({static_cast<std::int32_t>(l[0]),static_cast<std::int32_t>(l[1])});
        }
        g.nodes.push_back(std::move(node));
    }
    return g;
}
NpcAiController::NpcAiController(NpcAiSystem& system,const NpcNodeGraph& graph,std::int32_t start_node,std::uint32_t actor_id,
    std::uint64_t& random_state):system_(system),graph_(graph),actor_id_(actor_id),random_state_(&random_state),
    route_client_(std::make_shared<NpcRouteClient>()){
    start_node_=start_node;
    if(start_node>=0)graph.node(start_node);
    route_client_->id=actor_id;route_client_->local_14=1;                     // actor +c, +14 = 1 (00158bc0)
    // The request handle's view of the actor (the last actor this
    // controller was hosted with): its +f4 translation, and the path events
    // the route manager sends with transport 1, delivered at once.
    route_client_->translation=[this]{
        if(!route_actor_)throw std::runtime_error("NPC AI: a route request before the controller saw its actor");
        const auto m=root_placement_matrix(route_actor_->floor_root_placement());
        return RootVector{m[12],m[13],m[14],m[15]};
    };
    route_client_->deliver=[this](const NpcPathEvent& e){
        if(!route_actor_)throw std::runtime_error("NPC AI: a path event before the controller saw its actor");
        if(e.event==2||e.event==3)port_route_event_=e.event;                   // port-only bookkeeping (port_take_route_event)
        auto nested=host(*route_actor_);
        NpcAiSend s;s.kind=NpcAiSend::Kind::component;s.transport=1;s.message={NpcAiMessage::Kind::path,e.event,e.value};
        s.message.word_2c=e.second_2c;                                         // +2c: a node search's second node (N5c)
        nested.send(s);
        dispatch_npc_ai(component_,s.message,nested);                          // 00151d30
    };
    // N5c: the request handle's AI controller (actor +58, virtual +88 = 1):
    // 0014bfb0(component, region), its path follower's +50 / +54, and the
    // LOS-check result 005435a8 (00151790 names the component the receiver),
    // delivered at once (transport 1).
    route_client_->allowed_region=[this](std::int32_t region){return region<0||npc_region_bit(component_,region);};
    route_client_->path_nodes_50_54=[this]{return std::array<std::int32_t,2>{component_.path.current_50,component_.path.previous_54};};
    route_client_->deliver_los_result=[this](std::uint8_t hit){
        if(!route_actor_)throw std::runtime_error("NPC AI: a LOS-check result before the controller saw its actor");
        auto nested=host(*route_actor_);
        NpcAiSend s;s.kind=NpcAiSend::Kind::component;s.transport=1;s.message={NpcAiMessage::Kind::los_result,0,hit};
        nested.send(s);
        dispatch_npc_ai(component_,s.message,nested);                          // 00151d30
    };
    construct_npc_path_follower(component_.path,100,start_node);              // 00161840(pf, actor, 100, +3548)
    // 00158bc0: +141 bits 2, 4, 5 set, 6 (with +64 = 0) and 7 clear; bits 0, 1
    // and 3 are the allocated block's previous content (0036b180 does not
    // clear it; taken as 0). +e8 is not written either (taken as 0).
    component_.flags_141=npc_ai_constructor_flags_141(0);
    // 00158e48..00158e8c: +6c = 0.25 + 0.5 * (rand / 2^31).
    const float r=game_int_to_float(static_cast<std::int32_t>(next_game_random(random_state)));
    aggressiveness_6c_=0.25F+0.5F*(r/2147483648.0F);
    component_.aggressiveness_6c=aggressiveness_6c_;
}
NpcAiHost NpcAiController::host(SoloActorSimulation& actor){
    NpcAiHost h;h.graph=&graph_;
    h.network_contact=network_hooks_.contact;h.network_speed=network_hooks_.speed;h.network_heading=network_hooks_.heading;
    auto& a=h.actor;SoloActorSimulation* p=&actor;
    a.eligible=[p]{return p->eligible();};
    a.life_state=[p]{return p->life_state();};
    // Virtual +f4 (00132280): 00132380 (the conversion of root_placement_matrix,
    // interpreter-checked) of the 'flor' root world placement and its position.
    a.world_matrix=[p]{
        const auto m=root_placement_matrix(p->floor_root_placement());
        return std::array<RootVector,4>{{{m[0],m[1],m[2],m[3]},{m[4],m[5],m[6],m[7]},{m[8],m[9],m[10],m[11]},{m[12],m[13],m[14],m[15]}}};
    };
    a.flags_3204=[p]{return p->state_words().current_3204;};
    a.flags_3210=[p]{return p->actions().active_actions;};
    a.navigation_enabled_30ac=[p]{return p->navigation_target().enabled_30ac;};
    a.navigation_arrived_30ad=[p]{return static_cast<std::uint8_t>(p->navigation_target().arrived_30ad?1:0);};
    a.physical_present=[]{return true;};
    a.physical_mode_20=[p]{return p->physical().history.current.value;};
    a.physical_dc=[p]{return p->physical().response.physical.byte_dc!=0;};
    a.physical_e8=[p]{return p->physical().response.physical.e8!=0;};
    a.physical_2c8=[p]{return p->physical().response.physical.blend_2c8;};
    a.killed_33a5=[p]{return p->health().killed_33a5;};
    a.mode_320c=[p]{return static_cast<std::uint32_t>(p->actions().highest_action);};
    a.roots_unparented=[p]{return p->roots_unparented();};
    a.inventory_mode_84=[p]{const auto& inv=p->actions().inventory;return inv?inv->mode:std::uint8_t{6};};
    if(combat_.inventory&&combat_.database)
        a.held_class=[this]{                                                  // 001437a0(+3524)
            const WeaponInventory* inv=combat_.inventory();
            if(!inv)throw std::runtime_error("NPC AI: 001437a0 for an actor without a combat inventory");
            return weapon_held_class(*inv,*combat_.database);
        };
    a.stop_physical_motion=[p]{p->stop_physical_motion();};
    a.set_flags_3038=[p](std::uint32_t b){p->set_control_flags_3038(b);};
    a.falling_dead_1d_1e=[p]{return ((p->actions().active_actions>>29)&3U)!=0;};
    a.land_dead=[p]{p->land_dead_ground_snap();};
    a.land_reset=[p]{p->land_reset_ground_snap();};
    // Virtual +e0 (00133e00): the physical position plus +380 once the
    // physical tail is initialized, else the component translation.
    a.dying_ground_probe=[p]{
        if((p->control_history()[1]&2U)==0){
            const auto& f=p->physical();
            const RootVector base=f.tail.initialized?f.integrator.position:p->component();
            RootVector e0{};for(std::size_t i=0;i<4;++i)e0[i]=base[i]+f.response.physical.offset_380[i];
            if(trace_world_los(p->world(),{e0,{0.0F,0.0F,-100.0F,0.0F},0,0x40}))return true;   // 00148290
        }
        p->set_control_flags_3038(2U);
        return false;
    };
    a.secondary_3314=combat_.secondary;
    if(system_.actor_region)a.region_2fe0=[this,p]{
        const auto& f=p->physical();
        const RootVector base=f.tail.initialized?f.integrator.position:p->component();
        RootVector e0{};for(std::size_t i=0;i<4;++i)e0[i]=base[i]+f.response.physical.offset_380[i];   // virtual +e0 (00133e00)
        return system_.actor_region(p->handle(),e0);
    };
    a.deliver=[p](const NpcAiSend& s){
        if(s.kind==NpcAiSend::Kind::navigation_request){p->receive_navigation_request(s.target,s.yaw,s.speed);return;}
        if(s.kind==NpcAiSend::Kind::action&&s.action_kind==0){p->receive_actor_action(s.action,true);return;}
        if(s.kind==NpcAiSend::Kind::teleport){
            // 001bc4b0; the AI controller's teleport tail (virtual +50,
            // 0015b820) does nothing, and a parented root makes it a no-op.
            // Mode 2 (place): the stuck relocation; mode 0 (translate): 001b0a50's path-height teleport.
            if(s.mode_24!=2&&s.mode_24!=0)throw std::runtime_error("NPC AI: a Teleport Message mode other than 0 / 2 from the AI");
            (void)p->teleport(s.mode_24==2?ActorTeleportMode::place:ActorTeleportMode::translate,s.matrix);
            return;
        }
        throw std::runtime_error("NPC AI: an immediate message to the actor that is not hosted");
    };
    h.dead_removal_allowed=[this,p]{
        const NpcCorpseView self=corpse_view(*p);
        std::vector<NpcCorpseView> views;views.reserve(system_.dead_list.size());
        for(const auto& d:system_.dead_list){
            if(!d.view)throw std::runtime_error("NPC AI: a dead-list entry without a view");
            views.push_back(d.component==&component_?self:d.view());
        }
        std::vector<const NpcCorpseView*> list;
        for(std::size_t i=0;i<views.size();++i)list.push_back(system_.dead_list[i].component==&component_?&self:&views[i]);
        if(!system_.camera_position)throw std::runtime_error("NPC AI: 00159020 needs the camera position (NpcAiSystem::camera_position)");
        const auto camera=system_.camera_position();
        if(!camera)throw std::runtime_error("NPC AI: 00159020 without a camera object (004ffc14)");
        return npc_dead_removal_allowed(self,list,*camera,system_.far_distance);
    };
    h.remove_dead_actor=[p]{p->deactivate();};
    // N5a: the route manager and this actor's request handle; a path event
    // the manager sends (transport 1) reaches the dispatcher at once.
    h.routes=system_.routes;
    h.unwritten_route_reads=system_.unwritten_route_reads;h.value_free_route_reads=system_.value_free_route_reads;
    h.spawned_route_policy_reads=system_.spawned_route_policy_reads;
    h.node_association_eligible=system_.node_association_eligible;
    h.level_loaded=level_loaded_;                              // false for a generator's spawn (001b7b00)
    h.route_client=route_client_;
    route_actor_=p;
    if(perception_link_.sense){
        sense_=perception_link_.sense(actor);
        // N5c: the node objects (vtable 004d95b0) the look-around names: ID
        // +c, class word *004ea770, position and eye +c0 / +e4 = +80 (w 1).
        // Their +2c..+2e and the other fields are not read for them (taken
        // as 0).
        // The wrapper captures only this (port-only, speed, 132vs4: no allocation per host on the Vita).
        sense_object_base_=std::move(sense_->object);
        sense_->object=[this](std::uint64_t o)->std::optional<NpcSenseObject>{
            if(const auto n=graph_.node_of_object(o)){
                NpcSenseObject v;v.id=static_cast<std::int32_t>(graph_.node(*n).id);v.class_4ea770=true;
                v.position=v.eye=graph_.position(*n);
                return v;
            }
            if(!sense_object_base_)throw std::runtime_error("NPC perception: no object views (NpcPerceptionHost::object)");
            return sense_object_base_(o);
        };
        h.perception=&perception_;h.sense=&*sense_;
        h.object_by_id=perception_link_.object_by_id;
        h.projectile=combat_.projectile;
    }
    h.system=&system_;
    // Checkpoint 132zf12: mode 9 Follow (00153870). The target's +f4 / +3584 /
    // +30e0 / +58 +74 / AI +4c come from the combat link; this actor's +e8
    // row 3 is its 'flor' placement (as the perception's +e8), its +30e0 the
    // navigation block's speed (+3584, the network object, is 0 in solo:
    // 0016f5b0's spawned actors and the level actors write no network object).
    if(!follow_)follow_=std::make_unique<NpcFollowHost>();
    {
        auto& f=*follow_;
        f.self=p->handle();f.self_14=route_client_->local_14;                // actor +14 (00158bc0 sets 1)
        f.world_f4=[this](std::uint64_t o){
            if(!combat_.world_f4_of)throw std::runtime_error("NPC AI: mode 9 reads an object's +f4 (NpcCombatLink::world_f4_of)");
            return combat_.world_f4_of(o);};
        f.position_e8=[p](std::uint64_t o){
            if(o!=p->handle())throw std::runtime_error("NPC AI: mode 9 reads +e8 of another object");
            const auto m=root_placement_matrix(p->floor_root_placement());
            return RootVector{m[12],m[13],m[14],m[15]};};
        f.speed=[this,p](std::uint64_t o){
            if(o==p->handle())return p->navigation_target().speed_30e0;
            if(!combat_.speed_of)throw std::runtime_error("NPC AI: mode 9 reads a target's +30e0 (NpcCombatLink::speed_of)");
            return combat_.speed_of(o);};
        f.velocity_74=[this](std::uint64_t o)->std::optional<RootVector>{
            if(!combat_.velocity_74_of)throw std::runtime_error("NPC AI: mode 9 reads +58 virtual +74 (0022f510, NpcCombatLink::velocity_74_of)");
            return combat_.velocity_74_of(o);};
        f.hold_4c=[this](std::uint64_t o){
            if(!combat_.hold_4c_of)throw std::runtime_error("NPC AI: mode 9 reads an AI target's path follower +4c (NpcCombatLink::hold_4c_of)");
            return combat_.hold_4c_of(o);};
        auto routes=[this]()->NpcRouteManager&{
            if(!system_.routes||!system_.routes->regions)throw std::runtime_error("NPC AI: mode 9 needs the route manager 004ed9b0 and its regions");
            return *system_.routes;};
        f.region_at=[this,routes](const RootVector& point,std::int32_t previous,std::optional<float> f20,const char* site){
            return npc_region_at(*routes().regions,graph_,point,previous,f20,site);};   // 0015d790
        f.release_routes=[this,routes]{npc_route_release(routes(),actor_id_);};       // 00163c50
        f.request_route=[this,routes](std::int32_t start,std::int32_t region,std::int32_t destination,const RootVector& point,
            const std::shared_ptr<NpcRouteBuffer>& buffer){
            (void)npc_route_request(routes(),route_client_,start,-1,region,-1,destination,&point,buffer);};   // 0015efb0
        f.route_to_point=[this,routes](const RootVector& point,std::int32_t region){
            return npc_route_to_point(routes(),component_.path,route_client_,point,region);};   // 0015fa70
    }
    h.follow=follow_.get();
    // Checkpoint 132uk5: mode 8 CarryBody (00153150). The body's path
    // follower +4c and node 14 matrix come from the combat link.
    if(!carry_)carry_=std::make_unique<NpcCarryHost>();
    {
        auto& k=*carry_;
        k.hold_4c=[this](std::uint64_t o){
            if(!combat_.hold_4c_of)throw std::runtime_error("NPC AI: mode 8 reads an AI body's path follower +4c (NpcCombatLink::hold_4c_of)");
            return combat_.hold_4c_of(o);};
        k.head_row3=[this](std::uint64_t o){
            if(!combat_.node_14_of)throw std::runtime_error("NPC AI: mode 8 reads the body's node 14 matrix (NpcCombatLink::node_14_of)");
            return combat_.node_14_of(o)[3];};
        auto routes=[this]()->NpcRouteManager&{
            if(!system_.routes||!system_.routes->regions)throw std::runtime_error("NPC AI: mode 8 needs the route manager 004ed9b0 and its regions");
            return *system_.routes;};
        k.route_to_node=[this,routes](std::int32_t node){
            return npc_route_to_node(routes(),component_.path,route_client_,node);};   // 0015fb20
        k.route_to_point=[this,routes](const RootVector& point,std::int32_t region){
            return npc_route_to_point(routes(),component_.path,route_client_,point,region);};   // 0015fa70
    }
    h.carry=carry_.get();
    // Checkpoint 132ms4: mode 7's enter toward a dead object (00154660).
    if(!go_to_body_)go_to_body_=std::make_unique<NpcGoToBodyHost>();
    {
        auto& b=*go_to_body_;
        b.world_f4=[this](std::uint64_t o){
            if(!combat_.world_f4_of)throw std::runtime_error("NPC AI: mode 7 reads a body's +f4 (NpcCombatLink::world_f4_of)");
            return combat_.world_f4_of(o);};
        auto routes=[this]()->NpcRouteManager&{
            if(!system_.routes||!system_.routes->regions)throw std::runtime_error("NPC AI: mode 7 needs the route manager 004ed9b0 and its regions");
            return *system_.routes;};
        b.region_at=[this,routes](const RootVector& point,std::int32_t previous,std::optional<float> caller_f20){
            return npc_region_at(*routes().regions,graph_,point,previous,caller_f20,
                "001547d4 (mode 7 toward a dead object without a player or AI controller: the sender's f20, NpcAiHost::sender_f20)");};
        b.route_to_point=[this,routes](const RootVector& point,std::int32_t region){
            return npc_route_to_point(routes(),component_.path,route_client_,point,region);};   // 0015fa70
    }
    h.go_to_body=go_to_body_.get();
    // 00127dd0 is the one game RNG: the combat link's, or the state the constructor drew from.
    h.random=combat_.random?combat_.random:std::function<std::uint32_t()>([state=random_state_]{return next_game_random(*state);});
    // N5b: the attack stages with both links complete.
    if(perception_link_.sense&&combat_.aim&&combat_.aim_block&&combat_.actor_action&&combat_.discharge&&combat_.inventory&&combat_.database){
        if(!attack_){
            attack_=std::make_unique<NpcAttackHost>();
            auto& t=*attack_;
            t.actor_id=actor_id_;
            auto inventory=[this]()->WeaponInventory&{
                WeaponInventory* inv=combat_.inventory();
                if(!inv)throw std::runtime_error("NPC AI: the attack stages for an actor without a combat inventory");
                return *inv;
            };
            auto ops=[this,inventory](const std::function<void(WeaponInventoryOps&)>& f){
                WeaponInventoryHost ih;ih.player=false;
                std::vector<WeaponInventoryEvent> ev;
                WeaponInventoryOps o(inventory(),*combat_.database,ih,ev);
                f(o);
                if(!ev.empty())throw std::runtime_error("NPC AI: an inventory query of the attack stages made inventory events");
            };
            t.eye_c0=[this](std::uint64_t o,std::int32_t sub){
                if(const auto n=graph_.node_of_object(o))return graph_.position(*n);   // a node object: +c0 -> +e4, any subtarget
                if(!combat_.eye_c0_of)throw std::runtime_error("NPC AI: the eye +c0 of an object (NpcCombatLink::eye_c0_of)");
                return combat_.eye_c0_of(o,sub);};
            t.position_f4=[this](std::uint64_t o){
                // A node object's +f4 (vtable 004d95b0 -> 0015bf40 -> 0015c170): the yaw +8c matrix whose
                // row 3 is (+80, +84, +88, 1) (CHECHNYA GrenadeToss: SetGrenadeTarget on node 1643717).
                if(const auto n=graph_.node_of_object(o))return graph_.position(*n);
                if(!combat_.position_f4_of)throw std::runtime_error("NPC AI: +f4 of an object (NpcCombatLink::position_f4_of)");
                return combat_.position_f4_of(o);};
            t.inventory_84=[inventory]{return inventory().current_84;};
            t.slot_item=[inventory](std::uint8_t slot){return inventory().slots.at(slot).item;};
            t.reloading_a0=[inventory]{return inventory().reloading_a0;};
            t.can_attack=[this,inventory]{return weapon_can_attack(inventory(),*combat_.database);};
            t.port_hold_fire=[this]{if(port_hold_fire_)++port_held_shots_;return port_hold_fire_;};
            t.held_class=[this,inventory]{return weapon_held_class(inventory(),*combat_.database);};
            t.ready=[ops]{bool r=false;ops([&r](WeaponInventoryOps& o){r=o.ready(false);});return r;};
            t.full_auto=[ops]{bool r=false;ops([&r](WeaponInventoryOps& o){r=o.full_auto();});return r;};
            t.clip=[ops]{std::int32_t r=0;ops([&r](WeaponInventoryOps& o){r=o.clip(6);});return r;};
            t.byte_33e4=[this]{
                if(!combat_.byte_33e4)throw std::runtime_error("NPC AI: the actor's +33e4 (NpcCombatLink::byte_33e4)");
                return combat_.byte_33e4();};
            t.secondary=[this]()->std::optional<std::array<std::uint8_t,2>>{
                if(!combat_.secondary)throw std::runtime_error("NPC AI: the secondary component +3314 (NpcCombatLink::secondary)");
                return combat_.secondary();};
            t.secondary_slot1_415=[this]{
                if(!combat_.secondary_slot1_415)throw std::runtime_error("NPC AI: the secondary aim slot 1 (+415, NpcCombatLink::secondary_slot1_415)");
                return combat_.secondary_slot1_415();};
            t.aim=[this](std::uint64_t target,std::int32_t item,std::int32_t sub){combat_.aim(target,item,sub,component_.skill_38);};
            t.aim_block=[this]{return combat_.aim_block();};
            t.discharge=[this]{return combat_.discharge();};
            t.random=[this]{
                if(!combat_.random)throw std::runtime_error("NPC AI: the game RNG (NpcCombatLink::random)");
                return combat_.random();};
            t.route_head_pending=[this]{
                if(!system_.routes)throw std::runtime_error("NPC AI: Attack's relocation needs the route manager 004ed9b0 (NpcAiSystem::routes)");
                return npc_route_head_pending(*system_.routes,actor_id_);};
            t.route_to_point=[this](const RootVector& point,std::int32_t region){
                if(!system_.routes)throw std::runtime_error("NPC AI: Attack's relocation needs the route manager 004ed9b0 (NpcAiSystem::routes)");
                (void)npc_route_to_point(*system_.routes,component_.path,route_client_,point,region);};
            t.reload_allowed=[ops]{bool r=false;ops([&r](WeaponInventoryOps& o){r=o.reload_allowed();});return r;};   // 00143560
            t.select_item=[this](std::int32_t item){                          // 0014bae0
                if(!combat_.inventory)throw std::runtime_error("NPC AI: 0014bae0 needs the combat inventory");
                WeaponInventory* inv=combat_.inventory();
                if(!inv)throw std::runtime_error("NPC AI: 0014bae0 for an actor without a combat inventory");
                WeaponInventoryHost ih;ih.player=false;ih.attached=combat_.attached;ih.apply=combat_.apply;
                std::vector<WeaponInventoryEvent> ev;
                WeaponInventoryOps o(*inv,*combat_.database,ih,ev);
                npc_select_item(component_,item,o,actor_id_,combat_.random);
                if(combat_.inventory_changed)combat_.inventory_changed();
                frame_.inventory_events.insert(frame_.inventory_events.end(),ev.begin(),ev.end());
            };
            t.weapon_rating_of=[this](std::uint64_t o){
                if(!combat_.weapon_rating_of)throw std::runtime_error("NPC AI: 00142b50 of another actor (NpcCombatLink::weapon_rating_of)");
                return combat_.weapon_rating_of(o);};
        }
        h.attack=attack_.get();
    }
    h.held_weapon_rating=[this]()->std::int32_t{                             // 00142b50(actor +3524)
        if(!combat_.inventory||!combat_.database)throw std::runtime_error("NPC AI: mode 1 needs the held weapon's rating (combat link inventory)");
        const WeaponInventory* inventory=combat_.inventory();
        if(!inventory)throw std::runtime_error("NPC AI: mode 1 for an actor without a combat inventory");
        return npc_weapon_rating(*inventory,*combat_.database);
    };
    h.send=[this,p](const NpcAiSend& s){
        frame_.sends.push_back(s);
        if(s.kind==NpcAiSend::Kind::component&&s.transport==0)queue_.push_back(s.message);
        else if(s.kind==NpcAiSend::Kind::action&&s.transport==0){
            // The combat actions (aim, lower, fire, reload, look) go to the
            // combat host's pump (N5b); the others to the actor's queue.
            if(s.action>=0x39&&s.action<=0x3f){
                if(!combat_.actor_action)throw std::runtime_error("NPC AI: a combat action to the actor without the combat host (NpcCombatLink::actor_action)");
                combat_.actor_action(s);
            }else if(s.action==npc_carry_pickup_action&&s.action_kind==5){
                // Mode 8 (00153150): the pickup of the body by its ID (+2c); the
                // actor's 001a92a0 resolves it as the pickup menu's line does.
                if(!perception_link_.object_by_id)throw std::runtime_error("NPC AI: mode 8's pickup needs 0040c540 (NpcPerceptionLink::object_by_id)");
                p->queue_carry_pickup(perception_link_.object_by_id(s.word_2c),5);
            }else if((s.action==0x15||s.action==0x16)&&s.action_kind==4){
                // Path-link types 5 / 0xd / 0xc (00151d30): the running jump with
                // its point (+30); the actor's 001a92a0 hands it to 001914c0.
                p->queue_running_jump(s.action,s.point);
            }else p->receive_actor_action(s.action,false);
        }
        // The script notifications to the actor (+30) go to its queue;
        // immediate messages need nothing here (they are delivered by the
        // caller). The weapon-fired notice (0014f460 -> 0026f110, 004ff2a0)
        // and the TimeElapsed notice (0016f2d0, 004ff280) go to the
        // component's queue with transport 0.
        else if(s.kind==NpcAiSend::Kind::script&&s.key==npc_script_weapon_fired){
            if(s.transport!=0)throw std::runtime_error("NPC AI: an immediate weapon-fired notice is not hosted");
            NpcAiMessage m;m.kind=NpcAiMessage::Kind::weapon_fired;m.object=s.object;m.thrown_30=s.byte_38!=0;
            queue_.push_back(m);
        }
        // The perception's dead-object notice (0016c9d0 -> 004ff2a8) to its +3c, the component, transport 0.
        else if(s.kind==NpcAiSend::Kind::script&&s.key==npc_script_pickupable_awareness){
            if(s.transport!=0)throw std::runtime_error("NPC AI: an immediate dead-object notice (004ff2a8) is not hosted");
            NpcAiMessage m;m.kind=NpcAiMessage::Kind::pickupable_awareness;m.object=s.object;
            queue_.push_back(m);
        }
        // 0016f2d0: Event_Send(AITimeElapsedMsg, self, 0) to the component (132fx2).
        else if(s.kind==NpcAiSend::Kind::script&&s.key==npc_script_time_elapsed){
            if(s.transport!=0)throw std::runtime_error("NPC AI: an immediate TimeElapsed notice (004ff280) is not hosted");
            NpcAiMessage m;m.kind=NpcAiMessage::Kind::time_elapsed;
            queue_.push_back(m);
        }
        else if(s.kind==NpcAiSend::Kind::script){
            if(s.transport!=0)throw std::runtime_error("NPC AI: an immediate script notification is not hosted");
            ActorAiNotification n;n.key=s.key;n.node=s.node;n.node2=s.node2;
            if(s.key==npc_script_new_awareness){n.object=s.object;n.value=s.cause;}  // +24 the object, +28 the cause
            p->queue_ai_notification(n);
        }
        else if(s.kind==NpcAiSend::Kind::sound){
            if(s.transport!=0)throw std::runtime_error("NPC AI: an immediate sound message from the AI is not hosted");
            p->post_static_sound(s.key,false);                                 // 003c9420(msg, 004ffb50, 0)
        }
    };
    return h;
}
void NpcAiController::set_combat_link(NpcCombatLink link){
    combat_=std::move(link);
    attack_.reset();
    // 00158bc0 (00158dd8: sb 1, 0x99(actor +3524)): the AI init sets the actor's inventory +99, the
    // infinite reserve the reload 0013fb90 and the throw 00141ea0 keep the spare rounds by. The port's
    // inventory exists only once the combat link names it (it is built after the AI component).
    if(combat_.inventory)if(auto* inv=combat_.inventory())inv->infinite_reserve_99=true;
    if(combat_.fire_notice_hook)combat_.fire_notice_hook([this](std::uint64_t target,bool continuation){fire_notice(target,continuation);});
}
namespace {
// PORT-ONLY (132fx2, NpcAiSystem::port_logic_divider): the AI's logic frame (005383c8) and whether this
// world step runs it.
std::uint32_t npc_logic_divider(const NpcAiSystem& s){return s.port_logic_divider>1?s.port_logic_divider:1U;}
std::uint32_t npc_logic_frame(const NpcAiSystem& s,const SoloActorSimulation& a){return a.frame_counter()/npc_logic_divider(s);}
bool npc_logic_step(const NpcAiSystem& s,const SoloActorSimulation& a){
    const auto d=npc_logic_divider(s);
    return a.frame_counter()%d==d-1U;
}
}
void NpcAiController::fire_notice(std::uint64_t target,bool continuation){
    if(!route_actor_)throw std::runtime_error("NPC AI: a fire notice before the controller saw its actor");
    system_.frame=npc_logic_frame(system_,*route_actor_);                       // 005383c8
    auto h=host(*route_actor_);
    if(!h.attack)throw std::runtime_error("NPC AI: the fire notice +cc (0014f460) needs the attack stages' host (combat and perception links)");
    npc_fire_notice(component_,system_,target,continuation,h);
}
void NpcAiController::snapshot(){pending_=std::move(queue_);queue_.clear();}
void NpcAiController::pump(SoloActorSimulation& actor){
    if(keep_history_)history_.push_back(std::move(frame_));
    frame_=NpcAiControllerFrame{};
    auto pending=std::move(pending_);pending_.clear();
    // Port-only (speed, 132vs4): no message, no host. The host's views are
    // rebuilt by every host() call (update, always_run, nested sends), so
    // only the route actor it records is kept.
    if(pending.empty()){route_actor_=&actor;return;}
    auto h=host(actor);
    for(const auto& m:pending){
        frame_.delivered.push_back(m);
        if(m.kind==NpcAiMessage::Kind::weapon_fired){
            // 003c8de0: the script filter +38 (0014a6c0) first.
            const bool handled=weapon_fired_filter_&&weapon_fired_filter_(m.object,m.thrown_30);
            frame_.weapon_fired.push_back({m.object,handled});
            if(handled)continue;
        }
        if(m.kind==NpcAiMessage::Kind::pickupable_awareness){
            const bool handled=pickupable_awareness_filter_&&pickupable_awareness_filter_(m.object);
            frame_.pickupable_awareness.push_back({m.object,handled});
            if(handled)continue;
        }
        if(m.kind==NpcAiMessage::Kind::time_elapsed){
            // 003c8de0: the script filter +38 (0014a6c0) first; 00151d30 never loads the key and the base
            // handler 003cb210 drops it, so nothing follows either way (132fx2).
            (void)(time_elapsed_filter_&&time_elapsed_filter_());
            continue;
        }
        if(m.kind==NpcAiMessage::Kind::mode_change){
            const bool handled=mode_change_filter_&&mode_change_filter_(m.event);
            frame_.mode_change.push_back({m.event,handled});
            if(handled)continue;
        }
        dispatch_npc_ai(component_,m,h);                                        // 00151d30
    }
}
void NpcAiController::always_run(SoloActorSimulation& actor,float){
    system_.frame=npc_logic_frame(system_,actor);                              // 005383c8
    if(port_remote_controlled_)return;                                         // PORT-ONLY (132ln5h)
    if(remote_)return;                                                         // 132ln5h13: the remote controller (npc_remote.hpp)
    if(!npc_logic_step(system_,actor))return;                                  // PORT-ONLY (132fx2): once per logic frame
    if(!(component_.flags_141&0x80U))return;
    auto h=host(actor);
    NpcAiRadiusHost r;r.actor_id=actor_id_;
    r.closest_player_distance_squared=[this]{
        if(!player_distance_)throw std::runtime_error("NPC AI: the radius check needs the closest player's eye distance (003ce850)");
        return player_distance_();
    };
    SoloActorSimulation* p=&actor;
    r.activate_actor=[p]{p->activate();};
    r.deactivate_actor=[p]{p->deactivate();};
    check_npc_activation_radius(component_,system_,h,r);
}
void NpcAiController::update(SoloActorSimulation& actor,float dt){
    // PORT-ONLY (132fx2): the update 00159590 runs once per logic frame with that frame's dt
    // (NpcAiSystem::port_logic_divider); with divider 1 every step, as before.
    if(npc_logic_divider(system_)>1){
        port_logic_dt_+=dt;
        if(!npc_logic_step(system_,actor))return;
        dt=port_logic_dt_;port_logic_dt_=0;
    }
    system_.frame=npc_logic_frame(system_,actor);
    if(port_remote_controlled_)return;                                         // PORT-ONLY (132ln5h)
    port_last_dt_=dt;
    if(remote_){remote_update(actor,dt);return;}                               // 002d5160 (132ln5h13)
    auto h=host(actor);
    const auto f=update_npc_ai(component_,dt,system_,h);
    frame_.think=f.think;
    frame_.skipped.insert(frame_.skipped.end(),f.skipped.begin(),f.skipped.end());
}
void NpcAiController::activated(SoloActorSimulation& actor){
    if(remote_){remote_activate(actor);return;}                                // 002d40a0 (132ln5h13)
    auto h=host(actor);
    NpcAiActivationHost act;act.actor_id=actor_id_;
    if(perception_link_.sense)act.perception=&perception_;                   // 0016dfa0 at its place in Activate
    SoloActorSimulation* p=&actor;
    act.activate_actor=[p]{p->activate();};
    act.alive_44=[this,p]{return alive_or_eligible(*p);};
    act.append_to_dead_list=[this,p]{system_.dead_list.push_back({&component_,[this,p]{return corpse_view(*p);}});};   // 0014ab90(004ea370, &component)
    // An armed NPC (inventory +84 != 6) needs the combat link's inventory and RNG.
    std::optional<WeaponInventoryOps> ops;
    WeaponInventoryHost ih;ih.player=false;
    std::vector<WeaponInventoryEvent> inventory_events;
    if(combat_.inventory&&combat_.database){
        if(WeaponInventory* inv=combat_.inventory()){
            ih.attached=combat_.attached;ih.apply=combat_.apply;
            ops.emplace(*inv,*combat_.database,ih,inventory_events);
            act.inventory=&*ops;
        }
    }
    act.random=combat_.random;
    const auto skipped=activate_npc_ai(component_,system_,h,act);
    if(ops&&combat_.inventory_changed)combat_.inventory_changed();
    frame_.inventory_events.insert(frame_.inventory_events.end(),inventory_events.begin(),inventory_events.end());
    frame_.skipped.insert(frame_.skipped.end(),skipped.begin(),skipped.end());
}
void NpcAiController::deactivated(SoloActorSimulation& actor){
    if(remote_){remote_deactivate(actor,false);return;}                       // 002d4460 (132ln5h13)
    auto h=host(actor);
    NpcAiDeactivationHost d;
    // Called from the actor's deactivation (001b5190 -> 003928d0 -> 003ce790)
    // after +2d is cleared, so the body runs and +60 is not reached again.
    d.deactivate_actor=[]{throw std::runtime_error("NPC AI: Deactivate re-entered the actor's deactivation");};
    d.generator_release=generator_release();
    const auto skipped=deactivate_npc_ai(component_,system_,h,d);
    frame_.skipped.insert(frame_.skipped.end(),skipped.begin(),skipped.end());
}
void NpcAiController::deactivate_anonymous(SoloActorSimulation& actor){
    auto h=host(actor);
    NpcAiDeactivationHost d;
    SoloActorSimulation* p=&actor;
    d.deactivate_actor=[p]{p->deactivate();};
    const auto skipped=deactivate_npc_ai_anonymous(h,d);
    frame_.skipped.insert(frame_.skipped.end(),skipped.begin(),skipped.end());
}
void NpcAiController::deactivate(SoloActorSimulation& actor){
    if(remote_){remote_deactivate(actor,false);return;}                       // the remote's +4c, 002d4460 (132ln5h13)
    auto h=host(actor);
    NpcAiDeactivationHost d;
    SoloActorSimulation* p=&actor;
    d.deactivate_actor=[p]{p->deactivate();};
    d.generator_release=generator_release();
    const auto skipped=deactivate_npc_ai(component_,system_,h,d);
    frame_.skipped.insert(frame_.skipped.end(),skipped.begin(),skipped.end());
}
std::uint8_t NpcAiController::death_code_84(SoloActorSimulation& actor,std::uint8_t code){
    if(remote_)return 1;                                                       // the remote's +84 (132ln5h13)
    // 0015fe80's path event 2 (transport 1) reaches the dispatcher 00151d30
    // at once, before the death action.
    const auto deliver=[&](const NpcPathEvent& e){
        auto h=host(actor);
        NpcAiSend s;s.kind=NpcAiSend::Kind::component;s.transport=1;s.message={NpcAiMessage::Kind::path,e.event,e.value};
        s.message.word_2c=e.second_2c;
        h.send(s);
        dispatch_npc_ai(component_,s.message,h);
    };
    return npc_death_code_84(component_.path,graph_,code,deliver);             // 0014f3f0 (+1b0)
}
NpcCorpseView NpcAiController::corpse_view(SoloActorSimulation& actor) const{
    NpcCorpseView v;
    v.life_108=actor.life_state();v.flags_141=component_.flags_141;v.root={1,actor.byte_2d(),0};
    v.pickupable_3468=actor.pickupable_3468();v.held_354c=false;v.action_114=component_.action_114;
    v.flags_3204=actor.state_words().current_3204;v.word_38=combat_.word_38?combat_.word_38():0x80U;
    v.eye_c0=combat_.eye_c0;
    return v;
}
void NpcAiController::hit_notice_78(SoloActorSimulation& actor,std::uint64_t source,std::int32_t){
    if(remote_)return;                                                         // the remote's +78 does nothing (132ln5h13)
    if(source!=0&&combat_.port_ignored&&combat_.port_ignored(source))return;   // port-only: an ignored attacker raises no alert
    if(!combat_.object_view)throw std::runtime_error("NPC AI: the hit notice +78 needs the attacker's view (combat link)");
    const auto view=combat_.object_view(source);
    if(!view)throw std::runtime_error("NPC AI: the hit notice +78 from an object the combat link does not know");
    std::vector<PerceptionSend> sends;
    perception_add_awareness(perception_,*view,8,0.0F,sends);                // 0016c9d0(+1ac, source, 8, 0.0)
    frame_.perception_sends.insert(frame_.perception_sends.end(),sends.begin(),sends.end());
    auto h=host(actor);
    for(const auto& d:sends){                                                 // 0016cd7c: the dead-object notice, inside 0016c9d0
        NpcAiSend n;n.kind=NpcAiSend::Kind::script;n.transport=0;n.key=npc_script_pickupable_awareness;n.object=d.object;
        h.send(n);
    }
    NpcAiSend s;s.kind=NpcAiSend::Kind::component;s.transport=0;
    s.message={NpcAiMessage::Kind::ai,8,-1};                                  // AI event 8, +28 = -1
    h.send(s);
}
void NpcAiController::death_notice_7c(SoloActorSimulation& actor,const ActorDeathNotice& notice){
    if(remote_)throw std::runtime_error("NPC AI: the death notice of a remote NPC (the remote controller 0x004de2a0's virtual +7c, 002d4640) is not ported");
    if(!combat_.inventory||!combat_.database)throw std::runtime_error("NPC AI: the death notice +7c needs the combat link's inventory");
    WeaponInventory* inventory=combat_.inventory();
    if(!inventory)throw std::runtime_error("NPC AI: the death notice +7c for an actor without a combat inventory");
    WeaponInventoryHost ih;ih.player=false;ih.apply=combat_.apply;                                // an AI component's +54 answers 0
    std::vector<WeaponInventoryEvent> inventory_events;
    WeaponInventoryOps ops(*inventory,*combat_.database,ih,inventory_events);
    NpcDeathNoticeHost h;
    h.actor=actor.handle();
    h.actor_root={1,actor.byte_2d(),0};                                       // +2c = 1, +2e = 0 (constructor 003cf120 / 003cf130)
    h.actor_3468=actor.pickupable_3468();h.actor_3488=0;
    if(combat_.secondary)h.secondary=combat_.secondary();
    h.killer=combat_.killer;h.killer_hostile=combat_.killer_hostile;h.statistics=combat_.statistics;h.random=combat_.random;
    h.ammo_pct=system_.npc_ammo_pct;
    NpcAiComponent* self=&component_;
    h.remove_from_think_list=[this,self]{                                    // 0015ad70
        auto& list=system_.think_list;std::int32_t n=0;
        for(auto it=list.begin();it!=list.end();)if(it->component==self){it=list.erase(it);++n;}else ++it;
        return n;
    };
    SoloActorSimulation* p=&actor;
    h.append_to_dead_list=[this,self,p]{system_.dead_list.push_back({self,[this,p]{return corpse_view(*p);}});};   // 0014ab90(004ea370, &component)
    NpcDeathNoticeInput in;in.amount=notice.amount;in.killer=notice.killer;in.category=notice.category;in.type=notice.type;
    std::vector<NpcDeathNoticeEvent> events;
    npc_death_notice(component_,perception_,ops,in,h,events);
    if(combat_.inventory_changed)combat_.inventory_changed();
    for(const auto& e:events)
        if(e.kind==NpcDeathNoticeEvent::Kind::script_death){
            ActorAiNotification n;n.key=npc_script_death;n.object=e.object;n.value=e.value;
            actor.queue_ai_notification(n);                                    // 003c9420(notice, actor, 0)
        }else if(e.kind==NpcDeathNoticeEvent::Kind::voice&&combat_.voice_line)
            combat_.voice_line(e.object,e.value);                              // 0018c770(killer, line)
    frame_.death_notice.insert(frame_.death_notice.end(),events.begin(),events.end());
    frame_.inventory_events.insert(frame_.inventory_events.end(),inventory_events.begin(),inventory_events.end());
}
void NpcAiController::script_add_weapon(std::int32_t item){
    script_weapon("AddWeapon 0014b890",[&](WeaponInventoryOps& ops){ops.add(item,true,false);});   // a2 = 1, t0 = 0
}
void NpcAiController::port_add_weapon_loaded(std::int32_t item){
    // Without the combat link's model events (no weapon model library) it is the plain AddWeapon.
    const bool preload=static_cast<bool>(combat_.apply);
    script_weapon("AddWeapon 0014b890 (port-only preload)",[&](WeaponInventoryOps& ops){ops.add(item,true,preload);});
}
void NpcAiController::script_draw_weapon(std::int32_t item){
    script_weapon("DrawWeapon 0014bae0",[&](WeaponInventoryOps& ops){npc_select_item(component_,item,ops,actor_id_,combat_.random);});
}
void NpcAiController::script_change_weapon(){
    // 0014b9e0: 00143a60(inv, 1); again when the current slot +84 is then 6 (none); the burst reroll.
    script_weapon("ChangeWeapon 0014b9e0",[&](WeaponInventoryOps& ops){
        ops.cycle(true);
        if(ops.inventory().current_84==weapon_no_slot)ops.cycle(true);
        npc_reroll_burst(component_,ops,actor_id_,combat_.random);
    });
}
float NpcAiController::script_aim_percent() const{
    if(!combat_.aim_block)throw std::runtime_error("NPC AI: GetAimPercent 00149150 needs the aim block +20 (NpcCombatLink::aim_block)");
    return combat_.aim_block()[0];
}
void NpcAiController::script_store_weapon(){
    script_weapon("StoreWeapon 0014bbd0",[&](WeaponInventoryOps& ops){npc_store_weapon(component_,ops,actor_id_,combat_.random);});
}
bool NpcAiController::net_hold_item(std::int32_t item){
    bool held=false;
    script_weapon("001b2d20's 001439b0",[&](WeaponInventoryOps& ops){held=ops.equip_item(item);});
    return held;
}
void NpcAiController::script_remove_weapon(std::int32_t item){
    script_weapon("RemoveWeapon 0014b790",[&](WeaponInventoryOps& ops){npc_remove_weapon(component_,item,ops,actor_id_,combat_.random);});
}
void NpcAiController::script_send_new_awareness_of_players(){
    perception_.flags_21=static_cast<std::uint8_t>((perception_.flags_21&~1U)|1U);   // 0014a2f0 -> +f8 = 0014a320: +1ac +21 bit 0
}
void NpcAiController::script_force_holster(){
    script_weapon("ForceHolster 00143cc0",[](WeaponInventoryOps& ops){ops.force_holster();});
}
std::int32_t NpcAiController::script_active_weapon() const{
    const WeaponInventory* inventory=combat_.inventory?combat_.inventory():nullptr;
    if(!inventory)throw std::runtime_error("NPC AI: GetActiveWeapon 001855c0 for an actor without a combat inventory (+3524)");
    return weapon_active_item(*inventory);
}
void NpcAiController::script_weapon(const char* what,const std::function<void(WeaponInventoryOps&)>& body){
    if(!weapons_hosted())throw std::runtime_error(std::string("NPC AI: ")+what+" needs the combat link's inventory and weapon database");
    WeaponInventory* inventory=combat_.inventory();
    if(!inventory)throw std::runtime_error(std::string("NPC AI: ")+what+" for an actor without a combat inventory (+3524)");
    WeaponInventoryHost ih;ih.player=false;ih.attached=combat_.attached;ih.apply=combat_.apply;    // an AI component's +54 answers 0
    std::vector<WeaponInventoryEvent> inventory_events;
    WeaponInventoryOps ops(*inventory,*combat_.database,ih,inventory_events);
    body(ops);
    for(const auto& e:inventory_events){
        if(e.kind==WeaponInventoryEvent::Kind::attach&&!combat_.apply)
            throw std::runtime_error(std::string("NPC AI: ")+what+": attached weapon models (record +13c, 0038c7d0) are not hosted");
        if(e.kind==WeaponInventoryEvent::Kind::clear_attach_24a2)
            throw std::runtime_error(std::string("NPC AI: ")+what+": the slot-6/7 actor byte +24a2 is not hosted");
    }
    if(combat_.inventory_changed)combat_.inventory_changed();
    frame_.inventory_events.insert(frame_.inventory_events.end(),inventory_events.begin(),inventory_events.end());
}
void NpcAiController::dead_state_drop(SoloActorSimulation& actor){
    if(!combat_.inventory||!combat_.database)throw std::runtime_error("NPC AI: the dead-state drop 001a3520 needs the combat link's inventory");
    WeaponInventory* inventory=combat_.inventory();
    if(!inventory)throw std::runtime_error("NPC AI: the dead-state drop 001a3520 for an actor without a combat inventory");
    WeaponInventoryHost ih;ih.player=false;ih.apply=combat_.apply;
    std::vector<WeaponInventoryEvent> inventory_events;
    WeaponInventoryOps ops(*inventory,*combat_.database,ih,inventory_events);
    if(backpack_3464_!=0&&combat_.backpack_cleared_3464&&combat_.backpack_cleared_3464(backpack_3464_))backpack_3464_=0;   // 00254770
    NpcDeadDropView v;
    v.backpack_3464=backpack_3464_;v.armor_33a0=actor.health().armor_33a0;v.max_armor_339c=actor.health().max_armor_339c;
    v.root={1,actor.byte_2d(),0};v.pickupable_3468=actor.pickupable_3468();v.byte_14=actor.byte_14();
    NpcDeadDropHost h;
    h.items_3528=combat_.items_3528;h.create_backpack=combat_.create_backpack;
    h.add_weapon=combat_.add_backpack_weapon;h.add_armor=combat_.add_backpack_armor;
    if(const auto backpack=npc_dead_inventory_drop(ops,*combat_.database,v,h))backpack_3464_=*backpack;
    if(combat_.inventory_changed)combat_.inventory_changed();
    frame_.inventory_events.insert(frame_.inventory_events.end(),inventory_events.begin(),inventory_events.end());
}
void NpcAiController::controller_message(SoloActorSimulation&,std::int32_t code){
    if(port_hold_code6_&&code==6){port_code6_held_=true;return;}             // port-only (--coop-partner)
    NpcAiMessage m;m.kind=NpcAiMessage::Kind::controller;m.event=static_cast<std::uint8_t>(code);
    queue_.push_back(m);                                                      // transport 0
}
void NpcAiController::port_release_code6(SoloActorSimulation& actor){
    port_hold_code6_=false;
    if(!port_code6_held_)return;
    port_code6_held_=false;
    controller_message(actor,6);
}
void NpcAiController::controller_message_immediate(SoloActorSimulation& actor,std::int32_t code){
    if(port_hold_code6_&&code==6){port_code6_held_=true;return;}             // port-only (--coop-partner)
    NpcAiMessage m;m.kind=NpcAiMessage::Kind::controller;m.event=static_cast<std::uint8_t>(code);
    frame_.delivered.push_back(m);
    auto h=host(actor);
    dispatch_npc_ai(component_,m,h);                                          // 00151d30
}
bool NpcAiController::alive_or_eligible(SoloActorSimulation& actor){
    const auto life=actor.life_state();                                       // virtual +108
    if(life==2||life==3)return !npc_corpse_removable_164(corpse_view(actor));   // !virtual +164 (0014b610)
    return true;
}
void NpcAiController::activate_by_radius(float radius,std::function<std::optional<float>()> closest_player_distance_squared){
    activate_npc_by_radius(component_,radius);
    player_distance_=std::move(closest_player_distance_squared);
}
}
namespace omega {
namespace {
void need_link(const NpcPerceptionLink& link){
    if(!link.sense)throw std::runtime_error("NPC AI: the perception is not hosted (NpcAiController::set_perception_link)");
}
}
void NpcAiController::script_add_awareness(SoloActorSimulation& actor,std::uint64_t object){
    need_link(perception_link_);
    auto h=host(actor);std::vector<NpcPerceptionSend> out;
    npc_add_awareness(component_,object,h,out);                                  // 0014a410 -> +e4
    npc_forward_perception_sends(out,h);
}
void NpcAiController::script_add_awareness_of_players(SoloActorSimulation& actor){
    need_link(perception_link_);
    auto h=host(actor);std::vector<NpcPerceptionSend> out;
    npc_add_awareness_of_players(component_,h,out);                              // 00149420 -> +ec
    npc_forward_perception_sends(out,h);
}
void NpcAiController::script_set_target(SoloActorSimulation& actor,std::uint64_t object){
    need_link(perception_link_);
    auto h=host(actor);
    npc_script_set_target(component_,object,h);                                   // 0014a210
}
void NpcAiController::script_resume_ai_targeting(SoloActorSimulation& actor){
    need_link(perception_link_);
    auto h=host(actor);
    npc_resume_ai_targeting(h);                                                   // 0014a1c0
}
void NpcAiController::script_remove_awareness(SoloActorSimulation& actor,std::uint64_t object){
    need_link(perception_link_);
    auto h=host(actor);
    npc_perception_remove(perception_,*h.sense,object);                           // 0014a3c0 -> +e8 = 0014a400 -> 0016c8d0
}
void NpcAiController::script_resend_awareness(SoloActorSimulation& actor,std::uint64_t object){
    need_link(perception_link_);
    auto h=host(actor);
    npc_perception_resend(perception_,*h.sense,object);                           // +f4 = 0014a380 -> 0016c590
}
bool NpcAiController::script_in_view_cone(SoloActorSimulation& actor,std::uint64_t object){
    need_link(perception_link_);
    auto h=host(actor);
    return npc_perception_in_view_cone(perception_,*h.sense,object);              // 00149270 -> +188 = 0014afc0 -> 0016c1e0
}
void NpcAiController::script_set_view_cone_radius(float radius){
    perception_.radius_10=radius;                                                 // 00149320 -> +180 = 00149360: perception +10
}
void NpcAiController::script_set_view_cone_angle(float angle){
    perception_.half_angle_14=npc_view_cone_half_angle(angle);                    // 001492b0 -> +184 = 001492f0: perception +14
}
std::uint64_t NpcAiController::script_node_object(std::int32_t node){
    graph_.node(node);
    return npc_node_object(node);
}
void NpcAiController::script_resume_ai_mode(SoloActorSimulation& actor){
    need_link(perception_link_);
    auto h=host(actor);
    npc_resume_ai_mode(component_,h);                                             // 0014a470 -> +e0
}
void NpcAiController::perception_awareness(SoloActorSimulation& actor,std::uint64_t source,std::uint8_t cause,float value){
    need_link(perception_link_);
    auto h=host(actor);std::vector<NpcPerceptionSend> out;
    npc_perception_add(perception_,*h.sense,source,cause,value,out);           // 0016c9d0
    npc_forward_perception_sends(out,h);
}
void NpcAiController::hear(SoloActorSimulation& actor,std::uint8_t cause,std::uint64_t source,float value){
    need_link(perception_link_);
    auto h=host(actor);std::vector<NpcPerceptionSend> out;
    npc_hear(component_,cause,value,source,h,out);                              // virtual +d0 = 00152fd0
    npc_forward_perception_sends(out,h);
}
bool NpcAiController::senses(SoloActorSimulation& actor,std::uint64_t object){
    need_link(perception_link_);
    auto h=host(actor);
    const auto* r=npc_perception_sensed_record(perception_,*h.sense,object);    // 0016c6a0 / NpcPerception_SensedRecord
    return r&&r->byte_39!=0&&r->byte_38!=0;
}
void NpcAiController::script_look_around(SoloActorSimulation& actor){
    auto h=host(actor);
    npc_look_around(component_,100,h);                                            // 00149e50 -> +134(c, 100)
}
void NpcAiController::script_stop_and_look_around(SoloActorSimulation& actor){
    auto h=host(actor);
    npc_stop_and_look_around(component_,100,h);                                   // 00149e20 -> +138(c, 100)
}
void NpcAiController::script_request_action(SoloActorSimulation& actor,std::uint8_t action){
    auto h=host(actor);
    npc_request_action(component_,action,100,0.0F,h);                            // 00149ce0 -> +150 = 0014d8f0
}
void NpcAiController::script_request_facial(SoloActorSimulation& actor,std::uint64_t object,float duration){
    if(!facial_hosted())throw std::runtime_error("NPC AI: RequestFacial (0014dea0) needs the perception link and the attack stages");
    auto h=host(actor);
    npc_script_request_facial(component_,object,duration,h);                     // 00149e80 -> +12c(c, object, 100, f12)
}
void NpcAiController::script_request_facial_node(SoloActorSimulation& actor,std::int32_t node,float duration){
    graph_.node(node);
    script_request_facial(actor,npc_node_object(node),duration);                  // 003cc820 (identity): the node object
}
void NpcAiController::script_request_look_at(SoloActorSimulation& actor,std::uint64_t object,float duration){
    if(!facial_hosted())throw std::runtime_error("NPC AI: RequestLookAt (0014dfb0) needs the perception link and the attack stages");
    auto h=host(actor);
    npc_script_request_look_at(component_,object,duration,h);                    // 00149ed0 -> +128(c, object, 100, f12)
}
void NpcAiController::script_set_grenade_target(SoloActorSimulation& actor,std::uint64_t object){
    auto h=host(actor);
    npc_script_set_grenade_target(component_,object,h);                          // 00149810 -> +19c = 0014b410
}
void NpcAiController::script_set_grenade_target_node(SoloActorSimulation& actor,std::int32_t node){
    graph_.node(node);
    script_set_grenade_target(actor,npc_node_object(node));                     // 003cc820 (identity): the node object
}
void NpcAiController::script_request_look_at_node(SoloActorSimulation& actor,std::int32_t node,float duration){
    graph_.node(node);
    script_request_look_at(actor,npc_node_object(node),duration);                // 003cc820 (identity): the node object
}
void NpcAiController::script_resume_look_at(){
    npc_resume_look_at(component_,100);                                           // 00149df0 -> +13c = 0014de30(c, 100)
}
void NpcAiController::script_deactivate_dead(bool deactivate){
    npc_deactivate_dead(component_,deactivate);                                   // 00149fe0 -> +160 = 0014b680
}
void NpcAiController::script_set_target_node(SoloActorSimulation& actor,std::int32_t node){
    graph_.node(node);
    script_set_target(actor,npc_node_object(node));                              // 003cc820 (identity): the node object
}
void NpcAiController::script_add_awareness_node(SoloActorSimulation& actor,std::int32_t node){
    graph_.node(node);
    script_add_awareness(actor,npc_node_object(node));                           // 003cc820 (identity): the node object
}
}
namespace omega {
std::uint8_t NpcAiController::script_move_to_node(std::int32_t node){
    graph_.node(node);                                                           // 0015c120 (identity), node +78
    return npc_move_to_node(component_,node);                                    // 0014a5c0 -> +100 = 0014be50
}
std::uint8_t NpcAiController::move_to_object(std::int32_t object_id){
    return npc_move_to_object(component_,object_id);                             // virtual +104 = 0014be10
}
void NpcAiController::stop_movement(SoloActorSimulation& actor,std::int32_t priority,float duration){
    auto h=host(actor);
    npc_stop_movement(component_,priority,duration,h);                           // virtual +154 = 0014d4d0
}
void NpcAiController::resume_movement(SoloActorSimulation& actor){
    auto h=host(actor);
    resume_npc_movement(component_,h);                                           // virtual +158 = 0014d470
}
void NpcAiController::script_resume_movement(SoloActorSimulation& actor){
    auto h=host(actor);
    resume_npc_movement(component_,h);                                           // 00149c70 -> +158 = 0014d470
}
void NpcAiController::script_restrict_to_node_group(std::int32_t node){
    graph_.node(node);                                                           // 0015c120 (identity)
    npc_clear_allowed_regions(component_,graph_,node);                           // 00149500 -> +1f4 = 0014bea0
}
void NpcAiController::script_add_group_to_path_region(std::int32_t node){
    graph_.node(node);                                                           // 0015c120 (identity)
    npc_allow_region(component_,graph_,node);                                    // 001494c0 -> +1f8 = 0014bf60
}
void NpcAiController::script_remove_group_from_path_region(std::int32_t node){
    graph_.node(node);                                                           // 0015c120 (identity)
    npc_forbid_region(component_,graph_,node);                                   // 00149480 -> +1fc = 0014bf00
}
void NpcAiController::script_activate_by_radius_off(){
    deactivate_npc_by_radius(component_);                                        // 00148eb0 -> +1d0 = 0014ae30
}
void NpcAiController::script_activate_body_toss(bool flag){
    NpcAiMessage m;m.kind=NpcAiMessage::Kind::body_toss;m.value=flag?1:0;
    queue_.push_back(m);
}
void NpcAiController::script_set_disguise(SoloActorSimulation& actor,std::uint32_t a1){
    actor.set_disguise_3469(npc_disguise_store(actor.eligible(),actor.disguise_3469(),a1));
}
std::uint8_t npc_disguise_store(bool present,std::uint8_t byte_3469,std::uint32_t a1){
    const std::uint8_t cur=present?(byte_3469!=0?1:0):0;                         // 0014ac9c..0014acd8
    const auto value=static_cast<std::uint8_t>(a1&0xffU);
    return value!=cur?value:byte_3469;                                           // 0014acdc; 0014ace4: sb a1, +3469
}
void NpcAiController::script_set_pickupable(SoloActorSimulation& actor,bool value){
    actor.set_pickupable_3468(value?1:0);                                        // 0014a8ac: sb a1, actor +3468
    npc_deactivate_dead(component_,!value);                                      // 005721c8 clear: +160 = 0014b680(c, !value)
    const auto life=actor.life_state();                                          // virtual +108
    if(life!=2&&life!=3)return;
    auto& dead=system_.dead_list;
    if(value){
        // 0014a970..0014aa68: the component's entry in the dead list 004ea370;
        // when found, 0015af50(actor) (a think-list entry) and 00138ab0 erases it.
        const auto at=std::find_if(dead.begin(),dead.end(),[this](const NpcDeadListEntry& e){return e.component==&component_;});
        if(at==dead.end())return;
        auto h=host(actor);
        SoloActorSimulation* p=&actor;
        system_.think_list.push_back({&component_,h.actor.eligible,h.actor.life_state,actor_id_,
                                      [this,p]{return alive_or_eligible(*p);}});   // 0015af50
        dead.erase(at);
        return;
    }
    // 0014aa78..0014ab54: the first think-list entry whose handle resolves to
    // this actor; then 0015ad70(actor) and 0014ab90(004ea370, &component).
    auto& think=system_.think_list;
    if(std::none_of(think.begin(),think.end(),[this](const NpcAiThinker& t){return t.component==&component_;}))return;
    for(auto it=think.begin();it!=think.end();)if(it->component==&component_)it=think.erase(it);else ++it;   // 0015ad70
    SoloActorSimulation* p=&actor;
    dead.push_back({&component_,[this,p]{return corpse_view(*p);}});            // 0014ab90(004ea370, &component)
}
void NpcAiController::script_teleport(SoloActorSimulation& actor,std::int32_t node){
    graph_.node(node);                                                           // 0015c120 (identity), node +78
    auto h=host(actor);
    npc_teleport_to_node(component_,node,h);                                     // 00149c30 -> +10c = 0014afe0
}
void NpcAiController::script_force_weapon_fire(SoloActorSimulation& actor,std::int32_t shots,std::int32_t item,float delay){
    if(item<0){npc_force_weapon_fire_stores(component_,shots,delay);return;}  // 0014b494: bltz, no host read
    auto h=host(actor);
    if(!h.attack)throw std::runtime_error("NPC AI: ForceWeaponFire 0014b470 with an item needs the attack stages (perception and combat links) for +1a4 / +1ac");
    npc_force_weapon_fire(component_,shots,item,delay,h);                        // 00149b50 -> +198 = 0014b470
}
bool NpcAiController::script_dont_drop_weapons(){
    if(!combat_.inventory)return false;
    WeaponInventory* inventory=combat_.inventory();
    if(!inventory)throw std::runtime_error("NPC AI: DontDropWeapons 0014a7d0 for an actor without a combat inventory (+3524)");
    inventory->byte_a8=1;                                                        // 0014a7d0: *(actor +3524) +a8 = 1
    if(combat_.inventory_changed)combat_.inventory_changed();
    return true;
}
void NpcAiController::port_route_overlay(bool on){route_client_->port_overlay_links=on;}
void NpcAiController::port_relocate_path_follower(SoloActorSimulation& actor,std::int32_t node){
    auto h=host(actor);
    npc_port_relocate_path_follower(component_,node,h);
}
void NpcAiController::port_drop_path_segment(){npc_port_drop_path_segment(component_);}
std::optional<std::uint8_t> NpcAiController::port_route_to_point(SoloActorSimulation& actor,const RootVector& point){
    if(component_.mode_44!=7||component_.pending_50!=0)
        throw std::runtime_error("NPC AI (port-only): a route to a point needs mode 7 entered with no mode request pending");
    auto h=host(actor);
    auto& m=npc_route_manager(h);
    if(!m.regions)throw std::runtime_error("NPC AI (port-only): a route to a point needs the route manager's regions (0015d790)");
    const auto region=npc_region_at(*m.regions,graph_,point,component_.path.destination_5c,port_last_dt_,
        "001547d4 (port-only mode 7 route to a point: f20 = the last update's dt, as in mode 7's enter)");   // 001547d4 0015d790
    port_route_event_.reset();
    const auto r=npc_route_to_point(m,component_.path,h.route_client,point,region);              // 00154800 0015fa70
    component_.timer_e8=-1;                                                                        // 00154808
    return r;
}
void NpcAiController::script_set_mode_at_node(SoloActorSimulation& actor,std::uint32_t mode,std::int32_t node){
    graph_.node(node);
    auto h=host(actor);
    npc_set_mode_at_node(component_,mode,node,h);                                // 0014a4a0 -> +dc = 0014bcf0
}
}
