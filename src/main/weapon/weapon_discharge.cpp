#include "weapon_discharge.hpp"
#include "game_libm.hpp"
#include "water_effects.hpp"
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
constexpr float narrow_spread=0.02F;     // 00489c30
constexpr float wide_spread=0.04F;       // 00489c28
constexpr float pellet_step=0.04F;       // 00489c38
constexpr float pellet_jitter=0.5F;      // 00489c40
constexpr float pellet_damage=30.0F;     // 00489c48
constexpr float pellet_roll=6.28F;       // literal 0x40c8f5c3
constexpr std::array<std::int32_t,4> pellet_pitch{-1,0,1,0};   // 00489db0
constexpr std::array<std::int32_t,4> pellet_yaw{0,-1,0,1};     // 00489dc0
// float(r) / (1 + 2^31): the denominator rounds to 2^31 in binary32.
float unit_draw(const WeaponDischargeHost& host){
    if(!host.random)throw std::runtime_error("Weapon discharge: no game RNG");
    return game_int_to_float(static_cast<std::int32_t>(host.random()))/(1.0F+2147483648.0F);
}
template<class F> auto need(const F& f,const char* what)->const F&{
    if(!f)throw std::runtime_error(what);
    return f;
}
// VU results flush denormals to zero (sign kept); a vmadda product is rounded
// to binary32 but not flushed, only the sum is (tools/ee/ee.py, VU_ROUNDING.md).
float vu_flush(float v){return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;}
float vu_mul(float a,float b){return vu_flush(a*b);}
float vu_madd(float acc,float s,float t){const float p=s*t;return vu_flush(acc+p);}
constexpr float launch_arc=0.02F;        // 00489c50 (0x3ca3d70a)
}

bool discharge_launched_projectile(const WeaponInventory& inv,const WeaponRecord& rec,const ShotRequest& req,
    const WeaponDischargeInput& in,const WeaponDischargeHost& host,std::vector<WeaponDischargeEvent>& events){
    using K=WeaponDischargeEvent::Kind;
    if(rec.projectile_d4()!=1&&rec.projectile_d4()!=2)return false;
    const auto& launch=need(host.launch_projectile,"Weapon discharge: launched projectile weapons (0025d730 / 0025d4b0) are not hosted");
    {WeaponDischargeEvent e;e.kind=K::projectile;e.request=req;events.push_back(e);}
    RootVector point{};
    WeaponDischargeEvent e;e.kind=K::projectile_launch;e.a=launch_arc;
    if(in.target!=0&&!in.aim_view){
        // 001408a0: target vtable +e4 into sp+1a0.
        point=need(host.projectile_target,"Weapon discharge: no launch target point (target +e4)")();
        e.object=in.target;
    }else{
        // 00140800..0014087c: (0, range, 0, 1) through hand 0's new shot frame.
        float range=0.0F;
        if(in.aim_view){
            if(!inv.aim_view_range_f4)throw std::runtime_error("Weapon discharge: the aim-view range (inventory +f4) is unset");
            range=*inv.aim_view_range_f4;
        }else range=game_int_to_float(rec.far_3c());
        {WeaponDischargeEvent f;f.kind=K::shot_frame;f.flag=false;f.value=in.aim_view;events.push_back(f);}
        const auto m=need(host.shot_frame,"Weapon discharge: no shot frame (controller +64)")(false,in.aim_view);
        const RootVector v{0.0F,range,0.0F,1.0F};
        for(std::size_t k=0;k<4;++k){
            float acc=vu_mul(m[0][k],v[0]);
            acc=vu_madd(acc,m[1][k],v[1]);
            acc=vu_madd(acc,m[2][k],v[2]);
            point[k]=vu_madd(acc,m[3][k],v[3]);
        }
    }
    e.point=point;events.push_back(e);
    launch(req,point,launch_arc);
    return true;
}

