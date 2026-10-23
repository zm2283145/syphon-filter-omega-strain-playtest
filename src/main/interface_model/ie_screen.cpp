#include "ie_screen.hpp"
#include "game_libm.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace omega {
namespace {

[[noreturn]] void fail(const char* address, const std::string& what) {
    throw std::runtime_error(std::string("IE screen ") + address + ": " + what);
}

// One VU0 macro lane operation: the product rounded to binary32, the sum
// rounded to binary32, the result flushed (tools/ee/ee.py, VU_ROUNDING.md).
float vu_mul(float a, float b) { return ie_ftz(a * b); }
float vu_madd(float acc, float a, float b) {
    const float p = a * b;
    return ie_ftz(acc + p);
}

} // namespace

float ie_ftz(float v) {
    if (v != 0.0F && std::fabs(v) < std::numeric_limits<float>::min()) return std::copysign(0.0F, v);
    return v;
}

std::int32_t ie_cvt_w(float v) {
    const double d = v;
    if (std::isnan(d)) fail("cvt.w.s", "NaN conversion is not hosted");
    if (d >= 2147483647.0) return std::numeric_limits<std::int32_t>::max();
    if (d <= -2147483648.0) return std::numeric_limits<std::int32_t>::min();
    return static_cast<std::int32_t>(d);
}

IeScreen IeScreen::load(const IeFile& file, std::uint32_t text_word, IeTextureService* textures) {
    IeScreen screen;
    screen.version_ = file.version;  // 003e7b68: the global 0055c0b8
    screen.text_word_ = text_word;
    screen.add_node(file.root, textures, false);
    screen.nodes_[0].visible = 1;    // 003e7c08
    if (screen.find_child(0, "preview") >= 0)
        fail("003e7c24", "the preview element (003ef010, 003ee740, 003f4510) is not hosted");
    return screen;
}

std::uint8_t ie_colour_byte(float component) {
    constexpr float two31 = 2147483648.0F;  // 4f000000
    const float x = ie_ftz(255.0F * component);  // 437f0000
    if (two31 <= x) return static_cast<std::uint8_t>(ie_cvt_w(ie_ftz(x - two31)));  // | 80000000, low byte
    return static_cast<std::uint8_t>(ie_cvt_w(x));
}

