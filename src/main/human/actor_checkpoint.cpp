#include "actor_checkpoint.hpp"
#include "game_libm.hpp"
#include <cmath>
#include <stdexcept>
#include <utility>

namespace omega {
ActorCheckpoint make_actor_checkpoint(const std::array<float,16>& spawn_matrix,
    std::int32_t health,std::int32_t armor){
    for(const float value:spawn_matrix)if(!std::isfinite(value))
        throw std::runtime_error("Actor checkpoint: nonfinite spawn transform");
    ActorCheckpoint checkpoint;
    checkpoint.matrix=spawn_matrix;
    checkpoint.health_3450=health;
    checkpoint.armor_3458=armor;
    return checkpoint;
}

void set_actor_checkpoint(ActorCheckpoint& checkpoint,const std::array<float,3>& position,
    float yaw,std::int32_t health,std::int32_t armor,std::string label){
    if(!std::isfinite(yaw))throw std::runtime_error("Actor checkpoint: nonfinite node yaw");
    for(const float value:position)if(!std::isfinite(value))
        throw std::runtime_error("Actor checkpoint: nonfinite node position");
    const float cosine=game_cosf(yaw),sine=game_sinf(yaw);
    checkpoint.matrix={cosine,sine,0.0F,0.0F,
        -sine,cosine,0.0F,0.0F,
        0.0F,0.0F,1.0F,0.0F,
        position[0],position[1],position[2],1.0F};
    checkpoint.health_3450=health;
    checkpoint.armor_3458=armor;
    checkpoint.label_timer_6c=5.0F;
    checkpoint.label_70=std::move(label);
}

std::optional<CheckpointLabelDraw> update_checkpoint_label(ActorCheckpoint& checkpoint,float dt,std::uint8_t font_byte){
    if(!checkpoint.label_70)return std::nullopt;
    if(!checkpoint.label_timer_6c)throw std::runtime_error("0022e900: checkpoint label without its +6c timer (0022ea20 sets both)");
    const float timer=*checkpoint.label_timer_6c;
    if(!std::isfinite(timer)||!std::isfinite(dt))throw std::runtime_error("0022e900: nonfinite checkpoint label timer or dt");
    if(timer<0.0F)return std::nullopt;
    CheckpointLabelDraw draw;
    draw.text=*checkpoint.label_70;
    draw.font_index=font_byte;
    draw.colour={0.65F,0.7F,1.0F,timer<1.0F?timer:1.0F};   // 0x3f266666, 0x3f333333, 0x3f800000
    float next=timer-dt;                                     // 0022e9f4 sub.s
    if(next!=0.0F&&std::fabs(next)<1.17549435e-38F)next=std::copysign(0.0F,next);
    checkpoint.label_timer_6c=next;
    return draw;
}

std::int32_t checkpoint_label_x(std::int32_t width){
    const std::int64_t half=width>=0?(static_cast<std::int64_t>(width)>>1):((static_cast<std::int64_t>(width)+1)>>1);
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(-half));
}
}
