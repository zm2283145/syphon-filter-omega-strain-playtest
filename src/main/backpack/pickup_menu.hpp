#pragma once
// Checkpoint 129c: the item pickup menu of "pickup_menu.cc", the Triangle
// confirm object of a backpack (weapon_backpack.hpp). Native and
// interpreter-verified (tests/pickup_menu_goldens.hpp):
//   the builder 00253070 (element lookup 002535e0), the entry 00251060 with
//   its text setup 00251040, the entry constructor 00251350 and destructor
//   00251290, the cursor moves 00251690 (previous) / 002517d0 (next) and the
//   destroy 00252fb0.
//
// The menu is 0xb8 bytes: +0 the backpack, five 0x1c-byte entries at +4
// (built by 001004b0 with the constructor 00251350, which zeroes only
// +c..+18), +90 the entry count, +94 the cursor, +98 the fade start, and the
// GUI elements +9c "itempickupmenu", +a0 "trianglebutton", +a4 "xbutton",
// +a8 "inventorytextpanel" and +ac "pickuptextpanel" (children of +9c's
// node), +b0 "triangle_drop" / +b4 "x_drop" (drop mode only). The panels
// "panel1" / "panel2" under +9c's node go to the globals 004f7e90 / 004f7e98.
//
// Entries, in order: kind 0 for each of the backpack's items (002553e0, up
// to 5); kind 2 (the ammunition line) when the backpack holds ammunition
// (+2e4 set and +2e8 > 0) and the count plus the owner flags below stays
// under 5; kind 3 / kind 4 when the owner check is on and 00255e50 /
// 00255d90 report the owner's state items; kind 5 for each of the owner's
// nonzero pairs while the count is under 5. The original appends kinds 3 / 4
// without a bound, so a sixth entry would overwrite +90..+b4; the port
// throws there instead.
//
// Every call into the GUI, string table, heap and sound systems goes through
// PickupMenuHost in the original's call order. Text colours are the pickup
// text colour block 004f7e70 that the iarcade native SetPickupTextColor
// fills (Checkpoint 129b, hud_object.hpp).
//
// Checkpoint 132t: the per-frame update 002526f0, the Cross handler 00251920
// and the cursor's item cycles 00251370 / 00251500, native and
// interpreter-verified (tests/pickup_menu_frame_goldens.hpp) over
// PickupMenuFrameHost (every callee in call order; the 00128bd0 formats are
// done here and checked against the EE's own sprintf). Checkpoint 132bp: the
// crate sends (backpack +60 == 1, CrateInteractMsg 004f7ea0 at 00251c68 /
// 0025206c / 0025211c) go through PickupMenuFrameHost::crate_interact.
// Checkpoint 132zf5: the kind-5 owner pair line (00252cf4: 00470560 on the
// owner's and the actor's +3528 lists, the item row's +28 icon) and take
// (00252364: 004706c0 / 00470870 / 00470270, the "%s %s" HUD message and
// the InventoryMsg 004f7ee0 to the backpack). Not ported (throw naming the
// address): the kind-1 / >= 6 lines of the update (stale text buffers), the
// IR goggle switch 00199590 and the kind-3 / kind-4 owner messages
// (0025220c / 002522c4). Checkpoint 132fx5 hosts the multiplayer drop message
// (byte 005721c8 at 002524a8) through network_inventory_message.
#include "animation_root.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace omega {

inline constexpr std::uint32_t kPickupMenuPlayerList = 0x004ffb50;
inline constexpr std::uint32_t kPickupMenuTextColour = 0x004f7e70;
inline constexpr std::int32_t kPickupMenuDropStart = 100;   // *0048af00
inline constexpr std::int32_t kPickupMenuEntryFade = 10;    // *0048aef8
inline constexpr std::int32_t kPickupMenuButtonFade = 7;    // *0048aef0
inline constexpr std::int32_t kPickupMenuEntries = 5;
inline constexpr std::uint32_t kPickupMenuPreviewSize = 0x68;
inline constexpr std::uint32_t kPickupMenuPreviewLine = 0x352;
inline constexpr std::uint32_t kPickupMenuPanelPreviewLine = 0x35d;
inline constexpr float kPickupMenuPreviewRelease = 0.1f;   // 0x3dcccccd

