#include "blast_world.hpp"
#include <stdexcept>
#include <utility>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Blast world: "+what);}
}

ActorWorldBlastObjects::ActorWorldBlastObjects(ActorWorld& world,BlastWorldConfig config):world_(world),config_(std::move(config)){
    if(config_.object_ids.size()!=world_.size())fail("one object ID per actor");
}

std::optional<std::size_t> ActorWorldBlastObjects::actor_index(std::uint64_t object) const{
    for(std::size_t a=0;a<world_.size();++a)if(world_.actor(a).handle()==object)return a;
    return std::nullopt;
}

std::vector<BlastObject> ActorWorldBlastObjects::registry(){
    std::vector<BlastObject> out;
    for(std::size_t a=0;a<world_.size();++a){
        auto& s=world_.actor(a);
        BlastObject o;
        o.object=s.handle();o.id_c=config_.object_ids[a];o.human_4c=true;   // +4c: the actor class word
        const auto root=s.root_bytes();
        o.byte_2c=root[0]!=0;o.byte_2d=root[1]!=0;o.byte_2e=root[2]!=0;
        o.byte_14=s.byte_14()!=0;
        o.controller_player_54=s.player_controller();                       // +58 virtual +54 (00175330 / 0012f370)
        o.health_40=s.health().health_40;
        o.position_e4=s.published_position();                              // virtual +e4 (00132860)
        out.push_back(o);
    }
    for(const auto& e:config_.others){
        if(!e.known)fail("the object registry 00571748 holds "+e.what+", whose health +40 and damage class are not hosted");
        out.push_back(e.entry);
    }
    const auto& h=config_.host_objects;
    if(!h.objects.empty()&&(!h.health_40||!h.present_2d||!h.position_e4))fail("host objects without their +40 / +2d / +e4 services");
    for(std::size_t i=0;i<h.objects.size();++i){
        BlastObject o;
        o.object=h.objects[i].object;o.id_c=h.objects[i].id_c;
        o.byte_2c=h.enabled_2c?h.enabled_2c(i):true;o.byte_2d=h.present_2d(i);o.byte_2e=false;   // 003cf120..003cf130; +2d the activation
        o.byte_14=true;                                              // built locally
        o.health_40=h.health_40(i);
        if(o.byte_2d&&o.health_40>=1.0F)o.position_e4=h.position_e4(i);   // read only past the +40 test
        out.push_back(o);
    }
    return out;
}

std::optional<std::size_t> ActorWorldBlastObjects::host_object_index(std::uint64_t object) const{
    const auto& v=config_.host_objects.objects;
    for(std::size_t i=0;i<v.size();++i)if(v[i].object==object)return i;
    return std::nullopt;
}

BlastSource ActorWorldBlastObjects::host_source(std::size_t index) const{
    const auto& o=config_.host_objects.objects[index];
    return BlastSource{{o.object,o.id_c},true,config_.host_objects.position_e4(index)};
}

bool ActorWorldBlastObjects::valid(const ObjectWeakHandle& h){
    if(h.object==0)return false;
    if(const auto a=actor_index(h.object))return config_.object_ids[*a]==h.id;
    for(const auto& entry:script_objects_)if(entry.second.handle==h)return true;   // host objects are not destroyed natively
    for(const auto& e:config_.others)if(e.entry.object==h.object)return e.entry.id_c==h.id;
    if(const auto i=host_object_index(h.object))return config_.host_objects.objects[*i].id_c==h.id;   // never destroyed natively
    return false;
}

