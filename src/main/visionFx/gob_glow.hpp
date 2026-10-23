#pragma once
// Checkpoint 132cp5: the GOB glow 0025f270(cVisionFxMan, list, pass), called by the scene frame 0013e1b0 every
// frame at 0013e984 on the kind-4 GOB list sp+20f0 (pass 0) and, through 0025f230 (modes 1 / 2 only), on the
// kind-6 list sp+3100 (pass 1). Per listed object it builds the arguments of 00378170 (the wrapper of the corona
// sprite draw 003781b0 on the renderer 005381f0). Goldens: tools/ee/gen_gob_glow.py (the original's code on
// built objects: GOB +88 00213ac0 -> cVUM_GOBJ_v20 003947a0, +e0 003cd420; 00236f20 and 00378170 hooked).
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace omega {

// One listed object, as 0025f270 reads it.
struct GobGlowObject {
    std::uint32_t code_38{};          // the culler's code (0x80: outside; the scene frame stores it)
    std::uint8_t class_49{};          // 0xf: a GOB
    std::uint32_t kind_68{};          // GOB kind: 4 or 6 glow
    bool model_64{};                  // +64: the cVUM_GOBJ model instance is present
    bool loaded{};                    // *(+64 -> +68): the model's file data (Model_Load +0) is set
    std::uint32_t texture_count{};    // (+64 -> +68) +4 -> +14: the VUM header's texture count
    std::int16_t first_texture{};     // *(short*)((+64 -> +68) +8): the model's first texman handle (Model_BindTextures)
    std::uint16_t texture_psm{};      // texman +8 [index] -> +44 -> +a: that texture's TDX PSM (index: pass 0 only)
    bool named_60{};                  // +60: the asset is not "no_vum" (0021aee4); else +88 gives no box
    float bounds_min_x{}, bounds_max_x{};   // the VUM header +30 / +40 (cVUM_GOBJ_v20 returns header +30)
    std::array<float, 4> centre{0.0F, 0.0F, 0.0F, 1.0F};   // +e0 003cd420: the transform node +40, else (0,0,0,1)
};

// One 00378170(005381f0, &centre, &colour, texture, glare, 1, half) call.
struct GobGlowCall {
    std::size_t object{};             // index in the list
    std::array<float, 4> centre{};
    std::array<float, 4> colour{};    // (0.6, 0.6, 0.6, 0.77): 0x3f19999a x 3, 0x3f451eb8
    std::int32_t texture{};           // the flare (effect entry 0x1b), or pass 0: the model's first texture when PSMCT32
    bool model_texture{};             // the texture is the model's own
    std::uint32_t glare{};            // t0: (mode - 1) < 2, i.e. night / thermal
    std::uint32_t one{1};             // t1
    float half{};                     // $f12: 2.5, or 0.5 * |box +10 - box +0| when +88 returns a box
};

// 0025f270: flare_1b is *00236f20(0x1b). A pass-0 object whose model has no textures reads the texman slot -1
// (0025f388): texture_psm then stands for that read; the caller decides whether it can supply it.
std::vector<GobGlowCall> gob_glow_0025f270(std::int32_t vision_mode, std::span<const GobGlowObject> list,
                                           std::uint32_t pass, std::int32_t flare_1b);
// 0025f230: modes 1 / 2 run 0025f270(fx, list, 1); other modes draw nothing.
std::vector<GobGlowCall> gob_glow_0025f230(std::int32_t vision_mode, std::span<const GobGlowObject> list,
                                           std::int32_t flare_1b);

}
