#pragma once
// The weapon inventory every actor holds at +3524 (weapon.cc, 0xf8 bytes;
// COMBAT.md §2): constructor 00144dc0, add 00143df0 with 001861f0, equip
// 00142c50, holster 00142bb0 / 00143b60 (switch gate 00143bc0), equip-first
// 00143c50, remove 00142450, clear 001423e0, game::SetPlayerWeapons
// 00185330, slot of an item 00143d20, the ammo queries 00143870 / 00143940
// and the ready test 00143400. Single player only: the multiplayer sends of
// add/remove (005721c8) are not reproduced. Resource loads, model attachment
// and sounds are returned as events in call order, not run; the actor's model
// attachment table (actor +60 +2410) is read through the host.
#include "air_taser_object.hpp"
#include "animation_root.hpp"
#include "weapon_database.hpp"
#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

inline constexpr std::uint8_t weapon_no_slot=6;     // +84 when nothing is held
inline constexpr std::int32_t weapon_default_item=0x67;   // 00144dc0 with an object

struct WeaponSlot {
    std::uint8_t byte_0{};          // constructor: 00489d70[k] < 8
    std::int32_t item{-1};          // +4
    std::int32_t clip{};            // +8 rounds in the clip
    std::int32_t spare{};           // +c spare rounds
    bool operator==(const WeaponSlot&) const=default;
};
// The 0xd0-byte object 00266630 creates at +0 for roles 1 and 3: the air
// taser (air_taser_object.hpp): +0 the record of a hold-class-5 item (an item
// ID here), +4 the actor, +8 the state the switch gate reads (0 after
// construction; 00262d90 dispatches on 1, 2, 3).
using WeaponInventoryObject=AirTaserObject;
// The torch's flicker object (+d8, torch_light.hpp): the host's handle and
// the object bytes 00144080 reads back (+2d shown; +2c is 1 and +2e 0 from
// 003cf0e0, so +2d alone decides), its effect id +60 and whether its
// emitter +64 exists.
struct TorchFlickerObject {
    std::uint32_t handle{};
    bool shown_2d{},emitter_64{};
    std::int32_t effect_60{-1};
    bool operator==(const TorchFlickerObject&) const=default;
};
// The held item as game::GetActiveWeapon 001855c0 reads it: +84 == 6 -> -1,
// else the slot's item (+8 + 16 * +84).
struct WeaponInventory;
std::int32_t weapon_active_item(const WeaponInventory& inventory);
struct WeaponInventory {
    std::optional<WeaponInventoryObject> object_0;
    std::array<WeaponSlot,8> slots{};
    std::uint8_t current_84{weapon_no_slot},previous_85{weapon_no_slot};
    std::uint64_t actor_88{};
    std::int32_t word_8c{};
    std::uint64_t burst_target_90{};            // weak handle object
    std::int32_t burst_target_id_94{-1};        // and its +c (-1 none)
    bool aim_view_98{};
    std::int32_t burst_subtarget_9c{};
    bool infinite_reserve_99{};                 // set by the AI init (reload keeps the spare rounds)
    bool reloading_a0{};
    std::int32_t selection_a4{};                // -1 after an equip or holster
    std::uint8_t byte_a8{},hand_a9{};
    std::array<float,2> cooldown_ac{};          // +ac/+b0 per hand
    std::array<float,2> value_b4{10000.0F,10000.0F};   // +b4/+b8, cleared per shot
    std::array<std::int32_t,2> burst_bc{};      // +bc/+c0 shots left in the burst per hand
    // +c4/+c8: the looping fire sound's record name pointer per hand (the item here; empty = off).
    std::array<std::optional<std::int32_t>,2> loop_sound_c4{};
    std::array<float,2> spinup_cc{};            // +cc/+d0
    // +d4: the torch's point light (an index into the point pool), +d8 its
    // flicker object (0 handle: none); both 0 from the constructor.
    // +dc / +e0 / +e4: the flicker target, level and rate, written when the
    // light is created. The constructor 00144dc0 does not write them and the
    // 0xf8-byte block comes from 0036b180, which does not clear it, so until
    // a light is taken they hold the heap's previous content. A full point
    // pool leaves them so while the emitter's 003a2680 reads +e0 (00144868).
    // The port's heap fill is 0xa5 (the particle definitions' convention,
    // EFFECTS.md); with it, as with a zero block, the scale is exactly 0.8.
    static constexpr std::uint32_t heap_fill_word=0xa5a5a5a5U;
    std::optional<std::size_t> torch_light_d4;
    TorchFlickerObject torch_flicker_d8;
    float torch_target_dc{std::bit_cast<float>(heap_fill_word)};
    float torch_level_e0{std::bit_cast<float>(heap_fill_word)};
    float torch_rate_e4{std::bit_cast<float>(heap_fill_word)};
    float value_e8{180.0F};
    std::uint8_t byte_ec{},byte_ed{},byte_ee{},busy_ef{},byte_f0{};
    // +f4: the aim-view range of a launched projectile; the constructor does
    // not write it. Its only writer is the aim-view lock 00249b10 (00249ccc:
    // the 0022a8c0 probe distance), aim_view_lock; nullopt until it runs.
    std::optional<float> aim_view_range_f4;
    bool operator==(const WeaponInventory&) const=default;
};

