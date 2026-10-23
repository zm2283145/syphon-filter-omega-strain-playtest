#pragma once
// The runtime IE screen built from a decoded .IE file (interface.cc 003e7b00,
// interface_model.cc 003ef660 / 003f0770 / 003eeb30 / 003eebe0 / 003ee830 /
// 003ef010, interface_element.cc 003e9000 / 003e8de0 / 003e7eb0, the animation
// base 003f45a0 / 003f44e0, the vertex tracks 003fce90 / 003f3ff0 and the node
// rectangle 003ecb50).
//
// Nodes, tracks and elements are kept by index (pre-order, file order). The
// 0x60-byte text objects an element creates (003d0940) and the text manager's
// draw list (003d1020()+0x10: 003e8e80 add, 00138ab0 remove) are a recorded
// draw service: the create arguments, every word/byte the interface code
// writes into the object and the add/remove order are kept; nothing is drawn.
// Node textures go through IeTextureService (00380a40 on the texture slot
// manager *00539248, name + ".TDX"); without one the slot (+18) and the
// translucency byte +63 of a textured node stay unresolved: +63 is read only
// by the node draw code 003ecd60 / 003ed2e0, which is not ported. Every path
// whose effect is not modelled throws naming its address.
#include "frontend_formats.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

// The animation base shared by tracks and elements: +4 time, +8 / +c the
// range, +10 the rate, byte +14 the loop flag.
struct IeAnim {
    float t = 0.0F;
    float lo = -1.0F;
    float hi = -1.0F;
    float rate = 0.0F;
    std::uint8_t loop = 0;
};

// A VERTEX sub-track (vtable 004e07a0): +18 the cached key index, +1c the
// node position it drives (sub-track j drives position j), keys {t, x, y, z}.
struct IeVertexSubTrack {
    IeAnim anim;
    std::int32_t index = 0;
    std::uint32_t position = 0;
    std::vector<std::array<float, 4>> keys;
};

// A node track. VERTEX (group vtable 004e0820) drives the positions through
// its sub-tracks; Checkpoint 122b adds the scalar kinds (vtable 004e07c0,
// 003f3e70: OPACITY +ac, ROT_X/Y/Z +b0..+b8, UVOFF_U/V +c0/+c4, UVSCL_U/V
// +c8/+cc, keys {t, v}) and DIFFUSE (vtable 004e07e0, 003f41c0: +a0..+a8,
// keys {t, r, g, b}); +18 is the cached key index, keys in file order
// (003f24a0 appends, nothing sorts).
struct IeRuntimeTrack {
    IeTrackKind kind = IeTrackKind::opacity;
    IeAnim anim;
    std::vector<IeVertexSubTrack> subs;
    std::int32_t index = 0;
    std::vector<std::array<float, 2>> scalar;
    std::vector<std::array<float, 4>> diffuse;
};

// The TEXT frame at node +d0 (0x20 bytes, 003f0770): file points (a, b, c)
// stored as (a, c, b, 1.0); the second point is the zero vector when absent
// or equal to the first.
struct IeTextFrame {
    std::array<float, 4> p0{};
    std::array<float, 4> p1{};
};

// The texture slot manager *00539248 as 003ef660 uses it.
class IeTextureService {
public:
    virtual ~IeTextureService() = default;
    // 00380a40(manager, name): the slot index, or -1.
    virtual std::int32_t load(const std::string& name) = 0;
    // The +48 word of the slot record (*(manager + 8))[index].
    virtual std::uint32_t record_flags(std::int32_t index) = 0;
    // False when the service does not model the record's +48 word: +63 of a
    // node whose slot is >= 0 then stays unresolved. The node draw reads +63
    // only when +18 < 0 (003ecdc0 / 003ed408 / 003ed708), so a textured
    // node's +63 never reaches a draw.
    virtual bool record_flags_known() const { return true; }
};

// One colour of a node as 003efdf0..003efef0 stores it: 255 * component,
// truncated (biased by 2^31 at or above 2^31), the low byte.
std::uint8_t ie_colour_byte(float component);

