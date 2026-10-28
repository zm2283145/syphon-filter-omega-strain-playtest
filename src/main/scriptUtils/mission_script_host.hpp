#pragma once
// Mission script hosting (SCRIPT_HOSTING.md): the pieces shared by the S1/S2
// start probe and the live host the play sandbox drives.

#include "animation_root.hpp"
#include "hotbox_world.hpp"
#include "hud_script_state.hpp"
#include "item_inventory.hpp"
#include "level_animation.hpp"
#include "localization.hpp"
#include "npc_ai.hpp"
#include "npc_generator.hpp"
#include "object_events.hpp"
#include "objective_markers.hpp"
#include "objective_outcome.hpp"
#include "objective_progression.hpp"
#include "objective_stage.hpp"
#include "objectives.hpp"
#include "population.hpp"
#include "script.hpp"
#include "script_context.hpp"
#include "script_objects_runtime.hpp"
#include "string_pool.hpp"
#include "tank.hpp"
#include "team_relations.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace omega {

class HudObject;
struct GameScreenInteract;
struct HudGlobals;
class GameSoundManager;
class ElevatorWorld;

std::string quoted_json(const std::string& text);
// A value as the probe's JSON reports it (numbers bare, everything else quoted).
std::string describe_script_value(const Value& value);
// "name(type keys) -> return type key".
std::string script_native_signature(const Script& script, const Function& function);

// Every decoded population record by its (category << 16) | index ID in one
// scope (the sections share one ID space; a colliding ID keeps its first
// section and is reported). NPC 0 is the player and the rest are AI
// controllers. Which records the original's object table (0040c540) holds is
// not claimed.
struct PopulationObjects {
    explicit PopulationObjects(const Population& population);
    PopulationObjects(const PopulationObjects&) = delete;
    PopulationObjects& operator=(const PopulationObjects&) = delete;
    RuntimeObjectRegistry objects;
    std::map<Word, std::string> sections;
    std::map<Word, std::uint32_t> kinds;     // OBJ: records' kind words
    std::set<Word> spawned;                  // NPCs added by add_spawned_npc and not yet deleted
    std::vector<std::string> collisions;
    Word player{};
    // The receiver 003e1aa0 is given for a population record (an NPC gives
    // its AI or player component, other records their object), with a census
    // label. Throws for an ID outside the population.
    std::pair<std::string, ScriptContext::Receiver> receiver(Word id) const;
    // 003de8f0 / 0040c1f0: the ID word a live world object's vt+10 returns. An
    // actor GOBJ (vtable 004da690) and a path node (004d95b0) share the base
    // GetID 0012ece0 (&object+c, the population ID; a spawned actor's +c also
    // has bit 24, which 0040c1f0 masks off). Other sections throw.
    Word world_object_id(RuntimeObjectHandle object) const;
    // An NPC actor created at run time (a generator's spawn, 001b7b00): its
    // object-registry key with an AI component, as the NPC records.
    void add_spawned_npc(Word id);
    // Its removal was flushed out of the traversal tree: 0040c540 no longer
    // finds it (RuntimeObjectRegistry::hide).
    void flush_spawned_npc(Word id);
    // 0040bf50 deleted it: its handles are stale and its key is free.
    void delete_spawned_npc(Word id);
};

// Inputs of the objective callbacks beyond the scripts; each is optional.
struct MissionObjectiveInputs {
    // 00330b80 for the current level (AWARDS game::RegisterLevel +14, 0x16
    // for an unregistered level): the mission of the outcome actions. 0 = not
    // supplied, so apply_objective_outcome requests no action.
    std::int32_t mission_cue{};
    // game::AddObjective's table: the AWARDS game::RegisterObjective rows
    // over the base pool's intern IDs (GAMEDATA\COMMON\STRINGS.DAT order).
    // Without both the call stays deferred.
    std::optional<StringPool> base_pool;
    std::optional<ObjectiveProgression> progression;
    // 003fdb50's layered pool (base + the level's STRINGS.DAT): the display
    // labels and HUD objective labels. Without it those calls stay deferred.
    std::optional<Localization> localization;
};
// The inputs for a level from AWARDS.SO and the base pool: the progression
// rows (every key must be in the pool) and level_mission_number(level); with
// the level's pool also the layered localization.
MissionObjectiveInputs mission_objective_inputs(const Script& awards, StringPool base_pool, std::string_view level,
                                                std::optional<StringPool> mission_pool = std::nullopt);

// The objective callbacks (ObjectiveHost, solo) with the stage buckets of the
// objective stage probe (ObjectiveStage); Succeed/Fail through
// apply_objective_outcome, game::AddObjective through the progression bits.
class ObjectiveNatives {
public:
    explicit ObjectiveNatives(MissionObjectiveInputs inputs = {});
    ObjectiveNatives(const ObjectiveNatives&) = delete;
    ObjectiveNatives& operator=(const ObjectiveNatives&) = delete;
    static bool hosts(const Script& script, const Function& function);
    std::optional<Value> call(const Script& script, const Function& function, const std::vector<Value>& arguments);
    // game::IsMultiplayer's answer for the next calls (solo: false). Port-only
    // (132ob8): the mission host sets it around an allow-listed call site.
    void set_multiplayer_answer(bool multiplayer) { host_.set_multiplayer(multiplayer); }
    // game::AddObjective(key) (0032e150 -> 0032e180 -> 00333b90 on the local
    // agent; solo sends no event). false (not applied) without the inputs.
    bool add_progression(const Script& script, const Function& function, const std::vector<Value>& arguments);
    const ObjectiveStage& stage() const { return stage_; }
    // The ObjectiveHost records (SetType / SetMenuString properties), in AddObjective order.
    const std::vector<Objective>& objective_records() const { return host_.objectives; }
    struct Outcome {
        Word objective{};
        bool success{};
        ObjectiveOutcome outcome;
        bool voice_delivered{};   // the voice actions went to gameSound
        bool notice_delivered{};  // the text went to the HUD ticker (MissionScriptHost, checkpoint 132f10)
    };
    // Every Succeed/Fail transition. With gameSound (set_game_sound) the voice
    // actions are delivered as Succeed delivers them, 00245400(23 or 22, 1) at
    // 00226088 / 002260b0 (game_sound_voice.hpp). The notification goes to the
    // HUD ticker in MissionScriptHost::hosted_call (00228d50 / 00228dc0).
    const std::vector<Outcome>& outcomes() const { return outcomes_; }
    void mark_notice_delivered(std::size_t i) { outcomes_.at(i).notice_delivered = true; }
    void set_game_sound(GameSoundManager* sound) { game_sound_ = sound; }
    struct ProgressionAdd {
        std::string key;
        std::int32_t intern_id{};
        std::uint32_t index{};
    };
    const std::vector<ProgressionAdd>& progression_adds() const { return progression_adds_; }
    const AgentObjectiveBits& progression_bits() const { return progression_bits_; }
    // SetDisplayLabel(manager, key[, GOBJ]) (00225880 / 00225820 -> 00227a90):
    // in solo the label resolved through 003fdb50 goes to the notification
    // transport 00242b00 (skipped when null); the GOBJ is not used. false
    // (deferred) without the localization.
    bool display_label(const Script& script, const Function& function, const std::vector<Value>& arguments);
    struct DisplayLabel {
        std::string key;
        std::optional<std::string> text;   // nullopt: unresolved, no notification
    };
    const std::vector<DisplayLabel>& display_labels() const { return display_labels_; }
    // Checkpoint 132zf5: SetDisplayString(manager, string) 002258c0: 00225c40
    // (identity) then 00227c50 = j 00242b00(*004ffc2c, string), the HUD ticker
    // with the string itself (no 003fdb50 lookup, no multiplayer branch). The
    // scripts pass game::GetString's text, so it is false (deferred) without
    // the localization.
    bool display_string(const Script& script, const Function& function, const std::vector<Value>& arguments);
    const std::vector<std::string>& display_strings() const { return display_strings_; }
    // SetMenuLabel(Objective, key) 00225300: 00225790 (the handle's objective)
    // then 00228940(objective, key, 0): +60 = 00228b20(003fdb50(key)), the
    // strdup SetMenuString 00225560 writes too, so the menu text is the
    // record's "SetMenuString" property. The multiplayer send (005721c8) is
    // skipped in solo. A typed null objective is the interpreter's skipped
    // call; a key 003fdb50 does not resolve throws (00228b20's strlen of
    // NULL). false (deferred) without the localization.
    bool set_menu_label(const Script& script, const Function& function, const std::vector<Value>& arguments);
    // Checkpoint 132zf9: SetFailLabel 00225200 / SetCompleteLabel 00225240 /
    // SetAddLabel 00225280 / SetMapLabel 002252c0 are SetMenuLabel's shape:
    // 00225790 then 00228580 / 00228670 / 00228760 / 00228850(objective, key,
    // 0), each storing 00228b20(003fdb50(key)) into the field its Set*String
    // (00225430 +80 / 00225480 +78 / 002254d0 +70 / 00225520 +6c) writes.
    bool set_label(const Script& script, const Function& function, const std::vector<Value>& arguments,
                   const std::string& name);
    // game::AddHudObjective(GOBJ, key[, cue]) (00274d40 / 00274ec0): the
    // localized label (null or empty: logged, no marker) into 00273570 under
    // the GOBJ's +c key (a duplicate keeps the old record); the two-argument
    // form passes a null cue (empty). game::RemoveHudObjective(GOBJ[, int])
    // (00274c80 / 00274b60): 00272d50(key, 1) in solo; the int only gates the
    // multiplayer send. The caller has resolved the GOBJ. false (deferred)
    // without the localization.
    bool add_hud_objective(Word object, const std::string& key, std::optional<std::string> cue);
    bool remove_hud_objective(Word object);
    // SetMapObject / ClearMapObject's objective (00225790): a typed objective
    // handle on the stage, 0 for the typed null (the interpreter's skipped
    // call). The map object state (+34 / +38) is MissionScriptHost's (its
    // liveness is the world's); these file and drop the kind-1 marker
    // (00273210 / 00272d50) and read the objective's +6c (SetMapString).
    Word map_objective(const Value& value, const std::string& native) const;
    bool add_map_marker(Word object, Word objective) { return markers_.add_map(object, objective); }
    // 00228190 (map_object_set): true when the objective's slot takes the GOBJ.
    bool set_map_object(bool slot_live, std::optional<Word> object, Word objective,
                        const std::optional<std::string>& cue) {
        return map_object_set(markers_, slot_live, object, objective,
                              cue ? std::optional<std::string_view>(*cue) : std::nullopt);
    }
    bool remove_map_marker(Word object) { return markers_.remove(object); }
    std::optional<std::string> map_string(Word objective) const;
    struct MarkerEvent {
        MarkerAction action{};
        ObjectiveMarker marker;
    };
    // Only the calls that changed the list (insertions and removals).
    const std::vector<MarkerEvent>& marker_events() const { return marker_events_; }
    const ObjectiveMarkers& markers() const { return markers_; }
    // 00273b40's state 1 / 2 pass over the records (ObjectiveMarkers::update).
    void update_markers(const std::function<bool(std::uint32_t)>& found) { markers_.update(found); }
    const MissionObjectiveInputs& inputs() const { return inputs_; }
    // game::GetString(key) (003fd290 -> 003fdb50: 003fe650 lookup, common
    // pool first, then the mission pool past the base count; 003fdb80 text).
    // With the localization the result is the text and a missing key throws
    // (the original returns NULL, which its strdup consumers fault on);
    // without it the symbolic key is returned (research runs).
    struct StringLookup {
        std::string key;
        std::int32_t id{};
        std::string text;
    };
    const std::vector<StringLookup>& string_lookups() const { return string_lookups_; }
private:
    MissionObjectiveInputs inputs_;
    std::vector<StringLookup> string_lookups_;
    ObjectiveHost host_{false};
    ObjectiveStage stage_;
    std::vector<Outcome> outcomes_;
    std::vector<ProgressionAdd> progression_adds_;
    AgentObjectiveBits progression_bits_;
    std::vector<DisplayLabel> display_labels_;
    std::vector<std::string> display_strings_;
    ObjectiveMarkers markers_;
    std::vector<MarkerEvent> marker_events_;
    GameSoundManager* game_sound_{};
};

