#include "actor_damage.hpp"
#include "vu_float.hpp"
#include "water_effects.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
float bits(std::uint32_t v){return std::bit_cast<float>(v);}
float check(float v){
    if(!std::isfinite(v))throw std::runtime_error("Actor damage: nonfinite arithmetic");
    return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;
}
float add(float a,float b){return check(a+b);}
float sub(float a,float b){return check(a-b);}
float mul(float a,float b){return check(a*b);}
float div(float a,float b){return check(vu::div(a,b));}
// cvt.w.s (truncation); values outside the integer range are not native.
std::int32_t trunc_word(float v){
    check(v);
    if(!(std::fabs(v)<2147483648.0F))throw std::runtime_error("Actor damage: value outside the integer range");
    return static_cast<std::int32_t>(v);
}
// cvt.s.w; exact below 2^24 (larger magnitudes are not native).
float word_float(std::int32_t v){
    if(v>(1<<24)||v<-(1<<24))throw std::runtime_error("Actor damage: integer not exactly representable as a float");
    return static_cast<float>(v);
}
constexpr std::int32_t armor_resistance=10;   // 0048a218 ArmorPenetrationResistance
constexpr std::int32_t body_resistance=4;     // 0048a220 BodyPenetrationResistance
constexpr std::int32_t head_resistance=2;     // 0048a228 HeadPenetrationResistance
constexpr std::int32_t fire_to_ignite=20;     // 0048a1f8 FireDmgToIgnite
constexpr std::int32_t type7_death_code=1;    // 0048a2c0

std::uint8_t life(const ActorHealthState& s,const ActorDamageHost& h){
    if(!h.top_action)throw std::runtime_error("Actor damage: top action host required");
    return actor_life_state(s,h.top_action());
}
bool dying(std::uint8_t l){return l==2||l==3;}
ActorDamageEvent event(ActorDamageEvent::Kind k){ActorDamageEvent e;e.kind=k;return e;}
// 0020f990(msg, desc, actor), +20 = 1, 003c9420(msg, 004ffb50, transport): the death voice posts (00186e84,
// transport 0), the hit voice and the hit deliver at once (00187f60 / 00187fbc, transport 1).
ActorDamageEvent sound(ActorDamageSound d,bool immediate){
    auto e=event(ActorDamageEvent::Kind::sound);e.value=static_cast<std::int32_t>(d);e.immediate=immediate;return e;}
RootVector position_of(const ActorDamageHost& h,std::uint64_t object){
    if(!h.position_e4)throw std::runtime_error("Actor damage: object position (virtual +e4) is not hosted");
    return h.position_e4(object);
}
// 003ccb00: returns life state == 3. In a network game (005721c8) the MSGLIB_DAMAGE_REQUEST takes +20 = 2.
bool base_damage(ActorHealthState& s,const ActorDamageInput& in,std::uint64_t source,std::uint8_t type,
    float amount,const ActorDamageHost& h,std::vector<ActorDamageEvent>& out){
    if(source==0)return false;                                         // 003ccb40
    if(check(s.health_40)<=0.0F){                                      // 003ccb5c (c.ole.s)
        if(in.network&&!in.host_console&&!in.controller&&!in.quiet){   // 003ccd28: a client's actor without +58
            auto m=event(ActorDamageEvent::Kind::damage_message);
            m.value=trunc_word(amount);m.object=source;m.type_2c=type;m.category_2d=in.category;m.byte_2e=0;m.flags_20=2;
            out.push_back(m);                                          // 003ccdb8, transport 0
        }
        return life(s,h)==3;
    }
    // 003ccb68: a network game's controller whose virtual +5c answers (the remote NPC 0x004de2a0) keeps +40.
    if(!(in.network&&in.controller&&in.controller->virtual_5c))
        s.health_40=sub(s.health_40,amount);                           // 003ccbac
    const std::int32_t whole=trunc_word(amount);                       // 003ccbc8
    auto m=event(ActorDamageEvent::Kind::damage_message);
    m.value=whole;m.object=source;m.type_2c=type;m.category_2d=in.category;m.byte_2e=1;m.flags_20=5;
    if(!in.network)out.push_back(m);                                   // 003ccc90, transport 0
    else if(!in.controller||in.byte_14){                               // 003ccc0c..003ccc78
        m.flags_20=2;
        if(!in.controller&&!in.quiet)m.byte_2e=0;
        out.push_back(m);
    }
    if(trunc_word(s.health_40)<=0){                                    // 003ccca8
        s.health_40=0.0F;
        ActorKillInput k;k.actor=in.actor;k.controller=in.controller;k.byte_14=in.byte_14;k.amount=whole;
        k.source=source;k.category=in.category;k.type=type;k.hit_point=in.hit_point;
        kill_actor(s,k,h,out);                                         // virtual +10c
    }else if(in.controller&&source!=in.actor){                          // 003ccce0
        auto e=event(ActorDamageEvent::Kind::controller_hit);e.object=source;e.value=whole;out.push_back(e);
    }
    return life(s,h)==3;
}
}

