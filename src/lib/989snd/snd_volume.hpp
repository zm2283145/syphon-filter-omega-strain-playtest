#pragma once

// The volume, pan and duck code of the IOP sound driver 989snd
// (IRX\989NOMID.IRX, "989snd ... NO MIDI VERSION", the module the EE library
// 003a6xxx..003a8xxx talks to), checkpoint 119j. Addresses are IRX vaddrs
// (the image at 0, tools/ee/irx_disasm.py); every function is golden-tested
// on the IRX code run by the EE interpreter (tools/ee/gen_snd_irx.py,
// tests/snd989_goldens.hpp).
//
//   1ae88  AdjustVolToGroup: (master[g] * duck[g]) >> 16, v * m / 1024 and a
//          squared law v * v / 0x7fff (the SPU voice volume is this >> 1)
//   1a560  MakeVolume (vol1, pan1, vol2, pan2, vol3, pan3, out): the pan table
//          0x1dbf0 (*0x1dbec) and the phase rule on the previous out volumes
//   1a884  the 4-volume variant used by the stream voices (00016664, 00017b44)
//   1a408  the group master volume (0..0x400; group 16 the SPU master)
//   1b3b8  a ducker definition (0x25ddc, 4 x 0x18)
//   1b148  the duck tick (every 4th sound tick, 0000fb74)
//   1a3b8  the duck reset (sources -1, duck[15] = 0x10000)
//   3b1c   the stream volume fade (cmd 0x22), e8b0 / 3e1c its tick and step

#include <array>
#include <cstdint>
#include <optional>

