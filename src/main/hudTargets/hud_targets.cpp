#include "hud_targets.hpp"
#include <algorithm>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
template<class F> const F& need(const F& f,const char* what){
    if(!f)throw std::runtime_error(std::string("HUD targets: host function not supplied: ")+what);
    return f;
}
bool present(const HudTargetsHost& host,std::uint64_t object){
    const auto b=need(host.root_bytes,"root_bytes")(object);     // +2c, +2d, +2e
    return b[1]!=0&&b[0]!=0&&b[2]==0;
}
bool resolves(const TargetHandle& h,const std::function<std::int32_t(std::uint64_t)>& object_id){
    return h.object!=0&&object_id(h.object)==h.id;
}
void emit(const HudSpriteSink& sink,HudSpriteEvent e){if(sink)sink(e);}
void build(const HudSpriteSink& sink,const HudTargetMarker& m){
    HudSpriteEvent e;e.kind=HudSpriteEvent::Kind::build;e.serial=m.serial;e.id=m.id;e.enemy=m.enemy_c;e.state=m.state_10;
    emit(sink,e);
}
void kill(const HudSpriteSink& sink,const HudTargetMarker& m){
    HudSpriteEvent e;e.kind=HudSpriteEvent::Kind::kill;e.serial=m.serial;e.id=m.id;
    emit(sink,e);
}
void colour(const HudSpriteSink& sink,const HudTargetMarker& m,HudMarkerColour c,float progress=0.0F){
    HudSpriteEvent e;e.kind=HudSpriteEvent::Kind::colour;e.serial=m.serial;e.id=m.id;e.colour=c;e.progress=progress;
    emit(sink,e);
}
}

bool add_hud_target(HudTargets& hud,std::int32_t id,bool any,const HudTargetsHost& host){
    for(const auto& m:hud.markers)if(m.id==id&&m.state_10==0)return false;
    const std::uint64_t object=need(host.find,"find")(id);
    if(object==0)return false;
    if(!any&&!need(host.has_controller,"has_controller")(object))return false;
    HudTargetMarker m;
    m.id=id;m.state_10=0;m.serial=hud.next_serial++;
    m.enemy_c=need(host.enemy,"enemy")(host.local_actor,object)?1:0;     // 00274750(marker, flag)
    hud.markers.push_back(m);
    build(host.sprites,m);
    if(host.added)host.added(m);                                       // 0x00273ab8: listener +c
    return true;
}

bool remove_hud_target(HudTargets& hud,std::int32_t id,bool ending,const HudSpriteSink& sprites){
    for(auto it=hud.markers.begin();it!=hud.markers.end();++it){
        const bool ends=it->state_10==1||it->state_10==2;
        if(it->id!=id||ends!=ending)continue;
        if(const auto at=std::find(hud.list_24.begin(),hud.list_24.end(),it->serial);at!=hud.list_24.end())hud.list_24.erase(at);
        kill(sprites,*it);                                                  // 00273018..00273034
        hud.markers.erase(it);
        return true;
    }
    return false;
}

float update_hud_targets(HudTargets& hud,const HudTargetsHost& host){
    hud.enemies_18.clear();hud.list_24.clear();
    float maximum=0.0F;
    const auto& object_id=need(host.object_id,"object_id");
    for(std::size_t k=0;k<hud.markers.size();){
        auto& m=hud.markers[k];
        bool keep=true;
        // 00274260(marker, +24, +18, &max, +30).
        const std::uint64_t object=need(host.find,"find")(m.id);
        if(object==0)keep=false;
        else if(m.state_10==1){
            if(resolves(m.handle_14,object_id))hud.list_24.push_back(m.serial);
            else keep=false;
        }else if(m.state_10==2){
            hud.list_24.push_back(m.serial);
        }else if(need(host.has_controller,"has_controller")(object)){
            if(!present(host,object))keep=false;
            else if(need(host.player_controlled,"player_controlled")(object))colour(host.sprites,m,HudMarkerColour::player);
            else{
                const auto& enemy=need(host.enemy,"enemy");
                const bool hostile=enemy(host.local_actor,object)||enemy(object,host.local_actor);
                if(hostile!=(m.enemy_c!=0)){                                  // 0027443c..00274464
                    kill(host.sprites,m);
                    m.enemy_c=hostile?1:0;
                    build(host.sprites,m);                                    // 00274750(marker, flag)
                }
                if(!hostile)colour(host.sprites,m,HudMarkerColour::friendly);
                else{
                    if(need(host.blocker,"blocker")(object)!=0)colour(host.sprites,m,HudMarkerColour::blocked);
                    else{
                        float progress=0.0F;
                        if(present(host,object)&&need(host.kind_c4,"kind_c4")(object)==2){
                            if(const auto p=need(host.threat_progress,"threat_progress")(object))progress=*p;
                        }
                        if(!(progress<=maximum))maximum=progress;
                        colour(host.sprites,m,HudMarkerColour::threat,progress);
                    }
                    hud.enemies_18.push_back({object,object_id(object)});
                }
            }
        }
        if(keep)++k;
        else{
            kill(host.sprites,m);                                             // 00273c60..00273c7c
            hud.markers.erase(hud.markers.begin()+static_cast<std::ptrdiff_t>(k));
        }
    }
    return maximum;
}

bool drop_target_lock(PadWeaponState& pad,PlayerTargetingState& state,std::uint64_t object,
    const std::function<std::int32_t(std::uint64_t)>& object_id){
    const TargetHandle lock{pad.lock_c0,pad.lock_id_c4};
    const std::uint64_t resolved=resolves(lock,need(object_id,"object_id"))?lock.object:0;
    if(resolved!=object)return false;
    pad.subtarget_e4=-1;
    state.pick_c8={0,-1};
    state.recheck_failed_bc=1;
    return ((pad.selector_mask_3c>>5)&1U)!=0;
}

std::size_t update_target_markers(HudTargets& hud,const std::vector<TargetHandle>& registered,std::uint64_t actor,
    PadWeaponState& pad,PlayerTargetingState& state,const HudTargetsHost& host){
    const auto& object_id=need(host.object_id,"object_id");
    std::size_t hud_none=0;
    auto lose=[&](std::uint64_t object){
        if(drop_target_lock(pad,state,object,object_id))++hud_none;
        remove_hud_target(hud,object_id(object),false,host.sprites);
    };
    for(const auto& h:registered){
        if(!resolves(h,object_id))continue;
        const std::uint64_t object=h.object;
        if(object==actor)continue;
        if(need(host.player_controlled,"player_controlled")(object)){add_hud_target(hud,object_id(object),false,host);continue;}
        const TargetHandle pick=state.pick_c8;
        const std::uint64_t picked=resolves(pick,object_id)?pick.object:0;
        const auto& kind=need(host.kind_c4,"kind_c4");
        if(picked==object||kind(object)==2){
            if(present(host,object))add_hud_target(hud,object_id(object),false,host);
            else lose(object);
        }else if(kind(object)==0)lose(object);
    }
    return hud_none;
}

}
