#pragma once
// Checkpoint 122b: the IE node draw (interface_model.cc / interface_element.cc),
// from an element's draw down to the triangle batches the renderer flush
// 00371200 receives.
//
//   003ecd60  element draw (node, element, a2): mode, matrix, colours, tracks
//   003ed2e0  child draw: per element (+98 and a non-empty +8c list), or the
//             node alone when visible (+60)
//   003ed880  node triangles (+7c, count +6a) into V = 005381f0's batch, then
//             the flush 00371200, then the children (+84) through 003ed2e0
//   003ee360  colour of the element-less descendants, 003ee510 their tracks
//   003e8800  element matrix push, 003e87d0 pop, 003ed290 push, 003ed1b0
//             multiply, 003ed170 rotation test
//   003820e0  texture bind (*00539250, the record's +58 filter bits)
//   0025cd10 / 0025ca40 / 0025c9d0 / 0025f220  batch strips and vertices
//
// The primitive-list boundary is 00371200 past its head (VIF / GIF packet
// build and the VU1 program): ie_draw returns the batches it would receive.
// Paths the IE screens on the disc do not reach throw naming their address.

#include "ie_screen.hpp"
#include "../gui/gui_draw.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace omega {

// The renderer state the draw reads and writes (V = 005381f0).
struct IeDrawState {
    // V+a80 + 64 * depth (row-major; the VU products 003ed1b0 / 003e8800).
    std::vector<std::array<float, 16>> stack{std::array<float, 16>{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
    std::int32_t depth = 0;          // V+a74
    std::uint8_t dirty = 0;          // V+a78
    // *005392d0 (= V+10e0: the flush head skips the draw on 0x400 / 0x1000).
    std::uint32_t mode = 0;
    std::int32_t bound_slot = 0;     // *00539250
    std::uint64_t v8 = 0;            // V+8
    std::uint32_t vertex_count = 0;  // V+102c (0025ca40 counts every vertex)
    // 003820e0: the record +58 filter bits per slot (0x120 when the old mode
    // has 4, else 0x160 with 8, else 0x80).
    std::map<std::int32_t, std::uint32_t> filter;
    // 003e8190's element data +78 part (003e8248..003e87b8): the data
    // object's vtable +88 (its bounds: min xyzw at +0, max xyzw at +10) and
    // +58 (its draw, under the fit transform). Unset, ie_draw_element_text
    // throws naming 003e8254 (bounds) / 003e8784 (draw).
    using ElementBounds = std::array<std::array<float, 4>, 2>;
    std::function<ElementBounds(std::uint32_t data_78)> element_data_bounds;
    std::function<void(GuiDrawList& out, IeDrawState& st, std::uint32_t data_78)> element_data_draw;
    // Checkpoint 132r: a node with +61 (target_lock / target_lockswitch) draws
    // under the current 3D camera record (003ed2e0 at 003ed314 / 003ed74c):
    // 003ed250 copies *0055c0c0 over the pushed top, 003d3670 -> 003ed240
    // set the current record's +106 = 1 and +104 = 0 (perspective), the node
    // matrix is skipped while byte 0055c100 is set, and 003ed678 / 003ed7dc
    // set +106 = +104 = 1 again. *0055c0c0 is the identity from the static
    // constructor 004d6a10 (ctor list 004d8f50); its only other writer,
    // 0026ca70 (the vtable 004e18f0 draw through 00464a90), sets it with
    // 0055c100 = 1 around its own 003e7670 and restores the identity and 0.
    std::array<float, 16> matrix_55c0c0{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::uint8_t flag_55c100 = 0;
    // The current camera record (V+1f0 + 0x110 * V+a70): +104 (orthographic;
    // 1 under 003e7670 and the front-end canvas) and +106 (dirty). A batch
    // flushed with +104 = 0 is GuiDrawBatch::perspective.
    std::uint8_t record_104 = 1;
    std::uint8_t record_106 = 0;
    // Port-only (checkpoint 132ws): the wide renderer's HUD element tags. With port_anchor_root set (the HUD screen's
    // root, by hud_screen_draw), ie_draw_node gives the batches each child of that node draws GuiDrawBatch::port_anchor
    // child + 1, and ie_draw_element_text copies the current tag to the text object it places. No effect on the
    // draw's batches otherwise; -1 (the default) tags nothing.
    int port_anchor_root = -1;
    std::uint32_t port_anchor = 0;

    std::array<float, 16>& top();
    void push();  // 003ed290
    void pop();   // 003e87d0
};

// 003ecd60(node, element, 0): 1 when the element is shown (+18), else
// nothing is drawn and the result is still 1. a2 != 0 (the node's own
// matrix pushed first) is not reached by the decorator draw 00417340 and
// throws naming 003ecdfc.
int ie_draw_element(GuiDrawList& out, IeDrawState& st, IeScreen& screen, int node, int element, int a2 = 0);
// 003ed2e0(node).
void ie_draw_child(GuiDrawList& out, IeDrawState& st, IeScreen& screen, int node);
// 003e8190(element), called by both element draws after the node triangles
// when node +d0 (the TEXT frame) is set (003ecf94): nothing without a text
// object (+74); else the text object's +0 / +4 become cvt.w(top +30 + frame
// +0) / cvt.w(top +34 + frame +4) when the stack top's +38 is above zero
// (else both 1000), and with +19 set 003d0810 copies the colour block
// *0055a280 (Checkpoint 121k). Checkpoint 132h: then a non-zero element
// data +78 (003e8248..003e87b8): b = its bounds
// (vtable +88); unless b.min.x <= b.max.x, nothing. Else c = (min + max) *
// 0.5, e = max - min, s = min((frame +10 - frame +0) / e.x, (frame +4 -
// frame +14) / e.y); a push; the top becomes rows (1, 0, 0, 0), (0, 1, 0,
// 0), (0, 0, -1, 0) and the old top's row 3 with xyz += (frame +0 + frame
// +10) * 0.5 and += -s * (c.x, c.y, -c.z); then top = diag(s, s, s, 1) x
// top (rows 0..2 scaled); the draw (vtable +58); the pop. A zero extent
// (the EE's division by zero) throws naming 003e8398.
void ie_draw_element_text(GuiDrawList& out, IeDrawState& st, IeTextObject* obj, const IeTextFrame& frame,
                          std::uint8_t flag_19, const std::array<float, 4>& block, std::uint32_t data_78);
// 003ed880(node, colour).
void ie_draw_node(GuiDrawList& out, IeDrawState& st, IeScreen& screen, int node, const std::array<float, 4>& colour);

}  // namespace omega
