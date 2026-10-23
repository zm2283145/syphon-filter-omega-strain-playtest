#include "engine_variables.hpp"
#include "mission_script_host.hpp"

#include "actor_object_action.hpp"
#include "elevator_gobj.hpp"
#include "engine_globals.hpp"
#include "game_sound.hpp"
#include "game_sound_voice.hpp"
#include "hud_object.hpp"
#include "npc_alert.hpp"
#include "npc_attack.hpp"
#include "objective_registration.hpp"
#include "pad_interact_prompt.hpp"
#include "vu_float.hpp"
#include "water_effects.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace omega {

float script_gobj_distance(const RootVector& a, const RootVector& b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];   // vsub
    const float xx = x * x, yy = y * y, zz = z * z;                    // vmul.xyz
    const float sum = (xx + yy) + zz;                                  // vmulax.w / vmadday.w / vmaddz.w
    return vu::sqrt(sum);                                              // vsqrt Q; vaddq.x 0 + Q
}

// 003cc5c0: lw s1, 4(a0); mtc1 s1, f0; cvt.s.w f0, f0; swc1 f0, 0x40(s0).
float gobj_set_hit_points(std::int32_t value) { return game_int_to_float(value); }

// 003cc6d0: lwc1 f0, 0x40(v0); cvt.w.s f0, f0; mfc1 v0, f0.
std::int32_t gobj_hit_points(float health_40) {
    if (!(health_40 >= -2147483648.0F && health_40 < 2147483648.0F))
        throw std::runtime_error("GetHitpoints: +40 outside the cvt.w.s range");
    return static_cast<std::int32_t>(health_40);
}

std::string quoted_json(const std::string& text) {
    std::string result = "\"";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 0x20) {
            char escaped[8];
            std::snprintf(escaped, sizeof escaped, "\\u%04x", c);
            result += escaped;
        } else result += static_cast<char>(c);
    }
    return result + "\"";
}

std::string describe_script_value(const Value& value) {
    if (const auto* word = std::get_if<Word>(&value)) return std::to_string(*word);
    if (const auto* text = std::get_if<std::string>(&value)) return quoted_json(*text);
    if (const auto* object = std::get_if<ObjectHandle>(&value))
        return quoted_json("object " + std::to_string(int(object->kind)) + ":" + std::to_string(object->id));
    if (const auto* runtime = std::get_if<RuntimeObjectHandle>(&value))
        return quoted_json("runtime " + std::to_string(int(runtime->kind)) + ":" + std::to_string(runtime->id) + "/" +
                           std::to_string(runtime->generation));
    return quoted_json("address");
}

std::string script_native_signature(const Script& script, const Function& function) {
    std::string result = function.name + "(";
    for (std::size_t i = 0; i < function.parameters.size(); ++i)
        result += (i ? ", " : "") + script.type_key(function.parameters[i]);
    return result + ") -> " + script.type_key(function.return_type);
}

namespace {

// The stage label native records carry while a delivery or pump runs.
struct StageScope {
    std::string& stage;
    StageScope(std::string& target, std::string label) : stage(target) { stage = std::move(label); }
    ~StageScope() { stage = "running"; }
};

// Receiver script types (vt+0x34): SCRIPT_HOSTING.md §2.1's boot-snapshot
// list (actor components, hotbox, generator, pathed, interact, backpack); the
// rest are the script tables' names for those classes, or GOBJ.
std::string receiver_type(const std::string& section, std::uint32_t kind) {
    if (section == "hotbox") return "Hotbox";
    if (section == "generator") return "Generator";
    if (section == "node") return "Node";
    if (section == "backpack") return "Backpack";
    if (section == "fire") return "FireObj";
    if (section == "event_object") return "NIEventOBJ";
    if (section == "event_light") return "NIEventLight";
    if (section == "sound") return "SoundGOBJ";
    if (section == "camera") return "GameCamera";
    if (section == "tank") return "Tank";
    if (section == "object")
        return kind == 1 ? "GameGOBJ" : kind == 2 ? "PathedGOBJ" : kind == 3 ? "ElevatorGOBJ" : kind == 5 ? "InteractGOBJ" : "GOBJ";
    return "GOBJ";
}

Word number(const Value& value) {
    if (const auto* result = std::get_if<Word>(&value)) return *result;
    throw std::runtime_error("Objective callback expected an integer word");
}
void manager(const Value& value) {
    const auto* handle = std::get_if<ObjectHandle>(&value);
    if (!handle || handle->kind != ObjectHandle::Kind::objective_manager || handle->id != 0)
        throw std::runtime_error("Objective callback expected the manager handle");
}
void require_signature(const Script& script, const Function& function, Word return_type,
                       const std::vector<std::string>& types) {
    if (function.return_type != return_type || function.parameters.size() != types.size())
        throw std::runtime_error("Unsupported native signature: " + script_native_signature(script, function));
    for (std::size_t i = 0; i < types.size(); ++i)
        if (script.type_key(function.parameters[i]) != types[i])
            throw std::runtime_error("Unsupported native parameter: " + script_native_signature(script, function));
}

}

PopulationObjects::PopulationObjects(const Population& population) {
    objects.add_scope(1);
    objects.set_current_scope(1);
    auto add = [&](Word id, const char* section) {
        const auto [found, inserted] = sections.emplace(id, section);
        if (inserted) objects.register_world_object(id, 1);
        else collisions.push_back(std::to_string(id) + " " + found->second + "/" + section);
    };
    for (std::size_t i = 0; i < population.npcs.size(); ++i) {
        const auto id = population.npcs[i].id;
        if (!sections.emplace(id, "npc").second) throw std::runtime_error("Duplicate population NPC ID");
        objects.register_npc(id, 1);
        objects.attach(id, i ? RuntimeObjectHandle::Kind::ai : RuntimeObjectHandle::Kind::player);
    }
    if (!population.npcs.empty()) player = population.npcs[0].id;
    for (const auto& object : population.objects) add(object.id, "object");
    for (const auto& node : population.nodes) add(node.id, "node");
    for (const auto& terrain : population.terrain) add(terrain.id, "terrain");
    for (const auto& placement : population.placements) add(placement.id, "placement");
    for (const auto& hotbox : population.hotboxes) add(hotbox.id, "hotbox");
    for (const auto& light : population.point_lights) add(light.id(), "point_light");
    for (const auto& light : population.directional_lights) add(light.id(), "directional_light");
    for (const auto& environment : population.environments) add(environment.id(), "environment");
    for (const auto& generator : population.generators) add(generator.id, "generator");
    for (const auto& backpack : population.backpacks) add(backpack.id, "backpack");
    for (const auto& fire : population.fires) add(fire.id, "fire");
    for (const auto& light : population.event_lights) add(light.id, "event_light");
    for (const auto& object : population.event_objects) add(object.id, "event_object");
    for (const auto& sound : population.sound_objects) add(sound.id, "sound");
    for (const auto& camera : population.cameras) add(camera.id, "camera");      // CAM: (132ms2)
    for (const auto& object : population.objects) kinds.emplace(object.id, object.kind);
}

Word PopulationObjects::world_object_id(RuntimeObjectHandle object) const {
    if (object.kind != RuntimeObjectHandle::Kind::world_object || !objects.alive(object))
        throw std::runtime_error("Object text of a stale world object");
    const auto found = sections.find(object.id);
    const std::string section = found == sections.end() ? "unregistered" : found->second;
    if (section == "npc" || section == "node") return object.id;
    throw std::runtime_error("Object text (0040c1f0): the vt+10 of a " + section + " object (" + std::to_string(object.id) +
                             ") is not hosted");
}

void PopulationObjects::add_spawned_npc(Word id) {
    if (!sections.emplace(id, "npc").second) throw std::runtime_error("Spawned NPC ID already registered: " + std::to_string(id));
    objects.register_npc(id, 1);
    objects.attach(id, RuntimeObjectHandle::Kind::ai);
    spawned.insert(id);
}

void PopulationObjects::flush_spawned_npc(Word id) {
    if (!spawned.contains(id)) throw std::runtime_error("Flush of an unknown spawned NPC: " + std::to_string(id));
    objects.hide(id);
}

void PopulationObjects::delete_spawned_npc(Word id) {
    if (!spawned.contains(id)) throw std::runtime_error("Delete of an unknown spawned NPC: " + std::to_string(id));
    objects.remove(id);
    sections.erase(id);
    spawned.erase(id);
}

std::pair<std::string, ScriptContext::Receiver> PopulationObjects::receiver(Word id) const {
    const auto found = sections.find(id);
    if (found == sections.end()) throw std::runtime_error("Receiver outside the population: " + std::to_string(id));
    const auto& section = found->second;
    ScriptContext::Receiver receiver;
    receiver.id = id;
    std::string label = section;
    if (section == "npc") {
        // The filter's projection: an NPC actor gives its AI or player component.
        const auto controller = objects.resolve(id);
        receiver.object = controller;
        const bool is_player = controller.kind == RuntimeObjectHandle::Kind::player;
        receiver.type = is_player ? "Player" : "NPC";
        label = is_player ? "player" : "ai";
    } else {
        receiver.object = objects.resolve(id | 0x80000000U);
        const auto kind = kinds.find(id);
        receiver.type = receiver_type(section, kind == kinds.end() ? 0 : kind->second);
        if (section == "object") label += "/" + receiver.type;
    }
    return {label, std::move(receiver)};
}

bool ObjectiveNatives::hosts(const Script& script, const Function& function) {
    static const std::set<std::string> hosted{
        "game::IsMultiplayer", "game::GetObjectiveManager", "game::GetString", "AddObjective", "GetObjective",
        "IsComplete", "WasFailed", "IsActive", "SetName", "SetMenuString", "SetMapString", "SetAddString",
        "SetCompleteString", "SetFailString", "SetType", "SetNotifyType", "SetMultiType", "SetMaxOwners",
        "StartStage", "Activate", "Deactivate", "Succeed", "Fail", "DestroyObjectives"};
    if (!hosted.contains(function.name)) return false;
    if (function.name.starts_with("game::")) return true;
    if (function.parameters.empty()) return false;
    const auto receiver = script.type_key(function.parameters[0]);
    return receiver == "object:ObjectiveMan" || receiver == "object:Objective";
}

ObjectiveNatives::ObjectiveNatives(MissionObjectiveInputs inputs) : inputs_(std::move(inputs)) {
    markers_.observe([this](MarkerAction action, const ObjectiveMarker& marker) {
        marker_events_.push_back({action, marker});
    });
}

MissionObjectiveInputs mission_objective_inputs(const Script& awards, StringPool base_pool, std::string_view level,
                                                std::optional<StringPool> mission_pool) {
    MissionObjectiveInputs inputs;
    if (mission_pool) {
        inputs.localization.emplace(base_pool);
        inputs.localization->replace_mission(std::move(*mission_pool));
    }
    inputs.mission_cue = level_mission_number(read_level_registrations(awards), level);
    ObjectiveProgression progression;
    for (const auto& row : read_objective_registration_prefix(awards).rows) {
        const auto id = base_pool.lookup(row.key);
        if (id < 0) throw std::runtime_error("AWARDS objective key outside the base pool: " + row.key);
        progression.register_objective(id, row.points, row.solo_required, row.multi_required);
    }
    inputs.progression = std::move(progression);
    inputs.base_pool = std::move(base_pool);
    return inputs;
}

bool ObjectiveNatives::add_progression(const Script& script, const Function& function,
                                       const std::vector<Value>& arguments) {
    require_signature(script, function, 4, {"primitive:7"});
    if (!inputs_.progression || !inputs_.base_pool) return false;
    const auto* key = std::get_if<std::string>(&arguments.at(0));
    if (!key) throw std::runtime_error("game::AddObjective without a key string");
    // 00330aa0 clamps a missing key (or negative row value) to index 0; the
    // pool lookup's -1 reaches the same clamp.
    const auto intern_id = inputs_.base_pool->lookup(*key);
    progression_bits_.add(*inputs_.progression, intern_id);
    progression_adds_.push_back({*key, intern_id, inputs_.progression->lookup(intern_id)});
    return true;
}

bool ObjectiveNatives::display_label(const Script& script, const Function& function,
                                     const std::vector<Value>& arguments) {
    if (function.parameters.size() == 3)
        require_signature(script, function, 4, {"object:ObjectiveMan", "primitive:7", "object:GOBJ"});
    else
        require_signature(script, function, 4, {"object:ObjectiveMan", "primitive:7"});
    manager(arguments[0]);
    const auto* key = std::get_if<std::string>(&arguments[1]);
    if (!key) throw std::runtime_error("SetDisplayLabel without a key string");
    if (!inputs_.localization) return false;
    const auto text = inputs_.localization->resolve(*key);
    display_labels_.push_back({*key, text ? std::optional<std::string>(std::string(*text)) : std::nullopt});
    return true;
}

bool ObjectiveNatives::display_string(const Script& script, const Function& function,
                                      const std::vector<Value>& arguments) {
    require_signature(script, function, 4, {"object:ObjectiveMan", "primitive:3"});
    manager(arguments[0]);
    const auto* text = std::get_if<std::string>(&arguments[1]);
    if (!text) throw std::runtime_error("SetDisplayString without a string");
    if (!inputs_.localization) return false;
    display_strings_.push_back(*text);
    return true;
}

bool ObjectiveNatives::set_menu_label(const Script& script, const Function& function,
                                      const std::vector<Value>& arguments) {
    return set_label(script, function, arguments, "SetMenuLabel");
}

bool ObjectiveNatives::set_label(const Script& script, const Function& function,
                                 const std::vector<Value>& arguments, const std::string& name) {
    // Native -> (setter, the field the matching Set*String writes).
    static const std::map<std::string, std::pair<std::string, std::string>> labels{
        {"SetMenuLabel", {"00228940", "SetMenuString"}},         // +60
        {"SetMapLabel", {"00228850", "SetMapString"}},           // +6c
        {"SetAddLabel", {"00228760", "SetAddString"}},           // +70
        {"SetCompleteLabel", {"00228670", "SetCompleteString"}}, // +78
        {"SetFailLabel", {"00228580", "SetFailString"}},         // +80
    };
    const auto& [setter, property] = labels.at(name);
    require_signature(script, function, 4, {"object:Objective", "primitive:7"});
    const auto* handle = std::get_if<ObjectHandle>(&arguments.at(0));
    if (!handle || handle->kind != ObjectHandle::Kind::objective)
        throw std::runtime_error(name + " expected a typed objective");
    const auto* key = std::get_if<std::string>(&arguments.at(1));
    if (!key) throw std::runtime_error(name + " without a key string");
    if (handle->id == 0) return true;
    if (!stage_.find(handle->id)) throw std::runtime_error(name + " on an objective outside the stage");
    if (!inputs_.localization) return false;
    const auto text = inputs_.localization->resolve(*key);
    if (!text) throw std::runtime_error(name + " " + setter + ": key " + *key + " does not resolve (00228b20 strlen of NULL)");
    host_.objectives.at(handle->id - 1).properties[property] = {Value{std::string(*text)}};
    return true;
}

bool ObjectiveNatives::add_hud_objective(Word object, const std::string& key, std::optional<std::string> cue) {
    if (!inputs_.localization) return false;
    markers_.add(object, inputs_.localization->resolve(key), cue);
    return true;
}

Word ObjectiveNatives::map_objective(const Value& value, const std::string& native) const {
    const auto* handle = std::get_if<ObjectHandle>(&value);
    if (!handle || handle->kind != ObjectHandle::Kind::objective)
        throw std::runtime_error(native + " expected a typed objective");
    if (handle->id != 0 && !stage_.find(handle->id))
        throw std::runtime_error(native + " on an objective outside the stage");
    return handle->id;
}

std::optional<std::string> ObjectiveNatives::map_string(Word objective) const {
    const auto& properties = host_.objectives.at(objective - 1).properties;
    const auto found = properties.find("SetMapString");
    if (found == properties.end()) return std::nullopt;
    if (found->second.size() != 1 || !std::holds_alternative<std::string>(found->second[0]))
        throw std::runtime_error("SetMapString property without one string");
    return std::get<std::string>(found->second[0]);
}

bool ObjectiveNatives::remove_hud_objective(Word object) {
    if (!inputs_.localization) return false;
    markers_.remove(object);
    return true;
}

// Signatures and semantics were checked against the original callbacks; text
// and type configuration stays in ObjectiveHost, status in ObjectiveStage.
std::optional<Value> ObjectiveNatives::call(const Script& script, const Function& function,
                                            const std::vector<Value>& arguments) {
    const auto& name = function.name;
    if (name == "Succeed" || name == "Fail") {
        // The two-argument wrappers pass a null optional actor (no actor
        // action, OBJECTIVE_OUTCOMES.md); the actor form is not hosted.
        require_signature(script, function, 4, {"object:ObjectiveMan", "object:Objective"});
        manager(arguments[0]);
        const auto* handle = std::get_if<ObjectHandle>(&arguments[1]);
        if (!handle || handle->kind != ObjectHandle::Kind::objective)
            throw std::runtime_error(name + " expected a typed objective");
        // 003ddfc0 skips the native for a null declared object argument.
        if (handle->id == 0) return std::nullopt;
        if (!stage_.find(handle->id)) throw std::runtime_error(name + " on an objective outside the stage");
        const bool success = name == "Succeed";
        const auto& properties = host_.objectives.at(handle->id - 1).properties;
        const auto found = properties.find(success ? "SetCompleteString" : "SetFailString");
        std::optional<std::string> text;
        if (found != properties.end()) {
            if (found->second.size() != 1 || !std::holds_alternative<std::string>(found->second[0]))
                throw std::runtime_error(name + ": objective text is not a string");
            text = std::get<std::string>(found->second[0]);
        }
        outcomes_.push_back({handle->id, success,
                             apply_objective_outcome(stage_, handle->id, success, text, {inputs_.mission_cue, false})});
        // 00245400(threshold, 1) at 00226088 / 002260b0: the mission cue line, delivered at once.
        if (game_sound_) {
            for (const auto& effect : outcomes_.back().outcome.effects)
                if (const auto* a = std::get_if<ObjectiveAction>(&effect))
                    if (const auto line = mission_cue_line_name(a->mission, a->category, a->variant))
                        game_sound_->deliver(voice_line_message(*game_sound_, *line));
            outcomes_.back().voice_delivered = true;
        }
        return std::nullopt;
    }
    if (name == "IsComplete" || name == "WasFailed") {
        require_signature(script, function, 1, {"object:Objective"});
        const auto* handle = std::get_if<ObjectHandle>(&arguments[0]);
        if (!handle || handle->kind != ObjectHandle::Kind::objective)
            throw std::runtime_error(name + " expected a typed objective");
        // A typed null is the interpreter's skipped call: 0.
        if (handle->id == 0) return Word{0};
        const auto* object = stage_.find(handle->id);
        if (!object) throw std::runtime_error(name + " on an objective outside the stage");
        return Word(object->status == (name == "IsComplete" ? 1U : 2U));
    }
    if (name == "IsActive") {
        // IsActive(Objective) 002250b0 (132sx10, TOKYO): 00225790 (the
        // handle's objective), then lbu +25 (Activate / Deactivate / StartStage).
        require_signature(script, function, 1, {"object:Objective"});
        const auto* handle = std::get_if<ObjectHandle>(&arguments[0]);
        if (!handle || handle->kind != ObjectHandle::Kind::objective)
            throw std::runtime_error("IsActive expected a typed objective");
        if (handle->id == 0) return Word{0};   // the interpreter's skipped call
        const auto* object = stage_.find(handle->id);
        if (!object) throw std::runtime_error("IsActive on an objective outside the stage");
        return Word{object->active ? 1U : 0U};
    }
    if (name == "DestroyObjectives") {
        // DestroyObjectives(ObjectiveMan) 002257f0 -> 00227c60 (132sx11; the
        // CHECHNYA/ITALY/KYRGSTAN/MNTNEGR1 checkpoint ResetLevel routines):
        // every bucket's objects are destroyed and the counts zeroed; the ID
        // counter +a8 and the current stage are kept (ObjectiveStage::destroy_all).
        // The markers are not touched: 00229180 frees only the strings.
        require_signature(script, function, 4, {"object:ObjectiveMan"});
        manager(arguments[0]);
        stage_.destroy_all();
        for (auto& objective : host_.objectives) objective.destroyed = true;
        return std::nullopt;
    }
    if (name == "StartStage") {
        require_signature(script, function, 4, {"object:ObjectiveMan", "primitive:1"});
        manager(arguments[0]);
        stage_.start_stage(word_signed(number(arguments[1])));
        return std::nullopt;
    }
    if (name == "Activate" || name == "Deactivate") {
        require_signature(script, function, 4, {"object:Objective"});
        const auto* handle = std::get_if<ObjectHandle>(&arguments[0]);
        if (!handle || handle->kind != ObjectHandle::Kind::objective)
            throw std::runtime_error("Objective activation expected a typed objective");
        // A typed null is the interpreter's skipped call, not a native dereference.
        if (handle->id != 0) stage_.set_active(handle->id, name == "Activate");
        return std::nullopt;
    }
    if (name == "AddObjective") {
        if (arguments.size() != 2) throw std::runtime_error("AddObjective expects manager and stage");
        manager(arguments[0]);
        const auto id = stage_.add(word_signed(number(arguments[1])));
        if (!id) return Word{0};
        const auto result = host_.call(script, function, arguments);
        if (!result || !std::holds_alternative<Word>(*result) || std::get<Word>(*result) != id)
            throw std::runtime_error("Stage/configuration objective identities diverged");
        return result;
    }
    if (name == "game::GetString") {
        // ObjectiveHost checks the signature and records the call.
        auto result = host_.call(script, function, arguments);
        if (!inputs_.localization) return result;
        const auto& key = std::get<std::string>(result.value());
        const auto id = inputs_.localization->lookup(key);
        if (id < 0) throw std::runtime_error("game::GetString: key not in the loaded pools: " + key);
        std::string text(inputs_.localization->text(id).value());
        string_lookups_.push_back({key, id, text});
        return Value{std::move(text)};
    }
    return host_.call(script, function, arguments);
}

MissionScriptHost::MissionScriptHost(std::map<std::string, Script> modules, const Population& population,
                                     MissionScriptWorld& world, MissionObjectiveInputs objective_inputs)
    : modules_(std::move(modules)), objects_(population), world_(world), objectives_(std::move(objective_inputs)),
      to_hit_("ToHitSettings", to_hit_settings_defaults()),
      context_(*this, [this](const std::string& name) -> std::optional<Script> {
          const auto found = modules_.find(name);
          if (found == modules_.end()) return std::nullopt;
          return found->second;
      }) {
    if (population.npcs.empty()) throw std::runtime_error("Mission script host needs the player record");
    for (const auto& node : population.nodes) {
        nodes_.emplace(node.id, node);
        node_ids_.push_back(node.id);
    }
    for (const auto& object : population.objects) {
        if (object.kind == 5) interact_references_.emplace(object.id, object.references);
    }
    context_.define_engine_variable("game::ToHit", to_hit_.address());
    // Every other registered engine variable with its boot value (003d8a30;
    // engine_variables.hpp), e.g. game::DarknessDecreaser = 65.
    define_engine_variable_defaults(context_);
    // Checkpoint 132el6: the GOB loader 001c3510 (before the NPC loader) builds
    // each GOB through the GameGOBJ base 0021add0, which hands a record's
    // script name to 003d9d30 on its +18 holder (0021af60..0021affc; TOKYO's
    // lifts name ScriptElevator).
    for (const auto& object : population.objects)
        if (!object.script.empty()) { bind_entity_script(object.id, object.script); gob_entities_.insert(object.id); }
    // The population load (0013579c -> 00158bc0 -> 002300d0) binds each NPC's
    // entity script before 003e20a0 (0013588c) loads init.
    for (std::size_t i = 1; i < population.npcs.size(); ++i)
        if (!population.npcs[i].extra_name.empty()) bind_entity_script(population.npcs[i].id, population.npcs[i].extra_name);
}

void MissionScriptHost::bind_entity_script(Word npc, const std::string& name) {
    if (name.empty()) return;   // 003d9d30: strlen 0 binds nothing.
    // 003e15b0 (gate 004938d0 set): found -> holder +8 = type id, count = +48;
    // otherwise 003e1720 loads the module of that name and the lookup repeats.
    auto count = context_.script_type_members(name);
    if (!count && context_.load(name)) count = context_.script_type_members(name);
    // 003d9da0: the old storage is freed; count words zeroed, none for 0.
    auto& holder = entities_.holders[npc];
    holder.type = count ? name : std::string();
    holder.words.assign(count.value_or(0), Value(Word{0}));
}

Value& MissionScriptHost::EntityStorage::dereference(const Address& address) {
    const auto found = holders.find(static_cast<Word>(address.frame));
    if (found == holders.end() || address.slot < 0 || static_cast<std::size_t>(address.slot) >= found->second.words.size())
        throw std::runtime_error("Entity script storage word outside its holder");
    return found->second.words[static_cast<std::size_t>(address.slot)];
}

std::optional<Address> MissionScriptHost::entity_member(const Value& object, Word slot) {
    // 003de680 reads the holder at the object's +18: the AI component's
    // (002300d0), or a GOB record's (checkpoint 132el7: the GameGOBJ base
    // 0021add0 allocates a 12-byte holder at +18 (0021afa4, +0 = +4 = 0,
    // +8 = -1) and binds the record's script name into it with 003d9d30;
    // TOKYO's lifts, ScriptElevator). Other objects' +18 is not a script
    // holder here.
    const auto* handle = std::get_if<RuntimeObjectHandle>(&object);
    const bool gob = handle && handle->kind == RuntimeObjectHandle::Kind::world_object
                     && gob_entities_.contains(objects_.objects.id_word(*handle) & 0x7fffffffU);
    if (!handle || (handle->kind != RuntimeObjectHandle::Kind::ai && !gob))
        throw std::runtime_error("003de680: entity member of an object that is not an AI component or a scripted GOB record");
    const Word npc = objects_.objects.id_word(*handle) & 0x7fffffffU;
    const auto found = entities_.holders.find(npc);
    if (found == entities_.holders.end())
        throw std::runtime_error("003de680: NPC " + std::to_string(npc) + " has no entity script holder (003d9d30)");
    if (found->second.words.empty()) return std::nullopt;   // holder +0 null: 003d9d20 gives 0.
    if (slot >= found->second.words.size())
        throw std::runtime_error("003d9d20: entity slot outside NPC " + std::to_string(npc) + "'s storage");
    return Address{npc, static_cast<std::int64_t>(slot), &entities_};
}

std::optional<std::string> MissionScriptHost::entity_type(const Value& object) {
    // The holder +18 that 003de680 / 003de4f8 read (see entity_member).
    const auto* handle = std::get_if<RuntimeObjectHandle>(&object);
    const bool gob = handle && handle->kind == RuntimeObjectHandle::Kind::world_object
                     && gob_entities_.contains(objects_.objects.id_word(*handle) & 0x7fffffffU);
    if (!handle || (handle->kind != RuntimeObjectHandle::Kind::ai && !gob))
        throw std::runtime_error("003de4f8: script holder of an object that is not an AI component or a scripted GOB record");
    const Word id = objects_.objects.id_word(*handle) & 0x7fffffffU;
    const auto found = entities_.holders.find(id);
    if (found == entities_.holders.end()) {
        // Checkpoint 132zf12: an AI component gets its +18 holder only when its
        // actor's +3264 names a script (002300d0 returns before allocating it
        // otherwise; GENERATOR_RUNTIME.md), so +18 is null and the cast fails.
        if (handle->kind == RuntimeObjectHandle::Kind::ai) return std::nullopt;
        throw std::runtime_error("003de4f8: object " + std::to_string(id) + " has no entity script holder (003d9d30)");
    }
    return found->second.type;
}

void MissionScriptHost::start() {
    start_init();
    start_level_entries();
}

void MissionScriptHost::start_init() {
    if (init_run_ || started_) throw std::runtime_error("Mission scripts already started");
    stage_ = "start_level";
    if (!context_.start_level()) {
        std::string last = context_.log().empty() ? std::string("no log") : context_.log().back();
        throw std::runtime_error("Mission script level start failed: " + last);
    }
    init_run_ = true;
}

void MissionScriptHost::start_level_entries() {
    if (!init_run_) throw std::runtime_error("003e1a20 before the level load's 003e20a0 (init::Init)");
    if (started_) throw std::runtime_error("Mission scripts already started");
    for (const auto* entry : {"init::ClientStartLevel", "init::StartLevel"}) {
        stage_ = entry;
        // 003e1b60: when 003e0f50 finds no definition, 003e1bd4 prints 004bd920 through 003d7930 and returns 0.
        // 003e0f50 compares with 00128e80 (strcasecmp): LORELEI's init::startlevel
        // is its StartLevel (132ms2).
        const auto found = context_.entry_name(entry, 0);
        if (!found) {
            std::cout << "003e1b60: error!  function " << entry << " with 0 params not found!\n";
            continue;
        }
        context_.invoke(*found);
    }
    stage_ = "running";
    started_ = true;
}

void MissionScriptHost::invoke_function(const std::string& name) {
    if (!started_) throw std::runtime_error("Script call " + name + " before the mission scripts started");
    const StageScope scope(stage_, "harness call " + name);
    context_.invoke(name);
}