struct IeRuntimeNode {
    std::string name;
    std::string texture;                      // the name 00380a40 is given (texture + ".TDX")
    std::optional<std::int32_t> texture_slot; // +18: -1 without a texture; unresolved without a service
    std::optional<std::uint8_t> translucent;  // +63 (003f03dc..003f0440); unresolved as +18
    std::vector<std::array<std::uint8_t, 4>> colours;  // +78 (count +68)
    std::array<float, 16> matrix{};           // +20, rows (w 0, 0, 0, 1)
    std::uint8_t visible = 0;                 // +60
    std::uint8_t flag_61 = 0;                 // +61
    std::uint8_t flag_62 = 0;                 // +62
    std::vector<std::array<float, 3>> positions;  // +70
    std::vector<IeRuntimeTrack> tracks;       // +80
    std::vector<int> children;                // +84
    std::vector<int> elements;                // +8c list, creation (iteration) order
    std::array<float, 4> colour{1.0F, 1.0F, 1.0F, 1.0F};  // +a0
    std::optional<IeTextFrame> text;          // +d0 (the last TEXT record)
    // Checkpoint 122b (the draw 003ed880 reads them):
    std::vector<std::array<float, 2>> uvs;    // +74 (count +66)
    // +7c (count +6a): 9 u16 per triangle, the file words' low halves:
    // position indices +0/+2/+4, uv +6/+8/+a, colour +c/+e/+10.
    std::vector<std::array<std::uint16_t, 9>> triangles;
    std::array<float, 3> rot{};                    // +b0..+b8
    std::array<float, 2> uv_off{};                 // +c0, +c4
    std::array<float, 2> uv_scl{1.0F, 1.0F};       // +c8, +cc
    bool has_parent = false;                       // +98 (003ef660's a3) != 0
    // +d4: a NEXT record's 0x10-byte block {x, z, y, 1} (003f0ab8..003f0b94,
    // the file's second and third floats swapped like the TEXT points).
    std::optional<std::array<float, 3>> next;
};

// The text object 003d0940(obj, x, y, w, h, t1, t2) makes (0x60 bytes). The
// create arguments and the interface's later writes are recorded; a field
// nothing writes reads as the fresh allocation's zero.
//
// Checkpoint 119h adds the fade and scroll state (003d0940 initialises it):
// colour +30..+3c (copied from *t2; +3c is the alpha the fade drives), +40
// the scroll offset (-(float)w), +44 the last draw time in milliseconds (0),
// +48 the scroll period (float)*004937f0 (12), +4c the fade rate, +50 the
// fade target, +54 0.6, byte +59 fading, byte +5a scrolled out.
struct IeTextObject {
    std::int32_t x = 0, y = 0, w = 0, h = 0;
    std::uint32_t t1 = 0;
    std::uint32_t t2 = 0;
    std::uint32_t word_20 = 0;
    std::uint8_t byte_10 = 0;
    std::uint8_t byte_11 = 0;   // written by the pickup menu's 00251040 (vertical anchor)
    std::uint8_t byte_58 = 0;
    std::uint8_t byte_5b = 0;   // written by the pickup menu builder 00253070
    std::optional<std::string> text;  // the last 003d08b0(obj, text)
    std::array<float, 4> colour{};    // +30
    float f40 = 0.0F, f44 = 0.0F, f48 = 12.0F, f4c = 0.0F, f50 = 0.0F, f54 = 0.6F;
    std::uint8_t byte_59 = 0, byte_5a = 0;
    bool freed = false;  // freed by the element destructor 003e8ec0
    // Port-only (checkpoint 132ws): the HUD element the text was last placed by (IeDrawState::port_anchor at
    // ie_draw_element_text; 0 none), for the wide renderer's edge anchoring. Not game state.
    std::uint32_t port_anchor = 0;
};

// *004937f0, the scroll period 003d0940 converts into +48.
inline constexpr std::int32_t ie_text_scroll_period = 12;

// 003d08b0(obj, text) for a non-null text: the string (+14), then +44 = 0,
// +40 = -(float)w, +4c = 0, +59 = 0, +5a = 0.
void ie_text_set(IeTextObject& obj, std::string text);
// 003d0810(obj, rgba): +30..+3c, only while +59 is clear.
void ie_text_colour(IeTextObject& obj, const std::array<float, 4>& rgba);
// 003d0850(obj, target, seconds): with seconds == 0, +3c = target and +59 = 0;
// else +50 = target, +4c = (target - +3c) / (1000 * seconds), +59 = 1.
void ie_text_fade(IeTextObject& obj, float target, float seconds);
// The draw clock of 003cfab0 (003cfbc0..003cfc70): (T0_COUNT 10000800 +
// *00533870) as an unsigned float / 5.76, truncated, added to the 64-bit
// *00533878, then divided by 100 by the 31-step restoring loop.
std::int32_t ie_text_clock_ms(std::uint32_t t0_count, std::uint32_t t0_base, std::int64_t ticks);
// The state 003cfab0(obj) advances when the text manager draws the object
// (now_ms the draw clock): nothing without a font (+20); +44 takes now (first
// draw: delta 0), delta = now - old +44; while +59, +3c += +4c * delta and,
// once +50 - +3c and +4c no longer agree in sign (<= 0), +3c = +50 and +59 =
// 0; in scroll mode (+10 == 3) width = 0036aa50(+20, text, 0x200) and, when
// delta > +48, +40 += delta / +48 and +5a = 1 once +40 > width. The drawing
// itself (0036a2f0 / 003d04c0 / 0036a330 and the positions) is the draw
// service and is not performed. A scroll-mode object without a width service
// throws naming 0036aa50.
using IeTextWidth = std::int32_t (*)(const IeTextObject& obj, void* context);
void ie_text_draw_state(IeTextObject& obj, std::int32_t now_ms, IeTextWidth width = nullptr,
                        void* context = nullptr);

