#pragma once
// The backpack object's life cycle (Checkpoint 121g), native and
// interpreter-verified (tests/backpack_object_goldens.hpp, gen_backpack_object.py):
//   the cap check 00257a40 (with 001a3730) and its eviction 00257b7c, the constructor 00257640(actor, 1, 0):
//   the base objects 003cb090 / 003cf0e0, the reset 00258330, the transform
//   003cdff0 (vtable +90; a static component, vtable 004df6a0), +2f0 / +60 /
//   +310, the list append 00257620 / 00139200 (*004f7f30), the drop matrix
//   001a32a0 (statics 004eeda0..a8, 00140e20 with the game's sinf / cosf
//   00140e70 / 00140e80), the placement 002561d0 (00131c10, the player's
//   region 0013e1a0 -> 00224ac0, else 002248a0, the downward line of sight
//   003c9e30 / the LOS manager's virtual +c / 003c9d60) and the owner 002566b0
//   with the box block (003ce980, 0040c9d0, 00170e20, box +94); the destroy
//   00254770 with the list removal 001bac40; the tick 002568f0. Checkpoint 132bp:
//   the population's INV: crate constructor cBackpack_ctor 00257f30
//   (tests/backpack_crate_goldens.hpp, gen_backpack_crate.py), which
//   Loader_LoadInventoryDefs 001c2a10 -> 00257480 follows with the list
//   append 00257620.
// Calls into other systems are events in call order. Not ported (throw naming
// the address): the constructor without an actor
// (00257820), a placement into a region the host cannot name
// (002561d0 reads the null region's root bytes), 003ce980 with a +58 component, and the
// multiplayer blocks (005721c8, including the eviction's +308 filter).
// Checkpoint 132ms5c: the drop matrix of an inactive actor (001a3448: virtual
// +7c, the +58 controller's +5c and the +3584 block's +44..+4c) is ported.
// Checkpoint 132el6 (tests/backpack_rider_goldens.hpp, gen_backpack_rider.py):
// a backpack dropped onto a lift (the placement's line of sight hits a GOB of
// class *004f5468 with +68 = 3) is attached to its rider vector (00256630 ..
// 002564a0, the lift's side in ElevatorGOBJ::attach_rider); the box update
// 00171050; the rider's vt+a8 00256670 and the lift's mode-3 message through
// the receiver 00254600 -> 003cea10.
#include "animation_root.hpp"
#include "weapon_backpack.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

inline constexpr std::uint32_t backpack_object_vtable=0x004dc350;
inline constexpr std::uint32_t backpack_transform_vtable=0x004df6a0;
inline constexpr std::uint32_t backpack_object_cap=12;                 // *0048af10
inline constexpr std::uint32_t backpack_object_lifetime=0x40400000;    // *0048af18 = 3.0
inline constexpr std::uint32_t backpack_release_delay=0x3dcccccd;      // 0.1 to 003cb230

// The 0x60-byte transform component (bp +50): +0 vtable, +10..+4f the matrix,
// byte +50 dirty.
struct BackpackTransform {
    std::array<std::uint8_t,0x60> bytes{};
    std::uint32_t word(std::size_t offset) const;
    void set_word(std::size_t offset,std::uint32_t v);
    void set_byte(std::size_t offset,std::uint8_t v);
};

// A region (terrain object) as 002561d0 reads it: its identity (stored at
// +2f8) and root bytes +2c / +2d / +2e.
struct BackpackRegion {
    std::uint32_t handle{};
    std::array<std::uint8_t,3> root{};
};
// The first hit of the placement's line of sight: its object's +4c (class
// key) and +68, and the object (the host's identity for it, handed to
// attach_rider for a lift).
struct BackpackLosHit {
    std::uint32_t class_4c{},type_68{};
    std::uint32_t object{};
};

