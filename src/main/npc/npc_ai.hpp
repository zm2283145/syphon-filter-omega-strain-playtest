#pragma once
// The AI component (cNPC, vtable 004d9380, 0x1e0 bytes) for patrol
// (NPC_SPAWN_AND_AI.md, milestone N3): the mode block and the base update
// 0016f2d0 (AI events 1 and 0), the mode dispatch 001519a0 with the Patrol
// handler 00156f90, the message dispatcher 00151d30 for AI events and path
// events 2/4/5/6/7, the movement-state 0014d510 / weapon-draw 0014fc20 /
// travel-speed setters, the locomotion producer 0014bff0 and its grounding
// helper 001b0a50, the per-frame update 00159590 and the radius check
// 0015a440. Branches outside this subset throw std::runtime_error.
//
// Stages of 00159590 that the host may leave out (each frame lists them in
// NpcAiFrame::skipped): without NpcAiHost::attack (N5b, npc_attack.hpp) the
// aim/weapon update (component +60, 0022f5b0), the look-at selection
// 0014e0c0, the weapon posture 00150ca0 (which queues actor actions every
// frame) and the firing 00150340; without the perception
// (NpcAiHost::perception, N4a) the think frame's perception update 0016b580.
// The think frame's stuck detection and its relocation (0015ff90 and one
// Teleport Message, mode 2) are native; the snap-burst check after the perception update is
// native (its ForceWeaponFire 0014b470 too) and the hotbox occupancy (+1b4)
// throws when it would run. With the perception, Patrol leaves for mode 1 on
// AI events 4 and 7 (npc_alert.hpp); mode 4 Attack is npc_attack.hpp.
#include "npc_path_follower.hpp"
#include "weapon_inventory.hpp"
#include "animation_root.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace omega {
// A message delivered to the component: an AI message (key 004ee550, vtable
// 004d9340: +24 event, +28 word, +2c byte), a path message (004ed9b8, vtable
// 004d9760: +24 event, +28 value) or the mode-change notice cAIModeChangeMsg
// (004ea3e8, vtable 004d92e0: +24 the new mode), or the animation controller
// message (0055a1d8, vtable 004da950: +24 the code; kind controller).
struct NpcAiMessage {
    // los_result (N5c): the LOS-check result 005435a8 (vtable 004d91c0),
    // value = its byte +30 (hit).
    // weapon_fired (checkpoint 132sx): the script notice 004ff2a0 (0026f110,
    // vtable 004dcbf0) of the fire notice 0014f460; object = its +24 target.
    // The pump 003c8de0 offers it to the component's script filter
    // (0014a6c0); unhandled, 00151d30 does not test its key.
    // body_toss (checkpoint 132zf7): game::ActivateBodyToss's message (key
    // 00588038, vtable 004e1ae0, 00476740), value = its byte +24. The actor's
    // receiver forwards it (001b3c38); 00151d30 and the mode handlers never
    // load the key, so it falls to the base handler 003cb210.
    // pickupable_awareness (checkpoint 132zf7): the perception's dead-object
    // notice AIPickupableAwarenessMsg (004ff2a8, vtable 004dcbd0; 0016c9d0
    // sends it to the perception's +3c, the component); object = its +24.
    // Like weapon_fired the pump offers it to the script filter (0014a6c0);
    // the sender at 0016cd80 is the only reference to the key, so 00151d30
    // and the mode handlers drop it (003cb210).
    enum class Kind : std::uint8_t {ai,path,mode_change,controller,los_result,weapon_fired,body_toss,pickupable_awareness} kind{};
    std::uint8_t event{};
    std::int32_t value{-1};
    std::int32_t word_2c{};                                   // AI event 7: the cause (0016cf10); path event 0: a node search's second node (N5c); not written by other senders
    std::uint64_t object{};                                   // weapon_fired: the target (+24; 0: none); pickupable_awareness: the dead object (+24)
    bool thrown_30{};                                         // weapon_fired: +30, the held hold class (001437a0) is 1 (132sx10)
    bool operator==(const NpcAiMessage&) const=default;
};
// Everything the component sends, in order (003c9420).
struct NpcAiSend {
    enum class Kind : std::uint8_t {
        component,           // a message to the component itself (message)
        navigation_request,  // actor action 0 kind 0xd (vtable 004e0100): target, yaw, speed
        action,              // actor action message 0055d480 (vtable 004e0840): action, kind
        script,              // a script notification to the actor (key; node values)
        teleport,            // Teleport Message 0055d4e0 (vtable 004d9360): +24 mode, +30..+6f rows (001bc4b0)
        sound,               // 0020f990(msg, descriptor `key`, actor) to the sound manager 004ffb50
        // Checkpoint 132zf12: 002d7290's network message to the actor (key
        // 0051f0a8, vtable 004de4d0; npc_follow.hpp): action = +ac,
        // action_kind = +a0, node = +b0.
        network
    } kind{};
    std::uint8_t transport{};
    NpcAiMessage message;
    std::int32_t action{};std::uint8_t action_kind{};
    RootVector target{};float yaw{},speed{};
    std::uint32_t key{};std::int32_t node{-1},node2{-1};
    // NewAwareness (key 004ff298, vtable 004dcc90): +24 the object, +28 the cause byte.
    std::uint64_t object{};std::uint8_t cause{};
    // N5b payloads (npc_attack.hpp). Actor actions: 0x39 / 0x3d kind 4 (+30
    // the point: `point`), 0x15 / 0x16 kind 4 (+30 the running jump's point,
    // path-link types 5 / 0xd / 0xc), 0x3d / 0x3e kind 5 (+2c an object's ID), 0x3d kind
    // 1 (+2c a byte), 0x3b kind 0xb (+30 the target's ID or the none word
    // *004bdd00 = -1, +34 the subtarget, +38 the aim-view byte, +40 the
    // point, +50 the throw power). The weapon-fired notice (key 004ff2a0,
    // vtable 004dcbf0, 0026f110, to the component): +24 `object`, +28 the
    // held slot's item, +2c the shot count, +30 the thrown-class byte.
    std::int32_t word_28{},word_2c{},word_30{},word_34{};std::uint8_t byte_38{};
    RootVector point{};float power_50{};
    // Teleport Message: the mode byte +24 and rows 0..3 (+30..+6f).
    std::uint8_t mode_24{};std::array<RootVector,4> matrix{};
    bool operator==(const NpcAiSend&) const=default;
};
// Script notification keys (NPC_ACTIVATION_AND_RESPAWN.md).
inline constexpr std::uint32_t npc_script_death=0x004ff278;   // 0014f680: +24 killer, +28 damage type
// 0014d1e4 / 0014d1ec: the descriptor of the 0014bff0 tail's sound (V_BURN_SCREAM).
inline constexpr std::uint32_t npc_ledge_fall_sound=0x004f1610;
inline constexpr std::uint32_t npc_script_waypoint_reached=0x004ff288,npc_script_destination_reached=0x004ff290,
    npc_script_time_elapsed=0x004ff280,npc_script_activated=0x004ff268,npc_script_deactivated=0x004ff270;

