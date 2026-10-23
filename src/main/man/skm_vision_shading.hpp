#pragma once
// Checkpoint 132cp5: the night / thermal shading of the man model draw (Skm_UploadBones / Skm_Draw, 285370.s).
// Thermal (D_0053B358 != 0, the goggles' mode 2): 00387c00..00387e3c replaces the light matrix rows at sp+1060
// (non-thermal: D_00537DD0) by r0 = Inv (-1, 0, 0, 0) and r1 = Inv (0, 0, -1, 0), Inv = Mtx_InvertRigid 00133200 of
// the view matrix renderer +a80 + 64 [00538c64] (func_00131F00, the vmulax / vmadday / vmaddaz / vmaddw products,
// Matrix_TransposeInPlace): row j = (r0[j], r1[j], 0, 0). The microprogram set 4 then writes per vertex
// ST = 0.5 + 0.5 (r0 . n, r1 . n) (n the world normal: the palette normal rows are these rows times the bone),
// colour 0x80 with RGBAQ.A = s7; the texture is texman[D_0053B360 + 2 class] (0038a918; filled by 0038dd40, the class
// the part's +2dd0 from 00390570, man_sort.hpp).
// Night (D_0053B350 != 0, mode 1): the draw 00392a3c passes t1 = 1 (sp+ec); 0038804c..00388378 draws up to five
// ghosts from the history slots actor +480 + 0x540 s0 (21 bone matrices each, saved in screen space) before the
// model, then shifts the history and the stamps +2bf0..+2c00. Goldens: tools/ee/gen_skm_vision.py.
#include "character_shadows.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace omega {

// 00387c00..00387e3c: the three qwords written to sp+1060 for the view matrix `view`.
std::array<ShadowVector,3> skm_thermal_rows_00387c00(const ShadowMatrix& view);
// The microprogram set 4's texture coordinate for world normal n (VU1; traced, no EE golden).
std::array<float,2> skm_thermal_uv(const std::array<ShadowVector,3>& rows,const std::array<float,3>& n);

inline constexpr std::size_t skm_night_slots=5;
// 0038804c..00388078: the GIF tag word the ghosts draw with (scratch +e4): FGE by +2bd4 & 0x80.
std::uint32_t skm_night_gif_tag(std::uint32_t flags_2bd4);
// 00388088..003880a4: history slot s0 is drawn unless stamp < d0 - s0 - 1 (sltu).
bool skm_night_ghost_drawn(std::uint32_t stamp,std::uint32_t d0,std::uint32_t s0);
// 0038812c..00388188: the ghost's RGBAQ.A, (uint) (0.1f * (float) (int) ((5 - s0) * s7)) (cvt.w.s, the 2^31 path).
std::uint32_t skm_night_ghost_alpha(std::uint32_t s0,std::uint32_t s7);
// 00388344..00388374: +2c00 = +2bfc, ..., +2bf4 = +2bf0, +2bf0 = d0.
void skm_night_stamp_shift(std::array<std::uint32_t,skm_night_slots>& stamps,std::uint32_t d0);

}  // namespace omega
