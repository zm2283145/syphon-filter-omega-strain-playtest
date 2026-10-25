#include "ie_draw.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

namespace omega {
namespace {

[[noreturn]] void fail(std::uint32_t address, const std::string& what) {
    char a[16];
    std::snprintf(a, sizeof a, "%08x", address);
    throw std::runtime_error(std::string("IE draw ") + a + ": " + what);
}

using Mat = std::array<float, 16>;

// COP1 / VU0 lane operations: the product and the sum each rounded to
// binary32, the result flushed (ie_screen.cpp, tools/ee/ee.py).
float fmul(float a, float b) { return ie_ftz(a * b); }
float fadd(float a, float b) { return ie_ftz(a + b); }
float fsub(float a, float b) { return ie_ftz(a - b); }
float vmadd(float acc, float a, float b) {
    const float p = a * b;
    return ie_ftz(acc + p);
}

// vf4..7 = first, vf8..11 = second: row i = second[i].x * first0 +
// second[i].y * first1 + second[i].z * first2 + second[i].w * first3
// (vmulax / vmadday / vmaddaz / vmaddw), i.e. second x first.
Mat vu_product(const Mat& first, const Mat& second) {
    Mat r{};
    for (int i = 0; i < 4; ++i)
        for (int l = 0; l < 4; ++l) {
            float acc = fmul(first[static_cast<std::size_t>(l)], second[static_cast<std::size_t>(4 * i)]);
            acc = vmadd(acc, first[static_cast<std::size_t>(4 + l)], second[static_cast<std::size_t>(4 * i + 1)]);
            acc = vmadd(acc, first[static_cast<std::size_t>(8 + l)], second[static_cast<std::size_t>(4 * i + 2)]);
            r[static_cast<std::size_t>(4 * i + l)] =
                vmadd(acc, first[static_cast<std::size_t>(12 + l)], second[static_cast<std::size_t>(4 * i + 3)]);
        }
    return r;
}

constexpr float k_inv255 = std::bit_cast<float>(0x3b80807fu);

// 3ecd60 / 3ed2e0: f5 * (float)byte per channel.
std::array<float, 4> element_colour(const IeElement& el) {
    std::array<float, 4> c{};
    for (int k = 0; k < 4; ++k)
        c[static_cast<std::size_t>(k)] = fmul(k_inv255, static_cast<float>(el.colour[static_cast<std::size_t>(k)]));
    return c;
}

// The render mode both draws store: 4, | 1 and the texture bind with a
// slot (+18 >= 0), else | 0x200 for an opaque node (+63 == 0) whose
// opacity +ac is exactly 1.0 (read before the tracks are evaluated).
std::uint32_t node_mode(IeDrawState& st, const IeRuntimeNode& n, std::uint32_t at) {
    std::uint32_t mode = 4;
    if (!n.texture_slot) fail(at, "node '" + n.name + "' without a resolved texture slot (+18)");
    const std::int32_t slot = *n.texture_slot;
    if (slot >= 0) {
        // 003820e0(*00539248, slot): the record +58 filter from the old mode.
        const std::uint32_t old = st.mode;
        st.filter[slot] = (old & 4) ? 0x120u : (old & 8) ? 0x160u : 0x80u;
        st.bound_slot = slot;
        mode |= 1;
    } else {
        if (!n.translucent) fail(at, "node '" + n.name + "' without a resolved +63");
        if (*n.translucent == 0 && n.colour[3] == 1.0F) mode |= 0x200;
    }
    return mode;
}

// 3ed1b0(V, node +20) and the rotation test 3ed170 (vaddx.xyz rot + vf0.x;
// the MAC flag must read 0xe: x, y and z zero, no sign bit). A +61 node
// (003ed318..003ed358 / 003ed74c..003ed790) first replaces the pushed top
// with *0055c0c0 (003ed250) and switches the current camera record to
// perspective (003d3670 -> 003ed240: +106 = 1, +104 = 0); its own matrix is
// then skipped while byte 0055c100 is set.
void node_matrix(IeDrawState& st, const IeRuntimeNode& n) {
    if (n.flag_61) {
        st.top() = st.matrix_55c0c0;
        st.dirty = 1;
        st.record_106 = 1;
        st.record_104 = 0;
    }
    if (!(n.flag_61 && st.flag_55c100)) {
        st.top() = vu_product(st.top(), n.matrix);
        st.dirty = 1;
    }
    for (int k = 0; k < 3; ++k) {
        const float s = fadd(n.rot[static_cast<std::size_t>(k)], 0.0F);
        if (s != 0.0F || std::signbit(s)) fail(0x3ed040, "node '" + n.name + "' rotation (+b0..+b8)");
    }
}

// 003ed678 / 003ed7dc: a +61 node leaves the current camera record
// orthographic again (+106 = 1, +104 = 1) before the pop.
void restore_record(IeDrawState& st, const IeRuntimeNode& n) {
    if (!n.flag_61) return;
    st.record_106 = 1;
    st.record_104 = 1;
}

// 3e8800(element).
void element_matrix(IeDrawState& st, const IeScreen& screen, const IeElement& el) {
    st.push();
    Mat m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (int k = 0; k < 4; ++k) m[static_cast<std::size_t>(12 + k)] = el.words_20[static_cast<std::size_t>(k)];
    if (screen.node(el.node).flag_62) st.top() = vu_product(m, st.top());
    else st.top() = vu_product(st.top(), m);
    st.dirty = 1;
    static constexpr Mat identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    bool same = true;  // c.eq.s chained over +30..+6c
    for (int k = 0; k < 16 && same; ++k)
        same = el.matrix[static_cast<std::size_t>(k)] == identity[static_cast<std::size_t>(k)];
    if (!same) {
        st.top() = vu_product(st.top(), el.matrix);
        st.dirty = 1;
    }
}

// 003ee360(node, c): colour of every descendant reached through children
// without elements (the node's own colour is not set).
void colour_children(IeScreen& screen, int node, const std::array<float, 4>& c) {
    for (const int child : screen.node(node).children) {
        if (!screen.node(child).elements.empty()) continue;
        screen.node_mut(child).colour = c;  // 003ee5e0
        colour_children(screen, child, c);
    }
}

// 003ee510(node, t): 003f45a0 on every track, then the children without
// elements.
void evaluate_tracks(IeScreen& screen, int node, float t) {
    const auto tracks = static_cast<int>(screen.node(node).tracks.size());
    for (int i = 0; i < tracks; ++i) screen.evaluate_track(node, i, t);
    for (const int child : screen.node(node).children)
        if (screen.node(child).elements.empty()) evaluate_tracks(screen, child, t);
}

// The element body 3ecd60 / 3ed2e0 share once the mode is stored:
// 3e8800, 3ee360, 3ee510, 3ed880, the text part and 3e87d0.
void element_body(GuiDrawList& out, IeDrawState& st, IeScreen& screen, int node, const IeElement& el) {
    element_matrix(st, screen, el);
    const auto c1 = element_colour(el);
    colour_children(screen, node, c1);
    evaluate_tracks(screen, node, el.anim.t);
    const auto& n = screen.node(node);
    std::array<float, 4> c2{};
    for (int k = 0; k < 4; ++k) c2[static_cast<std::size_t>(k)] = fmul(n.colour[static_cast<std::size_t>(k)], c1[static_cast<std::size_t>(k)]);
    ie_draw_node(out, st, screen, node, c2);
    if (const auto& frame = screen.node(node).text) {
        const auto block = screen.text_colour_block();
        ie_draw_element_text(out, st, el.text >= 0 ? &screen.text_object(el.text) : nullptr, *frame, el.flag_19, block,
                             el.data_78);
    }
    st.pop();
}

// 003e8264..003e87b8: the fit transform around the data object's draw.
void element_data(GuiDrawList& out, IeDrawState& st, const IeTextFrame& frame, std::uint32_t data_78) {
    if (!st.element_data_bounds) fail(0x3e8254, "element data +78 (its vtable +88 bounds)");
    const IeDrawState::ElementBounds b = st.element_data_bounds(data_78);
    const auto& lo = b[0];
    const auto& hi = b[1];
    for (const auto& v : b)
        for (const float f : v)
            if ((std::bit_cast<std::uint32_t>(f) & 0x7f800000u) == 0x7f800000u)
                fail(0x3e8264, "exponent-255 bounds operand (the EE has no Inf / NaN)");
    if (!(lo[0] <= hi[0])) return;  // c.ole.s min.x, max.x; bc1f
    std::array<float, 4> c{}, e{};
    for (std::size_t k = 0; k < 4; ++k) {
        c[k] = fmul(fadd(lo[k], hi[k]), 0.5F);  // vadd, vmulx by 0.5
        e[k] = fsub(hi[k], lo[k]);              // vsub
    }
    const float fw = fsub(frame.p1[0], frame.p0[0]);
    const float fh = fsub(frame.p0[1], frame.p1[1]);
    if (e[0] == 0.0F || e[1] == 0.0F) fail(0x3e8398, "a zero bounds extent (the EE's div.s by zero)");
    const float sx = ie_ftz(fw / e[0]);
    const float sy = ie_ftz(fh / e[1]);
    const float s = sx < sy ? sx : sy;  // min.s
    st.push();
    std::array<float, 4> mid{};
    for (std::size_t k = 0; k < 4; ++k) mid[k] = fmul(fadd(frame.p0[k], frame.p1[k]), 0.5F);
    c[2] = fmul(c[2], -1.0F);
    Mat m = st.top();
    static constexpr std::array<float, 12> rows{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0};
    for (std::size_t k = 0; k < 12; ++k) m[k] = rows[k];
    for (std::size_t k = 0; k < 3; ++k) m[12 + k] = fadd(m[12 + k], mid[k]);  // vadd.xyz
    const float ns = -s;
    for (std::size_t k = 0; k < 3; ++k) m[12 + k] = fadd(m[12 + k], fmul(c[k], ns));
    st.top() = m;
    st.dirty = 1;
    const Mat scale{s, 0, 0, 0, 0, s, 0, 0, 0, 0, s, 0, 0, 0, 0, 1};
    st.top() = vu_product(st.top(), scale);
    st.dirty = 1;
    if (!st.element_data_draw) fail(0x3e8784, "element data +78 (its vtable +58 draw)");
    st.element_data_draw(out, st, data_78);
    st.pop();
}

}  // namespace

void ie_draw_element_text(GuiDrawList& out, IeDrawState& st, IeTextObject* obj, const IeTextFrame& frame,
                          std::uint8_t flag_19, const std::array<float, 4>& block, std::uint32_t data_78) {
    if (!obj) return;  // 003e81a0
    obj->port_anchor = st.port_anchor;  // port-only (132ws): the wide renderer's HUD element tag
    const Mat& m = st.top();
    for (const float f : {m[12], m[13], m[14], frame.p0[0], frame.p0[1]})
        if ((std::bit_cast<std::uint32_t>(f) & 0x7f800000u) == 0x7f800000u)
            fail(0x3e81c4, "exponent-255 operand (the EE has no Inf / NaN)");
    if (m[14] <= 0.0F) {  // c.ole.s top +38, 0 (a denormal compares unflushed)
        obj->x = 1000;
        obj->y = 1000;
    } else {
        obj->x = ie_cvt_w(fadd(m[12], frame.p0[0]));
        obj->y = ie_cvt_w(fadd(m[13], frame.p0[1]));
        if (flag_19) ie_text_colour(*obj, block);  // 003d0810(text, 0055a280)
    }
    if (data_78) element_data(out, st, frame, data_78);
}

std::array<float, 16>& IeDrawState::top() {
    if (depth < 0) fail(0x3ed290, "matrix stack depth below zero");
    if (static_cast<std::size_t>(depth) >= stack.size()) stack.resize(static_cast<std::size_t>(depth) + 1);
    return stack[static_cast<std::size_t>(depth)];
}

void IeDrawState::push() {  // 003ed290 / 003e8800's copy
    const Mat m = top();
    ++depth;
    top() = m;
}

void IeDrawState::pop() {  // 003e87d0
    dirty = 1;
    --depth;
}

int ie_draw_element(GuiDrawList& out, IeDrawState& st, IeScreen& screen, int node, int element, int a2) {
    const IeElement el = screen.element_at(element);
    if (!el.shown) return 1;  // 003ecd8c
    st.mode = node_mode(st, screen.node(node), 0x3ecd98);
    // a2 != 0 pushes the node's own matrix first; the decorator draw
    // 00417340 always passes 0.
    if (a2) fail(0x3ecd60, "a2 != 0 (node matrix pushed) is not reached by 00417340");
    element_body(out, st, screen, node, el);
    return 1;
}

void ie_draw_child(GuiDrawList& out, IeDrawState& st, IeScreen& screen, int node) {
    if (node == st.port_edge_band_node) {  // port-only (132ws9): tag the node's batches
        const std::size_t from = out.batches.size();
        st.port_edge_band_node = -1;
        ie_draw_child(out, st, screen, node);
        st.port_edge_band_node = node;
        for (std::size_t k = from; k < out.batches.size(); ++k) out.batches[k].port_edge_band = st.port_edge_band;
        return;
    }
    const auto& n = screen.node(node);
    if (n.has_parent && !n.elements.empty()) {
        st.push();
        node_matrix(st, n);
        // A copy of the element list (the body may touch the screen's nodes); port-only (132vf1): short lists stay on
        // the stack.
        const std::vector<int>& elements = n.elements;
        std::array<int, 32> small{};
        std::vector<int> large;
        std::span<const int> list;
        if (elements.size() <= small.size()) {
            std::copy(elements.begin(), elements.end(), small.begin());
            list = std::span<const int>(small.data(), elements.size());
        } else {
            large = elements;
            list = large;
        }
        for (const int e : list) {
            const IeElement el = screen.element_at(e);
            if (!el.shown) continue;
            st.mode = node_mode(st, screen.node(node), 0x3ed3e4);
            element_body(out, st, screen, node, el);
            // 003ed588..003ed634: each shown element moves the node's top
            // translation row by the NEXT offset (vadd.xyz), then V+a78 = 1.
            if (const auto& next = screen.node(node).next) {
                auto& m = st.top();
                for (std::size_t k = 0; k < 3; ++k) m[12 + k] = fadd(m[12 + k], (*next)[k]);
                st.dirty = 1;
            }
        }
        restore_record(st, screen.node(node));  // 003ed678
        st.dirty = 1;
        --st.depth;
        return;
    }
    if (!n.visible) return;
    st.mode = node_mode(st, n, 0x3ed6e4);
    st.push();
    node_matrix(st, n);
    ie_draw_node(out, st, screen, node, n.colour);
    restore_record(st, screen.node(node));  // 003ed7dc
    st.dirty = 1;
    --st.depth;
}

void ie_draw_node(GuiDrawList& out, IeDrawState& st, IeScreen& screen, int node, const std::array<float, 4>& c) {
    const IeRuntimeNode& n = screen.node(node);
    if (!n.triangles.empty()) {
        // The batch open (003ed8c0..003ed99c, 0025cd10).
        const bool textured = (st.mode & 1) != 0;
        GuiDrawBatch b = out.take();
        b.flush = 0x371200;
        b.prim = textured ? 3 : 2;
        b.strip_max = (textured ? 0x41u : 0x62u) & ~1u;
        b.strips.push_back(0);
        b.breaks.push_back({});
        b.breaks.back().fill(b.prim - 1);
        std::int32_t open = 0;     // V+1040
        std::int32_t nbreak = 0;   // V+1044
        // The scratch colours and uvs (0x70000000).
        // Port-only (132vf1): the scratch keeps its storage across nodes and frames (filled before the recursion).
        thread_local std::vector<std::array<float, 4>> col;
        thread_local std::vector<std::array<float, 2>> uv;
        col.resize(n.colours.size());
        uv.resize(n.uvs.size());
        b.vertices.reserve(n.triangles.size() * 3);
        for (std::size_t i = 0; i < n.colours.size(); ++i)
            for (int k = 0; k < 4; ++k)
                col[i][static_cast<std::size_t>(k)] =
                    fmul(fmul(k_inv255, static_cast<float>(n.colours[i][static_cast<std::size_t>(k)])), c[static_cast<std::size_t>(k)]);
        for (std::size_t i = 0; i < n.uvs.size(); ++i)
            for (int k = 0; k < 2; ++k) {
                const float x = fsub(fadd(n.uvs[i][static_cast<std::size_t>(k)], n.uv_off[static_cast<std::size_t>(k)]), 0.5F);
                uv[i][static_cast<std::size_t>(k)] = fadd(0.5F, fmul(n.uv_scl[static_cast<std::size_t>(k)], x));
            }
        for (const auto& tri : n.triangles) {
            for (int k = 0; k < 3; ++k) {
                const std::uint16_t pi = tri[static_cast<std::size_t>(k)];
                const std::uint16_t ui = tri[static_cast<std::size_t>(3 + k)];
                const std::uint16_t ci = tri[static_cast<std::size_t>(6 + k)];
                if (pi >= n.positions.size()) fail(0x3ed880, "position index past +70 in node '" + n.name + "'");
                if (ci >= col.size()) fail(0x3ed880, "colour index past the +68 scratch in node '" + n.name + "'");
                if (open == static_cast<std::int32_t>(b.strip_max)) fail(0x25ca40, "full strip");
                ++open;
                ++st.vertex_count;
                GuiVertex v;
                const auto& p = n.positions[pi];
                v.pos = {p[0], p[1], p[2], 1.0F};
                for (int q = 0; q < 4; ++q)
                    v.colour[static_cast<std::size_t>(q)] = ie_cvt_w(fmul(128.0F, col[ci][static_cast<std::size_t>(q)]));
                if (textured) {
                    if (ui >= uv.size()) fail(0x3ed880, "uv index past the +66 scratch in node '" + n.name + "'");
                    v.uv = {fadd(uv[ui][0], 2048.0F), fadd(uv[ui][1], 2048.0F), 1.0F, 0.0F};
                    st.v8 = std::uint64_t{6} << 32;
                } else {
                    st.v8 = std::uint64_t{2} << 32;
                }
                b.vertices.push_back(v);
                ++b.strips.back();
            }
            // 003ee128: the break word, or a new strip record after twelve.
            if (open > 0) {
                if (nbreak < 12) {
                    b.breaks.back()[static_cast<std::size_t>(nbreak)] =
                        static_cast<std::uint32_t>(open) * b.prim + b.prim - 1;
                    ++nbreak;
                } else {
                    b.strips.push_back(0);
                    b.breaks.push_back({});
                    b.breaks.back().fill(b.prim - 1);
                    open = 0;
                    nbreak = 0;
                }
            }
        }
        // 00371200's head closes the strip; it draws when the draw's first
        // strip has vertices and the mode word V+10e0 (*005392d0) has neither
        // 0x400 nor 0x1000 (never, as both draws store 4 | 1 or 4 | 0x200).
        if (b.strips.front() > 0 && !(st.mode & 0x1400)) {
            b.render_mode = st.mode;
            b.slot = static_cast<std::uint32_t>(st.bound_slot);
            b.v8 = st.v8;
            b.matrix = st.top();
            b.perspective = st.record_104 == 0;
            b.filter = textured ? st.filter.at(st.bound_slot) : 0;
            out.batches.push_back(std::move(b));
        }
    }
    // A copy of the child list (the draw may touch the screen's nodes); port-only (132vf1): short lists stay on the
    // stack.
    const std::vector<int>& kids = screen.node(node).children;
    std::array<int, 32> small{};
    std::vector<int> large;
    std::span<const int> children;
    if (kids.size() <= small.size()) {
        std::copy(kids.begin(), kids.end(), small.begin());
        children = std::span<const int>(small.data(), kids.size());
    } else {
        large = kids;
        children = large;
    }
    if (node == st.port_anchor_root && st.port_anchor == 0) {  // port-only (132ws): tag each HUD element's batches
        for (const int child : children) {
            const std::size_t from = out.batches.size();
            st.port_anchor = static_cast<std::uint32_t>(child) + 1;
            ie_draw_child(out, st, screen, child);
            for (std::size_t k = from; k < out.batches.size(); ++k) out.batches[k].port_anchor = st.port_anchor;
            st.port_anchor = 0;
        }
        return;
    }
    for (const int child : children) ie_draw_child(out, st, screen, child);
}

}  // namespace omega
