#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// Host controllers translated into the DualShock 2 report the game reads.
// The pad manager (0055a300, pad k at 0055a340 + k*0x1c0) reads 18-byte
// reports (00369cd0, research/CROUCH_INPUT.md):
//   bytes 0/1   buttons, active low; held mask 0055a44c = (b0 << 8 | b1) ^ 0xffff
//   bytes 2/3   right stick X/Y, bytes 4/5 left stick X/Y (0 = left/up, 0xff =
//               right/down; 00369d40 reads sp+0x24/0x25 and subtracts 127.5)
//   bytes 6..17 pressures Right, Left, Up, Down, Triangle, Circle, Cross, Square,
//               L1, R1, L2, R2; word 0055a48c + 4k takes byte 6+k only while the
//               held bit from 004bbe20 is set.
// Button mapping (host choice, not game data): XInput A/B/X/Y = Cross/Circle/
// Square/Triangle, LB/RB = L1/R1, LT/RT = L2/R2 (analog pressure), Back =
// Select, Start = Start, stick clicks = L3/R3. DualShock 4 / DualSense: the
// PlayStation layout as printed, Share/Create = Select, Options = Start; the PS
// button and touchpad have no DualShock 2 counterpart. DualShock 3: every
// button and pressure maps 1:1.
namespace omega {
namespace ds2 {
inline constexpr std::uint16_t l2=0x0001,r2=0x0002,l1=0x0004,r1=0x0008,triangle=0x0010,circle=0x0020,
    cross=0x0040,square=0x0080,select=0x0100,l3=0x0200,r3=0x0400,start=0x0800,up=0x1000,right=0x2000,
    down=0x4000,left=0x8000;
enum Pressure:std::size_t{p_right,p_left,p_up,p_down,p_triangle,p_circle,p_cross,p_square,p_l1,p_r1,p_l2,p_r2};
// 004bbe20: the held-mask bit that gates pressure k.
inline constexpr std::array<std::uint16_t,12> pressure_mask{right,left,up,down,triangle,circle,cross,square,l1,r1,l2,r2};
}

struct Ds2Pad {
    std::uint16_t held{};                                   // 0055a44c layout
    std::uint8_t right_x{0x80},right_y{0x80},left_x{0x80},left_y{0x80};
    std::array<std::uint8_t,12> pressure{};                 // report bytes 6..17
    std::array<std::uint8_t,18> report() const;             // buttons active low
    static Ds2Pad from_report(const std::array<std::uint8_t,18>& report);
    bool down(std::uint16_t button) const{return (held&button)!=0;}
    // The 0055a48c + 4k word: the pressure byte while its held bit is set, else 0.
    std::uint8_t pressure_word(std::size_t k) const{return down(ds2::pressure_mask.at(k))?pressure.at(k):std::uint8_t{0};}
};

// XINPUT_GAMEPAD (wButtons, bLeftTrigger, bRightTrigger, sThumbLX/LY/RX/RY; Y up).
struct XInputGamepad {
    std::uint16_t buttons{};
    std::uint8_t left_trigger{},right_trigger{};
    std::int16_t lx{},ly{},rx{},ry{};
};
Ds2Pad ds2_from_xinput(const XInputGamepad& pad);

enum class SonyPad { dualshock3,dualshock4,dualsense };
// Vendor 054c: DualShock 3 0268; DualShock 4 05c4, 09cc and the wireless
// adapter 0ba0; DualSense 0ce6 and DualSense Edge 0df2.
std::optional<SonyPad> sony_pad_kind(std::uint16_t vendor,std::uint16_t product);
const char* sony_pad_name(SonyPad kind);
// One HID input report, report ID first, as ReadFile returns it (padded to the
// collection's input report length). `bluetooth` selects the DualSense's
// Bluetooth "simple" 0x01 layout. Reports without pad state give nullopt.
std::optional<Ds2Pad> ds2_from_sony_report(SonyPad kind,bool bluetooth,std::span<const std::uint8_t> report);

// The DualShock 2 actuators of 00369920 (the small motor on / off, the large motor's speed 0..255) on the
// host's motors. Host choice, not game data: the large motor drives the strong (left, low-frequency)
// motor and the small one the weak (right, high-frequency) motor at full speed.
struct XInputVibration { std::uint16_t left{},right{}; };   // XINPUT_VIBRATION
XInputVibration xinput_vibration(bool small_on,std::uint8_t large);
// The HID output report that sets only the motors, report ID first. USB reports take `length` (the
// collection's output report length; empty when it is too short); Bluetooth ones are 78 bytes ending in the
// CRC-32 of 0xa2 and the report. The DualShock 3's USB report 0x01 (the hid-sony / DsHidMini layout the input
// side reads) also sets the LEDs; it keeps LED 1 lit and ignores `bluetooth`.
std::vector<std::uint8_t> sony_vibration_report(SonyPad kind,bool bluetooth,std::size_t length,bool small_on,std::uint8_t large);
}  // namespace omega