struct BackpackObjectEvent {
    enum class Kind {
        alloc_object,       // 0036b180(0x318) -> a
        alloc_transform,    // 0036b180(0x60) -> a
        contain,            // 00224ac0(p, the player's zone) (origin = p)
        nearest,            // 002248a0(p)
        los_begin,          // 003c9e30(request, 005435a0)
        los,                // the LOS manager's +c (origin, displacement)
        los_end,            // 003c9d60(request, 0)
        register_object,    // 0040c9d0(a)
        create_box,         // 00170e20(*004ffbd0, bp, 0, 1) -> a
        box_word,           // box a +b = c
        remove_box,         // 00170db0(*004ffbd0, a)
        unregister,         // 0040c9a0(a)
        release,            // 003cb230(a, 0.1)
        clear_owner_3464,   // owner actor a +3464 = 0
        add_weapon,         // 00255af0(bp, item a, spare b, clip c, flag d) (the INV: constructor)
        find_collision,     // IdMgr_FindCollision 0040c480(a, b)
        zone_associate,     // ZoneMgr_Associate 00173d60(zone a, bp b, 1, 0)
        attach_rider,       // 002563f8..00256444: the handle (bp b, id c = its +c) to the lift a's +84
        box_bounds,         // 00171050(*004ffbd0, box a, bp, 0): box +70 = origin, +80 = displacement (001710a0)
    } kind{};
    std::uint32_t a{},b{},c{},d{};
    RootVector origin{},displacement{};
    bool operator==(const BackpackObjectEvent&) const=default;
};

struct BackpackObjectHost {
    // The object at an address of the list (or +300's actor).
    std::function<WeaponBackpack&(std::uint32_t address)> object;
    std::function<WeaponBackpackOwner(std::uint32_t actor)> owner;
    std::function<std::optional<BackpackRegion>(const RootVector& p)> containing_region;   // 00224ac0 (nullopt: 0)
    std::function<std::optional<BackpackRegion>(const RootVector& p)> nearest_region;      // 002248a0
    std::function<std::optional<BackpackLosHit>(const RootVector& origin,const RootVector& displacement)> line_of_sight;
    std::uint32_t game_class_5468{};          // *004f5468
    std::function<std::uint32_t(std::uint32_t bp)> create_box;   // 00170e20
    bool multiplayer{};                       // 005721c8
    // 002563f8..00256444 for a hit on a lift: the lift's rider vector takes
    // the handle (ElevatorGOBJ::attach_rider: 00256500 find, 002564a0 append).
    std::function<void(std::uint32_t lift,std::uint32_t bp,std::uint32_t id)> attach_rider;
    // The backpack's vt+88 00253fb0: the gearpack model's +3e0 / +3f0 box
    // (*004ffd30 +b8), which 001710a0 reads for the box bounds.
    std::function<std::array<RootVector,2>()> model_bounds;
    // 00171050's stores: box +70 / +80 = the bounds.
    std::function<void(std::uint32_t box,const RootVector& minimum,const RootVector& maximum)> set_box_bounds;
};

// *004f7f30: the backpacks in order (the vector's +4 count / +8 data).
struct BackpackObjectList {
    std::vector<std::uint32_t> data;
};

// The actor 00257640 drops from: its address, view and virtual +f4 matrix
// (only row 3 is read). An inactive actor (001a3448) also reads its virtual
// +7c (00184cd0: the generator-spawned byte +3598), then the +58
// controller's virtual +5c (nullopt: +58 is null) and the +3584 block's
// +44 / +48 / +4c (nullopt: +3584 is null).
struct BackpackDropActor {
    std::uint32_t address{};
    WeaponBackpackOwner view;
    std::array<float,16> matrix_f4{};
    bool spawned_3598{};
    std::optional<bool> controller_5c;
    std::optional<std::array<float,3>> block_3584_44;
};

struct BackpackObjectCreate {
    WeaponBackpack* object{};
    std::uint32_t address{};
    BackpackTransform* transform{};
    std::uint32_t transform_address{};
    std::uint32_t id_40c240{};                // 0040c240(&id, 0xf5)
    std::uint32_t clock_4f7f70{};             // *004f7f70 (float bits)
};

// 00257a40: the count of list entries with +60 == 0, +30c >= 2 and no valid
// owner or 001a3730(owner); above *0048af10 the eviction 00257b7c marks the
// oldest (+310) of the eligible backpacks (no valid owner or 001a3730, +2f2
// clear, +314 < 0) with the highest +30c (>= 2): +2f2 = 1, +314 = 0 (the tick
// then ages it and the draw fades it), and releases its box (+2fc).
void backpack_object_cap_check(const BackpackObjectList& list,const BackpackObjectHost& host,
    std::vector<BackpackObjectEvent>& events);
