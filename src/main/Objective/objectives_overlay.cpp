#include "objectives_overlay.hpp"

#include <stdexcept>

namespace omega {

namespace {

// 32-bit EE arithmetic (addu / subu wrap).
std::int32_t add32(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}
std::int32_t sub32(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}

// (s8)+24 - 1 < 2 unsigned (sltiu): Succeed or Fail.
bool finished(const ObjectiveOverlayRecord& r) {
    return static_cast<std::uint32_t>(r.status - 1) < 2U;
}

struct Cursor {
    std::int32_t x;
    std::int32_t y;
    std::int32_t count;
    std::int32_t h;
};

template <class Match>
void section(std::vector<ObjectiveOverlayText>& out, const ObjectiveOverlayLists& lists, Cursor& c, const char* title,
             bool reset, ObjectiveOverlayColour colour, bool override_colour, Match match) {
    const auto pick = [&](ObjectiveOverlayColour slot) {
        return override_colour ? ObjectiveOverlayColour::override_colour : slot;
    };
    out.push_back({title, ObjectiveOverlayFont::header, pick(ObjectiveOverlayColour::header), c.x, c.y});
    c.y = sub32(c.y, c.h);
    if (reset) c.count = 0;
    for (const auto* list : {&lists.global, &lists.stage}) {
        for (const auto* r : *list) {
            if (r->active == 0 || !match(*r)) continue;
            out.push_back({r->text, ObjectiveOverlayFont::body, pick(colour), add32(c.x, 15), c.y});
            c.y = sub32(c.y, c.h);
            c.count = add32(c.count, 1);
        }
    }
    if (c.count == 0) {
        out.push_back({"[none]", ObjectiveOverlayFont::body, pick(colour), add32(c.x, 15), c.y});
        c.y = sub32(c.y, c.h);
    }
}

} // namespace

std::uint32_t objective_overlay_colour_address(ObjectiveOverlayColour colour) {
    switch (colour) {
    case ObjectiveOverlayColour::header: return 0x004f7580U;
    case ObjectiveOverlayColour::current: return 0x004f7590U;
    case ObjectiveOverlayColour::success: return 0x004f75a0U;
    case ObjectiveOverlayColour::fail: return 0x004f75b0U;
    case ObjectiveOverlayColour::override_colour: break;
    }
    throw std::runtime_error("objective_overlay_colour_address: the override has no table word");
}

std::vector<ObjectiveOverlayText> objectives_overlay(const ObjectiveOverlayLists& lists, std::int32_t body_height,
                                                     bool colour_override) {
    std::vector<ObjectiveOverlayText> out;
    // 00226200: sp+48 x = -0x69, sp+4c y = 0x8c, sp+44 count = 0; s0 = h.
    Cursor c{-105, 140, 0, add32(body_height, 2)};
    const auto open = [](const ObjectiveOverlayRecord& r) { return !finished(r); };
    // 00226d60 (0x4a5d70).
    section(out, lists, c, "Mission Objectives", false, ObjectiveOverlayColour::current, colour_override,
            [&](const ObjectiveOverlayRecord& r) { return open(r) && r.type != 5; });
    c.y = sub32(c.y, c.h);
    // 002265f0 (0x4a5d30).
    section(out, lists, c, "Completed Objectives", true, ObjectiveOverlayColour::success, colour_override,
            [](const ObjectiveOverlayRecord& r) { return r.status == 1 && r.type != 5; });
    c.y = sub32(c.y, c.h);
    // 002262f0 (0x4a5d10): no +26 test.
    section(out, lists, c, "Failed Objectives", true, ObjectiveOverlayColour::fail, colour_override,
            [](const ObjectiveOverlayRecord& r) { return r.status == 2; });
    c.y = sub32(c.y, c.h);
    // 00226910 (0x4a5d50): 002269a4..00226aa0 count the matches first and
    // return before the header when there are none.
    const auto parameter = [&](const ObjectiveOverlayRecord& r) { return open(r) && r.type == 5; };
    std::int32_t matches = 0;
    for (const auto* list : {&lists.global, &lists.stage})
        for (const auto* r : *list)
            if (r->active != 0 && parameter(*r)) ++matches;
    if (matches != 0)
        section(out, lists, c, "Mission Parameters", false, ObjectiveOverlayColour::current, colour_override, parameter);
    return out;
}

std::vector<const ObjectiveOverlayRecord*> objective_filter(const ObjectiveOverlayLists& lists, std::int32_t max,
                                                            const ObjectiveFilter& f) {
    std::vector<const ObjectiveOverlayRecord*> out;
    const auto want = [](std::int32_t v) { return v == 1; };
    for (const auto* list : {&lists.global, &lists.stage}) {
        // 002272e0: i < count, then (delay slot) n < max.
        for (std::size_t i = 0; i < list->size() && static_cast<std::int32_t>(out.size()) < max; ++i) {
            const auto& r = *(*list)[i];
            // +25 (lbu) must equal (a3 == 1) exactly: 2..255 never match.
            if (f.active != -1 && r.active != (want(f.active) ? 1U : 0U)) continue;
            if (f.finished != -1 && finished(r) != want(f.finished)) continue;
            if (f.parameter != -1 && (r.type == 5) != want(f.parameter)) continue;
            if (f.completed != -1 && (r.status == 1) != want(f.completed)) continue;
            if (f.failed != -1 && (r.status == 2) != want(f.failed)) continue;
            out.push_back(&r);
        }
    }
    return out;
}

ObjectiveFilter objective_filter_failed() { return {1, -1, -1, -1, 1}; }
ObjectiveFilter objective_filter_completed(std::uint32_t byte) {
    return {1, -1, -static_cast<std::int32_t>(byte & 0xffU), 1, -1};
}
ObjectiveFilter objective_filter_parameters() { return {1, 0, 1, -1, -1}; }
ObjectiveFilter objective_filter_open() { return {1, 0, 0, -1, -1}; }

std::int32_t objective_record_count(const ObjectiveOverlayLists& lists) {
    return static_cast<std::int32_t>(lists.global.size() + lists.stage.size());
}

ObjectiveOverlayLists ObjectiveOverlayState::lists() const {
    ObjectiveOverlayLists l;
    for (auto i : global) l.global.push_back(&records.at(i));
    for (auto i : stage) l.stage.push_back(&records.at(i));
    return l;
}

ObjectiveOverlayState objective_overlay_state(const ObjectiveStage& stage, const std::vector<Objective>& objectives) {
    ObjectiveOverlayState s;
    const auto& staged = stage.records();
    for (std::size_t i = 0; i < staged.size(); ++i) {
        const auto& st = staged[i];
        ObjectiveOverlayRecord r;
        r.status = static_cast<std::int8_t>(st.status);
        r.active = st.active ? 1 : 0;
        r.id = st.id;
        if (i < objectives.size()) {
            const auto& props = objectives[i].properties;
            if (const auto t = props.find("SetType"); t != props.end() && t->second.size() == 1)
                if (const auto* w = std::get_if<Word>(&t->second[0])) r.type = static_cast<std::int8_t>(*w & 0xffU);
            if (const auto t = props.find("SetMenuString"); t != props.end() && t->second.size() == 1)
                if (const auto* text = std::get_if<std::string>(&t->second[0])) {
                    r.text = *text;
                    r.has_text = true;
                }
        }
        s.records.push_back(std::move(r));
        if (st.destroyed) continue;   // DestroyObjectives: in no bucket
        if (st.stage == -1) s.global.push_back(i);
        else if (st.stage == stage.current_stage()) s.stage.push_back(i);
    }
    return s;
}

}
