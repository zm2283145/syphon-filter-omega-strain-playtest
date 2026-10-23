#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

struct ObjectiveMarker {
    std::uint32_t object_id{};
    std::uint32_t kind{2};
    std::string label;
    std::string cue;
    // Kind 1 (SetMapObject 00273210): +14 the objective (its 1-based handle);
    // 0 for kind 2 (00274b20 clears +14).
    std::uint32_t objective{};
    bool operator==(const ObjectiveMarker&) const = default;
};

enum class MarkerAction { added, removed };

// The HUD marker record fields read by 00274a60 / 002749b0 (00274b20's layout):
// the kind +10, the object +14 (non-NULL, and live when its +c is the record's
// +18 id) with its label +6c, and the strings +1c (the name) and +28 (the cue,
// a GetString key such as "30_20_3", "In the bank area.").
struct HudMarkerTextRecord {
    std::uint32_t kind_10{};
    bool object_14{};
    bool object_live{};
    std::optional<std::string> object_label_6c;
    std::string name_1c;
    std::string cue_28;
};

// 00274a60: kind 1 the live object's +6c, kind 2 the name +1c, else NULL.
std::optional<std::string> hud_marker_label(const HudMarkerTextRecord& record);
// 002749b0(record, buffer): NULL without a buffer or a label; with a non-empty
// cue that GetString 003fdb50 answers, sprintf(buffer, "%s %s" 004a8f78, label,
// text), else the label. The callers (0026c570, 003680a0) pass a stack buffer.
std::optional<std::string> hud_marker_ticker_text(
    const HudMarkerTextRecord& record, bool buffer,
    const std::function<std::optional<std::string>(std::string_view)>& get_string);
// A logical type-2 marker of this component as 00273570 files it: +1c the
// label, +28 the cue.
HudMarkerTextRecord hud_marker_text_record(const ObjectiveMarker& marker);
// A kind-1 marker as 00273210 files it: +14 the objective (never deleted, so
// live: +c == +18), whose +6c is its SetMapString (00225510; nullopt when
// never set), +1c empty and the cue +28: a3 (the cue overload 00224fa0's
// string), "" for a NULL a3 (004a8f70; SetMapObject 00225000).
HudMarkerTextRecord hud_marker_text_record(const ObjectiveMarker& marker, std::optional<std::string> objective_map_string);

// Logical type-1 / type-2 marker storage. Caller supplies an already resolved object
// ID and localized text. Icon resources and selection are outside this component;
// the +24 list is its update's (00273b40).
class ObjectiveMarkers;

// 00228190(objective, gobj, cue, 0) as SetMapObject 00225000 (cue NULL) and
// its cue overload 00224fa0 (cue = the third argument, *(args + 8)) call it,
// solo (005721c8 clear: the multiplayer send 0022830c.. is not taken).
// Nothing while the objective's map object +34 is live ((+34) +c == +38;
// slot_live) or for a null GOBJ. Otherwise +34 = the GOBJ and +38 = its +c
// (the caller stores the slot when this answers true), then 00273210 files
// the kind-1 marker (ObjectiveMarkers::add_map; a record of that id already
// filed adds nothing, but the slot is still set). Verified against EE
// goldens (tests/map_object_goldens.hpp).
bool map_object_set(ObjectiveMarkers& markers, bool slot_live, std::optional<std::uint32_t> gobj,
                    std::uint32_t objective, std::optional<std::string_view> cue);

class ObjectiveMarkers {
public:
    using Observer = std::function<void(MarkerAction, const ObjectiveMarker&)>;
    ObjectiveMarkers() = default;
    ObjectiveMarkers(const ObjectiveMarkers&) = delete;
    ObjectiveMarkers& operator=(const ObjectiveMarkers&) = delete;

    bool add(std::uint32_t object_id, std::optional<std::string_view> localized_text,
             std::optional<std::string_view> cue = std::nullopt);
    // 00273210(hud + 90, &GOBJ +c, objective, cue): nothing when a record of
    // that id in state 1 or 2 exists (every record is), else a kind-1 record
    // (00274b20: +0 the id, +14 the objective, +10 = 1, +28 the cue: a3
    // through 0013b480 / 0013d680, "" (004a8f70) for a NULL a3).
    bool add_map(std::uint32_t object_id, std::uint32_t objective,
                 std::optional<std::string_view> cue = std::nullopt);
    // 00272d50(hud + 90, &id, 1): the first record of that id in state 1 or 2
    // (also taken off +24, as hud_targets.cpp's remove_hud_target).
    bool remove(std::uint32_t object_id);
    // The state 1 / 2 records' part of the HUD targets update 00273b40(hud +
    // 90) (checkpoint 132hud3): per record in list order, 00274260: a record
    // whose object +0 Object_LookupById 0040c540 no longer finds ends (its
    // sprites killed and the record removed, 00273c58..00273cc0: observers see
    // `removed`); a kind-1 record (+10 = 1) also ends when its +14 objective
    // stops resolving (objectives are never deleted: never); every other
    // record is appended to +24, which the update rebuilds (00273b40 clears
    // +18 and +24 first). The state-0 target markers are HudTargets'.
    void update(const std::function<bool(std::uint32_t)>& found);
    // The +24 list as the last update built it (object IDs, in list order).
    // updated(): whether the host has run the update at all.
    const std::vector<std::uint32_t>& list_24() const { return list_24_; }
    bool updated() const { return updated_; }
    void observe(Observer observer);
    const std::vector<ObjectiveMarker>& records() const { return records_; }
    bool failed() const { return failed_; }
private:
    void mutable_context() const;
    void notify(MarkerAction action, const ObjectiveMarker& marker);
    std::vector<ObjectiveMarker> records_;
    std::vector<std::uint32_t> list_24_;
    bool updated_{};
    std::vector<Observer> observers_;
    bool notifying_{};
    bool failed_{};
};

}