int IeScreen::add_node(const IeNode& source, IeTextureService* textures, bool has_parent) {  // 003ef660
    const int index = static_cast<int>(nodes_.size());
    nodes_.emplace_back();
    {
        auto& n = nodes_.back();
        n.name = source.name;
        n.has_parent = has_parent;  // +98 = a3
        n.texture_slot = -1;  // 003ef6d0
        if (!source.texture.empty()) {
            // 003ef7e4 / 003ef864: the name and ".TDX" (004be048) are cut at 0x7e with '\n'.
            if (source.texture.size() + 4 >= 0x7e) fail("003ef864", "a truncated texture name is not hosted");
            n.texture = source.texture + ".TDX";
            if (textures != nullptr) n.texture_slot = textures->load(n.texture);  // 003ef8bc
            else n.texture_slot.reset();
        }
        for (const auto& c : source.colors)  // 003efdf0..003efefc
            n.colours.push_back({ie_colour_byte(c[0]), ie_colour_byte(c[1]), ie_colour_byte(c[2]), ie_colour_byte(c[3])});
        for (int row = 0; row < 4; ++row) {
            for (int k = 0; k < 3; ++k) n.matrix[4 * row + k] = source.matrix[3 * row + k];
            n.matrix[4 * row + 3] = row == 3 ? 1.0F : 0.0F;
        }
        n.positions = source.positions;
        for (const auto& t : source.tracks) {
            IeRuntimeTrack track;
            track.kind = t.kind;
            if (t.kind == IeTrackKind::vertex) {
                // 003f0d28..003f0d98: sub-track j is bound to node +70 + 12 * j.
                if (t.vertex.size() > n.positions.size())
                    fail("003f0d98", "a VERTEX sub-track past the node's positions (out-of-bounds binding) is not hosted");
                for (std::size_t j = 0; j < t.vertex.size(); ++j) {
                    IeVertexSubTrack sub;
                    sub.position = static_cast<std::uint32_t>(j);
                    sub.keys = t.vertex[j];
                    track.subs.push_back(std::move(sub));
                }
            }
            else if (t.kind == IeTrackKind::diffuse)
                track.diffuse = t.diffuse;  // 003f0bd0: target +a0
            else
                track.scalar = t.scalar;    // 003f0bd0: the kind's scalar target
            n.tracks.push_back(std::move(track));
        }
        n.uvs = source.uvs;  // 003f01ec: +74, 8 bytes per {u, v}
        for (const auto& tri : source.triangles) {  // 003eff98..003effdc: sh of each file word
            std::array<std::uint16_t, 9> t{};
            for (std::size_t k = 0; k < 9; ++k) t[k] = static_cast<std::uint16_t>(tri[k]);
            n.triangles.push_back(t);
        }
        for (const auto& r : source.text) {  // 003f0770
            if (r.next) {                    // +d4: read by the element draw 003ed588
                const auto& p = r.points.at(0);
                n.next = std::array<float, 3>{p[0], p[2], p[1]};
                continue;
            }
            IeTextFrame frame;
            const auto& a = r.points.at(0);
            frame.p0 = {a[0], a[2], a[1], 1.0F};
            if (static_cast<std::int32_t>(r.declared) >= 2) {
                const auto& b = r.points.at(1);
                frame.p1 = {b[0], b[2], b[1], 1.0F};
                // 003f09a0..003f09ec: vsub, vmul.xyz, the x-lane ACC sum.
                const float dx = ie_ftz(frame.p1[0] - frame.p0[0]);
                const float dy = ie_ftz(frame.p1[1] - frame.p0[1]);
                const float dz = ie_ftz(frame.p1[2] - frame.p0[2]);
                const float sx = vu_mul(dx, dx), sy = vu_mul(dy, dy), sz = vu_mul(dz, dz);
                float acc = vu_mul(1.0F, sx);
                acc = vu_madd(acc, 1.0F, sy);
                const float length = vu_madd(acc, 1.0F, sz);
                if (length == 0.0F) frame.p1 = {};
            }
            n.text = frame;
        }
    }
    std::vector<int> children;
    for (const auto& child : source.children) children.push_back(add_node(child, textures, true));
    auto& n = nodes_[static_cast<std::size_t>(index)];
    n.children = std::move(children);
    // 003f03dc..003f0454: a slot whose record +48 has 0x100 set, else any
    // colour whose alpha byte is not ff, sets +63.
    if (n.texture_slot.has_value()) {
        std::uint8_t t = 0;
        if (*n.texture_slot >= 0 && !textures->record_flags_known()) return index;  // +63 unresolved
        if (*n.texture_slot >= 0 && (textures->record_flags(*n.texture_slot) & 0x100U)) t = 1;
        else
            for (const auto& c : n.colours)
                if (c[3] != 0xff) { t = 1; break; }
        n.translucent = t;
    }
    return index;
}

IeRuntimeNode& IeScreen::node_at(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= nodes_.size()) fail("node", "null node");
    return nodes_[static_cast<std::size_t>(index)];
}

const IeRuntimeNode& IeScreen::node(int index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= nodes_.size()) fail("node", "null node");
    return nodes_[static_cast<std::size_t>(index)];
}

IeElement& IeScreen::element_at(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= elements_.size()) fail("element", "null element");
    auto& el = elements_[static_cast<std::size_t>(index)];
    if (el.freed) fail("003eee70", "use of an element its node removed and freed");
    return el;
}

const IeElement& IeScreen::element_at(int index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= elements_.size()) fail("element", "null element");
    return elements_[static_cast<std::size_t>(index)];
}

IeTextObject& IeScreen::text_object(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= texts_.size()) fail("003d0940", "null text object");
    auto& obj = texts_[static_cast<std::size_t>(index)];
    if (obj.freed) fail("003e8ec0", "use of a text object its element's destructor freed");
    return obj;
}

void IeScreen::set_text(int text, std::string value) { ie_text_set(text_object(text), std::move(value)); }

void IeScreen::queue_immediate(std::string text, std::uint32_t font, const std::array<float, 4>& colour,
                               std::int32_t x, std::int32_t y) {
    immediate_.push_back({std::move(text), font, colour, static_cast<float>(x), static_cast<float>(y)});
}

