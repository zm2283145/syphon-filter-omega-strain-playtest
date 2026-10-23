#pragma once
// Checkpoint 132cp6: the light corona pass 0025f4c0 (cVisionFxMan, called by the scene frame 0013e1b0 at 0013ea04
// every frame with *004ffc30) and its light selection 00372b40 (the light manager 00535d20). Per selected light it
// builds the arguments of the corona sprite draw 003781b0 (renderer 005381f0; src/main/lights/corona_draw.cpp).
// Goldens: tools/ee/gen_vision_corona.py (the original's code: 004d02f0 fills the colour rows, 00372b40 on a light
// table with its SPR DMA done by a hook, the lights' real vtables; 003781b0 hooked).
#include "level_lights.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace omega {

// 004a7de0: nine (r, g, b, a) rows, BSS filled by the static initializer 004d02f0.
inline constexpr std::array<std::array<float, 4>, 9> vision_corona_rows_004a7de0{{
    {1.0F, 1.0F, 1.0F, 0.7F},
    {0.75F, 0.95F, 0.27F, 0.3F},
    {0.91F, 0.24F, 0.24F, 0.6F},
    {0.3F, 0.8F, 0.35F, 0.2F},
    {0.38F, 0.28F, 0.9F, 0.25F},
    {0.91F, 0.28F, 0.14F, 0.25F},
    {0.93F, 0.78F, 0.34F, 0.2F},
    {0.99F, 0.42F, 0.16F, 0.15F},
    {0.99F, 0.65F, 0.11F, 0.1F},
}};
// 004a7e70 / 004a7ea0: the element offsets along the screen-centre axis and the element scales (ELF data).
inline constexpr std::array<float, 9> vision_corona_offsets_004a7e70{0.0F, 0.0F, 0.0F, 70.0F, 90.0F, -120.0F, -135.0F, -300.0F, -370.0F};
inline constexpr std::array<float, 9> vision_corona_scales_004a7ea0{1.0F, 1.0F, 2.0F, 0.4F, 0.8F, 0.75F, 0.93F, 1.1F, 1.65F};

// 00372b40(mgr, type mask, aux mask, class mask): the table rows whose entry +0 (the light's type +1e) & type, entry +1
// (its aux +1d) & aux unless aux is 0xff, and entry +2 (the class) & class are all non-zero, in table order.
std::vector<LevelLightRef> light_table_select_00372b40(const LevelLights& lights, std::span<const LevelLightTableRow> table,
                                                       std::uint8_t type_mask, std::uint8_t aux_mask, std::uint8_t class_mask);

// One 003781b0(005381f0, light +30, colours, fx +1d4, 004a7e70, 004a7ea0, count, glare, half, sprite) call.
struct VisionCoronaCall {
    LevelLightRef light;
    std::array<float, 4> position{};                       // light +0x30
    std::array<std::array<float, 4>, 9> colours{};         // the first `count` are passed
    std::uint32_t count{};                                 // 9 when the light's aux has 8, else 1
    bool glare{};                                          // t3: aux & 0x10
    std::uint32_t sprite{};                                // the caller's sp+0: vcall +0x24 & 0xff
    float half{};                                          // $f12: 0.5 * vcall +0x34, times vcall +0x20 when glare
};

// 0025f4c0: mode 0 selects (0x54, 0x18, 1) with alpha scale 1.0, modes 1 / 2 (0x44, 0xff, 1) with 0.7, other modes
// select nothing; it skips the vision's own point light fx +0x194. `flashlight_glare_40` answers vcall +0x20 of a
// flashlight-glare light (0045bbe0: light +0x40, the port's Flashlight::glare_40). Unported vtables throw.
std::vector<VisionCoronaCall> vision_corona_0025f4c0(std::int32_t vision_mode, const LevelLights& lights,
                                                     std::span<const LevelLightTableRow> table,
                                                     std::optional<std::size_t> point_194,
                                                     const std::function<float(const LevelLightRef&)>& flashlight_glare_40);

}
