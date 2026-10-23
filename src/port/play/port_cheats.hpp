#pragma once
// Port-only (not in the original game): the extras the port adds on top of
// the original's behaviour, all off by default.
//
// Front-end unlocks (checkpoint 132cc4c; research/AGENT_PERKS.md
// "Port cheats"). They set the agent record's unlocks through the original
// unlock routines the AWARDS script calls (call_awards_agent_native: the
// AgentData member natives 0032f130..0032fc60 and their workers), so every
// field they touch is one the original's own progression writes:
//
//   UnlockLevel 003330c0     each level registry row (00532af0): its bit of +b08..
//   UnlockWeapon 00333050    each unlock row (00532b10, the WEAPONS.WDB records): its bit of +b08..
//                            (rows registered with key -1, names absent from the common pool, are
//                            unreachable by name; their bit gets the same store directly)
//   UnlockSpecial            the special bonuses 0..5 (perks: NeckSnap .. IncreasedAmmo): bits 0..5 of +b08
//   UnlockPart 00332610      each PARTS.PRN row (00532b30): the owned-parts vector (+b58, count +b5c),
//                            which the appearance screens' locked-part list 00332500 reads
//   Unlock<L>Requirement     every requirement of the award registries (medal, commendation, special
//                            rating, bonus level, omega; 00332f60 family): 0 -> 1 in the agent's
//                            award elements (+aac..), so their items are no longer "Classified"
//   004ffbb5 = 0             the level restriction, as --unlock-all-levels (0013d270)
//
// The loadout screen's lists (002bf090) offer the weapons 00332ac0 returns:
// the unlock rows whose +b08 bit is set. port_unlock_all_weapons sets exactly
// those bits, so every weapon becomes selectable; nothing is added to the
// inventory.
//
// Not touched: UnlockZeus (the ZEUS files take script-chosen triples; their
// panel 00296314 is not ported), award flags and ranks (they are earned
// scores, rebuilt by the refill), the inventory.
//
// The refill 00335e50 zeroes +b08..+b2b and the parts vector and re-runs
// awards::SetAwardsAndBonuses, and the save does not store the unlock bits,
// so the front end re-applies the cheats after every refill while they are on
// (FrontendSession::refill_host). Nothing is written to the card here; the
// card changes only when an original save path runs.
//
// Perception marks. The CPU co-op partner's partner_stealth (checkpoint
// 132ob2, coop_partner.hpp) marks the partner, and the pause-menu cheat
// "NPCs Ignore You" (checkpoint 132f5) marks the player. Nothing is marked by
// default, so the enemy perception is the original's. The combat session's
// perception host (combat_play.cpp, link.sense) applies the mark to the NPC
// candidate round robin 0016e1a0 (npc_perception_think): a marked actor is
// left out of every unmarked observer's think list 004ea190, and a marked AI
// observer gets an empty think list (the ignore is mutual). The NPC entry
// points also test it through NpcPerceptionHost::port_ignored and
// NpcCombatLink::port_ignored (checkpoint 132f5): the local player's own
// candidate (*004ffc0c), the visibility test, the record add / update, the
// hearing reaction 001708b0 and the hit notice +78, so a marked player is
// never seen, heard or noticed and a held target is dropped.
//
// Checkpoint 132ob6: a marked AI observer that is also *engaged* keeps its own
// think list (it sees, scores and targets NPCs through the original
// perception), while unmarked observers still leave it out. The co-op
// partner is engaged while the player is in combat (coop_partner.hpp,
// CoopCombatConfig), so it fights without being seen; the play host clears
// the mark when the partner fires.
//
// Cheats (checkpoint 132f5). The pause menu's Cheats screen (PortGuiCheats,
// gui_runtime.cpp) toggles them through the play host; each is On / Off,
// except End Level, a one-shot action row (no value shown; Cross runs it).
// The effects are applied at the original's entry points by the port code
// that reads port_cheat(): the actor damage 00187360 (solo_actor_simulation),
// the ammo uses (combat_world, combat_play reload, combat_throw_play), the
// perception (above), the level end 0012fa60 and the agent record's unlocks
// (FrontendSession's port agent patch).
#include "agent_load.hpp"
#include "agent_refill.hpp"
#include "frontend_formats.hpp"
#include "parts_table.hpp"
#include "weapon_inventory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>