void IeScreen::draw_texts(std::int32_t now_ms, IeTextWidth width, void* context) {
    drawn_immediate_ = std::move(immediate_);
    immediate_.clear();
    for (const int text : drawn_) ie_text_draw_state(text_object(text), now_ms, width, context);
}

void ie_text_set(IeTextObject& obj, std::string text) {
    obj.text = std::move(text);  // 0013b480 / 0013d680 / 00138b70 on +14
    obj.f44 = 0.0F;
    obj.f40 = static_cast<float>(-obj.w);
    obj.f4c = 0.0F;
    obj.byte_59 = 0;
    obj.byte_5a = 0;
}

void ie_text_colour(IeTextObject& obj, const std::array<float, 4>& rgba) {
    if (obj.byte_59 == 0) obj.colour = rgba;
}

void ie_text_fade(IeTextObject& obj, float target, float seconds) {
    if (seconds == 0.0F) {
        obj.colour[3] = target;
        obj.byte_59 = 0;
        return;
    }
    obj.f50 = target;
    const float ms = ie_ftz(1000.0F * seconds);
    obj.f4c = ie_ftz(ie_ftz(target - obj.colour[3]) / ms);
    obj.byte_59 = 1;
}

std::int32_t ie_text_clock_ms(std::uint32_t t0_count, std::uint32_t t0_base, std::int64_t ticks) {
    const std::uint32_t count = t0_count + t0_base;
    float f = 0.0F;
    if (static_cast<std::int32_t>(count) >= 0) {
        f = static_cast<float>(static_cast<std::int32_t>(count));
    } else {  // 003cfbec: halve with the low bit folded in, convert, double
        f = static_cast<float>(static_cast<std::int32_t>((count >> 1) | (count & 1)));
        f = f + f;
    }
    const std::int32_t step = ie_cvt_w(ie_ftz(f / 5.76F));
    std::int64_t rest = ticks + step;
    std::int64_t divisor = std::int64_t{100} << 30;
    std::int32_t bit = 0x40000000, q = 0;
    for (int k = 0; k < 31; ++k) {
        const std::int64_t d = static_cast<std::int64_t>(static_cast<std::uint64_t>(rest) - static_cast<std::uint64_t>(divisor));
        if (d >= 0) {
            rest = d;
            q = static_cast<std::int32_t>(static_cast<std::uint32_t>(q) + static_cast<std::uint32_t>(bit));
        }
        divisor >>= 1;
        bit >>= 1;
    }
    return q;
}

void ie_text_draw_state(IeTextObject& obj, std::int32_t now_ms, IeTextWidth width, void* context) {
    if (obj.word_20 == 0) return;  // 003cfaf8
    const float now = static_cast<float>(now_ms);
    if (obj.f44 == 0.0F) obj.f44 = now;
    const float old = obj.f44;
    obj.f44 = now;
    const float delta = ie_ftz(now - old);
    if (obj.byte_59) {
        obj.colour[3] = ie_ftz(obj.colour[3] + ie_ftz(obj.f4c * delta));
        const bool reached = ie_ftz(obj.f50 - obj.colour[3]) <= 0.0F;
        const bool falling = obj.f4c <= 0.0F;
        if (reached != falling) {
            obj.colour[3] = obj.f50;
            obj.byte_59 = 0;
        }
    }
    if (obj.byte_10 != 3) return;
    if (!width) fail("0036aa50", "the scroll-mode text width (font service) is not hosted");
    const std::int32_t w = width(obj, context);
    if (delta <= obj.f48) return;
    obj.f40 = ie_ftz(obj.f40 + ie_ftz(delta / obj.f48));
    if (!(obj.f40 <= static_cast<float>(w))) obj.byte_5a = 1;
}

int IeScreen::find_child(int node_index, std::string_view name) const {
    for (const int child : node(node_index).children)
        if (nodes_[static_cast<std::size_t>(child)].name == name) return child;
    return -1;
}

void IeScreen::set_visible(int node_index, std::uint8_t v, bool recurse) {
    auto& n = node_at(node_index);
    n.visible = v;
    if (!recurse) return;
    for (const int child : n.children) set_visible(child, v, true);
}

void IeScreen::set_flag_61(int node_index, std::uint8_t v) { node_at(node_index).flag_61 = v; }
void IeScreen::set_flag_62(int node_index, std::uint8_t v) { node_at(node_index).flag_62 = v; }