ActorHealthState make_actor_health(std::uint32_t health_word,std::uint32_t armor_word){
    ActorHealthState s;
    s.max_health_3398=static_cast<std::int32_t>(health_word);
    s.health_40=word_float(s.max_health_3398);
    s.max_armor_339c=s.armor_33a0=static_cast<std::int32_t>(armor_word);
    return s;
}

void apply_player_role_armor(ActorHealthState& s,std::int32_t starting_armor){
    s.max_armor_339c=starting_armor;s.armor_33a0=starting_armor;           // 001ae628..001ae634
}

std::uint8_t actor_life_state(const ActorHealthState& s,std::int32_t top){
    if(s.killed_33a5||top==0x1f)return 3;
    if(top==0x1b)return 2;
    return trunc_word(s.health_40)==0?3:0;                             // 0018733c movz
}

void kill_actor(ActorHealthState& s,const ActorKillInput& in,const ActorDamageHost& h,std::vector<ActorDamageEvent>& out){
    if(s.invulnerable_33a4){                                            // 00186c30
        s.health_40=word_float(s.max_health_3398);                      // 003cc990(actor, +3398, 0)
        auto r=event(ActorDamageEvent::Kind::refill);r.value=s.max_health_3398;out.push_back(r);
        out.push_back(event(ActorDamageEvent::Kind::release_60));      // 0038cbf0(actor + 0x60)
    }else if(in.byte_14){                                               // 00186c58
        out.push_back(event(ActorDamageEvent::Kind::item_notice));     // 00143770(+3524)
        std::optional<ActorDamageSound> voice=s.gender_3260==1?ActorDamageSound::voice_death_f:ActorDamageSound::voice_death_m;
        std::uint8_t code=0;
        if(in.type==0xb){code=8;voice.reset();}
        else if(in.type==0xc)code=9;
        else if(in.type==0xe)code=10;
        else if(in.type==0xf)code=11;
        else{
            // 00186cf8 reads the source's virtual +e4 here; only the hit-direction classifier below consumes it
            // (a script Kill, category 2 type 0xa, of an actor without a frame yet has no published +e4).
            const auto point=[&]{return in.hit_point?*in.hit_point:position_of(h,in.source);};
            if(in.type==2||in.type==0xd||in.type==1){
                if(!h.hit_direction)throw std::runtime_error("Actor kill: hit-direction classifier 00186950 is not hosted");
                code=static_cast<std::uint8_t>(h.hit_direction(point(),in.type==1?6:4));
            }else if(in.category==2){code=1;voice.reset();}
        }
        if(in.controller){                                              // 00186d88: +84
            if(!h.adjust_death_code)throw std::runtime_error("Actor kill: controller virtual +84 is not hosted");
            code=h.adjust_death_code(code);
            if(code==2&&in.category!=2&&in.controller->virtual_58)
                out.push_back(event(ActorDamageEvent::Kind::ai_flag_1d6));
        }
        auto d=event(ActorDamageEvent::Kind::death_message);d.value=code;d.immediate=true;out.push_back(d);
        if(!h.death_action)throw std::runtime_error("Actor kill: the death action (action 0x42, transport 1) is not hosted");
        h.death_action(code);                                           // 003c9420 transport 1
        if(voice)out.push_back(sound(*voice,false));
        if(in.controller){                                              // 003ccac0: +7c(amount, source, category, type)
            auto n=event(ActorDamageEvent::Kind::controller_death);
            n.value=in.amount;n.object=in.source;n.category_2d=in.category;n.type_2c=in.type;
            out.push_back(n);
        }
    }
    s.killed_33a5=false;                                                // 00186ef8
}

