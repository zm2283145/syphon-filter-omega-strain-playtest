#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace omega {

// 004029b0(this, path, colours a2, dirs a3): the front end's light file reader (FRONTEND\MANLIGHT.TXT /
// GUNLIGHT.TXT, 3ds Max exports). The text (with a '\n' appended) is scanned for the first line that
// starts with "LIGHT" (strncmp 5); that line and the next ('{') are skipped, then each line is read with
// sscanf("%s %d, %*s %s %f %f %f  %f %f %f  %f %f %f  %f %f %f,  %f %f %f, %f") until a result other
// than 19. A light whose type starts with "DIRECT" (strncmp 6), while fewer than four are kept, gives
// direction[count] = its tm row 2 (the third triple) and colour[count].xyz = 128 * intensity * rgb.
// The four directions are normalised (vrsqrt; an unused zero row stays zero) and built into the light
// matrix by 00131f00. A file that does not open (0036def0 set after the open 0036e4b0) takes the default lights
// at 00402d80 instead: frontend_default_lights_402d80.
struct FrontendLightSet {
    std::array<std::array<float,4>,4> colours{};     // a2: four colour qwords (w 0), unused lights zero
    std::array<std::array<float,3>,4> directions{};  // a3's light rows, unit length, unused rows zero
    std::size_t count{};
};
FrontendLightSet parse_frontend_lights_4029b0(std::string_view text);
// 00402d80..00402f74 (004029b0 when the file did not open): directions (0, 0.5, -1.5), (1.5, 0.5, 0.5),
// (-1.5, 0.5, 0.5), (0.5, 0.5, 0.5), normalised by the same vrsqrt loop (00402f80); colours (0.2, 0.2, 0.2),
// (0.3, 0.3, 0.3), (0.9, 0.9, 0.9), 0 (w 0), stored as is: unlike a file's lights they are not scaled by 128.
FrontendLightSet frontend_default_lights_402d80();
// 004029b0 on an opened file's text, or nullopt for a file that did not open (the default lights).
FrontendLightSet frontend_lights_4029b0(std::optional<std::string_view> text);

// 004053b0(0055dda0), run once from the game init (00169574) after MANSORT2 (00403490):
// MANLIGHT's colours to +0x120 (0055dec0) and its directions times Rz(pi) (x and y negated) to +0x160
// (0055df00); GUNLIGHT's colours to +0x1a0 (0055df40) and its directions transposed (00133300, 00384a00,
// 00133300) to +0x1e0 (0055df80). The transposition is a layout change: `gun.directions` are the rows the
// VU1 program dots with the model-frame normals.
struct FrontendLights {
    FrontendLightSet man;  // the figure's lights (0055dec0 / 0055df00)
    FrontendLightSet gun;  // every 003836b0 gun preview's lights (0055df40 / 0055df80)
};
// A nullopt text is a file that did not open (00402d80's defaults for that set).
FrontendLights frontend_lights_4053b0(std::optional<std::string_view> manlight_text,std::optional<std::string_view> gunlight_text);

// The VU1 model program's lit colour (program set 2, 0x2f8..0x500): per light d = max(L . n, 0.02);
// p = bits->float(ftoi0(C + itof0(bits(d)) * 16)) with C = float bits d06e2000 (the exponent trick for
// d^16); e = d + p; colour = sum(colour_i * e_i) + 2^23 (the integer bias), clamped by mini against
// the program's I (255) and taken as its integer. Host IEEE rounding stands in for the VU's (a choice).
std::array<std::int32_t,3> frontend_light_colour(const FrontendLightSet& lights,const std::array<float,3>& normal,
                                                 std::int32_t clamp=255);

}  // namespace omega
