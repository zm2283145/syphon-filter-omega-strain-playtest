#include "solo_actor_simulation.hpp"
#include "weapon_field_of_view.hpp"
#include "actor_quick_turn.hpp"
#include "actor_flashbang_hide.hpp"
#include "actor_roll_action.hpp"
#include "actor_cancel_action.hpp"
#include "actor_edge_detection.hpp"
#include "actor_hanging.hpp"
#include "actor_body_carry.hpp"
#include "actor_landing_effect.hpp"
#include "actor_ledge_action.hpp"
#include "actor_ladder.hpp"
#include "actor_support_motion.hpp"
#include "actor_pad_actions.hpp"
#include "actor_specialized_body_query.hpp"
#include "morph_resources.hpp"
#include "actor_body_query.hpp"
#include "actor_contact_events.hpp"
#include "actor_gather_bounds.hpp"
#include "actor_movement_scheduler.hpp"
#include "caster_bounds.hpp"
#include "actor_physical_prediction.hpp"
#include "actor_physical_response.hpp"
#include "actor_physical_state.hpp"
#include "actor_response_placement.hpp"
#include "actor_water_support.hpp"
#include "actor_model_defaults.hpp"
#include "col_gather.hpp"
#include "collision_triangles.hpp"
#include "game_libm.hpp"
#include "game_sound_voice.hpp"
#include "hit_direction.hpp"
#include "world_los.hpp"
#include "morph_resources.hpp"
#include "player_camera.hpp"
#include "port_cheats.hpp"
#include "zone_profile.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>

