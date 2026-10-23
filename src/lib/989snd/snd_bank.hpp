#pragma once

// The bank sound player of the IOP sound driver 989snd (IRX\989NOMID.IRX),
// checkpoint 119k: the SBlk bank head relocation (97e0), the name lookups
// ca98 / cb0c / cca4, the play command be54 (cmd 0x5c) -> 3fa8, the user data
// bb84 (cmd 0x56), the grain sequencer 6214 / 4774 with the grain handlers of
// the table 0x1d830 the TORONTO1 banks use, the sound handler pool d750 /
// d968 / d82c / d9dc / da80 / dd44 / e3e4 / 4838, the restart c268 (volume /
// pan 6358, the pan glide 2bbc / 2ef0 on the effect pool 0x2386c), the voice
// allocator 18a58, key on / off 18d60 / 19740 / 198f8 / 19264 / 1937c, the
// group volume refresh 1af50 and the sound tick f884.
//
// The port is literal: it works on an emulated IOP memory holding the IRX
// .rodata / .data / .bss at their vaddrs (the image at 0, no relocation:
// tools/ee/irx_disasm.py) and bank heads placed by the host, so every
// structure (handlers 0x1ef6c x 0x124, voices 0x2535c x 0x38, the masks at
// 0x1e544..) is byte-compatible with the IRX and golden-tested against it
// (tools/ee/gen_snd_bank.py, tests/snd_bank_goldens.hpp). libsd (LIBSD.IRX)
// is behind SpuInterface; sceSdNote2Pitch (LIBSD 3890) is ported here.
// Paths the TORONTO1 banks never reach (MIDI banks c004, SBv2 heads,
// reserved voices 1886c, noise voices, LFOs, the reverb countdown d598,
// stream handles in the bank calls, effect callbacks other than the pan
// glide) throw naming the address.

#include "snd_volume.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace omega::snd989 {

// The libsd entry points 989NOMID.IRX imports (stubs 1b9e8..1ba20).
class SpuInterface {
public:
    virtual ~SpuInterface() = default;
    virtual void set_param(std::uint16_t entry, std::uint16_t value) = 0;   // 1b9e8 sceSdSetParam
    virtual std::uint16_t get_param(std::uint16_t entry) = 0;               // 1b9f0 sceSdGetParam
    virtual void set_switch(std::uint16_t entry, std::uint32_t value) = 0;  // 1b9f8 sceSdSetSwitch
    virtual std::uint32_t get_switch(std::uint16_t entry) = 0;              // 1ba00 sceSdGetSwitch
    virtual void set_addr(std::uint16_t entry, std::uint32_t value) = 0;    // 1ba08 sceSdSetAddr
};

// LIBSD.IRX 3890 sceSdNote2Pitch(center_note, center_fine, note, fine).
std::uint16_t note_to_pitch(std::uint16_t center_note, std::uint16_t center_fine, std::uint16_t note,
                            std::int16_t fine);

// A section of an IRX (a relocatable MIPS ELF loaded at 0): its vaddr and bytes.
struct IrxSection {
    std::uint32_t addr = 0;
    std::span<const std::uint8_t> bytes;
};
IrxSection irx_section(std::span<const std::uint8_t> irx, const std::string& name);

class BankEngine {
public:
    static constexpr std::uint32_t memory_size = 0x80000;
    static constexpr std::uint32_t handlers = 0x1ef6c;     // 0x40 x 0x124
    static constexpr std::uint32_t effects = 0x2386c;      // 0x40 x 0x24
    static constexpr std::uint32_t voices = 0x2535c;       // 48 x 0x38
    static constexpr std::uint32_t scratch = 0x2ff00;      // host scratch (cmd structs, names, user data)
    static constexpr std::uint32_t heads = 0x30000;        // bank heads placed from here

    // rodata / data: the IRX sections with their vaddrs (BSS zeroed).
    BankEngine(std::uint32_t rodata_addr, std::span<const std::uint8_t> rodata, std::uint32_t data_addr,
               std::span<const std::uint8_t> data, SpuInterface& spu, VolumeState& volume);

