#include "pad_vibration.hpp"
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
void finite(float value,const char* where){
    if(!std::isfinite(value))throw std::runtime_error(std::string(where)+": nonfinite vibration time (the EE has no inf / NaN)");
}
std::int32_t wrap_add(std::int32_t a,std::int32_t b){
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a)+static_cast<std::uint32_t>(b));
}
PadActuatorWrite write(PadVibration& v,std::int32_t port,std::uint8_t small_on,std::int32_t large){
    PadActuatorWrite w{port,pad_actuator_bytes(small_on,large)};
    v.last[static_cast<std::size_t>(port)]=w;
    return w;
}
}

std::array<std::uint8_t,2> pad_actuator_bytes(std::uint8_t small_on,std::int32_t large){
    const auto l=static_cast<std::uint32_t>(large);
    return {static_cast<std::uint8_t>((small_on!=0?1U:0U)+((l<<1)&0xfeU)),static_cast<std::uint8_t>((l&0x80U)!=0?1U:0U)};
}

std::int32_t start_pad_vibration(PadVibration& v,std::int32_t port,std::uint8_t small_on,std::int32_t large,
    std::int32_t period,float duration){
    if(port<0||port>1)throw std::runtime_error("0026bde0: pad vibration port "+std::to_string(port)+" outside the two ports");
    finite(duration,"0026bde0");
    for(auto& s:v.slots[static_cast<std::size_t>(port)]){
        if(s.active_18)continue;
        s.active_18=1;s.small_00=small_on;s.large_04=large;s.duration_08=duration;s.period_0c=period;s.elapsed_10=0.0F;
        s.serial_14=v.serial_48b270;
        v.serial_48b270=wrap_add(v.serial_48b270,1);
        return s.serial_14;
    }
    return 0;
}

void update_pad_vibration(PadVibration& v,float dt,std::array<PadActuatorWrite,2>* writes){
    finite(dt,"0026bc20");
    v.tick_4ff020=wrap_add(v.tick_4ff020,1);
    for(std::int32_t port=0;port<2;++port){
        std::uint8_t small_on=0;std::int32_t large=0;
        if(v.enabled_4ff028){
            for(auto& s:v.slots[static_cast<std::size_t>(port)]){
                if(!s.active_18)continue;
                s.elapsed_10+=dt;
                finite(s.elapsed_10,"0026bc20");
                if(s.period_0c==0)throw std::runtime_error("0026bc20: pad vibration period 0 (div by zero)");
                // div: the remainder takes the dividend's sign; INT_MIN / -1 leaves no remainder.
                const std::int32_t r=s.period_0c==-1?0:v.tick_4ff020%s.period_0c;
                if(r==0){
                    small_on=(small_on|s.small_00)!=0?1:0;
                    large=wrap_add(large,s.large_04);
                }
                // c.olt.s duration < 0 keeps it; c.ole.s elapsed <= duration keeps it (NaN compares false).
                if(!(s.duration_08<0.0F)&&!(s.elapsed_10<=s.duration_08))s.active_18=0;
            }
        }
        const auto w=write(v,port,small_on,large);
        if(writes)(*writes)[static_cast<std::size_t>(port)]=w;
    }
}

void stop_pad_vibration(PadVibration& v,std::array<PadActuatorWrite,2>* writes){
    for(auto& port:v.slots)for(auto& s:port)s.active_18=0;
    for(std::int32_t port=0;port<2;++port){
        const auto w=write(v,port,0,0);
        if(writes)(*writes)[static_cast<std::size_t>(port)]=w;
    }
}

void enable_pad_vibration(PadVibration& v,std::uint8_t enabled){v.enabled_4ff028=enabled;}
}  // namespace omega
