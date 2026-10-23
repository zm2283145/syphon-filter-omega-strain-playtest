#include "player_weapon_input.hpp"
#include "hud_crosshair.hpp"
#include "actor_throw.hpp"
#include <bit>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
bool bit(std::uint32_t v,unsigned b){return ((v>>b)&1U)!=0;}
void rescan(std::int32_t& selection,std::uint32_t mask){
    for(int k=11;k>=0;--k)if(bit(mask,static_cast<unsigned>(k))){selection=k;return;}
    selection=-1;
}
template<class F> const F& need(const F& f,const char* what){
    if(!f)throw std::runtime_error(std::string("Pad weapon block: host ")+what+" required");
    return f;
}
}

void pad_weak_handle_assign(std::uint64_t& object,std::int32_t& id,std::uint64_t target,std::int32_t target_id_c){
    object=target;
    id=target?target_id_c:-1;
}
RootVector pad_transform_point(const ShotFrame& m,const RootVector& v){
    RootVector out{};
    for(std::size_t k=0;k<4;++k){                                           // vmulax / vmadday / vmaddaz / vmaddw
        float acc=m[0][k]*v[0];
        acc=acc+m[1][k]*v[1];
        acc=acc+m[2][k]*v[2];
        out[k]=acc+m[3][k]*v[3];
    }
    return out;
}

void pad_selector_clear(std::int32_t& selection,std::uint32_t& mask,unsigned b){mask&=~(1U<<b);rescan(selection,mask);}
void pad_selector_set(std::int32_t& selection,std::uint32_t& mask,unsigned b){mask|=1U<<b;rescan(selection,mask);}

bool run_pad_weapon_block(PadWeaponState& s,const PadWeaponFrame& f,const PadWeaponHost& h){
    const bool square=f.square!=0.0F;                                       // 0024f1dc c.eq.s
    const std::int32_t cls=static_cast<std::int8_t>(static_cast<std::uint8_t>(need(h.hold_class,"001437a0")()));
    const bool aim_view=f.aim_selector_1e0&&(cls==1||f.near_camera_1f0);   // s7
    bool ready=true;                                                        // sp+170
    if(cls==0||cls==5)                                                      // 0024f274: 0024fcd0(+3314)
        ready=h.secondary&&secondary_state_allows_fire((*h.secondary)[0])&&secondary_state_allows_fire((*h.secondary)[1]);
    const bool press_class=cls>=3&&cls<=10&&((1U<<static_cast<unsigned>(cls-3))&0x83U)!=0;   // 3, 4, 10
    if(s.square_ab)s.hold_100=s.hold_100+f.dt;else s.hold_100=0.0F;        // 0024f2e0
    bool fire=false;                                                        // fp
    if(cls==1){                                                             // 0024f310: the release after holding
        fire=!square&&s.square_ab;
        if(square){
            s.power_e0=throw_power(s.hold_100);                             // 0025d430(+100)
            if(aim_view)throw std::runtime_error("Pad weapon block: the aim-view arc preview (0025cdc0, hand node 0049d02c) is not native");
        }
    }
    else if(press_class||cls==11)fire=square&&!s.square_ab;                 // press edge
    else if(cls==5)fire=square&&!bit(s.selector_mask_3c,1);                 // 0024f3e4 00250350(+38, 1)
    else{
        const std::int32_t total=need(h.spare_6,"00143870")()+need(h.clip_6,"00143940")();
        if(square){
            const bool repeat=need(h.full_auto,"00143820")()&&total!=0;
            fire=repeat||!s.square_ab;
        }
    }
    if(bit(s.selector_mask_3c,1)&&!square){                                 // 0024f4a8
        pad_selector_clear(s.group_40,s.group_mask_44,1);
        need(h.item_notice,"00143770")();
    }
    if(fire||(ready&&s.buffer_120>0.0F)){                                   // 0024f4d8
        fire=false;                                                         // 0024f514: fp = 0 (classes 1 / 7 set it again)
        if(cls==1){                                                         // 0024f518
            fire=true;
            if(aim_view)pad_weak_handle_assign(s.lock_c0,s.lock_id_c4,0,0);   // 0024f520 0024fc90(+c0, 0)
            // 0014d350(+c0): the lock's +e4 (virtual), else the actor-local point
            // (0, 0025d470(+100), 0, 0); the actor's +f4 is read there and not used.
            else if(s.lock_c0&&need(h.lock_valid,"0014d350")(s.lock_c0,s.lock_id_c4))s.point_d0=need(h.position_e4,"lock +e4")(s.lock_c0);
            else s.point_d0={0.0F,throw_range(s.hold_100),0.0F,0.0F};
        }else if(cls==7){                                                   // 0024f5d0
            fire=true;
            if(aim_view){
                pad_weak_handle_assign(s.lock_c0,s.lock_id_c4,0,0);         // 0024f5e4 0024fc90(+c0, 0)
                const ShotFrame frame=need(h.aim_shot_frame,"controller +64")();
                s.point_d0=pad_transform_point(frame,{0.0F,need(h.aim_range_f4,"inventory +f4")(),0.0F,1.0F});
            }else if(!(s.lock_c0&&need(h.lock_valid,"0014d350")(s.lock_c0,s.lock_id_c4))){   // 0024f648
                const ShotFrame frame=need(h.actor_matrix_f4,"actor +f4")();
                s.point_d0=pad_transform_point(frame,{0.0F,std::bit_cast<float>(0x43480000U),std::bit_cast<float>(0x40800000U),1.0F});
            }
        }
        if(ready){                                                          // 0024f6a8
            if(cls==5){pad_selector_set(s.group_40,s.group_mask_44,1);fire=true;}
            ActorFireRequest r;
            if(s.lock_c0&&need(h.lock_valid,"00150c60")(s.lock_c0,s.lock_id_c4)){r.target=s.lock_c0;r.target_id=s.lock_id_c4;}
            r.subtarget=s.subtarget_e4;r.aim_view=aim_view;r.point=s.point_d0;r.power=s.power_e0;
            if(!fire&&need(h.clip_6,"00143940")()!=0)need(h.shot_counter,"00249530")();
            need(h.send,"send")(r);
            s.buffer_120=0.0F;
        }else if(cls==0||cls==5)s.buffer_120=std::bit_cast<float>(0x3f400000U);   // 004a7598 (boot image 0.75)
        s.timer_11c=std::bit_cast<float>(0x40400000U);                      // 004a7588 (boot image 3.0)
    }
    const float left=s.buffer_120-f.dt;                                     // 0024f7f8 sub.s, 001f1350 max.s(0, x)
    s.buffer_120=left<0.0F?0.0F:left;
    s.square_ab=static_cast<std::uint8_t>(square?1:0);
    return aim_view;
}

std::optional<HudCrosshairRecord> crosshair_held(const WeaponRecord* record){
    if(!record)return std::nullopt;
    return HudCrosshairRecord{record->byte(0xf0),record->word(0x130),static_cast<std::int8_t>(record->byte(0x100))};
}

std::int8_t pad_crosshair_346e(bool aim_1e0,bool far_200,const WeaponRecord* record,
    const std::optional<std::int32_t>& goggles_130){
    const auto held=crosshair_held(record);
    const std::int32_t s2=hud_crosshair_index(aim_1e0,far_200,held,goggles_130,true);
    return s2>=0&&held?held->value_100:std::int8_t{0};                // lb +100
}

}
