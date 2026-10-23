#include "gs_alpha.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace omega {

GsAlpha decode_gs_alpha(std::uint64_t value) {
    return {static_cast<std::uint8_t>(value & 3), static_cast<std::uint8_t>(value >> 2 & 3),
            static_cast<std::uint8_t>(value >> 4 & 3), static_cast<std::uint8_t>(value >> 6 & 3),
            static_cast<std::uint8_t>(value >> 32 & 0xff)};
}

GsBlendState gs_blend_state(std::uint64_t alpha) {
    constexpr std::uint8_t cs = 0, cd = 1, zero = 2, as = 0, fix = 2;
    const auto f = decode_gs_alpha(alpha);
    const float factor = f.c == fix ? float(f.fix) / 128 : 1.0F;
    if (f.d == cd && (f.c == as || f.c == fix)) {
        const bool constant = f.c == fix;
        if (f.a == cs && f.b == cd)
            return {constant ? GsBlend::fix_over : GsBlend::source_alpha_over, factor};
        if (f.a == cs && f.b == zero)
            return {constant ? GsBlend::fix_add : GsBlend::source_alpha_add, factor};
        if (f.a == zero && f.b == cs)
            return {constant ? GsBlend::fix_subtract : GsBlend::source_alpha_subtract, factor};
    }
    char text[19];
    std::snprintf(text, sizeof text, "%llx", static_cast<unsigned long long>(alpha));
    throw std::runtime_error(std::string("Unsupported GS ALPHA_1 selection 0x") + text);
}

std::uint8_t gs_blend_channel(std::uint64_t alpha, std::uint8_t cs, std::uint8_t as, std::uint8_t cd) {
    const auto f = decode_gs_alpha(alpha);
    const auto pick = [&](std::uint8_t selector) -> int {
        if (selector == 0) return cs;
        if (selector == 1) return cd;
        if (selector == 2) return 0;
        throw std::runtime_error("GS ALPHA selection 3 is reserved");
    };
    const int c = f.c == 0 ? as : f.c == 2 ? f.fix : -1;
    if (c < 0) throw std::runtime_error("GS ALPHA C = Ad is not modelled");
    const int value = (((pick(f.a) - pick(f.b)) * c) >> 7) + pick(f.d);
    return static_cast<std::uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
}

std::uint8_t gs_premultiplied_source(std::uint64_t alpha, std::uint8_t cs, std::uint8_t as) {
    const auto state = gs_blend_state(alpha);
    int value = 0;
    if (state.blend == GsBlend::source_alpha_add) value = (int(cs) * as) >> 7;
    else if (state.blend == GsBlend::source_alpha_subtract) value = -((-int(cs) * as) >> 7);
    else throw std::runtime_error("gs_premultiplied_source: only ALPHA 0x48 / 0x42 take the premultiplied form");
    return static_cast<std::uint8_t>(value > 255 ? 255 : value);
}

}
