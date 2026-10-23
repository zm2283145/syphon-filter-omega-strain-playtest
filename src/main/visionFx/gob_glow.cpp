#include "gob_glow.hpp"

#include <bit>
#include <cmath>

namespace omega {

std::vector<GobGlowCall> gob_glow_0025f270(std::int32_t vision_mode, std::span<const GobGlowObject> list,
                                           std::uint32_t pass, std::int32_t flare_1b) {
    std::vector<GobGlowCall> calls;
    for (std::size_t k = 0; k < list.size(); ++k) {
        const auto& o = list[k];
        // 0025f2d4..0025f330: +38 != 0x80, +49 == 0xf, kind +68 4 or 6.
        if (o.code_38 == 0x80U || o.class_49 != 0x0fU || (o.kind_68 != 4U && o.kind_68 != 6U)) continue;
        // 0025f338..0025f358: s0 = *00236f20(0x1b); +64 and its model's data.
        if (!o.model_64 || !o.loaded) continue;
        GobGlowCall call{};
        call.object = k;
        call.texture = flare_1b;
        if (pass == 0U) {
            // 0025f368..0025f3b4: the model's first texture when its TDX is PSMCT32 (header +a == 0).
            const std::int32_t index = o.texture_count != 0U ? o.first_texture : -1;
            if (o.texture_psm == 0U) {
                call.texture = index;
                call.model_texture = true;
            }
        }
        // 0025f3b8..0025f3f4: vcall +88 (00213ac0: +60 and the model loaded -> the VUM header +30).
        call.half = 2.5F;
        if (o.named_60) call.half = 0.5F * std::fabs(o.bounds_max_x - o.bounds_min_x);
        call.centre = o.centre;   // 0025f3f8: vcall +e0
        const float c = std::bit_cast<float>(0x3f19999aU);
        call.colour = {c, c, c, std::bit_cast<float>(0x3f451eb8U)};
        call.glare = static_cast<std::uint32_t>(vision_mode - 1) < 2U ? 1U : 0U;
        calls.push_back(call);
    }
    return calls;
}

std::vector<GobGlowCall> gob_glow_0025f230(std::int32_t vision_mode, std::span<const GobGlowObject> list,
                                           std::int32_t flare_1b) {
    if (vision_mode != 1 && vision_mode != 2) return {};
    return gob_glow_0025f270(vision_mode, list, 1, flare_1b);
}

}