// 001a32a0(out, actor) for an active actor: the static rotation (first-use
// statics a = 0, b = pi/2, c = 0 through 00140e20) with row 3 = the actor's
// +f4 row 3. For an inactive actor (001a3448..001a34b4) with +3598 set, a
// +58 controller whose +5c answers nonzero and a +3584 block, the row's
// x / y / z become that block's +44 / +48 / +4c (w stays).
std::array<float,16> backpack_drop_matrix(const BackpackDropActor& actor);
// 00257640(actor, 1, 0): returns the backpack's address. The database is
// the reset's (00258330 with keep 0 reads none of it).
std::uint32_t backpack_object_construct(const BackpackObjectCreate& create,const BackpackDropActor& actor,
    BackpackObjectList& list,const BackpackObjectHost& host,std::vector<BackpackObjectEvent>& events,
    const WeaponDatabase& database);
// One INV: record as cBackpack_ctor 00257f30 reads it from the population
// stream (+0 version, +c cursor): the id words (+c = w0 << 16 | w1), the
// position (float bits), three slots (item, copies, amount, and from version
// 0x39 a flag byte padded to 4 bytes), the armour pair and the owner pair.
struct BackpackInvRecord {
    struct Slot {
        std::int32_t item{},copies{},amount{};
        std::uint8_t flag{};
    };
    std::uint32_t version{};
    std::array<std::uint32_t,2> id{};
    std::array<std::uint32_t,3> position{};
    std::array<Slot,3> slots{};
    std::array<std::uint32_t,2> armor{},owner{};
};
// The record's size in the stream: 0x54 bytes from version 0x39, else 0x48.
std::uint32_t backpack_inv_record_size(std::uint32_t version);
inline constexpr std::uint32_t backpack_gobj_class_4f7f68=0x004a7be8;   // *004f7f68 once the static constructors ran
struct BackpackInvHost {
    // 00255af0 on the backpack (WeaponBackpackOps::add_weapon; its box block
    // may create the hotbox +2fc).
    std::function<void(WeaponBackpack& bp,std::int32_t item,std::int32_t spare,std::int32_t clip,std::uint8_t flag)> add_weapon;
    // IdMgr_FindCollision 0040c480(category, index): the zone, or 0.
    std::function<std::uint32_t(std::uint32_t a0,std::uint32_t a1)> find_collision;
    std::function<std::uint32_t(std::uint32_t bp)> create_box;   // 00170e20
};
// cBackpack_ctor 00257f30(bp, reader): the population's INV: crate. The base
// object (cGOBJ_ctor 003cf0e0 with &(-1), 0, 004f7f68), +304 = -2, +300 = 0,
// the reset 00258330(bp, 0), the transform 003cdff0 (vtable +90; the
// identity 0055c9f0) and its virtual +14 001b4960 (row 3 = x, y, z, 1), +c
// the id, the slots through 00255af0 (an item >= 0 whose amount is not
// positive is skipped when its record's clip +70 is nonzero; a hold class
// +f0 of 1 or 7 adds amount x copies once, else copies times; spare =
// (amount - 1) x clip), the armour (+2e4..+2ec and the box block), the owner
// (0040c480 then 00173d60, else virtual +5c 003ce980), +2f8 = 0, +2f0 = 0,
// +60 = 1, the box block, +30c = +310 = 0. Not listed in *004f7f30. Below
// version 0x39 a slot add passes the caller's stale $fp and throws.
std::uint32_t backpack_object_construct_inv(const BackpackObjectCreate& create,const BackpackInvRecord& record,
    const BackpackInvHost& host,std::vector<BackpackObjectEvent>& events,const WeaponDatabase& database);
// 002566b0(bp, owner): owner 0 is no owner.
void backpack_object_set_owner(WeaponBackpack& bp,std::uint32_t bp_address,std::uint32_t owner,
    const std::optional<WeaponBackpackOwner>& owner_view,const BackpackObjectHost& host,
    std::vector<BackpackObjectEvent>& events);
// 00256120(bp) (the body pickup 001abbe8 on the body's +3464): with a box
// +2fc, 00170db0, 0040c9a0, 003cb230(box, 0.1) and +2fc = 0.
void backpack_object_hide(WeaponBackpack& bp,std::vector<BackpackObjectEvent>& events);
// 001a3250(actor) with its +3464 (cTurnOnWeaponPickupMsg 001b48f8, queued to
// a body its carrier let go, 001a02c0): the drop matrix 001a32a0, the
// placement 002561d0(bp, m, 1) and 00256180 (with no box +2fc: 00170e20,
// box +94 = bp +c).
void backpack_object_turn_on(WeaponBackpack& bp,std::uint32_t bp_address,BackpackTransform& transform,
    const BackpackDropActor& actor,const BackpackObjectHost& host,std::vector<BackpackObjectEvent>& events);
