#include "bullet_pool.hpp"
#include "projectile_candidates.hpp"
#include "water_effects.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
template<class F> const F& need(const F& f,const char* what){
    if(!f)throw std::runtime_error(what);
    return f;
}
constexpr float back_offset=-2.0F;          // 0048b348
constexpr float near_wall=7.0F;             // 0048b318
}

std::vector<BulletCandidate> target_list_candidates(const std::vector<BulletListEntry>& list,const BulletListHost& host){
    std::vector<BulletCandidate> out;
    for(const auto& e:list){
        if(e.object==0)continue;
        if(need(host.id_c,"Bullet scan: no object +c")(e.object)!=e.id)continue;
        const auto b=need(host.bounds_88,"Bullet scan: no bounds (+88)")(e.object);
        if(!b)continue;
        out.push_back({e.object,(*b)[0],(*b)[1],need(host.position_e4,"Bullet scan: no position (+e4)")(e.object)});
    }
    return out;
}

void scan_bullet_candidates(BulletHitRequest& req,std::size_t bullet,const BulletPoolHost& host,std::vector<BulletEvent>& events){
    ProjectileQueryMatrix world{};
    for(std::size_t k=0;k<4;++k)for(std::size_t j=0;j<4;++j)world[4*k+j]=req.frame_4[k][j];
    const auto frame=prepare_projectile_query_frame(world);
    struct Entry{std::uint64_t object;float distance;};
    std::vector<Entry> list;
    for(const auto& c:need(host.candidates,"Bullet scan: no world object list")()){
        if(c.object==0)throw std::runtime_error("Bullet scan: null candidate");
        if(c.object==req.shooter_0)continue;
        const RootVector position=c.object==req.designated_8?need(host.eye_c0,"Bullet scan: no designated-target eye (+c0)")(c.object,req.subtarget_10):c.position_e4;
        const auto y=projectile_candidate_distance(frame,position,c.minimum,c.maximum,req.step_14);
        if(!y)continue;
        if(list.size()==20)throw std::runtime_error("Bullet scan: a 21st candidate (the original prints a diagnostic and omits it)");
        list.push_back({c.object,*y});
    }
    req.category_30=0;req.victim_38=0;
    while(true){
        std::ptrdiff_t best=-1;
        for(std::size_t i=0;i<list.size();++i){
            if(list[i].object==0)continue;
            if(best<0||list[i].distance<list[static_cast<std::size_t>(best)].distance)best=static_cast<std::ptrdiff_t>(i);
        }
        if(best<0)break;
        auto& entry=list[static_cast<std::size_t>(best)];
        std::int32_t category=-1;bool stop=false;
        if(!(entry.object==req.designated_8&&!req.rolled_c)){
            req.damage_40=0.0F;
            if(!req.skip_damage_20){
                if(!req.record_18)throw std::runtime_error("Bullet scan: no weapon record");
                req.damage_40=game_int_to_float(weapon_band_damage(*req.record_18,req.travelled_1c+entry.distance));
            }
            category=need(host.hit,"Bullet scan: no hit entry (+104)")(entry.object,req);
        }
        events.push_back({BulletEvent::Kind::candidate,bullet,entry.object,entry.distance,category,0,req.damage_40,{},{}});
        if(category>0){
            if(!(req.penetration_24>0))stop=true;
        }else if(need(host.local_player_controller,"Bullet scan: no controller query")(entry.object)&&!req.skip_damage_20&&!req.bullet_type_2d){
            events.push_back({BulletEvent::Kind::near_miss_sound,bullet,entry.object,0.0F,-1,0,0.0F,{},{}});
        }
        entry.object=0;
        if(stop)break;
    }
}

