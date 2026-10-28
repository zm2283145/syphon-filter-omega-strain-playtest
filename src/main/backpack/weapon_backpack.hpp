#pragma once
// The backpack (weapon-drop) object of "backpack.cc" (0x318 bytes, vtable
// 004dc350, constructor 00257640), Checkpoint 121f. An NPC's dead state
// drops one (001a3520, npc_dead_drop.hpp); walking over it sends the
// pickup message *004f5440 to its receiver 00254600, which runs 001762c0 and
// the pickup PickupWpn 00254da0. Native, interpreter-verified
// (tests/weapon_backpack_goldens.hpp):
//   reset 00258330, the box block (00255a50 / 00255af0 / 00255cf0 / 00254da0
//   / 00256180: vtable +5c 003ce980, 0040c9d0, 00170e20, box +94), armor
//   00255a50 / 00255860, weapons 00255af0 / 00255cf0, the list 002553e0,
//   the ammunition query 00255500, take 002558a0, remove ammunition
//   00255650, PickupAmmo 00255290, PickupWpn 00254da0, the finish 00255f10
//   with the box priority 00255f60 and the empty check 00254990, the
//   receiver 00254600 and 001762c0 up to the Triangle confirm object.
// The object is kept as its raw bytes (the original has no bounds checks; a
// field access outside the 0x318 bytes throws). Calls into other systems
// are events in call order: 003ce980 (activate), 0040c9d0 (world
// register), 00170e20 (hotbox creation; the host returns the box, writes to
// it are box_word events), 00170db0 / 0040c9a0 / 003cb230 (box removal),
// 00254770 (destroy), the actor's virtual +c0 (position), 001314f0 +
// 00332f90 (the dual-pistol unlock 1 of the local player), 003fdb50
// (GetString), 00128bd0 ("%s %s" at 004a7b18) and 00242b00 (the HUD
// object *004ffc2c's message), 0020f860 + 003c9420 + 003c9d60 (a
// MSGGAME_PLAYSND of a sound descriptor at the position, to 004ffb50).
// Not ported (throw naming the address): the constructor's base object
// 003cf0e0 / id 0040c240 / vtable +90, the placement 002561d0, the box
// bounds 00253fb0 (the gearpack model's +3e0 box computed by VU0 micro
// code at model load, 0038d720), the multiplayer pickup sound send in
// 00254da0, and the default receiver 003cea10. Checkpoint 132fx5 hosts the
// remote-owner pickup request as an event. Checkpoint 132t: 001762c0 builds the Triangle
// confirm object 00253070 through WeaponBackpackHost::open_menu (the menu
// is pickup_menu.hpp, hosted in play by pickup_menu_play.hpp; without the
// host it still throws naming 00253070), and the menu's backpack callees
// cycle 00254ca0, slot_flag 002559d0 and mark_30c 00255a40.
#include "animation_root.hpp"
#include "weapon_database.hpp"
#include "weapon_inventory.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

inline constexpr std::size_t weapon_backpack_size=0x318;
inline constexpr std::uint32_t weapon_backpack_vtable=0x004dc350;
inline constexpr std::uint32_t backpack_string_pickup_wpn=0x004a7b20;    // "PickupWpn"
inline constexpr std::uint32_t backpack_string_pickup_armor=0x004a7b30;  // "PickupArmor"
inline constexpr std::uint32_t backpack_string_pickup_ammo=0x004a7b40;   // "PickupAmmo"
inline constexpr std::uint32_t backpack_format_pickup=0x004a7b18;        // "%s %s"
inline constexpr std::uint32_t backpack_sound_weapon=0x004f1af0;
inline constexpr std::uint32_t backpack_sound_ammo=0x004f17b0;
inline constexpr std::uint32_t backpack_sound_armor=0x004f1c90;

// Three slots at +64 + k x 0xd4: +0 the weapon slot byte (00143d20), four
// entries at +4 + j x 0x28 (spare rounds [4] at +0, clips [4] at +10, the
// item +20, the count of pairs +24), the entry count +cc and a flag byte
// +d0. Armor: +2e4 count, +2e8 armor, +2ec its maximum. +2f0 enabled,
// +2f1 flag-slot pickup, +2f2 menu, +2f3, +2f4; +2fc the hotbox, +300 the
// owner with its key +304, +308 the picker index, +30c, +310, +314 (float).
struct WeaponBackpack {
    std::array<std::uint8_t,weapon_backpack_size> bytes{};
    std::uint32_t word(std::size_t offset) const;
    std::int32_t value(std::size_t offset) const{return static_cast<std::int32_t>(word(offset));}
    void set_word(std::size_t offset,std::uint32_t v);
    std::uint8_t byte(std::size_t offset) const;
    void set_byte(std::size_t offset,std::uint8_t v);
    static std::size_t slot(std::size_t k){return 0x64+k*0xd4;}
    static std::size_t entry(std::size_t k,std::int64_t j);
};

struct WeaponBackpackEvent {
    enum class Kind {
        activate_5c,        // vtable +5c 003ce980(bp, 1)
        register_40c9d0,    // 0040c9d0(bp)
        create_box,         // 00170e20(*004ffbd0, bp, 0, 1) -> a (the box)
        box_word,           // box a +b = c
        box_not_simulated,  // the play diagnostic: the box block was skipped
        remove_box,         // 00170db0(*004ffbd0, box a)
        unregister_box,     // 0040c9a0(box a)
        release_box,        // 003cb230(box a, 0.1)
        destroy,            // 00254770(bp)
        position_c0,        // actor virtual +c0(out, actor, -1)
        unlock_query,       // 00332f90(001314f0(004ffb50, -1), 1)
        display_name,       // 003fdb50(record a +80)
        string,             // 003fdb50(a)
        format,             // 00128bd0(buf, a, ...)
        hud_message,        // 00242b00(*004ffc2c, buf)
        sound,              // 0020f860(msg, descriptor a, position) and its send
        pickup_request,     // multiplayer (132fx5): 003fb020(net id a = bp +c, InventoryMsg *005061b0 {+20 4, +24 2,
                            // +28 bp, +2c b = the picker's +68}, -1, c = bp +308), to the backpack's owner
    } kind{};
    std::uint32_t a{},b{},c{};
    RootVector position{};
    bool operator==(const WeaponBackpackEvent&) const=default;
};

