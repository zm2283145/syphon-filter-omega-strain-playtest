#include "secondary_component.hpp"
#include <bit>
#include <stdexcept>

namespace omega {
namespace {
float fb(std::uint32_t bits){return std::bit_cast<float>(bits);}
bool bit(std::uint32_t v,unsigned n){return ((v>>n)&1U)!=0;}
}

SecondaryModeChoice choose_secondary_mode(const SecondaryModeInput& in){
    const bool class1=in.hold_class==1;
    const auto state=in.state_320c;
    const bool busy=state==8||state==0x10;                                   // s0
    bool a3=false;
    if(state==8&&in.component_810&&bit(*in.component_810,9)&&bit(in.flags_3204,0))a3=true;
    const bool state16=state==0x10;                                          // a2 (reassigned)
    const bool flag3=bit(in.flags_3248,3);                                   // t4
    const bool pointed=in.byte_33ec.has_value()&&*in.byte_33ec!=0;
    bool raise=false;                                                        // a0
    if(in.raise){
        const bool held=in.inventory_84!=6;
        if((held&&!class1)||pointed)raise=true;
    }
    bool flag=false;                                                         // a1 -> +31b2
    if(raise&&(state==0||a3||state16||flag3))flag=true;
    const bool lowered=bit(in.flags_3204,4)&&!bit(in.flags_3204,0);         // s3
    const bool selected=in.inventory_18!=-1&&in.inventory_84==0;
    const auto kind=in.item_kind;
    int mode{};
    switch(kind){
    case 1:
        if(flag)mode=busy?0x17:selected?5:2;
        else if(selected||class1||busy)mode=1;
        else if(state!=0||bit(in.flags_3204,3)||in.value_31d0<=0.75F||in.value_31c4<=fb(0x3dcccccdU))mode=0xd;
        else mode=1;
        break;
    case 8:
        mode=flag?(busy?0x17:6):1;
        break;
    case 2: case 3: case 4: case 5:
        if(flag){
            if(busy)mode=kind==3?0x17:1;
            else if(kind==3)mode=bit(in.flags_3204,0)?9:8;
            else mode=bit(in.flags_3204,0)?0xb:0xa;
        }else mode=busy?1:kind==4?0xf:0xe;
        break;
    case 6:
        mode=busy?1:flag?0xc:0x10;
        break;
    case 0: case 7: case 9:
        if(flag&&pointed){mode=busy?(state16?1:0x17):7;break;}
        if(busy){mode=1;break;}
        switch(in.hold_class){                                               // 001437a0 again
        case 3: mode=0x11;break;
        case 4: mode=0x12;break;
        case 10: mode=0x13;break;
        default: mode=1;break;
        }
        break;
    default:
        throw std::runtime_error("Secondary mode: item kind 10 or above leaves the mode unset (001977f0 jump table)");
    }
    if(state!=0||lowered){
        bool keep=false;
        if((state==4||state==5||state==0xa||state==0xb)&&mode==0xe&&!bit(in.flags_3248,10)&&!bit(in.flags_3248,12))keep=true;
        if(!keep&&state==3&&mode>=0xd&&mode<=0xf)keep=true;
        if(!keep&&state==2)keep=(mode==0xd||mode==6||(mode>=0x10&&mode<=0x13))?flag3:true;
        if(!keep&&!busy)mode=1;
    }
    return {static_cast<std::uint8_t>(flag?1:0),static_cast<std::uint8_t>(mode)};
}

bool request_secondary_one_shot(SecondaryComponent& c,const std::array<std::uint8_t,48>& name,std::uint8_t kind,
    const SecondaryOneShot& d){
    std::size_t slot{};
    std::uint8_t one_shot{};
    if(kind==1){slot=0;one_shot=0x15;}
    else if(kind==3){slot=1;one_shot=0x16;}
    else return false;
    auto& s=c.states;
    if(s.current_310==one_shot||s.next_311==one_shot||s.request_312==one_shot)return false;
    s.request_312=one_shot;
    auto& o=c.one_shots[slot];
    if(!o.name)o.name=name;
    else if((*o.name)[0]!=0||name[0]!=0)o.name=name;
    o.code=d.code;o.action=d.action;o.receiver=d.receiver;o.posted=d.posted;o.value=d.value;
    return true;
}

SecondaryStep step_secondary_states(SecondaryComponent& c,float angle,float dt){
    auto& s=c.states;
    SecondaryStep out;
    const bool first=s.current_310==0||s.next_311==0;
    int target=s.request_312;
    if(target==2){
        target=(angle<=fb(0x3fa78d36U)&&!(angle<fb(0xbe860a92U)))?3:4;
    }else if(target==8||target==9){
        const float limit=s.target_314==4?fb(0xbe860a92U):fb(0xbf490fdbU);
        target=angle<limit?4:target==9?0xb:0xa;
    }
    const auto target_byte=static_cast<std::uint8_t>(target);
    s.target_314=target_byte;
    if(first){
        s.current_310=s.next_311=target_byte;
        out.snapped=out.changed=true;
        return out;
    }
    if(s.current_310==s.next_311){
        if(s.next_311!=target_byte){s.next_311=target_byte;out.changed=true;}
        return out;
    }
    float rate=1.0F;
    if(s.next_311!=target_byte&&target_byte==1)rate=0.5F;
    else if(s.next_311==0x16){
        const auto cur=s.current_310;
        if(cur==0x11||cur==0x12||cur==0x13||cur==0x14||cur==7)rate=0.5F;
    }
    const float span=s.duration_320*rate;
    if(span==0.0F)throw std::runtime_error("Secondary states: zero blend duration (+320)");
    const float t=s.blend_318+dt/span;
    s.blend_318=t;
    if(t<1.0F)return out;
    if(s.current_310==0x15||s.current_310==0x16){                             // 001d79b0 / 001d7940
        const std::size_t slot=s.current_310==0x15?0:1;
        auto& o=c.one_shots[slot];
        if(o.receiver!=0){
            if(o.posted==0){
                o.action=4;o.value=dt;
                out.posts.push_back({slot,o.code,o.action,o.receiver,o.value});
            }
            o.posted=1;o.receiver=0;
        }
    }
    s.current_310=s.next_311;
    s.next_311=target_byte;
    s.blend_318=0.0F;
    out.snapped=out.changed=true;
    return out;
}

}
