#include "hotbox_world.hpp"
#include "gobj_damage.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
// Binary32 as the EE interpreter models the VU: each operation rounded, a
// denormal result flushed to a zero of the same sign.
float flush(float v){return std::fpclassify(v)==FP_SUBNORMAL?(std::signbit(v)?-0.0F:0.0F):v;}
float add(float a,float b){return flush(a+b);}
float sub(float a,float b){return flush(a-b);}
float mul(float a,float b){return flush(a*b);}
// Types whose load (00172ae0), enter (00172450 for 1/6, 00172598 for 3, none
// for 7) and exit (00171f30 for 1, 00172088 for 7, none for 3/6) only message
// their +0x64 resolved links (cEnableMsg/cDisableMsg, the 0021b790 message).
bool link_message_type(std::uint32_t t){return t==1||t==3||t==6||t==7;}
// Types 1/6/7: their link messages are hosted when every link is an actor
// the host routes them to (link_actors).
bool actor_link_type(std::uint32_t t){return t==1||t==6||t==7;}
// Object-interaction boxes: their links may name mission objects.
bool interaction_type(std::uint32_t t){return t==8||t==12;}
// Type 11: the camera zone (00172878 / 0017230c), its first link a CAM: camera.
// Type 5: the environment switch (00172688 / 001721e0 -> 001781a0), its first link an ENV object.
bool hosted_type(std::uint32_t t){return link_message_type(t)||interaction_type(t)||t==5||t==9||t==11||t==13||t==14||t==15||t==16||t==17;}
bool is_camera(const std::vector<std::uint32_t>& cameras,std::uint32_t key){
    return std::find(cameras.begin(),cameras.end(),key)!=cameras.end();
}
// The registry key of 0040c480(a, b): 0040c500 builds ((-1 + 1) << 24) |
// (a << 16) | b after the range checks a < 0x100 (0040c530) and b < 0x10000
// (0040c520); outside them it returns null.
std::optional<std::uint32_t> registry_key(std::uint32_t a,std::uint32_t b){
    if(a>=0x100||b>=0x10000)return std::nullopt;
    return (a<<16)|b;
}
}

std::optional<std::size_t> HotboxWorld::find(std::uint32_t id) const{
    for(std::size_t i=0;i<boxes.size();++i)if(!boxes[i].deleted&&boxes[i].record.id==id)return i;
    return std::nullopt;
}

