#include "combat_throw_play.hpp"
#include "actor_fire.hpp"
#include "air_taser.hpp"
#include "camera_rig.hpp"
#include "game_sound_voice.hpp"
#include "port_cheats.hpp"
#include "upper_body_pass.hpp"
#include "water_effects.hpp"
#include "world_los.hpp"
#include <bit>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Combat throws: "+what);}
// 00141e88: lb a2, 0049d02c (byte 12 of the node table 0049d020 = node 20).
constexpr int hand_node=20;
std::array<RootVector,4> rows_of(const std::array<float,16>& m){
    std::array<RootVector,4> f{};
    for(std::size_t r=0;r<4;++r)f[r]={m[4*r],m[4*r+1],m[4*r+2],m[4*r+3]};
    return f;
}
// The executable's inventory tables 00489d70 / 00489d78 (model slots by
// inventory mode, read by the receiver's selection and hiding).
constexpr std::array<std::uint8_t,6> slots_00489d70{3,4,2,5,8,9};
constexpr std::array<std::uint8_t,6> slots_00489d78{0,1,0,0,0,0};
std::array<std::optional<RootVector>,upper_body_entry_count> zero_composed(){
    std::array<std::optional<RootVector>,upper_body_entry_count> z;z.fill(RootVector{});return z;
}
}

SecondaryMotionRegistry make_secondary_motion_registry(const AnimationGraph& graph,const AnimationClipBank& clips){
    const AnimationGraph* g=&graph;
    const AnimationClipBank* bank=&clips;
    SecondaryMotionRegistry r;
    r.find=[g](std::string_view name)->std::optional<std::uint32_t>{
        std::optional<std::uint32_t> found;
        const auto& nodes=g->nodes();
        for(std::size_t i=0;i<nodes.size();++i){
            if(nodes[i].name!=name)continue;
            if(found)fail("the motion name \""+std::string(name)+"\" names two graph nodes");
            found=static_cast<std::uint32_t>(i);
        }
        if(!found)fail("the upper-body motion \""+std::string(name)+"\" is not a node of the character's graph "
            "(0018a670 would answer registry ID 0; every name the playback installs must resolve)");
        return found;
    };
    r.entry=[g,bank](std::uint32_t id)->std::optional<SecondaryMotionEntry>{
        const auto& nodes=g->nodes();
        if(id>=nodes.size())return std::nullopt;
        const auto& node=nodes[id];
        SecondaryMotionEntry e;
        e.type_4=static_cast<std::uint8_t>(node.kind);
        if(node.kind==AnimationGraphKind::Leaf){
            const auto at=bank->find(node.leaf.clip_key);
            if(at==bank->end())fail("the clip \""+node.leaf.clip_key+"\" of motion node "+std::to_string(id)+" is not in the clip bank");
            const auto& c=at->second;
            e.clip_24=SecondaryClipTiming{c.version,static_cast<std::int32_t>(c.frame_intervals),static_cast<std::int32_t>(c.stride_words),
                c.frame_period,c.looping,id};
        }
        return e;
    };
    // The clip's payload from +70 in file order (decode_animation_clip keeps
    // every word: bit_cast floats and the raw root-yaw quadword).
    auto cache=std::make_shared<std::map<std::uint32_t,std::shared_ptr<const SecondaryClipKeys>>>();
    r.keys=[g,bank,cache](std::uint32_t id)->std::shared_ptr<const SecondaryClipKeys>{
        if(const auto at=cache->find(id);at!=cache->end())return at->second;
        const auto& nodes=g->nodes();
        if(id>=nodes.size()||nodes[id].kind!=AnimationGraphKind::Leaf)return nullptr;
        const auto at=bank->find(nodes[id].leaf.clip_key);
        if(at==bank->end())return nullptr;
        const AnimationClip& c=at->second;
        auto k=std::make_shared<SecondaryClipKeys>();
        const auto word=[&](std::size_t o){return static_cast<std::uint32_t>(c.header_bytes[o])|static_cast<std::uint32_t>(c.header_bytes[o+1])<<8|
            static_cast<std::uint32_t>(c.header_bytes[o+2])<<16|static_cast<std::uint32_t>(c.header_bytes[o+3])<<24;};
        k->root_translation_14=word(20);k->root_yaw_18=word(24);k->translation_mask_1c=word(28);k->rotation_mask_20=word(32);
        const auto put=[&](const std::array<float,4>& v){for(const float f:v)k->words.push_back(std::bit_cast<std::uint32_t>(f));};
        for(const ClipFrame& f:c.frames)for(std::size_t b=0;b<f.bones.size();++b){
            if(f.bones[b].translation)put(*f.bones[b].translation);
            if(f.bones[b].rotation){
                if(b==0&&f.root_yaw_words)for(const std::uint32_t w:*f.root_yaw_words)k->words.push_back(w);
                put(*f.bones[b].rotation);
            }
        }
        if(k->words.size()!=static_cast<std::size_t>(c.stride_words)*c.frames.size())fail("the key words of clip \""+at->first+"\" do not match its stride");
        cache->emplace(id,k);
        return k;
    };
    return r;
}

std::optional<ProjectileLosHit> projectile_world_los(const SoloCollisionWorld& world,const RootVector& origin,const RootVector& displacement){
    const auto hit=trace_world_los(world,WorldLosQuery{origin,displacement,0,0x10});
    if(!hit)return std::nullopt;
    // The result's object (0025b19c: *(result +84)) is the hit COL source's owner, the object whose +b8
    // gave the geometry (a GOB for a population model's COL), as the bullet's 00278060.
    const auto owner=world.game_object(hit->source.geometry);
    return ProjectileLosHit{hit->point,hit->normal,owner?owner:hit->source.object};
}

struct CombatThrows::Impl final:AreaWorld {
    CombatThrowSession s;
    CombatThrowConfig c;
    std::vector<SecondaryPlayback> playbacks;
    // Node +1c0 of the first 21 nodes (the composer's output; nullopt: unknown,
    // after a frame the pass could not report). Zero from the node constructor 003bac20.
    using Composed=std::array<std::optional<RootVector>,upper_body_entry_count>;
    std::vector<Composed> composed;
    // The look record +410 (001d81a0 / 001d8610's values; +5 / +10 live in
    // ActorAimState); known false after a frame whose pass ran unreported.
    struct Look {UpperBodyLookRecord record=upper_body_head_look_record();bool known=true;};
    std::vector<Look> looks;
    std::vector<ActorSenses> senses;
    std::vector<std::optional<TakedownCamera>> cameras;      // the last view given to G +d4 by each actor's takedown
    std::vector<ActorStunState> stuns;                       // actor +33bc / +33c0 / +33c8
    ActorAnimationInventoryBindings bindings;   // the receivers' slot tables and record +13c definitions
    std::vector<ActorEffectSlotState> effects;               // actor +354c..+3570 / +34e0 (00185d40)
    std::vector<ActorIgnitionState> ignitions;               // actor +31f4 / +3480..+3488 (00186360; +326a is the health's)
    // The effect / light objects 00185d40 creates (00236810 / 002624a0):
    // their keys (+c), the registration (00571748) / activity (+2d) /
    // release made on them, the actor (+74 / +90) and the object itself.
    struct EffectObject {
        std::uint32_t key{};bool registered{},active{},released{};
        std::size_t actor{};
        std::optional<FireEffectObject> effect;
        std::optional<FireZone> zone;
        bool level{};                                        // a FIR: zone of the level (001c0b00), not an actor's
    };
    std::map<std::uint32_t,EffectObject> effect_objects;
    std::uint32_t next_effect_object{0x00e00001U};
    // 00571748's vector +4 in registration order (0040b790 appends) and its
    // pending removals (0040b720 pushes the key; 0040b4b0 erases them after
    // the traversal 00133420).
    std::vector<std::uint32_t> registration,removals;
    std::map<std::uint32_t,CombatThrows::FireEmitter> emitters;   // the particle emitters (presentation)
    std::uint32_t next_emitter{1};
    std::vector<std::int16_t> glow;                          // actor +2df6 (model +2d96)
    // Checkpoint 132fx3: the render model's attach points (actor +60 +2410) are CombatWorld's attach table, which
    // the inventory, the draw, the grip and the muzzle read; the receiver's tokens act on it (receiver_host).
    std::unique_ptr<ThrowWorld> world;
    CombatThrowFrame frame;

    Impl(CombatThrowSession session,CombatThrowConfig config):s(std::move(session)),c(std::move(config)){}

    ActorWorld& aw() const{return *s.world;}
    void hook(std::size_t a){
        auto& actor=aw().actor(a);
        actor.set_logic_item_hooks([this,a](float){item_triggers(a);},
            [this,a](float dt){
                auto& actor=aw().actor(a);
                decay_actor_senses(senses.at(a),dt);                                     // 001b2328..001b237c
                actor.set_fire_damage_33b0(decay_fire_damage(actor.health().fire_damage_33b0,dt));   // 001b2380..001b23c4
                update_actor_stun(stuns.at(a),dt,stun_host(a));                         // 001b23c8..001b255c
            });
        SoloFireHooks fire;
        actor.set_logic_grip_hook([this,a](float dt){
            if(!aw().actor(a).eligible())return;                                       // 001b1c2c: +3314
            std::optional<UpperBodyGunGrip> gun;
            if(const auto handle=s.combat->attach_table(a).at(0);handle!=0){             // 00385230
                const auto& m=s.combat->weapon_models()->model(handle);
                const auto v=[&m](std::size_t o)->RootVector{return {m.real(o),m.real(o+4),m.real(o+8),m.real(o+12)};};
                gun=UpperBodyGunGrip{v(0x170),v(0x180),v(0x190)};
            }
            auto& c=s.actors->secondary(a);
            upper_body_grip_step(c.grip_rotation_480,c.grip_offset_490,gun,dt);
        });
        fire.update=[this,a](float dt){update_burning_actor(ignitions.at(a).f31f4,dt,burning_host(a));};
        fire.ignite=[this,a]{ignite(a,1,0,0,0,-1.0F);};
        fire.extinguish=[this,a]{ignite(a,0,0,0,0,-1.0F);};
        fire.fire_setter=[this,a]()->std::optional<SoloFireSetter>{
            const auto setter=fire_setter(a);
            if(!setter)return std::nullopt;
            return SoloFireSetter{{aw().actor(*setter).handle(),s.actors->damage_controller(*setter)},aw().actor(*setter).published_position()};
        };
        actor.set_fire_hooks(std::move(fire));
        actor.set_inventory_receiver(bindings,receiver_host(a));
    }
    // The weak handle +3480 / +3484 of actor a when it resolves (only actors
    // set it: the melee hit's ignition; the damage path's passes 0).
    std::optional<std::size_t> fire_setter(std::size_t a) const{
        const auto& st=ignitions.at(a);
        if(!st.source_3480)return std::nullopt;
        const auto v=actor_of(st.source_3480);
        if(!v||static_cast<std::uint32_t>(s.object_ids.at(*v))!=st.source_key_3484)return std::nullopt;
        return v;
    }
    // A request of virtual +100 by `source` (0: none) with its controller and +e4.
    SoloDamageRequest damage_request(std::uint64_t source,std::uint8_t category,std::uint8_t type,float amount){
        SoloDamageRequest r;
        r.source=source;r.category=category;r.type=type;r.amount=amount;        // quiet 0, hit point null, penetration null
        if(const auto v=actor_of(source)){
            r.source_controller=s.actors->damage_controller(*v);
            r.source_position=aw().actor(*v).published_position();
        }
        return r;
    }
    EffectObject& object(std::uint32_t o){
        const auto at=effect_objects.find(o);
        if(at==effect_objects.end())fail("object "+std::to_string(o)+" is not an effect object");
        return at->second;
    }

