#pragma once
// Hotboxes (CROUCH_ZONES_AND_RUNNING_JUMP.md): the world objects that the
// mission's BOX: records build (00172ae0, class word 004ee6b8), their
// containment test 001719c0, the local player's tracker (001716e0, handler
// 00171600, update 00170660) and the box side of a hotbox event (handler
// 00171b60 -> enter 00172400 / exit 00171ed0).
//
// Hosted box types: 1, 3, 6 and 7 without resolved links (their natives only
// message the links: load, enter and exit send nothing, and their HotboxMsg
// handlers act), 1, 6 and 7 whose links are all actors the host routes the
// link messages to (EnableMsg/DisableMsg: hotbox_load_link_messages,
// hotbox_link_messages), 8 and 12 (object-interaction boxes: enter/exit
// message the actor, Triangle sends their links trigger messages), 9 (crawl
// zone), 13 (enter/exit message the actor code 13, the player component's
// +0x184 nesting counter that the backpack pickup reads; TORONTO3's
// "p_dropbox" 402142, checkpoint 132zf7), 14 (the weather boxes the
// weather record 00232f10 reads; enter/exit 001726b0 / 00172220 send code 14
// to the actor, which the player component ignores; checkpoint 132ms2),
// 15/16 (running-jump zones) and 17
// (their targets, no messages). The
// other types, type 3 with links and 1/6/7 with other links send mission
// messages and throw when a world is built with them; select_hosted_hotboxes
// keeps the hosted ones. Run-time boxes (MakeNPCInteractable, 00170460) are
// type 3 linked to one NPC actor: add_runtime_hotbox appends them, and their
// enter (the GOBJTriggerEventMsg to the actor) throws in receive_hotbox_event.
#include "animation_root.hpp"
#include "population.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace omega {
struct Hotbox {
    PopulationHotbox record;
    bool enabled_2c{true};               // (-1, -1) association clears it (00173288)
    bool activated_2d{};                 // set by world activation (virtual +0x5c, 003ce980)
    bool removed_2e{};
    std::int32_t occupancy_98{};         // tracker count, 0..3 (00170660)
    std::int32_t priority_9c{10};        // 10 at load (00172b94); 00171260 keeps the first highest
    // Transform component +50 (003cdff0 with a static component, vtable
    // 004df6a0): its +0x10 matrix is the record's rows X, Y, Z (w 0) and the
    // translation (w 1) verbatim; its refresh (virtual +58, 00132900) only
    // clears the dirty byte.
    std::optional<std::array<float, 16>> world;
    std::vector<std::size_t> links;      // +0x60 link list: the links that are boxes (indices into the world)
    std::vector<std::uint32_t> link_keys; // every resolved link's +0x0c key, in +0x68 order
    // +0xa0, the interact prompt: 0 from the constructor (001735b0);
    // SetInteractMessage 0016fb70 stores 003fdb50(key) (nullopt for NULL),
    // ClearInteractMessage 0016fb60 stores 0. Read by the HUD update for the
    // selected interaction box (0024ee38: when 0, GetString("Interact")).
    std::optional<std::string> interact_message_a0;
    // Removed from the global list 004ee688 by 00170db0 (the index stays).
    bool deleted{};
    bool eligible() const { return !deleted && activated_2d && enabled_2c && !removed_2e; }
};
struct HotboxTracker;
struct HotboxWorld {
    std::vector<Hotbox> boxes;           // global list 004ee688, section order
    // 004ee6e0: the run-time boxes MakeNPCInteractable (00170460) appended, by index.
    std::vector<std::size_t> runtime_6e0;
    // 0040c540 / 0040c480 on the registry: the box with this +0x0c key (a box
    // removed by remove_object_hotbox is not found).
    std::optional<std::size_t> find(std::uint32_t id) const;
};
// Builds the boxes in record order. Links are resolved like 0040c480 during
// 00172ae0: only objects registered earlier resolve. objects are the keys of
// the other registered objects (the GOB reader 001c3510 runs in 001c3940
// before the BOX loader 001c0c00 in 001c4190, so mission objects resolve);
// links of types 8 and 12 may name them. Every other link must name an
// earlier box; a link to a later or missing key, or another type's link to
// an object, throws (not hosted). activated sets +2d on every box (the world
// activation of loaded hotboxes is not traced; see the note).
//
// The lift registration: at load a type-8 box walks its links, and for a link
// of class byte +0x49 = 0xe (a GOB whose asset is "no_vum", 0021af28) whose
// +0x98 vector (an InteractGOBJ's kept references, 00216ae0) has a first
// element with +0x68 = 3 (a lift, ElevatorGOBJ 00215700), appends itself to
// that lift's +0x118 recipient list (001732c0..00173388, 001734d0). No
// message is sent. The lift enables / disables its recipients with their
// vt+64 / vt+68 (research/ELEVATORS.md); the host builds the list
// (play_main) and applies those as set_hotbox_enabled.
// link_actors are the keys of the actors (NPC records, registered by
// 001c3940 before the BOX loader) a type-1/6/7 box may link to.
HotboxWorld build_hotbox_world(const std::vector<PopulationHotbox>& records, bool activated,
                               const std::vector<std::uint32_t>& objects = {},
                               const std::vector<std::uint32_t>& link_actors = {});