HotboxWorld build_hotbox_world(const std::vector<PopulationHotbox>& records,bool activated,const std::vector<std::uint32_t>& objects,
                               const std::vector<std::uint32_t>& link_actors,const std::vector<std::uint32_t>& cameras,
                               const std::vector<std::uint32_t>& environments,const std::vector<std::uint32_t>& link_gobs,
                               const std::vector<std::uint32_t>& trigger_gobs){
    HotboxWorld w;
    for(const auto& r:records){
        if(!hosted_type(r.type))
            throw std::runtime_error("Hotbox type "+std::to_string(r.type)+" is not hosted (only 1, 3, 5, 6, 7, 8, 9, 11, 12, 13, 14, 15, 16 and 17;"
                                     " 00172400 / 00171ed0 cases 2, 4, 10)");
        if(r.id_category>=0x100||r.id_index>=0x10000)
            throw std::runtime_error("Hotbox ID outside the registry key range of 0040c480");
        if(w.find(r.id))throw std::runtime_error("Duplicate hotbox ID");
        for(float v:r.minimum)if(!std::isfinite(v))throw std::runtime_error("Nonfinite hotbox bounds");
        for(float v:r.maximum)if(!std::isfinite(v))throw std::runtime_error("Nonfinite hotbox bounds");
        Hotbox b;b.record=r;b.activated_2d=activated;
        b.enabled_2c=!r.starts_disabled();                                // 00173270..00173288
        if(r.transformed){                                                 // 001730c8: 00131e90, 00131e70, 003cdff0
            std::array<float,16> m{};
            for(std::size_t row=0;row<3;++row)for(std::size_t c=0;c<3;++c)m[row*4+c]=r.basis[row*3+c];
            for(std::size_t c=0;c<3;++c)m[12+c]=r.basis[9+c];
            m[15]=1.0F;
            for(float v:m)if(!std::isfinite(v))throw std::runtime_error("Nonfinite hotbox transform");
            b.world=m;
        }
        // 00173140: each pair through 0040c480; a null result is skipped.
        // Boxes register as they load, so only earlier ones resolve; the
        // mission objects registered before the BOX loader.
        for(const auto& l:r.links){
            const auto key=registry_key(l[0],l[1]);
            if(!key)continue;
            if(r.type==5){
                // 00172688 hands +60 entry 0 to 001781a0 as the environment without a class test: an earlier box
                // there would be read as one. The ENV: records register before the BOX loader (001c4190).
                if(std::find(environments.begin(),environments.end(),*key)==environments.end())
                    throw std::runtime_error("Type-5 hotbox link to an object that is not an ENV environment"
                                             " (00172688 passes +60 entry 0 to 001781a0 unchecked)");
                b.link_keys.push_back(*key);
                continue;
            }
            if(const auto target=w.find(*key)){
                b.links.push_back(*target);b.link_keys.push_back(*key);
                continue;
            }
            if(r.type==11){
                // The CAM: records register before the BOX loader (001c4190: 001c0920 then 001c0c00).
                if(!is_camera(cameras,*key))
                    throw std::runtime_error("Type-11 hotbox link to an object that is neither an earlier box nor a CAM: camera"
                                             " (its class byte +49, read by 00172878 / 0017230c, is not established)");
                if(b.link_keys.empty())b.first_link_camera=true;                   // +60 entry 0: class 9 (0017e3f0)
                b.link_keys.push_back(*key);
                continue;
            }
            if(r.type==3&&(std::find(link_actors.begin(),link_actors.end(),*key)!=link_actors.end()||
                           std::find(trigger_gobs.begin(),trigger_gobs.end(),*key)!=trigger_gobs.end())){
                b.link_keys.push_back(*key);                                   // 00172598: a GOBJTriggerEventMsg on enter
                continue;
            }
            if(actor_link_type(r.type)){
                if(std::find(link_actors.begin(),link_actors.end(),*key)==link_actors.end()&&
                   std::find(link_gobs.begin(),link_gobs.end(),*key)==link_gobs.end())
                    throw std::runtime_error("Type-"+std::to_string(r.type)+" hotbox link to an object that is neither a hosted actor nor a hosted"
                                             " world object (EnableMsg/DisableMsg receiver)");
                b.link_keys.push_back(*key);
                continue;
            }
            const bool object=std::find(objects.begin(),objects.end(),*key)!=objects.end();
            if(!object)throw std::runtime_error("Hotbox link to an object that is not an earlier hosted hotbox or a registered object");
            if(!interaction_type(r.type))
                throw std::runtime_error("Type-"+std::to_string(r.type)+" hotbox link to a non-box object is not hosted");
            b.link_keys.push_back(*key);
        }
        // Types 1/6/7 send their +0x64 resolved links cDisableMsg at load,
        // cEnableMsg on enter (1, 6) and cDisableMsg on exit while +2c is set
        // (1, 7): hosted for links to actors (hotbox_load_link_messages,
        // hotbox_link_messages). Type 3 sends each link a 0021b790 message
        // (key 004f5440) on enter (NPC_ACTIVATION_AND_RESPAWN.md section 1),
        // not hosted. A type-1/6/7 link to a box is not hosted either.
        if(link_message_type(r.type)&&!b.links.empty())
            throw std::runtime_error("Type-"+std::to_string(r.type)+" hotbox links to boxes (0021b790 / EnableMsg / DisableMsg) are not hosted");
        w.boxes.push_back(std::move(b));
    }
    return w;
}

