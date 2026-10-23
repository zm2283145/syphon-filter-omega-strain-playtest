#include "shell_eject.hpp"
#include "camera_rig.hpp"
#include "secondary_aim.hpp"
#include <bit>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Shell eject: "+what);}

RootVector gun_vector(const GunModel& gun,std::size_t offset){
    if(!gun.all_defined(offset,16))fail("gun model +"+std::to_string(offset)+" is undefined");
    RootVector v{};
    for(std::size_t k=0;k<4;++k)v[k]=gun.real(offset+4*k);
    return v;
}

// The inline quaternion-to-matrix block (00384dd0.., 0038a418.., 0038968c..): rows 0..2 with
// w = 0 and row 3 = vf0.
ShotFrame rotation_matrix(const RootVector& q){
    const auto rows=camera_quaternion_rows(q);
    return {rows[0],rows[1],rows[2],RootVector{0,0,0,1}};
}

// vmulax / vmadday / vmaddaz / vmaddw of a vector through a matrix.
RootVector matrix_apply(const ShotFrame& m,const RootVector& v){
    return shot_frame_node_world(ShotFrame{v,v,v,v},m)[0];
}
}

bool shell_eject_spawn(ShellEjectState& s,const GunModel& gun,std::size_t hand,const ShotFrame& attach_world,
    const ShotFrame& root_world,const std::function<std::uint32_t()>& random){
    if(hand>1)fail("00384a70 for a hand other than 0 or 1");
    if(!gun.all_defined(0x404,4))fail("gun model +404 is undefined");
    const auto chunk=gun.word(0x404);
    if(chunk==0)return false;
    if(!gun.all_defined(0x408,2))fail("gun model +408 is undefined");
    // The first free slot, else the first of the oldest.
    std::size_t slot=0;std::uint16_t oldest=0;
    for(std::size_t i=0;i<s.slots.size();++i){
        const auto age=s.slots[i].age_27fe;
        if(age==0){slot=i;break;}
        if(oldest<age){oldest=age;slot=i;}
    }
    auto& out=s.slots[slot];
    // 00384d40: the rotation and the position.
    const auto turned=shot_frame_node_world(rotation_matrix(gun_vector(gun,0x220)),attach_world);
    std::array<float,16> m{};
    for(std::size_t r=0;r<4;++r)for(std::size_t c=0;c<4;++c)m[4*r+c]=turned[r][c];
    out.rotation=secondary_matrix_quaternion(m);
    const auto a=gun_vector(gun,0x170),b=gun_vector(gun,0x230);
    RootVector p{a[0]+b[0],a[1]+b[1],a[2]+b[2],a[3]+b[3]};
    p[3]=1.0F;
    out.position=matrix_apply(attach_world,p);
    // 00127dd0 & 15: the spread.
    const auto r=random()&0xfU;
    const float spread=std::bit_cast<float>(0x3dcccccdU)+std::bit_cast<float>(0x3b03126fU)*static_cast<float>(static_cast<std::int32_t>(r));   // 00493340 + 0.002 r
    const RootVector v{0.0F,-spread,spread,0.0F};
    const auto& cur=s.current_27a0[hand];const auto& prev=s.previous_2780[hand];
    for(std::size_t k=0;k<4;++k){
        const float half=(cur[k]-prev[k])*0.5F;
        out.velocity[k]=v[k]+half;
    }
    out.floor_27f4=root_world[3][2]-4.0F;
    out.fall_27f0=0.0F;
    out.chunk_27f8=chunk;
    out.qwords_27fc=static_cast<std::uint16_t>(gun.bytes[0x408]|(gun.bytes[0x409]<<8));
    out.age_27fe=1;
    return true;
}

void shell_eject_history(ShellEjectState& s,std::size_t hand,const ShotFrame& node_world){
    if(hand>1)fail("0038a278 for a hand other than 0 or 1");
    s.previous_2780[hand]=s.current_27a0[hand];
    s.current_27a0[hand]=node_world[3];
}

void shell_eject_update(ShellEjectState& s,float dt){
    const float steps=dt/std::bit_cast<float>(0x3d088889U);                    // 1/30
    const float loss=(1.0F-std::bit_cast<float>(0x3f733333U))*steps;           // 00493348 = 0.95
    const float keep=1.0F-loss;
    const float gravity=std::bit_cast<float>(0x3ca3d70aU);                      // 00493350 = 0.02
    std::int32_t live=0;
    for(auto& slot:s.slots){
        if(slot.age_27fe==0)continue;
        const auto m=rotation_matrix(slot.rotation);
        for(auto& v:slot.velocity)v=v*keep;
        const auto step=matrix_apply(m,slot.velocity);
        for(std::size_t k=0;k<4;++k)slot.position[k]=slot.position[k]+step[k];
        slot.position[2]=slot.position[2]-slot.fall_27f0;
        slot.fall_27f0=slot.fall_27f0+gravity*steps;
        ++slot.age_27fe;++live;
        if(slot.age_27fe>=60001){slot.age_27fe=0;--live;}
        if(slot.position[2]<slot.floor_27f4){slot.age_27fe=0;--live;}
    }
    s.live_2c12=static_cast<std::int8_t>(static_cast<std::uint8_t>(live));
}

ShotFrame shell_eject_world(const ShellSlot& slot){
    const auto rows=camera_quaternion_rows(slot.rotation);
    return {rows[0],rows[1],rows[2],slot.position};
}

std::vector<std::size_t> shell_eject_drawn(const ShellEjectState& s){
    std::vector<std::size_t> live;
    for(std::size_t i=0;i<s.slots.size();++i)if(s.slots[i].age_27fe!=0)live.push_back(i);
    if(s.live_2c12<=0)return {};
    if(static_cast<std::size_t>(s.live_2c12)>live.size())fail("00385c00 sends more shells (+2c12) than are live (stale scratchpad entries)");
    live.resize(static_cast<std::size_t>(s.live_2c12));
    return live;
}

}
