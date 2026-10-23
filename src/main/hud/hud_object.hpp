#pragma once
// The in-play HUD object *004ffc2c (hud.cc), built by 00245f00 at level start
// over the IARCADEN IE screen (ie_screen.hpp), and the HUD services that feed
// it: the info-bar ticker at +100 (003fa500 / 003fa3f0 / 003fa280 / 003fa380 /
// 003fa1b0, the notice 00242b00), the PDA quip list at +0 (00241da0, behind
// the AddPDAQuip native 001c63b0), the colour tables (00241e30 / 00246690 /
// 00246740), the gadget-icon fade 00244b20 and the callout rectangles
// 002440d0.
//
// Calls into code not ported here are kept, in order, in log() with their
// normalised arguments (H+off a HUD field, N<i> a screen node, T<i> a text
// object, MAP / GUI the service handles); none of their state is modelled:
//   00222b60 (+a8), the radar 002619b0 (+d0, unless hosted), the weapon display 00259420
//   (+634), the map 0026e070 (+6a0), 002cb3d0, the GUI manager calls
//   004187c0 / vtable +64 / 00418a80 / 0041e0f0. The text fade 003d0850 is
// logged and applied (ie_text_fade). The per-frame ticker update 003f9e60
// (called by the HUD update 00244fa0 at 0024519c) is ticker_update.
#include "hud_callouts.hpp"
#include "hud_crosshair.hpp"
#include "hud_radar.hpp"
#include "ie_screen.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

struct HudAmmoText;
struct ActorHealthState;

class HudServices {
public:
    virtual ~HudServices() = default;
    // 00241c70: 003f96e0(game, hog) then 0036e1b0 / 0036dc60 on the member.
    virtual std::vector<std::uint8_t> screen_file(const std::string& hog, const std::string& member) = 0;
    // *(001698c0() + 0x280 + 4 * index).
    virtual std::uint32_t font(std::uint8_t index) = 0;
    // 0026e070(0xa0-byte allocation, game, name): the object.
    virtual std::uint32_t create_map(std::uint32_t game, const std::string& name) = 0;
    // 004187c0(00414790(), class, file): the screen.
    virtual std::uint32_t gui_load(const std::string& cls, const std::string& file) = 0;
    // 0041db80(screen, name) kept when its vtable +58 IsA("GuiTextWidget")
    // is true, else 0; nullopt when the GUI manager cannot answer (the field
    // then throws at 0041db80 when read).
    virtual std::optional<std::uint32_t> gui_text_widget(std::uint32_t screen, const std::string& name) = 0;
    // The texture slot manager the screen's nodes load through, or null
    // (their slots and +63 then stay unresolved; see ie_screen.hpp).
    virtual IeTextureService* textures() = 0;
};

// A ticker slot (0xa0 bytes at +100 + 0xa0 * i).
struct HudTickerSlot {
    std::string text;                 // +0, 0x80 bytes
    std::array<float, 4> colour{};    // +80
    std::uint8_t priority = 0;        // +90
    std::uint8_t active = 0;          // +91
    float f94 = 0.0F;                 // seconds shown (while current)
    float f98 = 0.0F;                 // seconds waiting
};

// The info-bar ticker at hud +100 (003fa500).
struct HudTicker {
    std::array<HudTickerSlot, 8> slots;
    int current = -1;                 // +500, a slot or -1
    std::uint8_t level = 0;           // +504
    int element = -1;                 // +508
    int text = -1;                    // +50c, the element's text object
    std::uint8_t byte_510 = 0, byte_511 = 0, byte_512 = 0;
    float f514 = 0.0F, f518 = 0.0F, f51c = 0.0F, f520 = 0.0F, f524 = 0.0F;
};

// The ticker with its text object (+50c) and the log its 003d0850 calls go to.
struct HudTickerView {
    HudTicker& ticker;
    IeTextObject& text;
    std::vector<std::string>* log;  // "3d0850 <ref> <target> <seconds>" when non-null
    std::string ref;
    void fade(float target, float seconds) const;  // 003d0850, logged
};

// 003f9e60(ticker, dt), the ticker's per-frame update over its text object:
// waiting slots age (+98) and the highest priority at or above +504 wins (ties
// to the longest waiting); waiting priority-0 slots are dropped; with no
// current slot the winner is shown (003d08b0 / 003d0810 with alpha 0 /
// 003d0850 to its alpha over +514, +510 = 1); else the current ages (+94,
// not while fading in), a scroll-mode text retires once scrolled out (+5a),
// otherwise a waiting winner after +520 s fades it out over +51c (+512) or
// it expires after +524 s over +518 (+511); a finished fade (+3c == +50)
// clears +510 and, after a fade-out, retires the slot (with no current slot
// left the original stores through null: throws naming 003fa158).
void hud_ticker_update(const HudTickerView& view, float dt);
// 003fa3f0's slot fill (after the optional 003fa280 soft clear): the first
// inactive slot takes the text (0x80 bytes), colour, priority, +94 = +98 = 0.
void hud_ticker_add(HudTicker& ticker, std::string_view text, const std::array<float, 4>& colour,
                    std::uint8_t priority);