// An element (003e9000, 0x80 bytes, vtable 004e0220).
struct IeElement {
    int node = -1;                  // +70
    IeAnim anim;
    std::uint8_t shown = 1;         // +18
    std::uint8_t flag_19 = 1;       // +19
    std::array<std::uint8_t, 4> colour{};  // +1a..+1d
    std::array<float, 4> words_20{0.0F, 0.0F, 0.0F, 1.0F};  // +20..+2c
    std::array<float, 16> matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // +30, from vf0
    int text = -1;                  // +74, an index into text_objects()
    std::uint32_t data_78 = 0;      // +78 (003e7eb0)
    bool freed = false;             // removed and freed by 003eee70
};

// 003f43d0(anim, dt), the per-frame animation step: nothing at rate 0; t +=
// rate * dt; moving up past hi (or down below lo) it reverses at the bound
// when +14 loops, stops at the bound when lo == hi, else wraps to the other
// bound (t = lo going up, t = hi going down); then virtual +10(t), the no-op
// 003f43c0 for elements.
void ie_anim_update(IeAnim& anim, float dt);
// 003f4510(anim, a, b, loop): lo / hi = min / max(a, b); rate = 1 below lo,
// -1 above hi, else kept when it is +-1 and 1 otherwise; +14 = loop.
void ie_anim_range(IeAnim& anim, float a, float b, std::uint8_t loop);
// 003f45a0(anim, t): t, rate 0 (then virtual +10, the element no-op 003f43c0).
void ie_anim_evaluate(IeAnim& anim, float t);
// 003f44e0(anim, target): lo = hi = target, rate = (t <= target ? 1 : -1), loop 0.
void ie_anim_fade(IeAnim& anim, float target);
// 003e8070(element, v): the element matrix +30 = the rotation built by
// 00140e20 from cosf / sinf (game_libm) of v = (x, y, z), COP1 products and
// sums flushed, entries [9] / [10] through mula + msub / madd:
//   rows (cy cz, sz cx cy + sx sy, cy sx sz - cx sy, 0), (-sz, cx cz, sx cz, 0),
//        (sy cz, sz cx sy - sx cy, sz sx sy + cx cy, 0), (0, 0, 0, 1).
void ie_element_rotate(IeElement& element, const std::array<float, 3>& v);

enum class IeDrawOp : std::uint8_t { add, remove };
struct IeDrawEvent {
    IeDrawOp op = IeDrawOp::add;
    int text = -1;
};

// The colour block every element text object is created with (t2).
inline constexpr std::uint32_t ie_text_colour_block = 0x0055a280;

// EE COP1 helpers: a result flushed to zero when it is a denormal, and
// cvt.w.s (truncate, saturate).
float ie_ftz(float v);
std::int32_t ie_cvt_w(float v);

class IeScreen {
public:
    // 003e7b00 over the decoded file. text_word is *0055a278, the word every
    // element's new text object gets at +20. A root child named "preview"
    // (003e7c24: 003ef010, 003ee740, 003f4510) is not hosted.
    static IeScreen load(const IeFile& file, std::uint32_t text_word, IeTextureService* textures);