    // The update 001b1e10..001b2214 of actor a (actor_burning.hpp).
    BurningActorHost burning_host(std::size_t a){
        auto& actor=aw().actor(a);
        BurningActorHost h;
        h.actor=actor.handle();
        h.slot_intensity=[this,a](std::uint32_t k)->std::optional<float>{
            const auto& slot=effects.at(a).slots.at(k);
            if(!actor_effect_slot_valid(slot,effect_host(a)))return std::nullopt;
            auto& o=object(slot.object_354c);
            if(!o.effect)fail("an effect slot holds an object that is not an effect object (00235f60's +80)");
            return o.effect->intensity_80;
        };
        h.extinguish=[this,a]{ignite(a,0,0,0,0,-1.0F);};
        h.effect_off=[this,a](std::uint32_t k){set_effect(a,k,0,1.0F);};
        h.light_34e0=[this,a]{return effects.at(a).light_34e0!=0;};
        h.light_strength=[this,a](float f12){
            auto& o=object(effects.at(a).light_34e0);
            if(!o.zone)fail("+34e0 is not a fire zone (00262470)");
            set_fire_zone_strength(*o.zone,f12);
        };
        h.byte_14=[this,a]{return aw().actor(a).byte_14()!=0;};
        h.burning_326a=[this,a]{return aw().actor(a).health().burning_326a;};
        h.life_108=[this,a]{return aw().actor(a).life_state();};
        h.fire_setter_3480=[this,a]()->std::uint64_t{const auto v=fire_setter(a);return v?aw().actor(*v).handle():0;};
        h.damage=[this,a](std::uint64_t source,std::uint8_t category,std::uint8_t type,float amount){
            auto r=damage_request(source,category,type,amount);
            r.own_update=true;                                                     // inside this actor's update
            frame.fire_damage.push_back({a,r.source,r.category,r.type,r.amount});
            s.actors->damage_request(a,r);
        };
        h.controller_player=[this,a]{return aw().actor(a).player_controller();};
        h.controller_80=[this,a](float value){frame.heat.push_back({a,aw().actor(a).handle(),value,true});};
        h.glow_up=[this,a]{auto& g=glow.at(a);g=static_cast<std::int16_t>(g+1);};
        h.glow_down=[this,a]{auto& g=glow.at(a);g=model_glow_down(g);};
        h.health_40=[this,a]{return aw().actor(a).health().health_40;};
        return h;
    }

    // The effect object's update 00235f60 (actor +74).
    FireEffectHost fire_effect_host(std::uint32_t key,std::size_t a){
        FireEffectHost h;
        h.node_position=[this,a](std::int8_t node){return aw().actor(a).skeleton_node_world_position(node);};
        // 002360b0..002360c8: +3204 bit 5, else +346d (the water support flag 001e5ea8).
        h.fast_decay=[this,a]{
            const auto& actor=aw().actor(a);
            return ((actor.state_words().current_3204>>5)&1U)!=0||actor.in_water_346d();
        };
        h.life_108=[this,a]{return aw().actor(a).life_state();};
        h.create_emitter=[this,a,key](std::string_view name,const FireMatrix& m,float weight){
            const auto e=next_emitter++;
            emitters[e]=CombatThrows::FireEmitter{a,key,std::string(name),m,weight};
            if(c.particles.create)emitters[e].particle=c.particles.create(name,m,weight);   // 132fx3: 003a2dd0
            frame.emitter_events.push_back({a,"create",e,std::string(name)});
            return e;
        };
        const auto live=[this](std::uint32_t e)->CombatThrows::FireEmitter&{
            const auto at=emitters.find(e);
            if(at==emitters.end())fail("00235f60 on an emitter that does not exist");
            return at->second;
        };
        h.destroy_emitter=[this,a,live](std::uint32_t e){
            frame.emitter_events.push_back({a,"destroy",e,live(e).name});
            if(live(e).particle&&c.particles.destroy)c.particles.destroy(live(e).particle);   // 132fx3: 003a2ba0
            emitters.erase(e);
        };
        h.fade_emitter=[this,a,live](std::uint32_t e){
            live(e).faded=true;frame.emitter_events.push_back({a,"fade",e,live(e).name});
            if(live(e).particle&&c.particles.fade)c.particles.fade(live(e).particle);};
        // 00235f60 always passes &vel (zero without the record's +19) to 003a2400.
        h.move_emitter=[this,live](std::uint32_t e,float dt,const FireMatrix& m,const RootVector& v){
            auto& em=live(e);em.matrix=m;em.velocity=v;
            if(em.particle&&c.particles.update)c.particles.update(em.particle,m,&em.velocity,dt);};
        h.emitter_strength=[this,live](std::uint32_t e,float f12){
            live(e).strength=f12;if(live(e).particle&&c.particles.strength)c.particles.strength(live(e).particle,f12);};
        h.emitter_floor=[this,live](std::uint32_t e,float f){
            live(e).floor_5c=f;if(live(e).particle&&c.particles.floor)c.particles.floor(live(e).particle,f);};
        return h;
    }
    // The fire zone's update 00261df0 over the registered targets 004f78dc.
    FireZoneHost fire_zone_host(std::uint32_t key){
        FireZoneHost h;
        h.owner=[this,key]()->std::optional<std::pair<bool,RootVector>>{
            const auto& z=*object(key).zone;
            if(!resolves({z.owner_90,static_cast<std::int32_t>(z.owner_key_94)}))return std::nullopt;
            auto& owner=aw().actor(index(z.owner_90));
            const auto r=owner.root_bytes();
            return std::pair{r[1]!=0&&r[0]!=0&&r[2]==0,owner.published_position()};
        };
        h.entries=[this]{
            std::vector<std::pair<std::uint64_t,std::uint32_t>> out;
            for(const auto& t:*s.registered)out.emplace_back(t.object,static_cast<std::uint32_t>(t.id));
            return out;
        };
        h.valid=[this](std::uint64_t o,std::uint32_t id){return resolves({o,static_cast<std::int32_t>(id)});};
        h.target=[this](std::uint64_t o){
            auto& actor=aw().actor(index(o));
            const auto r=actor.root_bytes();
            FireZoneTarget t;
            t.active=r[1]!=0&&r[0]!=0&&r[2]==0;
            t.byte_14=actor.byte_14()!=0;
            t.human_4c=true;                                                       // actors are of the human class
            t.life_108=actor.life_state();
            t.byte_33a5=actor.health().killed_33a5;
            t.position_e4=actor.published_position();
            t.controller_58=true;
            t.controller_player_54=actor.player_controller();
            return t;
        };
        // virtual +100(object, zone, 3, 4, 0, 0, 0, amount): the zone has no
        // controller (+58 0 from 003cf0e0) and its +e4 (003cd260, +50 0) is (0, 0, 0, 1).
        h.damage=[this,key](std::uint64_t o,float amount){
            SoloDamageRequest r;
            r.source=key;r.category=3;r.type=4;r.amount=amount;r.source_position=RootVector{0,0,0,1};
            frame.fire_damage.push_back({index(o),r.source,r.category,r.type,r.amount});
            s.actors->damage_request(index(o),r);
        };
        h.heat=[this,key](std::uint64_t o,float value){frame.heat.push_back({index(o),key,value});};
        return h;
    }
    // The world-object part of the traversal 00133420 (001336b8..0013375c)
    // over the effect objects: each registered object's virtual +4c (003ce750:
    // nothing, +58 is null from 003cf0e0), then +48 when +2d && +2c && !+2e
    // (+2c 1 and +2e 0 from 003cf0e0); objects registered during the pass run
    // in it; then the removals (0040c5c0 -> 0040b4b0).
    void update_objects(float dt){
        for(std::size_t i=0;i<registration.size();++i){
            const auto key=registration[i];
            auto& o=object(key);
            if(!o.active)continue;
            if(o.effect)update_fire_effect_object(*o.effect,dt,fire_effect_host(key,o.actor));
            else if(o.zone)update_fire_zone(*o.zone,dt,fire_zone_host(key));
            else fail("a registered object without an update");
        }
        for(const auto key:removals)std::erase(registration,key);
        removals.clear();
    }
    std::optional<std::size_t> actor_of(std::uint64_t object) const{
        for(std::size_t a=0;a<aw().size();++a)if(aw().actor(a).handle()==object)return a;
        return std::nullopt;
    }
    std::size_t index(std::uint64_t object) const{
        if(const auto a=actor_of(object))return *a;
        fail("object "+std::to_string(object)+" is not an actor of the world (its class is not hosted)");
    }
    // ActorWorld never frees an actor: a handle resolves while its +c matches.
    bool resolves(const ObjectWeakHandle& h) const{
        const auto a=actor_of(h.object);
        return a&&s.object_ids.at(*a)==h.id;
    }

    // AreaWorld over the actors (00259880's reads and calls).
    std::vector<ObjectWeakHandle> objects() override{
        std::vector<ObjectWeakHandle> out;
        for(const auto& t:*s.registered)out.push_back({t.object,t.id});
        return out;
    }
    bool valid(const ObjectWeakHandle& h) override{return resolves(h);}
    // The fields of the admitted area types (3; 2 / 8 never reach the
    // per-type part; 4, the fire patch of a
    // hold-8/9 bullet, checkpoint 132uk4): +108, the controller's +54,
    // +14, the class word (actors are human), +e4, +f4 (the 'flor' placement),
    // +3470 (001b31b0, actor_goggles.hpp; 00259d54 tests == 1), +44, and the
    // fire fields +33b8 / +33a4 / +326a (the health's). The gas fields: +33a5 (the
    // health's) and +346b (SetNotGasable, CombatThrowConfig::not_gasable_346b;
    // Checkpoint 132fx1: the lethal and knockout gases 5 / 7).
    AreaTarget target(std::uint64_t object) override{
        const auto a=index(object);
        auto& actor=aw().actor(a);
        AreaTarget t;
        t.life_108=actor.life_state();
        t.controller_player_54=actor.player_controller();
        t.byte_14=actor.byte_14()!=0;
        t.human_4c=true;
        t.position_e4=actor.published_position();
        t.matrix_f4=rows_of(root_placement_matrix(actor.floor_root_placement()));
        t.invulnerable_33a4=actor.health().invulnerable_33a4;
        t.fire_immune_33b8=actor.health().fire_immune_33b8;
        t.burning_326a=actor.health().burning_326a;
        t.night_vision_3470=actor.item_actions().vision_3470==1;   // 00259d54: == 1
        t.killed_33a5=actor.health().killed_33a5;                    // Checkpoint 132fx1 (00259db8)
        if(c.not_gasable_346b)t.not_gasable_346b=c.not_gasable_346b(a);   // 002596e0 (read only by the gases)
        if(!c.stealth_44)fail("the stealth +44 (00133ab0, model light gather 00373320) the flash's blindness reads is not hosted");
        t.stealth_44=c.stealth_44(a);
        return t;
    }
    AreaOwner owner(std::uint64_t object) override{
        auto& actor=aw().actor(index(object));
        return {actor.player_controller(),actor.byte_14()!=0};
    }
    // Checkpoint 132fx1: the gases' reads. 00132910(00132a30(object), node): the skeleton node's world row 3.
    RootVector node_position(std::uint64_t object,std::int32_t node) override{
        if(!c.not_gasable_346b)fail("the gas exposure 002596e0 reads +346b (CombatThrowConfig::not_gasable_346b)");
        return aw().actor(index(object)).skeleton_node_world_matrix(node)[3];
    }
    // 00148290 with list 0 over the static world, the excluded mask 0x58 (0025a2d0 / 002596e0): the hit point.
    std::optional<RootVector> los(const RootVector& start,const RootVector& displacement) override{
        const auto hit=trace_world_los(aw().collision(),WorldLosQuery{start,displacement,0,0x58});
        if(!hit)return std::nullopt;
        return hit->point;
    }
    // 00259bb0 (fire): 00186360(object, 1, owner or null, 1, 0, -1.0).
    void ignite(std::uint64_t object,std::uint64_t owner) override{ignite(index(object),1,owner,1,0,-1.0F);}
    void flash(std::uint64_t object,float amount) override{apply_actor_flash(senses.at(index(object)),amount);}   // 001866e0
    void blind(std::uint64_t object,float amount) override{                                                      // 00186830
        const auto a=index(object);
        auto& actor=aw().actor(a);
        if(apply_actor_blind(senses.at(a),amount,actor.byte_14()!=0,actor.player_controller()))frame.white_outs.push_back({a,amount});
    }
    // Checkpoint 132fx1: the gases' object +100(object, owner, category, type, null, 0, null, amount), as the fire zone's.
    void damage(std::uint64_t object,std::uint64_t owner,std::uint8_t category,std::uint8_t type,float amount) override{
        const auto a=index(object);
        auto r=damage_request(owner,category,type,amount);
        frame.fire_damage.push_back({a,r.source,r.category,r.type,r.amount});
        s.actors->damage_request(a,r);
    }
    // Action 6 sets +3539; 00199980 consumes it during the target's next logic.
    void tear_gas(std::uint64_t object) override{aw().actor(index(object)).receive_tear_gas();}