// The world's AI NPCs (cNPC script receivers; NPC_ACTIVATION_AND_RESPAWN.md
// section 1) by population ID. Play: ActorWorldScriptNpcs
// (mission_script_npcs.hpp).
class MissionScriptNpcs {
public:
    virtual ~MissionScriptNpcs() = default;
    // The AI component (actor +58) of an NPC record; nullptr when the world
    // does not host that record.
    virtual NpcAiComponent* component(Word npc) = 0;
    // Actor virtual +5c(1) (001b53d0): what 00157800 does in single player
    // for ActivateAnon (0014a2c0, gobj null) and Activate(npc, gobj)
    // (0014a280; the gobj, 003cc820, is not used in solo).
    virtual void activate(Word npc) = 0;
    // Activate(NPC) (0014a250 -> component +48 = Activate 00157df0): the
    // component's own Activate, which returns unless +159 (inactive) and,
    // for an absent actor, calls the actor's +5c(1) when +44 holds (that
    // re-enters 00157df0 through the actor's activation). False when not
    // hosted (deferred).
    virtual bool activate_component(Word) { return false; }
    // DeactivateAnon (component +c0 = 00157510), what
    // DeactivateForAllPlayers (0014a010) calls unless +141 bit 7: in single
    // player the actor's deactivation (+60(1), 001b5190, then Deactivate
    // 00157ad0) when the actor is present.
    virtual void deactivate_anonymous(Word npc) = 0;
    // Deactivate(NPC) (0014a050 -> component +4c = 00157ad0): the actor's
    // deactivation (+60(1), 001b5190, which calls +4c again) when the actor
    // is present, otherwise the component's own Deactivate (route release,
    // the AIDeactivatedMsg unless +5d, +159 = 1). False when not hosted.
    virtual bool deactivate(Word) { return false; }
    // game::ResetNpc(NPC) (001485e0 -> 00148600, checkpoint 132sx9): unless
    // the actor's +7c (00184cd0, the generator-spawned +3598) answers
    // nonzero, the component's Deactivate (+4c), the destructor 001b57c0 and
    // the in-place rebuild 001b8130 (role 2, inactive, max health, the same
    // ID, serial, manifest, items and node at the checkpoint object's spawn
    // matrix). False when the world cannot rebuild an actor (deferred).
    virtual bool reset_npc(Word) { return false; }

    // Deactivate(NPC, GOBJ) (0014a080 -> 001573d0; checkpoint 132zf9): in
    // single player (005721c8 clear) the actor's +60(1) (001b5190) with no
    // presence check; the caller tests +141 bit 7 first. False when not hosted.
    virtual bool deactivate_actor(Word) { return false; }
    // ActivateByRadius(npc, r) (00148ee0 -> component +1cc = 0014ad30), with
    // the closest player's eye distance (003ce850) for the radius check.
    virtual void activate_by_radius(Word npc, float radius) = 0;
    // SetNotGasable (00148f80 -> 0014a750 -> 00199040): actor +346b.
    virtual void set_not_gasable(Word npc, bool value) = 0;
    // 003ce850(npc's actor, &d2): the squared eye distance to the closest
    // player (single player: the local player). Throws when the original
    // would fall back on 0040bf10's cached player (no player below 1e8).
    virtual float closest_player_distance_squared(Word npc) = 0;
    // The AI globals (004ea350).
    virtual NpcAiSystem& system() = 0;
    // The perception and mode natives (NPC_COMBAT.md, N4a/N4b; npc_alert.hpp).
    // object / target: an NPC record's actor, nullopt for the local player's
    // actor. false when the world does not host the NPC's perception (the
    // call is then deferred). AddAwareness 0014a410, AddAwarenessOfPlayers
    // 00149420, SetTarget 0014a210, ResumeAITargeting 0014a1c0, ResumeAIMode
    // 0014a470, LookAround 00149e50.
    virtual bool add_awareness(Word, std::optional<Word>) { return false; }
    virtual bool add_awareness_of_players(Word) { return false; }
    // AddAwareness(NPC, Node) / AddAwareness(NPC, GOB): the node object
    // (index into the level's nodes) / a population GOB (its record ID).
    virtual bool add_awareness_node(Word, std::int32_t) { return false; }
    virtual bool add_awareness_object(Word, Word) { return false; }
    virtual bool set_target(Word, std::optional<Word>) { return false; }
    // SetTarget(NPC, Node): the node object (index into the level's nodes).
    virtual bool set_target_node(Word, std::int32_t) { return false; }
    // SetTarget(NPC, GOB): a population GOB (its record ID) as the target.
    virtual bool set_target_object(Word, Word) { return false; }
    // Checkpoint 132sx10: SetGrenadeTarget(NPC, GOBJ) 00149810 -> +19c =
    // 0014b410(c, GOBJ, 4.0, 0). The GOBJ as the host resolved it: null, an
    // actor (nullopt: the player), a node (index) or a population GOB (ID).
    struct GobjTarget {
        enum class Kind { null, actor, node, object } kind{};
        std::optional<Word> actor;
        std::int32_t node{-1};
        Word object{};
    };
    virtual bool set_grenade_target(Word, const GobjTarget&) { return false; }
    virtual bool resume_ai_targeting(Word) { return false; }
    virtual bool resume_ai_mode(Word) { return false; }
    virtual bool look_around(Word) { return false; }
    // StopAndLookAround 00149e20 -> +138 = 0014dca0(c, 100) (132sx10, MINSK).
    virtual bool stop_and_look_around(Word) { return false; }
    // RequestAction (00149ce0 -> +150 = 0014d8f0 -> 0014d900(c, the enum's
    // low byte, 100, 0.0)); false when the world does not host the NPC.
    virtual bool request_action(Word, std::uint8_t) { return false; }
    // RequestFacial (00149e80 -> +12c = 0014dea0(c, object, 100, the float):
    // look entry 2); false when the world does not host the NPC's perception
    // and attack stages (the call is then deferred).
    virtual bool request_facial(Word, std::optional<Word>, float) { return false; }
    // RequestFacial of a Node GOBJ (index into the population's nodes): the
    // node object (vtable 004d95b0) is the look entry's object, its +c the ID.
    virtual bool request_facial_node(Word, std::int32_t, float) { return false; }
    // RequestFacial of a population GOB (its record ID): the look entry's
    // object is the GOB, its eye +c0 = 0014d340 -> +e4 003cd260.
    virtual bool request_facial_object(Word, Word, float) { return false; }
    // RequestFacial of a hotbox (its +0c key): the look entry's object is the
    // box (vtable 004d9ac0), its eye +c0 = 0014d340 -> +e4 003cd260
    // (hotbox_eye_c0); false when the world does not host the box or the NPC.
    virtual bool request_facial_hotbox(Word, Word, float) { return false; }
    // RequestFacial of a sound object (SND record ID; SoundGOBJ vtable 004daf30,
    // whose ctor 00212ee0 attaches its record matrix through +90 003cdff0): its
    // eye +c0 = 0014d340 -> +e4 003cd260 with +88 0013ee60 null, the position
    // (w 1). false when the world does not host the sound object or the NPC.
    virtual bool request_facial_sound(Word, Word, float) { return false; }
    // RequestLookAt (00149ed0 -> +128 = 0014dfb0(c, object, 100, the float):
    // look entry 0, +80 = 0), with the same GOBJ kinds and hosting as
    // RequestFacial. ResumeAILookAt (00149df0 -> +13c = 0014de30(c, 100)) and
    // DeactivateDead (00149fe0 -> +160 = 0014b680(c, int != 0): +141 bit 2).
    // false when the world does not host the NPC (the call is deferred).
    virtual bool request_look_at(Word, std::optional<Word>, float) { return false; }
    virtual bool request_look_at_node(Word, std::int32_t, float) { return false; }
    virtual bool request_look_at_object(Word, Word, float) { return false; }
    virtual bool request_look_at_hotbox(Word, Word, float) { return false; }
    virtual bool request_look_at_sound(Word, Word, float) { return false; }
    virtual bool resume_look_at(Word) { return false; }
    virtual bool deactivate_dead(Word, bool) { return false; }
    // EquipGoggles (00149070 -> +1f0 = 0014a7f0: 001992b0(actor, word != 0,
    // 0), actor_goggles.hpp); false when the world does not host the NPC's
    // secondary component and inventory (the call is then deferred).
    virtual bool equip_goggles(Word, bool) { return false; }
    // N5a (npc_script_control.hpp): MoveToNode (0014a5c0 -> +100 = 0014be50;
    // the byte it returns, nullopt when the world does not host the NPC) and
    // the three-argument SetMode (0014a4a0 -> +dc = 0014bcf0 with the node;
    // false when not hosted). node: the index in the node collection.
    virtual std::optional<std::uint8_t> move_to_node(Word, std::int32_t) { return std::nullopt; }
    // Checkpoint 132sx10: MoveToGobj(NPC, GOBJ) 0014a580: 003cc820 (identity), +104 = 0014be10(c, GOBJ)
    // (npc_move_to_object with the GOBJ's +c); the byte it returns. nullopt when not hosted.
    virtual std::optional<std::uint8_t> move_to_object(Word, Word) { return std::nullopt; }
    virtual bool set_mode_at_node(Word, std::uint32_t, std::int32_t) { return false; }
    // GetHitPoints (00149660): cvt.w.s of the actor's +40 health. nullopt when
    // the world does not host NPC health; the native then throws.
    virtual std::optional<std::int32_t> hit_points(Word) { return std::nullopt; }
    // AddWeapon (00149970 -> component +1b8 = 0014b890) and DrawWeapon
    // (00149a10 -> +1ac = 0014bae0) with the script's int; false when the
    // world does not host the NPC's weapon inventory (the call is deferred).
    virtual bool add_weapon(Word, std::int32_t) { return false; }
    virtual bool draw_weapon(Word, std::int32_t) { return false; }
    // 132fx2: ChangeWeapon (001499e0 -> +1b0 = 0014b9e0; false: deferred) and GetAimPercent (00149120 ->
    // +1e4 = 00149150, the aim block +20; nullopt: the native throws).
    virtual bool change_weapon(Word) { return false; }
    virtual std::optional<float> aim_percent(Word) { return std::nullopt; }
    // Kill (00149fb0 -> component +168 = cNPC_Kill 0014b370): unless the
    // actor's life state (+108) is 3 with +33a5 clear, its damage virtual
    // +100(actor, actor, 2, 0xa, 0, 0, 0, float(cvt.w.s(+40))). false when the
    // world does not host NPC damage (the call is deferred).
    virtual bool kill(Word) { return false; }
    // Checkpoint 132zf4 (the TORONTO2 SEWER natives; each wrapper is `lw
    // a0,(a0)` and a cNPC vtable 004d9380 slot, return 0; false: the world
    // does not host it and the call is deferred):
    //   SetHitPoints 001495b0 -> +174 = 001495f0: when cvt.w.s(actor +40) > 0,
    //     003cc990(actor, hp, 0): +40 = cvt.s.w(hp) (the multiplayer message
    //     behind byte 005721c8 is skipped in solo);
    //   SetInvulnerable 001490e0 (sltu a1, zero, v0) -> +1e8 = 0014a810 ->
    //     001b8750: byte +33a4 (the 005721c8 sends are skipped in solo);
    //   DontDropWeapons 00149040 -> +200 = 0014a7d0: *(actor +3524) +a8 = 1;
    //   SetOnFire 001493b0 -> +17c = 0014b350: j 00186360(actor, 1, 0, 0, 0, -1.0);
    //   ResumeMovement 00149c70 -> +158 = 0014d470;
    //   RestrictMovementToNodeGroup 00149500 -> 0015c120 (identity) -> +1f4 =
    //     0014bea0 (node: the index in the node collection).
    virtual bool set_hit_points(Word, std::int32_t) { return false; }
    virtual bool set_invulnerable(Word, bool) { return false; }
    virtual bool dont_drop_weapons(Word) { return false; }
    virtual bool set_on_fire(Word) { return false; }
    virtual bool resume_movement(Word) { return false; }
    // Checkpoint 132zf9 (BELARUS1 MILL): CarryBodyToNode 0014a520 -> +108 =
    // 0014bd60(c, body, node) (npc_carry_body_to_node; body: a population
    // record ID, node: the index in the node collection; the byte it
    // returns, nullopt when the NPC is not hosted).
    virtual std::optional<std::uint8_t> carry_body_to_node(Word, Word, std::int32_t) { return std::nullopt; }
    // Checkpoint 132zf11: Follow 00148880 -> +fc = 001488f0 (npc_follow; the
    // target's population ID, nullopt for a null GOBJ); false: not hosted.
    virtual bool follow(Word, std::optional<Word>, float, float) { return false; }
    virtual bool restrict_to_node_group(Word, std::int32_t) { return false; }
    // Checkpoint 132zf5: Teleport 00149c30 -> 0015c120 (identity) -> +10c =
    // 0014afe0 (the teleport message, 0015ff90, ResumeAIMode, 0014d510).
    virtual bool teleport(Word, std::int32_t) { return false; }
    // Checkpoint 132zf7 (TORONTO3 MALL; cNPC vtable 004d9380 slots, false
    // defers the call):
    //   SetUnconscious 00148fe0 -> +208 = 0014a770: unless actor +33a5, the
    //     actor's damage virtual +100(actor, actor, 1, 7, 0, 0, 0, 0.0);
    //   SetPickupable 00149230 (a1 = int != 0, a2 = 1) -> +1d4 = 0014a890;
    //   ActivateByRadiusOff 00148eb0 -> +1d0 = 0014ae30;
    //   AddGroupToPathRegion 001494c0 -> 0015c120 (identity) -> +1f8 =
    //     0014bf60 (node: the index in the node collection).
    virtual bool set_unconscious(Word) { return false; }
    virtual bool set_pickupable(Word, bool) { return false; }
    // Checkpoint 132sx10 (BELARUS2): SetHasDisguise 001491f0 (a1 = int != 0,
    // a2 = 1) -> +1d8 = 0014ac90 (actor +3469; NpcAiController::
    // script_set_disguise).
    virtual bool set_has_disguise(Word, bool) { return false; }
    virtual bool activate_by_radius_off(Word) { return false; }
    virtual bool add_group_to_path_region(Word, std::int32_t) { return false; }
    // RemoveGroupFromPathRegion 00149480 -> 0015c120 (identity) -> +1fc =
    //   0014bf00 (npc_forbid_region; 132sx10, YEMEN2).
    virtual bool remove_group_from_path_region(Word, std::int32_t) { return false; }
    // game::ActivateBodyToss(GOBJ, bool) 00476740: the message key 00588038
    // (vtable 004e1ae0, +24 = flag != 0) to 003cc820(object), transport 0.
    // An NPC actor forwards it to its AI component (001b3bf0), whose
    // dispatcher 00151d30 never tests the key. false defers the call.
    virtual bool activate_body_toss(Word, bool) { return false; }
    // Checkpoint 132zf5: the four-argument ForceWeaponFire 00149b50 (a1 =
    // shots, a2 = item, f12 = delay -> +198 = 0014b470). With an item >= 0
    // it reads the held item (+1a4) and may select the item (+1ac =
    // 0014bae0); false: the world does not host the NPC's weapons (deferred).
    virtual bool force_weapon_fire(Word, std::int32_t, std::int32_t, float) { return false; }
    // The actor virtual +e4 (00132860, class 004da690: the model centroid
    // +2ea0 through the transform component +2e78) of the NPC's actor, for
    // game::Distance; nullopt when the NPC is not hosted (Distance throws).
    virtual std::optional<RootVector> position_e4(Word) { return std::nullopt; }
    // What 001710a0 reads for MakeNPCInteractable's box: the actor's
    // transform component +40 translation and its virtual +88 bounds
    // (00184380: model context +2e80 minimum, +2e90 maximum). nullopt when
    // the NPC is not hosted (MakeNPCInteractable then throws).
    struct InteractableInputs {
        RootVector translation{};
        RootVector bounds_minimum{}, bounds_maximum{};
    };
    virtual std::optional<InteractableInputs> interactable_box_inputs(Word) { return std::nullopt; }
    // Checkpoint 132sx8. A GOBJ argument as the world names it (003cc820 is
    // the identity): an actor (an NPC record, nullopt for the local player),
    // a node (its index in the node collection: the node object
    // npc_node_object) or a population GOB (its record ID).
    struct AiObject {
        enum class Kind : std::uint8_t { actor, node, object } kind{};
        std::optional<Word> actor;
        std::int32_t node{-1};
        Word object{};
    };
    // RemoveAwareness(NPC, GOBJ) 0014a3c0 -> +e8 = 0014a400 -> 0016c8d0;
    // ResendNewAwarenessMessage(NPC) 0014a390 (+f4(c, 0)) and (NPC, GOBJ)
    // 0014a340 (003cc820, +f4(c, object)) -> 0014a380 -> 0016c590 (nullopt:
    // object 0, every record); ClearTarget(NPC) 0014a1e0: 0016ddb0(p, 0, 100).
    // false: the world does not host the NPC's perception (deferred).
    virtual bool remove_awareness(Word, const AiObject&) { return false; }
    virtual bool resend_new_awareness(Word, const std::optional<AiObject>&) { return false; }
    virtual bool clear_target(Word) { return false; }
    // InViewCone(NPC, GOBJ) 00149270 -> +188 = 0014afc0 -> 0016c1e0(p,
    // object, 1), andi 0xff; nullopt: not hosted (the native then throws).
    virtual std::optional<bool> in_view_cone(Word, const AiObject&) { return std::nullopt; }
    // SetViewConeRadius 00149320 -> +180 = 00149360 (perception +10) and
    // SetViewConeAngle 001492b0 -> +184 = 001492f0 (perception +14).
    virtual bool set_view_cone_radius(Word, float) { return false; }
    virtual bool set_view_cone_angle(Word, float) { return false; }
    // StoreWeapon(NPC) 00149a80 -> +1a8 = 0014bbd0 (npc_store_weapon); false
    // when the world does not host the NPC's weapon inventory (deferred).
    virtual bool store_weapon(Word) { return false; }
    // Checkpoint 132sx10: RemoveWeapon(NPC, int) 00149930 -> +1bc = 0014b790
    // (npc_remove_weapon; false without the NPC's weapon inventory) and
    // SendNewAwarenessOfPlayers(NPC) 0014a2f0 -> +f8 = 0014a320 (perception
    // +21 bit 0).
    virtual bool remove_weapon(Word, std::int32_t) { return false; }
    virtual bool send_new_awareness_of_players(Word) { return false; }
    // Checkpoint 132sx10: game::ForceHolster(GOBJ) 00185410 on an NPC actor: 00143cc0(actor +3524), i.e.
    // 00143b60 (holster) unless +84 is 6 (nothing held); the 005721c8 send is skipped in solo. false when
    // the world does not host the NPC's weapon inventory (deferred).
    virtual bool force_holster(Word) { return false; }
    // Checkpoint 132sx10: game::GetActiveWeapon(GOBJ) 001855c0 on an NPC actor: the item of the held
    // slot of +3524 (+8 + 16 * +84), -1 when +84 is 6. nullopt when the world does not host the NPC's
    // weapon inventory (the call then throws: it returns a value).
    virtual std::optional<std::int32_t> active_weapon(Word) { return std::nullopt; }
    // SetNotTaserable(NPC, int) 00148fb0 (sltu a1, zero, v0) -> +20c =
    // 0014a760 -> 00198f90(actor, flag, 0): actor +346a (the 005721c8
    // replication is skipped in solo). SetFireInvulnerability(NPC, int)
    // 00149010 (sltu) -> +204 = 0014a7c0: actor +33b8 = the flag.
    virtual bool set_not_taserable(Word, bool) { return false; }
    virtual bool set_fire_invulnerability(Word, bool) { return false; }
    // StopMovement(NPC, float) 00149ca0 -> +154(c, 100, f12) = 0014d4d0.
    virtual bool stop_movement(Word, float) { return false; }
    // Checkpoint 132ms3: game::SetFootstepSoundRadius(GOBJ, float) 00169b70 on an NPC: the AI
    // controller 004d9380's +58 (0015b7e0) answers 1, so swc1 f20, actor +31f8.
    virtual bool set_footstep_sound_radius(Word, float) { return false; }
};