int IeScreen::create_element(int node_index) {  // 003ef010
    set_visible(node_index, 1, true);
    IeElement el;  // 003e9000
    el.node = node_index;
    const auto& n = node(node_index);
    if (n.text) {
        const auto& f = *n.text;
        float w = 1000.0F, h = 1000.0F;
        if (!(f.p1[3] <= 0.0F)) {
            w = ie_ftz(f.p1[0] - f.p0[0]);
            h = ie_ftz(f.p0[1] - f.p1[1]);
        }
        IeTextObject obj;
        obj.x = ie_cvt_w(f.p0[0]);
        obj.y = ie_cvt_w(f.p0[1]);
        obj.w = ie_cvt_w(w);
        obj.h = ie_cvt_w(h);
        obj.t1 = 0;
        obj.t2 = ie_text_colour_block;
        obj.word_20 = text_word_;
        obj.colour = text_colour_;                                  // 003d09fc..003d0a28
        obj.f40 = static_cast<float>(-obj.w);                       // 003d0a2c..003d0a40
        obj.f48 = static_cast<float>(ie_text_scroll_period);        // 003d0a48..003d0a50
        el.text = static_cast<int>(texts_.size());
        texts_.push_back(std::move(obj));
        draw_.push_back({IeDrawOp::add, el.text});
        drawn_.push_back(el.text);
    }
    // 003e91e0..003e9360: 255 * c, through the 2^31 split, low byte.
    for (int k = 0; k < 4; ++k) {
        const float f = ie_ftz(255.0F * n.colour[static_cast<std::size_t>(k)]);
        std::uint32_t v = 0;
        if (2147483648.0F <= f) v = static_cast<std::uint32_t>(ie_cvt_w(ie_ftz(f - 2147483648.0F))) | 0x80000000u;
        else v = static_cast<std::uint32_t>(ie_cvt_w(f));
        el.colour[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(v);
    }
    const int index = static_cast<int>(elements_.size());
    elements_.push_back(el);
    node_at(node_index).elements.push_back(index);  // 003ef0f0: 00138ea0 before the end; 003ee610 walks +4 from +8
    return index;
}

int IeScreen::find_element(int node_index, std::string_view name) {  // 00260d50
    const int child = find_child(node_index, name);
    return child < 0 ? -1 : create_element(child);
}

void IeScreen::show_element(int element, std::uint8_t v) {  // 003e8de0
    auto& el = element_at(element);
    if (v == el.shown) return;
    if (el.shown) {
        if (el.text >= 0) {
            draw_.push_back({IeDrawOp::remove, el.text});
            std::erase(drawn_, el.text);  // 00138ab0: std::list::remove
        }
        el.shown = 0;
    } else {
        if (el.text >= 0) {
            draw_.push_back({IeDrawOp::add, el.text});
            drawn_.push_back(el.text);    // 003e8e80 -> 00138ea0 at the end
        }
        el.shown = 1;
    }
}

void ie_anim_evaluate(IeAnim& a, float t) {  // 003f45a0 (virtual +10: the element no-op 003f43c0)
    a.t = t;
    a.rate = 0.0F;
}

void ie_anim_fade(IeAnim& a, float target) {  // 003f44e0
    a.lo = target;
    a.hi = target;
    a.rate = a.t <= target ? 1.0F : -1.0F;
    a.loop = 0;
}

void IeScreen::evaluate_element(int element, float t) { ie_anim_evaluate(element_at(element).anim, t); }

void IeScreen::fade_element(int element, float target) { ie_anim_fade(element_at(element).anim, target); }

void ie_element_rotate(IeElement& el, const std::array<float, 3>& v) {  // 003e8070
    // cosf 00121688 (00140e80) / sinf 00121928 (00140e70) of y, z, x in that order.
    const float cy = game_cosf(v[1]), sy = game_sinf(v[1]);
    const float cz = game_cosf(v[2]), sz = game_sinf(v[2]);
    const float cx = game_cosf(v[0]), sx = game_sinf(v[0]);
    const auto mul = [](float a, float b) { return ie_ftz(a * b); };
    const auto add = [](float a, float b) { return ie_ftz(a + b); };
    const auto sub = [](float a, float b) { return ie_ftz(a - b); };
    const float cxcy = mul(cx, cy);                                       // 003e80e4
    const float m1 = add(mul(sz, cxcy), mul(sx, sy));                     // 003e80f4
    const float acc10 = mul(sz, mul(sx, sy));                             // mula 003e80f8
    const float p10 = cx * cy;                                            // madd 003e80fc: the product unflushed
    const float m10 = ie_ftz(acc10 + p10);
    const float cxsy = mul(cx, sy);                                       // 003e8100
    const float acc9 = mul(sz, cxsy);                                     // mula 003e8108
    const float p9 = sx * cy;                                             // msub 003e8114
    const float m9 = ie_ftz(acc9 - p9);
    const float m2 = sub(mul(cy, mul(sx, sz)), cxsy);                     // 003e8128
    // 00140e20: rows (f12, f15, f18, 0), (f13, f16, f19, 0), (f14, f17, sp+0, 0), (0, 0, 0, 1).
    el.matrix = {mul(cy, cz), m1, m2, 0.0F,
                 ie_ftz(-sz), mul(cx, cz), mul(sx, cz), 0.0F,
                 mul(sy, cz), m9, m10, 0.0F,
                 0.0F, 0.0F, 0.0F, 1.0F};
}

void ie_anim_update(IeAnim& a, float dt) {  // 003f43d0
    if (a.rate == 0.0F) return;
    a.t = ie_ftz(a.t + ie_ftz(a.rate * dt));
    if (!(a.rate <= 0.0F)) {
        if (a.t <= a.hi) return;
        if (a.loop) {
            a.rate = -a.rate;
            a.t = a.hi;
        } else if (a.lo == a.hi) {
            a.rate = 0.0F;
            a.t = a.hi;
        } else {
            a.t = a.lo;  // 003f4460: wraps to the start
        }
    } else {
        if (!(a.t < a.lo)) return;
        if (a.loop) {
            a.rate = -a.rate;
            a.t = a.lo;
        } else if (a.lo == a.hi) {
            a.rate = 0.0F;
            a.t = a.lo;
        } else {
            a.t = a.hi;  // 003f44b8: wraps to the end
        }
    }
    // virtual +10(t): the element's is the no-op 003f43c0.
}

void IeScreen::update_node(int node_index, float dt) {  // 003ee610
    const auto& n = node(node_index);
    if (!n.visible) return;
    for (const int e : n.elements) ie_anim_update(element_at(e).anim, dt);
    for (const int child : n.children) update_node(child, dt);
}

void IeScreen::update(float dt) { update_node(root(), ie_ftz(30.0F * dt)); }  // 003e79c0

void IeScreen::remove_element(int element) {  // 003e8eb0 -> 003eee70(element +70, element)
    auto& el = element_at(element);
    auto& n = node_at(el.node);
    const auto count = std::count(n.elements.begin(), n.elements.end(), element);
    for (std::ptrdiff_t k = 0; k < count; ++k) {
        // 003e8ec0(el, -1): with a text object, 00138ab0 removes it from the
        // draw list (whether shown or not), its string (+14) is destroyed
        // (00138b70) and it is freed; vtable 004e0800; 0036b130 frees el.
        if (el.text >= 0) {
            auto& obj = text_object(el.text);
            draw_.push_back({IeDrawOp::remove, el.text});
            std::erase(drawn_, el.text);
            obj.freed = true;
        }
        el.freed = true;
    }
    std::erase(n.elements, element);  // 0015af40 / 00138da0 per entry
    if (n.elements.empty()) n.visible = 0;  // 003eefc8: the list count 0 hides the node
}

void IeScreen::evaluate_track(int node_index, int track_index, float t) {
    auto& n = node_at(node_index);
    auto& track = n.tracks.at(static_cast<std::size_t>(track_index));
    track.anim.t = t;  // 003f45a0: +4 = t, +10 = 0, then vtable +10
    track.anim.rate = 0.0F;
    if (track.kind != IeTrackKind::vertex) {
        evaluate_keyed(n, track, t);
        return;
    }
    for (auto& sub : track.subs) {
        sub.anim.t = t;
        sub.anim.rate = 0.0F;
        // 003f3ff0
        const auto count = static_cast<std::int32_t>(sub.keys.size());
        if (count <= 0) continue;
        auto& pos = n.positions[sub.position];
        const auto& k = sub.keys;
        if (t <= k[0][0]) {
            pos = {k[0][1], k[0][2], k[0][3]};
            sub.index = 0;
            continue;
        }
        if (!(t < k[static_cast<std::size_t>(count - 1)][0])) {
            const auto& last = k[static_cast<std::size_t>(count - 1)];
            pos = {last[1], last[2], last[3]};
            sub.index = count - 1;
            continue;
        }
        const auto key = [&](std::int32_t i) -> const std::array<float, 4>& {
            if (i < 0 || i >= count) fail("003f3ff0", "the key walk leaves the key array (keys not ascending)");
            return k[static_cast<std::size_t>(i)];
        };
        std::int32_t i = sub.index;
        if (t < key(i)[0]) {
            do --i;
            while (t < key(i)[0]);
        }
        while (!(t <= key(i + 1)[0])) ++i;
        sub.index = i;
        const auto& a = key(i);
        const auto& b = key(i + 1);
        const float f10 = ie_ftz(b[0] - t);
        const float f9 = ie_ftz(t - a[0]);
        const float span = ie_ftz(b[0] - a[0]);
        // 003f4144 div.s: a zero divisor gives the signed largest float.
        const float f8 = span == 0.0F ? std::copysign(std::numeric_limits<float>::max(), span) : ie_ftz(1.0F / span);
        const float y = ie_ftz(ie_ftz(b[2] * f9) + ie_ftz(a[2] * f10));
        const float x = ie_ftz(ie_ftz(b[1] * f9) + ie_ftz(a[1] * f10));
        const float acc = ie_ftz(a[3] * f10);  // mula.s
        const float pz = b[3] * f9;
        const float z = ie_ftz(acc + pz);      // madd.s
        pos = {ie_ftz(x * f8), ie_ftz(y * f8), ie_ftz(z * f8)};
    }
}

namespace {

// The key walk 003f3e70 / 003f41c0 share with 003f3ff0: from the cached
// index down while t < key.t, then up while !(t <= next.t).
template <class Keys>
std::int32_t ie_key_walk(const Keys& k, std::int32_t i, float t, const char* address) {
    const auto count = static_cast<std::int32_t>(k.size());
    const auto key_t = [&](std::int32_t j) {
        if (j < 0 || j >= count) fail(address, "the key walk leaves the key array (keys not ascending)");
        return k[static_cast<std::size_t>(j)][0];
    };
    if (t < key_t(i)) {
        do --i;
        while (t < key_t(i));
    }
    while (!(t <= key_t(i + 1))) ++i;
    return i;
}

float ie_key_inverse(float span) {  // div.s: a zero divisor gives the signed largest float
    return span == 0.0F ? std::copysign(std::numeric_limits<float>::max(), span) : ie_ftz(1.0F / span);
}

} // namespace

void IeScreen::evaluate_keyed(IeRuntimeNode& n, IeRuntimeTrack& track, float t) {
    if (track.kind == IeTrackKind::diffuse) {  // 003f41c0, target +a0..+a8
        const auto& k = track.diffuse;
        const auto count = static_cast<std::int32_t>(k.size());
        if (count <= 0) return;
        const std::array<float, 4>* v = nullptr;
        if (t <= k[0][0]) {
            v = &k[0];
            track.index = 0;
        } else if (!(t < k[static_cast<std::size_t>(count - 1)][0])) {
            v = &k[static_cast<std::size_t>(count - 1)];
            track.index = count - 1;
        }
        if (v) {
            n.colour[0] = (*v)[1];
            n.colour[1] = (*v)[2];
            n.colour[2] = (*v)[3];
            return;
        }
        const std::int32_t i = ie_key_walk(k, track.index, t, "003f41c0");
        track.index = i;
        const auto& a = k[static_cast<std::size_t>(i)];
        const auto& b = k[static_cast<std::size_t>(i + 1)];
        const float f7 = ie_ftz(t - a[0]);
        const float f8 = ie_ftz(b[0] - t);
        const float f5 = ie_key_inverse(ie_ftz(b[0] - a[0]));
        for (std::size_t c = 0; c < 3; ++c) {
            const float acc = vu_mul(b[c + 1], f7);  // mula.s
            const float val = vu_madd(acc, a[c + 1], f8);  // madd.s
            n.colour[c] = ie_ftz(val * f5);
        }
        return;
    }
    float* target = nullptr;  // 003f0bd0's target per kind
    switch (track.kind) {
    case IeTrackKind::opacity: target = &n.colour[3]; break;
    case IeTrackKind::rot_x: target = &n.rot[0]; break;
    case IeTrackKind::rot_y: target = &n.rot[1]; break;
    case IeTrackKind::rot_z: target = &n.rot[2]; break;
    case IeTrackKind::uv_off_u: target = &n.uv_off[0]; break;
    case IeTrackKind::uv_off_v: target = &n.uv_off[1]; break;
    case IeTrackKind::uv_scl_u: target = &n.uv_scl[0]; break;
    case IeTrackKind::uv_scl_v: target = &n.uv_scl[1]; break;
    default: fail("003f0bd0", "unknown track kind");
    }
    const auto& k = track.scalar;  // 003f3e70
    const auto count = static_cast<std::int32_t>(k.size());
    if (count <= 0) return;
    if (t <= k[0][0]) {
        *target = k[0][1];
        track.index = 0;
        return;
    }
    if (!(t < k[static_cast<std::size_t>(count - 1)][0])) {
        *target = k[static_cast<std::size_t>(count - 1)][1];
        track.index = count - 1;
        return;
    }
    const std::int32_t i = ie_key_walk(k, track.index, t, "003f3e70");
    track.index = i;
    const auto& a = k[static_cast<std::size_t>(i)];
    const auto& b = k[static_cast<std::size_t>(i + 1)];
    const float f3 = ie_ftz(t - a[0]);
    const float f2 = ie_ftz(b[0] - t);
    const float acc = vu_mul(b[1], f3);
    const float val = vu_madd(acc, a[1], f2);
    *target = ie_ftz(ie_key_inverse(ie_ftz(b[0] - a[0])) * val);
}

int IeScreen::find_node(int node_index, std::string_view name) const {  // 003ee840
    const auto& n = node(node_index);
    if (n.name == name) return node_index;
    for (const int child : n.children)
        if (const int r = find_node(child, name); r >= 0) return r;
    return -1;
}

void ie_anim_range(IeAnim& a, float x, float y, std::uint8_t loop) {  // 003f4510
    float lo = x, hi = y;
    if (hi < lo) std::swap(lo, hi);  // c.olt.s f13, f12
    a.lo = lo;
    a.hi = hi;
    if (a.t < lo) a.rate = 1.0F;
    else if (!(a.t <= hi)) a.rate = -1.0F;
    else if (a.rate != 1.0F && a.rate != -1.0F) a.rate = 1.0F;
    a.loop = loop;
}

std::array<float, 4> IeScreen::rect(int node_index, float t) {  // 003ecb50
    if (!(t < 0.0F)) {
        const auto count = node(node_index).tracks.size();
        for (std::size_t i = 0; i < count; ++i) evaluate_track(node_index, static_cast<int>(i), t);
    }
    const auto& n = node(node_index);
    float minx = 10000.0F, maxx = -10000.0F, minz = 10000.0F, maxz = -10000.0F;
    for (const auto& p : n.positions) {
        const std::array<float, 4> v{p[0], p[1], p[2], 1.0F};
        std::array<float, 4> out{};
        for (int lane = 0; lane < 4; ++lane) {
            float acc = vu_mul(n.matrix[static_cast<std::size_t>(lane)], v[0]);
            acc = vu_madd(acc, n.matrix[static_cast<std::size_t>(4 + lane)], v[1]);
            acc = vu_madd(acc, n.matrix[static_cast<std::size_t>(8 + lane)], v[2]);
            out[static_cast<std::size_t>(lane)] = vu_madd(acc, n.matrix[static_cast<std::size_t>(12 + lane)], v[3]);
        }
        if (out[0] < minx) minx = out[0];
        if (!(out[0] <= maxx)) maxx = out[0];
        if (out[2] < minz) minz = out[2];
        if (!(out[2] <= maxz)) maxz = out[2];
    }
    if (minx <= maxx && minz <= maxz) return {minx, minz, ie_ftz(maxx - minx), ie_ftz(maxz - minz)};
    return {0.0F, 0.0F, 0.0F, 0.0F};
}

} // namespace omega
