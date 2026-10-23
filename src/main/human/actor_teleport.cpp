#include "float_bits.hpp"
#include "actor_teleport.hpp"
#include "camera_curves.hpp"
#include "game_libm.hpp"
#include "morph_resources.hpp"
#include "player_camera.hpp"
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
float check(float v){
    if(::omega::float_bits::nonfinite_or_subnormal(v))
        throw std::runtime_error("Actor teleport: unsupported nonfinite/subnormal arithmetic");
    return v;
}
RootVector check(const RootVector& v){for(float x:v)check(x);return v;}
std::uint8_t mode_value(ActorTeleportMode mode){
    const auto value=static_cast<std::uint8_t>(mode);
    if(value>4)throw std::runtime_error("Actor teleport: mode "+std::to_string(value)+" does not exist");
    return value;
}
float heading_of(const std::array<RootVector,3>& r){
    // Same selection as the final-heading producer (00121ab8, game libm).
    if(r[0][2]<r[1][2])return check(game_atan2f(r[0][1],r[0][0]));
    return check(game_atan2f(-r[1][0],r[1][1]));
}
}

bool actor_teleport_translates(ActorTeleportMode mode){
    const auto m=mode_value(mode);return m==0||m==2||m==3;
}
bool actor_teleport_rotates(ActorTeleportMode mode){
    const auto m=mode_value(mode);return m==1||m==2||m==4;
}

RootVector actor_teleport_delta(ActorTeleportMode mode,const ActorTeleportMatrix& matrix,const RootVector& floor_position){
    if(!actor_teleport_translates(mode))throw std::runtime_error("Actor teleport: rotation-only modes have no translation");
    check(matrix[3]);
    if(mode==ActorTeleportMode::offset)return matrix[3];
    check(floor_position);
    RootVector d;
    for(std::size_t i=0;i<4;++i)d[i]=check(matrix[3][i]-floor_position[i]);
    return d;
}

RootVector add_actor_teleport_xyz(const RootVector& value,const RootVector& delta){
    check(value);check(delta);
    return {check(value[0]+delta[0]),check(value[1]+delta[1]),check(value[2]+delta[2]),value[3]};
}
RootVector add_actor_teleport_lanes(const RootVector& value,const RootVector& delta){
    check(value);check(delta);
    RootVector r;
    for(std::size_t i=0;i<4;++i)r[i]=check(value[i]+delta[i]);
    return r;
}

void translate_actor_teleport_root(RootAnimationState& root,const RootVector& delta){
    auto next=root;
    next.pose_position=add_actor_teleport_xyz(root.pose_position,delta);   // 001bc350
    next.dirty_pose=true;
    next.position=add_actor_teleport_xyz(root.position,delta);             // 001b26a0
    next.dirty_transform=true;
    root=next;
}

ActorTeleportRotation actor_teleport_rotation(ActorTeleportMode mode,const ActorTeleportMatrix& matrix,
    const RootVector& floor_orientation){
    if(!actor_teleport_rotates(mode))throw std::runtime_error("Actor teleport: translation-only modes have no rotation");
    std::array<RootVector,3> rows{check(matrix[0]),check(matrix[1]),check(matrix[2])};
    if(mode==ActorTeleportMode::compose){
        // p = +50 (x) q0, then the rows of p (the 0019d2c0 block).
        const auto q0=check(camera_matrix_to_quaternion(rows));
        const auto p=check(camera_quaternion_multiply(check(floor_orientation),q0));
        const auto m=root_placement_matrix(RootParentTransform{p,{0,0,0,1}});
        for(std::size_t r=0;r<3;++r)rows[r]={check(m[r*4]),check(m[r*4+1]),check(m[r*4+2]),0};
    }
    ActorTeleportRotation result;
    result.heading=heading_of(rows);
    result.orientation=check(camera_matrix_to_quaternion(rows));
    return result;
}

void rotate_actor_teleport_floor_root(RootAnimationState& root,const RootVector& orientation){
    check(orientation);check(root.orientation);check(root.pose_orientation);
    // 001bc280: 0 - XYZ, W kept (a zero lane becomes +0).
    const RootVector conjugate{check(0.0F-root.orientation[0]),check(0.0F-root.orientation[1]),
        check(0.0F-root.orientation[2]),root.orientation[3]};
    // 001a7d80(x, y) stores y (x) x into x.
    const auto relative=check(camera_quaternion_multiply(conjugate,root.pose_orientation));
    auto next=root;
    next.pose_orientation=check(camera_quaternion_multiply(orientation,relative));   // 001bc1c0
    next.dirty_pose=true;
    next.orientation=orientation;                                                    // 001a7cd0
    next.dirty_transform=true;
    next.normalization_counter=0;
    root=next;
}
}