struct NpcAiComponent {
    std::int32_t mode_44{},requested_48{},priority_4c{};std::uint8_t pending_50{1};   // 0016e170
    float skill_38{1.0F};                                     // +38 (0022f2d0, SetSkill)
    std::int32_t team_54{4};                                  // +54 (0016f5b0: 4 when the actor has no player component; SetTeam)
    std::uint8_t quiet_5c{};                                  // +5c: no Activated notification (0016f1c0)
    std::uint8_t quiet_5d{};                                  // +5d: no Deactivated notification (0016f0e0)
    std::uint32_t flags_68{};
    std::int32_t field_84{},field_88{};
    float fire_delay_8c{};                                    // +8c (Activate: 0.2 + 0.01 (id & 15) unarmed)
    std::int32_t ammo_a4{},field_a8{};
    // +b0 / +b4: the held item Activate's first-activation branch reads
    // through virtual +1a4 (0014bcb0: -1 while the inventory mode +84 is 6).
    std::int32_t held_item_b0{},held_item_b4{};
    std::uint8_t byte_166{};
    float script_timer_58{-1.0F};
    float travel_speed_60{0.1F};std::int32_t speed_priority_64{};
    float firing_threshold_78{-1.0F};                         // +78 (0014bce0, SetFiringThreshold)
    float stop_timer_7c{};                                    // StopMovement countdown
    float radius_squared_98{};
    std::int32_t field_9c{-1};
    std::int32_t movement_priority_a0{};
    std::int32_t timer_e8{};                                  // countdown in updates (AI event 3)
    std::int32_t action_114{};                                // current path-link action type
    std::int32_t draw_priority_120{-1};
    std::uint32_t flags_124{};
    float timer_128{};
    std::int32_t pending_node_138{-1};
    // +f4: the object ID of a go-to-object request (0014be10; -1 from
    // MoveToNode 0014be50). The constructor does not write it; both requests
    // of mode 7 do before it can be entered (natively -1).
    std::int32_t object_f4{-1};
    // +f0: the carry-body node index (0014bd60, CarryBodyToNode; Checkpoint
    // 132zf9). Written with +f4 before mode 8 (00153150) can be entered.
    std::int32_t carry_node_f0{-1};
    // +100 / +104 / +108 / +10c: Follow's two floats and two zeroed words
    // (001488f0, Checkpoint 132zf11), written with +f4 before mode 9
    // (00153870, npc_follow.hpp, Checkpoint 132zf12) can be entered. Mode 9
    // also uses +f0 (carry_node_f0) as this NPC's region and +ec / +e8.
    float follow_100{}, follow_104{};
    std::uint32_t follow_108{}, follow_10c{};
    // ForceWeaponFire (virtual +198, 0014b470): +12c the item (-1: none), +130
    // the shot count, +134 the shot delay (cvt.w.s of the given delay, or of
    // +8c when it is 0). 00158bc0 writes +130 = +134 = 0; +12c is written only
    // by 0014b470 (natively 0 until then).
    std::int32_t force_item_12c{},force_count_130{},force_delay_134{};
    std::int8_t state_13c{};std::uint8_t draw_13d{},byte_13e{1},byte_13f{},byte_140{};
    // +118: the priority of the fire state +13e (0014b550; 00158bc0 and
    // 0015b7f0 write 0).
    std::int32_t fire_priority_118{};
    // +141: bit 2 DeactivateDead, bit 5 first activation pending, bit 6
    // speed by path link, bit 7 radius-controlled. The constructor 00158bc0
    // sets bits 2, 4 and 5 and clears 6 and 7 (0x34); bits 0, 1 and 3 keep
    // the allocated block's previous content (NpcAiController takes 0).
    std::uint8_t flags_141{};
    float timer_154{};
    std::uint8_t contact_158{},inactive_159{1};
    std::uint8_t occupied_hotbox_1b4{};                       // +1b4 non-null
    float since_think_1bc{},stuck_1c0{999999.0F},stuck_1c4{};
    std::int32_t facing_node_1d0{-1};                         // +1d0 node object (-1: null)
    std::uint8_t latch_1d4{},latch_1d5{},byte_1d6{};
    std::uint8_t perception_flags_20{};                       // perception (+1ac) byte +20
    // N4b (npc_alert.hpp). +6c aggressiveness (00158bc0: 0.25 + 0.5 r / 2^31;
    // SetAggressiveness). +80 the time since the last look request (constructor
    // 100.0; 0014dfb0 writes 0; advanced by the look-at selection 0014e0c0,
    // which the update runs only with NpcAiHost::attack: look_timer_80_reset
    // marks a reset value that an update without it does not advance). +110 /
    // +112 the pending pair
    // (tag, mode). The look entries (20 bytes from +16c: object +0, ID +4,
    // duration +8, priority +c, active +10, flag +11; the constructor clears
    // the priorities and active bytes): entries 0 and 1 here, entry 2
    // (+194..+1a5, written by RequestFacial's 0014dea0) in look_entry_2; the
    // active byte of entry i is what 0014d390 (+a8(i)) reads. +160, +164 the active
    // entry, +168 resume movement after a look (not written by the
    // constructor; the native block starts at 0).
    float aggressiveness_6c{};
    float look_timer_80{100.0F};
    bool look_timer_80_reset{};
    std::int16_t pending_tag_110{},pending_mode_112{};
    struct LookEntry {
        std::uint64_t object{};std::int32_t id{-1};float duration{};std::int32_t priority{};std::uint8_t active{},flag{};
    };
    std::array<LookEntry,2> look_entries{};
    LookEntry look_entry_2{};
    std::int32_t look_160{};
    std::uint8_t look_index_164{},look_resume_168{};
    // N5c look-around (npc_search.hpp, static): +15c the time left on the
    // current look point, +165 pick the nearest point (else the first).
    // Not written by the constructor (natively 0).
    float look_timer_15c{};
    std::uint8_t look_nearest_165{};
    // N5b (npc_attack.hpp; 00158bc0 values unless noted): +e0 / +e4 the
    // firing-position / cover node (-1), +ec the per-mode counter (not
    // written by the constructor; natively 0), +90 / +94 the throw scatter
    // 4.0 and angle 35.0, +b8 the weapon switch distance 0, +bc..+dc the
    // allowed-region bits (all set), +11c the aim subtarget (-1), +1c8 / +1cc
    // the throw target handle (0, -1), +167 written 0 by every look-at
    // selection, +1a8 a head look sent (0), +1b8 the look slot (an index into
    // NpcAiSystem::look_slots, set by Activate; -1 before).
    std::int32_t firing_node_e0{-1},cover_node_e4{-1},mode_counter_ec{};
    // N5c: +70 / +74 the node searches' query distances (00158bc0: -100.0,
    // 100.0; interpreter).
    float query_70{-100.0F},query_74{100.0F};
    float throw_scatter_90{4.0F},throw_angle_94{35.0F},switch_distance_b8{};
    std::array<std::uint32_t,9> allowed_regions_bc{0xffffffffU,0xffffffffU,0xffffffffU,0xffffffffU,0xffffffffU,0xffffffffU,
        0xffffffffU,0xffffffffU,0xffffffffU};
    std::int32_t subtarget_11c{-1};
    std::uint64_t throw_target_1c8{};std::int32_t throw_target_id_1cc{-1};
    std::uint8_t look_167{},look_sent_1a8{};
    std::int32_t look_slot_1b8{-1};
    NpcPathFollower path;                                     // +1b0
};
// A point of a look slot's list (0x14 bytes of list data): +0 the object (a
// node object, 0014f240), +4 its ID +c, +8 the look duration, +c the times
// it was found already seen while it was the best so far, +10 used.
struct NpcLookPoint {
    std::uint64_t object{};
    std::int32_t id{-1};
    float duration{};
    std::int32_t count{};
    std::uint8_t used{};
};
// A look slot of 004ea1d0 (16 entries of 0x18 bytes): +0 / +4 the list the
// look-around fills (0014f2e0 empties it, 0014f240 appends; npc_search.hpp),
// +c the shots counted by the fire notice 0014f460, +10 the frame of its last
// weapon-fired notice, +14 in use (00158a80 clears it; Activate takes the
// first free slot, or shares slot 0 when none is free; Deactivate frees it).
struct NpcLookSlot {
    std::vector<NpcLookPoint> points;
    std::int32_t count_c{},frame_10{};
    std::uint8_t in_use_14{};
};