struct PickupMenuEntry {
    std::uint8_t kind = 0;          // +0: 0 item, 2 ammunition, 3 / 4 owner state, 5 owner pair
    std::uint32_t item = 0;         // +4
    std::uint32_t word_08 = 0;      // +8
    std::uint32_t preview_0c = 0;   // +c: 00259420 object, released by 003cb230
    std::uint32_t preview_10 = 0;   // +10: the panel2 preview (base 0 only)
    std::uint32_t element_14 = 0;   // +14: created under panel1
    std::uint32_t element_18 = 0;   // +18: created under panel2 (base 0 only)
};

struct PickupMenu {
    std::uint32_t backpack = 0;                                    // +0
    std::array<PickupMenuEntry, kPickupMenuEntries> entries{};     // +4
    std::int32_t count = 0;                                        // +90
    std::int32_t cursor = 0;                                       // +94
    std::int32_t start = 0;                                        // +98
    std::uint32_t menu_9c = 0;           // "itempickupmenu"
    std::uint32_t triangle_a0 = 0;       // "trianglebutton"
    std::uint32_t x_a4 = 0;              // "xbutton"
    std::uint32_t inventory_a8 = 0;      // "inventorytextpanel"
    std::uint32_t pickup_ac = 0;         // "pickuptextpanel"
    std::uint32_t triangle_drop_b0 = 0;  // "triangle_drop"
    std::uint32_t x_drop_b4 = 0;         // "x_drop"
};

// The globals 004f7e90 / 004f7e98.
struct PickupMenuPanels {
    std::uint32_t panel1 = 0;
    std::uint32_t panel2 = 0;
};

// The backpack and owner words the builder reads.
struct PickupMenuBackpack {
    std::uint32_t address = 0;
    std::uint32_t word_2e4 = 0;
    std::int32_t word_2e8 = 0;
    std::uint32_t owner_300 = 0;
    std::uint32_t key_304 = 0;
    std::uint32_t owner_key_0c = 0;                                  // *(owner + 0xc)
    std::vector<std::pair<std::uint32_t, std::uint32_t>> owner_pairs;  // *(owner + 3528): +4 count, +8 pairs
};

class PickupMenuHost {
public:
    virtual ~PickupMenuHost() = default;
    virtual std::uint32_t find_child(std::uint32_t node, std::string_view name) = 0;  // 003eeb30
    virtual std::uint32_t create_element(std::uint32_t node) = 0;                     // 003ef010
    virtual std::uint32_t element_node(std::uint32_t element) = 0;                    // element +70
    virtual std::uint32_t element_text(std::uint32_t element) = 0;                    // element +74
    virtual void evaluate(std::uint32_t element, float time) = 0;                     // 003f45a0
    virtual void fade(std::uint32_t element, float time) = 0;                         // 003f44e0
    virtual void set_text(std::uint32_t element, std::string_view text) = 0;          // 003e7ec0
    virtual std::string get_string(std::string_view key) = 0;                         // 003fdb50
    virtual std::uint32_t player(std::uint32_t list, std::int32_t index) = 0;         // 001314f0
    virtual std::string player_name(std::uint32_t player) = 0;                        // 00336170
    virtual void text_byte(std::uint32_t text, std::uint32_t offset, std::uint8_t value) = 0;
    virtual void set_colour(std::uint32_t text, std::uint32_t colour) = 0;            // 003d0810
    virtual void set_visible(std::uint32_t node, std::int32_t a1, std::int32_t a2) = 0;  // 003eebe0
    virtual std::vector<std::uint32_t> list_items(std::uint32_t backpack, std::uint32_t inventory,
                                                  std::int32_t max) = 0;               // 002553e0
    virtual std::uint32_t owner_state_e50(std::uint32_t backpack) = 0;                // 00255e50
    virtual std::uint32_t owner_state_d90(std::uint32_t backpack) = 0;                // 00255d90
    virtual std::uint32_t allocate(std::uint32_t size, std::string_view file, std::uint32_t line) = 0;  // 0036b180
    virtual std::uint32_t construct_preview(std::uint32_t block) = 0;                 // 00259420
    // 001ca010(message, 3, name, 0, 0) / 0020faa0 / 003c9420(message, target, flag).
    virtual void post_sound(std::string_view name, std::uint32_t target, std::int32_t flag) = 0;
    virtual void release_preview(std::uint32_t preview, float seconds) = 0;          // 003cb230
    virtual void remove_element(std::uint32_t element) = 0;                           // 003e8eb0
    virtual void close_backpack(std::uint32_t backpack) = 0;                          // 00255f10
};