// The world side of the natives the live host applies (play: the solo
// simulation). Boxes are named by their population ID.
class MissionScriptWorld {
public:
    virtual ~MissionScriptWorld() = default;
    // Enable/Disable(Hotbox) (0016fcd0/0016fbb0 -> +2c); false when the world
    // does not host the box.
    virtual bool set_hotbox_enabled(Word box, bool enabled) = 0;
    // Occupied(Hotbox) (0016fb50: +0x98 > 0); nullopt when not hosted.
    virtual std::optional<bool> hotbox_occupied(Word box) const = 0;
    // SetInteractMessage(Hotbox, key) 0016fb70 (+0xa0 = 003fdb50(key)) and
    // ClearInteractMessage(Hotbox) 0016fb60 (+0xa0 = 0); false when the
    // world does not host the box (the call is deferred).
    virtual bool set_hotbox_interact_message(Word, std::optional<std::string>) { return false; }
    // MakeNPCInteractable's run-time box (00170e20 + 00170460,
    // add_runtime_hotbox): appended to the world's boxes with its ID, the
    // NPC actor (population ID) as its only link and the bounds of
    // npc_interactable_bounds. false when the world does not host hotboxes
    // (the native then throws).
    virtual bool add_npc_hotbox(Word, Word, const NpcInteractableBounds&) { return false; }
    // One draw of the shared game RNG (00127dd0, next_game_random).
    virtual std::uint32_t game_random() = 0;
    // game::Randomize's seed 00269230: T1_COUNT (10000800) + *00533870 as an
    // unsigned word; nullopt when the world has no timer model (deferred).
    virtual std::optional<std::uint32_t> timer_t1_sum() { return std::nullopt; }
    // srand 00127db8: the shared RNG state (*(00489738) + a8) = the
    // zero-extended seed (dsll32 / dsrl32, sd). false when not hosted.
    virtual bool seed_game_random(std::uint32_t) { return false; }
    // 00232bf0(type, a1, f12, f13, f14, f15) (game::SmokeToronto3 002326f0 ->
    // 002329d0: type 0x15, a1 -1, 20.0, 0.0, 40.0, 0.4): the world's
    // atmosphere effect (world +1a4, 00177060). false when the world hosts
    // none (deferred).
    struct AtmosphereEffect {
        std::int32_t type{};
        std::int32_t a1{};
        float f12{}, f13{}, f14{}, f15{};
        // The weather workers' group state 003a5f20(0x53b5c0, 0, 0, f14, f15,
        // f16) before the record (all zero for the smoke fields, which make
        // no such call).
        float group_f14{}, group_f15{}, group_f16{};
    };
    virtual bool start_atmosphere_effect(const AtmosphereEffect&) { return false; }
    // MSGGAME_ADD_CHECKPOINT (key 004f7930) queued with transport 0 to the
    // player's actor by SetCheckpoint (0022e080): the node's +80 position, its
    // +8c yaw (population_node_yaw) and a copy of the label. false when the
    // world has no player actor to send it to.
    virtual bool add_checkpoint(Word node, const std::array<float, 3>& position, float yaw,
                                const std::string& label) = 0;
    // The CAS action game::PerformAction_WithObject (003e67c0) queues with
    // transport 0 to the player's actor: the action byte, the InteractGOBJ by
    // its key, its +a8 node (position +80, yaw +8c) and its +ac world object
    // key. false when the world has no player actor to send it to.
    struct ObjectActionNode {
        Word id{};
        std::array<float, 3> position{};
        float yaw{};
    };
    virtual bool perform_object_action(std::int32_t action, Word object, const ObjectActionNode& node,
                                       Word world_object) = 0;
    // The 4/5-argument forms (003e64d0 / 003e6450 -> 003e6910) queue the
    // kind-10 message with transport 0 to the player's actor
    // (build_named_object_action: the action, forced to 0x25 by a name; the
    // lowercased 47-byte name; the time, -2.0 as FLT_MAX; +64 = 0), with the
    // object links of the 3-argument form. false (deferred) by default and
    // when the world has no player actor.
    struct NamedObjectAction {
        std::int32_t action{};
        std::array<char, 48> name{};
        float time{-1.0F};
        std::uint8_t byte_64{};
    };
    virtual bool perform_named_object_action(const NamedObjectAction&, Word /*object*/, const ObjectActionNode&,
                                             Word /*world_object*/) { return false; }
    // Checkpoint 132zf5: the same CAS action to an NPC's actor (003e67c0 /
    // 003e6910 take the receiver from 003cb220: an AI handle's +30 owner, or
    // the NPC's GOBJ), as game::PerformAction. named = nullopt for the
    // 3-argument form. false (deferred) by default.
    virtual bool perform_npc_object_action(Word /*npc*/, std::int32_t /*action*/,
                                           const std::optional<NamedObjectAction>& /*named*/, Word /*object*/,
                                           const ObjectActionNode&, Word /*world_object*/) { return false; }
    // game::PerformAction (2/3/4 arguments, no object): the kind-7 message
    // (003e67c0; name empty) or the kind-10 one (003e6910; name set, action
    // 0x25) queued with transport 0 to an actor: npc = the NPC record's actor
    // (an AI handle, through its component's +30 owner, or the NPC's GOBJ),
    // nullopt = the player's actor. false (deferred) by default.
    struct ActorAction {
        std::int32_t action{};
        std::optional<std::array<char, 48>> name;   // set: kind 10
        float time{-1.0F};
    };
    virtual bool perform_action(std::optional<Word> /*npc*/, const ActorAction&) { return false; }
    // game::StopPerformingAction (003e6300): the action-0x28 request (kind 2,
    // +2c = 1) queued with transport 0 to the same actor as perform_action.
    // false (deferred) by default.
    virtual bool stop_performing_action(std::optional<Word> /*npc*/) { return false; }
    // game::SetRadarBlip (0022c180) / game::ClearRadarBlip (0022c0f0): the
    // object's +c to the HUD targets *004ffc2c + 90 (00273890(&id, 1) /
    // 00272d50(&id, 0)). false (deferred) by default.
    virtual bool set_radar_blip(Word /*object*/, bool /*on*/) { return false; }
    // The world's AI NPCs; nullptr when NPCs are not hosted (their void
    // natives are then deferred and the value-returning ones throw).
    virtual MissionScriptNpcs* npcs() { return nullptr; }
    // The level's pathed objects (GOB kind 2, class 004db280, built by
    // 00219820) by population ID; nullptr when not hosted (SetSpeed/SetPos
    // are then deferred and GetPos throws).
    virtual PathWorld* pathed_objects() { return nullptr; }
    // The level's lifts (GOB kind 3, ElevatorGOBJ 004db040, built by 00215700)
    // by population ID; nullptr when not hosted (the void ElevatorGOBJ
    // natives are then deferred and the value-returning ones throw).
    virtual ElevatorWorld* elevators() { return nullptr; }
    // The level's generators (cGenerator 004db680) by population ID; nullptr
    // when not hosted (the Generator natives are then deferred).
    virtual GeneratorWorld* generators() { return nullptr; }
    // Checkpoint 132zf11: the script tanks (Tank, vtable 004dcf90, tank.hpp)
    // and the world calls they make. Without them game::CreateTank throws
    // naming 002840d0. tank_handle is the host's object for a new tank (a
    // bullet's shooter, 00286730); tank_target the combat object and its +c
    // for AimAt's GOBJ (003cc820, the identity), nullopt when not hosted.
    virtual MissionTanks* tanks() { return nullptr; }
    virtual TankHost* tank_host() { return nullptr; }
    virtual std::uint64_t tank_handle(Word) { return 0; }
    virtual std::optional<TankObjectRef> tank_target(Word) { return std::nullopt; }
    // Whether the world runs their update 00216c60 every frame, as the
    // object loop 00133420 does for every registered object (+4c 0021a8c0
    // runs +48 for one that is not present, the loop runs it for one that
    // is). Without it SetPos/GetPos on a moving object throw (its state
    // would be stale).
    virtual bool pathed_objects_ticked() const { return false; }
    // game::SetPlayerWeapons(first, second) (00185330) on the local player's
    // weapon inventory (+3524); false when the world hosts no weapons (the
    // call is then deferred).
    virtual bool set_player_weapons(std::int32_t, std::int32_t) { return false; }
    // Checkpoint 132sx10: StealPlayerWeapons(Backpack, slot, slot[, slot])
    // 00253c90 / 00253cc0 (the two-slot form passes 8) -> 00253dd0: the
    // player's weapons in those slots move into the backpack (its ID);
    // false when the world does not host it (the call is then deferred).
    virtual bool steal_player_weapons(Word, const std::array<std::uint8_t, 3>&) { return false; }
    // game::CreateBlast(GOBJ, int, float, float, float) (0021a170) on the
    // record's object: damage, strength, inner and outer radius; false when
    // the world hosts no explosions (the call is then deferred).
    virtual bool create_blast(Word, std::int32_t, float, float, float) { return false; }
    // Checkpoint 132cp3: game::AddTremor(GOBJ, duration, magnitude, radius)
    // (0012f8a0 -> 0012f910): a tremor at the object's virtual +e4 with inner
    // radius / 3 and outer radius (002727b0, tremor.hpp); false when the world
    // hosts no tremor manager (the call is then deferred).
    virtual bool add_tremor(Word, float, float, float) { return false; }
    // game::SpawnParticle(EffectName, GOBJ) (00230770 -> 002307b0(index, object,
    // 1)): the effect table entry `index` (00232120) on the identity frame
    // whose row 3 is the object's virtual +e4, then the solo
    // cNetSpawnParticleMsg (003c9420 to the object); false when the world
    // hosts no effect table (the call is then deferred).
    virtual bool spawn_particle(Word, std::int8_t) { return false; }
    // Checkpoint 132sx5: game::SpawnLight(GOBJ, float, float) (00230610 ->
    // 00230660(object, 1, f12, f13)): 00231f00's white point light at the
    // object's virtual +e4 (outer f12, scheduled over [0, f13]), then the solo
    // cNetSpawnLightMsg (key 004f7c00, 003c9420 to the object); false when the
    // world hosts no effect service (the call is then deferred).
    virtual bool spawn_light(Word, float, float) { return false; }
    // Checkpoint 132ms8d: the same for an actor (class 004da690, whose +e4 is
    // 00132860): the host resolves the player's or an NPC record's +e4 as for
    // game::Distance (CHECHNYA's photo triggers schedule SpawnLight(Who(msg),
    // 50.0, 0.269) on the player). The message goes to the actor's receiver
    // 001b3b60, which has no branch for the key (003cea10 -> 003caca0).
    virtual bool spawn_actor_light(Word, const RootVector&, float, float) { return false; }
    // Checkpoint 132g: game::Shatter(GOBJ glass, GOBJ other) (00230d90 ->
    // 00230de0(glass, other, 0)) on the glass record's object, with the
    // other object's +e4; false when the world hosts no shatter (deferred).
    virtual bool shatter(Word, const RootVector&) { return false; }
    // Enable/Disable(FireObj) (00261d60 / 00261d30 -> virtual +64 / +68 with
    // -1: 00261c00 / 00261c90, solo +2c = 1 / 0) on the FIR: zone of that
    // population ID; false when the world hosts no fire zones (deferred).
    virtual bool set_fire_enabled(Word, bool) { return false; }
    // Enable/Disable(PARTICLE_GOBJ) 00392640 / 00392610 -> virtual +64 / +68
    // with -1 (vtable 004daa10: 001c5710 / 001c5680, solo +2c = 1 / 0) and
    // ScaleBBox(PARTICLE_GOBJ, float) 003925d0 -> virtual +8c (001c5670:
    // +70 = f12) on the PLR: placement of that population ID; false when the
    // world hosts no placements (the call is then deferred).
    virtual bool set_placement_enabled(Word, bool) { return false; }
    virtual bool scale_placement_bbox(Word, float) { return false; }
    // Checkpoint 132vo2: Trigger(SoundGOBJ) 00210940 / KillSnd(SoundGOBJ)
    // 00210890 (`lw a0,(a0)`, return 0) on the SND: object of that population
    // ID: the play / stop of its sound (SoundGobjWorld::trigger / kill); false
    // when the world hosts no sound objects (the call is then deferred).
    virtual bool sound_object(Word, bool trigger) { (void)trigger; return false; }
    // Play(NIEventOBJ) 00237960 / Play(NIEventLight) 002375b0: `lw a0,(a0)`,
    // virtual +114 = 0023e820 (the SHUTTLE message, mode 0), return 0; on the
    // NIE: / NIL: object of that population ID; false when the world hosts no
    // event objects (the call is then deferred).
    virtual bool play_event_object(Word, bool /*light*/) { return false; }
    // Reset(NIEventOBJ) 00237900: `lw a0,(a0)`, virtual +11c = 0023e720 (the
    // SHUTTLE message, mode 2), return 0; false when the world hosts no
    // event objects (the call is then deferred).
    virtual bool reset_event_object(Word) { return false; }
    // Stop(NIEventOBJ) 00237930: virtual +118 = 0023e790(1), the SHUTTLE mode 1 (+20 left clear).
    virtual bool stop_event_object(Word) { return false; }
    // SetEndState(NIEventOBJ) 00237870: virtual +128 = 0023b4c0 at once (NiEventWorld::end_state; checkpoint
    // 132sx12); false when the world hosts no event objects (the call is then deferred).
    virtual bool end_state_event_object(Word) { return false; }
    // Checkpoint 132uk3: Disable / Enable(NIEventOBJ) 002378a0 / 002378d0 (virtual +68 / +64 with -1 = 0023bdc0 /
    // 0023be50: +2c of the object and its NIE children, NiEventWorld::set_enabled), return 0; false when the world
    // hosts no event objects (the call is then deferred).
    virtual bool enable_event_object(Word, bool /*enable*/) { return false; }
    // game::RequestLevelEnd (0012f9a0 -> 0012fa60(G, 0)): the end request,
    // the AgentData +b74 and the synchronous MSGGAME_ENDLEVEL delivery to
    // 00136a90 (state 0x13 and the 1 s fade-out; ignored in 0x13/0x14).
    // false when the world hosts no game state (the call is then deferred).
    virtual bool request_level_end(std::uint8_t /*kind*/) { return false; }
    // The local player's actor virtual +e4 (00132860), for game::Distance;
    // nullopt when the world has no player actor (Distance then throws).
    virtual std::optional<RootVector> player_position_e4() { return std::nullopt; }
    // A population GOB's virtual +e4 (003cd260 for every GOB vtable: 004db4c0
    // kinds 1 / 14, 004db280 kind 2, 004db160 kind 5, 004db3a0 kind 6), for
    // game::Distance; nullopt when the world does not host it (Distance throws).
    virtual std::optional<RootVector> object_position_e4(Word) { return std::nullopt; }
    // game::LoadAnimation (001bfd30 -> 001bff40) while SKALEVEL.HOG is open:
    // load_level_animation on the world's models; nullopt when the world hosts
    // no models (the call is then deferred).
    virtual std::optional<LevelAnimationLoad> load_level_animation(const std::string&) { return std::nullopt; }
    // game::SetNotGasable(GOBJ, int) 00185140 on the player's actor: +346b
    // (read only by the gas exposure test 002596e0). false when the world
    // does not host the player's flag (the call is then deferred).
    virtual bool set_player_not_gasable(bool) { return false; }
    // Checkpoint 132sx8: game::SetPlayerInvulnerability(int) 00175480: for player
    // slots 0..3 (003fb8d0) whose object is of the actor class (+4c ==
    // *0049d010), 001b8750(actor, sltu(0, value)): byte +33a4 (the 005721c8
    // sends are skipped in solo). Solo play has slot 0 only. false when the
    // world does not host the player's flag (the call is then deferred).
    virtual bool set_player_invulnerable(bool) { return false; }
    // Checkpoint 132ms3: game::SetFootstepSoundRadius(GOBJ, float) 00169b70 on the local player: the
    // player component 004d9cf0's +54 (IsPlayer) answers 1, so swc1 f20, actor +31f8 (the radius of the
    // moving actor's running_world event, every 16 frames). false when the world does not host the player's actor.
    virtual bool set_player_footstep_sound_radius(float) { return false; }
    // Checkpoint 132sx10: game::ReSpawn(Player) 0022e1a0: 00175fa0 (identity), then 0022e1d0 queues
    // MSGGAME_RESPAWN (key 004f7940, vtable 004db890, +1c = 0x40, +24 = 0; 003c9420 a2 = 0) to the
    // component's actor +30, as the fade poll 00177c60 does (001b45dc on delivery). false when the world
    // does not host the player's actor queue (the call is then deferred).
    virtual bool respawn_player() { return false; }
    // Checkpoint 132sx10: game::ForceHolster(GOBJ) 00185410 on the player's actor: 00143cc0(+3524) (see
    // MissionScriptNpcs::force_holster). false when the world does not host the player's inventory.
    virtual bool force_holster_player() { return false; }
    // Checkpoint 132ms2: game::SetHardpointVis(GOBJ, slot, vis) 00185550 on an actor (npc: its population
    // ID, nullopt: the player): 00142e40(actor +3524, slot, vis != 0) writes the slot's byte +4 + 16 *
    // slot = vis and the actor byte +2498 + 00489d70[slot] = !vis (003870c0 does not draw the point's
    // model while it is set). false when the world does not host the actor's inventory (deferred).
    virtual bool set_hardpoint_vis(std::optional<Word>, std::uint8_t, bool) { return false; }
    // Checkpoint 132ms2: PushCurrent(GameCamera) 0017a160 -> 00131320(004ffb50, camera) and PopCurrent
    // 0017a130 -> 00131240(004ffb50) on the game world's active object list +c4 (the published object
    // 00583878 is the view camera). camera: the CAM: record's population ID. false when the world does
    // not host the list (deferred).
    virtual bool push_current_camera(Word) { return false; }
    virtual bool pop_current_camera() { return false; }
    // Checkpoint 132sx10: game::GetActiveWeapon(GOBJ) 001855c0 on the player's actor (see
    // MissionScriptNpcs::active_weapon). nullopt when the world does not host the player's inventory.
    virtual std::optional<std::int32_t> player_active_weapon() { return std::nullopt; }
    // Checkpoint 132sx11: AddWeapon(Player, int) 00175bf0 and RemoveWeapon(Player, int)
    // 00176060 on the local player's actor (see the host): gated by the item record's loaded
    // model +13c, then 00143df0(+3524, item, 1, 1, 0, 0) / 00142450(+3524, slot, 1, 0, 0).
    // false when the world does not host the player's inventory and weapon models (the call
    // is then deferred).
    virtual bool add_player_weapon(std::int32_t) { return false; }
    virtual bool remove_player_weapon(std::int32_t) { return false; }
    // Checkpoint 132sx11: game::RemoveAllBackpacks 00253c60 (j 002570c0): the dropped
    // backpacks destroyed, the INV: crates restored from the loadout snapshot (00256d90)
    // and the run-time boxes cleared (00170570). false when the world does not host the
    // backpacks (the call is then deferred).
    virtual bool remove_all_backpacks() { return false; }
    // Checkpoint 132sx11: game::ResetLevel 0012f6e0 -> 0012f700 up to its last step: the
    // pickup menu closed (00247320 on G +50), 002570c0, the areas finished (0025a120), the
    // agent's level statistics reset (00333df0), HUD +6dc = 0 / +6e0 = -1.0, the PDA
    // cleared (0013a290), the ticker reset (003fa1e0) and the decals killed (002795a0).
    // The VM clears the schedules (003dfb20) after it. false when the world does not host
    // the combat session and the HUD (the call is then deferred and nothing is reset).
    virtual bool reset_level() { return false; }
    // game::SwitchBodies(GOBJ, NPC) 00175630 -> 001b3b60 -> 00390fc0 on the
    // receiver actor's model +60 (nullopt: the player's, else that NPC's) with
    // the costume NPC's +a0 packing, then ActorModel_Setup 0038c920; and
    // game::RestoreBody(GOBJ) 001754f0 -> 001b3b60 -> 00391210 (the backup
    // put back). false when the world does not host the receiver's model
    // (the call is then deferred).
    virtual bool switch_bodies(std::optional<Word> /*receiver*/, Word /*costume_npc*/) { return false; }
    virtual bool restore_body(std::optional<Word> /*receiver*/) { return false; }
    // game::ShockToDeath(GOBJ) 00185280 on an actor (nullopt: the player's,
    // else that NPC's): the lethal stun 00184ce0(actor, 1, 1, 0.5, 10000.0)
    // when its life state is not 2 or 3 and it is not stunned
    // (CombatThrows::shock_to_death). false when the world does not host
    // stuns (the call is then deferred).
    virtual bool shock_to_death(std::optional<Word> /*actor*/) { return false; }
};