namespace omega::snd989 {

// 0x1dbf0: 181 {s16, s16} pairs; [k] = trunc(16383 * (cos, sin)(k/180 * 90 deg)) for k < 180, [180] = (16383, 0).
const std::array<std::array<std::int16_t, 2>, 181>& pan_table();

struct Ducker {                // 0x25ddc + i * 0x18
    std::int32_t source = -1;  // +0  the volume group whose playing voices duck
    std::int32_t targets{};    // +4  the ducked groups (bit mask)
    std::int32_t full{};       // +8  the full duck multiplier (0x10000 = none)
    std::int32_t attack{};     // +c  the step per duck tick towards +8
    std::int32_t release{};    // +10 the step per duck tick back to 0x10000
    std::int32_t current{};    // +14
};

struct VolumeState {
    std::array<std::int32_t, 17> master{};  // 0x1e41c (.data: 0x400 each)
    std::array<std::int32_t, 15> duck{};    // 0x1e49c
    std::array<Ducker, 4> duckers{};        // 0x25ddc
    std::int16_t play_mode{};               // 0x1e51c (1 mono; 2 skips the rear phase rule)
    VolumeState();
};

void duck_reset(VolumeState& s);                                                              // 1a3b8
std::int16_t adjust_vol_to_group(const VolumeState& s, std::int32_t vol, std::int32_t group); // 1ae88

// 1a408 without its side effects: the clamped volume is stored for groups
// other than 15 (15 posts message 0x53 through 000020e8 instead). The result
// carries the 1af50 voice-refresh mask (1 << group for groups other than 15
// and 16; the stream refresh 0000b3d4 follows it) and, for group 16, the SPU
// master register value (vol * 0x3fff) / 1024 written to 0x981 / 0xa81.
struct MasterResult {
    std::uint32_t refresh_mask{};
    bool posted_53{};
    std::int32_t spu_master = -1;
};
MasterResult set_master_volume(VolumeState& s, std::int32_t group, std::int32_t vol);

// 1b3b8: which >= 4 is ignored; def == nullptr clears the source.
void set_ducker(VolumeState& s, std::uint32_t which, const std::array<std::int32_t, 6>* def);

// 1b148: `active_groups` is the mask its voice scan builds (a voice whose
// state +8 is 1 or 4, whose handler +c is set and whose (handler +16 & 2) is
// clear contributes voice_group_bit(+1b)). Returns the changed duck groups
// (the 1af50 refresh mask).
std::uint32_t duck_tick(VolumeState& s, std::uint32_t active_groups);
std::uint32_t voice_group_bit(std::int8_t group);  // sllv: 1 << (group & 31)

// 17768 / 177ac: a stream's new volume (+10) and pan (+12) from a set-volume
// request (179e4, the restart 003a7f00 -> handler 000009cc -> c268 -> c1d4):
// volume 0x7fffffff keeps `old`, a negative volume is taken raw (negated),
// otherwise (vol * 127) >> 10, at most 127; pan -1 is 0, -2 keeps `old`, then
// wrapped into 0..359.
std::int32_t stream_volume(std::int32_t old, std::int32_t vol);
std::int32_t stream_pan(std::int32_t old, std::int32_t pan);

// The stream volume fade: IRX cmd 0x22 (EE 003a7ec0; handler 0a14 reads the
// handle word and the s16 of vol, time, mode) -> 3b1c, ticked on every 240 Hz
// sound tick by e7d0 -> e8b0 -> the record's callback 3e1c. A record is one
// of the 64 x 0x24 pool 0x2386c (e9fc), on the stream's +8 list (ea44 appends,
// ea8c unlinks, e9c4 / e990 find / remove the first with flags & 2). Only the
// volume fade record (flags & 2, callback 3e1c) is modelled; a stream holds at
// most one (3b1c reuses it).
struct FadeRecord {
    std::uint32_t flags{};     // +0  bit 0 used (e9fc), bit 1 the volume fade, bit 31 locked (skipped by e8b0)
    std::int32_t count{};      // +4  ticks until the callback
    std::int32_t countdown{};  // +8  1: +4 counts down each tick (else the callback runs on every tick)
    std::int32_t step{};       // +18 the volume step (|step| >= 2: in 1/32 volume units)
    std::int32_t reload{};     // +1c the +4 reload when +8 is 1
    std::int32_t target{};     // +20 the target volume; -4: stop the stream when it reaches 0
};
struct FadeStream {
    std::uint32_t handle{};          // +0 (type: bits 24..28; type 5 keeps its volume at +3a)
    std::int16_t scale{};            // +c (127 for a VAG stream, 0x108e4)
    std::int16_t volume_10{};        // +10
    std::int16_t volume_3a{};        // +3a
    std::optional<FadeRecord> fade;  // the stream's volume fade record
};
// What 3b1c / 3e1c call on the stream: c060 (the stop: e3e4(stream, 1, 0, 0))
// and c1d4(handle, set_volume, -2) (179e4 -> 17768 / 177ac: pan kept).
struct FadeCall {
    bool stop{};
    std::optional<std::int32_t> set_volume;
};
// 3b1c (de2c found the stream). `pool_free`: e9fc finds an unused record.
FadeCall fade_start(FadeStream& s, bool pool_free, std::int16_t vol, std::int16_t time, std::int16_t mode);
// 3e1c on the stream's record (removed by ea8c when it reaches its target):
// the c1d4 argument and its return (1: e7d0 stops the stream with e3e4).
struct FadeStep {
    std::int32_t set_volume{};
    bool stop{};
};
FadeStep fade_callback(FadeStream& s);
// e8b0 on the stream's record: the callback's step when it ran.
std::optional<FadeStep> fade_tick(FadeStream& s);

// The pan glide 2bbc(handle, target, dir, time, mode) on a stream (the
// restart c268 with flag 4 calls 2bbc(handle, pan, 0, 0x1e, 4)): de2c finds
// the stream, whose +0x12 is its pan. A record of the same pool 0x2386c with
// flags & 4 and callback 2ef0, ticked by e8b0 like the fade (e990 / e9c4 with
// mask 4). The record uses the FadeRecord layout: +18 the pan step (|step|
// >= 2: in 1/32 degrees), +20 the target pan. dir 0 takes the shorter way.
struct GlideStream {
    std::int16_t pan_12{};            // +0x12
    std::optional<FadeRecord> glide;  // the stream's pan glide record
};
// The c1d4(handle, 0x7fffffff, pan) that 2bbc / 2ef0 call (179e4: volume kept).
struct GlideCall {
    std::optional<std::int32_t> set_pan;
};
// 2bbc (de2c found the stream). `pool_free`: e9fc finds an unused record.
GlideCall glide_start(GlideStream& s, bool pool_free, std::int32_t target, std::int32_t dir, std::int32_t time, std::int32_t mode);
// 2ef0 on the stream's record (returns 0; ea8c unlinks it at the target).
GlideCall glide_callback(GlideStream& s);
// e8b0 on the stream's glide record: the callback's call when it ran.
std::optional<GlideCall> glide_tick(GlideStream& s);

// 1a560 and 1a884: `out` holds the voice's previous {left, right} on entry.
void make_volume(const VolumeState& s, std::int32_t vol1, std::int32_t pan1, std::int32_t vol2, std::int32_t pan2,
                 std::int32_t vol3, std::int32_t pan3, std::array<std::int16_t, 2>& out);
void make_volume4(const VolumeState& s, std::int32_t vol4, std::int32_t vol1, std::int32_t pan1, std::int32_t vol3,
                  std::int32_t pan2, std::int32_t vol2, std::int32_t pan3, std::array<std::int16_t, 2>& out);

}