std::uint64_t BulletPool::request(const ShotRequest& r,const WeaponDatabase& db,const BulletPoolHost& host,std::vector<BulletEvent>& events){
    const auto& rec=db.record(r.record_dc);
    RootVector start{},displacement{};
    for(std::size_t i=0;i<4;++i){start[i]=r.frame[3][i]+r.frame[1][i]*back_offset;displacement[i]=r.frame[1][i]*r.range_d8;}
    const auto hit=need(host.static_hit,"Bullet request: no static LOS query (00148290)")(start,displacement,r.shooter_44);
    float remaining=r.range_d8;
    if(hit){
        RootVector d{};
        for(std::size_t i=0;i<4;++i)d[i]=hit->point[i]-r.frame[3][i];
        remaining=std::sqrt((d[0]*d[0]+d[1]*d[1])+d[2]*d[2]);
    }
    for(std::size_t i=0;i<capacity;++i){
        auto& b=records_[i];
        if(b.active_115)continue;
        // Checkpoint 132fx1: record +134 is -1 for good: the load 00147400, the copy 00147630 and the unload 00145200
        // are its only writers (no store of another value in the image), so the tracer particle 003a2dd0 of
        // Bullet_InitFromRequest is never made. A record that says otherwise is not the game's.
        if(rec.tracer_resource_134()>=0)throw std::runtime_error("Bullet request: record +134 >= 0 (the tracer particle 003a2dd0), which no writer in the image sets");
        b=BulletRecord{};
        b.frame_20=r.frame;b.speed_4=r.power_40;
        b.target_60=r.designated_48;b.target_id_64=r.designated_48?need(host.object_id,"Bullet request: no object ids")(r.designated_48):-1;
        b.shooter_68=r.shooter_44;b.shooter_id_6c=r.shooter_44?need(host.object_id,"Bullet request: no object ids")(r.shooter_44):-1;
        b.rolled_114=r.rolled_d4;b.aim_view_118=r.aim_view_d5;b.subtarget_c=r.subtarget_d0;
        b.active_115=true;b.alive_116=true;
        if(hit){
            b.static_object_70=hit->object;
            b.static_object_id_74=hit->object?need(host.object_id,"Bullet request: no object ids")(hit->object):-1;
            b.static_hit=hit;b.end_point_100=hit->point;
        }
        b.tracer_117=r.tracer_d6;b.item_148=r.record_dc;b.remaining_8=remaining;b.travelled_0=0.0F;
        b.tracer_texture_11c=need(host.effect_id,"Bullet request: no tracer texture lookup (*00236f20(0x1a))")(0x1a);
        b.tracer_phase_140=next_tracer_phase_;
        next_tracer_phase_=static_cast<std::uint8_t>((next_tracer_phase_+4U)&12U);  // 00504038, 00277dd4..00277e04
        b.penetration_10=rec.penetration_f8();b.statistics_119=true;
        events.push_back({BulletEvent::Kind::created,i,0,remaining,-1,r.record_dc,r.power_40,{},{}});
        return i+1;
    }
    events.push_back({BulletEvent::Kind::pool_full,0,0,0.0F,-1,r.record_dc,0.0F,{},{}});
    return 0;
}

const BulletRecord& BulletPool::record(std::uint64_t handle) const{
    if(handle==0||handle>capacity)throw std::runtime_error("Bullet pool: invalid bullet handle");
    return records_[handle-1];
}

void BulletPool::link(std::uint64_t a,std::uint64_t b){
    if(a==0||a>capacity||b==0||b>capacity)throw std::runtime_error("Bullet pool: invalid bullet handle");
    records_[a-1].links_80.push_back(b-1);
}

void BulletPool::update(float dt,const WeaponDatabase& db,const BulletPoolHost& host,std::vector<BulletEvent>& events){
    for(std::size_t i=0;i<capacity;++i)
        if(records_[i].active_115)update_one(i,dt,db,host,events);
}