std::vector<PopulationHotbox> select_hosted_hotboxes(const std::vector<PopulationHotbox>& records,const std::vector<std::uint32_t>& objects,
                                                     const std::vector<std::uint32_t>& link_actors,const std::vector<std::uint32_t>& cameras,
                                                     const std::vector<std::uint32_t>& environments,const std::vector<std::uint32_t>& link_gobs,
                                                     const std::vector<std::uint32_t>& trigger_gobs){
    std::vector<PopulationHotbox> out;
    for(const auto& r:records){
        if(!hosted_type(r.type))continue;
        if(r.type==5){                                     // links other than environments throw at build
            bool resolved=true;
            for(const auto& l:r.links){
                const auto key=registry_key(l[0],l[1]);
                if(key&&std::find(environments.begin(),environments.end(),*key)==environments.end())resolved=false;
            }
            if(!resolved)continue;
        }
        if(r.type==11){                                    // links other than earlier boxes and cameras throw at build
            bool resolved=true;
            for(const auto& l:r.links){
                const auto key=registry_key(l[0],l[1]);
                if(!key)continue;
                const bool box=std::any_of(out.begin(),out.end(),[&](const PopulationHotbox& h){return h.id==*key;});
                if(!box&&!is_camera(cameras,*key))resolved=false;
            }
            if(!resolved)continue;
        }
        if(link_message_type(r.type)){                     // links other than actors throw at build
            bool hosted=true;
            for(const auto& l:r.links){
                const auto key=registry_key(l[0],l[1]);
                if(!key)continue;
                const bool actor=std::find(link_actors.begin(),link_actors.end(),*key)!=link_actors.end();
                const bool gob=actor_link_type(r.type)&&std::find(link_gobs.begin(),link_gobs.end(),*key)!=link_gobs.end();
                const bool trigger=r.type==3&&std::find(trigger_gobs.begin(),trigger_gobs.end(),*key)!=trigger_gobs.end();
                if(!(actor_link_type(r.type)||r.type==3)||!(actor||gob||trigger))hosted=false;
            }
            if(!hosted)continue;
        }
        if(interaction_type(r.type)){                      // unresolved links throw at build
            bool resolved=true;
            for(const auto& l:r.links){
                const auto key=registry_key(l[0],l[1]);
                if(!key)continue;
                const bool box=std::any_of(out.begin(),out.end(),[&](const PopulationHotbox& h){return h.id==*key;});
                if(!box&&std::find(objects.begin(),objects.end(),*key)==objects.end())resolved=false;
            }
            if(!resolved)continue;
        }
        out.push_back(r);
    }
    return out;
}

std::array<float,16> hotbox_rigid_inverse(const std::array<float,16>& m){
    auto r=m;                                                              // 0013ee20: transpose 3x3
    std::swap(r[1],r[4]);std::swap(r[2],r[8]);std::swap(r[6],r[9]);
    if(r[12]==0.0F&&r[13]==0.0F&&r[14]==0.0F)return r;                     // 0013edb0
    // 0013ed90: t.xyz = 0 - t.xyz; 0013ed60: vmulax/vmadday/vmaddz over xyz.
    const float tx=sub(0.0F,r[12]),ty=sub(0.0F,r[13]),tz=sub(0.0F,r[14]);
    for(std::size_t c=0;c<3;++c)r[12+c]=add(add(mul(r[c],tx),mul(r[4+c],ty)),mul(r[8+c],tz));
    return r;
}

std::array<float,16> hotbox_matrix_e8(const Hotbox& box){
    if(box.world)return *box.world;                                        // 003cd1c0: component +0x10
    return {1.0F,0.0F,0.0F,0.0F, 0.0F,1.0F,0.0F,0.0F, 0.0F,0.0F,1.0F,0.0F, 0.0F,0.0F,0.0F,1.0F};   // 0055c9f0
}