// 003fa280(ticker, priority, hard): see HudObject::ticker_clear.
void hud_ticker_clear(const HudTickerView& view, std::uint8_t priority, bool hard);
// 003fa1e0(ticker) (game::ResetLevel 0012f768): every slot +91 = 0; the
// current one (+500 holds the slot's address) as 003fa280's hard path: +500
// = 0, 003d08b0(+50c, 004be220 = ""), 003d0850(+50c, 1.0, 0.0), +512 = +511
// = +510 = 0.
void hud_ticker_reset(const HudTickerView& view);
// 003fa380(ticker, priority): see HudObject::ticker_level.
void hud_ticker_level(const HudTickerView& view, std::uint8_t priority);
// 003fa1b0(ticker, a1): the text's +10 = a1 ? 3 (scroll) : 2.
void hud_ticker_style(IeTextObject& text, bool a1);

// A PDA quip record (0x21 bytes in the vector at hud +0, count +4): the C
// strings at +1 and +11 as 00241da0 leaves them.
struct HudPdaRecord {
    std::uint8_t flag = 0;            // +0
    std::string text;                 // +1
    std::string name;                 // +11
};

// The script colour stores 00246740 / 00246690 (hud.cc), 00253670
// (pickup_menu.cc) and 00227a40 (objective.cc): cvt.s.w(x) / 255.0 per
// channel, the alpha as given.
std::array<float, 4> script_colour_255(std::int32_t r, std::int32_t g, std::int32_t b, float alpha);

// The hud.cc globals.
struct HudGlobals {
    std::uint8_t font_index = 0;                 // byte 004f7d40
    std::uint32_t text_word = 0;                 // *0055a278
    std::array<float, 4> colours_a{};            // 004f7d60 (00246740)
    std::array<float, 4> colours_b{};            // 004f7d70 (00246690)
    std::array<float, 4> text_colour{};          // 0055a280 (00241e30)
    std::uint8_t colour_select = 0;              // byte 004f8258 (00260ec0)
    std::uint8_t byte_48b308 = 0;                // set by 002741f0
    int hscallout = -1;                          // 004f7d90
    std::array<int, 4> callouts{-1, -1, -1, -1}; // 004f7da0 left, right, top, bottom
    int origin_callout = -1;                     // 004f7db0
    // The lock colour ramps 004f80f0 (SetLockColor) and 004f8190
    // (SetNVLockColor): 10 entries {threshold, r, g, b} (r, g, b as given,
    // 0..255), constructed by 004d0600 / 004d0620 with the no-op 00261080
    // (BSS zero); byte 004f8230 is set by every SetNVLockColor.
    std::array<std::array<float, 4>, 10> lock_ramp_a{};
    std::array<std::array<float, 4>, 10> lock_ramp_b{};
    std::uint8_t lock_ramp_b_set = 0;            // byte 004f8230
};

// 00260fb0 (game::SetLockColor, table 004f80f0) / 00260ed0
// (game::SetNVLockColor, table 004f8190, which first sets byte 004f8230 = 1)
// with the script words (index, threshold, r, g, b): an index >= 10 stores
// nothing; otherwise the threshold is stored only when > 0 (c.ole.s thr, 0
// skips) and r, g, b as cvt.s.w. A negative index passes the signed slti and
// stores before the table (not hosted: throws naming the native).
void hud_set_lock_colour(HudGlobals& globals, bool nv, std::int32_t index, float threshold, std::int32_t r,
                         std::int32_t g, std::int32_t b);

// The script variables game::lockAlpha (0048b0a0) / game::NVlockAlpha
// (0048b0a8), the alpha 00260d90 stores.
struct HudLockAlpha {
    float lock = 0.0F;
    float nv = 0.0F;
};

