#pragma once
// The explosion world over an ActorWorld (COMBAT_ITEMS.md "Checkpoint 55"):
// the BlastObjects view 0021a200 / 0021b890 / game::CreateBlast need.
//   registry  the actors first (world order), then the host's other registry
//             entries in its order. The order of the original registry
//             *00571748 (0040c9d0 at each object's activation) relative to
//             the actors is not established; it only orders events of equal
//             delay. An entry whose class the host does not know (its +40,
//             +2c/+2d/+2e, +14 or +100) makes registry() throw, so a blast
//             never skips an object silently.
//   damage    an actor takes 00187360 through SoloActorSimulation::apply_damage
//             with blast_actor_damage_input (category 1, no penetration, the
//             blast point as the hit point: type 2's LOS request starts there).
//             A source that is not an actor gives its own +e4 (needed by
//             00187360 before the hit point replaces it) and no controller.
//             A non-actor victim other than a host object throws.
//   source    003cc820 of a script object: a world actor, or a host object
//             (BlastWorldConfig::script_source).
//   objects   the host's live population objects (GOB / NIL / NIE, after the
//             static entries): +2c the host's (default 1), +2d the host's, +2e 0, +14 1, no
//             controller, +40 and +e4 from the host (+e4 read only for an
//             active entry with cvt.w.s(+40) > 0, as 0021a200 reads it). A
//             blast victim among them takes 003ccb00 (object_base_damage):
//             +40 is written back and the DamageMsg it queues to the object
//             waits in take_object_damage() for the next message pump.
#include "actor_world.hpp"
#include "explosion.hpp"
#include "gobj_damage.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace omega {

// The port's handles of the population objects: GOB k (population order) is
// gob_host_handle(k); its model COL in the static world names it as its game
// object (SoloCollisionWorld::game_objects), so LOS requests can exclude it.
inline constexpr std::uint64_t gob_host_handle(std::size_t k){return 0xb0000000ULL+k;}
// Hotbox k of the world (HotboxWorld::boxes order, run-time boxes included):
// the object a script's RequestFacial of a box names (hotbox_eye_c0).
inline constexpr std::uint64_t hotbox_host_handle(std::size_t k){return 0xf0000000ULL+k;}
// Sound object k (population.sound_objects order, SoundGOBJ vtable 004daf30): the object a script's
// RequestFacial / RequestLookAt of a SND names (MYANMAR's Area3Thugs facing O_FIRE_MEDIUM); above every
// 32-bit tag the other handle ranges use.
inline constexpr std::uint64_t sound_host_handle(std::size_t k){return 0x100000000ULL+k;}

// A registry entry that is not an actor. `known` false: the host cannot give
// its fields (registry() then throws, naming `what`).
struct BlastStaticEntry {
    BlastObject entry;
    bool known{};
    std::string what;
};
// A live non-actor registry entry (a population or event object).
struct BlastHostObject {
    std::uint64_t object{};
    std::int32_t id_c{};
};
struct BlastObjectHost {
    std::vector<BlastHostObject> objects;                        // registry order, after `others`
    std::function<float(std::size_t index)> health_40;
    std::function<void(std::size_t index, float health)> set_health_40;
    std::function<bool(std::size_t index)> present_2d;
    // +2c (Enable / Disable(GameGOBJ) 0021aa20 / 0021a940); empty: 1, the
    // base constructor's 003cf0e0 value.
    std::function<bool(std::size_t index)> enabled_2c;
    std::function<RootVector(std::size_t index)> position_e4;    // virtual +e4 (003cd260)
};
struct BlastWorldConfig {
    std::vector<std::int32_t> object_ids;                        // actor +c per world index
    std::vector<BlastStaticEntry> others;                        // registry entries after the actors
    BlastObjectHost host_objects;                                // after `others`
    // 003cc820 for a script object that is not an actor (by its record ID):
    // the handle, +14 and +e4. Empty or nullopt: throws.
    std::function<std::optional<BlastSource>(std::uint32_t record)> script_source;
    // A script record of an actor (the world index), for script blasts on actors.
    std::function<std::optional<std::size_t>(std::uint32_t record)> actor_of_record;
};
struct BlastDamageRecord {
    std::size_t actor{};
    ExplosionDamageCall call;
    SoloDamageOutcome outcome;
};
// 003ccb00 on a host object: its index in BlastObjectHost::objects, +40
// afterwards and the outcome (the queued DamageMsg when one was sent).
struct BlastObjectDamageRecord {
    std::size_t object{};
    ExplosionDamageCall call;
    float health_40{};
    ObjectDamageOutcome outcome;
};

class ActorWorldBlastObjects final:public BlastObjects {
public:
    ActorWorldBlastObjects(ActorWorld& world,BlastWorldConfig config);
    std::vector<BlastObject> registry() override;
    bool valid(const ObjectWeakHandle& handle) override;
    void damage(const ExplosionDamageCall& call) override;
    BlastSource source(std::uint64_t object) override;
    // The script object of game::CreateBlast: its record ID (0: null).
    std::uint64_t script_object(std::uint32_t record);
    // A host object's current 003cc820 fields (an NIE: event object whose +e4
    // follows its animation): registered or updated by record; returns the handle.
    std::uint64_t set_host_source(std::uint32_t record,const BlastSource& source);
    // A host object's damage virtual +100 called outside a blast (a bullet's
    // 00278xxx static damage: category 1, no point): 003ccb00 on its +40, the
    // result recorded as take_object_damage() reports a blast victim's.
    void object_damage(std::uint64_t object,std::uint64_t source,std::uint8_t category,std::uint8_t type,float amount);
    std::vector<BlastDamageRecord> take_damage();
    // The host objects' 003ccb00 results since the last call (every call,
    // with or without a message), in order.
    std::vector<BlastObjectDamageRecord> take_object_damage();
    // The object ID +c of a run-time actor the world has just appended.
    void append_actor(std::int32_t object_id);
    std::optional<std::size_t> host_object_index(std::uint64_t object) const;
    // The record of a script source (003cc820) registered by script_object / set_host_source.
    std::optional<std::uint32_t> script_record(std::uint64_t object) const {
        for(const auto& entry:script_objects_)if(entry.second.handle.object==object)return entry.first;
        return std::nullopt;
    }
    const BlastHostObject& host_object(std::size_t index) const { return config_.host_objects.objects.at(index); }
private:
    std::optional<std::size_t> actor_index(std::uint64_t object) const;
    BlastSource host_source(std::size_t index) const;
    ActorWorld& world_;
    BlastWorldConfig config_;
    std::vector<std::pair<std::uint32_t,BlastSource>> script_objects_;   // non-actor sources by record
    std::vector<BlastDamageRecord> damage_;
    std::vector<BlastObjectDamageRecord> object_damage_;
};

}
