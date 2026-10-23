#include "objective_markers.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace omega {
namespace {
void validate_text(std::string_view text) {
    if (text.find('\0') != std::string_view::npos)
        throw std::runtime_error("Marker strings with embedded NUL are unsupported");
}
}

void ObjectiveMarkers::mutable_context() const {
    if (failed_) throw std::runtime_error("Marker observer context previously failed");
    if (notifying_) throw std::runtime_error("Reentrant marker mutation is unsupported");
}

void ObjectiveMarkers::observe(Observer observer) {
    mutable_context();
    if (!observer) throw std::runtime_error("Empty marker observer");
    observers_.push_back(std::move(observer));
}

void ObjectiveMarkers::notify(MarkerAction action, const ObjectiveMarker& marker) {
    notifying_ = true;
    try {
        for (const auto& observer : observers_) observer(action, marker);
    } catch (...) {
        notifying_ = false;
        failed_ = true;
        throw;
    }
    notifying_ = false;
}

bool ObjectiveMarkers::add(std::uint32_t object_id,
                           std::optional<std::string_view> localized_text,
                           std::optional<std::string_view> cue) {
    mutable_context();
    // Original public binding skips unresolved/empty localized labels.
    if (!localized_text || localized_text->empty()) return false;
    validate_text(*localized_text);
    const auto found = std::find_if(records_.begin(), records_.end(),
        [object_id](const auto& marker) { return marker.object_id == object_id; });
    if (found != records_.end()) return false; // Duplicate does not replace data.
    if (cue) validate_text(*cue);
    ObjectiveMarker marker{object_id, 2, std::string(*localized_text),
                           cue ? std::string(*cue) : std::string{}};
    // Allocation failures occur before observers receive the logical change.
    records_.reserve(records_.size() + 1);
    notify(MarkerAction::added, marker);
    records_.push_back(std::move(marker));
    return true;
}

bool ObjectiveMarkers::add_map(std::uint32_t object_id, std::uint32_t objective,
                               std::optional<std::string_view> cue) {
    mutable_context();
    const auto found = std::find_if(records_.begin(), records_.end(),
        [object_id](const auto& marker) { return marker.object_id == object_id; });
    if (found != records_.end()) return false;
    if (cue) validate_text(*cue);
    ObjectiveMarker marker{object_id, 1, {}, cue ? std::string(*cue) : std::string{}, objective};
    records_.reserve(records_.size() + 1);
    notify(MarkerAction::added, marker);
    records_.push_back(std::move(marker));
    return true;
}

bool map_object_set(ObjectiveMarkers& markers, bool slot_live, std::optional<std::uint32_t> gobj,
                    std::uint32_t objective, std::optional<std::string_view> cue) {  // 00228190
    if (slot_live || !gobj) return false;
    markers.add_map(*gobj, objective, cue);
    return true;
}

std::optional<std::string> hud_marker_label(const HudMarkerTextRecord& record) {  // 00274a60
    if (record.kind_10 == 1) {
        if (!record.object_14 || !record.object_live) return std::nullopt;
        return record.object_label_6c;
    }
    if (record.kind_10 == 2) return record.name_1c;
    return std::nullopt;
}

std::optional<std::string> hud_marker_ticker_text(
    const HudMarkerTextRecord& record, bool buffer,
    const std::function<std::optional<std::string>(std::string_view)>& get_string) {  // 002749b0
    if (!buffer) return std::nullopt;
    const auto label = hud_marker_label(record);
    if (!label) return std::nullopt;
    if (!record.cue_28.empty()) {
        if (const auto text = get_string(record.cue_28)) return *label + ' ' + *text;
    }
    return label;
}

HudMarkerTextRecord hud_marker_text_record(const ObjectiveMarker& marker) {
    HudMarkerTextRecord record;
    record.kind_10 = marker.kind;
    record.name_1c = marker.label;
    record.cue_28 = marker.cue;
    return record;
}

HudMarkerTextRecord hud_marker_text_record(const ObjectiveMarker& marker, std::optional<std::string> objective_map_string) {
    if (marker.kind != 1) throw std::runtime_error("A kind-1 marker text record for a kind-" + std::to_string(marker.kind) + " marker");
    HudMarkerTextRecord record;
    record.kind_10 = 1;
    record.object_14 = marker.objective != 0;
    record.object_live = record.object_14;
    record.object_label_6c = std::move(objective_map_string);
    record.cue_28 = marker.cue;
    return record;
}

bool ObjectiveMarkers::remove(std::uint32_t object_id) {
    mutable_context();
    const auto found = std::find_if(records_.begin(), records_.end(),
        [object_id](const auto& marker) { return marker.object_id == object_id; });
    if (found == records_.end()) return false;
    notify(MarkerAction::removed, *found);
    if (const auto at = std::find(list_24_.begin(), list_24_.end(), object_id); at != list_24_.end()) list_24_.erase(at);
    records_.erase(found);
    return true;
}

void ObjectiveMarkers::update(const std::function<bool(std::uint32_t)>& found) {  // 00273b40 (states 1 / 2)
    mutable_context();
    if (!found) throw std::runtime_error("unhosted(0x273b40): the marker update without Object_LookupById 0040c540");
    list_24_.clear();
    updated_ = true;
    for (std::size_t k = 0; k < records_.size();) {
        if (!found(records_[k].object_id)) {
            notify(MarkerAction::removed, records_[k]);
            records_.erase(records_.begin() + static_cast<std::ptrdiff_t>(k));
            continue;
        }
        list_24_.push_back(records_[k].object_id);
        ++k;
    }
}

}
