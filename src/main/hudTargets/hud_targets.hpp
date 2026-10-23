#pragma once
// The HUD target markers ("hudTargets.cc", the object at HUD +90) as far as
// their values go (research/COMBAT.md, "Checkpoint 54"): the marker list +c,
// the per-update lists +18 (the enemy characters the target picks read as
// *004ffc2c + a8) and +24 (markers in states 1 / 2), and the pad producer's
// pass 0022c210 that adds and removes markers for the target manager's
// registered objects. Interpreter-verified:
//   00273890  add a marker for an object ID (unless one in state 0 exists);
//   00272d50  remove the first marker of an ID whose "state 1 or 2" equals the flag;
//   00273b40  the update: per marker 00274260, removal of ended markers;
//   0022c210  the pass over the registration list +11c of 004f77c0;
//   002492a0  dropping the lock (+c0/+c4) when its object loses its marker.
// The sprites +4/+8 (00274750 builds them, 003e9770 kills them) and their
// colours (003e7ee0) are reported in order through HudTargetsHost::sprites
// (HudSpriteEvent; the radar blip +4 is drawn by hud_markers.hpp HudBlips and
// the map sprite +8 by HudMapSprites, checkpoint 132hud2). Not modelled:
// sounds, the
// listeners +8 / +10 (virtual +8/+10 of the list +0; +c is HudTargetsHost::added), the focus +3c (written by
// other HUD code; the update only clears it) and the meter 00261540 (its
// value, the largest aim progress of an enemy aiming at a local player, is
// returned).
#include "player_targeting.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

struct HudTargetMarker {
    std::int32_t id{};             // +0
    std::uint8_t enemy_c{};        // +c: the enemy flag the sprites were built with
    std::uint32_t state_10{};      // +10 (0: a target marker; 1 / 2 come from other markers)
    TargetHandle handle_14{0,-2};  // +14 / +18 (constructor 00274b20: 0 / -2; state 1: the marker ends when it stops resolving)
    std::uint32_t serial{};        // the marker's identity (the original's allocation)
    bool operator==(const HudTargetMarker&) const=default;
};

// The marker sprite calls, in call order.
enum class HudMarkerColour : std::uint8_t {
    player,   // 004ffdc0 (a player-controlled object)
    friendly, // 004ffdb0 (not an enemy)
    blocked,  // 004ffde0 (an enemy 0022cb10 reports)
    threat,   // 004ffdd0 / 004ffdf0 mixed by the aim progress (hud_marker_colour)
};
struct HudSpriteEvent {
    enum class Kind : std::uint8_t {
        build,   // 00274750(marker, enemy): +c = enemy, new sprites +8 / +4
        kill,    // 003e9770 on +8 and +4 (the rebuild in 00274260; the destructor with *0048b308 set)
        colour,  // 003e7ee0 on (+8) +60 and (+4) +60
    };
    Kind kind = Kind::build;
    std::uint32_t serial = 0;       // HudTargetMarker::serial
    std::int32_t id = 0;            // +0
    std::uint8_t enemy = 0;         // build: the flag
    std::uint32_t state = 0;        // build: +10
    HudMarkerColour colour = HudMarkerColour::player;
    float progress = 0.0F;          // colour threat: f20 before the +30 scale
    bool operator==(const HudSpriteEvent&) const=default;
};
using HudSpriteSink = std::function<void(const HudSpriteEvent&)>;

struct HudTargetsHost {
    std::function<std::uint64_t(std::int32_t id)> find;                   // 0040c540 (0: none)
    std::function<std::int32_t(std::uint64_t object)> object_id;          // +c
    std::function<bool(std::uint64_t object)> has_controller;             // +58 nonzero
    std::function<bool(std::uint64_t object)> player_controlled;          // +58 nonzero and its virtual +54
    std::function<std::array<std::uint8_t,3>(std::uint64_t object)> root_bytes;   // +2c, +2d, +2e
    std::function<std::uint8_t(std::uint64_t object)> kind_c4;            // virtual +c4
    std::function<bool(std::uint64_t a,std::uint64_t b)> enemy;           // 003cc8e0(a, b)
    std::uint64_t local_actor{};                                          // *(004ffc0c) +30
    std::function<std::uint64_t(std::uint64_t target)> blocker;           // 0022cb10(004f77c0, local actor, target, 0, 0)
    // The 003ea550 chain of 00274260: when the object's controller aims at an
    // object whose controller is a locally owned player, its +20; else nullopt.
    std::function<std::optional<float>(std::uint64_t object)> threat_progress;
    // Optional: the sprite calls (empty: not reported).
    HudSpriteSink sprites;
    // Optional (checkpoint 132hud5): the list +0's listener virtual +c, called by 00273890 (0x00273ab8..) with
    // the new marker once its sprites are built (the HUD map's 0026c8d0; empty: no listener).
    std::function<void(const HudTargetMarker&)> added;
};

struct HudTargets {
    std::vector<HudTargetMarker> markers;      // list +c, in order
    std::vector<TargetHandle> enemies_18;      // rebuilt by update
    std::vector<std::uint32_t> list_24;        // the markers (serials) in states 1 / 2, rebuilt by update
    std::uint32_t next_serial{1};
    bool operator==(const HudTargets&) const=default;
};

// 00273890(hud + 90, &id, any): false when a state-0 marker of the ID exists,
// the ID does not resolve, or (any = false) the object has no controller.
bool add_hud_target(HudTargets& hud,std::int32_t id,bool any,const HudTargetsHost& host);
// 00272d50(hud + 90, &id, ending); the removed marker's sprites are killed
// (00273018: *0048b308, 1 from the targets constructor 002741f0 at 00274240).
bool remove_hud_target(HudTargets& hud,std::int32_t id,bool ending,const HudSpriteSink& sprites={});
// 00273b40(hud + 90, ...): returns the value passed to 00261540.
float update_hud_targets(HudTargets& hud,const HudTargetsHost& host);

// 002492a0(input, object): when the lock +c0/+c4 resolves to the object, the
// subtarget +e4 = -1, the pick +c8/+cc is dropped, +bc = 1 and, with +3c bit 5,
// 00244280(hud) runs (returned true).
bool drop_target_lock(PadWeaponState& pad,PlayerTargetingState& state,std::uint64_t object,
    const std::function<std::int32_t(std::uint64_t)>& object_id);

// 0022c210(actor, input): for each resolving handle of the registration list
// except the actor: a player-controlled object gets a marker; otherwise the
// R1 pick or a character (+c4 = 2) gets one while present (+2d, +2c set, +2e
// clear) and loses it (002492a0 then 00272d50) when not; other objects of kind
// 0 lose it. Returns how many times 00244280 ran.
std::size_t update_target_markers(HudTargets& hud,const std::vector<TargetHandle>& registered,std::uint64_t actor,
    PadWeaponState& pad,PlayerTargetingState& state,const HudTargetsHost& host);

}