    // 00141e20 for actor a's held slot, reading +33cc / +33d0 from `state`
    // (the live fields or a receiver candidate) with the combat inventory
    // current.
    void launch(std::size_t a,float arc,const ActorAnimationReceiverState& state){
        auto& actor=aw().actor(a);
        auto& inv=s.combat->inventory(a);
        if(inv.current_84>=inv.slots.size())fail("00141e20 without a held slot");
        const auto& record=s.database->record(inv.slots[inv.current_84].item);
        if(record.hold_class_f0()!=1&&record.hold_class_f0()!=7)
            fail("a launch of hold class "+std::to_string(record.hold_class_f0())+" is not hosted");
        const auto type=record.damage_type_bc();
        // Type 4 (fire) since 132ms8: its area's fire branch (00259bb0) is this host's ignite (00186360).
        // Checkpoint 132fx1: the lethal (5) and knockout (7) gases (002596e0 over node_position / los, the damage
        // virtual), tear gas (6: action 6 / 00199980), and smoke (8: its area
        // does nothing; perception LOS reads it through 002594a0).
        if(!(type==0||type==2||type==3||type==4||type==5||type==6||type==7||type==8||type==9||type==13))
            fail("a thrown item of damage type "+std::to_string(type)+" is not hosted");
        ThrowLaunchActor l;
        l.hand_frame=actor.skeleton_node_world_matrix(hand_node);                         // 00132a30 / 00133ed0
        l.actor_matrix=rows_of(root_placement_matrix(actor.floor_root_placement()));       // virtual +f4
        l.ai_controller=!actor.player_controller();                                        // +58 virtual +58
        l.owner={actor.handle(),s.object_ids.at(a)};
        const auto in=throw_launch_input(state,arc,l,[this](std::uint32_t word)->std::optional<ThrowLaunchTarget>{
            if(word==*c.none_word)return std::nullopt;
            const auto o=s.find(static_cast<std::int32_t>(word));                         // 0040c540
            if(!o)return std::nullopt;
            return ThrowLaunchTarget{o,aw().actor(index(o)).published_position()};
        });
        auto r=[&]{
            PortAmmoGuard ammo(inv,actor.player_controller()&&port_cheat(PortCheat::infinite_ammo));   // port-only (132f5)
            return world->launch(inv,in);
        }();
        for(const auto& e:r.events){
            if(e.kind==InventoryLaunchEvent::Kind::star_statistics){
                // Checkpoint 132ia1: 0013fe80(inventory, point W == 1) after a hold-7 launch (00142060), as the
                // shot statistics (0013fea0..0013fee0): only a player controller's actor with +14 adds 9 (shots)
                // and, with the flag, 0xb.
                if(!actor.player_controller()||actor.byte_14()==0)continue;
                if(!s.statistics)fail("the star statistics 0013fe80 have no agent statistics (CombatThrowSession::statistics)");
                add_agent_statistic(*s.statistics,9,1);
                if(e.flag)add_agent_statistic(*s.statistics,0xb,1);
                continue;
            }
            if(e.kind!=InventoryLaunchEvent::Kind::fire_notice)continue;
            if(!s.fire_notice)fail("the fire notice +cc of an AI thrower is not hosted");
            s.fire_notice(a,e.object,e.flag);
        }
        frame.launches.emplace_back(a,std::move(r));
    }

    // 00199590(actor, ir) (actor_goggles.hpp): the item trigger's one-shot (below) and, Checkpoint 132fx1, the pickup
    // menu's goggles put back (0025200c, ir 0).
    void switch_ir(std::size_t a,std::uint8_t ir){
        auto& actor=aw().actor(a);
        GogglesActorView v;
        v.slot6_item=s.combat->inventory(a).slots.at(6).item;                         // 0013fb50: inventory +68
        if(v.slot6_item!=-1)v.record_100=static_cast<std::int8_t>(s.database->record(v.slot6_item).byte(0x100));
        v.controller_player=actor.player_controller();v.byte_14=actor.byte_14()!=0;
        std::vector<GogglesEvent> ev;
        switch_actor_ir(actor.item_actions(),ir,v,ev);
        for(const auto& e:ev){
            // 00142e00(inventory, 6, ir): the byte 003855d0 reads to draw the light part (Checkpoint 132cp10).
            if(e.kind==GogglesEvent::Kind::render_byte)s.combat->write_slot_render_byte(a,static_cast<std::uint8_t>(e.a),static_cast<std::uint8_t>(e.b));
            frame.goggles.emplace_back(a,e);
        }
    }
    // 00197e00 at 0x001b1d10 (from 00198278).
    void item_triggers(std::size_t a){
        auto& actor=aw().actor(a);
        ActorItemTriggerHost h;
        h.none_word=*c.none_word;
        h.phase=[this,a](const SecondaryMotionName& name){return secondary_motion_phase(s.actors->secondary(a),playbacks.at(a),name);};
        h.launch=[this,a](float arc,ActorAnimationReceiverState& state){
            s.actors->collect_inventory_view(a,*s.combat);
            launch(a,arc,state);
            s.actors->publish_inventory_view(a,*s.combat);
        };
        h.melee_hit=[this,a](std::uint32_t word){
            const auto o=s.find(static_cast<std::int32_t>(word));                         // 0040c540
            if(!o)return;                                                                  // 00198634: no call
            melee_hit(a,o);
        };
        // 00198410..00198460: 001ca210(desc, 1, "G_THROWING_STAR" 0049e060, 0, 0), 0020f990(msg, desc, actor),
        // +20 = 1, to the sound manager 004ffb50 with transport 0. Recorded, as the stun's sounds.
        h.star_sound=[this,a]{frame.star_sounds.push_back(a);};
        // 00199590(actor, +33e6) (actor_goggles.hpp). A local player with +33e7 (the toggles 00199770 / 00199860
        // write it) reaches the HUD 00244a80: the hud_ir event, applied by the host's HUD object.
        h.switch_on_ir=[this,a](std::uint8_t ir){switch_ir(a,ir);};
        // +3314 exists while the actor is activated (001b5560 / 001b5190).
        // The IR argument is +33e6 (00198564), written by the goggles switch 001992b0.
        const auto t=run_actor_item_triggers(actor.item_actions(),actor.eligible(),actor.item_actions().byte_33e6,h);
        if(t.launched||t.star||t.ir||t.melee)frame.triggers.emplace_back(a,t);
    }

    // 001960c0 -> 00197560 for hold class 1, 00197140 for hold class 7.
    void start(std::size_t a,std::int32_t hold_class,const ActorFireRequest& r){
        if(hold_class!=1&&hold_class!=7)fail("the throw start of hold class "+std::to_string(hold_class)+" is not hosted");
        // The request's name word (payload +0): the target's +c, -1 without one.
        if(r.target_id==-1&&*c.none_word!=0xffffffffU)fail("a request without a target carries -1, which is not the configured none word *004bdd00");
        auto& actor=aw().actor(a);
        SecondaryComponent* secondary=actor.eligible()?&s.actors->secondary(a):nullptr;
        // Checkpoint 132ia1: a local actor's star message 004eeed0 (st.star_message) goes to the actor itself
        // with transport 0: only the sender (00197274) and the network factory 00197440 reference its key, so
        // nothing receives it locally (a replication, as 004eeee8 and the stun's 004ef078). Recorded only.
        const auto st=hold_class==7
            ?start_actor_star_throw(actor.item_actions(),secondary,s.combat->inventory(a).busy_ef,actor.handle(),
                static_cast<std::uint32_t>(r.target_id),r.point)
            :start_actor_grenade_throw(actor.item_actions(),secondary,s.combat->inventory(a).busy_ef,actor.handle(),
                static_cast<std::uint32_t>(r.target_id),r.point,r.power);
        frame.starts.emplace_back(a,st);
    }

    // The services of the stun 00184ce0 and its countdown for actor a.
    ActorStunHost stun_host(std::size_t a){
        ActorStunHost h;
        // 004ef078 with transport 0: the network replication (its receiver
        // 00184f90 is a network factory; no local delivery, as 004eeee8).
        h.replicate=[](std::uint8_t,float,float){};
        h.effect=[this,a](std::uint32_t kind,std::uint32_t on,float f12){set_effect(a,kind,on,f12);};
        h.sound=[this,a](std::uint32_t d){frame.stun_sounds.push_back({a,d,true});};
        h.stop_sound=[this,a](std::uint32_t d){frame.stun_sounds.push_back({a,d,false});};
        // 0055d480 with transport 1: delivered at once to 001a92a0 (case 7 ignores +28).
        h.action=[this,a](std::int32_t action_24,std::uint8_t kind_28,std::int32_t code_2c){
            if(action_24!=stun_action_24||kind_28!=stun_action_kind_28)fail("an unexpected stun action message");
            const auto code=static_cast<std::uint32_t>(code_2c);
            frame.stunned_actions.push_back({a,code,aw().actor(a).receive_stunned_action(code)});
        };
        h.damage=[this,a](std::uint8_t category,std::uint8_t type,float amount){
            s.actors->damage_without_penetration(a,a,category,type,amount);
        };
        return h;
    }