void rotate_shot_frame(ShotFrame& frame,const ShotFrame& src,float a,float b,float c){
    // 0013ff40 0014061c..00140674 / 00140ea0 0014101c..00141070: the 00140e20
    // arguments, then 00140680..001406d8 (vmulax / vmadday / vmaddz .xyz).
    const float cc=game_cosf(c),sc=game_sinf(c),ca=game_cosf(a),sa=game_sinf(a),cb=game_cosf(b),sb=game_sinf(b);
    const float m28=cb*ca;
    const float f3=sb*cc;
    const float f1=sb*sc;
    const float m20=sb*ca;
    const float f2=cb*cc;
    const float m00=f2+f1*sa;
    const float m18=f1+f2*sa;
    const float f4=cb*sc;
    const float m10=-f4+f3*sa;
    const float m14=ca*cc;
    const float m04=ca*sc;
    const float m08=-f3+f4*sa;
    const float m24=-sa;
    const std::array<std::array<float,3>,3> m{{{m00,m04,m08},{m10,m14,m18},{m20,m24,m28}}};
    for(std::size_t k=0;k<3;++k){
        for(std::size_t j=0;j<3;++j){
            float acc=src[0][j]*m[k][0];
            acc=acc+src[1][j]*m[k][1];
            frame[k][j]=acc+src[2][j]*m[k][2];
        }
        frame[k][3]=0.0F;                        // 00140e20 rows have w = 0
    }
}