// What the component reads from and does to its actor (component +30).
struct NpcAiActor {
    std::function<bool()> eligible;                           // +2c/+2d/+2e
    std::function<std::uint8_t()> life_state;                 // virtual +108 (001872f0)
    // Virtual +f4 (00132280): the 'flor' root world matrix, rows x, y, z, t.
    std::function<std::array<RootVector,4>()> world_matrix;
    std::function<std::uint32_t()> flags_3204,flags_3210;
    std::function<bool()> navigation_enabled_30ac;
    std::function<std::uint8_t()> navigation_arrived_30ad;
    // Physical component +3310: present, mode +20, +dc (unsupported), +e8
    // (floor contact; virtual +f8 0014d3d0).
    std::function<bool()> physical_present;
    std::function<std::int32_t()> physical_mode_20;
    std::function<bool()> physical_dc,physical_e8;
    std::function<float()> physical_2c8;                       // physical +2c8 (contact blend)
    std::function<std::uint8_t()> killed_33a5;                 // actor +33a5
    std::function<std::uint32_t()> mode_320c;                  // actor +320c (top action)
    // Roots +3290/+3294/+3298 all without a parent (+b0).
    std::function<bool()> roots_unparented;
    // Actor +3524 inventory: weapon mode +84 (6: nothing) and 001437a0 == 1.
    std::function<std::uint8_t()> inventory_mode_84;
    // 001437a0(+3524): the held item's hold class (record +f0, signed; 11
    // with nothing held). Read by the locomotion's aim facing; empty: that
    // read throws.
    std::function<std::int32_t()> held_class;
    // 001b0e88..0ebc: physical +330 = +320 = +310 = 0, +300 = +2f0.
    std::function<void()> stop_physical_motion;
    // 001b0ec0..001b0ed8: with actor action 10 (+3210 bit 10) after the stop, the landing 001a0b30(actor, 0)
    // (the retained motion reset, as the teleport tail's; actor_hanging.hpp land_actor argument 0).
    std::function<void()> land_reset;
    std::function<void(std::uint32_t)> set_flags_3038;
    // The actor's priority actions 0x1d / 0x1e (0019e670).
    std::function<bool()> falling_dead_1d_1e;
    // 001b0ef0: 0019e670(actor, 1), the dead landing (actor_death.hpp
    // land_dead_actor) with the AI controller's +114 = 0 (0019e978).
    std::function<void()> land_dead;
    // 001b0aa8..001b0c18, a dying or dead actor without floor contact: with
    // +3034 bit 1 clear, the query 00148290 from virtual +e0 by (0, 0, -100)
    // (required mask 0, excluded 0x40, the actor excluded); a miss, or +3034
    // bit 1, sets +3038 bit 1. True: the query hit.
    std::function<bool()> dying_ground_probe;
    // The secondary component +3314 (nullopt: none) with its upper-body
    // states +310 / +311 (the think frame's snap-burst check, 00159ae4).
    std::function<std::optional<std::array<std::uint8_t,2>>()> secondary_3314;
    // 0014d460 / 0014d450 / 0014d410: the region of the actor's region
    // tracker (*(+2fe0) = +32a0, vtable 004da2f0; its +50 / +54 weak handle
    // that 002245f0 caches) as its +2c, +2d, +2e (nullopt: none). Read for
    // a carried body (action 22); empty: that read throws.
    std::function<std::optional<std::array<std::uint8_t,3>>()> region_2fe0;
    // Synchronous delivery (transport 1) of the navigation request and of
    // actor actions to 001a92a0; transport 0 actions are queued by the host.
    std::function<void(const NpcAiSend&)> deliver;
};
// The global state the AI shares: the think list 004ea190 with the selected
// thinker 004ea358 and the frame the selection was made for (004ea3a0), and
// the frame counter 005383c8; the far-distance global 0053834c.
struct NpcAiThinker {
    NpcAiComponent* component{};
    std::function<bool()> eligible;                           // its actor's +2c/+2d/+2e
    std::function<std::uint8_t()> life_state;                 // its actor's virtual +108
    std::uint32_t actor_id{};                                 // its actor's +c (0014fa00 compares actors)
    // Component virtual +44 (00157a40), read by 00159590 for a dying or dead
    // actor (an alive one is true); required only then.
    std::function<bool()> alive_44;
    // 132ln5h13: the controller's virtual +88 (00159590 selects only entries answering 1): 1 the AI controller
    // 0x004d9380, 2 the remote NPC controller 0x004de2a0 (002d40a0 puts it on the think list; npc_remote.hpp).
    std::uint8_t controller_88{1};
};
// A component and its actor as 00159020 reads them (npc_death_notice.hpp).
struct NpcCorpseView {
    std::uint8_t life_108{};                 // actor virtual +108
    std::uint8_t flags_141{};                // component +141 (bit 2: DeactivateDead)
    std::array<std::uint8_t,3> root{};       // actor +2c, +2d, +2e
    std::uint8_t pickupable_3468{};
    bool held_354c{};                        // the actor's +354c / +3550 weak handle resolves
    std::int32_t action_114{};               // component +114
    std::uint32_t flags_3204{};              // actor +3204 (bit 7 read)
    std::uint32_t word_38{};                 // actor +38
    std::function<RootVector()> eye_c0;      // actor virtual +c0(actor, -1), read only when needed
};
struct NpcRouteManager;
struct NpcDeadListEntry {
    NpcAiComponent* component{};
    std::function<NpcCorpseView()> view;
};
struct NpcAiSystem {
    // Insertion order (00157df0 -> 0015af50; inferred: appended).
    std::vector<NpcAiThinker> think_list;
    NpcAiComponent* thinker{};
    std::optional<std::uint32_t> selected_frame;
    std::uint32_t frame{};
    float far_distance{};
    // 004ea350 (00158a80: 4): the cap of the generator spawn check 0021edf0;
    // SetMaxActiveNpcCount (00149450 -> 0014b340) stores it and ignores its
    // receiver.
    std::int32_t max_active_npc_count{4};
    // The dead list 004ea370: components appended by the death notice
    // 0014f680 (0014ab90) when their actor is present.
    std::vector<NpcDeadListEntry> dead_list;
    // The camera object's translation (004ffc14's object, +50 transform +40)
    // for 00159020; nullopt throws when it is needed.
    std::function<std::optional<RootVector>()> camera_position;
    // The region selection 002245f0 (retained, 00224ac0 among its members,
    // else 002248a0) of an actor's region tracker +32a0 for its position
    // (virtual +e0, stored by 0013aca0): the region's +2c, +2d, +2e, nullopt
    // without one. Empty: NpcAiActor::region_2fe0 stays empty.
    std::function<std::optional<std::array<std::uint8_t,3>>(std::uint64_t actor,const RootVector& position)> actor_region;
    // The route manager 004ed9b0 (npc_route.hpp) the controllers link into
    // NpcAiHost::routes; null: route following throws.
    NpcRouteManager* routes{};
    // Counts the travel speed's (0014d730) route distances 00160130 read
    // from a route buffer no route has written (npc_unwritten_route_distance,
    // measured for level-loaded actors), so a host can report them. Null: not
    // counted.
    std::uint32_t* unwritten_route_reads{};
    // Counts the same reads by actors not loaded with the level (a generator's
    // spawn) whose speed does not depend on the heap content they read
    // (npc_travel_speed_for_purpose). Null: not counted.
    std::uint32_t* value_free_route_reads{};
    // Undefined-read policy (GENERATOR_RUNTIME.md, checkpoint 119d): when such
    // an actor's speed does depend on the heap content, the original's result
    // is not reproducible; non-null takes the level-loaded measured point (the
    // origin) and counts it, null throws.
    std::uint32_t* spawned_route_policy_reads{};
    // The +2c/+2d/+2e eligibility of a node's association object (node +60,
    // NpcNode::association_key) when the node graph does not decide it; null
    // throws when the locomotion producer reads one (0014c08c).
    std::function<bool(std::uint32_t key)> node_association_eligible;
    // 00158a80: the look slots 004ea1d0 (all free) and WorthShootingThreshold
    // 004ea388 = 0.5 (the firing threshold when +78 is negative).
    std::array<NpcLookSlot,16> look_slots{};
    float worth_shooting_388{0.5F};
};
struct NpcPerception;
struct NpcPerceptionHost;
struct NpcRouteClient;
struct NpcAttackHost;
struct NpcFollowHost;
struct NpcCarryHost;
struct NpcGoToBodyHost;
struct NpcAiHost {
    NpcAiActor actor;
    const NpcNodeGraph* graph{};
    // 132ln5h13: 0014bff0's network game part (*005721c8; unset outside one, PORT-ONLY: the play sets them for a
    // generator NPC of a LAN game, LanNetCore answers): network_contact at 0014c510..0014c5f8 (the NPC info entry's
    // contact byte 00433ec0 / 00433f20; with actor +14 and +3584 the cNetHuman block's +44..+4c = the world
    // position, virtual +e8's row 3), network_speed at 0014c6e0.. (+14 and +3584: the block's +3c), and
    // network_heading the navigation request's yaw, which the locomotion message's receiver 001a99a4 writes to
    // the block's +40 while +14 is set.
    std::function<void(std::uint8_t contact_158,const RootVector& position)> network_contact;
    std::function<void(float speed)> network_speed;
    std::function<void(float yaw)> network_heading;
    // 132ln5h13: the remote NPC controller 0x004de2a0 (002d6030; npc_remote.hpp) answers 0014bff0's virtual calls
    // on the controller from its own fields: +d8 002d3480 the speed +200, +a4 002d2bd0 (1 while +200 > 1e-6), +a8
    // 002d2b50 (0: no look entry 2), +b4 002d3ff0 / +ac 002d4030 (the cNetHuman linked and its +0x20) and +b0
    // 002d2b60 (the cNetHuman's +0x10..+0x18, w = 1). Unset: the AI controller's.
    struct RemoteVirtuals {
        float speed_200{};
        bool aiming_20{};
        RootVector aim_point_10{};
    };
    std::optional<RemoteVirtuals> remote;
    // N5a (npc_route.hpp): the route manager 004ed9b0 and this actor's
    // request handle (its ID, +f4 translation and the synchronous delivery
    // of path events to this component); without them route following throws.
    NpcRouteManager* routes{};
    std::shared_ptr<NpcRouteClient> route_client;
    std::uint32_t* unwritten_route_reads{};                   // NpcAiSystem::unwritten_route_reads
    std::uint32_t* value_free_route_reads{};                  // NpcAiSystem::value_free_route_reads
    std::uint32_t* spawned_route_policy_reads{};              // NpcAiSystem::spawned_route_policy_reads
    std::function<bool(std::uint32_t key)> node_association_eligible;   // NpcAiSystem::node_association_eligible
    // The actor was created by the level load (a population record), not by
    // a generator: the travel speed then reads an unwritten route buffer's
    // point as measured (npc_unwritten_route_distance); otherwise it throws
    // unless the speed does not depend on that point (npc_travel_speed_for_purpose)
    // or the host applies the undefined-read policy (spawned_route_policy_reads).
    bool level_loaded{};
    // The perception (component +1ac) and what it reads (npc_perception_think.hpp);
    // without them 0016b580 is skipped and the perception paths throw.
    NpcPerception* perception{};
    const NpcPerceptionHost* sense{};
    std::function<std::uint32_t()> random;                    // 00127dd0 (0014d730)
    std::function<std::int32_t()> held_weapon_rating;         // 00142b50(actor +3524) (mode 1)
    std::function<std::uint64_t(std::int32_t id)> object_by_id;   // 0040c540 (the dispatcher's AI event 7)
    // Every send in order; a component message with transport 0 is also
    // queued for the next pump by the host.
    std::function<void(const NpcAiSend&)> send;
    // The dying/dead branch (0015a2f8): 00159020 and actor virtual +60(1).
    std::function<bool()> dead_removal_allowed;
    std::function<void()> remove_dead_actor;
    // N5b (npc_attack.hpp): the aim update (virtual +60), the look-at
    // selection 0014e0c0, the weapon posture 00150ca0 and the firing gate
    // 00150340 of the update. Needs the perception; without it these stages
    // are listed as skipped (and a death burst pending in the dying branch
    // throws).
    NpcAttackHost* attack{};
    // N5c: the shared AI state, for the look slot list 004ea1d0 of the
    // look-around (0014f2e0 / 0014f240 / 0014e0c0 / 0014ee90); null: reading
    // an assigned slot throws.
    NpcAiSystem* system{};
    // Checkpoint 132zf12: mode 9 Follow (00153870, npc_follow.hpp); null:
    // the mode throws.
    NpcFollowHost* follow{};
    // Checkpoint 132uk5: mode 8 CarryBody (00153150, npc_carry_body.hpp);
    // null: its AI event 3 throws.
    NpcCarryHost* carry{};
    // Checkpoint 132ms4: mode 7's enter toward a dead object (00154660,
    // npc_script_control.hpp); null: that route throws.
    NpcGoToBodyHost* go_to_body{};
    // Checkpoint 132uk6d: the f20 the sender of the message being dispatched
    // left in the register (callee-saved; 001519a0, 00151d30 and the mode
    // handlers do not write it before their 0015d790 calls). The base update
    // sets it to its dt while it sends a mode change (0016f2d0: 0016f2ec
    // mov.s f20, f12; interpreter goldens tools/ee/gen_mode7_enter_f20.py);
    // nullopt elsewhere (not hosted).
    std::optional<float> sender_f20;
};
struct NpcAiFrame {
    bool think{};
    std::vector<std::string> skipped;
};