    // 00185d40's services for actor a: the effect object 00236810 (vtable
    // 004dbd80) and the fire zone 002624a0 (vtable 004dc970) with
    // session-assigned keys, updated by update_objects; the calls are recorded.
    ActorEffectSlotHost effect_host(std::size_t a){
        ActorEffectSlotHost h;
        const auto record=[this,a](std::string what,std::uint32_t object,std::uint32_t descriptor=0,std::uint32_t count=0,
                                   float value=0,float value2=0){
            frame.effect_calls.push_back({a,std::move(what),object,descriptor,count,value,value2});
        };
        const auto live=[this](std::uint32_t o)->EffectObject&{
            const auto at=effect_objects.find(o);
            if(at==effect_objects.end()||at->second.released)fail("00185d40 on an effect object that does not exist");
            return at->second;
        };
        const auto create=[this,a]{
            const auto o=next_effect_object++;
            effect_objects[o]=EffectObject{o};
            effect_objects[o].actor=a;
            return o;
        };
        h.key_of=[this](std::uint32_t o){
            const auto at=effect_objects.find(o);
            if(at==effect_objects.end())fail("00185d40 reads +c of an object that is not an effect object");
            return at->second.key;
        };
        h.create_effect=[this,a,record,create](std::uint32_t descriptor,std::uint32_t count,float f12){
            const auto o=create();
            effect_objects[o].effect=make_fire_effect_object(aw().actor(a).handle(),descriptor,count,f12);
            record("create",o,descriptor,count,f12);return o;};
        // 002624a0 registers itself (0040c9d0) and runs its virtual +5c(1) (003ce980: +2d = 1).
        h.create_light=[this,a,record,create](float f12,float f13){
            const auto o=create();
            auto& z=effect_objects[o];
            z.zone=make_fire_zone(aw().actor(a).handle(),static_cast<std::uint32_t>(s.object_ids.at(a)),f12,f13);
            z.registered=true;registration.push_back(o);
            z.active=true;
            record("light",o,0,0,f12,f13);return o;};
        h.register_object=[this,record,live](std::uint32_t o){
            if(o){live(o).registered=true;registration.push_back(o);}   // 0040c9d0 with a null handle adds nothing
            record("register",o);};
        h.unregister_object=[this,record,live](std::uint32_t o){live(o).registered=false;removals.push_back(o);record("unregister",o);};
        // 00394270 / 003941a0 with +60 = -1 and +64 = 0 (00236810): 003ce980 / 003ce790 (+2d = 1 / 0).
        h.activate=[record,live](std::uint32_t o){live(o).active=true;record("on",o);};
        h.deactivate=[record,live](std::uint32_t o){live(o).active=false;record("off",o);};
        // 003cb230 deletes it `delay` later; its destructor 00236580 kills each record's live instance
        // (003a2ba0 + 0036b130). Port-only: the host's instances go at the release (the object is off by then).
        h.release=[this,record,live](std::uint32_t o,float delay){
            auto& obj=live(o);obj.released=true;record("release",o,0,0,delay);
            if(obj.effect)for(const auto& st:obj.effect->states_7c){
                const auto at=emitters.find(st.emitter_0);
                if(st.emitter_0&&at!=emitters.end()&&at->second.particle){
                    if(c.particles.destroy)c.particles.destroy(at->second.particle);
                    at->second.particle=0;
                }
            }
        };
        h.stop_sound=[record](std::string_view name){record("stop_sound:"+std::string(name),0);};
        return h;
    }

    // 00186360(actor a, on, source, a3, t0, f12): +326a is the actor's health
    // byte (with +33a4 / +33b8); 00185d40 runs on the actor's effect slots and
    // the fire sound start / stop (to 004ffb50, transport 0) is recorded.
    // *005721c8 is 0 in solo: no replication.
    void ignite(std::size_t a,std::uint64_t on,std::uint64_t source,std::uint64_t a3,std::uint64_t t0,float f12){
        auto& actor=aw().actor(a);
        auto& st=ignitions.at(a);
        const auto& health=actor.health();
        st.burning_326a=health.burning_326a;st.invulnerable_33a4=health.invulnerable_33a4;st.fire_immune_33b8=health.fire_immune_33b8;
        ActorIgnitionHost h;
        h.effect=[this,a,&st](std::uint32_t kind,std::uint32_t on_,float value){
            auto& slots=effects.at(a);
            slots.burning_326a=st.burning_326a!=0;
            set_actor_effect(slots,kind,on_,value,effect_host(a));
            if(!slots.burning_326a)st.burning_326a=0;                      // 00185d40 k 0 off: +326a = 0
        };
        h.key_of=[this](std::uint64_t o){return static_cast<std::uint32_t>(s.object_ids.at(index(o)));};
        h.start_sound=[this,a](std::string_view name){frame.effect_calls.push_back({a,"fire_sound:"+std::string(name)});};
        h.stop_sound=[this,a](std::string_view name){frame.effect_calls.push_back({a,"fire_sound_stop:"+std::string(name)});};
        h.networked=[]{return false;};
        h.replicate=[](std::uint32_t,std::uint32_t){fail("the ignition's replication 004e0180 (*005721c8) is not hosted");};
        ignite_actor(st,on,source,a3,t0,f12,h);
        actor.set_burning_326a(st.burning_326a!=0);
        frame.ignitions.push_back({a,st});
    }

    // 00185d40(actor a, kind, on, f12) outside the ignition: the slot state's
    // +326a is the actor's health byte, and a kind-0 turn-off clears it.
    void set_effect(std::size_t a,std::uint32_t kind,std::uint32_t on,float f12){
        auto& actor=aw().actor(a);
        auto& slots=effects.at(a);
        slots.burning_326a=actor.health().burning_326a!=0;
        set_actor_effect(slots,kind,on,f12,effect_host(a));
        if(!slots.burning_326a&&actor.health().burning_326a)actor.set_burning_326a(false);
    }

    // 00141880(+3524, target, 0) for actor a (00198644).
    void melee_hit(std::size_t a,std::uint64_t target){
        auto& actor=aw().actor(a);
        const auto human=[this](std::uint64_t o)->SoloActorSimulation&{return aw().actor(index(o));};
        s.actors->collect_inventory_view(a,*s.combat);
        const auto& inv=s.combat->inventory(a);
        MeleeHitInput in;
        in.mode_84=inv.current_84;
        in.item_58=static_cast<std::uint32_t>(inv.slots[5].item);                         // +58: slot 5 (+8 + 5 * 16)
        in.attacker=actor.handle();                                                        // inventory +88
        in.target=target;in.forced=false;
        if(!c.knife_damage)fail("doKnifeDmg *00489da0 (CombatThrowConfig::knife_damage) is not supplied");
        in.knife_damage=c.knife_damage();
        MeleeHitHost h;
        h.record=[this](std::uint32_t item){
            const auto& r=s.database->record(static_cast<std::int32_t>(item));
            return MeleeHitRecord{r.hold_class_f0(),r.damage_mid_64(),r.penetration_f8()};
        };
        h.player=[human](std::uint64_t o){return human(o).player_controller();};
        h.position_e4=[human](std::uint64_t o){return human(o).published_position();};
        // 00148290 with list 0: 003c26d0 over the static world (as the dying
        // ground probe, npc_world.cpp), required mask 0.
        h.blocked=[this](const RootVector& origin,const RootVector& d,std::uint32_t excluded){
            return trace_world_los(aw().collision(),WorldLosQuery{origin,d,0,static_cast<std::uint8_t>(excluded)}).has_value();
        };
        h.life_state=[human](std::uint64_t o){return human(o).life_state();};
        h.actor_class_4c=[this](std::uint64_t o){(void)index(o);return true;};            // every actor of the world is human
        h.stun_immune_346a=[human](std::uint64_t o){return human(o).stun_immune_346a()!=0;};
        h.byte_14=[human](std::uint64_t o){return human(o).byte_14()!=0;};
        h.invulnerable_33a4=[human](std::uint64_t o){return human(o).health().invulnerable_33a4;};
        h.statistic=[this](std::uint32_t id,std::uint32_t amount){
            if(!s.statistics)fail("the knockout statistic 00333f10 has no agent statistics (CombatThrowSession::statistics)");
            add_agent_statistic(*s.statistics,id,static_cast<std::int32_t>(amount));
        };
        h.stun=[this](std::uint64_t o,std::uint32_t lethal,std::uint32_t notify,float duration,float f13){
            start_actor_stun(stuns.at(index(o)),lethal,notify,duration,f13,stun_host(index(o)));
        };
        h.ignite=[this](std::uint64_t o,std::uint32_t a1,std::uint64_t attacker,std::uint32_t a3,std::uint32_t t0,float f12){
            ignite(index(o),a1,attacker,a3,t0,f12);
        };
        // 00188140 with record +4c = 1: the bullet hit volumes with volume 1.0
        // (the result is not read).
        h.hit_volumes=[this](std::uint64_t o,const RootVector& origin,const RootVector& dir,std::int32_t mode,float volume){
            (void)s.actors->hit_volumes(index(o),origin,dir,mode,volume);
        };
        h.damage=[this](std::uint64_t o,std::uint64_t source,std::uint8_t category,std::uint8_t type,std::int32_t& pen,float amount){
            s.actors->damage(index(o),index(source),category,type,pen,amount);
        };
        frame.melee_hits.emplace_back(a,CombatMeleeHit{target,apply_melee_hit(in,h)});
    }