// 00267ee0's arithmetic on two +e4 positions (game::Distance(GOBJ, GOBJ)):
// vsub.xyzw, vmul.xyz, the w-lane sum (x*x + y*y) + z*z through ACC
// (vmulax.w / vmadday.w / vmaddz.w with vf0.w = 1), then vsqrt; each step
// one binary32 rounding (EE interpreter goldens, gen_gobj_distance.py).
float script_gobj_distance(const RootVector& a, const RootVector& b);

// GOBJ SetHitpoints 003cc5c0's store: +40 = cvt.s.w(int) (the EE rounds
// toward zero: game_int_to_float). GetHitpoints 003cc6d0's read: cvt.w.s(+40)
// (truncation). Outside the cvt.w.s range throws: SetHitpoints only stores a
// truncated float(int32), which is always in range. EE interpreter goldens,
// gen_gobj_hitpoints.py.
float gobj_set_hit_points(std::int32_t value);
std::int32_t gobj_hit_points(float health_40);

// The live solo script host: the mission's modules over the population
// objects, started like 003e20a0 + 003e1a20, then driven by HotboxMsg
// deliveries and the script event pump. Natives:
// - hosted: the objective callbacks (Succeed/Fail in the two-argument form,
//   game::AddObjective with the progression inputs); Enable/Disable/Occupied(Hotbox) through
//   the world; Team(GOBJ) (the player: 0 in solo; NPCs: npc_team), Team(Player)
//   (0), IsPlayer(GOBJ) on actors; SetHitpoints/GetHitpoints(GOBJ)
//   (003cc5c0 / 003cc6d0) on the +40 of population objects and event objects
//   (sections object / event_object), held by the host (a GetHitpoints before
//   any SetHitpoints, actors and other records throw); game::RandomInt on
//   the world's game RNG; game::SetCheckpoint(player, node, label) as the
//   world's queued ADD_CHECKPOINT; game::PerformAction_WithObject(actor,
//   action, object) for the player actor and an InteractGOBJ as the world's
//   queued CAS action; with the localization SetDisplayLabel (recorded
//   notification request), game::Add/RemoveHudObjective (ObjectiveMarkers),
//   game::RegisterItem (ItemTable) and game::PlayerAddItem/RemoveItem/
//   GetItemCount on the player actor's item list (ItemInventory) and
//   AddItem(NPC, key, n) on an NPC's (item_list); the
//   game::Team* relation natives on the team matrix (TeamRelations); with
//   the world's NPCs (MissionScriptWorld::npcs) the AI natives ActivateAnon,
//   Activate(NPC, GOBJ), ActivateByRadius, SetTeam/Team(AI), SetSkill,
//   SetFiringThreshold, Get/SetTravelSpeed, SetNotGasable,
//   SetMaxActiveNpcCount, GetClosestPlayer, DeactivateForAllPlayers and
//   SetMode(NPC, eMode) on the AI components, and with the NPC's perception
//   (MissionScriptNpcs::add_awareness ...) AddAwareness, AddAwarenessOfPlayers,
//   SetTarget, ResumeAITargeting, ResumeAIMode and LookAround; GetHitPoints,
//   AddWeapon/DrawWeapon on the NPC's combat inventory, and GetCurrentNode
//   (the path follower's hold node);
//   game::Distance(GOBJ, GOBJ) between actors (the player through the world,
//   NPCs through MissionScriptNpcs::position_e4; other objects throw); with the
//   world's pathed objects (MissionScriptWorld::pathed_objects)
//   SetSpeed/SetPos/GetPos(PathedGOBJ) (00211190 / 00211220 / 00211310).
// - deferred: any other void native is recorded (its effect is not applied).
//   Enable/Disable of a box the world does not host is deferred too.
// - every other value-returning native throws: nothing is answered.