// The records of types 1, 3, 6 and 7 without a non-null link pair, 1, 6 and
// 7 whose links are all link_actors, 8 and 12 whose links all resolve (to an
// earlier kept box or one of objects), 9, 13, 15, 16 and 17 (host choice for the
// solo simulation).
std::vector<PopulationHotbox> select_hosted_hotboxes(const std::vector<PopulationHotbox>& records,
                                                     const std::vector<std::uint32_t>& objects = {},
                                                     const std::vector<std::uint32_t>& link_actors = {});
// The link messages of boxes of types 1, 6 and 7 (NPC_ACTIVATION_AND_RESPAWN.md
// section 1; interpreter goldens): EnableMsg (cEnableMsg, key 0055d4b8,
// vtable 004e0880, +24 = 003fb340 of the occupant's key, +1c = 0x40, byte
// +20 = 1) or DisableMsg (cDisableMsg, key 0055d498, vtable 004e0860, +24 =
// -1 in single player, +1c = 0x40), each sent to one link with transport 0.
// The +24 word is not read by the single-player actor handler and is not
// carried.
struct HotboxLinkMessage {
    std::size_t box{};
    std::uint32_t receiver{};            // the link's +0x0c key
    bool enable{};
    bool operator==(const HotboxLinkMessage&) const = default;
};
// 00172ae0's load loop (00173140..00173200): a DisableMsg to every resolved
// link of every type-1/6/7 box, in record and link order.
std::vector<HotboxLinkMessage> hotbox_load_link_messages(const HotboxWorld& world);
// The native receiver's link messages for an event on a hosted box: enter
// (payload 0) of types 1/6 (00172450) an EnableMsg to each link; exit
// (payload 2) of types 1/7 (00171f30 / 00172088) a DisableMsg to each link
// while the box's +2c is set (virtual +6c = 0015bfa0); nothing otherwise.
std::vector<HotboxLinkMessage> hotbox_link_messages(const HotboxWorld& world, std::size_t box, std::uint8_t payload);

