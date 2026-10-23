#pragma once
// Checkpoint 121k: what decides whether (and how opaque) the man model draw
// 003870c0 draws an actor this frame.
//
// * object_cull_003c8990: 003c8ad0 + 003c8990, the VU0 micro routine
//   (microcode program 0) that culls an object's local box against the camera
//   clip matrix (renderer +0xf00), returning the code the scene frame 0013e1b0
//   stores at object +0x38 and the distance at +0x3c.
// * man_draw_flags_0037c350: the flags word 0037c350 returns (model +0x2bd4)
//   on the GS-state path (a3 with 0x80000, as 003870c0 calls it with
//   0x480000); its GS packet is rendering and is not modelled here.
// * man_draw_alpha_003872d8 / man_draw_mode_00387650: the segments of 003870c0
//   that set the model's alpha (+0x2bec and the GS globals 005382c8..d4) and
//   draw mode (+0x2bda; 0 skips the draw).
// Goldens: tools/ee/gen_man_draw_gate.py (tests/man_draw_gate_goldens.hpp).
#include "character_shadows.hpp"
#include <array>
#include <cstdint>
#include <optional>

namespace omega {
// code: 0x80 when all eight corners lie outside one common clip plane
// (|x|,|y|,|z| <= |w|); otherwise (0x10 when the largest corner z minus near
// is >= +0) | (4 when a corner has z < -|w|, else 2 when a corner is outside
// any plane, else 1). dist: -(largest corner z), the VU word (checkpoint 132uk5:
// the VU float range, exponent 255 a normal binade and overflow clamped to
// +-0x7fffffff, so dist can be an exponent-255 word, a NaN pattern as an IEEE
// float: compare it only through scene_walk_order_0013eee0). Throws (naming
// 003c8990) for VMAX of +0 against -0.
struct ObjectCull { std::uint32_t code{}; float dist{}; };
ObjectCull object_cull_003c8990(const ShadowMatrix& clip,float near_z,const std::array<float,3>& lo,const std::array<float,3>& hi,
    const std::optional<ShadowMatrix>& transform);

// 0037c350(renderer, transform, code, a3) return value (model +0x2bd4): 0x80000000
// for code 0x80; else s = (mode_word | a3) & 0xbfffffff with s & 0x80000 required
// (the other path, 0037c678, throws); 0x180 cleared when s has either bit and
// code lacks 0x10; 0x40000000 when code has 4. mode_word is renderer +0x10e0.
std::uint32_t man_draw_flags_0037c350(std::uint32_t mode_word,std::uint32_t code,std::uint32_t a3);

// 003872d8..00387568. a3 / t0 are 003870c0's a3 / t0 (s6 / fp), g4933e8 the
// word [004933e8] (boot -1), prev_2bd4 the model's +0x2bd4 from the last draw,
// depth the y of the view-stack top's translation after the model push
// (sp+0x10d4).
struct ManDrawAlphaInput {
    bool a3{},t0{};
    std::int32_t g4933e8{-1};
    std::uint32_t prev_2bd4{};
    std::uint16_t h2bd8{};
    std::int8_t b2bdd{};
    float depth{};
};
struct ManDrawAlpha {
    std::uint16_t h2bec{};          // model +0x2bec after the segment (unchanged when !wrote_2bec)
    bool wrote_2bec{};
    std::uint16_t h2bd8{};          // model +0x2bd8 after the segment
    std::uint32_t s7{};             // the alpha the draw uses (sign-extended +0x2bec, or 0x80)
    bool wrote_globals{};
    std::array<std::uint32_t,4> globals{};  // 005382c8..005382d4 when written
};
ManDrawAlpha man_draw_alpha_003872d8(const ManDrawAlphaInput& in,std::uint16_t h2bec_before);

// 00387650..003876ec: +0x2bda from the flags (bit 31 -> 0, bit 30 -> 2, else 1);
// mode 2 becomes 0 when (signed) +0x2da0 is 2, or when it is 0 and depth < 2.5.
// Mode 0 skips the model (003876f0 -> 003899e0).
std::uint8_t man_draw_mode_00387650(std::uint32_t flags,std::int8_t b2da0,float depth);

// Checkpoint 118e: 00386ef8..00387060 of the VU1 upload 00386ad0 (called from
// 00387fac with t2 = 003870c0's a3, t1 = its s2, t3 = its t0 and its s7 as the
// stack argument): the two GIF tags at upload +0xd0 / +0xe0 (VU1 qwords
// 129 / 130, kicked by the character microprogram from 0x22b8) and the
// model's +2bd8. words = upload +0xd0..+0xec; words[1] / words[5] are the
// tags' upper words (PRE = 1, PRIM, NREG). a3: 0x10424000 tags (XYZ2 only,
// +2bd8 = 0xff); flags & 0x80: 0x303e4000 / 0x307e4000 (0x303e4000 with s2),
// +2bd8 kept; s7 < 0x80 or t0: 0x302e4000 twice; otherwise 0x302e4000 /
// 0x306e4000 (0x302e4000 with s2); both with +2bd8 = 0xff.
struct ManDrawTagsInput {
    std::uint32_t a3{},s2{},t0{};
    std::uint32_t flags_2bd4{};
    std::int32_t s7{};
    std::uint16_t h2bd8{};
};
struct ManDrawTags {
    std::array<std::uint32_t,8> words{};
    std::uint16_t h2bd8{};
};
ManDrawTags man_draw_gif_tags_00386ef8(const ManDrawTagsInput& in);
// PRIM of a GIF tag's upper word (bits 47..57 of the tag).
constexpr std::uint32_t gif_tag_prim(std::uint32_t upper){return (upper>>15)&0x7ffU;}

// The GS pixel arithmetic of the man model's strips (Checkpoint 118e). The
// character microprogram writes RGBAQ.A = 003870c0's s7 on its lit path
// (micro 0x20f8 ilw.x vi9 = qword 123.x low half = 0x47000000 + s7; mfir.w
// into each output colour, whose later writes are .xyz only); TFX MODULATE
// gives As = (At * Av) >> 7. TEST_1 (0037c350 -> 003797e0) is ATE 1, ATST
// GEQUAL, AREF and AFAIL RGB_ONLY from 005382c8..d4 (the alpha segment's
// globals {1, 5, aref, 3}): a passing pixel writes colour and Z, a failing
// one colour only. ABE (the tags' PRIM 0x5c / 0xdc, or PRMODE +0x10c = 1
// from the renderer init) blends with ALPHA_1 0x44: (Cs - Cd) * As >> 7 + Cd.
struct ManDrawGs {
    std::uint8_t vertex_alpha{0x80};
    std::uint8_t aref{0x80};
};
// The model's GS state from its alpha segment; throws (naming 003872d8)
// when the segment kept the previous draw's TEST globals or wrote a TEST
// other than {1, 5, aref, 3}.
ManDrawGs man_draw_gs_state(const ManDrawAlpha& alpha);
struct ManDrawPixel {
    std::uint32_t as{};   // (At * Av) >> 7
    bool z_write{};       // ATST GEQUAL passed (AFAIL RGB_ONLY otherwise)
};
ManDrawPixel man_draw_pixel(std::uint8_t texel_alpha,const ManDrawGs& gs);
}
