#pragma once
// The AI perception's think-frame pipeline (NPC_COMBAT.md section 2, milestone
// N4a): the per-think update 0016b580 with the record update 0016cf10, target
// selection, the visibility test 0016bbb0 (sight test 0016c260, target-manager
// line, AI event 4, flashlight lit spot), the sighting register 0016c770, the
// last known position 0016c030, the target setter 0016ddb0, the candidate
// round robin 0016e1a0, the sensed-record queries 0016c6a0 / 0016c730 and the
// reset 0016dfa0. Interpreter goldens: tests/npc_sight_goldens.hpp (0016c260),
// tests/npc_record_goldens.hpp (0016cf10), tests/npc_think_goldens.hpp
// (0016b580 sequences). Single player only (005721c8 clear); the multiplayer
// player loops of 0016e1a0 are not modelled.
//
// The perception object is NpcPerception (npc_perception.hpp) and its flag
// byte +20 is NpcAiComponent::perception_flags_20 (the perception's +3c is
// the component): bit 0 target in view (head), 1 in view (body eye), 2
// target-manager line clear, 3 ever seen, 4 last known position valid, 6 the
// last reported "visible and clear", 7 the target / candidate alternation.
//
// Objects are named by their pointer (std::uint64_t; 0 is null) and held as
// weak handles {pointer, ID}: a handle resolves when the host still has an
// object at the pointer whose +c equals the ID.
#include "npc_ai.hpp"
#include "npc_perception.hpp"
#include "team_relations.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace omega {

// What the perception reads from an object (an actor, a player, a body, a
// node object named by SetTarget, the flashlight's lit spot).
struct NpcSenseObject {
    std::int32_t id{};                          // +c
    std::uint8_t life_108{};                    // virtual +108
    std::array<std::uint8_t,3> root{};          // +2c, +2d, +2e (present: +2d, +2c, !+2e)
    bool actor_class{};                         // +4c is the actor class word *0049d010
    bool class_4ea770{};                        // +4c is the class word *004ea770 (N5b: posture, look-at)
    std::uint8_t flag_3468{};
    std::uint8_t byte_14{};                     // +14 (0: a remote player, read with +3034 bit 0)
    std::uint32_t flags_3034{},flags_3204{};
    std::int32_t stealth_44{};                  // +44 (FLASHLIGHT.md section 6.1)
    RootVector position{};                      // virtual +e8 row 3
    RootVector eye{};                           // virtual +c0(-1)
    bool weapon_ready_d4{};                     // virtual +d4
    bool kind_5b0{},kind_580{};                 // virtual +f8 with the descriptors 004ee5b0 / 004ee580
    // The +58 controller: none, a player controller (virtual +54 true) or
    // an AI component (virtual +58 true; cNPC, vtable 004d9380).
    enum class Controller : std::uint8_t {none,player,ai} controller{};
    std::int32_t team{-1};                      // 003cc950: the controller's virtual +68, -1 without one
    std::uint8_t contact_158{};                 // AI: component +158
    std::int32_t region_5c{-1};                 // AI: path follower +5c
    std::uint64_t aim_target{};                 // player: 003ea550, its +28/+2c handle resolved (0: none)
    std::uint64_t ai_target{};                  // AI: its perception's target +40/+44 resolved (0: none)
    std::uint64_t lit_spot{};                   // 00199700 (actor class only; 0: no lit spot)
    RootVector lit_spot_position{};             // the spot's virtual +e4
};
bool npc_sense_present(const NpcSenseObject& o);       // +2d && +2c && !+2e
// A node object (0015c390: base 003cf0e0(node, &-1, 10, 004ea770), vtable
// 004d95b0; +c its record ID, +80 its position) as the perception reads it
// (SetTarget(NPC, Node), 0014a210): life +108 3 (00141850: +40 is 0.0),
// +2c/+2d/+2e 1/0/0 (nothing activates a node: the activation map lists no
// NOD member), +14 1, +44 0, the class word *004ea770, +c0(-1) and +e8 row 3
// both (x, y, z, 1) (0015bef0 / 0015c170), +d4 1 (0015c050), +f8 0
// (0013e060), no controller (+58 0: team -1 from 003cc950).
NpcSenseObject npc_node_sense_object(std::int32_t id,const std::array<float,3>& position);

// 0022cb10(004f77c0, viewer, target, 0, 0) and 0022c650(004f77c0, object):
// the target manager's visibility cache and registration (COMBAT.md,
// "Visibility cache"), hosted by the combat world. blocker returns the
// blocking object, 0 when the line is clear.
class NpcVisibilityQuery {
public:
    virtual ~NpcVisibilityQuery()=default;
    virtual std::uint64_t blocker(std::uint64_t viewer,std::uint64_t target)=0;
    virtual void register_target(std::uint64_t object)=0;
};