ActorDamageResult apply_actor_damage(ActorHealthState& s,const ActorDamageInput& in,const ActorDamageHost& h,
    std::vector<ActorDamageEvent>& out){
    ActorDamageResult result;
    std::uint64_t source=in.source.id;
    std::optional<ActorDamageController> source_controller=in.source.controller;
    std::uint8_t type=in.type;
    // 001873bc: type 4 with a valid fire handle, unless the source is a player.
    if(type==4&&in.fire_setter_3480){
        const bool player=source!=0&&source_controller&&source_controller->player_54;
        if(!player){source=in.fire_setter_3480->id;source_controller=in.fire_setter_3480->controller;}
    }
    if(source==0||s.invulnerable_33a4||(type==4&&s.fire_immune_33b8)){  // 001874a8
        result.penetration=in.penetration;return result;
    }
    if(type==2){                                                        // 001874e8: LOS Request
        RootVector from=position_of(h,source);
        if(in.hit_point)from=*in.hit_point;
        if(!h.position_c0)throw std::runtime_error("Actor damage: victim position (virtual +c0) is not hosted");
        const RootVector to=h.position_c0();
        auto q=event(ActorDamageEvent::Kind::los_request);
        q.direction={sub(to[0],from[0]),sub(to[1],from[1]),sub(to[2],from[2]),0.0F};   // vsub.xyzw, +4c = 0
        q.position=from;q.position[3]=1.0F;                             // +3c = 1.0
        out.push_back(q);
        if(!h.los_blocked)throw std::runtime_error("Actor damage: explosion LOS request (003caa50) is not hosted");
        if(h.los_blocked(q.position,q.direction)){result.penetration=in.penetration;return result;}
    }
    std::int32_t p=in.penetration?*in.penetration:0;                    // 00187698
    float f=0.0F;
    if(type==4)f=1.0F;
    else if(p>armor_resistance)f=div(word_float(p-armor_resistance),word_float(p));
    const float amount=check(in.amount);
    float armor_part=mul(sub(1.0F,f),amount);                           // f21
    float body=sub(amount,armor_part);                                  // f20
    const bool victim_player=in.controller&&in.controller->player_54;
    std::optional<ActorDamageSound> hit=victim_player?ActorDamageSound::body_hit_player:ActorDamageSound::body_hit_npc;
    std::optional<ActorDamageSound> voice;
    const auto either=[](const std::optional<ActorDamageController>& c){return c&&(c->virtual_5c||c->virtual_58);};
    if(in.category==2){                                                 // 00187768
        if(type==7)type=0;
        body=either(source_controller)&&either(in.controller)?amount:bits(0x47c35000U);   // 100000
        p-=head_resistance;
    }else if(in.category==3){                                           // 00187884
        if(type==7&&s.armor_33a0>0){body=0.0F;type=0;armor_part=body;}
        if(s.armor_33a0!=0&&armor_part<=word_float(s.armor_33a0)){      // c.ole.s
            p-=armor_resistance;
            if(type!=9)s.armor_33a0=trunc_word(sub(word_float(s.armor_33a0),armor_part));
            hit=ActorDamageSound::body_hit_kevlar;
            const bool source_player=source_controller&&source_controller->player_54;
            if(source_player&&!victim_player)out.push_back(event(ActorDamageEvent::Kind::flak_notice));
        }else{
            if(armor_part!=0.0F)
                p=trunc_word(sub(word_float(p),mul(word_float(armor_resistance),div(word_float(s.armor_33a0),armor_part))));
            body=add(body,sub(armor_part,word_float(s.armor_33a0)));
            if(type!=9)s.armor_33a0=0;
        }
        p-=body_resistance;
    }else{body=amount;p-=body_resistance;}
    if(type==4){                                                        // 00187a68
        s.fire_damage_33b0=trunc_word(add(word_float(s.fire_damage_33b0),body));
        if(s.fire_damage_33b0>fire_to_ignite&&!s.burning_326a&&in.byte_14)
            out.push_back(event(ActorDamageEvent::Kind::ignite));
        hit.reset();
    }else if(type==0xb||type==0xa||type==5)hit.reset();
    const bool dead_before=dying(life(s,h));                            // 00187b00
    ActorDamageInput base=in;base.penetration=p;
    base_damage(s,base,source,type,body,h,out);                         // 00187b50
    if(!in.byte_14&&!in.quiet){                                         // 00187b58: replica message
        auto m=event(ActorDamageEvent::Kind::damage_message);
        m.value=trunc_word(amount);m.object=source;m.type_2c=type;m.category_2d=in.category;m.byte_2e=0;m.flags_20=8;
        if(!dying(life(s,h))&&word_float(trunc_word(s.health_40))<=amount)m.word_1c=0x40;
        out.push_back(m);
    }
    if(type==7&&!s.invulnerable_33a4){                                  // 00187c40
        s.killed_33a5=true;
        auto d=event(ActorDamageEvent::Kind::death_message);d.value=type7_death_code;out.push_back(d);
        if(in.byte_14){auto u=event(ActorDamageEvent::Kind::unconscious_message);u.object=source;out.push_back(u);}
    }
    if(!dead_before&&in.controller&&type!=4&&type!=0xb){                // 00187d48: reactions
        if(dying(life(s,h))&&type==7)s.killed_33a5=true;
        if(in.controller->player_54){
            voice=s.gender_3260==1?ActorDamageSound::voice_player_hit_f:ActorDamageSound::voice_player_hit_m;
            if(in.byte_14){
                const std::int32_t intensity=amount<50.0F?0x7f:0xe1;
                if(!h.random)throw std::runtime_error("Actor damage: the game RNG (00127dd0) is not hosted");
                const std::uint32_t r=h.random();
                if(r>0x7fffffffU)throw std::runtime_error("Actor damage: RNG value outside 0..2^31-1");
                const float scale=add(1.0F,bits(0x4f000000U));          // 1 + 2^31
                // cvt.s.w of the draw (game_int_to_float: toward zero, as the
                // other consumers of the shared RNG), / (1 + 2^31), * 5.
                const std::int32_t n=trunc_word(mul(bits(0x40a00000U),div(game_int_to_float(static_cast<std::int32_t>(r)),scale)));
                const float unit=bits(0x3d088889U);                      // 1/30
                auto a=event(ActorDamageEvent::Kind::rumble);a.arguments={0,1,0,1};a.seconds=mul(unit,word_float(n));out.push_back(a);
                auto b=event(ActorDamageEvent::Kind::rumble);b.arguments={0,0,intensity,1};b.seconds=mul(unit,word_float(n+5));out.push_back(b);
            }
        }else voice=s.gender_3260==1?ActorDamageSound::voice_npc_hit_f:ActorDamageSound::voice_npc_hit_m;
    }
    if(type==9&&in.category==2){voice.reset();hit.reset();}             // 00187f08
    if(voice&&type!=0xa)out.push_back(sound(*voice,true));
    if(hit)out.push_back(sound(*hit,true));
    if(in.penetration)result.penetration=p;
    result.dead=life(s,h)==3;
    return result;
}
}