RootVector hotbox_eye_c0(const Hotbox& box,std::uint64_t selector){
    // Only a loaded box has a component, and the reader stores its bounds
    // with w = 1 (00172c94 / 00172cd4); run-time boxes have none.
    if(!box.world)return object_eye_c0(nullptr,std::nullopt,selector);
    const auto& w=*box.world;
    ObjectMatrix m{};
    for(std::size_t r=0;r<4;++r)for(std::size_t c=0;c<4;++c)m[r][c]=w[r*4+c];
    VumBounds b;
    for(std::size_t c=0;c<3;++c){b.min[c]=box.record.minimum[c];b.max[c]=box.record.maximum[c];}
    b.min[3]=b.max[3]=1.0F;
    return object_eye_c0(&m,b,selector);                                   // 0014d340 -> 003cd260 (+49 = 8, +88 = box + 0x70)
}

bool hotbox_contains(const Hotbox& box,const RootVector& point){
    RootVector p=point;
    for(float v:p)if(!std::isfinite(v))throw std::runtime_error("Nonfinite hotbox query point");
    if(box.world){
        const auto inv=hotbox_rigid_inverse(*box.world);
        p[3]=1.0F;                                                         // 001719f8
        RootVector q{};
        for(std::size_t c=0;c<4;++c)                                       // 00171b30
            q[c]=add(add(add(mul(inv[c],p[0]),mul(inv[4+c],p[1])),mul(inv[8+c],p[2])),mul(inv[12+c],p[3]));
        p=q;
    }
    const auto& lo=box.record.minimum;const auto& hi=box.record.maximum;
    for(std::size_t c=0;c<3;++c)if(std::signbit(sub(p[c],lo[c])))return false;   // vsub.xyz, MAC sign flags 0xe0
    for(std::size_t c=0;c<3;++c)if(std::signbit(sub(hi[c],p[c])))return false;
    return true;
}

std::vector<HotboxTrackerEvent> update_hotbox_tracker(HotboxWorld& w,HotboxTracker& t,const RootVector& p){
    std::vector<HotboxTrackerEvent> out;
    const float timer=sub(t.timer_24,std::bit_cast<float>(0x3e99999aU));  // 001706ac
    t.timer_24=timer<0.0F?0.0F:timer;
    const auto event=[&](std::size_t i,std::uint8_t payload){
        const auto type=w.boxes[i].record.type;
        out.push_back({i,payload,type,static_cast<std::uint8_t>(type==5||type==0x12?1:2)});
    };
    // Exits (00170760): containment first, then the eligibility bytes.
    for(std::size_t k=0;k<t.inside_28.size();){
        const auto i=t.inside_28[k];auto& b=w.boxes[i];
        if(hotbox_contains(b,p)&&b.eligible()){++k;continue;}
        b.occupancy_98=std::max(0,b.occupancy_98-1);                      // 001707e8 (005721c8 clear)
        event(i,2);
        t.inside_28.erase(t.inside_28.begin()+static_cast<std::ptrdiff_t>(k));
    }
    // Enters (001709a0) in global-list order.
    for(std::size_t i=0;i<w.boxes.size();++i){
        auto& b=w.boxes[i];
        if(!b.eligible()||!hotbox_contains(b,p))continue;
        bool inside=false;
        for(auto j:t.inside_28)if(j==i)inside=true;
        if(inside)continue;
        b.occupancy_98=std::min(3,b.occupancy_98+1);                      // 00170ad8
        t.inside_28.push_back(i);                                          // 00170c10
        event(i,0);
    }
    return out;
}

NpcInteractableBounds npc_interactable_bounds(const RootVector& t,const RootVector& lo,const RootVector& hi,bool full_xy){
    for(std::size_t c=0;c<4;++c)
        if(!std::isfinite(t[c])||!std::isfinite(lo[c])||!std::isfinite(hi[c]))
            throw std::runtime_error("Run-time hotbox bounds from a non-finite actor transform or model bounds");
    RootVector e{};
    for(std::size_t c=0;c<4;++c)e[c]=sub(hi[c],lo[c]);                     // 00171114
    if(full_xy)e[2]=flush(e[2]/2.0F);                                      // 00171168 div.s
    else for(std::size_t c=0;c<4;++c)e[c]=mul(e[c],0.5F);                 // 00171144 vmulx
    const auto max_s=[](float a,float b){return a<b?b:a;};                 // MAX.S (finite)
    e[0]=max_s(e[0],2.5F);                                                 // 00171180
    e[1]=max_s(e[1],2.5F);                                                 // 0017118c
    e[2]=max_s(e[2],4.0F);                                                 // 001711a4
    NpcInteractableBounds b;
    for(std::size_t c=0;c<4;++c){b.minimum[c]=sub(t[c],e[c]);b.maximum[c]=add(t[c],e[c]);}   // 001711c0 / 001711ec
    return b;
}