// 0016e170 (mode request): accepted when priority >= +4c.
bool request_npc_ai_mode(NpcAiComponent& c,std::int32_t mode,std::int32_t priority);
// 0014d510 (movement state with priority).
void set_npc_movement_state(NpcAiComponent& c,std::int8_t state,std::int32_t priority,NpcAiHost& host);
// 0014fc20 (weapon-draw request +13d with priority +120).
void request_npc_weapon_draw(NpcAiComponent& c,std::uint8_t value,std::int32_t priority);
// The think frame's stuck check (00159cc0..0015a088): progress along the
// path's dominant axis; more than 2 s without a new minimum relocates the
// actor onto path +50 (0015ff90, then a Teleport Message mode 2 with
// transport 1); the timer then still adds +1bc (0015a068).
void npc_stuck_check(NpcAiComponent& c,NpcAiHost& host);
// 0014afe0(c, node) (cNPC virtual +10c; script Teleport 00149c30 after
// 0015c120): a Teleport Message (key 0055d4e0, vtable 004d9360, +24 mode 0)
// with rows 0..2 the identity 0055c9f0 (0014b210, 0014b160) and row 3 the
// node position (0015c2c0) with w = 1 (0014b1e0), sent to the actor with
// transport 1 (003c9420 -> 001bc4b0 translate); then 0015ff90(+1b0, node)
// (relocate_npc_path_follower), ResumeAIMode (virtual +e0 = 0014b570), +a0 =
// 0 and 0014d510(c, 0, 0). The multiplayer send 002d7290 behind byte
// 005721c8 is skipped in solo.
void npc_teleport_to_node(NpcAiComponent& c,std::int32_t node,NpcAiHost& host);
// Port-only (checkpoint 132ob10, --coop-partner): TeleportToNode's path-follower
// step alone (0014b07c: 0015ff90(+1b0, node)), for an actor the port placed
// itself (001bc4b0 mode 2) next to `node`, so its next route search opens
// from that node (00162080 state 0 opens the +50 / +54 nodes), not from the
// nodes it held before the move.
// Checkpoint 132ob11: then npc_port_drop_path_segment.
void npc_port_relocate_path_follower(NpcAiComponent& c,std::int32_t node,NpcAiHost& host);
// Port-only (checkpoint 132ob11, --coop-partner): after the port moved the actor, the path segment of the route it
// held before (+50 when it is not the held node +4c) is dropped (+50 = -1) and the stuck counters reset (+1c0 =
// 999999, +1c4 = 0), so the stuck relocation (00159e84, which needs +50 >= 0) cannot send it back onto the old
// route's node. Route searches open from +4c (00162080), so they are unchanged. TORONTO1 P23: a 96 m jump
// back to the node 136628 of a route cut by a test teleport 2500 frames before, while it fought (AI mode 3).
void npc_port_drop_path_segment(NpcAiComponent& c);
// 00159ea0..00159f98: the relocation frame from the path direction with z
// cleared; rows side, forward, up, (0, 0, 0, 1). The helper-axis frame
// 00131c70 cannot be reached with z = 0 and throws.
std::array<RootVector,4> npc_relocation_frame(const RootVector& direction);
// Virtual +120 = 0014b550(c, state, priority): priority >= +118 -> the fire
// state +13e = state (0 hold fire, 1 normal, 2 fire), +118 = priority.
void request_npc_fire_state(NpcAiComponent& c,std::uint8_t state,std::int32_t priority);
// Virtual +14c = 0014d670: the speed class of +60: < 0.0005 (3a03126f) 0,
// < 0.25 1, <= 0.75 2, else 3 (NaN 3).
std::uint8_t npc_speed_class(const NpcAiComponent& c);
// 0014d900(c, eAction, priority, value), RequestAction: an action message
// (0055d480, vtable 004e0840, kind +28 = 0) to the actor with transport 0:
// 1 (Crouch) -> action 1 and +124 bit 0 set, 9 -> action 2 and bit 0
// cleared, both only when priority >= +9c, which takes it; 2 -> action 8,
// bit 1 set; 4 -> action 9, bit 2 set; 0xa -> action 0xa, bit 2 cleared; 8
// -> action 0x40 unless bit 3 is set, then bit 3 set and +128 = value; 0xb
// -> with bit 3 action 0x41 and bit 3 cleared. Other values do nothing.
void npc_request_action(NpcAiComponent& c,std::uint8_t action,std::int32_t priority,float value,NpcAiHost& host);
// 00151a90: the animation controller message (key 0055a1d8, +24 = code) to a
// living component. Code 6 -> action 0x11 with +114 == 4, else 0x13; 9 ->
// 0x11 with +114 == 4; 8 -> 0x0f with +114 == 4, 0x10 with 0xb, 0x1a with 3
// unless the actor's +320c is 0x10. Each is kind 0, transport 0; those codes
// return 1, any other (the stance codes 2..5) returns 0 without effect.
bool npc_controller_message(NpcAiComponent& c,std::int32_t code,NpcAiHost& host);
// Virtual +1ac = 0014bae0(c, item): 001439b0(inv, item) for item >= 0, else
// 00143c50(inv); then for an armed actor (inventory +84 != 6, 001b16c0) the
// burst length +a4 = 3 + (id & 7) + (r & 1), reduced to max(1, clip size /
// 2) when the current weapon's clip size (001438e0(inv, 6)) is smaller,
// else +a4 = 0; +a8 = 0. The RNG is drawn only when armed.
void npc_select_item(NpcAiComponent& c,std::int32_t item,WeaponInventoryOps& inventory,std::uint32_t actor_id,
    const std::function<std::uint32_t()>& random);