    // 001960c0's melee branch (001963ac..001963c4): 001414d0(inventory,
    // &sp+9f), then 00196480(actor, target, sp+9f).
    void melee(std::size_t a){
        auto& actor=aw().actor(a);
        const auto human=[this](std::uint64_t o)->SoloActorSimulation&{return aw().actor(index(o));};
        MeleeTargetHost th;
        th.attacker=actor.handle();                                                        // inventory +88
        th.candidates=*s.registered;                                                       // 004f78dc
        // Checkpoint 132pc1: the list also holds a script's SetTarget nodes and GOBs (0022c650 takes any class).
        // 001414d0 reads each candidate's +c, its +58 (0 for a node or GOB: 003cf0e0 at 003cf144..003cf164), its
        // virtual +108 (node / GOB 00141850) and then skips it at the +4c class test (00141630: not the actor
        // class *0049d010). The port threw at the first read ("not an actor of the world"): the play-test crash of
        // a baton swing with a SetTarget node or GOB registered.
        const auto base=[this](std::uint64_t o)->CombatThrowSession::BaseObject{
            const auto b=s.base_object?s.base_object(o):std::nullopt;
            if(!b)fail("001414d0: a listed object "+std::to_string(o)+" that is neither an actor, a node nor a hosted GOB");
            return *b;
        };
        th.object_id=[this,base](std::uint64_t o){return actor_of(o)?s.object_ids.at(*actor_of(o)):base(o).id;};
        th.player=[this,human](std::uint64_t o){return actor_of(o)&&human(o).player_controller();};   // +58 0: not a player
        th.life_state=[this,human,base](std::uint64_t o){return actor_of(o)?human(o).life_state():base(o).life_108;};
        th.actor_class_4c=[this](std::uint64_t o){return actor_of(o).has_value();};      // every actor of the world is human
        th.position_e4=[human](std::uint64_t o){return human(o).published_position();};
        th.matrix_f4=[human](std::uint64_t o){return rows_of(root_placement_matrix(human(o).floor_root_placement()));};
        th.eye_c0=[this](std::uint64_t o){return s.actors->eye_c0(index(o),-1);};
        th.invulnerable_33a4=[human](std::uint64_t o){return human(o).health().invulnerable_33a4;};
        th.velocity_310=[human](std::uint64_t o)->std::optional<RootVector>{
            const auto& t=human(o);
            if(!t.eligible())return std::nullopt;                                          // no +3310: 004e1d90
            return t.physical().integrator.velocity;
        };
        CombatMeleeStart rec;
        rec.choice=choose_melee_target(th);
        MeleeAttackInput in;
        in.actor=actor.handle();
        in.target=rec.choice.target;
        in.target_id=rec.choice.target?s.object_ids.at(index(rec.choice.target)):-1;
        in.takedown=rec.choice.takedown;
        in.hold_class=static_cast<std::uint32_t>(weapon_held_class(s.combat->inventory(a),*s.database));   // 001437a0(+3524)
        in.secondary=actor.eligible();                                                     // +3314 exists while activated
        in.attacker_physical=actor.eligible();
        in.target_physical=rec.choice.target&&human(rec.choice.target).eligible();
        if(actor.byte_14()){
            // *004ffbd0 is the hotbox tracker (0024a3ac / 0024ecdc pass it to
            // 001714c0 / 00171260); its +24 is the tracker timer.
            if(!actor.player_controller())fail("the hotbox tracker *004ffbd0 of a locally owned non-player actor is not hosted");
            in.guard_24=actor.hotbox_tracker().timer_24;
        }
        MeleeAttackHost h;
        h.life_state=th.life_state;
        h.player=th.player;
        h.position_e4=th.position_e4;
        // Physical +3d0 = 0 on the attacker and (with a physical) the victim.
        h.clear_physical_3d0=[human,&rec](std::uint64_t o){human(o).clear_physical_3d0();rec.clears_3d0.push_back(o);};
        h.physical_reset_11=[human](std::uint64_t o){human(o).request_physical_reset();};
        h.resolve=[human](std::uint64_t o,std::string_view name){return human(o).resolve_mapped_motion(name);};   // 0018c5a0
        h.retained=[human](std::uint64_t o){return human(o).retained_motion();};                                  // +326c
        h.install=[human](std::uint64_t o,const MotionRequest& q,MotionInstallMode m){human(o).install_motion(q,m);};   // 003ad500
        h.matrix_f4=th.matrix_f4;
        // 001bc4b0 with transport 1; an AI controller's teleport tail (virtual
        // +50, 0015b820) does nothing, and a parented root makes it a no-op
        // (as the AI's own teleport, npc_world.cpp).
        h.teleport=[human,&rec](std::uint64_t o,const TargetingMatrix& m){
            rec.teleports.emplace_back(o,human(o).teleport(ActorTeleportMode::place,m));
        };
        h.death_motion=[human](std::uint64_t o,std::string_view name,std::uint8_t code){human(o).set_death_motion(name,code);};   // 0018c9d0
        // Virtual +100 with t0 = 0 (no penetration), t1 = t2 = 0.
        h.damage=[this](std::uint64_t o,std::uint64_t source,std::uint8_t category,std::uint8_t type,float amount){
            s.actors->damage_without_penetration(index(o),index(source),category,type,amount);
        };
        // 00131540(G, actor, 0): the view of the camera object G +d4, then
        // 00131320(G, G +d4) and G +d8 = 1 on the attacker's world_objects.
        h.push_world_object=[this,human,&rec](std::uint64_t o){
            auto& attacker=human(o);
            const auto camera=player_takedown_camera(o,attacker.skeleton_node_world_position(takedown_camera_head_node),
                rows_of(root_placement_matrix(attacker.floor_root_placement())),
                [this](const LosServiceRequest& q){return answer_los_request(aw().collision(),q);});
            ActorWorldObjectSnapshot state;state.byte_2c=1;
            attacker.push_pending_world_object(takedown_camera_object,state);
            cameras.at(index(o))=camera;
            rec.camera=camera;
        };
        h.one_shot=[this,a](const std::array<std::uint8_t,48>& name,std::uint8_t kind,const SecondaryOneShot& e){
            return request_secondary_one_shot(s.actors->secondary(a),name,kind,e);                                // 001cae20
        };
        h.sound=[&rec](std::uint32_t descriptor,const RootVector& p){rec.sounds.emplace_back(descriptor,p);};
        // Solo 003c9420 with transport 0 clones and queues the message
        // (003c9950..003c997c); only 00196480 and the network factory 00197380
        // reference its key 004eeee8, no handler loads it, so the queued
        // delivery has no effect. MeleeAttackResult::replicated records it.
        h.replicate=[](std::uint64_t,std::uint64_t,bool){};
        rec.result=start_melee_attack(actor.item_actions(),in,h);
        frame.melee.emplace_back(a,std::move(rec));
    }

    // Checkpoint 132ia1c: the air taser (air_taser.hpp) on actor a's
    // inventory object +0 (roles 1 / 3 only, weapon_inventory.cpp).
    AirTaserObject& taser_object(std::size_t a){
        auto& inv=s.combat->inventory(a);
        if(!inv.object_0)fail("the air taser 00266200 / 00262d90 / 00263080 of an inventory without its object +0 (roles 1 / 3 only)");
        return *inv.object_0;
    }
    AirTaserContext taser_context(std::size_t a,const AirTaserObject& obj){
        auto& actor=aw().actor(a);
        if(!obj.record_0)fail("the air taser object +0 names no record (no hold class 5 item was set)");
        AirTaserContext ctx;
        ctx.owner=actor.handle();
        ctx.owner_id=s.object_ids.at(a);
        ctx.owned_14=actor.byte_14()!=0;
        ctx.record=&s.database->record(*obj.record_0);
        ctx.range_3c=ctx.record->far_3c();
        ctx.noise_fc=ctx.record->impact_noise_radius_fc();
        return ctx;
    }
    void taser_event(std::size_t a,std::string what,std::uint64_t object=0,std::uint32_t descriptor=0,
        const RootVector& position={},float value=0.0F,std::int32_t code=0){
        frame.taser.push_back({a,std::move(what),object,descriptor,position,value,code});
    }
    AirTaserHost taser_host(std::size_t a){
        const auto human=[this](std::uint64_t o)->SoloActorSimulation&{return aw().actor(index(o));};
        const auto base=[this](std::uint64_t o)->std::optional<CombatThrowSession::BaseObject>{
            return s.base_object?s.base_object(o):std::nullopt;};
        AirTaserHost h;
        h.controller_matrix=[this,a](bool aim_view){
            const auto f=s.actors->shot_frame(a,false,aim_view);
            return AirTaserMatrix{f[0],f[1],f[2],f[3]};
        };
        h.aim_range=[this,a]{
            const auto r=s.combat->inventory(a).aim_view_range_f4;
            if(!r)fail("the air taser's aim range (inventory +f4) is not known");
            return *r;
        };
        // Virtual +c0: an actor's eye, a listed node / GOB / hotbox's +e4 (0014d340).
        h.target_point=[this](std::uint64_t o,std::int32_t sub)->RootVector{
            if(const auto i=actor_of(o))return s.actors->eye_c0(*i,sub);
            const auto& list=s.combat->object_list();
            if(list.base_object&&list.base_object(o)&&list.base_eye_c0)return list.base_eye_c0(o);
            fail("the air taser's target "+std::to_string(o)+" is neither an actor nor a listed object");
        };
        // A gone object reads id -1: its weak handle (+2c / +30) no longer resolves.
        h.facts=[this,human,base](std::uint64_t o){
            AirTaserObjectFacts f;
            if(const auto i=actor_of(o)){
                auto& t=aw().actor(*i);
                f.id_c=s.object_ids.at(*i);
                f.actor_4c=true;                                                           // every actor of the world is human
                f.state_320c=t.actions().highest_action;
                f.invulnerable_33a4=t.health().invulnerable_33a4;
                f.byte_346a=t.stun_immune_346a()!=0;
                f.flak_33a0=s.actors->armor_33a0(*i)!=0;
            }else if(const auto b=base(o))f.id_c=b->id;
            return f;
        };
        h.life=[this,base](std::uint64_t o)->std::uint8_t{
            if(const auto i=actor_of(o))return aw().actor(*i).life_state();
            if(const auto b=base(o))return b->life_108;
            fail("the air taser's hit object "+std::to_string(o)+" is neither an actor nor a listed object");
        };
        h.player=[this](std::uint64_t o)->std::optional<bool>{
            if(const auto i=actor_of(o))return aw().actor(*i).player_controller();
            return std::nullopt;
        };
        h.positional_sound=[this,a](std::uint32_t d,const RootVector& p){taser_event(a,"sound",0,d,p);};
        h.sound=[this,a](std::uint32_t d,std::uint64_t o){taser_event(a,"sound",o,d);};
        h.stop_sound=[this,a](std::uint32_t d,std::uint64_t o){taser_event(a,"stop sound",o,d);};
        h.effect=[this,a](std::uint64_t o,std::int32_t kind,std::int32_t on,float amount){
            taser_event(a,"effect",o,static_cast<std::uint32_t>(kind),{},amount,on);
            set_effect(index(o),static_cast<std::uint32_t>(kind),static_cast<std::uint32_t>(on),amount);
        };
        h.stun_action=[this](std::uint64_t o,std::int32_t code){
            const auto i=index(o);
            frame.stunned_actions.push_back({i,static_cast<std::uint32_t>(code),aw().actor(i).receive_stunned_action(static_cast<std::uint32_t>(code))});
        };
        h.damage=[this,a](std::uint64_t o,std::uint64_t source,std::int32_t category,std::int32_t type,float amount){
            taser_event(a,"damage",o,static_cast<std::uint32_t>(type),{},amount,category);
            s.actors->damage_without_penetration(index(o),index(source),static_cast<std::uint8_t>(category),static_cast<std::uint8_t>(type),amount);
        };
        h.statistic=[this,a]{
            if(!s.statistics)fail("the knockout statistic 00333f10 has no agent statistics (CombatThrowSession::statistics)");
            add_agent_statistic(*s.statistics,6,1);
            taser_event(a,"statistic",0,6);
        };
        // 00131540(G, victim, taser owner): the target branch's camera on the
        // owner's world objects (as the takedown's, melee above).
        h.camera_target=[this,a,human](std::uint64_t v,std::uint64_t owner){
            auto& victim=human(v);
            auto& holder=human(owner);
            const auto f4=rows_of(root_placement_matrix(victim.floor_root_placement()));
            TakedownCameraTarget t;
            t.target_e4=holder.published_position();
            t.relative_z=melee_relative_angles(f4,s.actors->eye_c0(index(v),-1),s.actors->eye_c0(index(owner),-1)).angles[2];
            t.actor_e4=victim.published_position();
            const auto camera=player_takedown_camera(v,victim.skeleton_node_world_position(takedown_camera_head_node),f4,
                [this](const LosServiceRequest& q){return answer_los_request(aw().collision(),q);},&t);
            ActorWorldObjectSnapshot state;state.byte_2c=1;
            holder.push_pending_world_object(takedown_camera_object,state);
            cameras.at(index(owner))=camera;
            taser_event(a,"camera",v);
        };
        h.camera_release=[this,a]{aw().actor(a).finish_pending_world_object();taser_event(a,"camera release");};
        h.scan=[this](BulletHitRequest& q){
            std::vector<CombatEvent> hits;
            const auto pool=s.combat->scan_host(hits);
            std::vector<BulletEvent> events;
            scan_bullet_candidates(q,0,pool,events);
            for(auto& e:events)frame.scan_events.push_back(std::move(e));
            for(auto& e:hits)frame.hit_events.push_back(std::move(e));
        };
        h.los=[this](const RootVector& start,const RootVector& d,std::uint64_t){
            return projectile_world_los(aw().collision(),start,d).has_value();
        };
        // 0012f2b0 is o + 0xc and 00174080 a word compare: the object's +c is the id.
        h.registered=[this,base](std::uint64_t o,std::int32_t id){
            if(const auto i=actor_of(o))return s.object_ids.at(*i)==id;
            const auto b=base(o);
            return b&&b->id==id;
        };
        h.node_matrix=[this](std::uint64_t o,std::int32_t node){
            const auto m=aw().actor(index(o)).skeleton_node_world_matrix(node);
            return AirTaserMatrix{m[0],m[1],m[2],m[3]};
        };
        h.node_position=[this](std::uint64_t o,std::int32_t node){return aw().actor(index(o)).skeleton_node_world_position(node);};
        h.position=[this](std::uint64_t o){return rows_of(root_placement_matrix(aw().actor(index(o)).floor_root_placement()))[3];};
        h.random=[this]{return next_game_random(aw().random());};
        // The wire / spark particles (Tazer_Line_glo1, 003a2dd0) are presentation: a handle and the events, and
        // (Checkpoint 132fx3) the host's instance: 00265b90 (line 0xc6) and 00263890 (line 0x23b) both take
        // f12 = *0048b1a0 (0.8) and the 003a35c0 name 004a8200; 003a2400 runs with a null velocity (a2 = 0).
        h.create_effect=[this,a](std::int32_t line,const AirTaserMatrix& m){
            const auto e=next_taser_effect++;
            taser_event(a,"create effect",e,static_cast<std::uint32_t>(line),m[3]);
            if(c.particles.create)if(const auto p=c.particles.create("Tazer_Line_glo1",m,air_taser_effect_scale))taser_particles[e]=p;
            return e;
        };
        h.update_effect=[this](std::uint64_t e,const AirTaserMatrix& m,float dt){
            const auto at=taser_particles.find(e);
            if(at!=taser_particles.end()&&c.particles.update)c.particles.update(at->second,m,nullptr,dt);
        };
        h.destroy_effect=[this,a](std::uint64_t e,std::int32_t line){
            taser_event(a,"destroy effect",e,static_cast<std::uint32_t>(line));
            const auto at=taser_particles.find(e);
            if(at!=taser_particles.end()){
                if(c.particles.destroy)c.particles.destroy(at->second);
                taser_particles.erase(at);
            }
        };
        h.burn=[this,a](const AirTaserMatrix& m){
            taser_event(a,"burn",0,0,m[3]);
            if(c.particles.burn)c.particles.burn(m);};                                   // 132fx3: 00232120(10, m)
        h.flak=[this,a](std::uint64_t o){taser_event(a,"flak",o);};
        h.noise=[this,a](std::int32_t id,const RootVector& p,float radius){taser_event(a,"noise",0,static_cast<std::uint32_t>(id),p,radius);};
        const auto* torch=s.actors->torch_host(a);
        h.lights=torch?torch->lights:nullptr;                                              // null: air_taser.cpp throws at its first light use
        return h;
    }
    std::uint64_t next_taser_effect{1};
    std::map<std::uint64_t,std::uint64_t> taser_particles;          // 132fx3: taser effect -> particle handle
    static constexpr float air_taser_effect_scale=0.8F;             // *0048b1a0
    bool taser_fire(std::size_t a,std::uint64_t target,bool aim_view,std::int32_t subtarget){
        auto& obj=taser_object(a);
        const auto before=obj.state_8;
        const auto r=air_taser_entry(obj,target,aim_view,subtarget,taser_context(a,obj),taser_host(a));
        taser_event(a,r?"fire":"fire busy",target,before,obj.aim_a0,0.0F,subtarget);
        return r;
    }
    void taser_update(std::size_t a,float dt){
        auto& obj=taser_object(a);
        const auto before=obj.state_8;
        omega::air_taser_update(obj,dt,taser_context(a,obj),taser_host(a));
        if(obj.state_8!=before)taser_event(a,"state",obj.victim_2c,obj.state_8,obj.frame_60[3],obj.shock_b4,before);
    }
    void taser_release(std::size_t a){
        auto& obj=taser_object(a);
        const auto before=obj.state_8;
        if(before==0)return;                                                               // 00263080's first test
        omega::air_taser_release(obj,taser_context(a,obj),taser_host(a));
        if(obj.state_8!=before)taser_event(a,"release",obj.victim_2c,obj.state_8,obj.frame_60[3],obj.shock_b4,before);
    }