// The text globals outside hud.cc that IARCADE.SO's iarcade::Init sets, with
// their static-initializer values (00100410 hands the initializer table
// 0x004d8d10..0x004d903c to 00100b60: 004cdb40 at 0x004d8d74, 004cfcc0 at
// 0x004d8d94). Their readers are the item pickup menu
// (00251040 / 00253530 / 00253570, not hosted) and the objectives overlay
// 00226200 (the objective manager's frame handler 00225cf0 with +ac set,
// not hosted), so play only records them.
struct InterfaceTextGlobals {
    // pickup_menu.cc 004f7e70..7c: 004cfcc0 stores (0.3, 0.55, 0.75, 1.0);
    // 00253670 (SetPickupTextColor) overwrites them and sets byte 004f7e80.
    std::array<float, 4> pickup_colour{std::bit_cast<float>(0x3e99999aU), std::bit_cast<float>(0x3f0ccccdU),
                                       std::bit_cast<float>(0x3f400000U), std::bit_cast<float>(0x3f800000U)};
    std::uint8_t pickup_colour_set = 0;    // byte 004f7e80 (.bss)
    // objective.cc, 004cdb40's stores; 00227a40 (cvt.s.w(x) / 255, the
    // alpha as given) writes them for SetObjectiveColor 002279d0 (004f7580),
    // SetObjectiveCurrColor 00227960 (004f7590), SetObjectiveSuccColor
    // 002278f0 (004f75a0) and SetObjectiveFailColor 00227880 (004f75b0).
    std::array<float, 4> objective_colour{std::bit_cast<float>(0x3e800000U), std::bit_cast<float>(0x3f800000U),
                                          std::bit_cast<float>(0x3e800000U), std::bit_cast<float>(0x3f800000U)};
    std::array<float, 4> objective_curr_colour{std::bit_cast<float>(0x3ef2b021U), std::bit_cast<float>(0x3f208312U),
                                               std::bit_cast<float>(0x3f3374bcU), std::bit_cast<float>(0x3f800000U)};
    std::array<float, 4> objective_succ_colour{std::bit_cast<float>(0x3ea872b0U), std::bit_cast<float>(0x3efae148U),
                                               std::bit_cast<float>(0x3f116873U), std::bit_cast<float>(0x3f800000U)};
    std::array<float, 4> objective_fail_colour{std::bit_cast<float>(0x3f116873U), std::bit_cast<float>(0x3f0353f8U),
                                               std::bit_cast<float>(0x3ea872b0U), std::bit_cast<float>(0x3f800000U)};
    std::uint8_t objectives_font = 1;          // byte 0048ab20 (.data 1), SetObjectivesFont 00229b20
    std::uint8_t objectives_header_font = 0;   // byte 004f7560 (.bss), SetObjectivesHeaderFont 00229af0
};