struct WeaponInventoryEvent {
    enum class Kind {
        object_created,     // 00266630(obj, actor) (with_object)
        preload,            // 00145710(db, item, 0)
        unload,             // 00145630(db, item, 0)
        attach,             // 0038c7d0(actor+60, point, record +13c)
        detach,             // 0038c7c0(actor+60, point)
        move_attach,        // 0038c790(actor+60, from, to)
        clear_attach_24a2,  // add of a slot-6/7 item: actor byte +24a2+point = 0
        agent_unlock_query, // 001861f0: 00332f90(agent data, 5) for a player
        switch_sound,       // 00142c50: MSGGAME_PLAYSND descriptor 004f1950 at actor +c0, to 004ffb50, transport 0
    } kind{};
    std::int32_t item{},point{},to{};
    std::int32_t model{};
    RootVector position{};
    bool operator==(const WeaponInventoryEvent&) const=default;
};

struct WeaponInventoryHost {
    bool player{};          // actor +58 virtual +54
    bool byte_14{true};     // actor +14
    // Agent-data unlock 5 (IncreasedAmmo) of the local player; read by
    // 001861f0 only for a player.
    bool increased_ammo{};
    // actor +60 +2410[point] non-null: a model is attached at the point.
    std::function<bool(std::uint8_t point)> attached;
    // actor virtual +c0(actor, -1): the switch sound position.
    std::function<RootVector()> position_c0;
    // Optional: runs each event as it is made (the original calls the
    // preload and attach helpers in place, so a later step of the same
    // operation sees their effect: 00143df0's attach reads record +13c after
    // its 00145710, 00142c50's +2410 test sees 00142bb0's move).
    std::function<void(const WeaponInventoryEvent&)> apply;
};

