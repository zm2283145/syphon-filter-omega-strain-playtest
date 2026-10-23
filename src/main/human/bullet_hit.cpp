#include "bullet_hit.hpp"
#include "water_effects.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
// cvt.w.s: truncation, saturating to 0x7fffffff / 0x80000000 (EE goldens: tests/band_damage_zero_goldens.hpp).
// The EE FPU has no NaN or infinity; a host NaN or infinity is refused.
std::int32_t truncate(float value,const char* what){
    if(!std::isfinite(value))throw std::runtime_error(what);
    if(value>=2147483648.0F)return std::numeric_limits<std::int32_t>::max();
    if(value<=-2147483648.0F)return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(value);
}
// 0049d020: the 21 hit-node codes.
constexpr std::array<std::int8_t,21> hit_node_codes{0,2,5,9,14,8,13,17,19,10,15,18,20,1,4,7,12,3,6,11,16};
}

std::int32_t weapon_band_damage(const WeaponRecord& r,float d){
    const float near=game_int_to_float(r.near_34());
    if(d<=near)return r.damage_near_68();
    const float mid=game_int_to_float(r.mid_38());
    const float near_damage=game_int_to_float(r.damage_near_68());
    float delta,span,slope;
    if(d<=mid){
        delta=d-near;
        span=game_int_to_float(static_cast<std::int32_t>(static_cast<std::uint32_t>(r.mid_38())-static_cast<std::uint32_t>(r.near_34())));
        slope=game_int_to_float(static_cast<std::int32_t>(static_cast<std::uint32_t>(r.damage_mid_64())-static_cast<std::uint32_t>(r.damage_near_68())));
    }else{
        delta=d-mid;
        span=game_int_to_float(static_cast<std::int32_t>(static_cast<std::uint32_t>(r.far_3c())-static_cast<std::uint32_t>(r.mid_38())));
        slope=game_int_to_float(static_cast<std::int32_t>(static_cast<std::uint32_t>(r.damage_far_6c())-static_cast<std::uint32_t>(r.damage_mid_64())));
    }
    // Checkpoint 132uk4: equal bands (+3c == +38, the distance past +38): div.s 0022bf44 by zero gives
    // +-Fmax (the sign of the dividend; +Fmax for a zero slope), add.s keeps it and cvt.w.s 0022bf58
    // saturates. Every operand is a converted integer or a positive distance, so neither is -0.
    const float product=slope*delta;
    const float quotient=span!=0.0F?product/span
        :(product<0.0F?-std::numeric_limits<float>::max():std::numeric_limits<float>::max());
    return truncate(near_damage+quotient,"Weapon band damage outside the integer range");
}

std::int32_t bullet_hit_amount(float damage_40){
    return truncate(damage_40,"Bullet hit: damage that is not finite");
}

