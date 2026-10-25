#include "combat_actor_world.hpp"
#include "aim_view_targeting.hpp"
#include "caster_bounds.hpp"
#include "morph_resources.hpp"
#include "water_effects.hpp"
#include <stdexcept>
#include <string>

namespace omega {
namespace {
ShotFrame frame_of(const std::array<float,16>& m){
    ShotFrame f{};
    for(std::size_t r=0;r<4;++r)f[r]={m[4*r],m[4*r+1],m[4*r+2],m[4*r+3]};
    return f;
}
}

int actor_eye_node(std::int32_t subtarget){
    static constexpr std::uint8_t by_subtarget[4]={4,18,14,2};             // 0048a2a0
    static constexpr std::uint8_t node_codes[21]={0,2,5,9,14,8,13,17,19,10,15,18,20,1,4,7,12,3,6,11,16};   // 0049d020
    return node_codes[by_subtarget[static_cast<std::uint32_t>(subtarget)&3U]];
}

ActorWorldCombat::ActorWorldCombat(ActorWorld& world,ActorWorldCombatConfig config)
    :world_(world),config_(std::move(config)){
    const auto n=world_.size();
    if(config_.object_ids.size()!=n||config_.models.size()!=n)
        throw std::runtime_error("Combat actors: one object ID and one model entry per world actor");
    secondary_.assign(n,SecondaryComponent{});
    aim_.assign(n,make_secondary_aim_state());
    aim_slots_.assign(n,ActorAimState{});
    aim_angle_.assign(n,0.0F);
    aim_result_.assign(n,SecondaryAimResult{});
    gore_.assign(n,HitVolumeGore{});
    gore_2df2_.assign(n,0);
    draw_mode_2bda_.assign(n,2);
    for(std::size_t i=0;i<n;++i)if(config_.models[i])gore_[i].parts.resize(config_.models[i]->parts.size());
}

std::size_t ActorWorldCombat::size() const{return world_.size();}
std::uint64_t ActorWorldCombat::object(std::size_t a) const{return world_.actor(a).handle();}
std::int32_t ActorWorldCombat::object_id(std::size_t a) const{return config_.object_ids.at(a);}
bool ActorWorldCombat::player(std::size_t a) const{return world_.actor(a).player_controller();}
bool ActorWorldCombat::port_agent_slot(std::size_t a) const{return world_.in_agent_slot(a);}   // port-only (132ob7)
bool ActorWorldCombat::byte_14(std::size_t a) const{return world_.actor(a).byte_14()!=0;}
// ActorWorld never frees an actor, so its weak handles keep resolving.
bool ActorWorldCombat::alive_handle(std::size_t a) const{(void)world_.actor(a);return true;}

ActorFireState ActorWorldCombat::fire_state(std::size_t a){
    auto& s=world_.actor(a);
    if(!config_.agent_unlock_0)throw std::runtime_error("Combat actors: the agent-data unlock 0 (00332f90) is not supplied");
    ActorFireState f;
    f.life_state=s.life_state();
    f.flags_3210=s.actions().active_actions;f.flags_3248=s.actions().flags_3248;
    f.byte_33e4=s.actions().byte_33e4!=0;f.byte_33e8=s.actions().byte_33e8!=0;
    // +3314 exists while the actor is activated (001b5560 / 001b5190).
    if(s.eligible())f.secondary_states=std::array<std::uint8_t,2>{secondary_.at(a).states.current_310,secondary_.at(a).states.next_311};
    f.agent_unlock_0=*config_.agent_unlock_0;
    return f;
}

const CombatActorModel& ActorWorldCombat::model(std::size_t a) const{
    const auto& m=config_.models.at(a);
    if(!m)throw std::runtime_error("Combat actors: actor "+std::to_string(a)+" has no model data (part meshes, inverse binds, attach points)");
    return *m;
}

// Controller +64 outside the aim view (00176400 / 0014f380 -> 00385240): the
// hand's attach point through its skeleton node and the model component; the
// attached model's muzzle offset +130 (00385370) when one is attached (only
// with the weapon model library: record +13c is not loaded otherwise).
ShotFrame ActorWorldCombat::shot_frame(std::size_t a,bool second_hand,bool aim_view){
    if(aim_view){                                                    // 00176400 with a3 != 0
        if(!config_.aim_view_camera)throw std::runtime_error("Combat actors: the aim-view shot frame 00176400 needs the camera +50 transform (not supplied)");
        return aim_view_shot_frame(config_.aim_view_camera(a));
    }
    const auto& m=model(a);
    if(!m.attach)throw std::runtime_error("Combat actors: attach points of 0038e100 are not built (characters without a hard mesh use default frames that are not ported)");
    const auto& point=(*m.attach)[second_hand?1:0];
    if(!point)throw std::runtime_error("Combat actors: the hand attach point was not written by 0038e100");
    return attach_point_frame(a,second_hand?1:0,*point);
}

ShotFrame ActorWorldCombat::attach_point_frame(std::size_t a,std::uint8_t index,const WeaponAttachPoint& point){
    auto& s=world_.actor(a);
    const auto nodes=s.skeleton_node_matrices();
    if(point.node>=nodes.size())throw std::runtime_error("Combat actors: attach point node outside the skeleton");
    const auto component=frame_of(root_placement_matrix(s.component_placement()));
    std::optional<RootVector> muzzle;
    if(config_.attached_muzzle)muzzle=config_.attached_muzzle(a,index);
    return weapon_muzzle_frame(shot_frame_node_world(frame_of(nodes[point.node]),component),point,muzzle);
}

const ModelAttachSlots* ActorWorldCombat::attach_slots(std::size_t a) const{
    const auto& m=model(a);
    return m.attach?&*m.attach:nullptr;
}

std::optional<ShotFrame> ActorWorldCombat::attach_point_world(std::size_t a,std::uint8_t p){
    const auto& m=config_.models.at(a);
    if(!m||!m->attach||p>=m->attach->size())return std::nullopt;
    const auto& point=(*m->attach)[p];
    if(!point)return std::nullopt;
    auto& s=world_.actor(a);
    const auto nodes=s.skeleton_node_matrices();
    if(point->node>=nodes.size())throw std::runtime_error("Combat actors: attach point node outside the skeleton");
    const auto component=frame_of(root_placement_matrix(s.component_placement()));
    return shot_frame_node_world(point->local,shot_frame_node_world(frame_of(nodes[point->node]),component));
}

std::optional<ShotFrame> ActorWorldCombat::attach_node_world(std::size_t a,std::uint8_t p){
    const auto& m=config_.models.at(a);
    if(!m||!m->attach||p>=m->attach->size())return std::nullopt;
    const auto& point=(*m->attach)[p];
    if(!point)return std::nullopt;
    auto& s=world_.actor(a);
    const auto nodes=s.skeleton_node_matrices();
    if(point->node>=nodes.size())throw std::runtime_error("Combat actors: attach point node outside the skeleton");
    return shot_frame_node_world(frame_of(nodes[point->node]),frame_of(root_placement_matrix(s.component_placement())));
}

// 00384a70: 00385420 for the hand, and the model context's node 0 (0019f350 through the
// component; the component alone, 00133300, without a node collection).
std::optional<std::array<ShotFrame,2>> ActorWorldCombat::shell_spawn_frames(std::size_t a,std::size_t hand){
    const auto attach=attach_point_world(a,static_cast<std::uint8_t>(hand));
    if(!attach)return std::nullopt;
    auto& s=world_.actor(a);
    const auto nodes=s.skeleton_node_matrices();
    const auto component=frame_of(root_placement_matrix(s.component_placement()));
    return std::array<ShotFrame,2>{*attach,nodes.empty()?component:shot_frame_node_world(frame_of(nodes[0]),component)};
}

RootVector ActorWorldCombat::position_e4(std::size_t a){return world_.actor(a).published_position();}
// 00188d80: a present actor's eye node through 00132910, else the component translation.
RootVector ActorWorldCombat::eye_c0(std::size_t a,std::int32_t subtarget){
    auto& s=world_.actor(a);
    if(s.eligible())return s.skeleton_node_world_position(actor_eye_node(subtarget));
    return s.published_position();
}
// +2e80: the model context bounds (001af460 + 003a9220, skeleton space).
std::array<RootVector,2> ActorWorldCombat::bounds_88(std::size_t a){
    const auto b=human_context_bounds(world_.actor(a).skeleton_node_matrices());
    return {RootVector{b.minimum[0],b.minimum[1],b.minimum[2],b.minimum[3]},RootVector{b.maximum[0],b.maximum[1],b.maximum[2],b.maximum[3]}};
}
std::uint32_t ActorWorldCombat::flags_3204(std::size_t a){return world_.actor(a).state_words().current_3204;}
std::int32_t ActorWorldCombat::armor_33a0(std::size_t a){return world_.actor(a).health().armor_33a0;}
bool ActorWorldCombat::invulnerable_33a4(std::size_t a){return world_.actor(a).health().invulnerable_33a4;}
std::uint8_t ActorWorldCombat::life_state(std::size_t a){return world_.actor(a).life_state();}

BulletHitVolumes ActorWorldCombat::hit_volumes(std::size_t a,const RootVector& origin,const RootVector& direction,std::int32_t mode,float volume){
    const auto& m=model(a);
    auto& s=world_.actor(a);
    const auto nodes=hit_shot_frames(s.skeleton_node_matrices());
    HitVolumeActor h;
    h.parts=m.parts;h.gore=&gore_.at(a);h.nodes=nodes;h.inverse_bind=m.inverse_bind;
    h.component=frame_of(root_placement_matrix(s.component_placement()));
    h.life_state=s.life_state();h.gore_2df2=&gore_2df2_.at(a);
    std::vector<HitVolumeEffect> effects;
    auto out=bullet_hit_volumes(h,origin,direction,mode,volume,effects);
    for(const auto& e:effects)effects_.push_back({a,e,effect_cursor_?effect_cursor_():ActorHitEffect::Cursor{}});
    return out;
}

void ActorWorldCombat::damage(std::size_t victim,std::optional<std::size_t> shooter,std::uint8_t category,std::uint8_t type,
    std::int32_t& penetration,float amount){
    SoloDamageRequest r;
    if(shooter){
        auto& s=world_.actor(*shooter);
        r.source=s.handle();
        r.source_controller=s.damage_controller();
        r.source_position=s.published_position();
    }
    r.category=category;r.type=type;r.penetration=penetration;r.amount=amount;   // 00188780: quiet 0, hit point null
    auto out=world_.actor(victim).apply_damage(r);
    if(out.result.penetration)penetration=*out.result.penetration;
    damage_.emplace_back(victim,std::move(out));
}
void ActorWorldCombat::damage_from_object(std::size_t victim,std::uint64_t source,const RootVector& source_e4,std::uint8_t category,
    std::uint8_t type,std::int32_t& penetration,float amount){
    SoloDamageRequest r;
    r.source=source;                         // 00188c28: a1 = the shooter object
    r.source_position=source_e4;             // its virtual +e4; no +58 controller
    r.category=category;r.type=type;r.penetration=penetration;r.amount=amount;   // 00188780: quiet 0, hit point null
    auto out=world_.actor(victim).apply_damage(r);
    if(out.result.penetration)penetration=*out.result.penetration;
    damage_.emplace_back(victim,std::move(out));
}
void ActorWorldCombat::damage_without_penetration(std::size_t victim,std::optional<std::size_t> shooter,std::uint8_t category,
    std::uint8_t type,float amount){
    SoloDamageRequest r;
    if(shooter){
        auto& s=world_.actor(*shooter);
        r.source=s.handle();
        r.source_controller=s.damage_controller();
        // An actor without a frame yet (a script Kill of a deactivated NPC, SEWER's CompleteDeathSquad) reads its
        // load placement; one whose step had no response store (0x001e4dd8) the model step's translation.
        r.source_position=s.published_position();
    }
    r.category=category;r.type=type;r.amount=amount;                                // quiet 0, hit point null, penetration null
    damage_.emplace_back(victim,world_.actor(victim).apply_damage(r));
}
void ActorWorldCombat::damage_request(std::size_t victim,const SoloDamageRequest& request){
    damage_.emplace_back(victim,world_.actor(victim).apply_damage(request));
}
ActorDamageController ActorWorldCombat::damage_controller(std::size_t actor){
    return world_.actor(actor).damage_controller();
}

std::optional<BulletStaticHit> ActorWorldCombat::static_hit(const RootVector& start,const RootVector& displacement,std::uint64_t excluded){
    if(!config_.static_hit)throw std::runtime_error("Combat actors: bullet static collision (00148290 -> 003c3a60) is not hosted");
    return config_.static_hit(start,displacement,excluded);
}
std::int32_t ActorWorldCombat::static_object_id(std::uint64_t object){
    if(!config_.static_object_id)throw std::runtime_error("Combat actors: a static object's +c (weak handle generation) is not modelled");
    return config_.static_object_id(object);
}
void ActorWorldCombat::static_damage(std::uint64_t object,std::uint64_t shooter,std::uint8_t type,float amount){
    if(!config_.static_damage)throw std::runtime_error("Combat actors: a static object's damage virtual +100 (003ccb00 on terrain objects) is not hosted");
    config_.static_damage(object,shooter,type,amount);
}
std::int32_t ActorWorldCombat::effect_id(std::int32_t index){
    if(config_.effect_table)return config_.effect_table(index);
    if(config_.effects_not_drawn)return -1;
    throw std::runtime_error("Combat actors: the effect table *00236f20("+std::to_string(index)+") is not hosted");
}
const MaterialRow* ActorWorldCombat::static_material(const BulletStaticHit& hit){
    return config_.static_material?config_.static_material(hit):nullptr;
}
bool ActorWorldCombat::effect_service(){
    if(config_.effect_table)return true;
    if(config_.effects_not_drawn)return false;
    throw std::runtime_error("Combat actors: the effect service 003ebfb0(0x18) is not hosted");
}
// Actor +38: 0x80 from cGOBJ_ctor (003cf0e0); the scene frame 0013e1b0 rewrites it (0x80, 2, then the
// cull code of 003c8990) when the host runs it.
std::uint32_t ActorWorldCombat::word_38(std::size_t a){
    (void)world_.actor(a);
    return config_.scene_cull_38?config_.scene_cull_38(a):0x80U;
}
float ActorWorldCombat::value_005383d8(){
    if(config_.frame_rate_1e8)return config_.frame_rate_1e8();
    throw std::runtime_error("Combat actors: the video object's +1e8 (005383d8, 0037a9f0's measured frame rate) is not hosted");
}

SecondaryComponent& ActorWorldCombat::secondary(std::size_t a){return secondary_.at(a);}
SecondaryModeChoice ActorWorldCombat::apply_secondary_mode(std::size_t a,const SecondaryModeInput& input){
    const auto choice=choose_secondary_mode(input);
    auto& st=secondary_.at(a).states;
    apply_actor_secondary_mode(st.request_312,st.request_313,choice.mode);  // 001cb120
    return choice;
}
SecondaryStep ActorWorldCombat::step_secondary(std::size_t a,float dt){
    auto& actor=world_.actor(a);
    const auto nodes=actor.skeleton_node_matrices();
    const auto component=root_placement_matrix(actor.component_placement());
    const auto base=actor.base_aggregate();
    SecondaryAimInput in;
    auto& c=secondary_.at(a);
    in.current_310=c.states.current_310;in.next_311=c.states.next_311;in.dt=dt;
    for(std::size_t k=0;k<16;++k)in.model.component[k]=component[k];
    in.model.nodes=nodes;in.model.node0_rotation=actor.bone0_rotation();
    in.model.base_orientation=base.orientation;in.model.base_position=base.position;
    in.random=[this]{return next_game_random(world_.random());};
    const auto r=step_secondary_aim(aim_.at(a),aim_slots_.at(a),in);
    aim_angle_.at(a)=r.angle;aim_result_.at(a)=r;
    return step_secondary_states(c,r.angle,dt);
}
std::vector<SecondaryStep> ActorWorldCombat::step_secondaries(float dt){
    std::vector<SecondaryStep> steps(size());
    for(std::size_t a=0;a<size();++a)if(world_.actor(a).eligible())steps[a]=step_secondary(a,dt);
    return steps;
}
void ActorWorldCombat::attach_secondary_updates(){
    for(std::size_t a=0;a<size();++a)attach_secondary_update(a);
    secondary_attached_=true;
}
void ActorWorldCombat::attach_secondary_update(std::size_t a){
    world_.actor(a).set_secondary_update([this,a](float dt){
        const auto& st=secondary_.at(a).states;
        const bool first=st.current_310==0||st.next_311==0;
        auto step=step_secondary(a,dt);
        if(secondary_playback_)secondary_playback_(a,step,first,dt);
        secondary_steps_.emplace_back(a,std::move(step));
        // 00392aa0 then runs the render update 00389fa0 (actor +60), whose 0038a118 ages the gore with the
        // frame counter 005383c8 of this step and the draw mode the last draw stored.
        if(update_gore(a,draw_mode_2bda_.at(a),world_.frame_counter()))++gore_saturations_;});
}
void ActorWorldCombat::append_actor(std::int32_t object_id,std::optional<CombatActorModel> model){
    const auto a=config_.object_ids.size();
    if(world_.size()!=a+1)throw std::runtime_error("Combat actors: append one actor right after the world appends it");
    config_.object_ids.push_back(object_id);config_.models.push_back(std::move(model));
    secondary_.emplace_back();aim_.push_back(make_secondary_aim_state());aim_slots_.emplace_back();aim_angle_.push_back(0.0F);aim_result_.emplace_back();
    gore_.emplace_back();gore_2df2_.push_back(0);draw_mode_2bda_.push_back(2);
    if(config_.models[a])gore_[a].parts.resize(config_.models[a]->parts.size());
    if(secondary_attached_)attach_secondary_update(a);
}
void ActorWorldCombat::reset_actor(std::size_t a,std::optional<CombatActorModel> model){
    if(a>=config_.object_ids.size())throw std::runtime_error("Combat actors: reset of an unknown actor");
    config_.models[a]=std::move(model);
    secondary_[a]={};aim_[a]=make_secondary_aim_state();aim_slots_[a]={};aim_angle_[a]=0.0F;aim_result_[a]={};
    gore_[a]={};gore_2df2_[a]=0;draw_mode_2bda_[a]=2;
    if(config_.models[a])gore_[a].parts.resize(config_.models[a]->parts.size());
    if(secondary_attached_)attach_secondary_update(a);
}
bool ActorWorldCombat::update_gore(std::size_t a,std::int8_t draw_mode_2bda,std::uint32_t frame){
    if(a>=gore_.size())return false;
    if(!config_.models[a]){
        if(gore_[a].level_2c08!=0)throw std::runtime_error("0038a118: gore state on an actor without a model");
        return false;
    }
    return update_hit_gore(config_.models[a]->parts,gore_[a],draw_mode_2bda,frame);
}
void ActorWorldCombat::replace_model_parts(std::size_t a,std::vector<HitVolumePart> parts){
    if(a>=config_.models.size()||!config_.models[a])throw std::runtime_error("0038c920: body switch on an actor without a hit model");
    config_.models[a]->parts=std::move(parts);
    gore_.at(a).parts.assign(config_.models[a]->parts.size(),std::nullopt);
}
void ActorWorldCombat::recreate_secondary(std::size_t a){
    secondary_.at(a)=SecondaryComponent{};aim_.at(a)=make_secondary_aim_state();aim_angle_.at(a)=0.0F;aim_result_.at(a)={};
    auto& slot=aim_slots_.at(a);
    slot.aim_active_3a5=0;slot.aim_point_3b0={};slot.head_active_415=0;slot.head_point_420={};
}
void ActorWorldCombat::start_throw(std::size_t a,std::int32_t hold_class,const ActorFireRequest& request){
    if(!throw_start_){CombatActors::start_throw(a,hold_class,request);return;}
    throw_start_(a,hold_class,request);
}
void ActorWorldCombat::set_throw_start(std::function<void(std::size_t,std::int32_t,const ActorFireRequest&)> start){throw_start_=std::move(start);}
void ActorWorldCombat::melee_attack(std::size_t a){
    if(!melee_attack_){CombatActors::melee_attack(a);return;}
    melee_attack_(a);
}
void ActorWorldCombat::set_melee_attack(std::function<void(std::size_t)> melee){melee_attack_=std::move(melee);}
void ActorWorldCombat::set_torch_lights(LevelLights* lights){config_.lights=lights;}
// The torch branch's services over the actor: the point pool of the level's
// lights, *00236f20(7) (effect_id), the world's game RNG (00127dd0), attach
// point 0 (00385330(actor + 0x60, 0): the hand attach point's world frame,
// row 3 of shot_frame) and the root bytes +2c / +2d / +2e; the flicker
// object's presentation is recorded (take_torch_calls).
const TorchLightHost* ActorWorldCombat::torch_host(std::size_t a){
    if(!config_.lights)return nullptr;
    if(torch_hosts_.size()!=size())torch_hosts_.resize(size());
    auto& h=torch_hosts_.at(a);
    h.lights=config_.lights;
    if(h.random)return &h;
    using Kind=TorchFlickerCall::Kind;
    const auto record=[this,a](Kind kind,std::uint32_t o,std::int32_t effect,float value,const RootVector& p){
        TorchFlickerCall c;c.kind=kind;c.object=o;c.effect=effect;c.value=value;c.position=p;
        torch_calls_.emplace_back(a,c);};
    h.effect_id=[this](std::int32_t index){return effect_id(index);};
    h.random=[this]{return next_game_random(world_.random());};
    h.attach_point=[this,a]{return shot_frame(a,false,false)[3];};
    h.actor_shown=[this,a]{return world_.actor(a).eligible();};
    h.create_object=[this,record](std::int32_t effect){
        const auto o=++torch_objects_;record(Kind::create,o,effect,0.0F,{});return o;};
    h.register_object=[record](std::uint32_t o){record(Kind::register_object,o,-1,0.0F,{});};
    h.create_emitter=[record](std::uint32_t o,std::int32_t effect){record(Kind::create_emitter,o,effect,0.0F,{});};
    h.destroy_emitter=[record](std::uint32_t o){record(Kind::destroy_emitter,o,-1,0.0F,{});};
    h.emitter_scale=[record](std::uint32_t o,float f){record(Kind::emitter_scale,o,-1,f,{});};
    h.set_position=[record](std::uint32_t o,const RootVector& p){record(Kind::set_position,o,-1,0.0F,p);};
    h.unregister_object=[record](std::uint32_t o){record(Kind::unregister_object,o,-1,0.0F,{});};
    h.release=[record](std::uint32_t o,float delay){record(Kind::release,o,-1,delay,{});};
    return &h;
}
std::vector<std::pair<std::size_t,TorchFlickerCall>> ActorWorldCombat::take_torch_calls(){
    auto out=std::move(torch_calls_);torch_calls_.clear();return out;
}
void ActorWorldCombat::set_secondary_playback(std::function<void(std::size_t,const SecondaryStep&,bool,float)> playback){
    secondary_playback_=std::move(playback);}
std::vector<std::pair<std::size_t,SecondaryStep>> ActorWorldCombat::take_secondary_steps(){
    auto out=std::move(secondary_steps_);secondary_steps_.clear();return out;
}
SecondaryAimState& ActorWorldCombat::secondary_aim(std::size_t a){return aim_.at(a);}
ActorAimState& ActorWorldCombat::aim_state(std::size_t a){return aim_slots_.at(a);}
float ActorWorldCombat::last_aim_angle(std::size_t a) const{return aim_angle_.at(a);}
const SecondaryAimResult& ActorWorldCombat::last_aim_result(std::size_t a) const{return aim_result_.at(a);}

void ActorWorldCombat::publish_inventory_views(const CombatWorld& combat){
    for(std::size_t a=0;a<size();++a)publish_inventory_view(a,combat);
}
void ActorWorldCombat::collect_inventory_views(CombatWorld& combat){
    for(std::size_t a=0;a<size();++a)collect_inventory_view(a,combat);
}
void write_inventory_view(const WeaponInventory& inv,ActorAnimationReceiverState& state){
    if(!state.inventory)throw std::runtime_error("Combat actors: the actor has no inventory view");
    auto& v=*state.inventory;                                   // keeps the owner and object identities
    for(std::size_t k=0;k<6;++k){v.entries[k].item=inv.slots[k].item;v.entries[k].count=inv.slots[k].spare;}
    v.mode=inv.current_84;v.saved_mode=inv.previous_85;v.byte_ef=inv.busy_ef;
    v.pending_bc=inv.burst_bc[0];v.pending_c0=inv.burst_bc[1];v.selection_a4=inv.selection_a4;
    v.owner_control_active=inv.object_0&&inv.object_0->state_8!=0;
    state.attachment_byte_a0=static_cast<std::uint8_t>(inv.reloading_a0?1:0);
}
void read_inventory_view(const ActorAnimationReceiverState& state,WeaponInventory& inv){
    const auto& view=state.inventory;
    if(!view)throw std::runtime_error("Combat actors: the actor lost its inventory view");
    for(std::size_t k=0;k<6;++k){
        // The receiver's only item write is the emptied throwable's clear (00141cd0: 00141de8..00141df8 stores -1
        // at slot +4 of the same +3524 record, then 00143a60 cycles); any other change is not hosted.
        if(view->entries[k].item!=inv.slots[k].item) {
            if(view->entries[k].item!=-1)throw std::runtime_error("Combat actors: the receiver changed an inventory item (not hosted)");
            inv.slots[k].item=-1;
        }
        inv.slots[k].spare=view->entries[k].count;
    }
    inv.current_84=view->mode;inv.previous_85=view->saved_mode;inv.busy_ef=view->byte_ef;inv.selection_a4=view->selection_a4;
    inv.burst_bc={view->pending_bc,view->pending_c0};
    if(const auto a0=state.attachment_byte_a0)inv.reloading_a0=*a0!=0;
}
void ActorWorldCombat::publish_inventory_view(std::size_t a,const CombatWorld& combat){
    auto state=world_.actor(a).actions();
    write_inventory_view(combat.inventory(a),state);
    world_.actor(a).set_inventory_view(*state.inventory,*state.attachment_byte_a0);
}
void ActorWorldCombat::collect_inventory_view(std::size_t a,CombatWorld& combat){
    read_inventory_view(world_.actor(a).actions(),combat.inventory(a));
}

std::vector<std::pair<std::size_t,SoloDamageOutcome>> ActorWorldCombat::take_damage(){auto o=std::move(damage_);damage_.clear();return o;}
std::vector<ActorHitEffect> ActorWorldCombat::take_effects(){auto o=std::move(effects_);effects_.clear();return o;}

}