namespace omega {

struct ProfileAwards;

// The front end's unlocks (132cc4c; the AWARDS natives, ProfileAwards) next to the pause menu's
// direct-bit versions below (132f5, AwardTables). Every unlock above on the agent; `level_restrict`
// is the byte 004ffbb5.
void port_unlock_all(const ProfileAwards& awards, std::span<std::uint8_t, agent_record_size> agent,
    AgentLoadState& state, std::uint8_t& level_restrict);

// Only UnlockWeapon for every unlock row: each weapon becomes selectable in
// the loadout (00332ac0 / 002bf090). Nothing is added to the inventory.
void port_unlock_all_weapons(const ProfileAwards& awards, std::span<std::uint8_t, agent_record_size> agent,
    AgentLoadState& state);

class SoloActorSimulation;

bool port_perception_ignores(const SoloActorSimulation& actor);
void port_set_perception_ignored(const SoloActorSimulation& actor, bool ignored);
bool port_perception_engaged(const SoloActorSimulation& actor);
void port_set_perception_engaged(const SoloActorSimulation& actor, bool engaged);
// True when the actor, as an AI observer, sees no NPC: marked and not engaged.
bool port_perception_observer_blind(const SoloActorSimulation& actor);
// Clears every mark and every engagement (a new play session).
void port_clear_perception_ignored();

// The CPU partner's friendly-fire shield (PORT-ONLY, checkpoint 132pc3; port setting partner_friendly_fire, default 0;
// research/COOP.md "Checkpoint 132pc3"). Play registers the partner's actor by its object handle with a predicate that
// answers whether a damage source is friendly to the partner: the local player, or an actor whose team is not the
// partner's enemy on the team matrix *004ee620 (IsEnemyTeam 0022f440). The damage entry 00187360
// (SoloActorSimulation's damage) then, for a friendly source other than the partner itself:
//   - friendly_fire false (the default): the hit takes 00188780's player-vs-player form on every path (amount 0, type
//     7 -> 0, a head hit's category 2 -> 3, as 132ob7's bullet rule gives the partner), so bullets, melee, thrown
//     explosives and fire whose damage source is the player or an ally never hurt the partner; the 0-damage hit's
//     DamageMsg, hit notice +78 and sounds stay, as for two players' bullets;
//   - friendly_fire true: runs the original paths (132ob7's player-vs-player bullet rule in 00188780 gives 0; blasts
//     and fire hurt as in the original), except that the Super Agent cheat never raises such a hit (its 100000 replaced
//     the bullet rule's 0 and killed the partner in the user's TORONTO1 play-test).
// Enemies (a source the partner's team treats as an enemy), the partner's own fall damage and damage whose source is
// not an actor (a population object's blast, a fire zone) are unchanged. Nothing is registered by default.
struct PortPartnerShield {
    bool friendly_fire = false;
    std::function<bool(std::uint64_t source)> friendly;
};
void port_set_partner_shield(std::uint64_t actor, PortPartnerShield shield);
// The shield registered for the actor's object handle, or null.
const PortPartnerShield* port_partner_shield(std::uint64_t actor);
// Removes every shield (a new play session).
void port_clear_partner_shields();
// True when `source` hitting the shielded `actor` is a friendly hit: a nonzero source other than the actor itself
// that the shield's predicate calls friendly. False without a shield.
bool port_partner_friendly_source(const PortPartnerShield* shield, std::uint64_t actor, std::uint64_t source);

enum class PortCheat : std::uint8_t {
    infinite_health,   // the player takes no damage (00187360 sees +33a4 set)
    infinite_armour,   // the player's armour +33a0 is kept
    infinite_ammo,     // the player's clip and spare rounds are kept
    end_level,         // the level ends completed (objectives set, 0012fa60(G, 0))
    super_agent,       // the player's hits on other actors kill at once
    npcs_ignore_you,   // the NPC perception ignores the player
    unlock_all,        // every weapon and outfit unlocked in the agent record
};
inline constexpr std::size_t port_cheat_count = 7;
// The screen's row label keys ("PortCheat%d"), in PortCheat order.
inline constexpr std::array<const char*, port_cheat_count> port_cheat_names = {
    "Infinite Health", "Infinite Armour", "Infinite Ammo", "End Level", "Super Agent", "NPCs Ignore You", "Unlock All"};

bool port_cheat(PortCheat c);
// Sets the cheat and, when it changes, calls the changed hook.
void port_set_cheat(PortCheat c, bool on);
// Every cheat off, no hook calls (a new process state for tests).
void port_clear_cheats();
// The hook play installs (the immediate effects of a toggle); null for none.
void port_on_cheat_changed(std::function<void(PortCheat, bool)> hook);
// Whether the pause menu offers the Cheats entry (default on; --no-cheat-menu).
bool port_cheat_menu_enabled();
void port_set_cheat_menu_enabled(bool enabled);

// The Cheats screen's strings: StartMenu7 / StartMenuOpt7 (the start menu's
// 8th entry and its description), PortCheatsTitle and PortCheat0..6; StartMenu8 / StartMenuOpt8 the
// port-only Port Settings entry (checkpoint 132win11).
std::optional<std::string> port_cheat_string(const std::string& key);

// Super Agent's hit amount: 100000 (0x47c35000), the value the original's
// head shot uses at 00187768.
inline constexpr float port_super_agent_damage = 100000.0F;

// Unlock All, weapons: every AWARDS unlock row's bit (00532b10 +4) set in the
// agent's +b08 words, as 003352c0 sets one. The equipment list 00332ac0
// shows the weapons whose bit is set; nothing is added to an inventory.
// Returns the bits set that were clear.
std::size_t port_unlock_all_weapons(std::span<std::uint8_t, agent_record_size> agent, const AwardTables& tables);
// Unlock All: the weapons, then every parts-table row (the outfits and
// patches) appended to the +b58 vector as UnlockPart 00332610 does. Returns
// the weapon bits and the parts added.
struct PortUnlockResult {
    std::size_t weapon_bits = 0;
    std::size_t parts = 0;
};
PortUnlockResult port_unlock_all(std::span<std::uint8_t, agent_record_size> agent, AgentLoadState& state,
    const AwardTables& tables, std::span<const PartRow> parts);

// Infinite Ammo around one ammo use: the slots before it, then each slot that
// holds the same item gets back the clip and spare rounds it lost. The
// inventory's infinite reserve +99 (the AI's, kept by the reload 0013fb90
// and the throw 00141ea0) is set for the use too. Inactive when `active` is false.
class PortAmmoGuard {
public:
    PortAmmoGuard(WeaponInventory& inventory, bool active);
    ~PortAmmoGuard();
    PortAmmoGuard(const PortAmmoGuard&) = delete;
    PortAmmoGuard& operator=(const PortAmmoGuard&) = delete;

private:
    WeaponInventory& inv_;
    bool active_;
    std::array<WeaponSlot, 8> slots_{};
    bool reserve_{};
};

// The Cheats screen PORTCHEATS, synthesised from GAMEOPT2.GUI's widgets: its
// header label_controlsettings, its panel backhud and per cheat a row of
// textbackground8, label_freelookx and the value button value_manualaim
// (renamed pc_label%d / pc_value%d), each decorator keeping the original's
// IE ("GAMEOPT2" or "game") and node. Throws when a widget is missing.
GuiNode port_cheats_screen(const GuiNode& gameopt2);
// INGAMENU.GUI with the 8th button "08" ($StartMenu7) under
// ingame_button_grp: a copy of "07" one row lower, the group one row taller
// and label_paused moved below it. Throws when INGAMENU is not the expected layout.
void port_cheats_patch_ingamenu(GuiNode& ingamenu);
// Port-only (checkpoint 132win11): the Port Settings screen PORTSETTINGS, built like PORTCHEATS from
// GAMEOPT2.GUI: a title "ps_title" and `rows` rows "ps_background%d" / "ps_label%d" / "ps_value%d" 30 apart
// from y 90, their texts empty (PortGuiSettings fills them).
GuiNode port_settings_screen(const GuiNode& gameopt2, std::size_t rows);
// INGAMENU.GUI with the Port Settings button "09" ($StartMenu8) appended to ingame_button_grp (after "07",
// or "08" when the Cheats entry is there) and label_paused moved below it.
void port_settings_patch_ingamenu(GuiNode& ingamenu);
}  // namespace omega