// 00253070(menu, screen, backpack, actor, owner_check, drop); inventory is
// *(actor + 3524).
void pickup_menu_build(PickupMenu& menu, PickupMenuPanels& panels, PickupMenuHost& host,
                       std::uint32_t screen, const PickupMenuBackpack& backpack,
                       std::uint32_t inventory, std::uint32_t owner_check, std::uint32_t drop);
// 00251060(entry, kind, item, base).
void pickup_menu_entry(PickupMenuEntry& entry, const PickupMenuPanels& panels, PickupMenuHost& host,
                       std::uint8_t kind, std::uint32_t item, std::int32_t base);
// The 00147840 record fields the update and the Cross handler read.
struct PickupMenuRecord {
    std::uint8_t class_30 = 0;
    std::int32_t clip_70 = 0;
    std::int32_t clips_74 = 0;
    std::string name_80;            // the key at record +80 (003fdb50's argument)
    std::uint8_t hold_f0 = 0;
    std::uint32_t icon_11c = 0;
    std::uint32_t model_13c = 0;
};

// 00255500(bp, item, &a2, &a3, &t0): nullopt when the backpack has no entry
// (the outputs are then not written; reading them throws).
struct PickupMenuAmmo {
    std::int32_t spare = 0;   // a2
    std::int32_t clip = 0;    // a3
    std::int32_t total = 0;   // t0
};

inline constexpr std::uint32_t kPickupMenuSoundAmmo = 0x004f17b0;
inline constexpr std::uint32_t kPickupMenuSoundWeapon = 0x004f1af0;