    // What 001cb160 does after its state step: the completion posts (001d79b0
    // / 001d7940 -> 003c9420, immediate) reach the actor's receiver, then
    // 001d7480 and 001d6120's timing tail.
    void playback(std::size_t a,const SecondaryStep& step,bool first,float dt){
        auto& actor=aw().actor(a);
        for(const auto& p:step.posts){
            MotionEffect e;e.kind=MotionEffect::Kind::embedded;e.id=p.code;e.action=p.action;e.receiver=p.receiver;
            actor.receive_animation_effect(e);
            frame.posts.emplace_back(a,p);
        }
        if(!step.posts.empty()){
            s.actors->collect_inventory_view(a,*s.combat);
            s.actors->publish_inventory_view(a,*s.combat);
        }
        const auto& registry=c.registries.at(a);
        if(!registry){
            SecondaryMotionRegistry none;
            none.find=[a](std::string_view name)->std::optional<std::uint32_t>{
                fail("actor "+std::to_string(a)+" has no motion registry for \""+std::string(name)+"\"");};
            none.entry=[a](std::uint32_t)->std::optional<SecondaryMotionEntry>{fail("actor "+std::to_string(a)+" has no motion registry");};
            (void)run_secondary_playback(playbacks.at(a),s.actors->secondary(a),step,first,dt,none);
            if(!first)looks.at(a).known=false;
            return;
        }
        if(!registry->keys){
            (void)run_secondary_playback(playbacks.at(a),s.actors->secondary(a),step,first,dt,*registry);
            if(!first)looks.at(a).known=false;
            return;
        }
        auto& p=playbacks.at(a);auto& component=s.actors->secondary(a);
        install_secondary_motion_slots(p,component,step.snapped,step.changed,first,*registry);   // 001d7480
        if(first)return;                                                                          // 001cdd04
        upper_body(a,p,component,*registry,dt);
        (void)advance_secondary_motion_slots(p,component,dt);                                     // 001d6120's timing tail
    }

    // 001ccb18..001ce7c0 of actor a (upper_body_pass.hpp) on the model's
    // nodes after its step, the aim half's stack vectors and the records
    // 001d7480 installed. The sampling half of 001d6120 runs before its
    // timing tail; the rest of the pass reads neither the records nor +312,
    // and the tail reads nothing the pass writes, so the tail runs after it.
    // Reported rows go to the nodes (vt +38 / +3c); +1c0 stays known where
    // reported. An unreported frame writes nothing and forgets +1c0. The look
    // record +410 takes +5 / +10 from the actor's aim state, the component
    // matrix context +28 and node 14's skeleton matrix (001dd1c0).
    void upper_body(std::size_t a,SecondaryPlayback& p,const SecondaryComponent& component,const SecondaryMotionRegistry& registry,float dt){
        auto& actor=aw().actor(a);
        const auto model=actor.upper_body_nodes();
        auto& known=composed.at(a);
        UpperBodyPassInput in;
        in.state_a=component.states.current_310;in.state_b=component.states.next_311;in.weight_318=component.states.blend_318;
        in.parent_bytes=model.parent_bytes;in.nodes=model.nodes;
        for(std::size_t k=0;k<upper_body_entry_count;++k)if(known[k]){in.nodes[k].composed=*known[k];in.composed_known[k]=true;}
        in.aim=s.actors->last_aim_result(a);
        auto& aim=s.actors->secondary_aim(a);
        in.component_250=aim.curve_230.current;in.weight_308=aim.weight_308;
        in.component_480=component.grip_rotation_480;in.component_490=component.grip_offset_490;
        std::array<std::shared_ptr<const SecondaryClipKeys>,2> keys;
        const auto slot=[&](const SecondaryMotionSlot& m,std::shared_ptr<const SecondaryClipKeys>& k){
            UpperBodySampleSlot u;
            if(!m.clip)return u;
            u.present=true;u.bone_count=model.bone_count;
            u.cursor_10=m.cursor_10;u.fraction_14=m.fraction_14;u.key_18=m.key_18;u.next_key_1c=m.next_key_1c;
            k=registry.keys(m.clip->source_id);
            if(k){
                u.root_translation_14=k->root_translation_14;u.root_yaw_18=k->root_yaw_18;
                u.translation_mask_1c=k->translation_mask_1c;u.rotation_mask_20=k->rotation_mask_20;u.keys=k->words;
            }
            return u;
        };
        in.a=slot(p.current_e0,keys[0]);in.b=slot(p.next_110,keys[1]);
        auto& look=looks.at(a);
        const auto& head=s.actors->aim_state(a);
        in.look_410=look.record;in.look_known=look.known;
        in.look_410.active=head.head_active_415!=0;in.look_410.point=head.head_point_420;
        const auto placement=root_placement_matrix(actor.component_placement());
        for(std::size_t k=0;k<16;++k)in.component[k]=placement[k];
        const auto skeleton=actor.skeleton_node_matrices();
        const auto origin=static_cast<std::size_t>(in.look_410.nodes[0]);
        if(origin>=skeleton.size())fail("actor "+std::to_string(a)+" has no node "+std::to_string(origin)+" for 001dd1c0");
        for(std::size_t k=0;k<4;++k)in.look_origin[k]=skeleton[origin][12+k];
        in.dt=dt;
        // 001ccb60..001cdbc4: the physical component's (actor +3310) contacts
        // +40 / +70, nodes 11 / 7 / 16 / 12 +10 and nodes 16 / 12 +90.
        const auto& physical=actor.physical().response.physical;
        in.legs.contact_40={physical.contact_40,physical.contact_50,physical.contact_60};
        in.legs.contact_70={physical.contact_70,physical.contact_80,physical.contact_90};
        if(skeleton.size()<upper_body_entry_count)fail("actor "+std::to_string(a)+" has fewer than 21 nodes for 001ccb60");
        in.legs.node_11=skeleton[11];in.legs.node_7=skeleton[7];in.legs.node_16=skeleton[16];in.legs.node_12=skeleton[12];
        in.legs.translation_16=model.translation_90[16];in.legs.translation_12=model.translation_90[12];
        // 001b574c..001b5794: component +8 = 2 for the local player, else
        // the constructor's 1.
        in.component_8=actor.byte_14()!=0&&actor.player_controller()?2:1;
        const auto r=run_upper_body_pass(in);
        CombatThrowFrame::UpperBodyWrite w;w.actor=a;w.reported=r.reported;w.failure=r.failure;
        look.known=r.look_410.has_value();
        if(r.look_410)look.record=*r.look_410;
        w.look=r.look_410.has_value();
        if(!r.reported){known.fill(std::nullopt);frame.upper_body.push_back(std::move(w));return;}
        const auto keep=[](SecondaryMotionSlot& m,const UpperBodySampleSlot& u){
            if(!m.clip)return;
            m.cursor_10=u.cursor_10;m.key_18=u.key_18;m.next_key_1c=u.next_key_1c;
        };
        keep(p.current_e0,r.a);keep(p.next_110,r.b);
        aim.weight_308=r.weight_308;
        known=r.composed;
        actor.write_upper_body_nodes(r.rows,r.root_offset);                                       // 001ce7c0
        for(const auto& row:r.rows)if(row)++w.rows;
        w.hand=r.rows[hand_node].has_value();w.root=r.root_offset.has_value();
        frame.upper_body.push_back(std::move(w));
    }