    // 182cc's voice state (48 voices, groups [0, 47]) as left by the init.
    void init_voices();
    // A bank file's SBlk head copied to `addr`, 93ac's pre-steps (+24 the SPU
    // address, flags &= ~4, the sound / grain offsets made absolute once),
    // then 97e0. Returns the head address.
    std::uint32_t load_bank(std::span<const std::uint8_t> file, std::uint32_t addr, std::uint32_t spu_addr);
    void relocate_bank(std::uint32_t head);  // 97e0 (SBlk path 98c4)
    // 93ac without a supplied SPU address: the body's sound RAM from the
    // allocator (+8 bit 4: ed70, else f0d8 at +24). Returns the head, 0 when
    // the sound RAM is short (error 0x1a / 0x1b).
    std::uint32_t load_bank(std::span<const std::uint8_t> file, std::uint32_t addr);

    // The sound-RAM allocator (989snd's free-block tree, see snd_bank.cpp).
    void sram_init();                                                       // ed20
    std::uint32_t sram_alloc(std::uint32_t size);                           // ed70
    std::uint32_t sram_alloc_aligned(std::uint32_t size);                   // ee48
    void sram_free(std::uint32_t addr, std::uint32_t size);                 // ef68
    std::uint32_t sram_reserve(std::uint32_t addr, std::uint32_t size);     // f0d8
    std::uint32_t sram_free_total() const;                                  // f44c
    std::uint32_t sram_free_largest() const;                                // f48c
    // d0e4's sound-RAM part (the reverb work area of a core: the first set-up, or the same type again, d140).
    std::int32_t reverb_area(std::int32_t core, std::int32_t type);

    // be54 (cmd 0x5c): the command struct at `cmd` in memory. Returns the handle (0: none).
    std::uint32_t play(std::uint32_t cmd);
    // bb84 (cmd 0x56): the 16 user-data bytes of a sound to `out`; 1 if found.
    std::int32_t user_data(std::uint32_t bank, std::uint32_t name8, std::int32_t index, std::uint32_t name16,
                           std::uint32_t out);
    // c268 (cmd 0x21): a handle's volume / pan (flags 1 / 2), pan glide (4).
    std::uint32_t restart(std::uint32_t handle, std::uint32_t flags, std::int32_t vol, std::int32_t pan,
                          std::int32_t a4, std::int32_t a5);
    void stop(std::uint32_t handle);           // c060 (cmd 0x15)
    std::uint32_t lookup(std::uint32_t handle);  // de2c
    // f884 with one pending tick. Returns true when it ran 1b148 (the duck,
    // every 4th timestamp), which the caller runs with voice_groups_playing().
    bool tick();
    // 1af50: VOLL / VOLR of the playing voices of the masked groups again.
    void reapply_groups(std::uint32_t groups);
    // 1b148's voice scan: groups with a voice in state 1 or 4 whose handler is
    // set and not paused (+16 & 2).
    std::uint32_t voice_groups_playing() const;
    // 10324 (cmd 0x62): the stream-side group range (s16 lo / hi at 0x1e2ac +
    // group * 4, groups below 16). Only the stream code reads that table
    // (16c90 / 17fd0); no ported function does.
    void set_stream_group(std::int32_t group, std::int32_t lo, std::int32_t hi);
    // e174's walk of the active handler list (cmd 0x61): c060 on each handler
    // of a masked group (1 << +17) whose handle has bit 31 set.
    void stop_groups(std::uint32_t mask);
    // e0a8 / e214's walk (cmds 0x16 / 0x17): a masked handler that is not
    // paused (pause) or is paused (resume) goes to dfd0 / e03c with children
    // 1: its child handlers (+2c, linked by +30) first, then +16 bit 2 set and
    // 19b80 (its voices' VOLL / VOLR / PITCH to 0), or 19e18 (the voices'
    // volumes and pitch again) and the bit cleared. A stream handle (type 4)
    // would go to 11468 / 116bc, not ported: it throws naming the address.
    void pause_groups(std::uint32_t mask, bool pause);
    // e2e0 (cmd 0x18): e3e4(h, 1, 1, 0) on every active handler with no
    // parent (+28) that is not stopping (+16 bit 2), the next read after the
    // call, then 198f8 on the masks {-1, -1} with callback 1. The IRX then
    // waits (1b91c, 0x80e8 us at a time) until the active list is empty; the
    // caller runs the ticks of that wait.
    void stop_all();
    // 8e30 (cmd 6) on a bank head: 0 / -1 error 0x14; an SBlk head 8f7c ->
    // 900c (in the bank list 0x1d990, else error 0x16), then 9b8c: c978(head,
    // 1) (e5d4(sound, 1, 0) on each of the +16 sounds), the unlink (+30) and
    // ef68(+24, +2c) when +2c is set. The callback *0x1c614 is null (only the
    // IOP export 2194 sets it) and the head's memory (*0x1d984, FreeSysMemory)
    // is the caller's. Returns true when the head was unloaded.
    bool unload_bank(std::uint32_t head);
    // The bank list 0x1d990 (linked by +30): head addresses.
    std::vector<std::uint32_t> banks() const;
    // The active handler list (0x1e1f0, linked by +24): handle words.
    std::vector<std::uint32_t> active_handles() const;