    int root() const { return 0; }
    // 003ee840: the first node named exactly name (1c43a0: std::string
    // compare) in pre-order from node (node itself first), or -1.
    int find_node(int node, std::string_view name) const;
    IeRuntimeNode& node_mut(int index) { return node_at(index); }
    // 003eeb30: the first direct child named exactly name, or -1.
    int find_child(int node, std::string_view name) const;
    // 003eebe0: +60 = v, on the whole subtree when recurse.
    void set_visible(int node, std::uint8_t v, bool recurse);
    // 003ee830: +61 = v.
    void set_flag_61(int node, std::uint8_t v);
    void set_flag_62(int node, std::uint8_t v);
    // 003ef010: shows the subtree, makes an element and appends it.
    int create_element(int node);
    // 00260d50: find_child then create_element, or -1.
    int find_element(int node, std::string_view name);
    // 003e8de0.
    void show_element(int element, std::uint8_t v);
    // 003e7eb0.
    void set_element_data(int element, std::uint32_t v) { element_at(element).data_78 = v; }
    // 003f45a0 on an element (vtable +10 is the no-op 003f43c0).
    void evaluate_element(int element, float t);
    // 003f45a0 on a node track (group 003fce90 -> each 003f3ff0; scalar
    // kinds 003f3e70, DIFFUSE 003f41c0).
    void evaluate_track(int node, int track, float t);
    // 003f44e0 on an element: lo = hi = target, rate = (t <= target ? 1 : -1),
    // loop = 0.
    void fade_element(int element, float target);
    // 003e79c0(screen, dt): 003ee610(root, 30 * dt), which on a visible node
    // (+60) steps every element in its +8c list (003f43d0) and then recurses
    // into the children; an invisible node skips its whole subtree.
    void update(float dt);
    // 003e8eb0(element) = 003eee70(element +70, element): every list entry of
    // the node holding the element is destroyed (003e8ec0: a text object is
    // removed from the draw list by 00138ab0 even when not shown, and freed)
    // and erased; a node whose list is then empty gets +60 = 0. A later use
    // of the element or its text object throws.
    void remove_element(int element);
    // 003ecb50(out, node, t): {min x, min z, width, depth} of the transformed
    // positions after evaluating the tracks (skipped when t < 0).
    std::array<float, 4> rect(int node, float t);

    // The recorded text-object service.
    IeTextObject& text_object(int index);
    const std::vector<IeTextObject>& text_objects() const { return texts_; }
    const std::vector<IeDrawEvent>& draw_log() const { return draw_; }
    void set_text(int text, std::string value);  // 003d08b0 (ie_text_set)
    // The colour *0055a280 that create_element copies into a new text object
    // (003d0940 from t2); 00241e30 rewrites the block.
    void set_text_colour_block(const std::array<float, 4>& rgba) { text_colour_ = rgba; }
    const std::array<float, 4>& text_colour_block() const { return text_colour_; }
    // The text objects in the manager's draw list (003e8e80 appends,
    // 00138ab0 removes), in list order.
    const std::vector<int>& drawn_texts() const { return drawn_; }
    // 003d0ef0(manager, text, font, colour, x, y): a record (std::string
    // 0013b480 copy, +c font, +10..+1c colour, +20 / +24 cvt.s.w x / y)
    // appended to the immediate list at manager +4 (003d1280).
    struct ImmediateText {
        std::string text;
        std::uint32_t font = 0;
        std::array<float, 4> colour{};
        float x = 0.0F, y = 0.0F;
    };
    void queue_immediate(std::string text, std::uint32_t font, const std::array<float, 4>& colour, std::int32_t x,
                         std::int32_t y);
    const std::vector<ImmediateText>& immediate_texts() const { return immediate_; }
    // The immediate records the last draw_texts took: 003d0c10 draws each
    // as 0036a330(+c, text, +10, x, y, 0.001, 0, 0, 0) before the listed
    // objects' 003cfab0 (the host draws them, in this order).
    const std::vector<ImmediateText>& drawn_immediates() const { return drawn_immediate_; }
    // 003d0c10's draw loop over the list: ie_text_draw_state on each object;
    // then the immediate list is cleared (003d10b0, also with +1c clear).
    // The manager's +1c draw byte is taken as set.
    void draw_texts(std::int32_t now_ms, IeTextWidth width = nullptr, void* context = nullptr);

    const std::vector<IeRuntimeNode>& nodes() const { return nodes_; }
    const IeRuntimeNode& node(int index) const;
    const std::vector<IeElement>& elements() const { return elements_; }
    IeElement& element_at(int index);
    const IeElement& element_at(int index) const;
    std::uint16_t version() const { return version_; }

private:
    int add_node(const IeNode& source, IeTextureService* textures, bool has_parent);
    IeRuntimeNode& node_at(int index);
    void update_node(int node, float dt);
    static void evaluate_keyed(IeRuntimeNode& n, IeRuntimeTrack& track, float t);

    std::vector<IeRuntimeNode> nodes_;
    std::vector<IeElement> elements_;
    std::vector<IeTextObject> texts_;
    std::vector<IeDrawEvent> draw_;
    std::vector<int> drawn_;
    std::vector<ImmediateText> immediate_;
    std::vector<ImmediateText> drawn_immediate_;
    std::array<float, 4> text_colour_{};
    std::uint16_t version_ = 0;
    std::uint32_t text_word_ = 0;
};

} // namespace omega