// The burst reroll shared by 0014bae0's and 0014bbd0's tails (from 001b16c0).
void npc_reroll_burst(NpcAiComponent& c,WeaponInventoryOps& inventory,std::uint32_t actor_id,
    const std::function<std::uint32_t()>& random);
// StoreWeapon(NPC) 00149a80 -> virtual +1a8 = 0014bbd0(c): 00143b60(actor
// +3524) (holster the current weapon), then the same reroll as 0014bae0
// (an NPC that is still armed, e.g. while the switch gate refuses, rerolls).
void npc_store_weapon(NpcAiComponent& c,WeaponInventoryOps& inventory,std::uint32_t actor_id,
    const std::function<std::uint32_t()>& random);
// Checkpoint 132sx10: RemoveWeapon(NPC, int) 00149930 -> virtual +1bc =
// 0014b790(c, item): 00142450(actor +3524, 00143d20(item), 0, 0, 0) (the
// item's class slot is emptied whatever it holds), then the same burst reroll
// as 0014bae0 (+a4 / +a8, the RNG drawn when still armed).
void npc_remove_weapon(NpcAiComponent& c,std::int32_t item,WeaponInventoryOps& inventory,std::uint32_t actor_id,
    const std::function<std::uint32_t()>& random);
// Checkpoint 132sx8 component writers: SetAimMode(NPC, mode) 0014a190 (lbu
// of the enum word) -> +114 = 0014fc40 -> +bc(c, mode, 100) = 0014fc20
// (request_npc_weapon_draw); ResumeAIAiming 0014a100 -> +11c = 0014a130:
// +120 = 0, +13d = 1, +11c = -1; SetAimSubtarget 0014a150 (lb of the enum
// word) -> +118 = 0014a180: +11c = the sign-extended byte;
// ResumeAITravelSpeed 00149d70 -> +144 = 00149da0: +64 = 0.
void npc_resume_ai_aiming(NpcAiComponent& c);
void npc_set_aim_subtarget(NpcAiComponent& c,std::int8_t subtarget);
void npc_resume_ai_travel_speed(NpcAiComponent& c);
// 00149d50: travel speed of eTravelSpeed (table 00489dd0: Stand 0, Walk 0.1, Jog 0.5, Run 0.9).
float npc_travel_speed(std::uint8_t speed_enum);
// 0014d6f0 (script SetTravelSpeed, priority 100) / 0014d470 (+158 ResumeMovement).
void set_npc_travel_speed(NpcAiComponent& c,float speed);
void resume_npc_movement(NpcAiComponent& c,NpcAiHost& host);
// 001519a0 for mode 0 (Patrol, 00156f90); other modes throw. Returns the
// handler's "handled" byte.
bool dispatch_npc_ai_mode(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host);
// 00151d30 (component virtual +0c).
bool dispatch_npc_ai(NpcAiComponent& c,const NpcAiMessage& m,NpcAiHost& host);
// 0016f2d0 (base update): a pending mode change sends AI events 1 then 0 to
// the component (transport 1), then the +58 script timer.
void update_npc_ai_base(NpcAiComponent& c,float dt,NpcAiHost& host);
// 0014bff0 (locomotion producer).
void run_npc_locomotion(NpcAiComponent& c,NpcAiHost& host);
// 132ln5h13, for the remote NPC controller (npc_remote.cpp): a send with transport 1 to the actor also reaches
// 001a92a0 at once; the path follower's host (the actor's virtual +f4 row 3, path events to the component with
// transport 0); the +2c / +2d / +2e check of a node's association object (0014c08c..0014c0e8).
void npc_send_to_actor(NpcAiHost& host,const NpcAiSend& s);
NpcPathHost npc_path_host(NpcAiHost& host);
bool npc_node_association_eligible(const NpcNode& n,const NpcAiHost& host);
// 0014d1a0..0014d240, 0014bff0's tail after every branch: with +1d6 (the
// kill's death code 2 over a type-7 link) and actor +3204 bit 7, +1d6 = 0 and
// the static sound 004f1610 (0020f990, owned by the actor; either +3260)
// goes to the sound manager 004ffb50 with transport 0.
void npc_ledge_fall_sound_tail(NpcAiComponent& c,NpcAiHost& host);
// 0014c710..0014c7f4, 0014bff0's facing from look entry 2 (RequestFacial):
// nullopt when +a8(2) (0014d390, the active byte +1a4) is 0 (0014c7f8 goes on
// to the walk / aim facing); otherwise the handle {+194, +198} resolved by
// 0014d350 (`resolve`: the object when its +c is the ID, else 0) and the
// facing vector is the object's eye (virtual +c0(sp+210, object, -1), `eye`)
// minus the translation (vsub.xyzw, all four lanes), to 0014c990. An
// unresolved object branches to 0014c990 with the facing vector sp+610
// never written in this call (undefined in the original): throws.
std::optional<RootVector> npc_facial_facing(const NpcAiComponent& c,const RootVector& translation,
    const std::function<std::uint64_t(std::uint64_t object,std::int32_t id)>& resolve,
    const std::function<RootVector(std::uint64_t object)>& eye);