    // 19fd0: sceSdNote2Pitch on the tone's centre (a non-negative centre note is a 44.1 kHz sample).
    std::uint16_t tone_pitch(std::int32_t center_note, std::int32_t center_fine, std::uint16_t note,
                             std::int16_t fine) const;                                 // 19fd0
    // 6cbc: a tone's voice priority at a volume (the tone's +0 priority, +e flags bits 1 / 2).
    std::int32_t priority(std::int32_t vol, std::uint32_t tone) const;                 // 6cbc
    // 6c28: the pan of a distance-attenuated sound.
    std::int32_t distance_pan(std::int32_t pan, std::int32_t vol) const;               // 6c28

    std::uint32_t lookup_bank_by_name(std::uint32_t name8);                                    // ca98
    std::int32_t lookup_sound(std::uint32_t bank, std::uint32_t name16, std::uint32_t out);   // cb0c
    std::uint32_t random();                                                                    // ba74

    std::uint8_t r8(std::uint32_t a) const { return mem_[a & mask]; }
    std::uint16_t r16(std::uint32_t a) const;
    std::uint32_t r32(std::uint32_t a) const;
    std::int8_t s8(std::uint32_t a) const { return static_cast<std::int8_t>(r8(a)); }
    std::int16_t s16(std::uint32_t a) const { return static_cast<std::int16_t>(r16(a)); }
    std::int32_t s32(std::uint32_t a) const { return static_cast<std::int32_t>(r32(a)); }
    void w8(std::uint32_t a, std::uint32_t v) { mem_[a & mask] = static_cast<std::uint8_t>(v); }
    void w16(std::uint32_t a, std::uint32_t v);
    void w32(std::uint32_t a, std::uint32_t v);
    std::vector<std::uint8_t>& memory() { return mem_; }
    const std::vector<std::uint8_t>& memory() const { return mem_; }
    // 20e8 error codes reported (the error callback *0x1c7ec is null).
    const std::vector<std::int32_t>& errors() const { return errors_; }

private:
    static constexpr std::uint32_t mask = memory_size - 1;

    void error(std::int32_t code);                                                     // 20e8
    std::uint32_t sram_find(std::uint32_t node, std::uint32_t size) const;             // f414
    void sram_insert(std::uint32_t node);                                              // f380
    void sram_unlink(std::uint32_t node);                                              // f2c8
    std::int32_t voice_lock(std::int32_t wait, std::int32_t owner);                    // 18538
    void voice_unlock();                                                               // 1860c
    std::uint32_t voice_addr(std::int32_t v) const;
    void voice_play(std::int32_t v);                                                   // 1a10c
    void voice_reset(std::int32_t v);                                                  // 1a1c8
    void voice_list(std::int32_t v, std::int32_t clear);                               // 18760
    std::int32_t alloc_voice(std::int32_t group, std::int32_t prio, std::int32_t flag);  // 18a58
    void voice_callback(std::int32_t v, std::int32_t code);                            // V+10 (6ad4)
    void note_pitch_offset(std::uint32_t tone, std::int32_t bend, std::int32_t b, std::int32_t c, std::int32_t d,
                           std::int32_t& out1, std::int32_t& out2) const;              // 1a070
    void key_on(std::int32_t v, std::int32_t noise);                                   // 18d60
    void flush_key_on();                                                               // 19264
    void flush_key_off();                                                              // 1937c
    void voice_ended(std::int32_t v);                                                  // 1948c
    void voice_silent(std::int32_t v);                                                 // 19620
    void key_off_pending(std::int32_t core, std::int32_t vi);                          // 1a334
    void silence(std::uint32_t masks);                                                 // 19a94
    void key_off(std::uint32_t masks, std::int32_t callback);                          // 19740
    void key_off_now(std::uint32_t masks, std::int32_t callback);                      // 198f8
    std::int32_t next_voice(std::uint32_t h, std::int32_t from) const;                 // e6a8
    void voice_done(std::int32_t v, std::uint32_t h);                                  // 6ad4
    void set_bend(std::uint32_t handle, std::int32_t bend);                            // 6764
    void apply_bend(std::uint32_t h);                                                  // 685c
    void pause_voices(std::uint32_t h);                                                // 19b80
    void resume_voices(std::uint32_t h);                                               // 19e18
    void pause_handler(std::uint32_t h, std::int32_t children);                        // dfd0
    void resume_handler(std::uint32_t h, std::int32_t children);                       // e03c

