#pragma once
// Explosions (COMBAT_ITEMS.md C7a). 0021a200 visits the object registry
// (*00571748: count +8, array +c) and schedules one delayed damage event per
// object in range (0x40 bytes, GameGOBJ.cc, vtable 004daeb0) into the
// callback list 0055a2e0 (003ea1d0), the same list the script pump 003e9e30
// advances (ScriptScheduler). The event's +10 virtual 0021b890 calls the
// victim's damage virtual +100 with category 1. game::CreateBlast 0021a170 is
// the script entry. Single player only (005721c8 clear): the network message
// of 0021a200 is not reproduced. The request 002727b0 is returned as data, not
// run: it is the tremor add (camera shake / pad rumble; AddTremor 0012f910 uses
// it too), not a light (checkpoint 120h). There is no impulse: the strength only selects the type.
#include "actor_damage.hpp"
#include "animation_root.hpp"
#include "script_scheduler.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace omega {

// A weak object reference {object, its +c id} (00176ef0: a null object stores
// id -1). It resolves while the object exists and its +c still equals the id.
struct ObjectWeakHandle {
    std::uint64_t object{};
    std::int32_t id{-1};
    bool operator==(const ObjectWeakHandle&) const=default;
};

// One registry entry in array order, as 0021a200 reads it.
struct BlastObject {
    std::uint64_t object{};                      // 0: a null entry (skipped)
    std::int32_t id_c{};
    bool human_4c{};                             // +4c is the actor class word *0049d010
    bool byte_2c{},byte_2d{},byte_2e{};          // active: +2d and +2c and not +2e
    bool byte_14{};                              // owned by this machine
    std::optional<bool> controller_player_54;    // +58 controller's virtual +54 (nullopt: no controller)
    float health_40{};
    RootVector position_e4{};                    // virtual +e4
};
// The blast source (a0): the thrower or the scripted object.
struct BlastSource {
    ObjectWeakHandle handle;
    bool byte_14{};
    RootVector position_e4{};                    // read only without a point
};
struct BlastRequest {
    std::optional<BlastSource> source;           // a0 (nullopt: null)
    std::int32_t damage{};                       // a1
    bool kind{};                                 // a2: type 2 when set, else 13
    std::optional<RootVector> point;             // a3 (nullopt: the source's +e4, or (0, 0, 0, 1))
    bool skip_humans{};                          // t0
    float strength{},inner{},outer{};            // f12, f13, f14
};
// 002727b0(*004ffbdc, &P, 1.0, 1.0, inner * 1.0, outer * 2.0) (0048a9a0, 0048a9a8):
// the tremor record {+0 duration, +4 elapsed 0, +8 inner, +c outer, +10 magnitude, +20 P}.
struct BlastFlash {
    RootVector point{};
    float inner{},outer{};
    bool operator==(const BlastFlash&) const=default;
};
// The delayed damage event (0021a820 + 0021a6a8..0021a6fc).
struct ExplosionEvent {
    std::int32_t delay_ms_0{};                   // trunc(1000 * (d / 150)) (0048a9b0)
    bool game_clock_4{true};                     // counts down with the game clock (0 while paused)
    bool flush_5{};                              // not run by the level flush 003e9c20
    ObjectWeakHandle victim_c;
    std::int32_t damage_14{};
    ObjectWeakHandle source_18;
    std::int8_t type_20{};                       // 0, 2 or 13
    RootVector point_30{};
    bool operator==(const ExplosionEvent&) const=default;
};
struct BlastResult {
    BlastFlash flash;
    std::vector<ExplosionEvent> events;          // in registry order (= scheduling order)
};
// 0021a200 without the scheduling. A null registry entry is skipped unless
// skip_humans is set (the original then reads +4c of the null pointer): that
// throws, as do values outside the integer range at the conversions.
BlastResult prepare_blast(const BlastRequest& request,std::span<const BlastObject> registry);

// 0021b890: the victim's +100(victim, source or null, 1, type, null, 0, &P, float(damage)).
struct ExplosionDamageCall {
    std::uint64_t victim{};
    std::uint64_t source{};                      // 0 when the source handle no longer resolves
    std::uint8_t category{1};
    std::int8_t type{};
    RootVector point{};
    float amount{};
    bool operator==(const ExplosionDamageCall&) const=default;
};
// `valid`: the handle's object exists and its +c equals the stored id. Nothing
// is returned for a victim that no longer resolves.
std::optional<ExplosionDamageCall> explosion_damage_call(const ExplosionEvent& event,
    const std::function<bool(const ObjectWeakHandle&)>& valid);

// The victim side for a human actor: 00187360 with category 1, no
// penetration, not quiet, the blast point as the hit point (type 2: the LOS
// request starts there; a null source rejects the damage).
ActorDamageInput blast_actor_damage_input(const ExplosionDamageCall& call,std::uint64_t actor,
    std::optional<ActorDamageController> controller,bool byte_14,std::optional<ActorDamageController> source_controller,
    std::optional<ActorDamageParty> fire_setter_3480={});

// The world side: the registry, handle checks and every object's damage
// virtual +100. Actors map to apply_actor_damage (blast_actor_damage_input);
// other classes run the base damage 003ccb00 unless they override +100
// (open per class), so a host must throw for a class it does not know.
class BlastObjects {
public:
    virtual ~BlastObjects()=default;
    virtual std::vector<BlastObject> registry()=0;
    virtual bool valid(const ObjectWeakHandle& handle)=0;
    virtual void damage(const ExplosionDamageCall& call)=0;
    // 003cc820 of a script object: the source fields for game::CreateBlast.
    virtual BlastSource source(std::uint64_t object)=0;
};

// Blasts over a world: 0021a200 then 003ea1d0 for each event into `events`
// (the shared callback list: the script scheduler). When the scheduler runs
// an event, 0021b890 calls BlastObjects::damage. The flash requests are kept.
class ExplosionWorld {
public:
    ExplosionWorld(BlastObjects& objects,ScriptScheduler& events);
    BlastResult blast(const BlastRequest& request);
    // game::CreateBlast(GOBJ, int, float, float, float) = 0021a170:
    // 0021a200(003cc820(object), damage, 1, null, 0, 0, strength, inner, outer).
    // `object` 0 is the null object.
    BlastResult create_blast(std::uint64_t object,std::int32_t damage,float strength,float inner,float outer);
    const std::vector<BlastFlash>& flashes() const{return flashes_;}
    std::size_t fired() const{return fired_;}
private:
    BlastObjects& objects_;
    ScriptScheduler& events_;
    std::vector<BlastFlash> flashes_;
    std::size_t fired_{};
};

}