void BulletPool::update_one(std::size_t index,float dt,const WeaponDatabase& db,const BulletPoolHost& host,std::vector<BulletEvent>& events){
    auto& b=records_[index];
    if(!b.alive_116){
        b.active_115=false;
        for(const auto link:b.links_80){
            auto& list=records_[link].links_80;
            // 002756e0: an entry equal to this bullet takes the last entry's
            // place and the count shrinks; the moved entry is not re-checked.
            std::size_t last=list.size();
            for(std::size_t k=0;k<last;++k)
                if(list[k]==index){list[k]=list[last-1];--last;}
            list.resize(last);
        }
        events.push_back({BulletEvent::Kind::freed,index,0,0.0F,-1,0,0.0F,{},{}});
        return;
    }
    const auto& rec=db.record(b.item_148);
    const auto& valid=need(host.valid,"Bullet update: no weak-handle check");
    float step=dt*b.speed_4;
    bool reached=false;
    if(!(step<=b.remaining_8)){step=b.remaining_8;reached=true;}
    const bool shooter_valid=b.shooter_68!=0&&valid(b.shooter_68,b.shooter_id_6c);
    BulletHitRequest req;
    req.shooter_0=shooter_valid?b.shooter_68:0;
    req.frame_4=b.frame_20;
    req.designated_8=b.target_60!=0&&valid(b.target_60,b.target_id_64)?b.target_60:0;
    req.rolled_c=b.rolled_114;req.aim_view_d=b.aim_view_118;req.subtarget_10=b.subtarget_c;
    req.record_18=&rec;req.step_14=step;req.travelled_1c=b.travelled_0;req.skip_damage_20=false;
    req.penetration_24=b.penetration_10;req.volume_28=0.2F;req.statistics_2c=b.statistics_119;
    req.bullet_type_2d=rec.damage_type_bc()==1;
    std::uint64_t victim=0;
    if(shooter_valid){
        scan_bullet_candidates(req,index,host,events);
        b.penetration_10=req.penetration_24;
        victim=req.victim_38;
    }
    if(victim)step=req.distance_34;
    b.remaining_8=b.remaining_8-step;
    b.travelled_0=b.travelled_0+step;
    const RootVector before=b.frame_20[3];
    for(std::size_t j=0;j<3;++j){
        float acc=b.frame_20[0][j]*0.0F;
        acc=acc+b.frame_20[1][j]*step;
        b.frame_20[3][j]=b.frame_20[3][j]+(acc+b.frame_20[2][j]*0.0F);
    }
    const bool static_valid=b.static_object_70!=0&&valid(b.static_object_70,b.static_object_id_74);
    const auto type=static_cast<std::int32_t>(static_cast<std::int8_t>(rec.damage_type_bc()));
    if(reached&&static_valid&&!victim&&rec.damage_type_bc()!=4){
        BulletEvent e{BulletEvent::Kind::static_impact,index,b.static_object_70,0.0F,-1,rec.item_kind_48(),0.0F,b.end_point_100,{}};
        if(b.static_hit)e.source=b.static_hit->source;
        if(host.static_material&&b.static_hit){
            // 00189560(bullet +d0): the surface's material row (the COL fixup's index).
            if(const auto* row=host.static_material(*b.static_hit)){
                e.impact=bullet_static_impact(*row,rec.item_kind_48(),b.static_hit->normal,bullet_impact_step(before,b.frame_20[3]),b.end_point_100);
                if(e.impact->decal==BulletImpact::Decal::table)
                    e.impact->decal_id=need(host.effect_id,"Bullet update: no effect id lookup (00236f20)")(0x24);
            }
        }
        events.push_back(e);
        if(shooter_valid){
            events.push_back({BulletEvent::Kind::ai_impact_noise,index,b.shooter_68,0.0F,-1,b.shooter_id_6c,rec.impact_noise_radius_fc(),b.end_point_100,{}});
            const float amount=game_int_to_float(weapon_band_damage(rec,b.travelled_0));
            events.push_back({BulletEvent::Kind::static_damage,index,b.static_object_70,0.0F,1,type,amount,b.end_point_100,{}});
            need(host.static_damage,"Bullet update: no static-object damage (+100)")(b.static_object_70,b.shooter_68,static_cast<std::uint8_t>(type),amount);
        }
    }
    if((victim||(static_valid&&reached))&&rec.damage_type_bc()!=4){
        if(victim&&req.category_30==2&&static_valid&&rec.damage_type_bc()!=7&&b.remaining_8<near_wall&&
            need(host.is_actor,"Bullet update: no actor class test")(victim)){
            const auto id=need(host.effect_id,"Bullet update: no effect id lookup (00236f20)")(0x26);
            if(id>=0){
                // 002772d8..00277368: end + (frame +50 - end) * 0.2 (vsub, vmulx 0x3e4ccccd, vadd on xyzw).
                RootVector point{};
                for(std::size_t j=0;j<4;++j)point[j]=b.end_point_100[j]+(b.frame_20[3][j]-b.end_point_100[j])*0.2F;
                events.push_back({BulletEvent::Kind::blood_decal,index,victim,0.0F,-1,id,0.0F,point,{}});
            }
        }
        bullet_explosion(b,index,rec,host,events);                         // 00277388..002774dc
    }
    if(!bullet_hold_block(b,index,victim,reached,rec,host,events))return;
    if(victim&&b.statistics_119)
        for(const auto link:b.links_80)records_[link].statistics_119=false;
    b.alive_116=false;
    events.push_back({BulletEvent::Kind::ended,index,victim,b.travelled_0,req.category_30,0,0.0F,b.frame_20[3],{}});
}

