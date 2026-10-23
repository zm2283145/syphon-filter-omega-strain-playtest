#pragma once
// The burning actor (research/COMBAT.md "Checkpoint 120c burning actor"):
//
// - the actor effect object of 00185d40 (00236810, vtable 004dbd80): one
//   particle emitter state per descriptor record, updated by its virtual +48
//   00235f60 in the object traversal 00133420 (the record position 00236450);
// - the fire zone 002624a0 (vtable 004dc970) of the fire slot 0 (+34e0): its
//   strength setter 00262470 and its update 00261df0 (virtual +48), the fire
//   damage and heat of the objects around the burning actor;
// - the actor update's slot loop 001b1e10..001b2148, its post-loop
//   001b215c..001b21b4 and the +31f4 timer 001b21b8..001b2214;
// - the +33b0 decay 001b2380..001b23c4.
//
// The particle emitters (0036b180 + 003a35c0 + 003a2dd0, 003a2400, 003a2680,
// 003a2ba0 + 0036b130) are presentation and go through the host.
#include "animation_root.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <span>
#include <string_view>
#include <vector>

namespace omega {

// A descriptor record (stride 0x1c).
struct FireEmitterRecord {
    std::string_view name;          // +0 (the particle name, looked up by 003a35c0 in 0053b540)
    std::uint8_t node_a{};          // +4 (lbu): index into the node table 0049d020
    std::uint32_t node_b{};         // +8 (lw): 0x15 = none, else its low byte indexes 0049d020
    float weight{};                 // +c
    float minimum{};                // +10 (the emitter +5c floor)
    float rate{};                   // +14
    std::uint8_t strength{};        // +18: the strength is intensity * weight (else weight)
    std::uint8_t velocity{};        // +19: the velocity is the previous position - the position
};

// 0049d020 (lb): the skeleton node of each record node index.
inline constexpr std::array<std::int8_t,22> fire_node_table{0,2,5,9,14,8,13,17,19,10,15,18,20,1,4,7,12,3,6,11,16,0};
inline constexpr std::uint32_t fire_record_single_node=0x15;

// The descriptors of the effect slots (00185d40's table 0048a2d8).
std::span<const FireEmitterRecord> fire_effect_descriptor(std::uint32_t address);   // 0049d600 / 0049d730 / 0049d760
// The records' names: 0049d5e8 "FireHuman", 0049d718 "FireHumanSmoke", 0049d750 "Tazer_Shock1".

struct FireEmitterState {
    std::uint32_t emitter_0{};      // the particle emitter (0: none)
    float intensity_4{};
    RootVector previous_10{};       // the record position of the previous update
};
// 00236810(object, actor, descriptor, count, f12): +74 actor, +78 descriptor,
// +7c the states (each +0 = 0, +4 = f12), +84 count, +80 = 0.01.
struct FireEffectObject {
    std::uint64_t actor_74{};
    std::uint32_t descriptor_78{};
    std::vector<FireEmitterState> states_7c;
    float intensity_80{};
};
inline constexpr float fire_effect_initial_80=0.01F;      // 0x3c23d70a
FireEffectObject make_fire_effect_object(std::uint64_t actor,std::uint32_t descriptor,std::uint32_t count,float f12);

using FireMatrix=std::array<RootVector,4>;
struct FireEffectHost {
    // 00132910(out, 00132a30(+74), node): the actor's skeleton node world position.
    std::function<RootVector(std::int8_t node)> node_position;
    // +74's +3204 bit 5 or its +346d.
    std::function<bool()> fast_decay;
    std::function<std::uint8_t()> life_108;                 // +74 virtual +108
    // 0036b180(0, 0x70, ...) and, non-null, 003a2dd0(alloc, 003a35c0(0053b540,
    // name), &matrix, f12 = weight): the emitter (0 for a null allocation).
    std::function<std::uint32_t(std::string_view name,const FireMatrix& matrix,float weight)> create_emitter;
    std::function<void(std::uint32_t emitter)> destroy_emitter;          // 003a2ba0(e, -1) + 0036b130(0, e, ...)
    std::function<void(std::uint32_t emitter)> fade_emitter;             // e +6a = 1
    // 003a2400(e, dt, &matrix, &velocity); the velocity's w is the stack's
    // without +19 (only x, y, z are written): 0 here.
    std::function<void(std::uint32_t emitter,float dt,const FireMatrix& matrix,const RootVector& velocity)> move_emitter;
    std::function<void(std::uint32_t emitter,float strength)> emitter_strength;   // 003a2680(e, f12)
    std::function<void(std::uint32_t emitter,float floor)> emitter_floor;         // e +5c
};
// 00236450(object, record, out): rows 0..2 the identity, row 3 the node
// position (or the mean of the two nodes, VU add and * 0.5).
FireMatrix fire_effect_record_matrix(const FireEffectObject& object,std::size_t record,const FireEffectHost& host);
// 00235f60(object, dt).
void update_fire_effect_object(FireEffectObject& object,float dt,const FireEffectHost& host);

// 002624a0(zone, owner, f12, f13) after the base constructor 003cf0e0:
// +90 owner, +94 owner +c (-1 for null), +80 = +84 = +88 = f12, +60 =
// cvt.w(cvt.s(*0048b0e0) * f13 / 100), then 0040c9d0(zone) and the virtual
// +5c(1) (+2d = 1). +70 is not written.
struct FireZone {
    std::uint64_t owner_90{};
    std::uint32_t owner_key_94{};
    std::int32_t strength_60{};
    RootVector center_70{};
    RootVector extent_80{};          // +80 / +84 / +88 (+8c is not read)
    std::uint8_t byte_2c{1},byte_2d{},byte_2e{};
    std::uint32_t key_0c{};          // the FIR: record's key (002625b0 only)
};
inline constexpr std::int32_t fire_zone_scale=1000;         // *0048b0e0
inline constexpr float fire_zone_margin=7.0F;               // *0048b0e8
std::int32_t fire_zone_strength(float f12);                 // 00262470's +60
FireZone make_fire_zone(std::uint64_t owner,std::uint32_t owner_key,float f12,float f13);
inline void set_fire_zone_strength(FireZone& zone,float f12){zone.strength_60=fire_zone_strength(f12);}   // 00262470
// 002625b0(zone, stream), the FIR: record reader (a FireObj, vtable 004dc970)
// after the base constructor 003cf0e0: +0c the key, +90 = 0 and +94 = -1 (no
// owner), +70 = (second + first) * 0.5 and +80 = (second - first) * 0.5 (the w
// lanes are uninitialised stack), +60 = 00262470's strength of the value. It
// then runs the links (link_level_fire_zone) and the virtual +90, 003cdff0(zone,
// 0055c9f0, 0), which allocates a 0x60-byte transform (vtable 004df6a0) holding
// the identity 0055c9f0 at +10, its +50 byte set, at zone +50 (+48 = 1);
// 003fb650 runs only when networked (*005721c8). The base constructor leaves
// +2c = 1 and +2d = 0 (the zone is enabled but not yet activated) and the
// class tag +49 = 0x12.
FireZone make_level_fire_zone(std::uint32_t key,const RootVector& first,const RootVector& second,float value);
inline constexpr std::uint8_t fire_zone_class_tag=0x12;     // 003cf0e0's third argument (002625c4)
// A FIR: zone's virtual +e4 (003cd260): +50 is the transform and +88 is
// 0013ee60 (0), so its +58 (00132900, clears +50) runs and its +40 is
// answered, row 3 of the identity.
inline constexpr RootVector level_fire_zone_position_e4{0.0F,0.0F,0.0F,1.0F};
// Enable / Disable(FireObj) in solo: 00261d60 / 00261d30 -> virtual +64 / +68
// (00261c00 / 00261c90; their messages are built and sent only when
// networked, or offered to 003ff650, which returns in solo): +2c = 1 / 0.
void set_fire_zone_enabled(FireZone& zone,bool enabled);
// The zone tracker on a FIR: zone (class tag 0x12: 002244d0 answers no model):
// 00224330 -> +2e = 0, the virtual +5c(0) (003ce980, +58 null: +2d = 1), +2d = 1;
// 00224180, forced or not -> +60(0) (003ce790) when +4 holds the magic 0xbebaafde, then +2d = 0.
void set_fire_zone_present(FireZone& zone,bool present);
// 002625b0's link loop: each pair with neither word -1 is resolved by
// 0040c480(a, b); a found object is associated by 00173d60(found, zone, 1, 0).
void link_level_fire_zone(std::span<const std::array<std::uint32_t,2>> links,
                          const std::function<std::uint64_t(std::uint32_t,std::uint32_t)>& find,
                          const std::function<void(std::uint64_t)>& associate);

// An object of the list 004f78dc as 00261df0 reads it.
struct FireZoneTarget {
    bool active{};                   // +2d && +2c && !+2e
    bool byte_14{};
    bool human_4c{};                 // +4c == *0049d010
    std::uint8_t life_108{};         // virtual +108 (read for humans only)
    bool byte_33a5{};
    RootVector position_e4{};        // virtual +e4
    bool controller_58{};            // +58 non-null
    bool controller_player_54{};     // +58 virtual +54
};
struct FireZoneHost {
    // The owner weak handle (+90 / +94) when valid: active (+2d && +2c && !+2e) and its virtual +e4.
    std::function<std::optional<std::pair<bool,RootVector>>()> owner;
    // 004f78dc's entries (object, key) in list order, and each valid one's fields.
    std::function<std::vector<std::pair<std::uint64_t,std::uint32_t>>()> entries;
    std::function<bool(std::uint64_t object,std::uint32_t key)> valid;
    std::function<FireZoneTarget(std::uint64_t object)> target;
    // virtual +100(object, zone, 3, 4, 0, 0, 0, amount).
    std::function<void(std::uint64_t object,float amount)> damage;
    // +58 virtual +80(controller, zone, value).
    std::function<void(std::uint64_t object,float value)> heat;
};
// 00261df0(zone, dt). The owner is compared with the entry objects.
void update_fire_zone(FireZone& zone,float dt,const FireZoneHost& host);

// The actor update 001b1e10..001b2214 (slots k 0..2, the post-loop, the timer).
struct BurningActorHost {
    std::uint64_t actor{};
    // Slot k's object +80 while the weak handle +354c + 8k / +3550 + 8k is valid.
    std::function<std::optional<float>(std::uint32_t k)> slot_intensity;
    std::function<void()> extinguish;                                    // 00186360(actor, 0, 0, 0, 0, -1.0)
    std::function<void(std::uint32_t k)> effect_off;                     // 00185d40(actor, k, 0, 1.0)
    std::function<bool()> light_34e0;                                    // +34e0 non-null
    std::function<void(float f12)> light_strength;                       // 00262470(+34e0, f12)
    std::function<bool()> byte_14;
    std::function<bool()> burning_326a;
    std::function<std::uint8_t()> life_108;                              // virtual +108
    std::function<std::uint64_t()> fire_setter_3480;                     // +3480 when its handle is valid, else 0
    // virtual +100(actor, source, category, type, 0, 0, 0, amount).
    std::function<void(std::uint64_t source,std::uint8_t category,std::uint8_t type,float amount)> damage;
    std::function<bool()> controller_player;                             // +58 non-null and its virtual +54
    std::function<void(float value)> controller_80;                      // +58 virtual +80(controller, actor, value)
    std::function<void()> glow_up;                                       // halfword +2df6 += 1
    std::function<void()> glow_down;                                     // 00383680(actor + 0x60): +2df6 -= 2, at least 0
    std::function<float()> health_40;
};
inline constexpr float burning_light_scale=10.0F;           // *0048a1d8
inline constexpr float burning_damage_rate=40.0F;           // *0048a1f0
// 001b1e10..001b2214; timer_31f4 is +31f4.
void update_burning_actor(float& timer_31f4,float dt,const BurningActorHost& host);
// 001b20f8..001b2124: (float)((double)(f21 / 2) + 0.5) (0011bd70, __adddf3 0011c1a8, 0011c928;
// the soft-double add and narrowing truncate toward zero).
float burning_controller_value(float intensity);

// 001b2380: +33b0 > 0: cvt.w(cvt.s(+33b0) - dt * *0048a1e8), then 0 when negative.
inline constexpr float fire_damage_decay_rate=15.0F;        // *0048a1e8
std::int32_t decay_fire_damage(std::int32_t fire_damage_33b0,float dt);
// 00383680 on the halfword +2d96 of the model (+2df6 of the actor).
inline std::int16_t model_glow_down(std::int16_t v){v=static_cast<std::int16_t>(v-2);return v<0?std::int16_t{0}:v;}

}  // namespace omega