class WeaponInventoryOps {
public:
    WeaponInventoryOps(WeaponInventory& inventory,const WeaponDatabase& database,const WeaponInventoryHost& host,
        std::vector<WeaponInventoryEvent>& events);
    // 00144dc0(inv, actor, with_object): with_object = role 1 or 3 (001b6f70);
    // it creates the object and adds item 0x67 without ammo.
    static WeaponInventory construct(std::uint64_t actor,bool with_object,const WeaponDatabase& database,
        const WeaponInventoryHost& host,std::vector<WeaponInventoryEvent>& events);
    // 001861f0(actor, clips).
    std::int32_t clips_for_actor(std::int32_t clips);
    // 00143df0(inv, item, give_ammo, -, preload, no_net).
    void add(std::int32_t item,bool give_ammo,bool preload);
    // 00142c50: returns 1 when a model was moved to the hand.
    bool equip(std::uint8_t slot);
    void holster(std::uint8_t slot);            // 00142bb0
    void holster_current();                     // 00143b60
    // 00143cc0 (game::ForceHolster 00185410): 00143b60 unless +84 is 6, so
    // with nothing held +85 is left as it is.
    void force_holster();
    bool switch_allowed() const;                // 00143bc0
    void equip_first();                         // 00143c50
    // 00143a60(inv, holster): with switching allowed (00143bc0), nothing held
    // equips the first occupied slot; otherwise the next occupied slot after
    // the current one, skipping slot 1 (and slot 6), is equipped: without
    // holster the search wraps after slot 5 and stops at the current slot;
    // with holster it stops at slot 6 and then holsters the current weapon.
    void cycle(bool holster);
    // 001439b0(inv, item): with switching allowed, item -1 holsters the
    // current weapon (returns true); otherwise the item's slot (00143d20) is
    // equipped when it holds the item; returns whether it does.
    bool equip_item(std::int32_t item);
    void remove(std::uint8_t slot,bool unload); // 00142450
    void clear();                               // 001423e0
    // 00185330 game::SetPlayerWeapons(first, second) on this inventory.
    void set_player_weapons(std::int32_t first,std::int32_t second);
    std::uint8_t slot_of_item(std::int32_t item) const;   // 00143d20
    std::int32_t spare(std::uint8_t slot) const;          // 00143870
    std::int32_t clip(std::uint8_t slot) const;           // 00143940
    bool ready(bool continuation) const;                  // 00143400
    // 00143820: the current slot's record +78 is 1 (false with nothing held).
    bool full_auto() const;
    // 001438e0(inv, slot): the clip size (record +70) of the slot's item (6:
    // the current slot); 0 with nothing there.
    std::int32_t clip_size(std::uint8_t slot) const;
    // 00143670(inv, type): one of slots 0..5 holds an item whose record +10c
    // is the ammo type and whose spare rounds +c are positive.
    bool spare_of_ammo_type(std::int32_t type) const;
    // 00143560(inv), a reload can start: something held (+84 != 6), not
    // reloading (+a0), no burst left in either hand (+bc / +c0), the held
    // slot's item present; the held clip (00143940(inv, 6)), halved (toward
    // zero) when slot 0 is held with a slot-1 item, below the record's clip
    // size +70, record +78 != 2, and spare rounds of its ammo type +10c
    // (00143670).
    bool reload_allowed() const;
    const WeaponInventory& inventory() const{return inv_;}
    // 00142170 (from the AI death notice 0014f680): for slots 0..5 holding an
    // item, total = clip size (+70) x 001861f0(+74); fraction = r / 2^31 x
    // (MaxNpcAmmoPct 0.3 - MinNpcAmmoPct 0.05) + 0.05 (00489c20 / 00489c18),
    // doubled when the total is below 10; amount = trunc(total x fraction).
    // Hold class 1 keeps it all as spare (clip 0); otherwise the clip takes up
    // to the clip size and the rest is spare, and while spare rounds remain a
    // second draw moves trunc(clip size x r / 2^31) rounds from the clip to
    // the spare. Draws come from the game RNG (00127dd0).
    void randomize_dropped_ammunition(const std::function<std::uint32_t()>& random);
    // The ammunition services of a backpack pickup (Checkpoint 121f,
    // tests/weapon_backpack_goldens.hpp). Records are compared as the
    // 00147840 lookups they are (an item outside the table is record 0).
    // 001437e0: the record of the held slot's item (nullopt when +84 is 6).
    std::optional<std::size_t> held_record() const;
    // 00142ae0(inv, slot): clip size (+70) x 001861f0(actor, +74) minus the
    // slot's spare rounds (the EE's wrapping multiply).
    std::int32_t spare_room(std::uint8_t slot);
    // 00142760(inv, slot, rounds, -): the slot's clip = rounds; returns 1.
    std::int32_t set_clip(std::uint8_t slot,std::int32_t rounds);
    // 001429d0(inv, slot, rounds, no_limit): adds rounds to the slot's spare,
    // limited to 00142ae0 unless no_limit; returns the rounds added.
    std::int32_t add_spare(std::uint8_t slot,std::int32_t rounds,bool no_limit);
    // 00142960(-, a, b): the same record, or the same ammo type +10c with
    // neither hold class +f0 being 1.
    bool same_ammunition(std::size_t a,std::size_t b) const;
    // 00142830(inv, record, rounds): the held slot first (when it shares the
    // ammunition), then slots 0..5 other than the held one while rounds
    // remain; 00143d60 after; returns the rounds taken.
    std::int32_t add_ammunition(std::int32_t item,std::int32_t rounds);
    // 00143d60: with items in slots 0 and 1 (the dual pistols), both get
    // (clip0 + clip1 + 1) / 2 and (spare0 + spare1 + 1) / 2 (C division).
    void share_dual();
private:
    void event(WeaponInventoryEvent e){events_.push_back(e);if(host_.apply)host_.apply(e);}
    WeaponInventory& inv_;
    const WeaponDatabase& db_;
    const WeaponInventoryHost& host_;
    std::vector<WeaponInventoryEvent>& events_;
};

// 00489d70 (holstered) / 00489d78 (in hand) attach points per slot.
std::uint8_t weapon_holster_point(std::uint8_t slot);
std::uint8_t weapon_hand_point(std::uint8_t slot);

}