// The actor at +300, read when +300 is nonzero (it is the owner when its
// +c equals +304).
struct WeaponBackpackOwner {
    std::int32_t key_c{};
    std::array<std::uint8_t,3> root{};       // +2c, +2d, +2e
    std::uint8_t pickupable_3468{},byte_3469{};
    bool items_3528{};                       // 004704f0(+3528)
};

struct WeaponBackpackHost {
    // 00170e20: the hotbox. Empty: not hosted (throws), unless
    // box_not_simulated (the play diagnostic, which skips the block).
    std::function<std::uint32_t()> create_box;
    bool box_not_simulated{};
    std::function<WeaponBackpackOwner(std::uint32_t actor)> owner;
    std::function<RootVector()> position_c0;
    std::function<bool()> dual_unlock;       // 00332f90(.., 1)
    bool multiplayer{};                      // 005721c8
    // 001762c0's Triangle confirm object: 0036b180(0xb8, "pickup_menu.cc",
    // 0x27a) and 00253070(mem, *(*004ffc2c + 0x54), bp, comp + 30,
    // owner_check, 0); returns the object for comp +178 (0: the allocation
    // failed). Empty: not hosted (throws naming 00253070).
    std::function<std::uint32_t(std::uint32_t owner_check)> open_menu;
};

// The picker's controller (actor +58, "comp") as the receiver reads it.
struct WeaponBackpackPicker {
    bool present{};                          // actor +58 non-null
    bool predicate_54{};                     // comp virtual +54
    std::uint8_t byte_14{};
    std::int32_t index_68{};                 // comp virtual +68
    std::uint8_t pickup_1bc{},confirm_17c{};
    std::uint32_t confirm_178{};
    std::int32_t word_184{};                 // comp +184 (input +134): owner_check = !(+184 > 0)
};

struct WeaponBackpackActor {
    WeaponInventoryOps* inventory{};         // *(actor +3524)
    std::int32_t armor_33a0{},max_armor_339c{};
};

struct WeaponBackpackAmmo {
    std::int32_t spare{},clip{},total{};
};

class WeaponBackpackOps {
public:
    WeaponBackpackOps(WeaponBackpack& backpack,const WeaponDatabase& database,const WeaponBackpackHost& host,
        std::vector<WeaponBackpackEvent>& events);
    void reset(bool keep,const WeaponInventory& player);                        // 00258330
    void ensure_box();                                                          // the box block
    void add_armor(std::int32_t armor,std::int32_t max_armor);                  // 00255a50
    bool take_armor(std::int32_t& armor,std::int32_t& max_armor);               // 00255860
    void add_weapon(std::int32_t item,std::int32_t spare,std::int32_t clip,std::uint8_t flag);   // 00255af0
    void mark_slot(std::int32_t item);                                          // 00255cf0
    std::vector<std::int32_t> list(const WeaponInventory& inventory) const;     // 002553e0
    std::optional<WeaponBackpackAmmo> ammunition(std::int32_t item) const;      // 00255500 (nullopt: no entry)
    bool take(std::int32_t item,bool test_only);                                // 002558a0
    std::int32_t remove_ammunition(std::int32_t item,std::int32_t rounds);      // 00255650
    bool pickup_ammunition(WeaponInventoryOps& inventory);                      // 00255290
    void pickup(WeaponBackpackActor& actor);                                    // 00254da0
    void box_priority();                                                        // 00255f60
    void check_empty();                                                         // 00254990
    void finish();                                                              // 00255f10
    // 00254600(bp, msg): the pickup message *004f5440 (any other message goes
    // to the default receiver 003cea10, not hosted: throws); returns 1.
    bool receive(bool pickup_message,WeaponBackpackPicker& picker,WeaponBackpackActor& actor);
    // 001762c0(comp, bp).
    void picker_update(WeaponBackpackPicker& picker,WeaponBackpackActor& actor);
    // 00254ca0(bp, item, previous): the item after (previous: before) item
    // among the entries of the first slot of its weapon slot that holds it,
    // wrapping; item itself when no slot holds it.
    std::int32_t cycle(std::int32_t item,bool previous) const;
    // 002559d0(bp, item): byte +d0 of the first slot of item's weapon slot (0: none).
    std::uint8_t slot_flag(std::int32_t item) const;
    // 00255a40(bp): +30c = 1.
    void mark_30c(){bp_.set_word(0x30c,1);}
    // 00255e50 (d90 false) / 00255d90 (d90 true): the valid owner (+300 set,
    // its +c == +304) has +2d, +2c set, +2e clear and +3468 (+3469) set.
    bool owner_state(bool d90) const;
    const WeaponBackpack& backpack() const{return bp_;}
private:
    std::int8_t slot_of(std::int32_t item) const;
    std::optional<WeaponBackpackOwner> valid_owner() const;
    void copy_last_over(std::size_t k,std::int64_t j);
    void event(WeaponBackpackEvent e){events_.push_back(e);}
    WeaponBackpack& bp_;
    const WeaponDatabase& db_;
    const WeaponBackpackHost& host_;
    std::vector<WeaponBackpackEvent>& events_;
};

}