std::size_t add_runtime_hotbox(HotboxWorld& w,std::uint32_t id,std::uint32_t npc_key,const NpcInteractableBounds& bounds){
    if(w.find(id))throw std::runtime_error("Run-time hotbox ID already in the world: "+std::to_string(id));
    Hotbox box;
    box.record.type=3;                                                     // 001704a4 (the constructor's 8 overwritten)
    box.record.id_category=id>>16;box.record.id_index=id&0xffffU;box.record.id=id;
    box.record.parameter=0xffffffffU;                                      // +94 = -1 (00173590)
    for(std::size_t c=0;c<3;++c){box.record.minimum[c]=bounds.minimum[c];box.record.maximum[c]=bounds.maximum[c];}
    box.record.transformed=false;                                          // +50 = 0 (003cf15c)
    box.enabled_2c=true;box.activated_2d=true;box.removed_2e=false;        // 003cf120, 003ce980, 003cf130
    box.occupancy_98=0;box.priority_9c=10;                                 // 001735ac, 001735b4
    box.link_keys={npc_key};                                               // 00171c40
    w.boxes.push_back(std::move(box));                                     // 00171020: append to 004ee688
    w.runtime_6e0.push_back(w.boxes.size()-1);                             // 001704a0: 00170500(004ee6e0, box)
    return w.boxes.size()-1;
}

std::size_t add_object_hotbox(HotboxWorld& w,std::uint32_t id,std::uint32_t object_key,const NpcInteractableBounds& bounds){
    if(w.find(id))throw std::runtime_error("Object hotbox ID already in the world: "+std::to_string(id));
    Hotbox box;
    box.record.type=0x12;                                                  // 00170e20: a3 = 1
    box.record.id_category=id>>16;box.record.id_index=id&0xffffU;box.record.id=id;
    for(std::size_t c=0;c<3;++c){box.record.minimum[c]=bounds.minimum[c];box.record.maximum[c]=bounds.maximum[c];}
    box.record.transformed=false;                                          // +50 = 0 (003cf15c)
    box.enabled_2c=true;box.activated_2d=true;box.removed_2e=false;        // 003cf120, 003ce980, 003cf130
    box.occupancy_98=0;box.priority_9c=10;                                 // 001735ac, 001735b4
    box.link_keys={object_key};                                            // 00171c40
    box.record.parameter=0xffffffffU;                                      // +94 = -1 (00173590)
    w.boxes.push_back(std::move(box));                                     // 00171020: append to 004ee688
    return w.boxes.size()-1;
}

void remove_object_hotbox(HotboxWorld& w,HotboxTracker& t,std::size_t index){
    auto& b=w.boxes.at(index);
    if(b.deleted)throw std::runtime_error("00170db0: the box is not in the global list");
    b.deleted=true;                                                        // 00138ab0(004ee688, box)
    std::erase(t.inside_28,index);                                         // 00138ab0(tracker + 28, box)
}

void clear_runtime_hotboxes(HotboxWorld& w,HotboxTracker& t){
    for(const auto index:w.runtime_6e0){                                   // 00170590..001705d4
        w.boxes.at(index).deleted=true;                                    // 00138ab0(004ee688, box)
        std::erase(t.inside_28,index);                                     // 00138ab0(tracker + 28, box)
    }
    w.runtime_6e0.clear();                                                 // 004ee6e4 = 0
}