bool bullet_explosion(const BulletRecord& b,std::size_t index,const WeaponRecord& rec,const BulletPoolHost& host,
    std::vector<BulletEvent>& events){
    const float inner=rec.explosion_radius_c4();
    // 00277394 c.ole.s +c4, 0.0 under the EE FPU's order (no infinities or NaNs: a zero exponent reads as
    // zero, exponent 255 is a normal number beyond FLT_MAX), so only a positive word with a nonzero exponent runs.
    const auto word=std::bit_cast<std::uint32_t>(inner);
    if(((word>>23)&0xffU)==0||(word>>31)!=0)return false;
    if(rec.impact_effect_d8()!=-1){
        BulletEvent e{BulletEvent::Kind::impact_effect,index,0,0.0F,-1,rec.impact_effect_d8(),0.0F,b.frame_20[3],{}};
        e.frame=b.frame_20;                                                  // 002773b0: a1 = bullet +20
        events.push_back(e);
    }
    // 002773b8..00277414: end (+100) + frame row 1 (+30) * -2.0 (*0048b348; vmulx, vadd on xyzw).
    RootVector point{};
    for(std::size_t j=0;j<4;++j){
        const float scaled=b.frame_20[1][j]*-2.0F;
        point[j]=b.end_point_100[j]+scaled;
    }
    const auto& valid=need(host.valid,"Bullet update: no weak-handle check");
    const auto& blast=need(host.blast,"Bullet update: no blast (0021a200)");
    BulletBlastCall bc;
    if(b.shooter_68!=0&&valid(b.shooter_68,b.shooter_id_6c))bc.source={b.shooter_68,b.shooter_id_6c};   // 00277430..00277470
    bc.damage=rec.damage_mid_64();bc.kind=true;bc.point=point;bc.skip_humans=false;bc.local=true;
    bc.strength=1.0F;bc.inner=inner;bc.outer=rec.explosion_outer_c8();
    events.push_back({BulletEvent::Kind::explosion,index,bc.source.object,0.0F,-1,bc.damage,bc.strength,bc.point,{}});
    blast(bc);                                                               // 002774a4
    const auto id=need(host.effect_id,"Bullet update: no effect id lookup (00236f20)")(0x25);
    if(id>=0)events.push_back({BulletEvent::Kind::scorch_decal,index,0,0.0F,-1,id,5.0F,point,{}});   // 002774d4, f12 *0048b328
    return true;
}