std::uint8_t apply_bullet_hit(const BulletHitVictim& victim,const std::optional<BulletHitShooter>& shooter,BulletHitRequest& req,
    const BulletHitHost& host,std::vector<BulletHitEvent>& events){
    if(shooter.has_value()!=(req.shooter_0!=0)||(shooter&&shooter->object!=req.shooter_0))
        throw std::runtime_error("Bullet hit: shooter description does not match the request");
    const bool designated=req.designated_8==victim.object;
    const std::int32_t selection=designated?req.subtarget_10:-1;
    // lb: the record's damage type sign-extended; 8 without a record.
    std::int32_t type=req.record_18?static_cast<std::int8_t>(req.record_18->damage_type_bc()):8;
    std::int32_t amount=bullet_hit_amount(req.damage_40);
    const auto* sc=shooter&&shooter->controller?&*shooter->controller:nullptr;
    const bool victim_player=victim.controller_player_54.value_or(false);
    // Port-only (132ob7): an agent-slot actor (the CPU partner) counts as a player for this test only.
    const bool both_players=(victim_player||victim.port_agent_slot)&&sc&&(sc->player_54||sc->port_agent_slot);
    std::int32_t mode=designated?0:1;
    if((amount&0xffff)==0||victim.invulnerable_33a4||both_players||(type&0xff)==7)mode=0;
    else if(victim.armor_33a0<=0)mode=2;
    bool passes=false;
    if(sc&&!sc->player_54&&sc->aim_target!=victim.object){
        if(!host.random)throw std::runtime_error("Bullet hit: no game RNG");
        const float draw=game_int_to_float(static_cast<std::int32_t>(host.random()))/2147483648.0F;
        passes=!(draw<=0.3F);                                    // 0048a2b8
    }
    BulletHitVolumes volumes;
    if(!passes){
        events.push_back({BulletHitEvent::Kind::hit_test,mode,0,0,0.0F});
        if(!host.hit_volumes)throw std::runtime_error("Bullet hit: the actor hit volumes (00188140) are not resolved");
        volumes=host.hit_volumes(req.frame_4[3],req.frame_4[1],mode,req.volume_28);
        if(volumes.nodes.size()>8)throw std::runtime_error("Bullet hit: more than 8 hit nodes");
    }
    const auto flags=volumes.flags;
    auto category=static_cast<std::uint8_t>(flags&0x40?2:flags&0x20?4:flags&0x80?3:flags?1:0);
    const bool aimed=sc&&sc->aim_target==victim.object&&designated;
    if(aimed){
        const auto s=selection&3;
        category=static_cast<std::uint8_t>(s==0?2:s==3?3:1);
    }
    if(category&&shooter&&shooter->owned_14&&!req.skip_damage_20){
        // Original: both_players implies victim_player, so every player-to-player hit gets the zero
        // amount and, unless aimed, the player victim's category 3. The port-only agent-slot victim
        // (an AI controller) takes both inside this rule only: a head volume's category 2 would
        // otherwise reach 00187768, whose 100000 (a player on either side) kills through amount 0.
        if(both_players){
            amount=0;                                             // delay slot: every player-to-player hit
            if((type&0xff)==7)type=0;
        }
        if((victim_player||(both_players&&victim.port_agent_slot))&&!aimed)category=3;
        const auto t=static_cast<std::uint8_t>(type&0xff);
        const bool counted=t<14&&((1U<<t)&0x2683U)!=0;
        if(!host.life_state)throw std::runtime_error("Bullet hit: no victim life state");
        const auto life=host.life_state();
        if(!(life==2||life==3)&&counted&&sc&&sc->player_54&&shooter->owned_14&&!both_players&&req.statistics_2c){
            events.push_back({BulletHitEvent::Kind::statistic,10,0,0,0.0F});
            if(req.aim_view_d)events.push_back({BulletHitEvent::Kind::statistic,12,0,0,0.0F});
            if(t==7&&category!=2)events.push_back({BulletHitEvent::Kind::statistic,6,0,0,0.0F});
        }
        const float value=game_int_to_float(amount);
        events.push_back({BulletHitEvent::Kind::damage,category,t,req.penetration_24,value});
        if(!host.damage)throw std::runtime_error("Bullet hit: no victim damage virtual");
        host.damage(category,t,req.penetration_24,value);
    }
    if(flags&&req.penetration_24<=0){
        req.node_3c=-1;req.distance_34=volumes.distance;req.category_30=category;req.victim_38=victim.object;
        for(auto i=static_cast<std::ptrdiff_t>(volumes.nodes.size())-1;i>=0;--i){
            const auto code=volumes.nodes[static_cast<std::size_t>(i)];
            if(code<0)continue;
            for(std::size_t k=0;k<hit_node_codes.size();++k)
                if(hit_node_codes[k]==code){req.node_3c=static_cast<std::int32_t>(k);break;}
            break;
        }
    }
    return category;
}

std::uint8_t actor_stance_c8(std::uint32_t f){
    if(f&0x20U)return 2;
    if(f&0x2U||f&0x80U||f&0x40U)return 1;
    if((f&0x8U||f&0x10U)&&(f&1U))return 4;
    return 0;
}

float actor_target_size_cc(std::int32_t subtarget){
    const auto s=subtarget&3;
    return s==0?0.3F:(s==1||s==2)?0.5F:1.0F;
}

}