bool MissionScriptHost::deliver_hotbox(Word box, Word action) {
    if (!started_) throw std::runtime_error("HotboxMsg before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(box);
    if (label != "hotbox") throw std::runtime_error("HotboxMsg to a non-hotbox record: " + std::to_string(box));
    const StageScope scope(stage_, "HotboxMsg " + std::to_string(action) + " -> " + std::to_string(box));
    const auto player = objects_.player;
    ScriptContext::Message message{"HotboxMsg", {{"Action", Value(Word{action})},
                                                 {"Who", Value(objects_.objects.resolve(player | 0x80000000U))},
                                                 {"WhoPlayer", Value(objects_.objects.resolve(player))}}};
    return context_.deliver(receiver, std::move(message));
}

bool MissionScriptHost::deliver_trigger(Word id, Word action, std::optional<Word> who, Word data) {
    if (!started_) throw std::runtime_error("GOBJTriggerEventMsg before the mission scripts started");
    if (who && objects_.receiver(*who).first != "ai")
        throw std::runtime_error("GOBJTriggerEventMsg from a record that is not an NPC: " + std::to_string(*who));
    const auto [label, receiver] = objects_.receiver(id);
    const StageScope scope(stage_, "GOBJTriggerEventMsg " + std::to_string(action) + " -> " + std::to_string(id) + " (" + label + ")");
    const Word sender = who ? *who : objects_.player;
    // Data 00210c50: the +28 word (lw, not the +24 byte Action 00210c70 reads).
    ScriptContext::Message message{"GOBJTriggerEventMsg", {{"Action", Value(Word{action})},
                                                           {"Who", Value(objects_.objects.resolve(sender | 0x80000000U))},
                                                           {"Data", Value(Word{data})}}};
    return context_.deliver(receiver, std::move(message));
}

bool MissionScriptHost::trigger_has_handler(Word id) {
    if (!started_) throw std::runtime_error("GOBJTriggerEventMsg before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(id);
    static_cast<void>(label);
    return !context_.select_handlers(receiver, "GOBJTriggerEventMsg", 0).empty() ||
           !context_.select_handlers(receiver, "GOBJTriggerEventMsg", 1).empty();
}

std::optional<MissionScriptHost::InteractLinks> MissionScriptHost::interact_links(Word id) const {
    const auto references = interact_references_.find(id);
    if (references == interact_references_.end()) return std::nullopt;
    const auto& refs = references->second;
    if (refs.size() < 2)
        throw std::runtime_error("game::PerformAction_WithObject: InteractGOBJ with fewer than two references");
    auto key = [](const std::array<std::uint32_t, 2>& pair) {
        if (pair[0] > 0xffffU || pair[1] > 0xffffU)
            throw std::runtime_error("game::PerformAction_WithObject: unresolved InteractGOBJ reference");
        return static_cast<Word>((pair[0] << 16) | pair[1]);
    };
    const auto node_key = key(refs[refs.size() - 2]);
    const auto node = nodes_.find(node_key);
    if (node == nodes_.end())
        throw std::runtime_error("game::PerformAction_WithObject: InteractGOBJ +a8 is not a node: " + std::to_string(node_key));
    // +ac is often a camera record (keypad 140511: CAM p_cam_laptop 142763),
    // a section the population decoder does not keep; only its key is
    // passed (the world records the activation, it does not run it).
    // The GOB constructor stores every reference through 0040c480
    // (0021b3dc..0021b3f4, null included); 0040c480 gives 0 for a category
    // >= 0x100 (0040c530) or an index >= 0x10000 (0040c520), so a (2, -1)
    // reference leaves +ac = 0 and 0018fe68 pushes nothing (gear 140501).
    const auto& last = refs.back();
    InteractLinks links;
    links.world_object = last[0] >= 0x100U || last[1] >= 0x10000U ? 0U : key(last);
    const auto& p = node->second.position;
    links.node = {node_key, {p.x, p.y, p.z}, population_node_yaw(node->second)};
    return links;
}

bool MissionScriptHost::deliver_crate_interact(Word crate, std::int32_t taken, std::int32_t given, std::optional<Word> who) {
    if (!started_) throw std::runtime_error("CrateInteractMsg before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(crate);
    if (label != "backpack") throw std::runtime_error("CrateInteractMsg to a record that is not a backpack: " + std::to_string(crate));
    const StageScope scope(stage_, "CrateInteractMsg " + std::to_string(taken) + "/" + std::to_string(given) + " -> " +
                                       std::to_string(crate) + " (" + label + ")");
    ScriptContext::Message message{"CrateInteractMsg", {{"Taken", Value(Word{static_cast<Word>(taken)})},
                                                        {"Given", Value(Word{static_cast<Word>(given)})},
                                                        {"Who", Value(objects_.objects.resolve(who.value_or(objects_.player) | 0x80000000U))}}};
    return context_.deliver(receiver, std::move(message));
}

bool MissionScriptHost::deliver_inventory(Word backpack, std::int32_t item, std::int32_t count) {
    if (!started_) throw std::runtime_error("InventoryMsg before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(backpack);
    if (label != "backpack") throw std::runtime_error("InventoryMsg to a record that is not a backpack: " + std::to_string(backpack));
    const StageScope scope(stage_, "InventoryMsg " + std::to_string(item) + " x" + std::to_string(count) + " -> " +
                                       std::to_string(backpack) + " (" + label + ")");
    ScriptContext::Message message{"InventoryMsg", {{"Taken", Value(Word{static_cast<Word>(item)})},
                                                    {"Count", Value(Word{static_cast<Word>(count)})},
                                                    {"Who", Value(objects_.objects.resolve(objects_.player | 0x80000000U))}}};
    return context_.deliver(receiver, std::move(message));
}

void MissionScriptHost::register_runtime_backpack(Word id) {
    if (!objects_.objects.alive(objects_.objects.resolve(id | 0x80000000U)))
        throw std::runtime_error("A run-time backpack that is not a live world object: " + std::to_string(id));
    if (!objects_.sections.emplace(id, "backpack").second)
        throw std::runtime_error("A run-time backpack ID already in the population: " + std::to_string(id));
}

ItemInventory& MissionScriptHost::item_list(Word id) {
    if (id == objects_.player) return player_items_;
    // The play's actor keys are the +c words; a generator-spawned NPC's +c carries the high byte its script ID
    // lacks, and both name the one actor (one +3528 list).
    for (const auto& [npc, plus_c] : spawned_plus_c_)
        if (plus_c == id && plus_c != npc) return item_list(npc);
    const auto section = objects_.sections.find(id);
    if (section == objects_.sections.end() || section->second != "npc")
        throw std::runtime_error("Item list +3528 of a record that is not an actor: " + std::to_string(id));
    return npc_items_[id];
}

void MissionScriptHost::reset_item_list(Word id) {
    const auto section = objects_.sections.find(id);
    if (section == objects_.sections.end() || section->second != "npc")
        throw std::runtime_error("Item list +3528 reset of a record that is not an NPC: " + std::to_string(id));
    npc_items_.erase(id);
}

bool MissionScriptHost::deliver_ai_notification(Word npc, std::uint32_t key, std::int32_t node) {
    if (!started_) throw std::runtime_error("AI notification before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(npc);
    if (label != "ai") throw std::runtime_error("AI notification to a record without an AI component: " + std::to_string(npc));
    const char* type = key == 0x004ff268U ? "AIActivatedMsg" : key == 0x004ff288U ? "AIWaypointReachedMsg" :
                       key == 0x004ff290U ? "AIDestinationReachedMsg" : key == 0x004ff270U ? "AIDeactivatedMsg" : nullptr;
    if (!type) throw std::runtime_error("AI notification key not hosted");
    const StageScope scope(stage_, std::string(type) + " -> " + std::to_string(npc));
    ScriptContext::Message message{type, {}};
    if (key == 0x004ff268U) {
        // 0016f1c0: +24 = the local player component's actor (*004ffc0c +30).
        message.getters.emplace("Who", Value(objects_.objects.resolve(objects_.player | 0x80000000U)));
        message.getters.emplace("GetGOBJ", message.getters.at("Who"));   // GetGOBJ(AIGOBJMsg) 0026eff0: the same +24
    } else if (key == 0x004ff270U) {
        // 0016f0e0 does not write +24: Who(AIGOBJMsg) is not answered.
    } else {
        // 00151d30 path events 5 / 6: +24 = the node (null at a path end without one).
        Value value{RuntimeObjectHandle{}};
        if (node >= 0) {
            if (static_cast<std::size_t>(node) >= node_ids_.size())
                throw std::runtime_error("AI notification node outside the population's node list");
            value = objects_.objects.resolve(node_ids_[static_cast<std::size_t>(node)] | 0x80000000U);
        }
        message.getters.emplace("GetNode", std::move(value));
    }
    return context_.deliver(receiver, std::move(message));
}

bool MissionScriptHost::deliver_new_awareness(Word npc, std::optional<Word> who, std::uint8_t cause) {
    if (!started_) throw std::runtime_error("AI notification before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(npc);
    if (label != "ai") throw std::runtime_error("AI notification to a record without an AI component: " + std::to_string(npc));
    const Word object = who ? *who : objects_.player;
    const auto section = objects_.sections.find(object);
    if (section == objects_.sections.end() || section->second != "npc")
        throw std::runtime_error("AINewAwarenessMsg about a record that is not an actor: " + std::to_string(object));
    const StageScope scope(stage_, "AINewAwarenessMsg -> " + std::to_string(npc));
    // 0016cf10 (004ff298, vtable 004dcc90): +24 the object, read by Who (0026efe0).
    ScriptContext::Message message{"AINewAwarenessMsg", {}};
    message.getters.emplace("Who", Value(objects_.objects.resolve(object | 0x80000000U)));
    message.getters.emplace("Cause", Word{cause});   // 0026eee0: lbu +28 (Checkpoint 132sx8)
    message.getters.emplace("GetGOBJ", message.getters.at("Who"));   // GetGOBJ(AIGOBJMsg) 0026eff0: the same +24
    return context_.deliver(receiver, std::move(message));
}

bool MissionScriptHost::deliver_ai_weapon_fired(Word npc, bool has_target, std::optional<Word> target, bool thrown) {
    if (!started_) throw std::runtime_error("AI notification before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(npc);
    if (label != "ai") throw std::runtime_error("AI notification to a record without an AI component: " + std::to_string(npc));
    Value who{RuntimeObjectHandle{}};
    if (has_target) {
        const Word object = target ? *target : objects_.player;
        const auto section = objects_.sections.find(object);
        // An actor, or a node (SetTarget(NPC, Node) then ForceWeaponFire: the shot's target is the node object).
        if (section == objects_.sections.end() || (section->second != "npc" && section->second != "node"))
            throw std::runtime_error("AIWeaponFiredMsg target record that is neither an actor nor a node: " + std::to_string(object));
        who = objects_.objects.resolve(object | 0x80000000U);
    }
    const StageScope scope(stage_, "AIWeaponFiredMsg -> " + std::to_string(npc));
    // 0026f110 (004ff2a0, vtable 004dcbf0): +24 the target, read by Who (0026efe0).
    ScriptContext::Message message{"AIWeaponFiredMsg", {{"Who", who}, {"GetGOBJ", who}}};   // GetGOBJ 0026eff0: the same +24
    message.getters.emplace("IsThrownWeapon", Word{thrown ? 1U : 0U});   // 0026ee50: lbu +30
    return context_.deliver(receiver, std::move(message));
}

namespace {
// An Attacker (00408080: 003cc800(+28), the identity) the port can name: an actor, a FIR: zone (00261df0's
// damage), a population / event object (a GOB's blast, 0021a200 with the object as the source) or a node
// (a script CreateBlast 0021a170 on a Node GOBJ: STREETS OnDamageMsg #800 blasts at node 136626).
bool hosted_attacker_section(const std::string& section) {
    return section == "npc" || section == "fire" || section == "object" || section == "event_object" ||
           section == "event_light" || section == "node" || section == "tank";
}
}

bool MissionScriptHost::deliver_ai_dead(Word npc, bool has_attacker, std::optional<Word> attacker, std::uint8_t damage_type) {
    if (!started_) throw std::runtime_error("AI notification before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(npc);
    if (label != "ai") throw std::runtime_error("AI notification to a record without an AI component: " + std::to_string(npc));
    Value who{RuntimeObjectHandle{}};
    if (has_attacker) {
        const Word object = attacker ? *attacker : objects_.player;
        const auto section = objects_.sections.find(object);
        if (section == objects_.sections.end() || !hosted_attacker_section(section->second))
            throw std::runtime_error("AIDeadMsg attacker record that is not hosted: " + std::to_string(object));
        who = objects_.objects.resolve(object | 0x80000000U);
    }
    const StageScope scope(stage_, "AIDeadMsg -> " + std::to_string(npc));
    ScriptContext::Message message{"AIDeadMsg", {{"Attacker", std::move(who)}, {"DamageType", Value(Word{damage_type})}}};
    return context_.deliver(receiver, std::move(message));
}

bool MissionScriptHost::deliver_ai_dead_by_unnamed(Word npc, std::uint8_t damage_type) {
    if (!started_) throw std::runtime_error("AI notification before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(npc);
    if (label != "ai") throw std::runtime_error("AI notification to a record without an AI component: " + std::to_string(npc));
    const StageScope scope(stage_, "AIDeadMsg -> " + std::to_string(npc));
    ScriptContext::Message message{"AIDeadMsg", {{"DamageType", Value(Word{damage_type})}}};
    return context_.deliver(receiver, std::move(message));
}

bool MissionScriptHost::deliver_ai_unconscious(Word npc, bool has_who, std::optional<Word> who) {
    if (!started_) throw std::runtime_error("AI notification before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(npc);
    if (label != "ai") throw std::runtime_error("AI notification to a record without an AI component: " + std::to_string(npc));
    Value value{RuntimeObjectHandle{}};
    if (has_who) {
        const Word object = who ? *who : objects_.player;
        const auto section = objects_.sections.find(object);
        if (section == objects_.sections.end() || section->second != "npc")
            throw std::runtime_error("AIUnconsciousMsg source record that is not an actor: " + std::to_string(object));
        value = objects_.objects.resolve(object | 0x80000000U);
    }
    const StageScope scope(stage_, "AIUnconsciousMsg -> " + std::to_string(npc));
    ScriptContext::Message message{"AIUnconsciousMsg", {{"Who", std::move(value)}}};
    return context_.deliver(receiver, std::move(message));
}

bool MissionScriptHost::deliver_ai_pickupable_awareness(Word npc, std::optional<Word> who) {
    if (!started_) throw std::runtime_error("AI notification before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(npc);
    if (label != "ai") throw std::runtime_error("AI notification to a record without an AI component: " + std::to_string(npc));
    const Word object = who ? *who : objects_.player;
    const auto section = objects_.sections.find(object);
    if (section == objects_.sections.end() || section->second != "npc")
        throw std::runtime_error("AIPickupableAwarenessMsg about a record that is not an actor: " + std::to_string(object));
    const StageScope scope(stage_, "AIPickupableAwarenessMsg -> " + std::to_string(npc));
    // 0016cd7c (004ff2a8, vtable 004dcbd0): +24 the dead object, read by Who (0026efe0).
    ScriptContext::Message message{"AIPickupableAwarenessMsg", {}};
    message.getters.emplace("Who", Value(objects_.objects.resolve(object | 0x80000000U)));
    message.getters.emplace("GetGOBJ", message.getters.at("Who"));   // GetGOBJ(AIGOBJMsg) 0026eff0: the same +24
    return context_.deliver(receiver, std::move(message));
}

bool MissionScriptHost::deliver_ai_body_moved(Word npc, std::optional<Word> who, bool picked_up) {
    if (!started_) throw std::runtime_error("AI notification before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(npc);
    if (label != "ai") throw std::runtime_error("AI notification to a record without an AI component: " + std::to_string(npc));
    const Word object = who ? *who : objects_.player;
    const auto section = objects_.sections.find(object);
    if (section == objects_.sections.end() || section->second != "npc")
        throw std::runtime_error("AIBodyMovedMsg carrier record that is not an actor: " + std::to_string(object));
    const StageScope scope(stage_, "AIBodyMovedMsg " + std::to_string(picked_up ? 1 : 0) + " -> " + std::to_string(npc));
    // 0026edb0: +24 through 003cc800; 0026ed90: lbu +28.
    ScriptContext::Message message{"AIBodyMovedMsg", {}};
    message.getters.emplace("Who", Value(objects_.objects.resolve(object | 0x80000000U)));
    message.getters.emplace("PickedUp", Value(Word{picked_up ? 1U : 0U}));
    return context_.deliver(receiver, std::move(message));
}

bool MissionScriptHost::deliver_ai_disguise_taken(Word npc, std::optional<Word> who) {
    if (!started_) throw std::runtime_error("AI notification before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(npc);
    if (label != "ai") throw std::runtime_error("AI notification to a record without an AI component: " + std::to_string(npc));
    const Word object = who ? *who : objects_.player;
    const auto section = objects_.sections.find(object);
    if (section == objects_.sections.end() || section->second != "npc")
        throw std::runtime_error("AIDisguiseTakenMsg taker record that is not an actor: " + std::to_string(object));
    const StageScope scope(stage_, "AIDisguiseTakenMsg -> " + std::to_string(npc));
    // 00252300..00252318 (004ff2b8, vtable 004dcc50): +24 the taker, read by Who (0026efe0).
    ScriptContext::Message message{"AIDisguiseTakenMsg", {}};
    message.getters.emplace("Who", Value(objects_.objects.resolve(object | 0x80000000U)));
    return context_.deliver(receiver, std::move(message));
}

bool MissionScriptHost::deliver_ai_mode_change(Word npc, std::uint8_t mode) {
    if (!started_) throw std::runtime_error("AI notification before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(npc);
    if (label != "ai") throw std::runtime_error("AI notification to a record without an AI component: " + std::to_string(npc));
    const StageScope scope(stage_, "AIModeChangeMsg " + std::to_string(mode) + " -> " + std::to_string(npc));
    // GetAiMode 0014a6e0: lw v0,(a0); lbu v0,0x24(v0), the message's new mode.
    ScriptContext::Message message{"AIModeChangeMsg", {}};
    message.getters.emplace("GetAiMode", Value(Word{mode}));
    return context_.deliver(receiver, std::move(message));
}

bool MissionScriptHost::deliver_pathed_notice(Word object, const ObjectMessage& notice) {
    if (!started_) throw std::runtime_error("PathedNotice before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(object);
    if (label != "object/PathedGOBJ") throw std::runtime_error("PathedNotice to a non-PathedGOBJ record: " + std::to_string(object));
    if (notice.kind == ObjectMessage::Kind::trigger) {
        // Trigger(PathedGOBJ) 00211350's GOBJTriggerEventMsg: action +24 = 0, +28 = 0 and the sender key +30 = 0,
        // so Who answers a null GOBJ and Data 0. An InteractGOBJ's 0xe forward (002166a8, checkpoint 132ia1) sends
        // its cloned Triangle trigger: Action 00210c70 the +24 byte, Data 00210c50 the +28 word, Who the +30 agent.
        if (notice.actor_30 > 0xffffffffULL) throw std::runtime_error("Pathed object trigger from an unkeyed agent");
        const auto actor = static_cast<Word>(notice.actor_30);
        if (actor && actor != objects_.player && objects_.receiver(actor).first != "ai")
            throw std::runtime_error("Pathed object trigger from a record that is not an NPC: " + std::to_string(actor));
        const StageScope scope(stage_, "GOBJTriggerEventMsg " + std::to_string(notice.action) + " -> " + std::to_string(object) + " (" + label + ")");
        const Value who = actor ? Value(objects_.objects.resolve(actor | 0x80000000U)) : Value(RuntimeObjectHandle{});
        ScriptContext::Message message{"GOBJTriggerEventMsg", {{"Action", Value(Word{static_cast<Word>(notice.action) & 0xffU})},
                                                               {"Who", who}, {"Data", Value(Word{notice.word_28})}}};
        return context_.deliver(receiver, std::move(message));
    }
    if (notice.kind != ObjectMessage::Kind::pathed_notice)
        throw std::runtime_error("Pathed object message other than a PathedNotice or a trigger offered to the scripts");
    const StageScope scope(stage_, "PathedNotice " + std::to_string(notice.action) + " -> " + std::to_string(object));
    // Action 00210b30 reads the +24 byte, Data 00210b10 the +28 word: the waypoint (-1 for Trigger's Moving).
    return context_.deliver(receiver, ScriptContext::Message{"PathedNotice", {{"Action", Value(Word{static_cast<Word>(notice.action) & 0xffU})},
                                                                              {"Data", Value(Word{static_cast<Word>(notice.waypoint)})}}});
}

bool MissionScriptHost::deliver_lift_notice(Word object, bool elevator_notice, std::int32_t action, std::int32_t data) {
    const char* type = elevator_notice ? "ElevatorNotice" : "PathedNotice";
    if (!started_) throw std::runtime_error(std::string(type) + " before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(object);
    if (label != "object/ElevatorGOBJ")
        throw std::runtime_error(std::string(type) + " from a lift to a non-ElevatorGOBJ record: " + std::to_string(object));
    const StageScope scope(stage_, std::string(type) + " " + std::to_string(action) + " -> " + std::to_string(object));
    ScriptContext::Message message{type, {}};
    message.getters.emplace("Action", Value(Word{static_cast<std::uint32_t>(action) & 0xffU}));
    if (!elevator_notice) message.getters.emplace("Data", Value(static_cast<Word>(static_cast<std::uint32_t>(data))));
    return context_.deliver(receiver, std::move(message));
}

bool MissionScriptHost::deliver_ni_notice(Word object, std::int32_t value_24) {
    if (!started_) throw std::runtime_error("NINotice before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(object);
    if (label != "event_object") throw std::runtime_error("NINotice to a record that is not an NIE: object: " + std::to_string(object));
    const StageScope scope(stage_, "NINotice -> " + std::to_string(object));
    return context_.deliver(receiver, ScriptContext::Message{"NINotice",
        {{"Action", Value(Word{static_cast<std::uint32_t>(value_24) & 0xffU})}}});   // 00237500: lbu +24
}

bool MissionScriptHost::deliver_beam(Word object) {
    if (!started_) throw std::runtime_error("BeamMsg before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(object);
    if (label != "event_object") throw std::runtime_error("BeamMsg to a record that is not an NIE: object: " + std::to_string(object));
    const StageScope scope(stage_, "BeamMsg -> " + std::to_string(object));
    // 0023af48: +24 = the controller's actor (slot 0: the local player).
    return context_.deliver(receiver, ScriptContext::Message{"BeamMsg", {{"Who", Value(objects_.objects.resolve(objects_.player | 0x80000000U))}}});
}

bool MissionScriptHost::deliver_spawned_npc(Word generator, std::optional<Word> npc) {
    if (!started_) throw std::runtime_error("SpawnedNPCMsg before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(generator);
    if (label != "generator") throw std::runtime_error("SpawnedNPCMsg to a record that is not a generator: " + std::to_string(generator));
    Value who{RuntimeObjectHandle{}};
    if (npc) {
        const auto section = objects_.sections.find(*npc);
        if (section == objects_.sections.end() || section->second != "npc")
            throw std::runtime_error("SpawnedNPCMsg about a record that is not an NPC: " + std::to_string(*npc));
        who = objects_.objects.resolve(*npc);
    }
    const StageScope scope(stage_, "SpawnedNPCMsg -> " + std::to_string(generator));
    ScriptContext::Message message{"SpawnedNPCMsg", {}};
    message.getters.emplace("GetNPC", std::move(who));
    return context_.deliver(receiver, std::move(message));
}

void MissionScriptHost::register_spawned_npc(Word npc, std::optional<Word> plus_c) {
    objects_.add_spawned_npc(npc);
    if (plus_c) spawned_plus_c_[npc] = *plus_c;
}
void MissionScriptHost::flush_spawned_npc(Word npc) { objects_.flush_spawned_npc(npc); }
void MissionScriptHost::delete_spawned_npc(Word npc) {
    objects_.delete_spawned_npc(npc);
    spawned_plus_c_.erase(npc);
    entities_.holders.erase(npc);
}
void MissionScriptHost::drop_spawned_npc_from_groups(Word npc) {
    const auto section = objects_.sections.find(npc);
    if (section == objects_.sections.end() || section->second != "npc")
        throw std::runtime_error("003d9f40 for a record that is not an NPC: " + std::to_string(npc));
    const auto actor = objects_.objects.resolve(npc | 0x80000000U);
    const auto component = objects_.objects.resolve(npc);
    if (!objects_.objects.alive(actor) || !objects_.objects.alive(component) || component.kind != RuntimeObjectHandle::Kind::ai)
        throw std::runtime_error("003d9f40: NPC " + std::to_string(npc) + " has no live actor and AI component");
    context_.drop_from_groups(Value{actor}, Value{component});
}
bool MissionScriptHost::deliver_link_message(Word npc, bool enable) {
    if (!started_) throw std::runtime_error("Hotbox link message before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(npc);
    if (label != "ai") throw std::runtime_error("Hotbox link message to a record without an AI component: " + std::to_string(npc));
    const char* type = enable ? "EnableMsg" : "DisableMsg";
    const StageScope scope(stage_, std::string(type) + " -> " + std::to_string(npc));
    return context_.deliver(receiver, ScriptContext::Message{type, {}});
}

bool MissionScriptHost::deliver_generator_link_message(Word generator, bool enable) {
    if (!started_) throw std::runtime_error("Hotbox link message before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(generator);
    if (label != "generator") throw std::runtime_error("Hotbox link message to a record that is not a generator: " + std::to_string(generator));
    const char* type = enable ? "EnableMsg" : "DisableMsg";
    const StageScope scope(stage_, std::string(type) + " -> " + std::to_string(generator));
    return context_.deliver(receiver, ScriptContext::Message{type, {}});
}

bool MissionScriptHost::deliver_timer_expired(std::int32_t value) {
    if (!started_) throw std::runtime_error("TimerExpiredMsg before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(objects_.player);
    if (label != "player") throw std::runtime_error("TimerExpiredMsg: the player record is not the player controller");
    const StageScope scope(stage_, "TimerExpiredMsg -> " + std::to_string(objects_.player));
    // 00244fa0's cTimerExpiredMsg (key 004f7a08, vtable 004dba00): byte +20 the kind, word +24 the value.
    // IsPersonal 0022f1b0: sltu(0, word +24) - 1 for the personal timer +6ac (value 1), 0 for +6c4.
    return context_.deliver(receiver, ScriptContext::Message{"TimerExpiredMsg", {{"IsPersonal", Value(Word{value != 0 ? 1U : 0U})}}});
}

bool MissionScriptHost::deliver_out_of_ammo() {
    if (!started_) throw std::runtime_error("OutOfAmmoMsg before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(objects_.player);
    if (label != "player") throw std::runtime_error("OutOfAmmoMsg: the player record is not the player controller");
    const StageScope scope(stage_, "OutOfAmmoMsg -> " + std::to_string(objects_.player));
    return context_.deliver(receiver, ScriptContext::Message{"OutOfAmmoMsg", {}});
}

bool MissionScriptHost::deliver_weapon_vis_change(std::int32_t value) {
    if (!started_) throw std::runtime_error("WeaponVisChangeMsg before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(objects_.player);
    if (label != "player") throw std::runtime_error("WeaponVisChangeMsg: the player record is not the player controller");
    const StageScope scope(stage_, "WeaponVisChangeMsg -> " + std::to_string(objects_.player));
    // 00144270..00144314: byte +20 = 2, word +24 the value; Visibility 0043fb40 returns word +24.
    return context_.deliver(receiver, ScriptContext::Message{"WeaponVisChangeMsg",
                                                             {{"Visibility", Value(static_cast<Word>(value))}}});
}

bool MissionScriptHost::deliver_notify_checkpoint(std::optional<Word> actor) {
    if (!started_) throw std::runtime_error("NotifyCheckpointMsg before the mission scripts started");
    const Word id = actor ? *actor : objects_.player;
    const auto [label, receiver] = objects_.receiver(id);
    if (actor ? (label != "player" && label != "ai") : label != "player")
        throw std::runtime_error("NotifyCheckpointMsg to a record that is not an actor controller: " + std::to_string(id));
    const StageScope scope(stage_, "NotifyCheckpointMsg -> " + std::to_string(id));
    // 00189dd8: cNotifyCheckpointMsg (key 004f7938, vtable 004db8b0) has no getters.
    return context_.deliver(receiver, ScriptContext::Message{"NotifyCheckpointMsg", {}});
}

bool MissionScriptHost::deliver_damage(std::optional<Word> actor, std::optional<Word> attacker, std::int32_t damage,
                                       std::uint8_t type, std::uint8_t where) {
    if (!started_) throw std::runtime_error("DamageMsg before the mission scripts started");
    const Word id = actor ? *actor : objects_.player;
    const auto [label, receiver] = objects_.receiver(id);
    if (actor ? (label != "player" && label != "ai") : label != "player")
        throw std::runtime_error("DamageMsg to a record that is not an actor controller (props are not hosted): " +
                                 std::to_string(id));
    const Word source = attacker ? *attacker : objects_.player;
    // A FIR: zone's damage (00261df0 -> 003ccb00) has the zone as its source:
    // Attacker (00408080) is the +28 object itself, the FireObj.
    const auto section = objects_.sections.find(source);
    if (section == objects_.sections.end() || !hosted_attacker_section(section->second))
        throw std::runtime_error("DamageMsg attacker record that is not hosted: " + std::to_string(source));
    const StageScope scope(stage_, "DamageMsg -> " + std::to_string(id));
    // 003ccb00's message (key 005716f0, vtable 004e08a0): Damage 00408090 (+24),
    // Attacker 00408080 (003cc800(+28), the identity), Where 00408070 (lbu
    // +2d) and Type 00408060 (lbu +2c).
    ScriptContext::Message message{"DamageMsg",
                                   {{"Damage", Value(static_cast<Word>(damage))},
                                    {"Attacker", Value(objects_.objects.resolve(source | 0x80000000U))},
                                    {"Where", Value(Word{where})},
                                    {"Type", Value(Word{type})}}};
    return context_.deliver(receiver, std::move(message));
}

bool MissionScriptHost::deliver_damage_from_unnamed(std::optional<Word> actor, std::int32_t damage, std::uint8_t type,
                                                    std::uint8_t where) {
    if (!started_) throw std::runtime_error("DamageMsg before the mission scripts started");
    const Word id = actor ? *actor : objects_.player;
    const auto [label, receiver] = objects_.receiver(id);
    if (actor ? (label != "player" && label != "ai") : label != "player")
        throw std::runtime_error("DamageMsg to a record that is not an actor controller (props are not hosted): " +
                                 std::to_string(id));
    const StageScope scope(stage_, "DamageMsg -> " + std::to_string(id));
    ScriptContext::Message message{"DamageMsg",
                                   {{"Damage", Value(static_cast<Word>(damage))},
                                    {"Where", Value(Word{where})},
                                    {"Type", Value(Word{type})}}};
    return context_.deliver(receiver, std::move(message));
}

bool MissionScriptHost::deliver_object_damage(Word object, std::optional<Word> attacker, std::int32_t damage,
                                              std::uint8_t type, std::uint8_t where) {
    if (!started_) throw std::runtime_error("DamageMsg before the mission scripts started");
    const auto [label, receiver] = objects_.receiver(object);
    if (!label.starts_with("object/") && label != "event_object" && label != "event_light")
        throw std::runtime_error("Object DamageMsg to a record that is not a GOB:/NIE:/NIL: object: " + std::to_string(object));
    const Word source = attacker ? *attacker : objects_.player;
    const auto section = objects_.sections.find(source);
    if (section == objects_.sections.end() || !hosted_attacker_section(section->second))
        throw std::runtime_error("Object DamageMsg attacker record that is not hosted: " + std::to_string(source));
    const StageScope scope(stage_, "DamageMsg -> " + std::to_string(object));
    ScriptContext::Message message{"DamageMsg",
                                   {{"Damage", Value(static_cast<Word>(damage))},
                                    {"Attacker", Value(objects_.objects.resolve(source | 0x80000000U))},
                                    {"Where", Value(Word{where})},
                                    {"Type", Value(Word{type})}}};
    return context_.deliver(receiver, std::move(message));
}

std::size_t MissionScriptHost::pump(std::int32_t milliseconds) {
    if (!started_) throw std::runtime_error("Script event pump before the mission scripts started");
    const StageScope scope(stage_, "event pump");
    return context_.schedules().advance(milliseconds);
}

std::size_t MissionScriptHost::deferred() const {
    std::size_t count = 0;
    for (const auto& call : calls_) count += call.applied ? 0 : 1;
    return count;
}

bool partner_objective_multiplayer_site(const std::string& module, const std::string& function, std::size_t word) {
    // TORONTO1: OBJECTIVES CreateObjectives creates Obj_laptop (obj3) only when IsMultiplayer;
    // AddObjective_Laptop shows 'obj3_add' only then; STREETS AddLaptop adds it after the water main only then.
    // BELARUS1 (132zf13; the level's five game::IsMultiplayer calls, all on the multiplayer worker objective):
    // OBJECTIVES CreateObjectives word 867 creates Obj_SaveAllWorkers ('Obj6_Name', escort all seven workers);
    // MILL CheckAddWorker word 3212 shows 'Obj6_Add'; WorkerFinished word 4614 counts it down for a saved
    // worker (Obj_Update_SaveAllWorkers(workerCountAll - workerSavedCount)), word 4671 fails it for a dead one,
    // word 4726 completes it once workerSavedCount is 7.
    // BELARUS2 (132ms5; the level's four game::IsMultiplayer calls, three on the bridge objective): OBJECTIVES
    // CreateObjectives word 1158 creates Obj_Bridge ('Obj11_name'); BELARUS2 AddBridgeObjective word 9456 adds it
    // ('Obj11_Add', VO 5_6_2 for 5_6_3, map 536183); Tank2Chateau word 9128 schedules its FAIL in 30 s. Not the
    // solo-only helicopter branch of OnGOBJTriggerEventMsg word 7287.
    // UKRAINE (132uk6c; research/UKRAINE_COOP.md "Checkpoint 132uk6c"): OBJECTIVES CreateObjectives word 198 creates
    // Obj_DestroyMissile ('Stop Ivankov's missile from launching (Team)'); BASE LaunchMissile word 19146 fails it at the
    // launch. Not the VO site OnAIDestinationReachedMSG word 17791.
    struct Site {
        const char* module;
        const char* function;
        std::size_t word;
    };
    static constexpr Site sites[] = {{"objectives", "objectives::CreateObjectives", 318},
                                     {"objectives", "objectives::AddObjective_Laptop", 1296},
                                     {"streets", "streets::AddLaptop", 1300},
                                     {"objectives", "objectives::CreateObjectives", 867},
                                     {"mill", "mill::CheckAddWorker", 3212},
                                     {"mill", "mill::WorkerFinished", 4614},
                                     {"mill", "mill::WorkerFinished", 4671},
                                     {"mill", "mill::WorkerFinished", 4726},
                                     {"objectives", "objectives::CreateObjectives", 1158},
                                     {"belarus2", "belarus2::AddBridgeObjective", 9456},
                                     {"belarus2", "belarus2::Tank2Chateau", 9128},
                                     {"objectives", "objectives::CreateObjectives", 198},
                                     {"base", "base::LaunchMissile", 19146}};
    for (const auto& site : sites)
        if (module == site.module && function == site.function && word == site.word) return true;
    return false;
}

std::optional<Value> MissionScriptHost::hosted_call(const Script& script, const Function& function,
                                                    const std::vector<Value>& arguments, bool& applied) {
    const auto& name = function.name;
    applied = true;
    if (name == "game::IsMultiplayer" && partner_objectives_) {
        // Port-only (132ob8): the partner is the second player at the allow-listed objective call sites only.
        const auto site = context_.native_call_site(script);
        MultiplayerAnswer answer;
        if (site) answer = {site->module, site->function, site->word, false};
        answer.answer = site && partner_objective_multiplayer_site(site->module, site->function, site->word);
        multiplayer_answers_.push_back(answer);
        objectives_.set_multiplayer_answer(answer.answer);
        struct Reset {
            ObjectiveNatives& natives;
            ~Reset() { natives.set_multiplayer_answer(false); }
        } reset{objectives_};
        return objectives_.call(script, function, arguments);
    }
    if (ObjectiveNatives::hosts(script, function)) {
        const std::size_t before = objectives_.outcomes().size();
        auto result = objectives_.call(script, function, arguments);
        // Checkpoint 132f10: Succeed's 00228d50 (state 1) hands the complete
        // string (+78) and Fail's 00228dc0 (state 2) the fail string (+80, only
        // when set) to 00227c50 = j 00242b00(*004ffc2c, text), the HUD ticker
        // notice (solo: 002260f0 / 00225fa0 with 005721c8 clear).
        for (std::size_t i = before; hud_object_ && i < objectives_.outcomes().size(); ++i) {
            for (const auto& effect : objectives_.outcomes()[i].outcome.effects)
                if (const auto* note = std::get_if<ObjectiveNotification>(&effect)) hud_object_->notice(note->text);
            objectives_.mark_notice_delivered(i);
        }
        return result;
    }
    if (name == "game::AddObjective") {
        applied = objectives_.add_progression(script, function, arguments);
        return std::nullopt;
    }
    if (name == "SetMenuLabel") {
        applied = objectives_.set_menu_label(script, function, arguments);
        return std::nullopt;
    }
    if (name == "SetMapLabel" || name == "SetAddLabel" || name == "SetCompleteLabel" || name == "SetFailLabel") {
        applied = objectives_.set_label(script, function, arguments, name);
        return std::nullopt;
    }
    if (name == "SetDisplayLabel") {
        applied = objectives_.display_label(script, function, arguments);
        // Solo (005721c8 clear): 0x00227c18 hands the label 003fdb50 resolved
        // to 00242b00(*004ffc2c, text), the HUD ticker notice (null skipped).
        if (applied && hud_object_) hud_object_->notice(objectives_.display_labels().back().text);
        return std::nullopt;
    }
    if (name == "SetDisplayString") {
        applied = objectives_.display_string(script, function, arguments);
        if (applied && hud_object_) hud_object_->notice(objectives_.display_strings().back());
        return std::nullopt;
    }
    if (name == "game::AddHudObjective" || name == "game::RemoveHudObjective") {
        const bool add = name == "game::AddHudObjective";
        if (add)
            require_signature(script, function, 4, function.parameters.size() == 3 ?
                std::vector<std::string>{"object:GOBJ", "primitive:3", "primitive:3"} :
                std::vector<std::string>{"object:GOBJ", "primitive:3"});
        else
            require_signature(script, function, 4, function.parameters.size() == 2 ?
                std::vector<std::string>{"object:GOBJ", "primitive:1"} : std::vector<std::string>{"object:GOBJ"});
        // 003cc820 then the record's +c key: the population ID.
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + " without a live world object");
        if (!add) {
            applied = objectives_.remove_hud_objective(handle->id);
            return std::nullopt;
        }
        const auto* key = std::get_if<std::string>(&arguments.at(1));
        if (!key) throw std::runtime_error(name + " without a label string");
        std::optional<std::string> cue;
        if (arguments.size() == 3) {
            const auto* text = std::get_if<std::string>(&arguments[2]);
            if (!text) throw std::runtime_error(name + " without a cue string");
            cue = *text;
        }
        applied = objectives_.add_hud_objective(handle->id, *key, std::move(cue));
        return std::nullopt;
    }
    // SetMapObject(Objective, GOBJ) 00225000: 00225790 (the handle's
    // objective), 003cc820 (the GOBJ), then 00228190(objective, GOBJ, 0, 0):
    // nothing while the objective's map object +34 is live ((+34) +c == +38)
    // or for a null GOBJ; else +34 = the GOBJ, +38 = its +c and 00273210(hud +
    // 90, &GOBJ +c, objective, NULL) files a kind-1 marker (add_map).
    // ClearMapObject(Objective) 00224f70 -> 00228050(objective, 0): a live map
    // object's marker goes (00273200 = j 00272d50(hud + 90, &id, 1)), then +34
    // = 0, +38 = -1. Solo (005721c8 clear) skips both multiplayer sends; both
    // return 0. The markers are logical, as for AddHudObjective; the host runs
    // their 00273b40 update (ObjectiveMarkers::update, checkpoint 132hud3) and
    // draws them on the map / radar.
    // Checkpoint 132el8: the cue overload SetMapObject(Objective, GOBJ,
    // string) 00224fa0 is the same with a2 = *(args + 8), the string, which
    // 00228190 passes to 00273210 as the record's +28 cue (map_object_set;
    // EE goldens tests/map_object_goldens.hpp). TOKYO's objectives::ObjMap.
    if (name == "SetMapObject" || name == "ClearMapObject") {
        const bool set = name == "SetMapObject";
        const bool cue_form = set && function.parameters.size() == 3;
        require_signature(script, function, 4,
                          cue_form ? std::vector<std::string>{"object:Objective", "object:GOBJ", "primitive:7"} :
                          set ? std::vector<std::string>{"object:Objective", "object:GOBJ"} :
                                std::vector<std::string>{"object:Objective"});
        const auto objective = objectives_.map_objective(arguments.at(0), name);
        if (objective == 0) return std::nullopt;
        const auto current = map_objects_.find(objective);
        const bool live = current != map_objects_.end() && objects_.objects.alive(current->second);
        if (!set) {
            if (live) objectives_.remove_map_marker(current->second.id);
            map_objects_.erase(objective);
            return std::nullopt;
        }
        std::optional<std::string> cue;
        if (cue_form) {
            const auto* text = std::get_if<std::string>(&arguments.at(2));
            if (!text) throw std::runtime_error(name + " without a cue string");
            cue = *text;
        }
        if (live || null_handle(arguments.at(1))) return std::nullopt;
        const auto* gobj = std::get_if<RuntimeObjectHandle>(&arguments.at(1));
        if (!gobj || gobj->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*gobj))
            throw std::runtime_error(name + " without a live world object");
        if (objectives_.set_map_object(false, gobj->id, objective, cue)) map_objects_[objective] = *gobj;
        return std::nullopt;
    }
    // HUD callouts (hud_script_state.hpp). The GOBJ forms 00268360 /
    // 00268270 / 00267fe0 convert argument 0 with 003cc820 and pass &(GOBJ
    // +c); the NPC forms 00268090 / 00267f90 convert with 0014a690 and pass
    // &((AI +30 owner) +c). The NPC SetCallout 00268180 (0014a690, then
    // 002681d0) is the GOBJ form's 002683b0 with key = (AI +30) +c (132zf5:
    // SEWER's OnAIDeadMsg for the viral NPCs). SetCallout 002683b0 -> 00242fa0(*004ffc2c, key,
    // text, a3 0, t0 0, t1 0, f12 (float)int (cvt.s.w), f13 0);
    // SetCalloutLabel 002682c0 / 002680e0 -> 00242e40(..., t2 1), which
    // resolves the label with 003fdb50 and does nothing when it is null, then
    // 00242fa0 (its message only in multiplayer); ClearCallout 00268010 /
    // 00267fc0 -> 00242b40(*004ffc2c, key, 0, 0, t0 1). All return 0 (void).
    // Checkpoint 132hud4: the four-argument forms SetCallout(GOBJ) 00268300
    // (-> 002683b0), SetCallout(NPC) 00268120 (-> 002681d0) and
    // SetCalloutLabel(GOBJ) 00268210 (-> 002682c0) / (NPC) 00268030 (->
    // 002680e0) pass t1 = (argument 3 != 0) (sltu), the hscallout_bottom
    // callout; the three-argument forms pass t1 0.
    // The label form is deferred without the localization.
    if (((name == "game::SetCallout" || name == "game::SetCalloutLabel") &&
         (function.parameters.size() == 3 || function.parameters.size() == 4)) ||
        (name == "game::ClearCallout" && function.parameters.size() == 1)) {
        const bool clear = name == "game::ClearCallout";
        const bool label = name == "game::SetCalloutLabel";
        const bool bottom_form = function.parameters.size() == 4;
        const bool npc = !function.parameters.empty() && script.type_key(function.parameters[0]) == "object:NPC";
        const std::string object_type = npc ? "object:NPC" : "object:GOBJ";
        if (clear)
            require_signature(script, function, 4, {object_type});
        else if (label && bottom_form)
            require_signature(script, function, 4, {object_type, "primitive:7", "primitive:1", "primitive:1"});
        else if (label)
            require_signature(script, function, 4, {object_type, "primitive:7", "primitive:1"});
        else if (bottom_form)
            require_signature(script, function, 4, {object_type, "primitive:3", "primitive:1", "primitive:1"});
        else
            require_signature(script, function, 4, {object_type, "primitive:3", "primitive:1"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + " without a live object");
        Word key = 0;
        if (npc) {
            if (handle->kind != RuntimeObjectHandle::Kind::ai)
                throw std::runtime_error(name + "(NPC) without an AI controller handle");
            key = objects_.objects.get_gobj(*handle).id;   // (AI +30) +c
        } else {
            if (handle->kind != RuntimeObjectHandle::Kind::world_object)
                throw std::runtime_error(name + "(GOBJ) without a world object");
            key = handle->id;                              // GOBJ +c
        }
        // A generator-spawned actor's +c carries the high byte of 001b8068..
        // 001b8078 (and 0040b790 registers it under that word, 0021e8a4).
        if (const auto spawned = spawned_plus_c_.find(key); spawned != spawned_plus_c_.end()) key = spawned->second;
        // With a hosted HUD (hud_object.hpp, hud_callouts.hpp) the calls also
        // reach 00242fa0 / 00242b40 on it.
        const bool hosted = hud_object_ && hud_object_->callouts_hosted;
        if (clear) {
            hud_.clear_callout(key, 0);
            if (hosted) hud_object_->callout_clear(static_cast<std::int32_t>(key), 0, false, true);
            return std::nullopt;
        }
        const auto* text = std::get_if<std::string>(&arguments.at(1));
        if (!text) throw std::runtime_error(name + " without a string");
        const auto value = static_cast<float>(word_signed(number(arguments.at(2))));
        const bool t1 = bottom_form && number(arguments.at(3)) != 0;
        if (!label) {
            hud_.set_callout(key, 0, *text, value, t1);
            if (hosted) hud_object_->callout_set(static_cast<std::int32_t>(key), *text, 0, false, t1, value, 0.0F);
            return std::nullopt;
        }
        const auto& localization = objectives_.inputs().localization;
        if (!localization) {
            applied = false;
            return std::nullopt;
        }
        if (const auto resolved = localization->resolve(*text)) {
            hud_.set_callout(key, 0, std::string(*resolved), value, t1);
            if (hosted)
                hud_object_->callout_set(static_cast<std::int32_t>(key), std::string(*resolved), 0, false, t1,
                                         value, 0.0F);
        }
        return std::nullopt;
    }
    // game::setFxLightDist(VisionFxMan_Mode, float) 0025e460: for mode < 5
    // (sltiu) the float goes to *004ffc30 +74 + mode * 0x40; else nothing.
    if (name == "game::setFxLightDist") {
        require_signature(script, function, 4, {"enum:VisionFxMan_Mode", "primitive:2"});
        const Word mode = number(arguments.at(0));
        if (mode < vision_fx_light_dist_74_.size())
            vision_fx_light_dist_74_[mode] = std::bit_cast<float>(number(arguments.at(1)));
        return std::nullopt;
    }
    // game::SetLocation(GOBJ, string) 002b0ec0 -> 002b0f00: nothing for a
    // null GOBJ; in solo (005721c8 clear) player index 0 < 4 and
    // 0012f5c0(004ffb50, 0, 003fdb40(string)) stores the string id (003fe650,
    // -1 when missing) at 004ffb50 +0x130; the GOBJ is not otherwise read.
    // Deferred without the localization.
    if (name == "game::SetLocation") {
        require_signature(script, function, 4, {"object:GOBJ", "primitive:3"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + " without a live world object");
        const auto* key = std::get_if<std::string>(&arguments.at(1));
        if (!key) throw std::runtime_error(name + " without a string");
        const auto& localization = objectives_.inputs().localization;
        if (!localization) {
            applied = false;
            return std::nullopt;
        }
        hud_.set_location(localization->lookup(*key));
        return std::nullopt;
    }
    // game::AddPDAQuip(name, text) 001c63b0 on *004ffc2c (hud_object.hpp):
    // 00241da0 appends the PDA record, then the ticker notice 00242b00 of
    // 003fdb50("PDAMsg"). Solo game (005721c8 clear). Deferred without the
    // HUD object or the localization.
    if (name == "game::AddPDAQuip") {
        require_signature(script, function, 4, {"primitive:3", "primitive:3"});
        const auto* quip = std::get_if<std::string>(&arguments.at(0));
        const auto* text = std::get_if<std::string>(&arguments.at(1));
        if (!quip || !text) throw std::runtime_error(name + " without name and text strings");
        const auto& localization = objectives_.inputs().localization;
        if (!hud_object_ || !localization) {
            applied = false;
            return std::nullopt;
        }
        hud_object_->add_pda_quip(*quip, *text, [&](const std::string& key) -> std::optional<std::string> {
            if (const auto resolved = localization->resolve(key)) return std::string(*resolved);
            return std::nullopt;
        }, false);
        return std::nullopt;
    }
    // game::DisplayInteract(GOBJ, key, button) 0045e640 and
    // game::ClearInteract(GOBJ) 0045e5f0 (CLIMB_PROMPTS.md). The GOBJ goes
    // through 003cc820 (identity) and only the multiplayer branch of
    // 0045e930 / 0045e730 reads it; the solo game (005721c8 clear) runs
    // 0045eca0(key, lbu button): with *004ffc2c and its game screen +6a4,
    // 0045d070(gs, 003fdb50(key), button, 1), and 0045ec60: 0045cfb0(gs, 1).
    // Deferred without the HUD object, its game screen or (display) the
    // localization.
    if (name == "game::DisplayInteract" || name == "game::ClearInteract") {
        const bool display = name == "game::DisplayInteract";
        // 0045e6d0 (text, button), 0045e700 (text; button 1) and 0045e620 () (checkpoint
        // 132ms1): pad_interact_prompt.hpp.
        const bool text_form = display && function.parameters.size() <= 2;
        if (text_form && function.parameters.size() == 1)
            require_signature(script, function, 4, {"primitive:3"});
        else if (text_form)
            require_signature(script, function, 4, {"primitive:3", "enum:JoystickButton"});
        else if (display)
            require_signature(script, function, 4, {"object:GOBJ", "primitive:7", "enum:JoystickButton"});
        else if (function.parameters.empty())
            require_signature(script, function, 4, {});
        else
            require_signature(script, function, 4, {"object:GOBJ"});
        const auto& localization = objectives_.inputs().localization;
        if (!hud_object_ || !game_screen_interact_ || (display && !text_form && !localization)) {
            applied = false;
            return std::nullopt;
        }
        if (!display) {
            game_screen_clear_interact(game_screen_interact_, true);
            return std::nullopt;
        }
        if (text_form) {
            const auto* text = std::get_if<std::string>(&arguments.at(0));
            if (!text) throw std::runtime_error(name + " without a text string");
            const std::uint8_t button = arguments.size() > 1 ? static_cast<std::uint8_t>(number(arguments.at(1))) : 1;
            game_screen_display_interact_text(game_screen_interact_, true, *text, button);
            return std::nullopt;
        }
        const auto* key = std::get_if<std::string>(&arguments.at(1));
        if (!key) throw std::runtime_error(name + " without a key string");
        game_screen_display_interact(game_screen_interact_, true, *key, static_cast<std::uint8_t>(number(arguments.at(2))),
                                     [&](const std::string& text_key) -> std::optional<std::string> {
                                         if (const auto resolved = localization->resolve(text_key))
                                             return std::string(*resolved);
                                         return std::nullopt;
                                     });
        return std::nullopt;
    }
    // game::PlayXA (mission_script_host.hpp set_play_xa): 0045ed80 (name) / 0045ed40 (name, string) ->
    // 0045edb0(name, a2): nothing without *004ffc2c or its +6a4, else 0045ce10(gs, name, a2, 1). Void. The
    // interpreter's 003de3fc skips a call whose string word is below 1000 (ScriptContext::call).
    if (name == "game::PlayXA") {
        if (function.parameters.size() == 2)
            require_signature(script, function, 4, {"primitive:7", "primitive:3"});
        else
            require_signature(script, function, 4, {"primitive:7"});
        const auto* text = std::get_if<std::string>(&arguments.at(0));
        if (!text) throw std::runtime_error(name + " without a string");
        std::optional<std::string> second;
        if (function.parameters.size() == 2) {
            const auto* s = std::get_if<std::string>(&arguments.at(1));
            if (!s) throw std::runtime_error(name + " without its second string");
            second = *s;
        }
        if (!play_xa_) {
            applied = false;
            return std::nullopt;
        }
        play_xa_(*text, second);
        return std::nullopt;
    }
    // The Player choose-menu natives 00175d60 / 00175da0 / 00175de0 / 00175e20 (mission_script_host.hpp
    // set_choose_menu): argument 0 is the Player component (hosted for the local player's); its +30 is the
    // player's actor, so LaunchChooseMenu passes the agent. The interpreter skips NULL objects and strings.
    if ((name == "SetMenuReceiver" || name == "SetMenuTitle" || name == "AddMenuChoice" || name == "LaunchChooseMenu") &&
        !function.parameters.empty() && script.type_key(function.parameters[0]) == "object:Player") {
        if (name == "SetMenuReceiver") require_signature(script, function, 4, {"object:Player", "object:GOBJ"});
        else if (name == "LaunchChooseMenu") require_signature(script, function, 4, {"object:Player"});
        else require_signature(script, function, 4, {"object:Player", "primitive:3"});
        const auto* player = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!player || player->kind != RuntimeObjectHandle::Kind::player || player->id != objects_.player ||
            !objects_.objects.alive(*player))
            throw std::runtime_error(name + " is hosted only for the local player's component");
        ChooseMenuCall call;
        if (name == "SetMenuReceiver") {
            call.kind = ChooseMenuCall::Kind::receiver;
            const auto* gobj = std::get_if<RuntimeObjectHandle>(&arguments.at(1));
            if (!gobj || gobj->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*gobj))
                throw std::runtime_error(name + " without a live GOBJ");
            call.object = gobj->id;
        } else if (name == "LaunchChooseMenu") {
            call.kind = ChooseMenuCall::Kind::launch;
        } else {
            call.kind = name == "SetMenuTitle" ? ChooseMenuCall::Kind::title : ChooseMenuCall::Kind::add;
            const auto* text = std::get_if<std::string>(&arguments.at(1));
            if (!text) throw std::runtime_error(name + " without a string");
            call.text = *text;
        }
        if (!choose_menu_) {
            applied = false;
            return std::nullopt;
        }
        choose_menu_(call);
        return std::nullopt;
    }
    // The gameSound natives (game_sound.hpp), solo game (005721c8 clear):
    // PlayQuip(name, prio) 001c7180 -> 001c67c0(name, prio, 0) and
    // PlayQuip(name, GOBJ, prio) 001c7120 -> 001c6720(name, 003cc820(GOBJ),
    // prio); PlayMusic(name, GOBJ) 001c7200 -> 001c6a00 and StopMusic(name,
    // GOBJ) 001c71c0 -> 001c6890, whose GOBJ (003cc820) only the multiplayer
    // branch reads. Each posts one message for the next pump 003c8de0.
    // Deferred without the manager. An owned quip's owner word is the object
    // id, as for PlaySnd: actors (the player, "npc" objects) and population
    // GOBs ("object", Checkpoint 132k: +49 0xf / 0xe, so the LPD 003eae60 and
    // lip sync 00391810 of 001c8770 / 001c8e10 skip them) are hosted (the
    // camera's +64, fstream's LPD load 003eae60 and the lip sync 00391810 on
    // owner + 60 are the play host's); any other GOBJ owner is deferred.
    // Checkpoint 132sx7: a population node ("node", 0015c390: +c the id, class
    // 0xa) is hosted too (the play host's node view).
    // A sound owner (003cc820): the object id of a live actor, population GOB or node; nullopt for any other object.
    // Checkpoint 132zf9: for PlaySnd / StopSnd of a non-quip type an NIE: event object (0023c610, class 0x10)
    // or an SND: sound object (00212ee0, class 0xb) owner is hosted too, as for the NI sound events and the
    // SoundGOBJs that own their sound (the play host's owner views, Checkpoint 132vo2; BELARUS1 MILL's
    // O_PISTON_UP on NIE 470506).
    // Checkpoint 132ms8: so is a FIR: zone (002625b0 after 003cf0e0: +c the record key, +49 0x12; TRAINING's
    // StopSnd(0, O_FIRE_BUILDING, 1717226) as its fires go out, through the play host's FireObj owner view).
    const auto actor_owner = [&](const Value& value, const std::string& native,
                                 bool sound_owners = false) -> std::optional<std::uint32_t> {
        const auto* object = std::get_if<RuntimeObjectHandle>(&value);
        if (!object || object->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*object))
            throw std::runtime_error(native + " without a live GOBJ");
        const auto section = objects_.sections.find(object->id);
        if (object->id != objects_.player &&
            (section == objects_.sections.end() ||
             (section->second != "npc" && section->second != "object" && section->second != "node" &&
              !(sound_owners && (section->second == "event_object" || section->second == "sound" || section->second == "fire")))))
            return std::nullopt;
        return object->id;
    };    if (name == "game::PlayQuip") {
        const bool owned = arguments.size() == 3;
        if (owned)
            require_signature(script, function, 4, {"primitive:3", "object:GOBJ", "primitive:1"});
        else
            require_signature(script, function, 4, {"primitive:3", "primitive:1"});
        const auto* quip = std::get_if<std::string>(&arguments.at(0));
        if (!quip) throw std::runtime_error(name + " without a name string");
        std::uint32_t owner = 0;
        if (owned && !null_handle(arguments.at(1))) {
            const auto actor = actor_owner(arguments.at(1), name);
            if (!actor) {
                applied = false;
                return std::nullopt;
            }
            owner = *actor;
        }
        if (!game_sound_) {
            applied = false;
            return std::nullopt;
        }
        if (owned) game_sound_->play_quip(*quip, owner, word_signed(number(arguments.at(2))));
        else game_sound_->play_quip(*quip, word_signed(number(arguments.at(1))));
        return std::nullopt;
    }
    // game::PlaySnd(type, name, GOBJ) 001c7290: lb +0 (the signed type), lw +4,
    // 003cc820(+8) (identity), then 001c6d00(type, name, GOBJ) and return 0.
    // The owner word is the object id (the play host answers 00150c60 and the
    // owner view for actors and, since Checkpoint 132k, population GOBs); a
    // null GOBJ plays unowned. Any other owner is deferred, as is any
    // call without the manager. An owned type 4 is a quip (its LPD 003eae60
    // and lip sync 00391810 are the play host's).
    // Checkpoint 132sx12: game::PlaySnd(type, name) 001c7310 (registered at 004c8e38; ZURICH voice::Music):
    // lb +0, lw +4, then 001c6ea0(type, name) and return 0. No owner and no multiplayer branch; deferred
    // without the manager.
    if (name == "game::PlaySnd" && arguments.size() == 2) {
        require_signature(script, function, 4, {"enum:GameSound_SndBank", "primitive:3"});
        const auto* sound = std::get_if<std::string>(&arguments.at(1));
        if (!sound) throw std::runtime_error(name + " without a name string");
        if (!game_sound_) {
            applied = false;
            return std::nullopt;
        }
        game_sound_->play_sound(static_cast<std::int8_t>(number(arguments.at(0)) & 0xffU), *sound);
        return std::nullopt;
    }
    if (name == "game::PlaySnd") {
        require_signature(script, function, 4,         {"enum:GameSound_SndBank", "primitive:3", "object:GOBJ"});
        const auto* sound = std::get_if<std::string>(&arguments.at(1));
        if (!sound) throw std::runtime_error(name + " without a name string");
        const auto type = static_cast<std::int8_t>(number(arguments.at(0)) & 0xffU);
        std::uint32_t owner = 0;
        if (!null_handle(arguments.at(2))) {
            const auto actor = actor_owner(arguments.at(2), name, type != 4);
            if (!actor) {
                applied = false;
                return std::nullopt;
            }
            owner = *actor;
        }
        if (!game_sound_) {
            applied = false;
            return std::nullopt;
        }
        game_sound_->play_sound(type, *sound, owner);
        return std::nullopt;
    }
    // game::SmokeToronto3() 002326f0 -> 002329d0: 00232bf0(0x15, -1, 20.0,
    // 0.0, 40.0, 0.4) (lui 41a0 / 0 / 4220 / 3ecccccd), then, only when
    // 005721c8 (multiplayer) is set, the effect message 004df0f0 (+40 = 4,
    // 003c9420 / 003c9d60): not reached in solo play.
    if (name == "game::SmokeToronto3") {
        require_signature(script, function, 4, {});
        applied = world_.start_atmosphere_effect({0x15, -1, 20.0F, 0.0F, 40.0F, std::bit_cast<float>(0x3ecccccdU)});
        return std::nullopt;
    }
    // Checkpoint 132ms2: game::FogMyanmar() 00232710 -> 00232a90: the same
    // smoke field as SmokeToronto3 with effect 0x14 (00232bf0(0x14, -1, 20.0,
    // 0.0, 40.0, 0.4)); the multiplayer message (+40 = 4) is not reached solo.
    if (name == "game::FogMyanmar") {
        require_signature(script, function, 4, {});
        applied = world_.start_atmosphere_effect({0x14, -1, 20.0F, 0.0F, 40.0F, std::bit_cast<float>(0x3ecccccdU)});
        return std::nullopt;
    }
    // Checkpoint 132ms2: the weather natives, each `jal worker; return 0`:
    // the worker sets the weather group state 003a5f20(0, 0, f12, f13, f14)
    // and starts the precipitation record 00232bf0(type, count, f12..f15)
    // (the 0xd0 record 002348e0, WEATHER.md); the multiplayer message (+40 =
    // 3 / 2 / 1) is gated on 005721c8 and not reached in solo play.
    //   SnowBelarus2 00232690 -> 00232730: group (10, 20, 0.5), (0x16, 1200, 0, 15, 45, 0.4)
    //   RainLorelei  002326b0 -> 00232810: group (20, 40, 1.0), (0x18,  500, 0, 25, 45, 1.0)
    //   RainTokyo    002326d0 -> 002328f0: group (10, 20, 0.5), (0x17,  300, 0, 25, 45, 1.0)
    if (name == "game::SnowBelarus2" || name == "game::RainLorelei" || name == "game::RainTokyo") {
        require_signature(script, function, 4, {});
        MissionScriptWorld::AtmosphereEffect effect{};
        if (name == "game::SnowBelarus2")
            effect = {0x16, 1200, 0.0F, 15.0F, 45.0F, std::bit_cast<float>(0x3ecccccdU), 10.0F, 20.0F, 0.5F};
        else if (name == "game::RainLorelei")
            effect = {0x18, 500, 0.0F, 25.0F, 45.0F, 1.0F, 20.0F, 40.0F, 1.0F};
        else
            effect = {0x17, 300, 0.0F, 25.0F, 45.0F, 1.0F, 10.0F, 20.0F, 0.5F};
        applied = world_.start_atmosphere_effect(effect);
        return std::nullopt;
    }
    // Checkpoint 132ms2: the GameCamera stack natives on the game world
    // 004ffb50's active object list +c4 (the CAM: records, population.cameras):
    //   PushCurrent(GameCamera) 0017a160 -> 00131320(G, +0): the top's virtual
    //     +60(1), the camera's +5c(1), 00583878 / 0058387c = it and its +c,
    //     then it is inserted first.
    //   PopCurrent(GameCamera) 0017a130 -> 00131240(G) (the argument is not
    //     read): the first object removed (00138da0), its +60(1), the new
    //     first's +5c(1) and the published reference (null / -1 when empty).
    if ((name == "PushCurrent" || name == "PopCurrent") && function.parameters.size() == 1) {
        require_signature(script, function, 4, {"object:GameCamera"});
        if (name == "PopCurrent") {
            applied = world_.pop_current_camera();
            return std::nullopt;
        }
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || !objects_.objects.alive(*handle))
            throw std::runtime_error("PushCurrent: 00131320 without a live GameCamera (the original calls its virtual +5c)");
        const auto section = objects_.sections.find(handle->id);
        if (handle->kind != RuntimeObjectHandle::Kind::world_object || section == objects_.sections.end() ||
            section->second != "camera")
            throw std::runtime_error("PushCurrent: the object is not a CAM: camera: " + std::to_string(handle->id));
        applied = world_.push_current_camera(handle->id);
        return std::nullopt;
    }
    // game::ResetNpc (001485e0) is dispatched with the AI natives below
    // (132sx9). SetPlayerInvulnerability is hosted below (132sx8).
    // game::StopAllQuips() 001c7100 -> 001c6650(0) (checkpoint 132zf7): one
    // type-4 STOP message for the next pump; deferred without the manager.
    if (name == "game::StopAllQuips") {
        require_signature(script, function, 4, {});
        if (!game_sound_) {
            applied = false;
            return std::nullopt;
        }
        game_sound_->stop_all_quips();
        return std::nullopt;
    }
    // game::StopSnd(type, name) 001c72e0 -> 001c6c50(type, name) and
    // game::StopSnd(type, name, GOBJ) 001c7240 -> 001c6b70(type, name,
    // 003cc820(GOBJ)): one STOP message 0020ecb0 (+20 = 1), returning 0. The
    // owner is resolved as for PlaySnd; deferred without the manager or for
    // an owner the play host does not answer.
    if (name == "game::StopSnd") {
        const bool owned = arguments.size() == 3;
        if (owned)
            require_signature(script, function, 4, {"enum:GameSound_SndBank", "primitive:3", "object:GOBJ"});
        else
            require_signature(script, function, 4, {"enum:GameSound_SndBank", "primitive:3"});
        const auto* sound = std::get_if<std::string>(&arguments.at(1));
        if (!sound) throw std::runtime_error(name + " without a name string");
        const auto type = static_cast<std::int8_t>(number(arguments.at(0)) & 0xffU);
        std::uint32_t owner = 0;
        if (owned && !null_handle(arguments.at(2))) {
            const auto actor = actor_owner(arguments.at(2), name, type != 4);
            if (!actor) {
                applied = false;
                return std::nullopt;
            }
            owner = *actor;
        }
        if (!game_sound_) {
            applied = false;
            return std::nullopt;
        }
        game_sound_->stop_sound(type, *sound, owner);
        return std::nullopt;
    }
    if (name == "game::PlayMusic" || name == "game::StopMusic") {
        require_signature(script, function, 4, {"primitive:3", "object:GOBJ"});
        const auto* track = std::get_if<std::string>(&arguments.at(0));
        if (!track) throw std::runtime_error(name + " without a name string");
        if (!game_sound_) {
            applied = false;
            return std::nullopt;
        }
        if (name == "game::PlayMusic") game_sound_->play_music(*track);
        else game_sound_->stop_music(*track);
        return std::nullopt;
    }
    // StartGlobalTimer 0012f860 (Checkpoint 132sx3): 0012f890(f12 seconds, a0 0) -> j 00245560(*004ffc2c, 0,
    // seconds): the HUD timer +6c4 to 00245810 + seconds (no label), then 0020faa0(msg, 004f0f90 "E_TIMER"),
    // +20 = 1, 003c9420(msg, 004ffb50, 0) - also for 0 seconds (streets::BlowPowerSupply); with bytes 005721c8 and
    // 005721c0 a message 004f7dc0 (+20 = 4, +24 = end) to the player follows (multiplayer; play is solo). The
    // expiry's TimerExpiredMsg (004f7a08) goes to the player; no TORONTO1 or COMMON script handles it.
    // game::IsObjectiveComplete(GOBJ, string) 0032f5c0: 003cc820 (identity), then 0032f600(gobj, key): with
    // +4c == *0049d010 (the human actor class word) 00333b10(00185c70(gobj), key) & 0xff, else 0. In solo
    // 00185c70 gives slot 0's agent (001314f0(004ffb50, 0)) when the controller +58 answers virtual +54 (the
    // local player); an AI controller gives a null agent, which 00333b10 reads (+84): that throws.
    if (name == "game::IsObjectiveComplete" && function.parameters.size() == 2) {
        require_signature(script, function, 1, {"object:GOBJ", "primitive:7"});
        const auto* object = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!object || object->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*object))
            throw std::runtime_error("game::IsObjectiveComplete (0032f5c0) without a live GOBJ");
        const auto* key = std::get_if<std::string>(&arguments.at(1));
        if (!key) throw std::runtime_error("game::IsObjectiveComplete (0032f5c0): a null key reaches 003fda40");
        if (object->id != objects_.player)
            throw std::runtime_error("unhosted(0x0032f600): game::IsObjectiveComplete on a GOBJ other than the local player (" +
                                     std::to_string(object->id) + "): its class word +4c / controller +58 virtual +54 is not hosted");
        if (!agent_objective_complete_)
            throw std::runtime_error("unhosted(0x00333b10): game::IsObjectiveComplete needs the play agent record");
        return Value{static_cast<Word>(agent_objective_complete_(*key) ? 1 : 0)};
    }
    // game::SetFootstepSoundRadius(GOBJ, float) 00169b70 (Checkpoint 132ms3, tests/footstep_radius_goldens.hpp):
    // f20 = args +4, 003cc820(GOBJ) (identity); with the controller +58 present and its virtual +54 (IsPlayer)
    // or +58 answering, swc1 f20, +31f8; v0 = 0. The player component 004d9cf0 answers +54, the AI controller
    // 004d9380 +58 (0015b7e0: 1). A GOBJ outside the population (its +58 not hosted) throws naming 00169b70.
    if (name == "game::SetFootstepSoundRadius") {
        require_signature(script, function, 4, {"object:GOBJ", "primitive:2"});
        const auto* object = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!object || object->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*object))
            throw std::runtime_error("game::SetFootstepSoundRadius (00169b70) without a live GOBJ (the original reads +58 of it)");
        const float radius = std::bit_cast<float>(number(arguments.at(1)));
        if (object->id == objects_.player) {
            applied = world_.set_player_footstep_sound_radius(radius);
            return std::nullopt;
        }
        const auto section = objects_.sections.find(object->id);
        if (section == objects_.sections.end() || section->second != "npc")
            throw std::runtime_error("unhosted(0x00169b70): game::SetFootstepSoundRadius on a GOBJ that is not an actor (" +
                                     std::to_string(object->id) + "): its controller +58 is not hosted");
        auto* npcs = world_.npcs();
        applied = npcs && npcs->set_footstep_sound_radius(object->id, radius);
        return std::nullopt;
    }
    // The (float, string) form 0012f820 (ITALY, KYRGSTAN, LORELEI, UKRAINE, YEMEN2) loads the key word +4 and
    // the seconds +0, then the same 0012f890(f12 seconds, a0 key): 00245560 takes the label 003fdb50(key)
    // (0x0024558c) before 00244ee0; the rest is identical.
    if (name == "game::StartGlobalTimer") {
        const bool keyed = function.parameters.size() == 2;
        if (keyed) require_signature(script, function, 4, {"primitive:2", "primitive:7"});
        else require_signature(script, function, 4, {"primitive:2"});
        const std::string* key = nullptr;
        if (keyed) {
            key = std::get_if<std::string>(&arguments.at(1));
            if (!key) throw std::runtime_error("game::StartGlobalTimer (0012f820) without a key string");
        }
        if (!global_timer_) {
            applied = false;
            return std::nullopt;
        }
        global_timer_(std::bit_cast<float>(number(arguments.at(0))), key);
        if (game_sound_)
            game_sound_->post({GameSoundMessage::Kind::play_20faa0, 1,
                               game_sound_->message_copy(static_sound_record(static_sound_descriptor(0x004f0f90U)), false)});
        return std::nullopt;
    }
    // game::SetPersonalTimer 002677d0 (Checkpoint 132zf4): 003cc820(GOBJ) (identity), then 00267860(gobj,
    // f12 seconds, a1 0). With the GOBJ's controller +58 answering its virtual +54 and its byte +14 set (the
    // local player): 00244ee0(*004ffc2c +6ac, seconds + hud +6dc, null label: "" 0x4a7218); with byte +14 clear
    // a message 004f8580 (+20 = 0x40, +24 = 8) to the GOBJ (multiplayer); without the controller nothing. The
    // +6ac expiry is timer_message(2, 1):
    // the player's TimerExpiredMsg with IsPersonal 1 (SEWER OnTimerExpiredMsg).
    if (name == "game::SetPersonalTimer") {
        require_signature(script, function, 4, {"object:GOBJ", "primitive:2"});
        const auto* object = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!object || object->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*object))
            throw std::runtime_error("game::SetPersonalTimer without a live GOBJ");
        if (object->id != objects_.player)
            throw std::runtime_error("unhosted(0x00267860): game::SetPersonalTimer on a GOBJ other than the local player (" +
                                     std::to_string(object->id) + "): its controller +58 virtual +54 is not hosted");
        if (!personal_timer_) {
            applied = false;
            return std::nullopt;
        }
        personal_timer_(std::bit_cast<float>(number(arguments.at(1))));
        return std::nullopt;
    }
    // ITEM_INVENTORY.md. 00470200 -> 00470450 registers a row; the player
    // wrappers 00175910 / 001758a0 / 00175830 convert the GOBJ (003cc820) and
    // act on +0x3528 only for the actor class (+4c == *0x49d010), else they do
    // nothing (count 0). Only the player actor is hosted.
    // game::SetPlayerInvulnerability(int) 00175480 (MissionScriptWorld::
    // set_player_invulnerable): sltu s1, zero, v0; returns 0.
    // game::ReSpawn(Player) 0022e1a0 (MissionScriptWorld::respawn_player): the local player's component; returns 0.
    if (name == "game::ReSpawn") {
        require_signature(script, function, 4, {"object:Player"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::player || handle->id != objects_.player ||
            !objects_.objects.alive(*handle))
            throw std::runtime_error("game::ReSpawn(Player) requires the local player's component");
        applied = world_.respawn_player();
        return std::nullopt;
    }
    if (name == "game::SetPlayerInvulnerability") {
        require_signature(script, function, 4, {"primitive:1"});
        applied = world_.set_player_invulnerable(number(arguments.at(0)) != 0);
        return std::nullopt;
    }
    if (name == "game::RegisterItem") {
        require_signature(script, function, 4, {"primitive:7", "primitive:3"});
        const auto* key = std::get_if<std::string>(&arguments.at(0));
        const auto* resource = std::get_if<std::string>(&arguments.at(1));
        if (!key || !resource) throw std::runtime_error(name + " without key and resource strings");
        const auto& localization = objectives_.inputs().localization;
        if (!localization) {
            applied = false;
            return std::nullopt;
        }
        items_.register_item(*localization, *key, *resource);
        return std::nullopt;
    }
    if (name == "game::PlayerAddItem" || name == "game::PlayerRemoveItem" || name == "game::PlayerGetItemCount") {
        const bool query = name == "game::PlayerGetItemCount";
        if (query)
            require_signature(script, function, 1, {"object:GOBJ", "primitive:7"});
        else
            require_signature(script, function, 4, {"object:GOBJ", "primitive:7", "primitive:1"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        // 00175910 / 001758a0 / 00175830: 003cc820(GOBJ), then any actor (+4c == *0x49d010) uses its own +3528
        // list. Checkpoint 132ms5: besides the player, an actor the scripts know as an NPC (a CPU partner taking
        // BELARUS2's C-4 as the second agent, Who of its trigger) keeps its list in item_list; other GOBJs throw.
        const auto npc_actor = [this](Word id) {
            for (const auto& [npc, plus_c] : spawned_plus_c_)
                if (plus_c == id) return true;
            const auto section = objects_.sections.find(id);
            return section != objects_.sections.end() && section->second == "npc";
        };
        const bool actor_ok = handle && handle->kind == RuntimeObjectHandle::Kind::world_object &&
                              objects_.objects.alive(*handle) && (handle->id == objects_.player || npc_actor(handle->id));
        if (!actor_ok) throw std::runtime_error(name + " is hosted only for the player actor or an NPC actor");
        auto& items = item_list(handle->id);
        const auto* key = std::get_if<std::string>(&arguments.at(1));
        if (!key) throw std::runtime_error(name + " without an item key string");
        const auto& localization = objectives_.inputs().localization;
        if (!localization) {
            if (query) throw std::runtime_error(name + " needs the localization");
            applied = false;
            return std::nullopt;
        }
        const auto index = items_.index_of(*localization, *key);
        if (query) return static_cast<Word>(items.count(index));
        const auto requested = word_signed(number(arguments.at(2)));
        const bool removal = name == "game::PlayerRemoveItem";
        std::int32_t changed = 0;
        if (removal) {
            changed = items.remove(index, requested);
        } else {
            items.add(index, requested);
            changed = index < 0 ? 0 : requested;
        }
        if (handle->id == objects_.player)
            item_changes_.push_back({*key, index, requested, changed, items.count(index), removal});
        else
            item_changes_.push_back({*key, index, requested, changed, items.count(index), removal, handle->id});
        return std::nullopt;
    }
    // AddItem(NPC, key, n) 00148e50 -> component +218 = 0014a730: 00470a70(
    // *(c +30) +3528, key, n): 003fdb40(key), the table's linear search (-1:
    // none) and 00470870(list, index, n, 0) (Checkpoint 132zf5).
    if (name == "AddItem" && function.parameters.size() == 3 && script.type_key(function.parameters[0]) == "object:NPC") {
        require_signature(script, function, 4, {"object:NPC", "primitive:7", "primitive:1"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::ai) throw std::runtime_error(name + " without a live AI component");
        if (!objects_.objects.alive(*handle))
            throw std::runtime_error(name + " through a kept AI pointer to the deleted NPC " + std::to_string(handle->id) +
                                     ": the original calls 0014a730 on the freed component; not hosted");
        const auto* key = std::get_if<std::string>(&arguments.at(1));
        if (!key) throw std::runtime_error(name + " without an item key string");
        const auto& localization = objectives_.inputs().localization;
        if (!localization) {
            applied = false;
            return std::nullopt;
        }
        const auto index = items_.index_of(*localization, *key);
        const auto requested = word_signed(number(arguments.at(2)));
        auto& list = item_list(handle->id);
        list.add(index, requested);
        item_changes_.push_back({*key, index, requested, index < 0 ? 0 : requested, list.count(index), false, handle->id});
        return std::nullopt;
    }
    // Checkpoint 132sx8: GetItemCount(NPC, key) 00148d50 -> +220 = 0014a710:
    // 004705d0(*(c +30) +3528, key) (the index as AddItem's, then its count);
    // RemoveItem(NPC, key, n) 00148dd0 and RemoveItem(NPC, key) 00148d90 (n =
    // 0x186a0) -> +21c = 0014a720: 004709c0 (the index, then 004706c0(list,
    // index, n, 0), PlayerRemoveItem's removal).
    const bool npc_item_query = name == "GetItemCount" && function.parameters.size() == 2 &&
                                script.type_key(function.parameters[0]) == "object:NPC";
    const bool npc_item_removal = name == "RemoveItem" &&
                                  (function.parameters.size() == 2 || function.parameters.size() == 3) &&
                                  script.type_key(function.parameters[0]) == "object:NPC";
    if (npc_item_query || npc_item_removal) {
        if (npc_item_query) require_signature(script, function, 1, {"object:NPC", "primitive:7"});
        else if (function.parameters.size() == 3) require_signature(script, function, 4, {"object:NPC", "primitive:7", "primitive:1"});
        else require_signature(script, function, 4, {"object:NPC", "primitive:7"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::ai) throw std::runtime_error(name + " without a live AI component");
        if (!objects_.objects.alive(*handle))
            throw std::runtime_error(name + " through a kept AI pointer to the deleted NPC " + std::to_string(handle->id) +
                                     ": the original calls the component slot on freed memory; not hosted");
        const auto* key = std::get_if<std::string>(&arguments.at(1));
        if (!key) throw std::runtime_error(name + " without an item key string");
        const auto& localization = objectives_.inputs().localization;
        if (!localization) {
            if (npc_item_query) throw std::runtime_error(name + " needs the localization");
            applied = false;
            return std::nullopt;
        }
        const auto index = items_.index_of(*localization, *key);
        auto& list = item_list(handle->id);
        if (npc_item_query) return static_cast<Word>(list.count(index));
        const std::int32_t requested = function.parameters.size() == 3 ? word_signed(number(arguments.at(2))) : 0x186a0;
        const auto changed = list.remove(index, requested);
        item_changes_.push_back({*key, index, requested, changed, list.count(index), true, handle->id});
        return std::nullopt;
    }
    // 00169c00-0016a020: the 16x16 matrix *0x4ee620, bit 0 of [team][other]
    // (TEAM_RELATIONS.md). The level's matrix starts at 0016a750's defaults.
    // Checkpoint 132sx8: the registered team natives by name: TeamDelEnemyAllPlayers
    // 00169c00, TeamSetEnemyAllPlayers 00169ca0, TeamDelEnemyAllNpcs 00169d40,
    // TeamSetEnemyAllNpcs 00169e50, TeamDelEnemy 00169f60, TeamSetEnemy 00169fc0
    // and TeamEnemy 0016a020.
    static const std::set<std::string> team_natives{"game::TeamDelEnemyAllPlayers", "game::TeamSetEnemyAllPlayers",
        "game::TeamDelEnemyAllNpcs", "game::TeamSetEnemyAllNpcs", "game::TeamDelEnemy", "game::TeamSetEnemy", "game::TeamEnemy"};
    if (team_natives.contains(name)) {
        const bool all_players = name.ends_with("AllPlayers");
        const bool all_npcs = name.ends_with("AllNpcs");
        const bool query = name == "game::TeamEnemy";
        const bool pair = query || name == "game::TeamSetEnemy" || name == "game::TeamDelEnemy";
        if (!team_natives.count(name) || (!pair && !all_players && !all_npcs))
            throw std::runtime_error("Unsupported team native: " + script_native_signature(script, function));
        if (pair)
            require_signature(script, function, query ? 1 : 4, {"primitive:1", "primitive:1"});
        else
            require_signature(script, function, 4, {"primitive:1"});
        const auto team = word_signed(number(arguments.at(0)));
        if (query) return Word{teams_.enemy(team, word_signed(number(arguments.at(1)))) ? 1U : 0U};
        const bool enemy = name.starts_with("game::TeamSetEnemy");
        if (all_players)
            teams_.set_enemy_all_players(team, enemy);
        else if (all_npcs)
            teams_.set_enemy_all_npcs(team, enemy);
        else
            teams_.set_enemy(team, word_signed(number(arguments.at(1))), enemy);
        return std::nullopt;
    }
    // game::MakeNPCInteractable(NPC) -> Hotbox (00170020 -> 0014a690, the
    // identity: the receiver is the AI component's NPC -> 00170460 ->
    // 00170e20(_, actor, 1, 0)): a new 0xb0-byte hotbox every call (no
    // reuse, no life check) with an ID from 0040c240(&id, 0xf4), the bounds
    // of 001710a0 mode 1 from the actor as it is now, the NPC actor as its
    // only link, activated, type 3 and appended to the run-time list. The
    // multiplayer replication (005721c8 set -> 002d7290) is not sent in solo.
    // The inputs are read before the ID is allocated, so a throw leaves the
    // ID counter as it was.
    // game::ActivateBodyToss(GOBJ, bool) 00476740 (checkpoint 132zf7, MALL's
    // OnAiActivatedMsg): s1 = 003cc820(object), the message key 00588038
    // (vtable 004e1ae0, +24 = sltu(0, flag), +7c 0x40, +80 8) through
    // 003c9420(msg, s1, 0), returns 0. Without hosted NPCs the call is deferred.
    if (name == "game::ActivateBodyToss") {
        require_signature(script, function, 4, {"object:GOBJ", "primitive:1"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || !objects_.objects.alive(*handle)) throw std::runtime_error(name + " without a live object");
        auto* npcs = world_.npcs();
        applied = npcs && npcs->activate_body_toss(handle->id, number(arguments.at(1)) != 0);
        return std::nullopt;
    }
    if (name == "game::MakeNPCInteractable") {
        if (function.return_type < 100 || script.type_key(function.return_type) != "object:Hotbox" ||
            function.parameters.size() != 1 || script.type_key(function.parameters[0]) != "object:NPC")
            throw std::runtime_error("Unsupported native signature: " + script_native_signature(script, function));
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::ai || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + " without a live AI component");
        auto* npcs = world_.npcs();
        if (!npcs) throw std::runtime_error(name + " while NPCs are not hosted: " + std::to_string(handle->id));
        const auto inputs = npcs->interactable_box_inputs(handle->id);
        if (!inputs) throw std::runtime_error(name + " on an NPC the world does not host: " + std::to_string(handle->id));
        const auto bounds = npc_interactable_bounds(inputs->translation, inputs->bounds_minimum, inputs->bounds_maximum, true);
        const auto box = create_world_object(0xf4);
        if (!objects_.sections.emplace(box.id, "hotbox").second)
            throw std::runtime_error(name + ": hotbox ID already in the population: " + std::to_string(box.id));
        if (!world_.add_npc_hotbox(box.id, handle->id, bounds))
            throw std::runtime_error(name + ": the world does not host run-time hotboxes");
        return Value{box};
    }
    // Checkpoint 132sx11: AddWeapon(Player, int) 00175bf0 and RemoveWeapon(Player, int)
    // 00175bc0 -> 00176060 (registrations 004c6440..004c64e0 on the Player class 004ee788),
    // the checkpoint restart's loadout reset (CHECHNYA main::ResetLevel, ITALY / KYRGSTAN /
    // MNTNEGR1 OnNotifyCheckpointMsg). Both read the component's +30 actor (null, or not of
    // the actor class *0049d010: nothing) and the item's record 00147840(*004ffd30, item)
    // (record 0 outside the database), whose loaded model +13c gates the call; then
    // 00143df0(+3524, item, 1, 1, 0, 0) or, when the item's slot 00143d20 holds it (slot 0
    // with slot 1 holding it too: slot 1), 00142450(+3524, slot, 1, 0, 0). Both return 0.
    if ((name == "AddWeapon" || name == "RemoveWeapon") && function.parameters.size() == 2 &&
        script.type_key(function.parameters[0]) == "object:Player") {
        require_signature(script, function, 4, {"object:Player", "primitive:1"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::player || handle->id != objects_.player ||
            !objects_.objects.alive(*handle))
            throw std::runtime_error(name + "(Player) requires the local player's component");
        const auto item = word_signed(number(arguments.at(1)));
        applied = name == "AddWeapon" ? world_.add_player_weapon(item) : world_.remove_player_weapon(item);
        return std::nullopt;
    }
    // Checkpoint 132ms4: SetInvulnerable(Player, int) 00175eb0 (registered at 004c6228 beside the NPC
    // form 001490e0): v1 = the Player component, a0 = its +30 owner actor, 001b8750(actor, sltu(0, v)):
    // byte +33a4; returns 0. YEMEN1 main::LevelBegin. Solo play has the local player only, the actor
    // game::SetPlayerInvulnerability's slot loop reaches (MissionScriptWorld::set_player_invulnerable).
    if (name == "SetInvulnerable" && function.parameters.size() == 2 &&
        script.type_key(function.parameters[0]) == "object:Player") {
        require_signature(script, function, 4, {"object:Player", "primitive:1"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::player || handle->id != objects_.player ||
            !objects_.objects.alive(*handle))
            throw std::runtime_error("SetInvulnerable(Player) requires the local player's component");
        applied = world_.set_player_invulnerable(number(arguments.at(1)) != 0);
        return std::nullopt;
    }
    // Checkpoint 132ms4: SetMaxActiveNpcCount(Player, int) 00175980 (registered at 004c65d0, name
    // 0049ccc0; the NPC form 00149450 writes the global 004ea350 instead): the local actor (+30 +14)
    // stores the int to the component's +1b8, a remote one is sent 004f53f8 (handled in 00177380, the
    // same store); returns 0. YEMEN1 OnHotBoxMsg.
    if (name == "SetMaxActiveNpcCount" && function.parameters.size() == 2 &&
        script.type_key(function.parameters[0]) == "object:Player") {
        require_signature(script, function, 4, {"object:Player", "primitive:1"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::player || handle->id != objects_.player ||
            !objects_.objects.alive(*handle))
            throw std::runtime_error("SetMaxActiveNpcCount(Player) requires the local player's component");
        player_max_active_npc_count_ = static_cast<std::int32_t>(number(arguments.at(1)));
        applied = true;
        return std::nullopt;
    }
    // Checkpoint 132sx11: game::ResetLevel 0012f6e0 (-> 0012f700) and game::RemoveAllBackpacks
    // 00253c60 (j 002570c0), the checkpoint restart routines of CHECHNYA / ITALY / KYRGSTAN /
    // MNTNEGR1 (OnNotifyCheckpointMsg). ResetLevel's last step, the schedule clear 003dfb20,
    // is the VM's (Vm::invoke_index).
    if (name == "game::ResetLevel" || name == "game::RemoveAllBackpacks") {
        require_signature(script, function, 4, {});
        applied = name == "game::ResetLevel" ? world_.reset_level() : world_.remove_all_backpacks();
        return std::nullopt;
    }
    // The AI natives (NPC_ACTIVATION_AND_RESPAWN.md section 1): each wrapper
    // takes the receiver (the NPC's AI component, opcode 61's projection)
    // from argument 0 and calls a component slot or a global. Applied on the
    // world's NPCs; without them the void ones are deferred and the
    // value-returning ones throw (below).
    static const std::set<std::string> ai_natives{"ActivateAnon", "ActivateByRadius", "SetTeam", "SetSkill",
        "SetFiringThreshold", "GetTravelSpeed", "SetTravelSpeed", "SetNotGasable", "SetMaxActiveNpcCount",
        "GetClosestPlayer", "DeactivateForAllPlayers", "AddAwareness", "AddAwarenessOfPlayers", "SetTarget",
        "ResumeAITargeting", "ResumeAIMode", "SetMode", "LookAround", "StopAndLookAround", "MoveToNode", "MoveToGobj", "SetGrenadeTarget", "SetAggressiveness", "GetHitPoints",
        "AddWeapon", "DrawWeapon", "GetCurrentNode", "RequestFacial", "EquipGoggles", "RequestAction",
        "RequestLookAt", "ResumeAILookAt", "DeactivateDead", "SetHitPoints", "SetInvulnerable", "DontDropWeapons",
        "SetOnFire", "ResumeMovement", "RestrictMovementToNodeGroup", "Teleport", "SetUnconscious", "SetPickupable",
        "SetHasDisguise",
        "ActivateByRadiusOff", "AddGroupToPathRegion", "StoreWeapon", "ResumeAIAiming", "ClearTarget",
        "ResumeAITravelSpeed", "ResendNewAwarenessMessage", "SetAimMode", "SetAimSubtarget", "RemoveAwareness", "InViewCone",
        "SetViewConeRadius", "SetViewConeAngle", "SetNotTaserable", "SetFireInvulnerability", "StopMovement", "GetNextNode",
        "SetGrenadeThrowAngle", "SendNewAwarenessOfPlayers", "RemoveGroupFromPathRegion", "SetWeaponPreference",
        "GetDestinationNode", "GetWeapon", "CarryBodyToNode", "Follow"};
    static const std::set<std::string> receiver_only{"ActivateAnon", "DeactivateForAllPlayers", "AddAwarenessOfPlayers",
        "ResumeAITargeting", "ResumeAIMode", "LookAround", "StopAndLookAround", "ResumeAILookAt", "DontDropWeapons", "SetOnFire",
        "ResumeMovement", "SetUnconscious", "ActivateByRadiusOff", "StoreWeapon", "ResumeAIAiming", "ClearTarget",
        "ResumeAITravelSpeed", "SendNewAwarenessOfPlayers"};
    const bool ai_team = name == "Team" && function.parameters.size() == 1 && script.type_key(function.parameters[0]) == "object:AI";
    const bool ai_activate = name == "Activate" && function.parameters.size() == 2 &&
                             script.type_key(function.parameters[0]) == "object:NPC";
    // Activate(NPC) (0014a250: lw a0, (a0); component virtual +48 = Activate
    // 00157df0; v0 = 0; checkpoint 132ms4, YEMEN1's Zohar after delivery).
    const bool ai_activate_one = name == "Activate" && function.parameters.size() == 1 &&
                                 script.type_key(function.parameters[0]) == "object:NPC";
    // Deactivate(NPC) (0014a050 -> component +4c = 00157ad0; registered at
    // 004c4384 with the name 0049ba40 and one parameter).
    const bool ai_deactivate = name == "Deactivate" && function.parameters.size() == 1 &&
                               script.type_key(function.parameters[0]) == "object:NPC";
    // game::ResetNpc(NPC) (001485e0: lw a0, (a0); jal 00148600; v0 = 0).
    const bool ai_reset = name == "game::ResetNpc" && function.parameters.size() == 1 &&
                          script.type_key(function.parameters[0]) == "object:NPC";
    // Deactivate(NPC, GOBJ) (0014a080: the GOBJ through 003cc820, unused in
    // single player -> 001573d0; registered beside Activate 0014a280).
    const bool ai_deactivate_gobj = name == "Deactivate" && function.parameters.size() == 2 &&
                                    script.type_key(function.parameters[0]) == "object:NPC";
    // Kill (00149fb0 -> component +168 = cNPC_Kill 0014b370) and
    // ForceWeaponFire (00149bf0 -> +198 = 0014b470(c, shots, -1, 0.0)) and
    // its four-argument form (00149b50 -> +198 = 0014b470(c, shots, item, delay); 132zf5).
    const bool ai_kill = name == "Kill" && function.parameters.size() == 1 &&
                         script.type_key(function.parameters[0]) == "object:NPC";
    const bool ai_force_fire_item = name == "ForceWeaponFire" && function.parameters.size() == 4 &&
                                    script.type_key(function.parameters[0]) == "object:NPC";
    // RemoveWeapon(NPC, int) 00149930 (the Player form is a different native).
    const bool ai_remove_weapon = name == "RemoveWeapon" && function.parameters.size() == 2 &&
                                  script.type_key(function.parameters[0]) == "object:NPC";
    const bool ai_force_fire = ai_force_fire_item || (name == "ForceWeaponFire" && function.parameters.size() == 2 &&
                               script.type_key(function.parameters[0]) == "object:NPC");
    if (ai_natives.contains(name) || ai_team || ai_activate || ai_activate_one || ai_deactivate || ai_reset || ai_deactivate_gobj ||
        ai_kill || ai_force_fire || ai_remove_weapon) {
        const std::string receiver_type = name == "SetTeam" || ai_team ? "object:AI" : "object:NPC";
        if (receiver_only.contains(name) || ai_kill || ai_activate_one || ai_deactivate || ai_reset)
            require_signature(script, function, 4, {receiver_type});
        else if (name == "AddAwareness" || name == "SetTarget" || name == "SetGrenadeTarget")
            require_signature(script, function, 4, {receiver_type, "object:GOBJ"});
        else if (name == "RequestFacial" || name == "RequestLookAt")
            require_signature(script, function, 4, {receiver_type, "object:GOBJ", "primitive:2"});
        else if (name == "SetMode") {
            // The three-argument SetMode (0014a4a0) passes a node to 0014bcf0
            // (+13f and a route to it, 0015fb20; N5a).
            if (function.parameters.size() == 3) require_signature(script, function, 4, {receiver_type, "enum:NPC_Mode", "object:Node"});
            else require_signature(script, function, 4, {receiver_type, "enum:NPC_Mode"});
        }
        else if (name == "MoveToNode") require_signature(script, function, 1, {receiver_type, "object:Node"});
        else if (name == "MoveToGobj") require_signature(script, function, 1, {receiver_type, "object:GOBJ"});
        else if (name == "RemoveAwareness") require_signature(script, function, 4, {receiver_type, "object:GOBJ"});
        else if (name == "InViewCone") require_signature(script, function, 1, {receiver_type, "object:GOBJ"});
        else if (name == "ResendNewAwarenessMessage") {
            if (function.parameters.size() == 2) require_signature(script, function, 4, {receiver_type, "object:GOBJ"});
            else require_signature(script, function, 4, {receiver_type});
        }
        else if (name == "SetAimMode") require_signature(script, function, 4, {receiver_type, "enum:NPC_AimMode"});
        else if (name == "SetAimSubtarget") require_signature(script, function, 4, {receiver_type, "enum:NPC_AimSubtarget"});
        else if (name == "GetNextNode" || name == "GetDestinationNode") {
            if (function.return_type < 100 || script.type_key(function.return_type) != "object:Node" ||
                function.parameters.size() != 1 || script.type_key(function.parameters[0]) != receiver_type)
                throw std::runtime_error("Unsupported native signature: " + script_native_signature(script, function));
        }
        else if (name == "CarryBodyToNode") require_signature(script, function, 1, {receiver_type, "object:GOBJ", "object:Node"});
        else if (name == "Follow") require_signature(script, function, 4, {receiver_type, "object:GOBJ", "primitive:2", "primitive:2"});
        else if (name == "RestrictMovementToNodeGroup" || name == "Teleport" || name == "AddGroupToPathRegion" ||
                 name == "RemoveGroupFromPathRegion")
            require_signature(script, function, 4, {receiver_type, "object:Node"});
        else if (name == "GetClosestPlayer") {
            if (function.return_type < 100 || script.type_key(function.return_type) != "object:GOBJ" ||
                function.parameters.size() != 1 || script.type_key(function.parameters[0]) != receiver_type)
                throw std::runtime_error("Unsupported native signature: " + script_native_signature(script, function));
        }
        else if (name == "GetCurrentNode") {
            if (function.return_type < 100 || script.type_key(function.return_type) != "object:Node" ||
                function.parameters.size() != 1 || script.type_key(function.parameters[0]) != receiver_type)
                throw std::runtime_error("Unsupported native signature: " + script_native_signature(script, function));
        }
        else if (ai_activate || ai_deactivate_gobj) require_signature(script, function, 4, {receiver_type, "object:GOBJ"});
        else if (ai_team) require_signature(script, function, 1, {receiver_type});
        else if (name == "GetTravelSpeed") require_signature(script, function, 2, {receiver_type, "enum:NPC_TravelSpeed"});
        else if (name == "RequestAction") require_signature(script, function, 4, {receiver_type, "enum:NPC_Action"});
        else if (name == "GetWeapon") require_signature(script, function, 1, {receiver_type, "enum:NPC_WeaponPreference"});
        else if (name == "SetWeaponPreference")
            require_signature(script, function, 4, {receiver_type, "primitive:1", "enum:NPC_WeaponPreference"});
        else if (name == "GetHitPoints") require_signature(script, function, 1, {receiver_type});
        // Checkpoint 132uk2: DrawWeapon(NPC) (00149a50: +1ac with a1 = -1, v0 = 0; UKRAINE's Savin).
        else if (name == "DrawWeapon" && function.parameters.size() == 1) require_signature(script, function, 4, {receiver_type});
        else if (name == "SetFiringThreshold") require_signature(script, function, 2, {receiver_type, "primitive:2"});
        else if (ai_force_fire_item)
            require_signature(script, function, 4, {receiver_type, "primitive:1", "primitive:1", "primitive:2"});
        else if (name == "SetTeam" || name == "SetNotGasable" || name == "SetMaxActiveNpcCount" || name == "AddWeapon" ||
                 name == "DrawWeapon" || name == "EquipGoggles" || name == "DeactivateDead" || ai_force_fire ||
                 name == "SetHitPoints" || name == "SetInvulnerable" || name == "SetPickupable" ||
                 name == "SetHasDisguise" || ai_remove_weapon ||
                 name == "SetNotTaserable" || name == "SetFireInvulnerability")
            require_signature(script, function, 4, {receiver_type, "primitive:1"});
        else require_signature(script, function, 4, {receiver_type, "primitive:2"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::ai)
            throw std::runtime_error(name + " without a live AI component");
        // Opcode 61 gives null for a deleted NPC (003e0400 -> 0040c540), so a
        // stale receiver is a kept AI pointer: a deferred call keeps the word
        // as is under 003e0a70's AI policy (003df400 at schedule, 003df710 at
        // replay: no 003e0400), and 003e0ab0 skips only words below 1000.
        if (!objects_.objects.alive(*handle)) {
            // ResumeAIMode 0014a470 calls the freed component's +e0: the root
            // vtable 004dfde0 left by 003cb330 gives 0015bff0 (v0 = 0, no
            // writes; npc_freed_ai_resume_slot). SetMode 0014a4f0 (+dc =
            // 00148870) and AddAwarenessOfPlayers 00149420 (+ec = 0015c000)
            // reach root slots that also return at once (132zf5).
            if (name == "ResumeAIMode" || name == "SetMode" || name == "AddAwarenessOfPlayers") return std::nullopt;
            // ResendNewAwarenessMessage 0014a340 / 0014a390 reach +f4 =
            // 003cde00, which loads word +50 and, when nonzero, calls its
            // vtable +c. On the AI component +50 is the mode request's pending
            // byte (0016f5b0 sets it to 1, 0016e170 to 1 on an accepted
            // request) and +51..+53 are never written, so the word depends on
            // the heap's history (Checkpoint 132ms3). The port-only aid
            // set_freed_ai_resend_skip takes it as 0.
            if (name == "ResendNewAwarenessMessage" && freed_ai_resend_skip_) {
                ++freed_ai_resends_skipped_;
                return std::nullopt;
            }
            if (name == "ResendNewAwarenessMessage")
                throw std::runtime_error(name + " through a kept AI pointer to the deleted NPC " + std::to_string(handle->id) +
                                         " (0040bf50 -> 001b57c0 freed its component, 003ccf30 -> 0036b130): the root vtable"
                                         " 004dfde0 +f4 = 003cde00 loads the freed word +50 (the pending byte 0016f5b0 writes"
                                         " and three bytes no constructor writes) and calls its vtable +c when nonzero; not"
                                         " hosted");
            if (name == "RequestAction")
                throw std::runtime_error(name + " through a kept AI pointer to the deleted NPC " + std::to_string(handle->id) +
                                         ": 00149ce0 calls the root vtable 004dfde0 +150 = 003cd090, a vector getter that"
                                         " loads word 0x50(a1) with a1 = the action byte (an address error on the EE when"
                                         " the action is not a multiple of 4; Checkpoint 132ms3); not hosted");
            throw std::runtime_error(name + " through a kept AI pointer to the deleted NPC " + std::to_string(handle->id) +
                                     " (0040bf50 -> 001b57c0 freed its component, 003ccf30 -> 0036b130): the original"
                                     " calls the native's slot of the root vtable 004dfde0 on freed memory"
                                     " (003df710 -> 003e0ab0); not hosted");
        }
        auto* npcs = world_.npcs();
        if (npcs && name == "SetMaxActiveNpcCount") {
            // 00149450 -> 0014b340: the receiver is not read.
            set_npc_max_active_count(npcs->system(), word_signed(number(arguments.at(1))));
            return std::nullopt;
        }
        NpcAiComponent* component = npcs ? npcs->component(handle->id) : nullptr;
        if (!component) {
            applied = false;
            if (function.return_type == 4) return std::nullopt;
            throw std::runtime_error(name + " on an NPC the world does not host: " + std::to_string(handle->id));
        }
        if (name == "GetHitPoints") {
            // 00149630 -> component +170 = 00149660: cvt.w.s of the actor's
            // +40 health (component +30), no clamp and no alive check.
            const auto hit_points = npcs->hit_points(handle->id);
            if (!hit_points) {
                applied = false;
                throw std::runtime_error("GetHitPoints(NPC): the world does not host NPC health: " + std::to_string(handle->id));
            }
            return Word{static_cast<std::uint32_t>(*hit_points)};
        }
        auto overlay_node = [&](std::int32_t index) {
            if (port_overlay_first_ < 0 || index < port_overlay_first_) return false;
            ++port_overlay_node_answers_;
            return true;
        };
        if (name == "GetCurrentNode") {
            // 001496e0 -> component +18c = 0014af90: the path follower (+1b0)
            // +4c, the hold node index; < 0 -> null, else the node table
            // *(*004ffbe0 +8)[index] (the population's node list); 0015c100
            // is the identity.
            const std::int32_t hold = component->path.hold_4c;
            if (hold < 0 || overlay_node(hold)) return Value(RuntimeObjectHandle{});
            if (static_cast<std::size_t>(hold) >= node_ids_.size())
                throw std::runtime_error("GetCurrentNode: hold node outside the population's node list");
            return Value(objects_.objects.resolve(node_ids_[static_cast<std::size_t>(hold)] | 0x80000000U));
        }
        if (name == "GetNextNode") {
            // 001496b0 -> component +190 = 0014af60: the path follower (+1b0)
            // +50, the current route node index; < 0 -> null, else the node
            // table entry as GetCurrentNode; 0015c100 is the identity.
            const std::int32_t next = component->path.current_50;
            if (next < 0 || overlay_node(next)) return Value(RuntimeObjectHandle{});
            if (static_cast<std::size_t>(next) >= node_ids_.size())
                throw std::runtime_error("GetNextNode: route node outside the population's node list");
            return Value(objects_.objects.resolve(node_ids_[static_cast<std::size_t>(next)] | 0x80000000U));
        }
        if (name == "GetDestinationNode") {
            // 00149680 -> component +194 = 0014af30: the path follower (+1b0)
            // +58 (the route's +10 destination, npc_route); < 0 -> null, else
            // the node table entry; 0015c100 is the identity (132sx10, MINSK).
            const std::int32_t destination = component->path.field_58;
            if (destination < 0 || overlay_node(destination)) return Value(RuntimeObjectHandle{});
            if (static_cast<std::size_t>(destination) >= node_ids_.size())
                throw std::runtime_error("GetDestinationNode: route node outside the population's node list");
            return Value(objects_.objects.resolve(node_ids_[static_cast<std::size_t>(destination)] | 0x80000000U));
        }
        if (name == "GetWeapon") {
            // 001498f0: a1 = lbu +4 -> +1c0 = 0014b770: (a1 & 0xff) == 0
            // reads +b0, otherwise +b4 (132sx10, CHECHNYA / MINSK).
            const auto item = (number(arguments.at(1)) & 0xffU) == 0 ? component->held_item_b0 : component->held_item_b4;
            return Word{static_cast<Word>(item)};
        }
        auto real = [&](std::size_t index) {
            const float value = std::bit_cast<float>(number(arguments.at(index)));
            if (!std::isfinite(value)) throw std::runtime_error(name + ": nonfinite argument");
            return value;
        };
        auto bits = [](float value) { return Word{std::bit_cast<std::uint32_t>(value)}; };
        if (name == "ActivateAnon" || ai_activate) {
            // 0014a2c0 / 0014a280 -> 00157800: in single player (005721c8
            // clear) actor virtual +5c(1); Activate's GOBJ goes through
            // 003cc820 (identity) and is not used.
            npcs->activate(handle->id);
            return std::nullopt;
        }
        if (ai_activate_one) {
            // 0014a250: component +48 = 00157df0 (+159 clear returns; an
            // absent actor gets +5c(1) when +44 holds, which re-enters it).
            applied = npcs->activate_component(handle->id);
            return std::nullopt;
        }
        if (name == "DeactivateForAllPlayers") {
            // 0014a010: unless +141 bit 7 (radius-controlled), component +c0
            // = DeactivateAnon 00157510.
            if (!(component->flags_141 & 0x80U)) npcs->deactivate_anonymous(handle->id);
            return std::nullopt;
        }
        if (ai_deactivate) {
            // 0014a050: component +4c = 00157ad0, no +141 check.
            applied = npcs->deactivate(handle->id);
            return std::nullopt;
        }
        if (ai_reset) {
            // 00148600 on the receiver's component (no null check).
            applied = npcs->reset_npc(handle->id);
            return std::nullopt;
        }
        if (ai_deactivate_gobj) {
            // 001573d0: +141 bit 7 (radius-controlled) returns; in single
            // player (005721c8 clear) the actor's +60(1) (001b5190), which
            // reaches 00157ad0 through 003928d0.
            if (!(component->flags_141 & 0x80U)) applied = npcs->deactivate_actor(handle->id);
            return std::nullopt;
        }
        if (name == "GetClosestPlayer") {
            // 001493f0 -> virtual +f0 = 0014afd0: 003ce850(actor +30, null),
            // then 003cc800 (identity): the closest player's actor.
            npcs->closest_player_distance_squared(handle->id);
            return Value(objects_.objects.resolve(objects_.player | 0x80000000U));
        }
        if (name == "ActivateByRadius") {
            npcs->activate_by_radius(handle->id, real(1));                   // 00148ee0 -> +1cc
            return std::nullopt;
        }
        if (name == "SetTeam") {
            set_npc_team(*component, word_signed(number(arguments.at(1))));  // 0016abe0 -> +a0
            return std::nullopt;
        }
        if (ai_team) return Word(static_cast<std::uint32_t>(npc_team(*component)));   // 0016abb0 -> +68
        if (name == "SetSkill") {
            set_npc_skill(*component, real(1));                              // 00149ae0 -> 0022f2d0
            return std::nullopt;
        }
        if (name == "SetFiringThreshold") return bits(set_npc_firing_threshold(*component, real(1)));   // 00149b10 -> +16c
        if (name == "GetTravelSpeed")                                         // 00149d10: lbu of the enum word, +148
            return bits(npc_travel_speed(static_cast<std::uint8_t>(number(arguments.at(1)) & 0xffU)));
        if (name == "SetTravelSpeed") {
            set_npc_travel_speed(*component, real(1));                       // 00149db0 -> +140 = 0014d6f0
            return std::nullopt;
        }
        if (name == "SetNotGasable") {
            npcs->set_not_gasable(handle->id, number(arguments.at(1)) != 0);  // 00148f80: sltu a1, zero, v0
            return std::nullopt;
        }
        if (name == "SetAggressiveness") {
            set_npc_aggressiveness(*component, real(1));                     // 00149f70 -> +110 = 0014b600: +6c = the float
            return std::nullopt;
        }
        if (ai_force_fire_item) {
            // 00149b50: a1 = the shots word, a2 = the item word, f12 = the
            // delay -> 0014b470 (an item other than the held one is kept in
            // +12c and selected through +1ac; npc_force_weapon_fire).
            applied = npcs->force_weapon_fire(handle->id, word_signed(number(arguments.at(1))),
                                              word_signed(number(arguments.at(2))), real(3));
            return std::nullopt;
        }
        if (ai_force_fire) {
            // 00149bf0: a1 = the script's int, a2 = -1, f12 = 0.0 -> 0014b470:
            // +12c = -1, +130 = shots, +134 = cvt.w.s(+8c).
            npc_force_weapon_fire_stores(*component, word_signed(number(arguments.at(1))), 0.0F);
            return std::nullopt;
        }
        if (ai_kill) {
            // cNPC_Kill 0014b370: the actor's damage virtual +100 with itself
            // as the source (category 2, type 0xa) for cvt of its +40 health,
            // unless life state 3 with +33a5 clear. Deferred without the
            // world's damage host.
            applied = npcs->kill(handle->id);
            return std::nullopt;
        }
        if (name == "AddWeapon") {
            applied = npcs->add_weapon(handle->id, word_signed(number(arguments.at(1))));    // 00149970 -> +1b8 = 0014b890
            return std::nullopt;
        }
        if (name == "DrawWeapon") {
            // 00149a10 -> +1ac = 0014bae0 with the item; the one-argument form 00149a50 passes -1 (Checkpoint 132uk2).
            const std::int32_t item = function.parameters.size() == 1 ? -1 : word_signed(number(arguments.at(1)));
            applied = npcs->draw_weapon(handle->id, item);
            return std::nullopt;
        }
        // A Node argument (0015c120, identity): its index in the node collection.
        auto node_index = [&](std::size_t index) -> std::int32_t {
            const auto* node = std::get_if<RuntimeObjectHandle>(&arguments.at(index));
            if (!node || node->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*node))
                throw std::runtime_error(name + " without a live node object");
            const auto found = std::find(node_ids_.begin(), node_ids_.end(), node->id);
            if (found == node_ids_.end()) throw std::runtime_error(name + " on a non-node record: " + std::to_string(node->id));
            return static_cast<std::int32_t>(found - node_ids_.begin());
        };
        // Checkpoint 132zf4 (TORONTO2 SEWER; MissionScriptNpcs): false defers the call.
        if (name == "SetHitPoints") {
            applied = npcs->set_hit_points(handle->id, word_signed(number(arguments.at(1))));      // 001495b0 -> +174
            return std::nullopt;
        }
        if (name == "SetInvulnerable") {
            applied = npcs->set_invulnerable(handle->id, number(arguments.at(1)) != 0);            // 001490e0 -> +1e8
            return std::nullopt;
        }
        if (name == "DontDropWeapons") {
            applied = npcs->dont_drop_weapons(handle->id);                                         // 00149040 -> +200
            return std::nullopt;
        }
        if (name == "SetOnFire") {
            applied = npcs->set_on_fire(handle->id);                                               // 001493b0 -> +17c
            return std::nullopt;
        }
        if (name == "ResumeMovement") {
            applied = npcs->resume_movement(handle->id);                                           // 00149c70 -> +158
            return std::nullopt;
        }
        if (name == "RestrictMovementToNodeGroup") {
            applied = npcs->restrict_to_node_group(handle->id, node_index(1));                     // 00149500 -> +1f4
            return std::nullopt;
        }
        if (name == "SetUnconscious") {
            applied = npcs->set_unconscious(handle->id);                                           // 00148fe0 -> +208
            return std::nullopt;
        }
        if (name == "SetPickupable") {
            applied = npcs->set_pickupable(handle->id, number(arguments.at(1)) != 0);              // 00149230 -> +1d4
            return std::nullopt;
        }
        if (name == "SetHasDisguise") {
            applied = npcs->set_has_disguise(handle->id, number(arguments.at(1)) != 0);            // 001491f0 -> +1d8
            return std::nullopt;
        }
        if (name == "ActivateByRadiusOff") {
            applied = npcs->activate_by_radius_off(handle->id);                                    // 00148eb0 -> +1d0
            return std::nullopt;
        }
        if (name == "AddGroupToPathRegion") {
            applied = npcs->add_group_to_path_region(handle->id, node_index(1));                   // 001494c0 -> +1f8
            return std::nullopt;
        }
        if (name == "Teleport") {
            // 00149c30: 0015c120(node) (identity) -> +10c = 0014afe0: the teleport message (vtable 004d9360,
            // mode 0) through 003c9420 with transport 1, 0015ff90(+1b0, node +78), virtual +e0, +a0 = 0 and
            // 0014d510(c, 0, 0) (npc_teleport_to_node).
            applied = npcs->teleport(handle->id, node_index(1));
            return std::nullopt;
        }
        if (name == "SetMode") {
            if (function.parameters.size() == 3) {
                // 0014a4a0: lbu of the enum word, +dc = 0014bcf0(mode, node)
                applied = npcs->set_mode_at_node(handle->id, number(arguments.at(1)) & 0xffU, node_index(2));
                return std::nullopt;
            }
            npc_set_mode(*component, number(arguments.at(1)) & 0xffU);           // 0014a4f0: lbu, +dc = 0014bcf0(mode, 0)
            return std::nullopt;
        }
        if (name == "MoveToNode") {
            // 0014a5c0: 0015c120(node) (identity), +100 = 0014be50; the byte it returns.
            const auto result = npcs->move_to_node(handle->id, node_index(1));
            if (!result) throw std::runtime_error("MoveToNode on an NPC the world does not host: " + std::to_string(handle->id));
            return Word{*result};
        }
        if (name == "MoveToGobj") {
            // 0014a580: 003cc820 (identity), +104 = 0014be10: +f4 = the GOBJ's +c (0014be20 reads it, so a
            // null GOBJ is a null read), +138 = -1, mode 7 at priority 100; the byte it returns. The mode's
            // 0040c540 resolves actors and nodes here; a population GOB is not hosted.
            const auto* object = std::get_if<RuntimeObjectHandle>(&arguments.at(1));
            if (!object || object->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*object))
                throw std::runtime_error("MoveToGobj without a live GOBJ (0014be20 reads +c)");
            const auto section = objects_.sections.find(object->id);
            const bool node = std::find(node_ids_.begin(), node_ids_.end(), object->id) != node_ids_.end();
            if (!node && (section == objects_.sections.end() || section->second != "npc"))
                throw std::runtime_error("MoveToGobj toward a GOBJ that is neither an actor nor a node (not hosted): " +
                                         std::to_string(object->id));
            const auto result = npcs->move_to_object(handle->id, object->id);
            if (!result) throw std::runtime_error("MoveToGobj on an NPC the world does not host: " + std::to_string(handle->id));
            return Word{*result};
        }
        if (name == "CarryBodyToNode") {
            // Checkpoint 132zf9: 0014a520: 003cc820(gobj) (identity), 0015c120(node)
            // (identity) -> +108 = 0014bd60(c, gobj, node); the byte it returns (0).
            const auto* body = std::get_if<RuntimeObjectHandle>(&arguments.at(1));
            if (!body || body->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*body))
                throw std::runtime_error("CarryBodyToNode (0014bd60) without a live GOBJ");
            const auto result = npcs->carry_body_to_node(handle->id, body->id, node_index(2));
            if (!result) throw std::runtime_error("CarryBodyToNode (0014bd60) on an NPC the world does not host: " + std::to_string(handle->id));
            return Word{*result};
        }
        if (name == "Follow") {
            // Checkpoint 132zf11: 00148880: 003cc820(gobj) (identity), then the component's
            // +fc = 001488f0(c, gobj, arguments 2 / 3); returns 0. A null GOBJ: nothing.
            const auto* target = std::get_if<RuntimeObjectHandle>(&arguments.at(1));
            if (!target) throw std::runtime_error("Follow (001488f0) without an object argument");
            const auto who = objects_.objects.alive(*target) ? std::optional<Word>(target->id) : std::nullopt;
            applied = npcs->follow(handle->id, who, std::bit_cast<float>(number(arguments.at(2))),
                                   std::bit_cast<float>(number(arguments.at(3))));
            return std::nullopt;
        }
        // The GOBJ of AddAwareness / SetTarget (003cc820, identity): an actor
        // object; nullopt for the local player's.
        auto actor_object = [&](std::size_t index) -> std::optional<Word> {
            const auto* object = std::get_if<RuntimeObjectHandle>(&arguments.at(index));
            if (!object || object->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*object))
                throw std::runtime_error(name + " without a live GOBJ");
            if (object->id == objects_.player) return std::nullopt;
            const auto section = objects_.sections.find(object->id);
            if (section == objects_.sections.end() || section->second != "npc")
                throw std::runtime_error(name + " of a GOBJ that is not an actor (not hosted): " + std::to_string(object->id));
            return object->id;
        };
        if (name == "AddAwareness") {
            // 0016c9d0(p, object, 1, 0.0) with any GOBJ: a node object (vtable
            // 004d95b0) and a population GOB are hosted as well.
            const auto* object = std::get_if<RuntimeObjectHandle>(&arguments.at(1));
            if (object && object->kind == RuntimeObjectHandle::Kind::world_object && objects_.objects.alive(*object) &&
                std::find(node_ids_.begin(), node_ids_.end(), object->id) != node_ids_.end()) {
                applied = npcs->add_awareness_node(handle->id, node_index(1));
                return std::nullopt;
            }
            const auto section = object ? objects_.sections.find(object->id) : objects_.sections.end();
            if (section != objects_.sections.end() && section->second == "object" && objects_.objects.alive(*object)) {
                applied = npcs->add_awareness_object(handle->id, object->id);
                return std::nullopt;
            }
            applied = npcs->add_awareness(handle->id, actor_object(1));         // 0014a410 -> +e4 = 0014a450
            return std::nullopt;
        }
        if (name == "SetTarget") {
            // 0014a210: 0016ddb0(p, object, 100) with any GOBJ (003cc820 is the
            // identity); a node object (vtable 004d95b0) is hosted as well.
            const auto* object = std::get_if<RuntimeObjectHandle>(&arguments.at(1));
            if (object && object->kind == RuntimeObjectHandle::Kind::world_object && objects_.objects.alive(*object) &&
                std::find(node_ids_.begin(), node_ids_.end(), object->id) != node_ids_.end()) {
                applied = npcs->set_target_node(handle->id, node_index(1));
                return std::nullopt;
            }
            const auto section = object ? objects_.sections.find(object->id) : objects_.sections.end();
            // Checkpoint 132el8: an NIE: object as well (TOKYO's helicopter 1453573, tokyo::TargetHelicopterCheck).
            if (section != objects_.sections.end() && (section->second == "object" || section->second == "event_object") &&
                objects_.objects.alive(*object)) {
                applied = npcs->set_target_object(handle->id, object->id);         // a population GOB or an NIE
                return std::nullopt;
            }
            applied = npcs->set_target(handle->id, actor_object(1));
            return std::nullopt;
        }
        if (name == "SetGrenadeTarget") {
            // 00149810: 003cc820 (identity), +19c = 0014b410(c, GOBJ, 4.0, 0), which takes a null
            // GOBJ (+1cc = -1); returns 0. A node, a population GOB or an actor otherwise.
            MissionScriptNpcs::GobjTarget target;
            const auto* object = std::get_if<RuntimeObjectHandle>(&arguments.at(1));
            if (null_handle(arguments.at(1))) target.kind = MissionScriptNpcs::GobjTarget::Kind::null;
            else if (object && object->kind == RuntimeObjectHandle::Kind::world_object && objects_.objects.alive(*object) &&
                     std::find(node_ids_.begin(), node_ids_.end(), object->id) != node_ids_.end()) {
                target.kind = MissionScriptNpcs::GobjTarget::Kind::node;
                target.node = node_index(1);
            } else if (const auto section = object ? objects_.sections.find(object->id) : objects_.sections.end();
                       section != objects_.sections.end() && section->second == "object" && objects_.objects.alive(*object)) {
                target.kind = MissionScriptNpcs::GobjTarget::Kind::object;
                target.object = object->id;
            } else {
                target.kind = MissionScriptNpcs::GobjTarget::Kind::actor;
                target.actor = actor_object(1);
            }
            applied = npcs->set_grenade_target(handle->id, target);
            return std::nullopt;
        }
        if (name == "AddAwarenessOfPlayers") {
            applied = npcs->add_awareness_of_players(handle->id);               // 00149420 -> +ec = 0014aec0
            return std::nullopt;
        }
        if (name == "ResumeAITargeting") {
            applied = npcs->resume_ai_targeting(handle->id);                    // 0014a1c0: perception +1c = 0
            return std::nullopt;
        }
        if (name == "ResumeAIMode") {
            applied = npcs->resume_ai_mode(handle->id);                         // 0014a470 -> +e0 = 0014b570
            return std::nullopt;
        }
        if (name == "LookAround") {
            applied = npcs->look_around(handle->id);                            // 00149e50 -> +134 = 0014dd50(c, 100)
            return std::nullopt;
        }
        if (name == "StopAndLookAround") {
            applied = npcs->stop_and_look_around(handle->id);                           // 00149e20 -> +138 = 0014dca0(c, 100)
            return std::nullopt;
        }
        if (name == "RequestAction") {
            // 00149ce0: lbu of the enum word, a2 = 100, virtual +150 (vtable
            // 004d9380: 0014d8f0, f12 = 0) -> 0014d900; returns 0.
            applied = npcs->request_action(handle->id, static_cast<std::uint8_t>(number(arguments.at(1)) & 0xffU));
            return std::nullopt;
        }
        if (name == "RequestFacial" || name == "RequestLookAt") {
            // 00149e80: the float word goes to f12 as is (no check), the GOBJ
            // through 003cc820 (identity); +12c = 0014dea0(c, object, 100, f12).
            // RequestLookAt 00149ed0 the same with +128 = 0014dfb0 (entry 0).
            // A Node GOBJ is the node object itself (vtable 004d95b0; +c its ID);
            // a population GOB is the GOB (its +c0 0014d340 jumps to +e4); a
            // hotbox (BOX: record or MakeNPCInteractable) is the box (vtable
            // 004d9ac0, the same +c0).
            const bool look = name == "RequestLookAt";
            const float duration = std::bit_cast<float>(number(arguments.at(2)));
            const auto* object = std::get_if<RuntimeObjectHandle>(&arguments.at(1));
            const auto section = object ? objects_.sections.find(object->id) : objects_.sections.end();
            if (section != objects_.sections.end() && section->second == "node")
                applied = look ? npcs->request_look_at_node(handle->id, node_index(1), duration)
                               : npcs->request_facial_node(handle->id, node_index(1), duration);
            else if (section != objects_.sections.end() && section->second == "object" && objects_.objects.alive(*object))
                applied = look ? npcs->request_look_at_object(handle->id, object->id, duration)
                               : npcs->request_facial_object(handle->id, object->id, duration);   // a GOB: +c0 0014d340
            else if (section != objects_.sections.end() && section->second == "hotbox" && objects_.objects.alive(*object))
                applied = look ? npcs->request_look_at_hotbox(handle->id, object->id, duration)
                               : npcs->request_facial_hotbox(handle->id, object->id, duration);   // a box: +c0 0014d340
            else applied = look ? npcs->request_look_at(handle->id, actor_object(1), duration)
                                : npcs->request_facial(handle->id, actor_object(1), duration);
            return std::nullopt;
        }
        if (name == "ResumeAILookAt") {
            applied = npcs->resume_look_at(handle->id);                         // 00149df0 -> +13c = 0014de30(c, 100)
            return std::nullopt;
        }
        if (name == "DeactivateDead") {
            applied = npcs->deactivate_dead(handle->id, number(arguments.at(1)) != 0);   // 00149fe0: sltu a1, zero, v0
            return std::nullopt;
        }
        if (name == "EquipGoggles") {
            // 00149070: a1 = (word != 0), the component's +1f0 = 0014a7f0:
            // 001992b0(actor +30, a1, 0).
            applied = npcs->equip_goggles(handle->id, number(arguments.at(1)) != 0);
            return std::nullopt;
        }
        // Checkpoint 132sx8: the component writers (npc_ai.hpp).
        if (name == "SetAimMode") {
            request_npc_weapon_draw(*component, static_cast<std::uint8_t>(number(arguments.at(1)) & 0xffU), 100);   // 0014a190: lbu, +114 -> +bc
            return std::nullopt;
        }
        if (name == "SetAimSubtarget") {
            npc_set_aim_subtarget(*component, static_cast<std::int8_t>(number(arguments.at(1)) & 0xffU));   // 0014a150: lb, +118
            return std::nullopt;
        }
        if (name == "ResumeAIAiming") {
            npc_resume_ai_aiming(*component);                                   // 0014a100 -> +11c = 0014a130
            return std::nullopt;
        }
        if (name == "ResumeAITravelSpeed") {
            npc_resume_ai_travel_speed(*component);                             // 00149d70 -> +144 = 00149da0
            return std::nullopt;
        }
        if (name == "StoreWeapon") {
            applied = npcs->store_weapon(handle->id);                           // 00149a80 -> +1a8 = 0014bbd0
            return std::nullopt;
        }
        // Checkpoint 132sx10.
        if (ai_remove_weapon) {
            applied = npcs->remove_weapon(handle->id, word_signed(number(arguments.at(1))));   // 00149930: lw +4 -> +1bc = 0014b790
            return std::nullopt;
        }
        if (name == "SendNewAwarenessOfPlayers") {
            applied = npcs->send_new_awareness_of_players(handle->id);         // 0014a2f0 -> +f8 = 0014a320
            return std::nullopt;
        }
        if (name == "SetGrenadeThrowAngle") {
            component->throw_angle_94 = std::bit_cast<float>(number(arguments.at(1)));   // 00149710: lwc1 f12 -> +1a0 = 00149750: swc1 +94
            return std::nullopt;
        }
        if (name == "RemoveGroupFromPathRegion") {
            applied = npcs->remove_group_from_path_region(handle->id, node_index(1));              // 00149480 -> +1fc
            return std::nullopt;
        }
        // SetWeaponPreference(NPC, int, enum) 001498b0: a1 = lw +4, a2 = lbu +8
        // -> +1c4 = 0014b740: (a2 & 0xff) == 0 stores +b0, otherwise +b4.
        if (name == "SetWeaponPreference") {
            const auto item = static_cast<std::int32_t>(number(arguments.at(1)));
            if ((number(arguments.at(2)) & 0xffU) == 0) component->held_item_b0 = item;
            else component->held_item_b4 = item;
            return std::nullopt;
        }
        if (name == "SetNotTaserable") {
            applied = npcs->set_not_taserable(handle->id, number(arguments.at(1)) != 0);   // 00148fb0: sltu, +20c
            return std::nullopt;
        }
        if (name == "SetFireInvulnerability") {
            applied = npcs->set_fire_invulnerability(handle->id, number(arguments.at(1)) != 0);   // 00149010: sltu, +204
            return std::nullopt;
        }
        if (name == "StopMovement") {
            applied = npcs->stop_movement(handle->id, std::bit_cast<float>(number(arguments.at(1))));   // 00149ca0: f12 as is
            return std::nullopt;
        }
        if (name == "SetViewConeRadius") {
            applied = npcs->set_view_cone_radius(handle->id, std::bit_cast<float>(number(arguments.at(1))));   // 00149320 -> +180
            return std::nullopt;
        }
        if (name == "SetViewConeAngle") {
            applied = npcs->set_view_cone_angle(handle->id, std::bit_cast<float>(number(arguments.at(1))));    // 001492b0 -> +184
            return std::nullopt;
        }
        if (name == "ClearTarget") {
            applied = npcs->clear_target(handle->id);                           // 0014a1e0: 0016ddb0(p, 0, 100)
            return std::nullopt;
        }
        // The GOBJ of RemoveAwareness / ResendNewAwarenessMessage / InViewCone
        // (003cc820, identity), classified as AddAwareness's.
        auto ai_object = [&](std::size_t index) {
            MissionScriptNpcs::AiObject result;
            const auto* object = std::get_if<RuntimeObjectHandle>(&arguments.at(index));
            if (object && object->kind == RuntimeObjectHandle::Kind::world_object && objects_.objects.alive(*object) &&
                std::find(node_ids_.begin(), node_ids_.end(), object->id) != node_ids_.end()) {
                result.kind = MissionScriptNpcs::AiObject::Kind::node;
                result.node = node_index(index);
                return result;
            }
            const auto section = object ? objects_.sections.find(object->id) : objects_.sections.end();
            if (section != objects_.sections.end() && section->second == "object" && objects_.objects.alive(*object)) {
                result.kind = MissionScriptNpcs::AiObject::Kind::object;
                result.object = object->id;
                return result;
            }
            result.kind = MissionScriptNpcs::AiObject::Kind::actor;
            result.actor = actor_object(index);
            return result;
        };
        if (name == "RemoveAwareness") {
            applied = npcs->remove_awareness(handle->id, ai_object(1));         // 0014a3c0 -> +e8 = 0014a400
            return std::nullopt;
        }
        if (name == "ResendNewAwarenessMessage") {
            // 0014a390: +f4(c, 0); 0014a340: 003cc820(object), +f4(c, object).
            applied = npcs->resend_new_awareness(handle->id, function.parameters.size() == 2
                                                                 ? std::optional<MissionScriptNpcs::AiObject>(ai_object(1))
                                                                 : std::nullopt);
            return std::nullopt;
        }
        if (name == "InViewCone") {
            const auto result = npcs->in_view_cone(handle->id, ai_object(1));   // 00149270 -> +188 = 0014afc0, andi 0xff
            if (!result) {
                applied = false;
                throw std::runtime_error("InViewCone: the world does not host the NPC's perception: " + std::to_string(handle->id));
            }
            return Word{*result ? 1U : 0U};
        }
        throw std::runtime_error("AI native not dispatched: " + name);
    }
    // The PathedGOBJ natives (OBJECT_EVENT_CONTRACT.md; interpreter goldens
    // tests/pathed_object_native_goldens.hpp): the receiver is argument 0
    // itself. SetSpeed 00211190: +d4 = the float. SetPos 00211220: an index in
    // [0, points - 1] other than the current one clears +e0, sets the
    // direction, both targets and moving, and queues a PathedNotice (Moving,
    // the index) to the object (transport 0). GetPos 00211310: the index.
    // A world that does not tick the objects' update 00216c60
    // (MissionScriptWorld::pathed_objects_ticked) refuses SetPos and GetPos on
    // an object that is moving.
    // Trigger 00211350 (Checkpoint 132sx3): +e0 = 0, then a GOBJTriggerEventMsg
    // (003c9e30 key 004f5440, vtable 004daf10; +24 action 0, +28 0, +2c / +30
    // null) sent to the object itself with transport 0 (PathWorld::
    // script_trigger); the next pump offers it to the object's script filter
    // and, untaken, to the receiver 00219070 (PathMotion::trigger).
    // State 002111c0 (Checkpoint 132sx8): lbu +dc (the stopped byte), movz ->
    // 1 (Moving) while it is clear, else 0 (Stopped); golden native 3 of
    // tests/pathed_object_native_goldens.hpp. Like GetPos it is refused on a
    // moving object in a world that does not tick 00216c60.
    const bool pathed = function.parameters.size() >= 1 && script.type_key(function.parameters[0]) == "object:PathedGOBJ" &&
                        (name == "SetSpeed" || name == "SetPos" || name == "GetPos" || name == "Trigger" || name == "State");
    if (pathed) {
        if (name == "SetSpeed") require_signature(script, function, 4, {"object:PathedGOBJ", "primitive:2"});
        else if (name == "SetPos") require_signature(script, function, 4, {"object:PathedGOBJ", "primitive:1"});
        else if (name == "Trigger") require_signature(script, function, 4, {"object:PathedGOBJ"});
        else if (name == "State") {
            if (function.parameters.size() != 1 || script.type_key(function.return_type) != "enum:PathedGOBJ_ObjectStates")
                throw std::runtime_error("Unsupported native signature: " + script_native_signature(script, function));
        } else require_signature(script, function, 1, {"object:PathedGOBJ"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + "(PathedGOBJ) without a live object");
        const auto kind = objects_.kinds.find(handle->id);
        // Checkpoint 132zf13: GetPos on an ElevatorGOBJ (GOB kind 3, a PathedGOBJ specialised by 00215700).
        // 00211310 reads *(+d0); the lift constructor allocates its 0x30-byte state block, stores it at +128
        // (002159c8), copies the PathedGOBJ block's index and direction into it (002159e0..002159f0), frees
        // that block and stores the new one at +d0 too (00215aa8), so *(+d0) is the lift's floor index, the
        // word GetCurrentFloor 00211020 reads (+128 -> +0). BELARUS1's MILL OnAIWaypointReachedMsg (words
        // 4129..4131) asks it for lift 468145 when an escorted worker reaches waypoint 463663 / 463647.
        if (name == "GetPos" && kind != objects_.kinds.end() && kind->second == 3) {
            auto* lifts = world_.elevators();
            if (!lifts) throw std::runtime_error("GetPos(PathedGOBJ) on lift " + std::to_string(handle->id) + " without the world's lifts");
            return static_cast<Word>(static_cast<std::uint32_t>(lifts->at(handle->id).current_floor()));
        }
        if (kind == objects_.kinds.end() || kind->second != 2)
            throw std::runtime_error(name + "(PathedGOBJ) on a record that is not a kind-2 object: " + std::to_string(handle->id));
        auto* paths = world_.pathed_objects();
        if (!paths) {
            if (name == "GetPos" || name == "State")
                throw std::runtime_error(name + "(PathedGOBJ) without the world's pathed objects");
            applied = false;
            return std::nullopt;
        }
        const auto& motion = paths->motion(handle->id);
        if (name == "SetSpeed") {
            paths->script_set_speed(handle->id, std::bit_cast<float>(number(arguments.at(1))));
            return std::nullopt;
        }
        if (name == "Trigger") {
            paths->script_trigger(handle->id);
            return std::nullopt;
        }
        if (motion.moving() && !world_.pathed_objects_ticked())
            throw std::runtime_error(name + "(PathedGOBJ) on moving object " + std::to_string(handle->id) +
                                     ": the world does not run its update 00216c60");
        if (name == "GetPos") return static_cast<Word>(static_cast<std::uint32_t>(motion.position()));
        if (name == "State") return Word{motion.moving() ? 1U : 0U};
        paths->script_seek(handle->id, word_signed(number(arguments.at(1))));
        return std::nullopt;
    }
    // Checkpoint 132zf11: game::CreateTank 002840d0 (-> 00284100) and the Tank
    // natives (registrations 004d28b0; research/TANKS.md, tank.hpp). The
    // receiver is argument 0 (lw s0, (a0)); NIE arguments go through the
    // identity 002379d0, the GOBJ of AimAt through 003cc820 (the identity).
    if (name == "game::CreateTank") {
        if (function.parameters.size() != 1 || script.type_key(function.parameters[0]) != "object:NIEventOBJ" ||
            function.return_type < 100 || script.type_key(function.return_type) != "object:Tank")
            throw std::runtime_error("Unsupported native signature: " + script_native_signature(script, function));
        const auto* body = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!body || body->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*body))
            throw std::runtime_error("game::CreateTank 002840d0 without a live NIEventOBJ");
        const auto section = objects_.sections.find(body->id);
        if (section == objects_.sections.end() || section->second != "event_object")
            throw std::runtime_error("game::CreateTank 002840d0 on a record that is not an NIE: " + std::to_string(body->id));
        auto* tanks = world_.tanks();
        auto* host = world_.tank_host();
        if (!tanks || !host) throw std::runtime_error("game::CreateTank 002840d0: the tanks (00284100) are not hosted");
        const auto tank = create_world_object(0xf9);                     // 0040c240(&id, 0xf9)
        if (!objects_.sections.emplace(tank.id, "tank").second)
            throw std::runtime_error("game::CreateTank: tank ID already in the population: " + std::to_string(tank.id));
        tanks->create(tank.id, body->id, world_.tank_handle(tank.id), *host);
        return Value{tank};
    }
    if (function.parameters.size() >= 1 && script.type_key(function.parameters[0]) == "object:Tank") {
        auto nie = [&](std::size_t index) {
            const auto* h = std::get_if<RuntimeObjectHandle>(&arguments.at(index));
            if (!h || h->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*h))
                throw std::runtime_error(name + "(Tank) without a live NIEventOBJ argument");
            const auto s = objects_.sections.find(h->id);
            if (s == objects_.sections.end() || s->second != "event_object")
                throw std::runtime_error(name + "(Tank) with a record that is not an NIE: " + std::to_string(h->id));
            return h->id;
        };
        auto real = [&](std::size_t index) { return std::bit_cast<float>(number(arguments.at(index))); };
        const std::vector<std::string> nies{"object:Tank", "object:NIEventOBJ", "object:NIEventOBJ", "object:NIEventOBJ"};
        if (name == "SetTurret" && function.parameters.size() == 5)
            require_signature(script, function, 4, {"object:Tank", "object:NIEventOBJ", "object:NIEventOBJ", "object:NIEventOBJ",
                                                    "object:NIEventOBJ"});
        else if (name == "SetTurret") require_signature(script, function, 4, nies);
        else if (name == "SetLeftTread" || name == "SetRightTread")
            require_signature(script, function, 4, {"object:Tank", "object:NIEventOBJ", "object:NIEventOBJ"});
        else if (name == "AimAt") require_signature(script, function, 4, {"object:Tank", "object:GOBJ"});
        else if (name == "ResumeAiming" || name == "ClearEnemy" || name == "NetCreate" || name == "Reset" || name == "StopFiring")
            require_signature(script, function, 4, {"object:Tank"});
        else if (name == "SetRotLimit" || name == "SetPitchLimit")
            require_signature(script, function, 4, {"object:Tank", "primitive:2", "primitive:2"});
        else if (name == "Move" || name == "SetTurretAccel" || name == "SetFiringCone")
            require_signature(script, function, 4, {"object:Tank", "primitive:2"});
        else if (name == "SetWeapon" || name == "SetHitpoints") require_signature(script, function, 4, {"object:Tank", "primitive:1"});
        else if (name == "GetHitpoints") require_signature(script, function, 1, {"object:Tank"});
        else if (name == "GetGobj") {
            if (function.parameters.size() != 1 || function.return_type < 100 || script.type_key(function.return_type) != "object:GOBJ")
                throw std::runtime_error("Unsupported native signature: " + script_native_signature(script, function));
        } else throw std::runtime_error("Unhosted Tank native " + script_native_signature(script, function));
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + "(Tank) without a live tank");
        auto* tanks = world_.tanks();
        auto* host = world_.tank_host();
        if (!tanks || !host) throw std::runtime_error(name + "(Tank) without the world's tanks");
        auto& tank = tanks->at(handle->id);
        if (name == "SetTurret") {                                       // 00283410 / 002835a0
            const auto muzzle = function.parameters.size() == 5 ? std::optional<std::uint32_t>(nie(4)) : std::nullopt;
            tank.set_turret(nie(1), nie(2), nie(3), muzzle);
        } else if (name == "SetLeftTread") tank.set_left_tread(nie(1), nie(2), *host);    // 002832a0
        else if (name == "SetRightTread") tank.set_right_tread(nie(1), nie(2), *host);    // 00283130
        else if (name == "SetPitchLimit") tank.set_pitch_limit(real(1), real(2));          // 00282f30
        else if (name == "SetRotLimit") tank.set_rot_limit(real(1), real(2));              // 00282f90
        else if (name == "SetTurretAccel") tank.set_turret_accel(real(1));                 // 00282d90
        else if (name == "SetFiringCone") tank.set_firing_cone(real(1));                   // 00282d40
        else if (name == "SetWeapon") tank.set_weapon(word_signed(number(arguments.at(1))));   // 00282dc0
        else if (name == "Move") tank.move(real(1), *host);                                // 00282e10
        else if (name == "ResumeAiming") tank.resume_aiming();                             // 00283020
        else if (name == "ClearEnemy") tank.clear_enemy();                                 // 00282ff0
        else if (name == "StopFiring") tank.stop_firing();                                 // 00282af0
        else if (name == "Reset") tank.reset(*host);                                       // 00282bb0
        else if (name == "NetCreate") tank.net_create(*host);                              // 00282c00
        else if (name == "AimAt") {                                                        // 002830f0
            const auto* who = std::get_if<RuntimeObjectHandle>(&arguments.at(1));
            if (!who) throw std::runtime_error("AimAt(Tank, GOBJ) without an object");
            if (!objects_.objects.alive(*who)) {
                tank.aim_at({});                                         // a null GOBJ: 00285590(tank, 0)
                return std::nullopt;
            }
            const auto target = world_.tank_target(who->id);
            if (!target) throw std::runtime_error("AimAt(Tank, GOBJ) 002830f0: the target " + std::to_string(who->id) + " is not hosted");
            tank.aim_at(*target);
        } else if (name == "SetHitpoints") {                             // 00282d00: 003cc990(body, hp, 0)
            object_health_40_[tank.body()] = gobj_set_hit_points(word_signed(number(arguments.at(1))));
        } else if (name == "GetHitpoints") {                             // 00282ab0: cvt.w.s(body +40)
            const auto health = object_health_40(tank.body());
            if (!health)
                throw std::runtime_error("GetHitpoints(Tank) 00282ab0: the body's +40 read before any SetHitpoints: " +
                                         std::to_string(tank.body()));
            return Word{static_cast<std::uint32_t>(gobj_hit_points(*health))};
        } else return Value{objects_.objects.resolve(tank.body() | 0x80000000U)};   // GetGobj 00282ae0 -> 003cc800(body)
        return std::nullopt;
    }
    // The ElevatorGOBJ natives (research/ELEVATORS.md; the receiver is
    // argument 0, a kind-3 lift, ElevatorGOBJ 004db040): SetFloorName 00210e30
    // -> 00215010(floor, name); Set{Loop,Stop,Start,Ding}Sound 00210d30 /
    // 00210d70 / 00210db0 / 00210df0 -> 001ca010(+2e0 / +600 / +470 / +150,
    // 0, name, 0); LockFloor 00210d00 -> 002155f0; UnlockFloor 00210cd0 ->
    // 00215530; SeekFloor 002110e0 -> 00214c90(floor, word != 0, 0);
    // GetCurrentFloor 00211020 (+128 -> +0); GetDestFloor 00211040 (+128 ->
    // +20); GetState 00211080 (00214fa0 & 0xff); ChooseFloor 00210e70 (the
    // player: 00214c90(-1, 1, 0), the floor chooser; other GOBJs: 003fb020,
    // nothing solo). Passengers 00211060 (slt: +88 > 0; the rider list +84
    // only ever holds dropped backpacks, ElevatorGOBJ::passengers).
    const bool lift_native =
        function.parameters.size() >= 1 && script.type_key(function.parameters[0]) == "object:ElevatorGOBJ" &&
        (name == "SetFloorName" || name == "SetLoopSound" || name == "SetStopSound" || name == "SetStartSound" ||
         name == "SetDingSound" || name == "LockFloor" || name == "UnlockFloor" || name == "SeekFloor" ||
         name == "GetCurrentFloor" || name == "GetDestFloor" || name == "GetState" || name == "ChooseFloor" ||
         name == "Passengers");
    if (lift_native) {
        if (name == "SetFloorName") require_signature(script, function, 4, {"object:ElevatorGOBJ", "primitive:1", "primitive:3"});
        else if (name == "SeekFloor") require_signature(script, function, 4, {"object:ElevatorGOBJ", "primitive:1", "primitive:1"});
        else if (name == "LockFloor" || name == "UnlockFloor")
            require_signature(script, function, 4, {"object:ElevatorGOBJ", "primitive:1"});
        else if (name == "ChooseFloor") require_signature(script, function, 4, {"object:ElevatorGOBJ", "object:GOBJ"});
        else if (name == "GetCurrentFloor" || name == "GetDestFloor" || name == "Passengers")
            require_signature(script, function, 1, {"object:ElevatorGOBJ"});
        else if (name == "GetState") {
            require_signature(script, function, function.return_type, {"object:ElevatorGOBJ"});
            if (script.type_key(function.return_type) != "enum:ElevatorGOBJ_ElevatorStates")
                throw std::runtime_error("Unsupported native signature: " + script_native_signature(script, function));
        }
        else require_signature(script, function, 4, {"object:ElevatorGOBJ", "primitive:3"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + "(ElevatorGOBJ) without a live object");
        const auto kind = objects_.kinds.find(handle->id);
        if (kind == objects_.kinds.end() || kind->second != 3)
            throw std::runtime_error(name + "(ElevatorGOBJ) on a record that is not a kind-3 object: " + std::to_string(handle->id));
        auto* lifts = world_.elevators();
        if (!lifts) {
            if (function.return_type != 4) throw std::runtime_error(name + "(ElevatorGOBJ) without the world's lifts");
            applied = false;
            return std::nullopt;
        }
        auto& lift = lifts->at(handle->id);
        auto text = [&](std::size_t index) {
            const auto* value = std::get_if<std::string>(&arguments.at(index));
            if (!value) throw std::runtime_error(name + "(ElevatorGOBJ) without a string argument");
            return *value;
        };
        if (name == "SetFloorName") lift.set_floor_name(word_signed(number(arguments.at(1))), text(2));
        else if (name == "SetLoopSound") lift.set_sound(LiftSound::Loop, text(1));
        else if (name == "SetStopSound") lift.set_sound(LiftSound::Stop, text(1));
        else if (name == "SetStartSound") lift.set_sound(LiftSound::Start, text(1));
        else if (name == "SetDingSound") lift.set_sound(LiftSound::Ding, text(1));
        else if (name == "LockFloor") lift.lock_floor(word_signed(number(arguments.at(1))));
        else if (name == "UnlockFloor") lift.unlock_floor(word_signed(number(arguments.at(1))));
        else if (name == "SeekFloor") lift.seek_floor(word_signed(number(arguments.at(1))), number(arguments.at(2)) != 0);
        else if (name == "ChooseFloor") {
            // 00210e70: a GOBJ (003cc820) whose +14 is set and whose +58 controller's vt+54 answers 1 (the
            // player) gets the chooser, 00214c90(lift, -1, 1, 0). Any other GOBJ builds a 004daf10 trigger
            // (+24 = 1, +28 = -1, +30 = its +c) for 003fb020(lift +c, msg, 0, 0): with the network byte
            // 005721d0 clear that only serializes it (vt+10 002118d0) and returns, so nothing happens solo.
            const auto* who = std::get_if<RuntimeObjectHandle>(&arguments.at(1));
            if (who && who->kind == RuntimeObjectHandle::Kind::world_object && objects_.objects.alive(*who) &&
                who->id == objects_.player)
                lift.seek_floor(-1, true);
        }
        else if (name == "GetCurrentFloor") return static_cast<Word>(static_cast<std::uint32_t>(lift.current_floor()));
        else if (name == "GetDestFloor") return static_cast<Word>(static_cast<std::uint32_t>(lift.destination_floor()));
        else if (name == "Passengers") return static_cast<Word>(lift.passengers() ? 1U : 0U);  // slt v0, zero, +88
        else return static_cast<Word>(static_cast<std::uint32_t>(lift.get_state()) & 0xffU);
        return std::nullopt;
    }
    // The Generator natives (cGenerator 004db680, npc_generator.hpp; wrappers
    // registered from 004cd730, the receiver is argument 0 itself). With the
    // multiplayer byte 005721c8 clear each is one field write and returns 0:
    // Enable 0021cf40 / EnableAnon 0021cc60 / EnableBy 0021cdd0 -> +64(-1) =
    // 00220fd0 (+2c = 1); Disable 0021cf10 / DisableAnon 0021cc30 / DisableBy
    // 0021cc90 -> +68(-1) = 00220d70 (00220e40: +2c = 0); Pause 0021cb90
    // (+d1 = 1); Unpause 0021caf0 (+d1 = 0); ResetPulse 0021c9a0 (+c0 = 0);
    // SetMinSpawnDistance 0021ca30 (+b8 = d * d). EnableBy / DisableBy pass the
    // GOBJ through 003cc820 (identity) and read it only in multiplayer. A world
    // without generators defers them; the other Generator natives (GetNPC of
    // SpawnedNPCMsg, ...) keep the generic path below.
    const bool generator_native =
        function.parameters.size() >= 1 && script.type_key(function.parameters[0]) == "object:Generator" &&
        (name == "Enable" || name == "Disable" || name == "EnableBy" || name == "DisableBy" || name == "EnableAnon" ||
         name == "DisableAnon" || name == "Pause" || name == "Unpause" || name == "ResetPulse" || name == "SetMinSpawnDistance");
    if (generator_native) {
        if (name == "EnableBy" || name == "DisableBy") require_signature(script, function, 4, {"object:Generator", "object:GOBJ"});
        else if (name == "SetMinSpawnDistance") require_signature(script, function, 4, {"object:Generator", "primitive:2"});
        else require_signature(script, function, 4, {"object:Generator"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + "(Generator) without a live generator");
        const auto section = objects_.sections.find(handle->id);
        if (section == objects_.sections.end() || section->second != "generator")
            throw std::runtime_error(name + "(Generator) on a record that is not a generator: " + std::to_string(handle->id));
        auto* generators = world_.generators();
        if (!generators) {
            applied = false;
            return std::nullopt;
        }
        auto* generator = generators->find(handle->id);
        if (!generator) throw std::runtime_error(name + "(Generator) on a generator the world does not host: " + std::to_string(handle->id));
        const bool enable = name == "Enable" || name == "EnableAnon" || name == "EnableBy";
        const bool disable = name == "Disable" || name == "DisableAnon" || name == "DisableBy";
        // 132ln5h11: in a network game, +64(-1) = 00220fd0 / +68(-1) = 00220d70 take their network paths.
        if ((enable || disable) && generators->network_host) {
            const auto net = generators->network_host();
            if (enable) generator_enable_network(*generator, -1, net);
            else generator_disable_network(*generator, -1, net);
        } else if (enable) generator_enable(*generator);
        else if (disable) generator_disable(*generator);
        else if (name == "Pause") generator_pause(*generator);
        else if (name == "Unpause") generator_unpause(*generator);
        else if (name == "ResetPulse") generator_reset_pulse(*generator);
        else generator_set_min_spawn_distance(*generator, std::bit_cast<float>(number(arguments.at(1))));
        return std::nullopt;
    }
    // Enable / Disable(FireObj) 00261d60 / 00261d30: `lw a0,(a0)`, virtual
    // +64 / +68 with a1 = -1, return 0. +64 = 00261c00 builds and sends a
    // message (vtable 004e0880, 003ff930) only when networked (*005721c8),
    // then +2c = 1; +68 = 00261c90 builds a stack message (003c9e30 writes
    // only it), offers it to 003ff650 (returns at once in solo), +2c = 0 and
    // 003c9d60(msg, 0) (no free). A world without fire zones defers them.
    if ((name == "Enable" || name == "Disable") && function.parameters.size() == 1 &&
        script.type_key(function.parameters[0]) == "object:FireObj") {
        require_signature(script, function, 4, {"object:FireObj"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + "(FireObj) without a live fire zone");
        const auto section = objects_.sections.find(handle->id);
        if (section == objects_.sections.end() || section->second != "fire")
            throw std::runtime_error(name + "(FireObj) on a record that is not a FIR: zone: " + std::to_string(handle->id));
        applied = world_.set_fire_enabled(handle->id, name == "Enable");
        return std::nullopt;
    }
    // Enable / Disable(PARTICLE_GOBJ) 00392640 / 00392610 and
    // ScaleBBox(PARTICLE_GOBJ, float) 003925d0: `lw a0,(a0)`, virtual +64 /
    // +68 with a1 = -1 or +8c with f12, return 0. On the PLR: placements'
    // vtable 004daa10: +64 = 001c5710 / +68 = 001c5680 build and send a
    // message (003ff930 / 003ff650) only when networked (*005721c8), then
    // +2c = 1 / 0; +8c = 001c5670 stores +70 (the bounds scale vt+88 003943a0
    // applies). A world without placements defers them.
    if ((name == "Enable" || name == "Disable" || name == "ScaleBBox") && !function.parameters.empty() &&
        script.type_key(function.parameters[0]) == "object:PARTICLE_GOBJ") {
        if (name == "ScaleBBox") require_signature(script, function, 4, {"object:PARTICLE_GOBJ", "primitive:2"});
        else require_signature(script, function, 4, {"object:PARTICLE_GOBJ"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + "(PARTICLE_GOBJ) without a live particle object");
        const auto section = objects_.sections.find(handle->id);
        if (section == objects_.sections.end() || section->second != "placement")
            throw std::runtime_error(name + "(PARTICLE_GOBJ) on a record that is not a PLR: placement (its vtable is not "
                                            "004daa10; not hosted): " + std::to_string(handle->id));
        applied = name == "ScaleBBox" ? world_.scale_placement_bbox(handle->id, std::bit_cast<float>(number(arguments.at(1))))
                                      : world_.set_placement_enabled(handle->id, name == "Enable");
        return std::nullopt;
    }
    // Trigger(SoundGOBJ) 00210940: `lw v1,(a0)`, owner = +1f2 ? the object : 0;
    // unless +1f0, +1f0 = 1 and 0020f990(msg, +60, owner) is sent with
    // 003c9420(msg, 004ffb50, 1) (the sb 1 to msg+20 behind `bnez 001c6700`
    // never runs). KillSnd(SoundGOBJ) 00210890: owner = +1f2 ? the object :
    // (*004ffc0c ? 0 : the object), 0020eba0 sent likewise, +1f0 = 0. Both
    // return 0. A world without sound objects defers them.
    if ((name == "Trigger" || name == "KillSnd") && function.parameters.size() == 1 &&
        script.type_key(function.parameters[0]) == "object:SoundGOBJ") {
        require_signature(script, function, 4, {"object:SoundGOBJ"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + "(SoundGOBJ) without a live sound object");
        const auto section = objects_.sections.find(handle->id);
        if (section == objects_.sections.end() || section->second != "sound")
            throw std::runtime_error(name + "(SoundGOBJ) on a record that is not an SND: object: " + std::to_string(handle->id));
        applied = world_.sound_object(handle->id, name == "Trigger");
        return std::nullopt;
    }
    // Enable / Disable(GameGOBJ) 002114a0 / 00211470: `lw a0,(a0)`, virtual
    // +64 / +68 with a1 = -1, return 0. The GOB vtables (004db4c0 kinds 1 / 14,
    // 004db280 kind 2, 004db160 kind 5, 004db3a0 kind 6) all give +64 =
    // 0021aa20 / +68 = 0021a940. Solo (*005721c8 == 0) skips the network
    // message; +2c = 1 / 0 (0021aa94 / 0021a9b0), then virtual +64 / +68 (-1)
    // on each of the first +78 entries of the vector at +74 (capacity +74,
    // size +78, data +7c; 0021a9b4..0021a9f8). The loader 0021add0 builds it
    // with 001365c0 (all zero), reserves the second pair list's count with
    // 001396d0 (writes capacity and data only) and stores each 0040c480 result
    // straight into data[i] (0021b3e4..0021b3f4); nothing writes +78, so the
    // loop runs zero times and only the object's own +2c changes (a -1 pair's
    // null entry is never dereferenced). Other population kinds defer.
    if ((name == "Enable" || name == "Disable") && function.parameters.size() == 1 &&
        script.type_key(function.parameters[0]) == "object:GameGOBJ") {
        require_signature(script, function, 4, {"object:GameGOBJ"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + "(GameGOBJ) without a live object");
        const auto section = objects_.sections.find(handle->id);
        // A BOX: record passed as a GameGOBJ (TORONTO3 MALL's SwatCheck enables
        // 402142 "p_dropbox" so): 002114a0 makes the same virtual +64 / +68 (-1)
        // call as Enable / Disable(Hotbox) 0016fcd0, i.e. the hotbox's own
        // 0016fd00 / 0016fbe0 (checkpoint 132zf7).
        if (section != objects_.sections.end() && section->second == "hotbox") {
            applied = world_.set_hotbox_enabled(handle->id, name == "Enable");
            return std::nullopt;
        }
        const auto kind = objects_.kinds.find(handle->id);
        static constexpr std::array<std::uint32_t, 5> gob_kinds{1, 2, 5, 6, 14};
        if (section == objects_.sections.end() || section->second != "object" || kind == objects_.kinds.end() ||
            std::find(gob_kinds.begin(), gob_kinds.end(), kind->second) == gob_kinds.end()) {
            applied = false;
            return std::nullopt;
        }
        enabled_2c_[handle->id] = name == "Enable";
        return std::nullopt;
    }
    // Play(NIEventOBJ) 00237960 / Play(NIEventLight) 002375b0: `lw a0,(a0)`,
    // virtual +114 (0023e820 in both vtables 004dc050 / 004dbed0), return 0.
    if (name == "Play" && function.parameters.size() == 1 &&
        (script.type_key(function.parameters[0]) == "object:NIEventOBJ" || script.type_key(function.parameters[0]) == "object:NIEventLight")) {
        const bool light = script.type_key(function.parameters[0]) == "object:NIEventLight";
        require_signature(script, function, 4, {light ? "object:NIEventLight" : "object:NIEventOBJ"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error(std::string("Play(") + (light ? "NIEventLight" : "NIEventOBJ") + ") without a live event object");
        const auto section = objects_.sections.find(handle->id);
        if (section == objects_.sections.end() || section->second != (light ? "event_light" : "event_object"))
            throw std::runtime_error(std::string("Play(") + (light ? "NIEventLight" : "NIEventOBJ") + ") on a record that is not an " +
                                     (light ? "NIL:" : "NIE:") + " object: " + std::to_string(handle->id));
        applied = world_.play_event_object(handle->id, light);
        return std::nullopt;
    }
    // Reset(NIEventOBJ) 00237900: `lw a0,(a0)`, virtual +11c = 0023e720 (the SHUTTLE message 004f7c28 with +24 = 2,
    // queued by 003c9420 a2 = 0), return 0 (Checkpoint 132zf4; SEWER's power generators).
    // Stop(NIEventOBJ) 00237930: `lw a0,(a0)`, virtual +118 = 0023e790 with a1 = 1 (the SHUTTLE message with
    // +24 = 1; +20 is set only for a1 = 0), return 0 (Checkpoint 132sx10).
    if ((name == "Reset" || name == "Stop") && function.parameters.size() == 1 &&
        script.type_key(function.parameters[0]) == "object:NIEventOBJ") {
        require_signature(script, function, 4, {"object:NIEventOBJ"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + "(NIEventOBJ) without a live event object");
        const auto section = objects_.sections.find(handle->id);
        if (section == objects_.sections.end() || section->second != "event_object")
            throw std::runtime_error(name + "(NIEventOBJ) on a record that is not an NIE: object: " + std::to_string(handle->id));
        applied = name == "Reset" ? world_.reset_event_object(handle->id) : world_.stop_event_object(handle->id);
        return std::nullopt;
    }
    // Checkpoint 132uk3 (UKRAINE base::TankOff, BASE words 7038..7046): Disable / Enable(NIEventOBJ) 002378a0 /
    // 002378d0: `lw a0,(a0)`, virtual +68 / +64 with a1 = -1 (vtable 004dc050: 0023bdc0 / 0023be50), return 0.
    if ((name == "Enable" || name == "Disable") && function.parameters.size() == 1 &&
        script.type_key(function.parameters[0]) == "object:NIEventOBJ") {
        require_signature(script, function, 4, {"object:NIEventOBJ"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + "(NIEventOBJ) without a live event object");
        const auto section = objects_.sections.find(handle->id);
        if (section == objects_.sections.end() || section->second != "event_object")
            throw std::runtime_error(name + "(NIEventOBJ) on a record that is not an NIE: object: " + std::to_string(handle->id));
        applied = world_.enable_event_object(handle->id, name == "Enable");
        return std::nullopt;
    }
    // Checkpoint 132zf9 (BELARUS1 MILL, ZURICH init::StartLevel): SetEndState(NIEventOBJ) 00237870, its one
    // signature: `lw a0,(a0)`, virtual +128 = 0023b4c0 (the tracks stepped to their last keys and the rotations
    // composed), return 0. Checkpoint 132sx12: ported (NiEventWorld::end_state); deferred without the world's
    // event objects.
    if (name == "SetEndState") {
        require_signature(script, function, 4, {"object:NIEventOBJ"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error("SetEndState(NIEventOBJ) without a live event object");
        const auto section = objects_.sections.find(handle->id);
        if (section == objects_.sections.end() || section->second != "event_object")
            throw std::runtime_error("SetEndState(NIEventOBJ) on a record that is not an NIE: object: " + std::to_string(handle->id));
        applied = world_.end_state_event_object(handle->id);
        return std::nullopt;
    }
    auto box = [&](const char* expected_return) -> std::optional<Word> {
        if (function.parameters.size() != 1 || script.type_key(function.parameters[0]) != "object:Hotbox") return {};
        if (script.type_key(function.return_type) != expected_return)
            throw std::runtime_error("Unsupported hotbox native: " + script_native_signature(script, function));
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error("Hotbox native without a live box: " + name);
        const auto section = objects_.sections.find(handle->id);
        if (section == objects_.sections.end() || section->second != "hotbox")
            throw std::runtime_error("Hotbox native on a non-hotbox record: " + std::to_string(handle->id));
        return handle->id;
    };
    if (name == "Enable" || name == "Disable") {
        if (const auto id = box("primitive:4")) {
            applied = world_.set_hotbox_enabled(*id, name == "Enable");
            return std::nullopt;
        }
    }
    if (name == "Occupied") {
        if (const auto id = box("primitive:1")) {
            const auto occupied = world_.hotbox_occupied(*id);
            if (!occupied) throw std::runtime_error("Occupied(Hotbox) on a box the world does not host: " + std::to_string(*id));
            return Word{*occupied ? 1U : 0U};
        }
    }
    // SetInteractMessage(Hotbox, key) 0016fb70: +0xa0 = 003fdb50(key) (the
    // localized string, NULL when the key is missing); ClearInteractMessage
    // (Hotbox) 0016fb60: +0xa0 = 0. Both return 0. Deferred without the
    // localization or a hosted box.
    if (name == "SetInteractMessage" || name == "ClearInteractMessage") {
        const bool set = name == "SetInteractMessage";
        require_signature(script, function, 4,
                          set ? std::vector<std::string>{"object:Hotbox", "primitive:7"} : std::vector<std::string>{"object:Hotbox"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + " without a live box");
        const auto section = objects_.sections.find(handle->id);
        if (section == objects_.sections.end() || section->second != "hotbox")
            throw std::runtime_error(name + " on a non-hotbox record: " + std::to_string(handle->id));
        std::optional<std::string> message;
        if (set) {
            const auto* key = std::get_if<std::string>(&arguments.at(1));
            if (!key) throw std::runtime_error(name + " without a key string");
            const auto& localization = objectives_.inputs().localization;
            if (!localization) {
                applied = false;
                return std::nullopt;
            }
            if (const auto resolved = localization->resolve(*key)) message = std::string(*resolved);
        }
        applied = world_.set_hotbox_interact_message(handle->id, std::move(message));
        return std::nullopt;
    }
    // Owner lookups, all `lw v0,(a0); lw a0,+30(v0); j 003cc800` (identity):
    // GetGOBJ(AI) 0016ab80, Gobj(GameGobjController) 0022f100 and
    // GetPlayerObject(Player) 00175ee0 return the controller's +0x30 owner
    // GOBJ (written by 002300d0 through slot +0x8c).
    const bool owner_lookup =
        function.parameters.size() == 1 &&
        ((name == "GetGOBJ" && script.type_key(function.parameters[0]) == "object:AI") ||
         (name == "Gobj" && script.type_key(function.parameters[0]) == "object:GameGobjController") ||
         (name == "GetPlayerObject" && script.type_key(function.parameters[0]) == "object:Player"));
    if (owner_lookup) {
        if (function.return_type < 100 || script.type_key(function.return_type) != "object:GOBJ")
            throw std::runtime_error("Unsupported native signature: " + script_native_signature(script, function));
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle) throw std::runtime_error(name + " requires a controller handle");
        if (name == "GetGOBJ" && handle->kind != RuntimeObjectHandle::Kind::ai)
            throw std::runtime_error("GetGOBJ(AI) requires an AI controller handle");
        if (name == "GetPlayerObject" && handle->kind != RuntimeObjectHandle::Kind::player)
            throw std::runtime_error("GetPlayerObject(Player) requires a player controller handle");
        return Value(objects_.objects.get_gobj(*handle));
    }
    // 003cc580: the GOBJ's +0x58 controller (null -> -1, never produced by a
    // hosted GOBJ), virtual +0x68. Player component (004d9cf0) +68 =
    // 00175340: 0 while the multiplayer byte 005721c8 is clear. AI component
    // (004d9380) +68 = 0015b800: lw v0,0x54(a0), the team SetTeam writes
    // (npc_team). The population attaches the player component to the player
    // record and an AI component to the other "npc" records; other GOBJ
    // classes' +0x58 are not read and throw.
    // The GOBJ's +0x58 controller team (virtual +0x68); nullopt = null controller.
    const auto gobj_controller_team = [&](const Value& value, const std::string& what) -> std::optional<std::int32_t> {
        const auto* handle = std::get_if<RuntimeObjectHandle>(&value);
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error(what + " without a live GOBJ");
        if (handle->id == objects_.player) return 0;
        const auto section = objects_.sections.find(handle->id);
        // A node (constructor 0015c880, vtable 004d95b0) never writes +58 after the GOBJ base 003cf0e0 zeroes it.
        if (section != objects_.sections.end() && section->second == "node") return std::nullopt;
        if (section == objects_.sections.end() || section->second != "npc")
            throw std::runtime_error(what + " of a GOBJ that is not an actor (+58 not hosted): " + std::to_string(handle->id));
        auto* npcs = world_.npcs();
        if (!npcs) throw std::runtime_error(what + " of an NPC while NPCs are not hosted: " + std::to_string(handle->id));
        const auto* component = npcs->component(handle->id);
        if (!component) throw std::runtime_error(what + " of an NPC the world does not host: " + std::to_string(handle->id));
        return npc_team(*component);
    };
    // IsNPC 003cc710 and game::CastNPC 00169ac0 (003cc820 lookup; 132sx10,
    // MINSK / BELARUS1): the GOBJ's +58 controller, when present, answering
    // its virtual +58 (CastNPC: or +5c). The AI component 004d9380 has +58 =
    // 0015b7e0 (1) and +5c = 001535d0 (0); the player component 004d9cf0 has
    // +58 = 00140e10 (0) and +5c = 001535d0. IsNPC gives that bit; CastNPC the
    // controller itself (0014a670 is the identity) or null. A node's +58 is
    // null; the +58 of other GOBJ classes is not hosted (throws).
    if ((name == "IsNPC" || name == "game::CastNPC") && function.parameters.size() == 1 &&
        script.type_key(function.parameters[0]) == "object:GOBJ") {
        if (name == "IsNPC") require_signature(script, function, 1, {"object:GOBJ"});
        else if (function.return_type < 100 || script.type_key(function.return_type) != "object:NPC")
            throw std::runtime_error("Unsupported native signature: " + script_native_signature(script, function));
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + " without a live GOBJ (the original reads +58 of null)");
        bool npc = false;
        if (handle->id != objects_.player) {
            const auto section = objects_.sections.find(handle->id);
            if (section == objects_.sections.end() || (section->second != "npc" && section->second != "node"))
                throw std::runtime_error(name + " of a GOBJ that is not an actor (+58 not hosted): " + std::to_string(handle->id));
            npc = section->second == "npc";
        }
        if (name == "IsNPC") return Word{npc ? 1U : 0U};
        if (!npc) return Value(RuntimeObjectHandle{});
        const auto controller = objects_.objects.resolve(handle->id);
        if (controller.kind != RuntimeObjectHandle::Kind::ai || !objects_.objects.alive(controller))
            throw std::runtime_error("game::CastNPC: the NPC's AI component is not registered: " + std::to_string(handle->id));
        return Value(controller);
    }
    if (name == "Team" && function.parameters.size() == 1 && script.type_key(function.parameters[0]) == "object:GOBJ") {
        require_signature(script, function, 1, {"object:GOBJ"});
        const auto team = gobj_controller_team(arguments.at(0), "Team(GOBJ)");
        return Word(team ? static_cast<std::uint32_t>(*team) : 0xffffffffU);   // null controller -> -1 (003cc590)
    }
    // Enemy 003cc510: a's +58 null -> 1 (v0 = -1, sltu); else b's team (+58
    // null -> -1) and a's controller virtual +6c (0022f440 in the player and
    // both AI vtables): 0 for a negative team or a null / out-of-range matrix
    // 004ee620, else bit 0 of [team a][team b] (TeamRelations::enemy).
    if (name == "Enemy" && function.parameters.size() == 2) {
        require_signature(script, function, 1, {"object:GOBJ", "object:GOBJ"});
        const auto a = gobj_controller_team(arguments.at(0), "Enemy(GOBJ, GOBJ) a");
        if (!a) return Word{1};
        const auto b = gobj_controller_team(arguments.at(1), "Enemy(GOBJ, GOBJ) b");
        return Word{teams_.enemy(*a, b ? *b : -1) ? 1U : 0U};
    }
    // 00175d30: argument 0 is the Player component itself, virtual +0x68 =
    // 00175340: 0 while 005721c8 is clear (solo).
    // Checkpoint 132zf9 (BELARUS1 MILL): game::CastNPC(GOBJ) -> NPC 00169ac0:
    // 003cc820 (identity); the GOBJ's +58 controller when its virtual +58 or
    // +5c answers nonzero, else null; then 0014a670 (identity). cNPC
    // (004d9380) +58 = 0015b7e0 returns 1; the player component (004d9cf0)
    // +58 = 00140e10 and +5c = 001535d0 return 0. A node never writes +58
    // (null). Other GOBJ classes' +58 are not read and throw.
    if (name == "game::CastNPC") {
        if (function.return_type < 100 || script.type_key(function.return_type) != "object:NPC" ||
            function.parameters.size() != 1 || script.type_key(function.parameters[0]) != "object:GOBJ")
            throw std::runtime_error("Unsupported native signature: " + script_native_signature(script, function));
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error("game::CastNPC (00169ac0) without a live GOBJ");
        if (handle->id == objects_.player) return Value(RuntimeObjectHandle{});
        const auto section = objects_.sections.find(handle->id);
        if (section != objects_.sections.end() && section->second == "node") return Value(RuntimeObjectHandle{});
        if (section == objects_.sections.end() || section->second != "npc")
            throw std::runtime_error("game::CastNPC (00169ac0) of a GOBJ that is not an actor (+58 not hosted): " +
                                     std::to_string(handle->id));
        auto* npcs = world_.npcs();
        if (!npcs || !npcs->component(handle->id))
            throw std::runtime_error("game::CastNPC (00169ac0) of an NPC the world does not host: " + std::to_string(handle->id));
        return Value(objects_.objects.resolve(handle->id));
    }
    if (name == "Team" && function.parameters.size() == 1 && script.type_key(function.parameters[0]) == "object:Player") {
        require_signature(script, function, 1, {"object:Player"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::player || handle->id != objects_.player ||
            !objects_.objects.alive(*handle))
            throw std::runtime_error("Team(Player) requires the local player's component");
        return Word{0};
    }
    // GetActiveWeapon(Player) 00175cd0 (132sx10, YEMEN1): the component's +30
    // actor (null -> -1) of the actor class (key 0049d010, else -1): its +3524
    // inventory's held slot item (+84 * 0x10 + 8), -1 when +84 is 6 (as
    // game::GetActiveWeapon 001855c0 does for a GOBJ).
    if (name == "GetActiveWeapon" && function.parameters.size() == 1 &&
        script.type_key(function.parameters[0]) == "object:Player") {
        require_signature(script, function, 1, {"object:Player"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::player || handle->id != objects_.player ||
            !objects_.objects.alive(*handle))
            throw std::runtime_error("GetActiveWeapon(Player) requires the local player's component");
        const auto item = world_.player_active_weapon();
        if (!item) throw std::runtime_error("GetActiveWeapon(Player): the world does not host the actor's weapon inventory (+3524)");
        return Word{static_cast<Word>(*item)};
    }
    // 003cc760: the GOBJ's +0x58 controller; null -> 0, else (virtual +0x54)
    // != 0. Player component +54 = 00175330 (1), AI component +54 = 0012f370
    // (0). The class is fixed by the population, so an NPC record answers 0
    // whether or not the world hosts its component; a fire zone (the
    // attacker of its OnDamageMsg) has no controller and answers 0. Other
    // GOBJ classes throw.
    if (name == "IsPlayer" && function.parameters.size() == 1 && script.type_key(function.parameters[0]) == "object:GOBJ") {
        require_signature(script, function, 1, {"object:GOBJ"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error("IsPlayer(GOBJ) without a live GOBJ");
        if (handle->id == objects_.player) return Word{1};
        const auto section = objects_.sections.find(handle->id);
        // Fire zones (002624a0, vtable 004dc970) and nodes (0015c880, vtable 004d95b0) never write +58 after the
        // GOBJ base 003cf0e0 zeroes it (003cf164): null controller -> 0.
        if (section != objects_.sections.end() && (section->second == "fire" || section->second == "node")) return Word{0};
        // Checkpoint 132ms5: an InteractGOBJ (00216ae0 over the GOB constructor 0021add0, which calls 003cf0e0 and
        // neither writes +58) answers 0 too: BELARUS2's bridge C-4 533680 / 533679, the CreateBlast attackers of
        // BridgeExplosion, read by OnDamageMsg word 2557.
        if (section != objects_.sections.end() && section->second == "object") {
            const auto kind = objects_.kinds.find(handle->id);
            if (kind != objects_.kinds.end() && kind->second == 5) return Word{0};
        }
        if (section == objects_.sections.end() || section->second != "npc")
            throw std::runtime_error("IsPlayer(GOBJ) of a GOBJ that is not an actor (+58 not hosted): " + std::to_string(handle->id));
        return Word{0};
    }
    // GOBJ natives SetHitpoints(GOBJ, int) = 003cc5c0 and GetHitpoints(GOBJ)
    // -> int = 003cc6d0 (table 004d5a80, registrations 004d5bdc / 004d5b9c).
    // Both read the raw object a0[0] (no 003cc820, null or class check).
    // Set: +40 = cvt.s.w(int); with the multiplayer byte 005721c8 clear it
    // returns 0: no clamp, no destruction, no message (the scripts'
    // OnDamageMsg handlers test GetHitpoints <= 0 themselves). The
    // multiplayer health sync (003c9e30 with key 00582750, vtable 004e0ee0,
    // 003c9420 to the object) is not taken: solo host. Get: cvt.w.s(+40).
    // Hosted on population objects and event objects (their +40 is the GOBJ
    // base field, 0 from 003cf0e0 at 003cf144, but later constructor writes
    // are not established); actors (ActorHealthState::health_40) and other
    // records throw, as does a Get before any Set.
    // Checkpoint 132sx12: population nodes ("node", NOD records, 0015c880 class 0xa, vtable 004d95b0) too.
    // MINSK prague::GatePoints sets node 857566 to 1000. The node constructor writes +0, +64, +68 (0015c860),
    // +78, +7c and +2d and calls vt+90 = 003cdff0; neither it nor any method of 004d95b0 (0015bf50..0015c2f0)
    // touches +40, so a node's +40 is the base's 0 (003cf144) until a Set: a Get before any Set answers 0.
    if ((name == "SetHitpoints" || name == "GetHitpoints") && !function.parameters.empty() &&
        script.type_key(function.parameters[0]) == "object:GOBJ") {
        const bool set = name == "SetHitpoints";
        if (set) require_signature(script, function, 4, {"object:GOBJ", "primitive:1"});
        else require_signature(script, function, 1, {"object:GOBJ"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || handle->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + " without a live world object");
        const auto section = objects_.sections.find(handle->id);
        if (section == objects_.sections.end() ||
            (section->second != "object" && section->second != "event_object" && section->second != "node"))
            throw std::runtime_error(name + ": +40 of this record class is not hosted: " + std::to_string(handle->id) +
                                     (section == objects_.sections.end() ? std::string() : " (" + section->second + ")"));
        if (set) {
            object_health_40_[handle->id] = gobj_set_hit_points(word_signed(number(arguments.at(1))));
            return std::nullopt;
        }
        auto health = object_health_40(handle->id);
        if (!health && section->second == "node") health = 0.0F;   // 003cf144
        if (!health)
            throw std::runtime_error("GetHitpoints: +40 read before any SetHitpoints (initial value not established): " +
                                     std::to_string(handle->id));
        return Word{static_cast<std::uint32_t>(gobj_hit_points(*health))};
    }
    // game::Print(string) 002693c0: loads the argument, calls 002693f0 (an
    // empty `jr ra` in the retail build) and returns 0: nothing.
    if (name == "game::Print") {
        require_signature(script, function, 4, {"primitive:3"});
        return std::nullopt;
    }
    // game::Randomize() 00269130 -> 00269230: srand 00127db8(T1_COUNT +
    // *00533870), returning 0. Deferred when the world has no timer model.
    if (name == "game::Randomize") {
        require_signature(script, function, 4, {});
        const auto seed = world_.timer_t1_sum();
        applied = seed && world_.seed_game_random(*seed);
        return std::nullopt;
    }
    // 00269150 / 00269190 (cores 00269250 / 002692d0): one draw r of the game
    // RNG, u = cvt.s.w(r) / 2^31; with (low, high) the result is
    // cvt.w.s(cvt.s.w(int low) + cvt.s.w(int high - int low) * u), with (n)
    // cvt.w.s(cvt.s.w(int n) * u), returned as a float. cvt.w.s truncates.
    if (name == "game::RandomInt") {
        if (script.type_key(function.return_type) != "primitive:2" || function.parameters.empty() ||
            function.parameters.size() > 2)
            throw std::runtime_error("Unsupported native signature: " + script_native_signature(script, function));
        auto integer = [&](std::size_t index) {
            const auto* word = std::get_if<Word>(&arguments.at(index));
            if (!word || script.type_key(function.parameters[index]) != "primitive:2")
                throw std::runtime_error("Unsupported native signature: " + script_native_signature(script, function));
            const float value = std::bit_cast<float>(*word);
            if (!(std::fabs(value) < 2147483648.0F)) throw std::runtime_error("game::RandomInt: bound outside the integer range");
            return static_cast<std::int32_t>(value);
        };
        const float unit = game_int_to_float(static_cast<std::int32_t>(world_.game_random())) / 2147483648.0F;
        const auto low = integer(0);
        float scaled = game_int_to_float(low) * unit;
        if (function.parameters.size() == 2) {
            const auto span = static_cast<std::int32_t>(static_cast<std::uint32_t>(integer(1)) - static_cast<std::uint32_t>(low));
            scaled = game_int_to_float(low) + game_int_to_float(span) * unit;
        }
        if (!(std::fabs(scaled) < 2147483648.0F)) throw std::runtime_error("game::RandomInt: result outside the integer range");
        return Word{std::bit_cast<std::uint32_t>(game_int_to_float(static_cast<std::int32_t>(scaled)))};
    }
    // game::SetPlayerWeapons(int, int) -> 00185330(first, second): clear the
    // player's inventory, add both items (preload, full ammunition) and equip
    // the first item's slot (the second's when the first is negative). The VM
    // pushes the arguments last to first (as for game::RandomInt), so init's
    // push of -1 then 106 is SetPlayerWeapons(106, -1).
    if (name == "game::SetPlayerWeapons") {
        require_signature(script, function, 4, {"primitive:1", "primitive:1"});
        applied = world_.set_player_weapons(word_signed(number(arguments.at(0))), word_signed(number(arguments.at(1))));
        return std::nullopt;
    }
    // Checkpoint 132sx10: StealPlayerWeapons(Backpack, WpnSlot, WpnSlot[,
    // WpnSlot]) 00253c90 / 00253cc0: lbu of each slot (the two-slot form
    // passes 8, skipped), 00253dd0 on the backpack (+2f3 = 1, +2f0 = 0, the
    // slots' weapons from the local player's +3524); returns 0.
    if (name == "StealPlayerWeapons") {
        const bool three = function.parameters.size() == 4;
        std::vector<std::string> signature{"object:Backpack", "enum:Weapon_WpnSlot", "enum:Weapon_WpnSlot"};
        if (three) signature.push_back("enum:Weapon_WpnSlot");
        require_signature(script, function, 4, signature);
        const auto* backpack = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!backpack || backpack->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*backpack))
            throw std::runtime_error(name + " without a live backpack");
        const auto section = objects_.sections.find(backpack->id);
        if (section == objects_.sections.end() || section->second != "backpack")
            throw std::runtime_error(name + " of a record that is not a backpack: " + std::to_string(backpack->id));
        const auto slot = [&](std::size_t i) { return static_cast<std::uint8_t>(number(arguments.at(i)) & 0xffU); };
        const std::array<std::uint8_t, 3> slots{slot(1), slot(2), three ? slot(3) : std::uint8_t{8}};
        applied = world_.steal_player_weapons(backpack->id, slots);
        return std::nullopt;
    }
    // game::Distance(GOBJ, GOBJ) = 00267bd0: 003cc820 (identity) on both
    // arguments, then 00267ee0: each object's virtual +e4 (the first
    // argument's first), then the VU distance, returned in f0. No null check
    // (the interpreter skips the call for a null object). Hosted for actors
    // (class 004da690, +e4 = 00132860): the player and the population's NPC
    // records. Population GOBs answer their 003cd260 through the world
    // (object_position_e4, 132zf5); other GOBJ records throw.
    // Checkpoint 132sx8: the NPC / Player overloads (NPC, NPC) 00267a90 ->
    // 00267c20, (NPC, GOBJ) 00267ae0 -> 00267cd0, (Player, NPC) 00267b30 ->
    // 00267d80 and (Player, GOBJ) 00267b80 -> 00267e30 convert an NPC with
    // 0014a690 and a Player with 00175fa0 (both identity), read the
    // controller's +30 owner GOBJ and call its +e4; a GOBJ argument calls +e4
    // directly. The VU distance is the same instruction sequence as 00267ee0.
    if (name == "game::Distance") {
        const auto key = [&](std::size_t i) { return script.type_key(function.parameters.at(i)); };
        if (function.return_type != 2 || function.parameters.size() != 2)
            throw std::runtime_error("Unsupported native signature: " + script_native_signature(script, function));
        const auto first = key(0), second = key(1);
        const bool known = (first == "object:GOBJ" && second == "object:GOBJ") ||
                           (first == "object:NPC" && (second == "object:NPC" || second == "object:GOBJ")) ||
                           (first == "object:Player" && (second == "object:NPC" || second == "object:GOBJ"));
        if (!known) throw std::runtime_error("Unsupported native parameter: " + script_native_signature(script, function));
        auto position = [&](std::size_t index) -> RootVector {
            const auto* object = std::get_if<RuntimeObjectHandle>(&arguments.at(index));
            const auto type = key(index);
            RuntimeObjectHandle owner{};
            if (type != "object:GOBJ") {
                const auto kind = type == "object:NPC" ? RuntimeObjectHandle::Kind::ai : RuntimeObjectHandle::Kind::player;
                if (!object || object->kind != kind || !objects_.objects.alive(*object))
                    throw std::runtime_error("game::Distance without a live " + type.substr(7) + " controller");
                owner = objects_.objects.get_gobj(*object);
                object = &owner;
            }
            if (!object || object->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*object))
                throw std::runtime_error("game::Distance without a live GOBJ");
            if (object->id == objects_.player) {
                const auto p = world_.player_position_e4();
                if (!p) throw std::runtime_error("game::Distance: the world has no player actor");
                return *p;
            }
            const auto section = objects_.sections.find(object->id);
            // Checkpoint 132el8: an NIE: object (vtable 004dc050) has the same +e4, 003cd260.
            if (section != objects_.sections.end() && (section->second == "object" || section->second == "event_object")) {
                // A population GOB (SEWER's C-4 plant 271537, kind 5): +e4 = 003cd260.
                const auto p = world_.object_position_e4(object->id);
                if (!p) throw std::runtime_error("game::Distance of a GOB whose +e4 (003cd260) is not hosted: " +
                                                 std::to_string(object->id));
                return *p;
            }
            // Checkpoint 132ms2: a population node (vtable 004d95b0): +e4 = 0015bef0, its +80/+84/+88 position
            // with w = 1.0 (MINSK prague::NPCGetClosestPanel measures the alarm panels' nodes, 791542).
            if (section != objects_.sections.end() && section->second == "node") {
                const auto node = nodes_.find(object->id);
                if (node == nodes_.end()) throw std::runtime_error("game::Distance of an unknown node: " + std::to_string(object->id));
                const auto& p = node->second.position;
                return RootVector{p.x, p.y, p.z, 1.0F};
            }
            if (section == objects_.sections.end() || section->second != "npc")
                throw std::runtime_error("game::Distance of a GOBJ that is not an actor (+e4 not hosted): " +
                                         std::to_string(object->id));
            auto* npcs = world_.npcs();
            if (!npcs) throw std::runtime_error("game::Distance of an NPC while NPCs are not hosted: " + std::to_string(object->id));
            const auto p = npcs->position_e4(object->id);
            if (!p) throw std::runtime_error("game::Distance of an NPC the world does not host: " + std::to_string(object->id));
            return *p;
        };
        const auto a = position(0);
        const auto b = position(1);
        return Word{std::bit_cast<std::uint32_t>(script_gobj_distance(a, b))};
    }
    // game::CreateBlast(GOBJ, int, float, float, float) = 0021a170:
    // 0021a200(003cc820(object), damage, 1, null, 0, 0, strength, inner,
    // outer) (COMBAT_ITEMS.md C7). The VM pushes the arguments last to first
    // (STREETS / SUBWAY: outer, inner, strength, float_to_int damage, object).
    if (name == "game::CreateBlast") {
        require_signature(script, function, 4, {"object:GOBJ", "primitive:1", "primitive:2", "primitive:2", "primitive:2"});
        const auto* object = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!object || object->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*object))
            throw std::runtime_error("game::CreateBlast without a live world object");
        const auto real = [&](std::size_t index) {
            const auto* word = std::get_if<Word>(&arguments.at(index));
            if (!word) throw std::runtime_error("game::CreateBlast: a float argument is not a word");
            return std::bit_cast<float>(*word);
        };
        applied = world_.create_blast(object->id, word_signed(number(arguments.at(1))), real(2), real(3), real(4));
        return std::nullopt;
    }
    // Checkpoint 132cp3: game::AddTremor(GOBJ, float, float, float) = 0012f8a0:
    // args +4 / +8 / +c into f12 / f13 / f14, 003cc820(object), then 0012f910
    // calls the object's virtual +e4 and 002727b0(*004ffbdc, &position,
    // duration, magnitude, radius / 3.0, radius); returns 0.
    if (name == "game::AddTremor") {
        require_signature(script, function, 4, {"object:GOBJ", "primitive:2", "primitive:2", "primitive:2"});
        const auto* object = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!object || object->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*object))
            throw std::runtime_error("game::AddTremor without a live world object (0012f910 calls its virtual +e4)");
        const auto real = [&](std::size_t index) {
            const auto* word = std::get_if<Word>(&arguments.at(index));
            if (!word) throw std::runtime_error("game::AddTremor: a float argument is not a word");
            return std::bit_cast<float>(*word);
        };
        applied = world_.add_tremor(object->id, real(1), real(2), real(3));
        return std::nullopt;
    }
    // game::SpawnParticle(EffectName, GOBJ) 00230770: lb +0 (the signed table
    // index), 003cc820(lw +4), then 002307b0(index, object, 1) and return 0
    // (Checkpoint 132k; TORONTO1's keypad 140462 OnDamageMsg, effect 10).
    if (name == "game::SpawnParticle") {
        require_signature(script, function, 4, {"enum:EffectName", "object:GOBJ"});
        const auto* object = std::get_if<RuntimeObjectHandle>(&arguments.at(1));
        if (!object || object->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*object))
            throw std::runtime_error("game::SpawnParticle without a live world object (002307b0 calls its virtual +e4)");
        applied = world_.spawn_particle(object->id, static_cast<std::int8_t>(number(arguments.at(0)) & 0xffU));
        return std::nullopt;
    }
    // Checkpoint 132sx5: game::SpawnLight(GOBJ, float, float) 00230610: lw +8
    // and lw +4 (f13 / f12), 003cc820(lw +0), then 00230660(object, 1, f12,
    // f13) and return 0 (STREETS' BlowPowerSupply, SubwayBlow01 / 02 and the C4
    // OnDamageMsg handlers: (object, 60.0, 0.269)).
    if (name == "game::SpawnLight") {
        require_signature(script, function, 4, {"object:GOBJ", "primitive:2", "primitive:2"});
        const auto* object = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!object || object->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*object))
            throw std::runtime_error("game::SpawnLight without a live world object (00230660 calls its virtual +e4)");
        const auto real = [&](std::size_t index) {
            const auto* word = std::get_if<Word>(&arguments.at(index));
            if (!word) throw std::runtime_error("game::SpawnLight: a float argument is not a word");
            return std::bit_cast<float>(*word);
        };
        applied = world_.spawn_light(object->id, real(1), real(2));
        return std::nullopt;
    }
    // Checkpoint 132g: game::Shatter(GOBJ glass, GOBJ other) = 00230d90:
    // 00230de0(003cc820(glass), 003cc820(other), 0). The other object is
    // read only through its +e4 (00231024), hosted for the player and the
    // NPC records as for game::Distance; other classes throw.
    if (name == "game::Shatter") {
        require_signature(script, function, 4, {"object:GOBJ", "object:GOBJ"});
        const auto* glass = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!glass || glass->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*glass))
            throw std::runtime_error("game::Shatter without a live glass GOBJ");
        const auto* other = std::get_if<RuntimeObjectHandle>(&arguments.at(1));
        if (!other || other->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*other))
            throw std::runtime_error("game::Shatter without a live other GOBJ");
        RootVector at{};
        if (other->id == objects_.player) {
            const auto p = world_.player_position_e4();
            if (!p) throw std::runtime_error("game::Shatter: the world has no player actor");
            at = *p;
        } else {
            const auto section = objects_.sections.find(other->id);
            if (section == objects_.sections.end() || section->second != "npc")
                throw std::runtime_error("game::Shatter: the other GOBJ is not an actor (+e4 not hosted): " +
                                         std::to_string(other->id));
            auto* npcs = world_.npcs();
            if (!npcs) throw std::runtime_error("game::Shatter of an NPC while NPCs are not hosted: " + std::to_string(other->id));
            const auto p = npcs->position_e4(other->id);
            if (!p) throw std::runtime_error("game::Shatter of an NPC the world does not host: " + std::to_string(other->id));
            at = *p;
        }
        applied = world_.shatter(glass->id, at);
        return std::nullopt;
    }
    // game::RequestLevelEnd() = 0012f9a0: 0012fa60(G 004ffb50, 0), the solo
    // mission-complete request (kinds 1-3 have no script caller).
    if (name == "game::RequestLevelEnd") {
        require_signature(script, function, 4, {});
        applied = world_.request_level_end(0);
        return std::nullopt;
    }
    // game::SetPlayersCheckpoint(GOBJ, string) 0022de90 -> 0022ded0(003cc820(object), label)
    // (checkpoint 132zf7, MALL's OnAIBodyMovedMsg): for each player slot 0..3
    // (003fb8d0) that is set, a label copy and MSGGAME_ADD_CHECKPOINT (vtable
    // 004db8d0, key 004f7930: +24 object, +28 label) sent to that slot's actor
    // with transport 0, as SetCheckpoint's 0022e080 sends to the player's +30.
    // In solo only slot 0 (the local player's actor) is set.
    if (name == "game::SetPlayersCheckpoint") {
        require_signature(script, function, 4, {"object:GOBJ", "primitive:3"});
        const auto* node = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!node || node->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*node))
            throw std::runtime_error(name + " without a live node object");
        const auto record = nodes_.find(node->id);
        // 001b3b60 reads +80 / +8c of the object as a node's.
        if (record == nodes_.end())
            throw std::runtime_error(name + " on a non-node record (001b3b60 reads node +80 / +8c): " + std::to_string(node->id));
        const auto* label = std::get_if<std::string>(&arguments.at(1));
        if (!label) throw std::runtime_error(name + " without a label string");
        const auto& p = record->second.position;
        applied = world_.add_checkpoint(node->id, {p.x, p.y, p.z}, population_node_yaw(record->second), *label);
        return std::nullopt;
    }
    // 0022e030 -> 0022e080: the label is copied (strlen + 1 bytes, 0036b180,
    // 00129360) into MSGGAME_ADD_CHECKPOINT (+24 node, +28 label), sent to the
    // player's +30 (its actor) with transport 0. 001b3b60 later reads node
    // +80 (position) and +8c (yaw) in the message pump. Null objects never get
    // here (the interpreter skips the call).
    if (name == "game::SetCheckpoint") {
        require_signature(script, function, 4, {"object:Player", "object:GOBJ", "primitive:3"});
        const auto* player = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!player || player->kind != RuntimeObjectHandle::Kind::player || player->id != objects_.player ||
            !objects_.objects.alive(*player))
            throw std::runtime_error("game::SetCheckpoint is hosted only for the local player");
        const auto* node = std::get_if<RuntimeObjectHandle>(&arguments.at(1));
        if (!node || node->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*node))
            throw std::runtime_error("game::SetCheckpoint without a live node object");
        const auto record = nodes_.find(node->id);
        if (record == nodes_.end())
            throw std::runtime_error("game::SetCheckpoint on a non-node record: " + std::to_string(node->id));
        const auto* label = std::get_if<std::string>(&arguments.at(2));
        if (!label) throw std::runtime_error("game::SetCheckpoint without a label string");
        const auto& p = record->second.position;
        applied = world_.add_checkpoint(node->id, {p.x, p.y, p.z}, population_node_yaw(record->second), *label);
        return std::nullopt;
    }
    // 003e6540 (the three-argument variant): actor 003cb1c0(argument 0), the
    // action byte of argument 1 (sign-extended), object 003cc820(argument 2),
    // then 003e67c0(actor, action, object, 0, -1.0): a CAS action (key 0x40,
    // kind 7, +2c the object's +c key, motion -1, time -1.0) sent to the actor
    // with transport 0. Hosted for the player actor or an NPC's actor (the
    // receiver as game::PerformAction's, 132zf5) and an InteractGOBJ: its
    // constructor (00216ae0) keeps its last two references, +a8 (read as a
    // node by 0018d2c0) and +ac (the world object 0018fe68 pushes).
    // The 4/5-argument forms 003e64d0 / 003e6450 read the action byte (lb),
    // the object and the name (a char*, [0xc]), and the time ([0x10]; -1.0
    // for the 4-argument form) and call 003e6910(actor, action, object,
    // name, time, t0 = 0): the kind-10 message (build_named_object_action),
    // queued with transport 0 to the same actor (EE goldens,
    // tests/named_object_action_goldens.hpp). The 6-argument form 003e63c0
    // (132zf7: MALL's InteractGOBJ 'Autopsy_perform' / 'hacking_terminal')
    // is the same with t0 = ([0x14] != 0), which becomes the kind-7 resend's
    // +38 (actor +324c bit 0).
    if (name == "game::PerformAction_WithObject" &&
        function.parameters.size() >= 3 && function.parameters.size() <= 6) {
        const auto count = function.parameters.size();
        if (count == 3) require_signature(script, function, 4, {"object:System", "enum:CAS_Action", "object:GOBJ"});
        else if (count == 4)
            require_signature(script, function, 4, {"object:System", "enum:CAS_Action", "object:GOBJ", "primitive:3"});
        else if (count == 5)
            require_signature(script, function, 4,
                              {"object:System", "enum:CAS_Action", "object:GOBJ", "primitive:3", "primitive:2"});
        else
            require_signature(script, function, 4,
                              {"object:System", "enum:CAS_Action", "object:GOBJ", "primitive:3", "primitive:2", "primitive:1"});
        const auto* actor = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!actor || !objects_.objects.alive(*actor))
            throw std::runtime_error("game::PerformAction_WithObject without a live actor");
        // The receiver as game::PerformAction's (003cb220): the player's actor,
        // or an NPC's actor through an AI handle or its GOBJ (132zf5: SEWER's
        // LastBattleNPC lowering the ladder 206003, word 14456). Checkpoint
        // 132ms4: the player's controller handle (id_to_object of the player's
        // ID, YEMEN1's steel gate 664755 at word 23084) is a component whose
        // +30 owner is the player's actor, as for game::PerformAction.
        std::optional<Word> npc;
        if (actor->kind == RuntimeObjectHandle::Kind::player) {
            if (actor->id != objects_.player)
                throw std::runtime_error("game::PerformAction_WithObject: a player handle of another record");
        } else if (actor->kind == RuntimeObjectHandle::Kind::ai) {
            npc = actor->id;
        } else if (actor->kind == RuntimeObjectHandle::Kind::world_object && actor->id != objects_.player) {
            const auto section = objects_.sections.find(actor->id);
            if (section == objects_.sections.end() || section->second != "npc")
                throw std::runtime_error("game::PerformAction_WithObject to a GOBJ that is not an actor (its CAS receiver is not hosted): " +
                                         std::to_string(actor->id));
            npc = actor->id;
        } else if (actor->kind != RuntimeObjectHandle::Kind::world_object) {
            throw std::runtime_error("game::PerformAction_WithObject to an unhosted receiver kind");
        }
        const auto* action = std::get_if<Word>(&arguments.at(1));
        if (!action) throw std::runtime_error("game::PerformAction_WithObject without an action word");
        const auto* object = std::get_if<RuntimeObjectHandle>(&arguments.at(2));
        if (!object || object->kind != RuntimeObjectHandle::Kind::world_object || !objects_.objects.alive(*object))
            throw std::runtime_error("game::PerformAction_WithObject without a live object");
        const auto links = interact_links(object->id);
        if (!links)
            throw std::runtime_error("game::PerformAction_WithObject on a non-InteractGOBJ record: " + std::to_string(object->id));
        const auto& action_node = links->node;
        const Word world_object = links->world_object;
        const auto action_byte = static_cast<std::int8_t>(*action & 0xffU);
        if (count == 3) {
            applied = npc ? world_.perform_npc_object_action(*npc, action_byte, std::nullopt, object->id, action_node, world_object)
                          : world_.perform_object_action(action_byte, object->id, action_node, world_object);
            return std::nullopt;
        }
        const auto* text = std::get_if<std::string>(&arguments.at(3));
        if (!text) throw std::runtime_error("game::PerformAction_WithObject without a name string");
        float time = -1.0F;
        bool t0 = false;
        if (count >= 5) {
            const auto* word = std::get_if<Word>(&arguments.at(4));
            if (!word) throw std::runtime_error("game::PerformAction_WithObject without a time");
            time = std::bit_cast<float>(*word);
        }
        // 003e63c0 (6 arguments): t0 = [0x14] != 0 (sltu), else as 003e6450.
        if (count == 6) t0 = word_signed(number(arguments.at(5))) != 0;
        const auto message = build_named_object_action(action_byte, object->id, *text, time, t0);
        MissionScriptWorld::NamedObjectAction named{message.action, message.name, message.time, message.byte_64};
        applied = npc ? world_.perform_npc_object_action(*npc, named.action, named, object->id, action_node, world_object)
                      : world_.perform_named_object_action(named, object->id, action_node, world_object);
        return std::nullopt;
    }
    // game::PerformAction (functions with 2/3/4 parameters: 003e6650 /
    // 003e6600 / 003e65a0): the action word's low byte (lb), the name (a
    // char*) and the time (-1.0 for 2/3 arguments). 003e6770 sends the
    // kind-7 message (003e67c0, object -1, motion -1, +38 = 0) for an empty
    // name (the 2-argument form 003e6750 always) and the kind-10 message
    // (003e6910, action 0x25, object -1, +64 = 0) otherwise, transport 0
    // (perform_action_message; tests/perform_action_goldens.hpp). Both
    // senders take the receiver from 003cb220: an AI or player handle is a
    // component whose type cell (+8) is the GOBJ controller's 0x55a840
    // (003ea630 -> 003cb4b0, via 00230240 from 0016f5b0 / 00177b10), so the
    // message goes to its +30 owner, the actor; an actor's GOBJ receives it
    // directly. Other receivers (a GOBJ that is not an actor) are not hosted.
    const auto action_receiver = [&](const std::string& native) {
        const auto* actor = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!actor || !objects_.objects.alive(*actor)) throw std::runtime_error(native + " without a live actor");
        std::optional<Word> npc;
        if (actor->kind == RuntimeObjectHandle::Kind::player) {
            if (actor->id != objects_.player) throw std::runtime_error(native + ": a player handle of another record");
        } else if (actor->kind == RuntimeObjectHandle::Kind::ai) {
            npc = actor->id;
        } else if (actor->kind == RuntimeObjectHandle::Kind::world_object) {
            const auto section = objects_.sections.find(actor->id);
            if (section == objects_.sections.end() || section->second != "npc")
                throw std::runtime_error(native + " to a GOBJ that is not an actor (its CAS receiver is not hosted): " +
                                         std::to_string(actor->id));
            if (actor->id != objects_.player) npc = actor->id;
        } else {
            throw std::runtime_error(native + " to an unhosted receiver kind");
        }
        return npc;
    };
    // game::StopPerformingAction 003e6300: the action-0x28 message (key 0x40
    // at +1c, +20 = 8, +24 = 0x28, +28 kind 2, +2c = 1) to game::PerformAction's
    // receiver (003cb220), transport 0. At delivery 001ad0e8 admits it in
    // actor modes 0x11..0x14 (kind 2 with +2c != 0 needs no +324c bit 0) and
    // sends action 0x29 (001ad1d0: the retained motion), actor_cancel_action.hpp.
    // game::SetRadarBlip(GOBJ) 0022c180 / game::ClearRadarBlip(GOBJ) 0022c0f0: 003cc820(a0[0]) +c (the object's ID;
    // a null object would read +c of 0) to 0022c1f0 / 0022c120, the HUD targets *004ffc2c + 90: 00273890(&id, 1)
    // adds a marker for any object, 00272d50(&id, 0) removes its state-0 marker.
    if ((name == "game::SetRadarBlip" || name == "game::ClearRadarBlip") && function.parameters.size() == 1) {
        require_signature(script, function, 4, {"object:GOBJ"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || !objects_.objects.alive(*handle)) throw std::runtime_error(name + " without a live object (reads +c of null)");
        if (handle->kind != RuntimeObjectHandle::Kind::player && handle->kind != RuntimeObjectHandle::Kind::world_object)
            throw std::runtime_error(name + ": an AI component handle as the GOBJ (not hosted)");
        applied = world_.set_radar_blip(handle->id, name == "game::SetRadarBlip");
        return std::nullopt;
    }
    if (name == "game::StopPerformingAction" && function.parameters.size() == 1) {
        require_signature(script, function, 4, {"object:System"});
        applied = world_.stop_performing_action(action_receiver(name));
        return std::nullopt;
    }
    if (name == "game::PerformAction" && function.parameters.size() >= 2 && function.parameters.size() <= 4) {
        const auto count = function.parameters.size();
        if (count == 2) require_signature(script, function, 4, {"object:System", "enum:CAS_Action"});
        else if (count == 3) require_signature(script, function, 4, {"object:System", "enum:CAS_Action", "primitive:3"});
        else require_signature(script, function, 4, {"object:System", "enum:CAS_Action", "primitive:3", "primitive:2"});
        const auto npc = action_receiver(name);
        const auto* action = std::get_if<Word>(&arguments.at(1));
        if (!action) throw std::runtime_error("game::PerformAction without an action word");
        std::optional<std::string_view> text;
        if (count >= 3) {
            const auto* s = std::get_if<std::string>(&arguments.at(2));
            if (!s) throw std::runtime_error("game::PerformAction without a name string");
            text = *s;
        }
        float time = -1.0F;
        if (count == 4) {
            const auto* word = std::get_if<Word>(&arguments.at(3));
            if (!word) throw std::runtime_error("game::PerformAction without a time");
            time = std::bit_cast<float>(*word);
        }
        const auto message = perform_action_message(*action, text, time);
        MissionScriptWorld::ActorAction request;
        if (const auto* k7 = std::get_if<ActorObjectActionMessage>(&message)) {
            request.action = k7->action;
            request.time = k7->time;
        } else {
            const auto& k10 = std::get<NamedObjectActionMessage>(message);
            request.action = k10.action;
            request.name = k10.name;
            request.time = k10.time;
        }
        applied = world_.perform_action(npc, request);
        return std::nullopt;
    }
    // The actor-GOBJ natives: argument 0 is GObj_IdentityB(a0[0]), acted on
    // only when its +4c is the actor class *0049d010 (any other GOBJ: return 0,
    // nothing else). The population's actors are its "npc" records (the
    // player's included); the other sections are not actor classes.
    //  - game::SetNotGasable(GOBJ, int) 00185140: sb (int != 0) to +346b; the
    //    network message behind it is sent only while 005721c8 is set (solo:
    //    clear). An NPC's +346b is the one SetNotGasable(NPC) 00148f80 writes.
    //  - game::SwitchBodies(GOBJ, NPC) 00175630: 0014a690(a0[1]) +30 (the NPC's
    //    actor) must be an actor too; Event_Send(key 004f80b8, the GOBJ, 0)
    //    with the two IDs (+5c, +60), which Npc_HandleMessage 001b3b60 turns
    //    into 00390fc0(GOBJ +60, NPC +a0) (switch_body_packing).
    //  - game::RestoreBody(GOBJ) 001754f0: Event_Send(key 004f80a8, the GOBJ,
    //    0), which 001b3b60 turns into 00391210(GOBJ +60) (restore_body_packing).
    //  - game::ShockToDeath(GOBJ) 00185280: a null object returns 0 (00185294);
    //    otherwise, for the actor class, the stun 00184ce0(actor, 1, 1, 0.5,
    //    10000.0) unless its life state is 2 or 3 or it is stunned
    //    (MissionScriptWorld::shock_to_death).
    //  - game::ForceHolster(GOBJ) 00185410: a null object returns 0 (00185424);
    //    for the actor class, 00143cc0(actor +3524): the holster 00143b60
    //    unless +84 is 6 (the 005721c8 network send is skipped in solo).
    //  - game::GetActiveWeapon(GOBJ) 001855c0: a null object or another class
    //    returns -1; the actor's +3524 held slot item, -1 when +84 is 6.
    // Checkpoint 132ms2: game::SetHardpointVis(GOBJ, Weapon_WpnSlot, int)
    // 00185550: s1 = (+8 != 0), s0 = lbu +4; 003cc820(+0) null returns 0
    // (00185574); a class other than the actor's (+4c != *0049d010) returns 0;
    // else 00142e40(actor +3524, slot, s1) (MissionScriptWorld::set_hardpoint_vis).
    if (name == "game::SetHardpointVis" && function.parameters.size() == 3) {
        require_signature(script, function, 4, {"object:GOBJ", "enum:Weapon_WpnSlot", "primitive:1"});
        if (null_handle(arguments.at(0))) return std::nullopt;
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + ": 003cc820 without a live object (the original reads +4c of it)");
        const auto slot = static_cast<std::uint8_t>(number(arguments.at(1)) & 0xffU);   // lbu +4
        const bool visible = number(arguments.at(2)) != 0;                              // sltu s1, zero, v0
        std::optional<Word> npc;
        if (handle->kind == RuntimeObjectHandle::Kind::player) {
            if (handle->id != objects_.player) throw std::runtime_error(name + ": a player handle of another record");
        } else if (handle->kind == RuntimeObjectHandle::Kind::world_object) {
            const auto section = objects_.sections.find(handle->id);
            if (section == objects_.sections.end())
                throw std::runtime_error(name + ": GOBJ outside the population (its class +4c is not hosted): " +
                                         std::to_string(handle->id));
            if (section->second != "npc") return std::nullopt;     // 00185584: not the actor class
            if (handle->id != objects_.player) npc = handle->id;
        } else {
            throw std::runtime_error(name + ": an AI component handle as the GOBJ (not hosted)");
        }
        applied = world_.set_hardpoint_vis(npc, slot, visible);
        return std::nullopt;
    }
    if ((name == "game::ShockToDeath" || name == "game::ForceHolster") && function.parameters.size() == 1) {
        require_signature(script, function, 4, {"object:GOBJ"});
        if (null_handle(arguments.at(0))) return std::nullopt;
    }
    if (name == "game::GetActiveWeapon" && function.parameters.size() == 1) {
        require_signature(script, function, 1, {"object:GOBJ"});
        if (null_handle(arguments.at(0))) return Word{0xffffffffU};             // 001855d0
    }
    if ((name == "game::SetNotGasable" && function.parameters.size() == 2) ||
        (name == "game::SwitchBodies" && function.parameters.size() == 2) ||
        (name == "game::RestoreBody" && function.parameters.size() == 1) ||
        (name == "game::ShockToDeath" && function.parameters.size() == 1) ||
        (name == "game::ForceHolster" && function.parameters.size() == 1) ||
        (name == "game::GetActiveWeapon" && function.parameters.size() == 1)) {
        if (name == "game::SetNotGasable") require_signature(script, function, 4, {"object:GOBJ", "primitive:1"});
        else if (name == "game::SwitchBodies") require_signature(script, function, 4, {"object:GOBJ", "object:NPC"});
        else if (name == "game::GetActiveWeapon") require_signature(script, function, 1, {"object:GOBJ"});
        else require_signature(script, function, 4, {"object:GOBJ"});
        const auto* handle = std::get_if<RuntimeObjectHandle>(&arguments.at(0));
        if (!handle || !objects_.objects.alive(*handle))
            throw std::runtime_error(name + ": GObj_IdentityB without a live object (the original reads +4c of null)");
        std::optional<Word> npc;
        if (handle->kind == RuntimeObjectHandle::Kind::player) {
            if (handle->id != objects_.player) throw std::runtime_error(name + ": a player handle of another record");
        } else if (handle->kind == RuntimeObjectHandle::Kind::world_object) {
            const auto section = objects_.sections.find(handle->id);
            if (section == objects_.sections.end())
                throw std::runtime_error(name + ": GOBJ outside the population (its class +4c is not hosted): " +
                                         std::to_string(handle->id));
            if (section->second != "npc") {                         // not the actor class: return 0
                if (name == "game::GetActiveWeapon") return Word{0xffffffffU};   // 001855e4: -1
                return std::nullopt;
            }
            if (handle->id != objects_.player) npc = handle->id;
        } else {
            throw std::runtime_error(name + ": an AI component handle as the GOBJ (not hosted)");
        }
        if (name == "game::SetNotGasable") {
            const auto* value = std::get_if<Word>(&arguments.at(1));
            if (!value) throw std::runtime_error("game::SetNotGasable without an int");
            const bool not_gasable = *value != 0;                    // sltu s0, zero, v0
            if (npc) {
                auto* npcs = world_.npcs();
                if (npcs) npcs->set_not_gasable(*npc, not_gasable);
                else applied = false;
            } else {
                applied = world_.set_player_not_gasable(not_gasable);
            }
            return std::nullopt;
        }
        if (name == "game::GetActiveWeapon") {
            std::optional<std::int32_t> item;
            if (npc) {
                auto* npcs = world_.npcs();
                if (npcs && npcs->component(*npc)) item = npcs->active_weapon(*npc);
            } else {
                item = world_.player_active_weapon();
            }
            if (!item) {
                applied = false;
                throw std::runtime_error("game::GetActiveWeapon: the world does not host the actor's weapon inventory (+3524)");
            }
            return Word{static_cast<Word>(*item)};
        }
        if (name == "game::ForceHolster") {
            if (npc) {
                auto* npcs = world_.npcs();
                applied = npcs && npcs->component(*npc) && npcs->force_holster(*npc);
            } else {
                applied = world_.force_holster_player();
            }
            return std::nullopt;
        }
        if (name == "game::RestoreBody") {
            applied = world_.restore_body(npc);
            return std::nullopt;
        }
        if (name == "game::ShockToDeath") {
            applied = world_.shock_to_death(npc);
            return std::nullopt;
        }
        const auto* costume = std::get_if<RuntimeObjectHandle>(&arguments.at(1));
        if (!costume || costume->kind != RuntimeObjectHandle::Kind::ai || !objects_.objects.alive(*costume))
            throw std::runtime_error("game::SwitchBodies: 0014a690 without a live NPC component");
        const auto section = objects_.sections.find(costume->id);
        if (section == objects_.sections.end() || section->second != "npc")
            throw std::runtime_error("game::SwitchBodies: NPC component without a population actor: " + std::to_string(costume->id));
        applied = world_.switch_bodies(npc, costume->id);
        return std::nullopt;
    }
    // 001bfd30 -> 001bfd60 (signed A-Z lowercasing) -> 001bff40(*0x4ffbd8):
    // the clip named by the string is appended to every model lacking it,
    // read from GAMEDATA\COMMON\SKALEVEL.HOG, which 003e20a0 opens only
    // around the script start; outside that window the original reads
    // nothing and silently does nothing (not hosted: throws).
    if (name == "game::LoadAnimation") {
        require_signature(script, function, 4, {"primitive:3"});
        const auto* text = std::get_if<std::string>(&arguments.at(0));
        if (!text) throw std::runtime_error("game::LoadAnimation without a name string");
        if (stage_ != "start_level")
            throw std::runtime_error("game::LoadAnimation outside the script start (SKALEVEL.HOG is closed): " + *text);
        auto load = world_.load_level_animation(*text);
        applied = load.has_value();
        if (load) level_animations_.push_back(std::move(*load));
        return std::nullopt;
    }
    // 002466f0 -> 00246740 (table 004f7d60) / 00246640 -> 00246690 (004f7d70):
    // cvt.s.w(r, g, b) / 255.0, the alpha stored as given; the wrapper returns 0.
    if (name == "game::SetInterfaceTextColor" || name == "game::SetNVInterfaceTextColor") {
        require_signature(script, function, 4, {"primitive:1", "primitive:1", "primitive:1", "primitive:2"});
        std::array<Word, 4> w{};
        for (std::size_t i = 0; i < 4; ++i) {
            const auto* word = std::get_if<Word>(&arguments.at(i));
            if (!word) throw std::runtime_error(name + " without a primitive argument");
            w[i] = *word;
        }
        const bool table_b = name == "game::SetNVInterfaceTextColor";
        const auto r = static_cast<std::int32_t>(w[0]), g = static_cast<std::int32_t>(w[1]),
                   b = static_cast<std::int32_t>(w[2]);
        const auto alpha = std::bit_cast<float>(w[3]);
        if (interface_globals_) HudObject::set_colour_table(*interface_globals_, table_b, r, g, b, alpha);
        else if (hud_object_) hud_object_->set_colour_table(table_b, r, g, b, alpha);
        else throw std::runtime_error(name + ": the hud.cc colour tables are not hosted outside the level load");
        return std::nullopt;
    }
    // 00260fb0 (004f80f0) / 00260ed0 (004f8190, byte 004f8230 = 1): the words
    // (index, threshold, r, g, b) into the lock colour ramp; returns 0.
    if (name == "game::SetLockColor" || name == "game::SetNVLockColor") {
        require_signature(script, function, 4,
                          {"primitive:1", "primitive:2", "primitive:1", "primitive:1", "primitive:1"});
        std::array<Word, 5> w{};
        for (std::size_t i = 0; i < 5; ++i) {
            const auto* word = std::get_if<Word>(&arguments.at(i));
            if (!word) throw std::runtime_error(name + " without a primitive argument");
            w[i] = *word;
        }
        const bool nv = name == "game::SetNVLockColor";
        const auto index = static_cast<std::int32_t>(w[0]);
        const auto threshold = std::bit_cast<float>(w[1]);
        const auto r = static_cast<std::int32_t>(w[2]), g = static_cast<std::int32_t>(w[3]),
                   b = static_cast<std::int32_t>(w[4]);
        if (interface_globals_) hud_set_lock_colour(*interface_globals_, nv, index, threshold, r, g, b);
        else if (hud_object_) hud_object_->set_lock_colour(nv, index, threshold, r, g, b);
        else throw std::runtime_error(name + ": the lock colour ramps are not hosted outside the level load");
        return std::nullopt;
    }
    // iarcade::Init's other text natives (InterfaceTextGlobals), applied while
    // the interface script runs; a mission script's calls stay deferred.
    // SetPickupTextColor 00253620 -> 00253670 (004f7e70, byte 004f7e80 = 1);
    // the objective colours 002279d0 / 00227960 / 002278f0 / 00227880 ->
    // 00227a40 (004f7580 / 004f7590 / 004f75a0 / 004f75b0).
    if (interface_globals_ && (name == "game::SetPickupTextColor" || name == "game::SetObjectiveColor" ||
                               name == "game::SetObjectiveCurrColor" || name == "game::SetObjectiveSuccColor" ||
                               name == "game::SetObjectiveFailColor")) {
        require_signature(script, function, 4, {"primitive:1", "primitive:1", "primitive:1", "primitive:2"});
        std::array<Word, 4> w{};
        for (std::size_t i = 0; i < 4; ++i) {
            const auto* word = std::get_if<Word>(&arguments.at(i));
            if (!word) throw std::runtime_error(name + " without a primitive argument");
            w[i] = *word;
        }
        const auto colour = script_colour_255(static_cast<std::int32_t>(w[0]), static_cast<std::int32_t>(w[1]),
                                              static_cast<std::int32_t>(w[2]), std::bit_cast<float>(w[3]));
        auto& t = interface_text_;
        if (name == "game::SetPickupTextColor") {
            t.pickup_colour = colour;
            t.pickup_colour_set = 1;
        } else if (name == "game::SetObjectiveColor") t.objective_colour = colour;
        else if (name == "game::SetObjectiveCurrColor") t.objective_curr_colour = colour;
        else if (name == "game::SetObjectiveSuccColor") t.objective_succ_colour = colour;
        else t.objective_fail_colour = colour;
        return std::nullopt;
    }
    // The FontType natives (the wrapper passes lbu of the argument): SetInfoBarFont
    // 002467a0 -> 002467c0 (sb 004f7d40), SetInterfaceFont 001653c0 -> 001653e0
    // (*0055a278 = the font manager's +280 word for index & 0xff),
    // SetObjectivesFont 00229b00 -> 00229b20 (sb 0048ab20), SetObjectivesHeaderFont
    // 00229ad0 -> 00229af0 (sb 004f7560).
    if (interface_globals_ && (name == "game::SetInfoBarFont" || name == "game::SetInterfaceFont" ||
                               name == "game::SetObjectivesFont" || name == "game::SetObjectivesHeaderFont")) {
        require_signature(script, function, 4, {"enum:FontType"});
        const auto* word = std::get_if<Word>(&arguments.at(0));
        if (!word) throw std::runtime_error(name + " without a FontType");
        const auto index = static_cast<std::uint8_t>(*word & 0xffU);
        if (name == "game::SetInfoBarFont") interface_globals_->font_index = index;
        else if (name == "game::SetInterfaceFont") interface_globals_->text_word = (*interface_fonts_)(index);
        else if (name == "game::SetObjectivesFont") interface_text_.objectives_font = index;
        else interface_text_.objectives_header_font = index;
        return std::nullopt;
    }
    applied = false;
    if (function.return_type == 4) return std::nullopt;
    throw std::runtime_error("Unhosted non-void native " + script_native_signature(script, function));
}

std::optional<Value> MissionScriptHost::call(const Script& script, const Function& function,
                                             const std::vector<Value>& arguments) {
    Call record{stage_, script_native_signature(script, function), {}, {}, false};
    for (const auto& argument : arguments) record.arguments.push_back(describe_script_value(argument));
    auto result = hosted_call(script, function, arguments, record.applied);
    if (result) record.result = describe_script_value(*result);
    (interface_globals_ ? interface_calls_ : calls_).push_back(std::move(record));
    return result;
}

void MissionScriptHost::run_interface_script(Script iarcade, HudGlobals& globals,
                                             const std::function<std::uint32_t(std::uint8_t)>& font_table) {
    if (started_) throw std::runtime_error("00136990: the interface script runs before the mission scripts start");
    if (!font_table) throw std::runtime_error("00136990: SetInterfaceFont needs the font manager's table (001698c0 +280)");
    if (!modules_.emplace("iarcade", std::move(iarcade)).second)
        throw std::runtime_error("00136990: the mission's SCRIPTS.HOG also has an iarcade module");
    struct Scope {
        HudGlobals*& slot;
        const std::function<std::uint32_t(std::uint8_t)>*& fonts;
        ~Scope() { slot = nullptr; fonts = nullptr; }
    } scope{interface_globals_, interface_fonts_};
    interface_globals_ = &globals;
    interface_fonts_ = &font_table;
    const StageScope stage(stage_, "iarcade::init");
    if (!context_.load("iarcade")) throw std::runtime_error("00136990: 003e1720 could not load iarcade");
    context_.check_all_references();   // 003e1cf0: the result is not tested
    context_.execute_init_code();      // 003e1f30
    context_.invoke("iarcade::Init");  // 003e1b60
}

Word MissionScriptHost::enum_value(const std::string& name, Word) {
    throw std::runtime_error("External enum table not hosted: " + name);
}

Value MissionScriptHost::object_from_id(Word raw_id) { return objects_.objects.resolve(raw_id); }

Word MissionScriptHost::object_to_id(const Value& object) {
    const auto* handle = std::get_if<RuntimeObjectHandle>(&object);
    if (!handle) throw std::runtime_error("Object ID of a non-runtime object");
    return objects_.objects.id_word(*handle);
}

Word MissionScriptHost::world_object_id(const RuntimeObjectHandle& object) { return objects_.world_object_id(object); }

// Registered in the population's scope, the one scope this host has (the
// original's 0040c9d0 uses the registry at 00571748).
RuntimeObjectHandle MissionScriptHost::create_world_object(Word category) {
    return objects_.objects.create_world_object(category, 1);
}

std::int32_t ScriptFrameClock::delta(double elapsed_seconds) {
    if (!std::isfinite(elapsed_seconds) || elapsed_seconds < 0) throw std::runtime_error("Script clock: bad elapsed time");
    const auto now = static_cast<std::int64_t>(std::floor(elapsed_seconds * 1000.0));
    const auto previous = last_.value_or(now);
    last_ = now;
    const auto difference = now - previous;
    if (difference < 0) return 0;
    if (difference > 0x7fffffff) throw std::runtime_error("Script clock: delta out of range");
    return static_cast<std::int32_t>(difference);
}

}
