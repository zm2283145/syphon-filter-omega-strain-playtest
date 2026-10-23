#include "model_attach_points.hpp"
#include "actor_hit_volumes.hpp"
#include "vu_float.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
[[noreturn]] void fail(const char* message){throw std::runtime_error(message);}
void require(bool condition,const char* message){if(!condition)fail(message);}
float fb(std::uint32_t bits){return std::bit_cast<float>(bits);}

RootVector combine(const ShotFrame& m,const RootVector& v){   // vmulax / vmadday / vmaddaz / vmaddw
    RootVector out{};
    for(std::size_t k=0;k<4;++k){
        float acc=m[0][k]*v[0];
        acc=acc+m[1][k]*v[1];
        acc=acc+m[2][k]*v[2];
        out[k]=acc+m[3][k]*v[3];
    }
    return out;
}
RootVector sub(const RootVector& a,const RootVector& b){return {a[0]-b[0],a[1]-b[1],a[2]-b[2],a[3]-b[3]};}
RootVector cross(const RootVector& a,const RootVector& b){     // 001320f0, w = 0
    return {a[1]*b[2]-b[1]*a[2],a[2]*b[0]-b[2]*a[0],a[0]*b[1]-b[0]*a[1],0.0F};
}
bool zero3(const RootVector& v){return v[0]==0.0F&&v[1]==0.0F&&v[2]==0.0F;}   // 001320b0
float sum_squares(const RootVector& v){return (v[0]*v[0]+v[1]*v[1])+v[2]*v[2];}
RootVector normalized(const RootVector& v){                    // 00132030 / 00132060, w kept
    const float s=sum_squares(v);
    require(s!=0.0F,"Attach points: normalizing a zero vector");
    const float q=vu::rsqrt(s);
    return {v[0]*q,v[1]*q,v[2]*q,v[3]};
}
// 00131c70: rows (side, forward, up), row 3 = (0, 0, 0, 1).
ShotFrame single_axis_frame(const RootVector& v){
    const RootVector aux=std::fabs(v[2])<fb(0x3f34fdf4U)?RootVector{0.0F,0.0F,1.0F,0.0F}:RootVector{1.0F,0.0F,0.0F,0.0F};
    const auto forward=normalized(v);
    const auto side=normalized(cross(v,aux));
    const auto up=cross(side,forward);
    return {side,forward,up,RootVector{0.0F,0.0F,0.0F,1.0F}};
}
const ShotFrame identity{RootVector{1.0F,0.0F,0.0F,0.0F},RootVector{0.0F,1.0F,0.0F,0.0F},
                         RootVector{0.0F,0.0F,1.0F,0.0F},RootVector{0.0F,0.0F,0.0F,1.0F}};

// 0038fea0(model, first vertex, bone).
void attach_triple(const SkinChunk& chunk,const std::vector<RootVector>& p,std::uint32_t first,std::uint32_t bone,
                   std::optional<std::span<const ShotFrame>> scales,ModelAttachSlots& slots){
    const auto n=chunk.vertices.size();
    require(first<n,"Attach points: a triple starts past its chunk (the original reads stale VU0 memory)");
    float u=chunk.vertices[first].uv[0],v=chunk.vertices[first].uv[1];
    if(u<0.0F)u=0.0F;
    if(!(u<=1.0F))u=1.0F;
    if(v<0.0F)v=0.0F;
    if(!(v<=1.0F))v=1.0F;
    const auto slot=static_cast<std::int32_t>(4.0F*u)+4*static_cast<std::int32_t>(4.0F*v);
    if(slot>=10||slot<0)return;
    require(std::size_t(first)+2<n,"Attach points: a triple reaches past its chunk (the original reads stale scratchpad)");
    const std::array<RootVector,3> q{p[first],p[first+1],p[first+2]};
    const std::array<float,3> length{vu::sqrt(sum_squares(sub(q[0],q[1]))),vu::sqrt(sum_squares(sub(q[1],q[2]))),
                                     vu::sqrt(sum_squares(sub(q[2],q[0])))};
    int longest=0,shortest=0;
    float l=length[0];
    if(!(length[1]<=l)){l=length[1];longest=1;}
    if(!(length[2]<=l))longest=2;
    float s=length[0];
    if(length[1]<s){s=length[1];shortest=1;}
    if(length[2]<s)shortest=2;
    // The corner opposite the longest edge is i2; i0 / i1 follow the shortest edge.
    std::size_t i0{},i1{},i2{};
    if(longest==0){if(shortest==1){i0=1;i1=0;}else{i0=0;i1=1;}i2=2;}
    else if(longest==1){if(shortest==2){i0=2;i1=1;}else{i0=1;i1=2;}i2=0;}
    else{if(shortest==0){i0=0;i1=2;}else{i0=2;i1=0;}i2=1;}
    const ShotFrame& scale=scales?(*scales)[bone]:identity;
    const auto translation=combine(scale,q[i2]);
    const auto a=sub(q[i2],q[i0]),b=sub(q[i2],q[i1]);
    const auto c=cross(a,b);
    ShotFrame m;
    if(!zero3(c)){
        const auto an=normalized(a);
        const auto cn=normalized(c);
        m={cn,an,cross(cn,an),RootVector{0.0F,0.0F,0.0F,1.0F}};
    }else if(!zero3(a))m=single_axis_frame(a);
    else m=identity;
    const RootVector back{0.0F-m[2][0],0.0F-m[2][1],0.0F-m[2][2],0.0F-m[2][3]};
    slots[static_cast<std::size_t>(slot)]=WeaponAttachPoint{static_cast<std::uint8_t>(bone),ShotFrame{back,m[1],m[0],translation}};
}
}

void hard_mesh_attach_points(const SkinnedMesh& mesh,std::span<const ShotFrame> inverse_bind,
    std::optional<std::span<const ShotFrame>> scales,ModelAttachSlots& slots){
    require(inverse_bind.size()>=21,"Attach points: the setup loads 21 inverse-bind matrices; the skeleton has fewer");
    require(!scales||scales->size()>=21,"Attach points: bone scales for fewer than 21 nodes");
    const auto matrices=inverse_bind.first(21);
    for(const auto& chunk:mesh.chunks){
        const auto positions=hit_query_positions(chunk,matrices);
        const auto n=static_cast<std::uint32_t>(chunk.vertices.size());
        const std::uint32_t base=chunk.groups.front().output_address;
        std::uint32_t covered=0;
        std::size_t g=0;
        while(covered<n){
            require(g<chunk.groups.size(),"Attach points: groups end before the triples cover the chunk (the original reads past them)");
            const auto& group=chunk.groups[g++];
            std::uint32_t first=std::uint32_t(group.output_address)-base;
            auto count=static_cast<std::int32_t>(group.weights.size());
            while(count>0){
                attach_triple(chunk,positions,first,group.bone,scales,slots);
                first+=3;covered+=3;count-=3;
            }
        }
    }
    slots[7]=slots[0];
}

}