    // The receiver's inventory hosting for actor a.
    ActorAnimationReceiverHost receiver_host(std::size_t a){
        ActorAnimationReceiverHost h;
        // Checkpoint 132fx3: on CombatWorld's attach table (the one +2410 table; combat_world.hpp).
        h.set_model_slot=[this,a](std::uint64_t,std::size_t slot,const std::optional<ActorModelPart>& part,const ActorAnimationReceiverState&){
            if(part)fail("a non-null attached part (record +13c) for model slot "+std::to_string(slot));
            if(slot!=0)fail("model slot "+std::to_string(slot)+" set by the release");
            s.combat->clear_attach_point(a,slot);                                // 0038c7d0(+60, 0, null)
            frame.slot_writes.push_back(a);
        };
        // The original never bounds-checks +2410 + 4 x slot; the count only
        // lets the receiver reject a mapping outside the ten points.
        h.model_slot_count=[](std::uint64_t)->std::size_t{return actor_model_slot_count;};
        h.model_slot_present=[this,a](std::uint64_t,std::size_t slot)->bool{
            if(slot>=actor_model_slot_count)fail("model slot "+std::to_string(slot)+" outside the ten attach points");
            return s.combat->attach_table(a).at(slot)!=0;
        };
        h.swap_model_slots=[this,a](std::uint64_t,std::size_t first,std::size_t second,const ActorAnimationReceiverState&){
            s.combat->swap_attach_points(a,first,second);                        // 0038c790
        };
        // 00142c50 (00142cb8..00142d00): the owner is inventory +88, its
        // component +58 (always present on a solo actor: AI or player), the
        // component's virtual +54 and the actor byte +14.
        h.inventory_owner=[this,a](std::uint64_t)->ActorAnimationInventoryOwner{
            const auto& actor=aw().actor(a);
            return ActorAnimationInventoryOwner{static_cast<std::uint64_t>(static_cast<std::uint32_t>(s.object_ids.at(a))),
                actor.player_controller(),actor.byte_14()!=0};
        };
        // 00141d94..00141de0: cOutOfAmmoMsg (004ea150, +24 the thrown item)
        // to the player controller, transport 0: the actor's queue.
        h.enqueue_inventory_empty=[this,a](std::uint64_t,std::int32_t item,const ActorAnimationReceiverState&){
            aw().actor(a).queue_out_of_ammo(item);};
        // 00142d08..00142d78: MSGGAME_PLAYSND from descriptor 004f1950 at the
        // actor's virtual +c0 position, sent to the sound manager 004ffb50
        // (transport 0); recorded, as the other play sounds.
        h.enqueue_inventory_selection_sound=[this,a](std::uint64_t,const ActorAnimationReceiverState&){
            frame.effect_calls.push_back({a,"selection_sound",0,0x004f1950U});};
        // Tokens 0xa0 / 0xa1: 00141e20 on the candidate's fields, the view
        // collected from and republished into the candidate.
        h.launch_inventory_projectile=[this,a](float arc,ActorAnimationReceiverState& state,const MotionDeliveryView&){
            auto& inv=s.combat->inventory(a);
            read_inventory_view(state,inv);
            launch(a,arc,state);
            write_inventory_view(inv,state);
        };
        h.null_target_key=[this]{return *c.none_word;};
        // Checkpoint 132ms4: the takedown tokens 0x98 (knife, 001a3fd8) and 0x99 (unarmed, 001a40a0).
        // 0x98: 0040c540(&+33cc) (0: the token does nothing more), lw of *00236f20(0x19), 00132a30 / 00133ed0
        // on the target with the node byte *0049d023 (9; the receiver config's target_transform_selector),
        // 00232180(id, matrix +30, matrix row +10) unless the id is negative; then both tokens queue
        // 0020f990(preset, actor) to 004ffb50 (003c9420, transport 0): 004f24b0 (0x98) / 004f1e30 (0x99).
        h.resolve_named_target=[this](std::uint32_t word)->std::uint64_t{return s.find(static_cast<std::int32_t>(word));};
        if(c.effect_descriptor)h.effect_descriptor=c.effect_descriptor;
        h.target_transform=[this](std::uint64_t o,std::int8_t node){
            if(!actor_of(o))fail("token 0x98: 00133ed0 of a target that is not an actor of the world");
            const auto m=aw().actor(index(o)).skeleton_node_world_matrix(node);
            std::array<float,16> out{};
            for(std::size_t r=0;r<4;++r)for(std::size_t k=0;k<4;++k)out[4*r+k]=m[r][k];
            return out;
        };
        if(c.directional_effect)h.emit_directional_effect=[this,a](std::int32_t id,RootVector point,RootVector direction,ActorAnimationReceiverState&,const MotionDeliveryView&){
            c.directional_effect(a,id,point,direction);};
        h.sound_preset=[](std::int32_t token){
            if(token!=0x98&&token!=0x99)fail("a queued token sound preset other than 0x98 / 0x99");
            const auto r=static_sound_record(static_sound_descriptor(token==0x98?0x004f24b0U:0x004f1e30U));
            return ActorAnimationSoundPreset{r.type_0,r.name_10,static_cast<std::uint32_t>(r.priority_180),r.subtitle_186};
        };
        return h;
    }
};

const CombatThrowConfig& CombatThrows::config() const{return impl_->c;}
CombatThrows::CombatThrows(CombatThrowSession session,CombatThrowConfig config)
    :impl_(std::make_unique<Impl>(std::move(session),std::move(config))){
    auto& m=*impl_;
    if(!m.s.world||!m.s.actors||!m.s.combat||!m.s.database||!m.s.registered||!m.s.find)fail("the session's world, adapter, combat world, database, registrations and lookup are required");
    const auto n=m.s.world->size();
    if(m.s.object_ids.size()!=n||m.c.registries.size()!=n)fail("one object ID and one motion registry entry per actor");
    if(!m.c.tuning)fail("*004f7fe0 / *004f7fe8 (ThrownProjectileTuning) are not supplied");
    if(!m.c.gravity)fail("the gravity vector 004f5430 is not supplied");
    if(!m.c.none_word)fail("the none word *004bdd00 is not supplied");
    m.playbacks.assign(n,SecondaryPlayback{});
    m.composed.assign(n,zero_composed());
    m.looks.assign(n,Impl::Look{});
    m.senses.assign(n,ActorSenses{});
    m.cameras.assign(n,std::nullopt);
    m.stuns.assign(n,ActorStunState{});                       // +33bc = -1 (the spawn reset 001b61d8)
    m.effects.assign(n,ActorEffectSlotState{});               // the spawn constructor's 0 / -2 slots
    m.ignitions.assign(n,ActorIgnitionState{});
    m.glow.assign(n,std::int16_t{0});

    Impl* self=impl_.get();
    ThrowWorldHost h;
    h.los=[self](const RootVector& o,const RootVector& d){return projectile_world_los(self->aw().collision(),o,d);};
    h.scan=[self](BulletHitRequest& q){
        std::vector<CombatEvent> hits;
        const auto pool=self->s.combat->scan_host(hits);
        std::vector<BulletEvent> events;
        scan_bullet_candidates(q,0,pool,events);
        for(auto& e:events)self->frame.scan_events.push_back(std::move(e));
        for(auto& e:hits)self->frame.hit_events.push_back(std::move(e));
    };
    h.valid=[self](const ObjectWeakHandle& o){return self->resolves(o);};
    // Checkpoint 132ia1: the hold-7 hit's object virtual +100 (0025b19c..0025b2e4: the LOS result's
    // source owner, category 1, record +bc, the band damage), the same call as the bullet's static damage
    // (bullet_pool.cpp): a terrain object's 003ccb00 changes nothing, a population object takes its own.
    h.static_damage=[self](std::uint64_t o,std::uint64_t owner,std::int8_t type,float amount){
        self->s.actors->static_damage(o,owner,static_cast<std::uint8_t>(type),amount);};
    h.effect_id=[self](std::int32_t index){return self->s.actors->effect_id(index);};
    h.explode=[self](const ProjectileBlastCall& call){
        if(!self->c.explode)fail("the explosion 0021a200 of a thrown item is not hosted");
        std::optional<BlastSource> owner;
        if(call.owner.object){
            auto& actor=self->aw().actor(self->index(call.owner.object));
            owner=BlastSource{call.owner,actor.byte_14()!=0,actor.published_position()};
        }
        self->c.explode(projectile_blast_request(call,owner));
    };
    ThrowWorldConfig wc;
    wc.tuning=*m.c.tuning;wc.gravity=*m.c.gravity;wc.first_object=m.c.first_object;wc.first_id=m.c.first_id;
    // Port assumption (Checkpoint 132ia1): the hold-7 actor scan's request +d (0025b34c..0025b518 builds the
    // request at sp+df0 without writing +d) is stale stack on the EE; it only selects the hit's aim-view
    // statistic. The port passes 0 (no aim view).
    wc.scan_aim_view_byte=false;
    m.world=std::make_unique<ThrowWorld>(*m.s.database,m,std::move(h),wc);
    m.bindings.slots_70=slots_00489d70;m.bindings.slots_78=slots_00489d78;
    m.bindings.definitions.assign(m.s.database->size(),std::nullopt);     // record +13c: no part is loaded natively
    for(std::size_t a=0;a<n;++a)m.hook(a);
    m.s.actors->set_throw_start([self](std::size_t a,std::int32_t cls,const ActorFireRequest& r){self->start(a,cls,r);});
    m.s.actors->set_melee_attack([self](std::size_t a){self->melee(a);});
    m.s.actors->set_secondary_playback([self](std::size_t a,const SecondaryStep& step,bool first,float dt){self->playback(a,step,first,dt);});
}

CombatThrows::~CombatThrows(){
    auto& m=*impl_;
    m.s.actors->set_throw_start({});
    m.s.actors->set_melee_attack({});
    m.s.actors->set_secondary_playback({});
    for(std::size_t a=0;a<m.aw().size();++a){
        m.aw().actor(a).set_logic_item_hooks({},{});
        m.aw().actor(a).set_logic_grip_hook({});
        m.aw().actor(a).set_fire_hooks({});
        m.aw().actor(a).set_inventory_receiver(std::nullopt,{});
    }
}

void CombatThrows::append_actor(std::int32_t object_id,std::optional<SecondaryMotionRegistry> registry){
    auto& m=*impl_;
    const auto a=m.playbacks.size();
    if(m.aw().size()!=a+1)fail("append one actor right after the world appends it");
    m.s.object_ids.push_back(object_id);m.c.registries.push_back(std::move(registry));
    m.playbacks.emplace_back();m.composed.push_back(zero_composed());m.looks.emplace_back();m.senses.emplace_back();
    m.cameras.emplace_back(std::nullopt);m.stuns.emplace_back();   // +33bc = -1 (the spawn reset 001b61d8)
    m.effects.emplace_back();m.ignitions.emplace_back();             // the constructor's 0 / -2 slots
    m.glow.emplace_back(std::int16_t{0});

    m.hook(a);
}
void CombatThrows::reset_actor(std::size_t a,std::optional<SecondaryMotionRegistry> registry){
    auto& m=*impl_;
    if(a>=m.playbacks.size())fail("reset of an unknown actor");
    m.c.registries[a]=std::move(registry);
    m.playbacks[a]={};m.composed[a]=zero_composed();m.looks[a]={};m.senses[a]={};
    m.cameras[a]=std::nullopt;m.stuns[a]={};
    m.effects[a]={};m.ignitions[a]={};
    m.glow[a]=std::int16_t{0};

    m.hook(a);
}