namespace omega {
namespace {
// World query box -> owner-local box (all eight corners through the inverse).
Bounds local_bounds(const Bounds& world,const CollisionMatrix& m){
    const float a=m[0],b=m[4],c=m[8],d=m[1],e=m[5],f=m[9],g=m[2],h=m[6],k=m[10];
    const float det=a*(e*k-f*h)-b*(d*k-f*g)+c*(d*h-e*g);
    if(!(std::fabs(det)>1e-12F))throw std::runtime_error("Singular object owner matrix");
    const float inv[9]={(e*k-f*h)/det,(c*h-b*k)/det,(b*f-c*e)/det,
                        (f*g-d*k)/det,(a*k-c*g)/det,(c*d-a*f)/det,
                        (d*h-e*g)/det,(b*g-a*h)/det,(a*e-b*d)/det};
    Bounds out{{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()},
               {-std::numeric_limits<float>::max(),-std::numeric_limits<float>::max(),-std::numeric_limits<float>::max()}};
    for(int corner=0;corner<8;++corner){
        const float x=(corner&1?world.maximum.x:world.minimum.x)-m[12];
        const float y=(corner&2?world.maximum.y:world.minimum.y)-m[13];
        const float z=(corner&4?world.maximum.z:world.minimum.z)-m[14];
        const float l[3]={inv[0]*x+inv[1]*y+inv[2]*z,inv[3]*x+inv[4]*y+inv[5]*z,inv[6]*x+inv[7]*y+inv[8]*z};
        out.minimum={std::min(out.minimum.x,l[0]),std::min(out.minimum.y,l[1]),std::min(out.minimum.z,l[2])};
        out.maximum={std::max(out.maximum.x,l[0]),std::max(out.maximum.y,l[1]),std::max(out.maximum.z,l[2])};
    }

    return out;
}

// The single actor sees an empty actor list and no players.
struct SoloMovementHost final:ActorMovementWorldHost {
    ActorResponsePhysical* physical{};
    RootVector* component{};
    std::uint8_t* byte_3d0{};
    bool class_flag(std::uint64_t) override{return false;}
    std::uint8_t mode_108(std::uint64_t) override{return 0;}
    std::optional<bool> controller_54(std::uint64_t) override{return std::nullopt;}
    std::optional<bool> controller_58(std::uint64_t) override{return std::nullopt;}
    std::optional<bool> controller_5c(std::uint64_t) override{return std::nullopt;}
    std::int32_t physical_mode(std::uint64_t) override{return physical->mode_20;}
    bool physical_enabled(std::uint64_t) override{return *byte_3d0!=0;}   // physical +3d0
    Bounds local_bounds(std::uint64_t) override{return {{-1,-1,0},{1,1,6}};}
    CollisionMatrix refreshed_world_transform(std::uint64_t) override{
        return {1,0,0,0,0,1,0,0,0,0,1,0,(*component)[0],(*component)[1],(*component)[2],1};}
    bool byte_2c(std::uint64_t) override{return false;}
    bool byte_2d(std::uint64_t) override{return false;}
    bool byte_2e(std::uint64_t) override{return false;}
    // Actor +14: every object constructed locally has it (003cb0e0); the
    // list is empty, so only the actor itself could be asked.
    std::uint8_t byte_14(std::uint64_t) override{return 1;}
    std::uint8_t byte_49(std::uint64_t) override{return 2;}
    RootVector position_e0(std::uint64_t) override{return *component;}
    bool related(std::uint64_t,std::uint64_t) override{return false;}
    std::uint64_t list_begin() override{return 0;}
    std::uint64_t list_end() override{return 0;}
    std::uint64_t list_next(std::uint64_t) override{return 0;}
    std::optional<std::uint64_t> list_element(std::uint64_t) override{return std::nullopt;}
    std::optional<std::uint64_t> element_owner(std::uint64_t) override{return std::nullopt;}
    std::optional<std::uint64_t> player_slot(int) override{return std::nullopt;}
    float secondary_root_weight(std::uint64_t) override{return 0;}
    float tertiary_root_weight(std::uint64_t) override{return 0;}
    void send_interaction(std::uint64_t,std::uint64_t,std::int32_t) override{}
};

// Water-effect view of the single actor (generation 1, model component at the
// root pose, the same authored bounds as the movement host).
struct SoloWaterActors final:WaterEffectActorHost {
    RootVector* component{};
    std::optional<std::int32_t> generation(std::uint32_t) override{return 1;}
    bool has_transform_component(std::uint32_t) override{return true;}
    std::optional<WaterActorBounds> bounds(std::uint32_t) override{return WaterActorBounds{{-1,-1,0,1},{1,1,6,1}};}
    std::uint8_t byte_49(std::uint32_t) override{return 2;}
    RootVector refreshed_translation(std::uint32_t) override{return *component;}
    RootVector current_translation(std::uint32_t) override{return *component;}
};
struct SoloWaterSink final:WaterEffectSink {
    WaterRecordResources record_resources() override{return {0x28,0x27,1};}
    void request(const WaterEffectRequest&) override{}
    void splash(std::uint8_t,const RootVector&) override{}
};
}

void SoloCollisionWorld::add(std::vector<std::uint8_t> col,std::string name,std::optional<CollisionMatrix> owner_matrix){
    cols.push_back(std::move(col));names.push_back(std::move(name));owners.push_back(owner_matrix);game_objects.push_back(0);
}
void SoloCollisionWorld::set_game_object(std::uint64_t handle,std::uint64_t object){
    geometry(handle);
    if(game_objects.size()!=cols.size())throw std::runtime_error("COL geometry list without game objects");
    game_objects[handle-1]=object;
}
std::uint64_t SoloCollisionWorld::game_object(std::uint64_t handle) const{
    geometry(handle);
    return handle<=game_objects.size()?game_objects[handle-1]:0;
}
std::span<const std::uint8_t> SoloCollisionWorld::geometry(std::uint64_t handle) const{
    if(handle==0||handle>cols.size())throw std::runtime_error("Unknown COL geometry handle");
    return cols[handle-1];
}
std::optional<CollisionMatrix> SoloCollisionWorld::owner(std::uint64_t handle) const{geometry(handle);return owners[handle-1];}
void SoloCollisionWorld::set_owner(std::uint64_t handle,const CollisionMatrix& owner_matrix){
    geometry(handle);
    if(!owners[handle-1])throw std::runtime_error("COL geometry without a transform component cannot move");
    owners[handle-1]=owner_matrix;
}
std::vector<ExpandedCollisionTriangle> SoloCollisionWorld::expand(const Bounds& bounds) const{
    ExpandScratch scratch;
    expand(bounds,scratch);
    return std::move(scratch.triangles);
}
const std::vector<ExpandedCollisionTriangle>& SoloCollisionWorld::expand(const Bounds& bounds,ExpandScratch& scratch,
    std::uint8_t excluded_surface_flags) const{
    auto& drafts=scratch.drafts;drafts.clear();
    for(std::size_t i=0;i<cols.size();++i){
        if(object_eligible&&i<game_objects.size()&&game_objects[i]!=0&&!object_eligible(game_objects[i]))continue;   // 001830d0
        const auto& owner_matrix=owners[i];
        if(col_indices.size()<cols.size())col_indices.resize(cols.size());
        if(!col_indices[i])col_indices[i]=index_col5(cols[i]);
        gather_col5_triangles(*col_indices[i],owner_matrix?local_bounds(bounds,*owner_matrix):bounds,excluded_surface_flags,scratch.gathered);
        for(auto index:scratch.gathered.triangle_indices){
            try{
                drafts.push_back(expand_col5_triangle(cols[i],{i+1,index,i+1},
                    [&owner_matrix]{return owner_matrix;}));
            }catch(const std::exception& error){
                std::ostringstream s;s<<error.what()<<" (COL handle "<<(i+1)<<" triangle "<<index;
                if(i<game_objects.size())s<<", object "<<game_objects[i];
                if(owner_matrix){s<<", owner";for(const auto x:*owner_matrix)s<<' '<<std::hexfloat<<x;}
                s<<")";throw std::runtime_error(s.str());
            }
        }
    }
    finalize_collision_triangles(drafts,scratch.triangles);
    return scratch.triangles;
}
void SoloCollisionWorld::gather(const Bounds& bounds){prepare_collision_candidates(expand(bounds,gather_scratch),batch);}
ExpandedCollisionTriangle SoloCollisionWorld::refresh(const CollisionTriangleSource& source) const{
    return refresh_collision_source_triangle(source,
        [this](std::uint64_t h){return geometry(h);},
        [this](std::uint64_t h){return owner(h);});
}
RootVector SoloCollisionWorld::pick_start(float height) const{
    constexpr float big=std::numeric_limits<float>::max();
    const auto all=expand({{-big,-big,-big},{big,big,big}});
    float best=-1;RootVector start{};
    for(const auto& t:all){
        const auto& n=t.normal;
        const float len=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if(len==0||n[2]/len<0.9F||(t.surface[0]&0x80))continue;
        const float ax=t.vertices[1][0]-t.vertices[0][0],ay=t.vertices[1][1]-t.vertices[0][1];
        const float bx=t.vertices[2][0]-t.vertices[0][0],by=t.vertices[2][1]-t.vertices[0][1];
        const float area=std::fabs(ax*by-ay*bx);
        if(area>best){best=area;
            for(int i=0;i<3;++i)start[i]=(t.vertices[0][i]+t.vertices[1][i]+t.vertices[2][i])/3.0F;}
    }
    if(best<0)throw std::runtime_error("No upward-facing solid triangle in COL");
    start[2]+=height;start[3]=1;
    return start;
}

struct SoloActorSimulation::Impl {
    // The shared static collision world and this actor's candidate batch
    // (001811c0 gathers before each physical frame).
    std::shared_ptr<const SoloCollisionWorld> shared_world;
    const SoloCollisionWorld& world() const{return *shared_world;}
    CollisionCandidateBatch batch;
    SoloCollisionWorld::ExpandScratch gather_scratch;   // 132win11: kept across gathers (port-side speed only)
    void gather(const Bounds& bounds){prepare_collision_candidates(world().expand(bounds,gather_scratch),batch);}
    std::uint64_t handle{SoloCollisionWorld::actor_handle};   // the actor object
    std::uint64_t controller_handle{0x200000};                // actor +58
    // The +58 component (SoloActorController) and, for an AI component, the
    // navigation block +30ac..+30e0 and the inputs of 001a80e0.
    SoloActorController controller;
    bool player() const{return controller.ai==nullptr;}
    ActorNavigationTarget navigation;
    std::array<std::uint32_t,3> control_3030{};              // +3030/+3034/+3038
    std::uint8_t latch_303c{};                               // +303c
    std::uint8_t byte_2c{1},byte_2d{1},byte_2e{};            // root object bytes
    bool deactivated{};                                      // 001b5190 ran
    // The game RNG and the frame counter: the world's, or this simulation's own.
    std::uint64_t own_random{};
    std::uint64_t* rng{&own_random};
    std::uint32_t own_frame_counter{};
    const std::uint32_t* frame_counter{&own_frame_counter};
    SoloActorConfig config;
    SoloActorAnimation* model{};
    std::optional<ActorAnimationTuning> tuning;
    RootAttachments roots;
    std::size_t floor_root{},edge_root{},ladd_root{};
    std::array<float,3> weights{1,0,0}; // authored: used only when the model does not report root weights
    // Retained producer fields: prior +3040, latch +31dc, accumulation timer.
    SoloActorControlFields controls;
    SoloActorStateWords words;
    std::array<float,4> prior{};bool facing_positive{};float accumulation{};
    float approach_step_31e0{},running_31f8{};
    ActorHeadingState heading_state;
    std::optional<RootVector> heading_orientation;
    PlayerMoveState pad_state;
    PlayerQuickTurnState quick_turn;
    // Priority actions +3210/+3218 and mode +320c (highest action).
    ActorAnimationReceiverState actions;
    std::array<float,4> previous_left_60{};   // input +0x60 (0x0024fa58)
    // The model's transform component (P+8 +28): the model's published
    // placement when it reports one, otherwise the floor root's animated pose
    // (refreshed before the physical stage each frame).
    RootVector component{};
    std::optional<RootParentTransform> published_component;
    RootVector component_orientation(){
        return published_component?published_component->orientation:roots.world_pose(floor_root).orientation;}
    RootVector published{};bool published_set{};bool frame_begun{};   // frame_begun: a step has started (published_set cleared)
    bool response_ran{};   // the step's physical response 001e29xx has run (published, or its tail 0x001e4dd8 left the model's)
    // Model context +50 (P+8 +50): the constructor's (0,0,0,1) (0017f960) until
    // a world entry rebuilds it from the weighted nodes (003a9090, after the
    // physical step), so each physical step reads the previous entry's centroid.
    RootVector anchor_local{0,0,0,1};
    std::vector<float> centroid_weights;   // node +170 of the context's collection
    // 001af190 (activation): the human weight table on the node collection.
    void weight_model_centroid(){
        if(model&&model->human_node_collection())centroid_weights=human_centroid_weights(model->skeleton_node_matrices().size());
    }
    // 00392aa0 (392af0..392b08) with the node collection (+2e64): 003a9220
    // (the context bounds +30/+40, actor +2e80), then 003a9090.
    void rebuild_model_centroid(){
        if(!model||!model->human_node_collection())return;
        const auto nodes=model->skeleton_node_matrices();
        const auto b=human_context_bounds(nodes);
        context_bounds={{b.minimum[0],b.minimum[1],b.minimum[2]},{b.maximum[0],b.maximum[1],b.maximum[2]}};
        if(centroid_weights.empty())centroid_weights.assign(nodes.size(),0.0F);   // node constructor 003bac20: weight 0
        anchor_local=human_model_centroid(nodes,centroid_weights);
    }
    // Model context +30/+40 (actor +2e80): 00183010's sentinel until a rebuild.
    Bounds context_bounds{{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()},
        {-std::numeric_limits<float>::max(),-std::numeric_limits<float>::max(),-std::numeric_limits<float>::max()}};
    // 001e5880 (actor virtual +e4, 00132860): the context vector +50 through
    // the component matrix (rotation of the component placement, translation
    // the component translation).
    RootVector model_anchor_world(){
        return component_placed_vector(root_placement_matrix(RootParentTransform{component_orientation(),component}),anchor_local);
    }
    // Actor virtual +e4 (00132860: actor +2e78 matrix times actor +2ea0)
    // between frames: the component translation is the physical response's
    // publication. Before the first frame it is the load placement and +2ea0
    // (context +50) the constructor's (0,0,0,1) (0017f960): an actor that is
    // never activated (a HUD map record, 0026c570's fit) has that +e4. A step
    // whose response took the setup's tail (physical +12 clear or mode 12:
    // 0x001e4dd8) or whose mode skips the response has no component +14 store:
    // it leaves the translation the model
    // step wrote (actor +50 is the model component +2e78, HANGING.md), as the
    // hotbox tracker reads it. Inside a frame, before the response runs, it
    // is not modelled.
    RootVector position_e4(){
        if(frame_begun&&!response_ran)throw std::runtime_error("Solo actor: the actor's +e4 inside its frame before the physical response publishes");
        return component_placed_vector(root_placement_matrix(RootParentTransform{component_orientation(),
            published_set||!frame_begun?published:component}),anchor_local);
    }
    ActorPhysicalFrameState frame_state;
    SoloMovementHost movement;
    ActorMovementWorldHost* movement_world{};   // set_movement_world; null: the solo stand-in
    ActorPhysicalFrameHost frame_host;
    std::uint64_t last_contact_object{};
    std::unique_ptr<WaterEffectManager> own_water;
    SoloWaterActors own_water_actors;
    SoloWaterSink own_water_sink;
    // The manager the actor's water calls reach: the world's (SoloActorWorldLink::water) or its own.
    WaterEffectManager* water{};
    WaterEffectActorHost* water_actors{};
    WaterEffectSink* water_sink{};
    ActorWaterSupportHost water_host;
    int water_events{};
    bool in_water_346d{};                    // +346d: 0 from 001b5fe0 (001b6228), written by 001e5ea8
    // Cross/Triangle senders and the actor fields they reach. The input's
    // priority sets are not hosted: the current set keeps its constructor
    // value (selected 0, mask 1) and there are no code-9 zones.
    PlayerButtonState buttons;
    bool byte_31b1{},byte_31b5{};
    ActorMotionTransitionState transition_2ef8;            // actor +2ef8..+2f08
    // Physical +1f0/+200/+260/+2c0 live in frame_state.support_history, the state
    // 001e6300 retains and 001e7110 consumes; the ledge (00193180..) and ladder
    // installs write slots 1 and 2 there.
    void bind_support(std::size_t i,const ActorLedgeSupportBinding& b){
        auto& s=frame_state.support_history;
        s.owners.at(i)=b.object;s.local.at(i)=b.local;s.previous_owner.at(i)=b.previous_owner;s.retain.at(i)=b.enabled;
    }
    // The owner object's +50 transform component (001e6300's captured
    // pointer, refreshed per lookup from the owner matrix) and the three
    // roots' captured weight and placement, kept stable for the callbacks.
    std::map<std::uint64_t,ActorSupportHistoryComponent> support_components;
    std::array<ActorSupportHistoryRoot,3> support_roots{};
    std::uint32_t found_ledge_130{};                       // player input +0x130
    SoloActorFrame* current{};                             // frame being produced
    bool executing{};                                      // this actor's own phase of a world frame is running
    // Jump to grab and hang (HANGING.md).
    ActorJumpState jump;                                   // +3490, +34a0..+34d0, +30a0/+30a4
    ActorActionHistory action_history;                     // +3220/+3234, +3250/+3258
    std::int32_t selector_38{};                            // input +0x38 (selected value)
    std::int32_t previous_selector_30{};                   // input +0x30 (the previous frame's +0x38)
    std::array<float,4> look_90{};                         // input +0x90 (constructor zero; 0x0024fa68)
    // Input +0x124 / +0x125 / +0x126 (player +174 / +175 / +176): the options'
    // manual aim, free look X and free look Y bytes. The constructor zeroes them
    // (0x00250744); the level load (00135848..00135868) and the options apply
    // 0045a3a0 store the record *004ffd2c's +4 / +6 / +7 (set_input_option_byte).
    std::uint8_t manual_aim_124{},free_look_x_125{},free_look_y_126{};
    std::int32_t heading_entry_20{-1};                     // input +0x20 (constructor 002504e0: -1)
    std::uint8_t byte_2e00{};                              // actor +2e00 (0024a850)
    // 003c9420 with transport 0 clones the message into the pending queue; the
    // pump 003c8de0 delivers it later. The solo host delivers at the start of
    // the next step (the order relative to the frame's actor logic is inferred).
    // Kinds: a player-component code (class 0055a1d8), an actor action (0x11,
    // 0x13; 0x17 with its jump-zone message), a hotbox event to a box (from
    // the tracker), a hotbox event from a box to the actor and an interaction
    // trigger (0021b790) to a box link.
    struct QueuedMessage {
        enum class Kind {component,action,box,actor_hotbox,trigger,respawn,checkpoint,object_action,object_code,ai_notification,link,notify_checkpoint,death,carry_pickup,        turn_on_weapon_pickup,out_of_ammo,weapon_vis_change,running_jump} kind{};
                std::int32_t code{};                               // kind out_of_ammo: the item (+24); weapon_vis_change: the value (+24)
        std::size_t box{};std::uint8_t payload{};std::uint32_t word{};
        ActorJumpZoneMessage jump_zone;
        RootVector point{};                                // kind running_jump: the payload +30 (code 0x15 / 0x16)
        HotboxTrackerEvent tracker_event;                  // kind box
        HotboxTrigger trigger;                             // kind trigger
        std::array<float,3> node_position{};float node_yaw{};std::string label;   // kind checkpoint
        SoloActorSimulation::ObjectAction object_action;   // kind object_action
        std::uint64_t object{};                            // kind object_code (code = the CAS code); carry_pickup: the body (payload +28)
        ActorAiNotification notification;                 // kind ai_notification
        bool enable{};                                     // kind link: EnableMsg / DisableMsg
    };
    std::vector<QueuedMessage> queue;
    std::vector<QueuedMessage> pump_snapshot;              // the pump's snapshot of queue
    void queue_component(std::int32_t code){QueuedMessage m;m.kind=QueuedMessage::Kind::component;m.code=code;queue.push_back(m);}
    void queue_action(std::int32_t code){QueuedMessage m;m.kind=QueuedMessage::Kind::action;m.code=code;queue.push_back(m);}
    // Object actions (actor_object_action.hpp) and the world's active object
    // list 004ffb50 +c4. The objects an object action targeted receive the
    // motion tokens' CAS codes.
    ActorWorldStack world_objects;
    // Objects pushed with their own +2c/+2d/+2e (push_pending_world_object);
    // the others answer zero bytes.
    std::map<std::uint64_t,ActorWorldObjectSnapshot> hosted_world_objects;
    // Physical +3d0: 1 from the physical constructor (001eb990 at 001ebfa0).
    std::uint8_t physical_3d0{1};
    std::uint8_t stun_immune_346a{};                                   // actor +346a (001b61e8 = 0)
    std::vector<std::uint64_t> object_action_receivers;
    // +34e4..+3510, the death-motion overrides: -1 (001b6198) until 0018c9d0.
    std::array<std::int32_t,12> death_motion_34e4{-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
    ActorWorldStackHost world_stack_host();
    void deliver_object_action(const SoloActorSimulation::ObjectAction& a);
    void deliver_object_code(std::uint64_t object,std::int32_t code);
    // Hotboxes (CROUCH_ZONES_AND_RUNNING_JUMP.md): the hosted boxes, the local
    // player's tracker and actor +31dd for the take-off.
    HotboxWorld hotbox_world;
    HotboxTracker tracker;
    SoloActorSimulation::HotboxScriptFilter hotbox_filter;
    SoloActorSimulation::TriggerScriptFilter trigger_filter;
    SoloActorSimulation::NativeTriggerReceiver native_trigger_receiver;
    SoloActorSimulation::ObjectCodeReceiver object_code_receiver;
    bool multiplayer_host{};                               // port-only: 005721c8 / 005721c0 (132ob3)
    bool port_ladder_arrival_at_end{};                     // port-only: the partner's ladder arrival (132ob5)
    SoloActorSimulation::AiNotificationScriptFilter ai_notification_filter;
    SoloActorSimulation::HotboxLinkSink link_sink;
    SoloActorSimulation::LinkMessageScriptFilter link_filter;
    SoloActorSimulation::ObjectExists object_exists;
    SoloActorSimulation::ObjectTrigger object_trigger;
    void deliver_link_message(bool enable);
    SoloActorSimulation::NotifyCheckpointScriptFilter notify_checkpoint_filter;
    SoloActorSimulation::OutOfAmmoScriptFilter out_of_ammo_filter;
    SoloActorSimulation::WeaponVisChangeScriptFilter weapon_vis_change_filter;
    std::uint8_t lead_foot_31dd{};
    // Footsteps (set_footstep_hooks): the gait's +351c and the floor surface
    // byte physical +141 (001e1b70 on each floor-probe hit; 0 until the first).
    SoloActorSimulation::FootstepHooks footsteps;
    float gait_351c{};
    float shimmy_3520{};                                  // 00197e00's shimmy phase (0019820c)
    std::uint8_t floor_surface_141{};
    void footstep_gait();
    void deliver_actor_hotbox(const HotboxActorEvent& e);
    bool interact();
    void request_running_jump(const ActorJumpZoneMessage& m);
    void deliver_running_jump(std::int32_t action,const RootVector& payload);
    void set_pending_bit(std::uint32_t bit,bool set);
    ActorJumpPhysical* live_jump{};                        // view held by a running jump/hang function
    // Ladders (LADDERS.md): physical +980 (its z lane is +988), set by the
    // 001930a0 ladder branch; the constructor value is (0, 0, 0, 1) (001ebfe8).
    RootVector ladder_980{0,0,0,1};
    float slide_31d8{};                                    // actor +31d8
    ActorLadderExitFields exit_fields;                     // actor +31b6, +3514, +3518
    void apply_component_code(std::int32_t code);
    void place_ladd_root(const RootParentTransform& t,bool reset);
    std::optional<ActorLadderPhysicalView> ladder_view();
    ActorLadderBodyHost ladder_body_host(const ActorAnimationHost& ah);
    void request_ladder_exit(std::int32_t action);
    void cancel_ladder();
    SoloActorTeleportResult teleport(ActorTeleportMode mode,const ActorTeleportMatrix& matrix);
    void deliver_ladder_exit(const ActorLadderExitMessage& m);
    // Runs f with jp as the live physical view (see receive_motion_effect).
    template<class F> void with_live(ActorJumpPhysical& jp,F&& f){
        if(live_jump)throw std::runtime_error("Solo actor: nested jump/hang physical views");
        live_jump=&jp;
        struct Reset{ActorJumpPhysical*& p;~Reset(){p=nullptr;}} reset{live_jump};
        f();
    }

    float root_weight(ActorRoot r) const{
        static constexpr RootAnchorKey keys[3]={SoloActorSimulation::flor_key,SoloActorSimulation::edge_key,SoloActorSimulation::ladd_key};
        const auto i=static_cast<std::size_t>(r);
        if(model)if(const auto w=model->anchor_weight(keys[i]))return *w;
        return weights[i];
    }
    PlayerButtonActorView button_actor() const;
    void send_pad_action(std::int32_t action,bool alternate);
    void detect_edges(ActorEdgePresetState& state);
    void deliver_edge_messages(const std::vector<ActorEdgeMessage>& messages);
    void receive_motion_effect(const MotionEffect& effect,const MotionDeliveryView& view);
    ActorHangMotionHost hang_host();
    ActorJumpPhysical jump_physical() const;
    void store_jump_physical(const ActorJumpPhysical&);
    void catch_ledge(ActorJumpPhysical&);
    void jump_to_grab(const RootParentTransform& edge);
    void deliver_queued();
    void store_checkpoint(const std::array<float,3>& position,float yaw,std::string label);
    void advance_respawn_poll();
    void update_air_state(float dt);
    void fall();
    void land();
    void land_reset();
    void landing_effect(std::uint8_t a1,bool a2);
    void timed_event(ActorTimedEvent::Kind kind);
    // Damage and death (ACTOR_DAMAGE_AND_HEALTH.md).
    ActorHealthState health;
    ActorCheckpoint checkpoint;
    ActorFader fader;
    GameEndState game_end;                                 // G 004ffb50: +50 state, +1c4..+1c7
    ActorRespawnPoll respawn_poll;
    ActorRespawnTimer respawn_timer;                       // 001ba130 event (vtable 004da970)
    std::uint8_t byte_3269{};                              // +3269: 001b1710 returns at once while set
    std::uint8_t pickupable_3468{};                        // +3468: SetPickupable (0014a890)
    std::uint8_t disguise_3469{};                          // +3469: AI +1d8 (0014ac90)
    void revive();
    void advance_respawn_timer();
    void extinguish();
    int player_deaths{};
    std::uint8_t byte_31b2{};                              // +31b2 (001977f0)
    std::int32_t word_2dd0{},word_2dd4{},word_2dec{};      // death action tail stores
    std::uint8_t life_state() const{return actor_life_state(health,actions.highest_action);}   // 001872f0
    void apply_fall_damage(std::int32_t type,float amount);
    ActorDamageResult damage(const ActorDamageInput& in,std::optional<RootVector> source_position,
        std::optional<std::pair<std::uint64_t,RootVector>> setter_position={});
    void death_action(std::int32_t code);
    ActorDeathHost death_host();
    void land_dead();
    void land_dead_with(bool unsupported_dc);
    void enter_dead_state();

    Vec3 source_normal(std::uint32_t ordinal) const{
        const auto t=world().refresh(batch.sources.at(ordinal));return Vec3{t.normal[0],t.normal[1],t.normal[2]};
    }
    void build_hosts();
    std::uint32_t mode() const{
        if(actions.highest_action<0)throw std::runtime_error("Solo actor: no priority action is active");
        return static_cast<std::uint32_t>(actions.highest_action);
    }
    SoloActorSimulation* owner{};
    // The body carry (actor_body_carry.hpp): the relation tree +2fc0 (001ad8c0
    // links a body under its carrier, 001a07f0 clears it), the roots bound
    // under another actor's root (001a6d40, kept in both actors) and the
    // world's actor resolver and backpack hooks (set_actor_peers).
    std::optional<std::uint64_t> relation_parent;
    std::vector<std::uint64_t> relation_children;
    struct ExternalParent {std::uint64_t actor{};RootAnchorKey key{};};
    std::map<std::size_t,ExternalParent> external_parents;            // own root -> the parent actor's root
    struct BoundChild {std::uint64_t actor{};std::size_t root{};RootAnchorKey key{};};
    std::vector<BoundChild> bound_children;                           // other actors' roots under own root key
    SoloActorSimulation::ActorResolver peers;
    const SoloActorSimulation::CarryHooks* carry_hooks{};
    Impl* find_peer(std::uint64_t h) const{
        if(h==handle)return const_cast<Impl*>(this);
        if(!peers)return nullptr;
        const auto a=peers(h);
        return a?a->impl_.get():nullptr;
    }
    Impl& peer(std::uint64_t h) const{
        if(auto* p=find_peer(h))return *p;
        throw std::runtime_error("Solo actor: the body carry reached an actor ("+std::to_string(h)+") the world does not resolve");
    }
    std::size_t root_of(const RootAnchorKey& k) const{
        if(k==SoloActorSimulation::flor_key)return floor_root;
        if(k==SoloActorSimulation::edge_key)return edge_root;
        if(k==SoloActorSimulation::ladd_key)return ladd_root;
        throw std::runtime_error("Solo actor: a root key outside 'flor', 'edge' and 'ladd'");
    }
    // 001adaf0 on +3478: the linked body while its +347c key still names a live object.
    std::optional<std::uint64_t> linked() const{
        if(!actions.linked_model.identity)return std::nullopt;
        if(!find_peer(actions.linked_model.identity))return std::nullopt;
        return actions.linked_model.identity;
    }
    void bind_external(std::size_t id,const RootAnchorKey& key,Impl& parent,const RootAnchorKey& parent_key,bool reset);
    void unbind_external(std::size_t id);
    void clear_root_children(const RootAnchorKey& key);
    void attach_children(const RootAnchorKey& key);
    void sync_external_parents();
    ActorBodyCarryHost carry_host(std::uint64_t body);
    void deliver_carry_pickup(std::uint64_t body,std::uint8_t kind);
    void hide_inventory_for_carry();
    void restore_inventory();
    void leave_carry();
    void logic(const SoloActorFrameInput& input);
    ActorAnimationSnapshot animation_snapshot(float dt);
    void carry_pair_body(float dt);
    void neutral_channels_1000(float dt);
    void physics(const SoloActorFrameInput& input);
    std::function<void(float)> secondary_update;
    PadCombatHooks pad_combat;
    std::function<void(const RootVector&)> choose_menu_track;
    // The pad end latch 0x0024faa8 of the D-pad action values sp+fb0..sp+fbc
    // (input +b0..+b3).
    std::array<bool,4> dpad_latch_b0{};
    SoloActorSimulation::PadInteractPrompt pad_interact_prompt;
    bool dpad_left_a5{};   // player input +a5: sp+f84 != 0, latched at the producer's end (0x0024faa8..0x0024fad4)
    std::function<void(float)> inventory_update;
    std::function<SecondaryModeInput()> death_secondary_input;
    std::function<void(std::uint8_t)> death_secondary_apply;
    std::function<SecondaryComponent*()> secondary_component;
    void deliver_flashbang_hide(std::int32_t action);
    void deliver_roll();
    void deliver_cancel();
    std::function<void(bool)> presence_hook;
    SoloActorSimulation::SoundSink sound_sink;
    SoloActorSimulation::VoiceSink voice_sink;
    SoloActorSimulation::PositionalSoundSink positional_sink;
    SoloActorSimulation::TimedEventSink timed_event_sink;
    // A string-only sound site (001ca210 bank, name; 0020f990 with this actor; transport 1 unless posted).
    void own_sound(std::int32_t bank,const std::string& name,bool immediate=true){
        if(!sound_sink)return;
        ActorAnimationSoundRequest q;q.bank=bank;q.actor=handle;q.actor_generation=1;
        if(name.size()>=q.key.size())throw std::runtime_error("Solo actor: a sound name of 16 or more characters");
        for(std::size_t i=0;i<name.size();++i)q.key[i]=static_cast<std::uint8_t>(name[i]);
        sound_sink(q,immediate);
    }
    std::function<void()> deactivation_hook;                         // 001b5190: 00185d40(actor, 0..2, 0, 1.0)
    std::function<std::uint8_t()> activation_hook;                   // 001b5560: +3314 (001d81a0), 001977f0(actor, 0)
    void reset_physical_component();                                 // 001eb990 / 001b5560 state of +3310
    std::function<void(float)> item_triggers,sense_decay;            // 001b1d10 (00197e00), 001b2328..001b237c
    std::function<void(float)> grip;                                 // 001b1c28..001b1cec
    SoloFireHooks fire;                                              // the burning actor (actor_burning.hpp)
    std::optional<ActorAnimationInventoryBindings> inventory_bindings;
    ActorAnimationReceiverHost inventory_host;                      // its inventory members (set_inventory_receiver)
    void frame_event();
    void navigation_body();
};

void SoloActorSimulation::Impl::build_hosts(){
    const float dt=config.dt;
    auto& p=frame_state.response.physical;
    movement.physical=&p;movement.component=&component;movement.byte_3d0=&physical_3d0;
    ActorPhysicalResponseHost host;
    host.setup.floor_root_parented=[this]{return roots.parented(floor_root);};
    host.setup.root_weight=[this](ActorRoot r){return root_weight(r);};
    // Component +38 is matrix row 2, column 2. With the component bound to the
    // root pose (no scale), that is 1 - 2(qx^2 + qy^2) of the pose orientation.
    host.setup.component_scale_z=[this]{const auto q=component_orientation();
        return 1.0F-2.0F*(q[0]*q[0]+q[1]*q[1]);};
    host.setup.model_anchor=[this]{return model_anchor_world();};
    host.setup.floor_root_world_z=[this]{return roots.world_placement(floor_root).position[2];};
    host.setup.adjust_movement=[this](const RootVector& endpoint){
        return schedule_actor_movement_adjustment(movement_world?*movement_world:movement,handle,config.dt,endpoint);};
    host.resolve.floor_root_pose=[this]{return roots.world_pose(floor_root);};
    host.resolve.floor_root_base_position=[this]{return roots.world_placement(floor_root).position;};
    host.resolve.anchor_local=[this]{return anchor_local;};
    host.resolve.floor_root_motion=[this]{return roots.world(floor_root).motion;};
    host.resolve.component_translation=[this]{return component;};
    host.resolve.set_component_position=[this](const RootVector& v){published=v;published_set=true;};
    // Ledge resolution inputs (1e3074..3628), read only for edge/ladd weights.
    host.resolve.root_weight=[this](ActorRoot r){return root_weight(r);};
    host.resolve.node_position=[this](int node){
        if(!model)throw std::runtime_error("Solo actor: skeleton node positions need an animation model");
        return model->skeleton_node_world_position(node);};
    host.resolve.edge_root_resolved=[this]{return roots.world_placement(edge_root);};
    host.resolve.edge_root_world=[this]{return roots.world_placement(edge_root);};
    host.resolve.edge_root_parented=[this]{return roots.parented(edge_root);};
    host.resolve.edge_root_base_position=[this]{return roots.world_placement(edge_root).position;};
    host.resolve.set_edge_root_base_position=[this](const RootVector& v){roots.set_world_position(edge_root,v);};
    host.collision.candidates=[this]{return &batch;};
    host.collision.geometry=[this](std::uint64_t h){return world().geometry(h);};
    host.collision.owner=[this](std::uint64_t h){return world().owner(h);};
    host.body_query=[this](const ActorBodyQueryInput& q,bool specialized){
        if(!specialized)return run_static_actor_body_query(make_actor_body_query(q),batch.candidates,
            [this](std::uint32_t ordinal){return source_normal(ordinal);});
        // Active-root center (001e2b08..2ed4): ladd (004a03d8) then edge (004a03d0).
        ActorSpecializedRootHost rh;
        rh.weight=[this](ActorSpecializedRoot r){return root_weight(r==ActorSpecializedRoot::first_4a03d8?ActorRoot::ladder:ActorRoot::edge);};
        rh.placement=[this](ActorSpecializedRoot r){return roots.world_placement(r==ActorSpecializedRoot::first_4a03d8?ladd_root:edge_root);};
        return run_static_actor_body_query(make_specialized_actor_body_query(q,rh),batch.candidates,
            [this](std::uint32_t ordinal){return source_normal(ordinal);});
    };
    host.contact_events=[this](const ActorBodyQueryResult& r){
        auto& phys=frame_state.response.physical;
        ActorContactEventHost ch;
        ch.contact_object=[&]{return ContactIdentity{batch.sources.at(r.world_contact->source_id).object};};
        // Terrain has no +54 component; neither has a lift: the base 003cf0e0 zeroes +54 and
        // the lift constructors 00215700 / 00219820 / 0021add0 do not set it (checkpoint 132el4).
        ch.component=[](ContactIdentity){return ContactIdentity{};};
        ch.eligible=[](ContactIdentity){return false;};
        ch.component_velocity=[](ContactIdentity){return RootVector{};};
        ch.refresh_world_root=[](const std::array<std::uint8_t,4>&){};
        ch.normal_z=[&]{return r.world_contact?r.world_contact->normal.z:0.0F;};
        ch.comparison_object=[this](std::size_t i){return ContactIdentity{frame_state.support_history.owners.at(i)};};   // +1f0+i*4
        ch.actor_velocity=[&]{return phys.velocity_310;};
        ch.normal=[&]{return r.world_contact?RootVector{r.world_contact->normal.x,r.world_contact->normal.y,r.world_contact->normal.z,0}:RootVector{};};
        ch.send=[](const ActorContactEvent&){throw std::runtime_error("Unexpected terrain contact event");};
        produce_actor_contact_event({r.world_contact.has_value(),handle,{'f','l','o','r'}},ch);
    };
    host.refresh_skeleton_nodes=[]{};
    host.floor_root_orientation=[this]{return roots.world_placement(floor_root).orientation;};
    // Early exit 001e4de8..4dfc: 00182f30(P+8 +50, 001831b0(P+8 +28)), the
    // same ordered product as 001e5880.
    host.anchor_world=[this]{return model_anchor_world();};
    host.placement=[this,dt](ActorResponsePlacementState& st,const ActorResponsePlacementInput& in){
        ActorResponsePlacementInput full=in;full.primary=floor_root;full.secondary=edge_root;
        apply_actor_response_placement(st,roots,full);
        apply_actor_response_orientation(roots,{edge_root,{},0,false,dt});
    };

    frame_host.response=host;
    frame_host.initialize=[this](ActorPhysicalFrameState& s){
        ActorPhysicalInitializationHost ih;
        ih.root=[this]{return floor_root;};
        ih.world_pose=[this](std::size_t r){return roots.world_pose(r);};
        ih.external_vector=[this]{return anchor_local;};
        ih.world_placement=[this](std::size_t r){return roots.world_placement(r);};
        initialize_actor_physical_frame(s,ih);
    };
    // 001dfb80: entry, detection body and 001e1600 with immediate delivery.
    frame_host.edge_presets=[this](ActorPhysicalFrameState& s){
        ActorEdgePresetHost eh;
        eh.detect=[this](ActorEdgePresetState& state){detect_edges(state);};
        eh.report_requests=[this](ActorEdgePresetState& state){deliver_edge_messages(select_actor_edge_request(state.slots));};
        run_actor_edge_presets(s.edges,s.history.current.value,s.history.current.bits,eh);};
    // 001e7110 / 001e6300 over the support slots +1f0..+2c2 (checkpoint 132el4).
    // A slot object is a COL owner object (the contact +164, or the object the
    // ledge / ladder install binds). Its +50 transform component is the matrix
    // the gathers transform its geometry by (00183220): the owner matrix of an
    // owner-matrix entry (placed GOBs, lifts, pathed objects, NIE models), kept
    // current by the object updates that run before the actors (00133420).
    // Terrain has no +50 component, so both routines skip it. The component's
    // dirty byte (+50, vt+58 00132900 only clears it) never changes the matrix
    // here: the owner matrix is written whole by the object's pose.
    const auto support_root=[this](std::size_t slot){
        return slot==0?floor_root:slot==1?edge_root:ladd_root;};   // keys 004a03c8 / 004a03d0 / 004a03d8
    const auto support_key=[this](std::size_t id){
        if(id==floor_root)return SoloActorSimulation::flor_key;
        if(id==edge_root)return SoloActorSimulation::edge_key;
        if(id==ladd_root)return SoloActorSimulation::ladd_key;
        throw std::runtime_error("Solo actor: 001e7110 published a root outside 'flor', 'edge' and 'ladd'");
    };
    auto& sm=frame_host.support_motion;
    sm.primary_enabled=[this]{return frame_state.response.physical.e8!=0;};
    sm.object=[this](std::size_t i){return ActorSupportIdentity{frame_state.support_history.owners.at(i)};};
    sm.owner=[this](ActorSupportIdentity o)->ActorSupportIdentity{return world().owner(o)?o:ActorSupportIdentity{};};
    sm.local=[this](std::size_t i){return frame_state.support_history.local.at(i);};
    sm.previous_owner=[this](std::size_t i){return frame_state.support_history.previous_owner.at(i);};
    sm.owner_dirty=[](ActorSupportIdentity){return false;};
    sm.refresh_owner=[](ActorSupportIdentity){};
    sm.owner_matrix=[this](ActorSupportIdentity o){
        const auto m=world().owner(o);
        if(!m)throw std::runtime_error("Solo actor: 001e7110 read the +50 matrix of an object without one");
        return std::array<float,16>(*m);};
    // Root identities are the root ids + 1 (0 is the unresolved root).
    sm.root=[this](const std::array<std::uint8_t,4>& k){return ActorSupportIdentity{root_of(RootAnchorKey{k})+1};};
    sm.root_world=[this](ActorSupportIdentity r){return roots.world_placement(static_cast<std::size_t>(r-1));};
    sm.root_parented=[this](ActorSupportIdentity r){return roots.parented(static_cast<std::size_t>(r-1));};
    // 001e7110's setters reset the root's normalization counter; the model's
    // anchor mirrors the root as the other root writers do.
    sm.set_orientation_and_reset=[this,support_key](ActorSupportIdentity r,const RootVector& q){
        const auto id=static_cast<std::size_t>(r-1);const auto key=support_key(id);
        if(model&&model->has_anchor(key))model->set_anchor_world_orientation(key,q,true);
        roots.set_world_orientation(id,q,true);
    };
    sm.set_placement_and_reset=[this,support_key](ActorSupportIdentity r,const RootParentTransform& t){
        const auto id=static_cast<std::size_t>(r-1);const auto key=support_key(id);
        roots.set_world_placement(id,t,true);
        if(model&&model->has_anchor(key))model->set_anchor_world_placement(key,t,true);
    };
    auto& sh=frame_host.support_history;
    sh.primary_enabled=[this]{return frame_state.response.physical.e8!=0;};
    sh.contact_owner=[this]{return ActorSupportIdentity{frame_state.floor_contact_object_164};};
    sh.component=[this](ActorSupportIdentity o)->ActorSupportHistoryComponent*{
        const auto m=world().owner(o);
        if(!m)return nullptr;
        auto& c=support_components[o];c.matrix=*m;c.dirty=false;
        return &c;};
    // Root +d0 is the accumulated weight; the placement is read through 001910a0.
    sh.root=[this,support_root](std::size_t slot)->ActorSupportHistoryRoot*{
        auto& r=support_roots.at(slot);
        r.weight=root_weight(static_cast<ActorRoot>(slot));
        r.placement=roots.world_placement(support_root(slot));
        return &r;};
    sh.world_placement=[this,support_root](ActorSupportHistoryRoot& r,ActorSupportHistory&){
        const auto slot=static_cast<std::size_t>(&r-support_roots.data());
        r.placement=roots.world_placement(support_root(slot));
        return r.placement;};
    frame_host.component_translation=[this]{return component;};
    auto& ax=frame_host.auxiliary;
    ax.tuning=[]{return 5.0F;}; // 004a0358
    ax.candidates=[this]{return std::optional<std::span<FaceCandidate>>{std::span<FaceCandidate>{batch.candidates}};};
    ax.root_world=[this]{return roots.world_placement(edge_root);};
    ax.source_face_normal=[this](const FaceContact& c){return source_normal(c.source_id);};
    // 001e5cf0 with the native water-effect manager. The actor is built with
    // the class word 0049d010 (001b6ab0 passes it to 00393760) and always has
    // its +58 component, whose virtual +58 / +54 answer 0 / 1 for the player
    // and 1 / 0 for the AI (0015b7e0, 0012f370; see the damage controller):
    // every actor publishes +346d (001e5e18..001e5ea8), the only reader of
    // which is the fire effect's fast decay (002360c4).
    // Checkpoint 132cp11: an actor of an ActorWorld uses the world's manager (world +e4); a standalone one builds its own.
    if(!water){
        own_water=std::make_unique<WaterEffectManager>(*rng);
        own_water_actors.component=&component;
        water=own_water.get();water_actors=&own_water_actors;water_sink=&own_water_sink;
    }
    water_host.class_key_matches=[]{return true;};
    water_host.controller_58=[this]{return std::optional<bool>{!player()};};
    water_host.controller_54=[this]{return std::optional<bool>{player()};};
    water_host.set_in_water_346d=[this](bool v){in_water_346d=v;};
    frame_host.water_support=[this](float previous,float height,float normal_z){
        const auto r=notify_actor_water_support(previous,height,actor_unavailable_height,normal_z,
            static_cast<std::uint32_t>(handle),water_host,*water,*water_actors,*water_sink);   // actor handles are 0x100000 + index
        if(r.effect!=ActorWaterSupportResult::Effect::none)++water_events;
    };
}

PlayerButtonActorView SoloActorSimulation::Impl::button_actor() const{
    PlayerButtonActorView v;
    v.mode_320c=static_cast<std::int32_t>(mode());
    v.crouched_3204_bit4=((words.current_3204>>4)&1U)!=0;
    v.physical_present=true;
    v.physical_3d2=frame_state.auxiliary.first_hit!=0;
    v.physical_dc=frame_state.response.physical.byte_dc!=0;
    v.life_state=life_state();   // virtual +108
    v.human=true;     // +4c: the player is built by 001b6ab0 with the class word 0049d010
    return v;
}

// 001a92a0 actions 0x40 (0018d1a0) and 0x41 (0018d140) on +3314 and +324c.
void SoloActorSimulation::Impl::deliver_flashbang_hide(std::int32_t action){
    if(!secondary_component)
        throw std::runtime_error("Solo actor: actor action "+std::to_string(action)+" needs the secondary component +3314 (set_secondary_component)");
    if(current)current->pad_actions.push_back(action);
    auto* c=secondary_component();
    if(action==0x40)(void)handle_actor_flashbang_hide(actions.flags_324c,static_cast<std::int32_t>(mode()),c);
    else (void)handle_actor_flashbang_hide_end(actions.flags_324c,c);
}

// 001a92a0 action 8 (001aa468): the directional roll. The registrations
// +3270..+3284 are the named roots 001ae7d0 saves from 0049e7b0 + 0x10 * (class - 1)
// (the model's root map, 001badd0); +326c is the retained CoreMotions root.
void SoloActorSimulation::Impl::deliver_roll(){
    if(current)current->pad_actions.push_back(8);
    static constexpr std::array<const char*,7> roll_roots{nullptr,"RunRollForw","RunRollBackLeft","RunRollBackRght",
        "RunRollLeft","RunRollRght","RollBack"};
    ActorRollInput in;
    in.mode_320c=static_cast<std::int32_t>(mode());
    in.top_3234=action_history.older_3234.top;in.top_3220=action_history.newer_3220.top;
    in.flags_3204=words.current_3204;
    in.physical=true;in.byte_3d2=frame_state.auxiliary.first_hit;                // +3310 present, +3d2
    in.accumulation_309c=accumulation;
    in.receiver=handle;
    ActorRollHost host;
    host.registration=[this](std::uint32_t c){
        if(c==0)return model->retained_motion();
        if(c>=roll_roots.size())throw std::runtime_error("Solo actor: roll class outside 1..6");
        return model->resolve_named_root(roll_roots[c]);
    };
    host.install=[this](const MotionRequest& r,MotionInstallMode m){model->install_motion(r,m);};
    host.channel_15=[this]{return model->channel(15).current;};
    const auto r=handle_actor_roll(in,transition_2ef8,host);
    if(current&&r.roll_installed)current->ledge_motions.push_back(roll_roots[r.roll_class]);
    if(current&&r.core_installed)current->ledge_motions.push_back("retained");
}

// 001a92a0 action 0x29 (001ad1d0): the object-action cancel replaces the motion
// with the retained CoreMotions root +326c through the override (0.5 s).
void SoloActorSimulation::Impl::deliver_cancel(){
    ActorCancelHost host;
    host.retained=[this]{return model->retained_motion();};
    host.install=[this](const MotionRequest& r,MotionInstallMode m){model->install_motion(r,m);};
    const auto r=handle_actor_cancel(life_state(),transition_2ef8,host);
    if(current&&r.installed)current->ledge_motions.push_back("retained");
}

// 003c9420 -> 001a92a0 for the pad actions of the button senders.
void SoloActorSimulation::Impl::send_pad_action(std::int32_t action,bool alternate){
    if(current)current->pad_actions.push_back(action);
    if(alternate){                                                     // 0024a2c0: action 0x28, kind 1, transport 1
        if(action!=actor_cancel_request_action)throw std::runtime_error("Solo actor: an alternate pad message other than 0x28");
        if(actor_cancel_request_admitted(static_cast<std::int32_t>(mode()),actions.flags_324c,ActorCancelRequest{}))
            queue_action(actor_cancel_action);                         // 001ad0e8: 0x29, transport 0
        return;
    }
    const auto& p=frame_state.response.physical;
    ActorPadActionState s;
    s.flags_3248=actions.flags_3248;s.flags_3210=actions.active_actions;s.flags_324c=actions.flags_324c;
    s.mode_320c=static_cast<std::int32_t>(mode());s.byte_14=true;      // actor +14 (003cb0e0; LADDERS.md)
    s.component_is_player=player();                                    // +58 virtual +54: 1 player, 0 AI (0012f370)
    if(actions.attachment_byte_a0)s.attachment_a0=*actions.attachment_byte_a0!=0;   // +3524 byte +a0 (0 from 00144f08)
    s.byte_31b1=byte_31b1;s.byte_31b5=byte_31b5;s.life_state=life_state();s.linked_model_3478=linked().has_value();
    s.physical_present=true;s.physical_dd=p.byte_dd!=0;s.physical_dc=p.byte_dc!=0;
    s.physical_9a5=frame_state.edges.ladder.flags_99c[9]!=0;
    s.slots=&frame_state.edges.slots;
    if(action==0x2c||action==0x2d){                                    // 001ac360: the carried body's drop
        const auto body=linked();
        ActorCarryDropInput in;in.action=action;in.life_state=life_state();in.byte_14=true;
        in.physical_dc=p.byte_dc!=0;in.linked=body.has_value();
        if(body)in.body_flags_324c=peer(*body).actions.flags_324c;
        in.edge_root_weight=root_weight(ActorRoot::edge);
        const auto r=handle_actor_carry_drop(transition_2ef8,actions.flags_324c,handle,in,carry_host(body.value_or(0)));
        if(current)current->carry_motions.insert(current->carry_motions.end(),r.motions.begin(),r.motions.end());
        return;
    }
    if(!handle_actor_pad_action(s,action))
        throw std::runtime_error("Solo actor: pad action "+std::to_string(action)+" is not hosted");
    actions.flags_3248=s.flags_3248;byte_31b5=s.byte_31b5;
}

// 001dfb80 body: the live inputs read at 1dffbc..1e0070.
void SoloActorSimulation::Impl::detect_edges(ActorEdgePresetState& state){
    const auto& p=frame_state.response.physical;
    ActorEdgeDetectionInput in;
    in.mode_20=frame_state.history.current.value;in.bits_24=frame_state.history.current.bits;
    in.unsupported_dc=p.byte_dc!=0;in.byte_de=p.airborne_de!=0;in.value_394=p.height_394;
    in.velocity_310=p.velocity_310;                               // after prediction
    in.floor_root=roots.world_placement(floor_root).position;     // 003ae5e0 -> 001910a0 ('flor')
    // (*(P+8))->+28 component, row +20 of its matrix at +10: the image of the
    // local Y axis: the published placement, or the floor root pose.
    const auto m=root_placement_matrix(published_component?*published_component:roots.world_pose(floor_root));
    in.forward={m[4],m[5],m[6],m[7]};
    // Ladder path and mode-9 tail: 'edge'/'ladd' root weights and resolved
    // world placements, and the retained +994.
    in.edge_weight=root_weight(ActorRoot::edge);in.ladd_weight=root_weight(ActorRoot::ladder);
    in.edge_world=roots.world_placement(edge_root);in.ladd_world=roots.world_placement(ladd_root);
    in.band3_994=state.ladder.band3_994;
    auto r=detect_actor_edges(in,state.slots,batch,
        [this](std::uint64_t h){return world().geometry(h);},[this](std::uint64_t o){return world().owner(o);});
    state.slots=r.slots;state.ladder=r.ladder;
    if(r.edge_direction_a10)state.direction_a10=*r.edge_direction_a10;
    if(current)current->edge_detection=true;
}

// 001e1600 sends each message immediately (003c9420).
void SoloActorSimulation::Impl::deliver_edge_messages(const std::vector<ActorEdgeMessage>& messages){
    for(const auto& m:messages){
        switch(m.kind){
        case ActorEdgeMessage::Kind::cas_event:
            // 001e1730..001e1774: class 0055a1d8 code 8, sent at once
            // (003c9420, transport 1) to the +58 component. The player
            // component 00176660 accepts it without effect (table 0049cb10
            // entry 8); an AI component's 00151d30 -> 00151a90 repeats the
            // path-link action (+114 4 -> 0x0f), so a failed climb request
            // is retried on the next frame.
            if(current)++current->edge_events;
            if(!player())controller.ai->controller_message_immediate(*owner,8);
            break;
        case ActorEdgeMessage::Kind::found_ledge:
            found_ledge_130=m.mask;                                // 001777e8: component +0x180
            break;
        case ActorEdgeMessage::Kind::cas_action: {
            if(current){current->ledge_slot=m.slot;current->ledge_mask=m.mask;}
            // 001959c0: an owner with a transform component stores the grab
            // placement relative to it (inverse owner * world); otherwise the
            // world placement is kept.
            ActorLedgeActionMessage msg;msg.kind=m.action_kind;msg.mask=m.mask;
            if(m.object)msg.object=m.object;
            msg.placement={m.orientation,m.point};
            if(m.object)if(const auto o=world().owner(m.object))msg.placement=actor_owner_local_placement(msg.placement,*o);
            ActorLedgeActionHost h;
            h.owner_matrix=[this](std::uint64_t o){return world().owner(o);};
            h.place_edge_root=[this](const RootParentTransform& t){
                roots.set_world_placement(edge_root,t,true);
                if(model&&model->has_anchor(SoloActorSimulation::edge_key))model->set_anchor_world_placement(SoloActorSimulation::edge_key,t,true);
            };
            h.bind_support=[this](std::size_t i,const ActorLedgeSupportBinding& b){bind_support(i,b);};
            h.floor_root_world_position=[this]{return roots.world_placement(floor_root).position;};
            h.edge_root_world_position=[this]{return roots.world_placement(edge_root).position;};
            h.resolve=[this](std::string_view n){
                if(!model)throw std::runtime_error("Solo actor: ledge motions need an animation model");
                return model->resolve_motion(n);};
            h.retained=[this]{
                if(!model)throw std::runtime_error("Solo actor: ledge motions need an animation model");
                return model->retained_motion();};
            h.install=[this](const MotionRequest& r,MotionInstallMode mode){model->install_motion(r,mode);};
            h.edge_root_world_placement=[this]{return roots.world_placement(edge_root);};   // 001910a0
            h.place_ladd_root=[this](const RootParentTransform& t){place_ladd_root(t,true);};
            h.set_ladder_reference=[this](const RootVector& v){ladder_980=v;};
            h.jump_to_grab=[this](const RootParentTransform& t){jump_to_grab(t);};
            h.catch_ledge=[this]{auto jp=jump_physical();with_live(jp,[&]{catch_ledge(jp);});store_jump_physical(jp);};
            const auto out=handle_actor_ledge_action(actions,&frame_state.history.pending,transition_2ef8,
                handle,0,root_weight(ActorRoot::edge),msg,h);
            if(current)current->ledge_motions.insert(current->ledge_motions.end(),out.motions.begin(),out.motions.end());
            break;}
        }
    }
}

// Immediate animation effects for this actor: the embedded completion message
// of the quick turn (001a7f40 code 0) and the climb tokens (001a7f40 codes),
// with the physical fields the receiver touches mirrored from the frame state.
void SoloActorSimulation::Impl::receive_motion_effect(const MotionEffect& effect,const MotionDeliveryView& view){
    if(effect.receiver!=handle)
        throw std::runtime_error("Solo actor: animation effect for an unhosted receiver");
    ActorAnimationReceiverConfig receiver_config;receiver_config.receiver=handle;
    receiver_config.roots=ActorAnimationRootBindings{SoloActorSimulation::flor_key,SoloActorSimulation::edge_key};
    receiver_config.physical_restore_374=std::bit_cast<float>(0x3fdeb852U);   // 004a03e0 (1.74)
    // Token 0x42 (001a4a1c): the end placement of "buddyclimb_firstguyup" (0049dd50).
    receiver_config.climb_finish_motion="buddyclimb_firstguyup";
    // Actor +58 is the player component: token 0x14 queues code 6 to it.
    receiver_config.controller_receiver=controller_handle;
    // Token 0x0b: sound "M_GRUNT" (0049e1e0), bank 1, sent immediately to
    // the sound manager 004ffb50 (001a3d40). The solo actor's generation is 1,
    // as for the water-effect host; the request is recorded, not played.
    receiver_config.actor_generation=1;
    const auto sound_key=[](const char* name){
        std::array<std::uint8_t,16> key{};
        for(std::size_t i=0;name[i]&&i<16;++i)key[i]=static_cast<std::uint8_t>(name[i]);
        return key;
    };
    receiver_config.sound_keys[0x0b]=sound_key("M_GRUNT");
    // Token 0x73 (001a6228..001a6268): the same path with "M_BODY_PICKUP" (0049e1f8), bank 1.
    receiver_config.sound_keys[0x73]=sound_key("M_BODY_PICKUP");
    // Ladder tokens: 0x53/0x58/0x5c through 00188f00 ("F_LADDER_M", 0049d900),
    // 0x56 start and 0x57/0x59 stop "O_SLIDE_LADDER" (0049e1e8); bank 0.
    for(std::int32_t t:{0x53,0x58,0x5c})receiver_config.sound_keys[t]=sound_key("F_LADDER_M");
    for(std::int32_t t:{0x56,0x57,0x59})receiver_config.sound_keys[t]=sound_key("O_SLIDE_LADDER");
    // Token 3 (001a3a88, the roll's contact hook): physical +e4 against the
    // sentinel word 004a0438 (-FLT_MAX, written by the response setup in 001e2180);
    // equal: "M_ROLL" (0049e1c8), or "F_SNEAK" (0049d920) with +3248 bit 2,
    // bank 1; otherwise 001e96a0's 'flor' (004a03c8) root query, then
    // "F_WATER_SPLASH2" (0049e1d0), bank 0. Queued at transport 0 to the
    // sound manager 004ffb50 (003c9420 a2 = 0).
    receiver_config.contact_height_sentinel=std::bit_cast<float>(0xff7fffffU);
    receiver_config.contact_root=SoloActorSimulation::flor_key;
    receiver_config.contact_sound_keys=std::array<std::array<std::uint8_t,16>,3>{
        sound_key("M_ROLL"),sound_key("F_SNEAK"),sound_key("F_WATER_SPLASH2")};
    ActorAnimationReceiverHost receiver_host;
    receiver_host.enqueue_sound=[this](ActorAnimationSoundRequest q,const ActorAnimationReceiverState&,const MotionDeliveryView&){
        std::string name;for(auto c:q.key){if(!c)break;name.push_back(static_cast<char>(c));}
        if(current)current->sounds.push_back("queued "+name);
        if(sound_sink)sound_sink(q,false);
    };
    // 001e96a0: 003ae5e0(model +b0, 'flor' 004a03c8), 00132400, 00132370:
    // the 'flor' root's world position (as for the water height below).
    receiver_host.physical_root_position=[this](std::uint64_t identity,RootAnchorKey key,ActorAnimationReceiverState&,const MotionDeliveryView&)->RootVector{
        if(identity!=reinterpret_cast<std::uint64_t>(model)||key!=SoloActorSimulation::flor_key)
            throw std::runtime_error("Solo actor: the contact model-root query 001e96a0 for another model or a root other than 'flor'");
        return roots.world_placement(floor_root).position;};
    // The solo world objects have no inventory effect (world_objects).
    receiver_config.world_pop_keeps_inventory=true;
    receiver_host.motion_end_placement=[this](std::string_view name){
        if(!model)throw std::runtime_error("Solo actor: token 0x42 needs an animation model");
        return model->motion_end_placement(name);};
    receiver_host.pop_world_object=[this](ActorAnimationReceiverState&,const MotionDeliveryView&){
        pop_actor_world_object(world_objects,world_stack_host());};
    // Tokens 0x96 / 0x97: 00131510(G), the pending removal of G +d4.
    receiver_host.finish_world_pending=[this](ActorAnimationReceiverState&,const MotionDeliveryView&){
        finish_actor_world_pending(world_objects,world_stack_host());};
    receiver_host.enqueue_controller=[this](ActorAnimationControllerRequest q,const ActorAnimationReceiverState&,const MotionDeliveryView&){
        // Tokens 0x67/0x68/0x6a to an object action's object (+3474): the
        // CAS code (transport 0) its native receiver gets at the next pump.
        if(q.receiver!=controller_handle&&
           std::find(object_action_receivers.begin(),object_action_receivers.end(),q.receiver)!=object_action_receivers.end()){
            QueuedMessage m;m.kind=QueuedMessage::Kind::object_code;m.object=q.receiver;m.code=q.code;
            queue.push_back(m);
            if(current)current->object_codes.push_back({q.receiver,q.code});
            return;
        }
        if(q.receiver!=controller_handle)throw std::runtime_error("Solo actor: controller request for an unhosted receiver");
        if(current)current->component_codes.push_back(q.code);
        // An AI component queues it for its own pump (transport 0).
        if(!player()){controller.ai->controller_message(*owner,q.code);return;}
        queue_component(q.code);
    };
    // Transport 1 with event byte bit 0 (the 003c9e30 default 5): 003c9420
    // calls the player component at once (token 0x46 code 10, 0x48/0x4a code 11).
    receiver_host.controller_immediate=[this](const ActorAnimationControllerRequest& q,ActorAnimationReceiverState&,const MotionDeliveryView&){
        if(q.receiver!=controller_handle)throw std::runtime_error("Solo actor: controller message for an unhosted receiver");
        if(current)current->component_codes.push_back(q.code);
        // An AI component's virtual +0c is 00151d30 (00151a90 for the
        // controller key: codes 10 / 11 fall to its default, returning 0).
        if(!player()){controller.ai->controller_message_immediate(*owner,q.code);return;}
        apply_component_code(q.code);
    };
    // 001a3980 asks the +58 component's virtual +54 (00175330: 1).
    receiver_host.controller_is_player=[this](std::uint64_t r){
        if(r!=controller_handle)throw std::runtime_error("Solo actor: player-role query for an unhosted receiver");
        return player();};
    receiver_host.sound_immediate=[this](const ActorAnimationSoundRequest& q,ActorAnimationReceiverState&,const MotionDeliveryView&){
        std::string name;for(auto c:q.key){if(!c)break;name.push_back(static_cast<char>(c));}
        if(q.kind==ActorAnimationSoundRequest::Kind::stop)name="stop "+name;
        if(current)current->sounds.push_back(name);
        if(sound_sink)sound_sink(q,true);
    };
    // Token 0x92/0x93 (001a6b48: a call of 001a3780, the dead state). The
    // player branch: no 001a3520; +31c0 = the zero vector 004e1d90 (guarded
    // initialization 001a3848..3878, no other writer), +31d0/+31d4/+31b7
    // cleared, CAS code 16 queued to the player component.
    receiver_host.reset_history_template=[]{return std::array<std::uint32_t,4>{};};
    // 001a3520 (the receiver calls it only when +58 is not the player). The
    // receiver works on a copy of the actor's state: the inventory view the
    // drop publishes (set_inventory_view) is carried into that copy.
    receiver_host.create_world_inventory=[this](ActorAnimationReceiverState& s,const MotionDeliveryView&){
        if(player())throw std::runtime_error("Solo actor: the dead-state drop 001a3520 without an AI controller");
        controller.ai->dead_state_drop(*owner);
        s.inventory=actions.inventory;s.attachment_byte_a0=actions.attachment_byte_a0;};
    receiver_host.cancel_inventory_component=[](std::uint64_t,ActorAnimationReceiverState&,const MotionDeliveryView&){
        throw std::runtime_error("Solo actor: a held item's component teardown (00263080) is not hosted");};
    // The body carry's tokens: 0x75 / 0x76 / 0x7f / 0x80 run 0019fd60 on
    // 'edge' (0049d048), 0x7d on 'flor' (0049d040); 0x77 binds the relation
    // children's 'flor' under the own (001a6d40 with reset); 0x72 sets
    // channel 14 of the linked body's model through 001a6b48's live check.
    receiver_config.detach_roots={{0x75,SoloActorSimulation::edge_key},{0x76,SoloActorSimulation::edge_key},
        {0x7d,SoloActorSimulation::flor_key},{0x7f,SoloActorSimulation::edge_key},{0x80,SoloActorSimulation::edge_key}};
    receiver_config.related_root=SoloActorSimulation::flor_key;
    receiver_host.clear_root_children=[this](RootAnchorKey key,const ActorAnimationReceiverState&,const MotionDeliveryView&){
        clear_root_children(key);};
    receiver_host.attach_related_roots=[this](RootAnchorKey key,const ActorAnimationReceiverState&,const MotionDeliveryView&){
        attach_children(key);};
    receiver_host.object_generation=[this](std::uint64_t id)->std::optional<std::int32_t>{
        if(find_peer(id))return 1;
        return std::nullopt;};
    receiver_host.set_other_model_channel=[this](std::uint64_t id,std::size_t index,float value){
        auto& b=peer(id);
        if(!b.model)throw std::runtime_error("Solo actor: the linked body's channel write needs its animation model");
        const ActorChannelCommand c{ActorChannelOperation::set,index,value};
        b.model->apply_commands(std::span<const ActorChannelCommand>(&c,1));
    };
    // The inventory hosting of a combat host (set_inventory_receiver): the
    // release 00141cd0, the item tokens and their launch dependency.
    if(inventory_bindings){
        receiver_config.inventory=*inventory_bindings;
        const auto& ih=inventory_host;
        receiver_host.inventory_owner=ih.inventory_owner;receiver_host.model_slot_count=ih.model_slot_count;
        receiver_host.model_slot_present=ih.model_slot_present;receiver_host.swap_model_slots=ih.swap_model_slots;
        receiver_host.set_model_slot=ih.set_model_slot;receiver_host.enqueue_inventory_empty=ih.enqueue_inventory_empty;
        receiver_host.enqueue_inventory_selection_sound=ih.enqueue_inventory_selection_sound;
        receiver_host.launch_inventory_projectile=ih.launch_inventory_projectile;receiver_host.null_target_key=ih.null_target_key;
        // The takedown tokens 0x98 / 0x99 (Checkpoint 132ms4): 0040c540, 00236f20, 00133ed0 with the node
        // byte *0049d023 (9), 00232180 and the queued presets.
        receiver_host.resolve_named_target=ih.resolve_named_target;receiver_host.effect_descriptor=ih.effect_descriptor;
        receiver_host.target_transform=ih.target_transform;receiver_host.emit_directional_effect=ih.emit_directional_effect;
        receiver_host.sound_preset=ih.sound_preset;
        receiver_config.target_transform_selector=static_cast<std::int8_t>(9);
    }
    // With action 0x1f already set (the revive's step 5) it only clears 0x1b.
    if(effect.kind==MotionEffect::Kind::token&&(effect.id==0x92||effect.id==0x93)&&current&&
       !(actions.active_actions&0x80000000U))current->dead_state=true;
    // The receiver's view of the retained control fields +31c0/+31d0/+31d4/+31b7.
    for(std::size_t i=0;i<4;++i)actions.history_31c0[i]=std::bit_cast<std::uint32_t>(controls.desired[i]);
    actions.history_31d0=std::bit_cast<std::uint32_t>(controls.speed);actions.history_31d4=std::bit_cast<std::uint32_t>(controls.turn);
    actions.byte_31b7=controls.turn_pose_31b7?1:0;
    auto& h=frame_state.history;auto& p=frame_state.response.physical;
    // While a jump/hang function holds its view of the physical fields, the
    // tokens it triggers (synchronous cancel notifications of its installs)
    // read and write that view, so the original order of writes is kept.
    auto& pending=live_jump?live_jump->pending:h.pending;
    auto& keep_9c0=live_jump?live_jump->keep_contact_9c0:p.keep_contact_9c0;
    auto& radius_374=live_jump?live_jump->radius_374:h.target_radius;
    ActorAnimationPhysicalState ph;
    ph.flags_38=h.pending_flags;ph.byte_9c0=keep_9c0;ph.value_374=radius_374;
    ph.modes_2c=pending.bits;ph.highest_mode_28=pending.value;ph.byte_14=p.shape_14;
    ph.byte_3d0=physical_3d0;ph.byte_3d3=frame_state.auxiliary.second_requested;ph.byte_dd=p.byte_dd;ph.value_e4=p.e4;
    ph.model=reinterpret_cast<std::uint64_t>(model);                         // physical +c: the actor's model
    actions.physical=ph;
    struct Reset{std::optional<ActorAnimationPhysicalState>& s;~Reset(){s.reset();}} reset{actions.physical};
    receive_actor_animation(actions,receiver_config,effect,view,receiver_host);
    const auto& out=*actions.physical;
    physical_3d0=out.byte_3d0;
    if(out.byte_dd!=ph.byte_dd||out.value_e4!=ph.value_e4)throw std::runtime_error("Solo actor: receiver wrote unhosted physical fields");
    h.pending_flags=out.flags_38;keep_9c0=out.byte_9c0;radius_374=out.value_374;
    pending.bits=out.modes_2c;pending.value=out.highest_mode_28;p.shape_14=out.byte_14;
    frame_state.auxiliary.second_requested=out.byte_3d3;
    for(std::size_t i=0;i<4;++i)controls.desired[i]=std::bit_cast<float>(actions.history_31c0[i]);
    controls.speed=std::bit_cast<float>(actions.history_31d0);controls.turn=std::bit_cast<float>(actions.history_31d4);
    controls.turn_pose_31b7=actions.byte_31b7!=0;
}

// Virtual +100 as 0019f450 calls it: (actor, actor, 1, type, 0, 0, 0, amount).
// The player component answers +54/+58/+5c = 1/0/0 and returns the death
// code unchanged from +84 (0016fa10); an AI controller answers 0/1/0
// (0012f370, 0015b7e0, 001535d0) and its +78/+7c/+84 run as for any other
// damage, with the actor itself as the source (0019f450 passes a0 twice).
void SoloActorSimulation::Impl::apply_fall_damage(std::int32_t type,float amount){
    ActorDamageInput in;in.actor=handle;
    in.controller=player()?ActorDamageController{true,false,false}:ActorDamageController{false,true,false};
    in.byte_14=player()||actions.byte_14!=0;in.source={handle,in.controller};
    in.category=1;in.type=static_cast<std::uint8_t>(type);in.amount=amount;
    damage(in,std::nullopt);
}

// 00187360 on this actor with the controller's answers: the player component
// (+78 0016f9f0 nothing, +7c 00176540, +84 0016fa10) or the AI controller's
// virtuals. Events are appended to the frame being produced.
ActorDamageResult SoloActorSimulation::Impl::damage(const ActorDamageInput& in,std::optional<RootVector> source_position,
    std::optional<std::pair<std::uint64_t,RootVector>> setter_position){
    ActorDamageHost dh;
    dh.random=[this]{return next_game_random(*rng);};             // 00127dd0, the shared game RNG
    dh.top_action=[this]{return actions.highest_action;};
    dh.position_e4=[this,source_position,setter_position,source=in.source.id](std::uint64_t object)->RootVector{
        if(object==handle)return position_e4();
        if(object==source&&source_position)return *source_position;
        if(setter_position&&object==setter_position->first)return setter_position->second;   // the type-4 source of 001873bc
        if(!source_position)throw std::runtime_error("Solo actor: the damage source's position (+e4) is not known");
        return *source_position;};
    // Type 2 (001874e8): the victim's virtual +c0(actor, 0) (node 14 of a
    // present actor, else the published position, as ActorWorldCombat::eye_c0)
    // and the LOS Request (class 005435a0, vtable 004d9180: +30 the origin, +40
    // the whole displacement, +50 the source, +54 the victim, +58 0x10, +5c 0,
    // +3c 1.0, +4c 0) answered at once by 0013a480 over the static world; the
    // reply byte (result +0) is the hit.
    dh.position_c0=[this]()->RootVector{
        return owner->eligible()?owner->skeleton_node_world_position(14):owner->published_position();};
    dh.los_blocked=[this,source=in.source.id](const RootVector& from,const RootVector& direction){
        LosServiceRequest q;q.origin_30=from;q.displacement_40=direction;q.object_50=source;q.object_54=handle;
        q.mask_58=0x10;q.mode_5c=0;q.value_3c=1.0F;q.value_4c=0.0F;
        return answer_los_request(world(),q).hit;};
    // 00186950 (hit_direction.hpp) over this actor's virtual +f4 (the 'flor'
    // placement) and +c0(-1) (node 5 of a present actor, else the published
    // position, as ActorWorldCombat::eye_c0). A turn writes the selected
    // history entry +3098 (index 0 from the constructor, as for the teleport)
    // and retargets the heading curve with 0.5 s (00186b50).
    dh.hit_direction=[this](const RootVector& position,std::int32_t base)->std::int32_t{
        const auto m=root_placement_matrix(roots.world_placement(floor_root));
        TargetingMatrix frame{};
        for(std::size_t r=0;r<4;++r)frame[r]={m[4*r],m[4*r+1],m[4*r+2],m[4*r+3]};
        const RootVector eye=owner->eligible()?owner->skeleton_node_world_position(5):owner->published_position();
        const auto d=classify_hit_direction(frame,eye,position,base);
        if(d.heading){
            if(heading_state.history.empty())throw std::runtime_error("Solo actor: the hit-direction turn needs a heading history entry");
            apply_actor_heading_state_command(heading_state,{ActorHeadingCommand::Kind::history,heading_state.selected,*d.heading});
            ActorHeadingCommand target;target.kind=ActorHeadingCommand::Kind::target;target.duration=hit_direction_turn_duration;
            apply_actor_heading_state_command(heading_state,target);
            if(current)current->hit_direction_turns.push_back(*d.heading);
        }
        return d.code;};
    dh.adjust_death_code=[this](std::uint8_t code){
        return player()?code:controller.ai->death_code_84(*owner,code);};   // 0016fa10 / the AI's +84
    std::vector<ActorDamageEvent> events;
    dh.death_action=[this](std::int32_t code){
        if(current)current->death_code=code;
        death_action(code);};
    // Events the kill handles synchronously are acted on as they are listed;
    // the controller notices run in the original inside 003ccb00 / 00186bf0,
    // so they are dispatched here in event order after the call.
    // Port-only (checkpoint 132f5): the pause menu's cheats at the damage entry point 00187360. Infinite
    // Health sets the player's own invulnerable flag +33a4 for the call (00187360 returns at 001874a8;
    // the kill 00186c30 refills); Infinite Armour keeps the player's +33a0; Super Agent raises a hit on
    // another actor from a player source (its +58 virtual +54) to the head shot's 100000 (00187768).
    // Port-only (checkpoint 132pc3, port_cheats.hpp PortPartnerShield): the CPU partner's friendly-fire shield. With
    // partner_friendly_fire 0 a friendly source's hit takes 00188780's player-vs-player form on every damage path
    // (amount 0, type 7 -> 0, a head hit as category 3, as 132ob7's bullet rule gives the partner), so 00187360
    // subtracts nothing and its messages and notices stay those of a 0-damage hit; with 1 the original path runs.
    // Either way the Super Agent raise is skipped for a friendly source.
    const bool friendly_hit=port_partner_friendly_source(port_partner_shield(handle),handle,in.source.id);
    const ActorDamageInput* use=&in;
    ActorDamageInput shielded_in;
    if(friendly_hit&&!port_partner_shield(handle)->friendly_fire){
        shielded_in=in;shielded_in.amount=0.0F;
        if(shielded_in.type==7)shielded_in.type=0;
        if(shielded_in.category==2)shielded_in.category=3;
        use=&shielded_in;
        if(current)current->port_partner_shielded=true;
    }
    const bool god=player()&&port_cheat(PortCheat::infinite_health);
    const bool keep_armour=player()&&port_cheat(PortCheat::infinite_armour);
    ActorDamageInput super_in;
    if(!player()&&!friendly_hit&&port_cheat(PortCheat::super_agent)&&in.source.id!=handle&&in.source.controller&&
        in.source.controller->player_54&&in.amount<port_super_agent_damage){
        super_in=in;super_in.amount=port_super_agent_damage;use=&super_in;
    }
    const bool invulnerable=health.invulnerable_33a4;
    const auto armour=health.armor_33a0;
    if(god)health.invulnerable_33a4=true;
    const auto result=apply_actor_damage(health,*use,dh,events);
    if(god)health.invulnerable_33a4=invulnerable;
    if(keep_armour&&health.armor_33a0<armour)health.armor_33a0=armour;
    for(const auto& e:events){
        using K=ActorDamageEvent::Kind;
        if(e.kind==K::damage_message){
            // The replica 00187b58 (+2e 0, +20 8) needs actor +14 clear: a remotely owned actor.
            if(e.flags_20!=5||e.byte_2e!=1)throw std::runtime_error("Solo actor: the replica DamageMsg (00187b58) is not hosted");
            QueuedMessage m;m.kind=QueuedMessage::Kind::ai_notification;
            m.notification.key=actor_damage_message_key;m.notification.object=e.object;m.notification.value=e.value;
            m.notification.type_2c=e.type_2c;m.notification.category_2d=e.category_2d;
            // 003ccb00 queues it (003ccc90) before the kill (+10c) and so
            // ahead of the death notice's AIDeadMsg (+7c, dispatched below in
            // event order); the death action queues nothing on this actor.
            queue.push_back(std::move(m));
        }else if(e.kind==K::item_notice){
            // 00143770: +3524's first word (owner) is set; 00263080 returns at
            // once while the owner's byte +8 is clear.
            if(actions.inventory&&actions.inventory->owner_control_active)
                throw std::runtime_error("Solo actor: 00263080 for an active held item is not hosted");
        }else if(e.kind==K::controller_death){
            if(player()){
                // 00176540: PlayerDeaths += 1 (00333f10(stats, 8, 1)), then 0024b4d0:
                // input pending bit 11 and a rescan of bits 11..0.
                ++player_deaths;set_pending_bit(0x800U,true);
            }else controller.ai->death_notice_7c(*owner,ActorDeathNotice{e.value,e.object,e.category_2d,e.type_2c});
        }else if(e.kind==K::controller_hit){
            if(!player())controller.ai->hit_notice_78(*owner,e.object,e.value);   // the player's +78 is 0016f9f0 (nothing)
        }else if(e.kind==K::death_message&&!e.immediate){
            // Type 7 (00187c40..00187cb8): action 0x42 kind 2 code e.value to
            // self with transport 0; the next pump runs 001a92a0 -> 0018a950.
            QueuedMessage m;m.kind=QueuedMessage::Kind::death;m.code=e.value;queue.push_back(m);
        }else if(e.kind==K::unconscious_message){
            // 00187cd8..00187d24 (with +14): the Script AI Unconscious Message to
            // self with transport 0, after the death message. Its receiver is the
            // AI component (OnAIUnconsciousMsg); the player's is not hosted.
            if(player())throw std::runtime_error("Solo actor: the unconscious message 004ff2c0 to the player actor is not hosted");
            QueuedMessage m;m.kind=QueuedMessage::Kind::ai_notification;
            m.notification.key=actor_unconscious_message_key;m.notification.object=e.object;
            queue.push_back(std::move(m));
        }else if(e.kind==K::ai_flag_1d6){
            // 00186df8, before the death action in the original; nothing in
            // the death action reads the controller's +1d6.
            if(player())throw std::runtime_error("Solo actor: the +1d6 store (00186df8) on the player component");
            controller.ai->set_byte_1d6();
        }else if(e.kind==K::ignite){
            // 00187ac8: 00186360(actor, 1, 0, 0, 0, -1.0) runs inside the damage,
            // before its base part; nothing after it reads +326a or the
            // effect slots, so it runs here in event order.
            if(!fire.ignite)throw std::runtime_error("Solo actor: the ignition 00186360 from the fire damage +33b0 (00187ac8) is not hosted");
            fire.ignite();
        }else if(e.kind==K::sound){
            // 0020f990(msg, desc, actor): a static descriptor (owner 0, priority 0, +186 0) owned by this actor.
            const auto& d=static_sound_descriptor(static_cast<std::uint32_t>(e.value));
            own_sound(d.type,d.name,e.immediate);
        }
        // The LOS request was answered synchronously (dh.los_blocked): a record.
        else if(e.kind!=K::rumble&&e.kind!=K::death_message&&e.kind!=K::flak_notice&&
                e.kind!=K::los_request)
            throw std::runtime_error("Solo actor: damage event (ignition or invulnerable kill) is not hosted");
    }
    if(current)current->damage_events.insert(current->damage_events.end(),events.begin(),events.end());
    return result;
}

// Action 0x42 kind 2 delivered at once (transport 1): 001a92a0 -> 0018a950.
void SoloActorSimulation::Impl::death_action(std::int32_t code){
    if(!model)throw std::runtime_error("Solo actor: the death action needs an animation model");
    ActorDeathActor a;a.controller_is_player=player();a.byte_14=true;a.byte_326b=actions.byte_326b;a.physical_present=true;
    a.secondary_present=true;                                        // 001b5560 allocates +3314 for every activated actor
    // Root object bytes: +2c = 1 and +2e = 0 from the constructor (003cf120,
    // 003cf130), +2d = 1 from the activation (001b53d0 -> 00392940 -> 003ce980).
    a.object_2c=1;a.object_2d=1;a.object_2e=0;
    a.inventory_mode_84=actions.inventory?actions.inventory->mode:std::uint8_t{6};
    a.receiver=handle;
    a.motion_override_34e4=death_motion_34e4;   // -1 (001b6198) unless 0018c9d0 stored one
    a.flags_3204=actions.flags_3204;
    const auto r=handle_actor_death(actions,2,code,a,death_host());
    byte_31b2=r.byte_31b2;word_2dd0=r.word_2dd0;word_2dd4=r.word_2dd4;word_2dec=r.word_2dec;
    if(r.physical_14)frame_state.response.physical.shape_14=*r.physical_14;   // 0018abd0: an NPC's physical +14 = 2
    if(current){current->death=r;if(!r.motion.empty())current->ledge_motions.push_back(r.motion);}
}
// 0019e670(actor, 0): the dead landing after a fall while dying or dead.
void SoloActorSimulation::Impl::land_dead(){
    land_dead_with(frame_state.response.physical.byte_dc!=0);
}
// 0019e670(actor, a1): a1 = 1 (001b0a50, the AI's ground snap) skips the +dc test.
void SoloActorSimulation::Impl::land_dead_with(bool unsupported_dc){
    const auto r=land_dead_actor(actions,true,unsupported_dc,!player(),handle,death_host());
    if(current&&!r.motion.empty())current->ledge_motions.push_back(r.motion);
    if(r.clear_ai_114)controller.ai->clear_action_114();             // 0019e978 (AI virtual +58 0015b7e0 = 1)
}
// The death action's boundaries on the simulation.
ActorDeathHost SoloActorSimulation::Impl::death_host(){
    if(!model)throw std::runtime_error("Solo actor: the death action needs an animation model");
    ActorDeathHost dh;
    dh.random=[this]{return next_game_random(*rng);};
    dh.voice=[this](std::int32_t line){if(current)current->voice_lines.push_back(line);if(voice_sink)voice_sink(line);};
    dh.self_action=[this](std::int32_t action){                       // 001a92a0 -> 001ab9a0
        if(action!=0x24)throw std::runtime_error("Solo actor: unexpected death self action");
        if(current)current->delivered_actions.push_back(action);
        cancel_ladder();};
    dh.landing_pose_input=[this]{
        ActorLandingPoseInput in;
        // +2e64: the model's node collection; nodes 0..3 of the last evaluation.
        const auto nodes=model->skeleton_node_matrices();
        if(nodes.size()<4)throw std::runtime_error("Solo actor: the landing pose needs skeleton nodes 0..3");
        std::array<std::array<float,16>,4> n{};for(std::size_t k=0;k<4;++k)n[k]=nodes[k];
        in.node_matrices=n;
        in.component=root_placement_matrix(published_component?*published_component:roots.world_pose(floor_root));
        in.flor_orientation=roots.world_placement(floor_root).orientation;
        in.physical_present=true;in.velocity_z_310=frame_state.integrator.velocity[2];
        in.fall_height_30a8=jump.fall_height_30a8;
        return in;};
    dh.set_channel=[this](std::size_t channel,float value){
        const ActorChannelCommand c{ActorChannelOperation::set,channel,value,0,0,false};
        model->apply_commands(std::span<const ActorChannelCommand>(&c,1));};
    dh.sound=[this](const std::string& name){
        if(current)current->sounds.push_back(name);
        if(!positional_sink)return;
        // 0019f1c4..0019f208: 001ca210(desc, 1, name, 0, 0), virtual +f4 (the 'flor' placement) +30,
        // 0020f860(msg, desc, position), +20 = 1, 003c9420(msg, 004ffb50, 0).
        const auto m=root_placement_matrix(roots.world_placement(floor_root));
        positional_sink(1,name,RootVector{m[12],m[13],m[14],m[15]},false);};
    dh.landing_effect=[this](std::int32_t a1,std::int32_t a2){
        if(a1!=0||a2!=1)throw std::runtime_error("Solo actor: unexpected landing effect arguments");
        if(current)++current->death_landing_effects;
        landing_effect(0,true);};
    dh.node_id=[this](std::string_view n){return model->motion_node_id(n);};
    dh.resolve_id=[this](std::uint32_t id){return model->resolve_motion_id(id);};
    dh.install=[this](const MotionRequest& q,MotionInstallMode m){model->install_motion(q,m);};
    dh.dead_state=[this]{enter_dead_state();};
    dh.secondary_mode=[this](std::uint8_t secondary){
        if(current)current->secondary_modes.push_back(secondary);
        if(death_secondary_apply)death_secondary_apply(secondary);};
    if(death_secondary_input)dh.secondary_input=death_secondary_input;
    dh.resolve_name=[this](std::string_view n){return model->resolve_motion(n);};
    return dh;
}

// 001a3780 called directly (code 11): token 0x92 is exactly a call of it
// (001a6b48), so it runs through the receiver with an empty delivery view.
void SoloActorSimulation::Impl::enter_dead_state(){
    static const AnimationController no_controller{};
    MotionEffect e;e.kind=MotionEffect::Kind::token;e.receiver=handle;e.id=0x92;
    const MotionDeliveryView view(no_controller,nullptr,false);
    receive_motion_effect(e,view);
}

ActorHangMotionHost SoloActorSimulation::Impl::hang_host(){
    if(!model)throw std::runtime_error("Solo actor: hanging motions need an animation model");
    ActorHangMotionHost h;
    h.resolve=[this](std::string_view n){return model->resolve_motion(n);};
    h.resolve_root=[this](std::string_view n){return model->resolve_named_root(n);};
    h.retained=[this]{return model->retained_motion();};
    h.install=[this](const MotionRequest& r,MotionInstallMode m){model->install_motion(r,m);};
    return h;
}
// Physical fields of the jump/hang functions: +28/+2c, +9c0, +374, +9d0,
// +310, +330, +2e4, +12, +13, +dc, +dd and +2c4.
ActorJumpPhysical SoloActorSimulation::Impl::jump_physical() const{
    const auto& p=frame_state.response.physical;const auto& g=frame_state.integrator;
    ActorJumpPhysical j;j.present=true;j.pending=frame_state.history.pending;
    j.keep_contact_9c0=p.keep_contact_9c0;j.radius_374=frame_state.history.target_radius;
    j.override_9d0=g.override_motion;j.velocity_310=g.velocity;j.force_330=g.force;j.inverse_mass_2e4=g.inverse_mass;
    j.enabled_12=p.enabled_12;j.keep_velocity_13=p.byte_13;j.unsupported_dc=p.byte_dc;j.supported_dd=p.byte_dd;
    j.floor_2c4=p.floor_2c4;
    // +1f4: slot 1 is bound only to static COL owners, which have no +54 component.
    j.support_component_1f4=false;
    return j;
}
// Only the fields these functions write.
void SoloActorSimulation::Impl::store_jump_physical(const ActorJumpPhysical& j){
    frame_state.history.pending=j.pending;frame_state.response.physical.keep_contact_9c0=j.keep_contact_9c0;
    frame_state.history.target_radius=j.radius_374;frame_state.integrator.override_motion=j.override_9d0;
}
// 001a1c80 (from 001a2c00 or mask bit 7 of 001930a0).
void SoloActorSimulation::Impl::catch_ledge(ActorJumpPhysical& jp){
    const auto r=catch_actor_ledge(actions,jp,handle,hang_host());
    if(!current)return;
    current->ledge_caught=current->ledge_caught||r.caught;
    for(const auto& m:r.motions)current->ledge_motions.push_back(m.name);
    current->sounds.insert(current->sounds.end(),r.sounds.begin(),r.sounds.end());
    for(const auto& s:r.sounds){
        if(s!="M_SHIMMY_ON")throw std::runtime_error("Solo actor: an unknown ledge-catch sound "+s);
        own_sound(1,s);                                  // 001a22e0: bank 1, transport 1
    }
    current->delayed_sounds+=r.delayed_sounds;
    for(int i=0;i<r.delayed_sounds;++i)timed_event(ActorTimedEvent::Kind::shimmy);
}
// 001a236c..001a2448 / 001a12d0..001a13b0: one game RNG draw for the delay, the
// record queued on the script callback list; its handler 001b95c0 at the fire.
void SoloActorSimulation::Impl::timed_event(ActorTimedEvent::Kind kind){
    const auto e=actor_timed_event(kind,next_game_random(*rng));
    if(!timed_event_sink)return;
    timed_event_sink(e.delay,[this,kind]{
        if(kind==ActorTimedEvent::Kind::landing)landing_effect(1,true);        // 001b95f4: 00188fb0(actor, 1, 1)
        else own_sound(1,"M_SHIMMY_ON",true);                                 // 001b961c..001b9644: bank 1, transport 1
    });
}
// The self-sent action 0x14 of 001930a0, delivered at once: 001a92a0 -> 001914c0
// with the 'flor' and 'edge' root world placements (00132400/001910a0).
void SoloActorSimulation::Impl::jump_to_grab(const RootParentTransform& edge){
    ActorJumpGrabRequest r;r.life_state=life_state();r.action=0x14;r.kind=14;
    r.floor_z=roots.world_placement(floor_root).position[2];r.edge=edge;r.receiver=handle;
    const auto out=handle_actor_jump_to_grab(actions,jump,r,hang_host());
    if(current)for(const auto& m:out.motions)current->ledge_motions.push_back(m.name);
}
// The body carry's root bindings: 001a6d40 (parent root, child root, reset)
// with the parent root in another actor's collection. Both actors keep the
// binding; the child's model anchor follows the parent placement.
namespace {
const RootAnchorKey& carry_key_of(std::size_t id,std::size_t flor,std::size_t edge,std::size_t ladd){
    if(id==flor)return SoloActorSimulation::flor_key;
    if(id==edge)return SoloActorSimulation::edge_key;
    if(id==ladd)return SoloActorSimulation::ladd_key;
    throw std::runtime_error("Solo actor: a carried root outside 'flor', 'edge' and 'ladd'");
}
}
void SoloActorSimulation::Impl::bind_external(std::size_t id,const RootAnchorKey& key,Impl& parent,const RootAnchorKey& parent_key,bool reset){
    if(&parent==this)throw std::runtime_error("Solo actor: 001a6d40 under the actor's own root is not a carry binding");
    if(external_parents.count(id))unbind_external(id);
    const auto placement=parent.roots.world_placement(parent.root_of(parent_key));
    roots.attach_external(id,placement,reset);
    if(model&&model->has_anchor(key)){
        model->set_anchor_parent(key,placement);
        model->set_anchor_world_placement(key,roots.world_placement(id),reset);
    }
    external_parents[id]={parent.handle,parent_key};
    parent.bound_children.push_back({handle,id,parent_key});
}
// The unlink keeps the world placement (0019fd60 / 001a08d0).
void SoloActorSimulation::Impl::unbind_external(std::size_t id){
    const auto it=external_parents.find(id);
    if(it==external_parents.end())return;
    const auto world=roots.world_placement(id);
    roots.detach_external(id);
    const auto& key=carry_key_of(id,floor_root,edge_root,ladd_root);
    if(model&&model->has_anchor(key)){
        model->set_anchor_parent(key,std::nullopt);
        model->set_anchor_world_placement(key,world,false);
    }
    if(auto* p=find_peer(it->second.actor)){
        auto& c=p->bound_children;
        c.erase(std::remove_if(c.begin(),c.end(),[&](const BoundChild& b){return b.actor==handle&&b.root==id;}),c.end());
    }
    external_parents.erase(it);
}
// 0019fd60 on the root of key: its children detached keeping their world placement.
void SoloActorSimulation::Impl::clear_root_children(const RootAnchorKey& key){
    const auto children=bound_children;
    for(const auto& c:children){
        if(c.key!=key)continue;
        if(auto* p=find_peer(c.actor))p->unbind_external(c.root);
        else bound_children.erase(std::remove_if(bound_children.begin(),bound_children.end(),
            [&](const BoundChild& b){return b.actor==c.actor&&b.root==c.root;}),bound_children.end());
    }
}
// For each relation child (+2fc0 in order): 001a6d40(own root of key, the
// child's root of key, 1).
void SoloActorSimulation::Impl::attach_children(const RootAnchorKey& key){
    for(const auto c:relation_children){
        auto& p=peer(c);
        p.bind_external(p.root_of(key),key,*this,key,true);
    }
}
// A carried root reads its parent's latest placement (the carrier's model
// steps first in the world's actor order).
void SoloActorSimulation::Impl::sync_external_parents(){
    const auto bound=external_parents;
    for(const auto& [id,ep]:bound){
        auto* p=find_peer(ep.actor);
        if(!p){unbind_external(id);continue;}
        const auto placement=p->roots.world_placement(p->root_of(ep.key));
        roots.update_external(id,placement);
        const auto& key=carry_key_of(id,floor_root,edge_root,ladd_root);
        if(model&&model->has_anchor(key))model->set_anchor_parent(key,placement);
    }
}
// 00143b60 on +3524 (the pickup's holster).
void SoloActorSimulation::Impl::hide_inventory_for_carry(){
    if(!actions.inventory)throw std::runtime_error("Solo actor: the holster 00143b60 without an inventory view");
    ActorAnimationReceiverConfig cfg;cfg.receiver=handle;cfg.controller_receiver=controller_handle;
    if(inventory_bindings)cfg.inventory=*inventory_bindings;
    else if(actions.inventory->mode!=6)
        throw std::runtime_error("Solo actor: the holster of a held item (00143b60 -> 00142bb0) without the inventory receiver");
    hide_actor_inventory(actions,cfg,inventory_host);
}
// 00143cf0: with the saved inventory mode +85 = 6 (nothing held) it does
// nothing; otherwise 00142c50 equips the saved slot on the inventory view
// through the combat host's inventory members (set_inventory_receiver).
void SoloActorSimulation::Impl::restore_inventory(){
    if(!actions.inventory)throw std::runtime_error("Solo actor: the inventory restore 00143cf0 without an inventory view");
    if(actions.inventory->saved_mode==6)return;
    if(!inventory_bindings)
        throw std::runtime_error("Solo actor: inventory restore of a held item (00142c50) without the inventory receiver");
    ActorAnimationReceiverConfig cfg;cfg.receiver=handle;cfg.controller_receiver=controller_handle;
    cfg.inventory=*inventory_bindings;
    restore_actor_inventory(actions,cfg,inventory_host);
}
ActorBodyCarryHost SoloActorSimulation::Impl::carry_host(std::uint64_t body){
    ActorBodyCarryHost h;
    h.link=[this,body]{actions.linked_model={body,1};};                              // 001adab0
    h.activate_body=[this,body]{peer(body).owner->activate();};                     // virtual +5c(1)
    h.holster=[this]{hide_inventory_for_carry();};
    h.hide_backpack=[this,body]{
        if(!carry_hooks||!carry_hooks->hide_backpack)throw std::runtime_error("Solo actor: the backpack hide 00256120 is not hosted");
        carry_hooks->hide_backpack(body);};
    h.place_edge_root=[this](const RootParentTransform& t){                        // 001ad9c0
        roots.set_world_placement(edge_root,t,true);
        if(model&&model->has_anchor(SoloActorSimulation::edge_key))model->set_anchor_world_placement(SoloActorSimulation::edge_key,t,true);
    };
    h.floor_root_world_placement=[this]{return roots.world_placement(floor_root);};
    h.relate=[this,body]{                                                         // 001ad8c0
        auto& b=peer(body);
        if(b.relation_parent&&*b.relation_parent!=handle)
            throw std::runtime_error("Solo actor: 001ad8c0 on a body related to another actor");
        b.relation_parent=handle;
        if(std::find(relation_children.begin(),relation_children.end(),body)==relation_children.end())relation_children.push_back(body);
    };
    h.attach_child_edges=[this]{attach_children(SoloActorSimulation::edge_key);};
    h.body_action=[this,body](std::int32_t a){                                     // transport 1: the body's 001a92a0 at once
        auto& b=peer(body);
        const auto r=handle_actor_body_action(a,b.actions.flags_324c,b.handle,b.carry_host(handle));
        if(current)for(const auto& n:r.motions)current->carry_motions.push_back("body "+n);
    };
    h.notify_body_moved=[this,body](bool picked){                                  // AIBodyMovedMsg, queued to the body
        ActorAiNotification n;n.key=actor_body_moved_message_key;n.object=handle;n.value=picked?1:0;
        peer(body).owner->queue_ai_notification(n);
    };
    h.resolve=[this](std::string_view n){
        if(!model)throw std::runtime_error("Solo actor: the body carry needs an animation model");
        return model->resolve_motion(n);};
    h.retained=[this]{
        if(!model)throw std::runtime_error("Solo actor: the body carry needs an animation model");
        return model->retained_motion();};
    h.install=[this](const MotionRequest& r,MotionInstallMode mode){model->install_motion(r,mode);};
    return h;
}
// 001abbe8 with the body 0040c540 resolves from the message +2c key.
void SoloActorSimulation::Impl::deliver_carry_pickup(std::uint64_t body,std::uint8_t kind){
    ActorCarryPickupInput in;in.life_state=life_state();in.byte_14=true;in.mode_320c=static_cast<std::int32_t>(mode());
    in.linked=linked().has_value();in.kind_28=kind;in.controller_is_player=player();
    if(auto* b=find_peer(body);b&&b!=this){
        if(!carry_hooks||!carry_hooks->backpack)throw std::runtime_error("Solo actor: the body's backpack +3464 is not hosted");
        ActorCarryBody cb;cb.human=true;cb.mode_320c=b->actions.highest_action;
        cb.active=b->byte_2c&&b->byte_2d&&!b->byte_2e;cb.byte_14=true;
        cb.backpack_3464=carry_hooks->backpack(body);cb.flags_324c=b->actions.flags_324c;
        cb.floor_position=b->roots.world_placement(b->floor_root).position;
        const auto m=root_placement_matrix(b->published_component?*b->published_component:b->roots.world_pose(b->floor_root));
        cb.component_row2={m[8],m[9],m[10],m[11]};
        in.body=cb;
    }
    const auto r=handle_actor_carry_pickup(actions.flags_324c,handle,in,carry_host(body));
    if(current)current->carry_motions.insert(current->carry_motions.end(),r.motions.begin(),r.motions.end());
}
// 0019f890's bit-21 leave (actor_body_carry.hpp leave_actor_carry).
void SoloActorSimulation::Impl::leave_carry(){
    ActorCarryLeaveHost lh;
    lh.detach_floor_children=[this]{clear_root_children(SoloActorSimulation::flor_key);};
    lh.detach_edge_children=[this]{clear_root_children(SoloActorSimulation::edge_key);};
    lh.restore_inventory=[this]{restore_inventory();};
    lh.unlink=[this]{                                                               // 001a02c0
        const auto body=linked();
        ActorCarryUnlinkInput in;in.linked=body.has_value();in.controller_is_player=player();in.byte_14=true;
        in.flags_3254=action_history.flags_3250[1];in.flags_325c=action_history.flags_3258[1];
        ActorCarryUnlinkHost uh;
        uh.detach_relation_roots=[this]{                                            // 001a08d0
            const auto children=bound_children;
            for(const auto& c:children)if(auto* p=find_peer(c.actor))p->unbind_external(c.root);
            bound_children.clear();
        };
        uh.clear_relation=[this]{                                                   // 001a07f0
            for(const auto c:relation_children)if(auto* p=find_peer(c);p&&p->relation_parent==handle)p->relation_parent.reset();
            relation_children.clear();
        };
        uh.notify_body_moved=[this,body](bool picked){
            ActorAiNotification n;n.key=actor_body_moved_message_key;n.object=handle;n.value=picked?1:0;
            peer(*body).owner->queue_ai_notification(n);
        };
        uh.turn_on_weapon_pickup=[this,body]{                                       // cTurnOnWeaponPickupMsg, transport 0
            QueuedMessage m;m.kind=QueuedMessage::Kind::turn_on_weapon_pickup;
            peer(*body).queue.push_back(m);
        };
        uh.body_landing_pose=[this,body]{                                           // 0019e9a0(body, ..., 0, 0)
            return run_actor_landing_pose(peer(*body).death_host(),false,false).motion;
        };
        uh.resolve_body=[this,body](std::string_view n){                           // 0018c5a0 on the body
            auto& b=peer(*body);
            if(!b.model)throw std::runtime_error("Solo actor: the carried body's landing needs its animation model");
            if(b.current)b.current->ledge_motions.emplace_back(n);
            return b.model->resolve_motion(n);
        };
        uh.install_body=[this,body](const MotionRequest& q,MotionInstallMode m){    // 003ad500 on the body
            peer(*body).model->install_motion(q,m);
        };
        uh.unlink=[this]{actions.linked_model={};};
        unlink_actor_body(in,uh);
    };
    leave_actor_carry(lh);
}

// 003c8de0 for this actor's queued messages, in send order: player-component
// codes (00176660) and actor actions (001b3b60 -> 001a92a0).
void SoloActorSimulation::Impl::deliver_queued(){
    auto delivering=std::move(pump_snapshot);pump_snapshot.clear();
    for(std::size_t position=0;position<delivering.size();++position){
        const auto& m=delivering[position];
        using K=QueuedMessage::Kind;
        if(m.kind==K::component){apply_component_code(m.code);continue;}
        if(m.kind==K::notify_checkpoint){                  // 003c8de0: filter +0x38 (00175fd0 -> 003e1aa0) first
            // Not handled: the natives (00177380 for the player) know no key
            // 004f7938 and fall through to 003cb210; no native effect.
            const bool handled=notify_checkpoint_filter&&notify_checkpoint_filter();
            if(current)current->notify_checkpoints.push_back({position,handled});
            continue;
        }
        if(m.kind==K::out_of_ammo){                        // 003c8de0: filter +0x38 (00175fd0 -> 003e1aa0) first
            // Not handled: no native receiver tests key 004ea150; no effect.
            const bool handled=out_of_ammo_filter&&out_of_ammo_filter(m.code);
            if(current)current->out_of_ammo.push_back({m.code,handled});
            continue;
        }
        if(m.kind==K::weapon_vis_change){                  // 003c8de0: filter +0x38 (00175fd0 -> 003e1aa0) first
            // Not handled: no native receiver tests key 00583968; no effect.
            const bool handled=weapon_vis_change_filter&&weapon_vis_change_filter(m.code);
            if(current)current->weapon_vis_changes.push_back({m.code,handled});
            continue;
        }
        if(m.kind==K::box){                                // 003c8de0: filter +0x38 first
            if(hotbox_filter&&hotbox_filter(m.box,m.payload)){
                if(current)current->hotbox_script_events.push_back(m.tracker_event);
                continue;
            }
            if(m.payload==3){                              // 001717a0 action 3 -> +0x114 (00171880)
                if(!multiplayer_host)throw std::runtime_error("Solo actor: HotboxMsg action 3 outside a multiplayer game");
                QueuedMessage t;t.kind=K::trigger;t.trigger=hotbox_multiplayer_interaction_trigger(hotbox_world,m.box);
                queue.push_back(t);                        // transport 0
                if(current)current->triggers.push_back(t.trigger);
                continue;
            }
            // 00171b60 -> 00172400 / 00171ed0: the type-1/6/7 link messages
            // (transport 0) to their receivers' queues, then the event to the actor.
            for(const auto& l:hotbox_link_messages(hotbox_world,m.box,m.payload)){
                if(!link_sink)throw std::runtime_error("Solo actor: hotbox link message to "+std::to_string(l.receiver)+" without a link sink");
                link_sink(l);
            }
            if(const auto e=receive_hotbox_event(hotbox_world,m.box,m.payload)){
                QueuedMessage a;a.kind=K::actor_hotbox;a.code=static_cast<std::int32_t>(e->code);a.payload=e->payload;a.word=e->word_2c;
                queue.push_back(a);                        // transport 0: the next pump delivers it
            }
            continue;
        }
        if(m.kind==K::actor_hotbox){
            deliver_actor_hotbox({static_cast<std::uint32_t>(m.code),m.payload,m.word});
            continue;
        }
        if(m.kind==K::trigger){                            // 003c8de0: filter +0x38 first
            // A spawned backpack (00257640) has no script filter: its receiver
            // 00254600 takes the picker from the message +30 (the actor) and runs
            // 001762c0 with the player component's +1bc / +17c as they stand.
            if(object_exists&&object_trigger&&object_exists(m.trigger.receiver)){
                object_trigger(m.trigger.receiver,buttons);
                if(current)current->native_triggers.push_back(m.trigger);
                continue;
            }
            if(trigger_filter&&trigger_filter(m.trigger)){
                if(current)current->trigger_script_events.push_back(m.trigger);
                continue;
            }
            if(native_trigger_receiver&&native_trigger_receiver(m.trigger)){
                if(current)current->native_triggers.push_back(m.trigger);
                continue;
            }
            throw std::runtime_error("Solo actor: the native receiver of interaction trigger to key "+std::to_string(m.trigger.receiver)+
                                     " is not hosted (InteractGOBJ 00216580)");
        }
        if(m.kind==K::checkpoint){                                  // 001b3b60 MSGGAME_ADD_CHECKPOINT
            store_checkpoint(m.node_position,m.node_yaw,m.label);
            if(current)current->checkpoint_added=true;
            continue;
        }
        if(m.kind==K::object_action){deliver_object_action(m.object_action);continue;}
        if(m.kind==K::object_code){deliver_object_code(m.object,m.code);continue;}
        if(m.kind==K::link){deliver_link_message(m.enable);continue;}
        if(m.kind==K::death){                             // 001a92a0 case 0x42 -> 0018a950 (the queued type-7 death)
            if(current){current->delivered_actions.push_back(0x42);current->death_code=m.code;}
            death_action(m.code);
            continue;
        }
        if(m.kind==K::ai_notification){                   // 003c8de0: filter +0x38 (003cc850) first
            const bool handled=ai_notification_filter&&ai_notification_filter(m.notification);
            // Not handled: 001b3b60 -> 003cea10 -> 003caca0 over the empty child list +0x20 (dropped).
            if(current)current->ai_notifications.push_back({m.notification,handled});
            continue;
        }
        if(m.kind==K::respawn){                                     // 001b45dc
            if(current)current->respawn_requested=true;
            // Revive for an AI controller, with +14 clear, or with action
            // 0x1f set; a live local player re-enters the dead state.
            if(!player()||actions.byte_14==0||(actions.active_actions&(1U<<0x1f)))revive();
            else enter_dead_state();
            continue;
        }
        if(m.kind==K::carry_pickup){                       // 001a92a0 case 0x2b -> 001abbe8
            if(current)current->delivered_actions.push_back(0x2b);
            deliver_carry_pickup(m.object,m.payload);
            continue;
        }
        if(m.kind==K::running_jump){                       // 001a92a0 0x15 / 0x16 -> 001914c0
            if(current)current->delivered_actions.push_back(m.code);
            deliver_running_jump(m.code,m.point);
            continue;
        }
        if(m.kind==K::turn_on_weapon_pickup){              // cTurnOnWeaponPickupMsg: 001b48f8 -> 001a3250
            if(!carry_hooks||!carry_hooks->turn_on_weapon_pickup)
                throw std::runtime_error("Solo actor: cTurnOnWeaponPickupMsg (001b48f8 -> 001a3250) is not hosted");
            carry_hooks->turn_on_weapon_pickup(handle);
            continue;
        }
        if(current)current->delivered_actions.push_back(m.code);
        if(m.code==0x17){request_running_jump(m.jump_zone);continue;}
        // Actor +14 is set for every locally owned actor (003cb0e0), the AI
        // actors included; the +58 component decides the replica branches.
        ActorHangActionInput in;in.life_state=life_state();in.byte_14=true;in.component_is_player=player();
        in.receiver=handle;
        if(m.code==0x11){                                  // 001ab1d0
            // An AI component's controller codes 6 / 9 (00151a90) queue 0x11 on a
            // type-4 path link (+114 == 4) and 0x13 otherwise.
            auto jp=jump_physical();ActorPullUpResult r;
            with_live(jp,[&]{r=handle_actor_pull_up(actions,jp,frame_state.edges.slots,in,hang_host());});
            store_jump_physical(jp);
            // A rejection sends code 9 to the +58 component at once (001ab5a8):
            // 00176660 accepts it without effect (table entry 9); an AI
            // component's 00151b40 queues 0x11 again on a type-4 link.
            if(r.rejected&&!player())controller.ai->controller_message_immediate(*owner,9);
            if(current)for(const auto& x:r.motions)current->ledge_motions.push_back(x.name);
        }else if(m.code==0x13){                            // 001ab610
            handle_actor_hang_drop(actions,in,[this]{fall();});
        }else if(!player()&&(m.code==0x1a||m.code==1||m.code==2||m.code==9||m.code==0xa||m.code==0x0f||m.code==0x10)){
            // From an AI component: 0x1a (0014bff0, link type 3), the crouch /
            // stand requests 1 / 2 (0014d8f0 / npc_request_action) and the crawl
            // start / end 9 / 0xa (npc_request_action 4 / 0xa, path-link type 8):
            // 001a9a18 / 001a9a88 / 001a9ad0 / 001a9ae8 (actor_pad_actions; 9 / 0xa
            // are 001ae2a0(+3248, 1, 1 / 0), independent of the sender). The climb
            // requests 0x0f / 0x10 (001ab050 / 001ab140) are queued by the
            // controller code 8 (00151bb0) after a failed edge request.
            send_pad_action(m.code,false);
        }else if(!player()&&m.code>=0x1e&&m.code<=0x23){
            // From an AI component: 0x1e when its path link type leaves 3 (the ladder link,
            // 0014bff0 event 7, 001521a0), kind 0: 001a92a0 -> 001ab990 -> 00472e10 type 0.
            request_ladder_exit(m.code);
        }else if(m.code==0x40||m.code==0x41){              // 001ad5b0 / 001ad5c0
            deliver_flashbang_hide(m.code);
        }else if(m.code==actor_cancel_action){             // 001ad1d0
            deliver_cancel();
        }else if(m.code==actor_cancel_request_action){     // 001ad0e8 (queued, game::StopPerformingAction)
            if(actor_cancel_request_admitted(static_cast<std::int32_t>(mode()),actions.flags_324c,ActorCancelRequest{m.payload,m.word}))
                queue_action(actor_cancel_action);
        }else if(m.code==8){                               // 001aa468
            deliver_roll();
        }else throw std::runtime_error("Solo actor: queued actor action "+std::to_string(m.code)+" is not hosted (001a92a0)");
    }
}
// 00176660 (table 0049cb10) on the pending priority set (+0x40/+0x44): each
// step sets or clears one bit and rescans bits 11..0 into +0x40 (-1 without
// one). Codes 2 (001766a0: set 4, set 2), 3 (001767f0: clear 2), 4
// (00176898: clear 4, set 3), 5 (001769e8: clear 3) carry the stance; 6/7
// set/clear bit 7 (the hang, 00176a90/00176b38); 10/12 set/clear bit 6 (the
// ladder, 00176be0/00176c88); 8, 9 and 11 are accepted without effect
// (00176e38).
void SoloActorSimulation::Impl::apply_component_code(std::int32_t code){
    if(code==8||code==9||code==11)return;
    const auto change=[this](std::uint32_t bit,bool set){set_pending_bit(bit,set);};
    switch(code){
    case 2:change(0x10U,true);change(0x4U,true);return;
    case 3:change(0x4U,false);return;
    case 4:change(0x10U,false);change(0x8U,true);return;
    case 5:change(0x8U,false);return;
    case 6:case 7:change(0x80U,code==6);return;
    case 10:case 12:change(0x40U,code==10);return;
    case 16:
        start_actor_fade(fader,2,1.0F);
        schedule_actor_respawn_poll(respawn_poll);
        if(current)current->fade_out=true;
        return;                                                         // 00176d30
    case 17:                                                            // 00176e20 -> 0024b410
        // Clears pending bit 11 (the kill notice's) and rescans +40. Input
        // +11c/+120 are also cleared; the solo input has neither field (their
        // meaning is not identified, ACTOR_TELEPORT.md open questions).
        change(0x800U,false);
        return;
    default:throw std::runtime_error("Solo actor: player component message "+std::to_string(code)+" is not hosted");
    }
}
// 00191100 on root +3298 ('ladd'), then root +160 = 0; mirrored into the model's anchor.
void SoloActorSimulation::Impl::place_ladd_root(const RootParentTransform& t,bool reset){
    roots.set_world_placement(ladd_root,t,reset);
    if(model&&model->has_anchor(SoloActorSimulation::ladd_key))model->set_anchor_world_placement(SoloActorSimulation::ladd_key,t,reset);
}
// 001a1590. Physical +310 is read from the integrator (the fall writes no
// physical field). The forward row is actor +50's matrix row +20, taken as
// the model component's (as for the launch position; HANGING.md).
void SoloActorSimulation::Impl::fall(){
    ActorFallInput in;in.physical_present=true;in.velocity_310=frame_state.integrator.velocity;
    const auto m=root_placement_matrix(published_component?*published_component:roots.world_pose(floor_root));
    in.forward_row={m[4],m[5],m[6],m[7]};in.receiver=handle;
    const auto r=fall_actor(actions,in,hang_host());
    if(current){current->fell=true;for(const auto& x:r.motions)current->ledge_motions.push_back(x.name);}
}
// 001a0b30(actor, 1) from 001a2c00, with the live airtime +30a0.
void SoloActorSimulation::Impl::land(){
    ActorLandInput in;in.life_state=life_state();in.argument=1;in.airtime_30a0=jump.airtime_30a0;in.receiver=handle;
    const auto r=land_actor(actions,in,hang_host());
    if(!current)return;
    current->landed=true;
    for(const auto& x:r.motions)current->ledge_motions.push_back(x.name);
    current->landing_effects+=r.effects;current->delayed_landing_effects+=r.delayed_events;
    for(int i=0;i<r.effects;++i)landing_effect(2,true);
    for(int i=0;i<r.delayed_events;++i)timed_event(ActorTimedEvent::Kind::landing);
}
// 001a0b30(actor, 0) from the teleport tail (action 10): the retained motion
// reset with the model's default duration, physical +9c0 = 0 and +11 = 1, then
// the landing effect and timed event as for argument 1.
void SoloActorSimulation::Impl::land_reset(){
    ActorLandInput in;in.life_state=life_state();in.argument=0;in.receiver=handle;
    if(model)in.default_duration=model->default_motion_duration();
    const auto r=land_actor(actions,in,hang_host());
    if(r.physical_reset){frame_state.response.physical.keep_contact_9c0=0;frame_state.tail.reset_requested=1;}
    if(!current)return;
    current->landed=current->landed||r.handled;
    for(const auto& x:r.motions)current->ledge_motions.push_back(x.name);
    current->landing_effects+=r.effects;current->delayed_landing_effects+=r.delayed_events;
    for(int i=0;i<r.effects;++i)landing_effect(2,true);
    for(int i=0;i<r.delayed_events;++i)timed_event(ActorTimedEvent::Kind::landing);
}
// 00188fb0(actor, a1, a2). The water branch (00189440, actor_landing_effect.hpp)
// when +e4 holds a contact height: a sound by the depth from 001e96a0. Otherwise
// the dry branch (actor_footstep.hpp), hosted only with footstep hooks: the foot
// probe 001e9710 (foot 1 for a1 == 1, else 2; threshold 2.25), then on a hit the
// surface row's sound. Without a hit 00189440 has nothing to do on dry ground.
void SoloActorSimulation::Impl::landing_effect(std::uint8_t a1,bool a2){
    const auto& physical=frame_state.response.physical;
    if(physical.e4!=actor_unavailable_height){
        const float depth=landing_contact_depth(physical.e4,actor_unavailable_height,roots.world_placement(floor_root).position[2]);
        const auto name=landing_water_sound(depth);
        if(current)current->sounds.push_back(name);    // sent at once (transport 1)
        own_sound(0,name);                              // 00189480: bank 0
        return;
    }
    if(!footsteps.materials)return;
    const auto probe=actor_foot_probe(physical.e8!=0,a1==1?1:2,frame_state.plane_f0,actor_footstep_probe_threshold,
        [this](int node){
            // Model byte +14 (the node collection): 0019f430..00182f30 on the node; otherwise 001e20c0 + 00132020.
            if(!model||!model->human_node_collection())
                throw std::runtime_error("Solo actor: the foot probe without a node collection (001e20c0 / 00132020) is not hosted");
            return model->skeleton_node_world_position(node);});
    if(!probe.hit)return;
    const auto sound=actor_footstep_sound(footsteps.materials->row(floor_surface_141),words.current_3204,mode(),a2);
    if(sound.kind==ActorFootstepSound::Kind::owned){
        if(current)current->sounds.push_back(sound.name);   // 0020f990, transport 1
        own_sound(sound.bank,sound.name);
    }else if(sound.kind==ActorFootstepSound::Kind::positional){
        if(current)current->sounds.push_back(sound.name);   // 0020f860, transport 0
        if(footsteps.positional)footsteps.positional(sound.bank,sound.name,probe.point);
    }
    // Property +c: -1 / -2 skip the effect; the ids 004089f0 resolved go to 00232460.
    if(sound.effect_id!=-1&&sound.effect_id!=-2){
        if(!footsteps.effect)throw std::runtime_error("Solo actor: a resolved surface effect (00232460) is not hosted");
        footsteps.effect(sound.effect_id,actor_footstep_effect_frame(frame_state.integrator.velocity,probe.point));
        if(current)++current->surface_effects;
    }
    if(sound.effect_named&&current)++current->unresolved_surface_effects;
}
// 00197e00..00198018: the walking gait on the model's group-0 phase (00198f30 /
// 00198ca0 / 001987d0): +351c, +31dd and 00188fb0(actor, foot, 0). Then
// 0019801c..00198274: the shimmy move on the group-5 phase (+3520, M_SHIMMY_MOVE).
void SoloActorSimulation::Impl::footstep_gait(){
    const auto sample=model?model->gait_sample(0):std::nullopt;
    if(!sample)throw std::runtime_error("Solo actor: footsteps need the model's gait sample (00198f30)");
    const auto step=actor_gait_step(gait_351c,*sample,true,frame_state.response.physical.byte_dd!=0);
    if(step.foot)lead_foot_31dd=step.foot;
    if(step.effect){
        if(current)++current->footsteps;
        landing_effect(step.foot,false);
    }
    gait_351c=step.phase_351c;
    const auto shimmy_sample=model->gait_sample(actor_shimmy_motion_group);
    if(!shimmy_sample)throw std::runtime_error("Solo actor: the shimmy move needs the model's group-5 sample (0019801c)");
    const auto shimmy=actor_shimmy_step(shimmy_3520,*shimmy_sample);
    shimmy_3520=shimmy.phase_3520;
    if(shimmy.sound){
        if(current)current->sounds.push_back(actor_shimmy_move_sound);
        own_sound(1,actor_shimmy_move_sound);                              // 00198224: bank 1, transport 1
    }
}
// Ladder physical view: the contacts of the last detection, +980, +28/+2c.
std::optional<ActorLadderPhysicalView> SoloActorSimulation::Impl::ladder_view(){
    return ActorLadderPhysicalView{&frame_state.edges.ladder,ladder_980,&frame_state.history.pending};
}
// 00474050's boundaries on the simulation (LADDERS.md, Native wiring).
ActorLadderBodyHost SoloActorSimulation::Impl::ladder_body_host(const ActorAnimationHost& ah){
    ActorLadderBodyHost h;
    h.neutral_channels=[this,&ah]{                                       // 0019dd80(actor, 0, 0, 0, 0)
        const auto st=ah.snapshot();
        const auto plan=prepare_actor_neutral_channels(st.ordinary,*tuning,st.approach_step);
        ActorAnimationPatch patch;patch.commands=plan.commands;patch.prior_direction=plan.prior_direction;
        ah.apply(patch);
    };
    h.channel=[this](std::size_t i){const auto& c=model->channel(i);return ActorLadderChannel{c.previous,c.current,c.target};};
    h.set_channel=[this](std::size_t i,float v){
        const ActorChannelCommand c{ActorChannelOperation::set,i,v,0,0,false};model->apply_commands(std::span<const ActorChannelCommand>(&c,1));};
    h.set_channel_target=[this](std::size_t i,float t,float d,float damping,bool wrapped){
        const ActorChannelCommand c{ActorChannelOperation::target,i,t,d,damping,wrapped};model->apply_commands(std::span<const ActorChannelCommand>(&c,1));};
    h.root_world_position=[this](ActorRoot r){return roots.world_placement(r==ActorRoot::ladder?ladd_root:edge_root).position;};
    h.set_root_base=[this](ActorRoot r,const RootVector& p){                // 001e1f10
        if(r==ActorRoot::ladder){
            roots.set_world_position(ladd_root,p);
            if(model->has_anchor(SoloActorSimulation::ladd_key))model->set_anchor_world_position(SoloActorSimulation::ladd_key,p);
        }else{
            roots.set_world_position(edge_root,p);
            if(model->has_anchor(SoloActorSimulation::edge_key))model->set_anchor_world_position(SoloActorSimulation::edge_key,p);
        }
    };
    h.sound=[this]{if(current)current->sounds.push_back("F_LADDER_M");own_sound(0,"F_LADDER_M");};  // 00188f00: bank 0, transport 1
    h.dismount=[this]{                                                    // 004728d0
        const auto view=ladder_view();
        ActorLadderDismountHost dh;
        dh.channel=[this](std::size_t i){const auto& c=model->channel(i);return ActorLadderChannel{c.previous,c.current,c.target};};
        dh.root_world_placement=[this](ActorRoot r){return roots.world_placement(r==ActorRoot::ladder?ladd_root:edge_root);};
        // The +58 player component's virtual +54 is true and actor +14 is set
        // (003cb0e0), so the exit is sent (LADDERS.md, "Actor +14"). For an AI
        // component (checkpoint 132ob5) +14 is set as well, so the exit is sent
        // whatever its +54 answers.
        const auto m=dismount_actor_ladder(actions,exit_fields,&*view,{true,true},dh);
        if(current)current->ladder_dismount_checked=true;
        if(m)deliver_ladder_exit(*m);
    };
    h.resolve=[this](std::string_view n){return model->resolve_motion(n);};
    h.resolve_root=[this](std::string_view n){return model->resolve_named_root(n);};
    h.install=[this](const MotionRequest& r,MotionInstallMode m){model->install_motion(r,m);};
    return h;
}
// The dismount message delivered at once: 001a92a0 -> 001ab990 -> 00472e10 type 14.
void SoloActorSimulation::Impl::deliver_ladder_exit(const ActorLadderExitMessage& m){
    if(current)current->delivered_actions.push_back(m.action);
    ActorLadderInstallHost ih;
    ih.place_edge_root=[this](const RootParentTransform& t){
        roots.set_world_placement(edge_root,t,true);
        if(model->has_anchor(SoloActorSimulation::edge_key))model->set_anchor_world_placement(SoloActorSimulation::edge_key,t,true);
    };
    ih.bind_support=[this](std::size_t i,const RootParentTransform& local,std::uint64_t object){
        ActorLedgeSupportBinding b;b.object=object;b.local=local;b.previous_owner={{0,0,0,1},{0,0,0,1}};b.enabled=true;bind_support(i,b);};
    ih.resolve_id=[this](std::uint32_t id){return model->resolve_motion_id(id);};
    ih.retained=[this]{return model->retained_motion();};
    ih.install=[this](const MotionRequest& r,MotionInstallMode mode){model->install_motion(r,mode);};
    install_actor_ladder_exit(life_state(),true,m,transition_2ef8,handle,ih);
    if(current)current->ledge_motions.push_back("ladder exit");
}
// Pad case 6 request (immediate): 001a92a0 -> 001ab990 -> 00472e10 type 0.
void SoloActorSimulation::Impl::request_ladder_exit(std::int32_t action){
    const auto view=ladder_view();
    ActorLadderExitHost eh;
    eh.channel=[this](std::size_t i){const auto& c=model->channel(i);return ActorLadderChannel{c.previous,c.current,c.target};};
    eh.root_world_position=[this](ActorRoot r){return roots.world_placement(r==ActorRoot::ladder?ladd_root:edge_root).position;};
    eh.find_node=[this](std::string_view n){return model->motion_node_id(n);};
    eh.send_self=[this](std::int32_t a){                                  // action 0x24, immediate
        if(a!=0x24)throw std::runtime_error("Solo actor: unexpected ladder self action");
        if(current)current->delivered_actions.push_back(a);
        cancel_ladder();
    };
    request_actor_ladder_exit(actions,exit_fields,&*view,action,eh);
}
// 001ab9a0 (action 0x24).
void SoloActorSimulation::Impl::cancel_ladder(){
    ActorLadderCancelHost ch;
    ch.retained=[this]{return model->retained_motion();};
    ch.install=[this](const MotionRequest& r,MotionInstallMode m){model->install_motion(r,m);};
    cancel_actor_ladder(actions,&frame_state.history.pending,ch);
}
// Teleport Message 001bc4b0 (ACTOR_TELEPORT.md §2, native field map §4.1).
// The roots are the actor's root objects; the model anchors are the copies
// the next model evaluation starts from, so both receive the same operations.
// Everything up to the tail's actions is computed first and committed
// together (the model anchors are restored if a later commit is rejected).
SoloActorTeleportResult SoloActorSimulation::Impl::teleport(ActorTeleportMode mode,const ActorTeleportMatrix& matrix){
    const bool translates=actor_teleport_translates(mode),rotates=actor_teleport_rotates(mode);
    // 2.1: any parented root -> no write, no call.
    for(const auto r:{floor_root,edge_root,ladd_root})if(roots.node(r).parent)return {};
    // Action 10: the tail's landing 001a0b30(actor, 0); its hosting requirements are checked before any write.
    if((actions.active_actions&(1U<<10))&&life_state()!=2&&life_state()!=3){
        if(!model)throw std::runtime_error("Solo actor: the teleport's landing 001a0b30(actor, 0) needs an animation model");
        if(!model->retained_motion().node)throw std::runtime_error("Solo actor: the teleport's landing: retained motion (+326c) does not resolve");
        if(!model->default_motion_duration())throw std::runtime_error("Solo actor: the teleport's landing needs the model's default duration (0018a410)");
    }
    static constexpr RootAnchorKey keys[3]={SoloActorSimulation::flor_key,SoloActorSimulation::edge_key,SoloActorSimulation::ladd_key};
    const std::array<std::size_t,3> ids{floor_root,edge_root,ladd_root};
    std::array<RootAnimationState,3> root_states;
    std::array<std::optional<RootAnimationState>,3> model_states,model_before;
    for(std::size_t i=0;i<3;++i){
        root_states[i]=roots.node(ids[i]).local;
        if(model&&(i==0||model->has_anchor(keys[i])))model_before[i]=model_states[i]=model->anchor_state(keys[i]);
    }
    // 2.2: the controller delta stays the static zero vector unless a translation mode replaces it.
    RootVector delta{};
    auto next_component=component;auto next_published_component=published_component;auto next_published=published;
    auto next_integrator=frame_state.integrator;
    auto contact_40=frame_state.response.physical.contact_40,contact_70=frame_state.response.physical.contact_70;
    auto target_34c0=jump.target_34c0;
    if(translates){
        delta=actor_teleport_delta(mode,matrix,roots.world_placement(floor_root).position);
        for(std::size_t i=0;i<3;++i){
            translate_actor_teleport_root(root_states[i],delta);
            if(model_states[i])translate_actor_teleport_root(*model_states[i],delta);
        }
        next_integrator.position=add_actor_teleport_lanes(next_integrator.position,delta);                 // 001e9300
        next_integrator.previous_position=add_actor_teleport_lanes(next_integrator.previous_position,delta);
        contact_40=add_actor_teleport_lanes(contact_40,delta);contact_70=add_actor_teleport_lanes(contact_70,delta);
        // Model component virtual +24: the actor +50 translation (the model's
        // published placement and the physical response's publication are the
        // same component here).
        next_component=add_actor_teleport_xyz(next_component,delta);
        if(next_published_component)next_published_component->position=add_actor_teleport_xyz(next_published_component->position,delta);
        if(published_set||!frame_begun)next_published=add_actor_teleport_xyz(next_published,delta);   // before the first frame: the load placement
        target_34c0=add_actor_teleport_lanes(target_34c0,delta);                                           // +34c0
    }
    // 2.3: skipped when the flor weight compares equal to 0 (-0 included).
    auto next_heading=heading_state;
    const bool rotated=rotates&&root_weight(ActorRoot::floor)!=0.0F;
    if(rotated){
        const auto rotation=actor_teleport_rotation(mode,matrix,roots.world_placement(floor_root).orientation);
        if(next_heading.history.empty())throw std::runtime_error("Solo actor: teleport heading needs a history entry");
        // 00186be0 on the selected entry (+3098, constructor index 0), then
        // 00186b50 with -1: the curve target from the last entry.
        apply_actor_heading_state_command(next_heading,{ActorHeadingCommand::Kind::history,next_heading.selected,rotation.heading});
        ActorHeadingCommand target;target.kind=ActorHeadingCommand::Kind::target;target.duration=-1;
        apply_actor_heading_state_command(next_heading,target);
        rotate_actor_teleport_floor_root(root_states[0],rotation.orientation);
        if(model_states[0])rotate_actor_teleport_floor_root(*model_states[0],rotation.orientation);
        // Model component virtual +10: the rows of the new flor pose cache.
        if(next_published_component)next_published_component->orientation=root_states[0].pose_orientation;
    }
    // Commit.
    {
        const auto saved_roots=roots;
        try{
            for(std::size_t i=0;i<3;++i)if(model_states[i])model->set_anchor_state(keys[i],*model_states[i]);
            for(std::size_t i=0;i<3;++i)roots.set_local(ids[i],root_states[i]);
        }catch(...){
            for(std::size_t i=0;i<3;++i)if(model_before[i])model->set_anchor_state(keys[i],*model_before[i]);
            roots=saved_roots;
            throw;
        }
    }
    component=next_component;published_component=next_published_component;published=next_published;
    // 2.2 step 5 / 2.3 step 6: the ungated model context refresh 003a9220,
    // 003a9090 (001bc7c8, 001bc9f8). Its skeleton-space nodes are not touched
    // by the teleport, so one rebuild after the commit stores the same +50.
    if(translates||rotated)rebuild_model_centroid();
    frame_state.integrator=next_integrator;
    frame_state.response.physical.contact_40=contact_40;frame_state.response.physical.contact_70=contact_70;
    jump.target_34c0=target_34c0;
    heading_state=std::move(next_heading);
    // 2.4 tail. 1: the controller's virtual +50 (player: camera curves; AI: nothing).
    SoloActorTeleportResult result;result.applied=true;
    if(player()){
        result.camera_shift=delta;
        if(current)current->camera_shifts.push_back(delta);
    }
    frame_state.tail.reset_requested=1;                                                                  // physical +11
    if(actions.active_actions&(1U<<10))land_reset();                                                    // 3: 001a0b30(actor, 0)
    // 4: actions 7/8 -> self action 0x13 (hang drop), 5: action 16 without 15
    // -> self action 0x24 (ladder cancel), both with transport 1.
    if(actions.active_actions&((1U<<7)|(1U<<8))){
        if(current)current->delivered_actions.push_back(0x13);
        ActorHangActionInput in;in.life_state=life_state();in.byte_14=actions.byte_14!=0;in.component_is_player=player();
        in.receiver=handle;
        handle_actor_hang_drop(actions,in,[this]{fall();});
    }
    if((actions.active_actions&(1U<<16))&&!(actions.active_actions&(1U<<15))){
        if(current)current->delivered_actions.push_back(0x24);
        cancel_ladder();
    }
    // 6: force, acceleration, previous = position, velocity (the integrator
    // is the physical frame's source for +2f0..+330).
    auto& g=frame_state.integrator;
    g.force={0,0,0,0};g.acceleration={0,0,0,0};g.previous_position=g.position;g.velocity={0,0,0,0};
    // 7: +30a0, +30a4 = -FLT_MAX (ff7fffff), +30a8.
    jump.airtime_30a0=0;jump.fall_start_30a4=-std::numeric_limits<float>::max();jump.fall_height_30a8=0;
    return result;
}
// A box's hotbox event at the actor (001b3cb0): codes 8, 9, 0x0c..0x10 and
// 0x12 go synchronously to the +58 player component (00177380); actions 9/10
// of the zone helpers reach the actor at once (event byte 1, transport 1).
void SoloActorSimulation::Impl::deliver_actor_hotbox(const HotboxActorEvent& e){
    if(current)current->hotbox_codes.push_back(e);
    if(!actor_forwards_hotbox_code(e.code))return;
    PlayerButtonHost host;
    host.actor=[this]{return button_actor();};
    host.send=[this](std::int32_t a,bool alternate){send_pad_action(a,alternate);};
    host.interaction=[this]{return interact();};
    host.object_exists=object_exists;
    host.trigger_object=object_trigger;
    host.close_menu_for=pad_combat.pickup_menu.close_for;
    receive_player_hotbox_event(buttons,{e.code,e.payload,e.word_2c},host);
}
// 001714c0(tracker, actor, 1, 0) for the Triangle edge: the selection
// 00171260, the box's +0x114 (00171c90 queues a trigger to each link with
// transport 0) and the tracker timer 10.0 when a box was taken.
bool SoloActorSimulation::Impl::interact(){
    const auto r=interact_with_hotbox(hotbox_world,tracker,button_actor().mode_320c,multiplayer_host);
    for(const auto& t:r.triggers){
        QueuedMessage q;q.kind=QueuedMessage::Kind::trigger;q.trigger=t;queue.push_back(q);
    }
    if(r.box_message){                                     // 0017155c: HotboxMsg action 3 to the box, transport 0
        QueuedMessage q;q.kind=QueuedMessage::Kind::box;q.box=*r.box;q.payload=3;queue.push_back(q);
    }
    if(current){
        if(r.box)current->interaction_box=r.box;
        current->triggers.insert(current->triggers.end(),r.triggers.begin(),r.triggers.end());
    }
    return r.box.has_value();
}
// Action 0x17 (001a92a0 -> 00192800) and the self-sent take-off (001914c0).
void SoloActorSimulation::Impl::request_running_jump(const ActorJumpZoneMessage& m){
    std::optional<ActorJumpZoneTarget> target;
    // 0040c540: the key resolves only to a hosted box here (jump-zone links
    // must name hosted boxes), whose class word is the hotbox class.
    if(const auto i=hotbox_world.find(m.target_2c)){
        const auto& b=hotbox_world.boxes[*i];
        target=ActorJumpZoneTarget{b.record.minimum,b.record.maximum,b.world};
    }
    ActorJumpZoneActor a;
    a.byte_14=true;a.component_is_player=true;                         // 003cb0e0; player component
    a.flags_3204=words.current_3204;a.mode_320c=static_cast<std::int32_t>(mode());
    a.desired_31c0=controls.desired;a.flor=roots.world_placement(floor_root);
    const auto takeoff=request_actor_running_jump(m,a,target);
    if(!takeoff)return;
    if(current){current->running_jump=takeoff;current->delivered_actions.push_back(takeoff->action);}
    deliver_running_jump(takeoff->action,takeoff->payload);
}
// 001a92a0 -> 001914c0 for a CAS action 0x15 / 0x16 of kind 4.
void SoloActorSimulation::Impl::deliver_running_jump(std::int32_t action,const RootVector& payload){
    ActorJumpGrabRequest r;r.life_state=life_state();r.action=action;r.kind=4;r.payload=payload;
    r.lead_foot_31dd=lead_foot_31dd;r.receiver=handle;
    const auto out=handle_actor_jump_to_grab(actions,jump,r,hang_host());
    if(current)for(const auto& x:out.motions)current->ledge_motions.push_back(x.name);
}
// 002500c0 / 0024ffe0 on the pending set +0x40/+0x44: set or clear one bit and
// rescan bits 11..0 into +0x40 (-1 without one).
void SoloActorSimulation::Impl::set_pending_bit(std::uint32_t bit,bool set){
    if(set)buttons.pending_bits_44|=bit;else buttons.pending_bits_44&=~bit;
    std::int32_t top=11;
    while(top>=0&&!((buttons.pending_bits_44>>top)&1U))--top;
    buttons.pending_top_40=top;
}
// 0019f6c0 -> 001a2c00 (001b1bf4), before the channel producer 00199f30.
void SoloActorSimulation::Impl::update_air_state(float dt){
    auto jp=jump_physical();
    ActorAirHost air;
    air.launch=[&]{
        ActorJumpLaunchInput li;
        // Actor virtual +e4 (00132860) is 001e5880 on the model: the model
        // anchor through the component. Actor +50's translation is taken as
        // the model component's (inferred: the solo host has one transform
        // component; see HANGING.md).
        li.position=model_anchor_world();
        li.transform_position=component;
        // 001a2470: the kind's motion start placement (+3490 == 2:
        // DATA_ID_JumpGrabLedge, 1: animRunningJumpLand; other kinds throw).
        li.clip_start=model->motion_start_placement(jump.kind_3490==1?"animRunningJumpLand":"DATA_ID_JumpGrabLedge");
        li.flags_3248=actions.flags_3248;
        li.flor_orientation=roots.world_placement(floor_root).orientation;   // 00132400 -> 001323f0
        launch_actor_jump(jump,jp,li);
        if(current)current->jump_launched=true;
    };
    air.catch_ledge=[&]{catch_ledge(jp);};
    air.fall=[this]{fall();};
    air.land=[this]{land();};
    air.floor_z=[this]{return roots.world_placement(floor_root).position[2];};
    air.component_message=[this](std::int32_t code){
        queue_component(code);
        if(current)current->component_codes.push_back(code);
    };
    air.component_message_immediate=[this](std::int32_t code){
        if(current)current->component_codes.push_back(code);
        apply_component_code(code);
    };
    air.restore_inventory=[this]{restore_inventory();};               // 00143cf0
    air.leave_carry=[this]{leave_carry();};                            // 0019f8a4..0019f96c
    // 0019f450 runs with actor +14: virtual +100 (00187360) with category 1,
    // source = the actor, no penetration or hit point.
    air.damage=[this](std::int32_t type,float amount){apply_fall_damage(type,amount);};
    air.trace_fall_state=[this](const ActorFallStateTrace& t){if(current)current->fall_state=t;};
    air.land_dead=[this]{land_dead();};
    // 0019fb28: transport 1, event byte 1: the object's receiver (00216580) runs at once.
    air.object_code_immediate=[this](std::uint64_t object,std::int32_t code){
        if(std::find(object_action_receivers.begin(),object_action_receivers.end(),object)==object_action_receivers.end())
            throw std::runtime_error("Solo actor: 0019f890's CAS code to an object no object action named");
        if(current)current->object_codes.push_back({object,code});
        deliver_object_code(object,code);
    };
    ActorAirInput in;in.elapsed=dt;in.life_state=life_state();in.byte_14=true;in.component_is_player=player();
    in.health_40=health.health_40;in.health_3398=health.max_health_3398;in.byte_33a5=health.killed_33a5;
    with_live(jp,[&]{update_actor_air_state(actions,action_history,jump,jp,frame_state.edges.slots,in,air);});
    store_jump_physical(jp);
}

// The channel update's snapshot of this actor (00199f30's reads).
ActorAnimationSnapshot SoloActorSimulation::Impl::animation_snapshot(float dt){
    const auto& physical=frame_state.response.physical;
    // 001e96a0 (from 0019c0d4): with a water height +e4 (not the
    // sentinel *004a0438), +e4 minus the 'flor' root's world z (003ae5e0
    // 'flor' at 004a03c8, 00132400, 00132370 +8).
    std::optional<float> water_height;
    if(physical.e4!=actor_unavailable_height)water_height=physical.e4-roots.world_placement(floor_root).position[2];
    ActorAnimationSnapshot snap;auto& o=snap.ordinary;auto& d=o.direction;
    d.elapsed=dt;d.desired_x=controls.desired[0];d.desired_y=controls.desired[1];d.speed=controls.speed;
    for(std::size_t i=0;i<18&&i<model->channel_count();++i){
        d.current_channels[i]=model->channel(i).current;o.target_channels[i]=model->channel(i).target;
    }
    d.physical_present=true;d.contact_dc=physical.byte_dc!=0;d.facing_positive=facing_positive;
    d.flags3034=control_3030[1];                                 // +3034 (001b2680)
    d.flags3210=actions.active_actions;d.flags3248=actions.flags_3248;o.flags324c=actions.flags_324c;
    o.prior_direction=prior;o.desired_z=controls.desired[2];o.desired_w=controls.desired[3];
    o.turn=controls.turn;o.accumulation=accumulation;o.mode=mode();o.flags3204=words.current_3204;
    o.turn_correction_enabled=controls.turn_correction_31b3;o.turn_pose_enabled=controls.turn_pose_31b7;
    o.accumulate_enabled=controls.accumulate_31b4;o.contact_dd=physical.byte_dd!=0;o.physical_2c8=physical.blend_2c8;
    o.physical_height=water_height;
    snap.previous_flags=words.previous_3200;snap.frame_counter=*frame_counter;snap.source_id=1;
    snap.physical_3d2=frame_state.auxiliary.first_hit!=0;
    snap.actor_flag14=true;                                      // actor +14 (003cb0e0)
    // The +58 component's virtual +54: 1 for the player component,
    // 0 for an AI component (0012f370), which selects 001a80e0.
    snap.component_predicate=player();
    snap.physical_flags810=frame_state.edges.slots[6].mask;     // physical +810: slot 6 mask
    snap.running_value=running_31f8;snap.approach_step=approach_step_31e0;
    return snap;
}

// 0019dd80(actor, 1, 0, 0, 0): the neutral channels keeping channel 14.
void SoloActorSimulation::Impl::neutral_channels_1000(float dt){
    if(!model||!tuning)throw std::runtime_error("Solo actor: 0019dd80 needs an animation model and tuning");
    const auto plan=prepare_actor_neutral_channels(animation_snapshot(dt).ordinary,*tuning,approach_step_31e0,{true,false,false,false});
    model->apply_commands(plan.commands);
}

// 0019d950 (from 0019a9ac): the carrier drives its own channels 4 / 14 and
// the linked body's, sets both +3208 bit 0, then 0019dd80 on each.
void SoloActorSimulation::Impl::carry_pair_body(float dt){
    ActorCarryPairInput in;
    in.flags_324c=actions.flags_324c;
    const auto body=linked();in.linked=body.has_value();
    in.speed_31d0=controls.speed;in.desired_y_31c4=controls.desired[1];in.turn_31d4=controls.turn;
    const auto plan=plan_actor_carry_pair(in);
    if(!plan.carrying)return;
    if(!plan.linked){words.pending_3208&=~1U;neutral_channels_1000(dt);return;}
    auto& b=peer(*body);
    if(!b.model||!tuning)throw std::runtime_error("Solo actor: 0019d950 needs the linked body's animation model");
    const std::array<ActorChannelCommand,2> c{{{ActorChannelOperation::select,4,static_cast<float>(plan.selector_4)},
        {ActorChannelOperation::target,14,plan.channel_14,tuning->channel_timing[14],6.5F,false}}};
    model->apply_commands(c);
    b.model->apply_commands(c);
    if(plan.pending_bit0){words.pending_3208|=1U;b.words.pending_3208|=1U;}
    else{words.pending_3208&=~1U;b.words.pending_3208&=~1U;}
    neutral_channels_1000(dt);
    b.neutral_channels_1000(dt);
}

// Actor logic (actor +48 = 001b1710) in the first traversal.
void SoloActorSimulation::Impl::logic(const SoloActorFrameInput& input){
    const float dt=config.dt;
    SoloActorFrame& result=*current;
    // 001b1710 returns at once while +3269 is set (from the revive until the
    // RespawnTimer's ready stage 0).
    if(byte_3269!=0){result.logic_gated=true;return;}
    if(!model){
        if(input.pad)throw std::runtime_error("Pad control needs an animation model");
        if(!player())throw std::runtime_error("Solo actor: an AI controller needs an animation model");
        return;
    }
    {
        if(input.root_frame)throw std::runtime_error("Root frames are only used without an animation model");
        // Actor logic 00199f30: the channel update with its entry flag
        // bookkeeping and synchronous events (update_actor_animation_channels),
        // then the final heading. Mode +320c from the priority actions (0, or
        // 3 during a quick turn); the mask words +3248/+324c stay clear.
        // 0019f6c0 -> 001a2c00 (001b1bf4) precedes 00199f30.
        update_air_state(dt);
        if(grip)grip(dt);                                            // 001b1c28..001b1cec
        const auto& physical=frame_state.response.physical;
        ActorAnimationHost ah;
        ah.snapshot=[&]{return animation_snapshot(dt);};
        ah.apply=[&](const ActorAnimationPatch& patch){
            model->apply_commands(patch.commands);
            words.pending_3208=(words.pending_3208&~patch.pending_clear_bits)|patch.pending_set_bits;
            // Modes 7/8 (hang): shimmy side bits 0x100/0x200.
            actions.flags_3248=(actions.flags_3248&~patch.flags3248_clear_bits)|patch.flags3248_set_bits;
            if(patch.physical_request3d1)frame_state.auxiliary.first_requested=1;
            if(patch.physical_clear38_bit0)frame_state.history.pending_flags&=~1U;
            if(patch.physical_set38_bit0)frame_state.history.pending_flags|=1U;
            if(patch.approach_step)approach_step_31e0=*patch.approach_step;
            if(patch.accumulation)accumulation=*patch.accumulation;
            if(patch.desired_direction)controls.desired=*patch.desired_direction;
            if(patch.prior_direction)prior=*patch.prior_direction;
            if(patch.facing_positive)facing_positive=*patch.facing_positive;
        };
        ah.deliver=[&](const ActorAnimationEvent& event){
            // Stance messages (0019a700..0019a8f8, codes 2 then 3 on a rising
            // stance bit 4, 4 then 5 on a falling one) reach the +58 player
            // component at once (00176660). With actor +14 the low-ceiling
            // probe (+3d1/+3d2) can force the stance.
            if(event.kind==ActorAnimationEvent::Kind::stance){
                ++result.stance_events;
                result.component_codes.push_back(static_cast<std::int32_t>(event.action));
                // An AI component's receiver 00151d30 (key 0055a1d8 -> 00151a90).
                if(!player())controller.ai->controller_message_immediate(*owner,static_cast<std::int32_t>(event.action));
                else apply_component_code(static_cast<std::int32_t>(event.action));
            }else ++result.running_events;
        };
        ah.resolve_running_receiver=[]{return std::uint64_t{0};};
        // 001a80e0 for an AI component (0019a048..0019a0a0).
        if(!player())ah.navigation_body=[&](float){navigation_body();};
        // 0019a988..0019a9b4: +324c bit 3 or 6 runs 0019d950.
        ah.paired_body=[&]{carry_pair_body(dt);result.carry_pair_body=true;};
        // Mode 16: 0019a978 calls the ladder body 00474050 on the live fields.
        ah.ladder_body=[&](float elapsed){
            ActorLadderBodyFields f;
            f.pending_3208=words.pending_3208;f.slide_31d8=slide_31d8;f.slide_enabled_31b5=byte_31b5;
            f.exit_requested_31b6=exit_fields.requested_31b6;f.vertical_31c8=controls.desired[2];
            const auto view=ladder_view();
            run_actor_ladder_body(actions,f,&*view,transition_2ef8,handle,elapsed,ladder_body_host(ah));
            words.pending_3208=f.pending_3208;slide_31d8=f.slide_31d8;byte_31b5=f.slide_enabled_31b5;
            exit_fields.requested_31b6=f.exit_requested_31b6;
            result.ladder_body=true;
        };
        update_actor_animation_channels(ah,*tuning);
        // Final heading (00199f30 tail): heading from the current flor world
        // rotation plus turn*elapsed, placed through 0019d3b0 on root +3290
        // (flor), then the caller clears root +160 (normalization counter).
        {
            ActorHeadingHost hh;
            hh.snapshot=[&]{
                ActorHeadingSnapshot snap;auto& h=snap.heading;
                h.world_rotation=roots.world_placement(floor_root).orientation;
                h.history=heading_state.history;h.selected=heading_state.selected;
                h.turn=controls.turn;h.elapsed=dt;h.mode=mode();
                h.physical_present=true;h.contact_dd=physical.byte_dd!=0;h.flags3204=words.current_3204;
                h.flags3210=actions.active_actions;h.flags3248=actions.flags_3248;
                // Saved yaw: the retained angle curve +3050 current. Modes 4/5
                // with +3248 bit 10: +3234/+3220 tops, the resolved 'edge' root
                // orientation and 0049e9a0 (0 after initialization, no store found).
                h.saved_yaw=heading_state.curve.current;
                h.transition_previous=static_cast<std::uint32_t>(action_history.older_3234.top);
                h.transition_current=static_cast<std::uint32_t>(action_history.newer_3220.top);
                h.transition_rotation=roots.world_placement(edge_root).orientation;
                h.euler_negate=false;
                snap.ladder_reference_rotation=roots.world_placement(edge_root).orientation;   // mode 16: resolved +3294
                return snap;};
            hh.apply=[&](const ActorHeadingCommand& c){
                using K=ActorHeadingCommand::Kind;
                if(apply_actor_heading_state_command(heading_state,c))return;
                if(c.kind==K::stationary_latch){latch_303c=c.value!=0?1:0;return;}   // +303c
                if(c.kind==K::world_orientation){
                    model->set_anchor_world_orientation(flor_key,c.rotation,false);
                    roots.set_world_orientation(floor_root,c.rotation,false);
                    heading_orientation=c.rotation;
                }else if(c.kind==K::clear_root_160&&heading_orientation){
                    // Unparented flor: re-applying the same world rotation with the
                    // counter reset only clears +160.
                    model->set_anchor_world_orientation(flor_key,*heading_orientation,true);
                    roots.set_world_orientation(floor_root,*heading_orientation,true);
                }
            };
            update_actor_heading(hh);
        }
        // 0x001b1d10: 00197e00's gait (footstep hooks), then its item triggers (the combat host's, when set).
        if(footsteps.materials)footstep_gait();
        if(item_triggers)item_triggers(dt);
        // 001b1d20..1d64, after 00199f30/00199980/00197e00: +3200 = +3204, +3204 = +3208.
        words.previous_3200=words.current_3204;words.current_3204=words.pending_3208;
        // 001b1d74..001b1de4 have no hosted effect (research/COMBAT.md
        // "Checkpoint 120c"). 001b1de8..1e04: while actor +14 is set, the
        // checkpoint component +33f0's vtable +48 = 0022e900(dt): the label
        // (5 s after MSGGAME_ADD_CHECKPOINT) for the text manager's
        // immediate list; the font byte 004f7960 stays 0 (.bss; the
        // missions never call Script_SetCheckpointFont 0022ed00).
        // 0x001b1e10..0x001b2214: the burning actor (the combat host's, when set).
        if(actions.byte_14!=0){
            auto label=update_checkpoint_label(checkpoint,dt,0);
            if(label&&current)current->checkpoint_label=std::move(label);
        }
        if(fire.update)fire.update(dt);
        // 001b2218..2314: the retained angle curve +3050 advances (0017dff0).
        // Its position part (actor byte +31b0) is not hosted: the solo host
        // has no writer of +31b0 (HANGING.md, open).
        step_animation_rotation_curve(heading_state.curve,dt);
        // 001b2680 / 001b2670: +3030 = +3034, +3034 = +3038, +3038 = 0.
        control_3030[0]=control_3030[1];control_3030[1]=control_3030[2];control_3030[2]=0;
        // 0x001b2328..0x001b237c: the blindness / flash decay (the combat host's, when set).
        if(sense_decay)sense_decay(dt);
        if(!player()){
            // The AI component's +40 (00159590) where the player component
            // runs 0024b5d0.
            if(input.pad)throw std::runtime_error("Solo actor: pad input for an actor with an AI controller");
            controller.ai->update(*owner,dt);
        }else if(input.pad){
            // The attached player input runs inside actor logic 001b1710,
            // before model evaluation (actor virtual +50, 00392aa0).
            // 0024b5d0 reads actor +3204 bit 0 (0019d760 / 001ae080); the group
            // sends action 3 and 4 (payload 1, +31b3/+31b4) and kind 12.
            auto pad=*input.pad;pad.elapsed=dt;pad.actor_flag3204_bit0=(words.current_3204&1U)!=0;
            // 0x0024bda4..0x0024be2c (before the shift): sp+1e0 = (+38 == 8)
            // reads the previous frame's selector; with it set the weapon
            // block and the crosshair read the distance bytes sp+1f0 / sp+200.
            const bool aim_1e0=selector_38==8;
            PlayerAimViewDistance distance;
            if(aim_1e0){
                if(!input.camera_boom_934||!input.camera_9c4)
                    throw std::runtime_error("Solo actor: the aim view's distance test 0x0024bdc0 (camera +934, 0017a610 camera +9c4) has no camera");
                distance=player_aim_view_distance(*input.camera_boom_934,*input.camera_9c4);
            }
            const auto combat_fields=[&]{
                PadCombatFields f{selector_38,buttons.current_bits_3c,buttons.pending_top_40,buttons.pending_bits_44,buttons};
                f.aim_1e0=aim_1e0;f.near_1f0=distance.near_1f0;f.far_200=distance.far_200;
                return f;
            };
            // 0x0024c338..c348: +0x30 <- +0x38 <- +0x40 (00250280: selected value
            // and the twelve flag bits).
            previous_selector_30=selector_38;
            selector_38=buttons.pending_top_40;
            buttons.current_bits_3c=(buttons.current_bits_3c&~0xfffU)|(buttons.pending_bits_44&0xfffU);
            result.pad_selector=selector_38;
            // 0x0024c3d8..0x0024c6a4: the pickup menu (input +128) and the
            // Cross latch +2d. Triangle's close zeroes its sp+fa4 (0x0024c4ec),
            // so the button tail below sees it released.
            float triangle_fa4=input.triangle;
            const auto menu_open=[this]{return pad_combat.pickup_menu.open&&pad_combat.pickup_menu.open();};
            if(menu_open()){
                auto& m=pad_combat.pickup_menu;
                const auto need=[](const auto& f,const char* what){
                    if(!f)throw std::runtime_error(std::string("Solo actor: the pickup menu's ")+what+" is not hosted");};
                const bool up=!(input.dpad_up_fb0==0.0F),down=!(input.dpad_down_fb4==0.0F);
                const bool left=!(input.dpad_left_fb8==0.0F),right=!(input.dpad_right_fbc==0.0F);
                if(up&&!dpad_latch_b0[0]){need(m.previous,"00251690");m.previous();}
                else if(down&&!dpad_latch_b0[1]){need(m.next,"002517d0");m.next();}
                else if(left&&!dpad_latch_b0[2]){need(m.left,"00251370");m.left();}
                else if(right&&!dpad_latch_b0[3]){need(m.right,"00251500");m.right();}
                if(!(triangle_fa4==0.0F)&&!buttons.triangle_held_ad){
                    buttons.triangle_held_ad=true;                           // 0x0024c4e4
                    need(m.close,"00247320");m.close();                      // 0x0024c4e0
                    triangle_fa4=0.0F;                                       // 0x0024c4ec
                    result.pad_reset=true;                                   // 0x0024c4f0: 36a240(0055a300)
                    need(m.confirm_sound,"GS_CONFIRM 001ca010 / 003c9420");m.confirm_sound();
                }else if(!(input.cross==0.0F)){
                    buttons.cross_latched_2d=true;                           // 0x0024c5f8
                    if(!buttons.cross_held_ac){
                        buttons.cross_held_ac=true;                          // 0x0024c608
                        need(m.confirm,"00251920");
                        if(m.confirm()){need(m.close,"00247320");m.close();} // 0x0024c610 / 0x0024c620
                    }
                }
            }
            if(input.choose_menu_1c0){                                       // 0x0024c628
                if(!choose_menu_track)
                    throw std::runtime_error("Solo actor: the choose menu update 0045bd30 (HUD +6a4, 0x0024c674) is not hosted");
                choose_menu_track(published_set?published:component);      // actor +50 +40 (0x0024c660)
            }else if(input.cross==0.0F)buttons.cross_latched_2d=false;      // 0x0024c688..0x0024c6a0
            // 0x0024ce5c / 0x0024cec0: the HUD target-marker pass 0022c210 and the
            // targeting function 00247d60 (the combat host's stage, when set).
            if(pad_combat.targeting){
                auto fields=combat_fields();
                pad_combat.targeting(fields);
            }
            // 0x0024cef0..0x0024cf8c: Circle (sp+f98) sends action 8, the
            // directional roll 001aa468, at once (003c9420 transport 1) unless
            // current priority bit 6, 8 or 1 is set; every frame it is held.
            if(player_circle_roll_request(true,buttons.current_bits_3c,input.circle))deliver_roll();
            // The selector-8 sends 0x0024cf90..0x0024d14c (actions 0xb/0xc by
            // sp+fc4/sp+fc0 with +38 = 8, else 0xd; event byte 1, transport 0:
            // 001aaae0/001aab50/001aabc0 set/clear +3248 bits 30/31) are not
            // hosted; no native code reads those bits (COMBAT.md "Circle roll").
            // 0x0024d150: inside a jump zone (+0x26) the pad queues action 0x17
            // (00195c80 kind 6, +0x2c the target key from +0x28, +0x30 the side
            // +0x27, event byte 1, transport 0) every frame.
            if(buttons.jump_zone_26){
                QueuedMessage q;q.kind=QueuedMessage::Kind::action;q.code=0x17;
                q.jump_zone=ActorJumpZoneMessage{6,buttons.jump_target_28,buttons.jump_side_27};
                queue.push_back(q);result.queued_actions.push_back(0x17);
            }
            // 0x0024d1c8..0x0024d234: pending bit 10 in actor mode 1, bit 9 in
            // modes 4/5 (the next frame's selector).
            set_pending_bit(0x400U,mode()==1);
            set_pending_bit(0x200U,mode()==4||mode()==5);
            // 0x0024d25c: the quick-turn detector runs before the selector
            // dispatch, with the previous frame's physical +dc.
            PlayerQuickTurnInput turn;turn.left=pad.left;turn.previous_left_60=previous_left_60;
            turn.elapsed=dt;turn.physical_unsupported_dc=frame_state.response.physical.byte_dc!=0;
            turn.actor_mode_320c=mode();turn.selector_38=static_cast<std::uint32_t>(selector_38);
            result.quick_turn=update_player_quick_turn(quick_turn,turn);
            if(result.quick_turn){
                // 003c9420 delivers action 14 immediately (001a92a0 -> 001aac10).
                ActorQuickTurnHost host;
                host.resolve=[this](std::string_view name){return model->resolve_motion(name);};
                host.retained=[this]{return model->retained_motion();};
                host.install=[this](const MotionRequest& r,MotionInstallMode m){model->install_motion(r,m);};
                const auto handled=handle_actor_quick_turn(actions,words.current_3204,handle,host);
                if(handled.handled)result.quick_turn_motion=handled.motion;
            }
            PlayerMoveCommand command;
            std::array<float,4> rates_f30{};                               // sp+f30 (0x0024c1fc)
            bool byte_1b0=false;float yaw_f21=0.0F;
            // 002503e0(sp+…, sp+f60, lbu +125, lbu +126) -> sp+ed0, the camera's +d30
            // (00247800): selectors 0/4 at 0x0024d2b4, 11 at 0x0024e530, 1/2/3/9/10 at
            // 0x0024e560, 7 at 0x0024e590 and 6 at 0x0024e7c8 (jtbl_004a7800).
            const auto free_look=[&]{
                const auto d30=player_camera_look_offset(input.right_stick[0],input.right_stick[1],
                    free_look_x_125!=0,free_look_y_126!=0);
                result.camera_look_offset_d30=std::array<float,4>{d30[0],d30[1],d30[2],d30[3]};
            };
            if(selector_38==0||selector_38==4){
                free_look();                                               // 0x0024d2b4
                if(selector_38!=0)pad_state.backward_2c=false;             // 0x0024d274
                command=prepare_player_move(pad_state,pad);
                rates_f30[0]=-0.0F;                                        // -sp+cf4 (0x0024d978; cf0 zeroed at 0x0024d2a8)
            }else if(selector_38==7){                                      // 0x0024e590
                free_look();
                const auto hang=prepare_player_hang_move(pad_state,pad.left);
                command=hang.command;
                if(hang.queued_action>=0){
                    queue_action(hang.queued_action);
                    result.queued_actions.push_back(hang.queued_action);
                }
            }else if(selector_38==6){                                      // 0x0024e7c8 (ladder)
                free_look();
                const auto ladder=prepare_player_ladder_move(pad_state,pad,input.cross);
                command=ladder.command;
                if(ladder.exit_action>=0){
                    result.ladder_exit_requests.push_back(ladder.exit_action);
                    // Immediate (event byte 1, transport 1): 001a92a0 -> 001ab990 -> 00472e10 type 0.
                    request_ladder_exit(ladder.exit_action);
                }
            }else if(selector_38==1||selector_38==2||selector_38==3||selector_38==9||selector_38==10||selector_38==11){
                // 0x0024e560; selector 11 (pending bit 11, set by the death
                // notice 00176540) is 0x0024e530, the same body.
                free_look();
                command=prepare_player_idle_move(pad_state);
            }else if(selector_38==5){                                      // 0x0024dc28 (the R1 pick held)
                if(!input.camera_rows_50)
                    throw std::runtime_error("Solo actor: the lock movement 0024dc28 has no camera (+50 transform)");
                if(!pad_combat.pick_resolves)
                    throw std::runtime_error("Solo actor: the lock movement resolves the pick +c8 (0014d350 at 0x0024dc5c; no host)");
                PlayerLockMoveInput lock;
                lock.row0=(*input.camera_rows_50)[0];lock.row1=(*input.camera_rows_50)[1];
                if(pad_combat.pick_resolves()){
                    // 0x0024dc7c: the actor's virtual +f4 (the 'flor' placement), rows 0 and 1.
                    const auto m=root_placement_matrix(roots.world_placement(floor_root));
                    lock.actor_rows=std::array<std::array<float,4>,2>{
                        std::array<float,4>{m[0],m[1],m[2],m[3]},std::array<float,4>{m[4],m[5],m[6],m[7]}};
                }
                lock.left=pad.left;
                lock.pressure_10=pad.pressure_10;lock.pressure_11=pad.pressure_11;
                lock.bits_3c_bit1=((buttons.current_bits_3c>>1)&1U)!=0;     // 00250350(+38, 1)
                lock.actor_flag3204_bit0=(words.current_3204&1U)!=0;       // 001ae080(+3200, 0)
                const auto move=prepare_player_lock_move(pad_state,lock);
                command=move.command;
                byte_1b0=true;yaw_f21=move.yaw;
                result.camera_look_offset_d30=std::array<float,4>{};       // sp+ed0 = 004f7e20 (0x0024c310; 0x0024dc28 leaves it)
            }else if(selector_38==8){                                      // 0x0024e1d0 (the aim view)
                if(!input.camera_rows_50||!input.camera_half_fov_60)
                    throw std::runtime_error("Solo actor: the aim-view movement 0024e1d0 has no camera (+50 transform, +60)");
                PlayerAimViewMoveInput aim;
                aim.row0=(*input.camera_rows_50)[0];aim.row1=(*input.camera_rows_50)[1];
                aim.half_fov_60=*input.camera_half_fov_60;
                aim.left=pad.left;aim.right=input.right_stick;
                aim.invert_124=manual_aim_124!=0;                          // 0x0024e3a8: lbu +124 (the manual aim option)
                aim.bits_3c_bit1=((buttons.current_bits_3c>>1)&1U)!=0;     // 00250350(+38, 1)
                aim.previous_90=look_90;aim.elapsed=dt;
                const auto move=prepare_player_aim_view_move(pad_state,aim);
                command=move.command;rates_f30=move.rates_f30;
                byte_1b0=true;yaw_f21=move.yaw;
                look_90=move.look_ee0;                                     // 0x0024fa74 (stored at the pad's end)
                result.camera_look_offset_d30=std::array<float,4>{};       // sp+ed0 = 004f7e20 (0x0024c310)
            }else throw std::runtime_error("Solo actor: pad selector "+std::to_string(selector_38)+" is not hosted");
            if(selector_38!=8)look_90=rates_f30;                           // 0x0024fa88: +90 = sp+f30
            // 0x0024eb78: 00247410 (the aim heading entry of the stack +3088).
            update_actor_aim_heading(heading_state,heading_entry_20,previous_selector_30,selector_38,byte_1b0,yaw_f21);
            result.camera_rates_d60=player_camera_rates_d60(rates_f30);  // 00247800 at 0x0024ec14
            previous_left_60={pad.left[0],pad.left[1],0,0};
            controls.turn_correction_31b3=true;controls.accumulate_31b4=true;
            controls.desired=command.direction;controls.speed=command.speed;controls.turn=command.turn;
            controls.turn_pose_31b7=command.turn_in_place;
            result.pad=command;
            // 0x0024ec14: 00247800 camera bits 0x400/0x800 (selector 4).
            if(selector_38==4){
                const bool low=((words.current_3204>>4)&1U)&&frame_state.auxiliary.first_hit!=0;   // +3204 bit 4, physical +3d2
                if(!low&&buttons.zone_24)result.camera_crouch_flags|=0x400U;
                if(low)result.camera_crouch_flags|=0x800U;
            }
            // 0x0024ec14: 00247800 camera bits 2/3 (input +3c bit 8, 00247b20;
            // bit 3 unless the held class 001437a0(+3524) is 1).
            result.camera_aim_flags=player_camera_aim_flags_cf8(buttons.current_bits_3c,pad_combat.hold_class);
            result.camera_aim_flags|=player_camera_pick_flag_cf8(buttons.current_bits_3c);   // 00247b68: bit 1 (2)
            // 0x00247cd8..0x00247d14: camera +d90 = 001ca840(+3314) or zero.
            if(pad_combat.secondary_250){
                const auto q=pad_combat.secondary_250();
                result.camera_rotation_d90=q?camera_recoil_rotation(*q):RootVector{};
            }
            // 0x00247844..0x00247990: the lock and +d0c (bit 8 set: none, -1).
            if(((buttons.current_bits_3c>>8)&1U)==0&&pad_combat.camera_lock){
                const auto lock=pad_combat.camera_lock();
                result.camera_lock=lock.lock;
                result.camera_subtarget_d0c=lock.subtarget_e4;
            }
            // 0x0024ec1c..0x0024eea0: the interact prompt.
            if(pad_interact_prompt){
                PadInteractPromptInput p;
                // sp+f84: pad word 0055a490 (D-pad Left pressure), zeroed with priority bit 0xb (0x0024c0e8 / 0x0024c168).
                const float f84=((buttons.current_bits_3c>>11)&1U)!=0?0.0F:input.dpad_left_f84;
                p.pressure_f84=f84;
                p.previous_a5=dpad_left_a5;                                // input +a5: the previous frame's sp+f84 != 0
                p.object_128=menu_open();                                  // input +128: the pickup menu (component +178)
                p.word_130=found_ledge_130!=0;                             // input +130 (component +180): the "found ledge" mask, 001777e8
                // HUD +6a8 (the start menu, 002429c8 / 00242ae0) and +6a9 (the map, 00242850 / 002428e4) from the
                // GUI; +6aa's writers 002426e0 / 00242740 are not hosted (0 from the constructor, 0x002460c4).
                p.hud_6a8=input.hud_6a8;p.hud_6a9=input.hud_6a9;
                p.player_actor=true;
                p.linked_model_3478=linked().has_value();                  // 001adaf0(+3478): the carried body (001adab0)
                p.bits_3c_bit1=((buttons.current_bits_3c>>1)&1U)!=0;       // 00250350(+38, 1)
                p.bits_3c_bit8=((buttons.current_bits_3c>>8)&1U)!=0;       // 00250350(+38, 8)
                p.mode_320c=static_cast<std::int32_t>(mode());
                p.flags_324c=actions.flags_324c;
                pad_interact_prompt(p,[this]()->std::optional<PadInteractBox>{
                    const auto i=select_interaction_hotbox(hotbox_world,tracker,static_cast<std::int32_t>(mode()));
                    if(!i)return std::nullopt;
                    const auto& b=hotbox_world.boxes[*i];
                    return PadInteractBox{b.record.type,b.interact_message_a0};});
                dpad_left_a5=!(f84==0.0F);
            }
            // 0x0024f1c8..0x0024f810: the weapon block (the combat host's stage, when set).
            if(pad_combat.weapon){
                auto fields=combat_fields();
                pad_combat.weapon(fields);
            }
            // 0x0024f810..0x0024fa34: Triangle, Cross and the passive edge
            // slots 7/8, after the movement message.
            PlayerButtonTailHost bh;
            bh.buttons.actor=[this]{return button_actor();};
            bh.buttons.send=[this](std::int32_t a,bool alternate){send_pad_action(a,alternate);};
            bh.buttons.interaction=[this]{return interact();};
            bh.latch_passive=[this](std::size_t i){latch_actor_edge_request(frame_state.edges.slots.at(i),true);};
            bh.aim.camera_moving=[&input]{
                if(!input.camera_9c4)throw std::runtime_error("Solo actor: the aim view's camera test 0017a610 (camera +9c4) has no camera");
                return !(*input.camera_9c4==0.0F);};
            bh.aim.set_byte_2e00=[this](std::uint8_t v){byte_2e00=v;};
            PlayerButtonTailInput tail{triangle_fa4,input.cross,actions.flags_3248};
            tail.l1=input.l1;tail.r1=input.r1;tail.selector_38=selector_38;tail.previous_selector_30=previous_selector_30;
            tail.object_128=menu_open();
            run_player_button_tail(buttons,tail,bh);
            // 0x0024f928..0x0024f964: the crosshair 00241ec0 (between the L1/R1
            // handlers and Cross/Triangle of the tail; its inputs, the held
            // record, +346f, sp+1e0 and sp+200, are not written by the tail).
            if(pad_combat.crosshair){
                auto fields=combat_fields();
                pad_combat.crosshair(fields);
            }
            // 0x0024f968..0x0024fa08: the field of view 00142f20(+3524,
            // +38 == 8, zoom, camera +c64, elapsed), clamped, to camera
            // +c64 and (0024fc20) +c68.
            if(pad_combat.field_of_view){
                if(!input.camera_fov_c64)throw std::runtime_error("Solo actor: the field of view 00142f20 (0x0024f9ac) reads camera +c64, which has no camera");
                WeaponFieldOfViewInput fov;
                fov.aim=selector_38==8;
                fov.zoom=!(input.zoom_in_fc8==0.0F)?1:(!(input.zoom_out_fcc==0.0F)?-1:0);   // 0x0024f968..0x0024f9a0
                fov.current_c64=*input.camera_fov_c64;fov.elapsed=dt;
                result.camera_field_of_view=pad_field_of_view_clamp(pad_combat.field_of_view(fov));
            }
            buttons.zone_previous_25=buttons.zone_24;   // 0x0024fa50
            // 0x0024faa8: the end latch of sp+fb0..sp+fbc (+b0..+b3).
            dpad_latch_b0={!(input.dpad_up_fb0==0.0F),!(input.dpad_down_fb4==0.0F),
                           !(input.dpad_left_fb8==0.0F),!(input.dpad_right_fbc==0.0F)};
        }
        // 001b1d6c: the weapon inventory update 00144080 (the combat host's
        // stage, when set), after the component update of actor logic.
        if(inventory_update)inventory_update(dt);
    }
}

// The model/physical phase (actor +50 = 00392aa0) in the second traversal.
void SoloActorSimulation::Impl::physics(const SoloActorFrameInput& input){
    const float dt=config.dt;
    SoloActorFrame& result=*current;
    OMEGA_ZONE_LAPS(physics_laps);   // port-only (132vt2d, zone_profile.hpp): actor.physics's parts on the Vita
    if(model){
        model->set_anchor_constraint(flor_key,RootMotionConstraint{RootConstraintKind::reject_axis,frame_state.response.physical.up_axis_9f0});
        sync_external_parents();
        model->step(dt);
        OMEGA_ZONE_LAP(physics_laps,"physics.model_step");
        roots.set_local(floor_root,model->anchor_state(flor_key));
        // The model's 'edge' anchor is actor root +3294.
        if(model->has_anchor(edge_key))roots.set_local(edge_root,model->anchor_state(edge_key));
        // The model's 'ladd' anchor is actor root +3298.
        if(model->has_anchor(ladd_key))roots.set_local(ladd_root,model->anchor_state(ladd_key));
        for(std::size_t i=0;i<18&&i<model->channel_count();++i)result.channels[i]=model->channel(i).current;
        published_component=model->published_placement();
        component=published_component?published_component->position:roots.world_pose(floor_root).position;published_set=false;frame_begun=true;response_ran=false;
    }else{
        if(!input.root_frame)throw std::runtime_error("A root frame is required without an animation model");
        auto local=roots.node(floor_root).local;
        // Root output 003af400 passes the motion through root +164 virtual +0c.
        // Activation binds "flor" to physical +9e0 (001b8c00): reject the
        // component along its retained axis +9f0, which 001e2180's tail sets
        // to the contact normal (ANIMATION_CLIPS.md).
        finalize_root_animation(local,*input.root_frame,
            {RootConstraintKind::reject_axis,frame_state.response.physical.up_axis_9f0});
        roots.set_local(floor_root,local);
        component=roots.world_pose(floor_root).position;published_set=false;frame_begun=true;response_ran=false;
    }
    OMEGA_ZONE_LAP(physics_laps,"physics.roots");
    // 001811c0: bounds (001ea240) and gather run before 001e9980, so they see
    // the previous frame's +2f0/+300.
    {
        ActorGatherBoundsInput bounds_in;
        bounds_in.mode_20=frame_state.history.current.value;            // 001ea240: mode 9 uses -8..+14
        bounds_in.predicted_2f0=frame_state.integrator.position;
        bounds_in.position_300=frame_state.integrator.previous_position;
        bounds_in.floor_root_position=roots.world_placement(floor_root).position;
        for(std::size_t i=0;i<9;++i)bounds_in.slot_3e0[i]=frame_state.edges.slots[i].request;
        // The bytes 001ea240 reads first: slot 4 (+660), slot 3 (+5c0), the
        // second probe request +3d3 and slot 1 (+480).
        bounds_in.byte_660=frame_state.edges.slots[4].request;bounds_in.byte_5c0=frame_state.edges.slots[3].request;
        bounds_in.byte_3d3=frame_state.auxiliary.second_requested!=0;bounds_in.byte_480=frame_state.edges.slots[1].request;
        gather(actor_gather_bounds(bounds_in));
    }
    OMEGA_ZONE_LAP(physics_laps,"physics.gather");
    result.trace=run_actor_physical_frame(frame_state,dt,frame_host);response_ran=true;
    OMEGA_ZONE_LAP(physics_laps,"physics.physical_frame");
    // 001e40ec: a floor-probe hit stores its triangle info (001e1b70) at physical +100: the surface
    // record (001e1dc0: COL +2c[index], a static default before version 3) at +140, its byte 1 the
    // material index of the COL fixup 003c20c8.
    if(footsteps.materials&&result.trace.response&&result.trace.response->floor){
        const auto& source=result.trace.response->floor->source;
        const auto bytes=world().geometry(source.geometry);
        if(bytes.size()<4||bytes[3]<3)throw std::runtime_error("Solo actor: the pre-version-3 COL default surface (001e1dc0) is not hosted");
        floor_surface_141=material_surface_index(*footsteps.materials,world().refresh(source).surface);
    }
    // The physical response published the "flor" root; the model's anchor is that root.
    if(model)model->set_anchor_world_position(flor_key,roots.world_placement(floor_root).position);
    if(model&&model->has_anchor(edge_key))model->set_anchor_world_placement(edge_key,roots.world_placement(edge_root),false);
    result.water_events=water_events;
    result.found_ledge=found_ledge_130;
    // 00392aa0: the secondary component's update follows the physical step.
    if(secondary_update)secondary_update(dt);
    OMEGA_ZONE_LAP(physics_laps,"physics.secondary");
    // Then, with the node collection (actor +2e64), the context bounds 003a9220
    // (not retained here) and centroid 003a9090 (392af0..392b08).
    rebuild_model_centroid();
    OMEGA_ZONE_LAP(physics_laps,"physics.centroid");
}

// The frame-update event after the world update.
void SoloActorSimulation::Impl::frame_event(){
    SoloActorFrame& result=*current;
    update_actor_fader(fader,config.dt);
    result.fade_alpha=actor_fader_alpha(fader);
    // The frame-update event (005436b0, after the pump and the object update
    // in 00167c40) runs the tracker handler 00171600 for the actor (+14 set):
    // the actor +50 translation is the component the physical response
    // published this step (the model component otherwise; actor +50 is taken
    // as the model component, see HANGING.md). Its events are queued.
    if(!hotbox_world.boxes.empty()){
        const RootVector p=published_set?published:component;
        for(const auto& e:update_hotbox_tracker(hotbox_world,tracker,p)){
            QueuedMessage q;q.kind=QueuedMessage::Kind::box;q.box=e.box;q.payload=e.payload;q.tracker_event=e;
            queue.push_back(q);result.hotbox_events.push_back(e);
        }
    }
}

void SoloActorSimulation::Impl::advance_respawn_poll(){
    if(!advance_actor_respawn_poll(respawn_poll,config.dt*1000.0F,fader))return;
    QueuedMessage message;message.kind=QueuedMessage::Kind::respawn;queue.push_back(message);
}

// 00186360(actor, 0, 0, 0, 0, -1.0), fire off (actor_ignition.hpp): on an
// actor that is not burning it switches the effect slots 0 / 1 off
// (00185d40, with the G_TASER_BODY stop) and stops G_TASER_BODY. The slots
// are the combat host's (CombatThrows); they are empty on an actor that is
// not burning, since in play only the ignition fills slot 0 with +326a, so
// the call is counted and the two sound stops are not emitted. A burning
// actor goes to the fire hooks' extinguish (the full 00186360).
void SoloActorSimulation::Impl::extinguish(){
    if(health.burning_326a){
        if(!fire.extinguish)throw std::runtime_error("Solo actor: extinguishing a burning actor (00186360) is not hosted");
        fire.extinguish();
    }
    if(current)++current->extinguish_calls;
}

// RespawnTimer 001ba130 on the script clock (milliseconds, clock 1). It runs
// before the step's queued messages, so the event the revive schedules in
// this step's delivery is first advanced by the next step (inferred order).
// The event acts only in game state 004ffba0 = 15 (001ba14c; a level end
// moves it to 0x13 and the event then ends without re-arming); zone readiness
// 00224e00 is true in solo (inferred, ACTOR_TELEPORT.md §5).
void SoloActorSimulation::Impl::advance_respawn_timer(){
    if(!advance_actor_respawn_timer(respawn_timer,config.dt*1000.0F))return;
    const bool state_15=game_end.state_50==15;
    if(state_15&&respawn_timer.stage==0&&health.burning_326a&&!fire.extinguish)
        throw std::runtime_error("Solo actor: extinguishing a burning actor (00186360) is not hosted");
    if(current)current->respawn_timer_stage=respawn_timer.stage;
    const auto e=fire_actor_respawn_timer(respawn_timer,state_15,true,fader,health.invulnerable_33a4,byte_3269);
    if(e.extinguish)extinguish();
    if(current){
        current->fade_in=current->fade_in||e.fade_in;current->screen_effect=current->screen_effect||e.screen_effect;
        current->invulnerability_ended=current->invulnerability_ended||e.invulnerability_ended;
    }
}

// Revive 001898f0 (ACTOR_TELEPORT.md §5-6, NPC_ACTIVATION_AND_RESPAWN.md §6),
// in the interpreter's order. Hosting requirements are checked before the
// first write.
void SoloActorSimulation::Impl::revive(){
    if(!model)throw std::runtime_error("Solo actor: the revive needs an animation model");
    if(health.burning_326a&&!fire.extinguish)throw std::runtime_error("Solo actor: extinguishing a burning actor (00186360) is not hosted");
    const auto retained=model->retained_motion();                                       // +326c, request tag 2
    if(!retained.node)throw std::runtime_error("Solo actor: the revive's retained motion (+326c) does not resolve");
    std::optional<float> duration;
    if(!transition_2ef8.pending){
        duration=model->default_motion_duration();                                       // 0018a410
        if(!duration)throw std::runtime_error("Solo actor: the revive needs the model's default duration (0018a410)");
    }
    // 1. Action 0x0a, kind 0, to self at once: 001a92a0 -> 001ae2a0(+3248, 1, 0).
    if(current)current->delivered_actions.push_back(0x0a);
    actions.flags_3248&=~2U;
    // 2. 003ad500: the retained selection, reset (mode 3), with the pending
    // override +2efc when +2ef9 is set (consumed), else the default duration.
    const auto t=transition_2ef8.pending?transition_2ef8.value:actor_motion_transition(*duration);
    transition_2ef8.pending=false;
    MotionRequest q;q.node=*retained.node;q.selector=retained.selector;
    q.width=t.width;q.bias=t.bias;q.rate=t.rate;q.secondary_rate=t.secondary_rate;q.limit=-1;q.initial_phase=0;
    model->install_motion(q,MotionInstallMode::reset);
    if(current)current->ledge_motions.push_back("retained");
    // 3. 0018caf0: channels 0..22 of +2ec0 +10 through 0018d0f0.
    model->apply_commands(actor_default_channel_commands());
    // 4. Physical +11 = 1, +9c0 = 0.
    frame_state.tail.reset_requested=1;frame_state.response.physical.keep_contact_9c0=0;
    // 5. Action 0x1a off; 001a3780 (with 0x1f set it only clears 0x1b); action 0x1f off.
    set_actor_action(actions,0x1a,false);
    enter_dead_state();
    set_actor_action(actions,0x1f,false);
    // 6. 0018a0b0(+320c, 0): priority set {0}; +3218 records the low word's
    // changes only. 0018a0a0(+3248): +3248 = +324c = 0.
    actions.changes_3218|=actions.active_actions^1U;
    actions.active_actions=1U;actions.highest_action=0;
    actions.flags_3248=0;actions.flags_324c=0;
    // 7. Physical +374 = word 004a03e0 (1.74 at run time).
    frame_state.history.target_radius=std::bit_cast<float>(0x3fdeb852U);
    // 8. 0038cbf0(actor + 60): no attached effect handles in solo (inferred).
    // 9. The player with +14: invulnerable, RespawnTimer {1000, stage 0}, logic gate.
    if(player()&&actions.byte_14!=0){
        health.invulnerable_33a4=true;
        schedule_actor_respawn_timer(respawn_timer,1000.0F,0);
        byte_3269=1;
    }
    // 10. 00192fe0: channel 4 = 0.0 (0018d0f0), then the Teleport Message
    // mode 2 with the checkpoint component's matrix, at once (transport 1).
    {
        const ActorChannelCommand c{ActorChannelOperation::set,4,0.0F,0,0,false};
        model->apply_commands(std::span<const ActorChannelCommand>(&c,1));
    }
    ActorTeleportMatrix rows;
    for(std::size_t r=0;r<4;++r)
        rows[r]={checkpoint.matrix[r*4],checkpoint.matrix[r*4+1],checkpoint.matrix[r*4+2],checkpoint.matrix[r*4+3]};
    teleport(ActorTeleportMode::place,rows);
    // 11. +30ac = 0; fire off; +33b0 = 0; health = +3398 (003cc990; its sync
    // message has no solo receiver); armor = +339c. 00142140(+3524): the solo
    // actor holds nothing (inventory mode 6), no effect (inferred). +33a8 and
    // +33ac have no native field (no reader found); +33a5 = 0.
    navigation.enabled_30ac=false;
    extinguish();
    health.fire_damage_33b0=0;
    health.health_40=static_cast<float>(health.max_health_3398);
    health.armor_33a0=health.max_armor_339c;
    health.killed_33a5=false;
    // 12. 00189dd8: MSGGAME_NOTIFY_CHECKPOINT (cNotifyCheckpointMsg, key
    // 004f7938, no payload) to the controller +58, then 00189e14: CAS 0x11;
    // both transport 0, so the next pump delivers them in that order.
    if(current){current->revived=true;current->notify_checkpoint=true;current->component_codes.push_back(0x11);}
    {QueuedMessage m;m.kind=QueuedMessage::Kind::notify_checkpoint;queue.push_back(m);}
    // The CAS goes to the controller +58: an AI component queues it for its own pump (as the controller requests above).
    if(player())queue_component(0x11);
    else controller.ai->controller_message(*owner,0x11);
}

// 001a80e0 on the navigation block, for an AI component (00199f30 selects
// it at 0019a048..0019a0a0). It writes +31c0 (direction), +31d0 (speed),
// +31d4 (turn), +31b7 (turn pose from the latch +303c) and the arrival byte
// +30ad only when it updates; its playback-rate tail needs the +54 predicate,
// which is false here.
void SoloActorSimulation::Impl::navigation_body(){
    ActorNavigationHost nh;
    nh.snapshot=[this]{
        ActorNavigationInput in;
        in.physical_present=true;in.target_enabled=navigation.enabled_30ac;
        in.mode=mode();in.flags3204=words.current_3204;in.flags3210=actions.active_actions;
        in.world=roots.world_placement(floor_root);                     // root +3290 (00132400)
        if(in.mode==16){
            // 001a8140..001a81c8: the anchors 'edge' (D_004A03D0) and 'ladd' (D_004A03D8) by their
            // Root_GetWorldBasePlacement position and anchor weight; 'flor' (D_004A03C8) when the sum is <= 0.
            in.first_ladder_anchor={roots.world_placement(edge_root).position,root_weight(ActorRoot::edge)};
            in.second_ladder_anchor={roots.world_placement(ladd_root).position,root_weight(ActorRoot::ladder)};
            in.fallback_ladder_position=in.world.position;
        }
        in.target_position=navigation.target_30b0;in.target_yaw=navigation.yaw_30c0;in.speed_limit=navigation.speed_30e0;
        in.stationary_latch=latch_303c!=0;
        // +31b0 (position-curve retime): the simulation has no writer of it.
        in.retime_target=false;
        in.component_predicate=false;in.actor_flag14=true;
        const auto& eq=controller.equipment;
        in.equipment_present=eq.present;in.primary_classified=eq.primary_classified;in.secondary_classified=eq.secondary_classified;
        in.equipment_state=eq.state_312;
        in.body_class=actions.inventory?actions.inventory->mode:std::uint8_t{6};                // inventory +84
        in.body_enabled=actions.attachment_byte_a0&&*actions.attachment_byte_a0!=0;          // inventory +a0
        return in;
    };
    nh.apply=[this](const ActorNavigationPlan& plan){
        controls.desired=plan.direction;controls.speed=plan.speed;controls.turn=plan.turn;
        controls.turn_pose_31b7=plan.turn_pose;navigation.arrived_30ad=plan.arrived;
    };
    // 00476490: the ladder action (31 down, 32 up) as a self-directed action at once
    // (001a92a0 -> 001ab990 -> 00472e10 type 0, the pad case 6 request).
    nh.deliver_ladder_immediate=[this](std::uint8_t a){request_ladder_exit(a);};
    nh.set_arrived=[this](bool a){
        // Port-only (set_port_ladder_arrival_at_end, the co-op partner): no ladder-request arrival; the node is
        // reached by the stop distance once the exit has taken the actor off the ladder.
        if(a&&port_ladder_arrival_at_end)return;
        navigation.arrived_30ad=a;
    };
    update_actor_navigation(nh,controller.navigation);
}

void SoloActorSimulation::run_frame(std::span<SoloActorSimulation* const> actors,std::span<const SoloActorFrameInput> inputs,
    std::span<SoloActorFrame> frames,const std::function<void()>& services,const std::function<void()>& after_first_traversal,
    const std::function<std::optional<AppendedActor>()>& appended,std::span<const bool> traverse){
    if(actors.size()!=inputs.size()||actors.size()!=frames.size())throw std::runtime_error("Actor world: one input and one frame per actor");
    if(!traverse.empty()&&traverse.size()!=actors.size())throw std::runtime_error("Actor world: one traversal flag per actor");
    const auto traversed=[&](std::size_t i){return traverse.empty()||traverse[i];};
    for(std::size_t i=0;i<actors.size();++i)
        if(!traversed(i)&&actors[i]->impl_->player())throw std::runtime_error("Actor world: the player out of the traversal is not hosted");
    struct Clear{std::span<SoloActorSimulation* const> a;~Clear(){for(auto* x:a){x->impl_->current=nullptr;x->impl_->executing=false;}}} clear{actors};
    std::vector<AppendedActor> late;
    struct ClearLate{std::vector<AppendedActor>& a;~ClearLate(){for(auto& x:a){x.actor->impl_->current=nullptr;x.actor->impl_->executing=false;}}} clear_late{late};
    // Marks the actor whose own phase runs: another actor's damage may reach
    // it between its phases (apply_damage), not during them.
    const auto run=[](Impl& s,const auto& f){s.executing=true;f();s.executing=false;};
    OMEGA_ZONE_LAPS(actor_laps);   // port-only (132vt2, zone_profile.hpp): the frame's passes on the Vita
    for(std::size_t i=0;i<actors.size();++i){
        auto& s=*actors[i]->impl_;
        if(s.config.dt!=actors[0]->impl_->config.dt)throw std::runtime_error("Actor world: every actor steps with the world's elapsed time");
        frames[i]=SoloActorFrame{};s.current=&frames[i];if(!s.footsteps.materials)s.lead_foot_31dd=inputs[i].lead_foot_31dd;
    }
    // The pump (003c8de0) delivers each actor's queued messages (inferred:
    // before the world update, actor by actor in insertion order; an AI
    // component's own messages after its actor's).
    // Every queue is snapshotted before any delivery: a message sent while
    // the pump delivers (a script handler activating another actor, for
    // example) waits for the next pump, as with 003c8de0's pending list.
    for(auto* a:actors){
        a->impl_->advance_respawn_timer();
        a->impl_->pump_snapshot=std::move(a->impl_->queue);a->impl_->queue.clear();
        if(!a->impl_->player())a->impl_->controller.ai->snapshot();
    }
    for(auto* a:actors){
        run(*a->impl_,[&]{
            a->impl_->deliver_queued();
            a->impl_->advance_respawn_poll();
            if(!a->impl_->player())a->impl_->controller.ai->pump(*a);
        });
    }
    OMEGA_ZONE_LAP(actor_laps,"actor.pump");
    // The frame-update broadcast reaches the world's child services first.
    if(services)services();
    OMEGA_ZONE_LAP(actor_laps,"actor.services");
    // First traversal: +4c (component +3c), then actor logic when eligible.
    for(std::size_t i=0;i<actors.size();++i){
        if(!traversed(i))continue;
        auto& s=*actors[i]->impl_;
        run(s,[&]{
            if(!s.player())s.controller.ai->always_run(*actors[i],s.config.dt);
            if(actors[i]->eligible())s.logic(inputs[i]);
        });
        if(s.player())OMEGA_ZONE_LAP(actor_laps,"actor.logic player");
        else OMEGA_ZONE_LAP(actor_laps,"actor.logic ai");
    }
    // The array's later entries (00133420 walks 0040ca30 in registration
    // order: the level's generators 001c2ae0 come after its NPCs and player).
    if(after_first_traversal)after_first_traversal();
    // The actors those entries registered, visited by the same first pass.
    if(appended)while(const auto a=appended()){
        if(!a->actor||!a->frame||!a->input)throw std::runtime_error("Actor world: an appended actor needs its frame and input");
        for(const auto* x:actors)if(x==a->actor)throw std::runtime_error("Actor world: an appended actor is already in the traversal");
        for(const auto& x:late)if(x.actor==a->actor)throw std::runtime_error("Actor world: an actor appended twice");
        auto& s=*a->actor->impl_;
        if(!actors.empty()&&s.config.dt!=actors[0]->impl_->config.dt)
            throw std::runtime_error("Actor world: every actor steps with the world's elapsed time");
        *a->frame=SoloActorFrame{};s.current=a->frame;if(!s.footsteps.materials)s.lead_foot_31dd=a->input->lead_foot_31dd;
        late.push_back(*a);
        run(s,[&]{
            if(!s.player())s.controller.ai->always_run(*a->actor,s.config.dt);
            if(a->actor->eligible())s.logic(*a->input);
        });
    }
    OMEGA_ZONE_LAP(actor_laps,"actor.generators");
    // Second traversal: the model/physical phase of every eligible actor.
    for(std::size_t i=0;i<actors.size();++i)if(traversed(i)&&actors[i]->eligible())run(*actors[i]->impl_,[&]{actors[i]->impl_->physics(inputs[i]);});
    for(const auto& a:late)if(a.actor->eligible())run(*a.actor->impl_,[&]{a.actor->impl_->physics(*a.input);});
    OMEGA_ZONE_LAP(actor_laps,"actor.physics");
    // The frame-update event: the hotbox trackers of present actors.
    for(std::size_t i=0;i<actors.size();++i)if(traversed(i)&&actors[i]->eligible())run(*actors[i]->impl_,[&]{actors[i]->impl_->frame_event();});
    for(const auto& a:late)if(a.actor->eligible())run(*a.actor->impl_,[&]{a.actor->impl_->frame_event();});
    OMEGA_ZONE_LAP(actor_laps,"actor.frame_event");
}

SoloActorSimulation::SoloActorSimulation(SoloCollisionWorld world,const SoloActorConfig& config,
    SoloActorAnimation* animation,std::optional<ActorAnimationTuning> tuning,SoloActorControlFields initial)
    :SoloActorSimulation(SoloActorWorldLink{std::make_shared<const SoloCollisionWorld>(std::move(world))},
        config,animation,std::move(tuning),initial,SoloActorController{},true){}

SoloActorSimulation::SoloActorSimulation(SoloActorWorldLink link,const SoloActorConfig& config,SoloActorAnimation* animation,
    std::optional<ActorAnimationTuning> tuning,SoloActorControlFields initial,const SoloActorController& controller,bool present)
    :impl_(std::make_unique<Impl>()){
    auto& s=*impl_;
    if((animation==nullptr)!=!tuning.has_value())throw std::runtime_error("Animation model and tuning go together");
    if(!(config.dt>0))throw std::runtime_error("Simulation step must be positive");
    if(!link.world)throw std::runtime_error("Solo actor: no collision world");
    s.shared_world=std::move(link.world);s.handle=link.handle;s.controller_handle=link.controller_handle;
    if(link.random_state)s.rng=link.random_state;
    if(link.frame_counter)s.frame_counter=link.frame_counter;
    if(link.water){
        if(!link.water_actors||!link.water_sink)throw std::runtime_error("Solo actor: the world's water manager without its actor view and sink");
        s.water=link.water;s.water_actors=link.water_actors;s.water_sink=link.water_sink;
    }
    s.controller=controller;s.byte_2d=present?1:0;s.owner=this;
    s.config=config;s.model=animation;s.tuning=std::move(tuning);s.controls=initial;
    if(s.model&&!config.population_parameters)
        throw std::runtime_error("Solo actor: the population record's health/armor words (SoloActorConfig::population_parameters) are required with an animation model");
    if(config.population_parameters){
        s.health=make_actor_health((*config.population_parameters)[0],(*config.population_parameters)[1]);   // 001b5fe0
        if(!controller.ai)apply_player_role_armor(s.health);            // 001ae450: role 1 only (an AI actor is role 2)
        if(config.gender_3260)s.health.gender_3260=*config.gender_3260;   // 001b3460 (model init, after 001b5fe0)
    }
    s.jump.fall_start_30a4=config.fall_start_30a4;
    s.hotbox_world=build_hotbox_world(config.hotboxes,true,config.hotbox_link_objects,config.hotbox_link_actors);
    auto floor_state=make_initial_root_animation();const float half_yaw=config.yaw_degrees*3.14159265F/360.0F;
    set_root_placement(floor_state,{0,0,std::sin(half_yaw),std::cos(half_yaw)},config.start);
    s.floor_root=s.roots.add(floor_state);
    s.edge_root=s.roots.add(make_initial_root_animation());
    s.ladd_root=s.roots.add(make_initial_root_animation());
    s.buttons.current_bits_3c=1;   // 002508a0: selected 0, mask 1
    s.buttons.pending_top_40=0;s.buttons.pending_bits_44=1;
    s.heading_state.curve=make_animation_rotation_curve(15.0F);   // 001b6ab0 -> 001b7930
    s.heading_state.history={config.yaw_degrees*3.14159265F/180.0F};
    s.heading_state.active={1};s.heading_state.selected=0;   // 001b7940 push byte 1; +3098 = 0 (001b6be8)
    if(s.model)s.model->set_anchor_world_placement(flor_key,s.roots.world_placement(s.floor_root),true);
    // Ordinary play runs in actor mode 0, i.e. priority action 0 is the
    // highest active one. The initializer that enables it is not identified;
    // the sandbox starts there, as it did with its fixed mode 0.
    set_actor_action(s.actions,0,true);
    // The solo actor carries no inventory item: +3524's mode +84 and saved
    // mode +85 are 6 (nothing held), idle, so the ladder's 00143b60 (token
    // 0x46) and 00143cf0 (leaving action 16) have no effect (static).
    {ActorAnimationInventoryState inventory;inventory.owner=1;inventory.mode=6;inventory.saved_mode=6;
     s.actions.inventory=inventory;s.actions.attachment_byte_a0=std::uint8_t{0};}
    // Actor +14: the root object constructor 003cb090 stores 1 (003cb0e0);
    // in single player nothing clears it (LADDERS.md, "Actor +14").
    s.actions.byte_14=1;
    if(s.model)s.model->set_motion_sink([impl=impl_.get()](const MotionEffect& effect,const MotionDeliveryView& view){
        impl->receive_motion_effect(effect,view);
    });
    s.component=config.start;
    // The component translation between frames before the first one: the
    // load placement (an actor that is never activated never publishes, but
    // its virtual +c0, 00188d80's non-present path, reads it; the HUD callout
    // tracker's 003e9880 does so from the callout's creation).
    s.published=config.start;
    s.checkpoint=make_actor_checkpoint(root_placement_matrix(s.roots.world_placement(s.floor_root)),
        static_cast<std::int32_t>(std::trunc(s.health.health_40)),s.health.armor_33a0);
    s.reset_physical_component();
    // A present actor stands for an activated one: 001b53d0's 001af190.
    if(present)s.weight_model_centroid();
    s.build_hosts();
}
SoloActorSimulation::~SoloActorSimulation()=default;
// A one-actor world frame, then the simulation's own frame counter advances.
SoloActorFrame SoloActorSimulation::step(const SoloActorFrameInput& input){
    SoloActorFrame frame;
    SoloActorSimulation* self=this;
    run_frame(std::span<SoloActorSimulation* const>(&self,1),std::span<const SoloActorFrameInput>(&input,1),std::span<SoloActorFrame>(&frame,1));
    ++impl_->own_frame_counter;
    return frame;
}
std::uint64_t SoloActorSimulation::handle() const{return impl_->handle;}
std::uint64_t SoloActorSimulation::controller_handle() const{return impl_->controller_handle;}
bool SoloActorSimulation::player_controller() const{return impl_->player();}
bool SoloActorSimulation::eligible() const{return impl_->byte_2c&&impl_->byte_2d&&!impl_->byte_2e;}
std::uint8_t SoloActorSimulation::byte_2d() const{return impl_->byte_2d;}
void SoloActorSimulation::set_step_dt(float dt){
    if(!(dt>0.0F)||!std::isfinite(dt))throw std::runtime_error("Simulation step must be positive");
    impl_->config.dt=dt;
}
float SoloActorSimulation::step_dt() const{return impl_->config.dt;}
std::uint8_t SoloActorSimulation::byte_14() const{return impl_->actions.byte_14;}
bool SoloActorSimulation::in_water_346d() const{return impl_->in_water_346d;}
// The physical component +3310 as its constructor 001eb990 leaves it, linked
// by 001b5560: +11 set so the first 001e9980 runs 001e9380 (001b5560 also runs
// 001e9380 at once; it reads the actor's component, so the first frame's run
// repeats it), mass 75 (001ec2a0), +370 / +374 the radius, the three mode sets
// at value 0 with bit 0 (001ec340(set, 0), checked in the EE interpreter),
// +3d0 = 1 (001ebfa0), +980 = (0, 0, 0, 1) (001ebfe8), +9c0 = 0 and no support
// bindings or contact (+1f0..+2c0, +164). Assigned in place: the hosts keep
// pointers into these members. The "flor"/"edge" roots +9e0/+a00 bind to the
// model's roots, which the recreation leaves as they are.
void SoloActorSimulation::Impl::reset_physical_component(){
    frame_state=ActorPhysicalFrameState{};
    frame_state.tail.reset_requested=1;
    frame_state.integrator.mass=75.0F;frame_state.integrator.inverse_mass=1.0F/75.0F;
    frame_state.history.radius=config.radius;frame_state.history.target_radius=config.radius;
    frame_state.history.previous={0,1};frame_state.history.current={0,1};frame_state.history.pending={0,1};
    auto& p=frame_state.response.physical;
    p.enabled_12=1;p.shape_14=static_cast<std::uint8_t>(config.shape);p.scale_390=1;
    p.byte_13=config.keep_velocity?1:0;p.e4=actor_unavailable_height;
    physical_3d0=1;ladder_980={0,0,0,1};
}
// 001b53d0: 001b5560 (a new +3310 / +3314; the first activation's are the
// constructor's), 00392940 (+2d = 1), 001af190 (centroid), 0022c650. 001af460
// (bounds) and +3268 = 1 are not modelled.
// 001b53d0 has no +2d test (the script's Activate, 00157800 -> 00157910, calls
// vt+5c without one either), so a present actor takes the same path: 001b5560
// stores a new 0xa30-byte +3310 (constructor 001eb990) and a new 0x840-byte
// +3314 (001d81a0) over the old ones without destroying them (the destructor
// 001b4de0 runs only from 001b5190), 003ce980 sets +2d = 1 again and calls the
// AI's vt+48 (which returns while +159 is clear), and 0022c650 returns 0 for
// an object already in one of its 30 slots. The old +3310's water effect
// (registered by the leaked component and removed only by 001e98f0) is not
// hosted.
void SoloActorSimulation::activate(){
    auto& s=*impl_;
    if(s.byte_2d&&!s.deactivated){
        if(s.player())throw std::runtime_error("Solo actor: the player's re-activation (001b5560 +3314 +8 / +3310 +14) is not hosted");
        if(!s.activation_hook)throw std::runtime_error("Solo actor: activation of a present actor needs the +3314 recreation and 001977f0 (set_activation_hook)");
        if(s.frame_state.response.physical.e4!=actor_unavailable_height)
            throw std::runtime_error("Solo actor: activation of a present actor leaks the old +3310 with its water effect (not hosted)");
        s.reset_physical_component();
    }
    if(s.deactivated){
        // 001b5640..: the player's +3314 +8 = 2 and +3310 +14 = 3 are not hosted.
        if(s.player())throw std::runtime_error("Solo actor: the player's re-activation (001b5560 +3314 +8 / +3310 +14) is not hosted");
        if(!s.activation_hook)throw std::runtime_error("Solo actor: re-activation needs the +3314 recreation and 001977f0 (set_activation_hook)");
        s.reset_physical_component();
        s.deactivated=false;
    }
    if(s.activation_hook){
        s.byte_31b2=s.activation_hook();                     // 001b571c: 001977f0(actor, 0)
        if(s.actions.attachment_byte_a0)s.actions.attachment_byte_a0=std::uint8_t{0};   // 001b5730: +3524 +a0 = 0
    }
    s.byte_2d=1;
    s.weight_model_centroid();                               // 001b53d0 -> 001af190
    if(s.presence_hook)s.presence_hook(true);               // 001b5410: 0022c650(004f77c0, actor)
    if(!s.player())s.controller.ai->activated(*this);
}
std::size_t SoloActorSimulation::queued_messages() const{return impl_->queue.size();}
// 001b5190: 0038cbf0(+60) (no attached effect handles in solo), 0022c720,
// 00185d40(actor, 0..2, off, 1.0), 003928d0, then 001e98f0 (the water effect
// of a supported physical: +e4 available and +1a8 > 0x3f795810 -> 002b2330)
// and the destruction of +3310 / +3314 (recreated by the next activation).
// 001b5190 has no +2d test: an actor that is not present takes the same path.
// 0022c720 returns when 0022d380 finds no slot, 003ce790 clears +2d again and
// calls the AI's +4c (00157ad0, which returns while +159 is set), and the
// +3310 tests (001b527c, 001b5294) skip 001e98f0 and the destruction once a
// previous deactivation has destroyed it.
void SoloActorSimulation::deactivate(bool effect_slots){
    auto& s=*impl_;
    const bool physical_3310=!s.deactivated;
    s.byte_2d=0;s.deactivated=true;
    if(s.presence_hook)s.presence_hook(false);              // 001b51bc: 0022c720(004f77c0, actor)
    if(!effect_slots){}                                      // 001b4970: no 00185d40
    else if(s.deactivation_hook)s.deactivation_hook();      // 001b51d8: 00185d40 x3
    else if(s.activation_hook)throw std::runtime_error("Solo actor: 001b5190's 00185d40 effect slots are not hosted (set_deactivation_hook)");
    if(!s.player())s.controller.ai->deactivated(*this);
    if(!physical_3310)return;
    const float height=s.frame_state.response.physical.e4;   // 001e98f0
    if(height!=actor_unavailable_height){
        const auto& water=s.frame_state.response.water;
        if(!water)throw std::runtime_error("Solo actor: 001e98f0 reads +1a8 without a retained water triangle");
        if(water->triangle.normal[2]>std::bit_cast<float>(0x3f795810U))
            s.water->remove(static_cast<std::uint32_t>(s.handle),*s.water_actors,*s.water_sink);   // 002b2330
    }
}
std::array<std::uint8_t,3> SoloActorSimulation::root_bytes() const{return {impl_->byte_2c,impl_->byte_2d,impl_->byte_2e};}
void SoloActorSimulation::set_presence_hook(std::function<void(bool active)> hook){impl_->presence_hook=std::move(hook);}
void SoloActorSimulation::set_sound_sink(SoundSink sink){impl_->sound_sink=std::move(sink);}
void SoloActorSimulation::set_voice_sink(VoiceSink sink){impl_->voice_sink=std::move(sink);}
void SoloActorSimulation::voice_line(std::int32_t line){if(impl_->voice_sink)impl_->voice_sink(line);}
void SoloActorSimulation::post_static_sound(std::uint32_t descriptor,bool immediate){
    const auto& d=static_sound_descriptor(descriptor);
    if(impl_->current)impl_->current->sounds.push_back(immediate?std::string(d.name):"queued "+std::string(d.name));
    impl_->own_sound(d.type,d.name,immediate);
}
void SoloActorSimulation::set_positional_sound_sink(PositionalSoundSink sink){impl_->positional_sink=std::move(sink);}
void SoloActorSimulation::set_timed_event_sink(TimedEventSink sink){impl_->timed_event_sink=std::move(sink);}
void SoloActorSimulation::set_footstep_hooks(FootstepHooks hooks){impl_->footsteps=std::move(hooks);}
void SoloActorSimulation::set_activation_hook(std::function<std::uint8_t()> hook){impl_->activation_hook=std::move(hook);}
void SoloActorSimulation::set_deactivation_hook(std::function<void()> hook){impl_->deactivation_hook=std::move(hook);}
std::uint8_t SoloActorSimulation::byte_31b2() const{return impl_->byte_31b2;}
const ActorNavigationTarget& SoloActorSimulation::navigation_target() const{return impl_->navigation;}
void apply_actor_navigation_request(ActorNavigationTarget& n,const RootVector& target,float yaw,float speed){
    n.target_30b0=target;n.yaw_30c0=yaw;
    const float c=game_cosf(yaw),sn=game_sinf(yaw);                      // 00121688, 00121928
    n.facing_30d0={-sn,c,0,0};n.speed_30e0=speed;n.enabled_30ac=true;
}
void apply_actor_navigation_stop(ActorNavigationTarget& n,SoloActorControlFields& controls,std::uint8_t latch_303c,
    const RootVector& direction,float speed,float turn,std::uint8_t pose_byte){
    controls.desired=direction;controls.speed=speed;controls.turn=turn;
    if(pose_byte==2)controls.turn_pose_31b7=latch_303c!=0&&speed==0&&!(std::fabs(turn)<=1e-4F);      // 001ae3b0
    else controls.turn_pose_31b7=pose_byte==1;                                                    // 001ae430
    n.enabled_30ac=false;n.arrived_30ad=false;
}
void SoloActorSimulation::receive_actor_action(std::int32_t action,bool immediate){
    auto& s=*impl_;
    if(immediate&&(action==0x40||action==0x41))s.deliver_flashbang_hide(action);
    else if(immediate&&action==8)s.deliver_roll();
    else if(immediate)s.send_pad_action(action,false);
    else s.queue_action(action);
}
void SoloActorSimulation::stop_physical_motion(){
    auto& g=impl_->frame_state.integrator;
    g.force={0,0,0,0};g.acceleration={0,0,0,0};g.previous_position=g.position;g.velocity={0,0,0,0};
}
bool SoloActorSimulation::roots_unparented() const{
    const auto& s=*impl_;
    return !s.roots.parented(s.floor_root)&&!s.roots.parented(s.edge_root)&&
        !s.roots.parented(s.ladd_root);
}
void SoloActorSimulation::receive_navigation_request(const RootVector& target,float yaw,float speed){
    apply_actor_navigation_request(impl_->navigation,target,yaw,speed);
}
void SoloActorSimulation::receive_navigation_stop(const RootVector& direction,float speed,float turn,std::uint8_t pose_byte){
    apply_actor_navigation_stop(impl_->navigation,impl_->controls,impl_->latch_303c,direction,speed,turn,pose_byte);
}
const std::array<std::uint32_t,3>& SoloActorSimulation::control_history() const{return impl_->control_3030;}
void SoloActorSimulation::set_control_flags_3038(std::uint32_t bits){impl_->control_3030[2]|=bits;}
void SoloActorSimulation::land_dead_ground_snap(){impl_->land_dead_with(false);}
void SoloActorSimulation::land_reset_ground_snap(){impl_->land_reset();}
std::uint8_t SoloActorSimulation::stationary_latch() const{return impl_->latch_303c;}
std::uint32_t SoloActorSimulation::frame_counter() const{return *impl_->frame_counter;}
const SoloCollisionWorld& SoloActorSimulation::world() const{return impl_->world();}
const ActorPhysicalFrameState& SoloActorSimulation::physical() const{return impl_->frame_state;}
RootParentTransform SoloActorSimulation::floor_root_placement(){return impl_->roots.world_placement(impl_->floor_root);}
RootVector SoloActorSimulation::component() const{return impl_->component;}
RootParentTransform SoloActorSimulation::component_placement(){
    return impl_->published_component?*impl_->published_component:impl_->roots.world_pose(impl_->floor_root);}
const SoloActorControlFields& SoloActorSimulation::controls() const{return impl_->controls;}
SoloActorControlFields& SoloActorSimulation::controls(){return impl_->controls;}
const PlayerMoveState& SoloActorSimulation::pad_state() const{return impl_->pad_state;}
const PlayerQuickTurnState& SoloActorSimulation::quick_turn_state() const{return impl_->quick_turn;}
const ActorAnimationReceiverState& SoloActorSimulation::actions() const{return impl_->actions;}
ActorMotionRegistration SoloActorAnimation::resolve_motion(std::string_view) const{
    throw std::runtime_error("This animation model does not host actor motion installation");
}
ActorMotionRegistration SoloActorAnimation::resolve_named_root(std::string_view) const{
    throw std::runtime_error("This animation model does not host the named-root motion registry (001926d0)");
}
std::optional<std::uint32_t> SoloActorAnimation::motion_node_id(std::string_view) const{
    throw std::runtime_error("This animation model does not host the node-name index map (0018c740)");
}
ActorMotionRegistration SoloActorAnimation::resolve_motion_id(std::uint32_t) const{
    throw std::runtime_error("This animation model does not host numeric motion lookups (0018a420 tag 1)");
}
ActorMotionRegistration SoloActorAnimation::retained_motion() const{
    throw std::runtime_error("This animation model does not host actor motion installation");
}
void SoloActorAnimation::install_motion(const MotionRequest&,MotionInstallMode){
    throw std::runtime_error("This animation model does not host actor motion installation");
}
// Models without delivery hooks deliver no effects (the earlier behaviour).
void SoloActorAnimation::set_motion_sink(MotionDeliveryHooks::Sink){}
const SoloActorStateWords& SoloActorSimulation::state_words() const{return impl_->words;}
const PlayerButtonState& SoloActorSimulation::button_state() const{return impl_->buttons;}
std::uint32_t SoloActorSimulation::found_ledge() const{return impl_->found_ledge_130;}
std::uint32_t SoloActorSimulation::physical_flags810() const{return impl_->frame_state.edges.slots[6].mask;}
RootParentTransform SoloActorSimulation::edge_root_placement(){return impl_->roots.world_placement(impl_->edge_root);}
std::optional<float> SoloActorAnimation::anchor_weight(const RootAnchorKey&) const{return std::nullopt;}
bool SoloActorAnimation::has_anchor(const RootAnchorKey&) const{return false;}
std::optional<RootParentTransform> SoloActorAnimation::published_placement() const{return std::nullopt;}
RootVector SoloActorAnimation::skeleton_node_world_position(int) const{
    throw std::runtime_error("This animation model does not host skeleton node world positions (00132910)");
}
std::array<RootVector,4> SoloActorAnimation::skeleton_node_world_matrix(int) const{
    throw std::runtime_error("This animation model does not host skeleton node world matrices (00133ed0)");
}
RootParentTransform SoloActorAnimation::base_aggregate() const{
    throw std::runtime_error("This animation model does not host the base aggregate (model +e0/+f0)");
}
RootVector SoloActorAnimation::bone0_rotation() const{
    throw std::runtime_error("This animation model does not host node 0's rotation cache (+1a0)");
}
RootParentTransform SoloActorSimulation::base_aggregate() const{
    if(!impl_->model)throw std::runtime_error("Solo actor: the base aggregate needs an animation model");
    return impl_->model->base_aggregate();
}
RootVector SoloActorSimulation::bone0_rotation() const{
    if(!impl_->model)throw std::runtime_error("Solo actor: node 0's rotation cache needs an animation model");
    return impl_->model->bone0_rotation();
}
UpperBodyModelNodes SoloActorAnimation::upper_body_nodes() const{
    throw std::runtime_error("This animation model does not host the upper-body pass's nodes (+1a0 / +1b0)");
}
void SoloActorAnimation::write_upper_body_nodes(std::span<const std::optional<std::array<float,12>>>,const std::optional<RootVector>&){
    throw std::runtime_error("This animation model does not host the node writes of 001ce7c0 (vt +38 / +3c)");
}
UpperBodyModelNodes SoloActorSimulation::upper_body_nodes() const{
    if(!impl_->model)throw std::runtime_error("Solo actor: the upper-body nodes need an animation model");
    return impl_->model->upper_body_nodes();
}
void SoloActorSimulation::write_upper_body_nodes(std::span<const std::optional<std::array<float,12>>> rows,const std::optional<RootVector>& root_translation){
    if(!impl_->model)throw std::runtime_error("Solo actor: the upper-body node writes need an animation model");
    impl_->model->write_upper_body_nodes(rows,root_translation);
}
void SoloActorSimulation::set_secondary_update(std::function<void(float)> update){impl_->secondary_update=std::move(update);}
void SoloActorSimulation::set_pad_combat_hooks(PadCombatHooks hooks){impl_->pad_combat=std::move(hooks);}
void SoloActorSimulation::set_choose_menu_track(std::function<void(const RootVector&)> track){impl_->choose_menu_track=std::move(track);}
void SoloActorSimulation::set_pad_interact_prompt(PadInteractPrompt prompt){impl_->pad_interact_prompt=std::move(prompt);}
void SoloActorSimulation::set_inventory_update(std::function<void(float)> update){impl_->inventory_update=std::move(update);}
void SoloActorSimulation::set_death_secondary_hooks(std::function<SecondaryModeInput()> input,std::function<void(std::uint8_t)> apply){
    impl_->death_secondary_input=std::move(input);impl_->death_secondary_apply=std::move(apply);}
void SoloActorSimulation::set_secondary_component(std::function<SecondaryComponent*()> component){
    impl_->secondary_component=std::move(component);}
void SoloActorSimulation::set_logic_item_hooks(std::function<void(float)> triggers,std::function<void(float)> senses){
    impl_->item_triggers=std::move(triggers);impl_->sense_decay=std::move(senses);}
void SoloActorSimulation::set_logic_grip_hook(std::function<void(float)> grip){impl_->grip=std::move(grip);}
void SoloActorSimulation::set_fire_hooks(SoloFireHooks hooks){impl_->fire=std::move(hooks);}
void SoloActorSimulation::set_inventory_receiver(std::optional<ActorAnimationInventoryBindings> bindings,ActorAnimationReceiverHost host){
    impl_->inventory_bindings=std::move(bindings);impl_->inventory_host=std::move(host);}
ActorAnimationReceiverState& SoloActorSimulation::item_actions(){return impl_->actions;}
void SoloActorSimulation::receive_animation_effect(const MotionEffect& effect){
    static const AnimationController no_controller{};
    const MotionDeliveryView view(no_controller,nullptr,false);
    impl_->receive_motion_effect(effect,view);
}
std::span<const std::array<float,16>> SoloActorAnimation::skeleton_node_matrices() const{
    throw std::runtime_error("This animation model does not host skeleton node matrices (003a9220)");
}
std::span<const std::array<float,16>> SoloActorSimulation::skeleton_node_matrices() const{
    if(impl_->model==nullptr)throw std::runtime_error("The simulation has no animation model for skeleton node matrices");
    return impl_->model->skeleton_node_matrices();
}
RootVector SoloActorSimulation::skeleton_node_world_position(int node) const{
    if(impl_->model==nullptr)throw std::runtime_error("The simulation has no animation model for skeleton node positions");
    return impl_->model->skeleton_node_world_position(node);
}
std::array<RootVector,4> SoloActorSimulation::skeleton_node_world_matrix(int node) const{
    if(impl_->model==nullptr)throw std::runtime_error("The simulation has no animation model for skeleton node matrices");
    return impl_->model->skeleton_node_world_matrix(node);
}
bool SoloActorSimulation::published() const{return impl_->published_set;}
const ActorJumpState& SoloActorSimulation::jump_state() const{return impl_->jump;}
std::int32_t SoloActorSimulation::pad_selector() const{return impl_->selector_38;}
namespace{
std::uint8_t& option_byte(std::uint32_t offset,std::uint8_t& b124,std::uint8_t& b125,std::uint8_t& b126){
    switch(offset){
    case 0x174:return b124;
    case 0x175:return b125;
    case 0x176:return b126;
    default:{
        char text[96];std::snprintf(text,sizeof text,"Solo actor: player +%x is not an option byte (0045a3a0 / 00135848 write +174..+176)",offset);
        throw std::runtime_error(text);}
    }
}
}
void SoloActorSimulation::set_input_option_byte(std::uint32_t player_offset,std::uint8_t value){
    option_byte(player_offset,impl_->manual_aim_124,impl_->free_look_x_125,impl_->free_look_y_126)=value;
}
std::uint8_t SoloActorSimulation::input_option_byte(std::uint32_t player_offset) const{
    return option_byte(player_offset,impl_->manual_aim_124,impl_->free_look_x_125,impl_->free_look_y_126);
}
std::uint8_t SoloActorSimulation::aim_view_byte_2e00() const{return impl_->byte_2e00;}
const ActorLadderExitFields& SoloActorSimulation::ladder_exit_fields() const{return impl_->exit_fields;}
float SoloActorSimulation::ladder_slide_amount() const{return impl_->slide_31d8;}
RootVector SoloActorSimulation::ladder_reference() const{return impl_->ladder_980;}
std::optional<RootParentTransform> SoloActorAnimation::motion_start_placement(std::string_view) const{
    throw std::runtime_error("This animation model does not host motion start placements (00190f50/001901c0)");
}
std::optional<RootParentTransform> SoloActorAnimation::motion_end_placement(std::string_view) const{
    throw std::runtime_error("This animation model does not host motion end placements (001a77e0)");
}
std::optional<float> SoloActorAnimation::default_motion_duration() const{return std::nullopt;}
std::optional<RootAnchorKey> SoloActorAnimation::motion_anchor_key(std::string_view) const{
    throw std::runtime_error("This animation model does not host motion anchor keys (0018c5a0/001914a0)");
}
std::optional<RootAnchorKey> SoloActorAnimation::motion_anchor_key_by_id(std::uint32_t) const{
    throw std::runtime_error("This animation model does not host motion anchor keys by id (0018c6b0/001914a0)");
}
RootVector SoloActorSimulation::published_position() const{
    return impl_->published_set||!impl_->frame_begun?impl_->published:impl_->component;}
RootVector SoloActorSimulation::model_centroid() const{return impl_->anchor_local;}
RootVector SoloActorSimulation::position_e4() const{return impl_->position_e4();}
RootVector SoloActorSimulation::position_e0() const{
    const auto& f=impl_->frame_state;
    const RootVector base=f.tail.initialized?f.integrator.position:impl_->component;
    RootVector out{};
    for(std::size_t i=0;i<4;++i)out[i]=base[i]+f.response.physical.offset_380[i];
    return out;
}
Bounds SoloActorSimulation::context_bounds() const{return impl_->context_bounds;}
float SoloActorSimulation::root_weight(ActorRoot root) const{return impl_->root_weight(root);}
void SoloActorSimulation::set_movement_world(ActorMovementWorldHost* host){impl_->movement_world=host;}
const HotboxWorld& SoloActorSimulation::hotboxes() const{return impl_->hotbox_world;}
const HotboxTracker& SoloActorSimulation::hotbox_tracker() const{return impl_->tracker;}
void SoloActorSimulation::set_hotbox_script_filter(HotboxScriptFilter filter){impl_->hotbox_filter=std::move(filter);}
void SoloActorSimulation::set_trigger_script_filter(TriggerScriptFilter filter){impl_->trigger_filter=std::move(filter);}
void SoloActorSimulation::set_native_trigger_receiver(NativeTriggerReceiver receiver){impl_->native_trigger_receiver=std::move(receiver);}
void SoloActorSimulation::set_object_code_receiver(ObjectCodeReceiver receiver){impl_->object_code_receiver=std::move(receiver);}
void SoloActorSimulation::set_multiplayer_host(bool host){impl_->multiplayer_host=host;}
void SoloActorSimulation::set_port_ladder_arrival_at_end(bool on){impl_->port_ladder_arrival_at_end=on;}
void SoloActorSimulation::queue_trigger(const HotboxTrigger& trigger){
    if(!trigger.receiver)throw std::runtime_error("Solo actor: trigger without a receiver");
    Impl::QueuedMessage m;m.kind=Impl::QueuedMessage::Kind::trigger;m.trigger=trigger;
    impl_->queue.push_back(std::move(m));
}
void SoloActorSimulation::set_ai_notification_filter(AiNotificationScriptFilter filter){impl_->ai_notification_filter=std::move(filter);}
ActorLinkEffect actor_link_message_effect(bool enable,bool present,const std::function<bool()>& alive_44,
    const std::function<bool()>& radius_controlled_141){
    if(enable)return !present&&alive_44()?ActorLinkEffect::activate:ActorLinkEffect::none;
    return present&&!radius_controlled_141()?ActorLinkEffect::deactivate:ActorLinkEffect::none;
}
void SoloActorSimulation::set_hotbox_link_sink(HotboxLinkSink sink){impl_->link_sink=std::move(sink);}
std::vector<HotboxLinkMessage> SoloActorSimulation::hotbox_load_link_messages() const{
    return omega::hotbox_load_link_messages(impl_->hotbox_world);
}
void SoloActorSimulation::queue_link_message(bool enable){
    Impl::QueuedMessage m;m.kind=Impl::QueuedMessage::Kind::link;m.enable=enable;
    impl_->queue.push_back(std::move(m));
}
void SoloActorSimulation::set_link_message_filter(LinkMessageScriptFilter filter){impl_->link_filter=std::move(filter);}
void SoloActorSimulation::set_notify_checkpoint_filter(NotifyCheckpointScriptFilter filter){impl_->notify_checkpoint_filter=std::move(filter);}
void SoloActorSimulation::queue_out_of_ammo(std::int32_t item){
    if(!player_controller())throw std::runtime_error("Solo actor: cOutOfAmmoMsg (004ea150) to an actor without a player controller");
    Impl::QueuedMessage m;m.kind=Impl::QueuedMessage::Kind::out_of_ammo;m.code=item;
    impl_->queue.push_back(m);
}
void SoloActorSimulation::set_out_of_ammo_filter(OutOfAmmoScriptFilter filter){impl_->out_of_ammo_filter=std::move(filter);}
void SoloActorSimulation::queue_respawn(){
    Impl::QueuedMessage m;m.kind=Impl::QueuedMessage::Kind::respawn;
    impl_->queue.push_back(m);
}
void SoloActorSimulation::queue_weapon_vis_change(std::int32_t value){
    if(!player_controller())throw std::runtime_error("Solo actor: cWeaponVisChangeMsg (00583968) to an actor without a player controller");
    Impl::QueuedMessage m;m.kind=Impl::QueuedMessage::Kind::weapon_vis_change;m.code=value;
    impl_->queue.push_back(m);
}
void SoloActorSimulation::set_weapon_vis_change_filter(WeaponVisChangeScriptFilter filter){
    impl_->weapon_vis_change_filter=std::move(filter);}
// 003c8de0 for an EnableMsg / DisableMsg: the filter +0x38 (003cc850) first,
// then 001b3b60 (single player, 005721c8 clear): 1b4310 / 1b4440.
void SoloActorSimulation::Impl::deliver_link_message(bool enable){
    SoloActorFrame::LinkMessageEvent event;event.enable=enable;
    event.script_handled=link_filter&&link_filter(enable);
    if(!event.script_handled){
        if(player())throw std::runtime_error("Solo actor: a hotbox link message to the player actor is not hosted");
        auto& self=*owner;
        const auto effect=actor_link_message_effect(enable,byte_2c&&byte_2d&&!byte_2e,
            [&]{return controller.ai->alive_or_eligible(self);},[&]{return controller.ai->radius_controlled();});
        if(effect==ActorLinkEffect::activate){self.activate();event.effect=SoloActorFrame::LinkMessageEvent::Effect::activated;}
        if(effect==ActorLinkEffect::deactivate){self.deactivate();event.effect=SoloActorFrame::LinkMessageEvent::Effect::deactivated;}
    }
    if(current)current->link_messages.push_back(event);
}
void SoloActorSimulation::set_actor_peers(ActorResolver resolve,const CarryHooks* hooks){
    impl_->peers=std::move(resolve);impl_->carry_hooks=hooks;
}
void SoloActorSimulation::queue_carry_pickup(std::uint64_t body,std::uint8_t kind_28){
    Impl::QueuedMessage m;m.kind=Impl::QueuedMessage::Kind::carry_pickup;m.object=body;m.payload=kind_28;
    impl_->queue.push_back(std::move(m));
}
void SoloActorSimulation::queue_running_jump(std::int32_t action,const RootVector& payload){
    Impl::QueuedMessage m;m.kind=Impl::QueuedMessage::Kind::running_jump;m.code=action;m.point=payload;
    impl_->queue.push_back(std::move(m));
}
std::uint64_t SoloActorSimulation::relation_root() const{
    const Impl* s=impl_.get();
    for(std::size_t depth=0;s->relation_parent;++depth){
        if(depth>64)throw std::runtime_error("Solo actor: a relation cycle (001e8ca0)");
        const auto* p=s->find_peer(*s->relation_parent);
        if(!p)break;
        s=p;
    }
    return s->handle;
}
std::optional<std::uint64_t> SoloActorSimulation::linked_body() const{return impl_->linked();}
void SoloActorSimulation::queue_ai_notification(const ActorAiNotification& notification){
    if(notification.key!=0x004ff268U&&notification.key!=0x004ff270U&&notification.key!=0x004ff278U&&notification.key!=0x004ff288U&&
       notification.key!=0x004ff290U&&notification.key!=0x004ff298U&&notification.key!=actor_body_moved_message_key&&
       notification.key!=actor_disguise_taken_message_key)
        throw std::runtime_error("Solo actor: script AI notification key not hosted");
    Impl::QueuedMessage m;m.kind=Impl::QueuedMessage::Kind::ai_notification;m.notification=notification;
    impl_->queue.push_back(std::move(m));
}
void SoloActorSimulation::set_hotbox_enabled(std::size_t box,bool enabled){
    auto& boxes=impl_->hotbox_world.boxes;
    if(box>=boxes.size())throw std::out_of_range("Solo actor: no hotbox "+std::to_string(box));
    boxes[box].enabled_2c=enabled;                         // 0016fd00 / 0016fbe0
}
void SoloActorSimulation::set_hotbox_interact_message(std::size_t box,std::optional<std::string> message){
    auto& boxes=impl_->hotbox_world.boxes;
    if(box>=boxes.size())throw std::out_of_range("Solo actor: no hotbox "+std::to_string(box));
    boxes[box].interact_message_a0=std::move(message);     // 0016fb90 / 0016fb6c
}
std::size_t SoloActorSimulation::add_runtime_hotbox(std::uint32_t id,std::uint32_t npc_key,const NpcInteractableBounds& bounds){
    return omega::add_runtime_hotbox(impl_->hotbox_world,id,npc_key,bounds);
}
std::size_t SoloActorSimulation::add_object_hotbox(std::uint32_t id,std::uint32_t object_key,const NpcInteractableBounds& bounds){
    return omega::add_object_hotbox(impl_->hotbox_world,id,object_key,bounds);
}
void SoloActorSimulation::set_hotbox_word(std::size_t box,std::uint32_t offset,std::uint32_t value){
    auto& boxes=impl_->hotbox_world.boxes;
    if(box>=boxes.size())throw std::out_of_range("Solo actor: no hotbox "+std::to_string(box));
    if(offset==0x94)boxes[box].record.parameter=value;
    else if(offset==0x9c)boxes[box].priority_9c=static_cast<std::int32_t>(value);
    else throw std::runtime_error("Solo actor: a hotbox word +"+std::to_string(offset)+" write is not hosted");
}
void SoloActorSimulation::set_hotbox_bounds(std::size_t box,const RootVector& minimum,const RootVector& maximum){
    auto& boxes=impl_->hotbox_world.boxes;
    if(box>=boxes.size())throw std::out_of_range("Solo actor: no hotbox "+std::to_string(box));
    for(std::size_t c=0;c<3;++c){boxes[box].record.minimum[c]=minimum[c];boxes[box].record.maximum[c]=maximum[c];}
}
void SoloActorSimulation::clear_runtime_hotboxes(){
    omega::clear_runtime_hotboxes(impl_->hotbox_world,impl_->tracker);
}
void SoloActorSimulation::remove_object_hotbox(std::size_t box){
    omega::remove_object_hotbox(impl_->hotbox_world,impl_->tracker,box);
}
void SoloActorSimulation::set_object_trigger_hooks(ObjectExists exists,ObjectTrigger trigger){
    impl_->object_exists=std::move(exists);impl_->object_trigger=std::move(trigger);
}
void SoloActorSimulation::set_armor(std::int32_t armor_33a0,std::int32_t max_armor_339c){
    impl_->health.armor_33a0=armor_33a0;impl_->health.max_armor_339c=max_armor_339c;
}
void SoloActorSimulation::set_health_40(float value){impl_->health.health_40=value;}
bool SoloActorSimulation::byte_31b5() const{return impl_->byte_31b5;}
void SoloActorSimulation::set_byte_31b5(bool value){impl_->byte_31b5=value;}
void SoloActorSimulation::set_killed_33a5(bool value){impl_->health.killed_33a5=value;}
void SoloActorSimulation::set_physical_12_13(std::uint8_t value){
    auto& p=impl_->frame_state.response.physical;p.enabled_12=value;p.byte_13=value;   // 001ad5e0
}
void SoloActorSimulation::set_invulnerable_33a4(bool value){impl_->health.invulnerable_33a4=value;}
void SoloActorSimulation::set_running_31f8(float value){impl_->running_31f8=value;}   // 00169bd8
float SoloActorSimulation::running_31f8() const{return impl_->running_31f8;}
void SoloActorSimulation::set_fire_immune_33b8(bool value){impl_->health.fire_immune_33b8=value;}   // 0014a7c0
std::uint8_t SoloActorSimulation::pickupable_3468() const{return impl_->pickupable_3468;}
void SoloActorSimulation::set_pickupable_3468(std::uint8_t value){impl_->pickupable_3468=value;}
std::uint8_t SoloActorSimulation::disguise_3469() const{return impl_->disguise_3469;}
void SoloActorSimulation::set_disguise_3469(std::uint8_t value){impl_->disguise_3469=value;}
bool SoloActorSimulation::fire_ignite_hosted() const{return static_cast<bool>(impl_->fire.ignite);}
void SoloActorSimulation::fire_ignite(){
    if(!impl_->fire.ignite)throw std::runtime_error("unhosted(0x00186360): Solo actor: the ignition (SetOnFire 0014b350) is not hosted");
    impl_->fire.ignite();
}
const ActorHealthState& SoloActorSimulation::health() const{return impl_->health;}
void SoloActorSimulation::port_refill_health(){impl_->health.health_40=static_cast<float>(impl_->health.max_health_3398);}
void SoloActorSimulation::port_revive(){
    auto& s=*impl_;
    if(s.player())throw std::runtime_error("Solo actor: port_revive is for an AI controller (the player revives through its respawn poll)");
    if(!(s.actions.active_actions&0x80000000U))throw std::runtime_error("Solo actor: port_revive of an actor that is not dead (action 0x1f clear)");
    s.revive();
}
const ActorCheckpoint& SoloActorSimulation::checkpoint() const{return impl_->checkpoint;}
void SoloActorSimulation::Impl::store_checkpoint(const std::array<float,3>& position,float yaw,std::string label){
    const double value=health.health_40;
    if(!std::isfinite(value)||std::trunc(value)<static_cast<double>(std::numeric_limits<std::int32_t>::min())||
        std::trunc(value)>static_cast<double>(std::numeric_limits<std::int32_t>::max()))
        throw std::runtime_error("Solo actor: current health cannot be represented by checkpoint storage");
    set_actor_checkpoint(checkpoint,position,yaw,static_cast<std::int32_t>(std::trunc(value)),
        health.armor_33a0,std::move(label));
}
void SoloActorSimulation::set_checkpoint(const std::array<float,3>& position,float yaw,std::string label){
    impl_->store_checkpoint(position,yaw,std::move(label));
}
void SoloActorSimulation::queue_checkpoint(const std::array<float,3>& position,float yaw,std::string label){
    if(!std::isfinite(yaw))throw std::runtime_error("Solo actor: nonfinite checkpoint yaw");
    for(const float v:position)if(!std::isfinite(v))throw std::runtime_error("Solo actor: nonfinite checkpoint position");
    Impl::QueuedMessage m;m.kind=Impl::QueuedMessage::Kind::checkpoint;
    m.node_position=position;m.node_yaw=yaw;m.label=std::move(label);
    impl_->queue.push_back(std::move(m));
}
void SoloActorSimulation::queue_object_action(const ObjectAction& a){
    if(a.object&&(*a.object==0||*a.object==impl_->handle||*a.object==impl_->controller_handle))
        throw std::runtime_error("Solo actor: object action without a hosted interact object");
    if(!a.object&&(a.node||a.world_object!=0))throw std::runtime_error("Solo actor: object links without an object");
    if(a.node){
        if(!std::isfinite(a.node->yaw))throw std::runtime_error("Solo actor: nonfinite object node yaw");
        for(const float v:a.node->position)if(!std::isfinite(v))throw std::runtime_error("Solo actor: nonfinite object node position");
    }
    Impl::QueuedMessage m;m.kind=Impl::QueuedMessage::Kind::object_action;m.object_action=a;
    impl_->queue.push_back(std::move(m));
}
void SoloActorSimulation::queue_cancel_request(std::uint8_t kind,std::uint32_t word_2c){
    Impl::QueuedMessage m;m.kind=Impl::QueuedMessage::Kind::action;m.code=actor_cancel_request_action;
    m.payload=kind;m.word=word_2c;
    impl_->queue.push_back(std::move(m));
}
const ActorWorldStack& SoloActorSimulation::world_objects() const{return impl_->world_objects;}
// 00131320 / 001313f0 callbacks: the objects' virtual +5c (activate) and +60
// (deactivate) are recorded in order, not run. The published generation is
// the object's +c word, the population key of a script object.
ActorWorldStackHost SoloActorSimulation::Impl::world_stack_host(){
    ActorWorldStackHost h;
    h.snapshot=[this](std::uint64_t id){
        if(const auto it=hosted_world_objects.find(id);it!=hosted_world_objects.end())return it->second;
        if(id>static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()))
            throw std::runtime_error("Solo actor: world object key outside the population range");
        ActorWorldObjectSnapshot s;s.generation=static_cast<std::int32_t>(id);return s;};
    // A hosted object's +5c / +60 (the base camera's 003ce980 / 003ce790)
    // set / clear its +2d.
    h.activate=[this](std::uint64_t id,ActorWorldStack&){
        if(const auto it=hosted_world_objects.find(id);it!=hosted_world_objects.end())it->second.byte_2d=1;
        if(current)current->world_object_events.push_back({true,id});};
    h.deactivate=[this](std::uint64_t id,ActorWorldStack&){
        if(const auto it=hosted_world_objects.find(id);it!=hosted_world_objects.end())it->second.byte_2d=0;
        if(current)current->world_object_events.push_back({false,id});};
    return h;
}
// 001b3b60 -> 001a92a0 -> 0018d2c0 for the queued PerformAction_WithObject.
void SoloActorSimulation::Impl::deliver_object_action(const SoloActorSimulation::ObjectAction& a){
    ActorObjectActionMessage msg;msg.action=a.action;msg.kind=7;msg.time=a.time;
    if(a.object)msg.object=*a.object;
    const std::optional<std::uint64_t> object=msg.object;
    if(a.motion_name){
        // Kind-10 prelude (0018d300..0018d508): the resend runs in this pump.
        if(!model)throw std::runtime_error("Solo actor: object actions need an animation model");
        const auto resend=named_object_action_resend(static_cast<std::uint32_t>(a.action),0xa,object,*a.motion_name,a.time,a.byte_64,
            [this](const std::array<char,48>& n)->std::optional<std::uint32_t>{   // 0018c740 on model +24
                return model->motion_node_id(std::string(n.data(),static_cast<std::size_t>(std::find(n.begin(),n.end(),'\0')-n.begin())));});
        // Falling through, 0018d510 sees kind 10, which no hosted branch takes.
        if(!resend)throw std::runtime_error("Solo actor: named object action outside actions 0x25..0x27");
        msg=*resend;
        if(current){current->object_action_motion_id=msg.motion_id;current->object_action_byte_38=msg.byte_38;}
    }
    ActorObjectActionTarget target;target.node=a.node;target.world_object=a.world_object;
    ActorObjectActionHost h;
    h.anchor_key=[this](std::string_view n)->std::optional<RootAnchorKey>{
        if(!model)throw std::runtime_error("Solo actor: object actions need an animation model");
        return model->motion_anchor_key(n);};
    h.place_edge_root=[this](const RootParentTransform& t){
        roots.set_world_placement(edge_root,t,true);
        if(model&&model->has_anchor(SoloActorSimulation::edge_key))model->set_anchor_world_placement(SoloActorSimulation::edge_key,t,true);
    };
    h.floor_root_world_placement=[this]{return roots.world_placement(floor_root);};
    h.resolve=[this](std::string_view n){
        if(!model)throw std::runtime_error("Solo actor: object actions need an animation model");
        return model->resolve_motion(n);};
    h.anchor_key_by_id=[this](std::uint32_t id)->std::optional<RootAnchorKey>{
        if(!model)throw std::runtime_error("Solo actor: object actions need an animation model");
        return model->motion_anchor_key_by_id(id);};
    h.resolve_id=[this](std::uint32_t id){
        if(!model)throw std::runtime_error("Solo actor: object actions need an animation model");
        return model->resolve_motion_id(id);};
    h.retained=[this]{
        if(!model)throw std::runtime_error("Solo actor: object actions need an animation model");
        return model->retained_motion();};
    h.install=[this](const MotionRequest& r,MotionInstallMode mode){model->install_motion(r,mode);};
    // 0018fee8: code 0xd to the object at once (transport 1).
    h.reject=[this](std::uint64_t o){
        if(current)current->object_codes.push_back({o,0xd});
        deliver_object_code(o,0xd);};
    h.controller_is_player=[this]{return player();};
    h.push_world_object=[this](std::uint64_t o){push_actor_world_object(world_objects,o,world_stack_host());};
    if(a.object&&std::find(object_action_receivers.begin(),object_action_receivers.end(),*a.object)==object_action_receivers.end())
        object_action_receivers.push_back(*a.object);
    ActorObjectActionResult r;
    if(msg.action==0x32||msg.action==0x36)r=handle_actor_buddy_climb_cancel(actions,handle,msg,h);          // 001ad3a8
    else if(msg.action==0x31||msg.action==0x33||msg.action==0x34||msg.action==0x35||msg.action==0x37||msg.action==0x38)
        r=handle_actor_buddy_climb_action(actions,&frame_state.history.pending,handle,msg,target,h);          // 0018d2c0 buddy branches
    else r=handle_actor_object_action(actions,transition_2ef8,handle,msg,target,h);
    if(current){
        current->object_action=r;
        current->object_action_motions.insert(current->object_action_motions.end(),r.motions.begin(),r.motions.end());
    }
}
// InteractGOBJ CAS receiver 00216580 (class 0055a1d8) for a script-owned
// object. Code 0xe forwards the object's saved trigger (+bc) to its links;
// +bc is only set by the native trigger path (002165c0), which a script
// handler replaces, so it is empty and 0xe does nothing. Codes 0xf / 0xd free
// +bc, clear the actor from +b4[0..1] (+b0 = 0 when none remains) and, in a
// solo game (005721c8 clear), send the object itself a GOBJTriggerEventMsg
// (key 004f5440, vtable 004daf10) with transport 0: action 2 (NotifyDone) /
// 3 (NotifyInterrupted), +28 = 0, +2c = 0, +30 = the actor's key.
void SoloActorSimulation::Impl::deliver_object_code(std::uint64_t object,std::int32_t code){
    if(code!=0xf&&code!=0xd&&code!=0xe)
        throw std::runtime_error("Solo actor: interact-object CAS code "+std::to_string(code)+" is not hosted");
    if(object_code_receiver)object_code_receiver(object,code);
    if(code==0xe)return;
    if(object>std::numeric_limits<std::uint32_t>::max())
        throw std::runtime_error("Solo actor: interact object key outside the population range");
    QueuedMessage m;m.kind=QueuedMessage::Kind::trigger;
    m.trigger.receiver=static_cast<std::uint32_t>(object);
    m.trigger.action_24=code==0xf?2U:3U;
    queue.push_back(m);
}
std::uint8_t SoloActorSimulation::life_state() const{return impl_->life_state();}
SoloActorTeleportResult SoloActorSimulation::teleport(ActorTeleportMode mode,const ActorTeleportMatrix& matrix){
    return impl_->teleport(mode,matrix);
}
namespace {
SoloActorAnimation& takedown_model(SoloActorAnimation* model){
    if(!model)throw std::runtime_error("Solo actor: the takedown's model calls need an animation model");
    return *model;
}
}
ActorStunnedActionResult SoloActorSimulation::receive_stunned_action(std::uint32_t code_2c){
    auto& s=*impl_;
    takedown_model(s.model);
    ActorStunnedActionHost h;
    h.resolve=[this](std::string_view name){return resolve_mapped_motion(name);};        // 0018c5a0 / 0018c620
    h.retained=[this]{return retained_motion();};                                       // 001ae270 / 001ae260
    h.install=[this](const MotionRequest& q,MotionInstallMode m){install_motion(q,m);};  // 003ad500
    return handle_actor_stunned_action(s.actions,code_2c,h);
}
ActorMotionRegistration SoloActorSimulation::resolve_mapped_motion(std::string_view name) const{
    const auto& m=takedown_model(impl_->model);
    return m.resolve_motion_id(m.motion_node_id(name).value_or(0U));          // 0018c5a0 -> 0018c6b0
}
ActorMotionRegistration SoloActorSimulation::retained_motion() const{return takedown_model(impl_->model).retained_motion();}
void SoloActorSimulation::install_motion(const MotionRequest& request,MotionInstallMode mode){
    takedown_model(impl_->model).install_motion(request,mode);
}
void SoloActorSimulation::set_death_motion(std::string_view name,std::uint8_t code){
    const auto& m=takedown_model(impl_->model);
    if(code>=impl_->death_motion_34e4.size())
        throw std::runtime_error("Solo actor: 0018c9d0 with death code "+std::to_string(code)+" writes past +34e4[11]");
    // 0018c9d0: the bytes up to the first NUL, at most 47 (0018ca00..0018ca50).
    const auto end=std::min<std::size_t>(std::min<std::size_t>(name.find('\0'),name.size()),47);
    impl_->death_motion_34e4[code]=static_cast<std::int32_t>(m.motion_node_id(name.substr(0,end)).value_or(0U));
}
const std::array<std::int32_t,12>& SoloActorSimulation::death_motion_overrides() const{return impl_->death_motion_34e4;}
void SoloActorSimulation::request_physical_reset(){impl_->frame_state.tail.reset_requested=1;}
void SoloActorSimulation::push_pending_world_object(std::uint64_t object,const ActorWorldObjectSnapshot& state){
    auto& s=*impl_;
    if(!object)throw std::runtime_error("Solo actor: a pending world object needs a key");
    s.hosted_world_objects[object]=state;
    push_actor_world_object(s.world_objects,object,s.world_stack_host());       // 00131320(G, G +d4)
    s.world_objects.pending=true;s.world_objects.pending_object=object;         // G +d8 = 1
}
void SoloActorSimulation::finish_pending_world_object(){
    auto& s=*impl_;
    finish_actor_world_pending(s.world_objects,s.world_stack_host());           // 00131510(G)
}
void SoloActorSimulation::push_world_object(std::uint64_t object){
    push_actor_world_object(impl_->world_objects,object,impl_->world_stack_host());   // 00131320(G, object)
}
std::uint64_t SoloActorSimulation::pop_world_object(){
    return pop_actor_world_object(impl_->world_objects,impl_->world_stack_host());    // 00131240(G)
}
std::optional<ActorWorldObjectSnapshot> SoloActorSimulation::hosted_world_object(std::uint64_t object) const{
    const auto it=impl_->hosted_world_objects.find(object);
    if(it==impl_->hosted_world_objects.end())return std::nullopt;
    return it->second;
}
std::uint8_t SoloActorSimulation::physical_3d0() const{return impl_->physical_3d0;}
void SoloActorSimulation::clear_physical_3d0(){impl_->physical_3d0=0;}
std::uint8_t SoloActorSimulation::stun_immune_346a() const{return impl_->stun_immune_346a;}
void SoloActorSimulation::set_stun_immune_346a(std::uint8_t value){impl_->stun_immune_346a=value;}   // 00198f90's store (its replication needs *005721c8)
void SoloActorSimulation::set_burning_326a(bool value){impl_->health.burning_326a=value;}
void SoloActorSimulation::set_fire_damage_33b0(std::int32_t value){impl_->health.fire_damage_33b0=value;}
const ActorHeadingState& SoloActorSimulation::heading_state() const{return impl_->heading_state;}
int SoloActorSimulation::player_deaths() const{return impl_->player_deaths;}
void SoloActorSimulation::set_inventory_view(const ActorAnimationInventoryState& view,std::uint8_t byte_a0){
    impl_->actions.inventory=view;impl_->actions.attachment_byte_a0=byte_a0;
}
SoloDamageOutcome SoloActorSimulation::apply_damage(const SoloDamageRequest& r){
    auto& s=*impl_;
    // The actor's own logic may damage itself (the stun end 00184ce0: the
    // virtual +100(actor, actor, ...), as the fall damage); any other source
    // runs in its own phase.
    if(s.executing&&r.source!=s.handle&&!r.own_update)throw std::runtime_error("Solo actor: apply_damage from another actor during this actor's own step");
    if(r.own_update&&!s.executing)throw std::runtime_error("Solo actor: an own-update damage outside this actor's step");
    SoloDamageOutcome out;
    // Between frames the outcome carries the damage's record. Inside a world
    // frame (another actor's phase, as the melee hit 00141880 from the
    // attacker's logic: the virtual +100 runs at once) the record is this
    // actor's frame being produced, and the outcome's record stays empty.
    const bool in_frame=s.current!=nullptr;
    struct Restore{SoloActorFrame*& c;bool keep;~Restore(){if(!keep)c=nullptr;}} restore{s.current,in_frame};
    if(!in_frame)s.current=&out.record;
    ActorDamageInput in;in.actor=s.handle;
    // +58 virtuals +54/+58/+5c: player component 1/0/0; AI 0/1/0 (0012f370, 0015b7e0, 001535d0).
    in.controller=s.player()?ActorDamageController{true,false,false}:ActorDamageController{false,true,false};
    in.byte_14=s.actions.byte_14!=0;
    in.source={r.source,r.source_controller};
    in.category=r.category;in.type=r.type;in.penetration=r.penetration;in.quiet=r.quiet;in.hit_point=r.hit_point;in.amount=r.amount;
    std::optional<RootVector> source_position;
    if(r.source_position)source_position=*r.source_position;
    std::optional<SoloFireSetter> setter;
    if(r.type==4&&s.fire.fire_setter)setter=s.fire.fire_setter();             // 001873bc reads it for type 4 only
    if(setter)in.fire_setter_3480=setter->party;
    out.result=s.damage(in,source_position,setter?std::optional<std::pair<std::uint64_t,RootVector>>{{setter->party.id,setter->position_e4}}:std::nullopt);
    return out;
}
const ActorFader& SoloActorSimulation::fader() const{return impl_->fader;}
void SoloActorSimulation::set_fader_paused(bool paused){impl_->fader.paused_0c=paused;}
void SoloActorSimulation::start_level_fade_in(){start_actor_fade(impl_->fader,1,2.0F);}
LevelEndRequestResult SoloActorSimulation::request_level_end(std::uint8_t kind){
    return omega::request_level_end(impl_->game_end,kind,impl_->fader,false);
}
bool SoloActorSimulation::level_end_fade_check(){return omega::level_end_fade_check(impl_->game_end,impl_->fader,true);}
std::optional<std::uint8_t> SoloActorSimulation::level_end_teardown(){return omega::level_end_teardown(impl_->game_end);}
const GameEndState& SoloActorSimulation::game_end() const{return impl_->game_end;}
void SoloActorSimulation::set_end_agent(std::function<void(bool)> end_agent){impl_->game_end.end_agent=std::move(end_agent);}
void SoloActorSimulation::apply_game_state(std::uint8_t state_50,std::uint8_t previous_51){
    impl_->game_end.state_50=state_50;impl_->game_end.previous_51=previous_51;
}
const ActorRespawnPoll& SoloActorSimulation::respawn_poll() const{return impl_->respawn_poll;}
const ActorRespawnTimer& SoloActorSimulation::respawn_timer() const{return impl_->respawn_timer;}
std::uint8_t SoloActorSimulation::logic_gate_3269() const{return impl_->byte_3269;}
}
