#include "gamepad_reports.hpp"

#include <algorithm>
#include <iterator>

namespace omega {
namespace {
using namespace ds2;

void press(Ds2Pad& pad,std::uint16_t button,bool on,std::uint8_t pressure=0xff){
    if(!on)return;
    pad.held|=button;
    for(std::size_t k=0;k<pressure_mask.size();++k)
        if(pressure_mask[k]==button)pad.pressure[k]=pressure;
}

// The 4-bit hat of the DualShock 4 / DualSense (0 = N, clockwise; 8+ = released).
void hat(Ds2Pad& pad,std::uint8_t value){
    value&=0x0f;
    press(pad,up,value==0||value==1||value==7);
    press(pad,right,value>=1&&value<=3);
    press(pad,down,value>=3&&value<=5);
    press(pad,left,value>=5&&value<=7);
}

// The DualShock 4 / DualSense button bytes: b0 hat + Square 0x10, Cross 0x20,
// Circle 0x40, Triangle 0x80; b1 L1 0x01, R1 0x02, L2 0x04, R2 0x08,
// Share/Create 0x10, Options 0x20, L3 0x40, R3 0x80.
Ds2Pad playstation_layout(std::uint8_t lx,std::uint8_t ly,std::uint8_t rx,std::uint8_t ry,
    std::uint8_t b0,std::uint8_t b1,std::uint8_t l2_analog,std::uint8_t r2_analog){
    Ds2Pad pad;
    pad.left_x=lx;pad.left_y=ly;pad.right_x=rx;pad.right_y=ry;
    hat(pad,b0);
    press(pad,square,(b0&0x10)!=0);
    press(pad,cross,(b0&0x20)!=0);
    press(pad,circle,(b0&0x40)!=0);
    press(pad,triangle,(b0&0x80)!=0);
    press(pad,l1,(b1&0x01)!=0);
    press(pad,r1,(b1&0x02)!=0);
    press(pad,l2,(b1&0x04)!=0,l2_analog);
    press(pad,r2,(b1&0x08)!=0,r2_analog);
    press(pad,select,(b1&0x10)!=0);
    press(pad,start,(b1&0x20)!=0);
    press(pad,l3,(b1&0x40)!=0);
    press(pad,r3,(b1&0x80)!=0);
    return pad;
}

// DualShock 4 USB / Bluetooth-short 0x01 (base 1) and Bluetooth 0x11 (base 3):
// LX LY RX RY b0 b1 b2 L2 R2.
std::optional<Ds2Pad> ds4_at(std::span<const std::uint8_t> r,std::size_t base){
    if(r.size()<base+9)return std::nullopt;
    return playstation_layout(r[base],r[base+1],r[base+2],r[base+3],r[base+4],r[base+5],r[base+7],r[base+8]);
}
// DualSense USB 0x01 (base 1) and Bluetooth 0x31 (base 2): LX LY RX RY L2 R2
// counter b0 b1 b2.
std::optional<Ds2Pad> dualsense_at(std::span<const std::uint8_t> r,std::size_t base){
    if(r.size()<base+10)return std::nullopt;
    return playstation_layout(r[base],r[base+1],r[base+2],r[base+3],r[base+7],r[base+8],r[base+4],r[base+5]);
}
}  // namespace

std::array<std::uint8_t,18> Ds2Pad::report() const{
    std::array<std::uint8_t,18> r{};
    const auto raw=static_cast<std::uint16_t>(held^0xffff);
    r[0]=static_cast<std::uint8_t>(raw>>8);r[1]=static_cast<std::uint8_t>(raw);
    r[2]=right_x;r[3]=right_y;r[4]=left_x;r[5]=left_y;
    for(std::size_t k=0;k<pressure.size();++k)r[6+k]=pressure[k];
    return r;
}

Ds2Pad Ds2Pad::from_report(const std::array<std::uint8_t,18>& r){
    Ds2Pad pad;
    pad.held=static_cast<std::uint16_t>((r[0]<<8|r[1])^0xffff);
    pad.right_x=r[2];pad.right_y=r[3];pad.left_x=r[4];pad.left_y=r[5];
    for(std::size_t k=0;k<pad.pressure.size();++k)pad.pressure[k]=r[6+k];
    return pad;
}

Ds2Pad ds2_from_xinput(const XInputGamepad& in){
    Ds2Pad pad;
    const auto b=in.buttons;
    press(pad,up,(b&0x0001)!=0);
    press(pad,down,(b&0x0002)!=0);
    press(pad,left,(b&0x0004)!=0);
    press(pad,right,(b&0x0008)!=0);
    press(pad,start,(b&0x0010)!=0);
    press(pad,select,(b&0x0020)!=0);
    press(pad,l3,(b&0x0040)!=0);
    press(pad,r3,(b&0x0080)!=0);
    press(pad,l1,(b&0x0100)!=0);
    press(pad,r1,(b&0x0200)!=0);
    press(pad,cross,(b&0x1000)!=0);
    press(pad,circle,(b&0x2000)!=0);
    press(pad,square,(b&0x4000)!=0);
    press(pad,triangle,(b&0x8000)!=0);
    // XINPUT_GAMEPAD_TRIGGER_THRESHOLD (30) as the digital L2/R2 bit.
    press(pad,l2,in.left_trigger>30,in.left_trigger);
    press(pad,r2,in.right_trigger>30,in.right_trigger);
    // Signed thumb (Y up) to the report byte (0 = left/up).
    auto x=[](std::int16_t v){return static_cast<std::uint8_t>((static_cast<int>(v)+32768)>>8);};
    auto y=[](std::int16_t v){return static_cast<std::uint8_t>((32767-static_cast<int>(v))>>8);};
    pad.left_x=x(in.lx);pad.left_y=y(in.ly);pad.right_x=x(in.rx);pad.right_y=y(in.ry);
    return pad;
}

std::optional<SonyPad> sony_pad_kind(std::uint16_t vendor,std::uint16_t product){
    if(vendor!=0x054c)return std::nullopt;
    switch(product){
    case 0x0268:return SonyPad::dualshock3;
    case 0x05c4:case 0x09cc:case 0x0ba0:return SonyPad::dualshock4;
    case 0x0ce6:case 0x0df2:return SonyPad::dualsense;
    default:return std::nullopt;
    }
}

const char* sony_pad_name(SonyPad kind){
    switch(kind){
    case SonyPad::dualshock3:return "DualShock 3";
    case SonyPad::dualshock4:return "DualShock 4";
    case SonyPad::dualsense:return "DualSense";
    }
    return "Sony controller";
}

std::optional<Ds2Pad> ds2_from_sony_report(SonyPad kind,bool bluetooth,std::span<const std::uint8_t> r){
    if(r.empty())return std::nullopt;
    const auto id=r[0];
    switch(kind){
    case SonyPad::dualshock3:{
        // 0x01: [2] Select L3 R3 Start Up Right Down Left and [3] L2 R2 L1 R1
        // Triangle Circle Cross Square (the DualShock 2 bytes, active high),
        // [6..9] LX LY RX RY, [14..25] pressures Up Right Down Left L2 R2 L1 R1
        // Triangle Circle Cross Square.
        if(id!=0x01||r.size()<26)return std::nullopt;
        Ds2Pad pad;
        pad.held=static_cast<std::uint16_t>(r[2]<<8|r[3]);
        pad.left_x=r[6];pad.left_y=r[7];pad.right_x=r[8];pad.right_y=r[9];
        static constexpr std::array<std::size_t,12> at{15,17,14,16,22,23,24,25,20,21,18,19};
        for(std::size_t k=0;k<at.size();++k)pad.pressure[k]=r[at[k]];
        return pad;
    }
    case SonyPad::dualshock4:
        if(id==0x01)return ds4_at(r,1);
        if(id==0x11)return ds4_at(r,3);
        return std::nullopt;
    case SonyPad::dualsense:
        if(id==0x01)return bluetooth?ds4_at(r,1):dualsense_at(r,1);
        if(id==0x31)return dualsense_at(r,2);
        return std::nullopt;
    }
    return std::nullopt;
}

XInputVibration xinput_vibration(bool small_on,std::uint8_t large){
    return {static_cast<std::uint16_t>(large*257U),static_cast<std::uint16_t>(small_on?0xffffU:0U)};
}

std::vector<std::uint8_t> sony_vibration_report(SonyPad kind,bool bluetooth,std::size_t length,bool small_on,std::uint8_t large){
    const std::uint8_t weak=small_on?0xff:0;
    std::vector<std::uint8_t> r;
    // Report layouts as the Linux hid-sony / hid-playstation drivers and SDL's HIDAPI drivers write them.
    if(kind==SonyPad::dualshock4&&!bluetooth){
        if(length<32)return r;
        r.assign(length,0);r[0]=0x05;r[1]=0x01;r[4]=weak;r[5]=large;           // flag 0x01: the motors only
    }else if(kind==SonyPad::dualshock4){
        r.assign(78,0);r[0]=0x11;r[1]=0xc0;r[3]=0x01;r[6]=weak;r[7]=large;     // 0xc0: HID report with CRC
    }else if(kind==SonyPad::dualsense&&!bluetooth){
        if(length<48)return r;
        r.assign(length,0);r[0]=0x02;r[1]=0x03;r[3]=weak;r[4]=large;           // 0x03: compatible vibration
    }else if(kind==SonyPad::dualsense){
        r.assign(78,0);r[0]=0x31;r[1]=0x02;r[2]=0x03;r[4]=weak;r[5]=large;
    }else if(kind==SonyPad::dualshock3){
        // 0x01: [2]/[4] the right/left motor durations (0xff: until changed), [3] the right motor on, [5] the left
        // motor's force, [10] the LED bitmap (LED 1, 0x02) and [11..30] the four LED timings. The report also sets
        // the LEDs, so it keeps LED 1 lit.
        if(length<36)return r;
        static constexpr std::uint8_t base[36]{0x01,0x01,0xff,0x00,0xff,0x00,0x00,0x00,0x00,0x00,0x02,
            0xff,0x27,0x10,0x00,0x32,0xff,0x27,0x10,0x00,0x32,0xff,0x27,0x10,0x00,0x32,0xff,0x27,0x10,0x00,0x32};
        r.assign(length,0);std::copy(std::begin(base),std::end(base),r.begin());
        r[3]=small_on?1:0;r[5]=large;
        return r;
    }else return r;
    if(bluetooth){
        std::uint32_t c=0xffffffffU;
        auto feed=[&c](std::uint8_t b){c^=b;for(int k=0;k<8;++k)c=(c>>1)^(0xedb88320U&(0U-(c&1U)));};
        feed(0xa2);
        for(std::size_t k=0;k<74;++k)feed(r[k]);
        c=~c;
        for(std::size_t k=0;k<4;++k)r[74+k]=static_cast<std::uint8_t>(c>>(8*k));
    }
    return r;
}
}  // namespace omega