bool bullet_hold_block(const BulletRecord& b,std::size_t index,std::uint64_t victim,bool reached,const WeaponRecord& rec,
    const BulletPoolHost& host,std::vector<BulletEvent>& events){
    if(!victim&&!reached)return false;                                      // 002774e0..002774ec
    const auto& valid=need(host.valid,"Bullet update: no weak-handle check");
    const auto hold=rec.hold_class_f0();
    if(hold==9||(hold==8&&(victim||(b.static_object_70!=0&&valid(b.static_object_70,b.static_object_id_74))))){
        // 00277578..00277924. Each test of the shooter re-reads its weak handle (+68 / +6c).
        const auto shooter=[&]{return b.shooter_68!=0&&valid(b.shooter_68,b.shooter_id_6c)?b.shooter_68:std::uint64_t{0};};
        if(rec.impact_effect_d8()!=-1){
            // 0027758c: 00232460(record +d8, &bullet +20) (Checkpoint 132uk6).
            BulletEvent e{BulletEvent::Kind::impact_effect,index,0,0.0F,-1,rec.impact_effect_d8(),0.0F,b.frame_20[3],{}};
            e.frame=b.frame_20;
            events.push_back(e);
        }
        if(victim){
            const auto s=shooter();
            if(s==0)throw std::runtime_error("Bullet update: the hold block reads +58 of a null shooter (002775f8)");
            // 002775f8..00277700: the shooter's controller answers +54 and its +14 is set, hold 9, the
            // victim's +58 is null or does not answer +54, and the victim is human (+4c == *0049d010).
            if(need(host.local_player_controller,"Bullet update: no controller query")(s)&&hold==9&&
               !need(host.controller_player,"Bullet update: no victim controller query (00277698)")(victim)&&
               need(host.is_actor,"Bullet update: no actor class test")(victim))
                events.push_back({BulletEvent::Kind::statistic,index,victim,0.0F,-1,10,0.0F,{},{}});
        }
        // 00277708..00277778: 0036b180(0, 0xe0, "bullet.cc", 0x1f5) inside the 0010dc40(9) / 0010dca8(9) bracket;
        // the host's allocation does not fail.
        const auto& area=need(host.area,"Bullet update: no area object (0025a2d0)");
        BulletAreaCall ac;
        if(const auto s=shooter())ac.owner={s,b.shooter_id_6c};
        ac.point=b.frame_20[3];ac.record=b.item_148;ac.flag=true;
        events.push_back({BulletEvent::Kind::area,index,ac.owner.object,0.0F,-1,ac.record,0.0F,ac.point,{}});
        area(ac);
        const auto& blast=need(host.blast,"Bullet update: no blast (0021a200)");
        BulletBlastCall bc;
        if(const auto s=shooter())bc.source={s,b.shooter_id_6c};
        bc.damage=1;bc.kind=true;bc.point=b.frame_20[3];bc.skip_humans=true;bc.local=true;
        bc.strength=0.0F;bc.inner=10.0F;bc.outer=10.0F;
        events.push_back({BulletEvent::Kind::blast,index,bc.source.object,0.0F,-1,bc.damage,bc.strength,bc.point,{}});
        blast(bc);
        if(rec.hold_class_f0()==9){
            // 002778c0: 0020f860 places the sound at +100: the static hit point, or else the shot request's +c0
            // that 00277e20 copies at 00277f9c (00132020). No caller writes the request's +c0 (0013ff40 builds it
            // at sp+290 and leaves +c0..+cc unset; 00140ea0 passes the same request on), so without a static hit
            // the original reads stale stack. PORT-ONLY (checkpoint 132uk4, presentation only, as 132zf7's
            // 002797c0): the sound is then placed at the block's own point (frame +50), and value 1 marks it.
            if(b.static_hit)events.push_back({BulletEvent::Kind::hold_sound,index,0,0.0F,-1,0,0.0F,b.end_point_100,{}});
            else events.push_back({BulletEvent::Kind::hold_sound,index,0,0.0F,-1,1,0.0F,b.frame_20[3],{}});
        }
    }
    return true;
}

}
