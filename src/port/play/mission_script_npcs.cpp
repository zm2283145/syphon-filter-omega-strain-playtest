#include "mission_script_npcs.hpp"
#include "caster_bounds.hpp"
#include "npc_alert.hpp"
#include "npc_script_control.hpp"

#include <array>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

namespace omega {

ActorWorldScriptNpcs::ActorWorldScriptNpcs(ActorWorld& world, NpcAiSystem& ai_system, std::size_t player_actor)
    : world_(world), system_(ai_system), player_actor_(player_actor) {
    if (player_actor >= world.size() || !world.actor(player_actor).player_controller())
        throw std::runtime_error("Script NPCs: the player actor is not a player-controlled actor of the world");
}

void ActorWorldScriptNpcs::add(Word npc, std::size_t actor, NpcAiController& controller) {
    if (actor >= world_.size() || world_.actor(actor).player_controller())
        throw std::runtime_error("Script NPCs: NPC " + std::to_string(npc) + " is not an AI actor of the world");
    const auto [found, inserted] = npcs_.emplace(npc, Entry{actor, &controller});
    if (!inserted) throw std::runtime_error("Script NPCs: NPC " + std::to_string(npc) + " added twice");
    if (host_) connect_one(npc, found->second);
}

void ActorWorldScriptNpcs::connect_one(Word npc, const Entry& e) {
    MissionScriptHost& host = *host_;
    const Word id = npc;
    world_.actor(e.actor).set_ai_notification_filter([this, &host, id](const ActorAiNotification& n) {
        // NewAwareness (004ff298) carries the perceived object (+24).
        if (n.key == npc_script_new_awareness) return host.deliver_new_awareness(id, record_of_actor(n.object), static_cast<std::uint8_t>(n.value));
        // AIDeadMsg (004ff278): +24 the killer (0: none), +28 the damage type.
        if (n.key == npc_script_death) {
            if (n.object != 0 && unnamed_source(n.object)) return host.deliver_ai_dead_by_unnamed(id, static_cast<std::uint8_t>(n.value));
            return host.deliver_ai_dead(id, n.object != 0, n.object != 0 ? record_of_actor(n.object) : std::nullopt,
                                        static_cast<std::uint8_t>(n.value));
        }
        // DamageMsg (005716f0) from the base damage 003ccb00: +28 the source.
        if (n.key == actor_damage_message_key) {
            if (unnamed_source(n.object)) return host.deliver_damage_from_unnamed(id, n.value, n.type_2c, n.category_2d);
            return host.deliver_damage(id, record_of_actor(n.object), n.value, n.type_2c, n.category_2d);
        }
        // AIUnconsciousMsg (004ff2c0) from type-7 damage: +24 the source.
        if (n.key == actor_unconscious_message_key)
            return host.deliver_ai_unconscious(id, n.object != 0, n.object != 0 ? record_of_actor(n.object) : std::nullopt);
        // AIBodyMovedMsg (004ff2b0): +24 the carrier, +28 PickedUp.
        if (n.key == actor_body_moved_message_key) return host.deliver_ai_body_moved(id, record_of_actor(n.object), n.value != 0);
        // AIDisguiseTakenMsg (004ff2b8): +24 the taker.
        if (n.key == actor_disguise_taken_message_key) return host.deliver_ai_disguise_taken(id, record_of_actor(n.object));
        // Port-only: a path event at a co-op overlay node (set_port_overlay_first_node).
        if (port_overlay_first_ >= 0 && n.node >= port_overlay_first_) return false;
        return host.deliver_ai_notification(id, n.key, n.node);
    });
    world_.actor(e.actor).set_link_message_filter([&host, id](bool enable) { return host.deliver_link_message(id, enable); });
    world_.actor(e.actor).set_notify_checkpoint_filter([&host, id] { return host.deliver_notify_checkpoint(id); });
    world_.actor(e.actor).set_trigger_script_filter(
        [&host, id](const HotboxTrigger& t) { return host.deliver_trigger(t.receiver, t.action_24, id, t.word_28); });
    world_.actor(e.actor).set_native_trigger_receiver(
        [this, id](const HotboxTrigger& t) { return interact_trigger_ && interact_trigger_(id, t); });
    world_.actor(e.actor).set_object_code_receiver([this, id](std::uint64_t object, std::int32_t code) {
        if (interact_code_) interact_code_(id, object, code);
    });
    // AIWeaponFiredMsg (004ff2a0) to the AI component: +24 the shot's target (0: none).
    e.controller->set_weapon_fired_filter([this, &host, id](std::uint64_t target, bool thrown) {
        return host.deliver_ai_weapon_fired(id, target != 0, target != 0 ? record_of_actor(target) : std::nullopt, thrown);
    });
    // AIPickupableAwarenessMsg (004ff2a8) to the AI component: +24 the dead pickupable object.
    e.controller->set_pickupable_awareness_filter([this, &host, id](std::uint64_t object) {
        return host.deliver_ai_pickupable_awareness(id, record_of_actor(object));
    });
    // cAIModeChangeMsg (004ea3e8) to the AI component: +24 the new mode.
    e.controller->set_mode_change_filter([&host, id](std::uint8_t mode) { return host.deliver_ai_mode_change(id, mode); });
}

void ActorWorldScriptNpcs::connect(MissionScriptHost& host) {
    host_ = &host;
    host.set_port_overlay_first_node(port_overlay_first_);
    for (const auto& [npc, e] : npcs_) connect_one(npc, e);
    // The player actor's queue only receives the DamageMsg of its own base
    // damage (the AI notifications come from AI components); 003cc850
    // projects it to the player component.
    world_.actor(player_actor_).set_ai_notification_filter([this, &host](const ActorAiNotification& n) {
        if (n.key != actor_damage_message_key)
            throw std::runtime_error("Script NPCs: an AI notification queued to the player actor");
        if (unnamed_source(n.object)) return host.deliver_damage_from_unnamed(std::nullopt, n.value, n.type_2c, n.category_2d);
        return host.deliver_damage(std::nullopt, record_of_actor(n.object), n.value, n.type_2c, n.category_2d);
    });
}

std::vector<Word> ActorWorldScriptNpcs::link_actor_keys() const {
    std::vector<Word> keys;
    for (const auto& [npc, e] : npcs_) keys.push_back(npc);
    return keys;
}

void ActorWorldScriptNpcs::route_link_message(const HotboxLinkMessage& message) {
    if (!npcs_.count(message.receiver) && other_link_receiver_ && other_link_receiver_(message)) return;
    world_.actor(entry(message.receiver).actor).queue_link_message(message.enable);
}

void ActorWorldScriptNpcs::connect_hotbox_links(SoloActorSimulation& box_owner, OtherLinkReceiver other) {
    other_link_receiver_ = std::move(other);
    box_owner.set_hotbox_link_sink([this](const HotboxLinkMessage& m) { route_link_message(m); });
    for (const auto& m : box_owner.hotbox_load_link_messages()) route_link_message(m);
}

const ActorWorldScriptNpcs::Entry& ActorWorldScriptNpcs::entry(Word npc) const {
    const auto found = npcs_.find(npc);
    if (found == npcs_.end()) throw std::runtime_error("Script NPCs: NPC " + std::to_string(npc) + " is not hosted");
    // 0040bf50 freed it (ActorWorld::destroy): a script reference to it is a read of released memory.
    if (world_.destroyed(found->second.actor))
        throw std::runtime_error("Script NPCs: NPC " + std::to_string(npc) + " was deleted (0040bf50 -> 001b57c0): a native on freed memory is not hosted");
    return found->second;
}

std::size_t ActorWorldScriptNpcs::actor_of(Word npc) const { return entry(npc).actor; }

NpcAiController& ActorWorldScriptNpcs::controller(Word npc) { return *entry(npc).controller; }

NpcAiComponent* ActorWorldScriptNpcs::component(Word npc) {
    const auto found = npcs_.find(npc);
    return found == npcs_.end() ? nullptr : &found->second.controller->component();
}

void ActorWorldScriptNpcs::activate(Word npc) {
    world_.activate(entry(npc).actor);
}

bool ActorWorldScriptNpcs::activate_component(Word npc) {
    const auto& e = entry(npc);
    e.controller->activated(world_.actor(e.actor));
    return true;
}

void ActorWorldScriptNpcs::deactivate_anonymous(Word npc) {
    const auto& e = entry(npc);
    e.controller->deactivate_anonymous(world_.actor(e.actor));
}

bool ActorWorldScriptNpcs::deactivate(Word npc) {
    const auto& e = entry(npc);
    e.controller->deactivate(world_.actor(e.actor));
    return true;
}

bool ActorWorldScriptNpcs::reset_npc(Word npc) {
    const auto e = entry(npc);
    // 00148600: the owner's virtual +7c (00184cd0, lbu +3598) nonzero returns at once.
    if (e.controller->generator_spawned()) return true;
    if (!reset_) return false;
    // 00148600: component +4c (00157ad0) before the destructor 001b57c0.
    e.controller->deactivate(world_.actor(e.actor));
    const auto rebuilt = reset_(npc, e.actor);
    if (!rebuilt.controller || rebuilt.actor != e.actor || world_.actor(rebuilt.actor).player_controller())
        throw std::runtime_error("Script NPCs: the reset host of NPC " + std::to_string(npc) + " built no AI actor in place");
    auto& slot = npcs_.at(npc);
    slot = Entry{rebuilt.actor, rebuilt.controller};
    if (host_) connect_one(npc, slot);
    return true;
}

void ActorWorldScriptNpcs::replace_controller(Word npc, NpcAiController& controller) {
    auto& slot = npcs_.at(npc);
    if (world_.actor(slot.actor).player_controller())
        throw std::runtime_error("Script NPCs: NPC " + std::to_string(npc) + " was not rebuilt as an AI actor");
    slot.controller = &controller;
    if (host_) connect_one(npc, slot);
}

bool ActorWorldScriptNpcs::deactivate_actor(Word npc) {
    world_.deactivate(entry(npc).actor);
    return true;
}

void ActorWorldScriptNpcs::activate_by_radius(Word npc, float radius) {
    const auto& e = entry(npc);
    const auto actor = e.actor;
    e.controller->activate_by_radius(radius, [this, actor]() -> std::optional<float> {
        return actor_closest_player_distance_squared(actor);
    });
}

void ActorWorldScriptNpcs::set_not_gasable(Word npc, bool value) { entry(npc).controller->set_not_gasable(value); }

std::uint64_t ActorWorldScriptNpcs::actor_handle(std::optional<Word> npc) const {
    return world_.actor(npc ? entry(*npc).actor : player_actor_).handle();
}

bool ActorWorldScriptNpcs::unnamed_source(std::uint64_t handle) const {
    if (!unnamed_source_ || world_.actor(player_actor_).handle() == handle) return false;
    for (const auto& [npc, e] : npcs_)
        if (world_.actor(e.actor).handle() == handle) return false;
    return unnamed_source_(handle);
}

std::optional<Word> ActorWorldScriptNpcs::record_of_actor(std::uint64_t handle) const {
    if (world_.actor(player_actor_).handle() == handle) return std::nullopt;
    for (const auto& [npc, e] : npcs_)
        if (world_.actor(e.actor).handle() == handle) return npc;
    if (other_source_)
        if (const auto record = other_source_(handle)) return *record;
    std::ostringstream message;
    message << "Script NPCs: an object that is neither the player nor a hosted NPC (handle 0x" << std::hex << handle << ")";
    throw std::runtime_error(message.str());
}

bool ActorWorldScriptNpcs::add_awareness(Word npc, std::optional<Word> object) {
    const auto& e = entry(npc);
    if (!e.controller->perception_hosted()) return false;
    e.controller->script_add_awareness(world_.actor(e.actor), actor_handle(object));
    return true;
}

bool ActorWorldScriptNpcs::add_awareness_node(Word npc, std::int32_t node) {
    const auto& e = entry(npc);
    if (!e.controller->perception_hosted()) return false;
    e.controller->script_add_awareness_node(world_.actor(e.actor), node);
    return true;
}

bool ActorWorldScriptNpcs::add_awareness_object(Word npc, Word object) {
    const auto& e = entry(npc);
    if (!e.controller->perception_hosted()) return false;
    const auto handle = object_handle_ ? object_handle_(object) : std::nullopt;
    if (!handle) throw std::runtime_error("AddAwareness of a GOB the world does not host: " + std::to_string(object));
    e.controller->script_add_awareness(world_.actor(e.actor), *handle);            // 0014a410: 003cc820 (identity)
    return true;
}

bool ActorWorldScriptNpcs::add_awareness_of_players(Word npc) {
    const auto& e = entry(npc);
    if (!e.controller->perception_hosted()) return false;
    e.controller->script_add_awareness_of_players(world_.actor(e.actor));
    return true;
}

bool ActorWorldScriptNpcs::set_target(Word npc, std::optional<Word> object) {
    const auto& e = entry(npc);
    if (!e.controller->perception_hosted()) return false;
    e.controller->script_set_target(world_.actor(e.actor), actor_handle(object));
    return true;
}

bool ActorWorldScriptNpcs::set_target_node(Word npc, std::int32_t node) {
    const auto& e = entry(npc);
    if (!e.controller->perception_hosted()) return false;
    e.controller->script_set_target_node(world_.actor(e.actor), node);
    return true;
}

bool ActorWorldScriptNpcs::set_target_object(Word npc, Word object) {
    const auto& e = entry(npc);
    if (!e.controller->perception_hosted()) return false;
    const auto handle = object_handle_ ? object_handle_(object) : std::nullopt;
    if (!handle) throw std::runtime_error("SetTarget of a GOB the world does not host: " + std::to_string(object));
    e.controller->script_set_target(world_.actor(e.actor), *handle);              // 0014a210: 003cc820 (identity)
    return true;
}

bool ActorWorldScriptNpcs::set_grenade_target(Word npc, const GobjTarget& target) {
    const auto& e = entry(npc);
    if (!e.controller->perception_hosted()) return false;
    std::uint64_t handle = 0;
    switch (target.kind) {
    case GobjTarget::Kind::null: break;
    case GobjTarget::Kind::actor: handle = actor_handle(target.actor); break;
    case GobjTarget::Kind::node:
        e.controller->script_set_grenade_target_node(world_.actor(e.actor), target.node);
        return true;
    case GobjTarget::Kind::object: {
        const auto found = object_handle_ ? object_handle_(target.object) : std::nullopt;
        if (!found) throw std::runtime_error("SetGrenadeTarget of a GOB the world does not host: " + std::to_string(target.object));
        handle = *found;
        break;
    }
    }
    e.controller->script_set_grenade_target(world_.actor(e.actor), handle);      // 00149810: 003cc820 (identity)
    return true;
}

bool ActorWorldScriptNpcs::resume_ai_targeting(Word npc) {
    const auto& e = entry(npc);
    if (!e.controller->perception_hosted()) return false;
    e.controller->script_resume_ai_targeting(world_.actor(e.actor));
    return true;
}

bool ActorWorldScriptNpcs::resume_ai_mode(Word npc) {
    const auto& e = entry(npc);
    if (!e.controller->perception_hosted()) return false;
    e.controller->script_resume_ai_mode(world_.actor(e.actor));
    return true;
}

bool ActorWorldScriptNpcs::look_around(Word npc) {
    const auto& e = entry(npc);
    e.controller->script_look_around(world_.actor(e.actor));
    return true;
}

bool ActorWorldScriptNpcs::stop_and_look_around(Word npc) {
    const auto& e = entry(npc);
    e.controller->script_stop_and_look_around(world_.actor(e.actor));
    return true;
}

bool ActorWorldScriptNpcs::request_action(Word npc, std::uint8_t action) {
    const auto& e = entry(npc);
    e.controller->script_request_action(world_.actor(e.actor), action);
    return true;
}

bool ActorWorldScriptNpcs::request_facial(Word npc, std::optional<Word> object, float duration) {
    const auto& e = entry(npc);
    if (!e.controller->facial_hosted()) return false;
    e.controller->script_request_facial(world_.actor(e.actor), actor_handle(object), duration);
    return true;
}

bool ActorWorldScriptNpcs::request_facial_node(Word npc, std::int32_t node, float duration) {
    const auto& e = entry(npc);
    if (!e.controller->facial_hosted()) return false;
    e.controller->script_request_facial_node(world_.actor(e.actor), node, duration);
    return true;
}

bool ActorWorldScriptNpcs::request_facial_object(Word npc, Word object, float duration) {
    const auto& e = entry(npc);
    if (!e.controller->facial_hosted()) return false;
    const auto handle = object_handle_ ? object_handle_(object) : std::nullopt;
    if (!handle) throw std::runtime_error("RequestFacial of a GOB the world does not host: " + std::to_string(object));
    e.controller->script_request_facial(world_.actor(e.actor), *handle, duration);
    return true;
}

bool ActorWorldScriptNpcs::request_facial_hotbox(Word npc, Word box, float duration) {
    const auto& e = entry(npc);
    if (!e.controller->facial_hosted()) return false;
    const auto handle = hotbox_handle_ ? hotbox_handle_(box) : std::nullopt;
    if (!handle) return false;
    e.controller->script_request_facial(world_.actor(e.actor), *handle, duration);
    return true;
}

bool ActorWorldScriptNpcs::request_look_at(Word npc, std::optional<Word> object, float duration) {
    const auto& e = entry(npc);
    if (!e.controller->facial_hosted()) return false;
    e.controller->script_request_look_at(world_.actor(e.actor), actor_handle(object), duration);
    return true;
}

bool ActorWorldScriptNpcs::request_look_at_node(Word npc, std::int32_t node, float duration) {
    const auto& e = entry(npc);
    if (!e.controller->facial_hosted()) return false;
    e.controller->script_request_look_at_node(world_.actor(e.actor), node, duration);
    return true;
}

bool ActorWorldScriptNpcs::request_look_at_object(Word npc, Word object, float duration) {
    const auto& e = entry(npc);
    if (!e.controller->facial_hosted()) return false;
    const auto handle = object_handle_ ? object_handle_(object) : std::nullopt;
    if (!handle) throw std::runtime_error("RequestLookAt of a GOB the world does not host: " + std::to_string(object));
    e.controller->script_request_look_at(world_.actor(e.actor), *handle, duration);
    return true;
}

bool ActorWorldScriptNpcs::request_look_at_hotbox(Word npc, Word box, float duration) {
    const auto& e = entry(npc);
    if (!e.controller->facial_hosted()) return false;
    const auto handle = hotbox_handle_ ? hotbox_handle_(box) : std::nullopt;
    if (!handle) return false;
    e.controller->script_request_look_at(world_.actor(e.actor), *handle, duration);
    return true;
}

bool ActorWorldScriptNpcs::resume_look_at(Word npc) {
    entry(npc).controller->script_resume_look_at();
    return true;
}

bool ActorWorldScriptNpcs::deactivate_dead(Word npc, bool value) {
    entry(npc).controller->script_deactivate_dead(value);
    return true;
}
bool ActorWorldScriptNpcs::equip_goggles(Word npc, bool on) {
    if (!goggles_) return false;
    goggles_(entry(npc).actor, on);
    return true;
}

std::optional<std::uint8_t> ActorWorldScriptNpcs::move_to_node(Word npc, std::int32_t node) {
    return entry(npc).controller->script_move_to_node(node);
}

std::optional<std::uint8_t> ActorWorldScriptNpcs::move_to_object(Word npc, Word object) {
    return entry(npc).controller->move_to_object(static_cast<std::int32_t>(object));
}

bool ActorWorldScriptNpcs::set_mode_at_node(Word npc, std::uint32_t mode, std::int32_t node) {
    const auto& e = entry(npc);
    e.controller->script_set_mode_at_node(world_.actor(e.actor), mode, node);
    return true;
}

std::optional<std::int32_t> ActorWorldScriptNpcs::hit_points(Word npc) {
    // 00149660: lwc1 +40 of the actor (component +30), cvt.w.s.
    return npc_cvt_w_s(world_.actor(entry(npc).actor).health().health_40);
}

bool ActorWorldScriptNpcs::add_weapon(Word npc, std::int32_t item) {
    auto& c = *entry(npc).controller;
    if (!c.weapons_hosted()) return false;
    c.script_add_weapon(item);
    return true;
}

std::optional<RootVector> ActorWorldScriptNpcs::position_e4(Word npc) {
    const auto found = npcs_.find(npc);
    if (found == npcs_.end()) return std::nullopt;
    return world_.actor(found->second.actor).position_e4();
}

std::optional<MissionScriptNpcs::InteractableInputs> ActorWorldScriptNpcs::interactable_box_inputs(Word npc) {
    const auto found = npcs_.find(npc);
    if (found == npcs_.end()) return std::nullopt;
    const auto& a = world_.actor(found->second.actor);
    const auto b = human_context_bounds(a.skeleton_node_matrices());
    return InteractableInputs{a.component(), RootVector{b.minimum[0], b.minimum[1], b.minimum[2], b.minimum[3]},
                              RootVector{b.maximum[0], b.maximum[1], b.maximum[2], b.maximum[3]}};
}

bool ActorWorldScriptNpcs::draw_weapon(Word npc, std::int32_t item) {
    auto& c = *entry(npc).controller;
    if (!c.weapons_hosted()) return false;
    c.script_draw_weapon(item);
    return true;
}

bool ActorWorldScriptNpcs::kill(Word npc) {
    if (!damage_) return false;
    const auto actor = entry(npc).actor;
    const auto& a = world_.actor(actor);
    // 0014b384..0014b3ac: actor virtual +108; 3 with +33a5 clear returns.
    if (a.life_state() == 3 && !a.health().killed_33a5) return true;
    // 0014b3bc..0014b3e8: f12 = cvt.s.w(cvt.w.s(+40)), a1 = the actor itself.
    damage_(actor, 2, 0xa, static_cast<float>(npc_cvt_w_s(a.health().health_40)));
    return true;
}

bool ActorWorldScriptNpcs::set_hit_points(Word npc, std::int32_t hp) {
    auto& a = world_.actor(entry(npc).actor);
    // 001495f0: cvt.w.s of +40, blez skips; 003cc990: +40 = cvt.s.w(hp).
    if (npc_cvt_w_s(a.health().health_40) > 0) a.set_health_40(static_cast<float>(hp));
    return true;
}

bool ActorWorldScriptNpcs::set_invulnerable(Word npc, bool value) {
    world_.actor(entry(npc).actor).set_invulnerable_33a4(value);   // 001b8750: sb a1, +33a4
    return true;
}

bool ActorWorldScriptNpcs::dont_drop_weapons(Word npc) {
    return entry(npc).controller->script_dont_drop_weapons();
}

bool ActorWorldScriptNpcs::set_on_fire(Word npc) {
    auto& a = world_.actor(entry(npc).actor);
    if (!a.fire_ignite_hosted()) return false;
    a.fire_ignite();   // 0014b350: j 00186360(actor, 1, 0, 0, 0, -1.0)
    return true;
}

bool ActorWorldScriptNpcs::resume_movement(Word npc) {
    const auto& e = entry(npc);
    e.controller->script_resume_movement(world_.actor(e.actor));
    return true;
}

std::optional<std::uint8_t> ActorWorldScriptNpcs::carry_body_to_node(Word npc, Word body, std::int32_t node) {
    auto& c = entry(npc).controller->component();
    // 0014bd60 reads the body's virtual +108 and its +58 controller. Only an
    // NPC actor is hosted as the body (its AI component is the +58 while the
    // actor lives in the world); other GOBJs have their own +108.
    if (npcs_.find(body) == npcs_.end())
        throw std::runtime_error("CarryBodyToNode (0014bd60): body " + std::to_string(body) +
                                 " is not an NPC actor; its virtual +108 and +58 are not hosted");
    const auto life = world_.actor(entry(body).actor).life_state();
    return npc_carry_body_to_node(c, life, true, node, static_cast<std::int32_t>(body));
}

bool ActorWorldScriptNpcs::follow(Word npc, std::optional<Word> target, float a, float b) {
    npc_follow(entry(npc).controller->component(),
               target ? std::optional<std::int32_t>(static_cast<std::int32_t>(*target)) : std::nullopt, a, b);
    return true;
}

bool ActorWorldScriptNpcs::restrict_to_node_group(Word npc, std::int32_t node) {
    entry(npc).controller->script_restrict_to_node_group(node);
    return true;
}

bool ActorWorldScriptNpcs::teleport(Word npc, std::int32_t node) {
    const auto& e = entry(npc);
    e.controller->script_teleport(world_.actor(e.actor), node);
    return true;
}

bool ActorWorldScriptNpcs::set_unconscious(Word npc) {
    if (!damage_) return false;
    const auto actor = entry(npc).actor;
    // 0014a770: lbu +33a5, bnez returns; f12 = 0.0, a1 = the actor itself, a2 = 1, a3 = 7.
    if (world_.actor(actor).health().killed_33a5) return true;
    damage_(actor, 1, 7, 0.0F);
    return true;
}

bool ActorWorldScriptNpcs::set_pickupable(Word npc, bool value) {
    const auto& e = entry(npc);
    e.controller->script_set_pickupable(world_.actor(e.actor), value);
    return true;
}

bool ActorWorldScriptNpcs::set_has_disguise(Word npc, bool value) {
    const auto& e = entry(npc);
    NpcAiController::script_set_disguise(world_.actor(e.actor), value ? 1U : 0U);   // 001491f0: +1d8(c, arg != 0, 1)
    return true;
}

bool ActorWorldScriptNpcs::activate_body_toss(Word object, bool flag) {
    // Only an NPC actor is hosted as the receiver: the player component's
    // receiver stores the byte at +188 (00177840) and other GOBJs have their
    // own handlers.
    if (npcs_.find(object) == npcs_.end())
        throw std::runtime_error("game::ActivateBodyToss (00476740): object " + std::to_string(object) +
                                 " is not an NPC actor; its receiver (the player component's 00177840 or a GOBJ's) is not hosted");
    entry(object).controller->script_activate_body_toss(flag);
    return true;
}

bool ActorWorldScriptNpcs::activate_by_radius_off(Word npc) {
    entry(npc).controller->script_activate_by_radius_off();
    return true;
}

bool ActorWorldScriptNpcs::add_group_to_path_region(Word npc, std::int32_t node) {
    entry(npc).controller->script_add_group_to_path_region(node);
    return true;
}

bool ActorWorldScriptNpcs::remove_group_from_path_region(Word npc, std::int32_t node) {
    entry(npc).controller->script_remove_group_from_path_region(node);
    return true;
}

bool ActorWorldScriptNpcs::force_weapon_fire(Word npc, std::int32_t shots, std::int32_t item, float delay) {
    const auto& e = entry(npc);
    if (item >= 0 && !e.controller->weapons_hosted()) return false;
    e.controller->script_force_weapon_fire(world_.actor(e.actor), shots, item, delay);
    return true;
}

std::uint64_t ActorWorldScriptNpcs::ai_object_handle(const Entry& e, const AiObject& object, const char* native) const {
    switch (object.kind) {
    case AiObject::Kind::actor: return actor_handle(object.actor);
    case AiObject::Kind::node: return e.controller->script_node_object(object.node);
    case AiObject::Kind::object: {
        const auto handle = object_handle_ ? object_handle_(object.object) : std::nullopt;
        if (!handle)
            throw std::runtime_error(std::string(native) + " of a GOB the world does not host: " + std::to_string(object.object));
        return *handle;
    }
    }
    throw std::logic_error("AiObject kind");
}

bool ActorWorldScriptNpcs::remove_awareness(Word npc, const AiObject& object) {
    const auto& e = entry(npc);
    if (!e.controller->perception_hosted()) return false;
    e.controller->script_remove_awareness(world_.actor(e.actor), ai_object_handle(e, object, "RemoveAwareness"));
    return true;
}

bool ActorWorldScriptNpcs::resend_new_awareness(Word npc, const std::optional<AiObject>& object) {
    const auto& e = entry(npc);
    if (!e.controller->perception_hosted()) return false;
    const std::uint64_t handle = object ? ai_object_handle(e, *object, "ResendNewAwarenessMessage") : 0;   // 0014a390: a1 = 0
    e.controller->script_resend_awareness(world_.actor(e.actor), handle);
    return true;
}

bool ActorWorldScriptNpcs::clear_target(Word npc) {
    const auto& e = entry(npc);
    if (!e.controller->perception_hosted()) return false;
    e.controller->script_set_target(world_.actor(e.actor), 0);   // 0014a1e0: 0016ddb0(p, 0, 100)
    return true;
}

std::optional<bool> ActorWorldScriptNpcs::in_view_cone(Word npc, const AiObject& object) {
    const auto& e = entry(npc);
    if (!e.controller->perception_hosted()) return std::nullopt;
    return e.controller->script_in_view_cone(world_.actor(e.actor), ai_object_handle(e, object, "InViewCone"));
}

bool ActorWorldScriptNpcs::set_view_cone_radius(Word npc, float radius) {
    entry(npc).controller->script_set_view_cone_radius(radius);
    return true;
}

bool ActorWorldScriptNpcs::set_view_cone_angle(Word npc, float angle) {
    entry(npc).controller->script_set_view_cone_angle(angle);
    return true;
}

bool ActorWorldScriptNpcs::store_weapon(Word npc) {
    auto& c = *entry(npc).controller;
    if (!c.weapons_hosted()) return false;
    c.script_store_weapon();
    return true;
}

bool ActorWorldScriptNpcs::remove_weapon(Word npc, std::int32_t item) {
    auto& c = *entry(npc).controller;
    if (!c.weapons_hosted()) return false;
    c.script_remove_weapon(item);
    return true;
}

bool ActorWorldScriptNpcs::send_new_awareness_of_players(Word npc) {
    entry(npc).controller->script_send_new_awareness_of_players();
    return true;
}

bool ActorWorldScriptNpcs::force_holster(Word npc) {
    auto& c = *entry(npc).controller;
    if (!c.weapons_hosted()) return false;
    c.script_force_holster();
    return true;
}

std::optional<std::int32_t> ActorWorldScriptNpcs::active_weapon(Word npc) {
    const auto& c = *entry(npc).controller;
    if (!c.weapons_hosted()) return std::nullopt;
    return c.script_active_weapon();
}

bool ActorWorldScriptNpcs::set_not_taserable(Word npc, bool value) {
    world_.actor(entry(npc).actor).set_stun_immune_346a(value ? 1 : 0);   // 0014a760 -> 00198f90(actor, flag, 0)
    return true;
}

bool ActorWorldScriptNpcs::set_fire_invulnerability(Word npc, bool value) {
    world_.actor(entry(npc).actor).set_fire_immune_33b8(value);   // 0014a7c0: sb a1, +33b8
    return true;
}

bool ActorWorldScriptNpcs::set_footstep_sound_radius(Word npc, float radius) {
    world_.actor(entry(npc).actor).set_running_31f8(radius);   // 00169b70: AI +58 0015b7e0 answers 1; swc1 f20, +31f8
    return true;
}

bool ActorWorldScriptNpcs::stop_movement(Word npc, float duration) {
    const auto& e = entry(npc);
    e.controller->stop_movement(world_.actor(e.actor), 100, duration);   // 00149ca0: +154(c, 100, f12)
    return true;
}

RootVector ActorWorldScriptNpcs::eye_position(std::size_t actor) const {
    const auto& a = world_.actor(actor);
    // 00188d80: +2d and +2c set, +2e clear (the traversal eligibility).
    if (a.eligible()) return a.skeleton_node_world_position(npc_eye_node);
    // Not present: the model transform component (+50) translation +40.
    return a.component();
}

float ActorWorldScriptNpcs::actor_closest_player_distance_squared(std::size_t actor) const {
    const std::array<std::optional<RootVector>, 4> players{eye_position(player_actor_), std::nullopt, std::nullopt,
                                                           std::nullopt};
    return npc_closest_player_eye_distance_squared(eye_position(actor), players);
}

}