bool discharge_weapon(WeaponInventory& inv,const WeaponDatabase& db,const WeaponDischargeInput& in,
    const WeaponDischargeHost& host,std::vector<WeaponDischargeEvent>& events){
    using K=WeaponDischargeEvent::Kind;
    const WeaponRecord* rec=nullptr;
    if(inv.current_84!=weapon_no_slot){
        if(inv.current_84>=8)throw std::runtime_error("Weapon discharge: current slot outside the slot array");
        rec=&db.record(inv.slots[inv.current_84].item);
        if(rec->hold_class_f0()==5){
            // 00266200(*inv, target, aim view, subtarget); a shot makes the AI
            // noise (0016b4f0(msg, actor +c, 4, 1.0, record +b8) -> 004ee5c0)
            // and nothing else (no ammunition, sound or statistic).
            if(!host.air_taser)throw std::runtime_error("Weapon discharge: hold class 5 (00266200) is not hosted");
            const bool r=host.air_taser(in.target,in.aim_view,in.subtarget);
            if(r){
                WeaponDischargeEvent e;e.kind=K::ai_noise;e.value=host.actor_id;e.a=1.0F;e.b=rec->noise_radius_b8();events.push_back(e);
            }
            return r;
        }
    }
    std::vector<WeaponInventoryEvent> unused;
    const WeaponInventoryHost no_host;
    WeaponInventoryOps q(inv,db,no_host,unused);
    if(in.continuation&&!(q.clip(weapon_no_slot)>0)){inv.burst_bc[1]=0;inv.burst_bc[0]=0;}
    const bool dual=inv.slots[1].item!=-1&&inv.current_84==0;
    if(inv.hand_a9>1)throw std::runtime_error("Weapon discharge: active hand outside 0/1");
    const auto hand=inv.hand_a9;
    const bool second=dual&&hand!=0;
    if(!q.ready(in.continuation))return false;
    inv.value_b4[hand]=0.0F;
    if(dual)inv.value_b4[1]=0.0F;
    const bool looping=rec->looping_118()!=0;
    if(!(looping&&inv.loop_sound_c4[hand])){
        std::string name=rec->fire_sound_10();
        int flag=1;
        // 0012b270: tolower through the executable's ctype table (ASCII only here).
        if(!name.empty()&&static_cast<unsigned char>(name[0])>=0x80)throw std::runtime_error("Weapon discharge: non-ASCII fire sound name");
        if(!name.empty()&&(name[0]=='o'||name[0]=='O'))flag=0;
        else if(rec->full_auto_78()==1&&host.player&&host.byte_14){
            if(name.empty())throw std::runtime_error("Weapon discharge: empty fire sound name");
            name[0]='P';
        }
        WeaponDischargeEvent e;e.kind=K::fire_sound;e.name=name;e.value=flag;events.push_back(e);
        if(looping){
            inv.loop_sound_c4[hand]=inv.slots[inv.current_84].item;
            inv.spinup_cc[hand]=rec->hold_class_f0()==8?0.0F:0.5F;
        }
    }
    auto frame_of=[&](bool second_hand){
        WeaponDischargeEvent e;e.kind=K::shot_frame;e.flag=second_hand;e.value=in.aim_view;events.push_back(e);
        return need(host.shot_frame,"Weapon discharge: no shot frame (controller +64)")(second_hand,in.aim_view);
    };
    ShotRequest req;
    req.frame=frame_of(second);
    const bool spinning=inv.spinup_cc[hand]!=0.0F;
    if(!spinning){
        req.power_40=game_int_to_float(rec->power_104());
        req.designated_48=0;req.rolled_d4=false;req.aim_view_d5=in.aim_view;
        req.shooter_44=host.actor;req.record_dc=inv.slots[inv.current_84].item;req.subtarget_d0=in.subtarget;
        req.range_d8=game_int_to_float(rec->far_3c());req.value_e4=1.0F;req.word_e0=-1;
        auto muzzle_effect=[&](bool second_shot,bool flag){
            const std::int32_t id=in.aim_view?0:need(host.effect_id,"Weapon discharge: no effect id lookup (00236f20)")(6);
            if(!host.effect_service)throw std::runtime_error("Weapon discharge: the effect service host (003ebfb0) is required");
            if(!host.effect_service())return;
            WeaponDischargeEvent e;e.kind=K::muzzle_effect;e.flag=second_shot;e.value=flag;e.value2=id;e.request=req;events.push_back(e);
        };
        if(rec->hold_class_f0()!=8){
            bool flag=false;
            if(rec->model_13c()!=0)flag=need(host.model_flag_411,"Weapon discharge: no attached-model flag")(rec->model_13c());
            muzzle_effect(false,flag);
        }
        if(!in.aim_view&&in.target!=0&&rec->power_104()>=200){
            req.designated_48=in.target;
            const bool hit=need(host.hit_roll,"Weapon discharge: no hit roll")();
            {WeaponDischargeEvent e;e.kind=K::hit_roll;e.flag=hit;events.push_back(e);}
            if(hit){
                req.rolled_d4=true;
                if(!host.player&&req.subtarget_d0>=0){
                    const auto stance=need(host.target_stance,"Weapon discharge: no target stance")();
                    WeaponDischargeEvent e;e.kind=K::target_stance;e.stance=stance;events.push_back(e);
                    if(stance==2)req.subtarget_d0=-1;
                }
            }
        }
        const auto interval=rec->tracer_interval_a0();
        if(interval!=0&&inv.slots[inv.current_84].clip%interval==0)req.tracer_d6=true;
        float pitch=0.0F,yaw=0.0F;
        if(!in.aim_view&&rec->hold_class_f0()==0){
            const bool narrow=in.target==0||req.rolled_d4;
            const float s=narrow?narrow_spread:wide_spread;
            pitch=(2.0F*s)*unit_draw(host)-s;
            yaw=(2.0F*s)*unit_draw(host)-s;
            if(!narrow)yaw=yaw<0.0F?(yaw-s)/2.0F:(yaw+s)/2.0F;
        }
        const auto source=req.frame;
        rotate_shot_frame(req.frame,source,pitch,0.0F,yaw);
        const bool launched=discharge_launched_projectile(inv,*rec,req,in,host,events);
        auto fire=[&](const ShotRequest& r)->std::uint64_t{
            const auto b=need(host.bullet,"Weapon discharge: no bullet pool (00277e20)")(r);
            WeaponDischargeEvent e;e.kind=K::bullet;e.request=r;e.object=b;events.push_back(e);
            return b;
        };
        if(!launched&&!second){
            WeaponDischargeEvent e;e.kind=K::shake;
            e.a=game_int_to_float(rec->shake_13a())/50.0F;e.b=rec->rate_a4();events.push_back(e);
        }
        const auto first=launched?0:fire(req);
        if(!launched&&rec->damage_type_bc()==1){
            // 00140ea0: four more pellets around the first shot.
            const auto base=req.frame;
            const float power=req.power_40;
            req.tracer_d6=false;
            const float roll=pellet_roll*unit_draw(host);
            std::array<std::uint64_t,5> bullets{first,0,0,0,0};
            for(std::size_t k=0;k<4;++k){
                const float b=roll+(-pellet_jitter+(pellet_jitter- -pellet_jitter)*unit_draw(host));
                const float c=pellet_step*game_int_to_float(pellet_yaw[k]);
                const float a=pellet_step*game_int_to_float(pellet_pitch[k]);
                rotate_shot_frame(req.frame,base,a,b,c);
                req.power_40=power+(-pellet_damage+(pellet_damage- -pellet_damage)*unit_draw(host));
                bullets[k+1]=fire(req);
            }
            for(std::size_t i=0;i<5;++i)
                for(std::size_t j=0;j<5;++j){
                    if(i==j)continue;
                    if(bullets[i]==0||bullets[j]==0)throw std::runtime_error("Weapon discharge: pellet linking with a full bullet pool (00275750) is not hosted");
                    need(host.link_bullets,"Weapon discharge: no pellet linking (00275750)")(bullets[i],bullets[j]);
                    WeaponDischargeEvent e;e.kind=K::pellet_link;e.object=bullets[i];e.object2=bullets[j];events.push_back(e);
                }
        }
        if(!launched&&dual){
            req.frame=frame_of(true);
            fire(req);
            muzzle_effect(true,false);
        }
        if(rec->hold_class_f0()!=8){
            // actor +2df0 = 1 and 00384a70(actor +60, 0) (the shell eject), then the second hand's.
            WeaponDischargeEvent e;e.kind=K::flash;e.value=0;events.push_back(e);
            if(host.eject_shell)host.eject_shell(false);
            if(dual){e.value=1;events.push_back(e);if(host.eject_shell)host.eject_shell(true);}
        }
    }
    {
        WeaponDischargeEvent e;e.kind=K::ai_noise;e.value=host.actor_id;e.a=1.0F;e.b=rec->noise_radius_b8();events.push_back(e);
    }
    if(host.ai){
        WeaponDischargeEvent e;e.kind=K::fire_notice;e.object=in.target;e.flag=in.continuation;events.push_back(e);
    }
    const float rate=rec->rate_a4();
    inv.cooldown_ac[hand]=rate==0.0F?0.0F:1.0F/rate;
    if(in.continuation)inv.burst_bc[hand]=static_cast<std::int32_t>(static_cast<std::uint32_t>(inv.burst_bc[hand])-1U);
    else if(rec->burst_5c()>=2){
        inv.burst_bc[hand]=rec->burst_5c()-1;
        inv.burst_target_90=in.target;inv.burst_target_id_94=in.target!=0?in.target_id:-1;
        inv.burst_subtarget_9c=in.subtarget;
    }
    inv.aim_view_98=in.aim_view;
    if(inv.spinup_cc[hand]!=0.0F)return false;
    const std::size_t slot=inv.current_84+(hand!=0?1U:0U);
    if(slot+(dual?1U:0U)>=8)throw std::runtime_error("Weapon discharge: ammo slot outside the slot array");
    auto dec=[](std::int32_t& v){v=static_cast<std::int32_t>(static_cast<std::uint32_t>(v)-1U);};
    dec(inv.slots[slot].clip);
    if(dual)dec(inv.slots[slot+1].clip);
    if(inv.slots[slot].clip==0){
        inv.burst_bc[hand]=0;
        if(inv.slots[slot].spare!=0){WeaponDischargeEvent e;e.kind=K::reload_action;events.push_back(e);}
        else if(host.player){WeaponDischargeEvent e;e.kind=K::out_of_ammo;e.value=inv.slots[slot].item;events.push_back(e);}
    }
    {
        WeaponDischargeEvent e;e.kind=K::shot_statistics;e.flag=in.aim_view;events.push_back(e);
        if(dual)events.push_back(e);
    }
    return true;
}

}