// 0014ca28..0014cb40, 0014bff0's path-link type 9 turn (moving state 1): with
// +114 = 9, +124 bit 1 clear and actor +3204 bit 5 (s0) clear, the path yaw
// 00121ab8(-x, y) of the follower's direction +1b0+10 minus the facing yaw of
// the actor's matrix row 1 (virtual +f4), wrapped once into [-pi, pi] as in
// state 2; when |difference| <= 25 degrees (3edf66f3) RequestAction(2, 0, 0)
// (0014d900: actor action 8, +124 bit 1 set), then +124 bit 1 cleared and
// +114 = 0. The navigation request follows unchanged.
void npc_link9_turn(NpcAiComponent& c,const RootVector& path_direction,const RootVector& facing_row,bool s0,NpcAiHost& host);
// 001b0a50(actor, a1, a2, height) for an actor with an AI component.
void ground_npc_actor(NpcAiHost& host,bool a1,bool a2,float height);
// 00159590 (component virtual +40), the patrol subset described above.
NpcAiFrame update_npc_ai(NpcAiComponent& c,float dt,NpcAiSystem& system,NpcAiHost& host);
// Activate (component virtual +48, 00157df0).
// Without its actor present it calls the actor's activation (actor +5c,
// 001b53d0), which reaches Activate again once +2d is set. Present: join the
// think list, +166 = 0, clamp +e8, the Activated notification (0016f1c0,
// 004ff268 with the local player's actor, transport 0) unless +5c, the path
// follower start 00160f60, then the timers and +8c; with +141 bit 5 (set by
// the constructor, so on the first Activate) the bit is cleared and the held
// item set up: +b0 = +1a4 (0014bcb0: -1 for inventory mode 6), +1b0
// (0014b9e0: 00143a60 twice, +a4/+a8), +b4 = +1a4, +1ac(+b0) (0014bae0:
// 00143c50, +a4/+a8); for the native NPC's empty inventory (mode 6, all six
// slots -1: 00143a60 / 00143c50 change nothing, interpreter) that leaves
// +b0 = +b4 = -1, +a4 = +a8 = 0; finally +159 = 0. The look slot (004ea1d0,
// +1b8, 00158980) is taken after joining the think list (npc_take_look_slot);
// the perception reset 0016dfa0 runs when the perception is hosted. An armed NPC (inventory +84 != 6, 001b16c0)
// needs the inventory and the game RNG: the burst length +a4 = 3 + (id & 7)
// + (r & 1), reduced to max(1, clip size / 2) when the current weapon's clip
// size (001438e0(inv, 6)) is smaller; +8c = 0 for a full-auto weapon
// (00143820); the first-activation branch then runs 00143a60(inv, 1) (twice
// when the first leaves nothing held), the burst draw again, and
// 0014bae0(+b0): 001439b0 for an item, else 00143c50, and the burst draw a
// third time. A pending MoveToNode (+138) is replayed after the path start:
// virtual +100 (0014be50: mode 7 at priority 100, +138, +f4 = -1), then
// +138 = -1 (npc_script_control.hpp). Component virtual +44 (00157a40) gates
// both branches: not present, +5c(1) runs only when it holds; present, the
// component joins the think list when it holds and otherwise the dead list
// 004ea370 (0014ab90), and the rest of Activate runs either way.
struct NpcAiActivationHost {
    std::function<void()> activate_actor;                     // actor virtual +5c(1)
    // 00157a40 for a dying or dead actor (required only then), and 0014ab90(004ea370, &component).
    std::function<bool()> alive_44;
    std::function<void()> append_to_dead_list;
    // The perception reset 0016dfa0 at its place in Activate (null: recorded as skipped).
    NpcPerception* perception{};
    std::uint32_t actor_id{};                                 // actor +c
    WeaponInventoryOps* inventory{};                          // actor +3524 (required when armed)
    std::function<std::uint32_t()> random;                    // 00127dd0 (required when armed)
};
std::vector<std::string> activate_npc_ai(NpcAiComponent& c,NpcAiSystem& system,NpcAiHost& host,const NpcAiActivationHost& activation);
// 00158bc0's +141: bits 2, 4 and 5 set, 6 (with +64 = 0) and 7 cleared, bits
// 0, 1 and 3 kept from the block's previous content (interpreter goldens).
std::uint8_t npc_ai_constructor_flags_141(std::uint8_t previous);
// Deactivate (component virtual +4c, 00157ad0) in single player. Inactive
// (+159): nothing. Actor present: actor virtual +60(1) (001b5190, whose
// 003928d0 -> 003ce790 clears +2d and calls +4c again). Otherwise: 00163c50
// frees the actor's queued requests of the route manager 004ed9b0
// (npc_route_release, with NpcAiHost::routes), 00160e60 (the route
// buffer +74: +14 / +18 = the route's first / last node, the node array
// released; release_npc_route_buffer), the Deactivated
// notification (0016f0e0: key 004ff270, vtable 004dcd10, +20 = 2, to the
// actor with transport 0; its +24 is not written) unless +5d, the dead list
// 004ea370 (an alive NPC is not on it), the think list 004ea190 (0015ad70
// removes every entry of this actor; 004ea358 is left alone), the look-at
// slot (+1b8 freed: npc_release_look_slot), the replica branch (actor +7c = byte +3598,
// clear in single player), then +159 = 1. A dying or dead NPC throws (its
// 003ce850 call and the dead list are not hosted). Returns the stages not
// modelled.
struct NpcAiDeactivationHost {
    std::function<void()> deactivate_actor;                   // actor virtual +60(1)
    // 132ln5h13: 00157c3c..00157ca8, after the look slot: for a generator's actor (virtual +7c, byte +3598) whose
    // 0040c540(&+150) is a live object (+4 == 0xbebaafde), the message 0x004a4a68 (vtable 0x004db600, +24 the
    // actor, +28 = 1) to it with transport 0 (003c9420). Unset: no such object (single player, +150 = 0).
    std::function<void()> generator_release;
};
std::vector<std::string> deactivate_npc_ai(NpcAiComponent& c,NpcAiSystem& system,NpcAiHost& host,const NpcAiDeactivationHost& deactivation);
// DeactivateAnon (component virtual +c0, 00157510) in single player: the
// per-player activation record (0015b220(c, -1, 0) in the list 004ea1b0,
// read in single player only by 0015b060, which nothing hosted reaches),
// actor virtual +60(1) when the actor is present (+159 is not consulted),
// and 0042d910 (the NetObjectMgr map 00582700, empty in single player). The
// peer loop over 0055c780 sends nothing in single player. Returns the
// stages not modelled.
std::vector<std::string> deactivate_npc_ai_anonymous(NpcAiHost& host,const NpcAiDeactivationHost& deactivation);
// 0015a440 (component virtual +3c) in single player: with +141 bit 7 and on
// frames where (id & 0xffff) + frame is a multiple of 8, activate an absent
// NPC within the radius (+98, or the far distance when below 1e-5) and
// deactivate a present one beyond r^2 + 625, with the latches +1d5/+1d4.
// The distance is 003ce850: the closest player's squared eye distance (the
// virtual +c0 positions), which the host supplies (nullopt: no player). The
// replica check (actor +7c, byte +3598) and 004340c0 are taken as clear
// (single player; inferred).
struct NpcAiRadiusHost {
    std::function<std::optional<float>()> closest_player_distance_squared;
    std::function<void()> activate_actor;                     // actor +5c(1)
    std::function<void()> deactivate_actor;                   // actor +60(1)
    std::uint32_t actor_id{};
};
void check_npc_activation_radius(NpcAiComponent& c,const NpcAiSystem& system,NpcAiHost& host,const NpcAiRadiusHost& radius);
// ActivateByRadius (component +1cc, 0014ad30) / Off (+1d0, 0014ae30).
void activate_npc_by_radius(NpcAiComponent& c,float radius);
void deactivate_npc_by_radius(NpcAiComponent& c);
// Script natives on the component (registrations 004c3cfc..004c56a0; each
// wrapper reads its arguments from the script's argument array):
// SetTeam(AI, int) 0016abe0 -> virtual +a0 = 0016f490: +54 = team (the
// multiplayer copy into +3584 +30 is skipped in solo).
void set_npc_team(NpcAiComponent& c,std::int32_t team);
// Team(AI) 0016abb0 -> virtual +68 = 0015b800: +54.
std::int32_t npc_team(const NpcAiComponent& c);
// SetSkill(NPC, float) 00149ae0 -> 0022f2d0: +38 = skill (its message 0x37
// is sent in multiplayer only).
void set_npc_skill(NpcAiComponent& c,float skill);
// SetFiringThreshold(NPC, float) 00149b10 -> virtual +16c = 0014bce0:
// returns the previous +78 and stores the new value.
float set_npc_firing_threshold(NpcAiComponent& c,float threshold);
// SetMaxActiveNpcCount(NPC, int) 00149450 -> 0014b340: 004ea350 = n.
void set_npc_max_active_count(NpcAiSystem& system,std::int32_t count);
// The eye position 00188d80 (actor virtual +c0) with its argument -1 reads
// skeleton node 5 (byte 0048a2a3 = 2, then byte 0049d022 = 5) through
// 00132910 on the model context (+2e50) when the actor is present (+2d,
// +2c, !+2e); otherwise the translation +40 of its model transform
// component +50 (refreshed through its +58 when byte +50 is set).
inline constexpr int npc_eye_node=5;
// 003ce850(actor, &d2) in single player: over the four player slots
// (003fb8d0; nullopt = empty slot) the squared eye distance d = eye -
// player eye (vsub.xyzw), (dx*dx + dy*dy) + dz*dz in binary32 (vmul.xyz,
// then vmulax.x / vmadday.x / vmaddz.x with vf0.w = 1), kept when strictly
// below the running best, which starts at 1e8 (0x4cbebc20). When no player
// is closer than that, the original falls back on 0040bf10's cached player,
// which is not hosted: that throws.
float npc_closest_player_eye_distance_squared(const RootVector& eye,std::span<const std::optional<RootVector>> players);
}