// 001710a0(out, actor, mode): the bounds 00170e20 stores at a run-time box's
// +0x70 / +0x80 (interpreter goldens, gen_npc_hotbox_bounds.py). translation
// is the actor's transform component +40 (+50; its refresh through virtual
// +58 when the dirty byte +50 is set is not modelled: the port's components
// are current), bounds the actor's virtual +88 (00184380 on 004da690: the
// model context bounds +2e80). d = max - min (vsub.xyzw, 00171114); with
// full_xy (mode != 0, MakeNPCInteractable) only d.z is halved (div.s by 2.0,
// 00171168), otherwise every lane (vmulx 0.5, 00171144); then MAX.S x and y
// with 2.5, z with 4.0 (w kept); minimum = T - e (001711c0), maximum = T + e
// (001711ec), all four lanes. Non-finite inputs throw.
struct NpcInteractableBounds {
    RootVector minimum{}, maximum{};
    bool operator==(const NpcInteractableBounds&) const = default;
};
NpcInteractableBounds npc_interactable_bounds(const RootVector& translation, const RootVector& bounds_minimum,
                                              const RootVector& bounds_maximum, bool full_xy);
// 00170e20 + 00170460: a box with the constructor's state (001734f0 /
// 003cf0e0: +2c 1, +2e 0, +94 -1, +98 0, +9c 10, no transform component),
// activated through virtual +5c (003ce980, +2d = 1), type 3 written after
// construction (001704a4) and the NPC actor as its only link (00171c40),
// appended to the global list (00171020 -> 004ee688), so the existing
// indices stay. Returns its index; an ID already in the world throws.
std::size_t add_runtime_hotbox(HotboxWorld& world, std::uint32_t id, std::uint32_t npc_key,
                               const NpcInteractableBounds& bounds);
// 00170e20(*004ffbd0, object, 0, 1), a backpack's hotbox (00256860 /
// 00255d48): constructor 001734f0 as above, type 0x12 (a3 = 1; 8 otherwise),
// the bounds 001710a0(object, 0) (npc_interactable_bounds with full_xy
// false), appended to 004ee688 (00171020), the object as the only link
// (00171c40) and +2d = 1 (003ce980); +94 is the constructor's -1 until the
// caller stores the object's key there (002568d0 / 00255d48). Returns its
// index; an ID already in the world throws.
std::size_t add_object_hotbox(HotboxWorld& world, std::uint32_t id, std::uint32_t object_key,
                              const NpcInteractableBounds& bounds);
// 00170db0(tracker, box): 00138ab0 removes every entry equal to the box from
// the global list 004ee688 and from the tracker's inside list +28; no exit
// event is sent. The index stays (deleted).
void remove_object_hotbox(HotboxWorld& world, HotboxTracker& tracker, std::size_t box);
// 00170570 (the level reset 002570c0, 00134860): each box of 004ee6e0 is
// removed by 00138ab0 from the global list 004ee688 and from the tracker's
// +28 (an entry already removed is not found there: nothing), then the count
// 004ee6e4 = 0. The boxes are not freed; no exit event is sent.
void clear_runtime_hotboxes(HotboxWorld& world, HotboxTracker& tracker);
// The GOBJTriggerEventMsg (0021b790(msg, 0, 0, box, 0): key 004f5440, vtable
// 004daf10) the enter of a type-3 box sends each link with transport 0
// (00172598..00172600): +24 action 0, +28 0, +2c 0, +30 the box's key.
struct HotboxType3Trigger {
    std::size_t box{};
    std::uint32_t receiver{};
    std::uint32_t action_24{};
    std::uint32_t word_28{};
    std::uint32_t key_2c{};
    std::uint32_t key_30{};
    bool operator==(const HotboxType3Trigger&) const = default;
};
std::vector<HotboxType3Trigger> hotbox_type3_enter_triggers(const HotboxWorld& world, std::size_t box);

