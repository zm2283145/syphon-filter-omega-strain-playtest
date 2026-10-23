#include "weapon_shot_frame.hpp"

namespace omega {
namespace {
// vmulax / vmadday / vmaddaz / vmaddw: ((c0*v.x + c1*v.y) + c2*v.z) + c3*v.w per lane.
RootVector combine(const ShotFrame& c,const RootVector& v){
    RootVector out{};
    for(std::size_t k=0;k<4;++k){
        float acc=c[0][k]*v[0];
        acc=acc+c[1][k]*v[1];
        acc=acc+c[2][k]*v[2];
        out[k]=acc+c[3][k]*v[3];
    }
    return out;
}
}

ShotFrame shot_frame_node_world(const ShotFrame& node,const ShotFrame& component){
    ShotFrame out{};
    for(std::size_t i=0;i<4;++i)out[i]=combine(component,node[i]);
    return out;
}

ShotFrame weapon_muzzle_frame(const ShotFrame& world,const WeaponAttachPoint& point,const std::optional<RootVector>& offset){
    // 00385420: the attach matrix = local rows through the world matrix.
    ShotFrame m=shot_frame_node_world(point.local,world);
    // 00385370: vmulax/vmadday/vmaddz .xyz, then vadd.xyz into row 3.
    if(offset){
        const auto& o=*offset;
        for(std::size_t j=0;j<3;++j){
            float acc=m[0][j]*o[0];
            acc=acc+m[1][j]*o[1];
            m[3][j]=m[3][j]+(acc+m[2][j]*o[2]);
        }
    }
    // 00385240: rows (2, 0, 1, 3).
    return ShotFrame{m[2],m[0],m[1],m[3]};
}

ShotFrame player_aim_view_frame(const ShotFrame& camera){
    ShotFrame out=camera;
    out[3]=combine(camera,RootVector{0.0F,1.0F,0.0F,1.0F});
    return out;
}

}