std::vector<HotboxType3Trigger> hotbox_type3_enter_triggers(const HotboxWorld& w,std::size_t index){
    const auto& b=w.boxes.at(index);
    if(b.record.type!=3)throw std::runtime_error("Type-3 enter triggers of a box of type "+std::to_string(b.record.type));
    std::vector<HotboxType3Trigger> out;
    for(const auto key:b.link_keys)out.push_back({index,key,0,0,0,b.record.id});   // 0021b7d8 / 7e0 / 804 / 7f4
    return out;
}

std::optional<HotboxActorEvent> receive_hotbox_event(const HotboxWorld& w,std::size_t index,std::uint8_t payload){
    const auto& b=w.boxes.at(index);
    const auto type=b.record.type;
    if(payload==3)throw std::runtime_error("Hotbox event payload 3 (virtual +0x114) is not hosted");
    if(payload==0){                                                        // 00172400
        if(type==9)return HotboxActorEvent{9,0,b.record.parameter};        // 00172828
        if(type==13)return HotboxActorEvent{13,0,b.record.parameter};      // 00172710 (byte +20 = 5, +1c = 0x40)
        if(interaction_type(type)||type==14||type==0x12)return HotboxActorEvent{type,0,b.record.parameter}; // 001726b0
        if(type==15||type==16){                                            // 00172898
            if(b.links.empty())throw std::runtime_error("Jump-zone hotbox without a link (00172898 reads link 0 unchecked)");
            return HotboxActorEvent{type,0,w.boxes[b.links[0]].record.id};
        }
        if(type==17)return std::nullopt;                                   // table 0049c8e0: 00172908
        if(type==11)return std::nullopt;                                   // 00172878: the camera (hotbox_camera_action)
        if(type==5)return std::nullopt;                                    // 00172688: the environment (hotbox_environment_action)
        // 00172450 (1, 6; reads the occupant's player index for the
        // messages), 00172598 (3), 00172908 (7): nothing for the actor. A
        // type-3 box's links get the GOBJTriggerEventMsgs of
        // hotbox_type3_enter_triggers (sent by the caller, transport 0).
        if(link_message_type(type))return std::nullopt;
    }else if(payload==2){                                                  // 00171ed0
        if(type==9||type==15||type==16)return HotboxActorEvent{type,2,b.record.parameter}; // 00172288
        if(interaction_type(type)||type==13||type==14||type==0x12)return HotboxActorEvent{type,2,b.record.parameter}; // 00172220
        if(type==17)return std::nullopt;                                   // table 0049c890: 00172330
        if(type==11)return std::nullopt;                                   // 0017230c: the camera (hotbox_camera_action)
        if(type==5)return std::nullopt;                                    // 001721e0: the environment (hotbox_environment_action)
        // 00171f30 (1), 00172088 (7): +2c via virtual +0x6c (0015bfa0), then
        // the link loop; 00172330 (3, 6): nothing.
        if(link_message_type(type))return std::nullopt;
    }else return std::nullopt;                                             // 00171c10
    throw std::runtime_error("Hotbox type "+std::to_string(type)+" events are not hosted");
}

std::optional<HotboxCameraAction> hotbox_camera_action(const HotboxWorld& w,std::size_t index,std::uint8_t payload){
    const auto& b=w.boxes.at(index);
    if(b.record.type!=11||!b.first_link_camera||b.link_keys.empty())return std::nullopt;
    if(payload==0)return HotboxCameraAction{true,b.link_keys[0]};         // 00172878..001728a0: 00131320(G, +60[0])
    if(payload==2)return HotboxCameraAction{false,b.link_keys[0]};        // 0017230c..00172328: 001310f0(G, +68[0])
    return std::nullopt;
}

std::optional<HotboxEnvironmentAction> hotbox_environment_action(const HotboxWorld& w,std::size_t index,std::uint8_t payload){
    const auto& b=w.boxes.at(index);
    if(b.record.type!=5)return std::nullopt;
    if(payload==0){                                                        // 00172688: only with a link
        if(b.link_keys.empty())return std::nullopt;
        return HotboxEnvironmentAction{false,b.link_keys[0]};
    }
    if(payload==2)return HotboxEnvironmentAction{true,0};                  // 001721e0: the controller's first environment
    return std::nullopt;
}