// 001719c0 (virtual +0x110): with a transform component, p (w set to 1)
// through its rigid inverse 0013eca0 (00171b30, row-vector VU sums);
// inside when p - min and max - p have no x/y/z lane with the sign flag set
// (faces inclusive, a -0 difference is outside).
bool hotbox_contains(const Hotbox& box, const RootVector& p);
// The hotbox vtable 004d9ac0's +e8 (003cd1c0, shared with the GOB vtable
// 004db4c0): with a transform component +50 its +0x10 matrix (world: the
// refresh 00132900 only clears the dirty byte), otherwise the identity
// 0055c9f0. Row 3 is the box's position.
std::array<float, 16> hotbox_matrix_e8(const Hotbox& box);
// +c0 (0014d340: `lw t9,(a1); lw t9,0xe4(t9); jr t9`, the selector ignored)
// -> +e4 (003cd260): +50 null -> (0, 0, 0, 1); the class byte +49 is 8 (both
// constructors pass a2 = 8 to 003cf0e0: 00172b2c, 001734f0) and +88
// (00173660, `addiu v0, a0, 0x70`) never null, so the component matrix
// times the centre (min + max) * 0.5 of the +0x70 / +0x80 bounds (w 1;
// object_position_e4). The eye a RequestFacial on a box (look entry 2) faces
// (interpreter goldens, gen_hotbox_eye.py).
RootVector hotbox_eye_c0(const Hotbox& box, std::uint64_t selector);

// 0013eca0: transpose the 3x3, keep row 3 when its xyz are all zero (c.eq.s),
// otherwise row 3 = ((0 - t).x * R0 + (0 - t).y * R1) + (0 - t).z * R2 over xyz
// (w kept), with R the transposed rows.
std::array<float, 16> hotbox_rigid_inverse(const std::array<float, 16>& m);

// The tracker 001716e0: actor +0x20, timer +0x24, inside list +0x28.
struct HotboxTracker {
    float timer_24{};
    std::vector<std::size_t> inside_28;
};
// A hotbox event (class 004ee698, vtable 004d9940) queued to a box
// (transport 0): +0x24 payload, +0x28 code (the box type), +0x2c = 0,
// +0x30 = the actor, +0x1c = 0x40, event byte +0x20 = 1 for types 5 and
// 0x12, otherwise 2.
struct HotboxTrackerEvent {
    std::size_t box{};
    std::uint8_t payload{};              // 0 enter, 2 exit
    std::uint32_t code{};
    std::uint8_t event_byte{};
    bool operator==(const HotboxTrackerEvent&) const = default;
};
// 00170660(tracker, p, 0) in a single-player game (005721c8 clear): timer
// max(0, t - 0.3); exits in inside-list order for boxes no longer eligible or
// no longer containing p (containment is evaluated first), +0x98 decremented
// (clamped at 0); then enters in global order for eligible boxes containing p
// that are not inside yet, +0x98 incremented (clamped at 3), appended.
std::vector<HotboxTrackerEvent> update_hotbox_tracker(HotboxWorld& world, HotboxTracker& tracker, const RootVector& p);

// The event a box sends to the actor (class 004ee698, transport 0): +0x24
// payload, +0x28 code (the box type), +0x2c word, event byte +0x20 = 8,
// +0x1c = 0x40.
struct HotboxActorEvent {
    std::uint32_t code{};
    std::uint8_t payload{};
    std::uint32_t word_2c{};
    bool operator==(const HotboxActorEvent&) const = default;
};
// 00171b60 on a hosted box: payload 0 -> 00172400, 2 -> 00171ed0, 1 and
// others nothing, 3 (virtual +0x114) throws. Enter: type 9 sends code 9 with
// +0x2c = the parameter (+0x94), and type 13 (00172710: the same message
// with event byte +0x20 = 5) code 13; 8 and 12 (001726b0) send their code with the
// parameter (event byte left at the constructor's 5, +0x1c 0: transport 0
// queues either way), and so does 0x12 (table 0049c8e0 entry 18: 001726b0);
// 15/16 send their code with +0x2c = the +0x0c key of
// the first linked object (read without a count check: a box without links
// throws); 1, 3, 6, 7 (no links) and 17 send nothing. A type-3 box with
// links (a run-time box) throws on enter: its GOBJTriggerEventMsg
// (hotbox_type3_enter_triggers) goes to the NPC actor, whose receipt
// (001b3b60, base 003cea10) is not analysed. Exit: 8, 12, 13, 0x12 (00172220, table 0049c890),
// 9, 15 and 16 send payload 2 with the parameter; 1, 3, 6, 7 and 17 nothing.
// This is the box's native receiver (+0x0c); the pump calls it only when no
// script handler took the event.
std::optional<HotboxActorEvent> receive_hotbox_event(const HotboxWorld& world, std::size_t box, std::uint8_t payload);