    void unlink_lfos(std::uint32_t h);                                                 // 23e8 / 2428
    std::uint32_t pool_alloc(std::uint32_t pool, std::int32_t count, std::int32_t type, std::uint32_t stride);  // d750
    std::int32_t instance_limit(std::uint32_t sound, std::int32_t vol, std::int32_t inst, std::uint32_t& out);  // d82c
    std::uint32_t alloc_handler(std::uint32_t sound, std::int32_t vol, std::int32_t inst);  // d968
    void activate(std::uint32_t h);                                                    // d9dc
    void free_handler(std::uint32_t h, std::int32_t force);                            // da80
    void add_child(std::uint32_t parent, std::uint32_t handle);                        // dd44
    void stop_handler(std::uint32_t h, std::int32_t children, std::int32_t now, std::int32_t a3);  // e3e4
    void stop_handle(std::uint32_t handle, std::int32_t a1, std::int32_t a2, std::int32_t a3);     // df4c
    void stop_sound(std::uint32_t sound, std::int32_t a1, std::int32_t a2);            // e5d4
    std::int32_t release(std::uint32_t h, std::int32_t force, std::int32_t a2);        // 4838
    std::int32_t release_grain(std::uint32_t h) const;                                 // 4990
    std::int32_t step(std::uint32_t h);                                                // 6214
    std::int32_t grain(std::uint32_t h, std::uint32_t sound, std::uint32_t g);         // *(0x1d830 + type * 4)
    std::int32_t sound_tick(std::uint32_t h);                                          // 4774
    std::int32_t tone_grain(std::uint32_t h, std::uint32_t sound, std::uint32_t g);    // 4a08
    std::int32_t child_grain(std::uint32_t h, std::uint32_t g);                        // 5db0
    std::uint32_t play_sound(std::uint32_t bank, std::int32_t index, std::int32_t vol, std::int32_t pan,
                             std::uint32_t cmd);                                       // 3fa8
    void handlers_tick();                                                              // e7d0
    void set_vol_pan(std::uint32_t handle, std::int32_t vol, std::int32_t pan);        // c1d4
    void handler_vol_pan(std::uint32_t handle, std::int32_t vol, std::int32_t pan, std::int32_t a3);  // 6358
    void glide(std::uint32_t handle, std::int32_t target, std::int32_t dir, std::int32_t time, std::int32_t mode);  // 2bbc
    std::int32_t glide_tick(std::uint32_t e, std::uint32_t h);                         // 2ef0
    std::int32_t effect_tick(std::uint32_t e, std::uint32_t h);                        // e8b0
    void effect_free_chain(std::uint32_t e);                                           // e954
    void effect_remove(std::uint32_t h, std::uint32_t m);                              // e990
    std::uint32_t effect_find(std::uint32_t h, std::uint32_t m) const;                 // e9c4
    std::uint32_t effect_alloc();                                                      // e9fc
    void effect_append(std::uint32_t h, std::uint32_t e);                              // ea44
    void effect_unlink(std::uint32_t h, std::uint32_t e);                              // ea8c

    std::vector<std::uint8_t> mem_;
    SpuInterface& spu_;
    VolumeState& volume_;
    std::vector<std::int32_t> errors_;
    std::int32_t sema_count_ = 1;
    std::int32_t cmd_depth_ = 0;  // 5db0 recursion level (host command stack)
};

}  // namespace omega::snd989