// 002561d0(bp, m, 1) (the constructor's and 001a3250's placement): the
// transform's vt+c 00131c10 (+10..+4f = m), the region (00224ac0, else
// 002248a0) into +2f8 and +2f4, the downward line of sight (003c9e30 /
// 003caa50 vt+c / 003c9d60) with the lift attach, then 00171050 when +2fc is
// set.
void backpack_object_place(WeaponBackpack& bp,std::uint32_t bp_address,BackpackTransform& t,const std::array<float,16>& m,
    const BackpackObjectHost& host,std::vector<BackpackObjectEvent>& events);
// The backpack's vt+a8 00256670(bp, v) (the lift's rider pass with s3 = 0):
// 003cdbe0 -> the transform's vt+24 001bc330: +40.xyz += v.xyz (vadd.xyz,
// w kept; no transform: nothing), then 00171050(*004ffbd0, +2fc, bp, 0)
// when +2fc is set.
void backpack_object_move_a8(WeaponBackpack& bp,BackpackTransform& t,const RootVector& v,
    const BackpackObjectHost& host,std::vector<BackpackObjectEvent>& events);
// 00254600(bp, msg) for a message of type *0055d4e0 (not the pickup trigger):
// 003cea10. Mode +24 = 3 (the lift's rider message, s3 = 1): with a
// transform (+50; none answers 0), its vt+58 00132900 when byte +50 is set
// (clears it), then vt+14 001b4960 stores +40 = +40 + the message's row 3
// (+60, vadd.xyzw); the +54 and +58 components (0 for a backpack, 00257640)
// throw when set. Other modes throw naming 003cea10. Returns 003cea10's v0.
std::uint32_t backpack_object_receive_transform(WeaponBackpack& bp,BackpackTransform& t,std::uint8_t mode_24,
    const std::array<float,16>& matrix_30);
// 00254770(bp).
void backpack_object_destroy(std::uint32_t bp_address,BackpackObjectList& list,const BackpackObjectHost& host,
    std::vector<BackpackObjectEvent>& events);
// 002568f0(dt): *004f7f70 += dt, then each backpack whose +314 is not
// negative ages by dt and is destroyed past 3.0.
void backpack_object_tick(float dt,std::uint32_t& clock_4f7f70,BackpackObjectList& list,const BackpackObjectHost& host,
    std::vector<BackpackObjectEvent>& events);
// Checkpoint 132f7: one 0x290-byte record of the crate snapshot list
// *004f7f48 (Mem_Alloc 0036b180 tag 0x1ea): +0 the crate's +c, +4..+280 its
// three slots +64..+2e0 (the 0x27c-byte copy 00256cac..00256d0c) and
// +280..+28c its +2e0..+2ec. The init 00256fd0 (+280..+28c, +d0, +d4 and the
// slot entries zeroed) is entirely overwritten by those copies.
inline constexpr std::size_t backpack_crate_snapshot_size=0x290;
struct BackpackCrateSnapshot {
    std::array<std::uint8_t,backpack_crate_snapshot_size> bytes{};
    std::uint32_t word(std::size_t offset) const;
};
// 00256b40 (the loadout done path 00168bc0): 002569c0 frees every record of
// *004f7f48 (here: clears `snapshots`), then each backpack of the list
// *004f7f30 whose +60 is 1 (an INV: crate, cBackpack_ctor 00257f30) appends
// its record (00256d50). The level reset 002570c0 (script natives ResetLevel
// 0012f6e0 and RemoveAllBackpacks 00253c60) reads the list back (00256d90).
void backpack_crate_snapshot(const BackpackObjectList& list,const BackpackObjectHost& host,
    std::vector<BackpackCrateSnapshot>& snapshots);
// 00256d90: for each snapshot record in order, every backpack of the list
// whose +c matches (no break after a match) gets +2e0..+2ec = rec +280..+28c
// and +64..+2e0 = rec +4..+280; one without a box +2fc gets 00170e20(*004ffbd0,
// bp, 0, 1) and box +94 = bp +c.
void backpack_crate_restore(const BackpackObjectList& list,const BackpackObjectHost& host,
    const std::vector<BackpackCrateSnapshot>& snapshots,std::vector<BackpackObjectEvent>& events);
// 002570c0's first loop: every backpack of the list whose +60 is 0 (a drop,
// not an INV: crate) is destroyed (00254770) and the index steps back.
void backpack_remove_drops(BackpackObjectList& list,const BackpackObjectHost& host,
    std::vector<BackpackObjectEvent>& events);

}
