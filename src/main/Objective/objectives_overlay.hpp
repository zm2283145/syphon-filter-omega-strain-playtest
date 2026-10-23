#pragma once
// Checkpoint 129c: the objective manager's text overlay 00226200 and the
// record filters 00227170 / 002270a0 / 002270c0 / 002270e0 / 00227100 and
// count 00227120 (objective.cc).
//
// The manager (00225cd0) keeps the records in 33 buckets of 32 pointers: the
// stage buckets at +b0 + (stage << 7) (counts +1130 + 4 * stage) and the
// global bucket (stage -1) at +10b0 (count +11b0). The overlay and the filter
// walk the global bucket ("list A") and then the current stage's bucket
// (+a4, "list B"), each in insertion order.
//
// 00226200(manager, colour) is called by the manager's frame handler 00225cf0
// only while +ac is set, and nothing in the image stores a nonzero +ac: the
// overlay is unreachable in retail. The objectives list the game shows in play
// is the Start menu's GuiObjectives listbox (00367d80, INGAMENU), which reads
// the same records through the filter wrappers.
#include "objective_stage.hpp"
#include "objectives.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace omega {

// The record fields the overlay and the filter read (constructor 00229948
// zeroes +24 / +26 and +60).
struct ObjectiveOverlayRecord {
    std::int8_t status = 0;   // +24: 1 Succeed, 2 Fail (lb)
    std::uint8_t active = 0;  // +25: Activate / Deactivate (lbu)
    std::int8_t type = 0;     // +26: SetType 002256e0 (lb); 5 is a mission parameter
    std::string text;         // +60: the SetMenuString 00225560 copy (00228b20)
    bool has_text = false;    // +60 non-null (the constructor zeroes it; only SetMenuString sets it)
    std::uint32_t id = 0;     // +20: the manager's +a8 counter at 00227750 (StagedObjective::id)
    bool operator==(const ObjectiveOverlayRecord&) const = default;
};

struct ObjectiveOverlayLists {
    std::vector<const ObjectiveOverlayRecord*> global;  // +10b0, count +11b0
    std::vector<const ObjectiveOverlayRecord*> stage;   // +b0 + (+a4 << 7), count +1130 + 4 * +a4
};

// The colour block a text is queued with: the override argument (a1) when
// nonzero, else the table word.
enum class ObjectiveOverlayColour : std::uint8_t {
    header,   // 004f7580 (SetObjectiveColor)
    current,  // 004f7590 (SetObjectiveCurrColor)
    success,  // 004f75a0 (SetObjectiveSuccColor)
    fail,     // 004f75b0 (SetObjectiveFailColor)
    override_colour,  // a1
};
std::uint32_t objective_overlay_colour_address(ObjectiveOverlayColour colour);

// The fonts 001698c0()+0x280 + 4 * byte: header *(s8)004f7560, body *(s8)0048ab20.
enum class ObjectiveOverlayFont : std::uint8_t { header, body };

// One 003d0ef0(003d1020(), text, font, colour, x, y) record: the text
// manager's immediate list, drawn by 003d0c10 as 0036a330(font, text, colour,
// (float)x, (float)y, 0.001) and then cleared.
struct ObjectiveOverlayText {
    std::string text;
    ObjectiveOverlayFont font = ObjectiveOverlayFont::body;
    ObjectiveOverlayColour colour = ObjectiveOverlayColour::current;
    std::int32_t x = 0;
    std::int32_t y = 0;
    bool operator==(const ObjectiveOverlayText&) const = default;
};

// 00226200: x = -105, y = 140, the shared count 0, h = 0036a2f0(body font) +
// 2. Sections (each moves y down by h after the header and after every line;
// the driver leaves a blank h between sections):
//   00226d60 "Mission Objectives": active, +24 not 1 / 2, +26 != 5; current;
//   002265f0 "Completed Objectives": count reset; active, +24 == 1, +26 != 5; success;
//   002262f0 "Failed Objectives": count reset; active, +24 == 2; fail;
//   00226910 "Mission Parameters": only when some record is active, +24 not
//     1 / 2 and +26 == 5 (then it shows them, current).
// Headers use the header font and 004f7580; lines the body font at x + 15.
// "[none]" (body font, x + 15, the section's colour) follows a section whose
// count is still 0. With the override every text takes it.
std::vector<ObjectiveOverlayText> objectives_overlay(const ObjectiveOverlayLists& lists, std::int32_t body_height,
                                                     bool colour_override);

// 00227170(manager, out, max, a3, t0, t1, t2, t3): -1 is "any"; otherwise
// a3: +25 == (a3 == 1); t0: (+24 is 1 or 2) == (t0 == 1); t1: (+26 == 5) ==
// (t1 == 1); t2: (+24 == 1) == (t2 == 1); t3: (+24 == 2) == (t3 == 1). Matches
// are appended while fewer than max (list A, then list B); a null out returns
// 0 (every caller passes a buffer).
struct ObjectiveFilter {
    std::int32_t active = -1;     // a3
    std::int32_t finished = -1;   // t0
    std::int32_t parameter = -1;  // t1
    std::int32_t completed = -1;  // t2
    std::int32_t failed = -1;     // t3
};
std::vector<const ObjectiveOverlayRecord*> objective_filter(const ObjectiveOverlayLists& lists, std::int32_t max,
                                                            const ObjectiveFilter& filter);
// 002270a0: a3 = 1, t3 = 1 (the failed ones).
ObjectiveFilter objective_filter_failed();
// 002270c0(manager, out, max, byte): a3 = 1, t1 = -(byte & 0xff), t2 = 1 (the
// completed ones; parameters too only for byte 1).
ObjectiveFilter objective_filter_completed(std::uint32_t byte);
// 002270e0: a3 = 1, t0 = 0, t1 = 1 (the open parameters).
ObjectiveFilter objective_filter_parameters();
// 00227100: a3 = 1, t0 = 0, t1 = 0 (the open objectives).
ObjectiveFilter objective_filter_open();
// 00227120: count A + count B.
std::int32_t objective_record_count(const ObjectiveOverlayLists& lists);

// The overlay records of the port's objective state: one per ObjectiveStage
// record (status +24, active +25) with ObjectiveHost's SetType (+26) and
// SetMenuString (+60) properties; list A the stage -1 records, list B those
// of the current stage, in AddObjective order (the buckets only grow).
struct ObjectiveOverlayState {
    std::vector<ObjectiveOverlayRecord> records;
    ObjectiveOverlayLists lists() const;
    std::vector<std::size_t> global;
    std::vector<std::size_t> stage;
};
ObjectiveOverlayState objective_overlay_state(const ObjectiveStage& stage, const std::vector<Objective>& objectives);

}