void CombatThrows::update(float dt){
    auto step=impl_->world->update(dt);
    auto& fw=impl_->frame.world;
    const auto first=fw.area_events.size();
    if(fw.projectile_events.empty()&&fw.area_events.empty()&&fw.areas_done.empty()&&fw.areas_created.empty())fw=std::move(step);
    else{   // a reset's events (finish_areas) or a bullet's area (bullet_area) taken before this update stay first
        fw.projectile_events.insert(fw.projectile_events.end(),step.projectile_events.begin(),step.projectile_events.end());
        fw.area_events.insert(fw.area_events.end(),step.area_events.begin(),step.area_events.end());
        fw.detonated.insert(fw.detonated.end(),step.detonated.begin(),step.detonated.end());
        fw.areas_created.insert(fw.areas_created.end(),step.areas_created.begin(),step.areas_created.end());
        fw.areas_done.insert(fw.areas_done.end(),step.areas_done.begin(),step.areas_done.end());
    }
    // Checkpoint 132f12: the gas statistics 00333f10(001314f0(004ffb50, -1), id, 1) of 00259e74 (6, knockout
    // gas) and 00259f9c (4, lethal gas) go into the local agent record, in the area update's order.
    for(std::size_t k=first;k<fw.area_events.size();++k){
        const auto& e=fw.area_events[k].second;
        if(e.kind!=AreaEvent::Kind::statistic)continue;
        if(!impl_->s.statistics)fail("the gas statistic 00333f10 has no agent statistics (CombatThrowSession::statistics)");
        add_agent_statistic(*impl_->s.statistics,static_cast<std::uint32_t>(e.value),1);
    }
    impl_->update_objects(dt);
}
void CombatThrows::finish_areas(){
    auto done=impl_->world->finish_areas();
    auto& fw=impl_->frame.world;
    fw.area_events.insert(fw.area_events.end(),done.area_events.begin(),done.area_events.end());
    fw.areas_done.insert(fw.areas_done.end(),done.areas_done.begin(),done.areas_done.end());
}
void CombatThrows::bullet_area(const BulletAreaCall& call){
    impl_->world->add_area(call.owner,call.point,call.record,call.flag,impl_->frame.world);
}
void CombatThrows::launch_discharge(const ObjectWeakHandle& owner,const std::array<RootVector,4>& frame,std::int32_t item,
    const RootVector& point,float arc){
    impl_->world->launch_discharge(owner,frame,item,point,arc);
}
void CombatThrows::bullet_blast(const BulletBlastCall& call,const BulletObjectShooter* object){
    auto& m=*impl_;
    if(!m.c.explode)fail("the explosion 0021a200 of a bullet is not hosted");
    BlastRequest r;
    if(object)r.source=BlastSource{call.source,object->owned_14,object->position_e4};
    else if(call.source.object){
        auto& actor=m.aw().actor(m.index(call.source.object));
        r.source=BlastSource{call.source,actor.byte_14()!=0,actor.published_position()};
    }
    r.damage=call.damage;r.kind=call.kind;r.point=call.point;r.skip_humans=call.skip_humans;
    r.strength=call.strength;r.inner=call.inner;r.outer=call.outer;
    m.c.explode(r);   // t1 (local) 1: single player sends no network message
}
void CombatThrows::effect_off(std::size_t actor,std::uint32_t kind){
    auto& m=*impl_;
    if(actor>=m.effects.size()||kind>2)fail("00185d40: no effect slot "+std::to_string(kind)+" of actor "+std::to_string(actor));
    m.set_effect(actor,kind,0,1.0F);
}
void CombatThrows::stop_stun_sound(std::size_t actor){
    auto& m=*impl_;
    if(actor>=m.stuns.size())fail("0020eba0: no stun of actor "+std::to_string(actor));
    m.frame.stun_sounds.push_back({actor,0x004f12d0U,false});
}
bool CombatThrows::air_taser_fire(std::size_t actor,std::uint64_t target,bool aim_view,std::int32_t subtarget){
    return impl_->taser_fire(actor,target,aim_view,subtarget);
}
void CombatThrows::air_taser_update(std::size_t actor,float dt){impl_->taser_update(actor,dt);}
void CombatThrows::switch_ir(std::size_t actor,std::uint8_t ir){impl_->switch_ir(actor,ir);}
std::uint64_t CombatThrows::smoke_blocker(const RootVector& a,const RootVector& b) const{
    if(!impl_->world)return 0;
    const auto* db=impl_->s.database;
    return area_smoke_blocker(impl_->world->areas(),[db](std::int32_t id)->const WeaponRecord&{return db->record(id);},a,b);
}
std::optional<NpcProjectileView> CombatThrows::projectile_view(std::uint64_t object) const{
    if(!impl_->world)return std::nullopt;
    for(const auto& item:impl_->world->projectiles()){
        const auto& p=item.projectile;
        if(p.object!=object)continue;
        NpcProjectileView v;
        v.id=p.id_c;v.root={1,1,static_cast<std::uint8_t>(p.motion.retired?1:0)};
        v.owner=impl_->resolves(p.owner_124)?p.owner_124.object:0;
        v.damage_type=impl_->s.database->record(p.record_120).damage_type_bc();
        v.point_bc=p.node[3];
        return v;
    }
    return std::nullopt;
}
void CombatThrows::air_taser_release(std::size_t actor){impl_->taser_release(actor);}

bool CombatThrows::shock_to_death(std::size_t actor){
    auto& m=*impl_;
    if(actor>=m.stuns.size())fail("ShockToDeath 00185280: no stun of actor "+std::to_string(actor));
    const auto life=m.aw().actor(actor).life_state();                           // virtual +108 & 0xff
    if(life==2||life==3)return false;
    if(!(m.stuns[actor].elapsed_33bc<0.0f))return false;                        // c.olt.s +33bc, 0
    start_actor_stun(m.stuns[actor],1,1,shock_to_death_duration,stun_lethal_damage,m.stun_host(actor));   // 00184ce0, f13 = 0x461c4000
    return true;
}
void CombatThrows::add_level_fire_zone(const FireZone& zone){
    auto& m=*impl_;
    const auto key=zone.key_0c;
    if(key==0||m.effect_objects.count(key))fail("FIR: zone "+std::to_string(key)+" is already an object of the world");
    if(key>=0x00e00001U)fail("FIR: zone key "+std::to_string(key)+" collides with the session's effect object keys");
    Impl::EffectObject o{key};
    o.zone=zone;o.level=true;
    o.active=zone.byte_2d!=0;
    o.registered=true;
    m.effect_objects[key]=std::move(o);
    m.registration.push_back(key);                                          // 0040c9d0 (0040b790 appends)
}
bool CombatThrows::set_level_fire_zone_present(std::uint32_t key,bool present){
    const auto at=impl_->effect_objects.find(key);
    if(at==impl_->effect_objects.end()||!at->second.level)return false;
    set_fire_zone_present(*at->second.zone,present);                       // 00224330 / 00224180
    at->second.active=present;
    return true;
}
bool CombatThrows::set_level_fire_zone_enabled(std::uint32_t key,bool enabled){
    const auto at=impl_->effect_objects.find(key);
    if(at==impl_->effect_objects.end()||!at->second.level)return false;
    set_fire_zone_enabled(*at->second.zone,enabled);                       // 00261c00 / 00261c90 in solo
    return true;
}
bool CombatThrows::level_fire_zone(std::uint64_t key) const{
    if(key>0xffffffffU)return false;
    const auto at=impl_->effect_objects.find(static_cast<std::uint32_t>(key));
    return at!=impl_->effect_objects.end()&&at->second.level;
}
const FireZone* CombatThrows::level_fire_zone_state(std::uint32_t key) const{
    const auto at=impl_->effect_objects.find(key);
    if(at==impl_->effect_objects.end()||!at->second.level)return nullptr;
    return &*at->second.zone;
}
bool CombatThrows::actor_fire_zone(std::uint64_t key) const{
    if(key>0xffffffffU)return false;
    const auto at=impl_->effect_objects.find(static_cast<std::uint32_t>(key));
    return at!=impl_->effect_objects.end()&&at->second.zone&&!at->second.level;
}
CombatThrowFrame CombatThrows::take_frame(){auto out=std::move(impl_->frame);impl_->frame=CombatThrowFrame{};return out;}
const ThrowWorld& CombatThrows::world() const{return *impl_->world;}
const SecondaryPlayback& CombatThrows::playback(std::size_t actor) const{return impl_->playbacks.at(actor);}
const ActorSenses& CombatThrows::senses(std::size_t actor) const{return impl_->senses.at(actor);}
const ActorStunState& CombatThrows::stun(std::size_t actor) const{return impl_->stuns.at(actor);}
const ActorEffectSlotState& CombatThrows::effect_slots(std::size_t actor) const{return impl_->effects.at(actor);}
const ActorIgnitionState& CombatThrows::ignition(std::size_t actor) const{return impl_->ignitions.at(actor);}
std::int16_t CombatThrows::glow_2df6(std::size_t actor) const{return impl_->glow.at(actor);}

const FireEffectObject* CombatThrows::fire_effect(std::uint32_t object) const{
    const auto at=impl_->effect_objects.find(object);
    return at==impl_->effect_objects.end()||!at->second.effect?nullptr:&*at->second.effect;
}
const FireZone* CombatThrows::fire_zone(std::uint32_t object) const{
    const auto at=impl_->effect_objects.find(object);
    return at==impl_->effect_objects.end()||!at->second.zone?nullptr:&*at->second.zone;
}
const std::vector<std::uint32_t>& CombatThrows::registered_objects() const{return impl_->registration;}
const std::map<std::uint32_t,CombatThrows::FireEmitter>& CombatThrows::emitters() const{return impl_->emitters;}
std::vector<CombatThrows::BodyParticle> CombatThrows::body_particles() const{
    const auto& m=*impl_;
    std::vector<BodyParticle> out;
    for(const auto key:m.registration){
        const auto o=m.effect_objects.find(key);
        if(o==m.effect_objects.end()||!o->second.effect||!o->second.active||o->second.released)continue;
        for(const auto& st:o->second.effect->states_7c){
            if(!st.emitter_0)continue;
            const auto e=m.emitters.find(st.emitter_0);
            if(e!=m.emitters.end()&&e->second.particle)out.push_back({o->second.actor,e->second.particle,st.previous_10});
        }
    }
    return out;
}
std::uint64_t CombatThrows::taser_particle(std::uint64_t effect) const{
    const auto at=impl_->taser_particles.find(effect);
    return at==impl_->taser_particles.end()?0:at->second;
}
std::optional<AwarenessObjectView> CombatThrows::object_view(std::uint64_t object) const{
    const auto at=object<=0xffffffffU?impl_->effect_objects.find(static_cast<std::uint32_t>(object)):impl_->effect_objects.end();
    if(at==impl_->effect_objects.end())return std::nullopt;
    // 003cf0e0's fields: +2c 1 (a FIR: zone's follows Enable / Disable), +2d (the activity), +2e 0, +4c the object
    // class (not the actor's), +108 00141850 (3: +40 is 0), +e8 row 3 (0, 0, 0, 1; a FIR: zone's +50 is
    // 003cdff0's transform, the identity 0055c9f0, and 003cd260 answers its row 3).
    AwarenessObjectView v;
    const auto& zone=at->second.zone;
    v.object=object;v.id=static_cast<std::int32_t>(at->second.key);v.life_108=3;v.actor_class=false;
    v.root={static_cast<std::uint8_t>(zone?zone->byte_2c:1),static_cast<std::uint8_t>(at->second.active),
            static_cast<std::uint8_t>(zone?zone->byte_2e:0)};v.flag_3468=0;v.position={0,0,0,1};
    return v;
}
std::optional<CombatThrows::ActiveTakedownCamera> CombatThrows::takedown_camera(std::size_t actor) const{
    const auto& camera=impl_->cameras.at(actor);
    if(!camera)return std::nullopt;
    const auto state=impl_->aw().actor(actor).hosted_world_object(takedown_camera_object);
    if(!state||!state->byte_2d)return std::nullopt;
    return ActiveTakedownCamera{*camera,make_camera_rig(takedown_camera_fov).half_fov_60};   // 0017f9f0: +60 = fov / 2
}

}