// 00260d90(out, value): the NV ramp 004f8190 (and NVlockAlpha) when byte
// 004f8258 and byte 004f8230 are both set, else 004f80f0 (lockAlpha). The
// first t in 0..8 with value < entry[t + 1].threshold (c.olt.s) gives
// f = (value - e[t].th) / (e[t + 1].th - e[t].th) and each channel
// ((1 - f) e[t].c (mula.s) + f e[t + 1].c (madd.s)) / 255.0. nullopt when no
// segment matches: the original then leaves out unwritten.
std::optional<std::array<float, 4>> hud_lock_colour(const HudGlobals& globals, const HudLockAlpha& alpha, float value);
// 00244960's colour step on the fptarget element: 00260d90, then 003e7ee0
// (each component through ie_colour_byte to +1a..+1d); throws naming
// 00260d90 when no segment matches.
void hud_lock_meter_element(IeElement& element, const HudGlobals& globals, const HudLockAlpha& alpha, float value);

// The R1 lock-on reticle. 00244380 makes a target-lock tracker (hud.cc, 0x90
// bytes from 0036b180, vtable 004e0480 over the callout class 004e05a0, which
// it differs from only in the destructor +8 and the draw +110): the
// constructor 003e99b0(obj, element, &id) takes a new object id (0040c240,
// type 0xf3), runs the object base 003cf0e0 (type +49 = 0xd, +2c = 1, +2d
// = 1 through virtual +5c 003ce980, +2e = 0, +2f = 1: the object loop
// 00133420 calls its update +48 every pass), stores +60 = element, +64 = 1,
// +68 = *id, +84 = 0 and registers it in the scope *00571748 (0040c9d0 ->
// 0040b790, appended at the end of the scope's vector, so the loop reaches
// it in the pass that made it). +70 and +80 are not stored by the
// constructor; 003e97b0 sets +80 before anything reads it and the update
// writes +70 before the draw reads it (both gated by +64).
struct HudLockTracker {
    int element_60 = -1;                  // +60: the lock element
    std::uint8_t active_64 = 1;           // +64: the target is followed
    std::int32_t id_68 = 0;               // +68: the target's object id
    std::array<float, 4> point_70{};      // +70: the target point (view space once drawn)
    std::int32_t sub_80 = 0;              // +80: the subtarget (+c0's a2)
    std::uint8_t release_84 = 0;          // +84: 003e9730 / 003e9770 asked for the release
    bool in_tree = true;                  // 0040c9d0 until 0040c9a0
    bool deleted = false;                 // the destructor 003e9ba0 (003e9940) ran
    // A callout tracker (00242fa0: 0013d5c0(0x90) and 003e99b0 alone, vtable
    // 004e05a0): its draw +110 is 003e97d0 and its destructor +8 003e9940.
    bool callout = false;
    // Checkpoint 132map1: the map's player callout tracker (00242fa0 with t0:
    // 002434b0, 003e99b0 then vtable 004e0240). Its vtable differs from
    // 004e05a0 in the destructor +8 (003e93e0) and the draw +110 (003e9460,
    // hud_map_callout_place), which places the point through the map's view
    // 0055a290 instead of the game camera (EE goldens).
    bool map_callout = false;
};

// What the lock functions do on the IE screen and the HUD's colour ramp.
struct HudLockOps {
    std::function<int(int node)> create;                           // 003ef010
    std::function<void(int element)> remove;                       // 003e8eb0
    std::function<void(int element, float t)> evaluate;            // 003f45a0
    std::function<void(int element, float target)> fade;           // 003f44e0
    std::function<void(int element, float value)> colour;          // 00260d90(sp, value), 003e7ee0(element, sp)
    std::function<int(int element)> node;                          // element +70
    std::function<float(int element)> time;                        // element +4 (the animation time)
    std::function<void(int element, const std::array<float, 4>&)> point;   // 003e8180: element +20
};

// The HUD fields the lock functions use.
struct HudLockRefs {
    int node_5c;                          // target_lock (any subtarget)
    int node_60;                          // target_lockswitch (a subtarget)
    int& tracker_64;                      // index into trackers, -1 null
    int& element_68;
    std::vector<HudLockTracker>& trackers;
};