// The state 002526f0 / 00251920 read and write directly and every function
// they call (the inventory is *(actor + 3524), the backpack menu +0).
class PickupMenuFrameHost {
public:
    virtual ~PickupMenuFrameHost() = default;
    // Direct reads and writes.
    virtual std::uint8_t held_84() = 0;                                   // inventory +84
    virtual std::int32_t slot_item(std::uint32_t slot) = 0;               // inventory +8 + 16 slot
    virtual std::int32_t backpack_ammunition() = 0;                       // bp +2e4 ? bp +2e8 : 0
    virtual std::uint8_t backpack_60() = 0;                               // bp +60
    virtual bool owner_valid() = 0;                                       // bp +300 set, its +c == bp +304
    virtual std::int32_t armor_33a0() = 0;                                // actor +33a0
    virtual std::int32_t max_armor_339c() = 0;                            // actor +339c
    virtual void set_armor(std::int32_t armor, std::int32_t max_armor) = 0;   // actor +33a0 / +339c
    virtual void clear_selection_a4() = 0;                                // inventory +a4 = -1
    virtual bool ammo_label_48af08() = 0;                                 // *0048af08 != 0
    virtual bool multiplayer_5721c8() = 0;                                // byte 005721c8
    // Calls.
    virtual std::string get_string(std::string_view key) = 0;                                  // 003fdb50
    virtual std::uint32_t player(std::uint32_t list, std::int32_t index) = 0;                  // 001314f0
    virtual std::uint32_t agent_unlock(std::uint32_t player, std::int32_t unlock) = 0;         // 00332f90
    virtual std::int32_t slot_of_item(std::int32_t item) = 0;                                  // 00143d20
    virtual std::int32_t cycle(std::uint32_t backpack, std::int32_t item, std::int32_t dir) = 0;   // 00254ca0
    virtual std::uint32_t take(std::uint32_t backpack, std::int32_t item, std::int32_t test) = 0;  // 002558a0
    virtual std::uint32_t has_clip(std::int32_t item) = 0;                                     // 00143710(inv, item)
    virtual std::uint32_t slot_flag(std::uint32_t backpack, std::int32_t item) = 0;            // 002559d0
    virtual std::optional<PickupMenuAmmo> ammunition(std::uint32_t backpack, std::int32_t item) = 0;  // 00255500
    virtual PickupMenuRecord record(std::int32_t item) = 0;                                    // 00147840(*004ffd30, item)
    virtual std::int32_t spare(std::int32_t slot) = 0;                                         // 00143870(inv, slot)
    virtual std::int32_t clip(std::int32_t slot) = 0;                                          // 00143940(inv, slot)
    virtual std::int32_t clips_for_actor(std::int32_t clips) = 0;                              // 00143de0 -> 001861f0
    virtual void set_preview(std::uint32_t preview, std::uint32_t word_60, std::uint8_t byte_64) = 0;  // 00259420 object
    virtual void set_element_data(std::uint32_t element, std::uint32_t data) = 0;              // 003e7eb0
    virtual void set_text(std::uint32_t element, std::string_view text) = 0;                   // 003e7ec0
    virtual void anim_range(std::uint32_t element, float from, float to, std::int32_t loop) = 0;   // 003f4510
    virtual void fade(std::uint32_t element, float time) = 0;                                  // 003f44e0
    virtual void post_sound(std::string_view name, std::uint32_t target, std::int32_t flag) = 0;
    virtual RootVector actor_position() = 0;                                                   // actor virtual +c0(out, actor, -1)
    // 0020f860(msg, descriptor, position), byte +20 = 1, 003c9420(msg, 004ffb50, 0).
    virtual void positional_sound(std::uint32_t descriptor, const RootVector& position) = 0;
    virtual std::int32_t add_spare(std::int32_t slot, std::int32_t rounds, std::int32_t no_limit) = 0;  // 001429d0
    virtual std::int32_t remove_ammunition(std::uint32_t backpack, std::int32_t item, std::int32_t rounds) = 0;  // 00255650
    virtual void remove(std::int32_t slot) = 0;                                                // 00142450(inv, slot, 0, 0, 0)
    virtual void add(std::int32_t item) = 0;                                                   // 00143df0(inv, item, 0, 0, 0, 0)
    virtual void set_clip(std::int32_t slot, std::int32_t rounds) = 0;                         // 00142760(inv, slot, rounds, 0)
    virtual void equip(std::int32_t slot) = 0;                                                 // 00142c50
    virtual void hud_message(std::string_view text) = 0;                                       // 00242b00(*004ffc2c, text)
    virtual void mark_30c(std::uint32_t backpack) = 0;                                         // 00255a40
    // Checkpoint 132fx1: 0025200c: 00199590(actor, 0) (cHuman::EquipGoggles off, actor_goggles.hpp switch_actor_ir)
    // when the item put back is the goggles (record +30 == 6). The default throws naming the address.
    virtual void goggles_ir_off();
    virtual void add_weapon(std::uint32_t backpack, std::int32_t item, std::int32_t spare, std::int32_t clip,
                            std::int32_t flag) = 0;                                            // 00255af0
    virtual std::pair<std::int32_t, std::int32_t> take_armor(std::uint32_t backpack) = 0;      // 00255860 (armor, max)
    virtual void add_armor(std::uint32_t backpack, std::int32_t armor, std::int32_t max_armor) = 0;  // 00255a50
    virtual void pickup_ammunition(std::uint32_t backpack) = 0;                                // 00255290(bp, inv)
    virtual void share_dual() = 0;                                                             // 00143d60
    // The crate message (backpack +60 == 1): Event_Construct 003c9e30(msg,
    // 004f7ea0) (+0 004dfd80, +4 the key, +18 = +1c = 0, +20 = 5), vtable
    // 004dc480 (CrateInteractMsg), +24 taken (Taken 002587e0), +28 given
    // (Given 00258800), +2c the actor (Who 002587d0), +20 flags; then
    // Event_Send 003c9420(msg, backpack, 0) and cMessage_dtor 003c9d60.
    virtual void crate_interact(std::uint32_t backpack, std::int32_t taken, std::int32_t given,
                                std::uint8_t flags_20) = 0;
    // Checkpoint 132zf5: the kind-5 owner pair line / take. The item lists
    // are the owner's (bp +300) and the actor's +3528 (ItemInventory).
    virtual std::uint32_t item_icon(std::int32_t item) = 0;                    // *(*00587ff8 + item * 0x2c + 0x28)
    virtual std::int32_t owner_item_count(std::int32_t item) = 0;              // 00470560(owner +3528, item)
    virtual std::int32_t actor_item_count(std::int32_t item) = 0;              // 00470560(actor +3528, item)
    virtual std::int32_t owner_remove_item(std::int32_t item, std::int32_t count) = 0;  // 004706c0(owner +3528, item, n, 0)
    virtual void actor_add_item(std::int32_t item, std::int32_t count) = 0;    // 00470870(actor +3528, item, n, 0)
    virtual std::optional<std::string> item_name(std::int32_t item) = 0;       // 00470270 (null: none)
    // The InventoryMsg: Event_Construct 003c9e30(msg, 004f7ee0), vtable
    // 004dc460, +24 the item, +28 the count, +20 byte 2, +1c 0x40, +2c the
    // actor; Event_Send 003c9420(msg, backpack, 0), vtable 004dcf10 and
    // cMessage_dtor 003c9d60.
    virtual void inventory_message(std::uint32_t backpack, std::int32_t item, std::int32_t count) = 0;
    // Checkpoint 132fx5: the multiplayer drop message at the end of 00251920 (002524a8..0025254c, when byte
    // 005721c8 and the menu's +98 are set): the local client index *00494178 (002ebaf8 fills it when -1), then the
    // InventoryMsg Event_Construct 003c9e30(msg, 005061b0), vtable 004dcef0, +20 byte 4, +24 byte 0, +28 the menu's
    // backpack (+0), +2c the local index; Event_Send 003c9420(msg, actor, 0), vtable 004dcf10 and cMessage_dtor
    // 003c9d60. Unhosted (the single-player hosts): throw naming 002524a8.
    virtual std::uint32_t local_index_494178() {
        throw std::runtime_error("002524a8: the local client index (*00494178 / 002ebaf8) is not hosted");
    }
    virtual void network_inventory_message(std::uint32_t /*backpack*/, std::uint32_t /*local*/) {
        throw std::runtime_error("002524a8: the multiplayer drop message (InventoryMsg 005061b0) is not hosted");
    }
    // Checkpoint 132zf8: the kind-3 line (a body's backpack, 0025220c): the
    // actor message Event_Construct 003c9e30(msg, 0055d480), vtable 004e01a0,
    // +24 action 0x2b, byte +28 = 5, +2c the owner's (bp +300) +c, byte +20 =
    // 1; Event_Send 003c9420(msg, actor, 0) queues it for the actor's next
    // pump (the body pickup 001abbe8, actor_body_carry.hpp); vtable 004e0840
    // and cMessage_dtor 003c9d60. Unhosted: throws naming 0025220c.
    virtual void owner_pickup_message() {
        throw std::runtime_error("0025220c: the kind-3 owner message 0x2b (003c9e30) is not hosted");
    }
    // Checkpoint 132sx10: the kind-4 line (a body's disguise, 002522c4..
    // 00252344): when the owner (bp +300) has a controller (+58), its virtual
    // +1d8(0, 1) (the AI component's 0014ac90 clears +3469); then the
    // AIDisguiseTakenMsg: Event_Construct 003c9e30(msg, 004ff2b8), +24 the
    // actor (the taker), byte +20 = 2, +1c 0x40, vtable 004dcc50; Event_Send
    // 003c9420(msg, owner, 0) queues it for the owner's pump; vtable 004dcd70
    // and cMessage_dtor 003c9d60. Unhosted: throws naming 002522c4.
    virtual void owner_disguise_taken() {
        throw std::runtime_error("002522c4: the kind-4 owner +58 virtual +1d8 and message (003c9e30) are not hosted");
    }
};

// 002526f0(menu, actor): rebuilds each entry's previews and texts; returns
// whether the menu has entries (false: the caller closes it).
bool pickup_menu_update(PickupMenu& menu, PickupMenuFrameHost& host);
// 00251920(menu, actor): Cross on the selected entry; returns the close flag
// (a drop-mode take, a kind 3 / 4 line, or a drop slot >= 6).
bool pickup_menu_confirm(PickupMenu& menu, PickupMenuFrameHost& host);
// 00251370 / 00251500: the selected entry's item steps to the backpack's
// previous / next item of its slot (00254ca0 with 1 / 0), GS_SELECT on change.
void pickup_menu_left(PickupMenu& menu, PickupMenuFrameHost& host);
void pickup_menu_right(PickupMenu& menu, PickupMenuFrameHost& host);

void pickup_menu_previous(PickupMenu& menu, PickupMenuHost& host);  // 00251690
void pickup_menu_next(PickupMenu& menu, PickupMenuHost& host);      // 002517d0
// 00252fb0(menu, flag); the menu memory itself (00100740) stays with the caller.
void pickup_menu_destroy(PickupMenu& menu, PickupMenuHost& host);

}  // namespace omega
