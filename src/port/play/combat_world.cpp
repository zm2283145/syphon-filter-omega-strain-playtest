#include "combat_world.hpp"
#include "port_cheats.hpp"
#include "water_effects.hpp"
#include <stdexcept>

namespace omega {

const MaterialRow* CombatActors::static_material(const BulletStaticHit&){return nullptr;}
bool CombatActors::port_agent_slot(std::size_t) const{return false;}
void CombatActors::damage_from_object(std::size_t,std::uint64_t,const RootVector&,std::uint8_t,std::uint8_t,std::int32_t&,float){
    throw std::runtime_error("Combat actors: the damage virtual +100 (00187360) from a source that is not an actor is not hosted");
}
void CombatActors::start_throw(std::size_t,std::int32_t hold_class,const ActorFireRequest&){
    throw std::runtime_error(hold_class==1?"Combat world: the throw motion (00197560) is not hosted":
        "Combat world: the hold-class-7 path (00197140) is not hosted");
}
void CombatActors::melee_attack(std::size_t){
    throw std::runtime_error("Combat world: the melee attack (001414d0 / 00196480) is not hosted");
}
const TorchLightHost* CombatActors::torch_host(std::size_t){return nullptr;}
std::optional<std::array<ShotFrame,2>> CombatActors::shell_spawn_frames(std::size_t,std::size_t){return std::nullopt;}

CombatWorld::CombatWorld(const WeaponDatabase& database,CombatActors& actors,std::uint64_t& random)
    :db_(database),actors_(actors),random_(random){}

void CombatWorld::construct_inventories(const std::vector<bool>& with_object){
    if(with_object.size()!=actors_.size())throw std::runtime_error("Combat world: one constructor flag per actor");
    inventories_.clear();aims_.assign(actors_.size(),WeaponAimState{});inventory_hosts_.assign(actors_.size(),WeaponInventoryHost{});
    attach_.assign(actors_.size(),ModelAttachTable{});shells_.assign(actors_.size(),ShellEjectState{});
    point_bytes_.assign(actors_.size(),{});hidden_bytes_.assign(actors_.size(),{});
    for(std::size_t i=0;i<actors_.size();++i){
        std::vector<WeaponInventoryEvent> events;
        WeaponInventoryHost host;host.player=actors_.player(i);host.byte_14=actors_.byte_14(i);
        if(models_)host.apply=[this,i](const WeaponInventoryEvent& e){apply_event(i,e);};
        inventories_.push_back(WeaponInventoryOps::construct(actors_.object(i),with_object[i],db_,host,events));
        check_events(events);
    }
}

void CombatWorld::append_inventory(bool with_object){
    const auto i=inventories_.size();
    if(actors_.size()!=i+1)throw std::runtime_error("Combat world: append one inventory right after the actors append one");
    aims_.emplace_back();inventory_hosts_.emplace_back();attach_.emplace_back();shells_.emplace_back();point_bytes_.emplace_back();hidden_bytes_.emplace_back();
    std::vector<WeaponInventoryEvent> events;
    WeaponInventoryHost host;host.player=actors_.player(i);host.byte_14=actors_.byte_14(i);
    if(models_)host.apply=[this,i](const WeaponInventoryEvent& e){apply_event(i,e);};
    inventories_.push_back(WeaponInventoryOps::construct(actors_.object(i),with_object,db_,host,events));
    check_events(events);
}

void CombatWorld::reset_inventory(std::size_t i,bool with_object){
    if(i>=inventories_.size())throw std::runtime_error("Combat world: reset of an unknown inventory");
    aims_[i]=WeaponAimState{};inventory_hosts_[i]=WeaponInventoryHost{};attach_[i]=ModelAttachTable{};shells_[i]=ShellEjectState{};
    std::vector<WeaponInventoryEvent> events;
    WeaponInventoryHost host;host.player=actors_.player(i);host.byte_14=actors_.byte_14(i);
    if(models_)host.apply=[this,i](const WeaponInventoryEvent& e){apply_event(i,e);};
    inventories_[i]=WeaponInventoryOps::construct(actors_.object(i),with_object,db_,host,events);
    check_events(events);
}

WeaponInventory& CombatWorld::inventory(std::size_t actor){return inventories_.at(actor);}
const WeaponInventory& CombatWorld::inventory(std::size_t actor) const{return inventories_.at(actor);}
WeaponAimState& CombatWorld::aim(std::size_t actor){return aims_.at(actor);}

WeaponInventoryOps CombatWorld::inventory_ops(std::size_t actor,bool increased_ammo,std::vector<WeaponInventoryEvent>& events){
    auto& host=inventory_hosts_.at(actor);
    host=inventory_host(actor,increased_ammo);
    return WeaponInventoryOps(inventories_.at(actor),db_,host,events);
}

WeaponInventoryHost CombatWorld::inventory_host(std::size_t actor,bool increased_ammo){
    WeaponInventoryHost host;host.player=actors_.player(actor);host.byte_14=actors_.byte_14(actor);host.increased_ammo=increased_ammo;
    // actor +60 +2410: models are attached only from record +13c, a runtime
    // resource the native loader leaves 0 without the weapon model library
    // (00145710 preload is not run), so the table stays empty; an attach
    // event is then refused by the callers below.
    if(models_){
        host.attached=[this,actor](std::uint8_t point){return attach_.at(actor).at(point)!=0;};
        host.apply=[this,actor](const WeaponInventoryEvent& e){apply_event(actor,e);};
    }else host.attached=[](std::uint8_t){return false;};
    host.position_c0=[this,actor]{return actors_.eye_c0(actor,-1);};
    return host;
}

void CombatWorld::check_inventory_events(const std::vector<WeaponInventoryEvent>& events){
    for(const auto& e:events)
        if(e.kind==WeaponInventoryEvent::Kind::attach)
            throw std::runtime_error("Combat world: attached weapon models (record +13c, 0038c7d0) are not hosted");
}

void CombatWorld::set_weapon_models(WeaponModelLibrary* library){
    if(!inventories_.empty())throw std::runtime_error("Combat world: set the weapon model library before the inventories are constructed");
    models_=library;
}

void CombatWorld::check_events(const std::vector<WeaponInventoryEvent>& events) const{
    if(!models_)check_inventory_events(events);
}

void CombatWorld::apply_event(std::size_t actor,const WeaponInventoryEvent& e){
    using K=WeaponInventoryEvent::Kind;
    if(!models_)throw std::runtime_error("Combat world: inventory events run only with the weapon model library");
    switch(e.kind){
    case K::preload:models_->preload(e.item,0);break;     // 00143df0: 00145710(db, item, 0)
    case K::unload:models_->unload(e.item,0);break;       // 00142450: 00145630(db, item, 0)
    case K::attach:case K::detach:case K::move_attach:
        apply_attach_events(attach_.at(actor),std::span<const WeaponInventoryEvent>(&e,1),
            [this](std::int32_t model){return models_->attached(model);});
        break;
    case K::clear_attach_24a2:    // 00143df0: actor +24a2 + point = 0
        if(e.point<0||e.point>=10)throw std::runtime_error("Combat world: clear_attach_24a2 outside points 0..9");
        point_bytes_.at(actor)[static_cast<std::size_t>(e.point)]=0;
        break;
    default:break;
    }
}

const ModelAttachTable& CombatWorld::attach_table(std::size_t actor) const{return attach_.at(actor);}

void CombatWorld::write_slot_render_byte(std::size_t actor,std::uint8_t slot,std::uint8_t value){
    static constexpr std::uint8_t points[]{3,4,2,5,8,9,6,7};    // 00489d70
    if(slot>=8)throw std::runtime_error("00142e00: slot outside 0..7");
    point_bytes_.at(actor)[points[slot]]=value;
}

std::uint8_t CombatWorld::render_point_byte(std::size_t actor,std::uint8_t point) const{
    if(point>=10)throw std::runtime_error("Combat world: render byte outside points 0..9");
    return point_bytes_.at(actor)[point];
}

void CombatWorld::set_hardpoint_vis(std::size_t actor,std::uint8_t slot,bool visible){
    static constexpr std::uint8_t points[]{3,4,2,5,8,9,6,7};    // 00489d70
    if(slot>=8)throw std::runtime_error("00142e40: slot outside 0..7 (00489d70 has 8 points)");
    inventories_.at(actor).slots[slot].byte_0=visible?1:0;      // 00142e54: sb a2, 4(inv + 16 * slot)
    hidden_bytes_.at(actor)[points[slot]]=visible?0:1;          // 00142e78: sb !vis, +2498 + point
}

std::uint8_t CombatWorld::hidden_point_byte(std::size_t actor,std::uint8_t point) const{
    if(point>=10)throw std::runtime_error("Combat world: hidden byte outside points 0..9");
    return hidden_bytes_.at(actor)[point];
}
ShellEjectState& CombatWorld::shells(std::size_t actor){return shells_.at(actor);}
const ShellEjectState& CombatWorld::shells(std::size_t actor) const{return shells_.at(actor);}

void CombatWorld::queue_fire(std::size_t actor,const ActorFireRequest& request){
    if(actor>=actors_.size())throw std::runtime_error("Combat world: fire request for an unknown actor");
    queue_.emplace_back(actor,request);
}

std::optional<std::size_t> CombatWorld::actor_of(std::uint64_t object) const{
    for(std::size_t i=0;i<actors_.size();++i)if(actors_.object(i)==object)return i;
    return std::nullopt;
}

WeaponDischargeHost CombatWorld::discharge_host(std::size_t actor,std::vector<CombatEvent>& events){
    WeaponDischargeHost h;
    h.random=[this]{return next_game_random(random_);};
    h.shot_frame=[this,actor](bool second,bool aim_view){return actors_.shot_frame(actor,second,aim_view);};
    h.hit_roll=[this,actor]{return weapon_hit_roll(aims_.at(actor),actors_.player(actor),[this]{return next_game_random(random_);});};
    h.bullet=[this,actor,&events](const ShotRequest& r){
        std::vector<BulletEvent> be;
        const auto handle=pool_.request(r,db_,pool_host(events),be);
        for(auto& e:be){CombatEvent c;c.actor=actor;c.bullet=e;events.push_back(c);}
        return handle;};
    h.link_bullets=[this](std::uint64_t a,std::uint64_t b){pool_.link(a,b);};
    h.effect_id=[this](std::int32_t index){return actors_.effect_id(index);};
    h.effect_service=[this]{return actors_.effect_service();};
    if(models_){
        h.model_flag_411=[this](std::int32_t model){return models_->flare_411(model);};
        // 00384a70(actor +60, hand): the hand's +2410 model, its +404, then the matrices and 00127dd0.
        h.eject_shell=[this,actor](bool second){
            const std::size_t hand=second?1:0;
            const auto handle=attach_.at(actor).at(hand);
            if(handle==0)return;
            const auto& gun=models_->model(handle);
            if(!gun.all_defined(0x404,4))throw std::runtime_error("Combat world: the shell eject reads an undefined gun +404");
            if(gun.word(0x404)==0)return;
            const auto frames=actors_.shell_spawn_frames(actor,hand);
            if(!frames)throw std::runtime_error("Combat world: the shell eject 00384a70 needs the hand's attach matrix (00385420) and the model's root matrix (not hosted)");
            shell_eject_spawn(shells_.at(actor),gun,hand,(*frames)[0],(*frames)[1],[this]{return next_game_random(random_);});
        };
    }else h.model_flag_411=[](std::int32_t)->bool{throw std::runtime_error("Combat world: attached weapon models (record +13c) are not hosted");};
    h.actor=actors_.object(actor);h.actor_id=actors_.object_id(actor);
    if(effects_.launch)h.launch_projectile=[this,actor](const ShotRequest& r,const RootVector& point,float arc){
        effects_.launch(ObjectWeakHandle{actors_.object(actor),actors_.object_id(actor)},r,point,arc);};
    if(air_taser_.fire)h.air_taser=[this,actor](std::uint64_t target,bool aim_view,std::int32_t subtarget){
        return air_taser_.fire(actor,target,aim_view,subtarget);};
    h.player=actors_.player(actor);h.ai=!h.player;h.byte_14=actors_.byte_14(actor);
    return h;
}

std::uint64_t CombatWorld::request_bullet(const ShotRequest& r,const BulletObjectShooter& shooter,std::vector<CombatEvent>& events){
    if(actor_of(r.shooter_44))throw std::runtime_error("Combat world: request_bullet is for a shooter that is not an actor");
    if(r.shooter_44)object_shooters_[r.shooter_44]=shooter;
    std::vector<BulletEvent> be;
    const auto handle=pool_.request(r,db_,pool_host(events),be);
    for(auto& e:be){CombatEvent c;c.bullet=e;events.push_back(c);}
    return handle;
}

BulletPoolHost CombatWorld::pool_host(std::vector<CombatEvent>& events){
    BulletPoolHost h;
    h.static_hit=[this](const RootVector& s,const RootVector& d,std::uint64_t x){return actors_.static_hit(s,d,x);};
    h.static_material=[this](const BulletStaticHit& hit){return actors_.static_material(hit);};
    h.object_id=[this](std::uint64_t o){
        if(const auto a=actor_of(o))return actors_.object_id(*a);
        return actors_.static_object_id(o);};
    h.valid=[this](std::uint64_t o,std::int32_t id){
        if(const auto a=actor_of(o))return actors_.alive_handle(*a)&&actors_.object_id(*a)==id;
        return actors_.static_object_id(o)==id;};
    h.candidates=[this]{
        if(list_.candidates)return list_.candidates();
        std::vector<BulletCandidate> list;
        for(std::size_t i=0;i<actors_.size();++i){
            if(!actors_.alive_handle(i))continue;
            const auto b=actors_.bounds_88(i);
            list.push_back({actors_.object(i),b[0],b[1],actors_.position_e4(i)});
        }
        return list;};
    h.eye_c0=[this](std::uint64_t o,std::int32_t sub){
        if(const auto a=actor_of(o))return actors_.eye_c0(*a,sub);
        if(list_.base_object&&list_.base_object(o))return list_.base_eye_c0(o);
        throw std::runtime_error("Combat world: the designated target's +c0 (not an actor or a listed object) is not hosted");};
    h.hit=[this,&events](std::uint64_t o,BulletHitRequest& req)->std::uint8_t{
        const auto v=actor_of(o);
        if(!v&&list_.base_object&&list_.base_object(o))return 0;          // 0015c0a0
        if(!v)throw std::runtime_error("Combat world: a bullet reached a non-actor object (its +104 is not hosted)");
        BulletHitVictim victim{o,actors_.player(*v),actors_.armor_33a0(*v),actors_.invulnerable_33a4(*v),actors_.port_agent_slot(*v)};
        std::optional<BulletHitShooter> shooter;
        std::optional<std::size_t> s;
        const BulletObjectShooter* object_shooter=nullptr;
        if(req.shooter_0){
            s=actor_of(req.shooter_0);
            if(!s){
                // Checkpoint 132uk6: a shooter that is not an actor (the tank): its +14 and no +58 controller.
                const auto it=object_shooters_.find(req.shooter_0);
                if(it==object_shooters_.end())
                    throw std::runtime_error("Combat world: a bullet from a shooter that is neither an actor nor a request_bullet shooter (00188780 with its +14 / +58 and the damage source) is not hosted");
                object_shooter=&it->second;
                BulletHitShooter sh;sh.object=req.shooter_0;sh.owned_14=it->second.owned_14;
                shooter=sh;
            }
        }
        if(s){
            BulletHitShooter sh;sh.object=req.shooter_0;sh.owned_14=actors_.byte_14(*s);
            // 003ea550: the aim block's +28 when its weak handle still resolves.
            const auto& block=aims_.at(*s);
            std::uint64_t aimed=0;
            if(block.target_28){
                const auto t=actor_of(block.target_28);
                if(t&&actors_.alive_handle(*t)&&actors_.object_id(*t)==block.target_id_2c)aimed=block.target_28;
            }
            sh.controller=BulletHitShooter::Controller{actors_.player(*s),aimed,actors_.port_agent_slot(*s)};
            shooter=sh;
        }
        BulletHitHost hh;
        hh.random=[this]{return next_game_random(random_);};
        if(innocent_chance_)hh.innocent_chance=innocent_chance_();
        hh.hit_volumes=[this,v](const RootVector& origin,const RootVector& direction,std::int32_t mode,float volume){
            return actors_.hit_volumes(*v,origin,direction,mode,volume);};
        hh.life_state=[this,v]{return actors_.life_state(*v);};
        hh.damage=[this,v,s,object_shooter,source=req.shooter_0](std::uint8_t category,std::uint8_t type,std::int32_t& penetration,float amount){
            if(object_shooter){actors_.damage_from_object(*v,source,object_shooter->position_e4,category,type,penetration,amount);return;}
            actors_.damage(*v,s,category,type,penetration,amount);};
        std::vector<BulletHitEvent> he;
        const auto category=apply_bullet_hit(victim,shooter,req,hh,he);
        for(auto& e:he){CombatEvent c;c.actor=*v;c.hit=e;events.push_back(c);}
        return category;};
    h.local_player_controller=[this](std::uint64_t o){
        const auto a=actor_of(o);
        return a&&actors_.player(*a)&&actors_.byte_14(*a);};
    h.static_damage=[this](std::uint64_t o,std::uint64_t s,std::uint8_t type,float amount){actors_.static_damage(o,s,type,amount);};
    h.is_actor=[this](std::uint64_t o){return actor_of(o).has_value();};
    h.effect_id=[this](std::int32_t i){return actors_.effect_id(i);};
    h.controller_player=[this](std::uint64_t o){
        if(const auto a=actor_of(o))return actors_.player(*a);
        // A script's SetTarget node or GOB: +58 is null (003cf0e0 at 003cf144..003cf164).
        if(list_.base_object&&list_.base_object(o))return false;
        throw std::runtime_error("Combat world: the +58 controller of a bullet victim that is neither an actor, a node nor a hosted GOB is not hosted");};
    h.area=effects_.area;
    if(effects_.blast)h.blast=[this](const BulletBlastCall& c){
        const BulletObjectShooter* object=nullptr;
        if(c.source.object&&!actor_of(c.source.object)){
            const auto it=object_shooters_.find(c.source.object);
            if(it!=object_shooters_.end())object=&it->second;
        }
        effects_.blast(c,object);};
    return h;
}

// The target's virtual +c8: an actor's stance; a listed node, GOB or hotbox
// (004d95b0 / 004db4c0 / 004db280 / 004db160 / 004db3a0 / 004d9ac0) has
// 00140e90 (`jr ra; move v0, zero`): 0, as a script's SetTarget node.
std::uint8_t CombatWorld::target_stance_c8(std::uint64_t object,const char* what){
    if(!object)throw std::runtime_error(std::string("Combat world: a ")+what+" target stance without a target");
    if(const auto a=actor_of(object))return actor_stance_c8(actors_.flags_3204(*a));
    if(list_.base_object&&list_.base_object(object))return 0;
    throw std::runtime_error(std::string("Combat world: ")+what+" target is neither an actor nor a listed object (its +c8 is not hosted)");
}

// The launched projectile's target point (checkpoint 132uk6): the target's
// virtual +e4, an actor's position.
RootVector CombatWorld::projectile_target_e4(std::uint64_t object){
    if(const auto a=actor_of(object))return actors_.position_e4(*a);
    throw std::runtime_error("Combat world: a launched projectile at a target that is not an actor (its +e4) is not hosted");
}

void CombatWorld::pump(std::vector<CombatEvent>& events){
    while(!queue_.empty()){
        const auto [actor,request]=queue_.front();
        queue_.pop_front();
        const auto state=actors_.fire_state(actor);
        // Stance of the target for the discharge (target virtual +c8).
        std::vector<CombatEvent> inner;
        auto host=discharge_host(actor,inner);
        host.target_stance=[this,o=request.target]{return target_stance_c8(o,"fire");};
        host.projectile_target=[this,o=request.target]{return projectile_target_e4(o);};
        host.start_throw=[this,a=actor,&r=request](std::int32_t cls){actors_.start_throw(a,cls,r);};
        host.melee_attack=[this,a=actor]{actors_.melee_attack(a);};
        std::vector<WeaponDischargeEvent> de;
        // Port-only (checkpoint 132f5): the Infinite Ammo cheat around the player's ammo uses.
        PortAmmoGuard ammo(inventories_.at(actor),actors_.player(actor)&&port_cheat(PortCheat::infinite_ammo));
        const auto result=handle_actor_fire(inventories_.at(actor),db_,request,state,host,de);
        {CombatEvent c;c.actor=actor;c.fire=result;events.push_back(c);}
        for(auto& e:de){CombatEvent c;c.actor=actor;c.discharge=e;events.push_back(c);}
        events.insert(events.end(),inner.begin(),inner.end());
    }
}

bool CombatWorld::discharge(std::size_t actor,const WeaponDischargeInput& input,std::vector<CombatEvent>& events){
    if(actor>=actors_.size())throw std::runtime_error("Combat world: discharge for an unknown actor");
    if(input.target&&!actor_of(input.target)&&!(list_.base_object&&list_.base_object(input.target)))
        throw std::runtime_error("Combat world: discharge target is neither an actor nor a listed object");
    std::vector<CombatEvent> inner;
    auto host=discharge_host(actor,inner);
    host.target_stance=[this,o=input.target]{return target_stance_c8(o,"discharge");};
    host.projectile_target=[this,o=input.target]{return projectile_target_e4(o);};
    std::vector<WeaponDischargeEvent> de;
    PortAmmoGuard ammo(inventories_.at(actor),actors_.player(actor)&&port_cheat(PortCheat::infinite_ammo));   // port-only (132f5)
    const bool fired=discharge_weapon(inventories_.at(actor),db_,input,host,de);
    for(auto& e:de){CombatEvent c;c.actor=actor;c.discharge=e;events.push_back(c);}
    events.insert(events.end(),inner.begin(),inner.end());
    return fired;
}

void CombatWorld::update_inventories(float dt,std::vector<CombatEvent>& events){
    for(std::size_t actor=0;actor<actors_.size();++actor)update_inventory(actor,dt,events);
}

void CombatWorld::update_inventory(std::size_t actor,float dt,std::vector<CombatEvent>& events){
    {
        auto& inv=inventories_.at(actor);
        std::vector<CombatEvent> inner;
        auto host=discharge_host(actor,inner);
        host.target_stance=[this,o=inv.burst_target_90]{return target_stance_c8(o,"burst");};
        host.projectile_target=[this,o=inv.burst_target_90]{return projectile_target_e4(o);};
        WeaponUpdateHost uh;
        uh.valid=[this](std::uint64_t o,std::int32_t id){
            if(const auto a=actor_of(o))return actors_.alive_handle(*a)&&actors_.object_id(*a)==id;
            if(list_.base_object&&list_.base_object(o))return actors_.static_object_id(o)==id;
            return false;};
        // 00144080 reads the float at 005383d8 only past the +38 test (+38 != 0x80).
        uh.actor_word_38=actors_.word_38(actor);
        uh.value_005383d8=uh.actor_word_38!=0x80?actors_.value_005383d8():0.0F;
        uh.life_state=actors_.life_state(actor);
        uh.torch=actors_.torch_host(actor);
        if(air_taser_.update)uh.air_taser=[this,actor](float step){air_taser_.update(actor,step);};
        std::vector<WeaponDischargeEvent> de;
        PortAmmoGuard ammo(inv,actors_.player(actor)&&port_cheat(PortCheat::infinite_ammo));   // port-only (132f5)
        update_weapon_inventory(inv,db_,dt,uh,host,de);
        for(auto& e:de){CombatEvent c;c.actor=actor;c.discharge=e;events.push_back(c);}
        events.insert(events.end(),inner.begin(),inner.end());
    }
}

void CombatWorld::update_bullets(float dt,std::vector<CombatEvent>& events){
    std::vector<BulletEvent> be;
    std::vector<CombatEvent> inner;
    struct Updating{const std::vector<BulletEvent>*& p;~Updating(){p=nullptr;}} updating{updating_bullets_};
    updating_bullets_=&be;
    pool_.update(dt,db_,pool_host(inner),be);
    last_bullet_hits_=inner.size();
    // Hit-entry events happen inside the update; bullet events are listed after them.
    events.insert(events.end(),inner.begin(),inner.end());
    for(auto& e:be){CombatEvent c;c.actor=0;c.bullet=e;events.push_back(c);}
}

}