// 00244380(hud, &id, sub): node = sub == -1 ? +5c : +60. With +68 null:
// +68 = 003ef010(node) and a new tracker on it becomes +64 (an old +64 is
// not touched). With +68 on another node (+70): 003e9770(+64) when +64 is
// set, else 003e8eb0(+68); then the same new element and tracker. With +68
// already on the node nothing changes. Then 003e97b0(+64, &id, sub): +68 =
// id, byte +64 = 1, +80 = sub.
void hud_lock_target(const HudLockRefs& hud, const HudLockOps& ops, std::int32_t id, std::int32_t sub);
// 00244240(hud): 003e9730(+64) when set (003f45a0(+60, 0.0), +84 = 1), then
// 00244280.
void hud_lock_changed(const HudLockRefs& hud, const HudLockOps& ops);
// 00244280 (= 00244a00): with +68 set, 003e8eb0(+68) unless +64 is set, +68 =
// 0; then with +64 set, 003e9770(+64) (003f44e0(+60, 0.0), +84 = 1), +64 = 0.
void hud_lock_release(const HudLockRefs& hud, const HudLockOps& ops);
// 002442e0(hud, x): with +68 set, 00260d90(sp, 1.0 - x) and 003e7ee0(+68, sp),
// then 003f45a0(+68, x < 0.95 (c.olt.s 0x3f733333) ? 15.0 x : 15.0).
void hud_lock_aim(const HudLockRefs& hud, const HudLockOps& ops, float x);
// The tracker's update +48 = 003e9880. With byte +64: 0040c540(&+68) finds
// the object; found, its virtual +c0(out, object, +80) quadword goes to +70
// (point returns it), else byte +64 = 0. Then with byte +84 and the
// element's time < 0.5 (c.olt.s): 0040c9a0 (0040b720: the key joins the
// scope's pending removals, which 0040c5c0 -> 0040b4b0 applies after the
// object loop, before the render pass), 003cb230(obj, 0.1) and +84 = 0.
// Returns true when it asked for that delayed delete.
using HudLockPoint = std::function<std::optional<std::array<float, 4>>(std::int32_t id, std::int32_t sub)>;
bool hud_lock_tracker_update(HudLockTracker& tracker, const HudLockOps& ops, const HudLockPoint& point);
// The render pass's type-0xd draw +58 = 002610f0: with byte +64, +110 =
// 003e96f0: 003e9a60(&+70) and 003e8180(+60, &+70). 003e9a60 with the
// current camera *00583878 (its +c equal to *0058387c; G's camera stack
// push 00131320 / pop 00131240 keep both) puts +70 in view space: 00133330
// (the camera's +50 node) and 00133190 (the node +10 matrix through 00133300
// and the rigid inverse 00133200), then the vmula/vmadd product. No camera
// leaves +70 as it is. camera_node is the node +10 matrix, four columns.
void hud_lock_tracker_draw(HudLockTracker& tracker, const HudLockOps& ops,
                           const std::optional<std::array<std::array<float, 4>, 4>>& camera_node);
// A callout tracker's draw +110 = 003e97d0 (002610f0 gates it on byte +64
// too): 003e9a60(&+70) as above; behind = +74 < 0.0 (c.olt.s, the view-space
// depth); +70 = 00374bc0(camera, &+70) (hud_project_374bc0); when behind,
// +78 = -+78 (neg.s); then 003e8180(+60, &+70).
void hud_callout_tracker_draw(HudLockTracker& tracker, const HudLockOps& ops,
                              const std::optional<std::array<std::array<float, 4>, 4>>& camera_node,
                              const HudCalloutCamera& camera);
// Checkpoint 132map1: a map callout tracker's draw +110 = 003e9460 (vtable
// 004e0240; its callers 002610f0 and 00244c4c test +64 first): 003e8180(+60,
// hud_map_callout_place(camera, +70)). +70 is not written.
void hud_map_callout_tracker_draw(HudLockTracker& tracker, const HudLockOps& ops, const HudMapCalloutCamera& camera);
// The delayed delete's fire 003cb4f0 -> 0040bf50 (the tracker's removal is
// pending, so it is deleted): virtual +8 = 003e9ba0 (a callout tracker's
// 003e9940 does the same), which removes the element (003e8eb0(+60),
// unconditionally) before the object base 003cef20.
void hud_lock_tracker_delete(HudLockTracker& tracker, const HudLockOps& ops);

class HudObject {
public:
    // The lock colour natives on the HUD object's copy of the globals.
    void set_lock_colour(bool nv, std::int32_t index, float threshold, std::int32_t r, std::int32_t g, std::int32_t b);
    // 00244960(hud, value): with the fptarget element +68 set, 00260d90 and
    // 003e7ee0(+68, colour) (each component 255 c through the 2^31 split, low
    // byte, to +1a..+1d). A value no ramp segment covers would hand
    // 003e7ee0 the uninitialised stack: throws naming 00260d90.
    void lock_meter(float value, const HudLockAlpha& alpha);
    // 00245f00(hud, game + 0x3c, screen name, map name). With host_radar the
    // radar 002619b0(+d0, root child "radargroup") runs (hud_radar_construct)
    // where it is otherwise logged.
    static HudObject construct(HudServices& services, std::uint32_t game, std::string_view screen_name,
                               std::string_view map_name, const HudGlobals& globals, bool host_radar = false);