struct NpcPerceptionHost {
    std::uint64_t self{};                       // the NPC's actor (component +30)
    // The observing actor: +2c/+2d/+2e, virtual +c0(-1), skeleton node 14
    // (*0049d024) world matrix (read only while present), virtual +e8 row 1,
    // +33a8 (blinded while > 0), +3470 (1: stealth ignored).
    std::array<std::uint8_t,3> self_root{1,1,0};
    RootVector self_eye{};
    std::array<RootVector,4> self_head{};
    RootVector self_forward{};
    float blind_33a8{};
    std::uint8_t flag_3470{};
    // The object at a pointer (nullopt: none). Called again for every read,
    // so a host may build the view lazily; views must not change during a
    // call of these functions.
    std::function<std::optional<NpcSenseObject>(std::uint64_t object)> object;
    const TeamRelations* teams{};               // the matrix *004ee620
    // The think list 004ea190 (weak handles {actor, ID}, in list order).
    std::vector<std::pair<std::uint64_t,std::int32_t>> think_list;
    std::uint64_t local_player{};               // *004ffc0c +30
    std::array<std::uint64_t,4> player_slots{}; // 003fb8d0(0..3), 0 for an empty slot
    NpcVisibilityQuery* visibility{};
    // 004ed9c0[team]: a player target's region (by 003cc950).
    std::function<std::int32_t(std::int32_t team)> player_region;
    // 0015d790(perception, previous): the region at the perception's +00,
    // which 0016c030 reads before it stores the new position, with the f20
    // 0016c030's caller left (npc_region_at; nullopt: not hosted) and the
    // call site.
    std::function<std::int32_t(const RootVector& last_known,std::int32_t previous,std::optional<float> f20,const char* site)> region_at;
    // Port-only (not in the original; port_cheats.hpp, the "NPCs Ignore You"
    // cheat): an object this NPC must not perceive. Empty: none (the original).
    std::function<bool(std::uint64_t object)> port_ignored;
};
// Port-only: whether h.port_ignored marks the object (0 is never marked).
bool npc_perception_ignores(const NpcPerceptionHost& h,std::uint64_t object);

// What the perception sends: AI events (key 004ee550 to the component,
// transport 0: event 7 with +28 the object ID and +2c the record's cause,
// event 4 with +28 the new "visible and clear" state), NewAwareness (key
// 004ff298, vtable 004dcc90, +20 2, +24 the object, +28 the cause byte, to
// the actor, transport 0) and the dead-object notice (key 004ff2a8 to the
// component, transport 0; 0016c9d0). The dead-object notice is the script
// message AIPickupableAwarenessMsg (vtable 004dcbd0; its +14 getter 0026ee40
// reads global 004ff210 = registered type 126, boot snapshot; base AIGOBJMsg,
// so Who 0026efe0 answers +24).
inline constexpr std::uint32_t npc_script_new_awareness=0x004ff298;
inline constexpr std::uint32_t npc_script_pickupable_awareness=0x004ff2a8;
struct NpcPerceptionSend {
    enum class Kind : std::uint8_t {ai_event,new_awareness,dead_notice} kind{};
    std::uint8_t event{};
    std::int32_t value{};                       // AI event +28
    std::uint8_t cause{};                       // event 7 +2c / NewAwareness +28
    std::uint64_t object{};                     // NewAwareness / dead notice +24
    bool operator==(const NpcPerceptionSend&) const=default;
};

// 0016e0a0's state (constructor): +40 0, +44 -1, +48 0, +4c -2, +1c 0, +18 -1,
// flags +20 bits 0-6 and +21 bit 0 clear, no records; the view cone is set
// by the reset. 0016dfa0 (reset, Activate): +14 = pi/4 (0x3f490fdb), +10 =
// the far distance 0053834c, +50 = the frame counter 005383c8 + 20. No
// reader of +50 was found in 00148000..00172000 (static scan).
void npc_perception_reset(NpcPerception& p,float far_distance,std::uint32_t frame);
// 0016c9d0 through a host view (npc_perception.hpp).
void npc_perception_add(NpcPerception& p,const NpcPerceptionHost& h,std::uint64_t object,std::uint8_t cause,float value,
    std::vector<NpcPerceptionSend>& out);
