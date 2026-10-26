#pragma once
// The VU1 particle program at micro address 0x29c (byte 0x14e0 of the
// microcode set 1 the renderer uploads from 0047ba70), Checkpoint 122d: the
// part every 003a0050 MSCAL runs before it branches to one of the sprite
// programs 0x5e0 / 0x8a0 / 0x1cc8 (the GS packet builders; the host draws
// from there, research/EFFECTS.md "Checkpoint 122d office fire").
//
// Micro-mode float rules are those of tools/ee/ee_vu0.py with vu_float_range
// (Checkpoint 132cp8, VU User's Manual 2.1.1 / 2.3): operands with a zero
// exponent read as signed zero; exponent 255 is a normal binade (1.F x 2^128,
// the VU has no infinity or NaN); results round to nearest, a result past
// exponent 255 clamps to +-MAX (0x7fffffff), as does DIV by zero; denormal
// results keep their sign only; MADD/MSUB round the product first; MAX/MINI
// order the words sign-magnitude and pick the operand bits. The
// pipeline is resolved statically (every dependent read stalls until the
// write lands), so the pairs run in order, upper before lower, except pair
// 0x2d5 whose lower MTIR reads vf25.y before the upper ITOF0 of that pair
// writes it.
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace omega::particle {

using Qword = std::array<std::uint32_t, 4>;

struct Vu1State {
    std::array<Qword, 32> vf{};  // vf0 must hold (0, 0, 0, 1.0)
    Qword acc{};
    std::uint32_t i = 0, q = 0, r = 0;
    std::array<std::uint16_t, 16> vi{};
    std::array<Qword, 0x400> data{};  // VU1 data memory, qword addressed (wraps at 0x3ff)

    Vu1State() { vf[0] = {0, 0, 0, 0x3f800000u}; }
};

// Port-side speed only (132vd2, research/VITA.md "Particle programs on NEON (132vd2)"): this thread's Vu1State, its
// registers set as a fresh one's (vf0 = (0, 0, 0, 1), the rest zero) and its data memory as the last call on this
// thread left it, instead of a fresh 17 KB state zeroed per batch. cull_batch and the sprite / heat-haze programs read
// no data word; run_deferred_program's programs read only the words it writes first (DeferredProgram). Valid until
// the next call on the thread.
Vu1State& thread_vu1_state();

// Runs 0x29c with TOP = `top` (XTOP). Inputs: data 0xf..0x20 (the 003a0050
// uploads), TOP+0xb (low 15 bits of x: the particle count n) and TOP+0xc (the
// random-table qword). Outputs: n qwords vf10 at TOP+0xc and n qwords vf13 at
// TOP+0xc+n. Returns the sprite program it branches to (0x5e0 / 0x8a0 /
// 0x1cc8). Registers persist across calls as on the hardware.
std::uint32_t run_particle_program(Vu1State& vu, std::uint32_t top);

// Checkpoint 122f: the type-0 (physics) emitter program 0x170 (byte 0xb80),
// MSCAL 0x16 of 00398590 through the table 00538080 (003700d0). Same rules
// and interface as 0x29c: inputs data 0xf..0x1c (00398590's uploads of
// em+90..d0 and def+900..980), TOP+0xb (count) and TOP+0xc (random qword);
// outputs n qwords vf10 at TOP+0xc and n qwords vf13 at TOP+0xc+n; returns
// the sprite program it branches to (0x5e0 / 0x8a0 / 0x1cc8).
// Checkpoint 132a / 132cp8: its operands reach exponent 255 when the
// motion-blur time f24 (-= f23 at 003988b4) turns negative early in an
// emitter's life: powf 00121b30 of a negative base takes the (x - x) / (x - x)
// domain path, +FLT_MAX under div.s, so def+920 and the positions are near
// +-FLT_MAX and MADD products pass 2^128 ("bulfx_metal1b", pass 4); under the
// rules above they are plain values and the sprite clips (ADC under 0x5e0
// and 0x1cc8; range_cases in tests/particle_runtime_goldens.hpp).
std::uint32_t run_type0_program(Vu1State& vu, std::uint32_t top);
// Checkpoint 132cp8: the ShatterWindow particle program 0x208 (byte 0x1040),
// MSCAL 0x17 of 0039c680 (the 0039b7b0 draw of an emitter without a texture).
// Same rules and interface: inputs data 0xf..0x1d (em+90..ef and
// def+900..98f), TOP+0xb (count) and TOP+0xc (random qword); outputs n qwords
// vf10 at TOP+0xc and n qwords vf13 at TOP+0xc+n; returns 0x5e0 / 0x8a0 /
// 0x19a8. The D bit of pair 0x256 is a no-op (FBRST DE1 is never set).
std::uint32_t run_shatter_program(Vu1State& vu, std::uint32_t top);
// Checkpoint 122e: the renderer frame block the sprite programs read (VU data
// qwords 1..0xe, uploaded by 00377c70 from 0037c870, i.e. at every 0037c350
// of the particle draw 003a1ad0). The setup routine at pair 0x79 (reached
// through the program's BAL to pair 0) loads vf1..vf4 = data 5..8 (the
// world-to-clip matrix, columns: clip = vf1*x + vf2*y + vf3*z + vf4),
// vf5 = data 9 (camera +0x90: clip-to-screen scale), vf6 = data 0xa (camera
// +0xa0: screen offset); the sprite program loads qword 0xe .xy.
struct SpriteFrame {
    std::array<Qword, 4> clip{};  // data 5..8
    Qword scale{};                // data 9
    Qword offset{};               // data 0xa
    Qword sprite_scale{};         // data 0xe
    // Checkpoint 132cp4: data 0x2c / 0x2d, which only the heat-haze program
    // 0x1cc8 reads (0037c870 writes them at 0037d090 when draw flag 0x2000 is
    // set): heat_haze_clip_qword / heat_haze_uv_qword.
    Qword haze_clip{};            // data 0x2c
    Qword haze_uv{};              // data 0x2d
};

// 003a1ad0 pushes 2.0 (003a1b88) and 3.0 (003a1b90) onto the stacks 00538258
// and 00538268 (index 00538278) before its 0037c350, and pops them at 003a238c.
inline constexpr float kParticleScaleX = 2.0f, kParticleScaleY = 3.0f;

// 00377c70 qword 0xe from the camera projection (camera +0x10, or +0x50 when
// byte +0x104 is set; a row-major 4x4 of floats): (P[0] * (1 / a),
// P[5] * (1 / b), P[10], P[15]) with a / b the stack tops (div.s at
// 00377dd4 / 00377de0, mul.s at 00377e30 / 00377e34).
Qword sprite_scale_qword(const float (&projection)[16], float a, float b);

// The batch cull every sprite program runs first (BAL to pair 0; the data-0
// flag word 0 path, pairs 0x00..0x70): reject when the low 16 bits of
// bound_max.x (TOP+7) are 0xffff (pair 0xd); else the 8 corners of the box
// bound_min (TOP+6) / bound_max, in the order of pairs 0x3b..0x61, go through
// clip = cull[0]*x + cull[1]*y + cull[2]*z + cull[3] (data 1..4; MULA / MADDA
// / MADDW rounding) and CLIPW.xyz. vi3 = AND of the six-bit judgments: any
// plane all corners are outside of rejects (0x70; TOP+6.w = 0, the empty
// XGKICK of 0x76). Otherwise the mode stored at TOP+6.w (0xa6..0xb7) is 3 when
// a corner has z < -w (judgment bit 0x20), 2 when any corner is outside a
// plane, 1 when all are inside. Returns 0 for a culled batch.
std::uint32_t cull_batch(const Qword& bound_min, const Qword& bound_max, const std::array<Qword, 4>& cull);

// The GS SPRITE pairs of the sprite program `stop` (0x5e0: pairs 0xbe..0xf0;
// 0x8a0: pairs 0x116..0x14c), as the per-particle sequence of the software
// pipelined loop: for particle k with position p = position[k] (xyz, size in
// w) and colour c = colour[k], six qwords STQ0, c, XYZ0, STQ1, c, XYZ1 (what
// SQI writes from vi12 + 5). XYZ = ftoi0(screen) with w = MFIR (sign extended)
// of 0x7fff + (any clip flag of CLIPW xyz against w): 0xffff8000 sets the ADC
// bit (the GS does not draw the sprite). Throws for other stops.
std::vector<Qword> run_sprite_program(std::uint32_t stop, const std::vector<Qword>& position,
                                      const std::vector<Qword>& colour, const SpriteFrame& frame);

// Checkpoint 132cp4: 0037d090's VU data 0x2c, (0.5 * a, -0.5 * b, 1.0, 1.0)
// with a / b the renderer's stack tops (r+0x68 / r+0x78 at index r+0x88, the
// 2.0 / 3.0 that 003a1ad0 pushes), and 0x2d, (0.5, 0.5, 1.0 / float(W << 4),
// 1.0 / float(H << 4)) with W / H the screen size (r+0x1c / r+0x20).
Qword heat_haze_clip_qword(float a, float b);
Qword heat_haze_uv_qword(std::int32_t width, std::int32_t height);

// Checkpoint 132cp4: the heat-haze program 0x1cc8 (pairs 0x399..0x3e7, after
// the BAL to pair 0: the batch cull and the setup), FIRE_TO_LG02B. Per
// particle p = position[k] (xyz, size in w), c = colour[k]: 14 qwords, two GS
// SPRITEs under the tag `tag` (TOP+5, the emitter's def+0xd0; its x/y and z/w
// pairs become the two GIF tags with REGS 0x412412):
//   tag0, STQ (0, 0, q), c, XYZ0, STQ (q, q, q), c, XYZ1 (context 2: the
//   particle texture into destination alpha),
//   tag1, ST (u0, v0, 1), c, XYZ0, ST (u1, v1, 1), c, XYZ1 (context 1: the
//   screen copy at the normalized screen position, half extent grown by 1/64).
// XYZ = ftoi0(screen -+ half extent) with w = 0x7fff, or 0xffff8000 (ADC)
// when this particle's CLIPW judgment or either of the two before it (FCAND
// 0x3ffff, the clip flags reset by the setup's FCSET 0) is outside. The last
// particle's second tag gets x = 0x8001 (EOP, pair 0x3e7).
std::vector<Qword> run_heat_haze_program(const std::vector<Qword>& position, const std::vector<Qword>& colour,
                                         const SpriteFrame& frame, const Qword& tag);

}  // namespace omega::particle