    // 003fa3f0(hud + 100, text, colour, priority, clear).
    void ticker_add(std::string_view text, const std::array<float, 4>& colour, std::uint8_t priority, bool clear);
    // 003fa280(hud + 100, priority, a2).
    void ticker_clear(std::uint8_t priority, bool hard);
    // 003fa380(hud + 100, priority).
    void ticker_level(std::uint8_t priority);
    // 003fa1b0(hud + 100, a1).
    void ticker_style(bool a1);
    // Sets +500 directly (a slot or -1), for the goldens' starting states.
    void ticker_set_current(int slot);
    // 003fa1e0(hud + 100): see hud_ticker_reset.
    void ticker_reset();
    // 003f9e60(hud + 100, dt): see hud_ticker_update.
    void ticker_update(float dt);
    // 003f9e30(hud + 100, v): 003e8de0(+508, v) when the ticker has its element.
    void ticker_show(bool v);
    // 00242b00(hud, text): a null text does nothing.
    void notice(const std::optional<std::string>& text);
    // 00241da0(hud, name, text).
    void add_pda(std::string_view name, std::string_view text);
    // The AddPDAQuip native 001c63b0(name, text) on *004ffc2c: nothing for a
    // null name; a null or empty text is "PDAMsg" (0049f490); 00241da0, then
    // the notice 00242b00 of get_string("PDAMsg") (003fdb50, nullopt = NULL).
    // In multiplayer (byte 005721c8) the network message built from 001c6440
    // is not hosted: throws after the local effects.
    void add_pda_quip(const std::optional<std::string>& name, const std::optional<std::string>& text,
                      const std::function<std::optional<std::string>(const std::string&)>& get_string,
                      bool multiplayer);
    // 00241e30(hud, a1).
    void select_colours(bool a1);
    // 00246740 (table a) / 00246690 (table b).
    void set_colour_table(bool table_b, std::int32_t r, std::int32_t g, std::int32_t b, float alpha);
    // The same stores into the globals before a HUD object exists (IARCADE.SO's
    // iarcade::Init runs them during the level load, before 00245f00).
    static void set_colour_table(HudGlobals& globals, bool table_b, std::int32_t r, std::int32_t g, std::int32_t b,
                                 float alpha);
    // 00244b20(hud, a1).
    void gadget_fade(std::uint8_t a1);
    // 00244a80(hud, a1, a2): the light icon on +74 (the flashlight toggle
    // 00199770 passes a1 = 0 and its new state; the IR switch 00199590
    // passes the slot-6 record's +100, or 0, and the IR byte). Nothing when +74 is null; with
    // t = 210 + 30 * a1, a2 == 0 evaluates t + 10 then fades to t, any other
    // a2 evaluates t then fades to t + 10 (003f45a0 / 003f44e0).
    void flashlight_icon(std::uint8_t a1, std::int32_t a2);
    // 002440d0(hud): the four callout rectangles at +10.
    void callout_rects();
    // 00245e30(hud), called once at 00168b14: for each name of the weapon
    // database's reticle list (*(*004ffd30 + d4), 00145d58) the root child
    // of that exact name becomes an element (00260d50), is pushed to the
    // vector +7c (00245ee0 -> 00139200) and hidden (003e8de0(e, 0)); a name
    // with no such child pushes null and throws at 003e8df4.
    void build_reticles(const std::vector<std::string>& names);
    // 00244280 (= 00244a00): hud_lock_release on the HUD's screen.
    void reticle_release();
    // 00244380 / 00244240 / 002442e0 (hud_lock_target / hud_lock_changed /
    // hud_lock_aim) on the HUD's screen; lock_aim's colour step is
    // hud_lock_meter_element (00260d90 throws when no segment matches).
    void lock_target(std::int32_t id, std::int32_t sub);
    void lock_changed();
    void lock_aim(float x, const HudLockAlpha& alpha);
    // The object loop's update of every tracker still in the tree
    // (hud_lock_tracker_update, creation order); returns the trackers that
    // asked for the 0.1 s delayed delete (003cb230), which delete_tracker
    // fires.
    std::vector<int> update_trackers(const HudLockPoint& point);
    void delete_tracker(int tracker);
    // The render pass's draw of every tracker in the tree (002610f0 ->
    // 003e96f0, a callout tracker's 003e97d0 with callout_camera: throws
    // without it), before the HUD draw 00244b90.
    // Checkpoint 132map1: a map callout tracker (map_callout) takes 003e9460
    // with map_camera (the map's view 0055a290 and the render camera record);
    // without it the draw throws naming 003e9460.
    void draw_trackers(const std::optional<std::array<std::array<float, 4>, 4>>& camera_node,
                       const HudCalloutCamera* callout_camera = nullptr,
                       const HudMapCalloutCamera* map_camera = nullptr);
    // Checkpoint 132map1: the HUD draw 00244b90's map path (+6a0 and +6a9,
    // 0x00244bd8..0x00244c88), after the canvas (the map screen, 0026ca70)
    // and before the HUD screen 003e7670: for each record of the +6e8 list
    // with a tracker +10, an element +c and the tracker's +64, the tracker's
    // draw +110 again: 003e9460 for a map callout tracker, else 003e97d0
    // (camera_node, map_camera.record), which transforms +70 once more as
    // the original does. map_camera.record is the camera record as 0026ca70
    // leaves it (hud_map_callout_record).
    void map_callouts_place(const std::optional<std::array<std::array<float, 4>, 4>>& camera_node,
                            const HudMapCalloutCamera& map_camera);
    // 002434f0(hud), the HUD draw 00244b90's callout pass when the map is
    // not shown (Checkpoint 132hud4). The video rectangle is (-w/2, -h/2, w,
    // h) (*00538200 / *00538204, the video object's +10 / +14). Each record
    // with a tracker, an element (shown, +18) and +18 (hscallout_bottom):
    // its tracker +70, unless in front (+78 not below 0) and inside the
    // rectangle (002465b0), is placed by hud_callout_offscreen (0040c540 and
    // the object's virtual +c0 through point: not found skips the record;
    // no camera throws 00243668). Then the clamp to the rect +40 (rects_[3],
    // raised by the text's lines x font height above the node's TEXT frame
    // +d0's +4 when that is lower): left (s2 0), right (1), top (2 unless
    // 0), bottom 112 above the edge (3 unless 0); a clamped record further
    // than 3600.0 (+14) is skipped. +78 = 10.0; when s2 is not the index of
    // the element's node among *004f7da0[0..3], the element is replaced by
    // one of *004f7da0[s2] (003ef010) with the text (003d08b0, +54 = 1.0),
    // the animation +4..+14 and the colour 00587d90 (003e7ee0), the old one
    // removed (003e8eb0) and the tracker's +60 moved to it. Finally tracker
    // +70 = the point and 003e8180(+60, point).
    void callout_edges(const std::optional<HudCalloutEdgeCamera>& camera, const HudLockPoint& point,
                       std::int32_t video_w, std::int32_t video_h);
    const std::vector<HudLockTracker>& trackers() const { return trackers_; }
    // 00242fa0(hud, &key, text, a3 slot, t0, t1, f12 value, f13 colour), the
    // callout natives' producer. The first record of +6e8 with that key and
    // slot: 003e7ec0(+c, text), +4 = value. Otherwise a new record (+0 key,
    // +4 value, +18 t1; t0 / t1 pick the origincallout2 / hscallout_bottom
    // nodes): +c = 003ef010(hscallout *004f7d90, or with t1 hscallout_bottom
    // *004f7dac, Checkpoint 132hud4), +10 = a
    // callout tracker on it (003e99b0, the key), the text object's +54 = 1.0
    // (00243460), +8 = slot, 003e7ec0(+c, text), 003f44e0(+c, 10.0 + (lines -
    // 1) when 003d04c0 counts two or more lines), 003e97b0(+10, key, slot),
    // +14 = 0, 003e8de0(+c, 0) and the record joins the list's end. Then the
    // colour: 0.0 (c.eq.s) 003e7ee0(+c, 00587d90); below 0.0 the element's
    // +19 = 0 and 003d0810(text, 00587db0); else 00260d90(colour) (the lock
    // ramp: throws without alpha or a segment), 003e7ee0 and 003d0810(text,
    // 00587da0). Returns 1. The line count is callout_line_count (throws
    // when unset).
    // t0 (checkpoint 132hud2, the map's player callouts 0026d940) clones origincallout2 (*004f7db0) instead,
    // with a 002434b0 tracker and the text's +5b = 1 (00243490) and +10 = 2 (00243480) in place of +54.
    bool callout_set(std::int32_t key, const std::optional<std::string>& text, std::int32_t slot, bool t0, bool t1,
                     float value, float colour, const HudLockAlpha* alpha = nullptr);
    // 00242b40(hud, &key, slot, a3, t0): in multiplayer (byte 005721c8) with
    // t0 a network message (not hosted: throws). The first record with that
    // key and slot (none: 0): with a3, 003f45a0(+c, 0.0) and 003e8de0(+c, 0);
    // 003e7ec0(+c, null) when +c is set; 003e9770(+10) when +10 is set (the
    // tracker fades the element to 0 and releases it); the record is freed
    // (0036b130) and erased (0015af40). Returns 1.
    bool callout_clear(std::int32_t key, std::int32_t slot, bool a3, bool t0, bool multiplayer = false);
    // 00242330(hud, dt), the HUD update's step after the ticker (00245194,
    // only with a record in +6e8). Each record (the next one is read first):
    // without +c, the timer step. Paused (byte 004ffbb7), or 00241ac0 with
    // the GUI handle +6a4's +d4 not 1: 003e8de0(+c, 0), next (frame.gs_d4).
    // With +6a9 and +c on the hscallout node: hidden, next.
    // Else 003e8de0(+c, 1); 0040c540(&key): absent or not (+2d, +2c and not
    // +2e) hides it, then the timer step. With +18 and the object's virtual
    // +108 at 3, 00242b40(key, +8, 0, 0) and next, without the timer step
    // (Checkpoint 132hud4; an object without +108 throws 002424b4). With the
    // map +6a0 and +6a9, the timer step. With the player (*004ffc0c, its +30):
    // d = player +e4 - object +e4 (vsub), +14 = d.x^2 + d.y^2 + d.z^2 (vmul,
    // vmulax / vmadday / vmaddz by 1.0); above 8100.0 (c.ole.s) hidden. The
    // timer step: with +4 above 0.0, +4 -= dt and, once it is <= 0.0,
    // 00242b40(key, +8, 0, 0).
    void callout_update(const HudCalloutFrame& frame);
    const std::vector<HudCalloutRecord>& callouts() const { return callouts_; }
    // 003d04c0(text object): the line count the callout's fade uses (the
    // host's font layout).
    std::function<std::int32_t(const IeTextObject&)> callout_line_count;
    // 0036a2f0(font): the font's line height (002434f0's text adjustment;
    // the host's fonts).
    std::function<std::int32_t(std::uint32_t font)> callout_font_height;
    // Set by hosts that forward the callout natives to this object (play
    // with --gui-overlay); the script host only records them otherwise.
    bool callouts_hosted = false;
    // 00244a10: 00244280; with the fptarget node +58: 003ee830(+58, 0), +68 =
    // 003ef010(+58), 003e8de0(+68, 1), 003f45a0(+68, 0.0), 003f44e0(+68, 17.0).
    void reticle_default();
    // The pad producer's meters 0024cec8..0024ceec (every frame with an
    // actor): 002441e0(hud, trunc(+40), +3398) (health, element +6c) and
    // 00244180(hud, +33a0, +339c) (armor, +70); each does nothing without
    // its element, else 003f44e0(element, 45 x (max - current) / max)
    // (hud_meters, weapon_hud.hpp): the element's VERTEX track runs to that
    // time.
    void pad_meters(const ActorHealthState& health);
    // 00242190's stores (hud_ammo_text computes them): 003e7ec0(+69c, text) =
    // 003d08b0(element +74, text) (a null element or text object throws),
    // then the weapon display's model +694 and byte +698.
    void ammo(const HudAmmoText& ammo);
    // 002449a0 / 002449d0: 003e8de0(+78, 0 / 1) when +78 is set.
    void nofire_hide();
    void nofire_show();
    // The reticle switch 00241fc8..0024217c (hud_crosshair_update) for the
    // index s2 of hud_crosshair_index, its events applied to the HUD in order.
    HudCrosshairResult crosshair(std::int32_t s2, const std::optional<HudCrosshairRecord>& held);
    // The radar step of the HUD update 00244fa0: 00261690(+d0, player)
    // (hud_radar_update). Throws when the radar is not hosted.
    void radar_update(const HudRadarFrame& frame);
    // 00261540(+d0, environmental, value) (hud_radar_meter): the danger call
    // (environmental false) leaves the targets' +30 in f_c0. Throws when the
    // radar is not hosted.
    void radar_meter(bool environmental, float value, const HudMeterVars& vars);
    // 00272d10(hud + 90, object), the map / objectives screens' waypoint
    // selection: the targets' +34 (hud + c4) = object, +38 (hud + c8) =
    // object ? its +c (object_id) : -1.
    void select_waypoint(std::uint64_t object, std::int32_t object_id);
    // 00272cd0(hud + 90), the radar's selected waypoint: +34 when it is null
    // or its +c (object_id) equals +38, otherwise null (0).
    std::uint64_t waypoint(const std::function<std::int32_t(std::uint64_t)>& object_id) const;
    // The screen step of the HUD update 00244fa0 at 002451f8:
    // 003e79c0(*(hud + 54), dt).
    void update_screen(float dt);
    // 00245e00(hud, name, a2, a3), the quip subtitle of gameSound's frame
    // update (001c8e10): 0045ce10(*(hud + 6a4), name, a2, a3) when the GUI
    // handle +6a4 is set (the GuiGameScreen 004187c0 is not hosted: logged).
    void subtitle(std::string_view name, std::uint32_t a2, std::uint32_t a3);