// 0016c260(p, point, head, stealth): 0 while blinded (+33a8 > 0, or not <=
// 0); eye = node 14 translation of a present actor with head, else +c0;
// v = point - eye, d = sqrt((x x + y y) + z z) (VU); r = radius (100 -
// stealth) / 100 for stealth > 0; not seen unless d < r and d != 0; seen when
// d <= 1; else forward = node 14 row 1 (head, present) or +e8 row 1, and
// seen iff dot(forward, v / d) + (1 - cosf(half-angle)) > 1. No line test.
bool npc_sight_test(const NpcPerception& p,const NpcPerceptionHost& h,const RootVector& point,bool head,std::int32_t stealth);
// 0016c030(p, object, position, seen): see the source for the region.
// `f20`: the caller's f20 (0016c030 and 0016ddb0 do not write it), passed to
// the region lookup at 0016c15c; nullopt where the caller's is not hosted.
void npc_perception_set_last_known(NpcPerception& p,NpcAiComponent& c,const NpcPerceptionHost& h,std::uint64_t object,
    const RootVector& position,bool seen,std::optional<float> f20=std::nullopt);
// 0016ddb0(p, object, priority).
void npc_perception_set_target(NpcPerception& p,NpcAiComponent& c,const NpcPerceptionHost& h,std::uint64_t object,std::int32_t priority,
    std::optional<float> f20=std::nullopt);
// 0016cf10(p, dt): ages, scores and notices every record.
void npc_perception_update_records(NpcPerception& p,NpcAiComponent& c,const NpcPerceptionHost& h,float dt,
    std::vector<NpcPerceptionSend>& out);
// 0016c770(p, object, clear, view).
void npc_perception_register_sighting(NpcPerception& p,const NpcPerceptionHost& h,std::uint64_t object,bool clear,bool view,
    std::vector<NpcPerceptionSend>& out);
// 0016e1a0(p, previous): the next visibility candidate (the object itself
// when none).
std::uint64_t npc_perception_next_candidate(const NpcPerception& p,const NpcAiComponent& c,const NpcPerceptionHost& h,std::uint64_t previous);
// 0016bbb0(p, object).
void npc_perception_visibility_test(NpcPerception& p,NpcAiComponent& c,const NpcPerceptionHost& h,std::uint64_t object,
    std::vector<NpcPerceptionSend>& out);
// 0016b580(p, dt), dt = the component's +1bc.
void npc_perception_think(NpcPerception& p,NpcAiComponent& c,const NpcPerceptionHost& h,float dt,std::vector<NpcPerceptionSend>& out);
// 0016c6a0(p, object): the object's record when its age +24 >= 0 (not <
// 0), else none; 0016c730: that record's threat +30, 0 without one.
const AwarenessRecord* npc_perception_sensed_record(const NpcPerception& p,const NpcPerceptionHost& h,std::uint64_t object);
float npc_perception_threat(const NpcPerception& p,const NpcPerceptionHost& h,std::uint64_t object);
// The weak-handle resolution (00150c60 and the inline copies): the object
// when it exists and its +c equals the ID, else 0.
std::uint64_t npc_resolve_handle(const NpcPerceptionHost& h,std::uint64_t object,std::int32_t id);
// The component's IsEnemy (virtual +70 = 0022f340): team +54 >= 0, the
// object non-null with team (003cc950) >= 0, both below 16, bit 0 of the
// matrix [own team][its team].
bool npc_is_enemy(const NpcAiComponent& c,const NpcPerceptionHost& h,std::uint64_t object);
// Checkpoint 132sx8, the script natives on the perception (+1ac):
// 0016c8d0(p, object) (RemoveAwareness, component +e8 = 0014a400): a null
// object finds nothing (0016db40); otherwise the first record whose handle
// resolves to the object is freed (0036b130) and its list node erased
// (0015af40 / 00138da0). The target +40/+44 is untouched.
void npc_perception_remove(NpcPerception& p,const NpcPerceptionHost& h,std::uint64_t object);
// 0016c590(p, object) (ResendNewAwarenessMessage, +f4 = 0014a380): every
// record with object 0, or whose handle resolves to the object, gets +3c = 0
// (the NewAwareness latch of 0016cf10), so the next record update sends it
// again.
void npc_perception_resend(NpcPerception& p,const NpcPerceptionHost& h,std::uint64_t object);
// 0016c1e0(p, object, 1) (InViewCone, +188 = 0014afc0): the object's eye
// virtual +c0(-1), the stealth 0 when the observer's +3470 is 1 else the
// object's +44, then 0016c260(p, eye, 1, stealth). No line test. A null
// object throws (the original reads its vtable).
bool npc_perception_in_view_cone(const NpcPerception& p,const NpcPerceptionHost& h,std::uint64_t object);
// SetViewConeAngle 001492f0: +14 = bits 0x3c8efa35 (pi / 180) * (0.5 * angle).
// SetViewConeRadius 00149360 stores the float in +10 as is.
float npc_view_cone_half_angle(float angle);
}
