#include "bullet_render.hpp"
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
RootVector add(const RootVector& a,const RootVector& b){return {a[0]+b[0],a[1]+b[1],a[2]+b[2],a[3]+b[3]};}
RootVector sub(const RootVector& a,const RootVector& b){return {a[0]-b[0],a[1]-b[1],a[2]-b[2],a[3]-b[3]};}
RootVector mul(const RootVector& a,float b){return {a[0]*b,a[1]*b,a[2]*b,a[3]*b};}
RootVector cross(const RootVector& a,const RootVector& b){
    return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0],0};
}
}
const BulletTracerUvTable& bullet_tracer_uv_sets(){
    // Runtime image after __sinit_bullet.cc 004d2420..004d2644. The four
    // phases are vertical quarters of tracer01.tdx; q is 1 and the fourth
    // lane is 0 exactly as 002757a0 feeds 00132000.
    static const BulletTracerUvTable sets{{
        {1,0,1,0},{1,0.25F,1,0},{0,0,1,0},{0,0.25F,1,0},
        {1,0.25F,1,0},{1,0.5F,1,0},{0,0.25F,1,0},{0,0.5F,1,0},
        {1,0.5F,1,0},{1,0.75F,1,0},{0,0.5F,1,0},{0,0.75F,1,0},
        {1,0.75F,1,0},{1,1,1,0},{0,0.75F,1,0},{0,1,1,0},
    }};
    return sets;
}
std::optional<BulletTracerMesh> bullet_tracer_mesh(const BulletRecord& b){
    if(!b.active_115||!b.alive_116||!b.tracer_117)return std::nullopt;    // 00275628 / 002757e8
    for(const auto& row:b.frame_20)for(float v:row)
        if(!std::isfinite(v))throw std::runtime_error("Bullet tracer: nonfinite frame");
    const RootVector direction=b.frame_20[1];
    const RootVector position=b.frame_20[3];
    const RootVector side=mul(cross(direction,RootVector{0,0,1,0}),0.3F); // +120, 00277d30..00277d88; 00275af4
    const RootVector up=mul(cross(direction,cross(direction,RootVector{0,0,1,0})),0.3F); // +130
    const RootVector tail=sub(position,mul(direction,8.0F));              // +144, 00275ac4..00275ae8
    BulletTracerMesh out;out.phase=static_cast<std::uint8_t>(b.tracer_phase_140&12U);
    out.material.texture=b.tracer_texture_11c;
    out.vertices={add(position,side),sub(position,side),add(tail,side),sub(tail,side),
                  add(position,up),sub(position,up),add(tail,up),sub(tail,up)};
    const auto& sets=bullet_tracer_uv_sets();
    for(std::size_t strip=0;strip<2;++strip)for(std::size_t k=0;k<4;++k)
        out.uv[strip*4+k]=sets[out.phase+k];
    const RootVector head{1,1,1,1},tail_colour{1,1,1,0.5F};
    out.colour={head,head,tail_colour,tail_colour,head,head,tail_colour,tail_colour};
    // Rings are head 0,4,1,5 and tail 2,6,3,7. Four sides and two caps,
    // counter-wound so the native two-sided tracer remains visible.
    out.indices={0,4,6, 0,6,2, 4,1,3, 4,3,6, 1,5,7, 1,7,3,
                 5,0,2, 5,2,7, 0,5,1, 0,1,4, 2,6,3, 2,3,7};
    return out;
}
std::vector<BulletTracerMesh> bullet_tracer_meshes(const BulletPool& bullets){
    std::vector<BulletTracerMesh> out;
    for(const auto& bullet:bullets.records()){
        if(const auto mesh=bullet_tracer_mesh(bullet))out.push_back(*mesh);
    }
    return out;
}
}
