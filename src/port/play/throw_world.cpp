#include "throw_world.hpp"
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Throw world: "+what);}
}

InventoryLaunchInput throw_launch_input(const ActorAnimationReceiverState& a,float arc,const ThrowLaunchActor& l,
    const std::function<std::optional<ThrowLaunchTarget>(std::uint32_t)>& find){
    if(!find)fail("the named-object lookup (0040c540) is not hosted");
    InventoryLaunchInput in;
    in.hand_frame=l.hand_frame;in.actor_matrix=l.actor_matrix;
    if(const auto t=find(a.target_33cc)){in.target_position=t->position_e4;in.target_object=t->object;}
    in.point=a.aim_33d0;in.arc=arc;in.ai_controller=l.ai_controller;in.owner=l.owner;
    return in;
}

BlastRequest projectile_blast_request(const ProjectileBlastCall& c,std::optional<BlastSource> owner){
    if(c.owner.object!=0&&(!owner||!(owner->handle==c.owner)))fail("the blast source does not match the projectile's owner");
    if(c.owner.object==0&&owner)fail("a null blast source with source fields");
    BlastRequest r;
    r.source=owner;r.damage=c.damage;r.kind=true;r.point=c.point;r.skip_humans=false;
    r.strength=c.strength;r.inner=c.inner;r.outer=c.outer;
    return r;
}

ThrowWorld::ThrowWorld(const WeaponDatabase& database,AreaWorld& areas,ThrowWorldHost host,ThrowWorldConfig config)
    :db_(database),area_world_(areas),host_(std::move(host)),config_(config),next_object_(config.first_object),next_id_(config.first_id){
    if(next_object_==0)fail("the first object identity must not be the null object");
}

InventoryLaunchResult ThrowWorld::launch(WeaponInventory& inv,InventoryLaunchInput in){
    if(inv.current_84>=inv.slots.size())fail("00141e20 without a held slot");
    const auto item=inv.slots[inv.current_84].item;
    in.object=next_object_++;in.id_c=next_id_++;
    auto r=launch_inventory_projectile(inv,db_,in,config_.gravity[2],config_.tuning);
    projectiles_.push_back({r.projectile,item});
    return r;
}

ThrowWorldFrame ThrowWorld::update(float dt){
    ThrowWorldFrame out;
    std::vector<AreaEffect> created;
    ThrownProjectileHost ph;
    ph.los=host_.los;ph.scan=host_.scan;ph.valid=host_.valid;ph.static_damage=host_.static_damage;ph.effect_id=host_.effect_id;
    ph.explode=host_.explode;
    ph.area=[&](const ProjectileAreaCall& c){
        std::vector<AreaEvent> ev;
        const auto object=next_object_++;
        auto a=construct_area_effect(object,c.owner,c.point,db_.record(c.record),c.record,c.flag,area_world_,ev);
        for(auto& e:ev)out.area_events.emplace_back(object,std::move(e));
        out.areas_created.push_back(object);
        created.push_back(a);
    };
    const auto live=projectiles_.size();
    for(std::size_t k=0;k<live;++k){
        auto& item=projectiles_[k];
        const auto& record=db_.record(item.projectile.record_120);
        if(record.hold_class_f0()==7&&!config_.scan_aim_view_byte)fail("a hold-7 projectile needs the scan request's +d byte");
        auto step=update_thrown_projectile(item.projectile,dt,config_.gravity,record,ph,config_.scan_aim_view_byte.value_or(false));
        for(auto& e:step.events)out.projectile_events.emplace_back(item.projectile.object,std::move(e));
        if(step.detonated)out.detonated.push_back(item.projectile.object);
    }
    std::erase_if(projectiles_,[](const ThrownItem& t){return t.projectile.motion.retired;});
    const auto areas_live=areas_.size();
    for(std::size_t k=0;k<areas_live;++k){
        auto& a=areas_[k];
        std::vector<AreaEvent> ev;
        update_area_effect(a,dt,db_.record(a.record_80),area_world_,ev);
        for(auto& e:ev)out.area_events.emplace_back(a.object,std::move(e));
        if(a.done_d4)out.areas_done.push_back(a.object);
    }
    std::erase_if(areas_,[](const AreaEffect& a){return a.done_d4;});
    areas_.insert(areas_.end(),created.begin(),created.end());
    return out;
}

ThrowWorldFrame ThrowWorld::finish_areas(){
    ThrowWorldFrame out;
    for(auto& a:areas_){                                                    // 0025a148..0025a1a0 over 004f8040
        std::vector<AreaEvent> ev;
        if(!finish_area_effect(a,ev))continue;
        for(auto& e:ev)out.area_events.emplace_back(a.object,std::move(e));
        out.areas_done.push_back(a.object);
    }
    std::erase_if(areas_,[](const AreaEffect& a){return a.done_d4;});
    return out;
}

std::uint64_t ThrowWorld::add_area(const ObjectWeakHandle& owner,const RootVector& point,std::int32_t record,bool flag,ThrowWorldFrame& out){
    std::vector<AreaEvent> ev;
    const auto object=next_object_++;
    auto a=construct_area_effect(object,owner,point,db_.record(record),record,flag,area_world_,ev);
    for(auto& e:ev)out.area_events.emplace_back(object,std::move(e));
    out.areas_created.push_back(object);
    areas_.push_back(a);
    return object;
}

std::uint64_t ThrowWorld::launch_discharge(const ObjectWeakHandle& owner,const std::array<RootVector,4>& frame,std::int32_t item,
    const RootVector& point,float arc){
    const auto& record=db_.record(item);
    const auto object=next_object_++;
    auto p=construct_thrown_projectile(object,next_id_++,owner,frame,record,item,config_.tuning);
    launch_thrown_projectile(p,record,point,arc,config_.gravity[2],config_.tuning);
    projectiles_.push_back({p,item});
    return object;
}

}