class MissionScriptHost final : private Host {
public:
    struct Call {
        std::string stage;
        std::string native;
        std::vector<std::string> arguments;
        std::optional<std::string> result;
        bool applied{};
    };
    MissionScriptHost(std::map<std::string, Script> modules, const Population& population, MissionScriptWorld& world,
                      MissionObjectiveInputs objective_inputs = {});
    MissionScriptHost(const MissionScriptHost&) = delete;
    MissionScriptHost& operator=(const MissionScriptHost&) = delete;
    // 003e20a0 (load init/utils, initializers, init::Init, reference check),
    // then init::ClientStartLevel and, in solo, init::StartLevel. Throws when a
    // stage fails.
    void start();
    // start() in its two halves. start_init: 003e20a0's part, run by the level
    // load (001357f0 -> 0013588c). start_level_entries: 003e1a20's
    // init::ClientStartLevel / init::StartLevel (003e1a44 / 003e1a84), run by
    // the state-0x12 done path (00168bac) once the loadout screen's Accept has
    // cleared 004ffc29. Messages and calls need both halves run.
    void start_init();
    void start_level_entries();
    // 003d9d30 on the NPC's AI component holder (+18, allocated {0, 0, -1} by
    // 002300d0 when the actor's +3264 name is set): an empty name binds
    // nothing; otherwise 003e15b0 finds the script type by name (003e0be0,
    // loading the module of that name through 003e1720 when no loaded module
    // defines it) and 003d9da0 gives the holder that type's member count of
    // zeroed words (none when the type is still undefined). The level's NPCs
    // are bound by the constructor (the population load 0013579c precedes
    // 003e20a0); a generator's spawn when it is created.
    void bind_entity_script(Word npc, const std::string& name);
    bool init_run() const { return init_run_; }
    // A HotboxMsg for the box with this population ID (Action = the tracker
    // payload: Enter 0, Occupy 1, Exit 2; Who = the player actor; WhoPlayer =
    // its player component) through 003e1aa0; true when a handler was
    // selected (the native receiver is then skipped, 003c8de0).
    bool deliver_hotbox(Word box, Word action);
    // A GOBJTriggerEventMsg (0021b790, vtable 004daf10) for the object with
    // this population ID: Action = the +0x24 byte (Interact 4 from a type-8
    // box, GOBJTriggerEventMsg_ActionTypes member[3]); Who = the player actor
    // (+0x30, the interacting actor's key); Data = the +0x28 word (the box
    // parameter +0x94 for an interaction, 0 for NotifyDone / NotifyInterrupted).
    // true when a handler was selected. who: an NPC whose actor sent the
    // trigger (its object action's NotifyDone / NotifyInterrupted, 132zf5);
    // Who is then that NPC's actor.
    bool deliver_trigger(Word receiver, Word action, std::optional<Word> who = std::nullopt, Word data = 0);
    // Whether 003e0510 would select a GOBJTriggerEventMsg handler (pass 0 or
    // pass 1) for the object, without invoking one.
    bool trigger_has_handler(Word receiver);
    // Checkpoint 132bp: the CrateInteractMsg (key 004f7ea0, vtable 004dc480)
    // the pickup menu (00251920 / 00251c68) sent an INV: crate with this
    // population ID: Taken (002587e0, +24), Given (00258800, +28), Who
    // (002587d0, +2c: the player actor). true when a handler was selected
    // (003c8de0; TORONTO1: STREETS WeaponCrate OnCrateInteractMsg).
    // Checkpoint 132pw1: `who` is the taker's population ID when it is not the player (the PORT-ONLY CPU partner).
    bool deliver_crate_interact(Word crate, std::int32_t taken, std::int32_t given, std::optional<Word> who = std::nullopt);
    // Checkpoint 132zf5: the InventoryMsg (key 004f7ee0, vtable 004dc460) the
    // kind-5 take (00251920 -> 002523b0) sent a backpack (a population INV:
    // crate or a run-time backpack, register_runtime_backpack): Taken
    // (00258420, +24: the item index), Count (00258400, +28), Who (002583f0,
    // +2c: the player actor). true when a handler was selected (003c8de0;
    // TORONTO2: SEWER OnInventoryMsg(Backpack, InventoryMsg)).
    bool deliver_inventory(Word backpack, std::int32_t item, std::int32_t count);
    // A backpack the play constructs natively (00257640, its ID from
    // 0040c240(&id, 0xf5)): a cBackpack the scripts' Backpack type handlers
    // receive (003c8de0 selects by the receiver's type).
    void register_runtime_backpack(Word id);
    // PORT-ONLY (132fx5): give a received network backpack a collision-free
    // local script object while retaining its full network ID for routing.
    Word register_remote_runtime_backpack(Word network_id);
    // A script AI notification an NPC's AI component sent its actor
    // (ActorAiNotification): AIActivatedMsg (key 004ff268; getter Who
    // (AIGOBJMsg, 0026efe0): +24, the local player's actor),
    // AIWaypointReachedMsg (004ff288) or AIDestinationReachedMsg (004ff290)
    // (getter GetNode (AINodeMsg, 0026efb0 -> 0015c100): +24, the node; a
    // null node reads as the null object), AIDeactivatedMsg (004ff270, vtable
    // 004dcd10; 0016f0e0 leaves +24 unwritten, so no getter is answered).
    // Message types are the registered
    // script types of the vtables' +14 getters (004dcd30, 004dccd0, 004dccb0).
    // Nodes are indices of the population's node list. Through 003e1aa0 with
    // the NPC projected to its AI component; true when a handler was selected.
    bool deliver_ai_notification(Word npc, std::uint32_t key, std::int32_t node);
    // AINewAwarenessMsg (key 004ff298, vtable 004dcc90; the registered script
    // type of its +14 getter, global 004ff1d0 = type 124, interpreter): Who
    // (AIGOBJMsg, 0026efe0) answers +24, the perceived object: an NPC record's
    // actor, or the local player's actor for nullopt. Through 003e1aa0 with the
    // NPC projected to its AI component; true when a handler was selected.
    // Cause (0026eee0, checkpoint 132sx8) answers the byte +28, the
    // perception's NPC_CauseForAwareness.
    bool deliver_new_awareness(Word npc, std::optional<Word> who, std::uint8_t cause);
    // AIWeaponFiredMsg (key 004ff2a0, vtable 004dcbf0, ctor 0026f110; the fire
    // notice 0014f460 to the AI component, transport 0; script type of its +14
    // getter 0026eed0, global 004ff1e8 = registered type 125, boot snapshot):
    // Who (AIGOBJMsg, 0026efe0) answers +24, the shot's target: null when
    // has_target is false, else an NPC record's actor or the local player's
    // actor for nullopt. IsThrownWeapon (0026ee50: lbu +30; 132sx10, CHECHNYA)
    // answers `thrown`: the ctor writes +30 = (001437a0(firer +3524) & 0xff)
    // == 1 with a firer, 0 without. The other getters (+28 WeaponId, +2c) are
    // not read by a ported handler and are absent (a read throws).
    bool deliver_ai_weapon_fired(Word npc, bool has_target, std::optional<Word> target, bool thrown = false);
    // 132fx2: AITimeElapsedMsg (004ff280, vtable 004dcc10: no fields past the header) to the NPC's AI component.
    bool deliver_ai_time_elapsed(Word npc);
    // AIDeadMsg (key 004ff278, vtable 004dccf0; the AI death notice 0014f680):
    // Attacker (0026f060 -> 003cc800, the identity of +24, the killer: null
    // without one, an NPC record's actor, or the local player's actor for
    // nullopt) and DamageType (0026f050, byte +28). Through 003e1aa0 with the
    // NPC projected to its AI component; true when a handler was selected.
    bool deliver_ai_dead(Word npc, bool has_attacker, std::optional<Word> attacker, std::uint8_t damage_type);
    // The same AIDeadMsg whose killer (+24) is a source the script registry
    // cannot name (an actor's burning zone, see deliver_damage_from_unnamed):
    // the Attacker getter is left unanswered, DamageType as deliver_ai_dead.
    bool deliver_ai_dead_by_unnamed(Word npc, std::uint8_t damage_type);
    // AIUnconsciousMsg (key 004ff2c0, vtable 004dcc30; type-7 damage
    // 00187cd8): Who (0026ecf0 -> 003cc800, the identity of +24, the damage
    // source: null without one, an NPC record's actor, or the local player's
    // actor for nullopt). Through 003e1aa0 with the NPC projected to its AI
    // component; true when a handler was selected.
    bool deliver_ai_unconscious(Word npc, bool has_who, std::optional<Word> who);
    // AIPickupableAwarenessMsg (key 004ff2a8, vtable 004dcbd0; the
    // perception's dead-object notice 0016c9d0 to the AI component, transport
    // 0; script type of its +14 getter 0026ee40, global 004ff210 = registered
    // type 126, boot snapshot): Who (AIGOBJMsg, 0026efe0) answers +24, the
    // dead pickupable object: an NPC record's actor, or the local player's
    // actor for nullopt. Through 003e1aa0 with the NPC projected to its AI
    // component; true when a handler was selected.
    bool deliver_ai_pickupable_awareness(Word npc, std::optional<Word> who);
    // AIBodyMovedMsg (key 004ff2b0, vtable 004dcc70; the body carry's pickup
    // 001abbe8 and unlink 001a02c0 queue it to the body with event byte 2):
    // Who (472, 0026edb0 -> 003cc800) answers +24, the carrier (an NPC record's
    // actor, or the local player's actor for nullopt); PickedUp (473, 0026ed90)
    // the byte +28. Through 003e1aa0 with the NPC projected to its AI
    // component; true when a handler was selected.
    bool deliver_ai_body_moved(Word npc, std::optional<Word> who, bool picked_up);
    // AIDisguiseTakenMsg (key 004ff2b8 "Script AI Disguise Taken Message",
    // vtable 004dcc50; the pickup menu's kind-4 take 00252298 queues it to the
    // backpack's owner, transport 0, checkpoint 132sx10): Who (AIGOBJMsg,
    // 0026efe0) answers +24, the taker (an NPC record's actor, or the local
    // player's actor for nullopt). Through 003e1aa0 with the NPC projected to
    // its AI component; true when a handler was selected.
    bool deliver_ai_disguise_taken(Word npc, std::optional<Word> who);
    // AIModeChangeMsg (cAIModeChangeMsg 004ea3e8, vtable 004d92e0): the AI
    // component's notice to itself after AI event 1 applied the requested mode
    // (0016e160), transport 0. GetAiMode (0014a6e0: lbu +24) answers the new
    // mode. Through 003e1aa0 with the NPC projected to its AI component; true
    // when a handler was selected (checkpoint 132sx9).
    bool deliver_ai_mode_change(Word npc, std::uint8_t mode);
    // A PathedNotice (key 004f5450, vtable 004dae90: +24 action Stopped 0 /
    // Moving 1 / PosWaypoint 2, +28 the waypoint) a pathed object sent itself
    // (PathWorld's filter): through 003e1aa0 with the object as its own
    // receiver (type PathedGOBJ); true when a handler was selected. Action
    // (00210b30, +24) and Data (00210b10, +28) are answered. When none is
    // selected the native receiver 00219070 passes it to 003cea10, whose
    // 003caca0 walks the object's empty child list +0x20 (only G has children:
    // AddChild 003cac40's six callers are 00137ab0 x5 and 001350e0), so it is
    // dropped.
    bool deliver_pathed_notice(Word object, const ObjectMessage& notice);
    // A PathedNotice (vtable 004dae90) or an ElevatorNotice (003c9e30 key
    // 004f5448, vtable 004daed0) a lift (ElevatorGOBJ) sent itself with transport 0,
    // offered at the next pump to the lift's script filter (type
    // ElevatorGOBJ); true when a handler took it. Getters: Action (PathedNotice
    // 00210b30 / ElevatorNotice 00210ba0, lbu +24) and PathedNotice Data
    // (00210b10, lw +28). Untaken, the receiver 00214640 forwards it to
    // 00219070, which drops it (see deliver_pathed_notice).
    bool deliver_lift_notice(Word object, bool elevator_notice, std::int32_t action, std::int32_t data);
    // MSGGAME_NI_EVENT_NOTICE (key 004f7c30) offered to an NIE: object's
    // script filter (003c8de0) as NINotice; true when a handler took it.
    // Checkpoint 132uk3: its getter Action(NINotice) 00237500 (registered at
    // 004cf848 for the type cNINotice 004f7d20; `lw v0,(a0)`, `lbu v0,+24`)
    // answers the byte +24: 0 at a sequence's end, an event type 3's value
    // (NINotice_ActionTypes 004f7d38: member[0] 0, member[1] 1).
    bool deliver_ni_notice(Word object, std::int32_t value_24);
    // Checkpoint 132sx11: the kind-6 beam's BeamMsg (key 00506210, vtable
    // 004dcf30; +24 the player's actor, +20 = 1) offered to the NIE: object's
    // script filter (003c8de0); Who answers the player. True when handled.
    bool deliver_beam(Word object);
    // Checkpoint 132ms2: the kind-5 camera's HumanSeenMsg (key 004ee930, vtable 004da310; +24 the seen
    // actor, +20 = 1) offered to the NIE: object's script filter (003c8de0); no getters. True when handled.
    bool deliver_human_seen(Word object);
    // SpawnedNPCMsg (003c9e30 key 004a4a80, vtable 004db640; +24 the actor)
    // to its generator, offered to the script filter first (the generator's
    // native receiver 00221610 takes it with no effect). GetNPC (0021d060,
    // spawned_npc_msg.hpp) answers the AI component of `npc`, or null.
    bool deliver_spawned_npc(Word generator, std::optional<Word> npc);
    // Checkpoint 132fx1: DespawnedNPCMsg (key 004a4a88, vtable 004db620; +24 the actor) from 00220bb0, as above.
    bool deliver_despawned_npc(Word generator, Word npc);
    // A spawned NPC's script identity (PopulationObjects::add_spawned_npc).
    // `plus_c` is the actor's +c after 001b801c..001b8078 (the ID with its
    // high byte), the key 0021e8a4 -> 0040b790 registers it under in
    // 00571c10 and the word the GOBJ / NPC callout natives pass as the key.
    void register_spawned_npc(Word npc, std::optional<Word> plus_c = std::nullopt);
    // A generator's despawn (00220244): 003d9f40 drops the spawned NPC's
    // actor and then its AI component (+58) from every script Group
    // (ScriptContext::drop_from_groups). The registry entry stays until the
    // flush of its 0040b720 removal (flush_spawned_npc: ID lookups fail from
    // then on) and the delete 0040bf50 -> 001b57c0 (delete_spawned_npc).
    void drop_spawned_npc_from_groups(Word npc);
    void flush_spawned_npc(Word npc);
    void delete_spawned_npc(Word npc);
    // A hotbox link message (EnableMsg, key 0055d4b8, vtable 004e0880 /
    // DisableMsg, key 0055d498, vtable 004e0860; the registered script types
    // of their +14 getters, interpreter) a type-1/6/7 box sent an NPC: through
    // 003e1aa0 with the NPC projected to its AI component; no getter is
    // answered. true when a handler was selected (TORONTO1 has none).
    bool deliver_link_message(Word npc, bool enable);
    // A type-3 box's GOBJTriggerEventMsg (00172598: 0021b790, vtable 004daf10)
    // to the NPC it links: through 003e1aa0 with the NPC projected to its AI
    // component; Action = the +24 byte, Data = the +28 word, Who = the +30
    // key (the box object). True when a handler was selected.
    bool deliver_box_trigger(Word npc, Word action, Word data, Word box);
    // The same EnableMsg / DisableMsg sent to a generator, offered to its script
    // filter first (003c8de0); unhandled, the native receiver 00221610 takes it
    // (generator_receive_link_message). No getter is answered.
    bool deliver_generator_link_message(Word generator, bool enable);
    // Checkpoint 132fx4: a type-1/6/7 box's EnableMsg / DisableMsg to a world object (an InteractGOBJ), offered
    // to the object's script filter; false when no handler takes it (the host then runs 003cea10).
    bool deliver_object_link_message(Word object, bool enable);
    // The revive's NotifyCheckpointMsg (key 004f7938, vtable 004db8b0, no
    // getters) through 003e1aa0 with the actor projected to its controller
    // (00175fd0 for the player component): the player without an argument,
    // otherwise a player or AI record. true when a handler was selected
    // (TORONTO1: MIDDLE OnNotifyCheckpointMsg(Player), pass 1).
    bool deliver_notify_checkpoint(std::optional<Word> actor = std::nullopt);
    // 00244fa0's TimerExpiredMsg (key 004f7a08) to the player actor *(*004ffc0c + 30) on the expiry of the HUD
    // timer +6c4 (kind 1, value 0) or +6ac (2, 1), offered to the player record's handlers (assumed to be the
    // actor's script filter, as NotifyCheckpointMsg's). No TORONTO1 or COMMON script has one, and no native
    // receiver tests the key, so it is dropped there; TORONTO2's SEWER OnTimerExpiredMsg(GameGobjController,
    // TimerExpiredMsg) reads IsPersonal (0022f1b0: the message's word +24 != 0, i.e. value). true when a
    // handler was selected.
    bool deliver_timer_expired(std::int32_t value);
    // cOutOfAmmoMsg (key 004ea150, vtable 004d91e0; +24 the item) the
    // discharge 0013ff40 or the throw release 00141cd0 queued to the player
    // controller (transport 0), offered at its pump to the controller's script
    // filter (00175fd0 -> 003e1aa0) as OutOfAmmoMsg; no script native reads
    // the item. true when a handler was selected (ITALY's Player handler).
    bool deliver_out_of_ammo();
    // cWeaponVisChangeMsg (00583968, vtable 004e1140) the inventory update
    // 00144080 queued to the player controller (transport 0) when 00142e80's
    // value changed, offered at its pump to the controller's script filter as
    // WeaponVisChangeMsg; its getter Visibility (0043fb40) reads word +24, the
    // value. true when a handler was selected (MINSK / YEMEN1 / YEMEN2).
    bool deliver_weapon_vis_change(std::int32_t value);
    // The DamageMsg the base damage 003ccb00 queued to a damaged actor
    // (003ccc90; key 005716f0, vtable 004e08a0) through 003e1aa0 with the
    // actor projected to its controller: the player without an argument,
    // otherwise a player or AI record (props throw: their +100 is not
    // hosted). Getters: Damage (00408090, +24: the cvt.w.s amount), Attacker
    // (00408080 -> 003cc800, +28: an NPC record's actor, or the local
    // player's actor for nullopt; other sources throw), Where (00408070, lbu
    // +2d, the category) and Type (00408060, lbu +2c, the damage type).
    // true when a handler was selected.
    bool deliver_damage(std::optional<Word> actor, std::optional<Word> attacker, std::int32_t damage, std::uint8_t type,
                        std::uint8_t where);
    // The same DamageMsg from a source the script registry cannot name: an
    // actor's burning zone (002624a0's FireObj, +0c 0, never registered by
    // ID), whose 00261df0 damage has the zone as +28. Attacker (00408080) is
    // that object itself, which no script handle stands for, so the getter is
    // left unanswered: a handler that reads it throws ("Message getter is not
    // answered"). Damage, Where and Type as deliver_damage.
    bool deliver_damage_from_unnamed(std::optional<Word> actor, std::int32_t damage, std::uint8_t type, std::uint8_t where);
    // The DamageMsg 003ccb00 queues to a population object (GOB:), an event
    // object (NIE:) or an event light (NIL:), offered to the object's script
    // filter (+38, 003c8de0) as DamageMsg with the same getters; `attacker`
    // is an actor, a fire zone or another of these objects (nullopt: the
    // player). true when a handler was selected; otherwise the native
    // receivers (00219070 / 0023b7d0 / NIL) pass it to 003cea10, whose
    // 003caca0 walks the empty +0x20 child list (only G's is filled by
    // AddChild 003cac40): dropped.
    bool deliver_object_damage(Word object, std::optional<Word> attacker, std::int32_t damage, std::uint8_t type,
                               std::uint8_t where);
    // The script event pump 003e9e30 with the frame's script delta (ms).
    std::size_t pump(std::int32_t milliseconds);
    // Port testing convenience (play's --debug-script-call): calls a script
    // function by name outside any original caller, like start()'s entries.
    void invoke_function(const std::string& name);
    bool started() const { return started_; }
    const std::vector<Call>& calls() const { return calls_; }
    std::size_t deferred() const;
    const ScriptContext& context() const { return context_; }
    const PopulationObjects& objects() const { return objects_; }
    const ObjectiveNatives& objectives() const { return objectives_; }
    ObjectiveNatives& objectives() { return objectives_; }
    const ItemTable& items() const { return items_; }
    const ItemInventory& player_items() const { return player_items_; }
    // Checkpoint 132zf5: an actor's +0x3528 list by population ID: the
    // player's (PlayerAddItem) or an NPC's (AddItem 00148e50 -> component
    // +218 = 0014a730 -> 00470a70), the empty list 001b6a60 builds until an
    // add. Another record throws.
    ItemInventory& item_list(Word id);
    // Checkpoint 132sx9: ResetNpc's destructor 001b57c0 frees a level NPC's
    // +3528 list and the rebuild 001b8130 allocates an empty one (001b6a60).
    void reset_item_list(Word id);
    // 004703b0's row +28 (the item model handle the play loads).
    void set_item_resource_28(std::int32_t index, std::int32_t handle) { items_.set_resource_28(index, handle); }
    struct ItemChange {
        std::string key;
        std::int32_t index{};       // -1: not registered (no change)
        std::int32_t requested{};
        std::int32_t changed{};     // added n, or the amount removed
        std::int32_t held{};        // the pair's count afterwards
        bool removal{};
        Word actor{};               // the list's population ID (0: the player's)
    };
    const std::vector<ItemChange>& item_changes() const { return item_changes_; }
    // The hosted game::LoadAnimation calls, in order.
    const std::vector<LevelAnimationLoad>& level_animations() const { return level_animations_; }
    const TeamRelations& teams() const { return teams_; }
    // +40 (float health) of population objects and event objects as
    // SetHitpoints (003cc5c0) or the base damage 003ccb00 (the blast world's
    // host objects) wrote it; nullopt when not written (the base constructor
    // 003cf0e0 stores 0 and no GOB/NIE/NIL constructor writes it; GetHitpoints
    // before a write still throws).
    std::optional<float> object_health_40(Word object) const {
        const auto found = object_health_40_.find(object);
        if (found == object_health_40_.end()) return std::nullopt;
        return found->second;
    }
    void set_object_health_40(Word object, float health) { object_health_40_[object] = health; }
    // The cVisionFxMan (*004ffc30) floats game::setFxLightDist (0025e460)
    // wrote: +74 + mode * 0x40 for mode < 5 (unsigned); nullopt when never
    // written. The manager itself (its reader of +74) is not hosted.
    std::optional<float> vision_fx_light_distance_74(std::uint32_t mode) const {
        return mode < vision_fx_light_dist_74_.size() ? vision_fx_light_dist_74_[mode] : std::nullopt;
    }
    // +2c of a population object as Enable / Disable(GameGOBJ) (002114a0 /
    // 00211470 -> 0021aa20 / 0021a940) wrote it; 1 (003cf0e0's value) when
    // never written.
    bool object_enabled_2c(Word object) const {
        const auto found = enabled_2c_.find(object);
        return found == enabled_2c_.end() || found->second;
    }
    // Checkpoint 132fx4: +2c as the default receiver 003cea10 writes it for an EnableMsg / DisableMsg (the
    // host's box link messages); the same byte object_enabled_2c reads.
    void set_object_enabled_2c(Word object, bool enable) { enabled_2c_[object] = enable; }
    // The script context's callback list (0055a2e0), which the event pump
    // advances; explosion events (003ea1d0) are queued on it.
    ScriptScheduler& events() { return context_.schedules(); }
    // The HUD script state (hud_script_state.hpp): game::SetCallout /
    // SetCalloutLabel / ClearCallout (GOBJ and NPC forms) and
    // game::SetLocation; nothing is drawn.
    const HudScriptState& hud() const { return hud_; }
    // The in-play HUD object *004ffc2c (hud_object.hpp) the AddPDAQuip native
    // 001c63b0 acts on; without one (or without the localization) the call
    // is deferred. SetDisplayLabel also posts its resolved label to the
    // ticker notice 00242b00 (0x00227c18, solo) when one is set. Not owned.
    void set_hud_object(HudObject* hud) { hud_object_ = hud; }
    // The HUD's game screen (*004ffc2c +6a4, pad_interact_prompt.hpp) the
    // DisplayInteract / ClearInteract natives 0045e640 / 0045e5f0 act on;
    // without one the calls are deferred. Not owned.
    void set_game_screen_interact(GameScreenInteract* screen) { game_screen_interact_ = screen; }
    // 00136990, which the level load 001350e0 runs at 00135120 (before the HUD
    // object 001307f0 at 001357f0 and before the mission's SCRIPTS.HOG): the
    // interface script IARCADE.SO of COMMON\IARCADEN.HOG is loaded into the
    // same interpreter (003e1720 "iarcade"), the reference check 003e1cf0
    // runs (its result unused), then the initializers 003e1f30 and 003e1b60
    // "iarcade::init" (00128e80 compares case-insensitively: iarcade::Init).
    // game::SetInterfaceTextColor / SetNVInterfaceTextColor (002466f0 ->
    // 00246740 / 00246640 -> 00246690) store into `globals`' colour tables;
    // the registrar 004cf890 (at 0x004d8d8c in the initializer table) binds
    // them with 003d8740 at 0x004cf91c / 0x004cf95c. SetInfoBarFont 002467a0
    // -> 002467c0 stores the byte 004f7d40 (globals.font_index), and
    // SetInterfaceFont 001653c0 -> 001653e0 stores the font manager's
    // (001698c0) +280 + 4 * (index & 0xff) word into *0055a278
    // (globals.text_word): `font_table` answers that word (it throws for a
    // slot without a font). The pickup and objective colours and fonts go to
    // interface_text(). SetLockColor / SetNVLockColor (00260fb0 / 00260ed0,
    // 121l) store into `globals`' lock ramps. Its calls are kept apart from the mission's
    // (interface_calls()). Before start() only.
    void run_interface_script(Script iarcade, HudGlobals& globals,
                              const std::function<std::uint32_t(std::uint8_t)>& font_table);
    const std::vector<Call>& interface_calls() const { return interface_calls_; }
    const InterfaceTextGlobals& interface_text() const { return interface_text_; }
    // gameSound's manager (game_sound.hpp) the PlayQuip / PlayMusic /
    // StopMusic natives post to; without one the calls are deferred. Not owned.
    void set_game_sound(GameSoundManager* sound) { game_sound_ = sound; objectives_.set_game_sound(sound); }
    // StartGlobalTimer 0012f860 -> 0012f890(seconds, 0) -> 00245560(*004ffc2c, 0, seconds): `start` runs the
    // HUD timer +6c4 (hud_global_timer_start; solo), then the native posts the E_TIMER sound 004f0f90 to
    // gameSound when one is set. Without `start` the call is deferred. The (float, string) form 0012f820 ->
    // 0012f890(seconds, key) passes the key: 00245560 labels the timer with 003fdb50(key) (GetString; a null
    // result is 00244ee0's "").
    void set_global_timer(std::function<void(float seconds, const std::string* label_key)> start) {
        global_timer_ = std::move(start);
    }
    // game::SetPersonalTimer 002677d0 -> 00267860(gobj, seconds, 0) for the local player's GOBJ (its controller
    // +58 answers virtual +54 and its byte +14 is set): 00244ee0(*004ffc2c +6ac, seconds + hud +6dc, null).
    // `start` runs that HUD timer; without it the call is deferred (Checkpoint 132zf4).
    void set_personal_timer(std::function<void(float seconds)> start) { personal_timer_ = std::move(start); }
    // game::PlayXA(name) 0045ed80 -> 0045edb0(name, 0) and PlayXA(name, string) 0045ed40 -> 0045edb0(name,
    // argument 1's word) (registered by 004d7e20 at 0x004d7e68 / 0x004d7ea0): with *004ffc2c and its game
    // screen +6a4, 0045ce10(gs, name, a2, 1): the subtitle and its voice 0045ceec (checkpoint 132hud4); 0045ce10
    // never reads a2. `play` is that call (`second` is the two-argument form's string); without it (no HUD game
    // screen) the call is deferred. No level's scripts call it (the whole-game census finds no call site; every
    // VOICE.SO imports both forms, {primitive:7} and {primitive:7, primitive:3}).
    void set_play_xa(std::function<void(const std::string& name, const std::optional<std::string>& second)> play) {
        play_xa_ = std::move(play);
    }
    // The Player choose-menu natives (registered by 004c6280.. through 003d84f0 on the Player type *004ee788):
    // SetMenuReceiver(Player, GOBJ) 00175d60 -> 0045beb0(gs, 003cb1c0(GOBJ)), SetMenuTitle(Player, string)
    // 00175da0 -> 0045bf10, AddMenuChoice(Player, string) 00175de0 -> 0045bf40 and LaunchChooseMenu(Player)
    // 00175e20 -> 0045c030(gs, the component's +30 when its +4c is the actor class *0049d010, else 0). Each loads
    // *004ffc2c without a NULL test and does nothing when its game screen +6a4 is NULL. `call` is that path
    // (checkpoint 132hud4); without it the call is deferred. No level's scripts call them (whole-game census).
    struct ChooseMenuCall {
        enum class Kind { receiver, title, add, launch };
        Kind kind{};
        Word object{};      // SetMenuReceiver: the GOBJ
        std::string text;   // SetMenuTitle / AddMenuChoice
    };
    void set_choose_menu(std::function<void(const ChooseMenuCall&)> call) { choose_menu_ = std::move(call); }
    // game::IsObjectiveComplete(GOBJ, string) 0032f5c0 (Checkpoint 132sx11; MNTNEGR1 init::StartLevel) ->
    // 0032f600: a human actor (+4c == class word 0049d010) gives 00185c70's agent record (solo: the controller
    // +58 virtual +54 answers slot 0's, 001314f0(004ffb50, 0)) to 00333b10(agent, key) (003fda40 key, 00330aa0
    // row, the bit of the words +84); any other GOBJ answers 0. `done` answers 00333b10 for the local player's
    // agent; without it the call throws (a non-void native).
    void set_agent_objective_complete(std::function<bool(const std::string& key)> done) {
        agent_objective_complete_ = std::move(done);
    }
    // 0040c240(&id, category) for the objects play creates natively (the
    // backpack 0xf5 and its hotbox 0xf4): the same ID space as the scripts'.
    Word allocate_object_id(Word category) { return create_world_object(category).id; }
    // Whether an ID is a population / event record the scripts receive messages
    // for (a native object's hotbox is not: no script handler takes its events).
    bool has_receiver(Word id) const { return objects_.sections.count(id) != 0; }
    // An InteractGOBJ record's last two references (constructor 00216ae0):
    // +a8 the node (position +80, yaw +8c) and +ac the world object (0 for a
    // reference 0040c480 gives null). nullopt: not an OBJ kind-5 record. A
    // malformed record (fewer than two references, an unresolved reference,
    // +a8 not a node) throws.
    struct InteractLinks {
        MissionScriptWorld::ObjectActionNode node;
        Word world_object{};
    };
    std::optional<InteractLinks> interact_links(Word object) const;
    // Port-only (checkpoint 132ob8, omega_play --coop-partner / Options "CPU
    // Partner"): the CPU partner stands in for the second player of the
    // multiplayer objective paths only. game::IsMultiplayer (0012f810, the
    // byte 005721c8 read at 0012f818) answers 1 at the allow-listed call sites
    // (partner_objective_multiplayer_site: TORONTO1 OBJECTIVES CreateObjectives
    // word 318, AddObjective_Laptop word 1296, STREETS AddLaptop word 1300;
    // BELARUS1 OBJECTIVES CreateObjectives word 867, MILL CheckAddWorker word
    // 3212, WorkerFinished words 4614 / 4671 / 4726, since 132zf13) and
    // 0 (solo) everywhere else; no native reader of 005721c8 changes. Before
    // start().
    void set_partner_objectives(bool on) { partner_objectives_ = on; }
    bool partner_objectives() const { return partner_objectives_; }
    // Port-only aid (checkpoint 132ms3, omega_play --freed-ai-resend-skip; off
    // by default): ResendNewAwarenessMessage on a deleted NPC's kept AI pointer
    // (0014a340 / 0014a390 -> root vtable 004dfde0 +f4 = 003cde00) loads the
    // freed word +50, whose byte +50 is the mode request's pending byte
    // (0016e170 / 0016f5b0) and whose bytes +51..+53 no constructor writes
    // (heap history). The aid takes the word as 0 (003cde0c's branch: no call)
    // instead of throwing; every skip is counted.
    void set_freed_ai_resend_skip(bool on) { freed_ai_resend_skip_ = on; }
    std::size_t freed_ai_resends_skipped() const { return freed_ai_resends_skipped_; }
    // Port-only (--coop-partner, checkpoint 132ms5c; ActorWorldScriptNpcs::set_port_overlay_first_node): node
    // indices from `first` on are co-op overlay nodes (crumbs, walk bridges) appended past the population's node
    // list. GetCurrentNode / GetNextNode / GetDestinationNode (0014af90 / 0014af60 / 0014af30) on one answer
    // null, as for a negative index, since no script object exists for it (MILL's generic
    // OnAIWaypointReachedMsg -> mill::NpcDoorCheck asks the partner's next node at a level node whose route
    // goes on into a walk bridge). Every such answer is counted. -1 (default): no overlay.
    void set_port_overlay_first_node(std::int32_t first) { port_overlay_first_ = first; }
    std::size_t port_overlay_node_answers() const { return port_overlay_node_answers_; }
    struct MultiplayerAnswer {
        std::string module;
        std::string function;
        std::size_t word{};
        bool answer{};
    };
    // Every game::IsMultiplayer answer given while partner_objectives is on.
    const std::vector<MultiplayerAnswer>& multiplayer_answers() const { return multiplayer_answers_; }
    // Checkpoint 132ms4: SetMaxActiveNpcCount(Player, int) 00175980 writes the player component's +1b8
    // (constructor 00177b10: -1; a remote actor gets the message 004f53f8, whose handler in 00177380
    // (001777d0) stores its +24 there). No reader of the player's +1b8 is found in the image; the value is kept here.
    std::int32_t player_max_active_npc_count() const { return player_max_active_npc_count_; }
    // Port-only (checkpoint 132md16): game::ToHit (0048ac80), for a mod's [tohit] data overrides before the scripts run.
    EngineStruct& to_hit() { return to_hit_; }
private:
    std::optional<Value> call(const Script& script, const Function& function,
                              const std::vector<Value>& arguments) override;
    Word enum_value(const std::string& name, Word member) override;
    Value object_from_id(Word raw_id) override;
    Word object_to_id(const Value& object) override;
    Word world_object_id(const RuntimeObjectHandle& object) override;
    RuntimeObjectHandle create_world_object(Word category) override;
    // Group / NodeList Randomize (003d9a00) draw the world's game RNG (00127dd0).
    std::uint32_t game_random() override { return world_.game_random(); }
    std::optional<Address> entity_member(const Value& object, Word slot) override;
    std::optional<std::string> entity_type(const Value& object) override;
    std::optional<Value> hosted_call(const Script& script, const Function& function,
                                     const std::vector<Value>& arguments, bool& applied);
    std::map<std::string, Script> modules_;
    PopulationObjects objects_;
    std::map<Word, Word> remote_backpacks_;   // full network ID -> local script object ID
    std::map<Word, PopulationNode> nodes_;
    std::vector<Word> node_ids_;                                                       // node list index -> ID
    std::map<Word, std::vector<std::array<std::uint32_t, 2>>> interact_references_;   // OBJ kind 5
    std::map<Word, bool> enabled_2c_;   // population ID -> +2c written by Enable / Disable(GameGOBJ)
    MissionScriptWorld& world_;
    ObjectiveNatives objectives_;
    std::map<Word, RuntimeObjectHandle> map_objects_;   // objective -> its +34 map object (00228190)
    ItemTable items_;
    ItemInventory player_items_;
    std::map<Word, ItemInventory> npc_items_;
    std::vector<ItemChange> item_changes_;
    std::vector<LevelAnimationLoad> level_animations_;
    TeamRelations teams_;
    std::map<Word, float> object_health_40_;   // population ID -> GOBJ +40
    std::array<std::optional<float>, 5> vision_fx_light_dist_74_{};   // *004ffc30 +74 + mode * 0x40
    std::map<Word, Word> spawned_plus_c_;      // spawned NPC ID -> its actor's +c
    // The AI components' script holders (+18, 002300d0): {storage +0, count
    // +4, type id +8} per NPC ID, bound by 003d9d30. Address{id, slot, this}.
    class EntityStorage final : public AddressSpace {
    public:
        struct Holder {
            std::string type;
            std::vector<Value> words;
        };
        std::map<Word, Holder> holders;
        Value& dereference(const Address& address) override;
    };
    EntityStorage entities_;
    // Checkpoint 132el7: the GOB records bound by the GOB loader (0021afa4..0021affc:
    // the GameGOBJ base's own holder at +18, then 003d9d30), whose world object
    // answers 003de680 from that holder.
    std::set<Word> gob_entities_;
    HudScriptState hud_;
    HudObject* hud_object_{};
    GameScreenInteract* game_screen_interact_{};
    GameSoundManager* game_sound_{};
    std::function<void(float, const std::string*)> global_timer_;
    std::function<void(float)> personal_timer_;
    std::function<void(const std::string&, const std::optional<std::string>&)> play_xa_;
    std::function<void(const ChooseMenuCall&)> choose_menu_;
    std::function<bool(const std::string&)> agent_objective_complete_;
    EngineStruct to_hit_;
    ScriptContext context_;
    std::string stage_;
    std::vector<Call> calls_;
    std::vector<Call> interface_calls_;
    HudGlobals* interface_globals_{};   // set while run_interface_script runs
    const std::function<std::uint32_t(std::uint8_t)>* interface_fonts_{};   // likewise
    InterfaceTextGlobals interface_text_;
    bool started_{};
    bool init_run_{};
    bool partner_objectives_{};
    bool freed_ai_resend_skip_{};
    std::size_t freed_ai_resends_skipped_{};
    std::int32_t port_overlay_first_{-1};
    std::size_t port_overlay_node_answers_{};
    std::vector<MultiplayerAnswer> multiplayer_answers_;
    std::int32_t player_max_active_npc_count_{-1};
};

// Port-only (132ob8): the game::IsMultiplayer call sites the CPU partner
// answers as a second player for (the multiplayer objective's creation and
// completion paths). Module name, calling function, call word.
bool partner_objective_multiplayer_site(const std::string& module, const std::string& function, std::size_t word);

// The script clock (00167c40): the frame's absolute milliseconds (the T0
// count / 5.76 accumulated, floor / 100) minus the previous frame's, clamped
// at 0; the first sample gives 0. The pump's script delta is 0 while paused.
class ScriptFrameClock {
public:
    std::int32_t delta(double elapsed_seconds);
private:
    std::optional<std::int64_t> last_;
};

}