    IeScreen& screen() { return screen_; }
    const IeScreen& screen() const { return screen_; }
    const HudTicker& ticker() const { return ticker_; }
    // The map's open / close (0026dbc0 / 0026d720) save and restore +514..+524 and clear +504 directly.
    HudTicker& ticker_fields() { return ticker_; }
    const std::vector<HudPdaRecord>& pda() const { return pda_; }
    // 0013a290(hud) (game::ResetLevel 0012f758): the PDA vector's count +4 = 0.
    void clear_pda() { pda_.clear(); }
    // GuiRadioLog's event 0x13 (00454530): the played record's +0 = 1.
    void mark_pda(std::size_t i) { pda_.at(i).flag = 1; }
    const HudGlobals& globals() const { return globals_; }
    const std::array<std::array<float, 4>, 4>& callout_rect() const { return rects_; }
    // *(hud + 6e4): table b (004f7d70) when true, else table a.
    bool colour_table_b() const { return colour_b_; }
    const std::array<float, 4>& colour_selected() const { return colour_b_ ? globals_.colours_b : globals_.colours_a; }
    const std::vector<std::string>& log() const { return log_; }
    // +6bc / +6c0 (and +6d4 / +6d8).
    std::uint32_t timer_widget(bool value) const;

    // Fields (hud offsets): nodes / elements are screen indices (-1 null),
    // the service objects the handles the services returned.
    std::uint8_t byte_c = 0, byte_88 = 0;
    std::uint32_t game_50 = 0;
    int node_58 = -1, node_5c = -1, node_60 = -1;   // fptarget, target_lock, target_lockswitch
    int tracker_64 = -1;                            // the target-lock tracker (00244380), an index into trackers()
    int element_68 = -1;                            // the fptarget element 00244a10 creates
    std::uint32_t word_7c = 0, word_80 = 0;         // the reticle vector's capacity and count
    std::vector<int> reticles_84;                   // its elements (00245e30)
    int health_6c = -1, armor_70 = -1, gadget_74 = -1, nofire_78 = -1;
    std::int32_t word_8c = 0;
    // f_c0 is the targets' +30 (hud + 90 + 30): 1.0 from 002741f0, written by
    // the danger meter 00261540 and read by 00274260 (the markers' colour).
    float f_c0 = 0.0F;
    // The targets' +34 / +38 (hud + c4 / c8): the selected waypoint (an
    // object, 0 null) and its ID, written by 00272d10 (select_waypoint).
    std::uint64_t waypoint_c4 = 0;
    std::int32_t word_c8 = 0, word_cc = 0;
    std::uint32_t word_ec = 0, word_f0 = 0;
    std::uint32_t font = 0;
    int weapon_panel_630 = -1, ammo_69c = -1;
    // The weapon display at +634 (00259420): its +60 (the model, 0 at
    // construction) and byte +64 (7).
    std::int32_t model_694 = 0;
    std::uint8_t byte_698 = 7;
    std::uint32_t map_6a0 = 0, gui_6a4 = 0;
    // +6a9: set by the map screen's 002427f0 / 00242870 pair (not hosted; 0).
    std::uint8_t byte_6a9 = 0;
    std::uint32_t word_6dc = 0;
    float f_6e0 = 0.0F;
    // The list at +90: the map (its +0 = hud + 90), then hud + d0.
    std::vector<std::string> list_90;
    // The radar at +d0 when hosted.
    std::optional<HudRadar> radar_d0;

private:
    void record(std::string entry) { log_.push_back(std::move(entry)); }
    std::string text_name(int text) const;
    HudTickerView ticker_view();
    HudLockOps lock_ops(const HudLockAlpha* alpha);
    HudLockRefs lock_refs() { return {node_5c, node_60, tracker_64, element_68, trackers_}; }

    void callout_text(int element, const std::optional<std::string>& text, const char* address);

    IeScreen screen_;
    std::vector<HudLockTracker> trackers_;
    std::vector<HudCalloutRecord> callouts_;   // the std::list at +6e8, in order
    HudTicker ticker_;
    std::vector<HudPdaRecord> pda_;
    HudGlobals globals_;
    std::array<std::array<float, 4>, 4> rects_{};
    bool colour_b_ = false;
    std::optional<std::uint32_t> timer_text_, timer_value_;
    std::vector<std::string> log_;
};

} // namespace omega