void ActorWorldBlastObjects::damage(const ExplosionDamageCall& call){
    if(const auto i=host_object_index(call.victim)){
        auto& h=config_.host_objects;
        if(!h.set_health_40)fail("host objects without the +40 setter");
        float health=h.health_40(*i);
        const auto outcome=object_base_damage(health,call.source,call.category,static_cast<std::uint8_t>(call.type),call.amount);
        h.set_health_40(*i,health);
        object_damage_.push_back({*i,call,health,outcome});
        return;
    }
    const auto victim=actor_index(call.victim);
    if(!victim)fail("blast damage on an object that is neither an actor nor a host object (its +100 is not hosted)");
    auto& s=world_.actor(*victim);
    const ActorDamageController self=s.player_controller()?ActorDamageController{true,false,false}:ActorDamageController{false,true,false};
    std::optional<ActorDamageController> source_controller;
    std::optional<RootVector> source_position;
    if(call.source!=0){
        if(const auto a=actor_index(call.source)){
            const auto& src=world_.actor(*a);
            source_controller=src.player_controller()?ActorDamageController{true,false,false}:ActorDamageController{false,true,false};
            source_position=src.published_position();
        }else{
            bool found=false;
            for(const auto& entry:script_objects_)if(entry.second.handle.object==call.source){source_position=entry.second.position_e4;found=true;}
            if(!found)if(const auto i=host_object_index(call.source)){source_position=host_source(*i).position_e4;found=true;}
            if(!found)fail("a blast source that is neither an actor nor a script object");
        }
    }
    const auto in=blast_actor_damage_input(call,s.handle(),self,s.byte_14()!=0,source_controller);
    SoloDamageRequest r;
    r.source=in.source.id;r.source_controller=in.source.controller;r.category=in.category;r.type=in.type;
    r.penetration=in.penetration;r.quiet=in.quiet;r.hit_point=in.hit_point;r.amount=in.amount;r.source_position=source_position;
    damage_.push_back({*victim,call,s.apply_damage(r)});
}

BlastSource ActorWorldBlastObjects::source(std::uint64_t object){
    if(object==0)fail("003cc820 of a null object");
    if(const auto a=actor_index(object)){
        auto& s=world_.actor(*a);
        return BlastSource{{object,config_.object_ids[*a]},s.byte_14()!=0,s.published_position()};
    }
    for(const auto& entry:script_objects_)if(entry.second.handle.object==object)return entry.second;
    if(const auto i=host_object_index(object))return host_source(*i);
    fail("003cc820 of an object the world does not know");
}

std::uint64_t ActorWorldBlastObjects::script_object(std::uint32_t record){
    if(record==0)return 0;
    if(config_.actor_of_record)
        if(const auto a=config_.actor_of_record(record))return world_.actor(*a).handle();
    for(const auto& entry:script_objects_)if(entry.first==record)return entry.second.handle.object;
    for(const auto& o:config_.host_objects.objects)if(static_cast<std::uint32_t>(o.id_c)==record)return o.object;
    if(!config_.script_source)fail("the script object "+std::to_string(record)+" (003cc820) is not hosted");
    const auto src=config_.script_source(record);
    if(!src)fail("the script object "+std::to_string(record)+" (003cc820) is not hosted");
    if(src->handle.object==0||actor_index(src->handle.object))fail("a script source must be a distinct non-actor object");
    script_objects_.push_back({record,*src});
    return src->handle.object;
}

std::uint64_t ActorWorldBlastObjects::set_host_source(std::uint32_t record,const BlastSource& source){
    if(source.handle.object==0||actor_index(source.handle.object))fail("a host source must be a distinct non-actor object");
    for(auto& entry:script_objects_)if(entry.first==record){
        if(!(entry.second.handle==source.handle))fail("the host source "+std::to_string(record)+" changed its handle");
        entry.second=source;return source.handle.object;
    }
    for(const auto& entry:script_objects_)if(entry.second.handle.object==source.handle.object)fail("two host sources share one handle");
    script_objects_.push_back({record,source});
    return source.handle.object;
}

void ActorWorldBlastObjects::object_damage(std::uint64_t object,std::uint64_t source,std::uint8_t category,std::uint8_t type,float amount){
    const auto i=host_object_index(object);
    if(!i)fail("003ccb00 on an object that is not a host object");
    ExplosionDamageCall call;
    call.victim=object;call.source=source;call.category=category;call.type=static_cast<std::int8_t>(type);call.amount=amount;
    auto& h=config_.host_objects;
    if(!h.health_40||!h.set_health_40)fail("host objects without their +40 services");
    float health=h.health_40(*i);
    const auto outcome=object_base_damage(health,source,category,type,amount);
    h.set_health_40(*i,health);
    object_damage_.push_back({*i,call,health,outcome});
}

std::vector<BlastDamageRecord> ActorWorldBlastObjects::take_damage(){auto o=std::move(damage_);damage_.clear();return o;}
std::vector<BlastObjectDamageRecord> ActorWorldBlastObjects::take_object_damage(){
    auto o=std::move(object_damage_);object_damage_.clear();return o;}
void ActorWorldBlastObjects::append_actor(std::int32_t object_id){
    if(world_.size()!=config_.object_ids.size()+1)fail("append one object ID right after the world appends its actor");
    config_.object_ids.push_back(object_id);
}

}