// 00171260(tracker, 0), the Triangle's box selection: none unless the timer
// +0x24 is <= 0 (c.ole.s: NaN fails, a denormal counts as 0). Otherwise the
// inside list in order: an eligible type-12 box while the actor's +0x320c
// (through tracker +0x20) is 7, 8 or 10 ends the search with none; an
// eligible box of type 8, 0x12 or 12 whose +0x9c is greater than the best so
// far (starting at -1, signed) becomes the choice.
std::optional<std::size_t> select_interaction_hotbox(const HotboxWorld& world, const HotboxTracker& tracker,
                                                     std::int32_t actor_mode_320c);
// A trigger message (0021b790: class key 004f5440, vtable 004daf10) a box's
// interaction sends one link with transport 0: +0x24 action (4, Interact),
// +0x28 the box parameter +0x94, +0x2c the box's key, +0x30 the actor's key.
struct HotboxTrigger {
    std::size_t box{};
    std::size_t link{};                  // index into the box's link_keys
    std::uint32_t receiver{};            // link_keys[link]
    std::uint32_t action_24{};
    std::uint32_t word_28{};
    std::uint32_t sender_2c{};
    bool operator==(const HotboxTrigger&) const = default;
};
struct HotboxInteraction {
    std::optional<std::size_t> box;      // the selected box; the edge is taken when set
    std::vector<HotboxTrigger> triggers;
    // Multiplayer only (001714c0 at 00171508..0017157c, a type-12 box): the
    // HotboxMsg (class key 004ee698, vtable 004d9940) {+24 = 3, +28 = 12,
    // +2c = 0, +30 = the tracker's actor +20, event byte 2} sent to the box
    // itself with transport 0, in place of the direct +0x114.
    bool box_message{};
};
// 001714c0(tracker, actor, 1, 0): the selection above; with a box, its
// virtual +0x114 with the actor, then timer +0x24 = 10.0. +0x114 of the base
// vtable 004d9ac0 (00171c90) sends each link the Interact trigger for types 8
// and 0x12; the type-12 subclass's (vtable 004d99a0, 00171880) does nothing
// in a solo game. The result of +0x114 is ignored. The object boxes
// add_object_hotbox makes (type 0x12, constructor 001734f0) carry the base
// vtable. With `multiplayer` (005721c8 set) a type-12 box gets the HotboxMsg
// (box_message) instead of the direct call; the other types are unchanged.
HotboxInteraction interact_with_hotbox(const HotboxWorld& world, HotboxTracker& tracker, std::int32_t actor_mode_320c,
                                       bool multiplayer = false);
// The box receiver 001717a0 (key 004ee698) for that HotboxMsg's action 3: the
// virtual +0x114 with the message's actor +30, i.e. 00171880 for a type-12
// box in a multiplayer game as the host (005721c8 and 005721c0 set): the
// trigger 0021b790(msg, 7, +94, actor, box) {+24 = 7, +28 the parameter, +2c
// the box's key, +30 the actor; event byte 1} sent to the first link +68[0]
// (read unchecked: throws without one) with transport 0. Throws for a box
// that is not type 12.
HotboxTrigger hotbox_multiplayer_interaction_trigger(const HotboxWorld& world, std::size_t box);
// The base +0x114 (00171c90, vtable 004d9ac0) of a type-8 or 0x12 box with an
// actor: the Interact trigger 0021b790(msg, 4, +94, actor, box) to each link
// in order, as interact_with_hotbox sends them. Throws for another type.
std::vector<HotboxTrigger> hotbox_interaction_triggers(const HotboxWorld& world, std::size_t box);
}
