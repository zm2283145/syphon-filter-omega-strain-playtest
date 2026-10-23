#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace omega {
struct ActorCheckpoint {
    std::array<float,16> matrix{};
    std::int32_t health_3450{};
    std::int32_t armor_3458{};
    std::int32_t constructor_health_64{100};
    std::optional<float> label_timer_6c;
    std::optional<std::string> label_70;
};

ActorCheckpoint make_actor_checkpoint(const std::array<float,16>& spawn_matrix,
    std::int32_t health,std::int32_t armor);
// MSGGAME_ADD_CHECKPOINT: captures the node's position and yaw, current
// health/armor, and the already-resolved label string.
void set_actor_checkpoint(ActorCheckpoint& checkpoint,const std::array<float,3>& node_position,
    float node_yaw,std::int32_t health,std::int32_t armor,std::string label);

// The label 0022e900 queues: 003d0ef0(003d1020(), +70, font, colour, x, 0),
// the text manager's immediate list (003d0c10 draws it with 0036a330 at
// z 0.001 before the listed text objects, then clears it).
struct CheckpointLabelDraw {
    std::string text;                  // +70
    std::uint8_t font_index{};         // byte 004f7960 & 0xff: font = *(001698c0() + 0x280 + 4 * index)
    std::array<float,4> colour{};      // (0.65, 0.7, 1.0, min(+6c, 1.0))
};
// 0022e900(component +33f0, dt), the cCheckpoint vtable +48 the actor update
// calls at 001b1de8..1e04 while actor +14 is set: nothing while +70 (the
// label) is null or +6c (the timer 0022ea20 sets to 5.0) is below 0;
// otherwise the label with alpha +6c when below 1.0 (else 1.0), then +6c -= dt.
// font_byte is 004f7960 (.bss; only Script_SetCheckpointFont 0022ed00 ->
// 0022ed28 writes it).
std::optional<CheckpointLabelDraw> update_checkpoint_label(ActorCheckpoint& checkpoint,float dt,std::uint8_t font_byte);
// 0022e9c4..9d8: x = -(width / 2), width = 0036aa50(font, +70, 0x200), the
// halving rounded toward zero (bgez: sra 1, else (width + 1) sra 1); y = 0.
std::int32_t checkpoint_label_x(std::int32_t width);
}