std::optional<std::size_t> select_interaction_hotbox(const HotboxWorld& w,const HotboxTracker& t,std::int32_t mode){
    if(!(flush(t.timer_24)<=0.0F))return std::nullopt;                    // 00171288: c.ole.s timer, 0
    std::optional<std::size_t> chosen;
    std::int32_t best=-1;
    for(const auto i:t.inside_28){                                         // 00171370
        const auto& b=w.boxes.at(i);
        if(!b.eligible())continue;
        const auto type=b.record.type;
        if(type==12&&(mode==7||mode==8||mode==10))return std::nullopt;    // 001713c0
        if(type!=8&&type!=0x12&&type!=12)continue;                         // 00171430
        if(best<b.priority_9c){best=b.priority_9c;chosen=i;}
    }
    return chosen;
}

HotboxInteraction interact_with_hotbox(const HotboxWorld& w,HotboxTracker& t,std::int32_t mode,bool multiplayer){
    HotboxInteraction out;
    out.box=select_interaction_hotbox(w,t,mode);
    if(!out.box)return out;                                                // 001714f4
    const auto& b=w.boxes[*out.box];
    const auto type=b.record.type;
    if(multiplayer&&type==12)out.box_message=true;                         // 00171500..0017157c
    else if(type==8||type==0x12)out.triggers=hotbox_interaction_triggers(w,*out.box);   // 00171c90 (vtable 004d9ac0)
                                                                           // type 12: 00171880 (solo: nothing)
    t.timer_24=std::bit_cast<float>(0x41200000U);                         // 001715c8
    return out;
}

std::vector<HotboxTrigger> hotbox_interaction_triggers(const HotboxWorld& w,std::size_t box){
    const auto& b=w.boxes.at(box);
    if(b.record.type!=8&&b.record.type!=0x12)
        throw std::runtime_error("Hotbox +0x114 (00171c90) for a box of type "+std::to_string(b.record.type)+" (only 8 and 0x12)");
    std::vector<HotboxTrigger> out;
    for(std::size_t k=0;k<b.link_keys.size();++k)                         // 0021b790(msg, 4, +94, actor, box)
        out.push_back({box,k,b.link_keys[k],4,b.record.parameter,b.record.id});
    return out;
}

HotboxTrigger hotbox_multiplayer_interaction_trigger(const HotboxWorld& w,std::size_t box){
    const auto& b=w.boxes.at(box);
    if(b.record.type!=12)
        throw std::runtime_error("Hotbox +0x114 by HotboxMsg action 3 for a box of type "+std::to_string(b.record.type)+" is not hosted (only 12, 00171880)");
    if(b.link_keys.empty())throw std::runtime_error("Type-12 hotbox without a link (00171880 reads +68[0] unchecked)");
    return {box,0,b.link_keys[0],7,b.record.parameter,b.record.id};      // 001718a0..001718e8
}

std::vector<HotboxLinkMessage> hotbox_load_link_messages(const HotboxWorld& world){
    std::vector<HotboxLinkMessage> out;
    for(std::size_t i=0;i<world.boxes.size();++i){                          // 00173140..00173200, record order
        const auto& b=world.boxes[i];
        if(!actor_link_type(b.record.type))continue;
        for(const auto key:b.link_keys)out.push_back({i,key,false});
    }
    return out;
}

std::vector<HotboxLinkMessage> hotbox_link_messages(const HotboxWorld& world,std::size_t box,std::uint8_t payload){
    const auto& b=world.boxes.at(box);
    const auto t=b.record.type;
    std::vector<HotboxLinkMessage> out;
    if(payload==0&&(t==1||t==6))                                            // 00172450
        for(const auto key:b.link_keys)out.push_back({box,key,true});
    if(payload==2&&(t==1||t==7)&&b.enabled_2c)                              // 00171f30 / 00172088: virtual +6c (+2c)
        for(const auto key:b.link_keys)out.push_back({box,key,false});
    return out;
}
}
