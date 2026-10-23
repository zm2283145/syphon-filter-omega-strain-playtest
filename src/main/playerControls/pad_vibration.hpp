#pragma once
#include <array>
#include <cstdint>

// The pad vibration manager (decomp src/main/func_0026BC20.c and neighbours; no TU name). Two ports of six
// effect slots at 004feed0 (0x1c bytes each, port stride 0xa8), the tick 004ff020 right after them, the
// enable byte 004ff028 and the serial counter 0048b270 (.data, initially 1).
//   0026bde0 start(port, small, large, period, f12 duration): the port's first inactive slot; returns its
//            serial (counter++), or 0 when all six are active
//   0026bc20 update(f12 dt): tick++; per port, when enabled, each active slot's elapsed += dt; on
//            tick % period == 0 it adds its large word and ors its small byte; a slot with duration >= 0 and
//            elapsed > duration goes inactive after contributing; then 00369920(pad, small, large) (with
//            (0, 0) when disabled)
//   0026bd60 stop: every slot inactive, then 00369920(pad, 0, 0) for both ports
//   0026be70 enable(byte)
//   00369920 the DualShock 2 actuator bytes for 0026bb28(*(pad +100), 1, {3}, 2, bytes): byte 0 = small
//            != 0 plus (large << 1) & 0xfe, byte 1 = (large & 0x80) != 0 (the small motor on / off and the
//            large motor's speed large & 0xff)
namespace omega {
struct PadVibrationSlot {
    std::uint8_t small_00{};
    std::int32_t large_04{};
    float duration_08{};          // < 0: until stopped
    std::int32_t period_0c{};
    float elapsed_10{};
    std::int32_t serial_14{};
    std::uint8_t active_18{};
};

// One 00369920 call: the pad (0055a340 + port * 0x1c0) and its two actuator bytes.
struct PadActuatorWrite {
    std::int32_t port{};
    std::array<std::uint8_t,2> bytes{};
    bool small_on() const{return (bytes[0]&1)!=0;}
    std::uint8_t large() const{return static_cast<std::uint8_t>((bytes[0]>>1)|((bytes[1]&1)<<7));}
};

struct PadVibration {
    std::array<std::array<PadVibrationSlot,6>,2> slots{};   // 004feed0
    std::int32_t tick_4ff020{};
    std::uint8_t enabled_4ff028{};
    std::int32_t serial_48b270{1};
    std::array<PadActuatorWrite,2> last{PadActuatorWrite{0,{}},PadActuatorWrite{1,{}}};   // each port's last write
};

std::array<std::uint8_t,2> pad_actuator_bytes(std::uint8_t small_on,std::int32_t large);   // 00369920
// The calls append their 00369920 writes (port order) to `writes` when given.
std::int32_t start_pad_vibration(PadVibration& v,std::int32_t port,std::uint8_t small_on,std::int32_t large,
    std::int32_t period,float duration);                                                 // 0026bde0
void update_pad_vibration(PadVibration& v,float dt,std::array<PadActuatorWrite,2>* writes=nullptr);   // 0026bc20
void stop_pad_vibration(PadVibration& v,std::array<PadActuatorWrite,2>* writes=nullptr);             // 0026bd60
void enable_pad_vibration(PadVibration& v,std::uint8_t enabled);                                     // 0026be70
}  // namespace omega
